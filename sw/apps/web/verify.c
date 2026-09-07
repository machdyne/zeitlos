/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See verify.h.
 */

#include <string.h>
#include <stdio.h>

#include "verify.h"
#include "rsa.h"
#include "ecdsa.h"
#include "sha384.h"
#include "../../common/zsha256.h"

// One parsed certificate per link, plus the root. Kept as statics
// because an x509_cert_t is a few hundred bytes and the chain walk
// needs two live at once; the array exists so the caller's DER
// buffers are parsed exactly once each.
static x509_cert_t parsed[VERIFY_MAX_CHAIN];
static x509_cert_t root_cert;

static verify_clock_fn now_ms;

// -- one cached verification --
//
// Loading a single page takes several connections to the same host: a
// redirect, then the page. Each presents the IDENTICAL chain, and
// re-verifying costs the same tens of seconds every time -- measured
// at 19.5s per P-384 signature on hardware.
//
// So one result is remembered. The key is the hostname AND a hash of
// every byte of the chain as received, so a different host, a
// different chain, or one changed byte all miss. The cached answer
// also expires with the certificates it came from: a chain that
// verified an hour ago must not be trusted past its notAfter.
//
// This does NOT skip proof of possession. CertificateVerify is a
// signature over THIS connection's transcript and is checked in
// tls.c every time (tls.h). Caching the chain caches "this chain is
// trustworthy for this host", which does not vary between
// connections; caching the handshake signature would cache "the peer
// holds the key", which does. Only the first is cached here.
static struct {
	bool		valid;
	char		host[256];
	uint8_t		hash[32];
	int64_t		expires;
} cache;

void verify_flush_cache(void) { cache.valid = false; }

static void chain_hash(const cert_blob_t *chain, uint32_t n, uint8_t out[32]) {
	z_sha256_ctx c;
	z_sha256_init(&c);
	for (uint32_t i = 0; i < n; i++)
		z_sha256_update(&c, chain[i].der, chain[i].len);
	z_sha256_final(&c, out);
}

void verify_set_clock(verify_clock_fn clock) { now_ms = clock; }

// Names for the log. Which algorithm dominates is the whole question
// when a chain check takes long enough for the peer to hang up.
static const char *sig_name(uint8_t alg) {
	switch (alg) {
	case X509_SIG_RSA_PKCS1_SHA256: return "rsa-pkcs1-sha256";
	case X509_SIG_RSA_PKCS1_SHA384: return "rsa-pkcs1-sha384";
	case X509_SIG_RSA_PSS_SHA256:   return "rsa-pss-sha256";
	case X509_SIG_ECDSA_SHA256:     return "ecdsa-sha256";
	case X509_SIG_ECDSA_SHA384:     return "ecdsa-sha384";
	default:                        return "other";
	}
}

static const char *key_name(uint8_t alg) {
	switch (alg) {
	case X509_KEY_RSA:      return "rsa";
	case X509_KEY_EC_P256:  return "p256";
	case X509_KEY_EC_P384:  return "p384";
	default:                return "?";
	}
}

// Splits an ECDSA-Sig-Value: SEQUENCE { INTEGER r, INTEGER s }.
static bool ecdsa_split(const der_t *sig, der_t *r, der_t *s) {

	der_t in = *sig, body;

	if (!der_expect(&in, DER_SEQUENCE, &body)) return false;
	if (!der_expect(&body, DER_INTEGER, r)) return false;
	if (!der_expect(&body, DER_INTEGER, s)) return false;

	// Anything after the two integers is not a signature this
	// understands. Ignoring trailing bytes is how signature-
	// malleability bugs get in.
	return body.len == 0;

}

// Is `child` correctly signed by `parent`'s key?
static bool signature_ok_inner(const x509_cert_t *child,
	const x509_cert_t *parent, const char **err);

static bool signature_ok(const x509_cert_t *child, const x509_cert_t *parent,
	const char **err) {

	uint32_t t0 = now_ms ? now_ms() : 0;
	bool ok = signature_ok_inner(child, parent, err);

	if (now_ms)
		printf("verify: %s under %s key: %s, %lu ms\n",
			sig_name(child->sig_alg), key_name(parent->key_alg),
			ok ? "ok" : "FAILED",
			(unsigned long)(now_ms() - t0));

	return ok;

}

