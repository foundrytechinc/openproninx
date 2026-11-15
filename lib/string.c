#include "inc/cdefs.h"
#include "inc/types.h"
#include "inc/x86.h"

// COPYING
// under 16: overlapping pair; to 256: words; beyond: rep
// the direction flag is never touched
#define REP_MIN 256

typedef uint64_t u64u __aligned(1);
typedef uint32_t u32u __aligned(1);
typedef uint16_t u16u __aligned(1);

static void copy_short(char *d, const char *s, size_t n) {
  if (n >= 8) {
    uint64_t head = *(const u64u *)s, tail = *(const u64u *)(s + n - 8);

    *(u64u *)d = head;
    *(u64u *)(d + n - 8) = tail;
  } else if (n >= 4) {
    uint32_t head = *(const u32u *)s, tail = *(const u32u *)(s + n - 4);

    *(u32u *)d = head;
    *(u32u *)(d + n - 4) = tail;
  } else if (n >= 2) {
    uint16_t head = *(const u16u *)s, tail = *(const u16u *)(s + n - 2);

    *(u16u *)d = head;
    *(u16u *)(d + n - 2) = tail;
  } else if (n == 1) {
    *d = *s;
  }
}

// last word first. don't ask. it works
static void copy_forward(char *d, const char *s, size_t n) {
  size_t words;
  uint64_t tail;

  if (n < 16) {
    copy_short(d, s, n);
    return;
  }
  tail = *(const u64u *)(s + n - 8);
  if (n >= REP_MIN) {
    words = n >> 3;
    __asm __volatile("rep movsq" : "+D"(d), "+S"(s), "+c"(words) : : "memory");
    *(u64u *)(d - (n & ~(size_t)7) + n - 8) = tail;
    return;
  }
  words = n;
  while (words >= 32) {
    uint64_t a = *(const u64u *)s, b = *(const u64u *)(s + 8);
    uint64_t c = *(const u64u *)(s + 16), e = *(const u64u *)(s + 24);

    *(u64u *)d = a;
    *(u64u *)(d + 8) = b;
    *(u64u *)(d + 16) = c;
    *(u64u *)(d + 24) = e;
    d += 32;
    s += 32;
    words -= 32;
  }
  while (words >= 8) {
    *(u64u *)d = *(const u64u *)s;
    d += 8;
    s += 8;
    words -= 8;
  }
  *(u64u *)(d - 8 + words) = tail;
}

// from the far end down
static void copy_backward(char *d, const char *s, size_t n) {
  while (n >= 8) {
    n -= 8;
    *(u64u *)(d + n) = *(const u64u *)(s + n);
  }
  while (n-- != 0)
    d[n] = s[n];
}

int memcmp(const void *v1, const void *v2, size_t n) {
  const uchar *s1, *s2;

  s1 = v1;
  s2 = v2;
  while (n-- > 0) {
    if (*s1 != *s2)
      return *s1 - *s2;
    s1++, s2++;
  }

  return 0;
}

void *memset(void *dst, int c, size_t n) {
  uint64_t v = (uint64_t)(uchar)c * 0x0101010101010101ull;
  char *d = dst;
  size_t words;

  if (n < 16) {
    if (n >= 8) {
      *(u64u *)d = v;
      *(u64u *)(d + n - 8) = v;
    } else if (n >= 4) {
      *(u32u *)d = (uint32_t)v;
      *(u32u *)(d + n - 4) = (uint32_t)v;
    } else if (n >= 2) {
      *(u16u *)d = (uint16_t)v;
      *(u16u *)(d + n - 2) = (uint16_t)v;
    } else if (n == 1) {
      *d = (char)v;
    }
    return dst;
  }
  if (n >= REP_MIN) {
    words = n >> 3;
    __asm __volatile("rep stosq" : "+D"(d), "+c"(words) : "a"(v) : "memory");
    n &= 7;
    if (n != 0)
      *(u64u *)(d - 8 + n) = v;
    return dst;
  }
  while (n >= 8) {
    *(u64u *)d = v;
    d += 8;
    n -= 8;
  }
  if (n != 0)
    *(u64u *)(d - 8 + n) = v;
  return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
  copy_forward(dst, src, n);
  return dst;
}

// ???
void *memmove(void *dst, const void *src, size_t n) {
  if ((size_t)((const char *)dst - (const char *)src) >= n)
    copy_forward(dst, src, n);
  else if (n < 16)
    copy_short(dst, src, n);
  else
    copy_backward(dst, src, n);
  return dst;
}

int strncmp(const char *s1, const char *s2, size_t n) {
  if (n == 0)
    return 0;
  do {
    if (*s1 != *s2++)
      return *(const uchar *)s1 - *(const uchar *)--s2;
    if (*s1++ == 0)
      break;
  } while (--n != 0);
  return 0;
}

int strcmp(const char *s1, const char *s2) {
  while (*s1 == *s2++)
    if (*s1++ == 0)
      return 0;
  return *(const uchar *)s1 - *(const uchar *)--s2;
}

char *strncpy(char *dst, const char *src, size_t n) {
  if (n != 0) {
    char *d = dst;
    const char *s = src;

    do {
      if ((*d++ = *s++) == 0) {
        // the rest of n is cleared. no more
        while (--n != 0)
          *d++ = 0;
        break;
      }
    } while (--n != 0);
  }
  return dst;
}

// strlcpy: always ended; returns the source length
size_t strlcpy(char *dst, const char *src, size_t size) {
  const char *s = src;
  size_t left = size;

  if (left != 0)
    while (--left != 0)
      if ((*dst++ = *s++) == 0)
        break;
  if (left == 0) {
    if (size != 0)
      *dst = 0;
    while (*s++)
      ;
  }
  return (size_t)(s - src - 1);
}

size_t strlen(const char *str) {
  const char *s;

  for (s = str; *s != 0; ++s)
    ;
  return (size_t)(s - str);
}

// the terminator is a char like any other, as strchr says
char *strchr(const char *p, int ch) {
  for (;; ++p) {
    if (*p == (char)ch)
      return (char *)p;
    if (*p == 0)
      return 0;
  }
}
