/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See x509.h.
 */

#include <string.h>

#include "x509.h"

// -- object identifiers ---------------------------------------------
//
// Contents only, without the 0x06 tag and length. Written as byte
// arrays rather than dotted strings because comparing encoded bytes
// needs no parser and cannot disagree with one.

static const uint8_t oid_rsa_encryption[]   = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x01 };
static const uint8_t oid_rsa_pkcs1_sha256[] = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0B };
static const uint8_t oid_rsa_pkcs1_sha384[] = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0C };
static const uint8_t oid_rsa_pkcs1_sha512[] = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0D };
static const uint8_t oid_rsa_pss[]          = { 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0A };
static const uint8_t oid_ec_public_key[]    = { 0x2A,0x86,0x48,0xCE,0x3D,0x02,0x01 };
static const uint8_t oid_prime256v1[]       = { 0x2A,0x86,0x48,0xCE,0x3D,0x03,0x01,0x07 };
static const uint8_t oid_secp384r1[]        = { 0x2B,0x81,0x04,0x00,0x22 };
static const uint8_t oid_ecdsa_sha256[]     = { 0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x02 };
static const uint8_t oid_ecdsa_sha384[]     = { 0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x03 };

static const uint8_t oid_cn[]               = { 0x55,0x04,0x03 };
static const uint8_t oid_basic_constraints[]= { 0x55,0x1D,0x13 };
static const uint8_t oid_key_usage[]        = { 0x55,0x1D,0x0F };
static const uint8_t oid_subject_alt_name[] = { 0x55,0x1D,0x11 };

// -- time -----------------------------------------------------------

static bool digits2(const uint8_t *p, int *out) {
	if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return false;
	*out = (p[0] - '0') * 10 + (p[1] - '0');
	return true;
}

// Days from 1970-01-01 to y-m-d, proleptic Gregorian.
//
// Howard Hinnant's civil-from-days, which is exact for the whole
// range and needs no table and no loop. A loop over years would also
// work and would be slower on a certificate whose notAfter is in
// 9999, which is what a root certificate's often is.
static int64_t days_from_civil(int y, int m, int d) {

	int64_t yy = y;
	int64_t era, yoe, doy, doe;

	yy -= (m <= 2);
	era = (yy >= 0 ? yy : yy - 399) / 400;
	yoe = yy - era * 400;
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

	return era * 146097 + doe - 719468;

}

// UTCTime (YYMMDDHHMMSSZ) or GeneralizedTime (YYYYMMDDHHMMSSZ).
//
// Only the Z form is accepted. RFC 5280 requires it, and an offset
// form would mean parsing a timezone to decide whether a certificate
// has expired -- which is a decision no certificate should be able to
// make interesting.
static bool parse_time(uint8_t tag, const der_t *v, int64_t *out) {

	const uint8_t *p = v->p;
	int year, mon, day, hh, mm, ss;
	int n;

	if (tag == DER_UTCTIME) {

		if (v->len != 13 || p[12] != 'Z') return false;
		if (!digits2(p, &n)) return false;
		// RFC 5280: 00-49 means 2000-2049, 50-99 means 1950-1999.
		year = (n < 50) ? 2000 + n : 1900 + n;
		p += 2;

	} else if (tag == DER_GENTIME) {

		int hi, lo;
		if (v->len != 15 || p[14] != 'Z') return false;
		if (!digits2(p, &hi) || !digits2(p + 2, &lo)) return false;
		year = hi * 100 + lo;
		p += 4;

	} else {
		return false;
	}

	if (!digits2(p, &mon) || !digits2(p + 2, &day) ||
		!digits2(p + 4, &hh) || !digits2(p + 6, &mm) ||
		!digits2(p + 8, &ss)) return false;

	if (mon < 1 || mon > 12 || day < 1 || day > 31) return false;
	if (hh > 23 || mm > 59 || ss > 60) return false;

	*out = days_from_civil(year, mon, day) * 86400
		+ (int64_t)hh * 3600 + (int64_t)mm * 60 + ss;

	return true;

}

// -- name ------------------------------------------------------------

