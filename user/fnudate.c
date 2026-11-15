#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fnu/sys.h>
#include <sys/syscall.h>
#include "inc/product.h"
#include <err.h>

static const char *weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *months[]   = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static char *pad2(int v, char *out) {
  out[0] = '0' + (v / 10) % 10;
  out[1] = '0' + v % 10;
  out[2] = 0;
  return out;
}

static char *fmt4(int v, char *out) {
  int i = 3;
  out[4] = 0;
  if (v == 0) { out[i--] = '0'; }
  while (v > 0 && i >= 0) { out[i--] = '0' + (v % 10); v /= 10; }
  while (i >= 0) out[i--] = '0';
  return out;
}

int main(int argc, char **argv) {
  struct fnu_rtctime t;
  char a[4], b[4], c[4], d[4], y[8];
  int r;
  (void)argc; (void)argv;

  if ((r = fnu_rtctime(&t)) < 0) {
    warnx("rtctime: %s", fnu_errstr(r));
    exit(1);
  }

  printf("%s %s %s %s:%s:%s %s RTC\n",
         weekdays[t.wday < 7 ? t.wday : 0],
         months[(t.mon >= 1 && t.mon <= 12) ? t.mon - 1 : 0],
         pad2(t.mday, a),
         pad2(t.hour, b),
         pad2(t.min,  c),
         pad2(t.sec,  d),
         fmt4(t.year, y));

  if (t.year < 2000) {
    dprintf(2, "fnudate: warning -- RTC century byte missing; "
               "QEMU host time may be inaccurate\n");
  }
  exit(0);
}
