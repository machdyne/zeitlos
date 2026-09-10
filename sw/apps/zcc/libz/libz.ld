/*
 * libz linker script.
 *
 * The blob is linked at 0x80000000 -- the same address every Zeitlos
 * app runs at -- and zcc copies it verbatim to offset 0 of every image
 * it produces. So every absolute address inside it is correct by
 * construction, with no relocation and no position-independent code.
 *
 * Three things have to be at known places:
 *
 *   0x80000000  _start        the kernel jumps here (docs/app_runtime.md)
 *   +16         libz_header   ABI version, table offset, patch slots
 *   ...         libz_table    one `j` per entry, in libz_table.def order
 *
 * .bss is INSIDE the blob and is padded out as zeros in the file
 * rather than counted in the ZEXE header. That costs a few hundred
 * bytes of image and removes a whole class of ordering bug: libz's
 * bss would otherwise sit in the middle of the image, before the
 * user's .text, which is not a shape the ZEXE header can describe --
 * it has exactly one bss and it is at the end.
 */
ENTRY(_start)
SECTIONS
{
  . = 0x80000000;

  .start : { KEEP(*(.libz.start)) }
  . = 0x80000010;
  .header : { KEEP(*(.libz.header)) }
  .table : { KEEP(*(.libz.table)) }

  .text : { *(.text*) }
  . = ALIGN(4);
  .rodata : { *(.rodata*) *(.srodata*) }
  . = ALIGN(4);
  .data : { *(.data*) *(.sdata*) }
  . = ALIGN(4);
  .bss : { *(.bss*) *(.sbss*) *(COMMON) }
  . = ALIGN(16);
  _libz_end = .;

  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
