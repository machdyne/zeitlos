/*
 * zeitlos32 -- faults.
 *
 * Check ids 700-799.
 *
 * picorv32 in the configuration rtl/sysctl.v uses (CATCH_ILLINSN and
 * CATCH_MISALIGN both on, ENABLE_IRQ on) does not halt on an illegal
 * instruction or a misaligned access: it raises IRQ 1 or IRQ 2 and
 * lets software deal with it. Only if the relevant line is masked, or
 * the core is already inside a handler, does it stop and assert
 * `trap`.
 *
 * That distinction matters for this SOC. sw/bios/boot_picorv32.S
 * opens the mask completely, so on a running system a bad instruction
 * arrives at the kernel's interrupt handler rather than freezing the
 * board with LED_R lit -- and sw/bios/irq.c passes the bitmask
 * straight through to the kernel. Reproducing it is part of being
 * drop-in.
 */

#include "test.h"

#define r_type_insn(_f7, _rs2, _rs1, _f3, _rd, _opc) \
	.word (((_f7) << 25) | ((_rs2) << 20) | ((_rs1) << 15) | ((_f3) << 12) | ((_rd) << 7) | ((_opc) << 0))

#define maskirq(_rd, _rs)  r_type_insn(0b0000011, 0, _rs, 0b110, _rd, 0b0001011)
#define retirq()           r_type_insn(0b0000010, 0,   0, 0b000,   0, 0b0001011)
#define getq(_rd, _qs)     r_type_insn(0b0000000, 0, _qs, 0b100, _rd, 0b0001011)
#define setq(_qd, _rs)     r_type_insn(0b0000001, 0, _rs, 0b010, _qd, 0b0001011)

	.section .text
	.globl _start
_start:
	j	_test_main
	nop
	nop
	nop

	.org 0x10
irq_vec:
	setq(2, 1)					/* q2 = x1 */
	setq(3, 2)					/* q3 = x2 */

	la	x1, fault_log
	getq(2, 1)					/* x2 = q1, the delivered mask */
	sw	x2, 0(x1)
	lw	x2, 4(x1)
	addi	x2, x2, 1
	sw	x2, 4(x1)				/* count */

	/* Note there is no need to step q0 past the faulting instruction.
	 * The core retires the faulting instruction WITHOUT executing it
	 * and raises the interrupt at the following boundary, so q0
	 * already points at the next instruction. See the fault section
	 * of rtl/cpu/zeitlos32/zeitlos32.v -- this is the one place it
	 * deliberately differs from picorv32, which leaves the faulting
	 * instruction's own address in q0 and expects the handler to
	 * decide what to do about it. Nothing in sw/ relies on either
	 * behaviour: rtl/sysctl.v does not wire IRQ 1 or IRQ 2 to
	 * anything. */

	getq(1, 2)					/* x1 = q2 */
	getq(2, 3)					/* x2 = q3 */
	retirq()

	.org 0x100
_test_main:

	la	a0, fault_log
	sw	x0, 0(a0)
	sw	x0, 4(a0)

	/* open the mask, as sw/bios/boot_picorv32.S does at reset */
	li	a0, 0
	maskirq(0, 10)

	/* ---- an illegal instruction raises IRQ 1 ---- */

	.word	0x00000000			/* all zeroes: not a valid encoding */

	la	a0, fault_log
	lw	a1, 4(a0)
	CHECK_EQ 700, a1, 1
	lw	a1, 0(a0)
	CHECK_EQ 701, a1, 0x00000002	/* bit 1 */

	/* ---- ebreak raises IRQ 1 too ---- */

	la	a0, fault_log
	sw	x0, 4(a0)

	ebreak

	la	a0, fault_log
	lw	a1, 4(a0)
	CHECK_EQ 710, a1, 1
	lw	a1, 0(a0)
	CHECK_EQ 711, a1, 0x00000002

	/* ---- so does ecall ---- */

	la	a0, fault_log
	sw	x0, 4(a0)

	ecall

	la	a0, fault_log
	lw	a1, 4(a0)
	CHECK_EQ 715, a1, 1

	/* ---- a misaligned load raises IRQ 2 ---- */

	la	a0, fault_log
	sw	x0, 4(a0)

	la	a0, scratch
	addi	a0, a0, 1
	lw	a1, 0(a0)				/* word load, byte-offset address */

	la	a0, fault_log
	lw	a1, 4(a0)
	CHECK_EQ 720, a1, 1
	lw	a1, 0(a0)
	CHECK_EQ 721, a1, 0x00000004	/* bit 2 */

	/* ---- a misaligned halfword store raises IRQ 2 ---- */

	la	a0, fault_log
	sw	x0, 4(a0)

	la	a0, scratch
	addi	a0, a0, 1
	li	a1, 0x1234
	sh	a1, 0(a0)

	la	a0, fault_log
	lw	a1, 4(a0)
	CHECK_EQ 730, a1, 1

	/* ---- byte accesses are never misaligned ---- */

	la	a0, fault_log
	sw	x0, 4(a0)

	la	a0, scratch
	addi	a0, a0, 1
	li	a1, 0x77
	sb	a1, 0(a0)
	lbu	a2, 0(a0)
	CHECK_EQ 740, a2, 0x77

	la	a0, fault_log
	lw	a1, 4(a0)
	CHECK_Z 741, a1				/* no fault was raised */

	/* ---- a legal instruction the core might not know ----
	 *
	 * SLTI with every reserved bit set to something unusual is still
	 * SLTI; only funct7 is decoded strictly, and only where it means
	 * something. */

	li	a0, 5
	slti	a1, a0, 6
	CHECK_EQ 750, a1, 1

	li	a0, 0xffffffff
	maskirq(0, 10)

	TEST_PASS

/* ------------------------------------------------------------ */

	.data
	.balign 4
fault_log:
	.word 0			/* 0: last delivered bitmask */
	.word 0			/* 4: fault count */
	.word 0			/* 8: scratch */
	.balign 4
scratch:
	.fill 8, 4, 0
