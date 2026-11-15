#ifndef _STDLIB_H_
#define _STDLIB_H_

#include <sys/types.h>
#include "inc/cdefs.h"

#ifndef _WCHAR_T_DEFINED_
#define _WCHAR_T_DEFINED_
typedef __wchar_t wchar_t;
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7fffffff

extern char **environ;
extern char *__progname;
const char *getprogname(void);
void setprogname(const char *);
char *devname(dev_t, mode_t);
char *getbsize(int *, long *);

void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *aligned_alloc(size_t alignment, size_t size);

// getcap(3)
int cgetent(char **, char **, const char *);
int cgetmatch(char *, const char *);
char *cgetcap(char *, const char *, int);
int cgetnum(char *, const char *, long *);
int cgetset(const char *);
int cgetstr(char *, const char *, char **);
int cgetustr(char *, const char *, char **);
int cgetfirst(char **, char **);
int cgetnext(char **, char **);
int cgetclose(void);
void *realloc(void *ptr, size_t size);
quad_t strtoq(const char *nptr, char **endptr, int base);
u_quad_t strtouq(const char *nptr, char **endptr, int base);
void *reallocarray(void *ptr, size_t nmemb, size_t size);
void *recallocarray(void *ptr, size_t oldnmemb, size_t newnmemb, size_t size);
void freezero(void *ptr, size_t size);
void free(void *ptr);

void exit(int status) __dead;
void _Exit(int status) __dead;
void abort(void) __dead;
int atexit(void (*function)(void));

int atoi(const char *nptr);
long atol(const char *nptr);
long long atoll(const char *nptr);
double atof(const char *nptr);
long strtol(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
double strtod(const char *nptr, char **endptr);
float strtof(const char *nptr, char **endptr);
long long strtonum(const char *numstr, long long minval, long long maxval,
                   const char **errstrp);

int mblen(const char *, size_t);
size_t mbstowcs(wchar_t *, const char *, size_t);
int mbtowc(wchar_t *, const char *, size_t);
int wctomb(char *, wchar_t);
size_t wcstombs(char *, const wchar_t *, size_t);
long double strtold(const char *, char **);
extern size_t __mb_cur_max;
#define MB_CUR_MAX __mb_cur_max

char *realpath(const char *path, char *resolved);
char *getenv(const char *name);
char *mktemp(char *);
int mkstemp(char *);
char *mkdtemp(char *);
int mkostemp(char *, int);
char *mkdtemps(char *, int);
int mkstemps(char *, int);
int mkostemps(char *, int, int);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int putenv(char *string);

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));

int abs(int j);
long labs(long j);
long long llabs(long long j);

typedef struct {
  int quot;
  int rem;
} div_t;

typedef struct {
  long quot;
  long rem;
} ldiv_t;

typedef struct {
  long long quot;
  long long rem;
} lldiv_t;

div_t div(int num, int denom);
ldiv_t ldiv(long num, long denom);
lldiv_t lldiv(long long num, long long denom);
int rand(void);
void srand(unsigned int seed);
char *initstate(unsigned int, char *, size_t);
long random(void);
char *setstate(char *);
void srandom(unsigned int);
void srandom_deterministic(unsigned int);
void srandomdev(void);
uint32_t arc4random(void);
void arc4random_buf(void *buf, size_t n);
uint32_t arc4random_uniform(uint32_t upper);
#define randombytes_buf(b, n) arc4random_buf((b), (n))
int system(const char *command);
int heapsort(void *base, size_t nmemb, size_t size,
             int (*compar)(const void *, const void *));
char *__findenv(const char *name, int len, int *offset);

#endif
