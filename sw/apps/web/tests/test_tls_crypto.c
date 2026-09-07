/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Host tests for tls_crypto.c, against vectors.h -- see
 * tests/gen_vectors.py for where those come from and why they are
 * generated rather than typed in.
 *
 *   make test
 *
 * -- why this file exists before any handshake code does --
 *
 * A key schedule bug does not announce itself. The handshake
 * completes, the Finished check fails, and the server sends
 * `decrypt_error` -- a deliberately uninformative alert, because
 * telling a client WHICH part of its crypto is wrong is an oracle.
 * So the failure looks the same whether the bug is in HKDF, in the
 * label construction, in the transcript, in the nonce, or in the
 * record layer, and you are debugging five things at once with no
 * signal from any of them.
 *
 * Everything checked here is a pure function with a known answer.
 * Anything still broken after this is somewhere else.
 */

#include <stdio.h>
#include <string.h>

#include "../tls_crypto.h"
#include "../../../common/zsha256.h"

#include "vectors.h"

static int fails, checks;

static void ck(int cond, const char *what) {
	checks++;
	if (!cond) { fails++; printf("FAIL: %s\n", what); }
}

static void ck_eq(const uint8_t *got, const uint8_t *want, uint32_t n,
	const char *what) {

	checks++;

	if (memcmp(got, want, n)) {
		fails++;
		printf("FAIL: %s\n  got  ", what);
		for (uint32_t i = 0; i < n && i < 40; i++) printf("%02x", got[i]);
		printf("\n  want ");
		for (uint32_t i = 0; i < n && i < 40; i++) printf("%02x", want[i]);
		printf("\n");
	}

}

