/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * ECDSA verification on NIST P-256 and P-384. See ecdsa.h.
 *
 * This began as a P-256-only file (p256.c). The curve is now a
 * parameter passed explicitly to every routine rather than a set of
 * compile-time constants -- deliberately explicit, because the
 * alternative considered was a macro that redefined the limb count in
 * terms of a variable that every function was assumed to have in
 * scope, and that is the kind of cleverness that works until someone
 * adds a function.
 */

#include <string.h>
#include <stdio.h>

#include "ecdsa.h"

// The hardware path is OPT-IN, enabled by -DEC_HW from the app
// Makefile for target builds only.
//
// Opt-out would be the wrong way round: a host test compiled without
// remembering to disable it dereferences 0x70000600 and segfaults,
// which is exactly what happened. A host build should not be able to
// reach MMIO by forgetting a flag.
#ifdef EC_HW
#include "../../common/zsoc.h"
#endif

// Widest curve here, in 32-bit limbs: 384 bits.
#define EC_MAX_LIMBS 12

typedef uint32_t fe[EC_MAX_LIMBS];      // a field element, Montgomery form

// -- domain parameters ---------------------------------------------
//
// GENERATED, not transcribed (see the generator in the commit that
// added P-384): twelve limbs of hex typed by hand is a transcription
// error waiting to happen, and a wrong digit here surfaces as
// "signature invalid" with nothing at all to point at. The generator
// also checks that each base point satisfies y^2 = x^3 - 3x + b.

static const uint32_t p256_P[8] = {
	0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000,
	0x00000000, 0x00000000, 0x00000001, 0xFFFFFFFF,
};
static const uint32_t p256_N[8] = {
	0xFC632551, 0xF3B9CAC2, 0xA7179E84, 0xBCE6FAAD,
	0xFFFFFFFF, 0xFFFFFFFF, 0x00000000, 0xFFFFFFFF,
};
static const uint32_t p256_B[8] = {
	0x27D2604B, 0x3BCE3C3E, 0xCC53B0F6, 0x651D06B0,
	0x769886BC, 0xB3EBBD55, 0xAA3A93E7, 0x5AC635D8,
};
static const uint32_t p256_GX[8] = {
	0xD898C296, 0xF4A13945, 0x2DEB33A0, 0x77037D81,
	0x63A440F2, 0xF8BCE6E5, 0xE12C4247, 0x6B17D1F2,
};
static const uint32_t p256_GY[8] = {
	0x37BF51F5, 0xCBB64068, 0x6B315ECE, 0x2BCE3357,
	0x7C0F9E16, 0x8EE7EB4A, 0xFE1A7F9B, 0x4FE342E2,
};

static const uint32_t p384_P[12] = {
	0xFFFFFFFF, 0x00000000, 0x00000000, 0xFFFFFFFF,
	0xFFFFFFFE, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
	0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
};
static const uint32_t p384_N[12] = {
	0xCCC52973, 0xECEC196A, 0x48B0A77A, 0x581A0DB2,
	0xF4372DDF, 0xC7634D81, 0xFFFFFFFF, 0xFFFFFFFF,
	0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
};
static const uint32_t p384_B[12] = {
	0xD3EC2AEF, 0x2A85C8ED, 0x8A2ED19D, 0xC656398D,
	0x5013875A, 0x0314088F, 0xFE814112, 0x181D9C6E,
	0xE3F82D19, 0x988E056B, 0xE23EE7E4, 0xB3312FA7,
};
static const uint32_t p384_GX[12] = {
	0x72760AB7, 0x3A545E38, 0xBF55296C, 0x5502F25D,
	0x82542A38, 0x59F741E0, 0x8BA79B98, 0x6E1D3B62,
	0xF320AD74, 0x8EB1C71E, 0xBE8B0537, 0xAA87CA22,
};
static const uint32_t p384_GY[12] = {
	0x90EA0E5F, 0x7A431D7C, 0x1D7E819D, 0x0A60B1CE,
	0xB5F0B8C0, 0xE9DA3113, 0x289A147C, 0xF8F41DBD,
	0x9292DC29, 0x5D9E98BF, 0x96262C6F, 0x3617DE4A,
};

// -- limb helpers ---------------------------------------------------

static uint32_t sub_n(uint32_t *r, const uint32_t *a, const uint32_t *b,
	uint16_t nl) {
	uint32_t borrow = 0;
	for (uint16_t i = 0; i < nl; i++) {
		uint64_t d = (uint64_t)a[i] - b[i] - borrow;
		r[i] = (uint32_t)d;
		borrow = (d >> 32) ? 1u : 0u;
	}
	return borrow;
}

static uint32_t add_n(uint32_t *r, const uint32_t *a, const uint32_t *b,
	uint16_t nl) {
	uint32_t carry = 0;
	for (uint16_t i = 0; i < nl; i++) {
		uint64_t s = (uint64_t)a[i] + b[i] + carry;
		r[i] = (uint32_t)s;
		carry = (uint32_t)(s >> 32);
	}
	return carry;
}

static int cmp_n(const uint32_t *a, const uint32_t *b, uint16_t nl) {
	for (int i = (int)nl - 1; i >= 0; i--)
		if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
	return 0;
}

static bool is_zero_n(const uint32_t *a, uint16_t nl) {
	uint32_t acc = 0;
	for (uint16_t i = 0; i < nl; i++) acc |= a[i];
	return acc == 0;
}

// -- Montgomery arithmetic ------------------------------------------
//
// The same CIOS reduction as bignum.c, duplicated rather than shared:
// the generic version works on a bn_t, which is 512 bytes wide
// because it must hold a 4096-bit RSA modulus. Point arithmetic needs
// a dozen temporaries and runs thousands of multiplications per
// signature, and carrying 40x the necessary width through all of it
// costs far more than the duplication saves.

typedef struct {
	const uint32_t  *m;
	uint32_t        n0inv;
	uint32_t        rr[EC_MAX_LIMBS];   // R^2 mod m
	bool            ready;
} mont_t;

