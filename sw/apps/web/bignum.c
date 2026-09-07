/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See bignum.h.
 */

#include <string.h>

#include "bignum.h"

static void bn_zero(bn_t *a) { memset(a, 0, sizeof(*a)); }

static void bn_trim(bn_t *a) {
	while (a->n && a->v[a->n - 1] == 0) a->n--;
}

uint32_t bn_bits(const bn_t *a) {

	uint32_t top;
	uint32_t bits;

	if (a->n == 0) return 0;

	top = a->v[a->n - 1];
	bits = (uint32_t)(a->n - 1) * 32;

	while (top) { bits++; top >>= 1; }

	return bits;

}

int bn_cmp(const bn_t *a, const bn_t *b) {

	int i;
	uint16_t n = a->n > b->n ? a->n : b->n;

	for (i = (int)n - 1; i >= 0; i--) {
		uint32_t x = (i < a->n) ? a->v[i] : 0;
		uint32_t y = (i < b->n) ? b->v[i] : 0;
		if (x != y) return x < y ? -1 : 1;
	}

	return 0;

}

bool bn_from_bytes(bn_t *a, const uint8_t *data, uint32_t len) {

	uint32_t i;

	bn_zero(a);

	// Leading zeros carry no value and would otherwise inflate the
	// limb count -- which matters because DER integers are routinely
	// padded with one to keep them positive.
	while (len && *data == 0) { data++; len--; }

	if (len > BN_MAX_LIMBS * 4) return false;

	for (i = 0; i < len; i++) {
		uint32_t byte_from_end = len - 1 - i;
		a->v[byte_from_end / 4] |= (uint32_t)data[i] << (8 * (byte_from_end % 4));
	}

	a->n = (uint16_t)((len + 3) / 4);
	bn_trim(a);

	return true;

}

bool bn_to_bytes(const bn_t *a, uint8_t *out, uint32_t len) {

	uint32_t need = (bn_bits(a) + 7) / 8;
	uint32_t i;

	if (need > len) return false;

	memset(out, 0, len);

	for (i = 0; i < need; i++) {
		uint32_t byte_from_end = i;
		out[len - 1 - i] =
			(uint8_t)(a->v[byte_from_end / 4] >> (8 * (byte_from_end % 4)));
	}

	return true;

}

// -- primitives on raw limb arrays ---------------------------------
//
// These work on explicit (ptr, count) rather than bn_t because the
// Montgomery inner loop needs a 2n+1 limb accumulator that is not a
// valid bn_t.

// r -= m, assuming r >= m. Returns the final borrow, which the caller
// uses to decide whether the subtraction should have happened at all.
static uint32_t limbs_sub(uint32_t *r, const uint32_t *m, uint16_t n) {

	uint32_t borrow = 0;
	uint16_t i;

	for (i = 0; i < n; i++) {
		uint64_t d = (uint64_t)r[i] - m[i] - borrow;
		r[i] = (uint32_t)d;
		borrow = (d >> 32) ? 1 : 0;
	}

	return borrow;

}

static int limbs_cmp(const uint32_t *a, const uint32_t *b, uint16_t n) {
	int i;
	for (i = (int)n - 1; i >= 0; i--)
		if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
	return 0;
}

// -n^-1 mod 2^32, by Newton iteration.
//
// Each step doubles the number of correct bits, so five steps take
// the initial 2 bits of accuracy past 32. Requires n odd.
static uint32_t mont_n0inv(uint32_t n0) {

	uint32_t x = 1;
	int i;

	for (i = 0; i < 5; i++) x *= 2 - n0 * x;

	return (uint32_t)(0u - x);

}

