MANDIR=	${PREFIX:U/usr/local}/share/man/man
BINDIR=	${PREFIX:U/usr/local}/sbin
PROG=	ucored
MAN=	ucored.8
WARNS?=	6

SRCTOP=	${.CURDIR}/../..
LIBUCORE=	${SRCTOP}/lib/libucore/libucore.a

SRCS=	ucored.c \
	ucored_client.c \
	ucored_lua.c

SHAREDIR?=	${PREFIX:U/usr/local}/share/ucored

FILESGROUPS=	CONFIG FILES

CONFIG=		ucored.conf
CONFIGDIR=	${PREFIX:U/usr/local}/etc

FILES+=		config.lua
FILES+=		ucored.lua
FILESDIR=	${SHAREDIR}

LUAINC?=	${LOCALBASE:U/usr/local}/include/lua54
LUALIB?=	-L/usr/local/lib -llua-5.4

CFLAGS+=	-DUCORED -I${LUAINC} -I${SRCTOP}/lib/libucore
CFLAGS+=	-DUCORED_LUAFILE="\"${SHAREDIR}/ucored.lua\""
CFLAGS+=	-DUCORED_LUAPATH="\"${SHAREDIR}/?.lua;;\""
CFLAGS+=	-I${SRCTOP}/kmod

LDADD+=	-lutil ${LUALIB} ${LIBUCORE}
DPADD+=	-lutil ${LUALIB} ${LIBUCORE}

# liblua dependencies (because we're statically linked)
LDADD+=	-lm
DPADD+=	-lm

.include <bsd.prog.mk>
