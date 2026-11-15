// call numbers and the trap. no pinning: PINSYSCALL is empty
#ifndef _LIBC_AMD64_SYS_H_
#define _LIBC_AMD64_SYS_H_

#include <machine/asm.h>
#include "inc/syscall.h"


#define T_SYSCALL 0x30

#define PINSYSCALL(sysno, label)

#endif /* _LIBC_AMD64_SYS_H_ */
