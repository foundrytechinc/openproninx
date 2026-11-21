PROG:=	mtree

BASE_OBJS:=	\
	../../contrib/mknod/pack_dev.o \
	../../contrib/mtree/compare.o \
	../../contrib/mtree/crc.o \
	../../contrib/mtree/create.o \
	../../contrib/mtree/excludes.o \
	../../contrib/mtree/getid.o \
	../../contrib/mtree/misc.o \
	../../contrib/mtree/mtree.o \
	../../contrib/mtree/only.o \
	../../contrib/mtree/spec.o \
	../../contrib/mtree/specspec.o \
	../../contrib/mtree/verify.o

UNAME:=$(shell uname -s)
ifeq ($(UNAME), Linux)
	BASE_OBJS+=	\
		../../contrib/libc-pwcache/pwcache.o \
		../../contrib/libc-vis/unvis.o \
		../../contrib/libc-vis/vis.o \
		../../contrib/mtree/libmd.o \
		../../lib/libc/gen/getprogname.o \
		../../lib/libc/gen/setmode.o \
		../../lib/libc/gen/setprogname.o \
		../../lib/libc/gen/strtofflags.o \
		../../lib/libutil/fparseln.o
endif
ifeq ($(UNAME), CYGWIN_NT-10.0)
	BASE_OBJS+=	\
		../../contrib/libc-pwcache/pwcache.o \
		../../contrib/libc-vis/unvis.o \
		../../contrib/libc-vis/vis.o \
		../../contrib/mtree/libmd.o \
		../../lib/libc/gen/getprogname.o \
		../../lib/libc/gen/setmode.o \
		../../lib/libc/gen/setprogname.o \
		../../lib/libc/gen/strtofflags.o \
		../../lib/libutil/fparseln.o
endif
ifeq ($(UNAME), NetBSD)
	BASE_OBJS+=	\
		../../lib/libc/gen/strtofflags.o
else
	BASE_OBJS+=	\
		../../lib/libnetbsd/util.o
endif
ifeq ($(UNAME), OpenBSD)
	BASE_OBJS+=	\
		../../contrib/libc-vis/vis.o
endif
ifeq ($(UNAME), DragonFly)
	BASE_OBJS+=	\
		../../contrib/mtree/libmd.o
endif

LDLIBS:=
ifeq ($(UNAME), Linux)
	LDLIBS+=	-lcrypto
endif
ifeq ($(UNAME), CYGWIN_NT-10.0)
	LDLIBS+=	-lcrypto
endif
ifeq ($(UNAME), FreeBSD)
	LDLIBS+=	-lutil -lmd
endif
ifeq ($(UNAME), NetBSD)
	LDLIBS+=	-lutil
endif
ifeq ($(UNAME), OpenBSD)
	LDLIBS+=	-lutil
endif
ifeq ($(UNAME), DragonFly)
	LDLIBS+=	-lutil
	LDLIBS+=	$(shell pkg-config --libs openssl)
endif

.PHONY: all clean

all: $(PROG)
$(PROG): $(BASE_OBJS)
	$(CC) $(CFLAGS) -o $@ $(BASE_OBJS) $(LDLIBS)
clean:
	rm -f $(PROG)
-include $(DEPS)
