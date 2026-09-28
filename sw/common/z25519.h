#ifndef Z25519_H
#define Z25519_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * X25519 and Ed25519 signature checks on montmul's register file
 * (rtl/montmul.v with REGFILE, docs/montmul.md), or in software.
 * docs/z25519.md.
 *
 * Drop-in for Monocypher's crypto_x25519() and crypto_ed25519_check():
 * the same arguments, the same results, byte for byte and verdict for
 * verdict -- the tests hold them to it against Monocypher. Where the
 * bitstream has the register file and nobody else holds the block, the
 * work runs there, ~7-10x faster; otherwise these call Monocypher.
 * Callers do not need to know which.
 *
 * Signing stays in Monocypher: it is rare, and it handles a secret key
 * this file would have to keep out of the block's registers.
 */
#include <stdint.h>
#include <stddef.h>

// RFC 7748 X25519: shared = X25519(scalar, point). Constant time in the
// scalar, on the block as in Monocypher.
void z_x25519(uint8_t shared[32], const uint8_t scalar[32], const uint8_t point[32]);

// The public key for an X25519 secret: X25519(secret, 9), as
// crypto_x25519_public_key().
void z_x25519_public_key(uint8_t public_key[32], const uint8_t secret_key[32]);

// RFC 8032 Ed25519, with Monocypher's exact rules (cofactored; non-
// canonical A and R accepted; s < L required). 0 if the signature is
// valid, -1 if not -- as crypto_ed25519_check().
int z_ed25519_check(const uint8_t signature[64], const uint8_t public_key[32],
	const uint8_t *msg, size_t msg_size);

// Which path the last call took: 1 the block, 0 software. For tests
// and cryptobench.
extern int z25519_last_hw;

// Tests and cryptobench: force software (1) or allow the block (0).
extern int z25519_no_hw;

#endif
