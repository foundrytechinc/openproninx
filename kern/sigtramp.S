#include "inc/syscall.h"

#define T_SYSCALL 0x30
#define SIGF_SC 24

  .section .rodata
  .globl sigcode
  .align 16
sigcode:
  call *%rax
  leaq SIGF_SC(%rsp), %rdi
  movq $SYS_sigreturn, %rax
  int $T_SYSCALL
  .globl sigcoderet
sigcoderet:
  ud2
  .globl esigcode
esigcode:
