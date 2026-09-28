/*
 * Tests for sealed letters (core/fmail.c): known answers from an
 * independent Python implementation (tests/gen_mail_kat.py), every byte
 * tampered with, the wrong node's keys, what must be refused, and random
 * round trips.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/fmail.h"
#include "../../../common/zsha256.h"
#include "mail_kat.h"

static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
	static fmail_keys_t k, other;
	static uint8_t sealed[8192], opened[8192];
	uint8_t h[32];

	// -- 1. known answers --
	fmail_keys(&k, kat_seed);
	z_sha256(h, k.ek, ZMLKEM_EK_BYTES);
	CK(!memcmp(k.xpk, kat_xpk, 32) && !memcmp(h, kat_ek_sha256, 32), "the mail keys derived from a seed: as Python derives them");
	int n = fmail_seal(sealed, sizeof(sealed), kat_letter, sizeof(kat_letter), k.xpk, k.ek, kat_rnd);
	CK(n == (int)sizeof(kat_sealed) && !memcmp(sealed, kat_sealed, sizeof(kat_sealed)),
		"a sealed letter: byte for byte what Python makes (%d bytes)", n);
	int m = fmail_open(opened, sizeof(opened), kat_sealed, sizeof(kat_sealed), &k);
	CK(m == (int)sizeof(kat_letter) && !memcmp(opened, kat_letter, sizeof(kat_letter)), "Python's sealed letter opens");

	// -- 2. every byte tampered with --
	{
		int opened_bad = 0;
		static uint8_t t[sizeof(kat_sealed)];
		for (uint32_t i = 0; i < sizeof(kat_sealed); i++) {
			memcpy(t, kat_sealed, sizeof(t));
			t[i] ^= 0x01;
			if (fmail_open(opened, sizeof(opened), t, sizeof(t), &k) >= 0) opened_bad++;
		}
		CK(opened_bad == 0, "any one byte changed -- magic, eph, ML-KEM ciphertext, nonce, body, tag: never opens (%d did)", opened_bad);
	}

	// -- 3. the wrong node --
	{
		uint8_t s2[32];
		memset(s2, 0x55, 32);
		fmail_keys(&other, s2);
		CK(fmail_open(opened, sizeof(opened), kat_sealed, sizeof(kat_sealed), &other) < 0, "another node's keys: it does not open");
	}

	// -- 4. what is refused --
	{
		uint8_t zero[32] = { 0 };
		CK(fmail_seal(sealed, sizeof(sealed), kat_letter, sizeof(kat_letter), zero, k.ek, kat_rnd) < 0,
			"an X25519 key of low order (all zero): refused -- no secret from it");
		static uint8_t bad_ek[ZMLKEM_EK_BYTES];
		memcpy(bad_ek, k.ek, sizeof(bad_ek));
		bad_ek[0] = 0xff; bad_ek[1] = 0xff;						// a coefficient >= q
		CK(fmail_seal(sealed, sizeof(sealed), kat_letter, sizeof(kat_letter), k.xpk, bad_ek, kat_rnd) < 0,
			"an ML-KEM key failing FIPS 203's modulus check: refused");
		CK(fmail_open(opened, sizeof(opened), kat_sealed, FMAIL_OVERHEAD - 1, &k) < 0, "shorter than the overhead: refused");
		CK(fmail_open(opened, sizeof(opened), kat_sealed, sizeof(kat_sealed) - 1, &k) < 0, "one byte short: refused");
		CK(fmail_seal(sealed, FMAIL_OVERHEAD + 3, kat_letter, sizeof(kat_letter), k.xpk, k.ek, kat_rnd) < 0,
			"no room to seal into: refused, nothing written past it");
		CK(fmail_open(opened, 10, kat_sealed, sizeof(kat_sealed), &k) < 0, "no room to open into: refused");
	}

	// -- 5. random round trips --
	{
		srand(7);
		int bad = 0;
		static uint8_t letter[3000];
		for (int t = 0; t < 300; t++) {
			uint8_t seed[32], rnd[FMAIL_RANDOM];
			for (int i = 0; i < 32; i++) seed[i] = (uint8_t)rand();
			for (int i = 0; i < FMAIL_RANDOM; i++) rnd[i] = (uint8_t)rand();
			uint32_t len = (uint32_t)(rand() % 3000);
			for (uint32_t i = 0; i < len; i++) letter[i] = (uint8_t)rand();
			fmail_keys(&other, seed);
			int s = fmail_seal(sealed, sizeof(sealed), letter, len, other.xpk, other.ek, rnd);
			int o = s < 0 ? -1 : fmail_open(opened, sizeof(opened), sealed, (uint32_t)s, &other);
			if (s != (int)(len + FMAIL_OVERHEAD) || o != (int)len || memcmp(opened, letter, len)) bad++;
		}
		CK(bad == 0, "300 letters of 0-3000 bytes to random nodes: each opens to itself (%d did not)", bad);
	}

	// -- 6. classical: X25519 alone (ZMX1), for networks that choose it --
	{
		fmail_keys(&k, kat_seed);
		int n2 = fmail_seal_classical(sealed, sizeof(sealed), kat_letter, sizeof(kat_letter), k.xpk, kat_rnd_x);
		CK(n2 == (int)sizeof(kat_sealed_x) && n2 == (int)sizeof(kat_letter) + FMAIL_X_OVERHEAD &&
			!memcmp(sealed, kat_sealed_x, sizeof(kat_sealed_x)), "classical: byte for byte what Python makes, 76 bytes more");
		int m2 = fmail_open(opened, sizeof(opened), kat_sealed_x, sizeof(kat_sealed_x), &k);
		CK(m2 == (int)sizeof(kat_letter) && !memcmp(opened, kat_letter, sizeof(kat_letter)), "and opens -- fmail_open() tells the kinds apart");
		int bad = 0;
		static uint8_t t[sizeof(kat_sealed_x)];
		for (uint32_t i = 0; i < sizeof(kat_sealed_x); i++) {
			memcpy(t, kat_sealed_x, sizeof(t));
			t[i] ^= 0x01;
			if (fmail_open(opened, sizeof(opened), t, sizeof(t), &k) >= 0) bad++;
		}
		CK(bad == 0, "any one byte changed: never opens (%d did)", bad);
		CK(fmail_open(opened, sizeof(opened), kat_sealed_x, sizeof(kat_sealed_x), &other) < 0, "another node's keys: no");
		uint8_t zero[32] = { 0 };
		CK(fmail_seal_classical(sealed, sizeof(sealed), kat_letter, sizeof(kat_letter), zero, kat_rnd_x) < 0, "a low-order key: refused");
		// the kinds cannot stand in for each other
		static uint8_t swapped[sizeof(kat_sealed)];
		memcpy(swapped, kat_sealed, sizeof(swapped));
		memcpy(swapped, FMAIL_X_MAGIC, 4);
		CK(fmail_open(opened, sizeof(opened), swapped, sizeof(swapped), &k) < 0, "a hybrid letter relabelled classical: does not open");
		srand(11);
		int rbad = 0;
		for (int i = 0; i < 100; i++) {
			uint8_t seed[32], rnd[FMAIL_X_RANDOM], letter[500];
			for (int j = 0; j < 32; j++) seed[j] = (uint8_t)rand();
			for (int j = 0; j < FMAIL_X_RANDOM; j++) rnd[j] = (uint8_t)rand();
			uint32_t len = (uint32_t)(rand() % 500);
			for (uint32_t j = 0; j < len; j++) letter[j] = (uint8_t)rand();
			fmail_keys(&other, seed);
			int s = fmail_seal_classical(sealed, sizeof(sealed), letter, len, other.xpk, rnd);
			int o = s < 0 ? -1 : fmail_open(opened, sizeof(opened), sealed, (uint32_t)s, &other);
			if (o != (int)len || memcmp(opened, letter, len)) rbad++;
		}
		CK(rbad == 0, "100 classical letters to random nodes: each opens to itself (%d did not)", rbad);
	}

	printf("fmail: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
