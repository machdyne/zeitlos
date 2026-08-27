/*
 * zeitlos32 -- picorv32's interrupt ABI.
 *
 * Check ids 600-699.
 *
 * This is the test that matters most, because it is the only part of
 * the core that is not standard RISC-V. sw/bios/boot_picorv32.S and
 * sw/bios/custom_ops.S are a fixed interface: if any of the behaviour
 * checked here differs from picorv32's, the BIOS has to change, and
 * the whole point of this core is that it does not.
 *
 * The custom instruction encodings come from sw/bios/custom_ops.S,
 * reproduced below rather than included, so that this test fails if
 * the core and that file ever disagree -- including the encodings
 * themselves, not just the behaviour.
 *
 * The testbench raises interrupts on request, in the two shapes real
 * sources have (see tb_zeitlos32.v):
 *
 *   IRQ_PULSE  one cycle, like rtl/sysctl.v's irq_timer
 *   IRQ_LEVEL  held until cleared, like the 16550's interrupt output
 *
 * That distinction is the entire reason LATCHED_IRQ exists, so both
 * are exercised.
 */

#include "test.h"

#define IRQ_PULSE  0xe0000010
#define IRQ_LEVEL  0xe0000014

/* ---- sw/bios/custom_ops.S, kept in sync by hand ---- */

#define r_type_insn(_f7, _rs2, _rs1, _f3, _rd, _opc) \
	.word (((_f7) << 25) | ((_rs2) << 20) | ((_rs1) << 15) | ((_f3) << 12) | ((_rd) << 7) | ((_opc) << 0))

#define getq(_rd, _qs)     r_type_insn(0b0000000, 0, _qs, 0b100, _rd, 0b0001011)
#define setq(_qd, _rs)     r_type_insn(0b0000001, 0, _rs, 0b010, _qd, 0b0001011)
#define retirq()           r_type_insn(0b0000010, 0,   0, 0b000,   0, 0b0001011)
#define maskirq(_rd, _rs)  r_type_insn(0b0000011, 0, _rs, 0b110, _rd, 0b0001011)
#define waitirq(_rd)       r_type_insn(0b0000100, 0,   0, 0b100, _rd, 0b0001011)

/* ------------------------------------------------------------ */

	.section .text
	.globl _start
_start:
	j	_test_main
	nop
	nop
	nop

	.org 0x10
irq_vec:
	/* Save the two registers this handler needs before it has
	 * anywhere to put them -- the same trick
	 * sw/bios/boot_picorv32.S opens with. q2 and q3 are untouched
	 * afterwards, so restoring at the end is two instructions. */
	setq(2, 1)					/* q2 = x1 */
	setq(3, 2)					/* q3 = x2 */

	la	x1, irq_save
	getq(2, 0)					/* x2 = q0, the interrupted pc */
	sw	x2, 0(x1)
	getq(2, 1)					/* x2 = q1, the delivered mask */
	sw	x2, 4(x1)

	lw	x2, 8(x1)
	addi	x2, x2, 1
	sw	x2, 8(x1)				/* entry count */

	getq(1, 2)					/* x1 = q2 */
	getq(2, 3)					/* x2 = q3 */
	retirq()

	.org 0x100
