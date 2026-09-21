ENTRY(_start)
SECTIONS { . = 0x10000; .text : { *(.text*) } .rodata : { *(.rodata*) *(.srodata*) } .data : { *(.data*) *(.sdata*) } .bss : { *(.bss*) *(.sbss*) *(COMMON) } }
