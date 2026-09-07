/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * RSA signature VERIFICATION with SHA-256: PKCS#1 v1.5 and PSS.
 *
 * See rsa.h. Everything here operates on public values only -- a
 * public key, a signature off the wire, and a hash of data the
 * attacker already has -- so nothing needs to be constant time. See
 * bignum.h on why that claim is about this use and not about RSA.
 */

#include <string.h>

#include "rsa.h"
#include "bignum.h"
#include "../../common/zsha256.h"

// The DigestInfo prefix for SHA-256: the DER encoding of an
// AlgorithmIdentifier for id-sha256 with NULL parameters, followed by
// the OCTET STRING header for a 32-byte digest.
//
// Hard-coded rather than parsed. PKCS#1 v1.5 verification is
// notorious for implementations that PARSE the DigestInfo instead of
// comparing it, which lets an attacker append or restructure fields
// and forge signatures against small exponents (the Bleichenbacher
// e=3 attack, and its many rediscoveries). The only safe way to check
// this encoding is to build the expected bytes and compare all of
// them.
static const uint8_t sha256_digestinfo[] = {
	0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86,
	0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05,
	0x00, 0x04, 0x20,
};

// The DigestInfo prefix for SHA-384. Same construction as the
// SHA-256 one above and hard-coded for the same reason.
static const uint8_t sha384_digestinfo[] = {
	0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86,
	0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05,
	0x00, 0x04, 0x30,
};

#define HLEN 32

