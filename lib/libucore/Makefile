
LIB=	ucore
SRCS=	libucore.c \
	libucore_dev.c
WARNS=	6

SRCTOP=	${.CURDIR}/../..

CFLAGS+=	${PICFLAG}
CFLAGS+=	-I${SRCTOP}/kmod

.include <bsd.lib.mk>
