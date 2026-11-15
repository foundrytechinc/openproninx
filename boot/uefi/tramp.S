// move the kernel image to its link address and enter it
	.text
	.globl	tramp_start
	.globl	tramp_args
	.globl	tramp_end

tramp_start:
	jmp	1f
	.align	8
tramp_args:
	.quad	0
	.quad	0
	.quad	0
	.quad	0
	.quad	0
	.quad	0
1:
	cli
	leaq	tramp_args(%rip), %r10
	movq	32(%r10), %rax
	movq	%rax, %cr3
	leaq	tramp_stack_top(%rip), %rsp

	movq	0(%r10), %rdi
	movq	8(%r10), %rsi
	movq	16(%r10), %rcx
	cmpq	%rsi, %rdi
	jbe	2f
	leaq	-1(%rdi,%rcx), %rdi
	leaq	-1(%rsi,%rcx), %rsi
	std
	rep	movsb
	cld
	jmp	3f
2:
	cld
	rep	movsb
3:
	movq	24(%r10), %rax
	movq	40(%r10), %rdi
	jmp	*%rax

	.align	16
	.space	4096
tramp_stack_top:
tramp_end:
