SECTIONS
{
  . = 0;

  .text : {
    *(.text .text.*)
    _etext = .;
  }

  .rodata : {
    *(.rodata .rodata.*)
  }

  .data : {
    __global_pointer$ = . + 0x800;
    _sdata = .;
    *(.data .data.*)
    _edata = .;
  }

  .got ALIGN(4) : {
    _got_start = .;
    KEEP(*(.got))
    _got_end = .;
  }

  .got.plt ALIGN(4) : {
    _got.plt_start = .;
    KEEP(*(.got.plt))
    _got.plt_end = .;
  }

  .bss ALIGN(4) : {
    _sbss = .;
    *(COMMON)
    *(.sbss .sbss.*)
    *(.bss .bss.*)
    _ebss = .;
  }

  _end = .;
}
