// the clock, the timers, and the timestamps of a file
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fnu/sys.h>
#include "posix.h"

static int clock_read(clockid_t clock_id, int grain, struct timespec *tp) {
  struct fnu_timespec t;
  int which;

  switch (clock_id) {
  case CLOCK_REALTIME:
    which = FNU_CLOCK_WALL;
    break;
  case CLOCK_MONOTONIC:
  case CLOCK_UPTIME:
    which = FNU_CLOCK_UPTIME;
    break;
  default:
    errno = EINVAL;
    return -1;
  }
  if (__posix(fnu_gettime(&t, which | grain)) < 0)
    return -1;
  if (tp != 0) {
    tp->tv_sec = t.sec;
    tp->tv_nsec = t.nsec;
  }
  return 0;
}

int clock_gettime(clockid_t clock_id, struct timespec *tp) {
  return clock_read(clock_id, 0, tp);
}

int clock_getres(clockid_t clock_id, struct timespec *res) {
  return clock_read(clock_id, FNU_CLOCK_GRAIN, res);
}

// only the wall clock is set; the others count from boot
int clock_settime(clockid_t clock_id, const struct timespec *tp) {
  struct fnu_timespec t;

  if (clock_id != CLOCK_REALTIME || tp->tv_nsec < 0 || tp->tv_nsec >= 1000000000L) {
    errno = EINVAL;
    return -1;
  }
  t.sec = tp->tv_sec;
  t.nsec = (int32_t)tp->tv_nsec;
  t.pad = 0;
  return (int)__posix(fnu_settime(&t));
}

int settimeofday(const struct timeval *tv, const struct timezone *tz __unused) {
  struct timespec ts;

  if (tv == 0)
    return 0;
  if (tv->tv_usec < 0 || tv->tv_usec >= 1000000) {
    errno = EINVAL;
    return -1;
  }
  ts.tv_sec = tv->tv_sec;
  ts.tv_nsec = tv->tv_usec * 1000;
  return clock_settime(CLOCK_REALTIME, &ts);
}

int adjtime(const struct timeval *delta, struct timeval *olddelta) {
  struct fnu_timespec d, left;

  if (delta != 0) {
    if (delta->tv_usec < 0 || delta->tv_usec >= 1000000) {
      errno = EINVAL;
      return -1;
    }
    d.sec = delta->tv_sec;
    d.nsec = (int32_t)(delta->tv_usec * 1000);
    d.pad = 0;
  }
  if (__posix(fnu_slewtime(delta != 0 ? &d : 0, &left)) < 0)
    return -1;
  if (olddelta != 0) {
    olddelta->tv_sec = left.sec;
    olddelta->tv_usec = left.nsec / 1000;
  }
  return 0;
}

time_t time(time_t *tloc) {
  struct timespec ts;

  if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
    return -1;
  if (tloc != 0)
    *tloc = ts.tv_sec;
  return ts.tv_sec;
}

int gettimeofday(struct timeval *tp, struct timezone *tzp) {
  struct timespec ts;

  if (tp != 0) {
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
      return -1;
    tp->tv_sec = ts.tv_sec;
    tp->tv_usec = ts.tv_nsec / 1000;
  }
  if (tzp != 0)
    memset(tzp, 0, sizeof(*tzp));
  return 0;
}

int nanosleep(const struct timespec *want, struct timespec *left) {
  struct fnu_timespec w, l;
  int r;

  if (want->tv_nsec < 0 || want->tv_nsec >= 1000000000L) {
    errno = EINVAL;
    return -1;
  }
  w.sec = want->tv_sec;
  w.nsec = (int32_t)want->tv_nsec;
  w.pad = 0;
  r = (int)__posix(fnu_sleep(&w, left ? &l : 0));
  if (left != 0) {
    left->tv_sec = l.sec;
    left->tv_nsec = l.nsec;
  }
  return r;
}

// both times, now when none are given
static int settimes(int fd, const char *path, const struct fnu_timespec *t,
                    int flags) {
  struct fnu_setattr a;

  memset(&a, 0, sizeof(a));
  a.mask = FNU_SET_ATIME | FNU_SET_MTIME;
  if (t != 0) {
    a.atime = t[0];
    a.mtime = t[1];
  } else {
    a.atime.nsec = FNU_TIME_NOW;
    a.mtime.nsec = FNU_TIME_NOW;
  }
  return (int)__posix(fnu_setattr(fd, path, flags, &a));
}

int utimes(const char *path, const struct timeval times[2]) {
  struct fnu_timespec t[2];

  if (times == 0)
    return settimes(FNU_FD_CWD, path, 0, 0);
  t[0].sec = times[0].tv_sec;
  t[0].nsec = (int32_t)(times[0].tv_usec * 1000);
  t[0].pad = 0;
  t[1].sec = times[1].tv_sec;
  t[1].nsec = (int32_t)(times[1].tv_usec * 1000);
  t[1].pad = 0;
  return settimes(FNU_FD_CWD, path, t, 0);
}

