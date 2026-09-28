/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Letters between nodes, sealed. fmail.h; docs/fed.md, "Mail between
 * nodes". The known-answer test is tests/gen_mail_kat.py, which does all
 * of it again in Python (cryptography, kyber-py).
 */
#include <string.h>
#include "fmail.h"
#include "../../../common/zsha256.h"
#include "../../../common/z25519.h"
#include "../../../ext/monocypher/monocypher.h"

static void hmac(uint8_t out[32], const uint8_t *key, uint32_t klen, const uint8_t *m1, uint32_t l1,
		const uint8_t *m2, uint32_t l2, const uint8_t *m3, uint32_t l3) {
	uint8_t k[64], pad[64], inner[32];
	z_sha256_ctx c;
	memset(k, 0, sizeof(k));
	if (klen > 64) z_sha256(k, key, klen); else memcpy(k, key, klen);
	for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
	z_sha256_init(&c); z_sha256_update(&c, pad, 64);
	z_sha256_update(&c, m1, l1);
	if (l2) z_sha256_update(&c, m2, l2);
	if (l3) z_sha256_update(&c, m3, l3);
	z_sha256_final(&c, inner);
	for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
	z_sha256_init(&c); z_sha256_update(&c, pad, 64); z_sha256_update(&c, inner, 32); z_sha256_final(&c, out);
	crypto_wipe(k, sizeof(k));
	crypto_wipe(pad, sizeof(pad));
	crypto_wipe(inner, sizeof(inner));
}

void fmail_keys(fmail_keys_t *k, const uint8_t seed[32]) {
	static const uint8_t salt[] = "zfed mail keys 1";
	uint8_t prk[32], coins[64];
	hmac(prk, salt, sizeof(salt) - 1, seed, 32, NULL, 0, NULL, 0);
	hmac(k->xsk, prk, 32, (const uint8_t *)"x25519\x01", 7, NULL, 0, NULL, 0);
	hmac(coins, prk, 32, (const uint8_t *)"ml-kem-768 d\x01", 13, NULL, 0, NULL, 0);
	hmac(coins + 32, prk, 32, (const uint8_t *)"ml-kem-768 z\x01", 13, NULL, 0, NULL, 0);
	z_x25519_public_key(k->xpk, k->xsk);
	zmlkem_keypair(k->ek, k->dk, coins);
	crypto_wipe(prk, sizeof(prk));
	crypto_wipe(coins, sizeof(coins));
}

// The AEAD key from the two shared secrets, eph and the X25519 key.
static void mail_key(uint8_t key[32], const uint8_t ss_mlkem[32], const uint8_t ss_x[32],
		const uint8_t eph[32], const uint8_t xpk[32]) {
	static const uint8_t salt[] = "zfed mail 1";
	static const uint8_t label[] = "zfed mail key";
	uint8_t prk[32], ikm[64], tail[65];
	memcpy(ikm, ss_mlkem, 32);
	memcpy(ikm + 32, ss_x, 32);
	hmac(prk, salt, sizeof(salt) - 1, ikm, 64, NULL, 0, NULL, 0);
	memcpy(tail, eph, 32);
	memcpy(tail + 32, xpk, 32);
	tail[64] = 1;
	hmac(key, prk, 32, label, sizeof(label) - 1, tail, 65, NULL, 0);
	crypto_wipe(prk, sizeof(prk));
	crypto_wipe(ikm, sizeof(ikm));
}

static bool all_zero(const uint8_t *b, int n) {
	uint8_t acc = 0;
	for (int i = 0; i < n; i++) acc |= b[i];
	return acc == 0;
}

int fmail_seal(uint8_t *out, uint32_t cap, const uint8_t *letter, uint32_t len,
		const uint8_t xpk[32], const uint8_t ek[ZMLKEM_EK_BYTES], const uint8_t rnd[FMAIL_RANDOM]) {
	if (cap < FMAIL_OVERHEAD || len > cap - FMAIL_OVERHEAD) return -1;
	uint8_t eph_sk[32], ss_x[32], ss_m[32], key[32];
	uint8_t *eph = out + 4, *ct = out + 36, *nonce = out + FMAIL_HEAD, *body = out + FMAIL_HEAD + 24;
	memcpy(eph_sk, rnd, 32);
	if (!zmlkem_encaps(ct, ss_m, ek, rnd + 32)) { crypto_wipe(eph_sk, 32); return -1; }
	memcpy(out, FMAIL_MAGIC, 4);
	z_x25519_public_key(eph, eph_sk);
	z_x25519(ss_x, eph_sk, xpk);
	crypto_wipe(eph_sk, 32);
	if (all_zero(ss_x, 32)) { crypto_wipe(ss_m, 32); return -1; }		// a low-order key: no secret at all
	mail_key(key, ss_m, ss_x, eph, xpk);
	memcpy(nonce, rnd + 64, 24);
	crypto_aead_lock(body, body + len, key, nonce, out, FMAIL_HEAD, letter, len);
	crypto_wipe(key, 32); crypto_wipe(ss_x, 32); crypto_wipe(ss_m, 32);
	return (int)(len + FMAIL_OVERHEAD);
}

