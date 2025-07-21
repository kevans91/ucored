/*-
 * Copyright (c) 2025 Kyle Evans <kevans@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/conf.h>
#include <sys/event.h>
#include <sys/exec.h>
#include <sys/filio.h>
#include <sys/imgact.h>
#include <sys/fcntl.h>
#include <sys/jail.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mman.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/poll.h>
#include <sys/proc.h>
#include <sys/resourcevar.h>
#include <sys/sched.h>
#include <sys/selinfo.h>
#include <sys/syscallsubr.h>
#include <sys/sysent.h>
#include <sys/ucred.h>
#include <sys/uio.h>
#include <sys/unistd.h>
#include <sys/queue.h>

#include "coredump_ucored.h"

static d_open_t ucoredev_open;
static d_read_t ucoredev_read;
static d_ioctl_t ucoredev_ioctl;
static d_poll_t ucoredev_poll;
static d_kqfilter_t ucoredev_kqfilter;
static d_close_t ucoredev_close;

static struct cdev *ucore_dev;
static struct cdevsw ucore_cdevsw = {
	.d_version =	D_VERSION,
	.d_open =	ucoredev_open,
	.d_close =	ucoredev_close,
	.d_read =	ucoredev_read,
	.d_ioctl =	ucoredev_ioctl,
	.d_poll =	ucoredev_poll,
	.d_kqfilter =	ucoredev_kqfilter,
	.d_name =	"ucore",
};

struct ucoredev_shmfd {
	STAILQ_ENTRY(ucoredev_shmfd)	entry;

	struct shmfd			*shmfd;
};

struct coredump_ucore_ctx {
	struct shmfd			*shmfd;
};

struct selinfo ucoredev_sel;
static int ucoredev_kqread(struct knote *kn, long hint);
static void ucoredev_kqdetach(struct knote *kn);

static const struct filterops ucoredev_read_filterops = {
	.f_isfd =	1,
	.f_attach =	NULL,
	.f_detach =	ucoredev_kqdetach,
	.f_event =	ucoredev_kqread,
};

static coredump_write_fn coredump_shmwrite;
static coredump_extend_fn coredump_shmextend;

static coredumper_probe_fn coredump_ucored_probe;
static coredumper_handle_fn coredump_ucored;
COREDUMP_HANDLER(coredump_ucored, coredump_ucored_probe, coredump_ucored);

static MALLOC_DEFINE(M_UCORE, "ucorebufs", "ucore descriptor buffers");

static STAILQ_HEAD(, ucoredev_shmfd) ucores = STAILQ_HEAD_INITIALIZER(ucores);
static size_t ucoresz;

static struct mtx ucoredev_mtx;
MTX_SYSINIT(ucorelock, &ucoredev_mtx, "ucore list lock", MTX_DEF);

#define	UCORE_LOCK()	mtx_lock(&ucoredev_mtx)
#define	UCORE_UNLOCK()	mtx_unlock(&ucoredev_mtx)
#define	UCORE_LOCK_ASSERT()	mtx_assert(&ucoredev_mtx, MA_OWNED)

static void ucoredev_shmfd_free(struct ucoredev_shmfd *);

static int
ucoredev_open(struct cdev *dev, int flags, int mode, struct thread *td)
{
	/* XXX Only single open() */
	return (0);
}

