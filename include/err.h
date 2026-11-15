#ifndef _ERR_H_
#define _ERR_H_

#include <stdarg.h>
#include "inc/cdefs.h"

void err(int eval, const char *fmt, ...) __dead __printf0like(2, 3);
void errx(int eval, const char *fmt, ...) __dead __printf0like(2, 3);
void warn(const char *fmt, ...) __printf0like(1, 2);
void warnx(const char *fmt, ...) __printf0like(1, 2);
void verr(int eval, const char *fmt, va_list ap) __dead __printf0like(2, 0);
void verrx(int eval, const char *fmt, va_list ap) __dead
    __printf0like(2, 0);
void vwarn(const char *fmt, va_list ap) __printf0like(1, 0);
void vwarnx(const char *fmt, va_list ap) __printf0like(1, 0);
void errc(int eval, int code, const char *fmt, ...) __dead
    __printf0like(3, 4);
void warnc(int code, const char *fmt, ...) __printf0like(2, 3);
void verrc(int eval, int code, const char *fmt, va_list ap) __dead
    __printf0like(3, 0);
void vwarnc(int code, const char *fmt, va_list ap) __printf0like(2, 0);

#endif
