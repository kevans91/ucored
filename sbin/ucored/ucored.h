/*-
 * Copyright (c) 2025 Kyle Evans <kevans@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef UCORED_H
#define	UCORED_H

#include <sys/param.h>

#define	PATH_UCORED_SOCK	"/var/run/ucored.sock"

#define	UCORE_MAGIC	"UCORE0"

struct ucore {
	char		ucore_magic[sizeof(UCORE_MAGIC) - 1];
	uint64_t	ucore_pad1[4];
	size_t		ucore_datasegs;
	int		ucore_signo;
	int32_t		ucore_pad2[4];
	int		ucore_jid;
	pid_t		ucore_ppid;
	pid_t		ucore_pid;
	int8_t		ucore_pad3[8];
};

#define	UCORED_MAXSEGS	32		/* Max # data segments */
#define	UCORED_MAXSEGSZ	(1024 * 4)	/* 4k segments at most. */

enum ucore_data_type {
	UDT_COMM = 0,
	UDT_JAIL,
	UDT_JAILROOT,
	UDT_PATH,
};

struct ucore_data_hdr {
	enum ucore_data_type	uhdr_type;
	size_t			uhdr_size;	/* Payload size */
};

struct ucore_data {
	struct ucore_data_hdr	ud_hdr;
	uint8_t			ud_data[];
};

#endif	/* UCORED_H */
