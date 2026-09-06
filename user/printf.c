#include "user.h"

#define PRINTF_BUF_SIZE 512

struct print_buf {
  int fd;
  int pos;
  int print_cnt;
  char buf[PRINTF_BUF_SIZE];
};

static void pbuf_flush(struct print_buf *pb) {
  if (pb->pos > 0) {
    write(pb->fd, pb->buf, pb->pos);
    pb->pos = 0;
  }
}

static inline void printchar(struct print_buf *pb, char c) {
  pb->buf[pb->pos++] = c;
  pb->print_cnt++;
  if (pb->pos >= PRINTF_BUF_SIZE) {
    pbuf_flush(pb);
  }
}

static void printint(struct print_buf *pb, long long xx, int base, int sign) {
  static char digits[] = "0123456789abcdef";
  char buf[32];
  int i, neg;
  unsigned long long x;

  neg = 0;
  if (sign && xx < 0) {
    neg = 1;
    x = (unsigned long long)-xx;
  } else {
    x = (unsigned long long)xx;
  }

  i = 0;
  do {
    buf[i++] = digits[x % base];
  } while ((x /= base) != 0);
  if (neg)
    buf[i++] = '-';

  while (--i >= 0) {
    printchar(pb, buf[i]);
  }
}

/* Print string s with field width |width|.
 * left_justify=1: pad on the right; 0: pad on the left. */
static void printstr_width(struct print_buf *pb, const char *s,
                           int width, int left_justify) {
  int len = 0;
  const char *p = s;
  if (!s) s = "(null)";
  while (*p++) len++;

  if (!left_justify) {
    int pad = width - len;
    while (pad-- > 0) printchar(pb, ' ');
  }
  while (*s) printchar(pb, *s++);
  if (left_justify) {
    int pad = width - len;
    while (pad-- > 0) printchar(pb, ' ');
  }
}

/* Print integer with field width, right/left-aligned. */
static void printint_width(struct print_buf *pb, long long v, int base,
                           int sign, int width, int left_justify,
                           char pad_char) {
  char buf[32];
  int i = 0, neg = 0;
  unsigned long long x;

  if (sign && v < 0) { neg = 1; x = (unsigned long long)-v; }
  else                x = (unsigned long long)v;

  do { buf[i++] = "0123456789abcdef"[x % base]; } while ((x /= base) != 0);
  if (neg) buf[i++] = '-';

  int len = i;
  if (!left_justify) {
    int pad = width - len;
    while (pad-- > 0) printchar(pb, pad_char);
  }
  while (--i >= 0) printchar(pb, buf[i]);
  if (left_justify) {
    int pad = width - len;
    while (pad-- > 0) printchar(pb, ' ');
  }
}

static int do_printf(const char *fmt, int fd, va_list va) {
  struct print_buf pb;
  int c, i;

  pb.fd = fd;
  pb.pos = 0;
  pb.print_cnt = 0;

  for (i = 0; (c = fmt[i] & 0xff) != 0; i++) {
    if (c != '%') {
      printchar(&pb, c);
      continue;
    }

    /* Start of format spec */
    i++;
    c = fmt[i] & 0xff;
    if (c == 0) break;

    /* Flags */
    int left_justify = 0;
    while (c == '-') {
      left_justify = 1;
      i++;
      c = fmt[i] & 0xff;
    }

    /* Width: either '*' (from arg) or digits */
    int width = 0;
    if (c == '*') {
      width = va_arg(va, int);
      if (width < 0) {
        left_justify = 1;
        width = -width;
      }
      i++;
      c = fmt[i] & 0xff;
    } else {
      while (c >= '0' && c <= '9') {
        width = width * 10 + (c - '0');
        i++;
        c = fmt[i] & 0xff;
      }
    }

    char pad_char = ' ';

    /* Length modifier */
    int is_long = 0;
    if (c == 'l') {
      is_long = 1;
      i++;
      c = fmt[i] & 0xff;
      if (c == 'l') {
        i++;
        c = fmt[i] & 0xff;
      } /* ll */
    }

    /* Conversion */
    if (c == 'd' || c == 'i') {
      long long val = is_long ? va_arg(va, long long) : (long long)va_arg(va, int);
      if (width > 0)
        printint_width(&pb, val, 10, 1, width, left_justify, pad_char);
      else
        printint(&pb, val, 10, 1);
    } else if (c == 'u') {
      unsigned long long val = is_long ? (unsigned long long)va_arg(va, unsigned long long)
                                       : (unsigned long long)va_arg(va, unsigned int);
      if (width > 0)
        printint_width(&pb, (long long)val, 10, 0, width, left_justify, pad_char);
      else
        printint(&pb, (long long)val, 10, 0);
    } else if (c == 'x' || c == 'p') {
      unsigned long long val = is_long ? (unsigned long long)va_arg(va, unsigned long long)
                                       : (unsigned long long)(unsigned int)va_arg(va, int);
      if (width > 0)
        printint_width(&pb, (long long)val, 16, 0, width, left_justify, pad_char);
      else
        printint(&pb, (long long)val, 16, 0);
    } else if (c == 's') {
      char *s = va_arg(va, char *);
      if (!s) s = "(null)";
      if (width > 0)
        printstr_width(&pb, s, width, left_justify);
      else
        while (*s) printchar(&pb, *s++);
    } else if (c == 'c') {
      char ch = (char)va_arg(va, int);
      printchar(&pb, ch);
    } else if (c == '%') {
      printchar(&pb, '%');
    } else if (c == 0) {
      break;
    } else {
      /* Unknown specifier — print literally */
      printchar(&pb, '%');
      if (left_justify) printchar(&pb, '-');
      printchar(&pb, c);
    }
  }

  pbuf_flush(&pb);
  return pb.print_cnt;
}

// Print to stdout. Supports: %c %d %i %u %x %p %s %lld %lu %*s %-Ns %Nd etc.
int printf(const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  int res = do_printf(fmt, 1, va);
  va_end(va);
  return res;
}

// Print to the given fd.
int dprintf(int fd, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  int res = do_printf(fmt, fd, va);
  va_end(va);
  return res;
}

int putchar(int c) {
  char ch = (char)c;
  write(1, &ch, 1);
  return (uchar)ch;
}
