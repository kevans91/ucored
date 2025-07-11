/*-
 * Copyright (c) 2025 Kyle Evans <kevans@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdbool.h>

bool libucore_send_data(int fd, const void *payload, size_t payloadsz);
bool libucore_read_data(int fd, void *payload, size_t payloadsz);
