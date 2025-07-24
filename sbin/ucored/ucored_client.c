/*-
 * Copyright (c) 2025 Kyle Evans <kevans@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ucored.h"

static size_t ucored_client_lowat(struct ucore_readable *ur);

struct ucored_client {
	struct ucore_readable			cl_readable;
	struct ucore_provider			cl_provider;
	struct ucore				cl_hdr;
	SLIST_ENTRY(ucored_client)		cl_client;
	SLIST_HEAD(,  ucored_client_data)	cl_datasegs;
	struct ucored_client_data		*cl_curdataseg;
	size_t					cl_ndatasegs;
	size_t					cl_datasegs_recvd;
	struct timespec				cl_lastseen;
	enum ucored_state			cl_state;
};

#define	cl_fd	cl_readable.r_fd
#define	UCORED_CLIENT_FROM_READABLE(ur)	\
    __containerof(ur, struct ucored_client, cl_readable)
#define	UCORED_CLIENT_FROM_PROVIDER(ur)	\
    __containerof(ur, struct ucored_client, cl_provider)

static SLIST_HEAD(, ucored_client) all_clients =
    SLIST_HEAD_INITIALIZER(all_clients);

static const struct ucore_data *
ucored_client_data(const struct ucore_provider *up, enum ucore_data_type type)
{
	const struct ucored_client *cl = UCORED_CLIENT_FROM_PROVIDER(up);
	struct ucored_client_data *sd;

	SLIST_FOREACH(sd, &cl->cl_datasegs, cl_entry) {
		if (sd->cl_data.ud_hdr.uhdr_type == type)
			return (&sd->cl_data);
	}

	return (NULL);
}

static bool
ucored_client_fetch(struct ucore_readable *ur, size_t avail, bool eof)
{
	struct ucored_client *cl = UCORED_CLIENT_FROM_READABLE(ur);
	struct ucore_data_hdr datahdr;
	size_t wanted;

	while (avail >= (wanted = ucored_client_lowat(ur))) {
		void *buf;
		size_t bufsz;

		/*
		 * When we're signaled to terminate, just break out immediately
		 * and don't ask the caller to reschedule us.
		 */
		atomic_signal_fence(memory_order_acquire);
		if (ucored_terminate)
			return (false);

		assert(cl->cl_state != STATE_DONE);
		switch (cl->cl_state) {
		case STATE_HDR:
			libucore_log(LOG_DEBUG, "Reading client header");
			buf = &cl->cl_hdr;
			bufsz = sizeof(cl->cl_hdr);
			break;
		case STATE_DATASEGS:
			if (cl->cl_curdataseg == NULL) {
				libucore_log(LOG_DEBUG, "Reading data segment header");
				buf = &datahdr;
				bufsz = sizeof(datahdr);
			} else {
				libucore_log(LOG_DEBUG, "Reading data segment body");
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
			libucore_log(LOG_ERR, "closing connection");
			goto error;
		}

		switch (cl->cl_state) {
		case STATE_HDR:
			/* Validate the header we received. */
			libucore_log(LOG_DEBUG, "Validating header");

			buf = &cl->cl_hdr;
			if (memcmp(cl->cl_hdr.ucore_magic, UCORE_MAGIC,
			    sizeof(cl->cl_hdr.ucore_magic)) != 0) {
				libucore_log(LOG_ERR, "bad magic -- closing connection");
				goto error;
			}

			if (cl->cl_hdr.ucore_datasegs == 0) {
				libucore_log(LOG_ERR, "no data -- closing connection");
				goto error;
			}

			cl->cl_ndatasegs = cl->cl_hdr.ucore_datasegs;
			cl->cl_state = STATE_DATASEGS;
			break;
		case STATE_DATASEGS:
			if (cl->cl_curdataseg == NULL) {
				libucore_log(LOG_DEBUG, "Segment header received");
				/* ucored_client_newseg should issue error. */
				if (!ucored_client_newseg(cl, &datahdr))
					goto error;
			} else {
				/* Segment is finished. */
				libucore_log(LOG_DEBUG, "Data segment finished");
				cl->cl_datasegs_recvd++;
				SLIST_INSERT_HEAD(&cl->cl_datasegs,
				    cl->cl_curdataseg, cl_entry);
				cl->cl_curdataseg = NULL;

				if (cl->cl_datasegs_recvd == cl->cl_ndatasegs) {
					cl->cl_state = STATE_DONE;
					ucored_client_done(cl);

					/*
					 * We have all of our data, the caller
					 * can drop it; we've cleaned up.
					 */
					return (false);
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
		libucore_log(LOG_ERR, "client prematurely disappeared");
		goto error;
	}

	ucored_now(&cl->cl_lastseen);

	/* Re-arm the kevent because we need more data. */
	return (true);
error:
	if (eof) {
		/* Don't send anymore data! */
		close(cl->cl_fd);
		cl->cl_fd = -1;
	}

	/* Client's invalid, drop it -- the caller must drop it as well. */
	ucored_client_close(cl, false);
	return (false);
}

static const struct ucore *
ucored_client_header(const struct ucore_provider *up)
{
	const struct ucored_client *cl = UCORED_CLIENT_FROM_PROVIDER(up);

	return (&cl->cl_hdr);
}

static size_t
ucored_client_lowat(struct ucore_readable *ur)
{
	struct ucored_client *cl = UCORED_CLIENT_FROM_READABLE(ur);

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

/*
 * Our ucored_client takes description over the socket and leaves the data in
 * a file on-disk, which is what we open here.  Thus, the 'dataonly' hint is not
 * necessary because we will always only open the data-bits.
 */
static int
ucored_client_open_core(const struct ucore_provider *up, bool dataonly __unused)
{
	const struct ucore_data *sd;
	const char *corepath;
	int fd;

	sd = ucored_client_data(up, UDT_PATH);
	if (sd == NULL) {
		libucore_log(LOG_ERR, "Failed to open core; no path provided");
		errno = ENOENT;
		return (-1);
	}

	corepath = &sd->ud_data[0];

	fd = open(corepath, O_RDONLY | O_NOFOLLOW);
	if (fd == -1)
		return (-1);

	return (fd);
}

static void
ucored_client_init_readable(struct ucored_client *cl, int clsock)
{
	struct ucore_readable *ur;

	ur = &cl->cl_readable;
	ur->r_lowat = ucored_client_lowat;
	ur->r_read = ucored_client_fetch;
	ur->r_fd = clsock;
	ur->r_oneshot = true;
}

static void
ucored_client_init_provider(struct ucored_client *cl)
{
	struct ucore_provider *up;

	up = &cl->cl_provider;
	up->p_fetch_data = ucored_client_data;
	up->p_fetch_header = ucored_client_header;
	up->p_open_core = ucored_client_open_core;
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

	ucored_client_init_readable(cl, clsock);
	ucored_client_init_provider(cl);

	SLIST_INSERT_HEAD(&all_clients, cl, cl_client);
	SLIST_INIT(&cl->cl_datasegs);
	ucored_now(&cl->cl_lastseen);

	ucored_watch_socket(kq, &cl->cl_readable);
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

	if (!ucored_lua_handle(&cl->cl_provider)) {
		libucore_log(LOG_ERR, "Core[pid=%d] failed to process",
		    cl->cl_hdr.ucore_pid);
	}

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
		struct ucore_data *ud;
		size_t datasz;

		SLIST_REMOVE_HEAD(&cl->cl_datasegs, cl_entry);

		ud = &cld->cl_data;
		datasz = sizeof(*ud) + ud->ud_hdr.uhdr_size;
		explicit_bzero(ud, datasz);

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
