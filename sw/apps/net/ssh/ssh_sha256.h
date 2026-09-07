#ifndef SSH_SHA256_H
#define SSH_SHA256_H

/*
 * Zeitlos
 * Copyright (c) 2025-2026 Lone Dynamics Corporation. All rights reserved.
 *
 * THIS IS A FORWARDING SHIM. The implementation moved to
 * sw/common/zsha256.c when sw/apps/web needed SHA-256 for TLS 1.3 and
 * two apps ended up compiling the same file out of ssh's private
 * directory.
 *
 * The old names are kept working here rather than renamed at their 35
 * call sites across ssh_crypto.c, ssh_proto.c and sw/test/. Those are
 * a working SSH client; changing them would be a mechanical edit with
 * no upside and a real chance of a typo in code that is hard to test.
 *
 * New code should include sw/common/zsha256.h and use the z_ names
 * directly. Nothing here is deprecated in the sense of going away --
 * it costs one header and no instructions, since the wrappers below
 * are static inline and compile to nothing.
 *
 * ssh_sha256.c is now an empty stub and can be deleted.
 */

#include "../../../common/zsha256.h"

#define SSH_SHA256_DIGEST Z_SHA256_DIGEST
#define SSH_SHA256_BLOCK  Z_SHA256_BLOCK

typedef z_sha256_ctx ssh_sha256_ctx;

static inline void ssh_sha256_init(ssh_sha256_ctx *ctx) {
	z_sha256_init(ctx);
}

static inline void ssh_sha256_update(ssh_sha256_ctx *ctx,
	const void *data, uint32_t len) {
	z_sha256_update(ctx, data, len);
}

static inline void ssh_sha256_final(ssh_sha256_ctx *ctx,
	uint8_t out[SSH_SHA256_DIGEST]) {
	z_sha256_final(ctx, out);
}

static inline void ssh_sha256(uint8_t out[SSH_SHA256_DIGEST],
	const void *data, uint32_t len) {
	z_sha256(out, data, len);
}

#endif
