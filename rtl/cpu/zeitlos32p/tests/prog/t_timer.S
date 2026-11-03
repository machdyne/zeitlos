/*
 * zeitlos32p -- picorv32's IRQ timer (`timer rd, rs`).
 *
 * Check ids 900-949.
 *
 * The kernel yields with timer(1) (sw/os/kernel.c), so the bitstream's
 * picorv32 is built with ENABLE_IRQ_TIMER and zeitlos32p implements it:
 * rd gets the counter, the counter gets rs, it counts down one a
 * clock, and IRQ 0 is raised as it goes from 1 to 0; 0 is off. IRQ 0
 * is latched, so one raised while masked is delivered on unmasking.
 * zeitlos32 has no timer, so this runs on zeitlos32p alone.
 */

#include "test.h"

#define r_type_insn(_f7, _rs2, _rs1, _f3, _rd, _opc) \
	.word (((_f7) << 25) | ((_rs2) << 20) | ((_rs1) << 15) | ((_f3) << 12) | ((_rd) << 7) | ((_opc) << 0))

#define maskirq(_rd, _rs)  r_type_insn(0b0000011, 0, _rs, 0b110, _rd, 0b0001011)
#define retirq()           r_type_insn(0b0000010, 0,   0, 0b000,   0, 0b0001011)
#define getq(_rd, _qs)     r_type_insn(0b0000000, 0, _qs, 0b100, _rd, 0b0001011)
#define setq(_qd, _rs)     r_type_insn(0b0000001, 0, _rs, 0b010, _qd, 0b0001011)
#define timer(_rd, _rs)    r_type_insn(0b0000101, 0, _rs, 0b110, _rd, 0b0001011)

	.section .text
	.option norelax
	.globl _start
_start:
	j	_test_main
	nop
	nop
	nop

	.org 0x10
irq_vec:
	setq(2, 1)
	setq(3, 2)
	la	x1, irq_log
	getq(2, 1)				/* the delivered mask */
	sw	x2, 0(x1)
	lw	x2, 4(x1)
	addi	x2, x2, 1
	sw	x2, 4(x1)
	getq(1, 2)
	getq(2, 3)
	retirq()

_test_main:
	la	s0, irq_log
	li	a0, 0
	maskirq(0, 10)				/* everything open */

	/* timer(1): fires at once; the counter was off (0) */
	li	a1, 1
	timer(12, 11)				/* a2 = old, counter = 1 */
	CHECK_Z	900, a2
	nop
	nop
	nop
	lw	a3, 4(s0)
	CHECK_EQ 901, a3, 1
	lw	a3, 0(s0)
	CHECK_EQ 902, a3, 1			/* IRQ 0 */

	/* load 1000, read it back at once, switch it off */
	li	a1, 1000
	timer(12, 11)
	CHECK_Z	903, a2
	timer(12, 0)				/* a2 = what is left, counter off */
	li	t2, 1000
	bgeu	a2, t2, fail904
	li	t2, 900
	bltu	a2, t2, fail904
	li	t2, 3000			/* long past 1000: nothing fires */
1:	addi	t2, t2, -1
	bnez	t2, 1b
	lw	a3, 4(s0)
	CHECK_EQ 905, a3, 1

	/* timer(50), wait for it */
	li	a1, 50
	timer(12, 11)
	CHECK_Z	906, a2
	li	t2, 1000
2:	lw	a3, 4(s0)
	li	t3, 2
	beq	a3, t3, 3f
	addi	t2, t2, -1
	bnez	t2, 2b
	TEST_FAIL 907
3:
	/* masked: raised and held, delivered on unmasking */
	li	a0, 1
	maskirq(0, 10)
	li	a1, 5
	timer(12, 11)
	li	t2, 200
4:	addi	t2, t2, -1
	bnez	t2, 4b
	lw	a3, 4(s0)
	CHECK_EQ 908, a3, 2			/* not yet */
	li	a0, 0
	maskirq(0, 10)
	nop
	nop
	nop
	lw	a3, 4(s0)
	CHECK_EQ 909, a3, 3

	TEST_PASS
fail904:
	TEST_FAIL 904

	.align 4
irq_log:
	.word	0, 0
