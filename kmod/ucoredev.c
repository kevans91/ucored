/*-
 * Copyright (c) 2025 Kyle Evans <kevans@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/conf.h>
#include <sys/event.h>
#include <sys/fcntl.h>
#include <sys/filio.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mman.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/poll.h>
#include <sys/proc.h>
#include <sys/priv.h>
#include <sys/queue.h>
#include <sys/selinfo.h>
#include <sys/syscallsubr.h>
#include <sys/ucoredump.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include "ucoredev.h"

/*
 * Ideally we would have our own reservation, but this is fine.  We specifically
 * do not want a privilege that would be granted to a jail, because we do not
 * do any per-jail filtering here.  Maybe attaching ucore queues to jails with
 * OSDs would be a neat idea, then we allow jailed root to open ucoredev and
 * grab cores for its descendants.  For now, though, only allow unjailed root
 * the honor.
 */
#define	PRIV_UCOREDEV	PRIV_MODULE0

static d_open_t ucoredev_open;
static d_read_t ucoredev_read;
static d_ioctl_t ucoredev_ioctl;
static d_poll_t ucoredev_poll;
static d_kqfilter_t ucoredev_kqfilter;

static struct cdev *ucore_dev;
static struct cdevsw ucore_cdevsw = {
	.d_version =	D_VERSION,
	.d_open =	ucoredev_open,
	.d_read =	ucoredev_read,
	.d_ioctl =	ucoredev_ioctl,
	.d_poll =	ucoredev_poll,
	.d_kqfilter =	ucoredev_kqfilter,
	.d_name =	"ucore",
};

static struct ucoredev_softc {
	STAILQ_HEAD(, ucoredev_shmfd)	ucores;
	struct mtx			mtx;
	struct cv			wakeup;
	struct selinfo			sel;
	size_t				ucoresz;
	int				flags;
} usoftc;

#define	USOFTC_OPENED		0x0001
#define	USOFTC_UNLOADING	0x0002

MTX_SYSINIT(ucorelock, &usoftc.mtx, "ucore list lock", MTX_DEF);

#define	UCORE_LOCK()	mtx_lock(&usoftc.mtx)
#define	UCORE_UNLOCK()	mtx_unlock(&usoftc.mtx)
#define	UCORE_LOCK_ASSERT()	mtx_assert(&usoftc.mtx, MA_OWNED)

static int ucoredev_kqread(struct knote *kn, long hint);
static void ucoredev_kqdetach(struct knote *kn);

static const struct filterops ucoredev_read_filterops = {
	.f_isfd =	1,
	.f_attach =	NULL,
	.f_detach =	ucoredev_kqdetach,
	.f_event =	ucoredev_kqread,
};

static void ucoredev_shmfd_free(struct ucoredev_shmfd *);

MALLOC_DEFINE(M_UCORE, "ucorebufs", "ucore descriptor buffers");

static void
ucoredevdtor(void *data)
{

	knlist_clear(&usoftc.sel.si_note, 0);
	seldrain(&usoftc.sel);
	knlist_destroy(&usoftc.sel.si_note);

	UCORE_LOCK();
	usoftc.flags &= ~USOFTC_OPENED;
	UCORE_UNLOCK();
}

static int
ucoredev_open(struct cdev *dev, int flags, int mode, struct thread *td)
{
	int error;

	if ((error = priv_check(td, PRIV_UCOREDEV)) != 0)
		return (error);

	UCORE_LOCK();
	if ((usoftc.flags & (USOFTC_OPENED | USOFTC_UNLOADING)) != 0) {
		UCORE_UNLOCK();
		return (EBUSY);
	}

	usoftc.flags |= USOFTC_OPENED;
	knlist_init_mtx(&usoftc.sel.si_note, &usoftc.mtx);
	UCORE_UNLOCK();

	devfs_set_cdevpriv(&usoftc, ucoredevdtor);

	return (0);
}

static int
ucoredev_read(struct cdev *dev __unused, struct uio *uio, int flags)
{
	struct ucoredev_shmfd *next;
	struct thread *td = curthread;
	size_t out = 0;
	int error = 0, fd;

	UCORE_LOCK();
	while (error == 0 && uio->uio_resid > 0) {
		if (STAILQ_EMPTY(&usoftc.ucores)) {
			if ((flags & IO_NDELAY) != 0) {
				UCORE_UNLOCK();
				return (EWOULDBLOCK);
			}

			error = cv_wait_sig(&usoftc.wakeup, &usoftc.mtx);
			if (error != 0) {
				UCORE_UNLOCK();
				return (error);
			}

			continue;
		}

		next = STAILQ_FIRST(&usoftc.ucores);
		STAILQ_REMOVE_HEAD(&usoftc.ucores, entry);
		usoftc.ucoresz--;
		UCORE_UNLOCK();

		error = kern_shm_open2(td, SHM_ANON, O_RDONLY, 0400, 0, NULL,
		    NULL, next->shmfd);
		if (error != 0) {
			UCORE_LOCK();
			STAILQ_INSERT_HEAD(&usoftc.ucores, next, entry);
			usoftc.ucoresz++;

			/*
			 * If we managed to tap out *any* cores, then we'll call
			 * this a success and let the caller deal with those.
			 */
			if (out > 0)
				error = 0;

			printf("%s: shm_open2 error == %d\n", __func__, error);
			break;
		}

		fd = td->td_retval[0];
		MPASS(fd >= 0);

		error = uiomove(&fd, sizeof(int), uio);

		/*
		 * If we materialized the file but uiomove() failed for some reason, we
		 * will simply drop the newly allocated fd and continue tracking this
		 * shmfd as one we need to tap out.
		 */
		if (error != 0) {
			(void)kern_close(td, fd);
			UCORE_LOCK();
			usoftc.ucoresz++;
			STAILQ_INSERT_HEAD(&usoftc.ucores, next, entry);
			break;
		}

		out++;
		ucoredev_shmfd_free(next);
		UCORE_LOCK();
	}

	UCORE_UNLOCK();
	return (error);
}

