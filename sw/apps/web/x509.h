#ifndef X509_H
#define X509_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * X.509 certificate parsing: enough of RFC 5280 to decide whether a
 * TLS server is who it says it is, and no more.
 *
 * -- parsing only, in this file --
 *
 * Nothing here verifies a signature. x509_parse() tells you what a
 * certificate CLAIMS; it says nothing about whether the claim is
 * true. Chain building and signature checking sit above this, and
 * until they exist a parsed certificate is a decorative object.
 *
 * That split is deliberate rather than incidental. The parser is the
 * part that touches attacker-controlled bytes (see der.h), it is the
 * part that can be tested exhaustively against real certificates on
 * the build machine, and it is the part whose bugs are memory-safety
 * bugs rather than logic bugs. Getting it finished and fuzzed first
 * means that when verification is added, a failure there is a failure
 * of the logic and not of the reader underneath it.
 *
 * -- everything is a view --
 *
 * Every field below points into the caller's certificate buffer.
 * Nothing is copied and nothing is owned, so an x509_cert_t is only
 * valid for as long as the bytes it was parsed from. The one
 * exception is the display name, which is copied because it is shown
 * to a person after the buffer may be gone.
 *
 * -- what is deliberately not supported --
 *
 *   - **No CN fallback for hostname matching.** If a certificate has
 *     no subjectAltName it matches nothing, whatever its Common Name
 *     says. RFC 6125 deprecated the fallback, every mainstream
 *     browser removed it, and it is the mechanism behind a long list
 *     of certificate-confusion bugs. A certificate without a SAN has
 *     not been issued by a public CA this decade.
 *   - **No wildcards except a whole leftmost label.** `*.a.com` is
 *     accepted; `www*.a.com` is not, and neither is `*.com`.
 *   - **No IP address SANs.** This browser cannot verify an IP
 *     literal usefully and does not try.
 *   - **No name constraints, no policy constraints, no CRLs, no
 *     OCSP.** Revocation is a Phase 4 conversation at best; on a
 *     machine with no reliable clock and one TCP connection, an OCSP
 *     fetch during a handshake is not something to attempt lightly.
 */

#include <stdint.h>
#include <stdbool.h>

#include "der.h"

// Longest subject name kept for display. Not used for any decision.
#define X509_NAME_MAX 96

typedef enum {
	X509_KEY_UNKNOWN = 0,
	X509_KEY_RSA,
	X509_KEY_EC_P256,
	X509_KEY_EC_P384,
} x509_key_alg_t;

typedef enum {
	X509_SIG_UNKNOWN = 0,
	X509_SIG_RSA_PKCS1_SHA256,
	X509_SIG_RSA_PSS_SHA256,
	X509_SIG_ECDSA_SHA256,
	// SHA-384 and SHA-512 variants appear on real chains and are
	// recognised so that an unsupported-but-known algorithm can be
	// reported as such rather than as a parse failure. Verifying them
	// needs SHA-384/512, which this tree does not have for the first.
	X509_SIG_RSA_PKCS1_SHA384,
	X509_SIG_RSA_PKCS1_SHA512,
	X509_SIG_ECDSA_SHA384,
} x509_sig_alg_t;

typedef struct {

	// The whole certificate, and the tbsCertificate exactly as
	// encoded.
	//
	// `tbs` is the byte range a signature covers. It is kept as a
	// view rather than re-encoded from the parsed fields, because a
	// re-encoding that differs by one byte -- a different string type,
	// a length in the long form -- verifies against nothing.
	der_t			raw;
	der_t			tbs;

	uint32_t		version;		// 1, 2 or 3
	der_t			serial;

	// Issuer and subject as ENCODED DNs. Chain building compares
	// these byte for byte (RFC 5280 says to compare after
	// normalisation; byte equality is stricter, and every real chain
	// satisfies it because the child copies the parent's encoding).
	der_t			issuer;
	der_t			subject;

	// Seconds since the Unix epoch.
	int64_t			not_before;
	int64_t			not_after;

	x509_key_alg_t	key_alg;
	der_t			spki;			// whole SubjectPublicKeyInfo, encoded
	der_t			key_bits;		// the BIT STRING payload

	// For RSA: modulus and exponent, already unwrapped from the
	// RSAPublicKey SEQUENCE inside key_bits, with any DER sign-padding
	// zero removed.
	der_t			rsa_n;
	der_t			rsa_e;

	// The uncompressed public point, beginning 0x04: 65 bytes for
	// P-256, 97 for P-384.
	der_t			ec_point;

	x509_sig_alg_t	sig_alg;
	der_t			signature;		// BIT STRING payload

	// v3 extensions that are acted on.
	bool			has_basic_constraints;
	bool			is_ca;
	bool			has_path_len;
	uint32_t		path_len;
	bool			has_key_usage;
	uint16_t		key_usage;		// bit 5 = keyCertSign
	bool			has_san;
	der_t			san;			// the GeneralNames SEQUENCE contents

	// Subject CN, for the certificate viewer only. NEVER used for
	// hostname matching -- see this file's header.
	char			display_name[X509_NAME_MAX];

} x509_cert_t;

#define X509_KU_KEY_CERT_SIGN  0x0020

// Parses one DER certificate. Returns false on anything malformed;
// `err` receives a short reason if it is not NULL.
//
// A certificate that parses is not a certificate that is trusted, or
// current, or for the right host. It is only one whose bytes made
// sense.
bool x509_parse(const uint8_t *der, uint32_t len, x509_cert_t *out,
	const char **err);

// RFC 6125 hostname matching against the subjectAltName dNSName
// entries. See the header on what is deliberately not accepted.
bool x509_matches_host(const x509_cert_t *c, const char *host);

// notBefore <= now <= notAfter.
//
// `now` is seconds since the epoch. On a board with no battery-backed
// clock this comes from NTP (sw/common/zntp.h) and may be absent
// entirely, which is a decision for the caller: refusing every
// certificate is useless and accepting an expired one silently is
// worse. See docs/x509.md.
bool x509_valid_at(const x509_cert_t *c, int64_t now);

// Does `parent`'s subject equal `child`'s issuer? A necessary
// condition for a chain link and nowhere near a sufficient one --
// the signature check is what makes it mean something.
bool x509_issued_by(const x509_cert_t *child, const x509_cert_t *parent);

// A short human-readable name for the signature algorithm, for error
// messages and the certificate viewer.
const char *x509_sig_alg_name(x509_sig_alg_t a);

#endif
