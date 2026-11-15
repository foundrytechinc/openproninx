// the ls(1) date column
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <util.h>

#define SIXMONTHS (24 * 60 * 60 * 365 / 2)
static const char *const monthname[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static void put2(char *p, int v, char pad) {
  p[0] = v >= 10 ? (char)('0' + v / 10) : pad;
  p[1] = (char)('0' + v % 10);
}

void timecolumn(int64_t secs, int64_t now, char *out) {
  struct tm t;
  time_t clock = secs;
  const char *mon;
  int i;

  for (i = 0; i < 12; i++)
    out[i] = ' ';
  out[12] = 0;
  if (secs == 0)
    return;

  gmtime_r(&clock, &t);
  mon = monthname[t.tm_mon];
  out[0] = mon[0];
  out[1] = mon[1];
  out[2] = mon[2];
  put2(out + 4, t.tm_mday, ' ');
  if (secs > now + 3600 || secs < now - SIXMONTHS) {
    put2(out + 8, (t.tm_year + 1900) / 100, '0');
    put2(out + 10, (t.tm_year + 1900) % 100, '0');
  } else {
    put2(out + 7, t.tm_hour, '0');
    out[9] = ':';
    put2(out + 10, t.tm_min, '0');
  }
}