// Constant-time-ish equality. Not required here -- see above -- but
// it costs nothing and stops a future caller from reusing this on
// something secret and getting a surprise.
static bool eq(const uint8_t *a, const uint8_t *b, uint32_t n) {
	uint8_t d = 0;
	for (uint32_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
	return d == 0;
}

// s^e mod n, into exactly k bytes where k is the modulus size.
static bool rsavp1(uint8_t *em, uint32_t k,
	const uint8_t *n, uint32_t n_len,
	const uint8_t *e, uint32_t e_len,
	const uint8_t *sig, uint32_t sig_len) {

	static bn_t N, E, S, M;

	// A signature must be the same length as the modulus and strictly
	// less than it. Both are checked: a shorter signature that
	// happens to verify is a forgery attempt, and bn_modexp() refuses
	// a base wider than the modulus rather than reducing it.
	if (sig_len != k) return false;

	if (!bn_from_bytes(&N, n, n_len)) return false;
	if (!bn_from_bytes(&E, e, e_len)) return false;
	if (!bn_from_bytes(&S, sig, sig_len)) return false;

	if (bn_cmp(&S, &N) >= 0) return false;

	if (!bn_modexp(&M, &S, &E, &N)) return false;

	return bn_to_bytes(&M, em, k);

}

// -- PKCS#1 v1.5 ----------------------------------------------------

// The shared body of both PKCS#1 v1.5 variants. The scheme does not
// change with the digest -- only the DigestInfo prefix and the hash
// length do.
static bool pkcs1_verify(const uint8_t *n, uint32_t n_len,
	const uint8_t *e, uint32_t e_len,
	const uint8_t *sig, uint32_t sig_len,
	const uint8_t *hash, uint32_t hlen,
	const uint8_t *prefix, uint32_t prefix_len) {

	static uint8_t em[RSA_MAX_BYTES];
	static uint8_t expect[RSA_MAX_BYTES];
	uint32_t k, tlen = prefix_len + hlen;
	uint32_t i, ps;

	while (n_len && *n == 0) { n++; n_len--; }
	k = n_len;

	if (k > RSA_MAX_BYTES) return false;

	// The padding must leave room for at least 8 bytes of 0xFF, which
	// is what makes the encoding unambiguous.
	if (k < tlen + 11) return false;

	if (!rsavp1(em, k, n, n_len, e, e_len, sig, sig_len)) return false;

	// The expected encoding is built in full and compared byte for
	// byte. Nothing is parsed -- see the note on sha256_digestinfo.
	expect[0] = 0x00;
	expect[1] = 0x01;
	ps = k - tlen - 3;
	for (i = 0; i < ps; i++) expect[2 + i] = 0xFF;
	expect[2 + ps] = 0x00;
	memcpy(expect + 3 + ps, prefix, prefix_len);
	memcpy(expect + 3 + ps + prefix_len, hash, hlen);

	return eq(em, expect, k);

}

bool rsa_verify_pkcs1_sha256(const uint8_t *n, uint32_t n_len,
	const uint8_t *e, uint32_t e_len,
	const uint8_t *sig, uint32_t sig_len,
	const uint8_t hash[32]) {
	return pkcs1_verify(n, n_len, e, e_len, sig, sig_len, hash, 32,
		sha256_digestinfo, sizeof(sha256_digestinfo));
}

bool rsa_verify_pkcs1_sha384(const uint8_t *n, uint32_t n_len,
	const uint8_t *e, uint32_t e_len,
	const uint8_t *sig, uint32_t sig_len,
	const uint8_t hash[48]) {
	return pkcs1_verify(n, n_len, e, e_len, sig, sig_len, hash, 48,
		sha384_digestinfo, sizeof(sha384_digestinfo));
}

// -- PSS -------------------------------------------------------------

// MGF1 with SHA-256 (RFC 8017 B.2.1).
static void mgf1(uint8_t *mask, uint32_t mask_len,
	const uint8_t *seed, uint32_t seed_len) {

	uint32_t done = 0;
	uint32_t counter = 0;

	while (done < mask_len) {

		z_sha256_ctx c;
		uint8_t digest[HLEN];
		uint8_t cb[4];
		uint32_t take;

		cb[0] = (uint8_t)(counter >> 24);
		cb[1] = (uint8_t)(counter >> 16);
		cb[2] = (uint8_t)(counter >> 8);
		cb[3] = (uint8_t)counter;

		z_sha256_init(&c);
		z_sha256_update(&c, seed, seed_len);
		z_sha256_update(&c, cb, 4);
		z_sha256_final(&c, digest);

		take = mask_len - done;
		if (take > HLEN) take = HLEN;
		memcpy(mask + done, digest, take);

		done += take;
		counter++;

	}

}

bool rsa_verify_pss_sha256(const uint8_t *n, uint32_t n_len,
	const uint8_t *e, uint32_t e_len,
	const uint8_t *sig, uint32_t sig_len,
	const uint8_t hash[32]) {

	static uint8_t em[RSA_MAX_BYTES];
	static uint8_t db[RSA_MAX_BYTES];
	static uint8_t mask[RSA_MAX_BYTES];
	uint32_t k, em_bits, em_len, db_len;
	const uint8_t *h;
	uint32_t i;

	while (n_len && *n == 0) { n++; n_len--; }
	k = n_len;

	if (k > RSA_MAX_BYTES) return false;

	if (!rsavp1(em, k, n, n_len, e, e_len, sig, sig_len)) return false;

	// emBits is modBits - 1, so when the modulus length is a whole
	// number of bytes -- which it always is in practice -- emLen is
	// k and the top bit of EM must be zero.
	{
		bn_t N;
		if (!bn_from_bytes(&N, n, n_len)) return false;
		em_bits = bn_bits(&N) - 1;
	}
	em_len = (em_bits + 7) / 8;

	if (em_len > k) return false;

	// EM may be shorter than k; RSAVP1 left-padded it, so skip the
	// leading zeros that padding added.
	{
		uint32_t pad = k - em_len;
		for (i = 0; i < pad; i++) if (em[i] != 0) return false;
		memmove(em, em + pad, em_len);
	}

	// Salt length is fixed at the hash length. That is what TLS 1.3
	// mandates for rsa_pss_rsae_sha256 (RFC 8446 section 4.2.3), so a
	// variable salt length is not a case that needs supporting -- and
	// accepting one would accept signatures TLS says are invalid.
	if (em_len < HLEN + HLEN + 2) return false;

	if (em[em_len - 1] != 0xBC) return false;

	db_len = em_len - HLEN - 1;
	h = em + db_len;

	// RFC 8017 9.1.2 step 6: the leftmost 8*emLen - emBits bits of
	// maskedDB -- that is, of EM itself -- must be zero. This is a
	// CHECK.
	//
	// Step 9 then says to SET the same bits of DB to zero after
	// unmasking. That is a CLEAR, not a check, because the encoder
	// zeroed them in maskedDB after masking, so the corresponding
	// bits of DB carry whatever the mask had there.
	//
	// The first version of this checked DB instead of clearing it,
	// which is the same mistake made backwards. It passed at 2048
	// bits and failed at 3072 purely because the relevant mask bit
	// happened to be zero in one case and one in the other -- a bug
	// that a single key size would have hidden completely.
	{
		uint32_t clear = 8 * em_len - em_bits;
		if (clear) {
			uint8_t top = (uint8_t)(0xFFu << (8 - clear));
			if (em[0] & top) return false;
		}
	}

	mgf1(mask, db_len, h, HLEN);
	for (i = 0; i < db_len; i++) db[i] = (uint8_t)(em[i] ^ mask[i]);

	{
		uint32_t clear = 8 * em_len - em_bits;
		if (clear) {
			uint8_t keep = (uint8_t)(0xFFu >> clear);
			db[0] &= keep;
		}
	}

	// DB = PS (zeros) || 0x01 || salt
	{
		uint32_t ps = db_len - HLEN - 1;
		for (i = 0; i < ps; i++) if (db[i] != 0) return false;
		if (db[ps] != 0x01) return false;
	}

	// H' = SHA-256(eight zero bytes || mHash || salt)
	{
		static const uint8_t pad8[8] = { 0 };
		z_sha256_ctx c;
		uint8_t hp[HLEN];

		z_sha256_init(&c);
		z_sha256_update(&c, pad8, 8);
		z_sha256_update(&c, hash, HLEN);
		z_sha256_update(&c, db + db_len - HLEN, HLEN);
		z_sha256_final(&c, hp);

		return eq(hp, h, HLEN);
	}

}
