/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * cryptobench -- what the primitives zfed and the BBS depend on cost on
 * this machine. docs/cryptobench.md.
 *
 *   run cryptobench          (posix)      (run "cryptobench")   (repl)
 *
 * Prints to the console and writes the same lines to /cryptobench.txt.
 * Takes a minute or two; run it with nothing else busy -- the cycle
 * counter is a wall clock, so time given to other processes counts.
 *
 * Every software primitive is measured twice: as the tree builds it
 * (-Os, which is what `net` and every app ship) and at -O2, the free
 * experiment crypto_hw_options.md suggests. The -O2 copies are the
 * same sources compiled again with their symbols renamed o2_* (see the
 * Makefile). Each is first checked against a known answer, so a build
 * that is fast and wrong fails loudly instead of being timed.
 *
 * The montmul block, if the bitstream has it, is measured as a caller
 * would use it for Ed25519: a 2^255-19 multiply, padded to the block's
 * width, with the transfers in and out included.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#include "../../common/zeitlos.h"
#include "../../common/zcycles.h"
#include "../../common/zsha256.h"
#include "../../common/zfsapp.h"
#include "../../ext/monocypher/monocypher.h"
#include "../../ext/monocypher/monocypher-ed25519.h"
#include "../../common/zsoc.h"
#include "../web/ecdsa.h"
#include "../../common/z25519.h"
#include "../../common/zmlkem.h"
#include "../../common/zkeccak.h"
#define KYBER_K 3
#include "../../ext/mlkem/params.h"
#include "../../ext/mlkem/fips202.h"
#include "../web/tests/ecdsa_vectors.h"
extern int ec_rf_disable;
extern uint32_t ec_stat_rf, ec_stat_rf_bail;

// -- the -O2 copies (Makefile: objcopy --redefine-syms) --
void o2_z_sha256_init(z_sha256_ctx *ctx);
void o2_z_sha256_update(z_sha256_ctx *ctx, const void *data, uint32_t len);
void o2_z_sha256_final(z_sha256_ctx *ctx, uint8_t out[Z_SHA256_DIGEST]);
void o2_crypto_sha512(uint8_t hash[64], const uint8_t *msg, size_t len);
// zsha256.c as every app now links it: the SHA-256 block when there is one
void hw_z_sha256_init(z_sha256_ctx *ctx);
void hw_z_sha256_update(z_sha256_ctx *ctx, const void *data, uint32_t len);
void hw_z_sha256_final(z_sha256_ctx *ctx, uint8_t out[Z_SHA256_DIGEST]);
void o2_crypto_blake2b(uint8_t *hash, size_t hash_size, const uint8_t *msg, size_t len);
void o2_crypto_ed25519_key_pair(uint8_t sk[64], uint8_t pk[32], uint8_t seed[32]);
void o2_crypto_ed25519_sign(uint8_t sig[64], const uint8_t sk[64], const uint8_t *msg, size_t len);
int o2_crypto_ed25519_check(const uint8_t sig[64], const uint8_t pk[32], const uint8_t *msg, size_t len);
void o2_crypto_x25519(uint8_t shared[32], const uint8_t sk[32], const uint8_t pk[32]);
void o2_crypto_aead_lock(uint8_t *ct, uint8_t mac[16], const uint8_t key[32], const uint8_t nonce[24],
	const uint8_t *ad, size_t ad_size, const uint8_t *pt, size_t pt_size);

typedef struct {
	const char *name;
	void (*sha_init)(z_sha256_ctx *);
	void (*sha_update)(z_sha256_ctx *, const void *, uint32_t);
	void (*sha_final)(z_sha256_ctx *, uint8_t *);
	void (*sha512)(uint8_t *, const uint8_t *, size_t);
	void (*blake2b)(uint8_t *, size_t, const uint8_t *, size_t);
	void (*ed_pair)(uint8_t *, uint8_t *, uint8_t *);
	void (*ed_sign)(uint8_t *, const uint8_t *, const uint8_t *, size_t);
	int (*ed_check)(const uint8_t *, const uint8_t *, const uint8_t *, size_t);
	void (*x25519)(uint8_t *, const uint8_t *, const uint8_t *);
	void (*aead)(uint8_t *, uint8_t *, const uint8_t *, const uint8_t *, const uint8_t *, size_t,
		const uint8_t *, size_t);
} impl_t;

static const impl_t impls[2] = {
	{ "-Os", z_sha256_init, z_sha256_update, z_sha256_final, crypto_sha512, crypto_blake2b,
	  crypto_ed25519_key_pair, crypto_ed25519_sign, crypto_ed25519_check, crypto_x25519, crypto_aead_lock },
	{ "-O2", o2_z_sha256_init, o2_z_sha256_update, o2_z_sha256_final, o2_crypto_sha512, o2_crypto_blake2b,
	  o2_crypto_ed25519_key_pair, o2_crypto_ed25519_sign, o2_crypto_ed25519_check, o2_crypto_x25519,
	  o2_crypto_aead_lock },
};

// -- output: the console and /cryptobench.txt --

static char report[8192];
static int report_len;
static int failures;

static void out(const char *fmt, ...) {
	char line[200];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	printf("%s\n", line);
	int k = snprintf(report + report_len, sizeof(report) - (size_t)report_len, "%s\n", line);
	if (k > 0 && report_len + k < (int)sizeof(report)) report_len += k;
}

static void fail(const char *what) {
	out("  FAILED: %s -- the numbers below it are not to be trusted", what);
	failures++;
}

// -- timing --
//
// Runs `fn` until about a second has gone by (at least `min_runs`
// times), and keeps the fastest single run: a context switch can only
// make a run slower, never faster.

typedef void (*bench_fn)(const impl_t *, void *);

static uint32_t best_cycles(bench_fn fn, const impl_t *im, void *arg, int min_runs) {
	uint32_t best = 0xFFFFFFFFu, spent = 0;
	for (int i = 0; i < min_runs || (spent < Z_SYSCLK_HZ && i < 1000); i++) {
		uint32_t t0 = z_cycles();
		fn(im, arg);
		uint32_t dt = z_cycles() - t0;
		if (dt < best) best = dt;
		spent += dt;
	}
	return best;
}

