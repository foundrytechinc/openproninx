
#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static int failures;

static void ok(const char *what, int good) {
  printf("sertest: %-34s %s\n", what, good ? "ok" : "FAILED");
  if (!good)
    failures++;
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "/dev/ttyu1";
  struct termios saved, t;
  int fd, bits, n;

  if ((fd = open(path, O_RDWR)) < 0)
    err(1, "%s", path);
  if (tcgetattr(fd, &saved) < 0)
    err(1, "tcgetattr");

  ok("DTR and RTS are up", ioctl(fd, TIOCMGET, (uint64_t)&bits) == 0 &&
                               (bits & TIOCM_DTR) && (bits & TIOCM_RTS));

  bits = TIOCM_RTS;
  if (ioctl(fd, TIOCMBIC, (uint64_t)&bits) < 0)
    err(1, "TIOCMBIC");
  ok("RTS goes down", ioctl(fd, TIOCMGET, (uint64_t)&bits) == 0 &&
                          !(bits & TIOCM_RTS) && (bits & TIOCM_DTR));
  bits = TIOCM_RTS;
  if (ioctl(fd, TIOCMBIS, (uint64_t)&bits) < 0)
    err(1, "TIOCMBIS");
  ok("RTS comes back", ioctl(fd, TIOCMGET, (uint64_t)&bits) == 0 &&
                           (bits & TIOCM_RTS));

  t = saved;
  t.c_ispeed = t.c_ospeed = B9600;
  t.c_cflag = (t.c_cflag & ~CSIZE) | CS7 | PARENB;
  ok("9600 7e1 is taken", tcsetattr(fd, TCSADRAIN, &t) == 0 &&
                              tcgetattr(fd, &t) == 0 && t.c_ospeed == B9600 &&
                              (t.c_cflag & CSIZE) == CS7 &&
                              (t.c_cflag & PARENB) != 0);
  t.c_ispeed = t.c_ospeed = 4000000;
  ok("too fast a speed is refused", tcsetattr(fd, TCSANOW, &t) < 0);
  t.c_ispeed = t.c_ospeed = 1;
  ok("too slow a speed is refused", tcsetattr(fd, TCSANOW, &t) < 0);

  t = saved;
  ok("the line is set back", tcsetattr(fd, TCSADRAIN, &t) == 0);

  ok("a break is asked for and let go", tcsendbreak(fd, 1) == 0);
  ok("the output drains", tcdrain(fd) == 0);
  ok("nothing is left queued",
     ioctl(fd, TIOCOUTQ, (uint64_t)&n) == 0 && n == 0);

  close(fd);
  printf("sertest: %s\n", failures == 0 ? "all ok" : "FAILURES");
  exit(failures == 0 ? 0 : 1);
}
