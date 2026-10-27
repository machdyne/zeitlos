# tools/learn/rvbench: the start, for sim.py
	.section .text.init
	.globl _start
_start:
	li sp, 0x00ff0000
	.option push
	.option norelax
	la gp, __global_pointer$
	.option pop
	la t0, __bss_start
	la t1, _end
1:	bgeu t0, t1, 2f
	sw zero, 0(t0)
	addi t0, t0, 4
	j 1b
2:	call main
	li a7, 93
	ecall
3:	j 3b
