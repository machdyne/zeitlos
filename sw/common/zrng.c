/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * The system CSPRNG. See zrng.h for the API and the rules about when
 * its output is fit to be a key; see docs/trng.md for the hardware
 * underneath and the whole design.
 *
 * ChaCha20 in fast key erasure form, which is the same construction
 * OpenBSD's arc4random and Linux's get_random_bytes use:
 *
 *   every generate call produces 32 bytes of keystream for the NEXT
 *   key plus however many bytes the caller asked for, then overwrites
 *   the key with those 32 bytes
 *
 * The property that buys: an attacker who reads this process's memory
 * at time T learns nothing about bytes handed out before T, because
 * the key that produced them no longer exists anywhere. It costs one
 * extra ChaCha block per call.
 *
 * ChaCha20 is implemented here rather than pulled from sw/ext/
 * monocypher deliberately. This file is linked by ordinary apps that
 * want `(random)` and nothing else, and making every one of them
 * depend on a 23KB crypto library for 1.5KB of stream cipher would be
 * a bad trade. The SSH client links both; they do not conflict.
 */

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "zrng.h"

// -- ChaCha20 --

static uint32_t cc_state[16];
static uint8_t cc_key[32];
static uint64_t cc_nonce;			// never repeats within a boot; see below

// Rotate left. Written as a shift pair rather than an intrinsic
// because RV32IM has no rotate instruction -- this compiles to the
// three instructions it has to compile to, and this is the hot loop
// of the whole file. (An `rori` from the Zbb extension would cut
// ChaCha20's cost here by roughly 40%; see docs/trng.md's performance
// note.)
#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

#define QR(a, b, c, d) \
	a += b; d ^= a; d = ROTL32(d, 16); \
	c += d; b ^= c; b = ROTL32(b, 12); \
	a += b; d ^= a; d = ROTL32(d, 8);  \
	c += d; b ^= c; b = ROTL32(b, 7);

