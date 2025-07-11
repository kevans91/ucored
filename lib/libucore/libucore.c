/*-
 * Copyright (c) 2025 Kyle Evans <kevans@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/types.h>

#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>
#include <syslog.h>

#include "libucore.h"

bool
libucore_send_data(int fd, const void *payload, size_t payloadsz)
{
	while (payloadsz != 0) {
		ssize_t writesz;

		assert(payloadsz != 0);
		writesz = write(fd, payload, payloadsz);
		if (writesz < 0) {
			if (errno == EINTR)
				continue;

			syslog(LOG_ERR, "write: %m");
			return (false);
		} else if (writesz == 0) {
			syslog(LOG_ERR, "write: premature EOF");
			return (false);
		}

		payload = (const uint8_t *)payload + writesz;
		payloadsz -= writesz;
	}

	return (true);
}

bool
libucore_read_data(int fd, void *payload, size_t payloadsz)
{
	while (payloadsz != 0) {
		ssize_t ret;

		ret = read(fd, payload, payloadsz);
		if (ret == -1) {
			if (errno == EINTR)
				continue;

			syslog(LOG_ERR, "read: %m");
			return (false);
		} else if (ret == 0) {
			syslog(LOG_ERR, "read: EOF while awaiting ack");
			return (false);
		}

		payload = (uint8_t *)payload + ret;
		payloadsz -= ret;
	}

	return (true);
}
