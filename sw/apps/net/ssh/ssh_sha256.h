#ifndef SSH_SHA256_H
#define SSH_SHA256_H

#include <stdint.h>
#include <stddef.h>

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * SHA-256 (FIPS 180-4).
 *
 * -- Why this file exists at all --
 *
 * sw/ext/monocypher provides BLAKE2b and SHA-512 and NO SHA-256, and
 * SSH needs SHA-256 in two places that are not optional: the
 * `curve25519-sha256` exchange hash (RFC 8731) and the key derivation
 * in RFC 4253 section 7.2. So it is supplied here.
 *
 * Deliberately NOT added to sw/ext/monocypher/, even though that is
 * where it would sit most naturally. That directory is a vendored,
 * unmodified, hash-checkable copy of an upstream release (see its
 * README.md), and the moment anything local lives in it, the next
 * person to update Monocypher either loses this silently or has to
 * re-merge it by hand.
 *
 * -- Streaming, because the exchange hash needs it --
 *
 * The incremental API is not decoration. The SSH exchange hash covers
 * V_C || V_S || I_C || I_S || K_S || Q_C || Q_S || K, and I_S (the
 * server's KEXINIT) can be well over a kilobyte. Feeding it in as it
 * is parsed means never storing a second copy -- see ssh_kex.c, which
 * hashes V_C, V_S and I_C the moment our own KEXINIT goes out, long
 * before the server's reply arrives.
 */

#define SSH_SHA256_DIGEST 32
#define SSH_SHA256_BLOCK  64

typedef struct {
	uint32_t state[8];
	uint64_t count;					// total bytes fed, for the length field
	uint8_t  buf[SSH_SHA256_BLOCK];
	uint32_t buf_len;
} ssh_sha256_ctx;

void ssh_sha256_init(ssh_sha256_ctx *ctx);
void ssh_sha256_update(ssh_sha256_ctx *ctx, const void *data, uint32_t len);
void ssh_sha256_final(ssh_sha256_ctx *ctx, uint8_t out[SSH_SHA256_DIGEST]);

// One-shot convenience -- same result as init/update/final.
void ssh_sha256(uint8_t out[SSH_SHA256_DIGEST], const void *data, uint32_t len);

#endif
