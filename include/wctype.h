#ifndef _WCTYPE_H_
#define _WCTYPE_H_

#include <sys/types.h>
#include "inc/cdefs.h"

#ifndef _WINT_T_DEFINED_
#define _WINT_T_DEFINED_
typedef __wint_t wint_t;
#endif
typedef int wctype_t;  // a class: its index, 0 for none
typedef int wctrans_t; // a mapping: its index, 0 for none

#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif

// the C locale's classes: a wide character below 128 is the byte it names
__BEGIN_DECLS
int iswalnum(wint_t);
int iswalpha(wint_t);
int iswblank(wint_t);
int iswcntrl(wint_t);
int iswdigit(wint_t);
int iswgraph(wint_t);
int iswlower(wint_t);
int iswprint(wint_t);
int iswpunct(wint_t);
int iswspace(wint_t);
int iswupper(wint_t);
int iswxdigit(wint_t);
int iswctype(wint_t, wctype_t);
wint_t towctrans(wint_t, wctrans_t);
wint_t towlower(wint_t);
wint_t towupper(wint_t);
wctrans_t wctrans(const char *);
wctype_t wctype(const char *);
__END_DECLS

#endif
