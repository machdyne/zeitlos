/*
 * zeitlos32p -- the MPU (rtl/mpu_split.v), against zeitlos32 + wb_mpu.
 *
 * Check ids 800-899.
 *
 * Run by tb_mpu_lockstep.v, which builds two machines: zeitlos32 with
 * rtl/mpu.v (wb_mpu, XLATE_ON_BUS 0) and zeitlos32p with
 * rtl/mpu_split.v and rtl/cache_split.v, both with the MTU base at
 * 0x4000_8000. Every register write and store must match, so the
 * fault registers, read back into registers after each scenario, are
 * compared too; the checks below pin down the parts whose values
 * follow from docs/mpu.md alone.
 *
 * Memory (the benches' 64KB, aliased into every nibble):
 *
 *   0x0000_0000   this "kernel", in BIOS RAM: privileged
 *   0x4000_1800   kernel text in main memory (below KTEXT), not the
 *                 gate: one instruction, jr s11
 *   0x4000_8000   the app's block (SIZE 4KB), run through the MTU
 *                 window at 0x8000_0000
 *   0x4000_c000   kernel data in main memory
 *   0x4000_c100   code outside the app's block: jr s10
 *   0x4000_d000   the grant window, for one scenario
 *
 * Nothing is touched through two aliases of the same word.
 */

#include "test.h"

#define r_type_insn(_f7, _rs2, _rs1, _f3, _rd, _opc) \
	.word (((_f7) << 25) | ((_rs2) << 20) | ((_rs1) << 15) | ((_f3) << 12) | ((_rd) << 7) | ((_opc) << 0))

#define maskirq(_rd, _rs)  r_type_insn(0b0000011, 0, _rs, 0b110, _rd, 0b0001011)
#define retirq()           r_type_insn(0b0000010, 0,   0, 0b000,   0, 0b0001011)
#define getq(_rd, _qs)     r_type_insn(0b0000000, 0, _qs, 0b100, _rd, 0b0001011)
#define setq(_qd, _rs)     r_type_insn(0b0000001, 0, _rs, 0b010, _qd, 0b0001011)

#define MPU      0x90000100
#define M_CTRL   0x00
#define M_KTEXT  0x04
#define M_GATE   0x08
#define M_SIZE   0x0c
#define M_MASK   0x10
#define M_FADDR  0x14
#define M_FPC    0x18
#define M_FINFO  0x1c
#define M_COUNT  0x20
#define M_INFO   0x24
#define M_GBASE  0x28
#define M_GLIMIT 0x2c

#define KWORD    0x4000c000
#define STUB4    0x40001800
#define OUTSIDE  0x4000c100
#define OUTSIDE2 0x4000c200
#define GRANT    0x4000d000
#define APP_V    0x80000000
#define OWN_V    0x80000800
#define OWN_P    0x40008800

	.section .text
	/* the app and the other fixed-address code are placed with .org:
	 * no linker relaxation, which would shrink the code before them */
	.option norelax
	.globl _start
_start:
	j	_test_main
	nop
	nop
	nop

	/* IRQ 1 (an illegal instruction: here, a blocked fetch). Log the
	 * delivered mask, count, and leave for the scenario's resume
	 * address rather than q0: q0 is the instruction after the bad
	 * one, which is in code the app had no business running. */
	.org 0x10
irq_vec:
	setq(2, 1)
	setq(3, 2)
	la	x1, irq_log
	getq(2, 1)
	sw	x2, 0(x1)
	lw	x2, 4(x1)
	addi	x2, x2, 1
	sw	x2, 4(x1)
	lw	x2, 8(x1)			/* resume */
	setq(0, 2)
	getq(1, 2)
	getq(2, 3)
	retirq()

	/* the gate: where apps come back. Privileged again (BIOS RAM) */
gate:
	sw	s7, 0(s7)			/* privileged at once: BIOS RAM is
						   kernel-only for the app before it */
	la	t0, irq_log
	lw	t0, 8(t0)
	jr	t0

