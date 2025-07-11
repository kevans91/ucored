/*-
 * Copyright (c) 2025 Kyle Evans <kevans@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef UCORED_H
#define	UCORED_H

#include <sys/param.h>
#include <sys/queue.h>

#include <stdbool.h>
#include <syslog.h>

#include "libucore.h"

enum ucored_state {
	STATE_HDR = 0,
	STATE_DATASEGS,
	STATE_DONE,
};

struct ucored_client_data {
	SLIST_ENTRY(ucored_client_data)	cl_entry;
	struct ucore_data		cl_data;
};

struct ucored_client {
	struct ucore				cl_hdr;
	SLIST_ENTRY(ucored_client)		cl_client;
	SLIST_HEAD(,  ucored_client_data)	cl_datasegs;
	struct ucored_client_data		*cl_curdataseg;
	size_t					cl_ndatasegs;
	size_t					cl_datasegs_recvd;
	struct timespec				cl_lastseen;
	int					cl_fd;
	enum ucored_state			cl_state;
};

/* ucored.c */
void ucored_log(int, const char *, ...) __printflike(2, 3);

/* ucored_lua.c */
bool ucored_lua_init(void);
bool ucored_lua_handle(struct ucored_client *);

#endif	/* UCORED_H */
