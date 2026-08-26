/*
 * zeitlos32 -- RV32M.
 *
 * Check ids 400-499.
 *
 * Sign handling is where a multiplier goes wrong, so the four
 * multiply-high variants are all given the same negative operands and
 * checked against different expected values -- mulh, mulhu and mulhsu
 * differ ONLY in how they read the sign bits, so a core that treats
 * them alike passes any test that uses positive numbers.
 *
 * The divide cases include the two results the RISC-V spec defines
 * rather than leaves undefined: division by zero, and the signed
 * overflow of the most negative integer by -1. Neither traps, both
 * have a specified answer, and software (libgcc, and anything
 * compiled from sw/) relies on it.
 */

#include "test.h"

	TEST_ENTRY

	/* ---- mul ---- */

	li	a0, 7
	li	a1, 6
	mul	a2, a0, a1
	CHECK_EQ 400, a2, 42

	li	a0, -7
	li	a1, 6
	mul	a2, a0, a1
	CHECK_EQ 401, a2, -42

	li	a0, -7
	li	a1, -6
	mul	a2, a0, a1
	CHECK_EQ 402, a2, 42

	li	a0, 0
	li	a1, 0x7fffffff
	mul	a2, a0, a1
	CHECK_EQ 403, a2, 0

	/* mul keeps only the low 32 bits and does not care about signs */
	li	a0, 0x00010000
	li	a1, 0x00010000
	mul	a2, a0, a1
	CHECK_EQ 404, a2, 0

	li	a0, 0xffffffff
	li	a1, 0xffffffff
	mul	a2, a0, a1
	CHECK_EQ 405, a2, 1

	li	a0, 0x12345678
	li	a1, 0x00000010
	mul	a2, a0, a1
	CHECK_EQ 406, a2, 0x23456780

	/* ---- mulh family ---- */

	/* 0x00010000 * 0x00010000 = 0x0000000100000000 */
	li	a0, 0x00010000
	li	a1, 0x00010000
	mulh	a2, a0, a1
	CHECK_EQ 410, a2, 1
	mulhu	a2, a0, a1
	CHECK_EQ 411, a2, 1

	/* -1 * -1 = 1, so the high word is 0 signed... */
	li	a0, -1
	li	a1, -1
	mulh	a2, a0, a1
	CHECK_EQ 412, a2, 0
	/* ...but 0xffffffff * 0xffffffff = 0xfffffffe00000001 unsigned */
	mulhu	a2, a0, a1
	CHECK_EQ 413, a2, 0xfffffffe
	/* and mulhsu reads a0 as -1 and a1 as 4294967295:
	 * -4294967295 = 0xffffffff00000001 */
	mulhsu	a2, a0, a1
	CHECK_EQ 414, a2, 0xffffffff

	/* 0x80000000 * 0x80000000:
	 *   signed   (-2^31)^2 = 2^62      -> high 0x40000000
	 *   unsigned  2^31 * 2^31 = 2^62   -> high 0x40000000
	 *   mixed    -2^31 * 2^31 = -2^62  -> high 0xc0000000 */
	li	a0, 0x80000000
	li	a1, 0x80000000
	mulh	a2, a0, a1
	CHECK_EQ 415, a2, 0x40000000
	mulhu	a2, a0, a1
	CHECK_EQ 416, a2, 0x40000000
	mulhsu	a2, a0, a1
	CHECK_EQ 417, a2, 0xc0000000

	/* a case where the low word matters to the high word's rounding:
	 * 0x7fffffff * 0x7fffffff = 0x3fffffff00000001 */
	li	a0, 0x7fffffff
	li	a1, 0x7fffffff
	mulh	a2, a0, a1
	CHECK_EQ 418, a2, 0x3fffffff
	mul	a2, a0, a1
	CHECK_EQ 419, a2, 1

	/* ---- div / rem, signed ---- */

	li	a0, 100
	li	a1, 7
	div	a2, a0, a1
	CHECK_EQ 430, a2, 14
	rem	a2, a0, a1
	CHECK_EQ 431, a2, 2

	/* the remainder takes the sign of the DIVIDEND, and the quotient
	 * truncates toward zero */
	li	a0, -100
	li	a1, 7
	div	a2, a0, a1
	CHECK_EQ 432, a2, -14
	rem	a2, a0, a1
	CHECK_EQ 433, a2, -2

	li	a0, 100
	li	a1, -7
	div	a2, a0, a1
	CHECK_EQ 434, a2, -14
	rem	a2, a0, a1
	CHECK_EQ 435, a2, 2

	li	a0, -100
	li	a1, -7
	div	a2, a0, a1
	CHECK_EQ 436, a2, 14
	rem	a2, a0, a1
	CHECK_EQ 437, a2, -2

	/* exact division leaves no remainder */
	li	a0, -49
	li	a1, 7
	div	a2, a0, a1
	CHECK_EQ 438, a2, -7
	rem	a2, a0, a1
	CHECK_EQ 439, a2, 0

	/* divisor larger than dividend */
	li	a0, 3
	li	a1, 100
	div	a2, a0, a1
	CHECK_EQ 440, a2, 0
	rem	a2, a0, a1
	CHECK_EQ 441, a2, 3

	/* ---- divu / remu ---- */

	li	a0, 100
	li	a1, 7
	divu	a2, a0, a1
	CHECK_EQ 450, a2, 14
	remu	a2, a0, a1
	CHECK_EQ 451, a2, 2

	/* the same bit patterns, read unsigned: 0xffffffff / 7 */
	li	a0, -1
	li	a1, 7
	divu	a2, a0, a1
	CHECK_EQ 452, a2, 613566756
	remu	a2, a0, a1
	CHECK_EQ 453, a2, 3
	/* and signed, for contrast: -1 / 7 == 0 */
	div	a2, a0, a1
	CHECK_EQ 454, a2, 0
	rem	a2, a0, a1
	CHECK_EQ 455, a2, -1

	/* the full 32-bit range */
	li	a0, 0xffffffff
	li	a1, 0x80000000
	divu	a2, a0, a1
	CHECK_EQ 456, a2, 1
	remu	a2, a0, a1
	CHECK_EQ 457, a2, 0x7fffffff

	/* ---- the two specified special cases ---- */

	/* division by zero: quotient all ones, remainder the dividend */
	li	a0, 42
	li	a1, 0
	div	a2, a0, a1
	CHECK_EQ 460, a2, -1
	rem	a2, a0, a1
	CHECK_EQ 461, a2, 42
	divu	a2, a0, a1
	CHECK_EQ 462, a2, 0xffffffff
	remu	a2, a0, a1
	CHECK_EQ 463, a2, 42

	/* zero divided by zero is the same rule */
	li	a0, 0
	div	a2, a0, a1
	CHECK_EQ 464, a2, -1
	rem	a2, a0, a1
	CHECK_EQ 465, a2, 0

	/* signed overflow: -2^31 / -1 is not representable, and is
	 * defined to be -2^31 with a zero remainder */
	li	a0, 0x80000000
	li	a1, -1
	div	a2, a0, a1
	CHECK_EQ 470, a2, 0x80000000
	rem	a2, a0, a1
	CHECK_EQ 471, a2, 0

	/* unsigned, the same operands are an ordinary division:
	 * 0x80000000 / 0xffffffff == 0 remainder 0x80000000 */
	divu	a2, a0, a1
	CHECK_EQ 472, a2, 0
	remu	a2, a0, a1
	CHECK_EQ 473, a2, 0x80000000

	/* ---- rd == rs, the operand registers must survive ---- */

	li	a0, 12
	li	a1, 5
	mul	a0, a0, a1
	CHECK_EQ 480, a0, 60

	li	a0, 60
	div	a0, a0, a1
	CHECK_EQ 481, a0, 12

	/* ---- a multiply immediately after a multiply ---- */

	li	a0, 3
	li	a1, 5
	mul	a2, a0, a1
	mul	a3, a2, a1
	mul	a4, a3, a1
	CHECK_EQ 490, a2, 15
	CHECK_EQ 491, a3, 75
	CHECK_EQ 492, a4, 375

	TEST_PASS
