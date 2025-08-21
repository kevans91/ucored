
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

# libmagic dependencies (because we're statically linked)
LDADD+=	-lz
DPADD+=	-lz

.include <bsd.prog.mk>
