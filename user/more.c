#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <ctype.h>
#include <err.h>

static ssize_t readln(FILE *fp, char **out, size_t *cap) {
  ssize_t n;

  if ((n = getline(out, cap, fp)) < 0)
    return -1;
  if (n > 0 && (*out)[n - 1] == '\n')
    (*out)[--n] = 0;
  return n;
}

static int rows = 24;

static int keypress(void) {
  struct termios saved, raw;
  char c;
  int n, tty;

  if ((tty = open("/dev/tty", O_RDONLY)) < 0)
    return 'q';
  if (ioctl(tty, TIOCGETA, (uint64_t)&saved) < 0) {
    close(tty);
    return 'q';
  }
  raw = saved;
  raw.c_lflag &= ~(ICANON | ECHO);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  ioctl(tty, TIOCSETA, (uint64_t)&raw);
  n = (int)read(tty, &c, 1);
  ioctl(tty, TIOCSETA, (uint64_t)&saved);
  close(tty);
  return n == 1 ? c : 'q';
}

static int more(int fd, const char *name) {
  FILE *in;
  static char *line;
  static size_t cap;
  int shown = 0, key;

  if (name != 0)
    printf("::::::::::::::\n%s\n::::::::::::::\n", name), shown += 3;
  if ((in = fd == 0 ? stdin : fdopen(fd, "r")) == 0) {
    warn("%s", name != 0 ? name : "stdin");
    close(fd);
    return 0;
  }
  while (readln(in, &line, &cap) >= 0) {
    if (shown >= rows - 1) {
      fflush(stdout);
      dprintf(2, "--More--");
      key = keypress();
      dprintf(2, "\r        \r");
      if (key == 'q' || key == 'Q') {
        if (in != stdin)
          fclose(in);
        return -1;
      }
      shown = key == '\n' || key == '\r' ? rows - 2 : 0;
    }
    printf("%s\n", line);
    shown++;
  }
  if (in != stdin)
    fclose(in);
  return 0;
}

int main(int argc, char *argv[]) {
  struct winsize ws;
  int i = 1, fd, many;

  if (ioctl(1, TIOCGWINSZ, (uint64_t)&ws) == 0 && ws.ws_row > 2)
    rows = ws.ws_row;
  if (i < argc && argv[i][0] == '-' && isdigit(argv[i][1]))
    rows = atoi(argv[i++] + 1) + 1;
  many = argc - i > 1;
  if (i >= argc) {
    more(0, 0);
    exit(0);
  }
  for (; i < argc; i++) {
    if ((fd = open(argv[i], O_RDONLY)) < 0) {
      warn("%s", argv[i]);
      continue;
    }
    if (more(fd, many ? argv[i] : 0) < 0)
      break;
  }
  exit(0);
}
