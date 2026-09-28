/*
 * Zeitlos
 * Copyright (c) 2025-2026 Lone Dynamics Corporation. All rights reserved.
 *
 * SHA-256 (FIPS 180-4).
 *
 * Moved here from sw/apps/net/ssh/ssh_sha256.c when a second consumer
 * appeared: sw/apps/web needs it for TLS 1.3 (HMAC, HKDF, and the
 * handshake transcript). The implementation below is that file
 * unchanged apart from its names.
 *
 * The old header remains as a forwarding shim so that nothing in
 * ssh/ had to be touched -- see sw/apps/net/ssh/ssh_sha256.h.
 *
 * -- on the reference implementation --
 *
 * Straightforward, no unrolling. That was a deliberate size/speed
 * choice for SSH -- a few kilobytes per handshake and then never
 * again, since the bulk cipher carries no hash -- and it survives the
 * move for the same reason. TLS 1.3 uses SHA-256 harder than SSH did
 * (every handshake message goes through the transcript, and HKDF runs
 * it a few dozen times) but that is still milliseconds once per
 * connection, against ChaCha20-Poly1305 doing the actual bulk work.
 *
 * Unrolling the compression function would cost several KB of flash
 * on a machine where `web` is already the largest app. Compare
 * BLAKE2b in Monocypher, which IS fully unrolled at 23.5KB --
 * appropriate there, wrong here.
 *
 * If rtl/ ever grows a SHA-256 core, this file is the single place
 * that changes.
 *
 * -- the hardware (rtl/sha256.v, docs/sha256_hw.md) --
 *
 * It did grow one. Where the bitstream has it, a block goes through
 * the hardware in ~150 cycles instead of ~44,000 in software, and
 * every caller -- the kernel's auth, SSH, TLS, the BBS, zfed -- gets
 * that without a line changed. Where it has not, or another process
 * holds it, the software below runs as it always did.
 *
 * The block keeps nothing across calls that this file needs: each
 * z_sha256_update() that has whole blocks to compress claims the
 * block, loads H from the context, pushes the blocks, reads H back,
 * ZEROES H, and releases. So losing the block between two calls --
 * to preemption, to another process -- costs nothing, and the next
 * owner cannot read anything this caller hashed: H after a block is
 * derived from its input, and for HMAC that input is the key.
 */

#include <string.h>

#include "zsha256.h"

// -- the hardware's registers --
//
// Through two functions, so a host test can put the real RTL behind
// them (sw/common/tests/test_zsha256_hw.cpp, Verilator). On the
// target they are plain MMIO; anywhere else the hardware path is
// compiled out unless the test asks for it.
#if defined(Z_SHA256_HW_TEST)
#define ZSHA_HW 1
uint32_t zsha_hw_rd(uint32_t word);
void zsha_hw_wr(uint32_t word, uint32_t v);
uint32_t zsha_hw_owner_id(void);
static int zsha_hw_present(void) { return 1; }
#elif defined(__riscv) && !defined(Z_SHA256_NO_HW)
#define ZSHA_HW 1
#include "zsoc.h"
#include "zeitlos.h"
static inline uint32_t zsha_hw_rd(uint32_t w) {
	return *(volatile uint32_t *)(uintptr_t)(Z_SHA256_BASE + 4u * w);
}
static inline void zsha_hw_wr(uint32_t w, uint32_t v) {
	*(volatile uint32_t *)(uintptr_t)(Z_SHA256_BASE + 4u * w) = v;
}
static int zsha_hw_present(void) {
	return z_soc_has_feature2(Z_FEATURE2_SHA256) &&
		zsha_hw_rd(Z_SHA256_W_MAGIC) == Z_SHA256_MAGIC;
}
// Who is claiming: the kernel (built with Z_KERNEL_BUILD, sw/os/Makefile)
// has no pid and uses a fixed id; a process uses its pid, asked once.
static uint32_t zsha_hw_owner_id(void) {
#ifdef Z_KERNEL_BUILD
	return Z_HW_OWNER_KERNEL;
#else
	static uint32_t pid;
	if (!pid) pid = z_getpid() & 0x7FFFFFFFu;
	return pid ? pid : 0x7FFFFFFEu;
#endif
}
#endif

