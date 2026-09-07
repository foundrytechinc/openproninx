// A20 gate. Try the cheap methods first, verify after each one.

	.text
	.code16

// %ax = 1 when the line is open
a20_test:
	pushw	%ds
	pushw	%es
	pushw	%bx
	pushw	%si
	pushw	%di

	xorw	%ax, %ax
	movw	%ax, %es
	movw	$0x0500, %di
	movw	$0xffff, %ax
	movw	%ax, %ds
	movw	$0x0510, %si

	movb	%es:(%di), %bh
	movb	%ds:(%si), %bl

	movb	$0x00, %es:(%di)
	movb	$0xff, %ds:(%si)
	movb	%es:(%di), %al
	cmpb	$0xff, %al
	setne	%cl

	movb	%bl, %ds:(%si)
	movb	%bh, %es:(%di)

	movzbw	%cl, %ax
	popw	%di
	popw	%si
	popw	%bx
	popw	%es
	popw	%ds
	ret

// wait for the 8042 input buffer to drain, %cx runs out on dead hardware
kbd_wait_in:
	movw	$0xffff, %cx
1:	inb	$0x64, %al
	testb	$0x02, %al
	jz	2f
	loop	1b
2:	ret

kbd_wait_out:
	movw	$0xffff, %cx
1:	inb	$0x64, %al
	testb	$0x01, %al
	jnz	2f
	loop	1b
2:	ret

	.globl	a20_enable
a20_enable:
	call	a20_test
	testw	%ax, %ax
	jnz	done

	movw	$0x2401, %ax
	int	$0x15
	call	a20_test
	testw	%ax, %ax
	jnz	done

	inb	$0x92, %al
	testb	$0x02, %al
	jnz	kbd
	orb	$0x02, %al
	andb	$0xfe, %al		// bit 0 is fast reset
	outb	%al, $0x92
	call	a20_test
	testw	%ax, %ax
	jnz	done

kbd:
	call	kbd_wait_in
	movb	$0xad, %al
	outb	%al, $0x64

	call	kbd_wait_in
	movb	$0xd0, %al
	outb	%al, $0x64

	call	kbd_wait_out
	inb	$0x60, %al
	movb	%al, %bl

	call	kbd_wait_in
	movb	$0xd1, %al
	outb	%al, $0x64

	call	kbd_wait_in
	movb	%bl, %al
	orb	$0x02, %al
	outb	%al, $0x60

	call	kbd_wait_in
	movb	$0xae, %al
	outb	%al, $0x64
	call	kbd_wait_in

	call	a20_test
done:
	ret

	.section .note.GNU-stack,"",@progbits
