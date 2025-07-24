/*-
 * Copyright (c) 2025 Kyle Evans <kevans@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>

#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <strings.h>

#include "libucore.h"

static bool libucore_dbg;
static int libucore_verbose;

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

void
libucore_set_debug(bool dbg)
{
	libucore_dbg = dbg;
}

void
libucore_set_verbose(int verbose)
{
	libucore_verbose = verbose;
}

void
libucore_log(int priority, const char *fmt, ...)
{
	va_list ap;

	if (priority == LOG_INFO && libucore_verbose < 1)
		return;
	if (priority == LOG_DEBUG && libucore_verbose < 2)
		return;

	/*
	 * Note that a lot of our format strings use %m, so we need to be
	 * careful to save/restore errno if we're going to do something that
	 * might disturb it before stdio(3)/syslog(3) has a chance to grab it.
	 */
	va_start(ap, fmt);
	if (libucore_dbg) {
		FILE *fp;

		switch (priority) {
		case LOG_NOTICE:
		case LOG_INFO:
		case LOG_DEBUG:
			fp = stdout;
			break;
		case LOG_ERR:
		case LOG_WARNING:
		default:
			fp = stderr;
			break;
		}

		vfprintf(fp, fmt, ap);
		/* syslog messages omit the newline; just toss one in. */
		fputc('\n', fp);
	} else {
		vsyslog(priority, fmt, ap);
	}
	va_end(ap);
}

static bool
libucore_copy_file_fallback(int fromfd, int tofd, off_t fsize)
{
	/*
	 * We do parallel processing of cores, but only via fork() + process, so
	 * this global buffer is still fine.
	 */
	static char buf[MAXPHYS];
	size_t bufsz = sizeof(buf);
	off_t copied = 0;

	while (copied < fsize) {
		size_t clipsz = MIN(fsize - copied, (off_t)bufsz);

		if (!libucore_read_data(fromfd, buf, clipsz))
			break;

		if (!libucore_send_data(tofd, buf, clipsz))
			break;

		copied += clipsz;
	}

	/*
	 * Given that we're copying cores from varying privilege levels, let's
	 * be very careful to avoid any mishaps across handling of different
	 * dumps.  Usually we process them in a fork(), but if fork() failed
	 * then ucored(8) will process them in the main process to avoid a
	 * service disruption since we can't really return a ucore to sender.
	 *
	 * Perhaps ucoredev(4) should grow a mechanism to 'ack' a core as
	 * handled so that we can re-attempt it if the failure is transient?
	 */
	explicit_bzero(buf, sizeof(buf));
	return (copied == fsize);
}

bool
libucore_copy_file(int fromfd, int tofd, off_t fsize)
{
	off_t copied = 0;
	off_t fromoff, tooff = 0;

	/* We must assume the caller wants us to copy from the current position. */
	fromoff = lseek(fromfd, 0, SEEK_CUR);
	while (copied < fsize) {
		ssize_t ret;

		ret = copy_file_range(fromfd, &fromoff, tofd, &tooff,
		    fsize, 0);
		if (ret == -1) {
			if (errno == EINTR)
				continue;

			/*
			 * EINVAL probably means we're copying from a shmfd
			 * rather than a file, which is fine.  We'll just
			 * fallback to a manual read/write loop.
			 */
			if (errno == EINVAL && copied == 0)
				return (libucore_copy_file_fallback(fromfd,
				    tofd, fsize));

			return (false);
		} else if (ret == 0) {
			/*
			 * Truncation is maybe sketchy, but we'll give it a
			 * pass anyways.
			 */
			return (true);
		}

		/* Excellent, making progress. */
		copied += ret;
	}

	return (true);
}
