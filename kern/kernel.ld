/* the kernel: linked in the top 2G, loaded at 1M. KERNLINK in memlayout.h */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
	. = 0xffffffff80100000;

	.text : AT(0x100000) {
		*(.text .text.*)
	}

	PROVIDE(etext = .);

	.rodata : {
		*(.rodata .rodata.*)
	}

	/* what a loader puts in place; the padding around it is not loaded */
	PROVIDE(srodata = ADDR(.rodata));
	PROVIDE(erodata = ADDR(.rodata) + SIZEOF(.rodata));

	. = ALIGN(0x1000);

	/* the kernel map switches from read-only to writable here */
	PROVIDE(data = .);

	.data : {
		*(.data .data.*)
	}

	PROVIDE(edata = .);

	.bss : {
		*(.bss .bss.*)
		*(COMMON)
	}

	PROVIDE(end = .);

	/DISCARD/ : {
		*(.eh_frame .comment)
	}
}
