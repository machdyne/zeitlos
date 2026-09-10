/* Linker script for the REFERENCE build only.
 *
 * zcc needs no linker script -- it lays the image out itself. This
 * exists so the same test source can also be built with GCC, at the
 * same base address and into the same ZEXE format, so that the two can
 * be run against each other. See run.sh.
 */
ENTRY(_start)
SECTIONS
{
  . = 0x80000000;
  .text : { *(.text.start) *(.text*) *(.rodata*) *(.srodata*) }
  . = ALIGN(4);
  .data : { *(.data*) *(.sdata*) }
  . = ALIGN(4);
  _edata = .;
  .bss  : { *(.bss*) *(.sbss*) *(COMMON) }
  . = ALIGN(4);
  _end = .;
}
