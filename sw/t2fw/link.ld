/* zeitlos32g's memories (rtl/gpu/zeitlos32g.v, docs/zgl_t2.md) */
MEMORY {
	IMEM (rx) : ORIGIN = 0x00000000, LENGTH = 12K
	DMEM (rw) : ORIGIN = 0x00010000, LENGTH = 4K
}
SECTIONS {
	.text : { *(.text.start) *(.text .text.*) *(.rodata .rodata.* .srodata .srodata.*) } > IMEM
	.data : { *(.data .data.* .sdata .sdata.*) } > DMEM
	.bss (NOLOAD) : { __bss_start = .; *(.bss .bss.* .sbss .sbss.* COMMON) . = ALIGN(4); __bss_end = .; } > DMEM
	__stack_top = ORIGIN(DMEM) + LENGTH(DMEM);
	/DISCARD/ : { *(.comment) *(.riscv.attributes) *(.eh_frame) }
}
