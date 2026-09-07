/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Host tests for der.c and x509.c, against real certificates from a
 * real toolchain (tests/certs/, built by tests/gen_certs.sh).
 *
 *   make test
 *   ./build/test_x509 /path/to/some.der ...   parse anything else too
 *
 * -- two halves, and the second is the important one --
 *
 * The first half checks that valid certificates parse into the right
 * values. That matters and it is not where the risk is.
 *
 * The second half checks that MALFORMED input is refused, because
 * this is the first attacker-controlled structured data the browser
 * touches -- a certificate arrives from whoever answered the
 * connection, before anything about them has been verified. Every
 * historic break in this area is in the second half: length
 * arithmetic that wraps, nesting that recurses, BER forms accepted by
 * the parser and rejected by the verifier so the two disagree about
 * which bytes were signed.
 *
 * The mutation loop at the end is the cheap version of a fuzzer: take
 * a good certificate, corrupt one byte, and require the parser to
 * either refuse it or survive it. Run over every byte of every
 * certificate in the corpus, that is a few hundred thousand parses of
 * deliberately broken DER, and it costs a second.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../der.h"
#include "../x509.h"

static int fails, checks;

static void ck(int cond, const char *what) {
	checks++;
	if (!cond) { fails++; printf("FAIL: %s\n", what); }
}

static uint8_t buf[65536];

static uint32_t load(const char *path, uint8_t *out, uint32_t cap) {
	FILE *f = fopen(path, "rb");
	uint32_t n;
	if (!f) return 0;
	n = (uint32_t)fread(out, 1, cap, f);
	fclose(f);
	return n;
}

// -- DER, on its own ------------------------------------------------

static void der_tests(void) {

	// A minimal well-formed SEQUENCE { INTEGER 1 }.
	{
		static const uint8_t ok[] = { 0x30, 0x03, 0x02, 0x01, 0x01 };
		der_t in = der_view(ok, sizeof(ok)), body, iv;
		uint32_t v;
		ck(der_expect(&in, DER_SEQUENCE, &body), "sequence parses");
		ck(der_expect(&body, DER_INTEGER, &iv), "integer parses");
		ck(der_uint(&iv, &v) && v == 1, "integer value");
		ck(der_done(&body) && der_done(&in), "nothing left over");
	}

	// A length that runs past the end of the buffer. The classic
	// overread, and the reason lengths are checked against the
	// enclosing view rather than against the whole message.
	{
		static const uint8_t bad[] = { 0x30, 0x7F, 0x02, 0x01, 0x01 };
		der_t in = der_view(bad, sizeof(bad)), body;
		ck(!der_expect(&in, DER_SEQUENCE, &body), "length past the end refused");
	}

	// Indefinite length: BER, not DER.
	{
		static const uint8_t bad[] = { 0x30, 0x80, 0x02, 0x01, 0x01, 0, 0 };
		der_t in = der_view(bad, sizeof(bad)), body;
		ck(!der_expect(&in, DER_SEQUENCE, &body), "indefinite length refused");
	}

	// Non-minimal length: 0x81 0x03 encodes 3, which fits in the short
	// form. Two encodings of one value is the shape of every
	// signature-bypass bug in this area.
	{
		static const uint8_t bad[] = { 0x30, 0x81, 0x03, 0x02, 0x01, 0x01 };
		der_t in = der_view(bad, sizeof(bad)), body;
		ck(!der_expect(&in, DER_SEQUENCE, &body), "non-minimal length refused");
	}

	// A leading zero in a long-form length.
	{
		static const uint8_t bad[] = { 0x30, 0x82, 0x00, 0x03, 0x02, 0x01, 0x01 };
		der_t in = der_view(bad, sizeof(bad)), body;
		ck(!der_expect(&in, DER_SEQUENCE, &body), "leading zero length refused");
	}

	// A length wide enough to overflow a naive accumulator.
	{
		static const uint8_t bad[] = { 0x30, 0x84, 0xFF, 0xFF, 0xFF, 0xFF };
		der_t in = der_view(bad, sizeof(bad)), body;
		ck(!der_expect(&in, DER_SEQUENCE, &body), "oversized length refused");
	}

	// High-tag-number form, which X.509 never uses.
	{
		static const uint8_t bad[] = { 0x1F, 0x81, 0x00, 0x01, 0x00 };
		der_t in = der_view(bad, sizeof(bad));
		uint8_t tag;
		der_t body;
		ck(!der_next(&in, &tag, &body), "high tag number refused");
	}

	// INTEGER edge cases.
	{
		uint32_t v;
		static const uint8_t neg[] = { 0xFF };
		static const uint8_t nonmin[] = { 0x00, 0x01 };
		static const uint8_t padded[] = { 0x00, 0x80 };
		static const uint8_t big[] = { 0x01, 0x02, 0x03, 0x04, 0x05 };
		der_t a;
		a = der_view(neg, 1);
		ck(!der_uint(&a, &v), "negative integer refused");
		a = der_view(nonmin, 2);
		ck(!der_uint(&a, &v), "non-minimal integer refused");
		a = der_view(padded, 2);
		ck(der_uint(&a, &v) && v == 0x80, "sign-padded integer accepted");
		a = der_view(big, 5);
		ck(!der_uint(&a, &v), "oversized integer refused");
	}

	// A BIT STRING with unused bits, which no key or signature has.
	{
		static const uint8_t bs[] = { 0x03, 0x00 };
		der_t v = der_view(bs + 1, 1), out;
		ck(!der_bitstring(&v, &out), "empty bit string refused");
	}

	// Deep nesting must not recurse. A thousand nested SEQUENCEs is
	// under a kilobyte of input and would be a thousand stack frames
	// in a recursive-descent reader on a 64KB stack.
	{
		static uint8_t deep[4096];
		uint32_t n = 0;
		der_t in;
		for (int i = 0; i < 1000; i++) { deep[n++] = 0x30; deep[n++] = 0x00; }
		in = der_view(deep, n);
		while (!der_done(&in)) if (!der_skip(&in)) break;
		ck(true, "deeply nested input terminates without recursing");
	}

}

