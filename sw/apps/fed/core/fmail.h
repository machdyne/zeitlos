#ifndef FMAIL_H
#define FMAIL_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Letters between nodes, sealed. docs/fed.md, "Mail between nodes".
 *
 * A node's mail keys are derived from its seed; a letter is sealed to a
 * node's two public mail keys with a hybrid KEM -- X25519 and ML-KEM-768
 * -- so that breaking either alone reads nothing:
 *
 *   "ZML1" | ephemeral X25519 key (32) | ML-KEM-768 ciphertext (1088)
 *          | nonce (24) | XChaCha20-Poly1305(letter) | tag (16)
 *
 *   ss   = ML-KEM shared secret || X25519(eph, recipient's X25519 key)
 *   prk  = HMAC-SHA256("zfed mail 1", ss)
 *   key  = HMAC-SHA256(prk, "zfed mail key" | eph | recipient's X25519 key | 0x01)
 *
 * the header (magic, eph, ciphertext) authenticated as the AEAD's
 * associated data. As X-Wing combines them: X25519's ciphertext (eph)
 * and public key bound in; ML-KEM's, being IND-CCA, need not be.
 */
#include <stdint.h>
#include <stdbool.h>
#include "../../../common/zmlkem.h"

#define FMAIL_MAGIC      "ZML1"
#define FMAIL_HEAD       (4 + 32 + ZMLKEM_CT_BYTES)		// authenticated: 1124
#define FMAIL_OVERHEAD   (FMAIL_HEAD + 24 + 16)				// 1164
#define FMAIL_RANDOM     (32 + 32 + 24)						// ephemeral key, ML-KEM's m, nonce

// Classical, for networks that choose it in their profile (docs/fed.md,
// "Network profiles"): X25519 alone -- 76 bytes of overhead, not 1,164,
// and no protection against a quantum computer:
//
//   "ZMX1" | ephemeral X25519 key (32) | nonce (24) | XChaCha20-Poly1305(letter) | tag (16)
//   prk = HMAC-SHA256("zfed mail x 1", X25519(eph, recipient's key))
//   key = HMAC-SHA256(prk, "zfed mail key" | eph | recipient's key | 0x01)
#define FMAIL_X_MAGIC    "ZMX1"
#define FMAIL_X_HEAD     (4 + 32)
#define FMAIL_X_OVERHEAD (FMAIL_X_HEAD + 24 + 16)				// 76
#define FMAIL_X_RANDOM   (32 + 24)

typedef struct {
	uint8_t xsk[32], xpk[32];
	uint8_t ek[ZMLKEM_EK_BYTES], dk[ZMLKEM_DK_BYTES];
} fmail_keys_t;

// The node's mail keys, from its 32-byte seed (HKDF with fixed labels).
void fmail_keys(fmail_keys_t *k, const uint8_t seed[32]);

// A letter sealed to (xpk, ek) into out: its length, or -1 (no room;
// ek fails FIPS 203's check; xpk of low order). `rnd`: FMAIL_RANDOM
// fresh random bytes.
int fmail_seal(uint8_t *out, uint32_t cap, const uint8_t *letter, uint32_t len,
	const uint8_t xpk[32], const uint8_t ek[ZMLKEM_EK_BYTES], const uint8_t rnd[FMAIL_RANDOM]);

int fmail_seal_classical(uint8_t *out, uint32_t cap, const uint8_t *letter, uint32_t len,
	const uint8_t xpk[32], const uint8_t rnd[FMAIL_X_RANDOM]);

// A sealed letter -- either kind -- opened with our keys into out: its
// length, or -1: not ours, or tampered with, or not a sealed letter.
int fmail_open(uint8_t *out, uint32_t cap, const uint8_t *sealed, uint32_t len, const fmail_keys_t *k);

#endif
