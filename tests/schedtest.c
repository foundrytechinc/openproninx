#include <err.h>
#include <fnu/sys.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAXSPIN 64
#define SECONDS 3

static int64_t now_ms(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// cpu time given, in us; virtual cpus lie
static void spinner(int fd, int nice_by, int64_t until) {
  volatile uint64_t spun = 0;
  struct rusage ru;
  uint64_t work;

  if (nice_by != 0 && nice(nice_by) < 0)
    warn("nice");
  while (now_ms() < until)
    for (int i = 0; i < 20000; i++)
      spun++;
  if (getrusage(RUSAGE_SELF, &ru) < 0)
    err(1, "getrusage");
  work = (uint64_t)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000 +
         (uint64_t)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec);
  write(fd, &work, sizeof(work));
  _exit(0);
}

static void sleeper(int fd, int64_t until) {
  int64_t worst = 0, was = now_ms();

  while (was < until) {
    struct timespec req = {0, 10 * 1000 * 1000}, rem;
    int64_t late;

    nanosleep(&req, &rem);
    late = now_ms() - was - 10;
    if (late > worst)
      worst = late;
    was = now_ms();
  }
  write(fd, &worst, sizeof(worst));
  _exit(0);
}

int main(void) {
  int64_t until;
  uint64_t work[MAXSPIN + 1] = {0}, sum = 0, mean;
  int64_t worst = -1;
  int pipes[MAXSPIN + 2][2];
  long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  int i, n = 0, bad = 0, spinners, go[2];

  if (ncpu < 2)
    ncpu = 2;
  if (ncpu > MAXSPIN)
    ncpu = MAXSPIN;
  spinners = (int)ncpu;

  if (pipe(go) < 0)
    err(1, "pipe");
  for (i = 0; i < spinners + 2; i++)
    if (pipe(pipes[i]) < 0)
      err(1, "pipe");
  for (i = 0; i < spinners + 2; i++) {
    pid_t pid = fork();

    if (pid < 0)
      err(1, "fork");
    if (pid == 0) {
      close(go[1]);
      for (int k = 0; k < spinners + 2; k++)
        if (k != i)
          close(pipes[k][1]);
      close(pipes[i][0]);
      // all start together, whatever order fork made them in
      if (read(go[0], &until, sizeof(until)) != (int)sizeof(until))
        errx(1, "no start");
      if (i == spinners + 1)
        sleeper(pipes[i][1], until);
      spinner(pipes[i][1], i == spinners ? 19 : 0, until);
    }
    close(pipes[i][1]);
  }
  until = now_ms() + SECONDS * 1000;
  for (i = 0; i < spinners + 2; i++)
    if (write(go[1], &until, sizeof(until)) != (int)sizeof(until))
      err(1, "write");
  close(go[0]);
  close(go[1]);
  for (i = 0; i < spinners + 1; i++)
    if (read(pipes[i][0], &work[i], sizeof(work[i])) != (int)sizeof(work[i]))
      errx(1, "spinner %d said nothing", i);
  if (read(pipes[spinners + 1][0], &worst, sizeof(worst)) != (int)sizeof(worst))
    errx(1, "the sleeper said nothing");
  while (wait(0) > 0)
    n++;

  for (i = 1; i < spinners; i++) {
    uint64_t a = work[0], b = work[i];
    uint64_t hi = a > b ? a : b, lo = a > b ? b : a;

    printf("schedtest: spinner %d ran %llu us, spinner 0 ran %llu us\n", i,
           (unsigned long long)b, (unsigned long long)a);
    if (lo == 0 || hi > lo + lo / 4) {
      printf("schedtest: equal processes got unequal shares\n");
      bad = 1;
    }
  }
  for (i = 0; i < spinners; i++)
    sum += work[i];
  mean = sum / (uint64_t)spinners;
  printf("schedtest: nice 19 ran %llu us against %llu us\n",
         (unsigned long long)work[spinners], (unsigned long long)mean);
  if (work[spinners] >= mean) {
    printf("schedtest: nice bought nothing\n");
    bad = 1;
  }
  printf("schedtest: a sleeper waited %lld ms longer than it asked\n",
         (long long)worst);
  if (worst > 100) {
    printf("schedtest: a sleeper was kept waiting\n");
    bad = 1;
  }
  printf("schedtest: %s (%d children)\n", bad ? "FAILED" : "ok", n);
  return bad;
}