static void line(const char *what, const char *impl, uint32_t cycles, uint32_t per, const char *unit) {
	// microseconds, from cycles at the system clock
	uint64_t us = (uint64_t)cycles * 1000000u / Z_SYSCLK_HZ;
	if (per > 1)
		out("  %-34s %s %10lu cycles %9lu.%03lu ms   %lu cycles/%s", what, impl, (unsigned long)cycles,
			(unsigned long)(us / 1000), (unsigned long)(us % 1000), (unsigned long)(cycles / per), unit);
	else
		out("  %-34s %s %10lu cycles %9lu.%03lu ms", what, impl, (unsigned long)cycles,
			(unsigned long)(us / 1000), (unsigned long)(us % 1000));
}

// -- the benchmarks --

static uint8_t big[16384];
static uint8_t big_out[16384];

static void b_sha256_4k(const impl_t *im, void *arg) {
	(void)arg;
	z_sha256_ctx c;
	uint8_t h[32];
	im->sha_init(&c);
	im->sha_update(&c, big, 4096);
	im->sha_final(&c, h);
}

// The BBS's password hash (sw/apps/bbs/core/users.c, pw_hash()), the
// same shape: a 16-byte salt and an 8-character password.
static void b_pw(const impl_t *im, void *arg) {
	uint32_t iter = *(uint32_t *)arg;
	static const uint8_t salt[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
	const char *pw = "hunter22";
	uint8_t h[32];
	z_sha256_ctx c;
	im->sha_init(&c);
	im->sha_update(&c, salt, 16);
	im->sha_update(&c, pw, 8);
	im->sha_final(&c, h);
	for (uint32_t i = 1; i < iter; i++) {
		im->sha_init(&c);
		im->sha_update(&c, h, 32);
		im->sha_update(&c, salt, 16);
		im->sha_update(&c, pw, 8);
		im->sha_final(&c, h);
	}
}

static void b_sha512(const impl_t *im, void *arg) {
	uint8_t h[64];
	im->sha512(h, big, *(uint32_t *)arg);
}

static void b_blake2b(const impl_t *im, void *arg) {
	uint8_t h[32];
	im->blake2b(h, 32, big, *(uint32_t *)arg);
}

static uint8_t sk[64], pk[32], sig32[64], sig16k[64], xsk[32], xpk[32];

static void b_sign32(const impl_t *im, void *arg) {
	(void)arg;
	uint8_t s[64];
	im->ed_sign(s, sk, big, 32);
}

static void b_check32(const impl_t *im, void *arg) {
	(void)arg;
	if (im->ed_check(sig32, pk, big, 32) != 0) failures += 1000;   // reported after timing
}

static void b_check16k(const impl_t *im, void *arg) {
	(void)arg;
	if (im->ed_check(sig16k, pk, big, 16384) != 0) failures += 1000;
}

static void b_x25519(const impl_t *im, void *arg) {
	(void)arg;
	uint8_t shared[32];
	im->x25519(shared, xsk, xpk);
}

static void b_aead(const impl_t *im, void *arg) {
	static const uint8_t key[32] = { 7 }, nonce[24] = { 9 };
	uint8_t mac[16];
	im->aead(big_out, mac, key, nonce, NULL, 0, big, *(uint32_t *)arg);
}

// -- known answers, before anything is timed --

static int hexeq(const uint8_t *b, const char *hex) {
	for (int i = 0; hex[2 * i]; i++) {
		unsigned v;
		char t[3] = { hex[2 * i], hex[2 * i + 1], 0 };
		v = (unsigned)strtoul(t, NULL, 16);
		if (b[i] != v) return 0;
	}
	return 1;
}

static void known_answers(const impl_t *im) {
	uint8_t h[64];
	z_sha256_ctx c;
	im->sha_init(&c);
	im->sha_update(&c, "abc", 3);
	im->sha_final(&c, h);
	if (!hexeq(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")) fail("SHA-256 of \"abc\"");
	im->sha512(h, (const uint8_t *)"abc", 3);
	if (!hexeq(h, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a")) fail("SHA-512 of \"abc\"");
	im->blake2b(h, 64, (const uint8_t *)"abc", 3);
	if (!hexeq(h, "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d1")) fail("BLAKE2b-512 of \"abc\"");
	// RFC 8032, test 1: the empty message
	uint8_t seed[32], ksk[64], kpk[32], s[64];
	static const char *seed_hex = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
	for (int i = 0; i < 32; i++) { char t[3] = { seed_hex[2 * i], seed_hex[2 * i + 1], 0 }; seed[i] = (uint8_t)strtoul(t, NULL, 16); }
	im->ed_pair(ksk, kpk, seed);
	if (!hexeq(kpk, "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")) fail("Ed25519 public key (RFC 8032)");
	im->ed_sign(s, ksk, NULL, 0);
	if (!hexeq(s, "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155")) fail("Ed25519 signature (RFC 8032)");
	if (im->ed_check(s, kpk, NULL, 0) != 0) fail("Ed25519 check of a good signature");
	s[0] ^= 1;
	if (im->ed_check(s, kpk, NULL, 0) == 0) fail("Ed25519 check of a bad signature");
	// RFC 7748, section 5.2, first vector
	static const char *xs = "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4";
	static const char *xu = "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c";
	uint8_t a[32], u[32], r[32];
	for (int i = 0; i < 32; i++) {
		char t[3] = { xs[2 * i], xs[2 * i + 1], 0 }, v[3] = { xu[2 * i], xu[2 * i + 1], 0 };
		a[i] = (uint8_t)strtoul(t, NULL, 16);
		u[i] = (uint8_t)strtoul(v, NULL, 16);
	}
	im->x25519(r, a, u);
	if (!hexeq(r, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552")) fail("X25519 (RFC 7748)");
}

// -- web's ECDSA (sw/apps/web/ecdsa.c): TLS certificate checks --

static void b_ec256(const impl_t *im, void *arg) {
	(void)im; (void)arg;
	if (!ec_verify(EC_CURVE_P256, p256_pub, 65, p256_r, 32, p256_s, 32, p256_h, 32)) failures += 1000;
}
static void b_ec384(const impl_t *im, void *arg) {
	(void)im; (void)arg;
	if (!ec_verify(EC_CURVE_P384, p384_pub, 97, p384_r, 48, p384_s, 48, p384_h, 48)) failures += 1000;
}

static void ecdsa_bench(void) {
	uint8_t h[48];
	out("");
	out("ECDSA verify (sw/apps/web/ecdsa.c, as web builds it)");
	memcpy(h, p384_h, 48);
	h[5] ^= 1;
	for (int pass = 0; pass < 2; pass++) {
		const char *name = pass ? "classic" : "as built";
		uint32_t rf0 = ec_stat_rf;
		int before = failures;
		ec_rf_disable = pass;
		if (!ec_verify(EC_CURVE_P256, p256_pub, 65, p256_r, 32, p256_s, 32, p256_h, 32) ||
				!ec_verify(EC_CURVE_P384, p384_pub, 97, p384_r, 48, p384_s, 48, p384_h, 48))
			fail("a valid ECDSA signature did not verify");
		if (ec_verify(EC_CURVE_P384, p384_pub, 97, p384_r, 48, p384_s, 48, h, 48))
			fail("a tampered ECDSA signature verified");
		if (failures != before) continue;
		uint32_t c256 = best_cycles(b_ec256, &impls[0], NULL, 2);
		uint32_t c384 = best_cycles(b_ec384, &impls[0], NULL, 2);
		if (failures >= before + 1000) { failures = before; fail("ECDSA stopped verifying while timed"); continue; }
		out("  %s: %s", name, pass ? "the block's classic registers (or software without it)"
			: (ec_stat_rf != rf0 ? "the register file" : "no register file: as the classic line"));
		line("P-256 verify", "   ", c256, 0, "");
		line("P-384 verify", "   ", c384, 0, "");
	}
	ec_rf_disable = 0;
}

// -- X25519 and Ed25519 checks through sw/common/z25519.c --

static uint8_t zk[32], zu[32], zsig[64], zpk[32];

static void b_zx(const impl_t *im, void *arg) {
	(void)im; (void)arg;
	uint8_t o[32];
	z_x25519(o, zk, zu);
}
static void b_zcheck(const impl_t *im, void *arg) {
	(void)im; (void)arg;
	if (z_ed25519_check(zsig, zpk, big, 32) != 0) failures += 1000;
}

static void z25519_bench(void) {
	uint8_t seed[32], sk[64], a[32], b[32];
	out("");
	out("X25519 and Ed25519 checks (sw/common/z25519.c, as ssh, netserve and web use them)");
	for (int i = 0; i < 32; i++) { zk[i] = (uint8_t)(i * 7 + 1); zu[i] = (uint8_t)(i * 13 + 5); seed[i] = (uint8_t)i; }
	crypto_ed25519_key_pair(sk, zpk, seed);
	crypto_ed25519_sign(zsig, sk, big, 32);
	for (int pass = 0; pass < 2; pass++) {
		int before = failures;
		z25519_no_hw = pass;
		// the same answers as Monocypher, before anything is timed
		z_x25519(a, zk, zu);
		int hw = z25519_last_hw;
		crypto_x25519(b, zk, zu);
		if (memcmp(a, b, 32)) fail("z_x25519 against Monocypher");
		if (z_ed25519_check(zsig, zpk, big, 32) != 0) fail("z_ed25519_check of a good signature");
		zsig[40] ^= 1;
		if (z_ed25519_check(zsig, zpk, big, 32) == 0) fail("z_ed25519_check of a bad signature");
		zsig[40] ^= 1;
		if (failures != before) continue;
		out("  %s:", pass ? "software (Monocypher)" : (hw ? "the register file" : "no register file here -- software"));
		line("X25519", "   ", best_cycles(b_zx, &impls[0], NULL, 2), 0, "");
		uint32_t c = best_cycles(b_zcheck, &impls[0], NULL, 2);
		if (failures >= before + 1000) { failures = before; fail("z_ed25519_check stopped verifying while timed"); continue; }
		line("Ed25519 check, 32 bytes", "   ", c, 0, "");
	}
	z25519_no_hw = 0;
}

// -- ML-KEM-768 (FIPS 203) through sw/common/zmlkem.c --
//
// The known answers: NIST's first ML-KEM-768 key-generation vector (its
// d and z; SHA-256 of the ek and dk it must give), then an encapsulation
// with m = 0..31 whose answer kyber-py computed (the NIST vector and
// kyber-py agree). Then the times, and one Keccak-f[1600] permutation:
// how much of each operation is Keccak decides whether a Keccak block
// is worth its LUTs (docs/mlkem.md).

static const uint8_t mk_d[32] = { 0xe5, 0x82, 0xb7, 0xd7, 0x5e, 0x6c, 0x80, 0xb0, 0x5a, 0xe3, 0x92, 0xa1, 0xfc, 0x9f, 0x71, 0x53,
	0xb1, 0x23, 0x90, 0xfd, 0x99, 0x93, 0x03, 0x68, 0xcc, 0x67, 0xa7, 0x68, 0xba, 0xeb, 0xc8, 0xa0 };
static const uint8_t mk_z[32] = { 0x1c, 0xda, 0xcb, 0x87, 0x40, 0xc0, 0xb8, 0x7c, 0x4a, 0x37, 0x95, 0x75, 0xf1, 0x87, 0xb3, 0x67,
	0xcb, 0xfa, 0x3b, 0x30, 0x0b, 0xf5, 0x91, 0xb1, 0x09, 0xf7, 0x98, 0x16, 0xe9, 0xcb, 0xe8, 0xf0 };
static const uint8_t mk_ek256[32] = { 0x41, 0x58, 0xf6, 0xaf, 0xb5, 0xe5, 0x16, 0xc9, 0x9f, 0x1d, 0xa0, 0x7d, 0xa8, 0xc6, 0x51, 0x34,
	0x84, 0x22, 0xb1, 0x7c, 0x1f, 0x4e, 0x9a, 0x08, 0xad, 0x73, 0xfb, 0x1f, 0x91, 0x24, 0x9b, 0x3e };
static const uint8_t mk_dk256[32] = { 0x7a, 0xab, 0x35, 0x83, 0x92, 0x07, 0xf7, 0x2b, 0x31, 0x0a, 0xbe, 0x36, 0xe2, 0xda, 0xa1, 0xcc,
	0x7f, 0xf6, 0xf7, 0xfa, 0x89, 0x41, 0xe4, 0x39, 0x96, 0x7c, 0xd4, 0x7d, 0x9b, 0x43, 0x70, 0x79 };
static const uint8_t mk_ct256[32] = { 0xf6, 0xaa, 0xfd, 0xe6, 0x14, 0x6e, 0x9f, 0x03, 0xea, 0xe7, 0xce, 0xa5, 0x7c, 0xcb, 0xc3, 0x45,
	0x40, 0xbc, 0x70, 0x9e, 0x98, 0x78, 0x7f, 0xde, 0xcc, 0x79, 0x8b, 0xea, 0xd3, 0xc9, 0x09, 0x5d };
static const uint8_t mk_ss[32] = { 0x82, 0xc9, 0xc3, 0x7c, 0x49, 0xc9, 0xe5, 0x40, 0xd6, 0x4f, 0x90, 0x7e, 0xa0, 0xa3, 0xfb, 0x72,
	0x3a, 0x28, 0x00, 0x8c, 0xff, 0x00, 0x7d, 0xfd, 0x07, 0x69, 0x49, 0x2f, 0x5a, 0x47, 0xc4, 0xdd };

static uint8_t mk_ek[ZMLKEM_EK_BYTES], mk_dk[ZMLKEM_DK_BYTES], mk_ct[ZMLKEM_CT_BYTES];

static void b_mk_keypair(const impl_t *im, void *arg) {
	uint8_t coins[64];
	(void)im; (void)arg;
	memcpy(coins, mk_d, 32); memcpy(coins + 32, mk_z, 32);
	zmlkem_keypair(mk_ek, mk_dk, coins);
}
static void b_mk_encaps(const impl_t *im, void *arg) {
	uint8_t m[32], ss[32];
	(void)im; (void)arg;
	for (int i = 0; i < 32; i++) m[i] = (uint8_t)i;
	zmlkem_encaps(mk_ct, ss, mk_ek, m);
}
static void b_mk_decaps(const impl_t *im, void *arg) {
	uint8_t ss[32];
	(void)im; (void)arg;
	zmlkem_decaps(ss, mk_ct, mk_dk);
}
static void b_keccak32(const impl_t *im, void *arg) {
	keccak_state st;
	(void)im; (void)arg;
	shake128_absorb_once(&st, mk_d, 32);
	shake128_squeezeblocks(big_out, 32, &st);			// 32 permutations
}

// -- the Keccak block, taken apart --
//
// A permutation through the block measured ~31,000 cycles against an
// estimate of 3,000-7,000 (docs/keccak_hw.md): the block itself takes
// 24. So: the permutation alone, without a sponge around it; how many
// calls actually used the block; and the driver's three phases, and a
// register read, each timed on their own.

static uint64_t kt_state[25];
static void b_kf_alone(const impl_t *im, void *arg) {
	(void)im; (void)arg;
	zkeccak_f1600(kt_state);
}

static void keccak_taken_apart(void) {
	out("");
	out("Keccak block, taken apart");
#ifndef __riscv
	out("  no Keccak block here");		// the host build: no hardware to read
}
#else
	volatile uint32_t *k = (volatile uint32_t *)(uintptr_t)Z_KECCAK_BASE;
	if (!z_soc_has_feature2(Z_FEATURE2_KECCAK) || k[Z_KECCAK_W_MAGIC] != Z_KECCAK_MAGIC) {
		out("  no Keccak block here");
		return;
	}
	for (int i = 0; i < 25; i++) kt_state[i] = 0x0123456789ABCDEFull * (uint64_t)(i + 1);
	zkeccak_no_hw = 0;
	uint32_t hw = best_cycles(b_kf_alone, &impls[0], NULL, 4);
	zkeccak_no_hw = 1;
	uint32_t sw = best_cycles(b_kf_alone, &impls[0], NULL, 2);
	zkeccak_no_hw = 0;
	line("zkeccak_f1600() alone, the block", "   ", hw, 0, "");
	line("zkeccak_f1600() alone, software", "   ", sw, 0, "");
	int used = 0;
	for (int i = 0; i < 100; i++) { zkeccak_f1600(kt_state); used += zkeccak_last_hw; }
	out("  of 100 calls, %d used the block (the rest: software, the claim lost)", used);

	// the driver's phases, by hand
	uint32_t me = 0x7FFFFFF0u;
	k[Z_KECCAK_W_OWNER] = me;
	if (k[Z_KECCAK_W_OWNER] != me) { out("  could not claim the block to time it"); return; }
	uint32_t t0 = z_cycles();
	for (int i = 0; i < 50; i++) k[Z_KECCAK_W_IN] = (uint32_t)i;
	uint32_t t1 = z_cycles();
	k[Z_KECCAK_W_CTRL] = 1u;
	while (k[Z_KECCAK_W_CTRL] & 1u) { }
	uint32_t t2 = z_cycles();
	uint32_t sink = 0;
	for (int i = 0; i < 50; i++) sink ^= k[Z_KECCAK_W_OUT];
	uint32_t t3 = z_cycles();
	// one read: the best of several -- a single one can catch an interrupt
	// (one run measured 14,516 cycles; the one before, 61)
	uint32_t best = 0xFFFFFFFFu;
	for (int i = 0; i < 16; i++) {
		uint32_t a = z_cycles();
		sink ^= k[Z_KECCAK_W_MAGIC];
		uint32_t b = z_cycles();
		if (b - a < best) best = b - a;
	}
	uint32_t t4 = t3 + best;
	k[Z_KECCAK_W_CTRL] = 2u;
	k[Z_KECCAK_W_OWNER] = me | 0x80000000u;
	(void)sink;
	line("50 IN writes", "   ", t1 - t0, 50, "write");
	line("START, and wait for it", "   ", t2 - t1, 0, "");
	line("50 OUT reads", "   ", t3 - t2, 50, "read");
	line("one MAGIC read, for scale", "   ", t4 - t3, 0, "");
}
#endif

static void mlkem_bench(void) {
	uint8_t h[32], m[32], ss[32], ss2[32];
	out("");
	out("ML-KEM-768 (FIPS 203; sw/common/zmlkem.c, the pq-crystals reference in sw/ext/mlkem)");
	b_mk_keypair(&impls[0], NULL);
	z_sha256(h, mk_ek, sizeof(mk_ek));
	bool ok = !memcmp(h, mk_ek256, 32);
	z_sha256(h, mk_dk, sizeof(mk_dk));
	ok = ok && !memcmp(h, mk_dk256, 32);
	for (int i = 0; i < 32; i++) m[i] = (uint8_t)i;
	ok = ok && zmlkem_encaps(mk_ct, ss, mk_ek, m);
	z_sha256(h, mk_ct, sizeof(mk_ct));
	ok = ok && !memcmp(h, mk_ct256, 32) && !memcmp(ss, mk_ss, 32);
	ok = ok && zmlkem_decaps(ss2, mk_ct, mk_dk) && !memcmp(ss2, mk_ss, 32);
	mk_ct[100] ^= 1;
	ok = ok && zmlkem_decaps(ss2, mk_ct, mk_dk) && memcmp(ss2, mk_ss, 32);		// implicit rejection
	mk_ct[100] ^= 1;
	if (!ok) { fail("ML-KEM-768 known answers"); return; }
	out("  known answers: correct (NIST's key generation, an encapsulation, a round trip, a tampered ciphertext)");
	// Twice: on the Keccak block (rtl/keccak.v) if the bitstream has it,
	// then in software -- the same code, zkeccak.c choosing.
	for (int pass = 0; pass < 2; pass++) {
		uint64_t probe[25] = { 0 };
		zkeccak_no_hw = pass;
		zkeccak_f1600(probe);
		if (pass == 0 && !zkeccak_last_hw) out("  no Keccak block here (docs/keccak_hw.md): software only");
		if (pass == 0 && !zkeccak_last_hw) continue;
		// the known answers again, this way
		b_mk_keypair(&impls[0], NULL);
		z_sha256(h, mk_ek, sizeof(mk_ek));
		if (memcmp(h, mk_ek256, 32)) { fail(pass ? "ML-KEM in software" : "ML-KEM on the Keccak block"); continue; }
		out("  %s:", pass ? "software Keccak" : "the Keccak block (known answers correct through it)");
		uint32_t kg = best_cycles(b_mk_keypair, &impls[0], NULL, 2);
		uint32_t en = best_cycles(b_mk_encaps, &impls[0], NULL, 2);
		uint32_t de = best_cycles(b_mk_decaps, &impls[0], NULL, 2);
		// The bare permutation, and a whole SHAKE128 block (permutation and
		// squeeze). The share is the PERMUTATION's: an earlier version used
		// the SHAKE block, and so counted the sponge's own byte handling as
		// Keccak (docs/keccak_hw.md, "Measured").
		uint32_t kc = best_cycles(b_kf_alone, &impls[0], NULL, 2);
		uint32_t ks = best_cycles(b_keccak32, &impls[0], NULL, 2) / 32;
		line("keygen", "   ", kg, 0, "");
		line("encaps", "   ", en, 0, "");
		line("decaps (with FIPS 203's key check)", "   ", de, 0, "");
		line("one Keccak-f[1600] permutation", "   ", kc, 0, "");
		line("one SHAKE128 block (permutation, squeeze)", "   ", ks, 0, "");
		// permutations per operation, counted on the host (docs/mlkem.md):
		// keygen 43, encaps 44, decaps 44 + 9 for the key check
		out("  the permutations' share: keygen ~%lu%%, encaps ~%lu%%, decaps ~%lu%%",
			(unsigned long)(43ull * kc * 100 / kg), (unsigned long)(44ull * kc * 100 / en), (unsigned long)(53ull * kc * 100 / de));
	}
	zkeccak_no_hw = 0;
	out("  a zfed handshake adds: initiator keygen + decaps, responder encaps");
	keccak_taken_apart();
}

// -- the SHA-256 block, through zsha256.c as apps now get it --

static void sha256_hw_bench(void) {
	impl_t hw = impls[0];
	hw.name = "hw ";
	hw.sha_init = hw_z_sha256_init;
	hw.sha_update = hw_z_sha256_update;
	hw.sha_final = hw_z_sha256_final;
	out("");
	out("SHA-256 block (rtl/sha256.v), through zsha256.c as every app now links it");
#ifdef __riscv
	if (!z_soc_has_feature2(Z_FEATURE2_SHA256) ||
			*(volatile uint32_t *)(uintptr_t)(Z_SHA256_BASE + 4u * Z_SHA256_W_MAGIC) != Z_SHA256_MAGIC) {
		out("  not in this bitstream: zsha256.c uses software, as below");
	} else {
		out("  present (owner now: %lu)", (unsigned long)
			*(volatile uint32_t *)(uintptr_t)(Z_SHA256_BASE + 4u * Z_SHA256_W_OWNER));
	}
#else
	out("  a host build: no block, zsha256.c uses software");
#endif
	int before = failures;
	{
		// "abc", a million 'a' in pieces that cross block edges
		uint8_t h[32];
		z_sha256_ctx c;
		hw.sha_init(&c);
		hw.sha_update(&c, "abc", 3);
		hw.sha_final(&c, h);
		if (!hexeq(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")) fail("SHA-256 of \"abc\" via zsha256");
		memset(big_out, 'a', 1003);		// pieces start up to 3 bytes in
		hw.sha_init(&c);
		for (int i = 0; i < 1000; i++) hw.sha_update(&c, big_out + (i & 3), 1000);
		hw.sha_final(&c, h);
		if (!hexeq(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")) fail("SHA-256 of a million 'a' via zsha256");
	}
	if (failures == before) out("  known answers: correct (\"abc\"; a million 'a', misaligned pieces)");
	static uint32_t n2000 = 2000;
	line("SHA-256, 4 KB", hw.name, best_cycles(b_sha256_4k, &hw, NULL, 3), 64, "64-byte block");
	line("BBS password hash, 2000 rounds", hw.name, best_cycles(b_pw, &hw, &n2000, 1), 0, "");
}

// -- the montmul block --

#ifdef __riscv
static volatile uint32_t *mm(uint32_t w) {
	return (volatile uint32_t *)(uintptr_t)(Z_MONTMUL_BASE + 4u * w);
}
#endif

#define MM_MAX 16
static uint32_t mm_limbs;
static uint32_t p25519[MM_MAX], rr[MM_MAX], ma[MM_MAX], mb[MM_MAX], mr[MM_MAX];

// a >= b, both `n` limbs
static int mp_ge(const uint32_t *a, const uint32_t *b, uint32_t n) {
	for (int i = (int)n - 1; i >= 0; i--) if (a[i] != b[i]) return a[i] > b[i];
	return 1;
}

static void mp_sub(uint32_t *a, const uint32_t *b, uint32_t n) {
	uint64_t borrow = 0;
	for (uint32_t i = 0; i < n; i++) {
		uint64_t d = (uint64_t)a[i] - b[i] - borrow;
		a[i] = (uint32_t)d;
		borrow = (d >> 63) & 1;
	}
}

// R^2 mod p, R = 2^(32 * limbs): 1 doubled 2 * 32 * limbs times mod p.
static void mp_r2(uint32_t n) {
	memset(rr, 0, sizeof(rr));
	rr[0] = 1;
	for (uint32_t k = 0; k < 64 * n; k++) {
		uint32_t carry = 0;
		for (uint32_t i = 0; i < n; i++) {
			uint32_t v = rr[i];
			rr[i] = (v << 1) | carry;
			carry = v >> 31;
		}
		if (carry || mp_ge(rr, p25519, n)) mp_sub(rr, p25519, n);
	}
}

#ifdef __riscv
static void mm_mul(uint32_t *out, const uint32_t *a, const uint32_t *b) {
	for (uint32_t i = 0; i < mm_limbs; i++) *mm(Z_MONTMUL_W_A + i) = a[i];
	for (uint32_t i = 0; i < mm_limbs; i++) *mm(Z_MONTMUL_W_B + i) = b[i];
	*mm(Z_MONTMUL_W_CTRL) = 1;
	while (*mm(Z_MONTMUL_W_CTRL) & 1) { }
	for (uint32_t i = 0; i < 8; i++) out[i] = *mm(Z_MONTMUL_W_R + i);   // p < 2^256: 8 limbs out
	for (uint32_t i = 8; i < mm_limbs; i++) out[i] = 0;
}
#else
// On the host: a software model of the block's one operation,
// R = A * B * 2^-(32 n) mod N (CIOS, as rtl/montmul.v), so the
// arithmetic around it -- n0inv, R^2, the round trip -- is tested
// before it meets hardware.
static uint32_t sim_n[MM_MAX], sim_n0inv;
static void mm_mul(uint32_t *out, const uint32_t *a, const uint32_t *b) {
	uint32_t n = mm_limbs, t[MM_MAX + 2] = { 0 };
	for (uint32_t i = 0; i < n; i++) {
		uint64_t c = 0;
		for (uint32_t j = 0; j < n; j++) {
			uint64_t v = (uint64_t)a[j] * b[i] + t[j] + c;
			t[j] = (uint32_t)v; c = v >> 32;
		}
		uint64_t v = (uint64_t)t[n] + c;
		t[n] = (uint32_t)v; t[n + 1] = (uint32_t)(v >> 32);
		uint32_t m = t[0] * sim_n0inv;
		v = (uint64_t)m * sim_n[0] + t[0];
		c = v >> 32;
		for (uint32_t j = 1; j < n; j++) {
			v = (uint64_t)m * sim_n[j] + t[j] + c;
			t[j - 1] = (uint32_t)v; c = v >> 32;
		}
		v = (uint64_t)t[n] + c;
		t[n - 1] = (uint32_t)v;
		t[n] = t[n + 1] + (uint32_t)(v >> 32);
	}
	if (t[n] || mp_ge(t, sim_n, n)) mp_sub(t, sim_n, n);
	for (uint32_t i = 0; i < n; i++) out[i] = t[i];
}
#endif

static void b_mm(const impl_t *im, void *arg) {
	(void)im; (void)arg;
	for (int k = 0; k < 100; k++) mm_mul(mr, ma, mb);
}

// Where a multiply's cycles go: the operands in, the block's own work,
// the result out -- each 100 times, as above.
#ifdef __riscv
static void b_mm_in(const impl_t *im, void *arg) {
	(void)im; (void)arg;
	for (int k = 0; k < 100; k++) {
		for (uint32_t i = 0; i < mm_limbs; i++) *mm(Z_MONTMUL_W_A + i) = ma[i];
		for (uint32_t i = 0; i < mm_limbs; i++) *mm(Z_MONTMUL_W_B + i) = mb[i];
	}
}
static void b_mm_run(const impl_t *im, void *arg) {
	(void)im; (void)arg;
	for (int k = 0; k < 100; k++) {
		*mm(Z_MONTMUL_W_CTRL) = 1;
		while (*mm(Z_MONTMUL_W_CTRL) & 1) { }
	}
}
static void b_mm_out(const impl_t *im, void *arg) {
	(void)im; (void)arg;
	for (int k = 0; k < 100; k++)
		for (uint32_t i = 0; i < 8; i++) mr[i] = *mm(Z_MONTMUL_W_R + i);
}
static void b_mm_one(const impl_t *im, void *arg) {
	(void)im; (void)arg;
	for (int k = 0; k < 100; k++) (void)*mm(Z_MONTMUL_W_CONFIG);
}
#endif

#ifdef __riscv
// The register file: operands loaded once, then one-word commands.
static void mm_load(uint32_t reg, const uint32_t *v) {
	*mm(Z_MONTMUL_W_RSEL) = reg << 4;
	for (uint32_t i = 0; i < mm_limbs; i++) *mm(Z_MONTMUL_W_RDATA) = v[i];
}
static void mm_fetch(uint32_t reg, uint32_t *v) {
	*mm(Z_MONTMUL_W_RSEL) = reg << 4;
	for (uint32_t i = 0; i < mm_limbs; i++) v[i] = *mm(Z_MONTMUL_W_RDATA);
}
static void mm_cmd(uint32_t c) {
	*mm(Z_MONTMUL_W_CMD) = c;
	while (*mm(Z_MONTMUL_W_CMD) & 1) { }
}
static void b_rf_mul(const impl_t *im, void *arg) {
	(void)im; (void)arg;
	for (int k = 0; k < 100; k++) mm_cmd(Z_MONTMUL_CMD(Z_MONTMUL_OP_MUL, 3, 1, 2));
}
static void b_rf_add(const impl_t *im, void *arg) {
	(void)im; (void)arg;
	for (int k = 0; k < 100; k++) mm_cmd(Z_MONTMUL_CMD(Z_MONTMUL_OP_ADD, 4, 1, 2));
}

static void regfile_bench(void) {
	uint32_t nregs = (*mm(Z_MONTMUL_W_CONFIG) >> 8) & 0xFF;
	uint32_t want[MM_MAX], got[MM_MAX], sum[MM_MAX];
	out("  register file: %lu registers%s", (unsigned long)nregs,
		nregs ? "" : " -- not in this bitstream (MONTMUL_REGS)");
	if (!nregs) return;
	// the same product both ways: the classic interface and a MUL
	mm_mul(want, ma, mb);
	mm_load(1, ma);
	mm_load(2, mb);
	mm_cmd(Z_MONTMUL_CMD(Z_MONTMUL_OP_MUL, 3, 1, 2));
	mm_fetch(3, got);
	if (memcmp(got, want, 8 * sizeof(uint32_t))) { fail("register file MUL against the classic multiply"); return; }
	// ADD against schoolbook: ma + mb mod p
	{
		uint64_t c = 0;
		memset(sum, 0, sizeof(sum));
		for (int i = 0; i < 8; i++) { uint64_t v = (uint64_t)ma[i] + mb[i] + c; sum[i] = (uint32_t)v; c = v >> 32; }
		if (c || mp_ge(sum, p25519, 8)) mp_sub(sum, p25519, 8);
	}
	mm_cmd(Z_MONTMUL_CMD(Z_MONTMUL_OP_ADD, 4, 1, 2));
	mm_fetch(4, got);
	if (memcmp(got, sum, 8 * sizeof(uint32_t))) { fail("register file ADD against schoolbook"); return; }
	out("  register file: MUL agrees with the classic multiply, ADD with schoolbook");
	line("one MUL command, poll included", "   ", best_cycles(b_rf_mul, &impls[0], NULL, 3) / 100, 1, "");
	line("one ADD command, poll included", "   ", best_cycles(b_rf_add, &impls[0], NULL, 3) / 100, 1, "");
}
#endif

static void montmul_bench(void) {
	out("");
	out("montmul block (rtl/montmul.v), for Ed25519's field, p = 2^255-19");
#ifndef __riscv
	out("  a host build: a software model of the block, 12 limbs");
	mm_limbs = 12;
#else
	if (!z_soc_has_feature2(Z_FEATURE2_MONTMUL) || *mm(Z_MONTMUL_W_MAGIC) != Z_MONTMUL_MAGIC) {
		out("  not in this bitstream (CSR_FEATURES2 bit 3 clear)");
		return;
	}
	mm_limbs = *mm(Z_MONTMUL_W_CONFIG) & 0xFF;
	{
		// Claim it, as every user must now (docs/montmul.md,
		// "Sharing"); 0 back means a bitstream older than the claim.
		uint32_t me = z_getpid() ? z_getpid() : 0x7FFFFFFEu, who;
		*mm(Z_MONTMUL_W_OWNER) = me;
		who = *mm(Z_MONTMUL_W_OWNER);
		if (who != me && who != 0) { out("  held by pid %lu -- not measured; try again", (unsigned long)who); return; }
		out("  claimed%s", who ? "" : " (a bitstream without the claim register)");
	}
#endif
	out("  built for %lu limbs (%lu bits); 2^255-19 is padded to that width",
		(unsigned long)mm_limbs, (unsigned long)mm_limbs * 32);
	if (mm_limbs < 8 || mm_limbs > MM_MAX) { out("  unexpected width -- not measured"); return; }
	memset(p25519, 0, sizeof(p25519));
	p25519[0] = 0xFFFFFFEDu;
	for (int i = 1; i < 7; i++) p25519[i] = 0xFFFFFFFFu;
	p25519[7] = 0x7FFFFFFFu;
	// n0inv = -p^-1 mod 2^32, by Newton's iteration
	uint32_t inv = 1;
	for (int i = 0; i < 5; i++) inv *= 2u - p25519[0] * inv;
	uint32_t n0inv = (uint32_t)(0u - inv);
	mp_r2(mm_limbs);
	// NOTE: the block holds one modulus for the whole machine. `web`
	// loads its own for TLS; do not run this while a page is loading.
#ifdef __riscv
	*mm(Z_MONTMUL_W_N0INV) = n0inv;
	for (uint32_t i = 0; i < mm_limbs; i++) *mm(Z_MONTMUL_W_N + i) = p25519[i];
#else
	memcpy(sim_n, p25519, sizeof(sim_n));
	sim_n0inv = n0inv;
#endif
	// a round trip: to the Montgomery domain and back is the identity
	uint32_t a[MM_MAX] = { 0 }, one[MM_MAX] = { 0 }, t[MM_MAX], back[MM_MAX];
	for (int i = 0; i < 8; i++) a[i] = 0x12345678u * (uint32_t)(i + 1);
	a[7] &= 0x3FFFFFFFu;
	one[0] = 1;
	mm_mul(t, a, rr);          // a * R
	mm_mul(back, t, one);      // a
	if (memcmp(back, a, 8 * sizeof(uint32_t))) { fail("montmul round trip mod 2^255-19"); return; }
	out("  round trip mod 2^255-19: correct");
	memcpy(ma, t, sizeof(ma));
	memcpy(mb, rr, sizeof(mb));
	// a * b the long way, checked against the block: (aR)(bR)/R = abR,
	// then back out: ab mod p, compared with schoolbook mod p below
	uint32_t b2[MM_MAX] = { 0 }, tb[MM_MAX], tab[MM_MAX], ab[MM_MAX];
	for (int i = 0; i < 8; i++) b2[i] = 0x9E3779B9u * (uint32_t)(i + 3);
	b2[7] &= 0x3FFFFFFFu;
	mm_mul(tb, b2, rr);
	mm_mul(tab, t, tb);
	mm_mul(ab, tab, one);
	// schoolbook a*b mod p: 16-limb product, reduced by 2^256 = 38 mod p
	uint64_t prod[16] = { 0 };
	for (int i = 0; i < 8; i++) {
		uint64_t c = 0;
		for (int j = 0; j < 8; j++) {
			uint64_t v = (uint64_t)a[i] * b2[j] + prod[i + j] + c;
			prod[i + j] = (uint32_t)v; c = v >> 32;
		}
		prod[i + 8] = c;
	}
	uint32_t red[MM_MAX] = { 0 };
	uint64_t c = 0;
	for (int i = 0; i < 8; i++) { uint64_t v = prod[i] + prod[i + 8] * 38u + c; red[i] = (uint32_t)v; c = v >> 32; }
	while (c) {                                // fold what spilled past 2^256, again times 38
		uint64_t cc = c * 38u;
		c = 0;
		for (int i = 0; i < 8; i++) { uint64_t v = (uint64_t)red[i] + (i == 0 ? cc : 0) + c; red[i] = (uint32_t)v; c = v >> 32; if (!c && i > 0) break; }
	}
	while (mp_ge(red, p25519, 8)) mp_sub(red, p25519, 8);
	if (memcmp(ab, red, 8 * sizeof(uint32_t))) { fail("montmul product mod 2^255-19"); return; }
	out("  a product mod 2^255-19 agrees with schoolbook arithmetic");
	uint32_t cyc = best_cycles(b_mm, &impls[0], NULL, 3);
	line("one multiply, transfers included", "   ", cyc / 100, 1, "");
	// (released at the end of main)
#ifdef __riscv
	char what[64];
	snprintf(what, sizeof(what), "  of which %lu words in", (unsigned long)(2 * mm_limbs));
	line(what, "   ", best_cycles(b_mm_in, &impls[0], NULL, 3) / 100, 1, "");
	line("  of which start and wait", "   ", best_cycles(b_mm_run, &impls[0], NULL, 3) / 100, 1, "");
	line("  of which 8 words out", "   ", best_cycles(b_mm_out, &impls[0], NULL, 3) / 100, 1, "");
	line("one register read, for scale", "   ", best_cycles(b_mm_one, &impls[0], NULL, 3) / 100, 1, "");
	regfile_bench();
#endif
}

int main(void) {
	for (uint32_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i * 31 + 7);
	out("cryptobench -- %lu MHz; best of repeated runs; wall-clock cycles (docs/cryptobench.md)",
		(unsigned long)(Z_SYSCLK_HZ / 1000000u));

	for (int k = 0; k < 2; k++) {
		const impl_t *im = &impls[k];
		out("");
		out("known answers, %s", im->name);
		int before = failures;
		known_answers(im);
		if (failures == before) out("  all correct");
	}

	uint8_t seed[32] = { 42 };
	crypto_ed25519_key_pair(sk, pk, seed);
	crypto_ed25519_sign(sig32, sk, big, 32);
	crypto_ed25519_sign(sig16k, sk, big, 16384);
	memset(xsk, 5, sizeof(xsk));
	crypto_x25519_public_key(xpk, xsk);

	static uint32_t n200 = 200, n2000 = 2000, k1 = 1024, k16 = 16384;
	struct { const char *what; bench_fn fn; void *arg; int runs; uint32_t per; const char *unit; } B[] = {
		{ "SHA-256, 4 KB", b_sha256_4k, NULL, 3, 64, "64-byte block" },
		{ "BBS password hash, 200 rounds", b_pw, &n200, 2, 0, "" },
		{ "BBS password hash, 2000 rounds", b_pw, &n2000, 1, 0, "" },
		{ "SHA-512, 1 KB", b_sha512, &k1, 3, 1024, "byte" },
		{ "SHA-512, 16 KB", b_sha512, &k16, 2, 16384, "byte" },
		{ "BLAKE2b, 1 KB", b_blake2b, &k1, 3, 1024, "byte" },
		{ "BLAKE2b, 16 KB", b_blake2b, &k16, 2, 16384, "byte" },
		{ "Ed25519 sign, 32 bytes", b_sign32, NULL, 2, 0, "" },
		{ "Ed25519 check, 32 bytes", b_check32, NULL, 2, 0, "" },
		{ "Ed25519 check, 16 KB", b_check16k, NULL, 2, 0, "" },
		{ "X25519", b_x25519, NULL, 2, 0, "" },
		{ "XChaCha20-Poly1305, 1 KB", b_aead, &k1, 3, 1024, "byte" },
		{ "XChaCha20-Poly1305, 16 KB", b_aead, &k16, 2, 16384, "byte" },
	};
	out("");
	out("software");
	for (unsigned b = 0; b < sizeof(B) / sizeof(B[0]); b++) {
		for (int k = 0; k < 2; k++) {
			int before = failures;
			uint32_t c = best_cycles(B[b].fn, &impls[k], B[b].arg, B[b].runs);
			if (failures >= before + 1000) { failures = before; fail("a signature that should check did not"); }
			line(B[b].what, impls[k].name, c, B[b].per, B[b].unit);
		}
	}

	sha256_hw_bench();
	montmul_bench();
	ecdsa_bench();
	z25519_bench();
	mlkem_bench();

#ifdef __riscv
	if (z_soc_has_feature2(Z_FEATURE2_MONTMUL))
		*mm(Z_MONTMUL_W_OWNER) = (z_getpid() ? z_getpid() : 0x7FFFFFFEu) | Z_HW_OWNER_RELEASE;
#endif
	out("");
	out(failures ? "%d FAILURES -- see above" : "done, no failures", failures);
	int h = fs_open_write("/cryptobench.txt");
	if (h >= 0) {
		fs_write_chunk(h, report, report_len);
		fs_close_handle(h);
		printf("written to /cryptobench.txt\n");
	} else printf("could not write /cryptobench.txt\n");
	return failures ? 1 : 0;
}
