#ifndef _STDDEF_H_
#define _STDDEF_H_

#include "inc/types.h"

typedef long ptrdiff_t;
#ifndef _WCHAR_T_DEFINED_
#define _WCHAR_T_DEFINED_
typedef int wchar_t;
#endif

#define offsetof(type, member) __builtin_offsetof(type, member)

#endif
