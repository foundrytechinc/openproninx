// WITHOUT POSIX
#include "inc/fnu.h"

static uint64 len(const char *s) {
  uint64 n = 0;

  while (s[n] != 0)
    n++;
  return n;
}

static void say(const char *s) {
  struct fnu_segment seg;

  seg.base = (void *)s;
  seg.len = len(s);
  fnu_write(1, &seg, 1, FNU_IO_HERE);
}

static void sayn(long v) {
  char buf[24];
  int i = (int)sizeof(buf);
  int neg = v < 0;

  if (neg)
    v = -v;
  buf[--i] = 0;
  do {
    buf[--i] = (char)('0' + v % 10);
    v /= 10;
  } while (v != 0 && i > 1);
  if (neg)
    buf[--i] = '-';
  say(buf + i);
}

void fnu_main(void) {
  struct fnu_info inf;
  struct fnu_stat st;
  struct fnu_procids ids;
  struct fnu_ident id;
  struct fnu_segment seg;
  struct fnu_timespec ts;
  int fd, r;
  char buf[64];

  say("fnuonly: the system without POSIX\n");

  if (fnu_procids(0, &ids) == 0 && fnu_ident_get(&id) == 0) {
    say("  pid ");
    sayn(ids.pid);
    say(", uid ");
    sayn(id.uid);
    say("\n");
  }

  if (fnu_gettime(&ts, FNU_CLOCK_UPTIME) == 0) {
    say("  up ");
    sayn(ts.sec);
    say(" s\n");
  }
  if (fnu_info(&inf) == 0) {
    say("  ram free ");
    sayn((long)(inf.free_ram / 1024));
    say(" KB, procs ");
    sayn(inf.nprocs);
    say("\n");
  }
  if ((fd = fnu_open(FNU_FD_CWD, "/etc/profile", FNU_OPEN_READ, 0)) >= 0) {
    if (fnu_getattr(fd, 0, 0, &st) == 0) {
      say("  /etc/profile is ");
      sayn((long)st.size);
      say(" bytes, mode ");
      sayn(st.mode & 07777);
      say("\n");
    }
    seg.base = buf;
    seg.len = 16;
    if ((r = (int)fnu_read(fd, &seg, 1, FNU_IO_HERE)) > 0) {
      say("  first bytes read: ");
      sayn(r);
      say("\n");
    }
    fnu_close(fd);
  }
  r = (int)fnu_open(FNU_FD_CWD, "/no/such/file", FNU_OPEN_READ, 0);
  say("  opening what is not there answers ");
  sayn(r);
  say(r == -FNU_ERR_NOTFOUND ? " (not found)\n" : " (unexpected)\n");

  say(r == -FNU_ERR_NOTFOUND ? "fnuonly: ok\n" : "fnuonly: FAILED\n");
  fnu_exit(r == -FNU_ERR_NOTFOUND ? 0 : 1);
}
