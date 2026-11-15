// AP START
// where the hell are we? %cs knows
#include "mmu.h"
#include "apboot.h"

.code16
.globl start
start:
  jmp     real

  .org    AP_HDR_STEP
step:
  .word   0
  .org    AP_HDR_TABLES
tables:
  .long   0
  .org    AP_HDR_STACK
stack:
  .quad   0
  .org    AP_HDR_ENTRY
entry:
  .quad   0
  .org    AP_HDR_CR3
kcr3:
  .quad   0
  .org    AP_HDR_PROBE
probe:
  .word   0
  .org    AP_HDR_COUNT
count:
  .word   0
  .org    AP_HDR_IDS
ids:
  .space  AP_PROBE_MAX
  .org    AP_HDR_END

real:
  cli
  movw    %cs, %ax
  movw    %ax, %ds
  movw    %ax, %ss
  movw    $1, step - start
  cmpw    $0, probe - start
  je      1f

  # EDK2's broadcast wakeup: say who answered, then halt
  movl    $1, %eax
  cpuid
  shrl    $24, %ebx
  movw    $1, %ax
  lock xaddw %ax, count - start
  cmpw    $AP_PROBE_MAX, %ax
  jae     2f
  movw    %ax, %si
  movb    %bl, ids - start(%si)
2:
  cli
  hlt
  jmp     2b
1:

  xorl    %ebx, %ebx
  movw    %cs, %bx
  shll    $4, %ebx
  orl     %ebx, gdtdesc - start + 2
  orl     %ebx, gdt64desc - start + 2
  orl     %ebx, jmp32 - start + 2
  orl     %ebx, jmp64 - start + 1

  lgdtl   gdtdesc - start
  movl    %cr0, %eax
  orl     $CR0_PE, %eax
  movl    %eax, %cr0
jmp32:
  .byte   0x66, 0xea
  .long   start32 - start
  .word   0x8

.code32
start32:
  movw    $0x10, %ax
  movw    %ax, %ds
  movw    %ax, %es
  movw    %ax, %ss
  movw    $2, step - start(%ebx)

  movl    %cr4, %eax
  orl     $CR4_PAE, %eax
  movl    %eax, %cr4
  movl    tables - start(%ebx), %eax
  movl    %eax, %cr3

  # EFER.LME, and NXE where the kernel's tables use it; cpuid takes %ebx
  movl    %ebx, %edi
  movl    $0x80000001, %eax
  cpuid
  movl    %edi, %ebx
  andl    $0x100000, %edx
  shrl    $9, %edx
  orl     $0x100, %edx
  movl    %edx, %esi
  movl    $0xc0000080, %ecx
  rdmsr
  orl     %esi, %eax
  wrmsr

  movl    %cr0, %eax
  orl     $CR0_PG, %eax
  movl    %eax, %cr0
  movw    $3, step - start(%ebx)

  lgdtl   gdt64desc - start(%ebx)
jmp64:
  .byte   0xea
  .long   start64 - start
  .word   0x8

.code64
start64:
  movw    $0x10, %ax
  movw    %ax, %ds
  movw    %ax, %es
  movw    %ax, %ss
  movl    %ebx, %ebx
  movw    $4, step - start(%rbx)

  movq    stack - start(%rbx), %rsp
  movq    entry - start(%rbx), %rax
  movq    kcr3 - start(%rbx), %rcx
  jmp     *%rax

.p2align 4
gdt:
  .quad 0x0000000000000000
  .quad 0x00cf9a000000ffff
  .quad 0x00cf92000000ffff
gdtdesc:
  .word gdtdesc - gdt - 1
  .long gdt - start

.p2align 4
gdt64:
  .quad 0x0000000000000000
  .quad 0x00209a0000000000
  .quad 0x0000920000000000
gdt64desc:
  .word gdt64desc - gdt64 - 1
  .long gdt64 - start
