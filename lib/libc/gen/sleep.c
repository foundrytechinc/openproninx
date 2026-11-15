// sleep(3), usleep(3), alarm(3)
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

unsigned int sleep(unsigned int seconds) {
  struct timespec want, left;

  want.tv_sec = seconds;
  want.tv_nsec = 0;
  if (nanosleep(&want, &left) < 0)
    return (unsigned int)left.tv_sec + (left.tv_nsec != 0);
  return 0;
}

int usleep(useconds_t microseconds) {
  struct timespec want;

  want.tv_sec = microseconds / 1000000;
  want.tv_nsec = (long)(microseconds % 1000000) * 1000;
  return nanosleep(&want, 0);
}

unsigned int alarm(unsigned int seconds) {
  struct itimerval it, old;

  it.it_value.tv_sec = seconds;
  it.it_value.tv_usec = 0;
  it.it_interval.tv_sec = 0;
  it.it_interval.tv_usec = 0;
  if (setitimer(ITIMER_REAL, &it, &old) < 0)
    return 0;
  return (unsigned int)(old.it_value.tv_sec + (old.it_value.tv_usec != 0));
}
