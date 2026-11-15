#include "inc/cdefs.h"

#undef assert
#undef _assert

#ifdef NDEBUG
#define assert(e) ((void)0)
#define _assert(e) ((void)0)
#else
#define _assert(e) assert(e)
#define assert(e) ((e) ? (void)0 : __assert2(__FILE__, __LINE__, __func__, #e))
#endif

__BEGIN_DECLS
__dead void __assert(const char *, int, const char *);
__dead void __assert2(const char *, int, const char *, const char *);
__END_DECLS
