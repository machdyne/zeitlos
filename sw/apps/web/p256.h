#ifndef P256_H
#define P256_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * THIS IS A FORWARDING SHIM. The implementation became two-curve and
 * moved to ecdsa.c/ecdsa.h when P-384 turned out to be unavoidable --
 * a large share of real chains put a P-384 intermediate above a P-256
 * leaf, so P-256 alone verifies the leaf and then cannot check the
 * signature above it.
 *
 * A file called p256.c exporting a P-384 routine would have been
 * worse than a rename. New code should include ecdsa.h.
 *
 * p256.c is now an empty stub and can be deleted.
 */

#include "ecdsa.h"

static inline bool p256_verify(const uint8_t point[65],
	const uint8_t *r, uint32_t r_len,
	const uint8_t *s, uint32_t s_len,
	const uint8_t hash[32]) {
	return ec_verify(EC_CURVE_P256, point, 65, r, r_len, s, s_len, hash, 32);
}

#endif
