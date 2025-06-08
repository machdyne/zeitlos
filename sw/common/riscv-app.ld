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

  .bss ALIGN(4) : {
    _sbss = .;
    *(COMMON)
    *(.sbss .sbss.*)
    *(.bss .bss.*)
    _ebss = .;
  }

  _end = .;
}
