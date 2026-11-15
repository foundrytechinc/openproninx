// clocktest: the monotonic clock never goes back, whichever cpu reads it
#include <err.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SECONDS 4

static int64_t now_ns(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

int main(void) {
  long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  int64_t until, last, t, back = 0, ahead = 0;
  long i, backs = 0, reads = 0;
  int bad = 0;

  if (ncpu < 1)
    ncpu = 1;
  until = now_ns() + (int64_t)SECONDS * 1000000000;
  // more runners than cpus: the reader has to move
  for (i = 0; i < ncpu; i++) {
    pid_t pid = fork();

    if (pid < 0)
      err(1, "fork");
    if (pid == 0) {
      while (now_ns() < until)
        ;
      _exit(0);
    }
  }
  for (last = now_ns(); last < until; last = t, reads++) {
    t = now_ns();
    if (t < last) {
      backs++;
      if (last - t > back)
        back = last - t;
    } else if (t - last > ahead)
      ahead = t - last;
  }
  while (wait(0) > 0)
    ;
  printf("clocktest: %ld reads, %ld went back, by %lld us at most; longest step %lld us\n",
         reads, backs, (long long)(back / 1000), (long long)(ahead / 1000));
  if (backs != 0) {
    printf("clocktest: the clock went back\n");
    bad = 1;
  }
  printf("clocktest: %s\n", bad ? "FAILED" : "ok");
  return bad;
}
