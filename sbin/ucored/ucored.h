/*-
 * Copyright (c) 2025 Kyle Evans <kevans@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef UCORED_H
#define	UCORED_H

#include <sys/param.h>
#include <sys/queue.h>

#include <signal.h>
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

struct ucored_client;

/* ucored.c */
extern sig_atomic_t ucored_terminate;

void ucored_now(struct timespec *);
int ucored_watch_socket(int, int, struct ucored_client *);

/* ucored_client.c */
const struct ucored_client_data *ucored_client_data(const struct ucored_client *,
    enum ucore_data_type);
void ucored_client_fetch(struct ucored_client *, int, size_t, bool);
size_t ucored_client_lowat(struct ucored_client *);
const struct ucore *ucored_client_header(const struct ucored_client *);
bool ucored_client_newseg(struct ucored_client *, struct ucore_data_hdr *);
struct ucored_client *ucored_client_alloc(int, int);
void ucored_client_done(struct ucored_client *);
void ucored_client_close(struct ucored_client *, bool);
void ucored_client_close_all(void);

/* ucored_lua.c */
bool ucored_lua_init(void);
bool ucored_lua_handle(struct ucored_client *);

#endif	/* UCORED_H */
