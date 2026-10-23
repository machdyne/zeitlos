	.section .text._start
	.globl _start
_start:
	li sp, 0x03fffff0
	call main
1:	ebreak
	j 1b
