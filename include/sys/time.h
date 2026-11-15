#ifndef _SYS_TIME_H_
#define _SYS_TIME_H_

#include <sys/types.h>
#include <time.h>

struct timeval {
  time_t tv_sec;
  suseconds_t tv_usec;
};

struct timezone {
  int tz_minuteswest;
  int tz_dsttime;
};

struct itimerval {
  struct timeval it_interval;
  struct timeval it_value;
};

#define ITIMER_REAL 0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF 2

#define timerclear(tvp) (tvp)->tv_sec = (tvp)->tv_usec = 0
#define timerisset(tvp) ((tvp)->tv_sec || (tvp)->tv_usec)
#define timersub(tvp, uvp, vvp)                                                \
  do {                                                                         \
    (vvp)->tv_sec = (tvp)->tv_sec - (uvp)->tv_sec;                             \
    (vvp)->tv_usec = (tvp)->tv_usec - (uvp)->tv_usec;                          \
    if ((vvp)->tv_usec < 0) {                                                  \
      (vvp)->tv_sec--;                                                         \
      (vvp)->tv_usec += 1000000;                                               \
    }                                                                          \
  } while (0)
#define timespecsub(tsp, usp, vsp)                                             \
  do {                                                                         \
    (vsp)->tv_sec = (tsp)->tv_sec - (usp)->tv_sec;                             \
    (vsp)->tv_nsec = (tsp)->tv_nsec - (usp)->tv_nsec;                          \
    if ((vsp)->tv_nsec < 0) {                                                  \
      (vsp)->tv_sec--;                                                         \
      (vsp)->tv_nsec += 1000000000L;                                           \
    }                                                                          \
  } while (0)

int gettimeofday(struct timeval *tp, struct timezone *tzp);
int settimeofday(const struct timeval *tv, const struct timezone *tz);
int adjtime(const struct timeval *delta, struct timeval *olddelta);
int utimes(const char *path, const struct timeval times[2]);
int setitimer(int which, const struct itimerval *value, struct itimerval *ovalue);
int getitimer(int which, struct itimerval *value);

// operations on timespecs and timevals, as sys/time.h has always had them
#define timespecclear(tvp) ((tvp)->tv_sec = (tvp)->tv_nsec = 0)
#define timespecisset(tvp) ((tvp)->tv_sec || (tvp)->tv_nsec)
#define timespeccmp(tvp, uvp, cmp)                                             \
  (((tvp)->tv_sec == (uvp)->tv_sec) ? ((tvp)->tv_nsec cmp(uvp)->tv_nsec)       \
                                    : ((tvp)->tv_sec cmp(uvp)->tv_sec))
#define timespecadd(tsp, usp, vsp)                                             \
  do {                                                                         \
    (vsp)->tv_sec = (tsp)->tv_sec + (usp)->tv_sec;                             \
    (vsp)->tv_nsec = (tsp)->tv_nsec + (usp)->tv_nsec;                          \
    if ((vsp)->tv_nsec >= 1000000000L) {                                       \
      (vsp)->tv_sec++;                                                         \
      (vsp)->tv_nsec -= 1000000000L;                                           \
    }                                                                          \
  } while (0)
#define timespecsub(tsp, usp, vsp)                                             \
  do {                                                                         \
    (vsp)->tv_sec = (tsp)->tv_sec - (usp)->tv_sec;                             \
    (vsp)->tv_nsec = (tsp)->tv_nsec - (usp)->tv_nsec;                          \
    if ((vsp)->tv_nsec < 0) {                                                  \
      (vsp)->tv_sec--;                                                         \
      (vsp)->tv_nsec += 1000000000L;                                           \
    }                                                                          \
  } while (0)

#endif
