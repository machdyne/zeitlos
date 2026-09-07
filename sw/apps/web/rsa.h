#ifndef RSA_H
#define RSA_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * RSA signature verification with SHA-256, in the two schemes that
 * appear on the web: PKCS#1 v1.5 and PSS.
 *
 * VERIFICATION ONLY. There is no signing, no key generation and no
 * private-key operation anywhere in this tree, which is what lets
 * bignum.c skip constant-time discipline entirely -- everything these
 * functions touch is public.
 *
 * -- both schemes, because both are unavoidable --
 *
 * TLS 1.3 itself only allows PSS for RSA handshake signatures
 * (rsa_pss_rsae_sha256), so CertificateVerify is always PSS. But
 * CERTIFICATES are signed by CAs whenever they were issued, and the
 * installed base of intermediates and roots signed with PKCS#1 v1.5
 * is enormous. A client that implemented only PSS would fail on
 * almost every real chain.
 *
 * -- the modulus may arrive with a leading zero --
 *
 * A DER INTEGER is signed, so a modulus whose top bit is set is
 * encoded with a 0x00 in front. Both functions strip it, so a caller
 * can pass x509_cert_t.rsa_n straight through without knowing.
 */

#include <stdint.h>
#include <stdbool.h>

// 4096-bit keys, matching bignum.h. Anything larger is not a key this
// browser needs to interoperate with.
#define RSA_MAX_BYTES 512

// `hash` is the SHA-256 of the signed data -- for a certificate, of
// the tbsCertificate bytes exactly as encoded.
//
// Returns false on any failure whatsoever: bad padding, wrong digest,
// a signature not the same length as the modulus, an oversized key.
// There is deliberately no distinction between those, because the
// caller's only correct response to any of them is to reject the
// certificate.
bool rsa_verify_pkcs1_sha256(const uint8_t *n, uint32_t n_len,
	const uint8_t *e, uint32_t e_len,
	const uint8_t *sig, uint32_t sig_len,
	const uint8_t hash[32]);

// Salt length is fixed at 32, which is what TLS 1.3 mandates for
// rsa_pss_rsae_sha256 (RFC 8446 section 4.2.3). Accepting a variable
// salt length would accept signatures TLS considers invalid.
// PKCS#1 v1.5 with SHA-384. Same scheme, different DigestInfo and a
// 48-byte digest.
//
// Needed for certificate chains, not for TLS: plenty of RSA
// intermediates are signed sha384WithRSAEncryption, and those
// signatures were made long before this connection and are not
// constrained by what the handshake negotiated.
bool rsa_verify_pkcs1_sha384(const uint8_t *n, uint32_t n_len,
	const uint8_t *e, uint32_t e_len,
	const uint8_t *sig, uint32_t sig_len,
	const uint8_t hash[48]);

bool rsa_verify_pss_sha256(const uint8_t *n, uint32_t n_len,
	const uint8_t *e, uint32_t e_len,
	const uint8_t *sig, uint32_t sig_len,
	const uint8_t hash[32]);

#endif