/* run one app: \entry its virtual entry, \resume where to continue */
.macro RUN_APP entry, resume
	la	t0, irq_log
	la	t1, \resume
	sw	t1, 8(t0)
	li	t0, \entry
	jr	t0
.endm

/* read the fault registers into a0-a4 (compared write by write), then
 * clear them */
.macro DUMP_FAULTS
	li	t2, MPU
	lw	a0, M_FADDR(t2)
	lw	a1, M_FPC(t2)
	lw	a2, M_FINFO(t2)
	lw	a3, M_COUNT(t2)
	sw	x0, M_FINFO(t2)
	sw	x0, M_COUNT(t2)
	lw	a4, M_FINFO(t2)
	srli	a4, a4, 31			/* valid cleared (the rest stays) */
	CHECK_Z	899, a4
.endm

_test_main:
	li	a0, 0
	maskirq(0, 10)

	/* data cache on, posted writes (rtl/cache_split.v; zeitlos32's
	 * bench sinks the store) */
	li	t0, 0x70000110
	li	t1, 5
	sw	t1, 0(t0)

	/* the kernel data and the grant window */
	li	t0, KWORD
	li	t1, 0xaaaa0000
	sw	t1, 0(t0)
	li	t0, GRANT
	sw	x0, -4(t0)
	sw	x0, 0(t0)
	sw	x0, 28(t0)
	sw	x0, 32(t0)

	/* the MPU, as the kernel sets it up */
	li	t2, MPU
	lw	a0, M_INFO(t2)
	CHECK_EQ 800, a0, 0x3a500002
	li	t0, 0x40002000
	sw	t0, M_KTEXT(t2)
	la	t0, gate
	sw	t0, M_GATE(t2)
	li	t0, 0x1000
	sw	t0, M_SIZE(t2)
	li	t0, 0xf7ff			/* nibble 0xB off */
	sw	t0, M_MASK(t2)
	lw	a0, M_SIZE(t2)
	CHECK_EQ 801, a0, 0x1000

	la	s11, gate
	li	s10, STUB4
	li	s9, OUTSIDE
	la	s7, irq_log+12
	li	s8, KWORD
	la	s6, irq_log

	/* ---- 1: enforcing; app A's loads and stores, then a jump into
	 *         kernel text that is not the gate (reason 4) ---- */
	li	t2, MPU
	li	t0, 3
	sw	t0, M_CTRL(t2)
	RUN_APP APP_V, r1
r1:
	DUMP_FAULTS
	CHECK_EQ 810, a0, KWORD			/* the first: the kernel word store */
	CHECK_EQ 811, a2, 0x8102f000	/* valid, reason 1, store, sel 1111 */
	CHECK_EQ 812, a3, 6			/* 5 data, 1 fetch */
	li	t0, KWORD
	lw	a5, 0(t0)
	CHECK_EQ 813, a5, 0xaaaa0000	/* blocked: still the kernel's */
	li	t0, OWN_P
	lw	a5, 0(t0)
	CHECK_EQ 814, a5, 0x1111		/* the app's own store landed */
	li	t0, OWN_P+0x10			/* what app A read, logged in its block */
	lw	a5, 0(t0)
	CHECK_EQ 815, a5, 0xaaaa0000	/* the app could read kernel data */
	lw	a5, 4(t0)
	CHECK_Z	816, a5				/* ...not the masked nibble */
	lw	a5, 8(t0)
	CHECK_EQ 817, a5, 0x3a500002	/* ...and the MPU's INFO */
	la	t0, irq_log
	lw	a5, 4(t0)
	CHECK_EQ 818, a5, 1			/* one blocked fetch */

	/* ---- 2: enforcing; a jump outside the block (reason 1) ---- */
	li	t0, OWN_P
	sw	x0, 0(t0)
	RUN_APP APP_V+0x200, r2
