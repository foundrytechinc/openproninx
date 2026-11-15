#include "mmu.h"
#include "realmode.h"

// long mode -> real mode -> int n -> long mode, one cpu, interrupts off
// position independent. gdt pointers, bases and far jumps patched from %rip

#define CR4_PGE 0x00000080

#define S_CODE64 0x08
#define S_CODE32 0x10
#define S_DATA32 0x18
#define S_CODE16 0x20
#define S_DATA16 0x28

#define D(x) (RM_DATA + (x))

	.code64
	.globl realmode_enter
realmode_enter:
	cli
	leaq	realmode_enter(%rip), %rdx
	sgdt	D(BS_GDT)(%rdx)
	sidt	D(BS_IDT)(%rdx)
	movq	%rsp, D(BS_RSP)(%rdx)
	movq	%rbx, D(BS_RBX)(%rdx)
	movq	%rbp, D(BS_RBP)(%rdx)
	movq	%r12, D(BS_R12)(%rdx)
	movq	%r13, D(BS_R13)(%rdx)
	movq	%r14, D(BS_R14)(%rdx)
	movq	%r15, D(BS_R15)(%rdx)
	movq	%cr4, %rax
	movq	%rax, D(BS_CR4)(%rdx)
	movw	%cs, D(BS_CS)(%rdx)

	leaq	tramp_gdt(%rip), %rax
	movl	%eax, tramp_gdt64 + 2(%rip)
	movl	%eax, tramp_gdt32 + 2(%rip)
	movl	%edx, %eax
	movw	%ax, gdt_code16 + 2(%rip)
	movw	%ax, gdt_data16 + 2(%rip)
	shrl	$16, %eax
	movb	%al, gdt_code16 + 4(%rip)
	movb	%al, gdt_data16 + 4(%rip)
	movl	%edx, %eax
	shrl	$4, %eax
	movw	%ax, real_seg(%rip)
	leaq	back32(%rip), %rax
	movl	%eax, jmp_back32 + 2(%rip)
	leaq	back64(%rip), %rax
	movl	%eax, jmp_back64 + 1(%rip)

	lgdt	tramp_gdt64(%rip)
	leaq	RM_STACK(%rdx), %rsp
	pushq	$S_CODE32
	leaq	prot32(%rip), %rax
	pushq	%rax
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

	.byte	0xea			// ljmp S_CODE16, its base is ours
	.long	prot16 - realmode_enter
	.word	S_CODE16

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

	movl	$RM_STACK, %esp		// ss is based at us now
	pushw	real_seg - realmode_enter
	pushw	$real16 - realmode_enter
	lretw
real16:
	movw	%cs, %ax
	movw	%ax, %ds
	movw	%ax, %ss
	movw	$RM_STACK, %sp
	lidtl	real_idt - realmode_enter

	xorw	%ax, %ax		// the ivt through fs
	movw	%ax, %fs
	movw	%ax, %gs
	movw	D(BR_ES), %ax
	movw	%ax, %es
	movw	D(BR_VEC), %bp		// int n with a vector only known at run
	shlw	$2, %bp			// time: push flags, far call the ivt slot
	movw	D(BR_BX), %bx
	movw	D(BR_CX), %cx
	movw	D(BR_DX), %dx
	movw	D(BR_SI), %si
	movw	D(BR_DI), %di
	movw	D(BR_AX), %ax

	pushfw
	lcallw	*%fs:(%bp)

	cli
	cld
	movw	%ax, %bp		// the bios owns ds now; park ax, reset it
	movw	%cs, %ax
	movw	%ax, %ds
	movw	%ax, %ss
	movw	%bp, D(BR_AX)
	movw	%bx, D(BR_BX)
	movw	%cx, D(BR_CX)
	movw	%dx, D(BR_DX)
	xorl	%ebx, %ebx		// the base, for the way back
	movw	%cs, %bx
	shll	$4, %ebx
	lgdtl	tramp_gdt32 - realmode_enter
	movl	%cr0, %eax
	orl	$CR0_PE, %eax
	movl	%eax, %cr0
jmp_back32:
	.byte	0x66, 0xea
	.long	0			// back32, written at entry
	.word	S_CODE32

	.code32
back32:
	movw	$S_DATA32, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	%ax, %fs
	movw	%ax, %gs

	leal	RM_STACK(%ebx), %esp

	movl	%cr4, %eax
	orl	$CR4_PAE, %eax
	movl	%eax, %cr4

	movl	D(BS_CR3)(%ebx), %eax
	movl	%eax, %cr3

	movl	$0xc0000080, %ecx
	rdmsr
	orl	$0x100, %eax
	wrmsr

	movl	%cr0, %eax
	orl	$CR0_PG, %eax
	movl	%eax, %cr0
jmp_back64:
	.byte	0xea
	.long	0			// back64, written at entry
	.word	S_CODE64

	.code64
back64:
	leaq	realmode_enter(%rip), %rdx
	movq	D(BS_CR4)(%rdx), %rax
	movq	%rax, %cr4
	lgdt	D(BS_GDT)(%rdx)
	lidt	D(BS_IDT)(%rdx)
	movw	$0x10, %ax		// SEG_KDATA, flat in either table
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	%ax, %fs
	movw	%ax, %gs

	movq	D(BS_RBX)(%rdx), %rbx
	movq	D(BS_RBP)(%rdx), %rbp
	movq	D(BS_R12)(%rdx), %r12
	movq	D(BS_R13)(%rdx), %r13
	movq	D(BS_R14)(%rdx), %r14
	movq	D(BS_R15)(%rdx), %r15
	movq	D(BS_RSP)(%rdx), %rsp

	popq	%rax			// the caller's return address
	movzwq	D(BS_CS)(%rdx), %rcx
	pushq	%rcx
	pushq	%rax
	lretq

	.p2align 4
tramp_gdt:
	.quad	0x0000000000000000
	.quad	0x00209a0000000000	// 0x08 64 bit code
	.quad	0x00cf9a000000ffff	// 0x10 32 bit code
	.quad	0x00cf92000000ffff	// 0x18 32 bit data
gdt_code16:
	.quad	0x00009a000000ffff	// 0x20 16 bit code, based at us
gdt_data16:
	.quad	0x000092000000ffff	// 0x28 16 bit data, based at us
tramp_gdt_end:

	.p2align 4
tramp_gdt64:
	.word	tramp_gdt_end - tramp_gdt - 1
	.long	0			// written at entry
	.long	0

	.p2align 4
tramp_gdt32:
	.word	tramp_gdt_end - tramp_gdt - 1
	.long	0			// written at entry

	.p2align 4
real_idt:
	.word	0x3ff
	.long	0

real_seg:
	.word	0			// written at entry
