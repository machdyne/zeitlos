/*
 * Zeitlos OS
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See authcore.h.
 */
#include <string.h>
#include "authcore.h"

// HMAC keys longer than a block are hashed first (RFC 2104); shorter
// ones are zero-padded. Returns the inner and outer contexts already
// fed their pads, so each PBKDF2 iteration is two compressions rather
// than four -- the standard precomputation, and the whole difference
// between a usable and an unusable iteration count on this CPU.
static void hmac_keys(const uint8_t *key, uint32_t keylen,
		z_sha256_ctx *inner, z_sha256_ctx *outer) {
	uint8_t k[Z_SHA256_BLOCK], pad[Z_SHA256_BLOCK];
	uint32_t i;
	memset(k, 0, sizeof(k));
	if (keylen > Z_SHA256_BLOCK) {
		z_sha256_ctx c;
		z_sha256_init(&c);
		z_sha256_update(&c, key, keylen);
		z_sha256_final(&c, k);
	} else if (keylen)
		memcpy(k, key, keylen);
	for (i = 0; i < Z_SHA256_BLOCK; i++) pad[i] = k[i] ^ 0x36;
	z_sha256_init(inner);
	z_sha256_update(inner, pad, Z_SHA256_BLOCK);
	for (i = 0; i < Z_SHA256_BLOCK; i++) pad[i] = k[i] ^ 0x5c;
	z_sha256_init(outer);
	z_sha256_update(outer, pad, Z_SHA256_BLOCK);
	memset(k, 0, sizeof(k));
	memset(pad, 0, sizeof(pad));
}

static void hmac_finish(const z_sha256_ctx *inner0, const z_sha256_ctx *outer0,
		const uint8_t *msg, uint32_t len, uint8_t out[AUTH_DK_LEN]) {
	z_sha256_ctx c = *inner0;
	uint8_t ih[Z_SHA256_DIGEST];
	z_sha256_update(&c, msg, len);
	z_sha256_final(&c, ih);
	c = *outer0;
	z_sha256_update(&c, ih, sizeof(ih));
	z_sha256_final(&c, out);
}

void auth_pbkdf2(const uint8_t *pw, uint32_t pwlen, const uint8_t *salt,
		uint32_t saltlen, uint32_t iterations, uint8_t out[AUTH_DK_LEN]) {
	z_sha256_ctx inner, outer, c;
	uint8_t u[AUTH_DK_LEN], ih[Z_SHA256_DIGEST];
	static const uint8_t block1[4] = { 0, 0, 0, 1 };
	uint32_t i, k;

	hmac_keys(pw, pwlen, &inner, &outer);

	// U1 = HMAC(P, S || INT(1))
	c = inner;
	z_sha256_update(&c, salt, saltlen);
	z_sha256_update(&c, block1, 4);
	z_sha256_final(&c, ih);
	c = outer;
	z_sha256_update(&c, ih, sizeof(ih));
	z_sha256_final(&c, u);
	memcpy(out, u, AUTH_DK_LEN);

	// U_i = HMAC(P, U_{i-1}); T = U1 ^ U2 ^ ...
	for (i = 1; i < iterations; i++) {
		hmac_finish(&inner, &outer, u, AUTH_DK_LEN, u);
		for (k = 0; k < AUTH_DK_LEN; k++) out[k] ^= u[k];
	}
	memset(u, 0, sizeof(u));
	memset(&inner, 0, sizeof(inner));
	memset(&outer, 0, sizeof(outer));
	memset(&c, 0, sizeof(c));
}

void auth_rec_pack(const auth_rec_t *r, uint8_t out[AUTH_REC_LEN]) {
	out[0] = AUTH_REC_VERSION;
	out[1] = r->flags;
	out[2] = (uint8_t)r->iterations;
	out[3] = (uint8_t)(r->iterations >> 8);
	out[4] = (uint8_t)(r->iterations >> 16);
	out[5] = (uint8_t)(r->iterations >> 24);
	memcpy(out + 6, r->salt, AUTH_SALT_LEN);
	memcpy(out + 6 + AUTH_SALT_LEN, r->dk, AUTH_DK_LEN);
}

int auth_rec_unpack(const uint8_t *in, uint32_t len, auth_rec_t *r) {
	if (len != AUTH_REC_LEN || in[0] != AUTH_REC_VERSION) return -1;
	r->flags = in[1];
	r->iterations = (uint32_t)in[2] | ((uint32_t)in[3] << 8) |
		((uint32_t)in[4] << 16) | ((uint32_t)in[5] << 24);
	if (r->iterations == 0) return -1;
	memcpy(r->salt, in + 6, AUTH_SALT_LEN);
	memcpy(r->dk, in + 6 + AUTH_SALT_LEN, AUTH_DK_LEN);
	return 0;
}

int auth_eq(const uint8_t *a, const uint8_t *b, uint32_t n) {
	uint8_t d = 0;
	uint32_t i;
	for (i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
	return d == 0;
}

uint32_t auth_backoff_ms(uint32_t fails) {
	if (fails <= 3) return 0;
	if (fails - 4 >= 6) return 60000;       // 1, 2, 4, 8, 16, 32 s, then 60
	return 1000u << (fails - 4);
}