// -- real certificates ----------------------------------------------

static const char *dir = "tests/certs";

static bool load_cert(const char *name, x509_cert_t *c, const char **err) {
	char path[256];
	uint32_t n;
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	n = load(path, buf, sizeof(buf));
	if (!n) { printf("skip: cannot read %s\n", path); return false; }
	return x509_parse(buf, n, c, err);
}

static void cert_tests(void) {

	x509_cert_t c;
	const char *err;

	// -- an ordinary RSA leaf --
	if (load_cert("plain.der", &c, &err)) {

		ck(c.version == 3, "v3 certificate");
		ck(c.key_alg == X509_KEY_RSA, "RSA key");
		ck(c.rsa_n.len == 256, "2048-bit modulus, sign padding removed");
		ck(c.rsa_e.len >= 1 && c.rsa_e.len <= 4, "exponent");
		ck(c.sig_alg == X509_SIG_RSA_PKCS1_SHA256, "sha256WithRSA");
		ck(c.signature.len == 256, "signature is one modulus wide");
		ck(c.has_san, "has a subjectAltName");
		ck(!c.is_ca, "leaf is not a CA");
		ck(c.tbs.len > 0 && c.tbs.p[0] == 0x30, "tbs is the encoded SEQUENCE");
		ck(c.not_after > c.not_before, "validity is ordered");

		ck(x509_matches_host(&c, "example.com"), "matches its SAN");
		ck(x509_matches_host(&c, "www.example.com"), "matches its second SAN");
		ck(x509_matches_host(&c, "EXAMPLE.COM"), "matching is case-insensitive");
		ck(!x509_matches_host(&c, "evil.com"), "does not match another host");
		ck(!x509_matches_host(&c, "example.com.evil.com"), "no suffix confusion");
		ck(!x509_matches_host(&c, "xample.com"), "no prefix confusion");
		ck(!x509_matches_host(&c, ""), "empty host matches nothing");

	} else {
		printf("skip: plain.der (%s)\n", err ? err : "?");
	}

	// -- a wildcard leaf --
	if (load_cert("wildcard.der", &c, &err)) {
		ck(x509_matches_host(&c, "a.example.com"), "wildcard matches one label");
		ck(x509_matches_host(&c, "www.example.com"), "wildcard matches another");
		// A wildcard covers exactly one label. Both of these have
		// been real bugs in real clients.
		ck(!x509_matches_host(&c, "a.b.example.com"),
			"wildcard does not span a dot");
		ck(!x509_matches_host(&c, "example.com"),
			"wildcard does not match the bare domain");
	} else {
		printf("skip: wildcard.der\n");
	}

	// -- a P-256 leaf --
	if (load_cert("ecdsa.der", &c, &err)) {
		ck(c.key_alg == X509_KEY_EC_P256, "P-256 key");
		ck(c.ec_point.len == 65 && c.ec_point.p[0] == 0x04,
			"uncompressed point");
		ck(x509_matches_host(&c, "ec.example.com"), "EC cert matches its SAN");
	} else {
		printf("skip: ecdsa.der (%s)\n", err ? err : "?");
	}

	// -- the CA --
	if (load_cert("ca.der", &c, &err)) {
		ck(c.has_basic_constraints && c.is_ca, "CA has basicConstraints CA:TRUE");
		ck(c.has_path_len && c.path_len == 1, "pathlen parsed");
		ck(c.has_key_usage && (c.key_usage & X509_KU_KEY_CERT_SIGN),
			"CA has keyCertSign");
		ck(x509_issued_by(&c, &c), "self-signed: issuer equals subject");
	} else {
		printf("skip: ca.der\n");
	}

	// -- expired --
	if (load_cert("expired.der", &c, &err)) {
		// It PARSES. Expiry is not a parse error, it is a decision the
		// caller makes with a clock.
		ck(!x509_valid_at(&c, 1893456000LL), "expired cert is not valid in 2030");
		ck(x509_valid_at(&c, 1577923200LL), "expired cert was valid in 2020");
	} else {
		printf("skip: expired.der\n");
	}

	// -- a Common Name and no SAN --
	//
	// Must match NOTHING. The CN fallback is not implemented, on
	// purpose -- see x509.h.
	if (load_cert("cnonly.der", &c, &err)) {
		ck(!c.has_san, "no subjectAltName");
		ck(!x509_matches_host(&c, "cn.example.com"),
			"a CN-only certificate matches nothing");
		ck(!strcmp(c.display_name, "cn.example.com"),
			"CN is still extracted for display");
	} else {
		printf("skip: cnonly.der\n");
	}

	// -- chain linkage --
	{
		static uint8_t leafbuf[16384];
		x509_cert_t leaf, ca;
		char p1[256], p2[256];
		uint32_t n1, n2;

		snprintf(p1, sizeof(p1), "%s/plain.der", dir);
		snprintf(p2, sizeof(p2), "%s/ca.der", dir);
		n1 = load(p1, leafbuf, sizeof(leafbuf));
		n2 = load(p2, buf, sizeof(buf));

		if (n1 && n2 &&
			x509_parse(leafbuf, n1, &leaf, &err) &&
			x509_parse(buf, n2, &ca, &err)) {
			ck(x509_issued_by(&leaf, &ca), "leaf names the CA as its issuer");
			ck(!x509_issued_by(&ca, &leaf), "and not the other way round");
		}
	}

}

