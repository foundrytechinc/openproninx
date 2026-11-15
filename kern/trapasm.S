#include "mmu.h"

#define TF_CS 144   /* offsetof(struct trapframe, cs) */
#define MSR_GS_BASE 0xc0000101

  # vectors.S sends all traps here.
.globl alltraps
alltraps:
  # Build trap frame.
  push %r15
  push %r14
  push %r13
  push %r12
  push %r11
  push %r10
  push %r9
  push %r8
  push %rdi
  push %rsi
  push %rbp
  push %rdx
  push %rcx
  push %rbx
  push %rax

  # from a process: its %gs goes aside and the cpu's comes in
  testw $3, TF_CS(%rsp)
  jz 1f
  swapgs
1:
  # Set up data segments.
  mov $(SEG_KDATA<<3), %ax
  mov %ax, %ds
  mov %ax, %es
  # SMAP: whatever AC came in with, the process's pages stay shut
  cmpl $0, smap_on(%rip)
  je 2f
  .byte 0x0f, 0x01, 0xca
2:

  # Call trap(tf), where tf=%esp
  mov %rsp, %rdi
  call trap

  # Return falls through to trapret...
.globl trapret
trapret:
  # nothing may land between the swap and the iretq
  cli
  testw $3, TF_CS(%rsp)
  jz trapret_noswap
  swapgs
.globl trapret_noswap
trapret_noswap:
  # restore data segments
  mov $(SEG_UDATA<<3), %ax
  mov %ax, %ds
  mov %ax, %es

  # restore general registers
  pop %rax
  pop %rbx
  pop %rcx
  pop %rdx
  pop %rbp
  pop %rsi
  pop %rdi
  pop %r8
  pop %r9
  pop %r10
  pop %r11
  pop %r12
  pop %r13
  pop %r14
  pop %r15

  add $0x10, %rsp  # trapno and errcode
  iretq

  # The four that arrive on their own stacks can arrive anywhere, the one
  # instruction between trapret's swapgs and its iretq included, so the
  # frame's cs says nothing about whose %gs is loaded. The msr does: the
  # cpu's is a kernel address and the process's is not.
.globl alltraps_ist
alltraps_ist:
  push %r15
  push %r14
  push %r13
  push %r12
  push %r11
  push %r10
  push %r9
  push %r8
  push %rdi
  push %rsi
  push %rbp
  push %rdx
  push %rcx
  push %rbx
  push %rax

  movl $MSR_GS_BASE, %ecx
  rdmsr
  testl %edx, %edx
  js 2f
  swapgs
  mov $(SEG_KDATA<<3), %ax
  mov %ax, %ds
  mov %ax, %es
  mov %rsp, %rdi
  call trap
  cli
  swapgs
  jmp trapret_noswap
2:
  mov $(SEG_KDATA<<3), %ax
  mov %ax, %ds
  mov %ax, %es
  mov %rsp, %rdi
  call trap
  cli
  jmp trapret_noswap
