#ifndef _CTYPE_H_
#define _CTYPE_H_

#include "inc/cdefs.h"

// the C locale's classes, the only ones there are
__BEGIN_DECLS
int isalnum(int);
int isalpha(int);
int isblank(int);
int iscntrl(int);
int isdigit(int);
int isgraph(int);
int islower(int);
int isprint(int);
int ispunct(int);
int isspace(int);
int isupper(int);
int isxdigit(int);
int tolower(int);
int toupper(int);
int isascii(int);
int toascii(int);
int _tolower(int);
int _toupper(int);
__END_DECLS

#endif