// Pulls the last CN out of a Name, for display only.
//
// The LAST rather than the first: in a DN like
// "C=US, O=Example, CN=Example CA, CN=example.com" the more specific
// name is the later one. This is a cosmetic choice because nothing
// decides anything from it (x509.h), but showing the wrong half of a
// name in a certificate viewer is its own small lie.
static void extract_cn(const der_t *name, char *out, uint32_t cap) {

	der_t rdns = *name;

	out[0] = '\0';

	while (!der_done(&rdns)) {

		der_t set, atv, oid, val;
		uint8_t tag;

		if (!der_expect(&rdns, DER_SET, &set)) return;

		while (!der_done(&set)) {

			if (!der_expect(&set, DER_SEQUENCE, &atv)) break;
			if (!der_expect(&atv, DER_OID, &oid)) break;
			if (!der_next(&atv, &tag, &val)) break;

			if (DER_OID_IS(&oid, oid_cn)) {
				uint32_t n = val.len < cap - 1 ? val.len : cap - 1;
				uint32_t w = 0;
				for (uint32_t i = 0; i < n; i++) {
					// Everything here is drawn with a font that has
					// no glyphs outside 0x20..0x7f
					// (sw/common/zfont_data.c), and a control
					// character in a name is a display attack besides.
					uint8_t ch = val.p[i];
					out[w++] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '?';
				}
				out[w] = '\0';
			}

		}

	}

}

// -- extensions -------------------------------------------------------

static bool parse_extensions(x509_cert_t *c, der_t exts, const char **err) {

	while (!der_done(&exts)) {

		der_t ext, oid, val;
		uint8_t tag;
		bool critical = false;

		if (!der_expect(&exts, DER_SEQUENCE, &ext)) {
			*err = "malformed extension";
			return false;
		}

		if (!der_expect(&ext, DER_OID, &oid)) {
			*err = "extension without an OID";
			return false;
		}

		// The critical flag is OPTIONAL and DEFAULT FALSE, so a
		// certificate that is not critical simply omits it.
		if (!der_done(&ext)) {
			der_t peek = ext;
			der_t b;
			if (der_expect(&peek, DER_BOOLEAN, &b)) {
				critical = (b.len == 1 && b.p[0] != 0);
				ext = peek;
			}
		}

		if (!der_expect(&ext, DER_OCTET_STRING, &val)) {
			*err = "extension without a value";
			return false;
		}

		if (DER_OID_IS(&oid, oid_basic_constraints)) {

			der_t bc;
			if (!der_expect(&val, DER_SEQUENCE, &bc)) {
				*err = "malformed basicConstraints";
				return false;
			}
			c->has_basic_constraints = true;
			if (!der_done(&bc)) {
				der_t peek = bc, b;
				if (der_expect(&peek, DER_BOOLEAN, &b)) {
					c->is_ca = (b.len == 1 && b.p[0] != 0);
					bc = peek;
				}
			}
			if (!der_done(&bc)) {
				der_t pl;
				if (der_expect(&bc, DER_INTEGER, &pl)) {
					uint32_t v;
					if (der_uint(&pl, &v)) {
						c->has_path_len = true;
						c->path_len = v;
					}
				}
			}

		} else if (DER_OID_IS(&oid, oid_key_usage)) {

			der_t ku, bits;
			if (!der_expect(&val, DER_BIT_STRING, &ku)) {
				*err = "malformed keyUsage";
				return false;
			}
			// keyUsage is the one BIT STRING here with a non-zero
			// unused-bit count in practice, so it is unpacked by hand
			// rather than through der_bitstring().
			//
			// And the bit order is the trap. X.509 numbers keyUsage
			// bits from 0, and DER puts bit 0 in the MOST significant
			// position of the first octet -- so keyCertSign, bit 5,
			// is mask 0x04 of byte 0, not 0x20. Reading the octet as
			// a little-endian bitmask gives an answer that is wrong
			// for every usage except the ones that happen to be
			// symmetric, and the visible symptom is a CA that appears
			// not to be allowed to sign certificates.
			//
			// Normalised here so that X509_KU_* are plain (1 << n)
			// and nothing above this line has to think about it.
			if (ku.len >= 2) {
				c->has_key_usage = true;
				bits.p = ku.p + 1;
				bits.len = ku.len - 1;
				c->key_usage = 0;
				for (uint32_t n = 0; n < 16 && (n / 8) < bits.len; n++)
					if (bits.p[n / 8] & (0x80u >> (n % 8)))
						c->key_usage |= (uint16_t)(1u << n);
			}

		} else if (DER_OID_IS(&oid, oid_subject_alt_name)) {

			der_t san;
			if (!der_expect(&val, DER_SEQUENCE, &san)) {
				*err = "malformed subjectAltName";
				return false;
			}
			c->has_san = true;
			c->san = san;

		} else if (critical) {

			// An unrecognised CRITICAL extension means the issuer
			// said this certificate must not be used by software that
			// does not understand it. Ignoring it is exactly what
			// "critical" forbids.
			//
			// Name constraints are the case that matters: a CA
			// restricted to one domain, whose restriction this
			// browser cannot enforce, must not be treated as
			// unrestricted.
			*err = "certificate has a critical extension we cannot honour";
			return false;

		}

		(void)tag;

	}

	return true;

}

