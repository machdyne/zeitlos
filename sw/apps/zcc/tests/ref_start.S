/* Entry stub for the REFERENCE build, matching the one zcc emits at
 * .text offset 0: call main, then make the Z_SYS_EXIT syscall through
 * reg_kernel at 0x0c. Kept byte-for-byte equivalent in behaviour so
 * that any output difference between the two builds is the compiler's
 * and not the startup code's. */
    .section .text.start
    .globl _start
_start:
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
