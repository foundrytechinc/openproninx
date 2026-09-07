// MBR. Finds the fnu-boot partition in the GPT and loads stage2 at 0x10000.
// BIOS enters at 0:0x7c00 with %dl = drive.

#define RELOC		0x0600
#define GPTHDR		0x0900
#define GPTENT		0x0b00
#define STACK		0x7c00
#define STAGE2_SEG	0x1000

#define DAP_SIZE	0x0800
#define DAP_COUNT	0x0802
#define DAP_OFF		0x0804
#define DAP_SEG		0x0806
#define DAP_LBA		0x0808
#define DAP		0x0800

	.section .stage_1, "awx"
	.code16
.globl start
start:
	cli
	xorw	%ax, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	$STACK, %sp
	sti
	cld

	movw	$STACK, %si
	movw	$RELOC, %di
	movw	$256, %cx
	rep	movsw
	ljmp	$0, $reloc

reloc:
	movb	%dl, drive

	movw	$0x55aa, %bx
	movb	$0x41, %ah
	int	$0x13
	jc	no_ext
	cmpw	$0xaa55, %bx
	jne	no_ext

	movl	$1, %eax
	xorl	%edx, %edx
	movw	$GPTHDR, %bx
	movw	$1, %cx
	call	read

	movw	$GPTHDR, %si
	movw	$sig, %di
	movw	$8, %cx
	repe	cmpsb
	jne	no_gpt

	movl	GPTHDR+80, %eax
	movw	%ax, nents
	movl	GPTHDR+84, %eax
	movw	%ax, entsz
	movl	GPTHDR+72, %eax
	movl	GPTHDR+76, %edx

scan_sector:
	movw	$GPTENT, %bx
	movw	$1, %cx
	call	read
	incl	%eax
	adcl	$0, %edx

	movw	$GPTENT, %di
scan_entry:
	cmpw	$0, nents
	je	no_part

	pushw	%di
	movw	$fnuboot, %si
	movw	$16, %cx
	repe	cmpsb
	popw	%di
	je	found

	testb	$0x04, 48(%di)		// GPT_ENT_ATTR_LEGACY_BIOS_BOOTABLE
	jnz	found

	decw	nents
	addw	entsz, %di
	cmpw	$GPTENT+512, %di
	jb	scan_entry
	jmp	scan_sector

found:
	movl	32(%di), %eax
	movl	36(%di), %edx
	movl	%eax, part_lba
	movl	%edx, part_lba+4

	movw	nsects, %si
	movw	$STAGE2_SEG, %bx
load:
	movw	%si, %cx
	cmpw	$32, %cx
	jbe	1f
	movw	$32, %cx
1:
	movw	%cx, chunk
	movw	%bx, DAP_SEG
	movw	$0, DAP_OFF
	call	read_seg

	movw	chunk, %cx
	movzwl	%cx, %edi
	addl	%edi, %eax
	adcl	$0, %edx
	shlw	$5, %cx
	addw	%cx, %bx
	subw	chunk, %si
	jnz	load

	movb	drive, %dl
	movw	$part_lba, %si
	ljmp	$STAGE2_SEG, $0

// %eax:%edx = lba, %cx = count, %bx = offset with segment 0
read:
	movw	%bx, DAP_OFF
	movw	$0, DAP_SEG
read_seg:
	movw	$0x0010, DAP_SIZE
	movw	%cx, DAP_COUNT
	movl	%eax, DAP_LBA
	movl	%edx, DAP_LBA+4
	pushal
	movw	$DAP, %si
	movb	drive, %dl
	movb	$0x42, %ah
	int	$0x13
	jc	io_err
	popal
	ret

no_ext:		movw	$msg_ext, %si
		jmp	die
no_gpt:		movw	$msg_gpt, %si
		jmp	die
no_part:	movw	$msg_part, %si
		jmp	die
io_err:		movw	$msg_io, %si
die:
	lodsb
	testb	%al, %al
	jz	1f
	movb	$0x0e, %ah
	movb	$0x07, %bl
	int	$0x10
	jmp	die
1:	hlt
	jmp	1b

sig:		.ascii	"EFI PART"
fnuboot:	.long	0x7b589da3
		.word	0xbdc2, 0x4f6c
		.byte	0x80, 0x28, 0xfc, 0x43, 0xd4, 0x6c, 0x59, 0x53

msg_ext:	.asciz	"no int13ext"
msg_gpt:	.asciz	"no gpt"
msg_part:	.asciz	"no fnu-boot"
msg_io:		.asciz	"io"

drive:		.byte	0
nents:		.word	0
entsz:		.word	0
chunk:		.word	0
part_lba:	.quad	0

	// patched by the image builder
	.globl	nsects
nsects:		.word	64

	.org 440
disk_sig:	.long	0
		.word	0
	.org 446
ptable:		.fill	64, 1, 0
.org 510
	.word	0xaa55

.section .note.GNU-stack,"",@progbits
