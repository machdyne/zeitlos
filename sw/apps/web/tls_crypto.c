/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See tls_crypto.h.
 */

#include <string.h>

#include "tls_crypto.h"

// Cross-app reference, deliberately. tls_crypto.h explains why it is
// this rather than a copy or a promotion to sw/common.
#include "../../common/zsha256.h"

#include "../../ext/monocypher/monocypher.h"

#define SHA256_BLOCK 64

// -- HMAC-SHA256 ---------------------------------------------------

void tls_hmac(uint8_t out[TLS_HASH_LEN],
	const uint8_t *key, uint32_t key_len,
	const uint8_t *data, uint32_t data_len) {

	uint8_t k[SHA256_BLOCK];
	uint8_t pad[SHA256_BLOCK];
	uint8_t inner[TLS_HASH_LEN];
	z_sha256_ctx c;
	int i;

	// A key longer than the block is replaced by its hash; a shorter
	// one is zero padded. Both are RFC 2104 and both are easy to skip
	// because TLS never uses a key long enough to notice.
	memset(k, 0, sizeof(k));
	if (key_len > SHA256_BLOCK) {
		z_sha256(k, key, key_len);
	} else if (key_len) {
		memcpy(k, key, key_len);
	}

	for (i = 0; i < SHA256_BLOCK; i++) pad[i] = (uint8_t)(k[i] ^ 0x36);
	z_sha256_init(&c);
	z_sha256_update(&c, pad, SHA256_BLOCK);
	if (data_len) z_sha256_update(&c, data, data_len);
	z_sha256_final(&c, inner);

	for (i = 0; i < SHA256_BLOCK; i++) pad[i] = (uint8_t)(k[i] ^ 0x5C);
	z_sha256_init(&c);
	z_sha256_update(&c, pad, SHA256_BLOCK);
	z_sha256_update(&c, inner, TLS_HASH_LEN);
	z_sha256_final(&c, out);

	crypto_wipe(k, sizeof(k));
	crypto_wipe(pad, sizeof(pad));
	crypto_wipe(inner, sizeof(inner));

}

// -- HKDF ----------------------------------------------------------

void tls_hkdf_extract(uint8_t prk[TLS_HASH_LEN],
	const uint8_t *salt, uint32_t salt_len,
	const uint8_t *ikm, uint32_t ikm_len) {

	// Extract is HMAC with the SALT as key and the keying material as
	// data -- which is the opposite way round from how it reads, and
	// swapping them produces a schedule that is internally consistent
	// and shares no secrets with the server.
	static const uint8_t zero[TLS_HASH_LEN] = { 0 };

	if (!salt || salt_len == 0) { salt = zero; salt_len = TLS_HASH_LEN; }

	tls_hmac(prk, salt, salt_len, ikm, ikm_len);

}

bool tls_hkdf_expand(uint8_t *out, uint32_t out_len,
	const uint8_t prk[TLS_HASH_LEN],
	const uint8_t *info, uint32_t info_len) {

	uint8_t t[TLS_HASH_LEN];
	uint8_t block[TLS_HASH_LEN + 256 + 1];
	uint32_t done = 0;
	uint8_t counter = 1;
	uint32_t t_len = 0;

	if (out_len > TLS_HKDF_MAX) return false;
	if (info_len > 256) return false;

	while (done < out_len) {

		uint32_t n = 0;
		uint32_t take;

		// T(i) = HMAC(PRK, T(i-1) || info || i). T(0) is empty, which
		// is why t_len starts at zero rather than at TLS_HASH_LEN.
		if (t_len) { memcpy(block, t, t_len); n = t_len; }
		if (info_len) { memcpy(block + n, info, info_len); n += info_len; }
		block[n++] = counter;

		tls_hmac(t, prk, TLS_HASH_LEN, block, n);
		t_len = TLS_HASH_LEN;

		take = out_len - done;
		if (take > TLS_HASH_LEN) take = TLS_HASH_LEN;
		memcpy(out + done, t, take);
		done += take;
		counter++;

	}

	crypto_wipe(t, sizeof(t));
	crypto_wipe(block, sizeof(block));
	return true;

}

// -- TLS 1.3 label expansion ---------------------------------------

bool tls_expand_label(uint8_t *out, uint32_t out_len,
	const uint8_t secret[TLS_HASH_LEN], const char *label,
	const uint8_t *context, uint32_t context_len) {

	uint8_t info[2 + 1 + 255 + 1 + 255];
	uint32_t n = 0;
	uint32_t label_len = (uint32_t)strlen(label);

	if (label_len + 6 > 255) return false;
	if (context_len > 255) return false;
	if (out_len > 0xFFFF) return false;

	info[n++] = (uint8_t)(out_len >> 8);
	info[n++] = (uint8_t)(out_len & 0xFF);

	info[n++] = (uint8_t)(6 + label_len);
	memcpy(info + n, "tls13 ", 6);
	n += 6;
	memcpy(info + n, label, label_len);
	n += label_len;

	info[n++] = (uint8_t)context_len;
	if (context_len) { memcpy(info + n, context, context_len); n += context_len; }

	return tls_hkdf_expand(out, out_len, secret, info, n);

}

