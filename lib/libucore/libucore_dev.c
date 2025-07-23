/*-
 * Copyright (c) 2025 Kyle Evans <kevans@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/conf.h>
#include <sys/ioctl.h>
#include <sys/linker.h>
#include <sys/mman.h>
#include <sys/module.h>
#include <sys/stat.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libucore.h"

#define	_PATH_DEVUCORE	"/dev/ucore"

struct ucore_dev {
	struct ucore_readable	 dev_readable;
	ucore_handle_fn		*dev_handler;
};

struct ucore_dev_core {
	struct ucore_provider			 core_provider;
	const struct ucore			*core_hdr;
	const struct ucore_data			**core_datasegs;
	void					 *core_map;
	off_t					 core_datapos;
	size_t					 core_datasz;
	size_t					 core_mapsz;
	size_t					 core_ndatasegs;
	int					 core_fd;
};

#define	UCORE_DEV_FROM(ur)	\
	__containerof(ur, struct ucore_dev, dev_readable)
#define	UCORE_DEV_CORE_FROM(up)	\
	__containerof(up, struct ucore_dev_core, core_provider)

static void libucore_dev_core_free(struct ucore_dev_core *);
static struct ucore_dev_core *libucore_dev_core_decode(int);

#ifndef O_CLOFORK
#define	O_CLOFORK	0
#endif

bool
libucore_dev_available(bool loadit)
{
	struct stat sb;

	if (modfind("ucoredev") == -1) {
		/*
		 * Some callers, like ucored(8), may want to go ahead and let us
		 * load it because it was clearly intended to be used.
		 */
		if (errno != ENOENT || !loadit)
			return (false);
		if (kldload("ucoredev") == -1)
			return (false);
	}

	/*
	 * We still want to do a basic sanity check of the device we're about
	 * to open, though.
	 */
	if (stat(_PATH_DEVUCORE, &sb) == -1)
		return (false);
	return (S_ISCHR(sb.st_mode));
}

static bool
libucore_dev_read(struct ucore_readable *ur, size_t avail, bool eof __unused)
{
	struct ucore_dev *udev = UCORE_DEV_FROM(ur);
	struct ucore_dev_core *core;
	int corefd;

	if (avail == (size_t)-1) {
		int nb;

		if (ioctl(ur->r_fd, FIONREAD, &nb) == -1) {
			libucore_log(LOG_ERR, "ioctl: %m");
			goto failed;
		}

		assert(nb >= 0);
		avail = nb;
	}
	assert((avail % sizeof(int)) == 0);
	while (avail != 0) {
		if (!libucore_read_data(ur->r_fd, &corefd, sizeof(int)))
			goto failed;

		avail -= sizeof(int);
		assert(corefd >= 0);

		libucore_log(LOG_INFO, "process fd %d", corefd);

		/* The core owns the corefd now, it will be closed at free. */
		core = libucore_dev_core_decode(corefd);
		if (core != NULL) {
			libucore_log(LOG_INFO, "Received a core with %zu segments, core size %zu",
			    core->core_ndatasegs, core->core_datasz);
		}

		/* Call handle callback */
		if (!(*udev->dev_handler)(&core->core_provider))
			libucore_log(LOG_ERR, "Core failed to process");

		libucore_dev_core_free(core);
	}

	return (true);
failed:
	/*
	 * XXX Should we terminate?  I don't know, but this is
	 * probably not a transient condition and termination to
	 * make it clear we're not doing anything seems... not
	 * unreasonable.
	 */
	raise(SIGTERM);
	return (true);
}

struct ucore_dev *
libucore_dev_open(ucore_handle_fn *ucore_handler)
{
	struct ucore_dev *udev;
	struct ucore_readable *ur;
	int fd;

	assert(ucore_handler != NULL);

	fd = open(_PATH_DEVUCORE, O_RDONLY | O_CLOFORK);
	if (fd == -1) {
		libucore_log(LOG_ERR, "open: %m");
		return (NULL);
	}

	udev = calloc(1, sizeof(*udev));
	if (udev == NULL) {
		libucore_log(LOG_ERR, "calloc: %m");

		close(fd);
		return (NULL);
	}

	udev->dev_handler = ucore_handler;
	ur = &udev->dev_readable;
	ur->r_read = libucore_dev_read;
	ur->r_fd = fd;
	return (udev);
}

struct ucore_readable *
libucore_dev_readable(struct ucore_dev *udev)
{
	return (&udev->dev_readable);
}

void
libucore_dev_close(struct ucore_dev *udev)
{

	if (udev == NULL)
		return;

	close(udev->dev_readable.r_fd);
	free(udev);
}

static void
libucore_dev_core_free(struct ucore_dev_core *core)
{
	if (core == NULL)
		return;

	if (core->core_map != MAP_FAILED)
		munmap(core->core_map, core->core_mapsz);
	close(core->core_fd);
	free(core->core_datasegs);
	free(core);
}

