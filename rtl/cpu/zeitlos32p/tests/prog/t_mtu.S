/*
 * zeitlos32p -- changing the MTU base, against zeitlos32.
 *
 * Check ids 950-989. Run by tb_mpu_lockstep.v (make mpu), whose MTU
 * base is a register written by a store to 0x9000_0000.
 *
 * The kernel switches the base (reg_mtu, sw/os/kernel.c) and may touch
 * the app window right after. On zeitlos32 the store has finished by
 * then; on zeitlos32p it may still be on its way to the bus while the
 * next instruction translates its address. The new base must be the
 * one used -- for loads, stores, and a jump into the window.
 */

#include "test.h"

#define MTU_REG  0x90000000
#define BASE_A   0x40008000
#define BASE_B   0x40009000

	.section .text
	.option norelax
	.globl _start
_start:
	j	_test_main
	nop
	nop
	nop
	.org 0x10
	TEST_FAIL 989				/* no interrupts expected */

_test_main:
	li	s0, MTU_REG
	li	s1, BASE_A
	li	s2, BASE_B
	li	s3, 0x80000400			/* a data word in the window */

	/* store through A, switch to B, store and load at once */
	sw	s1, 0(s0)
	li	t2, 0x1111
	sw	t2, 0(s3)
	sw	s2, 0(s0)
	li	t2, 0x2222
	sw	t2, 0(s3)			/* right behind the switch */
	lw	a0, 0(s3)
	CHECK_EQ 950, a0, 0x2222
	sw	s1, 0(s0)
	lw	a1, 0(s3)			/* back to A, at once */
	CHECK_EQ 951, a1, 0x1111

	/* where did they land? */
	li	t3, BASE_A+0x400
	lw	a2, 0(t3)
	CHECK_EQ 952, a2, 0x1111
	li	t3, BASE_B+0x400
	lw	a3, 0(t3)
	CHECK_EQ 953, a3, 0x2222

	/* load right behind a switch, no store in between */
	sw	s2, 0(s0)
	lw	a4, 0(s3)
	CHECK_EQ 954, a4, 0x2222

	/* a jump into the window right behind a switch: base B's code
	 * returns 0xb, base A's 0xa */
	sw	s1, 0(s0)
	nop
	sw	s2, 0(s0)
	li	t2, 0x80000100
	jalr	ra, t2
	CHECK_EQ 960, a5, 0xb
	sw	s1, 0(s0)
	li	t2, 0x80000100
	jalr	ra, t2
	CHECK_EQ 961, a5, 0xa

	TEST_PASS

	/* base A's block (phys 0x4000_8000, image 0x8000) */
	.org 0x8100
	li	a5, 0xa
	ret
	.org 0x8400
	.word	0

	/* base B's block (phys 0x4000_9000, image 0x9000) */
	.org 0x9100
	li	a5, 0xb
	ret
	.org 0x9400
	.word	0
