/* zeitlos32 test programs: flat image at 0, matching PROGADDR_RESET.
 * Stack goes at the top of the 64KB the testbench models. */

OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY {
	ram (rwx) : ORIGIN = 0x00000000, LENGTH = 64K
}

SECTIONS {
	.text : {
		*(.text)
		*(.text.*)
		*(.rodata)
		*(.rodata.*)
	} > ram

	.data : ALIGN(4) {
		*(.data)
		*(.data.*)
		*(.sdata)
		*(.sdata.*)
	} > ram

	.bss : ALIGN(4) {
		_bss_start = .;
		*(.bss)
		*(.bss.*)
		*(.sbss)
		*(.sbss.*)
		*(COMMON)
		_bss_end = .;
	} > ram

	_end = .;
}
