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
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libutil.h>

#include "ucored.h"

/* SOCK_CLOFORK wasn't introduced until 15.0. */
#ifndef SOCK_CLOFORK
#define SOCK_CLOFORK 0
#endif

#define	UCORED_TIMEOUT	60	/* Seconds */

/* sysctl to enable devctl notifications upon coredump. */
#define	DEVCTL_SYSCTL	"kern.coredump_devctl"

static int ucored_loop(int);

sig_atomic_t ucored_terminate;

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

	sock = socket(PF_UNIX, SOCK_STREAM | SOCK_CLOFORK, 0);
	if (sock == -1) {
		libucore_log(LOG_ERR, "socket: %m");
		return (-1);
	}

	if (fchmod(sock, 0600) == -1) {
		libucore_log(LOG_ERR, "fchmod: %m");
		close(sock);
		return (-1);
	}

	sun.sun_family = AF_UNIX;
	ret = strlcpy(&sun.sun_path[0], PATH_UCORED_SOCK, sizeof(sun.sun_path));
	assert(ret < sizeof(sun.sun_path));

	sun.sun_len = SUN_LEN(&sun);

	(void)unlink(PATH_UCORED_SOCK);
	if (bind(sock, (const struct sockaddr *)&sun, sizeof(sun)) == -1) {
		libucore_log(LOG_ERR, "bind: %m");
		close(sock);
		return (-1);
	}

	return (sock);
}

int
ucored_watch_socket(int kq, int sock, struct ucored_client *cl)
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
		libucore_log(LOG_ERR, "kevent: %m");
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
		libucore_log(LOG_ERR, "sysctlbynme: %m");
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
	int ch, error = 1, devctl_prev, kq = -1, sock = -1, verbose = 0;
	bool debug = false, socket_initiated;

	while ((ch = getopt(argc, argv, "dp:v")) != -1) {
		switch (ch) {
		case 'd':
			debug = true;
			break;
		case 'p':
			pidfile = optarg;
			break;
		case 'v':
			verbose++;
			break;
		default:
			usage();
			break;
		}
	}

	argc -= optind;
	argv += optind;

	libucore_set_debug(debug);
	libucore_set_verbose(verbose);

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

	if (!debug && !socket_initiated && daemon(0, 0) == -1) {
		fprintf(stderr, "daemon: %s", strerror(errno));
		pidfile_remove(pidfh);
		return (1);
	}

	pidfile_write(pidfh);

	kq = kqueue();
	if (kq == -1) {
		libucore_log(LOG_ERR, "kqueue: %m");
		goto done_nosock;
	}

	if (!socket_initiated) {
		sock = ucored_sock();
		if (sock == -1)
			goto done_nosock;

		if (listen(sock, 10) == -1) {
			libucore_log(LOG_ERR, "listen: %m");
			goto done;
		}

		if (ucored_watch_socket(kq, sock, 0) == -1)
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

	ucored_client_close_all();

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

void
ucored_now(struct timespec *tsp)
{
	(void)clock_gettime(CLOCK_UPTIME, tsp);
}

static void
ucored_accept(int kq, int fd, int backlog)
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
		clsock = accept4(fd, NULL, NULL, SOCK_CLOFORK);
		if (clsock == -1) {
			libucore_log(LOG_ERR, "accept: %m");
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

			libucore_log(LOG_ERR, "kevent: %m");
			return (1);
		}

		for (int idx = 0; idx < ret; idx++) {
			const struct kevent *evt = &kev[idx];
			struct ucored_client *cl;
			int fd;

			fd = evt->ident;
			cl = evt->udata;
			if (cl == NULL) {
				ucored_accept(kq, fd, evt->data);
				continue;
			}

			libucore_log(LOG_DEBUG, "Fetching data from client %d",
			    fd);
			ucored_client_fetch(cl, kq, evt->data,
			    (evt->flags & EV_EOF) != 0);
			/* Client may not be valid anymore. */
			if (ucored_terminate)
				goto out;
		}
	}

out:
	return (0);
}