typedef struct {
	uint16_t        nl;                 // limbs
	uint16_t        bytes;              // nl * 4
	const uint32_t  *p, *n, *b, *gx, *gy;
	mont_t          mp, mn;             // mod p (field), mod n (scalars)
	fe              b_mont;
	fe              gx_mont, gy_mont;

	// The active field representation.
	//
	// `hw` false: plain integers, fe_mul reduces with Solinas.
	// `hw` true:  Montgomery form, because that is what the block
	//             computes -- A*B*R^-1 mod p.
	//
	// `one` and `three` are the identity and the constant 3 IN THAT
	// REPRESENTATION, so the arithmetic above never has to ask which
	// it is in.
	bool            hw;
	fe              p_hw;               // p, zero-padded to the block width
	fe              one;
	fe              three;
	bool            ready;
} ec_curve_t;

static ec_curve_t curves[2];

static uint32_t n0inv_of(uint32_t m0) {
	uint32_t x = 1;
	for (int i = 0; i < 5; i++) x *= 2u - m0 * x;
	return (uint32_t)(0u - x);
}

static void mont_mul(uint32_t *out, const uint32_t *a, const uint32_t *b,
	const mont_t *mc, uint16_t nl) {

	uint32_t t[EC_MAX_LIMBS + 2];

	memset(t, 0, (size_t)(nl + 2) * sizeof(uint32_t));

	for (uint16_t i = 0; i < nl; i++) {

		uint32_t carry = 0, mi;

		for (uint16_t j = 0; j < nl; j++) {
			uint64_t pr = (uint64_t)a[i] * b[j] + t[j] + carry;
			t[j] = (uint32_t)pr;
			carry = (uint32_t)(pr >> 32);
		}
		{
			uint64_t s = (uint64_t)t[nl] + carry;
			t[nl] = (uint32_t)s;
			t[nl + 1] += (uint32_t)(s >> 32);
		}

		mi = t[0] * mc->n0inv;
		carry = 0;
		for (uint16_t j = 0; j < nl; j++) {
			uint64_t pr = (uint64_t)mi * mc->m[j] + t[j] + carry;
			t[j] = (uint32_t)pr;
			carry = (uint32_t)(pr >> 32);
		}
		{
			uint64_t s = (uint64_t)t[nl] + carry;
			t[nl] = (uint32_t)s;
			t[nl + 1] += (uint32_t)(s >> 32);
		}

		for (uint16_t j = 0; j <= nl; j++) t[j] = t[j + 1];
		t[nl + 1] = 0;

	}

	// The overflow limb must take part in the comparison: t can
	// exceed m while its low limbs alone look smaller.
	if (t[nl] || cmp_n(t, mc->m, nl) >= 0) sub_n(t, t, mc->m, nl);

	memcpy(out, t, (size_t)nl * sizeof(uint32_t));

}

static void mont_setup(mont_t *mc, const uint32_t *m, uint16_t nl) {

	uint32_t r[EC_MAX_LIMBS];

	if (mc->ready) return;

	mc->m = m;
	mc->n0inv = n0inv_of(m[0]);

	// R^2 mod m by doubling from 1, 2*32*nl times: 32*nl doublings
	// reach R, the rest reach R^2. Shifts, compares and subtracts
	// only -- no division, which is the point of Montgomery here.
	//
	// Deliberately does NOT assume the top bit of m is set. bignum.c
	// made that assumption and it was silently wrong for any modulus
	// that did not satisfy it.
	memset(r, 0, sizeof(r));
	r[0] = 1;
	for (uint32_t i = 0; i < 2u * 32u * nl; i++) {
		uint32_t carry = 0;
		for (uint16_t j = 0; j < nl; j++) {
			uint32_t hi = r[j] >> 31;
			r[j] = (r[j] << 1) | carry;
			carry = hi;
		}
		if (carry || cmp_n(r, m, nl) >= 0) sub_n(r, r, m, nl);
	}
	memcpy(mc->rr, r, (size_t)nl * sizeof(uint32_t));

	mc->ready = true;

}

static void to_mont(uint32_t *out, const uint32_t *a, const mont_t *mc,
	uint16_t nl) {
	mont_mul(out, a, mc->rr, mc, nl);
}

static void from_mont(uint32_t *out, const uint32_t *a, const mont_t *mc,
	uint16_t nl) {
	uint32_t one[EC_MAX_LIMBS];
	memset(one, 0, sizeof(one));
	one[0] = 1;
	mont_mul(out, a, one, mc, nl);
}

static void mod_add(uint32_t *r, const uint32_t *a, const uint32_t *b,
	const uint32_t *m, uint16_t nl) {
	uint32_t carry = add_n(r, a, b, nl);
	if (carry || cmp_n(r, m, nl) >= 0) sub_n(r, r, m, nl);
}

static void mod_sub(uint32_t *r, const uint32_t *a, const uint32_t *b,
	const uint32_t *m, uint16_t nl) {
	if (sub_n(r, a, b, nl)) add_n(r, r, m, nl);
}

// Inverse by Fermat: a^(m-2). Both p and n are prime for both curves,
// so this is valid for either, and it avoids a separate extended
// Euclid with its own edge cases -- at the cost of 32*nl squarings,
// which against the thousands the scalar multiplication needs is not
// where the time goes.
static void mont_inv(uint32_t *out, const uint32_t *a, const mont_t *mc,
	uint16_t nl) {

	uint32_t e[EC_MAX_LIMBS], two[EC_MAX_LIMBS];
	uint32_t r[EC_MAX_LIMBS], x[EC_MAX_LIMBS], one[EC_MAX_LIMBS];

	memset(two, 0, sizeof(two));
	two[0] = 2;
	sub_n(e, mc->m, two, nl);

	memset(one, 0, sizeof(one));
	one[0] = 1;
	to_mont(r, one, mc, nl);
	memcpy(x, a, (size_t)nl * sizeof(uint32_t));

	for (int bit = 32 * (int)nl - 1; bit >= 0; bit--) {
		mont_mul(r, r, r, mc, nl);
		if ((e[bit / 32] >> (bit % 32)) & 1) mont_mul(r, r, x, mc, nl);
	}

	memcpy(out, r, (size_t)nl * sizeof(uint32_t));

}

// -- hardware Montgomery multiplier, when the board has one ---------
//
// rtl/montmul.v at 0x7000_0600, advertised by Z_FEATURE2_MONTMUL.
//
// The field arithmetic below is written twice, and both versions are
// live: this one when the block is present, the Solinas software one
// otherwise. They are not interchangeable representations -- the
// block computes A*B*R^-1 mod N, so field elements are in MONTGOMERY
// form when it is in use and in normal form when it is not. The
// choice is made once, in curve_setup(), and everything downstream
// goes through fe_mul() without caring.
//
// Keeping both is worth the duplication: a bitstream without the
// block still verifies certificates, just slowly, and the software
// path is what the host tests exercise.

