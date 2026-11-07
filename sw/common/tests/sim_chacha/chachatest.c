/*
 * ChaCha20 on the machine's CPU, in the simulator: z_chacha20_rounds()
 * (sw/common/zrng.c) with the rotate instructions (ZSIM_ROT=1) and
 * without, and monocypher's crypto_chacha20_djb() built on it (as net,
 * netserve and web build it) -- against RFC 8439's vectors, with the
 * instructions each took. docs/chacha.md.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "zeitlos.h"
#include "zsoc.h"
#include "zrng.h"
#include "../../../ext/monocypher/monocypher.h"

static uint32_t instret(void) { uint32_t v; __asm__ volatile ("rdinstret %0" : "=r"(v)); return v; }

static int fails;
static void ck(int ok, const char *what) { printf("  %s  %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

int main(void) {
	// RFC 8439 2.3.2: the block function
	static const uint32_t expect[16] = {
		0xe4e7f110, 0x15593bd1, 0x1fdd0f50, 0xc47120a3, 0xc7f4d1c7, 0x0368c033, 0x9aaa2204, 0x4e6cd4c3,
		0x466482d2, 0x09aa9f07, 0x05d7c214, 0xa2028bd9, 0xd19c12b5, 0xb94e16de, 0xe883d0cb, 0x4e3c50a2,
	};
	uint32_t in[16] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574 }, out[16];
	for (int i = 0; i < 8; i++) in[4 + i] = 0x03020100u + 0x04040404u * (uint32_t)i;
	in[12] = 1; in[13] = 0x09000000; in[14] = 0x4a000000; in[15] = 0;
	bool rot = z_cpu_has_rot();
	printf("chacha: the CPU %s the rotate instructions\n", rot ? "has" : "does not have");

	uint32_t t = instret();
	z_chacha20_rounds(out, in);
	t = instret() - t;
	for (int i = 0; i < 16; i++) out[i] += in[i];
	ck(!memcmp(out, expect, sizeof(out)), "RFC 8439 2.3.2, the block function");
	printf("        one block: %lu instructions\n", (unsigned long)t);

	// RFC 8439 2.4.2 through monocypher (djb's 64-bit counter and nonce:
	// the IETF nonce's first word is the counter's high half)
	static const char plain[] = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
	static const uint8_t first16[16] = { 0x6e, 0x2e, 0x35, 0x9a, 0x25, 0x68, 0xf9, 0x80, 0x41, 0xba, 0x07, 0x28, 0xdd, 0x0d, 0x69, 0x81 };
	static const uint8_t last4[4] = { 0x87, 0x4d };	/* not used: see below */
	(void)last4;
	uint8_t key[32], nonce[8] = { 0, 0, 0, 0x4a, 0, 0, 0, 0 }, ct[114];
	for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
	uint64_t ctr = 1 | ((uint64_t)0 << 32);
	t = instret();
	crypto_chacha20_djb(ct, (const uint8_t *)plain, 114, key, nonce, ctr);
	t = instret() - t;
	ck(!memcmp(ct, first16, 16), "RFC 8439 2.4.2, through monocypher (its first 16 bytes)");
	printf("        114 bytes: %lu instructions\n", (unsigned long)t);

	// a kilobyte of keystream, for the speed
	static uint8_t ks[1024];
	t = instret();
	crypto_chacha20_djb(ks, NULL, sizeof(ks), key, nonce, 0);
	t = instret() - t;
	printf("        1 KB of keystream: %lu instructions\n", (unsigned long)t);

	if (!fails) printf("chacha: all ok, with %s\n", rot ? "rori" : "shifts");
	return fails;
}
