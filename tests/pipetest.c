
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static volatile int sigpipes;

static void ok(const char *what, int good) {
  printf("pipetest: %-40s %s\n", what, good ? "ok" : "FAILED");
  if (!good)
    failures++;
}

static void on_pipe(int sig __unused) { sigpipes++; }

static long fill(int fd, int chunk) {
  static char buf[4096];
  long total = 0;
  int n;

  memset(buf, 'x', sizeof(buf));
  while ((n = (int)write(fd, buf, (size_t)chunk)) > 0)
    total += n;
  return total;
}

int main(void) {
  static char buf[70000], back[70000];
  struct stat st;
  int fd[2], n, i, bad;
  pid_t pid;

  signal(SIGPIPE, on_pipe);
  if (pipe(fd) < 0)
    err(1, "pipe");

  ok("fstat on a pipe works", fstat(fd[0], &st) == 0);
  ok("it is a FIFO and empty", S_ISFIFO(st.st_mode) && st.st_size == 0);
  write(fd[1], "hello", 5);
  fstat(fd[0], &st);
  ok("five bytes show as its size", st.st_size == 5);
  n = -1;
  ok("FIONREAD says five",
     ioctl(fd[0], FIONREAD, (unsigned long)&n) == 0 && n == 5);
  ok("pathconf PIPE_BUF is 512", fpathconf(fd[0], _PC_PIPE_BUF) == 512);
  read(fd[0], back, 5);

  fcntl(fd[1], F_SETFL, fcntl(fd[1], F_GETFL) | O_NONBLOCK);
  n = (int)fill(fd[1], 1000);
  ok("a ring takes 16 KB before it refuses", n >= 16000 && n <= 16384);
  ok("a full ring says EAGAIN", write(fd[1], "x", 1) < 0 && errno == EAGAIN);
  ok("a small write is not split",
     (n = (int)read(fd[0], back, 100)) == 100 &&
         write(fd[1], buf, 512) < 0 && errno == EAGAIN);
  fcntl(fd[0], F_SETFL, fcntl(fd[0], F_GETFL) | O_NONBLOCK);
  while (read(fd[0], back, sizeof(back)) > 0)
    ;
  ok("an empty ring says EAGAIN to a reader", errno == EAGAIN);
  close(fd[0]);
  close(fd[1]);

  if (pipe(fd) < 0)
    err(1, "pipe");
  for (i = 0; i < (int)sizeof(buf); i++)
    buf[i] = (char)(i * 7 + i / 251);
  if ((pid = fork()) == 0) {
    close(fd[0]);
    for (i = 0; i < 3; i++)
      if (write(fd[1], buf, sizeof(buf)) != (ssize_t)sizeof(buf))
        _exit(1);
    _exit(0);
  }
  close(fd[1]);
  bad = 0;
  for (i = 0; i < 3; i++) {
    int got = 0;

    memset(back, 0, sizeof(back));
    while (got < (int)sizeof(back)) {
      n = (int)read(fd[0], back + got, sizeof(back) - (size_t)got);
      if (n <= 0)
        break;
      got += n;
    }
    if (got != (int)sizeof(back) || memcmp(back, buf, sizeof(buf)) != 0)
      bad++;
  }
  ok("70000 bytes three times arrive intact", bad == 0);
  ok("then the end of file reads as zero", read(fd[0], back, 10) == 0);
  waitpid(pid, &n, 0);
  ok("the writer finished", WIFEXITED(n) && WEXITSTATUS(n) == 0);
  close(fd[0]);

  if (pipe(fd) < 0)
    err(1, "pipe");
  for (i = 0; i < 2; i++)
    if (fork() == 0) {
      char line[200];

      close(fd[0]);
      memset(line, 'a' + i, sizeof(line));
      for (n = 0; n < 400; n++)
        if (write(fd[1], line, sizeof(line)) != (ssize_t)sizeof(line))
          _exit(1);
      _exit(0);
    }
  close(fd[1]);
  bad = 0;
  for (n = 0; n < 800; n++) {
    char line[200];
    int got = 0, r;

    while (got < 200 && (r = (int)read(fd[0], line + got, (size_t)(200 - got))) > 0)
      got += r;
    if (got != 200)
      break;
    for (i = 1; i < 200; i++)
      if (line[i] != line[0])
        bad++;
  }
  ok("200-byte writes arrive unmixed", bad == 0 && n == 800);
  wait(0);
  wait(0);
  close(fd[0]);

  if (pipe(fd) < 0)
    err(1, "pipe");
  close(fd[0]);
  errno = 0;
  n = (int)write(fd[1], "x", 1);
  ok("a write with no reader is EPIPE", n < 0 && errno == EPIPE);
  ok("and raises SIGPIPE", sigpipes == 1);
  close(fd[1]);

  if (pipe(fd) < 0)
    err(1, "pipe");
  if ((pid = fork()) == 0) {
    close(fd[1]);
    n = (int)read(fd[0], back, 10);
    _exit(n == 3 ? 0 : 1);
  }
  close(fd[0]);
  sleep(1);
  write(fd[1], "abc", 3);
  waitpid(pid, &n, 0);
  ok("a sleeping reader gets what is written",
     WIFEXITED(n) && WEXITSTATUS(n) == 0);
  close(fd[1]);

  printf("pipetest: %s\n", failures == 0 ? "all ok" : "FAILURES");
  exit(failures == 0 ? 0 : 1);
}