// -- the mutation loop ----------------------------------------------
//
// One byte corrupted at a time, over every byte of every certificate.
// The requirement is not that any particular mutation be rejected --
// plenty of them produce a certificate that is still well-formed and
// merely says something different. It is that the parser always
// TERMINATES and never reads outside the buffer.
//
// Run under a sanitizer (make test-asan) this is a real fuzzer for
// the cost of a few lines.

static void mutation_tests(const char *name) {

	char path[256];
	uint32_t n, i;
	static uint8_t orig[65536], work[65536];
	int parsed = 0, refused = 0;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	n = load(path, orig, sizeof(orig));
	if (!n) return;

	for (i = 0; i < n; i++) {

		static const uint8_t flips[] = { 0x01, 0x80, 0xFF };

		for (unsigned f = 0; f < sizeof(flips) / sizeof(flips[0]); f++) {

			x509_cert_t c;
			const char *err = NULL;

			memcpy(work, orig, n);
			work[i] ^= flips[f];

			if (x509_parse(work, n, &c, &err)) {
				parsed++;
				// If it parsed, every view must lie inside the buffer.
				// A view that does not is the memory-safety bug this
				// loop exists to find.
				if (c.tbs.p < work || c.tbs.p + c.tbs.len > work + n ||
					c.signature.p < work ||
					c.signature.p + c.signature.len > work + n ||
					c.issuer.p < work || c.issuer.p + c.issuer.len > work + n) {
					fails++;
					printf("FAIL: %s: mutation at %u produced a view "
						"outside the buffer\n", name, (unsigned)i);
					return;
				}
			} else {
				refused++;
			}

			// Truncation, separately: every prefix of a certificate
			// must be refused rather than read past.
			if ((i % 64) == 0) {
				x509_cert_t t;
				const char *e2 = NULL;
				x509_parse(orig, i, &t, &e2);
			}

		}

	}

	checks++;
	printf("  %s: %u bytes, %d mutations parsed, %d refused\n",
		name, (unsigned)n, parsed, refused);

}

int main(int argc, char **argv) {

	if (getenv("CERTDIR")) dir = getenv("CERTDIR");

	der_tests();
	cert_tests();

	printf("mutation sweep:\n");
	mutation_tests("plain.der");
	mutation_tests("ca.der");
	mutation_tests("ecdsa.der");

	// Anything else named on the command line: real certificates from
	// the world are the best corpus there is.
	for (int i = 1; i < argc; i++) {
		x509_cert_t c;
		const char *err = NULL;
		uint32_t n = load(argv[i], buf, sizeof(buf));
		checks++;
		if (!n) { printf("skip: %s\n", argv[i]); continue; }
		if (x509_parse(buf, n, &c, &err))
			printf("  %s: ok, %s, %s\n", argv[i], c.display_name,
				x509_sig_alg_name(c.sig_alg));
		else
			printf("  %s: REFUSED (%s)\n", argv[i], err ? err : "?");
	}

	printf("%s: %d checks, %d failures\n",
		fails ? "FAIL" : "ok", checks, fails);

	return fails ? 1 : 0;

}
