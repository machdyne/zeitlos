#ifndef ZSHA1_H
#define ZSHA1_H

#include <stdint.h>
#include <stddef.h>

/*
 * Zeitlos
 * Copyright (c) 2025-2026 Lone Dynamics Corporation. All rights reserved.
 *
 * SHA-1 (RFC 3174) and a base64 encoder with padding (RFC 4648 section 4).
 *
 * -- why SHA-1 at all --
 *
 * Not for security: SHA-1 is broken for anything that needs collision
 * resistance, and nothing here should use it for that. It is here
 * because a protocol still asks for it -- the WebSocket opening
 * handshake (RFC 6455 section 4.2.2) answers a client's key with
 * base64(SHA-1(key || GUID)). Use zsha256.h for anything new.
 *
 * -- why not a copy of zsha256's interface with the hardware behind it --
 *
 * The same init/update/final shape, so a caller reads the same, but it
 * is software only: it is run once per connection, on 60 bytes, and
 * there is no SHA-1 core in rtl/ to claim. No tables, no malloc; the
 * context is a flat POD with no pointers and can be copied.
 *
 * -- why base64 is in this header --
 *
 * The handshake needs the encoder and nothing else in sw/common
 * exports one (ssh_crypto.c and authkeys.c each keep a private
 * decoder). It is general -- any length -- because the general
 * version is no bigger than one fixed at 20 bytes would be.
 */

#define Z_SHA1_DIGEST 20
#define Z_SHA1_BLOCK  64

typedef struct {
	uint32_t state[5];
	uint64_t count;					// total bytes fed, for the length field
	uint8_t  buf[Z_SHA1_BLOCK];
	uint32_t buf_len;
} z_sha1_ctx;

void z_sha1_init(z_sha1_ctx *ctx);
void z_sha1_update(z_sha1_ctx *ctx, const void *data, uint32_t len);
void z_sha1_final(z_sha1_ctx *ctx, uint8_t out[Z_SHA1_DIGEST]);

// One-shot convenience -- same result as init/update/final.
void z_sha1(uint8_t out[Z_SHA1_DIGEST], const void *data, uint32_t len);

// Bytes z_base64_encode() writes for n input bytes, NUL included.
#define Z_BASE64_SIZE(n) (4 * (((n) + 2) / 3) + 1)

// Encodes with '=' padding and NUL-terminates. `out` must hold
// Z_BASE64_SIZE(len) bytes. Returns the length of the text, without
// the NUL.
size_t z_base64_encode(char *out, const void *data, size_t len);

#endif
