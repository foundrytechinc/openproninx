SRCS:=	compare.c crc.c create.c excludes.c getid.c misc.c mtree.c only.c spec.c specspec.c verify.c

UNAME:=$(shell uname -s)
ifeq ($(UNAME), Linux)
SRCS+=		libmd.c
endif
ifeq ($(UNAME), CYGWIN_NT-10.0)
SRCS+=		libmd.c
endif
ifeq ($(UNAME), DragonFly)
SRCS+=		libmd.c
CFLAGS+=	$(shell pkg-config --cflags openssl)
endif

OBJS:=$(SRCS:.c=.o)
DEPS:=$(OBJS:.o=.d)

CFLAGS+=	-I. -I../../sys -I../../lib/libutil -I../libc-vis -I../mknod

UNAME:=$(shell uname -s)
ifneq ($(UNAME), NetBSD)
	CFLAGS+=	-I../../lib/libnetbsd
endif

.PHONY: all clean

all: $(OBJS)
.c.o:
	$(CC) $(CFLAGS) -c $<
clean:
	rm -f $(OBJS) $(DEPS)
-include $(DEPS)