static bool signature_ok_inner(const x509_cert_t *child,
	const x509_cert_t *parent, const char **err) {

	uint8_t hash[48];
	uint32_t hlen;

	// The hash covers the tbsCertificate BYTES AS RECEIVED. x509.c
	// keeps them as a view for exactly this reason -- a re-encoding
	// that differs by a single byte, a different string type or a
	// length in the long form, verifies against nothing.
	//
	// Which hash depends on the signature algorithm, not on anything
	// this connection negotiated: these signatures were made when the
	// certificate was issued.
	switch (child->sig_alg) {
	case X509_SIG_RSA_PKCS1_SHA384:
	case X509_SIG_ECDSA_SHA384:
		sha384(hash, child->tbs.p, child->tbs.len);
		hlen = 48;
		break;
	default:
		z_sha256(hash, child->tbs.p, child->tbs.len);
		hlen = 32;
		break;
	}


	switch (child->sig_alg) {

	case X509_SIG_RSA_PKCS1_SHA256:
		if (parent->key_alg != X509_KEY_RSA) {
			*err = "certificate signed with RSA but issuer key is not RSA";
			return false;
		}
		return rsa_verify_pkcs1_sha256(parent->rsa_n.p, parent->rsa_n.len,
			parent->rsa_e.p, parent->rsa_e.len,
			child->signature.p, child->signature.len, hash);

	case X509_SIG_RSA_PSS_SHA256:
		if (parent->key_alg != X509_KEY_RSA) {
			*err = "certificate signed with RSA but issuer key is not RSA";
			return false;
		}
		return rsa_verify_pss_sha256(parent->rsa_n.p, parent->rsa_n.len,
			parent->rsa_e.p, parent->rsa_e.len,
			child->signature.p, child->signature.len, hash);

	case X509_SIG_ECDSA_SHA256:
	case X509_SIG_ECDSA_SHA384: {

		der_t r, s;
		ec_curve_id_t curve;

		// The CURVE comes from the issuer's key and the HASH from the
		// signature algorithm, and the two are independent. A P-384
		// key signing with SHA-256, or a P-256 key signing with
		// SHA-384, are both legal and both appear; ec_verify() takes
		// the hash length for exactly that reason.
		if (parent->key_alg == X509_KEY_EC_P256) curve = EC_CURVE_P256;
		else if (parent->key_alg == X509_KEY_EC_P384) curve = EC_CURVE_P384;
		else {
			*err = "certificate signed with ECDSA but the issuer key "
				"is not an EC key";
			return false;
		}

		if (!ecdsa_split(&child->signature, &r, &s)) {
			*err = "malformed ECDSA signature";
			return false;
		}

		return ec_verify(curve, parent->ec_point.p, parent->ec_point.len,
			r.p, r.len, s.p, s.len, hash, hlen);
	}

	case X509_SIG_RSA_PKCS1_SHA384:
		if (parent->key_alg != X509_KEY_RSA) {
			*err = "certificate signed with RSA but issuer key is not RSA";
			return false;
		}
		return rsa_verify_pkcs1_sha384(parent->rsa_n.p, parent->rsa_n.len,
			parent->rsa_e.p, parent->rsa_e.len,
			child->signature.p, child->signature.len, hash);

	// Recognised and not supported. Reported as such rather than as a
	// generic failure, because naming the algorithm is actionable
	// where "signature invalid" is not.
	case X509_SIG_RSA_PKCS1_SHA512:
		*err = "chain uses an RSA/SHA-512 signature, which this build "
			"cannot check";
		return false;

	default:
		*err = "unknown signature algorithm";
		return false;

	}

}

// Does `c` look like a usable CA for signing the certificate below
// it?
static bool ca_usable(const x509_cert_t *c, uint32_t depth_below,
	const char **err) {

	// RFC 5280 4.2.1.9: a v3 CA certificate must carry basicConstraints
	// with cA true. A certificate without it is an end-entity
	// certificate, and treating one as a CA is the single most
	// damaging X.509 bug there is -- it lets anyone with a valid
	// certificate for any domain mint certificates for every other
	// domain.
	if (!c->has_basic_constraints || !c->is_ca) {
		*err = "a certificate in the chain is used as a CA but is not one";
		return false;
	}

	// keyUsage, when present, must allow certificate signing.
	if (c->has_key_usage && !(c->key_usage & X509_KU_KEY_CERT_SIGN)) {
		*err = "a CA in the chain is not permitted to sign certificates";
		return false;
	}

	// pathLenConstraint counts the non-self-issued CAs that may
	// follow. depth_below is how many links sit between this CA and
	// the leaf.
	if (c->has_path_len && depth_below > c->path_len + 1) {
		*err = "chain is longer than a CA's pathLenConstraint allows";
		return false;
	}

	return true;

}

