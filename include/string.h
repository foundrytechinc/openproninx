#ifndef _STRING_H_
#define _STRING_H_

#include <sys/types.h>
#include <strings.h>

void *memchr(const void *s, int c, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *b, int c, size_t n);
char *strcat(char *s, const char *append);
char *strncat(char *s, const char *append, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);
void strmode(mode_t mode, char *out);
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t maxlen);
char *strstr(const char *big, const char *little);
char *strerror(int errnum);
int strerror_r(int errnum, char *buf, size_t buflen);
size_t strspn(const char *s, const char *charset);
size_t strcspn(const char *s, const char *charset);
char *strpbrk(const char *s, const char *charset);
char *strsep(char **stringp, const char *delim);
char *strtok(char *str, const char *sep);
char *strtok_r(char *str, const char *sep, char **last);
void explicit_bzero(void *b, size_t len);
int strcoll(const char *, const char *);
size_t strxfrm(char *, const char *, size_t);
int strcasecmp(const char *s1, const char *s2);
int strncasecmp(const char *s1, const char *s2, size_t n);
void bzero(void *b, size_t len);
char *index(const char *, int);
char *rindex(const char *, int);
void bcopy(const void *src, void *dst, size_t len);
int bcmp(const void *b1, const void *b2, size_t len);
int timingsafe_bcmp(const void *b1, const void *b2, size_t len);

#endif
char *strchrnul(const char *, int);
void *memccpy(void *, const void *, int, size_t);