static void ns_of(const struct timespec *ts, struct fnu_timespec *k) {
  k->sec = ts->tv_sec;
  k->nsec = ts->tv_nsec == UTIME_NOW    ? FNU_TIME_NOW
            : ts->tv_nsec == UTIME_OMIT ? FNU_TIME_KEEP
                                        : (int32_t)ts->tv_nsec;
  k->pad = 0;
}

int utimensat(int fd, const char *path, const struct timespec times[2],
              int flags) {
  struct fnu_timespec t[2];

  if ((flags & ~AT_SYMLINK_NOFOLLOW) || (flags = __at_flags(flags)) < 0) {
    errno = EINVAL;
    return -1;
  }
  fd = __at_fd(fd);
  if (times == 0)
    return settimes(fd, path, 0, flags);
  ns_of(&times[0], &t[0]);
  ns_of(&times[1], &t[1]);
  return settimes(fd, path, t, flags);
}

int futimens(int fd, const struct timespec times[2]) {
  struct fnu_timespec t[2];

  if (times == 0)
    return settimes(fd, 0, 0, 0);
  ns_of(&times[0], &t[0]);
  ns_of(&times[1], &t[1]);
  return settimes(fd, 0, t, 0);
}

static void tv_of(const struct fnu_timespec *ts, struct timeval *tv) {
  tv->tv_sec = ts->sec;
  tv->tv_usec = ts->nsec / 1000;
}

static void ts_of(const struct timeval *tv, struct fnu_timespec *ts) {
  ts->sec = tv->tv_sec;
  ts->nsec = (int32_t)(tv->tv_usec * 1000);
  ts->pad = 0;
}

// numbered as the system numbers them
_Static_assert(ITIMER_REAL == FNU_TIMER_REAL && ITIMER_VIRTUAL == FNU_TIMER_VIRTUAL &&
                   ITIMER_PROF == FNU_TIMER_PROFILE,
               "itimer numbers");
_Static_assert(RLIMIT_CPU == FNU_LIMIT_CPU && RLIMIT_FSIZE == FNU_LIMIT_FILESIZE &&
                   RLIMIT_DATA == FNU_LIMIT_DATA && RLIMIT_STACK == FNU_LIMIT_STACK &&
                   RLIMIT_CORE == FNU_LIMIT_CORE && RLIMIT_RSS == FNU_LIMIT_RSS &&
                   RLIMIT_MEMLOCK == FNU_LIMIT_MEMLOCK &&
                   RLIMIT_NPROC == FNU_LIMIT_PROCS &&
                   RLIMIT_NOFILE == FNU_LIMIT_OPENFILES &&
                   RLIM_NLIMITS == FNU_LIMIT_COUNT &&
                   RLIM_INFINITY == FNU_LIMIT_NONE,
               "rlimit numbers");
_Static_assert(RUSAGE_SELF == FNU_USAGE_SELF &&
                   RUSAGE_CHILDREN == FNU_USAGE_CHILDREN,
               "rusage numbers");

int setitimer(int which, const struct itimerval *value, struct itimerval *ovalue) {
  struct fnu_timer in, out;

  if (value != 0) {
    ts_of(&value->it_value, &in.value);
    ts_of(&value->it_interval, &in.interval);
  }
  if (__posix(fnu_timer(which, value ? &in : 0, ovalue ? &out : 0)) < 0)
    return -1;
  if (ovalue != 0) {
    tv_of(&out.value, &ovalue->it_value);
    tv_of(&out.interval, &ovalue->it_interval);
  }
  return 0;
}

int getitimer(int which, struct itimerval *value) {
  struct fnu_timer out;

  if (__posix(fnu_timer(which, 0, &out)) < 0)
    return -1;
  tv_of(&out.value, &value->it_value);
  tv_of(&out.interval, &value->it_interval);
  return 0;
}

void __rusage_of(const struct fnu_usage *k, struct rusage *ru) {
  tv_of(&k->utime, &ru->ru_utime);
  tv_of(&k->stime, &ru->ru_stime);
  ru->ru_maxrss = (long)k->maxrss;
  ru->ru_nsignals = (long)k->nsignals;
  ru->ru_nvcsw = (long)k->nvcsw;
  ru->ru_nivcsw = (long)k->nivcsw;
}

int getrusage(int who, struct rusage *ru) {
  struct fnu_usage k;

  if (__posix(fnu_usage(who, &k)) < 0)
    return -1;
  __rusage_of(&k, ru);
  return 0;
}

int getrlimit(int resource, struct rlimit *rlp) {
  struct fnu_limit k;

  if (__posix(fnu_limit(resource, 0, &k)) < 0)
    return -1;
  rlp->rlim_cur = k.cur;
  rlp->rlim_max = k.max;
  return 0;
}

int setrlimit(int resource, const struct rlimit *rlp) {
  struct fnu_limit k;

  k.cur = rlp->rlim_cur;
  k.max = rlp->rlim_max;
  return (int)__posix(fnu_limit(resource, &k, 0));
}