#ifdef EC_HW

static bool hw_checked, hw_ok;
static uint16_t hw_limbs;

#ifdef EC_HW_SIM

// A software model of rtl/montmul.v, for host tests.
//
// The hardware path cannot otherwise be tested off-target, and it has
// now produced TWO bugs that only a board could find: field elements
// left in the wrong representation, and a Montgomery constant
// computed at the curve's width instead of the block's. Both were
// caught by hw_self_test() on hardware, which is late.
//
// This models what the block does -- CIOS at HW_SIM_LIMBS words on a
// zero-padded modulus, including the final conditional subtract -- so
// tests/test_ecdsa.c can run the entire hardware path, padding and
// representation included, on a machine with no block in it.
//
// It is a MODEL, not the RTL: it proves the software around the block
// is right, not that the block is. rtl/tests/tb_montmul.v is what
// proves the block.

#define HW_SIM_LIMBS 12

static uint32_t sim_n[HW_SIM_LIMBS];
static uint32_t sim_n0inv;

static void sim_mul(uint32_t *out, const uint32_t *a, const uint32_t *b) {

	uint32_t t[HW_SIM_LIMBS + 2];
	int i, j;

	memset(t, 0, sizeof(t));

	for (i = 0; i < HW_SIM_LIMBS; i++) {

		uint64_t c = 0;
		uint32_t m;

		for (j = 0; j < HW_SIM_LIMBS; j++) {
			uint64_t s = (uint64_t)t[j] + (uint64_t)a[j] * b[i] + c;
			t[j] = (uint32_t)s;
			c = s >> 32;
		}
		{
			uint64_t s = (uint64_t)t[HW_SIM_LIMBS] + c;
			t[HW_SIM_LIMBS] = (uint32_t)s;
			t[HW_SIM_LIMBS + 1] = (uint32_t)(s >> 32);
		}

		m = t[0] * sim_n0inv;

		c = (uint64_t)t[0] + (uint64_t)m * sim_n[0];
		for (j = 1; j < HW_SIM_LIMBS; j++) {
			uint64_t s = (uint64_t)t[j] + (uint64_t)m * sim_n[j] + (c >> 32);
			t[j - 1] = (uint32_t)s;
			c = s;
		}
		{
			uint64_t s = (uint64_t)t[HW_SIM_LIMBS] + (c >> 32);
			t[HW_SIM_LIMBS - 1] = (uint32_t)s;
			t[HW_SIM_LIMBS] = t[HW_SIM_LIMBS + 1] + (uint32_t)(s >> 32);
			t[HW_SIM_LIMBS + 1] = 0;
		}

	}

	// The conditional subtract the block does in S_SUB/S_CPY.
	{
		uint32_t d[HW_SIM_LIMBS];
		uint32_t borrow = 0;
		for (j = 0; j < HW_SIM_LIMBS; j++) {
			uint64_t s = (uint64_t)t[j] - sim_n[j] - borrow;
			d[j] = (uint32_t)s;
			borrow = (s >> 32) ? 1u : 0u;
		}
		if (!(borrow && t[HW_SIM_LIMBS] == 0))
			memcpy(t, d, sizeof(d));
	}

	memcpy(out, t, HW_SIM_LIMBS * sizeof(uint32_t));

}

#else

static inline volatile uint32_t *hw_reg(uint32_t w) {
	return (volatile uint32_t *)(uintptr_t)(Z_MONTMUL_BASE + 4u * w);
}

#endif

// Is there a usable block, and is it wide enough for `nl` limbs?
//
// Both questions matter. The feature bit says the block was built;
// CONFIG says how wide. Handing a 12-limb modulus to an 8-limb block
// would be silently truncated -- a wrong answer rather than an error
// -- so the width is checked rather than assumed.
static bool hw_available(uint16_t nl) {

	if (!hw_checked) {
		hw_checked = true;
#ifdef EC_HW_SIM
		hw_limbs = HW_SIM_LIMBS;
		hw_ok = true;
#else
		hw_ok = false;
		if (z_soc_has_feature2(Z_FEATURE2_MONTMUL)) {
			if (*hw_reg(Z_MONTMUL_W_MAGIC) == Z_MONTMUL_MAGIC) {
				hw_limbs = (uint16_t)(*hw_reg(Z_MONTMUL_W_CONFIG) & 0xFF);
				hw_ok = (hw_limbs >= 8);
			}
		}
#endif
	}

	return hw_ok && nl <= hw_limbs;

}

// Operands are ZERO-PADDED to the block's full width.
//
// The width is a synthesis parameter now, not a runtime register --
// removing that one register removed every dynamic comparison against
// it, and was a large part of taking this block from 5,874 LUTs to
// under a thousand. A narrower modulus is simply the same integer
// with leading zeros, which Montgomery handles: R is larger, and the
// only requirements are R > N and gcd(R, N) = 1.
//
// So a P-256 curve on a 12-limb block uses R = 2^384. Nothing above
// here needs to know, because R never appears outside to_field() and
// from_field().
static void hw_pad(uint32_t *dst, const uint32_t *src, uint16_t nl) {
	uint16_t i;
	for (i = 0; i < nl; i++) dst[i] = src[i];
	for (; i < hw_limbs; i++) dst[i] = 0;
}

// Loads the modulus and n0inv. Once per curve, not once per multiply
// -- which is most of why the transfer cost is bearable.
// Which curve's modulus is currently loaded into the block.
//
// THE BLOCK HOLDS ONE MODULUS. Loading it once per curve at setup is
// not enough: a single certificate chain routinely uses both curves
// -- a P-256 leaf under P-384 intermediates is the common shape, and
// is what en.wikipedia.org serves -- so whichever curve was set up
// last would own the block and the other would compute against the
// wrong modulus.
//
// Silently, and with plausible-looking results. Found by the software
// model in tests, not by reasoning, and not by hardware: on hardware
// P-256 was falling back to software for an unrelated reason, which
// masked it entirely.
static const void *hw_cur;

