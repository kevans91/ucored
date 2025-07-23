/*-
 * Copyright (c) 2025 Kyle Evans <kevans@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/compressor.h>
#include <sys/event.h>
#include <sys/exec.h>
#include <sys/filio.h>
#include <sys/imgact.h>
#include <sys/fcntl.h>
#include <sys/filedesc.h>
#include <sys/jail.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mman.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/queue.h>
#include <sys/resourcevar.h>
#include <sys/sysent.h>
#include <sys/ucoredump.h>
#include <sys/ucred.h>
#include <sys/uio.h>
#include <sys/unistd.h>
#include <sys/vnode.h>

#include "ucoredev.h"

struct coredump_ucore_ctx {
	struct shmfd			*shmfd;
	off_t				 corepos;
	int				 compression;
};

static coredump_init_fn coredump_shminit;
static coredump_write_fn coredump_shmwrite;
static coredump_extend_fn coredump_shmextend;

static coredumper_probe_fn coredump_ucoredev_probe;
static coredumper_handle_fn coredump_ucoredev;
struct coredumper ucoredev_coredumper = {
	.cd_name = "ucoredev",
	.cd_probe = coredump_ucoredev_probe,
	.cd_handle = coredump_ucoredev,
};

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
coredump_shminit(const struct coredump_writer *cdw,
    const struct coredump_params *cdp __unused, int compression)
{
	struct coredump_ucore_ctx *uctx = cdw->ctx;

	uctx->compression = compression;
	return (0);
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
coredump_ucoredev_probe(struct thread *td)
{
	return (COREDUMPER_SPECIAL);
}

static int
coredump_ucoredev(struct thread *td, off_t limit)
{
	struct coredump_ucore_ctx uctx = { };
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
	prison_hold(pr);

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
	/*

	 * No need to write out our jail name/path for the host system,
	 * but some other system properties use prison0 for their
	 * storage (e.g., hostname/domainname) so we'll just ignore these that
	 * aren't as useful.
	 */
	if (pr->pr_id != 0) {
		write_segment_string(&uc, shm, &corepos, UDT_JAIL,
		    pr->pr_name, td);
		write_segment_string(&uc, shm, &corepos, UDT_JAILROOT,
		    pr->pr_path, td);
	}

	write_segment_string(&uc, shm, &corepos, UDT_DOMAINNAME,
	    pr->pr_domainname, td);
	write_segment_string(&uc, shm, &corepos, UDT_HOSTNAME,
	    pr->pr_hostname, td);

	prison_free(pr);
	pr = NULL;

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
	uctx.compression = -1;

	cdw.ctx = &uctx;
	cdw.init_fn = coredump_shminit;
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

	switch (uctx.compression) {
	case 0:
		uc.ucore_compression = UCOMP_NONE;
		break;
	case COMPRESS_GZIP:
		uc.ucore_compression = UCOMP_GZIP;
		break;
	case COMPRESS_ZSTD:
		uc.ucore_compression = UCOMP_ZSTD;
		break;
	default:
		uc.ucore_compression = UCOMP_UNKNOWN;
		break;
	}

	uc.ucore_size = shm->shm_size - uctx.corepos;
	error = do_write(shm, 0, &uc, &datasz, UIO_SYSSPACE, td);
	if (error != 0) {
		shm_drop(shm);
		return (error);
	}

	MPASS(datasz == 0);

	ucshm = malloc(sizeof(*ucshm), M_UCORE, M_WAITOK | M_ZERO);
	ucshm->shmfd = shm;

	ucoredev_enqueue(ucshm);

	return (0);
}
