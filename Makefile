SUBDIR_PARALLEL=

SYSDIR?=	/usr/src/sys

.if exists(${SYSDIR}/sys/param.h)
OSVERSION!=	awk '/^\#define[[:space:]]+__FreeBSD_version/ {print $$3}' \
    ${SYSDIR}/sys/param.h
.else
OSVERSION?=	0
.endif

SUBDIR+=	lib
SUBDIR+=	.WAIT

SUBDIR+=	devd
SUBDIR+=	examples
.if ${OSVERSION} >= 1500055
SUBDIR+=	kmod
.endif
SUBDIR+=	libexec
SUBDIR+=	man
SUBDIR+=	rc.d
SUBDIR+=	sbin
SUBDIR+=	syslog.d

.include <bsd.subdir.mk>
