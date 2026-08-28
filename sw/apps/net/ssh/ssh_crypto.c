/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * chacha20-poly1305@openssh.com and RFC 4253 key derivation. See
 * ssh_crypto.h for the construction and for why nothing is trusted
 * before the tag verifies.
 */

#include <string.h>

#include "ssh_crypto.h"
#include "../../../ext/monocypher/monocypher.h"

void ssh_cipher_init(ssh_cipher *c) {
	memset(c, 0, sizeof(*c));
}

void ssh_cipher_wipe(ssh_cipher *c) {
	crypto_wipe(c->key, sizeof(c->key));
	memset(c, 0, sizeof(*c));
}

void ssh_cipher_set_key(ssh_cipher *c, const uint8_t key[SSH_AEAD_KEY_LEN]) {
	memcpy(c->key, key, SSH_AEAD_KEY_LEN);
	c->active = true;
	// seq deliberately untouched -- see ssh_crypto.h.
}

// The sequence number becomes the 8-byte ChaCha20 nonce, big endian.
static void nonce_of(uint64_t seq, uint8_t nonce[8]) {
	int i;
	for (i = 0; i < 8; i++) nonce[i] = (uint8_t)(seq >> (56 - 8 * i));
}

// K_2 is the first half of the key, K_1 the second. Getting these the
// wrong way round produces a cipher that is internally consistent and
// completely incompatible with OpenSSH, which is a miserable thing to
// debug against a live server -- the handshake completes and then the
// first real packet fails its tag.
#define K2(c) ((c)->key)
#define K1(c) ((c)->key + 32)

uint32_t ssh_aead_peek_len(const ssh_cipher *c, const uint8_t enc_len[4]) {

	uint8_t plain[4];

	if (!c->active) {
		// Before NEWKEYS everything is plaintext.
		return ((uint32_t)enc_len[0] << 24) | ((uint32_t)enc_len[1] << 16) |
			   ((uint32_t)enc_len[2] << 8) | ((uint32_t)enc_len[3]);
	}

	uint8_t nonce[8];
	nonce_of(c->seq, nonce);

	crypto_chacha20_djb(plain, enc_len, 4, K1(c), nonce, 0);

	return ((uint32_t)plain[0] << 24) | ((uint32_t)plain[1] << 16) |
		   ((uint32_t)plain[2] << 8) | ((uint32_t)plain[3]);

}

bool ssh_aead_open(ssh_cipher *c, uint8_t *pkt, uint32_t payload_len) {

	uint8_t nonce[8];
	uint8_t poly_key[64];
	uint8_t tag[SSH_AEAD_TAG_LEN];
	crypto_poly1305_ctx pctx;
	uint8_t *payload = pkt + SSH_AEAD_LEN_LEN;
	const uint8_t *recv_tag = payload + payload_len;
	bool ok;

	if (!c->active) {
		// Plaintext phase: the length is already clear and there is no
		// tag on the wire at all.
		c->seq++;
		return true;
	}

	nonce_of(c->seq, nonce);

	// Poly1305 key = first 32 bytes of the K_2 keystream at counter 0.
	// The rest of that block is discarded, which is why the payload
	// starts at counter 1 rather than continuing the stream.
	crypto_chacha20_djb(poly_key, 0, 64, K2(c), nonce, 0);

	// Tag covers the ENCRYPTED length bytes and the ENCRYPTED payload,
	// computed before any decryption happens.
	crypto_poly1305_init(&pctx, poly_key);
	crypto_poly1305_update(&pctx, pkt, SSH_AEAD_LEN_LEN);
	crypto_poly1305_update(&pctx, payload, payload_len);
	crypto_poly1305_final(&pctx, tag);

	// Constant time. A memcmp here would leak the tag through timing
	// and permit byte-at-a-time forgery.
	ok = (crypto_verify16(tag, recv_tag) == 0);

	if (ok)
		crypto_chacha20_djb(payload, payload, payload_len, K2(c), nonce, 1);

	crypto_wipe(poly_key, sizeof(poly_key));
	crypto_wipe(tag, sizeof(tag));

	// Advanced even on failure: a bad tag terminates the session, so
	// there is no retry path that would want the old value.
	c->seq++;

	return ok;

}