bool verify_chain(const cert_blob_t *chain, uint32_t n,
	const char *host, int64_t now,
	verify_root_fn find_root, void *user,
	const char **err) {

	const char *unused = NULL;
	uint32_t i;

	if (!err) err = &unused;
	*err = NULL;

	if (n == 0) { *err = "the server sent no certificates"; return false; }
	if (n > VERIFY_MAX_CHAIN) { *err = "certificate chain too long"; return false; }

	// The cache, checked before any parsing. It is only ever a
	// shortcut to the same answer: every input to that answer is
	// either part of the key or re-checked below.
	{
		uint8_t h[32];
		chain_hash(chain, n, h);

		if (cache.valid && now != 0 && now < cache.expires &&
			!strcmp(cache.host, host) && !memcmp(cache.hash, h, 32)) {
			if (now_ms) printf("verify: chain cached, skipping\n");
			return true;
		}
	}

	// -- parse everything first --
	for (i = 0; i < n; i++) {
		const char *perr = NULL;
		if (!x509_parse(chain[i].der, chain[i].len, &parsed[i], &perr)) {
			*err = perr ? perr : "malformed certificate";
			return false;
		}
	}

	// -- the leaf must be for the host we asked for --
	//
	// Checked FIRST, before any signature work. A certificate that is
	// perfectly valid for another domain is the most likely thing an
	// attacker has, and there is no reason to spend three signature
	// verifications discovering it.
	if (!x509_matches_host(&parsed[0], host)) {
		*err = "the certificate is not valid for this host";
		return false;
	}

	// -- validity dates --
	//
	// `now` of 0 means the caller has no trustworthy clock. That is
	// NOT a reason to skip the check: an expired certificate is the
	// normal state of a certificate whose key has since been
	// compromised, and accepting one silently is most of the way to
	// accepting anything. The caller is told to fix its clock.
	if (now == 0) {
		*err = "the system clock is not set, so certificate dates "
			"cannot be checked";
		return false;
	}

	for (i = 0; i < n; i++) {
		if (!x509_valid_at(&parsed[i], now)) {
			*err = (i == 0)
				? "the certificate has expired or is not yet valid"
				: "a certificate in the chain has expired";
			return false;
		}
	}

	// -- walk the chain, anchoring as EARLY as possible --
	//
	// At each step, ask the local store whether it already trusts
	// this certificate's issuer. If it does, that is the end of the
	// chain: verify this one against the root and ignore whatever
	// else the server sent.
	//
	// This is ordinary path building, and here it is worth real time.
	// en.wikipedia.org sends four certificates, and walking all of
	// them cost four signature checks -- three of them P-384 at about
	// 15 seconds each on this CPU. Anchoring at the first certificate
	// the store recognises cuts that to two.
	//
	// It is also the more correct behaviour: the trust anchor is the
	// one in the local store, not whatever the server chose to append
	// after it. Extra and cross-signed certificates beyond the anchor
	// are exactly that -- extra.
	for (i = 0; i < n; i++) {

		if (i > 0) {
			// parsed[i] is being used as a CA for parsed[i-1].
			if (!ca_usable(&parsed[i], i - 1, err)) return false;
			if (!signature_ok(&parsed[i - 1], &parsed[i], err)) {
				if (!*err) *err = "a signature in the chain is invalid";
				return false;
			}
		}

		if (!find_root || !find_root(user, &parsed[i].issuer, &root_cert))
			{
				// Not anchorable here. Continue only if there is
				// another certificate that claims to have issued this
				// one.
				if (i + 1 >= n) {
					*err = "the certificate chain does not lead to a "
						"known root";
					return false;
				}
				if (!x509_issued_by(&parsed[i], &parsed[i + 1])) {
					// Servers do send chains out of order, or with
					// unrelated certificates included. Reordering
					// them is something a full implementation does;
					// saying so plainly is what this one does.
					*err = "the certificate chain is not in order";
					return false;
				}
				continue;
			}

		// Anchored. The root must still be a usable, unexpired CA,
		// and must actually have signed this certificate -- a
		// matching issuer name proves nothing on its own.
		if (!x509_valid_at(&root_cert, now)) {
			*err = "the trusted root for this chain has expired";
			return false;
		}

		if (!ca_usable(&root_cert, i, err)) return false;

		if (!signature_ok(&parsed[i], &root_cert, err)) {
			if (!*err) *err = "the chain's signature by its root is invalid";
			return false;
		}

		break;

	}

	return true;

}