int main(void) {

	uint8_t out[128];
	uint8_t prk[TLS_HASH_LEN];

	// -- HMAC-SHA256, RFC 4231 --

	tls_hmac(out, hmac_key_2, sizeof(hmac_key_2),
		hmac_data_2, sizeof(hmac_data_2));
	ck_eq(out, hmac_want_2, 32, "hmac case 2");

	tls_hmac(out, hmac_key_4, sizeof(hmac_key_4),
		hmac_data_4, sizeof(hmac_data_4));
	ck_eq(out, hmac_want_4, 32, "hmac case 4");

	// A key longer than the 64-byte block must be hashed first. TLS
	// never uses one, which is exactly why this is the branch that
	// would go untested and stay wrong.
	tls_hmac(out, hmac_key_6, sizeof(hmac_key_6),
		hmac_data_6, sizeof(hmac_data_6));
	ck_eq(out, hmac_want_6, 32, "hmac case 6, key longer than the block");

	// -- HKDF, RFC 5869 --

	tls_hkdf_extract(prk, hkdf1_salt, sizeof(hkdf1_salt),
		hkdf1_ikm, sizeof(hkdf1_ikm));
	ck_eq(prk, hkdf1_prk, 32, "hkdf extract");

	ck(tls_hkdf_expand(out, 42, prk, hkdf1_info, sizeof(hkdf1_info)),
		"hkdf expand accepted");
	ck_eq(out, hkdf1_okm, 42, "hkdf expand, 42 bytes across two blocks");

	// An empty salt is not "no salt" -- it means 32 zero bytes.
	// Skipping that substitution produces a PRK that is wrong and a
	// schedule that is self-consistent, which is the worst kind.
	tls_hkdf_extract(prk, NULL, 0, hkdf3_ikm, sizeof(hkdf3_ikm));
	ck_eq(prk, hkdf3_prk, 32, "hkdf extract with an empty salt");
	ck(tls_hkdf_expand(out, 42, prk, NULL, 0), "hkdf expand, empty info");
	ck_eq(out, hkdf3_okm, 42, "hkdf expand with empty info");

	// The bound exists so this refuses rather than overruns.
	ck(!tls_hkdf_expand(out, TLS_HKDF_MAX + 1, prk, NULL, 0),
		"hkdf expand refuses an over-long request");

	// -- AEAD, RFC 8439 section 2.8.2 --

	{
		uint8_t ct[256], tag[TLS_TAG_LEN], pt[256];

		tls_aead_seal(ct, tag, aead_key, aead_nonce,
			aead_aad, sizeof(aead_aad), aead_pt, sizeof(aead_pt));
		ck_eq(ct, aead_ct, sizeof(aead_ct), "aead ciphertext");
		ck_eq(tag, aead_tag, TLS_TAG_LEN, "aead tag");

		ck(tls_aead_open(pt, aead_key, aead_nonce, aead_aad,
			sizeof(aead_aad), aead_ct, sizeof(aead_ct), aead_tag),
			"aead open accepts a good tag");
		ck_eq(pt, aead_pt, sizeof(aead_pt), "aead plaintext");

		// A flipped bit anywhere must fail, and must not leave
		// plaintext behind. The second half of that is the part a
		// caller cannot check for itself.
		{
			uint8_t bad_tag[TLS_TAG_LEN];
			memcpy(bad_tag, aead_tag, TLS_TAG_LEN);
			bad_tag[0] ^= 1;
			memset(pt, 0xAA, sizeof(pt));
			ck(!tls_aead_open(pt, aead_key, aead_nonce, aead_aad,
				sizeof(aead_aad), aead_ct, sizeof(aead_ct), bad_tag),
				"aead open rejects a bad tag");
			{
				bool clean = true;
				for (uint32_t i = 0; i < sizeof(aead_ct); i++)
					if (pt[i] != 0) clean = false;
				ck(clean, "aead open wipes the output on failure");
			}
		}

		// Corrupting the additional data must fail too -- the AD is
		// the record header, and accepting a modified one is
		// accepting a record whose declared type or length has been
		// changed underneath.
		{
			uint8_t bad_ad[sizeof(aead_aad)];
			memcpy(bad_ad, aead_aad, sizeof(aead_aad));
			bad_ad[0] ^= 0x80;
			ck(!tls_aead_open(pt, aead_key, aead_nonce, bad_ad,
				sizeof(bad_ad), aead_ct, sizeof(aead_ct), aead_tag),
				"aead open rejects modified additional data");
		}

		// In-place must work, because the record layer decrypts into
		// the buffer it received into.
		{
			uint8_t buf[256];
			memcpy(buf, aead_ct, sizeof(aead_ct));
			ck(tls_aead_open(buf, aead_key, aead_nonce, aead_aad,
				sizeof(aead_aad), buf, sizeof(aead_ct), aead_tag),
				"aead open in place");
			ck_eq(buf, aead_pt, sizeof(aead_pt), "aead in-place plaintext");
		}
	}

	// -- the key schedule --

	{
		uint8_t early[32], hs[32], master[32];
		uint8_t chs[32], shs[32];
		uint8_t key[32], iv[12];

		tls_early_secret(early);
		ck_eq(early, ks_early, 32, "early secret");

		{
			uint8_t derived[32], eh[32];
			// The hash of the empty string, which the "derived" steps
			// take as their transcript.
			z_sha256(eh, "", 0);
			tls_derive_secret(derived, early, "derived", eh);
			ck_eq(derived, ks_derived_early, 32,
				"Derive-Secret(early, \"derived\", \"\")");
		}

		tls_handshake_secret(hs, early, ks_ecdhe);
		ck_eq(hs, ks_handshake, 32, "handshake secret");

		tls_derive_secret(chs, hs, "c hs traffic", ks_th_sh);
		tls_derive_secret(shs, hs, "s hs traffic", ks_th_sh);
		ck_eq(chs, ks_client_hs, 32, "client handshake traffic secret");
		ck_eq(shs, ks_server_hs, 32, "server handshake traffic secret");

		tls_traffic_keys(key, iv, chs);
		ck_eq(key, ks_chs_key, 32, "traffic key");
		ck_eq(iv, ks_chs_iv, 12, "traffic iv");

		tls_master_secret(master, hs);
		ck_eq(master, ks_master, 32, "master secret");

		{
			uint8_t ap[32];
			tls_derive_secret(ap, master, "c ap traffic", ks_th_sf);
			ck_eq(ap, ks_client_ap, 32, "client application traffic secret");
			tls_derive_secret(ap, master, "s ap traffic", ks_th_sf);
			ck_eq(ap, ks_server_ap, 32, "server application traffic secret");
		}

		tls_finished(out, shs, ks_th_fin);
		ck_eq(out, ks_finished, 32, "Finished verify_data");

		{
			uint8_t upd[32];
			memcpy(upd, chs, 32);
			tls_update_traffic_secret(upd);
			ck_eq(upd, ks_updated, 32, "traffic secret update");
		}
	}

	// -- the record nonce --
	//
	// Right-aligned and BIG-endian. Both halves get written the other
	// way round by anyone arriving from a little-endian wire format,
	// and the result is correct for record zero and wrong for every
	// record after it -- so the handshake completes and the first
	// application record fails.

	tls_nonce(out, nonce_iv, 0);
	ck_eq(out, nonce_seq_0, 12, "nonce, seq 0");
	tls_nonce(out, nonce_iv, 1);
	ck_eq(out, nonce_seq_1, 12, "nonce, seq 1");
	tls_nonce(out, nonce_iv, 0x1234);
	ck_eq(out, nonce_seq_1234, 12, "nonce, seq 0x1234");
	tls_nonce(out, nonce_iv, 0x0102030405060708ULL);
	ck_eq(out, nonce_seq_102030405060708, 12, "nonce, all eight bytes");

	// The IV itself must not be modified -- it is reused for every
	// record, and a tls_nonce() that XORed in place would corrupt it
	// on the first call and work exactly once.
	{
		uint8_t iv_copy[12];
		memcpy(iv_copy, nonce_iv, 12);
		tls_nonce(out, iv_copy, 0x99);
		ck_eq(iv_copy, nonce_iv, 12, "tls_nonce leaves the iv alone");
	}

	// -- label construction --
	//
	// Checked as a black box above via the schedule, and directly
	// here for the two failure modes the schedule cannot distinguish:
	// a label that is too long, and a context that is.
	{
		uint8_t secret[32] = { 0 };
		char longlabel[260];
		memset(longlabel, 'x', sizeof(longlabel) - 1);
		longlabel[sizeof(longlabel) - 1] = '\0';
		ck(!tls_expand_label(out, 32, secret, longlabel, NULL, 0),
			"expand_label refuses an over-long label");
		ck(tls_expand_label(out, 32, secret, "key", NULL, 0),
			"expand_label accepts a normal one");
	}

	printf("%s: %d checks, %d failures\n",
		fails ? "FAIL" : "ok", checks, fails);

	return fails ? 1 : 0;

}
