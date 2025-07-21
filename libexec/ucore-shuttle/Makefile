
BINDIR=	${PREFIX:U/usr/local}/libexec
PROG=	ucore-shuttle
MAN=
WARNS?=	6

SRCTOP=	${.CURDIR}/../..
LIBUCORE=	${SRCTOP}/lib/libucore/libucore.a

CFLAGS+=	-I${SRCTOP}/lib/libucore
CFLAGS+=	-I${SRCTOP}/kmod
LDADD+=	-ljail ${LIBUCORE}
DPADD+=	-ljail ${LIBUCORE}

.include <bsd.prog.mk>