static uint32_t rd32le(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32le(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// One 64-byte ChaCha20 block from cc_state into `out`.
static void chacha_block(uint8_t *out) {

	uint32_t x[16];
	int i;

	for (i = 0; i < 16; i++) x[i] = cc_state[i];

	for (i = 0; i < 10; i++) {
		QR(x[0], x[4], x[ 8], x[12])
		QR(x[1], x[5], x[ 9], x[13])
		QR(x[2], x[6], x[10], x[14])
		QR(x[3], x[7], x[11], x[15])
		QR(x[0], x[5], x[10], x[15])
		QR(x[1], x[6], x[11], x[12])
		QR(x[2], x[7], x[ 8], x[13])
		QR(x[3], x[4], x[ 9], x[14])
	}

	for (i = 0; i < 16; i++) wr32le(out + 4 * i, x[i] + cc_state[i]);

}

// Load cc_key/cc_nonce into cc_state with the block counter at zero.
static void chacha_setup(void) {

	int i;

	// "expand 32-byte k"
	cc_state[0] = 0x61707865; cc_state[1] = 0x3320646e;
	cc_state[2] = 0x79622d32; cc_state[3] = 0x6b206574;

	for (i = 0; i < 8; i++) cc_state[4 + i] = rd32le(cc_key + 4 * i);

	cc_state[12] = 0;						// block counter
	cc_state[13] = 0;
	cc_state[14] = (uint32_t)cc_nonce;
	cc_state[15] = (uint32_t)(cc_nonce >> 32);

}

// -- generator state --

static bool seeded = false;
static bool secure = false;
static uint32_t since_reseed = 0;

// Reseed after this many bytes. Not a security requirement -- fast key
// erasure already gives forward secrecy and ChaCha20 will not run out
// of keystream in any plausible lifetime -- but pulling fresh physical
// entropy in periodically limits how much output any single seeding
// event is responsible for, which matters if that event turns out to
// have been worse than it looked.
#define RESEED_BYTES (1024u * 1024u)

static uint32_t rdcycle(void) {
	uint32_t v;
	__asm__ volatile ("rdcycle %0" : "=r"(v));
	return v;
}

// -- entropy gathering --

// Absorb arbitrary bytes into the key, one-way.
//
// XOR into the key and immediately run a generate step, so the new key
// depends on the whole ChaCha permutation of what went in rather than
// on the raw bytes. This cannot reduce the entropy already in the key,
// whatever is fed to it -- which is what lets z_rng_stir() accept
// anything from anyone without a trust argument.
static void absorb(const void *data, uint32_t len) {

	const uint8_t *p = (const uint8_t *)data;
	uint8_t block[64];
	uint32_t i;

	for (i = 0; i < len; i++) cc_key[i % 32] ^= p[i];

	cc_nonce++;
	chacha_setup();
	chacha_block(block);
	memcpy(cc_key, block, 32);

	memset(block, 0, sizeof(block));

}

// Pull `words` words out of the hardware, spinning until they arrive
// or the budget runs out.
//
// The budget is in polls rather than milliseconds on purpose: this can
// run before the kernel's tick is available (the BIOS may want to seed
// early) and a spin count needs no clock. rtl/trng.v delivers roughly
// 1,400 words/sec, so 64 words is about 45ms of real waiting -- the
// generous budget below covers that with a wide margin and still
// terminates on a board where the source is simply dead.
static uint32_t hw_gather(uint8_t *buf, uint32_t words) {

	uint32_t got = 0;
	uint32_t spins = 0;
	uint32_t w;

	// ~40M polls was the first number here and it is far too generous:
	// at a few cycles per iteration that is on the order of EIGHT
	// SECONDS on a 48MHz core if the source never delivers. Anything
	// calling this inside an RPC would then blow its caller's timeout
	// and look like a hang rather than a failure -- repl's own ssh
	// prepare has a 5s budget, so this could have outlived it.
	//
	// 2M polls is ~0.4s worst case, and still an enormous margin over
	// the ~46ms a healthy source needs for 64 words at rtl/trng.v's
	// ~1400 words/sec.
	while (got < words && spins < 2000000u) {
		if (z_rng_hw_word(&w)) {
			wr32le(buf + 4 * got, w);
			got++;
		} else {
			spins++;
		}
	}

	return got;

}

// Scrape whatever unpredictability exists on a board with no usable
// TRNG.
//
// This is NOT a substitute and the code says so by setting `secure`
// false wherever it is used. What it actually collects is the low bits
// of the cycle counter across a loop whose iteration time varies with
// cache state, refresh cycles, and interrupt arrival -- real jitter,
// but a handful of bits of it at best, and an attacker who knows when
// the board booted can narrow it a great deal further.
//
// It exists so that `(random)` in Scheme still shuffles a deck on a
// bitstream without `TRNG, not so that anything can pretend to be
// seeded.
static void weak_gather(void) {

	uint32_t samples[32];
	uint32_t i, j, t;

	for (i = 0; i < 32; i++) {
		t = rdcycle();
		// A deliberately data-dependent spin, so the time this takes
		// depends on what came out of the counter last iteration.
		for (j = 0; j < (17u + (t & 0x3f)); j++) {
			__asm__ volatile ("" ::: "memory");
		}
		samples[i] = rdcycle() ^ (t << 11);
	}

	absorb(samples, sizeof(samples));
	memset(samples, 0, sizeof(samples));

}

void z_rng_reseed(void) {

	uint8_t entropy[256];
	uint32_t got;
	uint32_t marks[4];

	// Cheap and always available. Worth mixing in even in the good
	// case: it costs nothing and it makes two boards seeded from
	// identical hardware entropy still diverge.
	marks[0] = rdcycle();
	marks[1] = (uint32_t)(uintptr_t)&marks;			// stack position
	marks[2] = (uint32_t)(uintptr_t)&z_rng_reseed;	// load address
	marks[3] = since_reseed;
	absorb(marks, sizeof(marks));

	if (z_rng_present() && z_rng_hw_healthy()) {

		// 256 bytes -- eight times the 32 the key can hold. The excess
		// is not waste: raw oscillator output carries well under one
		// bit of entropy per bit, and the honest way to handle a
		// source whose rate you cannot measure precisely is to take
		// far more of it than the arithmetic says you need.
		got = hw_gather(entropy, sizeof(entropy) / 4);

		if (got == sizeof(entropy) / 4) {

			absorb(entropy, sizeof(entropy));
			memset(entropy, 0, sizeof(entropy));

			// Re-check AFTER the read, not just before it. The health
			// monitor is continuous, so a source that failed a test
			// while we were draining it has already flagged itself --
			// and those are exactly the words we just absorbed.
			if (z_rng_hw_healthy()) {
				secure = true;
				seeded = true;
				since_reseed = 0;
				return;
			}

		}

	}

	// No hardware, or it went unhealthy mid-read. Fall back, and mark
	// the generator permanently unfit for keys -- once false, `secure`
	// is never set true again by anything short of a successful
	// hardware reseed above.
	memset(entropy, 0, sizeof(entropy));
	weak_gather();
	secure = false;
	seeded = true;
	since_reseed = 0;

}

static void ensure_seeded(void) {
	if (!seeded) z_rng_reseed();
}

// -- public API --

void z_rng_stir(const void *data, uint32_t len) {
	ensure_seeded();
	if (data && len) absorb(data, len);
}

// Pool for z_rng_stir_event(). Deliberately not zeroed after absorbing
// -- carrying the previous window forward costs nothing and means a
// caller that stops calling does not leave a pool of zeros behind.
static uint32_t ev_pool[8];
static uint32_t ev_count;

void z_rng_stir_event(void) {

	uint32_t t = rdcycle();

	// The low bits of the interval carry whatever jitter there is; the
	// high bits are just elapsed time and are near-predictable. Both
	// go in -- absorption is one-way, so including the predictable
	// part cannot hurt, and separating them would be guessing at where
	// the entropy is.
	ev_pool[ev_count & 7] ^= t;
	ev_pool[(ev_count + 3) & 7] += (t << 13) ^ (t >> 7);

	if ((++ev_count & 31) == 0) {
		// Only every 32nd event pays for a ChaCha block.
		if (seeded) absorb(ev_pool, sizeof(ev_pool));
	}

}

bool z_rng_secure(void) {
	ensure_seeded();
	// A source that has gone unhealthy since we seeded from it does
	// not retroactively poison the key we already have -- that key was
	// derived from words the monitor had approved at the time -- but
	// it does mean the next reseed will fail, and a caller about to
	// generate a long-term key deserves to know now rather than then.
	if (secure && z_rng_present() && !z_rng_hw_healthy()) return false;
	return secure;
}

void z_rng_bytes(void *buf, uint32_t len) {

	uint8_t *out = (uint8_t *)buf;
	uint8_t block[64];
	uint32_t n;

	ensure_seeded();

	if (since_reseed > RESEED_BYTES) z_rng_reseed();
	since_reseed += len;

	// First block: 32 bytes become the next key, the remaining 32 are
	// output. This is the key erasure step and it happens before any
	// byte is handed out, so the key that produced this call's output
	// is already gone by the time the caller sees it.
	cc_nonce++;
	chacha_setup();
	chacha_block(block);
	memcpy(cc_key, block, 32);

	n = len < 32 ? len : 32;
	memcpy(out, block + 32, n);
	out += n;
	len -= n;

	// Remaining output comes from the NEW key, advancing the block
	// counter -- not from re-running the erasure step, which would
	// cost a key rotation per 32 bytes for no benefit.
	if (len) {

		chacha_setup();

		while (len) {
			chacha_block(block);
			cc_state[12]++;		// 64-bit counter; the high word at
			if (!cc_state[12])	// cc_state[13] will not be reached in
				cc_state[13]++;	// any lifetime, but carrying is free
			n = len < 64 ? len : 64;
			memcpy(out, block, n);
			out += n;
			len -= n;
		}

	}

	memset(block, 0, sizeof(block));

}

uint32_t z_rng_u32(void) {
	uint32_t v;
	z_rng_bytes(&v, sizeof(v));
	return v;
}

uint32_t z_rng_below(uint32_t n) {

	uint32_t limit;
	uint32_t v;

	if (n == 0) return 0;

	// Rejection sampling. `% n` on a full 32-bit draw is biased toward
	// small values whenever n does not divide 2^32 -- usually a tiny
	// bias, but it is free to avoid and it is exactly the kind of
	// thing that is invisible until it matters. Discard the top
	// partial interval instead.
	limit = (uint32_t)(0x100000000ull - (0x100000000ull % (uint64_t)n));

	do {
		v = z_rng_u32();
	} while (v >= limit);

	return v % n;

}
