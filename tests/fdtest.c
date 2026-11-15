
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

static void ok(const char *what, int good) {
  printf("fdtest: %-44s %s\n", what, good ? "ok" : "FAILED");
  if (!good)
    failures++;
}

int main(void) {
  struct rlimit rl;
  struct pollfd *pf;
  fd_set rd;
  int fd[2], i, n, last, high;
  pid_t pid;
  int st;

  ok("the soft limit is 256 or more",
     getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur >= 256);
  ok("sysconf agrees", sysconf(_SC_OPEN_MAX) == (long)rl.rlim_cur);

  rl.rlim_cur = 1000;
  ok("the limit is raised", setrlimit(RLIMIT_NOFILE, &rl) == 0);
  last = -1;
  for (n = 0; n < 2000; n++) {
    if ((last = open("/dev/null", O_RDONLY)) < 0)
      break;
  }
  ok("open stops at the limit with EMFILE", last < 0 && errno == EMFILE);
  ok("and the limit is where it was put", n >= 990 && n <= 1000);
  high = n + 2;
  for (i = 3; i < high; i++)
    close(i);
  ok("everything closes", close(3) < 0 && errno == EBADF);

  ok("dup2 to 900 works", dup2(1, 900) == 900);
  ok("and the descriptor is there", write(900, "", 0) == 0);
  ok("dup2 past the limit is refused", dup2(1, 1000) < 0);
  close(900);

  if (pipe(fd) < 0)
    err(1, "pipe");
  for (i = 0; i < 5; i++)
    dup2(fd[1], 500 + i);
  fcntl(503, F_SETFD, FD_CLOEXEC);
  if ((pid = fork()) == 0) {
    char c = 'k';

    ok("the child has the high ones", write(502, &c, 1) == 1);
    ok("and the flag came with them", (fcntl(503, F_GETFD) & FD_CLOEXEC) != 0);
    _exit(failures);
  }
  waitpid(pid, &st, 0);
  ok("the child agreed", WIFEXITED(st) && WEXITSTATUS(st) == 0);
  for (i = 0; i < 5; i++)
    close(500 + i);

  close(fd[0]);
  close(fd[1]);

  if (pipe(fd) < 0)
    err(1, "pipe");
  dup2(fd[0], 700);
  write(fd[1], "x", 1);
  FD_ZERO(&rd);
  FD_SET(700, &rd);
  ok("select over descriptor 700", select(701, &rd, 0, 0, 0) == 1 &&
                                       FD_ISSET(700, &rd));
  pf = calloc(300, sizeof(*pf));
  for (i = 0; i < 300; i++) {
    pf[i].fd = i < 299 ? 5000 + i : 700;
    pf[i].events = POLLIN;
  }
  n = poll(pf, 300, 0);
  ok("poll over 300 entries finds the one that is ready",
     n == 300 && (pf[299].revents & POLLIN) && (pf[0].revents & POLLNVAL));
  free(pf);
  pf = calloc(5000, sizeof(*pf));
  ok("poll over more than the call takes is EINVAL",
     poll(pf, 5000, 0) < 0 && errno == EINVAL);
  free(pf);
  close(700);
  ok("F_SETFL O_NONBLOCK sets it", fcntl(fd[0], F_SETFL, O_NONBLOCK) == 0 &&
                                       (fcntl(fd[0], F_GETFL) & O_NONBLOCK) != 0);
  ok("F_SETFL 0 clears it", fcntl(fd[0], F_SETFL, 0) == 0 &&
                                (fcntl(fd[0], F_GETFL) & O_NONBLOCK) == 0);
  ok("the access mode stays", (fcntl(fd[0], F_GETFL) & O_ACCMODE) == O_RDONLY);
  close(fd[0]);
  close(fd[1]);

  printf("fdtest: %s\n", failures == 0 ? "all ok" : "FAILURES");
  exit(failures == 0 ? 0 : 1);
}
