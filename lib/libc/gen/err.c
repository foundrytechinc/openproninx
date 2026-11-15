// err(3): the program's name, the message, and errno's text
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char *__progname;

void vwarnx(const char *fmt, va_list ap) {
  fprintf(stderr, "%s: ", __progname);
  if (fmt != 0)
    vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
}

void vwarn(const char *fmt, va_list ap) {
  int saved = errno;

  fprintf(stderr, "%s: ", __progname);
  if (fmt != 0) {
    vfprintf(stderr, fmt, ap);
    fputs(": ", stderr);
  }
  fprintf(stderr, "%s\n", strerror(saved));
}

void verr(int eval, const char *fmt, va_list ap) {
  vwarn(fmt, ap);
  exit(eval);
}

void verrx(int eval, const char *fmt, va_list ap) {
  vwarnx(fmt, ap);
  exit(eval);
}

void err(int eval, const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  verr(eval, fmt, ap);
  va_end(ap);
}

void errx(int eval, const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  verrx(eval, fmt, ap);
  va_end(ap);
}

void warn(const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  vwarn(fmt, ap);
  va_end(ap);
}

void warnx(const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  vwarnx(fmt, ap);
  va_end(ap);
}

// same, for a call that returns its error as a value
void vwarnc(int code, const char *fmt, va_list ap) {
  fprintf(stderr, "%s: ", __progname);
  if (fmt != 0) {
    vfprintf(stderr, fmt, ap);
    fputs(": ", stderr);
  }
  fprintf(stderr, "%s\n", strerror(code));
}

void verrc(int eval, int code, const char *fmt, va_list ap) {
  vwarnc(code, fmt, ap);
  exit(eval);
}

void warnc(int code, const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  vwarnc(code, fmt, ap);
  va_end(ap);
}

void errc(int eval, int code, const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  verrc(eval, code, fmt, ap);
  va_end(ap);
}
