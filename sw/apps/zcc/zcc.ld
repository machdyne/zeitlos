/*
 * zcc as a Zeitlos app.
 *
 * Same shape as sw/common/riscv-app.ld -- everything at 0x80000000,
 * with _edata and _end so that tools/mkexec.py can compute the ZEXE
 * header's bss_size. A separate script rather than reusing that one
 * because this binary has no newlib sections to place, and because
 * .text.start has to come first: the kernel jumps to the image base
 * and expects to find the entry point there.
 *
 * There is no jump table here. That belongs to the blob zcc PRODUCES,
 * not to zcc, which links libz's objects the ordinary way.
 */
ENTRY(_start)
SECTIONS
{
  . = 0x80000000;
  .text : { KEEP(*(.text.start)) *(.text*) }
  . = ALIGN(4);
  .rodata : { *(.rodata*) *(.srodata*) }
  . = ALIGN(4);
  .data : { *(.data*) *(.sdata*) }
  . = ALIGN(4);
  _edata = .;
  .bss : { *(.bss*) *(.sbss*) *(COMMON) }
  . = ALIGN(16);
  _end = .;

  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