// Montgomery product: out = a * b * R^-1 mod m, where R = 2^(32n).
//
// Interleaved multiply-and-reduce (CIOS): one pass, one temporary,
// no intermediate 2n-limb product to hold.
static void mont_mul(uint32_t *out, const uint32_t *a, const uint32_t *b,
	const uint32_t *m, uint16_t n, uint32_t n0inv) {

	// n + 2 limbs: the running value, plus a carry limb, plus the
	// extra bit the final conditional subtraction can need.
	static uint32_t t[BN_MAX_LIMBS + 2];
	uint16_t i, j;

	memset(t, 0, (size_t)(n + 2) * sizeof(uint32_t));

	for (i = 0; i < n; i++) {

		uint32_t carry = 0;
		uint32_t mi;

		// t += a[i] * b
		for (j = 0; j < n; j++) {
			uint64_t p = (uint64_t)a[i] * b[j] + t[j] + carry;
			t[j] = (uint32_t)p;
			carry = (uint32_t)(p >> 32);
		}
		{
			uint64_t s = (uint64_t)t[n] + carry;
			t[n] = (uint32_t)s;
			t[n + 1] += (uint32_t)(s >> 32);
		}

		// t += (t[0] * n0inv) * m, which makes the low limb zero
		mi = t[0] * n0inv;
		carry = 0;
		for (j = 0; j < n; j++) {
			uint64_t p = (uint64_t)mi * m[j] + t[j] + carry;
			t[j] = (uint32_t)p;
			carry = (uint32_t)(p >> 32);
		}
		{
			uint64_t s = (uint64_t)t[n] + carry;
			t[n] = (uint32_t)s;
			t[n + 1] += (uint32_t)(s >> 32);
		}

		// shift down one limb
		for (j = 0; j <= n; j++) t[j] = t[j + 1];
		t[n + 1] = 0;

	}

	// The result is below 2m, so at most one subtraction is needed.
	// The overflow limb has to be part of that decision: t can exceed
	// m while its low n limbs compare as smaller.
	if (t[n] || limbs_cmp(t, m, n) >= 0) limbs_sub(t, m, n);

	memcpy(out, t, (size_t)n * sizeof(uint32_t));

}

