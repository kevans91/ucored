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
#include <sys/un.h>
#include <sys/wait.h>

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

/* sysctl to enable devctl notifications upon coredump. */
#define	DEVCTL_SYSCTL	"kern.coredump_devctl"

struct ucored_server {
	struct ucore_readable	serv_readable;
	int			serv_kq;
};

#define	UCORED_SERVER_OF(ur)	\
    __containerof(ur, struct ucored_server, serv_readable)

static int ucored_loop(int);
static bool ucored_accept(struct ucore_readable *, size_t, bool);

sig_atomic_t ucored_checkpwait;
sig_atomic_t ucored_terminate;

static void
handle_sigchld(int signo __unused)
{
	ucored_checkpwait = 1;
	atomic_signal_fence(memory_order_release);
}

static void
handle_termsig(int signo __unused)
{
	ucored_terminate = 1;
	atomic_signal_fence(memory_order_release);
}

static void
ucored_drain_zombies(void)
{
	pid_t wpid;
	int status;

	while ((wpid = waitpid(0, &status, WNOHANG)) != 0) {
		if (wpid == -1) {
			if (errno == EINTR)
				continue;
			if (errno == ECHILD)
				break;

			__assert_unreachable();
		}

		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			libucore_log(LOG_NOTICE,
			    "Child %d terminated abnormally (status=%x)",
			    wpid, status);
			continue;
		}

		libucore_log(LOG_INFO, "Child %d terminated OK", wpid);
	}
}

