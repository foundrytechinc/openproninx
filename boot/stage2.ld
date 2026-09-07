OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start)

SECTIONS {
	. = 0;

	.text : {
		*(.entry)
		*(.text .text.*)
	}

	.rodata : { *(.rodata .rodata.*) }
	.data : { *(.data .data.*) }

	. = ALIGN(4096);
	__bss_start = .;
	.bss (NOLOAD) : {
		*(.bss.stack)
		. = ALIGN(4096);
		*(.bss .bss.*)
		*(COMMON)
	}
	. = ALIGN(16);
	__bss_end = .;

	/DISCARD/ : {
		*(.note.GNU-stack)
		*(.comment)
		*(.eh_frame)
	}
}
