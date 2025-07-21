/*-
 * Copyright (c) 2025 Kyle Evans <kevans@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdbool.h>
#include <syslog.h>

#include "coredump_ucored.h"

#define	PATH_UCORED_SOCK	"/var/run/ucored.sock"

struct ucore_provider;
struct ucore_readable;

/* Fetches data described by the type, if available.  Returns NULL otherwise. */
typedef const struct ucore_data *
    ucore_fetch_data_fn(const struct ucore_provider *, enum ucore_data_type);
/* Fetches the ucore header belonging to this provider.  Cannot fail. */
typedef const struct ucore *
    ucore_fetch_header_fn(const struct ucore_provider *);
/* Returns an fd for the core belonging to this provider, or -1 on error. */
typedef int ucore_open_core_fn(const struct ucore_provider *);

struct ucore_provider {
	ucore_fetch_data_fn	*p_fetch_data;
	ucore_fetch_header_fn	*p_fetch_header;
	ucore_open_core_fn	*p_open_core;
};

/*
 * Returns true if we should continue watching this fd, false if it we should
 * drop it.
 */
typedef bool ucore_read_fn(struct ucore_readable *, size_t, bool);
/* Returns the low-watermark requested for this socket (optional) */
typedef size_t ucore_lowat_fn(struct ucore_readable *);

struct ucore_readable {
	ucore_lowat_fn		*r_lowat;
	ucore_read_fn		*r_read;
	int			 r_fd;
	bool			 r_oneshot;
};

bool libucore_send_data(int fd, const void *payload, size_t payloadsz);
bool libucore_read_data(int fd, void *payload, size_t payloadsz);

void libucore_set_debug(bool dbg);
void libucore_set_verbose(int verbose);
void libucore_log(int priority, const char *fmt, ...) __printflike(2, 3);
