/* user programs: linked at 2M, the pages under it never mapped */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
	. = 0x200000;

	.text : {
		*(.text .text.*)
	}

	PROVIDE(etext = .);

	.rodata : {
		*(.rodata .rodata.*)
	}

	. = ALIGN(0x1000);

	.data : {
		*(.data .data.*)
	}

	PROVIDE(edata = .);

	/* the thread-local template: initialised, then zeroed */
	.tdata : {
		PROVIDE(__tls_start = .);
		*(.tdata .tdata.*)
		PROVIDE(__tdata_end = .);
	}
	.tbss : {
		*(.tbss .tbss.*)
		*(.tcommon)
		PROVIDE(__tls_end = .);
	}
	PROVIDE(__tls_align = MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)));

	.bss : {
		*(.bss .bss.*)
		*(COMMON)
	}

	PROVIDE(end = .);

	/DISCARD/ : {
		*(.eh_frame .comment)
	}
}
