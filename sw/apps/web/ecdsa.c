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
#if defined(EC_HW) && !defined(EC_HW_SIM)
#include "../../common/zeitlos.h"		// z_getpid(), z_proc_wait(): the claim
#endif
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

// -- inverses --
//
// Binary extended Euclid (HAC 14.61, the form for an odd modulus): only
// shifts and subtractions, ~2 steps a bit. It replaced inversion by
// Fermat (a^(m-2): 32*nl squarings and as many multiplies again) in
// 2026, once the scalar multiplication moved onto montmul's register
// file: the inverse mod n, untouched, had become ~80% of a P-384 check
// -- ~840 ms of 1,082 on the board. The comment that stood here said,
// rightly at the time, that against the thousands of multiplies the
// scalar multiplication needed the inverse was "not where the time
// goes". Making the thousands fast made it where the time went.
//
// VARIABLE TIME: how many steps it takes depends on the value. Every
// value inverted here is public -- from a signature, a hash, a point
// being checked -- and nothing secret may ever be passed to it.
//
// Its edge cases are the reason Fermat was chosen originally, and they
// are why the old exponentiation is kept below (under EC_TEST_HOOKS) as
// the reference ec_test_inverses() holds this to: random values and the
// edges -- 1, 2, m-1, m-2, (m+1)/2, powers of two, all-ones patterns --
// on both curves' p and n.

static bool is_one_n(const uint32_t *a, uint16_t nl) {
	if (a[0] != 1) return false;
	for (uint16_t i = 1; i < nl; i++) if (a[i]) return false;
	return true;
}

// x >>= 1, `top` shifted in at the most significant bit.
static void shr1_n(uint32_t *x, uint32_t top, uint16_t nl) {
	for (uint16_t i = 0; i < nl; i++) {
		uint32_t hi = (i + 1 < nl) ? (x[i + 1] & 1u) : top;
		x[i] = (x[i] >> 1) | (hi << 31);
	}
}

// x/2 mod m, for 0 <= x < m, m odd: (x + m)/2 when x is odd. x + m can
// carry out of nl limbs; that carry is the bit shifted back in.
static void half_mod(uint32_t *x, const uint32_t *m, uint16_t nl) {
	if (x[0] & 1u) {
		uint32_t c = add_n(x, x, m, nl);
		shr1_n(x, c, nl);
	} else shr1_n(x, 0, nl);
}

// out = a^-1 mod m: m odd, 0 < a < m, gcd(a, m) = 1 -- which p and n,
// both prime, and every value below them but 0, satisfy. Given 0 (or a
// shared factor) it returns 0 rather than looping forever.
static void inv_mod(uint32_t *out, const uint32_t *a, const uint32_t *m, uint16_t nl) {
	uint32_t u[EC_MAX_LIMBS], v[EC_MAX_LIMBS], x1[EC_MAX_LIMBS], x2[EC_MAX_LIMBS];
	memcpy(u, a, (size_t)nl * sizeof(uint32_t));
	memcpy(v, m, (size_t)nl * sizeof(uint32_t));
	memset(x1, 0, sizeof(x1)); x1[0] = 1;
	memset(x2, 0, sizeof(x2));
	while (!is_one_n(u, nl) && !is_one_n(v, nl)) {
		if (is_zero_n(u, nl) || is_zero_n(v, nl)) { memset(out, 0, (size_t)nl * sizeof(uint32_t)); return; }
		while (!(u[0] & 1u)) { shr1_n(u, 0, nl); half_mod(x1, m, nl); }
		while (!(v[0] & 1u)) { shr1_n(v, 0, nl); half_mod(x2, m, nl); }
		if (cmp_n(u, v, nl) >= 0) { sub_n(u, u, v, nl); mod_sub(x1, x1, x2, m, nl); }
		else { sub_n(v, v, u, nl); mod_sub(x2, x2, x1, m, nl); }
	}
	memcpy(out, is_one_n(u, nl) ? x1 : x2, (size_t)nl * sizeof(uint32_t));
}

