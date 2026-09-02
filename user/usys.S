#include "inc/syscall.h"

#define T_SYSCALL 0x30

#define SYSCALL(name) \
  .globl name; \
  name: \
    mov $SYS_ ## name, %rax; \
    int $T_SYSCALL; \
    ret

SYSCALL(fork)
SYSCALL(exit)
SYSCALL(wait)
SYSCALL(waitpid)
SYSCALL(pipe)
SYSCALL(read)
SYSCALL(kill)
SYSCALL(exec)
SYSCALL(stat)
SYSCALL(malloc)
SYSCALL(free)
SYSCALL(info)
SYSCALL(reboot)
SYSCALL(procinfo)
SYSCALL(ioctl)
SYSCALL(getdents)
SYSCALL(chdir)
SYSCALL(dup)
SYSCALL(getpid)
SYSCALL(sbrk)
SYSCALL(sleep)
SYSCALL(open)
SYSCALL(write)
SYSCALL(mknod)
SYSCALL(unlink)
SYSCALL(link)
SYSCALL(mkdir)
SYSCALL(close)

.section .note.GNU-stack,"",@progbits
