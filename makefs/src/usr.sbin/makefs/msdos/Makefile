SRCS:=	msdosfs_conv.c msdosfs_denode.c msdosfs_fat.c msdosfs_lookup.c msdosfs_vfsops.c msdosfs_vnops.c

OBJS:=$(SRCS:.c=.o)
DEPS:=$(OBJS:.o=.d)

CFLAGS+=	-DMAKEFS
CFLAGS+=	-D_WANT_MSDOSFS_INTERNALS
# not -I.
CFLAGS+=	-I.. -I../../../sys -I../../../sbin/newfs_msdos

UNAME:=$(shell uname -s)
ifneq ($(UNAME), NetBSD)
	CFLAGS+=	-I../../../lib/libnetbsd
endif

.PHONY: all clean

all: $(OBJS)
.c.o:
	$(CC) $(CFLAGS) -c $<
clean:
	rm -f $(OBJS) $(DEPS)
-include $(DEPS)
