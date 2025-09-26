SUBDIR_PARALLEL=

.include "Makefile.inc"

.if !make(install)
SUBDIR+=	lib
SUBDIR+=	.WAIT
.endif

SUBDIR+=	devd
SUBDIR+=	examples
.if ${KMOD_BUILD:Uno} != "no"
SUBDIR+=	kmod
.endif
SUBDIR+=	libexec
SUBDIR+=	man
SUBDIR+=	rc.d
SUBDIR+=	sbin
SUBDIR+=	syslog.d

.include <bsd.subdir.mk>
