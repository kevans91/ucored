
BINDIR=	${PREFIX:U/usr/local}/libexec
PROG=	ucore-shuttle
MAN=
WARNS?=	6

SRCTOP=	${.CURDIR}/../..
LIBUCORE=	${SRCTOP}/lib/libucore/libucore.a

CFLAGS+=	-I${SRCTOP}/lib/libucore
CFLAGS+=	-I${SRCTOP}/kmod
LDADD+=	-ljail -lmagic ${LIBUCORE}
DPADD+=	-ljail -lmagic ${LIBUCORE}

.include <bsd.prog.mk>
