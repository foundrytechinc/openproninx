# SLEEP
#   int acpi_savecpu(void *ctx);   0 when saved, 1 when it comes back

#include "mmu.h"
#include "wake.h"

.text
.globl acpi_savecpu
acpi_savecpu:
  movq %rbx, CTX_RBX(%rdi)
  movq %rbp, CTX_RBP(%rdi)
  movq %r12, CTX_R12(%rdi)
  movq %r13, CTX_R13(%rdi)
  movq %r14, CTX_R14(%rdi)
  movq %r15, CTX_R15(%rdi)
  movq (%rsp), %rax
  movq %rax, CTX_RIP(%rdi)
  leaq 8(%rsp), %rax
  movq %rax, CTX_RSP(%rdi)
  movq %cr0, %rax
  movq %rax, CTX_CR0(%rdi)
  movq %cr3, %rax
  movq %rax, CTX_CR3(%rdi)
  movq %cr4, %rax
  movq %rax, CTX_CR4(%rdi)
  sgdt CTX_GDT(%rdi)
  sidt CTX_IDT(%rdi)
  pushfq
  popq %rax
  movq %rax, CTX_FLAGS(%rdi)
  xorl %eax, %eax
  ret

# from the wake code: on its page tables, interrupts off, no stack
.globl acpi_resume_entry
acpi_resume_entry:
  movabsq $acpi_cpuctx, %rdi
  lgdt CTX_GDT(%rdi)
  movq CTX_CR4(%rdi), %rax
  movq %rax, %cr4
  movq CTX_CR3(%rdi), %rax
  movq %rax, %cr3
  movq CTX_RSP(%rdi), %rsp
  pushq $(SEG_KCODE << 3)
  leaq 1f(%rip), %rax
  pushq %rax
  lretq
1:
  movl $(SEG_KDATA << 3), %eax
  movl %eax, %ds
  movl %eax, %es
  movl %eax, %ss
  movl %eax, %fs
  movl %eax, %gs
  lidt CTX_IDT(%rdi)
  movq CTX_CR0(%rdi), %rax
  movq %rax, %cr0
  movq CTX_RBX(%rdi), %rbx
  movq CTX_RBP(%rdi), %rbp
  movq CTX_R12(%rdi), %r12
  movq CTX_R13(%rdi), %r13
  movq CTX_R14(%rdi), %r14
  movq CTX_R15(%rdi), %r15
  pushq CTX_FLAGS(%rdi)
  popfq
  movl $1, %eax
  jmp *CTX_RIP(%rdi)

# HIBERNATE
#   void hib_switch(uint64_t pml4, uint64_t stack, void (*fn)(void *), void *arg);
.globl hib_switch
hib_switch:
  cli
  movq %cr4, %rax
  andq $~CR4_PGE, %rax
  movq %rax, %cr4
  movq %rdi, %cr3
  movq %rsi, %rsp
  movq %rcx, %rdi
  callq *%rdx
1:
  hlt
  jmp 1b

# into the image
.globl hib_resume_jump
hib_resume_jump:
  jmp acpi_resume_entry