#ifdef ZSHA_HW
#define ZSHA_W_CTRL   1u
#define ZSHA_W_OWNER  3u
#define ZSHA_W_PUSH   4u
#define ZSHA_W_H      8u
#define ZSHA_RELEASE  0x80000000u

static int zsha_hw_state = -1;		// -1 not asked yet, 0 absent, 1 present

// Claims the block and loads ctx's state into it. False: use software.
static int hw_begin(const uint32_t state[8]) {
	uint32_t me;
	if (zsha_hw_state < 0) zsha_hw_state = zsha_hw_present() ? 1 : 0;
	if (!zsha_hw_state) return 0;
	me = zsha_hw_owner_id();
	zsha_hw_wr(ZSHA_W_OWNER, me);
	if (zsha_hw_rd(ZSHA_W_OWNER) != me) return 0;		// someone else has it
	for (int i = 0; i < 8; i++) zsha_hw_wr(ZSHA_W_H + (uint32_t)i, state[i]);
	return 1;
}

// One block through the hardware. Words go in as a little-endian CPU
// loads them; the block swaps them to SHA-256's big-endian order.
static void hw_block(const uint8_t *p) {
	if (((uintptr_t)p & 3u) == 0) {
		const uint32_t *w = (const uint32_t *)(const void *)p;
		for (int i = 0; i < 16; i++) zsha_hw_wr(ZSHA_W_PUSH, w[i]);
	} else {
		for (int i = 0; i < 16; i++, p += 4)
			zsha_hw_wr(ZSHA_W_PUSH, (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
				((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
	}
	zsha_hw_wr(ZSHA_W_CTRL, 1);
	// ~144 cycles. Pushes are ignored while it is busy, so the next
	// block has to wait for this.
	while (zsha_hw_rd(ZSHA_W_CTRL) & 1u) { }
}

// Reads the state back, clears the block's copy, and releases it.
static void hw_end(uint32_t state[8]) {
	for (int i = 0; i < 8; i++) {
		state[i] = zsha_hw_rd(ZSHA_W_H + (uint32_t)i);
		zsha_hw_wr(ZSHA_W_H + (uint32_t)i, 0);
	}
	zsha_hw_wr(ZSHA_W_OWNER, zsha_hw_owner_id() | ZSHA_RELEASE);
}
#endif

static const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
	0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
	0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
	0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
	0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
	0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

// RV32IM has no rotate instruction, so this is three instructions, not
// one. It is the hot operation in the compression function below; a
// `rori` from the Zbb extension would cut this file's cost by roughly a
// third. See docs/trng.md's note on the same issue in ChaCha20, where
// it matters considerably more.
#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROTR(x, 2)  ^ ROTR(x, 13) ^ ROTR(x, 22))
#define BSIG1(x) (ROTR(x, 6)  ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SSIG0(x) (ROTR(x, 7)  ^ ROTR(x, 18) ^ ((x) >> 3))
#define SSIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static void compress(z_sha256_ctx *ctx, const uint8_t *block) {

	uint32_t w[64];
	uint32_t a, b, c, d, e, f, g, h, t1, t2;
	int i;

	// Big endian, per the standard -- SSH is big endian throughout, so
	// this file never sees a little-endian byte order anywhere.
	for (i = 0; i < 16; i++)
		w[i] = ((uint32_t)block[4 * i] << 24) |
			   ((uint32_t)block[4 * i + 1] << 16) |
			   ((uint32_t)block[4 * i + 2] << 8) |
			   ((uint32_t)block[4 * i + 3]);

	for (i = 16; i < 64; i++)
		w[i] = SSIG1(w[i - 2]) + w[i - 7] + SSIG0(w[i - 15]) + w[i - 16];

	a = ctx->state[0]; b = ctx->state[1];
	c = ctx->state[2]; d = ctx->state[3];
	e = ctx->state[4]; f = ctx->state[5];
	g = ctx->state[6]; h = ctx->state[7];

	for (i = 0; i < 64; i++) {
		t1 = h + BSIG1(e) + CH(e, f, g) + K[i] + w[i];
		t2 = BSIG0(a) + MAJ(a, b, c);
		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}

	ctx->state[0] += a; ctx->state[1] += b;
	ctx->state[2] += c; ctx->state[3] += d;
	ctx->state[4] += e; ctx->state[5] += f;
	ctx->state[6] += g; ctx->state[7] += h;

	// w[] held message schedule derived from whatever was hashed. For
	// the exchange hash that includes the shared secret K, so this is
	// not ceremonial.
	memset(w, 0, sizeof(w));

}

void z_sha256_init(z_sha256_ctx *ctx) {
	ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
	ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
	ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
	ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
	ctx->count = 0;
	ctx->buf_len = 0;
}

void z_sha256_update(z_sha256_ctx *ctx, const void *data, uint32_t len) {

	const uint8_t *p = (const uint8_t *)data;
	uint32_t n;
	int hw = 0;

	ctx->count += len;

#ifdef ZSHA_HW
	// The hardware, if this call compresses at least one block.
	if (ctx->buf_len + len >= Z_SHA256_BLOCK) hw = hw_begin(ctx->state);
#endif

	// Top up a partial buffer first, then take whole blocks straight
	// from the caller's memory without copying, then keep the tail.
	if (ctx->buf_len) {
		n = Z_SHA256_BLOCK - ctx->buf_len;
		if (n > len) n = len;
		memcpy(ctx->buf + ctx->buf_len, p, n);
		ctx->buf_len += n;
		p += n;
		len -= n;
		if (ctx->buf_len == Z_SHA256_BLOCK) {
#ifdef ZSHA_HW
			if (hw) hw_block(ctx->buf); else
#endif
			compress(ctx, ctx->buf);
			ctx->buf_len = 0;
		}
	}

	while (len >= Z_SHA256_BLOCK) {
#ifdef ZSHA_HW
		if (hw) hw_block(p); else
#endif
		compress(ctx, p);
		p += Z_SHA256_BLOCK;
		len -= Z_SHA256_BLOCK;
	}

#ifdef ZSHA_HW
	if (hw) hw_end(ctx->state);
#else
	(void)hw;
#endif

	if (len) {
		memcpy(ctx->buf, p, len);
		ctx->buf_len = len;
	}

}

void z_sha256_final(z_sha256_ctx *ctx, uint8_t out[Z_SHA256_DIGEST]) {

	uint64_t bits = ctx->count * 8;
	uint8_t pad[Z_SHA256_BLOCK * 2];
	uint32_t padlen;
	int i;

	// 0x80, then zeros, then a 64-bit big-endian bit count, landing on
	// a block boundary. Needs a second block whenever the message tail
	// leaves fewer than 9 bytes of room.
	memset(pad, 0, sizeof(pad));
	pad[0] = 0x80;

	padlen = (ctx->buf_len < 56)
		? (56 - ctx->buf_len)
		: (120 - ctx->buf_len);

	for (i = 0; i < 8; i++)
		pad[padlen + i] = (uint8_t)(bits >> (56 - 8 * i));

	z_sha256_update(ctx, pad, padlen + 8);

	for (i = 0; i < 8; i++) {
		out[4 * i]     = (uint8_t)(ctx->state[i] >> 24);
		out[4 * i + 1] = (uint8_t)(ctx->state[i] >> 16);
		out[4 * i + 2] = (uint8_t)(ctx->state[i] >> 8);
		out[4 * i + 3] = (uint8_t)(ctx->state[i]);
	}

	memset(ctx, 0, sizeof(*ctx));

}

void z_sha256(uint8_t out[Z_SHA256_DIGEST], const void *data, uint32_t len) {
	z_sha256_ctx ctx;
	z_sha256_init(&ctx);
	z_sha256_update(&ctx, data, len);
	z_sha256_final(&ctx, out);
}
