# picorv32's rotates (ENABLE_ROT): ror, rol, rori against shifts and ORs
# computed by the base ISA, many values and every amount. A mismatch
# stores its index at 0x400 and stops; all equal stores -1. tb_rot.v.
	.section .text
	.globl _start
_start:
	li   s0, 0x400          # the result
	li   s1, 0              # mismatches
	li   s2, 0x9E3779B9     # the value, stepped
	li   s3, 0              # cases
outer:
	li   s4, 0              # the amount, 0..31 (and 32..63: only its low 5 bits count)
inner:
	# ror s2, s4 by hand: (x >> n) | (x << (32 - n)), n = s4 & 31
	andi t0, s4, 31
	srl  t1, s2, t0
	neg  t2, t0
	andi t2, t2, 31
	sll  t3, s2, t2
	beqz t0, 1f
	or   t1, t1, t3
1:	ror  t4, s2, s4
	bne  t4, t1, bad
	# rol: (x << n) | (x >> (32 - n))
	sll  t1, s2, t0
	srl  t3, s2, t2
	beqz t0, 2f
	or   t1, t1, t3
2:	rol  t4, s2, s4
	bne  t4, t1, bad
	addi s3, s3, 1
	addi s4, s4, 1
	li   t5, 64
	blt  s4, t5, inner
	# the next value: x * 5 + 1
	slli t0, s2, 2
	add  s2, s2, t0
	addi s2, s2, 1
	li   t5, 2048
	blt  s3, t5, outer
	# rori, each amount ChaCha20 and SHA-256 use, on one value
	li   s2, 0x12345678
	rori t4, s2, 16
	li   t1, 0x56781234
	bne  t4, t1, bad
	rori t4, s2, 20
	li   t1, 0x45678123
	bne  t4, t1, bad
	rori t4, s2, 24
	li   t1, 0x34567812
	bne  t4, t1, bad
	rori t4, s2, 25
	li   t1, 0x1A2B3C09
	bne  t4, t1, bad
	rori t4, s2, 0
	bne  t4, s2, bad
	rori t4, s2, 31
	li   t1, 0x2468ACF0
	bne  t4, t1, bad
	# x0 as the destination stays zero
	ror  zero, s2, s4
	bnez zero, bad
	li   t0, -1
	sw   t0, 0(s0)
	ebreak
bad:
	sw   s3, 0(s0)
	ebreak
