/*-
 * Copyright (c) 2025 Kyle Evans <kevans@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <jail.h>

#include "ucored.h"

struct ucore_shuttle_data {
	SLIST_ENTRY(ucore_shuttle_data)	sd_entry;
	struct ucore_data		sd_data;
};

static SLIST_HEAD(, ucore_shuttle_data) sd_segments;
static size_t sd_nsegments;

static void
usage(void)
{

	fprintf(stderr, "usage: %s [-j jail] command corepath\n", getprogname());
	exit(1);
}

/*
 * Resolves `jid` to a path, grabs the jail name while we're at it so we can
 * send that along.
 */
static char *
resolve_path(int jid, int pid, const char *comm, const char *core,
    char *ojailpath, char *ojailname)
{
	char *corepath;
	char strjid[16];

	if (jid == 0)
		return (strdup(core));

	(void)snprintf(strjid, sizeof(strjid), "%d", jid);

	if (jail_getv(0, "jid", strjid, "path", ojailpath,
	    "name", ojailname, NULL) == -1) {
		syslog(LOG_ERR,
		    "%s: jid %s for %s[pid=%d] seems to have disappeared",
		    core, strjid, comm, pid);
		exit(1);
	}

	/*
	 * The core path we get in the devd notification should be absolute
	 * from the jail's root, so we just need to prepend the jail path.
	 */
	if (asprintf(&corepath, "%s%s", ojailpath, core) == -1)
		return (NULL);

	return (corepath);
}

static int ucored_connect(void);

static int
ucored_connect(void)
{
	struct sockaddr_un sun;
	size_t ret;
	int sock;

	sock = socket(PF_UNIX, SOCK_STREAM, 0);
	if (sock == -1) {
		syslog(LOG_ERR, "socket: %m");
		return (-1);
	}

	sun.sun_family = AF_UNIX;
	ret = strlcpy(&sun.sun_path[0], PATH_UCORED_SOCK, sizeof(sun.sun_path));
	assert(ret < sizeof(sun.sun_path));

	sun.sun_len = SUN_LEN(&sun);
	if (connect(sock, (const struct sockaddr *)&sun, sizeof(sun)) == -1) {
		syslog(LOG_ERR, "connect: %m");
		close(sock);
		return (-1);
	}

	return (sock);
}

static int
add_segment(enum ucore_data_type type, const void *payload, size_t payloadsz)
{
	struct ucore_shuttle_data *useg;
	struct ucore_data *data;

	if (sd_nsegments == UCORED_MAXSEGS) {
		/* XXX Shouldn't be hit. */
		syslog(LOG_ERR, "too many segments");
		return (1);
	} else if (payloadsz >= UCORED_MAXSEGSZ) {
		syslog(LOG_ERR,
		    "payload type %d size %zu exceeds max segment size",
		    type, payloadsz);
		return (1);
	}

	useg = malloc(sizeof(*useg) + payloadsz);
	if (useg == NULL) {
		syslog(LOG_ERR, "malloc: %m");
		return (1);
	}

	data = &useg->sd_data;
	SLIST_INSERT_HEAD(&sd_segments, useg, sd_entry);

	data->ud_hdr.uhdr_type = type;
	data->ud_hdr.uhdr_size = payloadsz;
	memcpy(&data->ud_data, payload, payloadsz);

	sd_nsegments++;
	return (0);
}

static bool
ucored_send_data(int ucored, const void *payload, size_t payloadsz)
{
	size_t written = 0;

	while (written < payloadsz) {
		ssize_t writesz;

		assert(payloadsz != 0);
		writesz = write(ucored, payload, payloadsz);
		if (writesz < 0) {
			if (errno == EINTR)
				continue;

			syslog(LOG_ERR, "write: %m");
			return (false);
		} else if (writesz == 0) {
			syslog(LOG_ERR, "write: premature EOF");
			return (false);
		}

		written += writesz;
		payload = (const uint8_t *)payload + writesz;
		payloadsz -= writesz;
	}

	return (true);
}

