/* zeitlos32g: T2's geometry unit starts here (rtl/gpu/zeitlos32g.v).
 * The stack at the top of its data memory; .bss cleared; then the
 * program's loop, for ever. */
	.section .text.start
	.globl _start
_start:
	la	sp, __stack_top
	la	t0, __bss_start
	la	t1, __bss_end
1:	bgeu	t0, t1, 2f
	sw	zero, 0(t0)
	addi	t0, t0, 4
	j	1b
2:	call	main
	j	2b
