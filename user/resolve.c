#include "inc/fnu.h"

static uint64 len(const char *s) {
  uint64 n = 0;

  while (s[n] != 0)
    n++;
  return n;
}

static void out(int fd, const char *s) {
  struct fnu_segment seg;

  seg.base = (void *)s;
  seg.len = len(s);
  fnu_write(fd, &seg, 1, FNU_IO_HERE);
}

static char *number(char *p, int v) {
  if (v >= 100)
    *p++ = (char)('0' + v / 100);
  if (v >= 10)
    *p++ = (char)('0' + v / 10 % 10);
  *p++ = (char)('0' + v % 10);
  return p;
}

int main(int argc, char **argv) {
  uint8 a[4];
  char text[16], *p;
  int i, k, r, status = 0;

  if (argc < 2) {
    out(2, "usage: resolve name ...\n");
    return 2;
  }
  for (i = 1; i < argc; i++) {
    if ((r = fnu_resolve(argv[i], a, 5000)) < 0) {
      out(2, "resolve: ");
      out(2, argv[i]);
      out(2, ": ");
      out(2, fnu_errstr(r));
      out(2, "\n");
      status = 1;
      continue;
    }
    for (p = text, k = 0; k < 4; k++) {
      p = number(p, a[k]);
      *p++ = k < 3 ? '.' : 0;
    }
    out(1, argv[i]);
    out(1, " ");
    out(1, text);
    out(1, "\n");
  }
  return status;
}