void tls_derive_secret(uint8_t out[TLS_HASH_LEN],
	const uint8_t secret[TLS_HASH_LEN], const char *label,
	const uint8_t transcript[TLS_HASH_LEN]) {
	tls_expand_label(out, TLS_HASH_LEN, secret, label,
		transcript, TLS_HASH_LEN);
}

// -- key schedule --------------------------------------------------

static const uint8_t zeros32[TLS_HASH_LEN] = { 0 };

// The hash of the empty string, which Derive-Secret needs for the
// "derived" steps -- those cover no transcript at all.
static void empty_hash(uint8_t out[TLS_HASH_LEN]) {
	z_sha256(out, "", 0);
}

void tls_early_secret(uint8_t out[TLS_HASH_LEN]) {
	// No PSK, so the input keying material is 32 zero bytes and the
	// salt is empty. The result is a fixed, well-known constant --
	// which is fine: it is not secret and never was.
	tls_hkdf_extract(out, NULL, 0, zeros32, TLS_HASH_LEN);
}

void tls_handshake_secret(uint8_t out[TLS_HASH_LEN],
	const uint8_t early[TLS_HASH_LEN], const uint8_t ecdhe[TLS_KEY_LEN]) {

	uint8_t derived[TLS_HASH_LEN];
	uint8_t eh[TLS_HASH_LEN];

	empty_hash(eh);
	tls_derive_secret(derived, early, "derived", eh);
	tls_hkdf_extract(out, derived, TLS_HASH_LEN, ecdhe, TLS_KEY_LEN);

	crypto_wipe(derived, sizeof(derived));

}

void tls_master_secret(uint8_t out[TLS_HASH_LEN],
	const uint8_t handshake[TLS_HASH_LEN]) {

	uint8_t derived[TLS_HASH_LEN];
	uint8_t eh[TLS_HASH_LEN];

	empty_hash(eh);
	tls_derive_secret(derived, handshake, "derived", eh);
	tls_hkdf_extract(out, derived, TLS_HASH_LEN, zeros32, TLS_HASH_LEN);

	crypto_wipe(derived, sizeof(derived));

}

void tls_traffic_keys(uint8_t key[TLS_KEY_LEN], uint8_t iv[TLS_IV_LEN],
	const uint8_t secret[TLS_HASH_LEN]) {
	tls_expand_label(key, TLS_KEY_LEN, secret, "key", NULL, 0);
	tls_expand_label(iv, TLS_IV_LEN, secret, "iv", NULL, 0);
}

void tls_finished(uint8_t out[TLS_HASH_LEN],
	const uint8_t base_secret[TLS_HASH_LEN],
	const uint8_t transcript[TLS_HASH_LEN]) {

	uint8_t fk[TLS_HASH_LEN];

	tls_expand_label(fk, TLS_HASH_LEN, base_secret, "finished", NULL, 0);
	tls_hmac(out, fk, TLS_HASH_LEN, transcript, TLS_HASH_LEN);

	crypto_wipe(fk, sizeof(fk));

}

void tls_update_traffic_secret(uint8_t secret[TLS_HASH_LEN]) {
	uint8_t next[TLS_HASH_LEN];
	tls_expand_label(next, TLS_HASH_LEN, secret, "traffic upd", NULL, 0);
	memcpy(secret, next, TLS_HASH_LEN);
	crypto_wipe(next, sizeof(next));
}

// -- record protection ---------------------------------------------

void tls_nonce(uint8_t out[TLS_IV_LEN], const uint8_t iv[TLS_IV_LEN],
	uint64_t seq) {

	int i;

	memcpy(out, iv, TLS_IV_LEN);

	// Right-aligned, big-endian. See tls_crypto.h.
	for (i = 0; i < 8; i++)
		out[TLS_IV_LEN - 1 - i] ^= (uint8_t)((seq >> (8 * i)) & 0xFF);

}

void tls_aead_seal(uint8_t *out, uint8_t tag[TLS_TAG_LEN],
	const uint8_t key[TLS_KEY_LEN], const uint8_t nonce[TLS_IV_LEN],
	const uint8_t *ad, uint32_t ad_len,
	const uint8_t *plain, uint32_t len) {

	crypto_aead_ctx ctx;

	crypto_aead_init_ietf(&ctx, key, nonce);
	crypto_aead_write(&ctx, out, tag, ad, ad_len, plain, len);
	crypto_wipe(&ctx, sizeof(ctx));

}

bool tls_aead_open(uint8_t *out,
	const uint8_t key[TLS_KEY_LEN], const uint8_t nonce[TLS_IV_LEN],
	const uint8_t *ad, uint32_t ad_len,
	const uint8_t *cipher, uint32_t len, const uint8_t tag[TLS_TAG_LEN]) {

	crypto_aead_ctx ctx;
	int rv;

	crypto_aead_init_ietf(&ctx, key, nonce);
	rv = crypto_aead_read(&ctx, out, tag, ad, ad_len, cipher, len);
	crypto_wipe(&ctx, sizeof(ctx));

	// Monocypher already refuses to write plaintext on a bad tag, but
	// wiping here as well makes the contract of this function true on
	// its own terms rather than by reference to another library's.
	if (rv != 0) { crypto_wipe(out, len); return false; }

	return true;

}
