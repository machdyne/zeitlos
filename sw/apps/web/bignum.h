#ifndef BIGNUM_H
#define BIGNUM_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Just enough big-integer arithmetic to verify an RSA signature:
 * modular exponentiation with a small public exponent, and nothing
 * else.
 *
 * -- deliberately not a general bignum library --
 *
 * No division, no GCD, no primality testing, no signing. Every one of
 * those is needed to CREATE keys or signatures and none of them is
 * needed to CHECK one, and each is a substantial amount of code with
 * its own timing hazards. This browser only ever verifies.
 *
 * -- and deliberately not constant time --
 *
 * Everything this touches is PUBLIC: a public modulus, a public
 * exponent, a signature that arrived over the wire, and a hash of
 * data the attacker already has. There is no secret to leak, so the
 * usual constant-time discipline buys nothing and costs speed on a
 * CPU that needs it.
 *
 * That is a claim about this specific use, not about RSA. The moment
 * anything here is pointed at a private exponent it becomes wrong,
 * which is why bn_modexp() takes a modulus that must be odd and a
 * comment that says so rather than growing a "private" flag.
 *
 * -- Montgomery, because division is the expensive part --
 *
 * A schoolbook modmul at this size is two 64-limb multiplies and a
 * 128-limb-by-64-limb division. Montgomery reduction replaces the
 * division with another multiply, which on rv32im -- where MUL exists
 * and DIV is slow or absent -- is the difference between a handshake
 * that feels instant and one that does not.
 *
 * The cost, measured in multiplies: e = 65537 is 16 squarings and one
 * multiply, so 17 Montgomery modmuls, each about 8,200 32x32 products
 * at 2048 bits. Around 140,000 multiplies for one signature, or a few
 * milliseconds. A three-certificate chain is three of those. Signing
 * would be 2048 modmuls and is not something this machine should do.
 */

#include <stdint.h>
#include <stdbool.h>

// 4096 bits. RSA-2048 is the overwhelming majority of what is on the
// web, RSA-4096 exists on a few roots, and anything larger is not a
// key this browser needs to interoperate with.
#define BN_MAX_BITS   4096
#define BN_MAX_LIMBS  (BN_MAX_BITS / 32)

typedef struct {
	uint32_t	v[BN_MAX_LIMBS];	// little-endian limbs
	uint16_t	n;					// limbs in use
} bn_t;

// Big-endian bytes in, as they appear in DER. Leading zeros are
// dropped. Returns false if the value is wider than BN_MAX_BITS.
bool bn_from_bytes(bn_t *a, const uint8_t *data, uint32_t len);

// Big-endian bytes out, left-padded with zeros to exactly `len`.
// Returns false if the value does not fit -- which for a signature
// check means the result is not a valid encoded message and the
// caller must reject it rather than truncate.
bool bn_to_bytes(const bn_t *a, uint8_t *out, uint32_t len);

// out = base^exp mod m.
//
// `m` MUST BE ODD. Montgomery reduction requires it, and every RSA
// modulus is a product of two odd primes, so this costs nothing --
// but it is checked rather than assumed, because a caller that
// somehow supplies an even modulus would otherwise get a silently
// wrong answer, and a silently wrong answer here is a signature that
// verifies when it should not.
//
// Returns false if the modulus is even, zero or too large, or if
// `base` is not already less than it. The last is not a convenience
// check: for an RSA signature, s >= n is invalid, and reducing it
// instead of refusing would accept something the standard rejects.
bool bn_modexp(bn_t *out, const bn_t *base, const bn_t *exp, const bn_t *m);

// Number of significant bits.
uint32_t bn_bits(const bn_t *a);

// -1, 0 or 1.
int bn_cmp(const bn_t *a, const bn_t *b);

#endif
