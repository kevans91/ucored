/*-
 * Copyright (c) 2025 Kyle Evans <kevans@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <sys/types.h>
#include <sys/queue.h>

#define	UCORE_MAGIC	"UCORE0"

struct ucore {
	char		ucore_magic[sizeof(UCORE_MAGIC) - 1];
	struct timespec	ucore_time;
	int64_t		ucore_pad1[4];
	size_t		ucore_datasegs;
	size_t		ucore_size;
	int		ucore_signo;
	int32_t		ucore_pad2[4];
	int		ucore_uid;
	int		ucore_gid;
	int		ucore_jid;
	fflags_t	ucore_fflags;
	pid_t		ucore_ppid;
	pid_t		ucore_pid;
	int8_t		ucore_pad3[8];
	bool		ucore_tainted;
};

struct ucore_ack {
	char		ucore_magic[sizeof(UCORE_MAGIC) - 1];
	/*
	 * I'm not creative enough to know what else to stash here, so we'll
	 * just stuff a bunch of padding in it for now.
	 */
	int64_t		ucore_pad1[4];
	int32_t		ucore_pad2[4];
	int32_t		ucore_status;
	int8_t		ucore_pad3[4];
};

#define	UCORED_MAXSEGS	32		/* Max # data segments */
#define	UCORED_MAXSEGSZ	(1024 * 4)	/* 4k segments at most. */

enum ucore_data_type {
	UDT_COMM = 0,
	UDT_JAIL,
	UDT_JAILROOT,
	UDT_PATH,
	UDT_PWD,
	UDT_HOSTNAME,
	UDT_DOMAINNAME,
};

struct ucore_data_hdr {
	enum ucore_data_type	uhdr_type;
	size_t			uhdr_size;	/* Payload size */
};

struct ucore_data {
	struct ucore_data_hdr		ud_hdr;

	/*
	 * We *might* end up stuffing arbitrary data in here, so bump the
	 * alignment requirement to avoid problems down the road.
	 */
	_Alignas(__max_align_t) uint8_t	ud_data[];
};

#ifdef _KERNEL
struct ucoredev_shmfd {
	STAILQ_ENTRY(ucoredev_shmfd)	entry;

	struct shmfd			*shmfd;
};

MALLOC_DECLARE(M_UCORE);

extern struct coredumper ucoredev_coredumper;

void ucoredev_enqueue(struct ucoredev_shmfd *);
#endif
