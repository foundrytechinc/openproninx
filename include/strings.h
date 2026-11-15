#ifndef _STRINGS_H_
#define _STRINGS_H_

#include <sys/types.h>

int strcasecmp(const char *s1, const char *s2);
int strncasecmp(const char *s1, const char *s2, size_t n);
void bzero(void *b, size_t len);
void bcopy(const void *src, void *dst, size_t len);
int bcmp(const void *b1, const void *b2, size_t len);
int ffs(int value);
char *index(const char *, int);
char *rindex(const char *, int);

#endif
