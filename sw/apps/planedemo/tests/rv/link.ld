ENTRY(_start)
SECTIONS {
	. = 0x10000;
	.text : { *(.text._start) *(.text*) }
	.rodata : { *(.rodata*) *(.srodata*) }
	.data : { *(.data*) *(.sdata*) }
	.bss : { __bss_start = .; *(.bss*) *(.sbss*) *(COMMON) __bss_end = .; }
	. = ALIGN(16);
	end = .;
	_end = .;
}
