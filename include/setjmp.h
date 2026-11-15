#ifndef _SETJMP_H_
#define _SETJMP_H_

#include "inc/cdefs.h"
#include <machine/setjmp.h>

typedef long sigjmp_buf[_JBLEN + 1];
typedef long jmp_buf[_JBLEN];

__BEGIN_DECLS
int setjmp(jmp_buf);
void longjmp(jmp_buf, int) __dead;
int sigsetjmp(sigjmp_buf, int);
void siglongjmp(sigjmp_buf, int) __dead;
int _setjmp(jmp_buf);
void _longjmp(jmp_buf, int) __dead;
__END_DECLS

#endif
