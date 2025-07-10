
BINDIR=	${PREFIX:U/usr/local}/libexec
PROG=	ucore-shuttle
MAN=
WARNS?=	6

CFLAGS+=	-I${.CURDIR}/../../sbin/ucored
LDADD+=	-ljail
DPADD+=	-ljail

.include <bsd.prog.mk>
