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
#include <sys/filedesc.h>
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
#include <sys/ucoredump.h>
#include <sys/ucred.h>
#include <sys/uio.h>
#include <sys/vnode.h>
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
	off_t				 corepos;
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
struct coredumper ucoredev_coredumper = {
	.cd_name = "ucoredev",
	.cd_probe = coredump_ucored_probe,
	.cd_handle = coredump_ucored,
};

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
do_write(struct shmfd *shmfd, off_t offset, const void *data, size_t *datasz,
    enum uio_seg seg, struct thread *td)
{
	struct iovec iov;
	struct uio uio;
	off_t newsz;
	int error;

	newsz = MAX(shmfd->shm_size, offset + *datasz);
	if (shmfd->shm_size < newsz) {
		error = shm_dotruncate(shmfd, newsz);
		if (error != 0)
			return (error);
	}

	iov.iov_base = __DECONST(void *, data);
	iov.iov_len = *datasz;

	uio.uio_iov = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_offset = offset;
	uio.uio_resid = *datasz;
	uio.uio_segflg = seg;
	uio.uio_rw = UIO_WRITE;
	uio.uio_td = td;

	error = uiomove_object(shmfd->shm_object, newsz, &uio);
	*datasz = uio.uio_resid;
	return (error);
}


static int
coredump_shmwrite(const struct coredump_writer *cdw, const void *base,
    size_t len, off_t offset, enum uio_seg seg, struct ucred *cred,
    size_t *resid, struct thread *td)
{
	struct coredump_ucore_ctx *uctx = cdw->ctx;
	int error;

	offset += uctx->corepos;

	error = do_write(uctx->shmfd, offset, base, &len, seg, td);
	if (resid != NULL)
		*resid = len;
	return (error);
}

static int
coredump_shmextend(const struct coredump_writer *cdw, off_t newsz,
    struct ucred *ucred __unused)
{
	struct coredump_ucore_ctx *uctx = cdw->ctx;

	newsz += uctx->corepos;
	MPASS(newsz > uctx->shmfd->shm_size);
	return (shm_dotruncate(uctx->shmfd, newsz));
}

static void
write_segment_string(struct ucore *uc, struct shmfd *shmfd, off_t *poff,
    enum ucore_data_type type, const char *str, struct thread *td)
{
	struct ucore_data_hdr uhdr;
	size_t datasz, odatasz, writesz;
	off_t offset = *poff;
	int error;

	/* Write the header out first. */
	datasz = strlen(str) + 1 /* NUL */;
	uhdr.uhdr_type = type;
	uhdr.uhdr_size = datasz;

	/* We'll just ignore all errors and avoid counting this segment. */
	writesz = sizeof(uhdr);
	error = do_write(shmfd, offset, &uhdr, &writesz, UIO_SYSSPACE, td);
	if (error != 0)
		return;

	MPASS(writesz == 0);
	offset += sizeof(uhdr);

	/* Now write out the string itself. */
	odatasz = datasz;
	error = do_write(shmfd, offset, str, &datasz, UIO_SYSSPACE, td);
	if (error != 0)
		return;

	MPASS(datasz == 0);
	offset += odatasz;

	*poff = offset;
	uc->ucore_datasegs++;
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
	struct ucore uc = { };
	struct ucoredev_shmfd *ucshm;
	struct shmfd *shm;
	struct proc *p;
	struct prison *pr;
	struct pwd *pwd;
	struct vnode *cwd;
	struct ucred *cred;
	char *fullpath, *freepath = NULL;
	off_t corepos;
	size_t datasz;
	int error;

	p = td->td_proc;
	PROC_LOCK_ASSERT(p, MA_OWNED);

	cred = p->p_ucred;
	pr = cred->cr_prison;

	vfs_timestamp(&uc.ucore_time);
	uc.ucore_pid = p->p_pid;
	uc.ucore_uid = cred->cr_uid;
	uc.ucore_gid = cred->cr_gid;
	uc.ucore_tainted = (p->p_flag & P_SUGID) != 0;

	uc.ucore_ppid = p->p_oppid;
	uc.ucore_signo = p->p_sig;
	uc.ucore_jid = pr->pr_id;
	if (pr->pr_id != 0)
		prison_hold(pr);
	else
		pr = NULL;

	pwd = pwd_hold(td);
	PROC_UNLOCK(p);

	memcpy(&uc.ucore_magic, UCORE_MAGIC, sizeof(uc.ucore_magic));

	/* XXX Should this borrow against a global coredump_ucore ucred? */
	shm = shm_alloc(p->p_ucred, O_RDONLY, false);
	MPASS(shm != NULL);

	/*
	 * Populate our header first.  We skip the header to start with until
	 * we know exactly how many segments we're going to have, then we'll
	 * write it out right before we write the core.
	 */
	corepos = sizeof(uc);
	if (pr != NULL) {
		write_segment_string(&uc, shm, &corepos, UDT_JAIL,
		    pr->pr_name, td);
		write_segment_string(&uc, shm, &corepos, UDT_JAILROOT,
		    pr->pr_path, td);
		prison_free(pr);
		pr = NULL;
	}

	/*
	 * If our best effort fails, at least provide p_comm as a hint
	 * to the command run.
	 */
	if (vn_fullpath_global(p->p_textvp, &fullpath, &freepath) == 0) {
		write_segment_string(&uc, shm, &corepos, UDT_COMM,
		    fullpath, td);
		free(freepath, M_TEMP);
		freepath = NULL;
	} else {
		write_segment_string(&uc, shm, &corepos, UDT_COMM,
		    p->p_comm, td);
	}

	/* Grab the process cwd, as well. */
	cwd = pwd->pwd_cdir;
	if (cwd != NULL && vn_fullpath_global(cwd, &fullpath,
	    &freepath) == 0) {
		write_segment_string(&uc, shm, &corepos, UDT_PWD,
		    fullpath, td);
		free(freepath, M_TEMP);
		freepath = NULL;
	}

	pwd_drop(pwd);

	uctx.shmfd = shm;
	uctx.corepos = corepos;

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

	/*
	 * Now return to write the core header out.  Unlike with the data
	 * segments, this is not optional and we can't really proceed with the
	 * dump without it.
	 */
	datasz = sizeof(uc);
	uc.ucore_size = shm->shm_size - uctx.corepos;
	uprintf("Recorded size: %zu\n", uc.ucore_size);
	error = do_write(shm, 0, &uc, &datasz, UIO_SYSSPACE, td);
	if (error != 0) {
		shm_drop(shm);
		return (error);
	}

	MPASS(datasz == 0);

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
		coredumper_register(&ucoredev_coredumper);
		break;

	case MOD_UNLOAD:
		/* XXX Return EBUSY if the device is open. */
		destroy_dev(ucore_dev);
		knlist_clear(&ucoredev_sel.si_note, 0);
		seldrain(&ucoredev_sel);
		knlist_destroy(&ucoredev_sel.si_note);
		coredumper_unregister(&ucoredev_coredumper);

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