// The inverse of a Montgomery-form value, in Montgomery form: out of it,
// inverted, back in -- two multiplies around inv_mod().
static void mont_inv(uint32_t *out, const uint32_t *a, const mont_t *mc,
	uint16_t nl) {
	uint32_t x[EC_MAX_LIMBS];
	from_mont(x, a, mc, nl);
	inv_mod(x, x, mc->m, nl);
	to_mont(out, x, mc, nl);
}

#ifdef EC_TEST_HOOKS
// The old inverse, by Fermat -- kept only as the reference for tests.
static void mont_inv_fermat(uint32_t *out, const uint32_t *a, const mont_t *mc,
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
#endif

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
// Set when the block could not be had (another process held it too
// long): from then on this process uses software field arithmetic.
static bool hw_forced_off;
static uint16_t hw_limbs;
static uint16_t hw_nregs;		// the register file's size: CONFIG[15:8]; 0 = none

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

	if (hw_forced_off) return false;

	if (!hw_checked) {
		hw_checked = true;
#ifdef EC_HW_SIM
		hw_limbs = HW_SIM_LIMBS;
		hw_nregs = 16;
		hw_ok = true;
#else
		hw_ok = false;
		if (z_soc_has_feature2(Z_FEATURE2_MONTMUL)) {
			if (*hw_reg(Z_MONTMUL_W_MAGIC) == Z_MONTMUL_MAGIC) {
				hw_limbs = (uint16_t)(*hw_reg(Z_MONTMUL_W_CONFIG) & 0xFF);
				hw_nregs = (uint16_t)((*hw_reg(Z_MONTMUL_W_CONFIG) >> 8) & 0xFF);
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
static void to_field(uint32_t *out, const uint32_t *a, const ec_curve_t *cv);
static void from_field(uint32_t *out, const uint32_t *a, const ec_curve_t *cv);

// The field inverse, in the active representation: out of it, inv_mod()
// (above -- public values only), back in.
static void fe_inv(uint32_t *out, const uint32_t *a, const ec_curve_t *cv) {
	uint32_t x[EC_MAX_LIMBS];
	from_field(x, a, cv);
	inv_mod(x, x, cv->p, cv->nl);
	to_field(out, x, cv);
}

#ifdef EC_TEST_HOOKS
// The old field inverse, by Fermat -- the reference for tests.
static void fe_inv_fermat(uint32_t *out, const uint32_t *a, const ec_curve_t *cv) {

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
#endif

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

// -- the register file (rtl/montmul.v with REGFILE; docs/montmul.md) --
//
// The classic path above moves both operands in and the result out for
// every multiply: ~3,450 cycles, ~700 of them the block's own work. With
// the register file the double-scalar multiplication below keeps its
// running point INSIDE the block: sixteen registers of the block's
// width, one-word MUL / ADD / SUB commands, and only the table point of
// each addition loaded. Measured on the board a MUL command is ~776
// cycles and an ADD ~190.
//
// It only ever REPLACES shamir() when it can finish: an addition whose
// two points share an x coordinate (where the formula would divide by
// zero), or a table point at infinity, makes rf_shamir() give up and
// the classic path answer. For honest signatures that never happens;
// an attacker can arrange it, and then gets the classic path's answer.

// Which path verifications took: for the tests.
uint32_t ec_stat_rf, ec_stat_rf_bail;
int ec_rf_disable;			// tests: force the classic path

#ifdef EC_HW
#define RF_REGS 16

#ifdef EC_HW_SIM
static uint32_t sim_rf[RF_REGS][HW_SIM_LIMBS];
static uint32_t sim_sel;

static void rf_load(int r, const uint32_t *v, uint16_t nl) {
	for (int i = 0; i < HW_SIM_LIMBS; i++) sim_rf[r][i] = i < nl ? v[i] : 0;
}
static void rf_fetch(int r, uint32_t *v, uint16_t nl) {
	memcpy(v, sim_rf[r], (size_t)nl * sizeof(uint32_t));
}
// The block's ADD and SUB, at its full width, modulo the loaded N.
static void rf_op(uint32_t op, int d, int a, int b) {
	uint32_t t[HW_SIM_LIMBS];
	uint64_t c = 0;
	int i;
	(void)sim_sel;
	if (op == Z_MONTMUL_OP_MUL) { sim_mul(t, sim_rf[a], sim_rf[b]); memcpy(sim_rf[d], t, sizeof(t)); return; }
	if (op == Z_MONTMUL_OP_ADD) {
		for (i = 0; i < HW_SIM_LIMBS; i++) { c += (uint64_t)sim_rf[a][i] + sim_rf[b][i]; t[i] = (uint32_t)c; c >>= 32; }
		{
			uint32_t dd[HW_SIM_LIMBS], borrow = 0;
			for (i = 0; i < HW_SIM_LIMBS; i++) {
				uint64_t s = (uint64_t)t[i] - sim_n[i] - borrow;
				dd[i] = (uint32_t)s; borrow = (s >> 32) ? 1u : 0u;
			}
			if (!(borrow && c == 0)) memcpy(t, dd, sizeof(dd));
		}
	} else {
		uint32_t borrow = 0;
		for (i = 0; i < HW_SIM_LIMBS; i++) {
			uint64_t s = (uint64_t)sim_rf[a][i] - sim_rf[b][i] - borrow;
			t[i] = (uint32_t)s; borrow = (s >> 32) ? 1u : 0u;
		}
		if (borrow) {
			for (i = 0; i < HW_SIM_LIMBS; i++) { c += (uint64_t)t[i] + sim_n[i]; t[i] = (uint32_t)c; c >>= 32; }
		}
	}
	memcpy(sim_rf[d], t, sizeof(t));
}
#else
static void rf_load(int r, const uint32_t *v, uint16_t nl) {
	*hw_reg(Z_MONTMUL_W_RSEL) = (uint32_t)r << 4;
	for (uint16_t i = 0; i < hw_limbs; i++) *hw_reg(Z_MONTMUL_W_RDATA) = i < nl ? v[i] : 0;
}
static void rf_fetch(int r, uint32_t *v, uint16_t nl) {
	*hw_reg(Z_MONTMUL_W_RSEL) = (uint32_t)r << 4;
	for (uint16_t i = 0; i < nl; i++) v[i] = *hw_reg(Z_MONTMUL_W_RDATA);
}
static void rf_op(uint32_t op, int d, int a, int b) {
	*hw_reg(Z_MONTMUL_W_CMD) = Z_MONTMUL_CMD(op, d, a, b);
	while (*hw_reg(Z_MONTMUL_W_CMD) & 1u) { }
}
#endif

#define MUL(d, a, b) rf_op(Z_MONTMUL_OP_MUL, (d), (a), (b))
#define ADD(d, a, b) rf_op(Z_MONTMUL_OP_ADD, (d), (a), (b))
#define SUB(d, a, b) rf_op(Z_MONTMUL_OP_SUB, (d), (a), (b))

// Registers are named by the formulas as they go: a tiny allocator,
// and results renamed rather than copied (the block has no copy).
static uint16_t rf_used;
static int rf_get(void) {
	for (int i = 0; i < RF_REGS; i++)
		if (!(rf_used & (1u << i))) { rf_used |= (uint16_t)(1u << i); return i; }
	return 0;			// cannot happen: the formulas below need at most 12
}
static void rf_put(int r) { rf_used &= (uint16_t)~(1u << r); }

// (X, Y, Z) <- 2 (X, Y, Z): dbl-2001-b, a = -3, as pt_dbl().
static void rf_dbl(int *X, int *Y, int *Z) {
	int d = rf_get(), g = rf_get(), b = rf_get(), t1 = rf_get(), t2 = rf_get(), al = rf_get();
	int x3 = rf_get(), y3, z3;
	MUL(d, *Z, *Z);			// delta = Z^2
	MUL(g, *Y, *Y);			// gamma = Y^2
	MUL(b, *X, g);			// beta = X gamma
	SUB(t1, *X, d);
	ADD(t2, *X, d);
	MUL(al, t1, t2);
	ADD(t1, al, al);
	ADD(al, t1, al);		// alpha = 3 (X - delta)(X + delta)
	MUL(x3, al, al);
	ADD(t2, b, b);
	ADD(t2, t2, t2);		// 4 beta
	SUB(x3, x3, t2);
	SUB(x3, x3, t2);		// X' = alpha^2 - 8 beta
	z3 = rf_get();
	ADD(z3, *Y, *Z);
	MUL(z3, z3, z3);
	SUB(z3, z3, g);
	SUB(z3, z3, d);			// Z' = (Y + Z)^2 - gamma - delta
	SUB(t2, t2, x3);
	MUL(t2, al, t2);		// alpha (4 beta - X')
	MUL(t1, g, g);
	ADD(t1, t1, t1);
	ADD(t1, t1, t1);
	ADD(t1, t1, t1);		// 8 gamma^2
	y3 = rf_get();
	SUB(y3, t2, t1);		// Y'
	rf_put(d); rf_put(g); rf_put(b); rf_put(t1); rf_put(t2); rf_put(al);
	rf_put(*X); rf_put(*Y); rf_put(*Z);
	*X = x3; *Y = y3; *Z = z3;
}

static bool rf_is_zero(int r, uint16_t nl) {
	uint32_t v[EC_MAX_LIMBS];
	rf_fetch(r, v, nl);
	return is_zero_n(v, nl);
}

// (X, Y, Z) <- (X, Y, Z) + (x2, y2, 1): madd-2007-bl. False if the two
// points share an x coordinate -- H = 0, where the formula would divide
// by zero -- and nothing has changed but x2 and y2 being used up.
static bool rf_madd(int *X, int *Y, int *Z, int x2, int y2, uint16_t nl) {
	int z1z1 = rf_get(), u2 = rf_get(), s2, h, hh, i, j, r, v, x3, y3, t, z3;
	MUL(z1z1, *Z, *Z);
	MUL(u2, x2, z1z1);
	rf_put(x2);
	s2 = rf_get();
	MUL(s2, y2, *Z);
	rf_put(y2);
	MUL(s2, s2, z1z1);		// S2 = y2 Z^3
	h = rf_get();
	SUB(h, u2, *X);			// H = U2 - X
	rf_put(u2);
	if (rf_is_zero(h, nl)) {
		rf_put(z1z1); rf_put(s2); rf_put(h);
		return false;
	}
	hh = rf_get(); MUL(hh, h, h);
	i = rf_get(); ADD(i, hh, hh); ADD(i, i, i);		// I = 4 HH
	j = rf_get(); MUL(j, h, i);				// J = H I
	r = rf_get(); SUB(r, s2, *Y); ADD(r, r, r);		// r = 2 (S2 - Y)
	rf_put(s2);
	v = rf_get(); MUL(v, *X, i);				// V = X I
	rf_put(i);
	x3 = rf_get();
	MUL(x3, r, r); SUB(x3, x3, j); SUB(x3, x3, v); SUB(x3, x3, v);
	y3 = rf_get();
	SUB(y3, v, x3); MUL(y3, r, y3);
	rf_put(v); rf_put(r);
	t = rf_get(); MUL(t, *Y, j); ADD(t, t, t);
	SUB(y3, y3, t);						// Y3 = r (V - X3) - 2 Y J
	rf_put(t); rf_put(j);
	z3 = rf_get();
	ADD(z3, *Z, h); MUL(z3, z3, z3); SUB(z3, z3, z1z1); SUB(z3, z3, hh);
	rf_put(z1z1); rf_put(hh); rf_put(h);
	rf_put(*X); rf_put(*Y); rf_put(*Z);
	*X = x3; *Y = y3; *Z = z3;
	return true;
}

// dst <- src^(p-2) = src^-1, in the Montgomery domain: left to right,
// the top bit's "acc = src" folded into the first squaring.
static int rf_inv(int src, const ec_curve_t *cv) {
	uint32_t e[EC_MAX_LIMBS], two[EC_MAX_LIMBS];
	uint16_t nl = cv->nl;
	int acc = rf_get(), top, bit;
	memset(two, 0, sizeof(two));
	two[0] = 2;
	sub_n(e, cv->p, two, nl);
	for (top = 32 * nl - 1; top > 0 && !((e[top / 32] >> (top % 32)) & 1); top--) ;
	MUL(acc, src, src);
	if ((e[(top - 1) / 32] >> ((top - 1) % 32)) & 1) MUL(acc, acc, src);
	for (bit = top - 2; bit >= 0; bit--) {
		MUL(acc, acc, acc);
		if ((e[bit / 32] >> (bit % 32)) & 1) MUL(acc, acc, src);
	}
	return acc;
}

// u1 G + u2 Q, the result's AFFINE x (Montgomery form) in r->x and
// r->z = one -- so the caller skips its own inversion. False: the
// classic shamir() must answer.
static bool rf_shamir(const ec_curve_t *cv, pt_t *out, const uint32_t *u1,
	const pt_t *g, const uint32_t *u2, const pt_t *q) {

	uint16_t nl = cv->nl;
	fe tx[4], ty[4];
	pt_t t3;
	int X = -1, Y = -1, Z = -1;
	bool inf = true;

	// The table, affine: G, Q (both have z = one), and G + Q made so.
	pt_add(cv, &t3, g, q);
	if (pt_is_zero(&t3, nl)) return false;			// Q = -G
	hw_select(cv);
	rf_used = 0;
	{
		int z = rf_get(), zi, z2, z3, x, y;
		rf_load(z, t3.z, nl);
		zi = rf_inv(z, cv);
		rf_put(z);
		z2 = rf_get(); MUL(z2, zi, zi);
		z3 = rf_get(); MUL(z3, z2, zi);
		x = rf_get(); rf_load(x, t3.x, nl); MUL(x, x, z2);
		y = rf_get(); rf_load(y, t3.y, nl); MUL(y, y, z3);
		rf_fetch(x, tx[3], nl);
		rf_fetch(y, ty[3], nl);
		rf_used = 0;
	}
	memcpy(tx[1], g->x, sizeof(fe)); memcpy(ty[1], g->y, sizeof(fe));
	memcpy(tx[2], q->x, sizeof(fe)); memcpy(ty[2], q->y, sizeof(fe));

	for (int bit = 32 * (int)nl - 1; bit >= 0; bit--) {
		int idx = (int)(((u1[bit / 32] >> (bit % 32)) & 1) |
			(((u2[bit / 32] >> (bit % 32)) & 1) << 1));
		if (!inf) rf_dbl(&X, &Y, &Z);
		if (!idx) continue;
		if (inf) {
			X = rf_get(); rf_load(X, tx[idx], nl);
			Y = rf_get(); rf_load(Y, ty[idx], nl);
			Z = rf_get(); rf_load(Z, cv->one, nl);
			inf = false;
			continue;
		}
		{
			int x2 = rf_get(), y2 = rf_get();
			rf_load(x2, tx[idx], nl);
			rf_load(y2, ty[idx], nl);
			if (!rf_madd(&X, &Y, &Z, x2, y2, nl)) return false;
		}
	}

	if (inf) { pt_zero(out); return true; }
	// affine x = X / Z^2
	{
		int zi = rf_inv(Z, cv), z2 = rf_get(), x = rf_get();
		MUL(z2, zi, zi);
		MUL(x, X, z2);
		memset(out, 0, sizeof(*out));
		rf_fetch(x, out->x, nl);
		memcpy(out->z, cv->one, sizeof(fe));
	}
	return true;
}
#undef MUL
#undef ADD
#undef SUB
#endif

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

// -- sharing the block (docs/montmul.md, "Sharing") --
//
// The block holds one modulus for the whole machine, and zfed's `fed`,
// cryptobench or anything else may use it too. So each verification
// CLAIMS it first (the OWNER register) and forgets which modulus it
// last loaded, so hw_select() loads its own again: a few dozen writes a
// verification, against whatever the last user left there.
//
// A claim that does not succeed within ~2 s means someone holds the
// block for long: this process then switches to software for good, its
// curve constants rebuilt in software's form, rather than wait again
// every time. The two forms cannot be mixed within a verification.
//
// On a bitstream older than the OWNER register, the claim reads back 0:
// nothing to share with, and the block is used as it always was.
#if defined(EC_HW) && !defined(EC_HW_SIM)
static bool hw_claim(void) {
	static uint32_t me;
	if (!hw_available(1)) return false;
	if (!me) me = z_getpid() & 0x7FFFFFFFu;
	if (!me) me = 0x7FFFFFFEu;
	for (int tries = 0; tries < 180; tries++) {
		*hw_reg(Z_MONTMUL_W_OWNER) = me;
		uint32_t got = *hw_reg(Z_MONTMUL_W_OWNER);
		if (got == me || got == 0) {
			hw_cur = NULL;			// whatever is loaded is not ours
			return true;
		}
		z_proc_wait(8);			// ~11 ms
	}
	printf("ecdsa: the montmul block has been busy for 2 s; using software from now on\n");
	hw_forced_off = true;
	for (int i = 0; i < 2; i++) {
		if (curves[i].hw) { curves[i].hw = false; curves[i].ready = false; }
	}
	return false;
}

static void hw_release(void) {
	*hw_reg(Z_MONTMUL_W_OWNER) = (z_getpid() & 0x7FFFFFFFu) | Z_HW_OWNER_RELEASE;
}
#else
static bool hw_claim(void) { return false; }
static void hw_release(void) { }
#endif

static bool ec_verify_held(ec_curve_id_t id, const uint8_t *point, uint32_t point_len,
	const uint8_t *r_bytes, uint32_t r_len,
	const uint8_t *s_bytes, uint32_t s_len,
	const uint8_t *hash, uint32_t hash_len);

bool ec_verify(ec_curve_id_t id, const uint8_t *point, uint32_t point_len,
	const uint8_t *r_bytes, uint32_t r_len,
	const uint8_t *s_bytes, uint32_t s_len,
	const uint8_t *hash, uint32_t hash_len) {
	bool held = hw_claim();
	bool ok = ec_verify_held(id, point, point_len, r_bytes, r_len, s_bytes, s_len, hash, hash_len);
	if (held) hw_release();
	return ok;
}

static bool ec_verify_held(ec_curve_id_t id, const uint8_t *point, uint32_t point_len,
	const uint8_t *r_bytes, uint32_t r_len,
	const uint8_t *s_bytes, uint32_t s_len,
	const uint8_t *hash, uint32_t hash_len) {

	ec_curve_t *cv = curve_for(id);
	uint16_t nl, nb;
	uint32_t r[EC_MAX_LIMBS], s[EC_MAX_LIMBS], e[EC_MAX_LIMBS];
	uint32_t u1[EC_MAX_LIMBS], u2[EC_MAX_LIMBS];
	uint8_t rb[EC_MAX_LIMBS * 4], sb[EC_MAX_LIMBS * 4], eb[EC_MAX_LIMBS * 4];
	pt_t Q, R;
	bool affine = false;		// R.x is already affine (the register file's path)

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
#ifdef EC_HW
		if (cv->hw && hw_nregs >= RF_REGS && !ec_rf_disable) {
			if (rf_shamir(cv, &R, u1, &G, u2, &Q)) { ec_stat_rf++; affine = true; }
			else { ec_stat_rf_bail++; shamir(cv, &R, u1, &G, u2, &Q); }
		} else
#endif
		shamir(cv, &R, u1, &G, u2, &Q);
	}

	if (pt_is_zero(&R, nl)) return false;

	// Back to affine and compare x with r, modulo n. One inverse --
	// which is what the Jacobian representation bought.
	{
		fe zinv, z2, x_aff;
		uint32_t xa[EC_MAX_LIMBS];

		if (affine) memcpy(x_aff, R.x, sizeof(fe));		// rf_shamir() did it
		else {
			fe_inv(zinv, R.z, cv);
			fe_mul(z2, zinv, zinv, cv);
			fe_mul(x_aff, R.x, z2, cv);
		}
		from_field(xa, x_aff, cv);

		// x is a field element and r is a scalar, so the comparison
		// is modulo n. p and n are close enough on both curves that
		// at most one subtraction is needed.
		if (cmp_n(xa, cv->n, nl) >= 0) sub_n(xa, xa, cv->n, nl);

		return cmp_n(xa, r, nl) == 0;
	}

}

#ifdef EC_TEST_HOOKS
// Tests only: inv_mod() and its two users against the Fermat inverses
// they replaced, on P-256's and P-384's p and n -- random values and the
// edges. Returns how many disagree (0 is right); *checked, how many
// values were tried.
static uint32_t test_rng = 20260927u;
static uint32_t test_rand(void) { test_rng = test_rng * 1664525u + 1013904223u; return test_rng; }

// the value `k` of the edge list, or a random one, below m and not 0
static bool test_value(uint32_t *a, const uint32_t *m, uint16_t nl, int k) {
	uint32_t one[EC_MAX_LIMBS];
	int bits = 32 * nl;
	memset(a, 0, (size_t)nl * sizeof(uint32_t));
	memset(one, 0, sizeof(one)); one[0] = 1;
	if (k == 0) a[0] = 1;
	else if (k == 1) a[0] = 2;
	else if (k == 2) a[0] = 3;
	else if (k == 3) sub_n(a, m, one, nl);						// m - 1
	else if (k == 4) { sub_n(a, m, one, nl); sub_n(a, a, one, nl); }	// m - 2
	else if (k == 5) { memcpy(a, m, (size_t)nl * 4); add_n(a, a, one, nl); shr1_n(a, 0, nl); }	// (m + 1)/2
	else if (k < 6 + bits) { int b = k - 6; a[b / 32] = 1u << (b % 32); }	// 2^b
	else if (k < 6 + 2 * bits) { int b = k - 6 - bits + 1; for (int i = 0; i < b; i++) a[i / 32] |= 1u << (i % 32); }	// 2^b - 1
	else if (k == 6 + 2 * bits) for (uint16_t i = 0; i < nl; i++) a[i] = 0x55555555u;
	else if (k == 7 + 2 * bits) for (uint16_t i = 0; i < nl; i++) a[i] = 0xAAAAAAAAu;
	else for (uint16_t i = 0; i < nl; i++) a[i] = test_rand();
	while (cmp_n(a, m, nl) >= 0) a[nl - 1] >>= 1;
	return !is_zero_n(a, nl);
}

int ec_test_inverses(int randoms, int *checked) {
	int bad = 0;
	*checked = 0;
	for (int c = 0; c < 2; c++) {
		ec_curve_t *cv = curve_for(c == 0 ? EC_CURVE_P256 : EC_CURVE_P384);
		uint16_t nl = cv->nl;
		for (int which = 0; which < 2; which++) {		// p, then n
			const uint32_t *m = which ? cv->n : cv->p;
			mont_t mc;
			memset(&mc, 0, sizeof(mc));
			mont_setup(&mc, m, nl);
			int edges = 8 + 2 * 32 * nl;
			for (int k = 0; k < edges + randoms; k++) {
				uint32_t a[EC_MAX_LIMBS], x[EC_MAX_LIMBS], am[EC_MAX_LIMBS], f[EC_MAX_LIMBS], y[EC_MAX_LIMBS];
				if (!test_value(a, m, nl, k)) continue;
				(*checked)++;
				inv_mod(x, a, m, nl);
				// against Fermat
				to_mont(am, a, &mc, nl);
				mont_inv_fermat(f, am, &mc, nl);
				from_mont(f, f, &mc, nl);
				if (cmp_n(x, f, nl)) { bad++; continue; }
				// and a * a^-1 = 1
				to_mont(y, x, &mc, nl);
				mont_mul(y, y, am, &mc, nl);
				from_mont(y, y, &mc, nl);
				if (!is_one_n(y, nl)) { bad++; continue; }
				// the wrappers: mont_inv() on its own form, fe_inv() on the field's
				mont_inv(y, am, &mc, nl);
				from_mont(y, y, &mc, nl);
				if (cmp_n(x, y, nl)) { bad++; continue; }
				if (!which) {
					fe fa, fi, ff;
					to_field(fa, a, cv);
					fe_inv(fi, fa, cv);
					fe_inv_fermat(ff, fa, cv);
					if (cmp_n(fi, ff, nl)) bad++;
				}
			}
		}
		// 0 has no inverse: 0 back, and no hang
		{
			uint32_t z[EC_MAX_LIMBS], x[EC_MAX_LIMBS];
			memset(z, 0, sizeof(z));
			inv_mod(x, z, cv->p, nl);
			if (!is_zero_n(x, nl)) bad++;
		}
	}
	return bad;
}
#endif