/*
 * Clamp the # cores available to the type that we need to eventually squeeze
 * this into; the result is expected to be multiplied by sizeof(int).
 */
static size_t
ucoredev_cores_available(uintmax_t type_max)
{
	UCORE_LOCK_ASSERT();
	return (MIN(type_max / sizeof(int), usoftc.ucoresz));
}

static int
ucoredev_ioctl(struct cdev *dev __unused, u_long cmd, caddr_t data,
    int flags __unused, struct thread *td __unused)
{

	switch (cmd) {
	case FIONREAD:
		UCORE_LOCK();
		*(int *)data = ucoredev_cores_available(INT_MAX) * sizeof(int);
		UCORE_UNLOCK();

		return (0);
	}

	return (ENOIOCTL);
}

static int
ucoredev_poll(struct cdev *dev, int events, struct thread *td)
{
	int revents = 0;

	/* We only support reading from /dev/ucore. */
	if ((events & (POLLIN | POLLRDNORM)) != 0) {
		UCORE_LOCK();
		if (usoftc.ucoresz != 0)
			revents = events & (POLLIN | POLLRDNORM);
		else
			selrecord(td, &usoftc.sel);
		UCORE_UNLOCK();
	}

	return (revents);
}

static int
ucoredev_kqfilter(struct cdev *dev, struct knote *kn)
{
	if (kn->kn_filter != EVFILT_READ)
		return (EINVAL);

	kn->kn_fop = &ucoredev_read_filterops;
	kn->kn_hook = NULL;

	UCORE_LOCK();
	knlist_add(&usoftc.sel.si_note, kn, 1);
	UCORE_UNLOCK();
	return (0);
}

static int
ucoredev_kqread(struct knote *kn, long hint)
{
	UCORE_LOCK_ASSERT();
	kn->kn_data = ucoredev_cores_available(INT64_MAX) * sizeof(int);
	return (kn->kn_data != 0);
}

static void
ucoredev_kqdetach(struct knote *kn)
{
	UCORE_LOCK();
	knlist_remove(&usoftc.sel.si_note, kn, 1);
	UCORE_UNLOCK();
}

static void
ucoredev_shmfd_free(struct ucoredev_shmfd *ucs)
{

	shm_drop(ucs->shmfd);
	free(ucs, M_UCORE);
}

void
ucoredev_enqueue(struct ucoredev_shmfd *ucshm)
{
	UCORE_LOCK();
	STAILQ_INSERT_TAIL(&usoftc.ucores, ucshm, entry);
	usoftc.ucoresz++;

	KNOTE_LOCKED(&usoftc.sel.si_note, 0);
	selwakeup(&usoftc.sel);
	cv_broadcast(&usoftc.wakeup);
	UCORE_UNLOCK();
}

static int
ucoredev_modevent(module_t mod __unused, int type, void *data __unused)
{
	switch(type) {
	case MOD_LOAD:
		STAILQ_INIT(&usoftc.ucores);
		cv_init(&usoftc.wakeup, "ucores");
		ucore_dev = make_dev_credf(MAKEDEV_ETERNAL_KLD, &ucore_cdevsw,
		    0, NULL, UID_ROOT, GID_WHEEL, 0400, "ucore");
		coredumper_register(&ucoredev_coredumper);
		break;

	case MOD_UNLOAD:
		UCORE_LOCK();
		if ((usoftc.flags & USOFTC_OPENED) != 0) {
			UCORE_UNLOCK();

			return (EBUSY);
		}

		/* Block further opens until the device is destroyed. */
		usoftc.flags |= USOFTC_UNLOADING;
		UCORE_UNLOCK();

		destroy_dev(ucore_dev);
		coredumper_unregister(&ucoredev_coredumper);
		cv_destroy(&usoftc.wakeup);

		UCORE_LOCK();
		while (!STAILQ_EMPTY(&usoftc.ucores)) {
			struct ucoredev_shmfd *ucs;

			ucs = STAILQ_FIRST(&usoftc.ucores);
			STAILQ_REMOVE_HEAD(&usoftc.ucores, entry);

			ucoredev_shmfd_free(ucs);
			usoftc.ucoresz--;
		}

		MPASS(usoftc.ucoresz == 0);
		UCORE_UNLOCK();
		return (0);

	case MOD_SHUTDOWN:
		break;

	default:
		return (EOPNOTSUPP);
	}

	return (0);
}

DEV_MODULE(ucoredev, ucoredev_modevent, NULL);
MODULE_VERSION(ucoredev, 1);
