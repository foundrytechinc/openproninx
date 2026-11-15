#ifndef _SYS_EVENT_H_
#define _SYS_EVENT_H_

#include <sys/types.h>

// kqueue numbers; libc maps them to the event queues
#define EVFILT_READ (-1)
#define EVFILT_WRITE (-2)
#define EVFILT_AIO (-3)
#define EVFILT_VNODE (-4)
#define EVFILT_PROC (-5)
#define EVFILT_SIGNAL (-6)
#define EVFILT_TIMER (-7)
#define EVFILT_DEVICE (-8)
#define EVFILT_EXCEPT (-9)
#define EVFILT_USER (-10)

#define EVFILT_SYSCOUNT 10

#define EV_SET(kevp, a, b, c, d, e, f)                                         \
  do {                                                                         \
    struct kevent *__kevp = (kevp);                                            \
    (__kevp)->ident = (a);                                                     \
    (__kevp)->filter = (b);                                                    \
    (__kevp)->flags = (c);                                                     \
    (__kevp)->fflags = (d);                                                    \
    (__kevp)->data = (e);                                                      \
    (__kevp)->udata = (f);                                                     \
  } while (0)

struct kevent {
  uintptr_t ident;
  short filter;
  unsigned short flags;
  unsigned int fflags;
  int64_t data;
  void *udata;
};

#define EV_ADD 0x0001
#define EV_DELETE 0x0002
#define EV_ENABLE 0x0004
#define EV_DISABLE 0x0008
#define EV_ONESHOT 0x0010
#define EV_CLEAR 0x0020
#define EV_RECEIPT 0x0040
#define EV_DISPATCH 0x0080
#define EV_SYSFLAGS 0xf800
#define EV_FLAG1 0x2000
#define EV_EOF 0x8000
#define EV_ERROR 0x4000

#define NOTE_LOWAT 0x0001
#define NOTE_EOF 0x0002

#define NOTE_DELETE 0x0001
#define NOTE_WRITE 0x0002
#define NOTE_EXTEND 0x0004
#define NOTE_ATTRIB 0x0008
#define NOTE_LINK 0x0010
#define NOTE_RENAME 0x0020
#define NOTE_REVOKE 0x0040
#define NOTE_TRUNCATE 0x0080

#define NOTE_EXIT 0x80000000
#define NOTE_FORK 0x40000000
#define NOTE_EXEC 0x20000000
#define NOTE_PCTRLMASK 0xf0000000
#define NOTE_PDATAMASK 0x000fffff
#define NOTE_TRACK 0x00000001
#define NOTE_TRACKERR 0x00000002
#define NOTE_CHILD 0x00000004

#define NOTE_MSECONDS 0x00000000
#define NOTE_SECONDS 0x00000001
#define NOTE_USECONDS 0x00000002
#define NOTE_NSECONDS 0x00000003
#define NOTE_ABSTIME 0x00000010

struct timespec;

int kqueue(void);
int kqueue1(int flags);
int kevent(int kq, const struct kevent *changelist, int nchanges,
           struct kevent *eventlist, int nevents, const struct timespec *timeout);

#endif
