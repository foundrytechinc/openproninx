# SETJMP: the kernel's, for x86emu's halt
#   int setjmp(label_t *);  void longjmp(label_t *);

.text
.globl setjmp
setjmp:
  movq (%rsp), %rax
  movq %rax, 0x00(%rdi)
  movq %rbx, 0x08(%rdi)
  movq %rsp, 0x10(%rdi)
  movq %rbp, 0x18(%rdi)
  movq %r12, 0x20(%rdi)
  movq %r13, 0x28(%rdi)
  movq %r14, 0x30(%rdi)
  movq %r15, 0x38(%rdi)
  xorl %eax, %eax
  ret

.globl longjmp
longjmp:
  movq 0x08(%rdi), %rbx
  movq 0x10(%rdi), %rsp
  movq 0x18(%rdi), %rbp
  movq 0x20(%rdi), %r12
  movq 0x28(%rdi), %r13
  movq 0x30(%rdi), %r14
  movq 0x38(%rdi), %r15
  movq 0x00(%rdi), %rax
  movq %rax, (%rsp)
  movl $1, %eax
  ret
