#ifndef _TIME_H_
#define _TIME_H_

#include <sys/types.h>
#include "inc/cdefs.h"

#define CLOCKS_PER_SEC 100
#define CLK_TCK 100

typedef long clock_t;

struct timespec {
  time_t tv_sec;
  long tv_nsec;
};

struct tm {
  int tm_sec;
  int tm_min;
  int tm_hour;
  int tm_mday;
  int tm_mon;
  int tm_year;
  int tm_wday;
  int tm_yday;
  int tm_isdst;
  long tm_gmtoff;
  const char *tm_zone;
};

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 3
#define CLOCK_UPTIME 5 // since boot, which is what the monotonic clock counts

__BEGIN_DECLS
char *asctime(const struct tm *);
clock_t clock(void);
char *ctime(const time_t *);
double difftime(time_t, time_t);
struct tm *gmtime(const time_t *);
struct tm *localtime(const time_t *);
time_t mktime(struct tm *);
size_t strftime(char *__restrict, size_t, const char *__restrict, const struct tm *__restrict);
time_t time(time_t *);
extern int daylight;
extern long timezone;
char *asctime_r(const struct tm *__restrict, char *__restrict);
char *ctime_r(const time_t *, char *);
struct tm *gmtime_r(const time_t *__restrict, struct tm *__restrict);
struct tm *localtime_r(const time_t *__restrict, struct tm *__restrict);
extern char *tzname[2];
void tzset(void);
int clock_gettime(clockid_t, struct timespec *);
int clock_settime(clockid_t, const struct timespec *);
int clock_getres(clockid_t, struct timespec *);
int nanosleep(const struct timespec *, struct timespec *);
#define TIME_UTC 1
int timespec_get(struct timespec *, int);
void tzsetwall(void);
time_t timelocal(struct tm *);
time_t timegm(struct tm *);
char *strptime(const char *, const char *, struct tm *);
time_t timeoff(struct tm *, const long);
time_t time2posix(time_t);
time_t posix2time(time_t);
__END_DECLS

#endif