static void hw_set_modulus(const uint32_t *n, uint32_t n0inv, uint16_t nl) {
	uint32_t p[EC_MAX_LIMBS];
	hw_pad(p, n, nl);
#ifdef EC_HW_SIM
	memcpy(sim_n, p, HW_SIM_LIMBS * sizeof(uint32_t));
	sim_n0inv = n0inv;
#else
	*hw_reg(Z_MONTMUL_W_N0INV) = n0inv;
	for (uint16_t i = 0; i < hw_limbs; i++) *hw_reg(Z_MONTMUL_W_N + i) = p[i];
#endif
}

// Makes sure the block holds this curve's modulus before using it.
//
// The comparison is a pointer, so switching costs one compare per
// multiply and a reload only when the curve actually changes -- once
// per chain link, not once per multiply.
static void hw_select(const ec_curve_t *cv) {
	if (hw_cur == (const void *)cv) return;
	hw_set_modulus(cv->p, cv->mp.n0inv, cv->nl);
	hw_cur = (const void *)cv;
}

static void hw_mul(uint32_t *out, const uint32_t *a, const uint32_t *b,
	uint16_t nl) {

	uint16_t i;
	uint32_t pa[EC_MAX_LIMBS], pb[EC_MAX_LIMBS];

	hw_pad(pa, a, nl);
	hw_pad(pb, b, nl);

#ifdef EC_HW_SIM
	{
		uint32_t r[EC_MAX_LIMBS];
		sim_mul(r, pa, pb);
		memcpy(out, r, (size_t)nl * sizeof(uint32_t));
		return;
	}
#else
	for (i = 0; i < hw_limbs; i++) *hw_reg(Z_MONTMUL_W_A + i) = pa[i];
	for (i = 0; i < hw_limbs; i++) *hw_reg(Z_MONTMUL_W_B + i) = pb[i];

	*hw_reg(Z_MONTMUL_W_CTRL) = 1;

	// 517 cycles for 12 limbs, so this spins a few dozen times at
	// most. No timeout: a block that never clears BUSY is a broken
	// bitstream, and every alternative here -- returning a wrong
	// answer, or silently falling back mid-verification -- is worse
	// than stopping.
	while (*hw_reg(Z_MONTMUL_W_CTRL) & 1) { }

	for (i = 0; i < nl; i++) out[i] = *hw_reg(Z_MONTMUL_W_R + i);
#endif

}

#endif

// -- fast reduction (Solinas) ---------------------------------------
//
// Both primes here are chosen so that reduction modulo p is a handful
// of ADDITIONS of rearranged words of the product, with no
// multiplication at all:
//
//   p256 = 2^256 - 2^224 + 2^192 + 2^96 - 1
//   p384 = 2^384 - 2^128 - 2^96 + 2^32 - 1
//
// That is the entire reason the NIST primes look the way they do, and
// it replaces the second pass of a Montgomery multiply -- the
// `mi * m[j]` loop, which is half of all the multiplying -- with
// about a dozen add-with-carry passes.
//
// It matters here more than it would elsewhere. This CPU has no data
// cache and SDRAM is ~13 cycles a word, so a Montgomery multiply
// spends most of its time stalled on memory rather than multiplying;
// removing half the multiply-accumulates removes half the memory
// traffic with it.
//
// The tables below are FIPS 186-4 D.2. They are transcribed, and a
// transcription error here produces wrong answers only for some
// inputs -- so tests/test_ecdsa.c checks them against products
// computed in Python, corner cases included.

// P-256, from FIPS 186-4 D.2.3, accumulated in ONE signed pass.
//
// The nine terms are added into a signed 64-bit accumulator and
// normalised once at the end, rather than each being conditionally
// reduced and added modulo p in turn. The first version did the
// latter -- a copy, a compare, a subtract and an add per term -- and
// measured SLOWER than the Montgomery reduction it replaced, which
// rather defeated the point. Nine terms of eight words is 72
// additions; the Montgomery pass it replaces is 64 multiply-adds,
// and on a CPU with no data cache the additions win only if they are
// not wrapped in three other passes each.
#define Z 0xFF
static const uint8_t p256_terms[9][8] = {
	{  0,  1,  2,  3,  4,  5,  6,  7 },		// s1
	{  Z,  Z,  Z, 11, 12, 13, 14, 15 },		// s2, doubled
	{  Z,  Z,  Z, 12, 13, 14, 15,  Z },		// s3, doubled
	{  8,  9, 10,  Z,  Z,  Z, 14, 15 },		// s4
	{  9, 10, 11, 13, 14, 15, 13,  8 },		// s5
	{ 11, 12, 13,  Z,  Z,  Z,  8, 10 },		// s6, subtracted
	{ 12, 13, 14, 15,  Z,  Z,  9, 11 },		// s7, subtracted
	{ 13, 14, 15,  8,  9, 10,  Z, 12 },		// s8, subtracted
	{ 14, 15,  Z,  9, 10, 11,  Z, 13 },		// s9, subtracted
};
static const int8_t p256_signs[9] = { 1, 2, 2, 1, 1, -1, -1, -1, -1 };
#undef Z

static void fe_reduce_p256(uint32_t *out, const uint32_t *c,
	const uint32_t *p) {

	int64_t acc[9];
	int64_t carry;
	int i, t;

	for (i = 0; i < 9; i++) acc[i] = 0;

	for (t = 0; t < 9; t++) {
		int sgn = p256_signs[t];
		for (i = 0; i < 8; i++) {
			uint8_t w = p256_terms[t][i];
			if (w == 0xFF) continue;
			acc[i] += (int64_t)sgn * (int64_t)c[w];
		}
	}

	carry = 0;
	for (i = 0; i < 8; i++) {
		int64_t v = acc[i] + carry;
		out[i] = (uint32_t)v;
		carry = v >> 32;				// arithmetic: keeps the sign
	}

	// carry is a small signed multiple of 2^256. Fold it with
	// 2^256 == 2^224 - 2^192 - 2^96 + 1 (mod p), then correct.
	while (carry > 0) {
		uint32_t f[8] = { 1, 0, 0, 0, 0, 0, 0, 0 };
		uint32_t g[8] = { 0, 0, 0, 0, 0, 0, 0, 1 };		// 2^224
		uint32_t h[8] = { 0, 0, 0, 0, 0, 0, 1, 0 };		// 2^192
		uint32_t k[8] = { 0, 0, 0, 1, 0, 0, 0, 0 };		// 2^96
		mod_add(out, out, f, p, 8);
		mod_add(out, out, g, p, 8);
		mod_sub(out, out, h, p, 8);
		mod_sub(out, out, k, p, 8);
		carry--;
	}
	while (carry < 0) {
		uint32_t f[8] = { 1, 0, 0, 0, 0, 0, 0, 0 };
		uint32_t g[8] = { 0, 0, 0, 0, 0, 0, 0, 1 };
		uint32_t h[8] = { 0, 0, 0, 0, 0, 0, 1, 0 };
		uint32_t k[8] = { 0, 0, 0, 1, 0, 0, 0, 0 };
		mod_sub(out, out, f, p, 8);
		mod_sub(out, out, g, p, 8);
		mod_add(out, out, h, p, 8);
		mod_add(out, out, k, p, 8);
		carry++;
	}

	while (cmp_n(out, p, 8) >= 0) sub_n(out, out, p, 8);

}

