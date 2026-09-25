SECTIONS {
	.memory : {
		. = 0x000000;
		KEEP(*(.init));
		*(.text);
		*(*);
		. = ALIGN(4);
		end = .;
	}
}

