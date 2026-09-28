#ifndef ZMLKEM_H
#define ZMLKEM_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * ML-KEM-768 (FIPS 203), post-quantum key encapsulation. docs/mlkem.md.
 * The pq-crystals reference (sw/ext/mlkem, public domain) behind a
 * small interface, with the input checks FIPS 203 requires.
 *
 * Randomness is the CALLER's: every function that needs it takes it as
 * an argument, filled from the TRNG. (Nothing here reaches the library's
 * own randombytes().)
 */
#include <stdint.h>
#include <stdbool.h>

#define ZMLKEM_EK_BYTES 1184		// the encapsulation key: public
#define ZMLKEM_DK_BYTES 2400		// the decapsulation key: secret
#define ZMLKEM_CT_BYTES 1088
#define ZMLKEM_SS_BYTES 32

// A key pair from 64 random bytes: d, then z (FIPS 203, ML-KEM.KeyGen).
void zmlkem_keypair(uint8_t ek[ZMLKEM_EK_BYTES], uint8_t dk[ZMLKEM_DK_BYTES], const uint8_t coins[64]);

// A shared secret and its ciphertext for ek, from 32 random bytes (m).
// False -- and nothing written -- if ek fails FIPS 203's check.
bool zmlkem_encaps(uint8_t ct[ZMLKEM_CT_BYTES], uint8_t ss[ZMLKEM_SS_BYTES],
	const uint8_t ek[ZMLKEM_EK_BYTES], const uint8_t m[32]);

// The shared secret from a ciphertext. A ciphertext that was tampered
// with does NOT fail: it gives a different, pseudorandom secret
// ("implicit rejection"), so the session keys will not match. False
// only if dk fails FIPS 203's check.
bool zmlkem_decaps(uint8_t ss[ZMLKEM_SS_BYTES], const uint8_t ct[ZMLKEM_CT_BYTES],
	const uint8_t dk[ZMLKEM_DK_BYTES]);

// FIPS 203's input checks: every coefficient of ek below q (the
// "modulus check"); dk's copy of H(ek) right (the "hash check").
bool zmlkem_ek_ok(const uint8_t ek[ZMLKEM_EK_BYTES]);
bool zmlkem_dk_ok(const uint8_t dk[ZMLKEM_DK_BYTES]);

#endif
