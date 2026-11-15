/* Inside libc: what the library keeps to itself. */
#ifndef _LIBC_STDLIB_H_
#define _LIBC_STDLIB_H_

#include_next <stdlib.h>

void	__atexit_register_cleanup(void (*)(void));

#endif /* _LIBC_STDLIB_H_ */
