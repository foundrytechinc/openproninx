#include "mmu.h"
#include "memlayout.h"

.code16
.globl start
start:
  cli

  xorw    %ax, %ax
  movw    %ax, %ds
  movw    %ax, %es
  movw    %ax, %ss

  lgdt    gdtdesc

  movl    %cr0, %eax
  orl     $CR0_PE, %eax
  movl    %eax, %cr0

  ljmp    $0x8, $start32

.code32
start32:
  movw    $0x10, %ax
  movw    %ax, %ds
  movw    %ax, %es
  movw    %ax, %ss

  # Enable PAE
  movl    %cr4, %eax
  orl     $CR4_PAE, %eax
  movl    %eax, %cr4

  # Load AP boot PML4 from 0x8000
  movl    $0x8000, %eax
  movl    %eax, %cr3

  # Enable Long Mode in EFER MSR
  movl    $0xc0000080, %ecx
  rdmsr
  orl     $0x100, %eax
  wrmsr

  # Enable Paging (CR0.PG | CR0.PE)
  movl    %cr0, %eax
  orl     $CR0_PG, %eax
  movl    %eax, %cr0

  # Load 64-bit GDT
  lgdt    gdt64desc

  # Far jump into 64-bit Long Mode
  ljmp    $0x8, $start64

.code64
start64:
  movw    $0x10, %ax
  movw    %ax, %ds
  movw    %ax, %es
  movw    %ax, %ss

  movabsq $0x6ff8, %rbx
  movq    (%rbx), %rsp
  movabsq $0x6ff0, %rbx
  movq    (%rbx), %rax
  jmp     *%rax

.p2align 4
gdt:
  .quad 0x0000000000000000
  .quad 0x00cf9a000000ffff
  .quad 0x00cf92000000ffff

gdtdesc:
  .word gdtdesc - gdt - 1
  .long gdt

.p2align 4
gdt64:
  .quad 0x0000000000000000
  .quad 0x00209a0000000000
  .quad 0x0000920000000000

gdt64desc:
  .word gdt64desc - gdt64 - 1
  .long gdt64

.section .note.GNU-stack,"",@progbits
