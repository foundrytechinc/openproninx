#include "inc/fnu.h"

#define CONSBUF_MAX (64 * 1024)
#define MSGBUF_MAX (256 * 1024)

static char buf[MSGBUF_MAX];

static uint64 len(const char *s) {
  uint64 n = 0;

  while (s[n] != 0)
    n++;
  return n;
}

static void out(int fd, const char *s, uint64 n) {
  struct fnu_segment seg;
  long w;

  while (n > 0) {
    seg.base = (void *)s;
    seg.len = n;
    if ((w = fnu_write(fd, &seg, 1, FNU_IO_HERE)) <= 0)
      fnu_exit(1);
    s += w;
    n -= (uint64)w;
  }
}

static void fail(const char *what, int err) {
  const char *why = fnu_errstr(-err);

  out(2, "dmesg: ", 7);
  out(2, what, len(what));
  out(2, ": ", 2);
  out(2, why, len(why));
  out(2, "\n", 1);
  fnu_exit(1);
}

static void tail(char last) {
  if (last != '\n')
    out(1, "\n", 1);
}

// msgbuf: the boot before, then this one
// <pri> shown only with -a
static void show(const char *b, uint64 n, int all) {
  uint64 i = 0, start, k;
  char last = '\n';

  while (i < n) {
    for (start = i; i < n && b[i] != '\n'; i++)
      ;
    k = start;
    if (b[k] == '<') {
      for (k++; k < i && b[k] >= '0' && b[k] <= '9'; k++)
        ;
      if (k < i && b[k] == '>' && k > start + 1) {
        if (!all) {
          i++;
          continue;
        }
        start = k + 1;
      }
    }
    if (i < n)
      i++;
    out(1, b + start, i - start);
    last = b[i - 1];
  }
  tail(last);
}

static void msgbuf(int all) {
  uint64 n = sizeof(buf);
  int r;

  if ((r = fnu_sysctl("kern.msgbuf", buf, &n, 0, 0)) < 0 && r != -FNU_ERR_NOMEMORY)
    fail("kern.msgbuf", r);
  if (n > sizeof(buf))
    n = sizeof(buf);
  if (n == 0)
    return;
  show(buf, n, all);
}

static void consbuf(void) {
  uint64 n = sizeof(buf);
  int r;

  if ((r = fnu_sysctl("kern.consbuf", buf, &n, 0, 0)) < 0 &&
      r != -FNU_ERR_NOMEMORY)
    fail("kern.consbuf", r);
  if (n > sizeof(buf))
    n = sizeof(buf);
  if (n == 0)
    return;
  out(1, buf, n);
  tail(buf[n - 1]);
}

int main(int argc, char **argv) {
  if (argc == 2 && argv[1][0] == '-' && argv[1][1] == 's' && argv[1][2] == 0)
    consbuf();
  else if (argc == 2 && argv[1][0] == '-' && argv[1][1] == 'a' && argv[1][2] == 0)
    msgbuf(1);
  else if (argc == 1)
    msgbuf(0);
  else {
    out(2, "usage: dmesg [-a | -s]\n", 23);
    return 1;
  }
  return 0;
}
