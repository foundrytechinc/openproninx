#include "mmu.h"
#include "realmode.h"

/*
 * Long mode -> real mode -> int $0x10 -> long mode, on one cpu, interrupts off.
 * Linked and copied to RM_CODE, low enough that a null segment reaches
 * every label. Enter with cr3 already pointing at the trampoline tables.
 */

#define CR4_PGE 0x00000080

#define S_CODE64 0x08
#define S_CODE32 0x10
#define S_DATA32 0x18
#define S_CODE16 0x20
#define S_DATA16 0x28

	.code64
	.globl realmode_enter
realmode_enter:
	cli
	sgdt	RM_DATA + BS_GDT
	sidt	RM_DATA + BS_IDT
	movq	%rsp, RM_DATA + BS_RSP
	movq	%rbx, RM_DATA + BS_RBX
	movq	%rbp, RM_DATA + BS_RBP
	movq	%r12, RM_DATA + BS_R12
	movq	%r13, RM_DATA + BS_R13
	movq	%r14, RM_DATA + BS_R14
	movq	%r15, RM_DATA + BS_R15
	movq	%cr4, %rax
	movq	%rax, RM_DATA + BS_CR4
	movw	%cs, RM_DATA + BS_CS

	lgdt	tramp_gdt64
	movl	$RM_STACK, %esp
	pushq	$S_CODE32
	pushq	$prot32
	lretq

	.code32
prot32:
	movw	$S_DATA32, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	%ax, %fs
	movw	%ax, %gs

	movl	%cr0, %eax		// paging off first, then long mode off
	andl	$~CR0_PG, %eax
	movl	%eax, %cr0

	movl	$0xc0000080, %ecx
	rdmsr
	andl	$~0x100, %eax
	wrmsr

	movl	%cr4, %eax
	andl	$~(CR4_PAE | CR4_PGE), %eax
	movl	%eax, %cr4

	ljmp	$S_CODE16, $prot16

	.code16
prot16:
	movw	$S_DATA16, %ax		// 64K limits, so the caches survive PE=0
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	%ax, %fs
	movw	%ax, %gs

	movl	%cr0, %eax
	andl	$~CR0_PE, %eax
	movl	%eax, %cr0

	ljmp	$0, $real16
real16:
	xorw	%ax, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	%ax, %fs
	movw	%ax, %gs
	movw	$RM_STACK, %sp
	lidtl	real_idt

	movw	RM_DATA + BR_ES, %ax
	movw	%ax, %es
	movw	RM_DATA + BR_VEC, %bp	// int n with a vector only known at run
	shlw	$2, %bp			// time: push flags, far call the ivt slot
	movw	RM_DATA + BR_AX, %ax
	movw	RM_DATA + BR_BX, %bx
	movw	RM_DATA + BR_CX, %cx
	movw	RM_DATA + BR_DX, %dx
	movw	RM_DATA + BR_SI, %si
	movw	RM_DATA + BR_DI, %di

	pushfw
	lcallw	*(%bp)

	cli
	cld
	movw	%ax, %bp		// the bios owns ds now; park ax, reset it
	xorw	%ax, %ax
	movw	%ax, %ds
	movw	%ax, %ss
	movw	%bp, RM_DATA + BR_AX
	movw	%bx, RM_DATA + BR_BX
	movw	%cx, RM_DATA + BR_CX
	movw	%dx, RM_DATA + BR_DX
	lgdtl	tramp_gdt32
	movl	%cr0, %eax
	orl	$CR0_PE, %eax
	movl	%eax, %cr0
	ljmpl	$S_CODE32, $back32

	.code32
back32:
	movw	$S_DATA32, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	%ax, %fs
	movw	%ax, %gs

	movl	%cr4, %eax
	orl	$CR4_PAE, %eax
	movl	%eax, %cr4

	movl	$RM_PML4, %eax
	movl	%eax, %cr3

	movl	$0xc0000080, %ecx
	rdmsr
	orl	$0x100, %eax
	wrmsr

	movl	%cr0, %eax
	orl	$CR0_PG, %eax
	movl	%eax, %cr0

	ljmp	$S_CODE64, $back64

	.code64
back64:
	movq	RM_DATA + BS_CR4, %rax
	movq	%rax, %cr4
	lgdt	RM_DATA + BS_GDT
	lidt	RM_DATA + BS_IDT
	movw	$0x10, %ax		// SEG_KDATA, flat in either table
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	%ax, %fs
	movw	%ax, %gs

	movq	RM_DATA + BS_RBX, %rbx
	movq	RM_DATA + BS_RBP, %rbp
	movq	RM_DATA + BS_R12, %r12
	movq	RM_DATA + BS_R13, %r13
	movq	RM_DATA + BS_R14, %r14
	movq	RM_DATA + BS_R15, %r15
	movq	RM_DATA + BS_RSP, %rsp

	popq	%rax			// the caller's return address
	movzwq	RM_DATA + BS_CS, %rcx
	pushq	%rcx
	pushq	%rax
	lretq

	.p2align 4
tramp_gdt:
	.quad	0x0000000000000000
	.quad	0x00209a0000000000	// 0x08 64 bit code
	.quad	0x00cf9a000000ffff	// 0x10 32 bit code
	.quad	0x00cf92000000ffff	// 0x18 32 bit data
	.quad	0x00009a000000ffff	// 0x20 16 bit code
	.quad	0x000092000000ffff	// 0x28 16 bit data
tramp_gdt_end:

	.p2align 4
tramp_gdt64:
	.word	tramp_gdt_end - tramp_gdt - 1
	.long	tramp_gdt		// base is 64 bit, but never above 4G here
	.long	0

	.p2align 4
tramp_gdt32:
	.word	tramp_gdt_end - tramp_gdt - 1
	.long	tramp_gdt

	.p2align 4
real_idt:
	.word	0x3ff
	.long	0

	.section .note.GNU-stack,"",@progbits
