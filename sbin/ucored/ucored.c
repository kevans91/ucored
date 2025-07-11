/*-
 * Copyright (c) 2025 Kyle Evans <kevans@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/queue.h>
#include <sys/un.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libutil.h>

#include "ucored.h"

static SLIST_HEAD(, ucored_client) all_clients =
    SLIST_HEAD_INITIALIZER(all_clients);

#define	UCORED_TIMEOUT	60	/* Seconds */

/* sysctl to enable devctl notifications upon coredump. */
#define	DEVCTL_SYSCTL	"kern.coredump_devctl"

static int ucored_loop(int);
static size_t ucored_client_lowat(struct ucored_client *);
static bool ucored_client_newseg(struct ucored_client *,
    struct ucore_data_hdr *);
static void ucored_client_done(struct ucored_client *);
static struct ucored_client *ucored_client_alloc(int, int);
static void ucored_client_close(struct ucored_client *, bool);

static bool ucored_debug;
static int ucored_verbose;
static sig_atomic_t ucored_terminate;

static void
handle_signal(int signo __unused)
{

	ucored_terminate = 1;
	atomic_signal_fence(memory_order_release);
}

static void
ucored_signal_setup(void)
{
	sigset_t set;
	struct sigaction sa = {
		/* No SA_RESTART */
		.sa_handler = handle_signal,
	};

	(void)sigaction(SIGINT, &sa, NULL);
	(void)sigaction(SIGTERM, &sa, NULL);

	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);
	(void)sigprocmask(SIG_UNBLOCK, &set, NULL);
}

static int
ucored_sock(void)
{
	struct sockaddr_un sun;
	size_t ret;
	int sock;

	sock = socket(PF_UNIX, SOCK_STREAM, 0);
	if (sock == -1) {
		ucored_log(LOG_ERR, "socket: %m");
		return (-1);
	}

	sun.sun_family = AF_UNIX;
	ret = strlcpy(&sun.sun_path[0], PATH_UCORED_SOCK, sizeof(sun.sun_path));
	assert(ret < sizeof(sun.sun_path));

	sun.sun_len = SUN_LEN(&sun);

	(void)unlink(PATH_UCORED_SOCK);
	if (bind(sock, (const struct sockaddr *)&sun, sizeof(sun)) == -1) {
		ucored_log(LOG_ERR, "bind: %m");
		close(sock);
		return (-1);
	}

	return (sock);
}

static int
watch_socket(int kq, int sock, struct ucored_client *cl)
{
	struct kevent ev;

	if (cl == NULL) {
		/* Listening socket */
		EV_SET(&ev, sock, EVFILT_READ, EV_ADD, 0, 0, NULL);
	} else {
		/*
		 * Clients do one-shot for each segment of data they expect
		 * to receive.
		 */
		EV_SET(&ev, sock, EVFILT_READ, EV_ADD | EV_ONESHOT,
		    NOTE_LOWAT, ucored_client_lowat(cl), cl);
	}

	if (kevent(kq, &ev, 1, NULL, 0, NULL) == -1) {
		ucored_log(LOG_ERR, "kevent: %m");
		return (-1);
	}

	return (0);
}

/*
 * Sets the sysctl to newst and returns the previous state.  We'll be a good
 * citizen and disable it on our way out.
 */
static int
devctl_fiddle(int newst)
{
	int oldval;
	size_t oldlen;

	oldlen = sizeof(oldval);
	if (sysctlbyname(DEVCTL_SYSCTL, &oldval, &oldlen, &newst,
	    sizeof(newst)) == -1) {
		ucored_log(LOG_ERR, "sysctlbynme: %m");
		return (-1);
	}

	return (oldval);
}

static void
usage(void)
{

	fprintf(stderr, "usage: %s [-dv] [-p pidfile]\n", getprogname());
	exit(1);
}

static bool
ucored_socket_check(int fd)
{
	struct stat sb;

	/*
	 * If we can't tell that it's a socket, then we err on the side of
	 * caution and assume it's not.  Otherwise, we'll operate on it.
	 */
	if (fstat(fd, &sb) == -1)
		return (false);
	return (S_ISSOCK(sb.st_mode));
}

