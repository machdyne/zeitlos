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

  .bss : {
    _sbss = .;
    *(COMMON)
    *(.bss .bss.*)
    _ebss = .;
  }

  _end = .;
}
