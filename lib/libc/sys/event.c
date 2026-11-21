// kqueue/kevent over the system's queues
#include <sys/types.h>
#include <sys/event.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fnu/sys.h>
#include "posix.h"

int __sig_to_native(int);
int __sig_of_native(int);
int __status_of(const struct fnu_exitinfo *e);

_Static_assert(sizeof(struct kevent) == sizeof(struct fnu_event), "kevent");
_Static_assert(EV_ADD == FNU_EV_ADD && EV_DELETE == FNU_EV_DELETE &&
                   EV_ENABLE == FNU_EV_ENABLE && EV_DISABLE == FNU_EV_DISABLE &&
                   EV_ONESHOT == FNU_EV_ONESHOT && EV_CLEAR == FNU_EV_CLEAR &&
                   EV_RECEIPT == FNU_EV_RECEIPT && EV_DISPATCH == FNU_EV_DISPATCH &&
                   EV_ERROR == FNU_EV_ERROR && EV_EOF == FNU_EV_EOF,
               "event flags");
_Static_assert(NOTE_DELETE == FNU_NOTE_DELETE && NOTE_WRITE == FNU_NOTE_WRITE &&
                   NOTE_EXTEND == FNU_NOTE_EXTEND && NOTE_ATTRIB == FNU_NOTE_ATTRIB &&
                   NOTE_LINK == FNU_NOTE_LINK && NOTE_RENAME == FNU_NOTE_RENAME &&
                   NOTE_TRUNCATE == FNU_NOTE_TRUNCATE && NOTE_EXIT == FNU_NOTE_EXIT &&
                   NOTE_FORK == FNU_NOTE_FORK && NOTE_EXEC == FNU_NOTE_EXEC,
               "notes");

static const struct {
  short bsd;
  short native;
} filters[] = {
    {EVFILT_READ, FNU_EVF_READ},     {EVFILT_WRITE, FNU_EVF_WRITE},
    {EVFILT_VNODE, FNU_EVF_FILE},    {EVFILT_PROC, FNU_EVF_PROC},
    {EVFILT_SIGNAL, FNU_EVF_SIGNAL}, {EVFILT_TIMER, FNU_EVF_TIMER},
    {EVFILT_USER, FNU_EVF_USER},
};

static const unsigned int units[] = {FNU_NOTE_MSECONDS, FNU_NOTE_SECONDS,
                                     FNU_NOTE_USECONDS, FNU_NOTE_NSECONDS};

// 0: no such filter, the change is refused
static short filter_to(short f) {
  size_t i;

  for (i = 0; i < sizeof(filters) / sizeof(filters[0]); i++)
    if (filters[i].bsd == f)
      return filters[i].native;
  return 0;
}

static short filter_of(short f) {
  size_t i;

  for (i = 0; i < sizeof(filters) / sizeof(filters[0]); i++)
    if (filters[i].native == f)
      return filters[i].bsd;
  return 0;
}

static void to_native(const struct kevent *k, struct fnu_event *n) {
  n->ident = k->ident;
  n->filter = filter_to(k->filter);
  n->flags = k->flags;
  n->fflags = k->fflags;
  n->data = k->data;
  n->udata = (uint64_t)(uintptr_t)k->udata;
  if (n->filter == FNU_EVF_SIGNAL)
    n->ident = (uint64_t)__sig_to_native((int)k->ident);
  // a unit the system has not, or an absolute time, is refused
  if (n->filter == FNU_EVF_TIMER)
    n->fflags = (k->fflags & ~3u) == 0 ? units[k->fflags & 3] : ~0u;
}

static void of_native(const struct fnu_event *n, const struct kevent *asked,
                      struct kevent *k) {
  struct fnu_exitinfo e;

  k->ident = (uintptr_t)n->ident;
  k->filter = filter_of(n->filter);
  k->flags = n->flags;
  k->fflags = n->fflags;
  k->data = n->data;
  k->udata = (void *)(uintptr_t)n->udata;
  if (n->flags & FNU_EV_ERROR) {
    // the change as given, its error in errno's numbers
    *k = *asked;
    k->flags = EV_ERROR;
    k->data = n->data != 0 ? __errno_of((int)n->data) : 0;
    return;
  }
  if (n->filter == FNU_EVF_SIGNAL)
    k->ident = (uintptr_t)__sig_of_native((int)n->ident);
  if (n->filter == FNU_EVF_TIMER) {
    unsigned int i;

    for (i = 0; i < 4 && units[i] != n->fflags; i++)
      ;
    k->fflags = i < 4 ? i : 0;
  }
  if (n->filter == FNU_EVF_PROC && (n->fflags & NOTE_EXIT)) {
    memset(&e, 0, sizeof(e));
    e.reason = (int32_t)(n->data >> 32);
    e.status = (int32_t)(uint32_t)n->data;
    k->data = __status_of(&e);
  }
}

int kqueue1(int flags) {
  if (flags & ~O_CLOEXEC) {
    errno = EINVAL;
    return -1;
  }
  return (int)__posix(fnu_eventq((flags & O_CLOEXEC) ? FNU_FD_CLOSEEXEC : 0));
}

int kqueue(void) { return kqueue1(0); }

int kevent(int kq, const struct kevent *changelist, int nchanges,
           struct kevent *eventlist, int nevents, const struct timespec *timeout) {
  struct fnu_event *in = 0, *out = 0;
  struct fnu_timespec t;
  int i, r, error;

  if (nchanges < 0 || nevents < 0) {
    errno = EINVAL;
    return -1;
  }
  if (timeout != 0) {
    t.sec = timeout->tv_sec;
    t.nsec = (int32_t)timeout->tv_nsec;
    t.pad = 0;
  }
  if ((nchanges > 0 && (in = malloc((size_t)nchanges * sizeof(*in))) == 0) ||
      (nevents > 0 && (out = malloc((size_t)nevents * sizeof(*out))) == 0)) {
    free(in);
    return -1;
  }
  for (i = 0; i < nchanges; i++)
    to_native(&changelist[i], &in[i]);
  r = (int)fnu_events(kq, in, nchanges, out, nevents, timeout != 0 ? &t : 0);
  error = r < 0 ? -r : 0;
  // an error reported names its change by ident and filter
  for (i = 0; i < r; i++) {
    const struct kevent *asked = 0;
    int j;

    for (j = 0; (out[i].flags & FNU_EV_ERROR) && j < nchanges && asked == 0; j++)
      if (in[j].ident == out[i].ident && in[j].filter == out[i].filter)
        asked = &changelist[j];
    if ((out[i].flags & FNU_EV_ERROR) && asked == 0) {
      struct kevent k;

      memset(&k, 0, sizeof(k));
      k.ident = (uintptr_t)out[i].ident;
      asked = &k;
      of_native(&out[i], asked, &eventlist[i]);
    } else
      of_native(&out[i], asked, &eventlist[i]);
  }
  free(in);
  free(out);
  if (error != 0) {
    errno = __errno_of(error);
    return -1;
  }
  return r;
}
