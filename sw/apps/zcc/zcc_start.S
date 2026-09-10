/*
 * Entry point for zcc-as-an-app.
 *
 * NOT libz/start.S. That one exists to be the front of a blob that zcc
 * PRODUCES: it carries the jump table and calls main through a word
 * the compiler patches. zcc itself is an ordinary GCC-built app that
 * links libz's objects directly, so it needs neither -- it needs the
 * two jobs that patching does, done here instead.
 *
 * The kernel starts a process with pc at the image base and sp at the
 * top of its block, with a sentinel return address already on the
 * stack (docs/app_runtime.md).
 */
    .section .text.start, "ax"
    .globl _start
_start:
    /* libz's malloc grows the heap from here. `_end` is the top of
     * .bss, which the linker script computes and mkexec.py turns into
     * the ZEXE header's bss_size -- so this is the first byte past
     * everything the loader wrote. */
    la      t0, _end
    la      t1, libz_heap_start
    sw      t0, 0(t1)

    /* argc = 0, argv = NULL.
     *
     * main() ignores both on this build (see its comment), but leaving
     * them holding whatever the kernel left behind is how the bug that
     * prompted this comment got in. Two instructions to make the
     * assembly say what the C assumes. */
    li      a0, 0
    li      a1, 0
    call    main

    /* main returning is a normal exit. z_exit() is in libz and does
     * not return; the spin is the backstop for a kernel that somehow
     * does, which would otherwise fall into whatever follows. */
    call    z_exit
1:  j       1b

    .data
    .globl libz_heap_start
libz_heap_start:
    .word   0
