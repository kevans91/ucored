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

typedef const struct ucore_data *
    ucore_fetch_data_fn(const struct ucore_provider *, enum ucore_data_type);
typedef const struct ucore *
    ucore_fetch_header_fn(const struct ucore_provider *);
typedef int ucore_open_core_fn(const struct ucore_provider *);

struct ucore_provider {
	void			*p_ctx;
	ucore_fetch_data_fn	*p_fetch_data;
	ucore_fetch_header_fn	*p_fetch_header;
	ucore_open_core_fn	*p_open_core;
};

bool libucore_send_data(int fd, const void *payload, size_t payloadsz);
bool libucore_read_data(int fd, void *payload, size_t payloadsz);

void libucore_set_debug(bool dbg);
void libucore_set_verbose(int verbose);
void libucore_log(int priority, const char *fmt, ...) __printflike(2, 3);