// -- public key -------------------------------------------------------

static bool parse_spki(x509_cert_t *c, der_t spki, const char **err) {

	der_t alg, oid, bits, payload;

	if (!der_expect(&spki, DER_SEQUENCE, &alg)) {
		*err = "malformed public key algorithm";
		return false;
	}
	if (!der_expect(&alg, DER_OID, &oid)) {
		*err = "public key without an algorithm";
		return false;
	}

	if (!der_expect(&spki, DER_BIT_STRING, &bits)) {
		*err = "malformed public key";
		return false;
	}
	if (!der_bitstring(&bits, &payload)) {
		*err = "public key is not a whole number of bytes";
		return false;
	}

	c->key_bits = payload;

	if (DER_OID_IS(&oid, oid_rsa_encryption)) {

		der_t rsa, n, e;

		c->key_alg = X509_KEY_RSA;

		if (!der_expect(&payload, DER_SEQUENCE, &rsa) ||
			!der_expect(&rsa, DER_INTEGER, &n) ||
			!der_expect(&rsa, DER_INTEGER, &e)) {
			*err = "malformed RSA public key";
			return false;
		}

		// DER pads a positive integer whose top bit is set with a
		// leading zero. Left in, every modulus is one byte longer
		// than the key size and modular arithmetic on it is wrong.
		if (n.len > 1 && n.p[0] == 0) { n.p++; n.len--; }
		if (e.len > 1 && e.p[0] == 0) { e.p++; e.len--; }

		if (n.len < 128) {
			// 1024-bit RSA has been off the public web for a decade
			// and is within reach of a determined attacker. Refusing
			// it here is cheap; the alternative is a green padlock
			// over a key that can be broken.
			*err = "RSA key is too small";
			return false;
		}

		c->rsa_n = n;
		c->rsa_e = e;

	} else if (DER_OID_IS(&oid, oid_ec_public_key)) {

		der_t curve;

		if (!der_expect(&alg, DER_OID, &curve)) {
			*err = "EC key without a named curve";
			return false;
		}
		// P-256 AND P-384.
		//
		// P-256 alone is not enough for the real web: a large share
		// of ECDSA chains put a P-384 intermediate above a P-256
		// leaf, so the leaf verifies and the signature above it
		// cannot be checked at all. That is how this was found --
		// en.wikipedia.org's intermediate produced "EC key is not
		// P-256" from right here.
		{
			uint32_t want;

			if (DER_OID_IS(&curve, oid_prime256v1)) {
				c->key_alg = X509_KEY_EC_P256;
				want = 65;
			} else if (DER_OID_IS(&curve, oid_secp384r1)) {
				c->key_alg = X509_KEY_EC_P384;
				want = 97;
			} else {
				*err = "EC key is on a curve this build does not support "
					"(only P-256 and P-384)";
				return false;
			}

			// Uncompressed point only. The compressed forms need a
			// square root in the field to decode, and nothing on the
			// public web uses them.
			if (payload.len != want || payload.p[0] != 0x04) {
				*err = "EC point is not in uncompressed form";
				return false;
			}
		}

		c->ec_point = payload;

	} else {
		*err = "unsupported public key algorithm";
		return false;
	}

	return true;

}

static x509_sig_alg_t sig_alg_from_oid(const der_t *oid) {
	if (DER_OID_IS(oid, oid_rsa_pkcs1_sha256)) return X509_SIG_RSA_PKCS1_SHA256;
	if (DER_OID_IS(oid, oid_ecdsa_sha256))     return X509_SIG_ECDSA_SHA256;
	if (DER_OID_IS(oid, oid_rsa_pss))          return X509_SIG_RSA_PSS_SHA256;
	if (DER_OID_IS(oid, oid_rsa_pkcs1_sha384)) return X509_SIG_RSA_PKCS1_SHA384;
	if (DER_OID_IS(oid, oid_rsa_pkcs1_sha512)) return X509_SIG_RSA_PKCS1_SHA512;
	if (DER_OID_IS(oid, oid_ecdsa_sha384))     return X509_SIG_ECDSA_SHA384;
	return X509_SIG_UNKNOWN;
}