r2:
	DUMP_FAULTS
	CHECK_EQ 820, a0, OUTSIDE
	CHECK_EQ 821, a2, 0x8100f000	/* valid, reason 1, fetch */
	CHECK_EQ 822, a3, 1

	/* ---- 3: enforcing; a jump into peripheral space: blocked at the
	 *         I port on zeitlos32p, by wb_mpu on zeitlos32 ---- */
	RUN_APP APP_V+0x300, r3
r3:
	DUMP_FAULTS
	CHECK_EQ 830, a0, 0x60000000
	CHECK_EQ 831, a2, 0x8100f000
	CHECK_EQ 832, a3, 1
	la	t0, irq_log
	lw	a5, 4(t0)
	CHECK_EQ 833, a5, 3

	/* ---- 4: report only: everything proceeds and is counted ---- */
	li	t2, MPU
	li	t0, 1
	sw	t0, M_CTRL(t2)
	li	t0, OWN_P
	sw	x0, 0(t0)
	RUN_APP APP_V, r4			/* A, through the stub, home by the gate */
r4:
	DUMP_FAULTS
	CHECK_EQ 840, a0, KWORD
	CHECK_EQ 841, a3, 6
	li	t0, KWORD
	lw	a5, 0(t0)
	CHECK_EQ 842, a5, 0x2222		/* report only: the store landed */
	li	t1, 0xaaaa0000
	sw	t1, 0(t0)
	RUN_APP APP_V+0x200, r5		/* B: outside, then the stub */
r5:
	DUMP_FAULTS
	CHECK_EQ 850, a0, OUTSIDE
	CHECK_EQ 851, a3, 2			/* outside (1), stub (4) */
	la	t0, irq_log
	lw	a5, 4(t0)
	CHECK_EQ 852, a5, 3			/* no blocked fetch since */

	/* ---- 5: enforcing, with a grant window ---- */
	li	t2, MPU
	li	t0, GRANT
	sw	t0, M_GBASE(t2)
	addi	t0, t0, 32
	sw	t0, M_GLIMIT(t2)
	li	t0, 3
	sw	t0, M_CTRL(t2)
	RUN_APP APP_V+0x400, r6
r6:
	DUMP_FAULTS
	CHECK_EQ 860, a0, GRANT-4
	CHECK_EQ 861, a3, 2
	li	t0, GRANT
	lw	a5, -4(t0)
	CHECK_Z	862, a5
	lw	a5, 0(t0)
	CHECK_EQ 863, a5, 0x5501
	lw	a5, 28(t0)
	CHECK_EQ 864, a5, 0x5502
	lw	a5, 32(t0)
	CHECK_Z	865, a5
	li	t2, MPU
	sw	x0, M_GBASE(t2)
	sw	x0, M_GLIMIT(t2)

	/* ---- 7: privilege changes with the instruction: app E's very
	 *         first instruction stores to kernel memory, and the
	 *         gate's very first stores to BIOS RAM ---- */
	li	t2, MPU
	sw	x0, 12(s6)
	li	t3, 0x3333
	RUN_APP APP_V+0x500, r8
r8:
	DUMP_FAULTS
	CHECK_EQ 880, a0, KWORD
	CHECK_EQ 881, a3, 1
	lw	a5, 0(s8)
	CHECK_EQ 882, a5, 0xaaaa0000
	lw	a5, 12(s6)
	CHECK_REG 883, a5, s7			/* the gate's store landed */

	/* ---- 8: a store at the end of the block, then falling out of
	 *         it: the store is older, so it is the fault latched, even
	 *         with the D port slow behind an uncached load ---- */
	RUN_APP APP_V+0xff0, r9
r9:
	DUMP_FAULTS
	CHECK_EQ 890, a0, KWORD
	CHECK_EQ 891, a2, 0x8102f000
	CHECK_EQ 892, a3, 2
	lw	a5, 0(s8)
	CHECK_EQ 893, a5, 0xaaaa0000

	/* ---- 9: report only; a store outside the block, reached by a
	 *         jump right behind a slow uncached load: the fetch report
	 *         waits for the load, and the store must still be made
	 *         exactly once ---- */
	li	t2, MPU
	li	t0, 1
	sw	t0, M_CTRL(t2)
	li	s5, OUTSIDE2
	li	t3, 0x4444
	RUN_APP APP_V+0x600, r10
