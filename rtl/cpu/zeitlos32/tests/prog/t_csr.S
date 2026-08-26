/*
 * zeitlos32 -- the four counter reads.
 *
 * Check ids 500-599.
 *
 * These exist because sw/os/kernel.c's k_cpu_report() and sw/os/sh.c
 * read rdcycle and rdinstret directly with inline asm. They are the
 * only CSRs this core implements; there is no CSR file. picorv32 does
 * the same thing (see its instr_rdcycle decode), which is why the OS
 * can take them for granted.
 *
 * Nothing here checks an absolute count -- that would encode this
 * core's CPI into the test suite and break on any change to the FSM.
 * What is checked is what software actually depends on: the counters
 * advance, they advance monotonically, and cycles advance faster than
 * instructions do.
 */

#include "test.h"

	TEST_ENTRY

	/* ---- rdcycle advances ---- */

	rdcycle	s0
	nop
	nop
	nop
	rdcycle	s1
	sub	a0, s1, s0
	CHECK_NZ 500, a0

	/* and in the right direction */
	sltu	a0, s0, s1
	CHECK_EQ 501, a0, 1

	/* ---- rdinstret advances ---- */

	rdinstret	s0
	nop
	nop
	nop
	rdinstret	s1
	sltu	a0, s0, s1
	CHECK_EQ 510, a0, 1

	/* Exactly five instructions retire between those two reads: the
	 * first rdinstret is counted when IT retires, then three nops,
	 * then the second rdinstret reads the count BEFORE its own
	 * retirement. So the difference is 4.
	 *
	 * This is the one absolute value worth pinning down, because it
	 * is the difference between "counts instructions" and "counts
	 * something correlated with instructions". */
	rdinstret	s0
	nop
	nop
	nop
	rdinstret	s1
	sub	a0, s1, s0
	CHECK_EQ 511, a0, 4

	/* ---- cycles outpace instructions ---- */

	rdcycle		s0
	rdinstret	s2
	li	a0, 20
1:	addi	a0, a0, -1
	bne	a0, x0, 1b
	rdcycle		s1
	rdinstret	s3

	sub	s1, s1, s0			/* cycles elapsed */
	sub	s3, s3, s2			/* instructions retired */
	sltu	a0, s3, s1
	CHECK_EQ 520, a0, 1

	/* ---- the high halves exist and are readable ---- */

	rdcycleh	a0
	rdinstreth	a1
	/* Nothing has run long enough to overflow 32 bits, so both must
	 * still be zero. A core that aliased the high read onto the low
	 * counter would fail here. */
	CHECK_Z 530, a0
	CHECK_Z 531, a1

	/* ---- reading into x0 is discarded ---- */

	rdcycle	x0
	CHECK_EQ 540, x0, 0

	TEST_PASS
