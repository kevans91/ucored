SUBDIR_PARALLEL=

SUBDIR+=	lib
SUBDIR+=	.WAIT

SUBDIR+=	devd
SUBDIR+=	examples
SUBDIR+=	libexec
SUBDIR+=	rc.d
SUBDIR+=	sbin
SUBDIR+=	syslog.d

.include <bsd.subdir.mk>
