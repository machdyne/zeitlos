/*
 * A known-answer test of the whole zfed handshake. docs/fed.md,
 * "Sessions, exactly".
 *
 * Two sessions run the handshake in one process with fixed randomness
 * (plat_fake_random); gen_handshake_kat.py recomputed what must come of
 * it with independent implementations -- cryptography's X25519 and
 * Ed25519, kyber-py's ML-KEM-768, hashlib's HMAC. The first two messages
 * and the transcript hash must match byte for byte, and so must the
 * session keys: the only check that notices a handshake leaving out one
 * of its two secrets, which would otherwise work perfectly.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../core/fsess.h"
#include "../core/fstore.h"
#include "../../../common/zsha256.h"
#include "../../../ext/monocypher/monocypher-ed25519.h"
#include "handshake_kat.h"

extern int plat_quiet, plat_fake_random;
extern uint32_t plat_fake_random_at;
static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static bool yes(const uint8_t k[32], void *c) { (void)k; (void)c; return true; }
static bool no(const uint8_t k[32], void *c) { (void)k; (void)c; return false; }
static bool yes_o(const uint8_t k[32], const char *t, void *c) { (void)k; (void)t; (void)c; return true; }
static fsess_t I, R, R2;

// Moves what one side has to send to the other.
static void pass(fsess_t *from, fsess_t *to) {
	const uint8_t *p;
	uint32_t n = fsess_output(from, &p);
	if (n) { fsess_input(to, p, n); fsess_consumed(from, n); }
}

int main(void) {
	char dir[64], cmd[128];
	uint8_t isk[64], ipk[32], rsk[64], rpk[32], seed[32], h[32];
	plat_quiet = 1;
	snprintf(dir, sizeof(dir), "/tmp/fhs-test-%d", (int)getpid());
	fstore_open(dir, 0, NULL, 0);				// HELLO needs a store's epoch
	memset(seed, 0x11, 32); crypto_ed25519_key_pair(isk, ipk, seed);
	memset(seed, 0x22, 32); crypto_ed25519_key_pair(rsk, rpk, seed);

	plat_fake_random = 1;
	plat_fake_random_at = 0;
	fsess_cfg_t ci = { isk, rpk, NULL, yes_o, "t/*", 0, NULL, false, NULL, NULL };
	fsess_cfg_t cr = { rsk, NULL, yes, yes_o, "t/*", 0, NULL, false, NULL, NULL };
	fsess_init(&I, &ci, true);				// I's X25519 key, I's ML-KEM coins
	fsess_init(&R, &cr, false);				// R's X25519 key
	z_sha256(h, I.m0, FSESS_M0);
	CK(!memcmp(h, kat_m0_sha256, 32), "the first message, byte for byte");
	pass(&I, &R);							// R: the key first, then ML-KEM encaps
	z_sha256(h, R.m1, FSESS_M1);
	CK(!memcmp(h, kat_m1_sha256, 32), "the reply, byte for byte");
	CK(!memcmp(R.th, kat_th, 32), "the transcript hash");
	pass(&R, &I);							// I: decaps, keys, R's AUTH checked
	CK(!memcmp(I.th, kat_th, 32), "the same transcript on both sides");
	CK(!memcmp(I.key_out, kat_i2r, 32) && !memcmp(I.key_in, kat_r2i, 32),
		"the session keys: X25519 and ML-KEM both in them, as recomputed independently");
	CK(!memcmp(R.key_in, kat_i2r, 32) && !memcmp(R.key_out, kat_r2i, 32), "and the responder's the same");
	plat_fake_random = 0;
	for (int i = 0; i < 20 && (I.state == FS_RUNNING || R.state == FS_RUNNING); i++) { pass(&I, &R); pass(&R, &I); }
	CK(I.state == FS_DONE && R.state == FS_DONE, "and the session finishes (%s / %s)", I.error, R.error);

	// a time a moment BEFORE the session began is no time at all -- not
	// 4 billion ms (fed on Zeitlos passed one, and new sessions failed)
	{
		fsess_cfg_t c = { rsk, NULL, yes, yes_o, "t/*", 0, NULL, false, NULL, NULL };
		fsess_init(&R2, &c, false);
		fsess_poll(&R2, 100000);
		fsess_poll(&R2, 99999);
		CK(R2.state == FS_RUNNING, "polled with a time just before its start: still running (%s)", R2.error);
		fsess_poll(&R2, 100000 + FSESS_SHAKE_MS + 1);
		CK(R2.state == FS_FAILED && strstr(R2.error, "handshake"), "and the handshake limit still works (%s)", R2.error);
	}

	// a stranger is refused after 37 bytes: before the ML-KEM key is even read
	{
		fsess_cfg_t c = { rsk, NULL, no, yes_o, "t/*", 0, NULL, false, NULL, NULL };
		fsess_init(&R2, &c, false);
		fsess_input(&R2, I.m0, FSESS_M0_WHO);
		CK(R2.state == FS_FAILED && strstr(R2.error, "not a node"), "a stranger: refused on its first 37 bytes (%s)", R2.error);
		const uint8_t *p;
		CK(fsess_output(&R2, &p) == 0, "and told nothing");
	}
	// an ML-KEM key that fails FIPS 203's check is refused
	{
		static uint8_t m0[FSESS_M0];
		fsess_cfg_t c = { rsk, NULL, yes, yes_o, "t/*", 0, NULL, false, NULL, NULL };
		memcpy(m0, I.m0, FSESS_M0);
		m0[69] = 0xFF; m0[70] |= 0x0F;			// the first coefficient: 4095, not below q
		fsess_init(&R2, &c, false);
		fsess_input(&R2, m0, FSESS_M0);
		CK(R2.state == FS_FAILED && strstr(R2.error, "ML-KEM"), "an ML-KEM key with a coefficient >= q: refused (%s)", R2.error);
	}
	fstore_close();
	snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
	if (system(cmd)) return 1;
	printf("handshake: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
