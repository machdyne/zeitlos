/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * ML-KEM-768 over the pq-crystals reference. docs/mlkem.md; zmlkem.h.
 */
#include <string.h>
#include "zmlkem.h"

#define KYBER_K 3
#include "../ext/mlkem/params.h"
#include "../ext/mlkem/kem.h"
#include "../ext/mlkem/fips202.h"
#include "../ext/mlkem/randombytes.h"

#if KYBER_PUBLICKEYBYTES != ZMLKEM_EK_BYTES || KYBER_SECRETKEYBYTES != ZMLKEM_DK_BYTES || KYBER_CIPHERTEXTBYTES != ZMLKEM_CT_BYTES
#error "zmlkem.h's sizes do not match the library's"
#endif

// The library's own randomness is never used: every call here passes
// the caller's. Reaching it would be a bug that could make keys from
// nothing, so it stops rather than returning anything.
void randombytes(uint8_t *out, size_t outlen) {
	(void)out; (void)outlen;
	for (;;) { }
}

bool zmlkem_ek_ok(const uint8_t ek[ZMLKEM_EK_BYTES]) {
	// 12-bit coefficients, two in three bytes; each must be below q.
	for (int i = 0; i < KYBER_K * 256 / 2; i++) {
		const uint8_t *b = ek + 3 * i;
		uint16_t c0 = (uint16_t)(b[0] | ((b[1] & 0x0F) << 8));
		uint16_t c1 = (uint16_t)((b[1] >> 4) | (b[2] << 4));
		if (c0 >= KYBER_Q || c1 >= KYBER_Q) return false;
	}
	return true;
}

bool zmlkem_dk_ok(const uint8_t dk[ZMLKEM_DK_BYTES]) {
	uint8_t h[32];
	sha3_256(h, dk + KYBER_INDCPA_SECRETKEYBYTES, KYBER_PUBLICKEYBYTES);
	return !memcmp(h, dk + KYBER_SECRETKEYBYTES - 2 * KYBER_SYMBYTES, 32);
}

void zmlkem_keypair(uint8_t ek[ZMLKEM_EK_BYTES], uint8_t dk[ZMLKEM_DK_BYTES], const uint8_t coins[64]) {
	crypto_kem_keypair_derand(ek, dk, coins);
}

bool zmlkem_encaps(uint8_t ct[ZMLKEM_CT_BYTES], uint8_t ss[ZMLKEM_SS_BYTES],
	const uint8_t ek[ZMLKEM_EK_BYTES], const uint8_t m[32]) {
	if (!zmlkem_ek_ok(ek)) return false;
	crypto_kem_enc_derand(ct, ss, ek, m);
	return true;
}

bool zmlkem_decaps(uint8_t ss[ZMLKEM_SS_BYTES], const uint8_t ct[ZMLKEM_CT_BYTES],
	const uint8_t dk[ZMLKEM_DK_BYTES]) {
	if (!zmlkem_dk_ok(dk)) return false;
	crypto_kem_dec(ss, ct, dk);
	return true;
}
