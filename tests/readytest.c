
#include <err.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static volatile sig_atomic_t chld;

static void on_chld(int sig __unused) { chld = 1; }

static void ok(const char *what, int good) {
  printf("readytest: %-34s %s\n", what, good ? "ok" : "FAILED");
  if (!good)
    failures++;
}

static long elapsed_ms(const struct timeval *a, const struct timeval *b) {
  return (b->tv_sec - a->tv_sec) * 1000 + (b->tv_usec - a->tv_usec) / 1000;
}

int main(void) {
  int fd[2], r;
  struct pollfd pf[2];
  struct timeval before, after, tv;
  struct timespec ts;
  fd_set rd, wr;
  sigset_t none;
  char c;

  if (pipe(fd) < 0)
    err(1, "pipe");

  // nothing written yet
  pf[0].fd = fd[0];
  pf[0].events = POLLIN;
  pf[0].revents = 0;
  ok("empty pipe is not readable", poll(pf, 1, 0) == 0);

  FD_ZERO(&rd);
  FD_SET(fd[0], &rd);
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  ok("select agrees it is not readable", select(fd[0] + 1, &rd, 0, 0, &tv) == 0);

  pf[0].fd = fd[1];
  pf[0].events = POLLOUT;
  pf[0].revents = 0;
  ok("empty pipe is writable", poll(pf, 1, 0) == 1 &&
                                   (pf[0].revents & POLLOUT) != 0);

  if (write(fd[1], "x", 1) != 1)
    err(1, "write");
  pf[0].fd = fd[0];
  pf[0].events = POLLIN;
  pf[0].revents = 0;
  ok("a written byte makes it readable",
     poll(pf, 1, 0) == 1 && (pf[0].revents & POLLIN) != 0);

  FD_ZERO(&rd);
  FD_SET(fd[0], &rd);
  ok("select sees the byte too",
     select(fd[0] + 1, &rd, 0, 0, &tv) == 1 && FD_ISSET(fd[0], &rd));

  if (read(fd[0], &c, 1) != 1)
    err(1, "read");

  // a timeout is a timeout
  gettimeofday(&before, 0);
  r = poll(pf, 1, 120);
  gettimeofday(&after, 0);
  ok("poll waits out its timeout", r == 0 && elapsed_ms(&before, &after) >= 100);

  FD_ZERO(&rd);
  FD_SET(fd[0], &rd);
  tv.tv_sec = 0;
  tv.tv_usec = 120000;
  gettimeofday(&before, 0);
  r = select(fd[0] + 1, &rd, 0, 0, &tv);
  gettimeofday(&after, 0);
  ok("select waits out its timeout",
     r == 0 && elapsed_ms(&before, &after) >= 100);

  close(fd[1]);
  pf[0].fd = fd[0];
  pf[0].events = POLLIN;
  pf[0].revents = 0;
  ok("a closed writer is a hangup",
     poll(pf, 1, 0) == 1 && (pf[0].revents & POLLHUP) != 0);

  FD_ZERO(&rd);
  FD_SET(fd[0], &rd);
  tv.tv_usec = 0;
  ok("select calls the hangup readable",
     select(fd[0] + 1, &rd, 0, 0, &tv) == 1 && FD_ISSET(fd[0], &rd));
  close(fd[0]);

  pf[0].fd = fd[0];
  pf[0].events = POLLIN;
  pf[0].revents = 0;
  ok("a closed descriptor is invalid",
     poll(pf, 1, 0) == 1 && (pf[0].revents & POLLNVAL) != 0);

  FD_ZERO(&rd);
  FD_SET(fd[0], &rd);
  errno = 0;
  ok("select answers EBADF for it",
     select(fd[0] + 1, &rd, 0, 0, &tv) < 0 && errno == EBADF);

  if (pipe(fd) < 0)
    err(1, "pipe");
  pf[0].fd = -1;
  pf[0].events = POLLIN;
  pf[0].revents = 0;
  pf[1].fd = fd[1];
  pf[1].events = POLLOUT;
  pf[1].revents = 0;
  ok("a negative descriptor is ignored",
     poll(pf, 2, 0) == 1 && pf[0].revents == 0 &&
         (pf[1].revents & POLLOUT) != 0);

  sigemptyset(&none);
  ts.tv_sec = 0;
  ts.tv_nsec = 120000000;
  pf[0].fd = fd[0];
  pf[0].events = POLLIN;
  pf[0].revents = 0;
  gettimeofday(&before, 0);
  r = ppoll(pf, 1, &ts, &none);
  gettimeofday(&after, 0);
  ok("ppoll waits and returns nothing",
     r == 0 && elapsed_ms(&before, &after) >= 100);

  // both ends in one call
  FD_ZERO(&rd);
  FD_ZERO(&wr);
  FD_SET(fd[0], &rd);
  FD_SET(fd[1], &wr);
  ok("select reports only the ready half",
     select(fd[1] + 1, &rd, &wr, 0, &tv) == 1 && !FD_ISSET(fd[0], &rd) &&
         FD_ISSET(fd[1], &wr));
  close(fd[0]);
  close(fd[1]);

  // fnusvc's wait; the set has extra ones in it
  // a wide mask read would eat them
  {
    struct {
      sigset_t set;
      uint32_t beside;
    } open = {0, 0xffffffffu};
    sigset_t blocked;
    pid_t pid;
    int st;

    signal(SIGCHLD, on_chld);
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGCHLD);
    sigprocmask(SIG_BLOCK, &blocked, 0);
    if ((pid = fork()) == 0) {
      usleep(100000);
      _exit(0);
    }
    ts.tv_sec = 5;
    ts.tv_nsec = 0;
    gettimeofday(&before, 0);
    r = ppoll(0, 0, &ts, &open.set);
    gettimeofday(&after, 0);
    ok("a child's exit ends ppoll", r < 0 && errno == EINTR && chld &&
                                        elapsed_ms(&before, &after) < 4000);
    ok("and it is reaped", waitpid(pid, &st, WNOHANG) == pid);
    sigprocmask(SIG_UNBLOCK, &blocked, 0);
  }

  if (failures != 0) {
    printf("readytest: %d failed\n", failures);
    return 1;
  }
  printf("readytest: ok\n");
  return 0;
}
