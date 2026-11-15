// a key stops the count, s boots single user
#include "inc/types.h"
#include "boot/bootdisk.h"

#define DELAY 3

void putstr(const char *s);

static int number(const char *s, int *out) {
  int n = 0, neg = *s == '-';

  if (neg)
    s++;
  if (*s < '0' || *s > '9')
    return -1;
  while (*s >= '0' && *s <= '9')
    n = n * 10 + (*s++ - '0');
  if (*s != 0)
    return -1;
  *out = neg ? -n : n;
  return 0;
}

static void say_seconds(int n) {
  char buf[24];
  int i = sizeof(buf) - 1;

  buf[i] = 0;
  buf[--i] = '\r';
  buf[--i] = ' ';
  buf[--i] = 's';
  buf[--i] = ' ';
  do
    buf[--i] = (char)('0' + n % 10);
  while ((n /= 10) != 0 && i > 0);
  putstr("Booting in ");
  putstr(buf + i);
}

// NO or -1: boot now. -1 does not even ask
void autoboot(struct bootenv *e, int (*key)(void), void (*nap)(void)) {
  const char *v = env_get(e, "autoboot_delay");
  int delay = DELAY, tenths, k, stopped = 0;

  if (v != 0 && (v[0] == 'N' || v[0] == 'n'))
    return;
  if (v != 0 && number(v, &delay) == 0 && delay < 0)
    return;
  while ((k = key()) >= 0)
    ;
  putstr("\nEnter boots, s boots single user, any other key waits.\n");
  for (tenths = delay * 10; tenths > 0 || stopped; tenths--) {
    if (!stopped && tenths % 10 == 0)
      say_seconds(tenths / 10);
    k = key();
    if (k == '\r' || k == '\n')
      break;
    if (k == 's' || k == 'S') {
      env_set(e, "boot_single", "YES");
      putstr("\nSingle user.\n");
      return;
    }
    if (k >= 0 && !stopped) {
      stopped = 1;
      putstr("\nWaiting: Enter boots, s boots single user.\n");
    }
    nap();
  }
  putstr("\n");
}