bool bn_modexp(bn_t *out, const bn_t *base, const bn_t *exp, const bn_t *m) {

	static uint32_t r[BN_MAX_LIMBS];
	static uint32_t x[BN_MAX_LIMBS];
	static uint32_t rr[BN_MAX_LIMBS * 2 + 1];
	uint16_t n = m->n;
	uint32_t n0inv;
	int bit;

	if (n == 0) return false;
	if ((m->v[0] & 1) == 0) return false;		// see bignum.h
	if (n > BN_MAX_LIMBS) return false;

	n0inv = mont_n0inv(m->v[0]);

	// R^2 mod m, needed to move values into Montgomery form.
	//
	// Computed by doubling 64n times starting from 1: that reaches
	// 2^(32n) = R after the first 32n, and R * 2^(32n) = R^2 after
	// the rest. Only shifts, compares and subtracts -- no division,
	// which is the whole reason for using Montgomery here.
	//
	// The obvious shortcut is to write R mod m as R - m, since a
	// single subtraction lands in range. THAT IS ONLY TRUE WHEN THE
	// TOP BIT OF THE TOP LIMB IS SET. Every RSA modulus satisfies it
	// -- their bit lengths are multiples of 8 and the high bit is
	// always on -- so the shortcut passed every RSA-sized test and
	// silently produced zero for a modulus like 497 or 2^2000+1.
	// Caught by putting deliberately un-normalised moduli in the
	// vectors, which is the only reason it was found: nothing in a
	// certificate would ever have exercised it, right up until
	// something did.
	//
	// The cost is real -- roughly as many operations as the
	// exponentiation itself -- and it is paid once per signature. If
	// profiling ever says it matters, the fix is a proper reduction
	// of 2^(32n) rather than a special case for normalised moduli.
	{
		uint32_t i;

		// Start from R mod m rather than from 1 when the modulus is
		// NORMALISED -- top bit of the top limb set -- which halves
		// this loop.
		//
		// For such a modulus, m < R < 2m, so R mod m is R - m: one
		// subtraction instead of 32n doublings. The remaining 32n
		// doublings then take R to R^2.
		//
		// This file's own comment used to say the fix, if profiling
		// ever asked for one, was "a proper reduction of 2^(32n)
		// rather than a special case for normalised moduli".
		// Profiling asked: RSA-2048 verification is 6.9 seconds on
		// hardware, this setup is roughly as many operations as the
		// exponentiation itself, and a server hung up mid-chain
		// while it ran. The special case is taken, but GUARDED --
		// the general path is still there and still correct for any
		// modulus, which is what the earlier warning was really
		// about. An RSA modulus is always normalised; the scalar
		// fields this file also serves are not necessarily.
		{
			bool normalised = (m->v[n - 1] & 0x80000000u) != 0;
			uint32_t start;

			memset(rr, 0, sizeof(uint32_t) * ((size_t)n + 1));

			if (normalised) {
				// rr = R - m, computed as (0 - m) in n limbs, which
				// is exactly 2^(32n) - m.
				uint32_t borrow = 0;
				uint16_t j;
				for (j = 0; j < n; j++) {
					uint64_t d = (uint64_t)0 - m->v[j] - borrow;
					rr[j] = (uint32_t)d;
					borrow = (d >> 32) ? 1u : 0u;
				}
				start = 32u * n;
			} else {
				rr[0] = 1;
				start = 0;
			}

		// A modulus of 1 makes everything zero and the loop below
		// would spin harmlessly; nothing valid reaches here with one.
		for (i = start; i < 64u * n; i++) {
			uint32_t carry = 0;
			uint16_t j;
			for (j = 0; j < n; j++) {
				uint32_t hi = rr[j] >> 31;
				rr[j] = (rr[j] << 1) | carry;
				carry = hi;
			}
			if (carry || limbs_cmp(rr, m->v, n) >= 0) limbs_sub(rr, m->v, n);
		}
		}
	}

	// x = base mod m, in Montgomery form.
	{
		static uint32_t b[BN_MAX_LIMBS];
		// A base at or above the modulus is REFUSED, not reduced.
		//
		// For the only caller that matters, that is exactly right: an
		// RSA signature must satisfy 0 <= s < n, and one that does not
		// is invalid. Reducing it instead would quietly accept a
		// signature the standard rejects.
		//
		// The first version tested base->n > m->n, which compares
		// LIMB COUNTS and so let through any base that happened to
		// occupy the same number of limbs -- 0xffffffff against a
		// modulus of 497, for instance, which then came out wrong
		// rather than refused.
		memset(b, 0, sizeof(uint32_t) * n);
		if (bn_cmp(base, m) >= 0) return false;
		memcpy(b, base->v, sizeof(uint32_t) * base->n);
		mont_mul(x, b, rr, m->v, n, n0inv);
	}

	// r = 1 in Montgomery form, i.e. R mod m. Obtained the same way
	// everything else enters Montgomery form: multiply by R^2.
	{
		static uint32_t one[BN_MAX_LIMBS];
		memset(one, 0, sizeof(uint32_t) * n);
		one[0] = 1;
		mont_mul(r, one, rr, m->v, n, n0inv);
	}

	// Square-and-multiply, most significant bit first.
	for (bit = (int)bn_bits(exp) - 1; bit >= 0; bit--) {
		mont_mul(r, r, r, m->v, n, n0inv);
		if ((exp->v[bit / 32] >> (bit % 32)) & 1)
			mont_mul(r, r, x, m->v, n, n0inv);
	}

	// Out of Montgomery form: multiply by 1.
	{
		static uint32_t one[BN_MAX_LIMBS];
		memset(one, 0, sizeof(uint32_t) * n);
		one[0] = 1;
		mont_mul(r, r, one, m->v, n, n0inv);
	}

	bn_zero(out);
	memcpy(out->v, r, sizeof(uint32_t) * n);
	out->n = n;
	bn_trim(out);

	return true;

}
