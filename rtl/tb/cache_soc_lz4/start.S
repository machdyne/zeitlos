/* rtl/tb/cache_soc: reset entry for the cache SoC testbench program */
	.section .text.start
	.globl _start
_start:
	li sp, 0x40010000        /* top of the 64KB test window */
	la t0, _bss_start
	la t1, _bss_end
1:	bgeu t0, t1, 2f
	sw zero, 0(t0)
	addi t0, t0, 4
	j 1b
2:	call main
	li t0, 0xf0000004        /* DONE register: a0 is the result */
	sw a0, 0(t0)
3:	j 3b

/* A three-instruction "app" for the MPU test (prog.c). Copied into its
 * own block and run unprivileged: stores a3 into its own memory (a0),
 * tries to store into kernel memory (a1, must be blocked), then leaves
 * through the gate (a2). It never returns: returning into the middle of
 * kernel code is exactly what the gate forbids. */
	.section .text.app
	.globl app_code
	.globl app_code_end
app_code:
	sw a3, 0(a0)
	sw a3, 0(a1)
	jr a2
app_code_end:
