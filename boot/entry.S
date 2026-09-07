// stage2 entry. stage1 jumps here in real mode with %dl = drive and
// %si = &part_lba in segment 0.

	.section .entry, "awx"
	.code16
	.globl _start
_start:
	cli
	movl	(%si), %eax
	movl	4(%si), %ebx
	movzbl	%dl, %edx

	movw	%cs, %cx
	movw	%cx, %ds
	movw	%cx, %es
	movw	%cx, %ss
	movw	$stack_top, %sp
	cld

	movl	%eax, boot_part_lba
	movl	%ebx, boot_part_lba+4
	movl	%edx, boot_drive

	call	gdt_fixup
	call	a20_enable
	testw	%ax, %ax
	jz	no_a20
	calll	real_to_prot
	.code32

	movl	$__bss_start, %edi
	movl	$__bss_end, %ecx
	subl	%edi, %ecx
	xorl	%eax, %eax
	rep	stosb

	call	main
1:	hlt
	jmp	1b

	.code16
no_a20:
	movw	$msg_a20, %si
1:	lodsb
	testb	%al, %al
	jz	2f
	movb	$0x0e, %ah
	movb	$0x07, %bl
	int	$0x10
	jmp	1b
2:	hlt
	jmp	2b
msg_a20:
	.asciz	"stage2: cannot enable A20"

	.code32
// enter_long(uint64 entry, uint32 cr3, uint32 info)
	.globl	enter_long
enter_long:
	.code32
	cli

	movl	4(%esp), %eax
	movl	%eax, kentry
	movl	8(%esp), %eax
	movl	%eax, kentry+4
	movl	16(%esp), %eax
	movl	%eax, kinfo
	movl	12(%esp), %eax
	movl	%eax, kcr3

	pushl	$gdt64
	call	vtophys
	addl	$4, %esp
	movl	%eax, gdt64desc+2

	pushl	$long_entry
	call	vtophys
	addl	$4, %esp
	movl	%eax, farptr

	lgdt	gdt64desc

	movl	kcr3, %eax
	movl	%eax, %cr3

	movl	%cr4, %eax
	orl	$0x20, %eax
	movl	%eax, %cr4

	movl	$0xc0000080, %ecx
	rdmsr
	orl	$0x100, %eax
	wrmsr

	movl	%cr0, %eax
	orl	$0x80000000, %eax
	movl	%eax, %cr0

	movl	kentry, %ebx
	movl	kentry+4, %ebp
	movl	kinfo, %edi
	ljmp	*farptr

	.code64
long_entry:
	movl	$0x10, %eax
	movl	%eax, %ds
	movl	%eax, %es
	movl	%eax, %ss
	shlq	$32, %rbp
	orq	%rbp, %rbx
	jmp	*%rbx

	.data
	.p2align 3
gdt64:
	.quad	0
	.quad	0x00209a0000000000
	.quad	0x0000920000000000
gdt64desc:
	.word	. - gdt64 - 1
	.long	0
farptr:
	.long	0
	.word	0x08
kentry:	.quad	0
kcr3:	.long	0
kinfo:	.long	0

	.p2align 3
	.globl	boot_part_lba
boot_part_lba:
	.quad	0
	.globl	boot_drive
boot_drive:
	.long	0

	.section .bss.stack, "aw", @nobits
	.p2align 4
stack:
	.space	16384
stack_top:

	.section .note.GNU-stack,"",@progbits
