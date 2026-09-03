# Initial process execs /usr/bin/init.
# This code runs in user space.

#include "inc/syscall.h"
#include "trap.h"

.globl start
start:
# exec("/usr/bin/init", argv);
  lea init(%rip),%rax
  mov %rax,%rdi
  lea argv(%rip),%rax
  mov %rax,%rsi
  mov $SYS_exec, %rax
  int $T_SYSCALL

# for(;;) exit();
exit:
  mov $SYS_exit, %rax
  int $T_SYSCALL
  jmp exit

init:
  .string "/usr/bin/init\0"

.p2align 3
argv:
  .long init
  .long 0

.section .note.GNU-stack,"",@progbits