int
main(int argc, char *argv[])
{
	const char *pidfile = NULL;
	struct pidfh *pidfh = NULL;
	struct ucored_client *cl;
	int ch, error = 1, devctl_prev, kq = -1, sock = -1;
	bool socket_initiated;

	while ((ch = getopt(argc, argv, "dp:v")) != -1) {
		switch (ch) {
		case 'd':
			ucored_debug = true;
			break;
		case 'p':
			pidfile = optarg;
			break;
		case 'v':
			ucored_verbose++;
			break;
		default:
			usage();
			break;
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 0)
		usage();

	/*
	 * Check if we're socket initiated, e.g., inetd-style.
	 */
	socket_initiated = ucored_socket_check(STDIN_FILENO);
	if (socket_initiated)
		sock = STDIN_FILENO;

	if (pidfile != NULL &&
	    (pidfh = pidfile_open(pidfile, 0644, NULL)) == NULL) {
		if (errno == EEXIST)
			errx(1, "ucored is already running");

		warn("pidfile_open: %m");
	}

	if (!ucored_lua_init()) {
		pidfile_remove(pidfh);
		return (1);
	}

	if (!ucored_debug && !socket_initiated && daemon(0, 0) == -1) {
		fprintf(stderr, "daemon: %s", strerror(errno));
		pidfile_remove(pidfh);
		return (1);
	}

	pidfile_write(pidfh);

	kq = kqueue();
	if (kq == -1) {
		ucored_log(LOG_ERR, "kqueue: %m");
		goto done_nosock;
	}

	if (!socket_initiated) {
		sock = ucored_sock();
		if (sock == -1)
			goto done_nosock;

		if (listen(sock, 10) == -1) {
			ucored_log(LOG_ERR, "listen: %m");
			goto done;
		}

		if (watch_socket(kq, sock, 0) == -1)
			goto done;
	} else if (ucored_client_alloc(kq, sock) == NULL) {
		goto done;
	}

	/*
	 * We'll setup SIGINT/SIGTERM to interrupt us, after which we'll briefly
	 * evacuate the daemon and clean up our current state.  Any cores that
	 * are in the process of being handled will be finished, but if we were
	 * in the middle of receiving core metadata we'll drop that one on the
	 * floor.
	 */
	ucored_signal_setup();

	if (!socket_initiated)
		devctl_prev = devctl_fiddle(1);

	error = ucored_loop(kq);

	/*
	 * Cleanup time; make sure we don't leave our socket laying around, and
	 * we'll also want to restore the previous system state for whether or
	 * not the kernel generates devctl notifications for coredumps.
	 */
	if (!socket_initiated && !devctl_prev)
		(void)devctl_fiddle(0);

	while ((cl = SLIST_FIRST(&all_clients)) != NULL)
		ucored_client_close(cl, false);

done:
	if (!socket_initiated)
		(void)unlink(PATH_UCORED_SOCK);
done_nosock:
	pidfile_remove(pidfh);

	if (kq >= 0)
		close(kq);
	if (sock >= 0)
		close(sock);
	return (error);
}

static void
ucore_now(struct timespec *tsp)
{
	(void)clock_gettime(CLOCK_UPTIME, tsp);
}

static void
ucore_accept(int kq, int fd, int backlog)
{
	int clsock;

	/*
	 * All errors here are considered non-fatal.  We'll return and move on
	 * in case it's a transient condition; perhaps some other event we will
	 * be executing on would relieve it.  In the worst case scenario, all
	 * we have are new connections and we'll keep returning here just to
	 * shut them down.
	 */
	for (int i = 0; i < backlog; i++) {
		clsock = accept(fd, NULL, NULL);
		if (clsock == -1) {
			ucored_log(LOG_ERR, "accept: %m");
			return;
		}

		/*
		 * XXX Consider setting up an EVILT_TIMER with
		 * with ident=(uintptr_t)cl to release resources from a
		 * ucored client if they don't send us something valid in a
		 * timely manner.  We don't bother in the socket-initiated case
		 * because each instance services just a single client.
		 */
		if (ucored_client_alloc(kq, clsock) == NULL)
			return;
	}
}

static void
ucore_fetch(int kq, struct ucored_client *cl, size_t avail, bool eof)
{
	struct ucore_data_hdr datahdr;
	size_t wanted;

	while (avail >= (wanted = ucored_client_lowat(cl))) {
		void *buf;
		size_t bufsz;

		/*
		 * When we're signaled to terminate, just break out immediately.
		 */
		atomic_signal_fence(memory_order_acquire);
		if (ucored_terminate)
			return;

		assert(cl->cl_state != STATE_DONE);
		switch (cl->cl_state) {
		case STATE_HDR:
			ucored_log(LOG_DEBUG, "Reading client header");
			buf = &cl->cl_hdr;
			bufsz = sizeof(cl->cl_hdr);
			break;
		case STATE_DATASEGS:
			if (cl->cl_curdataseg == NULL) {
				ucored_log(LOG_DEBUG, "Reading data segment header");
				buf = &datahdr;
				bufsz = sizeof(datahdr);
			} else {
				ucored_log(LOG_DEBUG, "Reading data segment body");
				buf = &cl->cl_curdataseg->cl_data.ud_data;
				bufsz = cl->cl_curdataseg->cl_data.ud_hdr.uhdr_size;
			}

			break;
		case STATE_DONE:
			__assert_unreachable();
			break;
		}

		if (!libucore_read_data(cl->cl_fd, buf, bufsz)) {
			/* XXX Some way to identify the connection closed? */
			ucored_log(LOG_ERR, "closing connection");
			goto error;
		}

		switch (cl->cl_state) {
		case STATE_HDR:
			/* Validate the header we received. */
			ucored_log(LOG_DEBUG, "Validating header");

			buf = &cl->cl_hdr;
			if (memcmp(cl->cl_hdr.ucore_magic, UCORE_MAGIC,
			    sizeof(cl->cl_hdr.ucore_magic)) != 0) {
				ucored_log(LOG_ERR, "bad magic -- closing connection");
				goto error;
			}

			if (cl->cl_hdr.ucore_datasegs == 0) {
				ucored_log(LOG_ERR, "no data -- closing connection");
				goto error;
			}

			cl->cl_ndatasegs = cl->cl_hdr.ucore_datasegs;
			cl->cl_state = STATE_DATASEGS;
			break;
		case STATE_DATASEGS:
			if (cl->cl_curdataseg == NULL) {
				ucored_log(LOG_DEBUG, "Segment header received");
				/* ucored_client_newseg should issue error. */
				if (!ucored_client_newseg(cl, &datahdr))
					goto error;
			} else {
				/* Segment is finished. */
				ucored_log(LOG_DEBUG, "Data segment finished");
				cl->cl_datasegs_recvd++;
				SLIST_INSERT_HEAD(&cl->cl_datasegs,
				    cl->cl_curdataseg, cl_entry);
				cl->cl_curdataseg = NULL;

				if (cl->cl_datasegs_recvd == cl->cl_ndatasegs) {
					cl->cl_state = STATE_DONE;
					ucored_client_done(cl);
					return;
				}
			}

			break;
		case STATE_DONE:
			__assert_unreachable();
		}
	}

	/*
	 * We don't read() until EOF unless there's some malformed data; it
	 * could be that they sent some valid data then closed up shop.
	 */
	if (eof) {
		ucored_log(LOG_ERR, "client prematurely disappeared");
		goto error;
	}

	/* Re-arm the kevent because we need more data. */
	watch_socket(kq, cl->cl_fd, cl);
	ucore_now(&cl->cl_lastseen);

	return;
error:
	if (eof) {
		/* Don't send anymore data! */
		close(cl->cl_fd);
		cl->cl_fd = -1;
	}

	ucored_client_close(cl, false);
}

static int
ucored_loop(int kq)
{
	struct kevent kev[4];
	int ret;

	assert(kq >= 0);

	for (;;) {
		atomic_signal_fence(memory_order_acquire);
		if (ucored_terminate)
			break;

		ret = kevent(kq, NULL, 0, &kev[0], nitems(kev), NULL);
		if (ret == -1) {
			if (errno == EINTR)
				continue;

			ucored_log(LOG_ERR, "kevent: %m");
			return (1);
		}

		for (int idx = 0; idx < ret; idx++) {
			const struct kevent *evt = &kev[idx];
			struct ucored_client *cl;
			int fd;

			fd = evt->ident;
			cl = evt->udata;
			if (cl == NULL) {
				ucore_accept(kq, fd, evt->data);
				continue;
			}

			ucored_log(LOG_DEBUG, "Fetching data from client %d",
			    fd);
			ucore_fetch(kq, cl, evt->data, (evt->flags & EV_EOF) != 0);
			/* Client may not be valid anymore. */
			if (ucored_terminate)
				goto out;
		}
	}

out:
	return (0);
}

static size_t
ucored_client_lowat(struct ucored_client *cl)
{

	switch (cl->cl_state) {
	case STATE_HDR:
		return (sizeof(cl->cl_hdr));
	case STATE_DATASEGS:
		/*
		 * The data-sgement part of the state machine is either waiting
		 * for headers or waiting for the data part of the segment.  For
		 * simplicity, we won't read() until we have all of the data we
		 * need (for better or worse).
		 */
		if (cl->cl_curdataseg == NULL)
			return (sizeof(struct ucore_data_hdr));
		else
			return (cl->cl_curdataseg->cl_data.ud_hdr.uhdr_size);
		break;
	default:
		/* Unknown: Just return if we have *anything* to read. */
		return (1);
	}
}

static bool
ucored_client_newseg(struct ucored_client *cl, struct ucore_data_hdr *hdr)
{
	struct ucored_client_data *dataseg;

	assert(cl->cl_curdataseg == NULL);
	if (hdr->uhdr_size > UCORED_MAXSEGSZ) {
		ucored_log(LOG_ERR,
		    "overly large segment (%zu) -- closing connection",
		    hdr->uhdr_size);
		return (false);
	}

	dataseg = malloc(sizeof(struct ucored_client_data) + hdr->uhdr_size);
	if (dataseg == NULL) {
		ucored_log(LOG_ERR, "malloc newseg: %m -- closing connection");
		return (false);
	}

	memcpy(&dataseg->cl_data.ud_hdr, hdr, sizeof(*hdr));
	cl->cl_curdataseg = dataseg;

	return (true);
}

/*
 * Send an ack with the indicated status, to be interpreted as a normal exit
 * status for the time being.  That is, 0 is 'good', non-zero is an
 * 'error condition'. We don't really define those non-zero values at the moment.
 */
static void
ucored_client_send_ack(struct ucored_client *cl, int status)
{
	struct ucore_ack ack = { .ucore_status = status };

	memcpy(&ack.ucore_magic, UCORE_MAGIC, sizeof(ack.ucore_magic));
	if (!libucore_send_data(cl->cl_fd, &ack, sizeof(ack)))
		ucored_log(LOG_ERR, "Failed to ack with status=%d", status);
}

static void
ucored_client_done(struct ucored_client *cl)
{

	assert(cl->cl_state == STATE_DONE);

	shutdown(cl->cl_fd, SHUT_RD);
	ucored_client_send_ack(cl, 0);

	ucored_log(LOG_INFO, "Core details received [pid=%d, ppid=%d, signo=%d]",
	    cl->cl_hdr.ucore_pid, cl->cl_hdr.ucore_ppid, cl->cl_hdr.ucore_signo);

	/* XXX Check return... fork? */
	ucored_lua_handle(cl);

	ucored_client_close(cl, true);
}

static struct ucored_client *
ucored_client_alloc(int kq, int clsock)
{
	struct ucored_client *cl;
	uid_t uid;
	gid_t gid;

	if (getpeereid(clsock, &uid, &gid)  != 0) {
		ucored_log(LOG_ERR, "getpeereid: %m");
		close(clsock);
		return (NULL);
	} else if (uid != 0) {
		ucored_log(LOG_ERR, "terminating attempted connection by user %d",
		    uid);
		close(clsock);
		return (NULL);
	}

	cl = calloc(1, sizeof(*cl));
	if (cl == NULL) {
		ucored_log(LOG_ERR, "malloc: %m");
		close(clsock);
		return (NULL);
	}

	SLIST_INSERT_HEAD(&all_clients, cl, cl_client);
	SLIST_INIT(&cl->cl_datasegs);
	cl->cl_fd = clsock;
	ucore_now(&cl->cl_lastseen);

	watch_socket(kq, clsock, cl);
	return (cl);
}

static void
ucored_client_close(struct ucored_client *cl, bool acked)
{
	struct ucored_client_data *cld;

	if (cl->cl_fd >= 0) {
			if (!acked)
				ucored_client_send_ack(cl, 1);

			close(cl->cl_fd);
			cl->cl_fd = -1;
	}

	while ((cld = SLIST_FIRST(&cl->cl_datasegs)) != NULL) {
		SLIST_REMOVE_HEAD(&cl->cl_datasegs, cl_entry);
		free(cld);
	}

	SLIST_REMOVE(&all_clients, cl, ucored_client, cl_client);

	free(cl);
}

void
ucored_log(int priority, const char *fmt, ...)
{
	va_list ap;

	if (priority == LOG_INFO && ucored_verbose < 1)
		return;
	if (priority == LOG_DEBUG && ucored_verbose < 2)
		return;

	va_start(ap, fmt);
	if (ucored_debug) {
		FILE *fp;

		switch (priority) {
		case LOG_NOTICE:
		case LOG_INFO:
		case LOG_DEBUG:
			fp = stdout;
			break;
		case LOG_ERR:
		case LOG_WARNING:
		default:
			fp = stderr;
			break;
		}

		vfprintf(fp, fmt, ap);
		/* syslog messages omit the newline; just toss one in. */
		fputc('\n', fp);
	} else {
		vsyslog(priority, fmt, ap);
	}
	va_end(ap);
}
