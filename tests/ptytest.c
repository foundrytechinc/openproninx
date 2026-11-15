
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <util.h>

static int failures;

static void ok(const char *what, int good) {
  printf("ptytest: %-40s %s\n", what, good ? "ok" : "FAILED");
  if (!good)
    failures++;
}

static void show(const char *what, const char *buf, int n) {
  int i;

  printf("ptytest: %s %d bytes: ", what, n);
  for (i = 0; i < n; i++)
    if (buf[i] < ' ' || buf[i] == 0x7f)
      printf("^%c", buf[i] == 0x7f ? '?' : buf[i] + '@');
    else
      putchar(buf[i]);
  putchar('\n');
}

static int gather(int master, char *buf, int size, const char *want, int secs) {
  struct timeval tv;
  fd_set rd;
  int got = 0, n;

  buf[0] = 0;
  while (got < size - 1) {
    FD_ZERO(&rd);
    FD_SET(master, &rd);
    tv.tv_sec = secs;
    tv.tv_usec = 0;
    if (select(master + 1, &rd, 0, 0, &tv) <= 0)
      break;
    if ((n = (int)read(master, buf + got, (size_t)(size - 1 - got))) <= 0)
      return n < 0 && got == 0 ? -1 : got;
    got += n;
    buf[got] = 0;
    if (want != 0 && strstr(buf, want) != 0)
      break;
  }
  return got;
}

int main(void) {
  struct winsize ws = {.ws_row = 24, .ws_col = 80}, back;
  char name[16], buf[2048];
  int master, slave, status, n;
  pid_t pid;

  if (openpty(&master, &slave, name, 0, &ws) < 0)
    err(1, "openpty");
  ok("openpty names the slave", strncmp(name, "/dev/", 5) == 0);
  ok("the size goes with it",
     ioctl(slave, TIOCGWINSZ, &back) == 0 && back.ws_col == 80 &&
         back.ws_row == 24);
  if ((pid = fork()) < 0)
    err(1, "fork");
  if (pid == 0) {
    close(master);
    if (login_tty(slave) < 0)
      _exit(2);
    execl("/usr/bin/sh", "sh", "-c",
          "test -t 0 && echo tty-yes; read l; echo got:$l; exit 7", (char *)0);
    _exit(3);
  }
  close(slave);
  n = gather(master, buf, sizeof(buf), "tty-yes", 5);
  ok("the child's input is a terminal", n > 0 && strstr(buf, "tty-yes") != 0);
  write(master, "hello\n", 6);
  n = gather(master, buf, sizeof(buf), "got:hello", 5);
  ok("the master's writes are its input",
     n > 0 && strstr(buf, "got:hello") != 0);
  if (n <= 0 || strstr(buf, "got:hello") == 0)
    show("the master read", buf, n);
  ok("and the line discipline echoes them", strstr(buf, "hello\r\n") != 0);
  waitpid(pid, &status, 0);
  ok("the child's end comes back as it was",
     WIFEXITED(status) && WEXITSTATUS(status) == 7);
  n = gather(master, buf, sizeof(buf), 0, 2);
  ok("and hangs the master up", n <= 0);
  close(master);

  printf("ptytest: %s\n", failures == 0 ? "all ok" : "FAILURES");
  exit(failures == 0 ? 0 : 1);
}
