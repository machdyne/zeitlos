/* rtl/tb/cache_soc: everything in the first 64KB of main memory */
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x40000000, LENGTH = 0xC000 }
SECTIONS {
	.text : { *(.text.start) *(.text*) } > RAM
	. = ALIGN(64);
	.text.exec_a : { *(.text.exec_a) } > RAM
	. = ALIGN(64);
	.text.exec_b : { *(.text.exec_b) } > RAM
	.rodata : { *(.rodata*) *(.srodata*) } > RAM
	.data : { *(.data*) *(.sdata*) } > RAM
	.bss : { _bss_start = .; *(.bss*) *(.sbss*) *(COMMON) . = ALIGN(4); _bss_end = .; } > RAM
}
