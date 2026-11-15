// BIOS SERVICES, cdecl

	.text
	.code32

#define ENTER	pushl %ebp; movl %esp, %ebp; pushl %ebx; pushl %esi; pushl %edi
#define LEAVE	popl %edi; popl %esi; popl %ebx; popl %ebp; ret

// int bios_putc(int c)
	.globl	bios_putc
bios_putc:
	ENTER
	movl	8(%ebp), %eax

	call	prot_to_real
	.code16
	movb	$0x0e, %ah
	movb	$0x07, %bl
	xorb	%bh, %bh
	int	$0x10
	calll	real_to_prot
	.code32

	xorl	%eax, %eax
	LEAVE

// int bios_getkey(void) - the character waiting, or -1
	.globl	bios_getkey
bios_getkey:
	ENTER

	call	prot_to_real
	.code16
	movl	$-1, %ebx
	movb	$0x01, %ah
	int	$0x16
	jz	1f
	xorb	%ah, %ah
	int	$0x16
	movzbl	%al, %ebx
1:
	calll	real_to_prot
	.code32

	movl	%ebx, %eax
	LEAVE

// void bios_wait(int us) - int 15h 86h
	.globl	bios_wait
bios_wait:
	ENTER
	movl	8(%ebp), %edx
	movl	%edx, %ecx
	shrl	$16, %ecx

	call	prot_to_real
	.code16
	movb	$0x86, %ah
	int	$0x15
	calll	real_to_prot
	.code32

	LEAVE

// int bios_disk_extcheck(int drive)
	.globl	bios_disk_extcheck
bios_disk_extcheck:
	ENTER
	movl	8(%ebp), %edx
	movw	$0x55aa, %bx
	xorl	%ecx, %ecx

	call	prot_to_real
	.code16
	movb	$0x41, %ah
	stc
	int	$0x13
	setnc	%al
	calll	real_to_prot
	.code32

	movzbl	%al, %eax
	cmpw	$0xaa55, %bx
	sete	%dl
	movzbl	%dl, %edx
	andl	%edx, %eax
	andl	$1, %ecx		// bit 0: fixed disk access subset
	andl	%ecx, %eax
	LEAVE

// int bios_disk_read(int drive, struct bios_dap *)
	.globl	bios_disk_read
bios_disk_read:
	ENTER
	movl	8(%ebp), %edx
	movl	12(%ebp), %esi

	call	prot_to_real
	.code16
	pushw	%ds
	pushl	%esi
	shrl	$4, %esi
	movw	%ds, %ax
	addw	%si, %ax
	popl	%esi
	andw	$0xf, %si
	movw	%ax, %ds
	movb	$0x42, %ah
	int	$0x13
	setc	%bl
	movb	%ah, %bh
	popw	%ds
	calll	real_to_prot
	.code32

	movzwl	%bx, %eax
	LEAVE

// int bios_disk_reset(int drive)
	.globl	bios_disk_reset
bios_disk_reset:
	ENTER
	movl	8(%ebp), %edx

	call	prot_to_real
	.code16
	xorb	%ah, %ah
	int	$0x13
	setc	%bl
	calll	real_to_prot
	.code32

	movzbl	%bl, %eax
	LEAVE

// drive params. whatever the bios says
	.globl	bios_disk_params
bios_disk_params:
	ENTER
	movl	8(%ebp), %edx
	movl	12(%ebp), %esi

	call	prot_to_real
	.code16
	pushw	%ds
	pushl	%esi
	shrl	$4, %esi
	movw	%ds, %ax
	addw	%si, %ax
	popl	%esi
	andw	$0xf, %si
	movw	%ax, %ds
	movb	$0x48, %ah
	int	$0x13
	setc	%bl
	popw	%ds
	calll	real_to_prot
	.code32

	movzbl	%bl, %eax
	LEAVE

// e820. 0 ok, -1 done, or the bios is dumb
	.globl	bios_e820
bios_e820:
	ENTER
	movl	8(%ebp), %eax
	movl	(%eax), %ebx
	movl	12(%ebp), %edi
	movl	$20, %ecx
	movl	$0x534d4150, %edx

	call	prot_to_real
	.code16
	pushw	%es
	pushw	%di
	shrl	$4, %edi
	movw	%ds, %ax
	addw	%di, %ax
	movw	%ax, %es
	popw	%di
	andw	$0xf, %di
	movl	$0xe820, %eax
	int	$0x15
	setc	%dl
	popw	%es
	calll	real_to_prot
	.code32

	cmpl	$0x534d4150, %eax
	jne	1f
	testb	%dl, %dl
	jnz	1f
	movl	8(%ebp), %eax
	movl	%ebx, (%eax)
	xorl	%eax, %eax
	LEAVE
1:
	movl	$-1, %eax
	LEAVE

// int bios_vbe_info(void *buf)
	.globl	bios_vbe_info
bios_vbe_info:
	ENTER
	movl	8(%ebp), %edi

	call	prot_to_real
	.code16
	pushw	%es
	pushw	%di
	shrl	$4, %edi
	movw	%ds, %ax
	addw	%di, %ax
	movw	%ax, %es
	popw	%di
	andw	$0xf, %di
	movw	$0x4f00, %ax
	int	$0x10
	popw	%es
	calll	real_to_prot
	.code32

	movzwl	%ax, %eax
	LEAVE

// int bios_vbe_mode_info(int mode, void *buf)
	.globl	bios_vbe_mode_info
bios_vbe_mode_info:
	ENTER
	movl	8(%ebp), %ecx
	movl	12(%ebp), %edi

	call	prot_to_real
	.code16
	pushw	%es
	pushw	%di
	shrl	$4, %edi
	movw	%ds, %ax
	addw	%di, %ax
	movw	%ax, %es
	popw	%di
	andw	$0xf, %di
	movw	$0x4f01, %ax
	int	$0x10
	popw	%es
	calll	real_to_prot
	.code32

	movzwl	%ax, %eax
	LEAVE

// EDID over VBE/DDC: 128 bytes of hope
	.globl	bios_vbe_edid
bios_vbe_edid:
	ENTER
	movl	8(%ebp), %edi

	call	prot_to_real
	.code16
	pushw	%es
	pushw	%di
	shrl	$4, %edi
	movw	%ds, %ax
	addw	%di, %ax
	movw	%ax, %es
	popw	%di
	andw	$0xf, %di
	movw	$0x4f15, %ax
	movw	$0x0001, %bx		// read EDID block
	xorw	%cx, %cx		// controller 0
	xorw	%dx, %dx		// block 0
	int	$0x10
	popw	%es
	calll	real_to_prot
	.code32

	movzwl	%ax, %eax
	LEAVE

// ???
	.globl	bios_vbe_get_mode
bios_vbe_get_mode:
	ENTER

	call	prot_to_real
	.code16
	movw	$0x4f03, %ax
	xorw	%bx, %bx
	int	$0x10
	cmpw	$0x004f, %ax
	je	1f
	movw	$0xffff, %bx
1:
	movzwl	%bx, %ebx
	calll	real_to_prot
	.code32

	movl	%ebx, %eax
	LEAVE

// int bios_vbe_set_mode(int mode)
	.globl	bios_vbe_set_mode
bios_vbe_set_mode:
	ENTER
	movl	8(%ebp), %ebx
	orl	$0x4000, %ebx		// linear framebuffer

	call	prot_to_real
	.code16
	movw	$0x4f02, %ax
	int	$0x10
	calll	real_to_prot
	.code32

	movzwl	%ax, %eax
	LEAVE
