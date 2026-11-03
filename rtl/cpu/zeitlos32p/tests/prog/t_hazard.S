/*
 * zeitlos32p -- a load-use stall must not judge the stale operand.
 *
 * Check ids 990-998.
 *
 * A load or store waiting in X for the load that gives it its base
 * register sees the register's OLD value until the load is done. If
 * that old value is misaligned, the access must not be taken for
 * misaligned: with IRQ 2 masked (or inside a handler) that would halt
 * the core. Found running the OS's LZ4 decoder (rtl/tb/cache_soc_lz4),
 * where `lw a4,12(sp); sw a5,0(a4)` met an a4 still holding 0x1dca.
 * Everything here runs with every interrupt masked.
 */

#include "test.h"

#define r_type_insn(_f7, _rs2, _rs1, _f3, _rd, _opc) \
	.word (((_f7) << 25) | ((_rs2) << 20) | ((_rs1) << 15) | ((_f3) << 12) | ((_rd) << 7) | ((_opc) << 0))
#define maskirq(_rd, _rs)  r_type_insn(0b0000011, 0, _rs, 0b110, _rd, 0b0001011)

	.section .text
	.option norelax
	.globl _start
_start:
	j	_test_main
	nop
	nop
	nop
	.org 0x10
	TEST_FAIL 998				/* no interrupt may be raised */

_test_main:
	li	a0, -1
	maskirq(0, 10)				/* all masked: a misaligned access halts */
	la	s0, slots
	la	t3, target
	sw	t3, 0(s0)			/* an aligned pointer, in memory */

	/* store through a pointer just loaded over a misaligned value */
	li	a4, 0x1dca
	lw	a4, 0(s0)
	li	a5, 0x1234
	sw	a5, 0(a4)
	lw	a6, 0(t3)
	CHECK_EQ 990, a6, 0x1234

	/* the same with no instruction between */
	li	a4, 0x0003
	lw	a4, 0(s0)
	sw	s0, 0(a4)
	lw	a6, 0(t3)
	CHECK_REG 991, a6, s0

	/* a load through it */
	li	a4, 0x0002
	lw	a4, 0(s0)
	lw	a6, 4(a4)
	CHECK_EQ 992, a6, 0x5678

	/* a halfword store through it */
	li	a4, 0x0001
	lw	a4, 0(s0)
	sh	a5, 8(a4)
	lhu	a6, 8(t3)
	CHECK_EQ 993, a6, 0x1234

	/* behind a load that misses its way through slow memory: two
	 * loads in a row, the second one's base from the first */
	li	a4, 0x7
	li	a3, 0x5
	lw	a3, 0(s0)
	lw	a4, 0(s0)
	sw	a5, 12(a4)
	sw	a5, 12(a3)
	lw	a6, 12(t3)
	CHECK_EQ 994, a6, 0x1234

	TEST_PASS

	.align 4
slots:
	.word	0, 0
target:
	.word	0, 0x5678, 0, 0