// P-384 reduces by FOLDING, not by a table.
//
// The prime says how directly:
//
//   p384 = 2^384 - 2^128 - 2^96 + 2^32 - 1
//   so   2^384 = 2^128 + 2^96 - 2^32 + 1   (mod p)
//
// which in 32-bit words is: word 12 folds onto words 4, 3, -1 and 0.
// Every high word c[12+j] therefore adds to words 4+j and 3+j and 0+j
// and subtracts from 1+j. The highest index that reaches is 4+11 =
// 15, so a second pass clears everything above word 11, and two
// passes always suffice.
//
// Derived rather than transcribed. FIPS 186-4 D.2.4 gives the same
// thing pre-expanded as ten s_i terms, and typing those in by hand
// got five of the ten wrong -- shifted or reversed -- passing three
// of thirty test products. The fold is read straight off the prime
// and cannot be got wrong in that way.
//
// P-256 keeps its table (above): its 2^224 term shifts so far that
// folding needs seven passes, where the pre-expanded form needs one.
static void fe_reduce_p384(uint32_t *out, const uint32_t *c,
	const uint32_t *p) {

	int64_t acc[24];
	int64_t carry;
	int i, j, pass;

	for (i = 0; i < 24; i++) acc[i] = (int64_t)c[i];

	// Two folds, then the carry out of word 11 folded once more. The
	// accumulator is SIGNED and deliberately not normalised between
	// passes: masking to 32 bits mid-fold re-creates high words and
	// the loop stops converging.
	for (pass = 0; pass < 3; pass++) {

		int64_t hi[12];
		bool any = false;

		for (j = 0; j < 12; j++) {
			hi[j] = acc[12 + j];
			acc[12 + j] = 0;
			if (hi[j]) any = true;
		}

		if (!any && pass) break;

		for (j = 0; j < 12; j++) {
			int64_t v = hi[j];
			if (!v) continue;
			acc[4 + j] += v;
			acc[3 + j] += v;
			acc[1 + j] -= v;
			acc[0 + j] += v;
		}

		// Propagate into word 12 so the next pass can fold it away.
		carry = 0;
		for (i = 0; i < 12; i++) {
			int64_t t = acc[i] + carry;
			acc[i] = (int64_t)(uint32_t)t;
			carry = t >> 32;			// arithmetic: keeps the sign
		}
		acc[12] += carry;

	}

	for (i = 0; i < 12; i++) out[i] = (uint32_t)acc[i];

	// Final correction into [0, p).
	while (cmp_n(out, p, 12) >= 0) sub_n(out, out, p, 12);

}

#undef Z

// Reduces a 2*nl-limb product into nl limbs, mod p.
static void fe_reduce(uint32_t *out, const uint32_t *c, const ec_curve_t *cv) {

	uint16_t nl = cv->nl;
	if (nl == 12) fe_reduce_p384(out, c, cv->p);
	else fe_reduce_p256(out, c, cv->p);

}

// Schoolbook multiply, then reduce. Replaces mont_mul for the FIELD;
// scalars mod n still use Montgomery, since n is not a Solinas prime.
static void fe_mul(uint32_t *out, const uint32_t *a, const uint32_t *b,
	const ec_curve_t *cv) {

	uint16_t nl = cv->nl;

#ifdef EC_HW
	// Montgomery form, in hardware. See hw_available().
	if (cv->hw) {
		hw_select(cv);
		hw_mul(out, a, b, nl);
		return;
	}
#endif

	uint32_t c[2 * EC_MAX_LIMBS];

	memset(c, 0, (size_t)(2 * nl) * sizeof(uint32_t));

	for (uint16_t i = 0; i < nl; i++) {
		uint32_t carry = 0;
		for (uint16_t j = 0; j < nl; j++) {
			uint64_t pr = (uint64_t)a[i] * b[j] + c[i + j] + carry;
			c[i + j] = (uint32_t)pr;
			carry = (uint32_t)(pr >> 32);
		}
		c[i + nl] = carry;
	}

	fe_reduce(out, c, cv);

}

// a^(p-2) mod p: the field inverse, by Fermat, on top of fe_mul.
//
// The scalars mod n keep using Montgomery (mont_inv above): n is not
// a Solinas prime and there is no fast reduction for it. Two inverses
// per verification, against thousands of multiplies, so it does not
// matter which they use.
static void fe_inv(uint32_t *out, const uint32_t *a, const ec_curve_t *cv) {

	uint16_t nl = cv->nl;
	uint32_t e[EC_MAX_LIMBS], two[EC_MAX_LIMBS], r[EC_MAX_LIMBS];
	uint32_t x[EC_MAX_LIMBS];

	memset(two, 0, sizeof(two));
	two[0] = 2;
	sub_n(e, cv->p, two, nl);

	// The identity in the ACTIVE representation, not a plain 1.
	// In Montgomery form the multiplicative identity is R mod p, and
	// seeding this with 1 would compute a^(p-2) * R^-(p-2) instead --
	// wrong, and wrong in a way that still produces a plausible
	// looking field element.
	memcpy(r, cv->one, (size_t)nl * sizeof(uint32_t));
	memcpy(x, a, (size_t)nl * sizeof(uint32_t));

	for (int bit = 32 * (int)nl - 1; bit >= 0; bit--) {
		fe_mul(r, r, r, cv);
		if ((e[bit / 32] >> (bit % 32)) & 1) fe_mul(r, r, x, cv);
	}

	memcpy(out, r, (size_t)nl * sizeof(uint32_t));

}

// -- the curve ------------------------------------------------------
//
// Jacobian coordinates: (X, Y, Z) represents (X/Z^2, Y/Z^3), with the
// point at infinity as Z == 0. Affine addition needs a modular
// inverse per operation and an inverse is 32*nl squarings; Jacobian
// costs a few multiplications instead and pays for exactly one
// inverse, at the end.

