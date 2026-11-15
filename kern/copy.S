# USER COPIES
#   int copy_user(void *dst, const void *src, size_t len, uintptr_t *onfault);
#   int copy_user_str(char *dst, const char *src, size_t len, size_t *done,
#                     uintptr_t *onfault);

#include "inc/error.h"

#define USER_OPEN                                                              \
  cmpl $0, smap_on(%rip);                                                      \
  je 9f;                                                                       \
  .byte 0x0f, 0x01, 0xcb;                                                      \
9:
#define USER_CLOSE                                                             \
  cmpl $0, smap_on(%rip);                                                      \
  je 9f;                                                                       \
  .byte 0x0f, 0x01, 0xca;                                                      \
9:

.globl copy_user
copy_user:
  movq %rcx, %r8
  leaq 1f(%rip), %rax
  movq %rax, (%r8)
  movq %rdx, %rcx
  USER_OPEN
  rep movsb
  USER_CLOSE
  movq $0, (%r8)
  xorl %eax, %eax
  ret
1:
  USER_CLOSE
  movl $FNU_ERR_FAULT, %eax
  ret

# a string, the end included
.globl copy_user_str
copy_user_str:
  leaq 3f(%rip), %rax
  movq %rax, (%r8)
  xorl %eax, %eax
  USER_OPEN
1:
  cmpq %rdx, %rax
  je 2f
  movb (%rsi,%rax), %r9b
  movb %r9b, (%rdi,%rax)
  incq %rax
  testb %r9b, %r9b
  jnz 1b
  USER_CLOSE
  movq $0, (%r8)
  testq %rcx, %rcx
  jz 4f
  movq %rax, (%rcx)
4:
  xorl %eax, %eax
  ret
2:
  USER_CLOSE
  movq $0, (%r8)
  movl $FNU_ERR_NAMETOOLONG, %eax
  ret
3:
  USER_CLOSE
  movl $FNU_ERR_FAULT, %eax
  ret