_test_main:

	/* ---- q registers are plain scratch storage ---- */

	li	a0, 0x11111111
	setq(0, 10)					/* q0 = a0 */
	li	a0, 0x22222222
	setq(1, 10)
	li	a0, 0x33333333
	setq(2, 10)
	li	a0, 0x44444444
	setq(3, 10)

	getq(11, 0)					/* a1 = q0 */
	CHECK_EQ 600, a1, 0x11111111
	getq(11, 1)
	CHECK_EQ 601, a1, 0x22222222
	getq(11, 2)
	CHECK_EQ 602, a1, 0x33333333
	getq(11, 3)
	CHECK_EQ 603, a1, 0x44444444

	/* getq into x0 is discarded */
	getq(0, 3)
	CHECK_EQ 604, x0, 0

	/* ---- maskirq returns the OLD mask and installs the new one ----
	 *
	 * The mask resets to all ones (everything masked), which is what
	 * lets sw/bios/boot_picorv32.S run its reset path before any
	 * handler can fire. */

	li	a0, 0xffffffff
	maskirq(10, 10)				/* a0 = old mask, mask = all ones */
	CHECK_EQ 610, a0, 0xffffffff

	li	a0, 0x0000ff00
	maskirq(11, 10)				/* a1 = old (all ones), mask = 0xff00 */
	CHECK_EQ 611, a1, 0xffffffff

	li	a0, 0xffffffff
	maskirq(11, 10)				/* a1 = 0xff00, mask back to all ones */
	CHECK_EQ 612, a1, 0x0000ff00

	/* ---- latched lines survive a one-cycle pulse ----
	 *
	 * Everything is masked here, so nothing is delivered; the pulse
	 * has to be caught and held by irq_pending on its own. */

	li	a0, IRQ_PULSE
	li	a1, 0x00000008			/* IRQ 3, latched */
	sw	a1, 0(a0)

	waitirq(12)
	CHECK_EQ 620, a2, 0x00000008

	/* ---- waitirq REPORTS the pending set; it does not consume it ----
	 *
	 * This is picorv32's behaviour and it is load-bearing:
	 * sw/bios/boot_picorv32.S is waitirq immediately followed by
	 * maskirq(zero, zero), and the interrupt it just waited for is
	 * meant to be delivered the moment the mask opens. A waitirq that
	 * cleared the pending set would swallow the first timer tick. */

	waitirq(12)
	CHECK_EQ 621, a2, 0x00000008

	/* ---- and latched lines accumulate ---- */

	li	a0, IRQ_PULSE
	li	a1, 0x00000020			/* IRQ 5, also latched */
	sw	a1, 0(a0)
	waitirq(12)
	CHECK_EQ 622, a2, 0x00000028

	/* ---- an unlatched line does NOT survive a pulse ----
	 *
	 * IRQ 4 is the one line rtl/sysctl.v leaves out of LATCHED_IRQ,
	 * because the 16550 holds its interrupt output until the cause is
	 * read. A pulse on it must leave nothing behind, so the pending
	 * set here is unchanged from the check above. */

	li	a0, IRQ_PULSE
	li	a1, 0x00000010			/* IRQ 4, NOT latched */
	sw	a1, 0(a0)
	waitirq(12)
	CHECK_EQ 630, a2, 0x00000028	/* bit 4 evaporated */

	/* ---- a held line is pending for as long as it is held ---- */

	li	a0, IRQ_LEVEL
	li	a1, 0x00000010			/* IRQ 4, held */
	sw	a1, 0(a0)
	waitirq(12)
	CHECK_EQ 631, a2, 0x00000038

	li	a0, IRQ_LEVEL
	sw	x0, 0(a0)				/* drop it */
	waitirq(12)
	CHECK_EQ 632, a2, 0x00000028	/* and it is gone again */

	/* ---- DELIVERY is what clears the pending set ----
	 *
	 * Opening the mask with 0x28 pending must produce exactly one
	 * entry carrying both lines, and must leave nothing behind. */

	la	a0, irq_save
	sw	x0, 8(a0)				/* entry count = 0 */

	li	a0, 0
	maskirq(0, 10)				/* unmask everything */
	nop
	nop
	nop
	nop
	nop
	nop
	li	a0, 0xffffffff
	maskirq(0, 10)				/* mask again */

	la	a0, irq_save
	lw	a1, 8(a0)
	CHECK_EQ 633, a1, 1			/* one delivery... */
	lw	a1, 4(a0)
	CHECK_EQ 634, a1, 0x00000028	/* ...carrying both lines */

	/* and nothing is left pending, so opening the mask again does
	 * nothing at all */

	la	a0, irq_save
	sw	x0, 8(a0)

	li	a0, 0
	maskirq(0, 10)
	nop
	nop
	nop
	nop
	nop
	nop
	li	a0, 0xffffffff
	maskirq(0, 10)

	la	a0, irq_save
	lw	a1, 8(a0)
	CHECK_Z 635, a1

	/* ---- taking an interrupt ---- */

	la	a0, irq_save
	sw	x0, 8(a0)				/* entry count = 0 */

	li	a0, 0
	maskirq(0, 10)				/* unmask everything */

	li	s0, 0xcafe0000			/* values that must survive */
	li	s1, 0x0000babe
	li	ra, 0x600d0000

	li	a0, IRQ_PULSE
	li	a1, 0x00000008
	sw	a1, 0(a0)

	/* The handler runs somewhere in here. It cannot be pinned to a
	 * particular instruction -- the point is that it runs exactly
	 * once and leaves everything else alone. */
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop

	la	a0, irq_save
	lw	a1, 8(a0)
	CHECK_EQ 640, a1, 1			/* entered exactly once */

	CHECK_EQ 641, s0, 0xcafe0000
	CHECK_EQ 642, s1, 0x0000babe
	CHECK_EQ 643, ra, 0x600d0000	/* the handler saved and restored x1 */

	/* q1 held the delivered bitmask on entry */
	la	a0, irq_save
	lw	a1, 4(a0)
	CHECK_EQ 644, a1, 0x00000008

	/* q0 held the return address, which must be word aligned */
	lw	a1, 0(a0)
	andi	a2, a1, 3
	CHECK_Z 645, a2

	/* ---- a masked interrupt is held, not lost ---- */

	la	a0, irq_save
	sw	x0, 8(a0)

	li	a0, 0xffffffff
	maskirq(0, 10)				/* mask everything */

	li	a0, IRQ_PULSE
	li	a1, 0x00000008
	sw	a1, 0(a0)

	nop
	nop
	nop
	nop
	nop
	nop

	la	a0, irq_save
	lw	a1, 8(a0)
	CHECK_Z 650, a1				/* handler did not run */

	li	a0, 0
	maskirq(0, 10)				/* open the mask */

	nop
	nop
	nop
	nop
	nop
	nop

	la	a0, irq_save
	lw	a1, 8(a0)
	CHECK_EQ 651, a1, 1			/* and now it does */

	/* ---- two interrupts, serviced separately ---- */

	la	a0, irq_save
	sw	x0, 8(a0)

	li	a0, IRQ_PULSE
	li	a1, 0x00000008
	sw	a1, 0(a0)
	nop
	nop
	nop
	nop
	nop
	nop

	li	a0, IRQ_PULSE
	li	a1, 0x00000008
	sw	a1, 0(a0)
	nop
	nop
	nop
	nop
	nop
	nop

	la	a0, irq_save
	lw	a1, 8(a0)
	CHECK_EQ 660, a1, 2

	/* ---- mask everything again before finishing ---- */

	li	a0, 0xffffffff
	maskirq(0, 10)

	TEST_PASS

/* ------------------------------------------------------------ */

	.data
	.balign 4
irq_save:
	.word 0			/* 0: q0, the interrupted pc */
	.word 0			/* 4: q1, the delivered bitmask */
	.word 0			/* 8: entry count */