static const struct ucore_data *
libucore_dev_core_fetch_data(const struct ucore_provider *up,
    enum ucore_data_type type)
{
	struct ucore_dev_core *core = UCORE_DEV_CORE_FROM(up);

	for (size_t i = 0; i < core->core_ndatasegs; i++) {
		const struct ucore_data *ud = core->core_datasegs[i];

		if (ud->ud_hdr.uhdr_type == type)
			return (ud);
	}

	return (NULL);
}

static const struct ucore *
libucore_dev_core_fetch_header(const struct ucore_provider *up)
{
	struct ucore_dev_core *core = UCORE_DEV_CORE_FROM(up);

	return (core->core_hdr);
}

static int
libucore_dev_core_open_core(const struct ucore_provider *up)
{
	struct ucore_dev_core *core = UCORE_DEV_CORE_FROM(up);
	int nfd;

	nfd = dup(core->core_fd);
	if (nfd == -1) {
		libucore_log(LOG_ERR, "dup: %m");
		return (-1);
	}

	/* Reset position in case we re-opened it. */
	(void)lseek(nfd, core->core_datapos, SEEK_SET);
	return (nfd);
}

static void
libucore_dev_core_init_provider(struct ucore_dev_core *core)
{
	struct ucore_provider *up = &core->core_provider;

	up->p_fetch_data = libucore_dev_core_fetch_data;
	up->p_fetch_header = libucore_dev_core_fetch_header;
	up->p_open_core = libucore_dev_core_open_core;
}

static struct ucore_dev_core *
libucore_dev_core_decode(int fd)
{
	struct stat sb;
	struct ucore_dev_core *core;
	const struct ucore *hdr;
	const struct ucore_data **coredata;
	void *map = MAP_FAILED;
	const uint8_t *pend, *walker;
	size_t coresz;

	if (fstat(fd, &sb) == -1) {
		libucore_log(LOG_ERR, "fstat: %m");
		close(fd);
		return (NULL);
	} else if (sb.st_size <= (off_t)sizeof(struct ucore)) {
		libucore_log(LOG_ERR, "ucore too short to be valid");
		close(fd);
		return (NULL);
	}

	coresz = sb.st_size;
	core = calloc(1, sizeof(*core));
	if (core == NULL) {
		libucore_log(LOG_ERR, "calloc: %m");
		close(fd);
		return (NULL);
	}

	core->core_map = MAP_FAILED;
	core->core_fd = fd;
	map = mmap(NULL, coresz, PROT_READ, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED) {
		libucore_log(LOG_ERR, "mmap: %m");
		goto invalid;
	}

	core->core_map = map;
	core->core_mapsz = coresz;
	pend = (uint8_t *)map + coresz;
	walker = map;

#define	ADVANCE(ptr, size) do {				\
	if ((size_t)(pend - (ptr)) < (size)) {			\
		libucore_log(LOG_ERR, "shmfd too short -- drop");	\
		goto invalid;				\
	}						\
	(ptr) += (size);				\
} while(0)

	/* Peel the header off, first. */
	hdr = core->core_hdr = (const void *)walker;
	ADVANCE(walker, sizeof(*hdr));
	if (memcmp(hdr->ucore_magic, UCORE_MAGIC,
	    strlen(UCORE_MAGIC)) != 0) {
		libucore_log(LOG_ERR, "shmfd missing magic header -- drop");
		goto invalid;
	} else if (hdr->ucore_datasegs > UCORED_MAXSEGS) {
		libucore_log(LOG_ERR, "%zu datasegs more than max (%d) -- drop",
		    hdr->ucore_datasegs, UCORED_MAXSEGS);
		goto invalid;
	}

	core->core_ndatasegs = hdr->ucore_datasegs;
	coredata = calloc(hdr->ucore_datasegs, sizeof(*coredata));
	if (coredata == NULL) {
		libucore_log(LOG_ERR, "calloc: %m");
		goto invalid;
	}

	core->core_datasegs = coredata;
	for (size_t i = 0; i < hdr->ucore_datasegs; i++) {
		const struct ucore_data_hdr *dhdr;
		const struct ucore_data *ud;

		coredata[i] = ud = (const void *)walker;
		dhdr = &ud->ud_hdr;

		if (dhdr->uhdr_size > UCORED_MAXSEGSZ) {
			libucore_log(LOG_ERR, "%zu segment size larger than max (%d) -- drop",
			    dhdr->uhdr_size, UCORED_MAXSEGSZ);
			goto invalid;
		}

		ADVANCE(walker, sizeof(*ud) + dhdr->uhdr_size);
	}

	core->core_datapos = walker - (uint8_t *)map;
	core->core_datasz = coresz - core->core_datapos;

	libucore_dev_core_init_provider(core);

	return (core);
invalid:
	libucore_dev_core_free(core);
	return (NULL);
}