const char *x509_sig_alg_name(x509_sig_alg_t a) {
	switch (a) {
	case X509_SIG_RSA_PKCS1_SHA256: return "RSA-PKCS1-SHA256";
	case X509_SIG_RSA_PSS_SHA256:   return "RSA-PSS-SHA256";
	case X509_SIG_ECDSA_SHA256:     return "ECDSA-SHA256";
	case X509_SIG_RSA_PKCS1_SHA384: return "RSA-PKCS1-SHA384";
	case X509_SIG_RSA_PKCS1_SHA512: return "RSA-PKCS1-SHA512";
	case X509_SIG_ECDSA_SHA384:     return "ECDSA-SHA384";
	default:                        return "unknown";
	}
}

// -- the certificate --------------------------------------------------

bool x509_parse(const uint8_t *der, uint32_t len, x509_cert_t *out,
	const char **err) {

	const char *dummy;
	der_t all, cert, tbs, tbs_raw, v;
	uint8_t tag;

	if (!err) err = &dummy;
	*err = NULL;

	memset(out, 0, sizeof(*out));

	all = der_view(der, len);

	if (!der_expect_raw(&all, DER_SEQUENCE, &cert, &out->raw)) {
		*err = "not a DER certificate";
		return false;
	}

	// Trailing bytes after the certificate are not a certificate with
	// something appended, they are a message this parser and whatever
	// produced it disagree about.
	if (!der_done(&all)) { *err = "trailing data after the certificate"; return false; }

	if (!der_expect_raw(&cert, DER_SEQUENCE, &tbs, &tbs_raw)) {
		*err = "no tbsCertificate";
		return false;
	}
	out->tbs = tbs_raw;

	// version [0] EXPLICIT, DEFAULT v1
	out->version = 1;
	{
		der_t peek = tbs, ver, iv;
		if (der_expect(&peek, DER_CTX(0), &ver)) {
			uint32_t n;
			if (der_expect(&ver, DER_INTEGER, &iv) && der_uint(&iv, &n)) {
				out->version = n + 1;
				tbs = peek;
			}
		}
	}

	if (!der_expect(&tbs, DER_INTEGER, &out->serial)) {
		*err = "no serial number";
		return false;
	}

	// The inner signature algorithm. RFC 5280 requires it to equal the
	// outer one; a mismatch is a signature-substitution attempt and is
	// checked at the end.
	{
		der_t inner, ioid;
		if (!der_expect(&tbs, DER_SEQUENCE, &inner) ||
			!der_expect(&inner, DER_OID, &ioid)) {
			*err = "no inner signature algorithm";
			return false;
		}
		out->sig_alg = sig_alg_from_oid(&ioid);
	}

	if (!der_expect_raw(&tbs, DER_SEQUENCE, &v, &out->issuer)) {
		*err = "no issuer";
		return false;
	}

	{
		der_t validity, nb, na;
		uint8_t t1, t2;
		if (!der_expect(&tbs, DER_SEQUENCE, &validity) ||
			!der_next(&validity, &t1, &nb) ||
			!der_next(&validity, &t2, &na)) {
			*err = "no validity period";
			return false;
		}
		if (!parse_time(t1, &nb, &out->not_before) ||
			!parse_time(t2, &na, &out->not_after)) {
			*err = "unparseable validity dates";
			return false;
		}
		if (out->not_after < out->not_before) {
			*err = "certificate expires before it starts";
			return false;
		}
	}

	{
		der_t subj;
		if (!der_expect_raw(&tbs, DER_SEQUENCE, &subj, &out->subject)) {
			*err = "no subject";
			return false;
		}
		extract_cn(&subj, out->display_name, sizeof(out->display_name));
	}

	{
		der_t spki;
		if (!der_expect_raw(&tbs, DER_SEQUENCE, &spki, &out->spki)) {
			*err = "no public key";
			return false;
		}
		if (!parse_spki(out, spki, err)) return false;
	}

	// issuerUniqueID [1], subjectUniqueID [2] -- v2, essentially
	// extinct, skipped if present.
	while (!der_done(&tbs)) {

		der_t peek = tbs, body;

		if (!der_next(&peek, &tag, &body)) break;

		if (tag == DER_CTX(3)) {
			der_t exts;
			tbs = peek;
			if (!der_expect(&body, DER_SEQUENCE, &exts)) {
				*err = "malformed extensions";
				return false;
			}
			if (!parse_extensions(out, exts, err)) return false;
			break;
		}

		tbs = peek;

	}

	// The outer AlgorithmIdentifier and the signature itself.
	{
		der_t alg, oid, bits;
		x509_sig_alg_t outer;

		if (!der_expect(&cert, DER_SEQUENCE, &alg) ||
			!der_expect(&alg, DER_OID, &oid)) {
			*err = "no signature algorithm";
			return false;
		}

		outer = sig_alg_from_oid(&oid);

		// RFC 5280 section 4.1.1.2: the two algorithm identifiers
		// must match. They are signed and unsigned respectively -- the
		// inner one is covered by the signature, the outer one is not
		// -- so an attacker can rewrite the outer freely. Believing it
		// over the inner is how a verifier is talked into using a
		// weaker algorithm than the issuer chose.
		if (outer != out->sig_alg) {
			*err = "signature algorithm mismatch between tbs and certificate";
			return false;
		}

		if (!der_expect(&cert, DER_BIT_STRING, &bits) ||
			!der_bitstring(&bits, &out->signature)) {
			*err = "malformed signature";
			return false;
		}
	}

	if (!der_done(&cert)) { *err = "trailing data inside the certificate"; return false; }

	return true;

}

