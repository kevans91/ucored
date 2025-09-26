SYSDIR?=	/usr/src/sys
.if exists(${SYSDIR}/sys/param.h)
OSVERSION!=	awk '/^\#define[[:space:]]+__FreeBSD_version/ {print $$3}' \
    ${SYSDIR}/sys/param.h
.else
OSVERSION?=	0
.endif

.if ${OSVERSION} >= 1500055
KMOD_BUILD=	yes
.endif

# Given that ucored(8) is *the* handler of coredumps when it is installed on
# FreeBSD 15.0+, it seems prudent to try and make it more resilient to weird
# system problems.
NO_SHARED=
