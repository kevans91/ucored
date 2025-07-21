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

bool libucore_send_data(int fd, const void *payload, size_t payloadsz);
bool libucore_read_data(int fd, void *payload, size_t payloadsz);

void libucore_set_debug(bool dbg);
void libucore_set_verbose(int verbose);
void libucore_log(int priority, const char *fmt, ...) __printflike(2, 3);