static void mail_key_x(uint8_t key[32], const uint8_t ss_x[32], const uint8_t eph[32], const uint8_t xpk[32]) {
	static const uint8_t salt[] = "zfed mail x 1";
	static const uint8_t label[] = "zfed mail key";
	uint8_t prk[32], tail[65];
	hmac(prk, salt, sizeof(salt) - 1, ss_x, 32, NULL, 0, NULL, 0);
	memcpy(tail, eph, 32);
	memcpy(tail + 32, xpk, 32);
	tail[64] = 1;
	hmac(key, prk, 32, label, sizeof(label) - 1, tail, 65, NULL, 0);
	crypto_wipe(prk, sizeof(prk));
}

int fmail_seal_classical(uint8_t *out, uint32_t cap, const uint8_t *letter, uint32_t len,
		const uint8_t xpk[32], const uint8_t rnd[FMAIL_X_RANDOM]) {
	if (cap < FMAIL_X_OVERHEAD || len > cap - FMAIL_X_OVERHEAD) return -1;
	uint8_t eph_sk[32], ss_x[32], key[32];
	uint8_t *eph = out + 4, *nonce = out + FMAIL_X_HEAD, *body = out + FMAIL_X_HEAD + 24;
	memcpy(out, FMAIL_X_MAGIC, 4);
	memcpy(eph_sk, rnd, 32);
	z_x25519_public_key(eph, eph_sk);
	z_x25519(ss_x, eph_sk, xpk);
	crypto_wipe(eph_sk, 32);
	if (all_zero(ss_x, 32)) return -1;
	mail_key_x(key, ss_x, eph, xpk);
	memcpy(nonce, rnd + 32, 24);
	crypto_aead_lock(body, body + len, key, nonce, out, FMAIL_X_HEAD, letter, len);
	crypto_wipe(key, 32); crypto_wipe(ss_x, 32);
	return (int)(len + FMAIL_X_OVERHEAD);
}

static int open_classical(uint8_t *out, uint32_t cap, const uint8_t *sealed, uint32_t len, const fmail_keys_t *k) {
	if (len < FMAIL_X_OVERHEAD) return -1;
	uint32_t n = len - FMAIL_X_OVERHEAD;
	if (n > cap) return -1;
	const uint8_t *eph = sealed + 4, *nonce = sealed + FMAIL_X_HEAD, *body = sealed + FMAIL_X_HEAD + 24;
	uint8_t ss_x[32], key[32];
	z_x25519(ss_x, k->xsk, eph);
	if (all_zero(ss_x, 32)) return -1;
	mail_key_x(key, ss_x, eph, k->xpk);
	int bad = crypto_aead_unlock(out, body + n, key, nonce, sealed, FMAIL_X_HEAD, body, n);
	crypto_wipe(key, 32); crypto_wipe(ss_x, 32);
	if (bad) { crypto_wipe(out, n); return -1; }
	return (int)n;
}

int fmail_open(uint8_t *out, uint32_t cap, const uint8_t *sealed, uint32_t len, const fmail_keys_t *k) {
	if (len >= 4 && !memcmp(sealed, FMAIL_X_MAGIC, 4)) return open_classical(out, cap, sealed, len, k);
	if (len < FMAIL_OVERHEAD || memcmp(sealed, FMAIL_MAGIC, 4)) return -1;
	uint32_t n = len - FMAIL_OVERHEAD;
	if (n > cap) return -1;
	const uint8_t *eph = sealed + 4, *ct = sealed + 36, *nonce = sealed + FMAIL_HEAD, *body = sealed + FMAIL_HEAD + 24;
	uint8_t ss_x[32], ss_m[32], key[32];
	if (!zmlkem_decaps(ss_m, ct, k->dk)) return -1;
	z_x25519(ss_x, k->xsk, eph);
	if (all_zero(ss_x, 32)) { crypto_wipe(ss_m, 32); return -1; }
	mail_key(key, ss_m, ss_x, eph, k->xpk);
	int bad = crypto_aead_unlock(out, body + n, key, nonce, sealed, FMAIL_HEAD, body, n);
	crypto_wipe(key, 32); crypto_wipe(ss_x, 32); crypto_wipe(ss_m, 32);
	if (bad) { crypto_wipe(out, n); return -1; }
	return (int)n;
}
