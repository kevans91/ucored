KMOD=	ucoredev
MAN=	ucoredev.4
SRCS=	ucoredev.c \
	ucoredev_coredump.c \
	vnode_if.h

CFLAGS+=	-DINVARIANTS -DINVARIANT_SUPPORT

.include <bsd.kmod.mk>
.include <bsd.man.mk>
