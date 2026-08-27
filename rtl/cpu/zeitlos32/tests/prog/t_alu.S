/*
 * zeitlos32 -- RV32I arithmetic, logic, shift and compare.
 *
 * Check ids 100-199.
 *
 * The register-register and register-immediate forms are tested
 * separately even where they share an ALU path, because they do NOT
 * share operand selection -- rtl/cpu/zeitlos32/zeitlos32.v's alu_b
 * mux and shamt mux both pick different sources for the two, and that
 * mux is exactly the kind of thing that works for one form and not
 * the other.
 */

#include "test.h"

	TEST_ENTRY

	/* ---- lui / auipc ---- */

	lui	a0, 0xabcde
	CHECK_EQ 100, a0, 0xabcde000

	lui	a0, 0x80000
	CHECK_EQ 101, a0, 0x80000000

	/* auipc at a known address: jal leaves the address of the NEXT
	 * instruction in a1, and auipc x,0 is that same address */
	jal	a1, 1f
1:	auipc	a0, 0
	sub	a0, a0, a1
	CHECK_EQ 102, a0, 0

	/* and the immediate really is shifted left by 12 */
	auipc	a0, 1
	auipc	a1, 0
	sub	a0, a0, a1
	CHECK_EQ 103, a0, 0xffc

	/* ---- add / addi / sub ---- */

	li	a0, 100
	li	a1, 23
	add	a2, a0, a1
	CHECK_EQ 110, a2, 123

	sub	a2, a0, a1
	CHECK_EQ 111, a2, 77

	sub	a2, a1, a0
	CHECK_EQ 112, a2, -77

	addi	a2, a0, -150
	CHECK_EQ 113, a2, -50

	/* wraparound */
	li	a0, 0xffffffff
	addi	a2, a0, 1
	CHECK_EQ 114, a2, 0

	li	a0, 0x7fffffff
	addi	a2, a0, 1
	CHECK_EQ 115, a2, 0x80000000

	/* addi sign-extends its 12-bit immediate */
	li	a0, 0
	addi	a2, a0, -1
	CHECK_EQ 116, a2, 0xffffffff

	addi	a2, a0, 2047
	CHECK_EQ 117, a2, 2047

	addi	a2, a0, -2048
	CHECK_EQ 118, a2, -2048

	/* writes to x0 are discarded */
	li	a0, 5
	add	x0, a0, a0
	CHECK_EQ 119, x0, 0

	/* ---- logic ---- */

	li	a0, 0xf0f0f0f0
	li	a1, 0x0ff00ff0

	and	a2, a0, a1
	CHECK_EQ 120, a2, 0x00f000f0

	or	a2, a0, a1
	CHECK_EQ 121, a2, 0xfff0fff0

	xor	a2, a0, a1
	CHECK_EQ 122, a2, 0xff00ff00

	andi	a2, a0, 0x0ff
	CHECK_EQ 123, a2, 0x000000f0

	ori	a2, a0, 0x00f
	CHECK_EQ 124, a2, 0xf0f0f0ff

	xori	a2, a0, -1
	CHECK_EQ 125, a2, 0x0f0f0f0f

	/* ---- shifts ---- */

	li	a0, 0x00000001
	slli	a2, a0, 31
	CHECK_EQ 130, a2, 0x80000000

	slli	a2, a0, 0
	CHECK_EQ 131, a2, 1

	li	a0, 0x80000000
	srli	a2, a0, 31
	CHECK_EQ 132, a2, 1

	srai	a2, a0, 31
	CHECK_EQ 133, a2, 0xffffffff

	srai	a2, a0, 4
	CHECK_EQ 134, a2, 0xf8000000

	li	a0, 0x7fffffff
	srai	a2, a0, 4
	CHECK_EQ 135, a2, 0x07ffffff

	/* register-controlled shift, and the shift amount must be masked
	 * to five bits -- an unmasked barrel shifter gives 0 here */
	li	a0, 0x00000001
	li	a1, 33
	sll	a2, a0, a1
	CHECK_EQ 136, a2, 2

	li	a0, 0x80000000
	li	a1, 36
	sra	a2, a0, a1
	CHECK_EQ 137, a2, 0xf8000000

	li	a1, 4
	srl	a2, a0, a1
	CHECK_EQ 138, a2, 0x08000000

	/* ---- set-less-than ---- */

	li	a0, -1
	li	a1, 1

	slt	a2, a0, a1
	CHECK_EQ 140, a2, 1

	sltu	a2, a0, a1				/* 0xffffffff > 1 unsigned */
	CHECK_EQ 141, a2, 0

	slt	a2, a1, a0
	CHECK_EQ 142, a2, 0

	sltu	a2, a1, a0
	CHECK_EQ 143, a2, 1

	slti	a2, a0, 0
	CHECK_EQ 144, a2, 1

	sltiu	a2, a0, 1				/* 0xffffffff <u 1 is false */
	CHECK_EQ 145, a2, 0

	sltiu	a2, a1, -1				/* 1 <u 0xffffffff is true */
	CHECK_EQ 146, a2, 1

	/* equal values are not less than each other */
	li	a0, 7
	slt	a2, a0, a0
	CHECK_EQ 147, a2, 0
	sltu	a2, a0, a0
	CHECK_EQ 148, a2, 0

	/* ---- every register is really 32 bits and independently
	 *      addressable (catches a register file wired with too few
	 *      index bits, which otherwise only shows up under compiler
	 *      output that uses the high registers) ---- */

	li	x1,  0x11111111
	li	x8,  0x88888888
	li	x15, 0x15151515
	li	x16, 0x16161616
	li	x31, 0x31313131

	CHECK_EQ 150, x1,  0x11111111
	CHECK_EQ 151, x8,  0x88888888
	CHECK_EQ 152, x15, 0x15151515
	CHECK_EQ 153, x16, 0x16161616
	CHECK_EQ 154, x31, 0x31313131

	/* ---- fence must be a nop, not a fault ---- */

	fence
	/* fence.i, encoded by hand -- binutils wants -march=..._zifencei
	 * to accept the mnemonic, and the point here is the decode, not
	 * the assembler */
	.word	0x0000100f
	li	a0, 42
	CHECK_EQ 160, a0, 42

	TEST_PASS
