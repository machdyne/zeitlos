/*
 * zeitlos32 -- loads and stores.
 *
 * Check ids 300-399.
 *
 * The core drives a 32-bit wishbone bus with byte selects, so every
 * sub-word access is a lane-steering problem: a store has to place
 * the data in the right byte lane AND set the right wbm_sel_o bits,
 * and a load has to pick the right lane back out and sign- or
 * zero-extend it. Both directions are tested at all four byte offsets
 * and both halfword offsets, because getting three of four lanes
 * right is the normal failure mode, not getting none of them right.
 *
 * Stores are also checked for what they must NOT disturb -- a store
 * with the wrong byte selects usually produces a correct-looking
 * readback of the byte it wrote while quietly destroying its
 * neighbours.
 */

#include "test.h"

	TEST_ENTRY

	la	s0, scratch

	/* ---- sw / lw ---- */

	li	a0, 0x12345678
	sw	a0, 0(s0)
	lw	a1, 0(s0)
	CHECK_EQ 300, a1, 0x12345678

	/* a second word, to catch an address that ignores its low bits */
	li	a0, 0xdeadbeef
	sw	a0, 4(s0)
	lw	a1, 4(s0)
	CHECK_EQ 301, a1, 0xdeadbeef
	lw	a1, 0(s0)
	CHECK_EQ 302, a1, 0x12345678

	/* negative offset */
	lw	a1, -4(s0)
	sw	a1, 8(s0)
	lw	a2, 8(s0)
	CHECK_REG 303, a1, a2

	/* ---- lb / lbu at all four offsets ---- */

	li	a0, 0x8090a0b0
	sw	a0, 0(s0)

	lbu	a1, 0(s0)
	CHECK_EQ 310, a1, 0xb0
	lbu	a1, 1(s0)
	CHECK_EQ 311, a1, 0xa0
	lbu	a1, 2(s0)
	CHECK_EQ 312, a1, 0x90
	lbu	a1, 3(s0)
	CHECK_EQ 313, a1, 0x80

	/* the same bytes signed: all four have bit 7 set */
	lb	a1, 0(s0)
	CHECK_EQ 314, a1, 0xffffffb0
	lb	a1, 1(s0)
	CHECK_EQ 315, a1, 0xffffffa0
	lb	a1, 2(s0)
	CHECK_EQ 316, a1, 0xffffff90
	lb	a1, 3(s0)
	CHECK_EQ 317, a1, 0xffffff80

	/* and a positive byte, so sign extension is not just always-on */
	li	a0, 0x01020304
	sw	a0, 0(s0)
	lb	a1, 0(s0)
	CHECK_EQ 318, a1, 4
	lb	a1, 3(s0)
	CHECK_EQ 319, a1, 1

	/* ---- lh / lhu at both offsets ---- */

	li	a0, 0x80007fff
	sw	a0, 0(s0)

	lhu	a1, 0(s0)
	CHECK_EQ 320, a1, 0x7fff
	lhu	a1, 2(s0)
	CHECK_EQ 321, a1, 0x8000

	lh	a1, 0(s0)
	CHECK_EQ 322, a1, 0x7fff
	lh	a1, 2(s0)
	CHECK_EQ 323, a1, 0xffff8000

	/* ---- sb at all four offsets, with neighbours preserved ---- */

	li	a0, 0
	sw	a0, 0(s0)

	li	a0, 0xaa
	sb	a0, 0(s0)
	lw	a1, 0(s0)
	CHECK_EQ 330, a1, 0x000000aa

	li	a0, 0xbb
	sb	a0, 1(s0)
	lw	a1, 0(s0)
	CHECK_EQ 331, a1, 0x0000bbaa

	li	a0, 0xcc
	sb	a0, 2(s0)
	lw	a1, 0(s0)
	CHECK_EQ 332, a1, 0x00ccbbaa

	li	a0, 0xdd
	sb	a0, 3(s0)
	lw	a1, 0(s0)
	CHECK_EQ 333, a1, 0xddccbbaa

	/* sb must ignore the upper 24 bits of its source */
	li	a0, 0xffffff11
	sb	a0, 0(s0)
	lw	a1, 0(s0)
	CHECK_EQ 334, a1, 0xddccbb11

	/* ---- sh at both offsets ---- */

	li	a0, 0
	sw	a0, 0(s0)

	li	a0, 0x1234
	sh	a0, 0(s0)
	lw	a1, 0(s0)
	CHECK_EQ 340, a1, 0x00001234

	li	a0, 0x5678
	sh	a0, 2(s0)
	lw	a1, 0(s0)
	CHECK_EQ 341, a1, 0x56781234

	/* sh must ignore the upper 16 bits of its source */
	li	a0, 0xffff9abc
	sh	a0, 0(s0)
	lw	a1, 0(s0)
	CHECK_EQ 342, a1, 0x56789abc

	/* ---- a load into x0 is discarded, but must still happen ---- */

	li	a0, 0x11223344
	sw	a0, 0(s0)
	lw	x0, 0(s0)
	CHECK_EQ 350, x0, 0

	/* ---- back-to-back accesses, no state left over between them ---- */

	li	a0, 0
	li	a1, 16
1:	sw	a0, 0(s0)
	addi	s0, s0, 4
	addi	a0, a0, 1
	bne	a0, a1, 1b

	la	s0, scratch
	li	a0, 0
1:	lw	a2, 0(s0)
	CHECK_REG 360, a2, a0
	addi	s0, s0, 4
	addi	a0, a0, 1
	bne	a0, a1, 1b

	TEST_PASS

/* ------------------------------------------------------------ */

	.data
	.balign 4
	.fill 4, 4, 0xa5a5a5a5		/* guard, and a target for -4(s0) */
scratch:
	.fill 64, 4, 0