typedef struct { fe x, y, z; } pt_t;

static void pt_zero(pt_t *r) { memset(r, 0, sizeof(*r)); }

static bool pt_is_zero(const pt_t *a, uint16_t nl) {
	return is_zero_n(a->z, nl);
}

// Multiplies a plain integer into the active representation, and back.
// Both are identities when the field is not in Montgomery form.
static void to_field(uint32_t *out, const uint32_t *a, const ec_curve_t *cv) {
#ifdef EC_HW
	if (cv->hw) { hw_select(cv); hw_mul(out, a, cv->mp.rr, cv->nl); return; }
#endif
	memcpy(out, a, (size_t)cv->nl * sizeof(uint32_t));
}

static void from_field(uint32_t *out, const uint32_t *a,
	const ec_curve_t *cv) {
#ifdef EC_HW
	if (cv->hw) {
		uint32_t plain_one[EC_MAX_LIMBS];
		memset(plain_one, 0, sizeof(plain_one));
		plain_one[0] = 1;
		hw_select(cv);
		hw_mul(out, a, plain_one, cv->nl);
		return;
	}
#endif
	memcpy(out, a, (size_t)cv->nl * sizeof(uint32_t));
}

#ifdef EC_HW
// Does the block actually compute what it claims?
//
// Checked once, against the simplest product whose answer is known
// without doing the arithmetic: R * R * R^-1 == R mod p, which is
// to_field(1) twice over. If it does not hold, the block is not used
// at all and the software path takes over.
//
// This exists because the hardware path is the one thing here that
// CANNOT be host-tested -- tests/test_ecdsa.c runs on a machine with
// no block, so it exercises the software path only. A bitstream with
// a broken or half-wired accelerator would otherwise fail as invalid
// signatures on every site, which looks like a certificate problem.
static bool hw_self_test(ec_curve_t *cv) {

	uint32_t plain_one[EC_MAX_LIMBS], r1[EC_MAX_LIMBS], r2[EC_MAX_LIMBS];
	uint16_t nl = cv->nl;

	memset(plain_one, 0, sizeof(plain_one));
	plain_one[0] = 1;

	// r1 = 1 * RR * R^-1 = R mod p
	hw_mul(r1, plain_one, cv->mp.rr, nl);

	// r2 = r1 * 1 * R^-1 = 1
	hw_mul(r2, r1, plain_one, nl);

	if (memcmp(r2, plain_one, (size_t)nl * sizeof(uint32_t))) return false;

	// And a value that exercises carries rather than a lone bit:
	// (p-1) in Montgomery form, brought back out, must be (p-1).
	{
		uint32_t pm1[EC_MAX_LIMBS], t1[EC_MAX_LIMBS], t2[EC_MAX_LIMBS];
		uint32_t two[EC_MAX_LIMBS];

		memset(two, 0, sizeof(two));
		two[0] = 1;
		sub_n(pm1, cv->p, two, nl);

		hw_mul(t1, pm1, cv->mp.rr, nl);
		hw_mul(t2, t1, plain_one, nl);

		if (memcmp(t2, pm1, (size_t)nl * sizeof(uint32_t))) return false;
	}

	return true;

}
#endif

static void curve_setup(ec_curve_t *cv) {

	uint32_t one[EC_MAX_LIMBS];

	if (cv->ready) return;

	mont_setup(&cv->mn, cv->n, cv->nl);

	// The field: hardware Montgomery if the board has the block and
	// it passes its own self-test, otherwise software Solinas.
	cv->hw = false;
#ifdef EC_HW
	if (hw_available(cv->nl)) {

		// R^2 mod p, computed at the BLOCK's width, not the curve's.
		//
		// The block's limb count is fixed at synthesis, so a P-256
		// modulus is zero-padded and the Montgomery factor it uses is
		// R = 2^384, not 2^256. Computing R^2 at the curve's own
		// width gives the wrong constant, every conversion into the
		// field is then wrong, and every signature fails to verify.
		//
		// This was not caught by reasoning -- hw_self_test() caught
		// it on hardware, which is exactly why that check exists. The
		// P-384 curve happened to match the block width and worked;
		// P-256 did not and fell back to software, which is how it
		// announced itself.
		//
		// rr is still stored at the curve's width: it is a residue
		// mod p, so it fits, whatever R it was derived from. n0inv
		// depends only on p[0] and is unaffected.
		{
			mont_t wide;

			memset(&wide, 0, sizeof(wide));
			hw_pad(cv->p_hw, cv->p, cv->nl);
			mont_setup(&wide, cv->p_hw, hw_limbs);

			memcpy(cv->mp.rr, wide.rr,
				(size_t)cv->nl * sizeof(uint32_t));
			cv->mp.n0inv = wide.n0inv;
			cv->mp.m = cv->p_hw;
			cv->mp.ready = true;
		}

		hw_set_modulus(cv->p, cv->mp.n0inv, cv->nl);
		hw_cur = (const void *)cv;
		cv->hw = true;
		if (!hw_self_test(cv)) {
			printf("ecdsa: montmul block failed its self-test, "
				"using software\n");
			cv->hw = false;
		}
	}
#endif

	memset(one, 0, sizeof(one));
	one[0] = 1;

	// Constants, in whichever representation the field is using.
	memset(one, 0, sizeof(one));
	one[0] = 1;

	to_field(cv->b_mont, cv->b, cv);
	to_field(cv->gx_mont, cv->gx, cv);
	to_field(cv->gy_mont, cv->gy, cv);
	to_field(cv->one, one, cv);

	{
		uint32_t three[EC_MAX_LIMBS];
		memset(three, 0, sizeof(three));
		three[0] = 3;
		to_field(cv->three, three, cv);
	}

	cv->ready = true;

}