static void
ucored_signal_setup(void)
{
	sigset_t set;
	struct sigaction sa = {
		/* No SA_RESTART */
		.sa_handler = handle_termsig,
	};

	(void)sigaction(SIGINT, &sa, NULL);
	(void)sigaction(SIGTERM, &sa, NULL);

	sa.sa_handler = handle_sigchld;
	(void)sigaction(SIGCHLD, &sa, NULL);

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
ucored_watch_socket(int kq, struct ucore_readable *ur)
{
	struct kevent ev;
	int fd = ur->r_fd;

	/*
	 * The continuous monitoring case is likely the listeing socket, since
	 * we don't really have a need to do that.  Clients will generally do
	 * one-shot for each segment of data they expect to receive.
	 */
	if (!ur->r_oneshot) {
		/* Listening socket */
		EV_SET(&ev, fd, EVFILT_READ, EV_ADD, 0, 0, ur);
	} else if (ur->r_lowat != NULL) {
		EV_SET(&ev, fd, EVFILT_READ, EV_ADD | EV_ONESHOT,
		    NOTE_LOWAT, (*ur->r_lowat)(ur), ur);
	} else {
		EV_SET(&ev, fd, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, ur);
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
	struct ucore_dev *udev = NULL;
	struct ucored_server *userv = NULL;
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
		struct ucore_readable *ur;

		/*
		 * XXX Should the socket and device be mutually exclusive?
		 */
		if (libucore_dev_available(true)) {
			udev = libucore_dev_open(ucored_lua_handle);
			if (udev == NULL) {
				libucore_log(LOG_ERR, "failed to open /dev/ucore");
				goto done;
			}

			ur = libucore_dev_readable(udev);
			if (ucored_watch_socket(kq, ur) == -1)
				goto done;

			libucore_log(LOG_INFO, "ucored polling /dev/ucore");
		} else {
			sock = ucored_sock();
			if (sock == -1)
				goto done_nosock;

			if (listen(sock, 10) == -1) {
				libucore_log(LOG_ERR, "listen: %m");
				goto done;
			}

			userv = calloc(1, sizeof(*userv));
			if (userv == NULL) {
				libucore_log(LOG_ERR, "calloc: %m");
				goto done;
			}

			userv->serv_kq = kq;

			ur = &userv->serv_readable;
			ur->r_read = ucored_accept;
			ur->r_fd = sock;

			if (ucored_watch_socket(kq, ur) == -1)
				goto done;
		}
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

	if (!socket_initiated && sock >= 0)
		devctl_prev = devctl_fiddle(1);

	error = ucored_loop(kq);

	/*
	 * Cleanup time; make sure we don't leave our socket laying around, and
	 * we'll also want to restore the previous system state for whether or
	 * not the kernel generates devctl notifications for coredumps.
	 */
	if (!socket_initiated && sock >= 0 && !devctl_prev)
		(void)devctl_fiddle(0);

	ucored_client_close_all();

done:
	if (!socket_initiated && sock >= 0)
		(void)unlink(PATH_UCORED_SOCK);
done_nosock:
	pidfile_remove(pidfh);

	if (kq >= 0)
		close(kq);
	libucore_dev_close(udev);
	free(userv);
	if (sock >= 0)
		close(sock);
	return (error);
}

void
ucored_now(struct timespec *tsp)
{
	(void)clock_gettime(CLOCK_UPTIME, tsp);
}

static bool
ucored_accept(struct ucore_readable *ur, size_t backlog, bool eof __unused)
{
	struct ucored_server *userv = UCORED_SERVER_OF(ur);
	int clsock;
	int fd = ur->r_fd, kq = userv->serv_kq;

	/*
	 * All errors here are considered non-fatal.  We'll return and move on
	 * in case it's a transient condition; perhaps some other event we will
	 * be executing on would relieve it.  In the worst case scenario, all
	 * we have are new connections and we'll keep returning here just to
	 * shut them down.
	 */
	for (size_t i = 0; i < backlog; i++) {
		clsock = accept4(fd, NULL, NULL, SOCK_CLOFORK);
		if (clsock == -1) {
			libucore_log(LOG_ERR, "accept: %m");
			return (true);
		}

		if (ucored_client_alloc(kq, clsock) == NULL)
			return (true);
	}

	return (true);
}

static void
ucored_timer(void)
{
	size_t purged;

	purged = ucored_client_purge_inactive();
	if (purged > 0)
		libucore_log(LOG_NOTICE, "purged %zu inactive clients", purged);
}

static bool
ucored_set_timer(int kq, bool state)
{
	struct kevent kev;
	int flag;

	flag = EV_ADD | (state ? EV_ENABLE : EV_DISABLE);
	EV_SET(&kev, 0, EVFILT_TIMER, flag, NOTE_SECONDS, UCORED_TIMEOUT + 1,
	    NULL);
	if (kevent(kq, &kev, 1, NULL, 0, NULL) == -1) {
		libucore_log(LOG_ERR, "%s timer kevent: %m",
		    state ? "enable" : "disable");
		return (false);
	}

	return (true);
}

static int
ucored_loop(int kq)
{
	struct kevent kev[4];
	int ret;

	assert(kq >= 0);

	for (;;) {
		size_t current_clients;
		bool timer_fired;

		atomic_signal_fence(memory_order_acquire);

		if (ucored_checkpwait) {
			ucored_drain_zombies();
			ucored_checkpwait = 0;
		}

		if (ucored_terminate)
			break;

		ret = kevent(kq, NULL, 0, &kev[0], nitems(kev), NULL);
		if (ret == -1) {
			if (errno == EINTR)
				continue;

			libucore_log(LOG_ERR, "kevent: %m");
			return (1);
		}

		current_clients = ucored_clients;
		timer_fired = false;
		for (int idx = 0; idx < ret; idx++) {
			const struct kevent *evt = &kev[idx];
			struct ucore_readable *ur;
			int fd;
			bool rearm, oneshot;

			fd = evt->ident;
			ur = evt->udata;

			/*
			 * Process any other events before we process the timer, just in
			 * case one of them is a client trying to send us information.
			 */
			if (evt->filter == EVFILT_TIMER) {
				timer_fired = true;
				continue;
			}

			assert(ur != NULL);

			libucore_log(LOG_DEBUG, "Fetching data from fd %d",
			    fd);

			oneshot = ur->r_oneshot;
			rearm = (*ur->r_read)(ur, evt->data,
			    (evt->flags & EV_EOF) != 0);

			/*
			 * Client may not be valid anymore, unless we were
			 * asked to re-arm.  If we were asked to terminate, just
			 * bail out anyways.
			 */
			if (ucored_terminate)
				goto out;

			/*
			 * The readable needs to coordinate its own removal if
			 * it wants to go away; we don't manage the lifetime of
			 * ucore_readable objects here.  We stashed r_oneshot
			 * away just in case the object was freed during the
			 * r_read() callback, but if it requested re-arm then
			 * the readable should still be alive.
			 */
			if (!oneshot) {
				assert(rearm);
				continue;
			}

			if (rearm)
				ucored_watch_socket(kq, ur);
		}

		if (timer_fired)
			ucored_timer();

		/* On a state transition, arm or disarm our timer. */
		if ((current_clients != 0) != (ucored_clients != 0)) {
			bool state = ucored_clients != 0;

			libucore_log(LOG_INFO,
			    "Clients went from %zu -> %zu, %sarming the alarm",
			    current_clients, ucored_clients,
			    state ? "" : "dis");
			ucored_set_timer(kq, state);
		}
	}

out:
	return (0);
}
