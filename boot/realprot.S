/*
 * Mode switching adapted from NetBSD sys/arch/i386/stand/lib/realprot.S.
 *
 * Copyright (c) 2003 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by David Laight.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

	.text
	.align 16

	.globl	bootgdt
bootgdt:
	.word	0, 0
	.byte	0, 0x00, 0x00, 0

flatcodeseg = . - bootgdt
	.word	0xffff, 0
	.byte	0, 0x9f, 0xcf, 0

	.globl	flatdataseg
flatdataseg = . - bootgdt
	.word	0xffff, 0
	.byte	0, 0x93, 0xcf, 0

bootcodeseg = . - bootgdt
	.word	0xffff, 0
	.byte	0, 0x9e, 0x4f, 0

bootdataseg = . - bootgdt
	.word	0xffff, 0
	.byte	0, 0x92, 0xcf, 0

bootrealseg = . - bootgdt
	.word	0xffff, 0
	.byte	0, 0x9e, 0x00, 0

bootrealdata = . - bootgdt
	.word	0xffff, 0
	.byte	0, 0x92, 0x00, 0
bootgdtlen = . - bootgdt

	.align	16
gdtarg:
	.word	bootgdtlen - 1
	.long	0

toreal:	.word	xreal
ourseg:	.word	0

stkseg:	.word	0
stkdif:	.long	0

// call once from real mode before anything else
	.globl	gdt_fixup
gdt_fixup:
	.code16
	pushl	%eax
	pushl	%edx

	xorl	%eax, %eax
	mov	%cs, %ax
	mov	%ax, ourseg
	xorl	%edx, %edx
	movw	%ss, %dx
	movw	%dx, stkseg
	subl	%eax, %edx
	shll	$4, %edx
	movl	%edx, stkdif

	mov	%ax, %dx
	shll	$4, %eax
	shr	$12, %dx

#define FIXUP(i) \
	movw	%ax, bootgdt+i+2; \
	movb	%dl, bootgdt+i+4

	FIXUP(bootcodeseg)
	FIXUP(bootrealseg)
	FIXUP(bootdataseg)

	addl	$bootgdt, %eax
	movl	%eax, gdtarg+2

	popl	%edx
	popl	%eax
	ret

// must be reached with a 32 bit call
	.globl	real_to_prot
real_to_prot:
	.code16
	pushl	%eax
	cli

	lgdt	%cs:gdtarg

	movl	%cr0, %eax
	or	$1, %ax
	movl	%eax, %cr0

	ljmp	$bootcodeseg, $1f
1:
	.code32
	mov	$bootdataseg, %eax
	mov	%ax, %ds
	mov	%ax, %es
	mov	%ax, %ss
	addl	stkdif, %esp

	popl	%eax
	ret

	.globl	prot_to_real
prot_to_real:
	.code32
	pushl	%eax

	// 16 bit attributes must be cached before PE drops
	movw	$bootrealdata, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss

	ljmp	$bootrealseg, $1f
1:
	.code16
	movl	%cr0, %eax
	and	$~1, %eax
	movl	%eax, %cr0

	ljmp	*%cs:toreal
xreal:
	mov	%cs, %ax
	mov	%ax, %ds
	mov	%ax, %es
	mov	stkseg, %ax
	mov	%ax, %ss
	subl	stkdif, %esp

	sti
	popl	%eax
	retl

// vtophys(void *) - loader address to linear
	.globl	vtophys
vtophys:
	.code32
	xorl	%eax, %eax
	movw	ourseg, %ax
	shll	$4, %eax
	addl	4(%esp), %eax
	ret

// flat_copy(void *dst, uint32 src, uint32 n) - src is linear
	.globl	flat_copy
flat_copy:
	.code32
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%esi
	pushl	%edi
	pushl	%ds

	movl	8(%ebp), %edi
	movl	12(%ebp), %esi
	movl	16(%ebp), %ecx
	movw	$flatdataseg, %ax
	movw	%ax, %ds
	cld
	rep	movsb

	popl	%ds
	popl	%edi
	popl	%esi
	popl	%ebp
	ret

// flat_move(uint32 dst, uint32 src, uint32 n) - both linear
	.globl	flat_move
flat_move:
	.code32
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%esi
	pushl	%edi
	pushl	%ds
	pushl	%es

	movl	8(%ebp), %edi
	movl	12(%ebp), %esi
	movl	16(%ebp), %ecx
	movw	$flatdataseg, %ax
	movw	%ax, %ds
	movw	%ax, %es
	cld
	rep	movsb

	popl	%es
	popl	%ds
	popl	%edi
	popl	%esi
	popl	%ebp
	ret

// flat_zero(uint32 dst, uint32 n) - dst is linear
	.globl	flat_zero
flat_zero:
	.code32
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%edi
	pushl	%es

	movl	8(%ebp), %edi
	movl	12(%ebp), %ecx
	movw	$flatdataseg, %ax
	movw	%ax, %es
	xorl	%eax, %eax
	cld
	rep	stosb

	popl	%es
	popl	%edi
	popl	%ebp
	ret

// flat_write(uint32 dst, const void *src, uint32 n) - dst is linear
	.globl	flat_write
flat_write:
	.code32
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%esi
	pushl	%edi
	pushl	%es

	movl	8(%ebp), %edi
	movl	12(%ebp), %esi
	movl	16(%ebp), %ecx
	movw	$flatdataseg, %ax
	movw	%ax, %es
	cld
	rep	movsb

	popl	%es
	popl	%edi
	popl	%esi
	popl	%ebp
	ret

	.section .note.GNU-stack,"",@progbits