// Doubling, "dbl-2001-b". Uses a = -3, which both these curves have
// and which is most of why the NIST curves are faster than a general
// short Weierstrass curve.
static void pt_dbl(const ec_curve_t *cv, pt_t *r, const pt_t *a) {

	uint16_t nl = cv->nl;
	const uint32_t *P = cv->p;
	fe delta, gamma, beta, alpha, t1, t2;

	if (pt_is_zero(a, nl)) { pt_zero(r); return; }

	fe_mul(delta, a->z, a->z, cv);
	fe_mul(gamma, a->y, a->y, cv);
	fe_mul(beta, a->x, gamma, cv);

	mod_sub(t1, a->x, delta, P, nl);
	mod_add(t2, a->x, delta, P, nl);
	fe_mul(alpha, t1, t2, cv);
	mod_add(t1, alpha, alpha, P, nl);
	mod_add(alpha, t1, alpha, P, nl);           // 3(X-Z^2)(X+Z^2)

	fe_mul(t1, alpha, alpha, cv);
	mod_add(t2, beta, beta, P, nl);
	mod_add(t2, t2, t2, P, nl);                 // 4*beta
	mod_sub(t1, t1, t2, P, nl);
	mod_sub(t1, t1, t2, P, nl);                 // X' = alpha^2 - 8beta

	{
		fe yz;
		mod_add(yz, a->y, a->z, P, nl);
		fe_mul(yz, yz, yz, cv);
		mod_sub(yz, yz, gamma, P, nl);
		mod_sub(yz, yz, delta, P, nl);
		memcpy(r->z, yz, sizeof(fe));
	}

	{
		fe g2;
		mod_sub(t2, t2, t1, P, nl);             // 4beta - X'
		fe_mul(t2, alpha, t2, cv);
		fe_mul(g2, gamma, gamma, cv);
		mod_add(g2, g2, g2, P, nl);
		mod_add(g2, g2, g2, P, nl);
		mod_add(g2, g2, g2, P, nl);             // 8*gamma^2
		mod_sub(r->y, t2, g2, P, nl);
	}

	memcpy(r->x, t1, sizeof(fe));

}

// Addition, "add-2007-bl". The zero and equal-point cases are handled
// by explicit branches rather than by being complete: these are
// public points, so branching on them leaks nothing.
static void pt_add(const ec_curve_t *cv, pt_t *r, const pt_t *a,
	const pt_t *b) {

	uint16_t nl = cv->nl;
	const uint32_t *P = cv->p;
	fe z1z1, z2z2, u1, u2, s1, s2, h, i, j, rr, v, t;

	if (pt_is_zero(a, nl)) { *r = *b; return; }
	if (pt_is_zero(b, nl)) { *r = *a; return; }

	fe_mul(z1z1, a->z, a->z, cv);
	fe_mul(z2z2, b->z, b->z, cv);
	fe_mul(u1, a->x, z2z2, cv);
	fe_mul(u2, b->x, z1z1, cv);
	fe_mul(s1, a->y, b->z, cv);
	fe_mul(s1, s1, z2z2, cv);
	fe_mul(s2, b->y, a->z, cv);
	fe_mul(s2, s2, z1z1, cv);

	if (cmp_n(u1, u2, nl) == 0) {
		// Same x: either the same point, where this formula divides
		// by zero and doubling is required, or two points summing to
		// infinity.
		if (cmp_n(s1, s2, nl) == 0) { pt_dbl(cv, r, a); return; }
		pt_zero(r);
		return;
	}

	mod_sub(h, u2, u1, P, nl);
	mod_add(i, h, h, P, nl);
	fe_mul(i, i, i, cv);
	fe_mul(j, h, i, cv);
	mod_sub(rr, s2, s1, P, nl);
	mod_add(rr, rr, rr, P, nl);
	fe_mul(v, u1, i, cv);

	fe_mul(t, rr, rr, cv);
	mod_sub(t, t, j, P, nl);
	mod_sub(t, t, v, P, nl);
	mod_sub(t, t, v, P, nl);

	{
		fe y3;
		mod_sub(y3, v, t, P, nl);
		fe_mul(y3, rr, y3, cv);
		fe_mul(s1, s1, j, cv);
		mod_add(s1, s1, s1, P, nl);
		mod_sub(r->y, y3, s1, P, nl);
	}

	{
		fe z3;
		mod_add(z3, a->z, b->z, P, nl);
		fe_mul(z3, z3, z3, cv);
		mod_sub(z3, z3, z1z1, P, nl);
		mod_sub(z3, z3, z2z2, P, nl);
		fe_mul(r->z, z3, h, cv);
	}

	memcpy(r->x, t, sizeof(fe));

}

// u1*G + u2*Q by Shamir's trick: one pass over the bits, doubling
// once and adding at most once per bit, rather than two independent
// scalar multiplications. Roughly halves the work.
static void shamir(const ec_curve_t *cv, pt_t *r, const uint32_t *u1,
	const pt_t *g, const uint32_t *u2, const pt_t *q) {

	pt_t tbl[4];

	pt_zero(&tbl[0]);
	tbl[1] = *g;
	tbl[2] = *q;
	pt_add(cv, &tbl[3], g, q);

	pt_zero(r);

	for (int bit = 32 * (int)cv->nl - 1; bit >= 0; bit--) {
		int idx = (int)(((u1[bit / 32] >> (bit % 32)) & 1) |
			(((u2[bit / 32] >> (bit % 32)) & 1) << 1));
		pt_dbl(cv, r, r);
		if (idx) pt_add(cv, r, r, &tbl[idx]);
	}

}

// -- public ---------------------------------------------------------

static ec_curve_t *curve_for(ec_curve_id_t id) {

	ec_curve_t *cv;

	if (id == EC_CURVE_P256) {
		cv = &curves[0];
		if (!cv->nl) {
			cv->nl = 8; cv->bytes = 32;
			cv->p = p256_P; cv->n = p256_N; cv->b = p256_B;
			cv->gx = p256_GX; cv->gy = p256_GY;
		}
	} else if (id == EC_CURVE_P384) {
		cv = &curves[1];
		if (!cv->nl) {
			cv->nl = 12; cv->bytes = 48;
			cv->p = p384_P; cv->n = p384_N; cv->b = p384_B;
			cv->gx = p384_GX; cv->gy = p384_GY;
		}
	} else {
		return NULL;
	}

	curve_setup(cv);
	return cv;

}

uint32_t ec_point_len(ec_curve_id_t curve) {
	return curve == EC_CURVE_P384 ? 97u : 65u;
}

static void bytes_to_limbs(uint32_t *out, const uint8_t *in, uint16_t nl) {
	for (uint16_t i = 0; i < nl; i++) {
		int b = ((int)nl - 1 - (int)i) * 4;
		out[i] = ((uint32_t)in[b] << 24) | ((uint32_t)in[b + 1] << 16) |
			((uint32_t)in[b + 2] << 8) | (uint32_t)in[b + 3];
	}
}

