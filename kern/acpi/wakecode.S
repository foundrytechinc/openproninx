# WAKE
# where the kernel put it, FreeBSD's mpboot.S way: the base from %cs, the gdt
# pointers and far jumps patched with it

#include "mmu.h"
#include "wake.h"

.code16
.globl wakeup_start
wakeup_start:
  jmp real

  .org WAKE_HDR_FLAGS
flags:
  .byte 0
  .org WAKE_HDR_TABLES
tables:
  .long 0
  .org WAKE_HDR_EFER
efer:
  .quad 0
  .org WAKE_HDR_ENTRY
entry:
  .quad 0
  .org WAKE_HDR_RTC
rtc:
  .space 2 * WAKE_RTC_SLOT
  .org WAKE_HDR_END

real:
  cli
  cld
  movw %cs, %ax
  movw %ax, %ds
  movw %ax, %es
  movw %ax, %ss
  movw $WAKE_STACK, %sp

  testb $WAKE_TRACE, flags - wakeup_start
  jz 1f
  call rtcget
  movw $WAKE_HDR_RTC, %si
  call rtcset
1:
  # reset_video: firmware may skip the video bios on resume
  testb $WAKE_VIDEO, flags - wakeup_start
  jz 2f
  andb $~WAKE_VIDEO, flags - wakeup_start
  lcall $0xc000, $3
  cli
  cld
  movw %cs, %ax
  movw %ax, %ds
  movw %ax, %es
  movw %ax, %ss
  movw $WAKE_STACK, %sp
  testb $WAKE_TRACE, flags - wakeup_start
  jz 2f
  movw $(WAKE_HDR_RTC + WAKE_RTC_SLOT), %si
  call rtcset
2:
  xorl %ebx, %ebx
  movw %cs, %bx
  shll $4, %ebx
  orl %ebx, gdtdesc - wakeup_start + 2
  orl %ebx, gdt64desc - wakeup_start + 2
  orl %ebx, jmp32 - wakeup_start + 2
  orl %ebx, jmp64 - wakeup_start + 1

  lgdtl gdtdesc - wakeup_start
  movl %cr0, %eax
  orl $CR0_PE, %eax
  movl %eax, %cr0
jmp32:
  .byte 0x66, 0xea
  .long wake32 - wakeup_start
  .word 0x8

# the clock as the sleep left it, before a step goes over it
rtcget:
  movw $0xffff, %cx
4:
  movb $0x0a, %al
  outb %al, $0x70
  inb $0x71, %al
  testb $0x80, %al
  loopnz 4b
  movw $WAKE_HDR_CLOCK_REGS, %si
  movw $WAKE_HDR_CLOCK, %di
5:
  lodsb
  cmpb $0xff, %al
  je 6f
  outb %al, $0x70
  inb $0x71, %al
  stosb
  jmp 5b
6:
  movb $1, WAKE_HDR_CLOCK_TAKEN
  ret

# the clock image at %si: register and value pairs to 0xff
rtcset:
  lodsb
  cmpb $0xff, %al
  je 3f
  outb %al, $0x70
  lodsb
  outb %al, $0x71
  jmp rtcset
3:
  ret

.code32
wake32:
  movw $0x10, %ax
  movw %ax, %ds
  movw %ax, %es
  movw %ax, %ss

  movl %cr4, %eax
  orl $CR4_PAE, %eax
  movl %eax, %cr4
  movl tables - wakeup_start(%ebx), %eax
  movl %eax, %cr3

  # long mode, and no-execute if the kernel had it
  movl $0xc0000080, %ecx
  movl efer - wakeup_start(%ebx), %eax
  xorl %edx, %edx
  wrmsr

  movl %cr0, %eax
  orl $CR0_PG, %eax
  movl %eax, %cr0

  lgdtl gdt64desc - wakeup_start(%ebx)
jmp64:
  .byte 0xea
  .long wake64 - wakeup_start
  .word 0x8

.code64
wake64:
  movw $0x10, %ax
  movw %ax, %ds
  movw %ax, %es
  movw %ax, %ss
  movl %ebx, %ebx
  movq entry - wakeup_start(%rbx), %rax
  jmp *%rax

.p2align 4
gdt:
  .quad 0x0000000000000000
  .quad 0x00cf9a000000ffff
  .quad 0x00cf92000000ffff
gdtdesc:
  .word gdtdesc - gdt - 1
  .long gdt - wakeup_start

.p2align 4
gdt64:
  .quad 0x0000000000000000
  .quad 0x00209a0000000000
  .quad 0x0000920000000000
gdt64desc:
  .word gdt64desc - gdt64 - 1
  .long gdt64 - wakeup_start
