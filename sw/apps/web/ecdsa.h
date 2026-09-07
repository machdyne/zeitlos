#ifndef ECDSA_H
#define ECDSA_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * ECDSA signature VERIFICATION on NIST P-256 and P-384.
 *
 * -- why both curves --
 *
 * P-256 alone is not enough to verify the real web. A large share of
 * ECDSA chains put a P-384 intermediate above a P-256 leaf --
 * DigiCert's ECC intermediates and Let's Encrypt's E-series are both
 * that shape -- so a client that stops at P-256 parses the chain
 * fine, verifies the leaf fine, and then cannot check the one
 * signature that matters.
 *
 * That is exactly how this was found: en.wikipedia.org returned "EC
 * key is not P-256" from the X.509 parser, on the intermediate.
 *
 * -- one implementation, two curves --
 *
 * Both are short Weierstrass curves with a = -3 over a prime field,
 * which is the whole reason this is one file with a curve parameter
 * rather than two files. The limb count is the only thing that
 * really differs: 8 for P-256, 12 for P-384.
 *
 * -- verification only, and everything is public --
 *
 * No signing, no key generation, no ECDH. Nothing here touches a
 * secret, so it is not constant time and must not be reused for
 * anything that is. Same claim, and same caveat, as bignum.h.
 *
 * -- what it costs --
 *
 * A double-scalar multiplication, with Shamir's trick: one doubling
 * and at most one addition per bit. P-384 is around 2.7x P-256 --
 * half again as many bits, each costing 2.25x as much field work,
 * since a schoolbook multiply is quadratic in the limb count.
 *
 * On this CPU that puts a P-384 verification in the high hundreds of
 * milliseconds. A chain with a P-384 intermediate pays it once.
 */

#include <stdint.h>
#include <stdbool.h>

typedef enum {
	EC_CURVE_P256 = 0,
	EC_CURVE_P384,
} ec_curve_id_t;

// `point` is the uncompressed public key: 0x04 followed by x and y,
// 32 bytes each for P-256 and 48 for P-384. That is what an X.509
// SubjectPublicKeyInfo carries; the compressed form is refused rather
// than supported, since it needs a field square root and appears on
// essentially no TLS certificate.
//
// `r` and `s` are as they appear in the DER ECDSA-Sig-Value:
// big-endian, possibly with a leading zero, possibly short. Both are
// normalised here.
//
// `hash` may be any length. Per SEC1, the leftmost bits of it are
// taken, up to the bit length of the curve order -- so SHA-384 with
// P-256 is truncated, and SHA-256 with P-384 is used whole.
//
// Returns false on anything wrong at all: a malformed point, a point
// not on the curve, a component outside [1, n-1], or a signature that
// does not verify. The caller's only correct response to any of them
// is to reject the certificate, so they are not distinguished.
bool ec_verify(ec_curve_id_t curve, const uint8_t *point, uint32_t point_len,
	const uint8_t *r, uint32_t r_len,
	const uint8_t *s, uint32_t s_len,
	const uint8_t *hash, uint32_t hash_len);

// Bytes in an uncompressed point for each curve, including the 0x04.
uint32_t ec_point_len(ec_curve_id_t curve);

#endif
