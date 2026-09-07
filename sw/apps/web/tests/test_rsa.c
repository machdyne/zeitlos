/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Host tests for bignum.c and rsa.c.
 *
 * The modexp cases are checked against Python's pow(), the signatures
 * against ones OpenSSL made and considers valid. Neither shares any
 * code with what is being tested -- see tests/gen_rsa_vectors.py.
 *
 * The negative cases matter more than the positive ones. A verifier
 * that accepts every valid signature and also accepts some invalid
 * ones passes every test anyone writes by accident.
 */

#include <stdio.h>
#include <string.h>

#include "../rsa.h"
#include "../bignum.h"

#include "rsa_vectors.h"

static int fails, checks;

static void ck(int cond, const char *what) {
	checks++;
	if (!cond) { fails++; printf("FAIL: %s\n", what); }
}

#define V15(p) rsa_verify_pkcs1_sha256(p##n, sizeof p##n, p##e, sizeof p##e, \
	p##sig15, sizeof p##sig15, p##h)
#define VPSS(p) rsa_verify_pss_sha256(p##n, sizeof p##n, p##e, sizeof p##e, \
	p##sigpss, sizeof p##sigpss, p##h)

int main(void) {

	// -- bignum --
	{
		bn_t a, b, m, o;
		uint8_t buf[8];

		// 4^13 mod 497 == 445, the worked example in every textbook.
		{
			uint8_t bb = 4, ee = 13;
			uint8_t mm[2] = { 0x01, 0xf1 };
			bn_from_bytes(&a, &bb, 1);
			bn_from_bytes(&b, &ee, 1);
			bn_from_bytes(&m, mm, 2);
			ck(bn_modexp(&o, &a, &b, &m), "modexp runs");
			ck(bn_to_bytes(&o, buf, 2) && buf[0] == 0x01 && buf[1] == 0xbd,
				"4^13 mod 497 == 445");
		}

		// An even modulus must be refused, not silently mishandled --
		// Montgomery reduction needs an odd one, and a wrong answer
		// here is a signature that verifies when it should not.
		{
			uint8_t mm[2] = { 0x01, 0xf0 };
			bn_from_bytes(&m, mm, 2);
			ck(!bn_modexp(&o, &a, &b, &m), "even modulus refused");
		}

		// A base wider than the modulus is an invalid signature, not
		// something to reduce.
		{
			uint8_t big[4] = { 0xff, 0xff, 0xff, 0xff };
			uint8_t mm[2] = { 0x01, 0xf1 };
			bn_from_bytes(&a, big, 4);
			bn_from_bytes(&m, mm, 2);
			ck(!bn_modexp(&o, &a, &b, &m), "oversized base refused");
		}
	}

	// -- valid signatures --

	ck(V15(k2048_), "pkcs1 v1.5, 2048-bit");
	ck(V15(k3072_), "pkcs1 v1.5, 3072-bit");
	ck(V15(e3_), "pkcs1 v1.5, e=3");
	ck(VPSS(k2048_), "pss, 2048-bit");
	// 3072 is not redundant: the PSS masked-bit handling was wrong in
	// a way that passed at 2048 and failed here.
	ck(VPSS(k3072_), "pss, 3072-bit");

	// -- forgeries and mistakes --

	{
		uint8_t h[32];
		memcpy(h, k2048_h, 32);
		h[31] ^= 1;
		ck(!rsa_verify_pkcs1_sha256(k2048_n, sizeof k2048_n, k2048_e,
			sizeof k2048_e, k2048_sig15, sizeof k2048_sig15, h),
			"pkcs1 rejects a one-bit-wrong hash");
		ck(!rsa_verify_pss_sha256(k2048_n, sizeof k2048_n, k2048_e,
			sizeof k2048_e, k2048_sigpss, sizeof k2048_sigpss, h),
			"pss rejects a one-bit-wrong hash");
	}

	{
		static uint8_t s[512];
		memcpy(s, k2048_sig15, sizeof k2048_sig15);
		s[100] ^= 0x40;
		ck(!rsa_verify_pkcs1_sha256(k2048_n, sizeof k2048_n, k2048_e,
			sizeof k2048_e, s, sizeof k2048_sig15, k2048_h),
			"pkcs1 rejects a tampered signature");
		memcpy(s, k2048_sigpss, sizeof k2048_sigpss);
		s[10] ^= 0x08;
		ck(!rsa_verify_pss_sha256(k2048_n, sizeof k2048_n, k2048_e,
			sizeof k2048_e, s, sizeof k2048_sigpss, k2048_h),
			"pss rejects a tampered signature");
	}

	// A signature shorter than the modulus, which is how a padding
	// oracle or a length-confusion bug usually gets in.
	ck(!rsa_verify_pkcs1_sha256(k2048_n, sizeof k2048_n, k2048_e,
		sizeof k2048_e, k2048_sig15, sizeof k2048_sig15 - 1, k2048_h),
		"pkcs1 rejects a short signature");

	// The two schemes must not accept each other's signatures.
	ck(!rsa_verify_pkcs1_sha256(k2048_n, sizeof k2048_n, k2048_e,
		sizeof k2048_e, k2048_sigpss, sizeof k2048_sigpss, k2048_h),
		"pkcs1 rejects a pss signature");
	ck(!rsa_verify_pss_sha256(k2048_n, sizeof k2048_n, k2048_e,
		sizeof k2048_e, k2048_sig15, sizeof k2048_sig15, k2048_h),
		"pss rejects a pkcs1 signature");

	// A valid signature under the wrong key.
	ck(!rsa_verify_pkcs1_sha256(k3072_n, sizeof k3072_n, k3072_e,
		sizeof k3072_e, k2048_sig15, sizeof k2048_sig15, k2048_h),
		"pkcs1 rejects a signature made by another key");
	ck(!rsa_verify_pkcs1_sha256(e3_n, sizeof e3_n, e3_e, sizeof e3_e,
		k2048_sig15, sizeof k2048_sig15, k2048_h),
		"pkcs1 rejects a signature under a different modulus");

	// An all-zero signature, which is what an uninitialised buffer
	// looks like.
	{
		static uint8_t z[256];
		memset(z, 0, sizeof(z));
		ck(!rsa_verify_pkcs1_sha256(k2048_n, sizeof k2048_n, k2048_e,
			sizeof k2048_e, z, sizeof(z), k2048_h),
			"pkcs1 rejects an all-zero signature");
		ck(!rsa_verify_pss_sha256(k2048_n, sizeof k2048_n, k2048_e,
			sizeof k2048_e, z, sizeof(z), k2048_h),
			"pss rejects an all-zero signature");
	}

	printf("%s: %d checks, %d failures\n",
		fails ? "FAIL" : "ok", checks, fails);

	return fails ? 1 : 0;

}
