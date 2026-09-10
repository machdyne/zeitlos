/*
 * Entry stub for the REFERENCE build of a libz test.
 *
 * zcc patches two words inside the blob -- main's address and the
 * image's _end, which is where the heap starts. A GCC build links
 * libz's objects directly and never goes through the blob, so it has
 * to do the same two jobs here: store _end into libz_heap_start, then
 * call main.
 *
 * Without this the reference build's malloc() returns NULL forever
 * and the two outputs differ for a reason that has nothing to do with
 * the compiler under test.
 */
/* libz_heap_start lives in start.S in a real blob build, and start.S
 * is not linked here -- the reference build has its own entry point.
 * Defining it here rather than pulling in start.S keeps the two entry
 * paths from fighting over _start. */
    .data
    .globl libz_heap_start
libz_heap_start:
    .word   0

    .section .text.start
    .globl _start
_start:
    la      t0, _end
    la      t1, libz_heap_start
    sw      t0, 0(t1)
    call    main
    addi    sp, sp, -16
    sw      zero, 0(sp)
    sw      a0, 4(sp)
    li      a0, 1
    mv      a1, sp
    mv      a2, zero
    lw      t0, 12(zero)
    jalr    ra, t0, 0
1:  j       1b