// -- hostname matching -------------------------------------------------

static char lc(char c) {
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// Case-insensitive comparison of a SAN entry against a host, where
// the entry may begin with a whole-label wildcard.
static bool name_matches(const uint8_t *pat, uint32_t plen, const char *host) {

	uint32_t hlen = (uint32_t)strlen(host);

	// An embedded NUL is the classic certificate-confusion attack:
	// "www.bank.com\0.attacker.com" is one string to a parser that
	// respects the length and another to anything using C string
	// functions. Refused outright rather than truncated.
	for (uint32_t i = 0; i < plen; i++)
		if (pat[i] == 0) return false;

	if (plen == 0) return false;

	if (pat[0] == '*') {

		const char *dot;

		// Only a whole leftmost label, and only with a '.' right
		// after it. "www*.a.com" and "*.com" are both refused -- the
		// first because partial-label wildcards have caused real
		// confusion bugs, the second below.
		if (plen < 2 || pat[1] != '.') return false;

		dot = strchr(host, '.');
		if (!dot) return false;

		// The wildcard covers exactly one label, so what follows the
		// host's first dot must equal what follows the pattern's.
		{
			uint32_t suffix_len = plen - 1;			// includes the dot
			uint32_t host_suffix = (uint32_t)strlen(dot);

			if (host_suffix != suffix_len) return false;

			for (uint32_t i = 0; i < suffix_len; i++)
				if (lc(dot[i]) != lc((char)pat[1 + i])) return false;

			// "*.com" must not match "anything.com". Requiring at
			// least two dots in the remainder means the wildcard sits
			// under a registrable name rather than under a public
			// suffix. This is a heuristic -- a real public-suffix
			// list is tens of kilobytes -- and it errs toward
			// refusing.
			{
				int dots = 0;
				for (uint32_t i = 0; i < suffix_len; i++)
					if (pat[1 + i] == '.') dots++;
				if (dots < 2) return false;
			}

			// A wildcard must not match the bare domain itself:
			// "*.a.com" does not cover "a.com".
			if (dot == host) return false;

			return true;
		}

	}

	if (plen != hlen) return false;

	for (uint32_t i = 0; i < plen; i++)
		if (lc((char)pat[i]) != lc(host[i])) return false;

	return true;

}

bool x509_matches_host(const x509_cert_t *c, const char *host) {

	der_t san;

	// No SAN means no match, whatever the Common Name says. See
	// x509.h.
	if (!c->has_san || !host || !*host) return false;

	san = c->san;

	while (!der_done(&san)) {

		uint8_t tag;
		der_t entry;

		if (!der_next(&san, &tag, &entry)) return false;

		// [2] dNSName, primitive. Other GeneralName forms -- rfc822,
		// URI, IP, directoryName -- are skipped rather than
		// interpreted.
		if (tag != DER_CTX_PRIM(2)) continue;

		if (name_matches(entry.p, entry.len, host)) return true;

	}

	return false;

}

bool x509_valid_at(const x509_cert_t *c, int64_t now) {
	return now >= c->not_before && now <= c->not_after;
}

bool x509_issued_by(const x509_cert_t *child, const x509_cert_t *parent) {
	return child->issuer.len == parent->subject.len &&
		!memcmp(child->issuer.p, parent->subject.p, child->issuer.len);
}
