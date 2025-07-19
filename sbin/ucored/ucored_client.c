/*-
 * Copyright (c) 2025 Kyle Evans <kevans@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ucored.h"

static SLIST_HEAD(, ucored_client) all_clients =
    SLIST_HEAD_INITIALIZER(all_clients);

size_t
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

bool
ucored_client_newseg(struct ucored_client *cl, struct ucore_data_hdr *hdr)
{
	struct ucored_client_data *dataseg;

	assert(cl->cl_curdataseg == NULL);
	if (hdr->uhdr_size > UCORED_MAXSEGSZ) {
		libucore_log(LOG_ERR,
		    "overly large segment (%zu) -- closing connection",
		    hdr->uhdr_size);
		return (false);
	}

	dataseg = malloc(sizeof(struct ucored_client_data) + hdr->uhdr_size);
	if (dataseg == NULL) {
		libucore_log(LOG_ERR, "malloc newseg: %m -- closing connection");
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
		libucore_log(LOG_ERR, "Failed to ack with status=%d", status);
}

struct ucored_client *
ucored_client_alloc(int kq, int clsock)
{
	struct ucored_client *cl;
	uid_t uid;
	gid_t gid;

	if (getpeereid(clsock, &uid, &gid)  != 0) {
		libucore_log(LOG_ERR, "getpeereid: %m");
		close(clsock);
		return (NULL);
	} else if (uid != 0) {
		libucore_log(LOG_ERR, "terminating attempted connection by user %d",
		    uid);
		close(clsock);
		return (NULL);
	}

	cl = calloc(1, sizeof(*cl));
	if (cl == NULL) {
		libucore_log(LOG_ERR, "malloc: %m");
		close(clsock);
		return (NULL);
	}

	SLIST_INSERT_HEAD(&all_clients, cl, cl_client);
	SLIST_INIT(&cl->cl_datasegs);
	cl->cl_fd = clsock;
	ucored_now(&cl->cl_lastseen);

	ucored_watch_socket(kq, clsock, cl);
	return (cl);
}

void
ucored_client_done(struct ucored_client *cl)
{

	assert(cl->cl_state == STATE_DONE);

	shutdown(cl->cl_fd, SHUT_RD);
	ucored_client_send_ack(cl, 0);

	libucore_log(LOG_INFO, "Core details received [pid=%d, ppid=%d, signo=%d]",
	    cl->cl_hdr.ucore_pid, cl->cl_hdr.ucore_ppid, cl->cl_hdr.ucore_signo);

	/* XXX Check return... fork + close all FDs? */
	ucored_lua_handle(cl);

	ucored_client_close(cl, true);
}

void
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
ucored_client_close_all(void)
{
	struct ucored_client *cl;

	while ((cl = SLIST_FIRST(&all_clients)) != NULL)
		ucored_client_close(cl, false);
}
