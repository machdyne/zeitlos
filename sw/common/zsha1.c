/*
 * Zeitlos
 * Copyright (c) 2025-2026 Lone Dynamics Corporation. All rights reserved.
 *
 * SHA-1 (RFC 3174) and base64 with padding. See zsha1.h.
 *
 * Straightforward, no unrolling: it hashes a handful of bytes once per
 * connection, so size wins over speed. The message schedule is kept as
 * a 16-word circular buffer rather than 80 words, which keeps the
 * stack use at 20 words instead of 80.
 */

#include <string.h>

#include "zsha1.h"

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_block(uint32_t h[5], const uint8_t *p)
{
	uint32_t w[16];
	uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];

	for (int i = 0; i < 16; i++)
		w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
			(uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];

	for (int i = 0; i < 80; i++) {
		uint32_t f, k, t;

		if (i >= 16) {
			t = w[(i - 3) & 15] ^ w[(i - 8) & 15] ^
				w[(i - 14) & 15] ^ w[i & 15];
			w[i & 15] = ROL(t, 1);
		}
		if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
		else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
		else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
		else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
		t = ROL(a, 5) + f + e + k + w[i & 15];
		e = d; d = c; c = ROL(b, 30); b = a; a = t;
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void z_sha1_init(z_sha1_ctx *ctx)
{
	ctx->state[0] = 0x67452301;
	ctx->state[1] = 0xEFCDAB89;
	ctx->state[2] = 0x98BADCFE;
	ctx->state[3] = 0x10325476;
	ctx->state[4] = 0xC3D2E1F0;
	ctx->count = 0;
	ctx->buf_len = 0;
}

void z_sha1_update(z_sha1_ctx *ctx, const void *data, uint32_t len)
{
	const uint8_t *p = data;

	ctx->count += len;
	if (ctx->buf_len) {
		uint32_t n = Z_SHA1_BLOCK - ctx->buf_len;

		if (n > len)
			n = len;
		memcpy(ctx->buf + ctx->buf_len, p, n);
		ctx->buf_len += n;
		p += n;
		len -= n;
		if (ctx->buf_len < Z_SHA1_BLOCK)
			return;
		sha1_block(ctx->state, ctx->buf);
		ctx->buf_len = 0;
	}
	for (; len >= Z_SHA1_BLOCK; len -= Z_SHA1_BLOCK, p += Z_SHA1_BLOCK)
		sha1_block(ctx->state, p);
	memcpy(ctx->buf, p, len);
	ctx->buf_len = len;
}

void z_sha1_final(z_sha1_ctx *ctx, uint8_t out[Z_SHA1_DIGEST])
{
	uint64_t bits = ctx->count * 8;
	uint32_t n = ctx->buf_len;

	ctx->buf[n++] = 0x80;
	if (n > 56) {
		memset(ctx->buf + n, 0, Z_SHA1_BLOCK - n);
		sha1_block(ctx->state, ctx->buf);
		n = 0;
	}
	memset(ctx->buf + n, 0, 56 - n);
	for (int i = 0; i < 8; i++)
		ctx->buf[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
	sha1_block(ctx->state, ctx->buf);

	for (int i = 0; i < 5; i++) {
		out[4 * i]     = (uint8_t)(ctx->state[i] >> 24);
		out[4 * i + 1] = (uint8_t)(ctx->state[i] >> 16);
		out[4 * i + 2] = (uint8_t)(ctx->state[i] >> 8);
		out[4 * i + 3] = (uint8_t)ctx->state[i];
	}
	memset(ctx, 0, sizeof(*ctx));
}

void z_sha1(uint8_t out[Z_SHA1_DIGEST], const void *data, uint32_t len)
{
	z_sha1_ctx ctx;

	z_sha1_init(&ctx);
	z_sha1_update(&ctx, data, len);
	z_sha1_final(&ctx, out);
}

size_t z_base64_encode(char *out, const void *data, size_t len)
{
	static const char tab[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	const uint8_t *in = data;
	size_t o = 0;

	for (size_t i = 0; i < len; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16;

		if (i + 1 < len)
			v |= (uint32_t)in[i + 1] << 8;
		if (i + 2 < len)
			v |= in[i + 2];
		out[o++] = tab[(v >> 18) & 63];
		out[o++] = tab[(v >> 12) & 63];
		out[o++] = i + 1 < len ? tab[(v >> 6) & 63] : '=';
		out[o++] = i + 2 < len ? tab[v & 63] : '=';
	}
	out[o] = 0;
	return o;
}