void ssh_aead_seal(ssh_cipher *c, uint8_t *pkt, uint32_t payload_len) {

	uint8_t nonce[8];
	uint8_t poly_key[64];
	crypto_poly1305_ctx pctx;
	uint8_t *payload = pkt + SSH_AEAD_LEN_LEN;
	uint8_t *tag = payload + payload_len;

	if (!c->active) {
		c->seq++;
		return;
	}

	nonce_of(c->seq, nonce);

	crypto_chacha20_djb(poly_key, 0, 64, K2(c), nonce, 0);

	// Length with K_1 at counter 0, payload with K_2 at counter 1.
	crypto_chacha20_djb(pkt, pkt, SSH_AEAD_LEN_LEN, K1(c), nonce, 0);
	crypto_chacha20_djb(payload, payload, payload_len, K2(c), nonce, 1);

	crypto_poly1305_init(&pctx, poly_key);
	crypto_poly1305_update(&pctx, pkt, SSH_AEAD_LEN_LEN);
	crypto_poly1305_update(&pctx, payload, payload_len);
	crypto_poly1305_final(&pctx, tag);

	crypto_wipe(poly_key, sizeof(poly_key));

	c->seq++;

}

void ssh_derive_key(uint8_t out[SSH_AEAD_KEY_LEN],
	const uint8_t *k_mpint, uint32_t k_mpint_len,
	const uint8_t h[SSH_SHA256_DIGEST], char letter,
	const uint8_t session_id[SSH_SHA256_DIGEST]) {

	ssh_sha256_ctx ctx;
	uint8_t k1[SSH_SHA256_DIGEST];
	uint8_t k2[SSH_SHA256_DIGEST];
	uint8_t l = (uint8_t)letter;

	// K_1 = HASH(K || H || X || session_id)
	ssh_sha256_init(&ctx);
	ssh_sha256_update(&ctx, k_mpint, k_mpint_len);
	ssh_sha256_update(&ctx, h, SSH_SHA256_DIGEST);
	ssh_sha256_update(&ctx, &l, 1);
	ssh_sha256_update(&ctx, session_id, SSH_SHA256_DIGEST);
	ssh_sha256_final(&ctx, k1);

	// K_2 = HASH(K || H || K_1), giving 64 bytes total. This cipher
	// wants exactly 64, so one extension round is all that ever runs.
	ssh_sha256_init(&ctx);
	ssh_sha256_update(&ctx, k_mpint, k_mpint_len);
	ssh_sha256_update(&ctx, h, SSH_SHA256_DIGEST);
	ssh_sha256_update(&ctx, k1, SSH_SHA256_DIGEST);
	ssh_sha256_final(&ctx, k2);

	memcpy(out, k1, SSH_SHA256_DIGEST);
	memcpy(out + SSH_SHA256_DIGEST, k2, SSH_SHA256_DIGEST);

	crypto_wipe(k1, sizeof(k1));
	crypto_wipe(k2, sizeof(k2));

}

void ssh_fingerprint(char *out, uint32_t out_cap,
	const uint8_t *keyblob, uint32_t len) {

	static const char b64[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	uint8_t d[SSH_SHA256_DIGEST];
	uint32_t i, o;

	if (out_cap < 56) { if (out_cap) out[0] = 0; return; }

	ssh_sha256(d, keyblob, len);

	memcpy(out, "SHA256:", 7);
	o = 7;

	// Unpadded base64, as ssh-keygen -lf prints it. 32 bytes is 10
	// full 3-byte groups plus a 2-byte remainder, so the tail case
	// below always runs and always emits exactly 3 characters.
	for (i = 0; i + 2 < SSH_SHA256_DIGEST; i += 3) {
		out[o++] = b64[d[i] >> 2];
		out[o++] = b64[((d[i] & 0x03) << 4) | (d[i + 1] >> 4)];
		out[o++] = b64[((d[i + 1] & 0x0f) << 2) | (d[i + 2] >> 6)];
		out[o++] = b64[d[i + 2] & 0x3f];
	}

	if (i < SSH_SHA256_DIGEST) {
		out[o++] = b64[d[i] >> 2];
		if (i + 1 < SSH_SHA256_DIGEST) {
			out[o++] = b64[((d[i] & 0x03) << 4) | (d[i + 1] >> 4)];
			out[o++] = b64[(d[i + 1] & 0x0f) << 2];
		} else {
			out[o++] = b64[(d[i] & 0x03) << 4];
		}
	}

	out[o] = 0;

}
