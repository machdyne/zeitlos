/*
 * zeitlos32p -- the BTB must never change what a program does.
 *
 * Check ids 960-979.
 *
 * Every case trains an entry (a branch or jal run taken a few times),
 * then changes what the program does at that address: the instruction
 * is overwritten (as when another program is loaded where this one
 * was), or the branch goes the other way. Each time the code must do
 * what it now says, not what the BTB remembers. Every interrupt is
 * masked: a wrong-path instruction that faulted would halt the core.
 */

#include "test.h"

#define r_type_insn(_f7, _rs2, _rs1, _f3, _rd, _opc) \
	.word (((_f7) << 25) | ((_rs2) << 20) | ((_rs1) << 15) | ((_f3) << 12) | ((_rd) << 7) | ((_opc) << 0))
#define maskirq(_rd, _rs)  r_type_insn(0b0000011, 0, _rs, 0b110, _rd, 0b0001011)

/* jal x0 from \from to \to, as a word */
#define JAL0(from, to) .word ( \
	((((to) - (from)) >> 20) & 1) << 31 | \
	((((to) - (from)) >> 1) & 0x3ff) << 21 | \
	((((to) - (from)) >> 11) & 1) << 20 | \
	((((to) - (from)) >> 12) & 0xff) << 12 | 0x6f)

/* overwrite the word at \site with the word at \src. The core orders
 * fetches after stores no more than RISC-V does (fence.i): so read the
 * word back and jump through a register that depends on it -- a jalr
 * always refetches, after the load and so after the store. */
.macro PATCH site, src
	la	t0, \src
	lw	t0, 0(t0)
	la	t2, \site
	sw	t0, 0(t2)
	lw	t0, 0(t2)
	la	t2, .Lpatched_\@
	add	t2, t2, t0
	sub	t2, t2, t0
	jr	t2
.Lpatched_\@:
.endm

	.section .text
	.option norelax
	.globl _start
_start:
	j	_test_main
	nop
	nop
	nop
	.org 0x10
	TEST_FAIL 979				/* no interrupt may be raised */

_test_main:
	li	a0, -1
	maskirq(0, 10)

	/* -- A: a loop branch trained, then overwritten by an addi -- */
	li	s1, 3
1:	li	a0, 0
	li	a1, 5
	call	f_a
	CHECK_EQ 960, a0, 5
	addi	s1, s1, -1
	bnez	s1, 1b
	PATCH	site_a, alt_addi
	li	a0, 0
	li	a1, 5
	call	f_a
	CHECK_EQ 961, a0, 101			/* one pass, then +100, then out */

	/* -- B: a jal trained, then pointed elsewhere -- */
	li	s1, 3
1:	call	f_b
	CHECK_EQ 962, a0, 1
	addi	s1, s1, -1
	bnez	s1, 1b
	PATCH	site_b, alt_jal
	call	f_b
	CHECK_EQ 963, a0, 2

	/* -- C: a branch trained taken, then not taken, then taken -- */
	li	s1, 4
1:	li	a1, 0
	call	f_c
	CHECK_EQ 964, a0, 9
	addi	s1, s1, -1
	bnez	s1, 1b
	li	a1, 1
	call	f_c
	CHECK_EQ 965, a0, 7
	li	a1, 1
	call	f_c
	CHECK_EQ 966, a0, 7
	li	a1, 0
	call	f_c
	CHECK_EQ 967, a0, 9

	/* -- D: a trained branch overwritten by a multiply -- */
	li	s1, 3
1:	li	a0, 3
	li	a1, 4
	call	f_d
	addi	s1, s1, -1
	bnez	s1, 1b
	PATCH	site_d, alt_mul
	li	a0, 3
	li	a1, 4
	call	f_d
	CHECK_EQ 968, a0, 12			/* 3 * 4, then out */

	/* -- E: a trained branch overwritten by a return -- */
	li	s1, 3
1:	li	a0, 0
	li	a1, 3
	call	f_e
	CHECK_EQ 969, a0, 3
	addi	s1, s1, -1
	bnez	s1, 1b
	PATCH	site_e, alt_ret
	li	a0, 0
	li	a1, 3
	call	f_e
	CHECK_EQ 970, a0, 1			/* one pass, then the return */

	/* -- F: two loops at the same index (256 bytes apart), in turn -- */
	li	s1, 4
1:	li	a0, 0
	li	a1, 3
	call	f_f1
	li	a1, 2
	call	f_f2
	CHECK_EQ 971, a0, 23			/* 3 x 1 + 2 x 10 */
	addi	s1, s1, -1
	bnez	s1, 1b

	/* -- G: a trained branch overwritten by a load of what it skipped -- */
	li	s1, 3
1:	li	a1, 0
	call	f_g
	CHECK_EQ 972, a0, 5
	addi	s1, s1, -1
	bnez	s1, 1b
	PATCH	site_g, alt_lw
	li	a1, 0
	call	f_g
	CHECK_EQ 973, a0, 0x77			/* the load, then the fall-through */

	TEST_PASS

	/* the replacement words */
	.balign 4
alt_addi:
	addi	a0, a0, 100
alt_mul:
	mul	a0, a0, a1
alt_ret:
	ret
alt_lw:
	lw	a0, 0(s0)
alt_jal:
	JAL0(site_b, f_b2)

	.org 0x1000
f_a:
2:	addi	a0, a0, 1
	addi	a1, a1, -1
site_a:	bnez	a1, 2b
	ret

	.org 0x1100
f_b:
site_b:	j	f_b1
f_b1:	li	a0, 1
	ret
f_b2:	li	a0, 2
	ret

	.org 0x1200
f_c:
	beqz	a1, 3f
	li	a0, 7
	ret
3:	li	a0, 9
	ret

	.org 0x1300
f_d:
site_d:	bnez	a1, 4f
	ret
4:	li	a0, 99
	ret

	.org 0x1400
f_e:
2:	addi	a0, a0, 1
	addi	a1, a1, -1
site_e:	bnez	a1, 2b
	ret

	.org 0x1500
f_f1:
2:	addi	a0, a0, 1
	addi	a1, a1, -1
	bnez	a1, 2b
	ret
	.org 0x1600
f_f2:
2:	addi	a0, a0, 10
	addi	a1, a1, -1
	bnez	a1, 2b
	ret

	.org 0x1700
f_g:
	la	s0, word77
site_g:	beqz	a1, 5f
	ret
5:	li	a0, 5
	ret
	.balign 4
word77:	.word 0x77
