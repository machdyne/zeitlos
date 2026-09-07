#ifndef ZSHA256_H
#define ZSHA256_H

#include <stdint.h>
#include <stddef.h>

/*
 * Zeitlos
 * Copyright (c) 2025-2026 Lone Dynamics Corporation. All rights reserved.
 *
 * SHA-256 (FIPS 180-4).
 *
 * -- why this is here and not in sw/ext/monocypher --
 *
 * Monocypher provides BLAKE2b and SHA-512 and NO SHA-256. That
 * directory is a vendored, unmodified, hash-checkable copy of an
 * upstream release (see its README.md), and the moment anything local
 * lives in it, the next person to update Monocypher either loses this
 * silently or has to re-merge it by hand.
 *
 * -- why it is in sw/common rather than in an app --
 *
 * It started in sw/apps/net/ssh/, which was right when SSH was the
 * only thing that needed it: the `curve25519-sha256` exchange hash
 * (RFC 8731) and the key derivation of RFC 4253 section 7.2.
 *
 * sw/apps/web is the second consumer -- TLS 1.3 uses SHA-256 for the
 * handshake transcript, for HMAC, and for the whole HKDF key schedule
 * (docs/tls.md). Two apps compiling the same file out of a third
 * app's private directory is the point at which it stops being an SSH
 * detail.
 *
 * sw/apps/net/ssh/ssh_sha256.h remains as a forwarding shim, so
 * nothing in ssh/ had to change. See that file.
 *
 * -- a hardware implementation would land here --
 *
 * This interface is the seam. A SHA-256 core in rtl/ would replace
 * zsha256.c and nothing else: the streaming API below is what an
 * accelerator wants anyway (feed blocks, read the digest), and both
 * consumers already use it that way rather than assuming a one-shot.
 *
 * -- streaming, because both consumers need it --
 *
 * Not decoration in either case. SSH's exchange hash covers
 * V_C || V_S || I_C || I_S || K_S || Q_C || Q_S || K, and I_S can be
 * well over a kilobyte; ssh_kex.c hashes the early parts the moment
 * its own KEXINIT goes out. TLS's transcript covers every handshake
 * message in order, including a certificate chain far too large to
 * buffer, and sw/apps/web/tls.c hashes it as it streams past.
 *
 * The context is a flat POD with no pointers, so it can be COPIED --
 * which TLS relies on: a Finished message's verify_data covers the
 * transcript up to but not including itself, so tls.c keeps a
 * snapshot taken before each message.
 */

#define Z_SHA256_DIGEST 32
#define Z_SHA256_BLOCK  64

typedef struct {
	uint32_t state[8];
	uint64_t count;					// total bytes fed, for the length field
	uint8_t  buf[Z_SHA256_BLOCK];
	uint32_t buf_len;
} z_sha256_ctx;

void z_sha256_init(z_sha256_ctx *ctx);
void z_sha256_update(z_sha256_ctx *ctx, const void *data, uint32_t len);
void z_sha256_final(z_sha256_ctx *ctx, uint8_t out[Z_SHA256_DIGEST]);

// One-shot convenience -- same result as init/update/final.
void z_sha256(uint8_t out[Z_SHA256_DIGEST], const void *data, uint32_t len);

#endif
