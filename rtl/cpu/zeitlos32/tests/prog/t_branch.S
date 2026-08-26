/*
 * zeitlos32 -- branches, jumps and link registers.
 *
 * Check ids 200-299.
 *
 * Every conditional branch is exercised in BOTH directions. Testing
 * only the taken case passes on a core that always branches, and
 * testing only the not-taken case passes on a core that never does;
 * neither half is worth much alone.
 *
 * Backward branches get their own checks because they are the only
 * ones with a negative immediate, and imm_b's sign extension is
 * assembled from four separate slices of the instruction word
 * (rtl/cpu/zeitlos32/zeitlos32.v) -- the single most fiddly piece of
 * decode in the core.
 */

#include "test.h"

	TEST_ENTRY

	li	s0, 0				/* progress counter */

	/* ---- beq ---- */

	li	a0, 5
	li	a1, 5
	beq	a0, a1, 1f
	TEST_FAIL 200
1:	li	a1, 6
	beq	a0, a1, 2f
	j	3f
2:	TEST_FAIL 201
3:

	/* ---- bne ---- */

	li	a0, 5
	li	a1, 6
	bne	a0, a1, 1f
	TEST_FAIL 202
1:	li	a1, 5
	bne	a0, a1, 2f
	j	3f
2:	TEST_FAIL 203
3:

	/* ---- blt / bge (signed) ---- */

	li	a0, -1
	li	a1, 1

	blt	a0, a1, 1f
	TEST_FAIL 210
1:	blt	a1, a0, 2f
	j	3f
2:	TEST_FAIL 211
3:

	bge	a1, a0, 1f
	TEST_FAIL 212
1:	bge	a0, a1, 2f
	j	3f
2:	TEST_FAIL 213
3:

	/* equal: bge taken, blt not */
	bge	a0, a0, 1f
	TEST_FAIL 214
1:	blt	a0, a0, 2f
	j	3f
2:	TEST_FAIL 215
3:

	/* ---- bltu / bgeu (unsigned) ---- */

	li	a0, -1				/* 0xffffffff, the largest unsigned */
	li	a1, 1

	bltu	a1, a0, 1f
	TEST_FAIL 220
1:	bltu	a0, a1, 2f
	j	3f
2:	TEST_FAIL 221
3:

	bgeu	a0, a1, 1f
	TEST_FAIL 222
1:	bgeu	a1, a0, 2f
	j	3f
2:	TEST_FAIL 223
3:

	/* signed and unsigned must disagree on this pair -- the single
	 * most common way to get the comparator wrong */
	blt	a0, a1, 1f			/* -1 < 1 signed: taken */
	TEST_FAIL 224
1:	bltu	a0, a1, 2f		/* 0xffffffff < 1 unsigned: not taken */
	j	3f
2:	TEST_FAIL 225
3:

	/* ---- backward branch ---- */

	li	a0, 0
	li	a1, 10
1:	addi	a0, a0, 1
	bne	a0, a1, 1b
	CHECK_EQ 230, a0, 10

	/* ---- jal: link register and target ---- */

	li	s1, 0
	jal	ra, subr_a
	CHECK_EQ 240, s1, 0xa5

	/* jal x0 does not write a link register */
	li	ra, 0x1234
	jal	x0, 1f
	TEST_FAIL 241
1:	CHECK_EQ 242, ra, 0x1234

	/* ---- jalr ---- */

	li	s1, 0
	la	a0, subr_b
	jalr	ra, a0, 0
	CHECK_EQ 250, s1, 0x5a

	/* jalr with a nonzero offset */
	li	s1, 0
	la	a0, subr_b
	addi	a0, a0, -8
	jalr	ra, a0, 8
	CHECK_EQ 251, s1, 0x5a

	/* jalr must clear bit 0 of the computed target rather than
	 * faulting on it */
	li	s1, 0
	la	a0, subr_b
	jalr	ra, a0, 1
	CHECK_EQ 252, s1, 0x5a

	/* nested calls: the link register really is a register */
	li	s1, 0
	jal	ra, subr_outer
	CHECK_EQ 260, s1, 3

	TEST_PASS

/* ------------------------------------------------------------ */

subr_a:
	li	s1, 0xa5
	ret

subr_b:
	li	s1, 0x5a
	ret

subr_outer:
	addi	sp, sp, -8
	sw	ra, 0(sp)
	addi	s1, s1, 1
	jal	ra, subr_inner
	addi	s1, s1, 1
	lw	ra, 0(sp)
	addi	sp, sp, 8
	ret

subr_inner:
	addi	s1, s1, 1
	ret
