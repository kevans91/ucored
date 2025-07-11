
BINDIR=	${PREFIX:U/usr/local}/libexec
PROG=	ucore-shuttle
MAN=
WARNS?=	6

SRCTOP=	${.CURDIR}/../..
LIBUCORE=	${SRCTOP}/lib/libucore/libucore.a


CFLAGS+=	-I${.CURDIR}/../../sbin/ucored
LDADD+=	-ljail ${LIBUCORE}
DPADD+=	-ljail ${LIBUCORE}

.include <bsd.prog.mk>