static int
ucoredev_read(struct cdev *dev __unused, struct uio *uio, int flags __unused)
{
	struct ucoredev_shmfd *next;
	struct thread *td = curthread;
	size_t out = 0;
	int error = 0, fd;

	UCORE_LOCK();
	if (STAILQ_EMPTY(&ucores)) {	/* XXX */
		UCORE_UNLOCK();
		return (EWOULDBLOCK);
	}

	while (error == 0 && uio->uio_resid > 0) {
		next = STAILQ_FIRST(&ucores);
		STAILQ_REMOVE_HEAD(&ucores, entry);
		ucoresz--;
		UCORE_UNLOCK();

		error = kern_shm_open2(td, SHM_ANON, O_RDONLY, 0400, 0, NULL,
		    NULL, next->shmfd);
		if (error != 0) {
			UCORE_LOCK();
			STAILQ_INSERT_HEAD(&ucores, next, entry);
			ucoresz++;

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

		printf("%s: uiomove error == %d\n", __func__, error);

		/*
		 * If we materialized the file but uiomove() failed for some reason, we
		 * will simply drop the newly allocated fd and continue tracking this
		 * shmfd as one we need to tap out.
		 */
		if (error != 0) {
			(void)kern_close(td, fd);
			UCORE_LOCK();
			ucoresz++;
			STAILQ_INSERT_HEAD(&ucores, next, entry);
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
	return (MIN(type_max / sizeof(int), ucoresz));
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
ucoredev_close(struct cdev *dev __unused, int flag __unused, int mode __unused,
    struct thread *td __unused)
{
	/* XXX */
	return (0);
}

static int
ucoredev_poll(struct cdev *dev, int events, struct thread *td)
{
	int revents = 0;

	/* We only support reading from /dev/ucore. */
	if ((events & (POLLIN | POLLRDNORM)) != 0) {
		UCORE_LOCK();
		if (ucoresz != 0)
			revents = events & (POLLIN | POLLRDNORM);
		else
			selrecord(td, &ucoredev_sel);
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
	knlist_add(&ucoredev_sel.si_note, kn, 1);
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
	knlist_remove(&ucoredev_sel.si_note, kn, 1);
	UCORE_UNLOCK();
}

static void
ucoredev_shmfd_free(struct ucoredev_shmfd *ucs)
{

	shm_drop(ucs->shmfd);
	free(ucs, M_UCORE);
}

static int
coredump_shmwrite(const struct coredump_writer *cdw, const void *base,
    size_t len, off_t offset, enum uio_seg seg, struct ucred *cred,
    size_t *resid, struct thread *td)
{
	struct coredump_ucore_ctx *uctx = cdw->ctx;
	struct iovec iov;
	struct uio uio;
	int error;

	error = coredump_shmextend(cdw, len + offset, cred);
	if (error != 0)
		return (error);

	iov.iov_base = __DECONST(void *, base);
	iov.iov_len = len;

	uio.uio_iov = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_offset = offset;
	uio.uio_resid = len;
	uio.uio_segflg = seg;
	uio.uio_rw = UIO_WRITE;
	uio.uio_td = td;

	return (uiomove_object(uctx->shmfd->shm_object, len + offset, &uio));
}

static int
coredump_shmextend(const struct coredump_writer *cdw, off_t newsz,
    struct ucred *ucred __unused)
{
	struct coredump_ucore_ctx *uctx = cdw->ctx;

	MPASS(newsz > uctx->shmfd->shm_size);
	return (shm_dotruncate(uctx->shmfd, newsz));
}

static int
coredump_ucored_probe(struct thread *td)
{
	return (COREDUMPER_SPECIAL);
}

static int
coredump_ucored(struct thread *td, off_t limit)
{
	struct coredump_ucore_ctx uctx;
	struct coredump_writer cdw;
	struct ucoredev_shmfd *ucshm;
	struct shmfd *shm;
	struct proc *p;
#if 1
	int error;
#else
	int error, jid, ppid, sig;
#endif

	p = td->td_proc;
	PROC_LOCK_ASSERT(p, MA_OWNED);

#if 0
	ppid = p->p_oppid;
	sig = p->p_sig;
	jid = p->p_ucred->cr_prison->pr_id;
#endif
	PROC_UNLOCK(p);

	/* XXX Should this borrow against a global coredump_ucore ucred? */
	shm = shm_alloc(p->p_ucred, O_RDONLY, false);
	MPASS(shm != NULL);

	uctx.shmfd = shm;

	cdw.ctx = &uctx;
	cdw.write_fn = coredump_shmwrite;
	cdw.extend_fn = coredump_shmextend;

	if (p->p_sysent->sv_coredump != NULL)
		error = p->p_sysent->sv_coredump(td, &cdw, limit, 0);
	else
		error = ENOSYS;

	if (error != 0) {
		shm_drop(shm);
		return (error);
	}

	ucshm = malloc(sizeof(*ucshm), M_UCORE, M_WAITOK | M_ZERO);
	ucshm->shmfd = shm;

	UCORE_LOCK();
	STAILQ_INSERT_TAIL(&ucores, ucshm, entry);
	ucoresz++;

	KNOTE_LOCKED(&ucoredev_sel.si_note, 0);
	selwakeup(&ucoredev_sel);
	UCORE_UNLOCK();

	return (error);
}

static int
coredump_ucored_modevent(module_t mod __unused, int type, void *data __unused)
{
	switch(type) {
	case MOD_LOAD:
		knlist_init_mtx(&ucoredev_sel.si_note, &ucoredev_mtx);
		ucore_dev = make_dev_credf(MAKEDEV_ETERNAL_KLD, &ucore_cdevsw,
		    0, NULL, UID_ROOT, GID_WHEEL, 0400, "ucore");
		break;

	case MOD_UNLOAD:
		/* XXX Return EBUSY if the device is open. */
		destroy_dev(ucore_dev);
		knlist_clear(&ucoredev_sel.si_note, 0);
		seldrain(&ucoredev_sel);
		knlist_destroy(&ucoredev_sel.si_note);

		UCORE_LOCK();
		while (!STAILQ_EMPTY(&ucores)) {
			struct ucoredev_shmfd *ucs;

			ucs = STAILQ_FIRST(&ucores);
			STAILQ_REMOVE_HEAD(&ucores, entry);

			ucoredev_shmfd_free(ucs);
			ucoresz--;
		}

		MPASS(ucoresz == 0);
		UCORE_UNLOCK();
		return (0);

	case MOD_SHUTDOWN:
		break;

	default:
		return (EOPNOTSUPP);
	}

	return (0);
}

DEV_MODULE(coredump_ucored, coredump_ucored_modevent, NULL);
MODULE_VERSION(coredump_ucored, 1);
