#include <string.h>
#include <strings.h>
#include <stdlib.h>

char *strcpy(char *dst, const char *src) {
  char *os = dst;

  while ((*dst++ = *src++) != 0)
    ;
  return os;
}

char *strcat(char *dst, const char *src) {
  strcpy(dst + strlen(dst), src);
  return dst;
}

char *strrchr(const char *s, int c) {
  const char *last = 0;

  do {
    if (*s == (char)c)
      last = s;
  } while (*s++ != 0);
  return (char *)last;
}

void bzero(void *b, size_t len) { memset(b, 0, len); }

char *index(const char *s, int c) { return strchr(s, c); }

char *rindex(const char *s, int c) { return strrchr(s, c); }

void bcopy(const void *src, void *dst, size_t len) { memmove(dst, src, len); }

int bcmp(const void *b1, const void *b2, size_t len) { return memcmp(b1, b2, len); }