static int
ucored_send(int ucored, int jid, int ppid, int pid, int signo)
{
	struct ucore_shuttle_data *sd;
	struct ucore uc = { 0 };

	assert(sd_nsegments != 0);
	memcpy(&uc.ucore_magic[0], UCORE_MAGIC, sizeof(uc.ucore_magic));
	uc.ucore_datasegs = sd_nsegments;
	uc.ucore_jid = jid;
	uc.ucore_ppid = ppid;
	uc.ucore_pid = pid;
	uc.ucore_signo = signo;

	if (!ucored_send_data(ucored, &uc, sizeof(uc)))
		return (1);

	SLIST_FOREACH(sd, &sd_segments, sd_entry) {
		struct ucore_data *ud;

		/*
		 * We send the header and payload separate in case we have a
		 * payload that trends towards the larger size of the maximum
		 * segment size.
		 */
		ud = &sd->sd_data;
		if (!ucored_send_data(ucored, &ud->ud_hdr,
		    sizeof(struct ucore_data_hdr)))
			return (false);
		if (!ucored_send_data(ucored, &ud->ud_data,
		    ud->ud_hdr.uhdr_size))
			return (false);
	}

	return (0);
}

int
main(int argc, char *argv[])
{
	char jailpath[MAXPATHLEN];
	char jailname[MAXHOSTNAMELEN];
	const char *comm, *core;
	char *corepath;
	const char *errstr, *jail = NULL;
	int ch, error = 1, jid = 0, ppid = 0, pid = 0, signo = 0, ucored = -1;

	memset(jailname, 0, sizeof(jailname));
	while ((ch = getopt(argc, argv, "j:P:p:s:")) != -1) {
		switch (ch) {
		case 'j':
			if (strcmp(optarg, "0") != 0)
				jail = optarg;
			break;
		case 'P':
			ppid = strtonum(optarg, 0, INT_MAX, &errstr);
			if (errstr != NULL) {
				syslog(LOG_ERR, "ppid: %s", errstr);
				return (1);
			}

			break;
		case 'p':
			pid = strtonum(optarg, 0, INT_MAX, &errstr);
			if (errstr != NULL) {
				syslog(LOG_ERR, "pid: %s", errstr);
				return (1);
			}

			break;
		case 's':
			signo = strtonum(optarg, 0, INT_MAX, &errstr);
			if (errstr != NULL) {
				syslog(LOG_ERR, "signal: %s", errstr);
				return (1);
			}

			break;
		default:
			/* Weird, we're normally non-interactive. */
			usage();
			return (1);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 2) {
		syslog(LOG_ERR, "Missing command or core path");
		return (1);
	}

	comm = argv[0];
	core = argv[1];

	if (jail != NULL) {
			syslog(LOG_INFO,
				"%s: notification received for jail %s process %s[pid=%d]",
				core, jail, comm, pid);
	} else {
			syslog(LOG_INFO,
				"%s: notification received for unjailed process %s[pid=%d]",
				core, comm, pid);
	}

	if (jail != NULL && (jid = jail_getid(jail)) == -1) {
		syslog(LOG_ERR,
		    "%s: jail %s for %s[pid=%d] seems to have disappeared",
		    core, jail, comm, pid);
		return (1);
	}

	corepath = resolve_path(jid, pid, comm, core, &jailpath[0],
	    &jailname[0]);
	if (corepath == NULL) {
		syslog(LOG_ERR, "resolve_path: %m");
		return (1);
	}

	ucored = ucored_connect();
	if (ucored == -1) {
		free(corepath);
		return (1);
	}

	/* Add NUL terminators across the board. */
	if ((error = add_segment(UDT_COMM, comm, strlen(comm) + 1)) != 0)
		goto out;
	if ((error = add_segment(UDT_PATH, corepath, strlen(corepath) + 1)) != 0)
		goto out;

	if (jailname[0] != '\0') {
		error = add_segment(UDT_JAIL, jailname, strlen(jailname) + 1);
		if (error != 0)
			goto out;
		error = add_segment(UDT_JAILROOT, jailpath,
		    strlen(jailpath) + 1);
		if (error != 0)
			goto out;
	}

	error = ucored_send(ucored, jid, ppid, pid, signo);
out:
	if (corepath != NULL)
		free(corepath);
	if (ucored >= 0)
		close(ucored);

	return (error);
}