bool ec_verify(ec_curve_id_t id, const uint8_t *point, uint32_t point_len,
	const uint8_t *r_bytes, uint32_t r_len,
	const uint8_t *s_bytes, uint32_t s_len,
	const uint8_t *hash, uint32_t hash_len) {

	ec_curve_t *cv = curve_for(id);
	uint16_t nl, nb;
	uint32_t r[EC_MAX_LIMBS], s[EC_MAX_LIMBS], e[EC_MAX_LIMBS];
	uint32_t u1[EC_MAX_LIMBS], u2[EC_MAX_LIMBS];
	uint8_t rb[EC_MAX_LIMBS * 4], sb[EC_MAX_LIMBS * 4], eb[EC_MAX_LIMBS * 4];
	pt_t Q, R;

	if (!cv) return false;
	nl = cv->nl;
	nb = cv->bytes;

	// Uncompressed points only -- see ecdsa.h.
	if (point_len != (uint32_t)nb * 2u + 1u) return false;
	if (point[0] != 0x04) return false;

	{
		uint32_t qx[EC_MAX_LIMBS], qy[EC_MAX_LIMBS], one[EC_MAX_LIMBS];

		bytes_to_limbs(qx, point + 1, nl);
		bytes_to_limbs(qy, point + 1 + nb, nl);

		if (cmp_n(qx, cv->p, nl) >= 0 || cmp_n(qy, cv->p, nl) >= 0)
			return false;

		memset(one, 0, sizeof(one));
		one[0] = 1;
		to_field(Q.x, qx, cv);
		to_field(Q.y, qy, cv);
		memcpy(Q.z, cv->one, (size_t)nl * sizeof(uint32_t));
	}

	// The point must be ON the curve: y^2 == x^3 - 3x + b.
	//
	// An attacker who gets a peer to compute with a point on a
	// DIFFERENT curve -- one whose order has small factors -- can
	// recover a private key from the results. There is no private key
	// on this side of a verification, so that attack does not apply
	// here; the check is three multiplications and the alternative is
	// relying on that argument staying true.
	{
		fe y2, x3, t;

		fe_mul(y2, Q.y, Q.y, cv);
		fe_mul(x3, Q.x, Q.x, cv);
		fe_mul(x3, x3, Q.x, cv);
		fe_mul(t, cv->three, Q.x, cv);
		mod_sub(x3, x3, t, cv->p, nl);
		mod_add(x3, x3, cv->b_mont, cv->p, nl);

		if (cmp_n(y2, x3, nl) != 0) return false;
	}

	// DER INTEGERs: a leading zero may be present and the value may
	// be shorter than the curve size.
	while (r_len && *r_bytes == 0) { r_bytes++; r_len--; }
	while (s_len && *s_bytes == 0) { s_bytes++; s_len--; }
	if (r_len == 0 || r_len > nb || s_len == 0 || s_len > nb) return false;

	memset(rb, 0, sizeof(rb));
	memset(sb, 0, sizeof(sb));
	memcpy(rb + (nb - r_len), r_bytes, r_len);
	memcpy(sb + (nb - s_len), s_bytes, s_len);

	bytes_to_limbs(r, rb, nl);
	bytes_to_limbs(s, sb, nl);

	// 0 < r < n and 0 < s < n. A zero or out-of-range component is
	// the classic forgery against an implementation that skips this.
	if (is_zero_n(r, nl) || cmp_n(r, cv->n, nl) >= 0) return false;
	if (is_zero_n(s, nl) || cmp_n(s, cv->n, nl) >= 0) return false;

	// e: the LEFTMOST bits of the hash, up to the order's bit length
	// (SEC1 4.1.4). Both orders here are a whole number of bytes, so
	// this is a byte-aligned truncation and needs no bit shifting.
	//
	// A longer hash is truncated (SHA-384 with P-256); a shorter one
	// is used whole, right-aligned (SHA-256 with P-384).
	memset(eb, 0, sizeof(eb));
	if (hash_len >= nb) memcpy(eb, hash, nb);
	else memcpy(eb + (nb - hash_len), hash, hash_len);

	bytes_to_limbs(e, eb, nl);
	if (cmp_n(e, cv->n, nl) >= 0) sub_n(e, e, cv->n, nl);

	// w = s^-1, u1 = e*w, u2 = r*w, all mod n.
	{
		uint32_t sm[EC_MAX_LIMBS], em[EC_MAX_LIMBS];
		uint32_t rm[EC_MAX_LIMBS], wm[EC_MAX_LIMBS];

		to_mont(sm, s, &cv->mn, nl);
		mont_inv(wm, sm, &cv->mn, nl);
		to_mont(em, e, &cv->mn, nl);
		to_mont(rm, r, &cv->mn, nl);

		// Montgomery, not Solinas: these are mod n, and n has no
		// fast reduction. Only the FIELD moved.
		mont_mul(u1, em, wm, &cv->mn, nl);
		mont_mul(u2, rm, wm, &cv->mn, nl);

		from_mont(u1, u1, &cv->mn, nl);
		from_mont(u2, u2, &cv->mn, nl);
	}

	{
		pt_t G;
		memcpy(G.x, cv->gx_mont, sizeof(fe));
		memcpy(G.y, cv->gy_mont, sizeof(fe));
		{
			uint32_t one[EC_MAX_LIMBS];
			memset(one, 0, sizeof(one));
			one[0] = 1;
			memcpy(G.z, cv->one, (size_t)nl * sizeof(uint32_t));
		}
		shamir(cv, &R, u1, &G, u2, &Q);
	}

	if (pt_is_zero(&R, nl)) return false;

	// Back to affine and compare x with r, modulo n. One inverse --
	// which is what the Jacobian representation bought.
	{
		fe zinv, z2, x_aff;
		uint32_t xa[EC_MAX_LIMBS];

		fe_inv(zinv, R.z, cv);
		fe_mul(z2, zinv, zinv, cv);
		fe_mul(x_aff, R.x, z2, cv);
		from_field(xa, x_aff, cv);

		// x is a field element and r is a scalar, so the comparison
		// is modulo n. p and n are close enough on both curves that
		// at most one subtraction is needed.
		if (cmp_n(xa, cv->n, nl) >= 0) sub_n(xa, xa, cv->n, nl);

		return cmp_n(xa, r, nl) == 0;
	}

}
