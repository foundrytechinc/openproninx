#include <string.h>

void explicit_bzero(void *p, size_t n) {
  volatile unsigned char *v = p;

  while (n-- > 0)
    *v++ = 0;
}

int timingsafe_bcmp(const void *a, const void *b, size_t n) {
  const unsigned char *p = a, *q = b;
  int r = 0;

  while (n-- > 0)
    r |= *p++ ^ *q++;
  return r != 0;
}
