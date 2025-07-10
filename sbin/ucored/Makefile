MANDIR=	${PREFIX:U/usr/local}/share/man/man
BINDIR=	${PREFIX:U/usr/local}/sbin
PROG=	ucored
MAN=	ucored.8
WARNS?=	6

SRCS=	ucored.c \
	ucored_lua.c

SHAREDIR?=	${PREFIX:U/usr/local}/share/ucored

FILES+=		config.lua
FILES+=		ucored.lua
FILESDIR=	${SHAREDIR}

LUAINC?=	${LOCALBASE:U/usr/local}/include/lua54
LUALIB?=	-L/usr/local/lib -llua-5.4

CFLAGS+=	-DUCORED -I${LUAINC}
CFLAGS+=	-DUCORED_LUAFILE="\"${SHAREDIR}/ucored.lua\""
CFLAGS+=	-DUCORED_LUAPATH="\"${SHAREDIR}/?.lua;;\""

LDADD+=	-lutil ${LUALIB}
DPADD+=	-lutil ${LUALIB}

.include <bsd.prog.mk>
