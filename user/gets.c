#include "user.h"

char *gets(char *buf, int max) {
  int i = 0, n;
  char c;

  for (i = 0; i + 1 < max;) {
    n = read(0, &c, 1);
    if (n < 1) {
      break;
    }
    buf[i++] = c;
    if (c == '\n' || c == '\r') {
      break;
    }
  }
  buf[i] = '\0';
  return buf;
}

int read_password(char *buf, int max) {
  struct termios saved, hidden;
  int length;
  if (ioctl(0, TCGETS, (uint64_t)&saved) < 0)
    return -1;
  hidden = saved;
  hidden.c_lflag &= ~ECHO;
  hidden.c_lflag |= ECHOPASS;
  if (ioctl(0, TCSETS, (uint64_t)&hidden) < 0)
    return -1;
  gets(buf, max);
  ioctl(0, TCSETS, (uint64_t)&saved);
  printf("\n");
  for (length = 0; buf[length]; length++)
    if (buf[length] == '\n' || buf[length] == '\r') {
      buf[length] = 0;
      break;
    }
  return 0;
}
