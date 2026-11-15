// flocktest: flock(2), held by the open file
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/wait.h>

#define PATH "/tmp/flocktest"

static int failures;

static void ok(const char *what, int good) {
  printf("flocktest: %-48s %s\n", what, good ? "ok" : "FAILED");
  if (!good)
    failures++;
}

static int busy(int fd, int op) { return flock(fd, op | LOCK_NB) < 0 && errno == EWOULDBLOCK; }

int main(void) {
  struct timespec t0, t1, pause = {0, 300000000};
  int a, b, c, d, st;
  pid_t pid;

  unlink(PATH);
  a = open(PATH, O_RDWR | O_CREAT, 0644);
  b = open(PATH, O_RDWR);
  c = open(PATH, O_RDONLY);
  ok("three opens", a >= 0 && b >= 0 && c >= 0);
  ok("exclusive", flock(a, LOCK_EX) == 0);
  ok("a second exclusive waits", busy(b, LOCK_EX));
  ok("so does a shared one", busy(b, LOCK_SH));
  ok("taking it again is no change", flock(a, LOCK_EX | LOCK_NB) == 0);
  d = dup(a);
  close(d);
  ok("closing a dup keeps the lock", busy(b, LOCK_EX));
  ok("unlock", flock(a, LOCK_UN) == 0);
  ok("then it can be had", flock(b, LOCK_EX | LOCK_NB) == 0);
  close(b);
  ok("the last close lets it go", flock(a, LOCK_SH | LOCK_NB) == 0);
  ok("shared beside shared", flock(c, LOCK_SH | LOCK_NB) == 0);
  b = open(PATH, O_RDWR);
  ok("but no exclusive beside them", busy(b, LOCK_EX));
  flock(c, LOCK_UN);
  ok("a shared one turns exclusive", flock(a, LOCK_EX | LOCK_NB) == 0);

  clock_gettime(CLOCK_MONOTONIC, &t0);
  if ((pid = fork()) == 0) {
    int e = open(PATH, O_RDWR);

    _exit(flock(e, LOCK_EX) == 0 ? 0 : 1);
  }
  nanosleep(&pause, 0);
  flock(a, LOCK_UN);
  waitpid(pid, &st, 0);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  ok("a blocked one gets it once it is let go",
     WIFEXITED(st) && WEXITSTATUS(st) == 0 &&
         (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 >= 250);
  ok("its exit let go of it", flock(a, LOCK_EX | LOCK_NB) == 0);
  {
    int p[2];

    ok("a pipe takes none", pipe(p) == 0 && flock(p[0], LOCK_SH | LOCK_NB) < 0);
    close(p[0]);
    close(p[1]);
  }
  close(a);
  close(b);
  close(c);
  unlink(PATH);
  printf("flocktest: %s\n", failures ? "FAILED" : "ok");
  return failures != 0;
}