r10:
	DUMP_FAULTS
	CHECK_EQ 895, a0, OUTSIDE2
	lw	a5, 4(s8)
	CHECK_EQ 896, a5, 0x4444
	sw	x0, 4(s8)

	/* ---- 6: disabled: app A does it all, nothing counted ---- */
	sw	x0, M_CTRL(t2)
	RUN_APP APP_V, r7
r7:
	DUMP_FAULTS
	CHECK_Z	870, a3
	li	t0, KWORD
	lw	a5, 0(t0)
	CHECK_EQ 871, a5, 0x2222

	TEST_PASS

	.align 4
irq_log:
	.word	0, 0, 0, 0		/* last mask, count, resume, - */

/* ------------------------------------------------------------------ */

	/* kernel text in main memory (0x4000_1800), not the gate */
	.org 0x1800
	jr	s11

	/* the app's block, entered through the MTU window. Position
	 * independent: relative branches only, addresses as constants. */
	.org 0x8000
app_a:
	li	t2, OWN_V
	li	t3, 0x1111
	sw	t3, 0(t2)			/* own block: fine */
	li	t2, KWORD
	li	t3, 0x2222
	sw	t3, 0(t2)			/* kernel memory: reason 1 */
	lw	t4, 0(t2)			/* reading it is allowed */
	sb	t3, 1(t2)			/* ...a byte of it is not (reason 1) */
	li	t2, 0xb0000000
	lw	t5, 0(t2)			/* masked nibble: reason 3, reads 0 */
	li	t2, 0x70000110
	sw	x0, 0(t2)			/* cache control: kernel only (2) */
	li	t2, MPU
	sw	x0, M_CTRL(t2)		/* the MPU itself: kernel only (2) */
	lw	t6, M_INFO(t2)		/* reading INFO is allowed */
	li	t2, OWN_V+0x10		/* log what was read, in the own block */
	sw	t4, 0(t2)
	sw	t5, 4(t2)
	sw	t6, 8(t2)
	jr	s10				/* kernel text, not the gate: reason 4 */
	/* fetched on zeitlos32p behind the jump, never run */
	sw	t3, 0(t2)
	sw	t3, 4(t2)
	li	t2, KWORD
	sw	t3, 4(t2)
	nop
	nop

	.org 0x8200
app_b:
	jr	s9				/* outside the block: reason 1 */
	nop
	nop

	.org 0x8300
app_c:
	li	t2, 0x60000000
	jr	t2				/* peripheral space */
	nop
	nop

	.org 0x8400
app_d:
	li	t2, GRANT
	li	t3, 0x5501
	sw	t3, -4(t2)			/* just below the grant: reason 1 */
	sw	t3, 0(t2)			/* the first word of it: fine */
	li	t3, 0x5502
	sw	t3, 28(t2)			/* the last: fine */
	sw	t3, 32(t2)			/* just above: reason 1 */
	jr	s11

	.org 0x8500
app_e:
	sw	t3, 0(s8)			/* the first instruction: unprivileged */
	jr	s11

	.org 0x8600
app_g:
	lw	t4, 0(s6)			/* uncached (BIOS RAM), slow */
	jr	s5				/* to OUTSIDE2, while it is in flight */

	.org 0x8ff0
app_f:
	lw	t4, 0(s6)			/* uncached (BIOS RAM), slow */
	lw	t4, 4(s6)
	nop
	sw	t3, 0(s8)			/* kernel memory, the block's last word */
	/* 0x8000_1000: outside the block -- a fetch violation */

	/* code outside the app's block (0x4000_c100) */
	.org 0xc100
	jr	s10

	.org 0xc200
	sw	t3, 4(s8)			/* outside the block, and a store */
	jr	s11
