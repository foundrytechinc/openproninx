// FORMATTING
#include "defs.h"
#include "inc/stdarg.h"

struct out {
  void (*func)(int, void *);
  void *arg;
  size_t n;
};

static void put(struct out *o, char c) {
  o->func(c & 0xff, o->arg);
  o->n++;
}

static void pad(struct out *o, char c, int n) {
  while (n-- > 0)
    put(o, c);
}

static int digits(uint64_t v, int base, int upper, char *tmp) {
  const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  int n = 0;

  do {
    tmp[n++] = set[v % (uint64_t)base];
    v /= (uint64_t)base;
  } while (v != 0);
  return n;
}

int kvprintf(const char *fmt, void (*func)(int, void *), void *arg,
             va_list ap) {
  struct out o = {func, arg, 0};

  for (; *fmt != 0; fmt++) {
    int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
    int width = 0, prec = -1, lng = 0, c;
    uint64_t v;
    int neg = 0;

    if (*fmt != '%') {
      put(&o, *fmt);
      continue;
    }
    for (fmt++;; fmt++) {
      if (*fmt == '-')
        left = 1;
      else if (*fmt == '0')
        zero = 1;
      else if (*fmt == '+')
        plus = 1;
      else if (*fmt == ' ')
        space = 1;
      else if (*fmt == '#')
        alt = 1;
      else
        break;
    }
    if (*fmt == '*') {
      width = va_arg(ap, int);
      if (width < 0) {
        left = 1;
        width = -width;
      }
      fmt++;
    } else {
      while (*fmt >= '0' && *fmt <= '9')
        width = width * 10 + (*fmt++ - '0');
    }
    if (*fmt == '.') {
      fmt++;
      prec = 0;
      if (*fmt == '*') {
        prec = va_arg(ap, int);
        fmt++;
      } else {
        while (*fmt >= '0' && *fmt <= '9')
          prec = prec * 10 + (*fmt++ - '0');
      }
    }
    for (;; fmt++) {
      if (*fmt == 'l')
        lng++;
      else if (*fmt == 'h')
        lng--;
      else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't')
        lng = 1;
      else
        break;
    }
    c = *fmt;
    if (c == 0)
      break;

    switch (c) {
    case '%':
      put(&o, '%');
      break;
    case 'c':
      pad(&o, ' ', left ? 0 : width - 1);
      put(&o, (char)va_arg(ap, int));
      pad(&o, ' ', left ? width - 1 : 0);
      break;
    case 's': {
      const char *s = va_arg(ap, const char *);
      int len = 0;

      if (s == 0)
        s = "(null)";
      while (s[len] != 0 && (prec < 0 || len < prec))
        len++;
      pad(&o, ' ', left ? 0 : width - len);
      for (int i = 0; i < len; i++)
        put(&o, s[i]);
      pad(&o, ' ', left ? width - len : 0);
      break;
    }
    case 'd':
    case 'i':
    case 'u':
    case 'x':
    case 'X':
    case 'o':
    case 'p': {
      char tmp[24], sign = 0;
      int base = c == 'o' ? 8 : (c == 'x' || c == 'X' || c == 'p') ? 16 : 10;
      int n, len, zeros, prefix = 0;

      if (c == 'p') {
        v = (uint64_t)(uintptr_t)va_arg(ap, void *);
        alt = 1;
      } else if (c == 'd' || c == 'i') {
        int64_t s = lng >= 1 ? va_arg(ap, long)
                    : lng == 0 ? (int64_t)va_arg(ap, int)
                    : lng == -1 ? (int64_t)(short)va_arg(ap, int)
                                : (int64_t)(signed char)va_arg(ap, int);

        neg = s < 0;
        v = neg ? (uint64_t)0 - (uint64_t)s : (uint64_t)s;
      } else {
        v = lng >= 1 ? va_arg(ap, unsigned long)
            : lng == 0 ? (uint64_t)va_arg(ap, unsigned int)
            : lng == -1 ? (uint64_t)(unsigned short)va_arg(ap, unsigned int)
                        : (uint64_t)(unsigned char)va_arg(ap, unsigned int);
      }
      if (c == 'd' || c == 'i') {
        sign = neg ? '-' : plus ? '+' : space ? ' ' : 0;
      } else if (alt && v != 0 && base == 16) {
        prefix = 2;
      } else if (alt && base == 8) {
        prefix = 1;
      }
      n = (prec == 0 && v == 0) ? 0 : digits(v, base, c == 'X', tmp);
      zeros = prec > n ? prec - n : 0;
      len = n + zeros + prefix + (sign != 0);
      if (!left && !(zero && prec < 0))
        pad(&o, ' ', width - len);
      if (sign)
        put(&o, sign);
      if (prefix == 2) {
        put(&o, '0');
        put(&o, c == 'X' ? 'X' : 'x');
      } else if (prefix == 1 && zeros == 0) {
        put(&o, '0');
      }
      if (!left && zero && prec < 0)
        pad(&o, '0', width - len);
      pad(&o, '0', zeros);
      while (n > 0)
        put(&o, tmp[--n]);
      if (left)
        pad(&o, ' ', width - len);
      break;
    }
    default:
      put(&o, '%');
      put(&o, (char)c);
      break;
    }
  }
  return (int)o.n;
}

struct snbuf {
  char *buf;
  size_t size, n;
};

static void snput(int c, void *arg) {
  struct snbuf *b = arg;

  if (b->n + 1 < b->size)
    b->buf[b->n] = (char)c;
  b->n++;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
  struct snbuf b = {buf, size, 0};
  int n = kvprintf(fmt, snput, &b, ap);

  if (size > 0)
    buf[b.n < size ? b.n : size - 1] = 0;
  return n;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
  va_list ap;
  int n;

  va_start(ap, fmt);
  n = vsnprintf(buf, size, fmt, ap);
  va_end(ap);
  return n;
}
