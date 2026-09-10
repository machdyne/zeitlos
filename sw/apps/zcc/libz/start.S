/*
 * libz -- entry point, header and jump table.
 *
 * Assembled with -x assembler-with-cpp so that the table can be
 * generated from libz_table.def by the same X-macro the C side and
 * the symbol generator use. Three copies of that list, hand-kept in
 * step, is exactly the drift libz_table.def's header warns about.
 */

#include "libz.h"

    .section .libz.start, "ax"
    .globl _start
_start:
    /*
     * The kernel starts a process here: pc = image base, sp = top of
     * its block, with a sentinel return address already on the stack
     * (docs/app_runtime.md, "Process model").
     *
     * main's address is not known when this blob is built, so it is
     * read from a slot that zcc patches at output time. An indirect
     * call rather than a patched `jal` because a patched immediate
     * would have to encode a J-type offset, and a word is a word.
     */
    lw      t0, libz_main_ptr
    jalr    ra, t0, 0
    j       libz_finish

    .section .libz.header, "a"
    .globl libz_header
libz_header:
    .word   0x7a4c4942              /* "BILz" -- magic, little-endian */
    .word   LIBZ_ABI_VERSION
    .globl libz_main_ptr
libz_main_ptr:
    .word   0                       /* patched by zcc: address of main */
    .globl libz_heap_start
libz_heap_start:
    .word   0                       /* patched by zcc: image _end */

    .section .libz.table, "ax"
    .globl libz_table
libz_table:
/*
 * One instruction per entry: an unconditional jump to the real
 * function. A caller does `jal ra, table + 4*i`, which lands here with
 * ra pointing back into its own code; the `j` then transfers to the
 * function with ra untouched, so the function returns straight to the
 * caller. One extra instruction per call and no register clobbered.
 */
#define LIBZ_ENTRY(name) j name
#include "libz_table.def"
#undef LIBZ_ENTRY
