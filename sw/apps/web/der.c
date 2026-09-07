/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See der.h.
 */

#include <string.h>

#include "der.h"

// Reads the identifier and length, leaving `at` on the first content
// byte. Returns the header size in *hdr and the content length in
// *body_len.
static bool read_header(const der_t *in, uint8_t *tag,
	uint32_t *hdr, uint32_t *body_len) {

	uint32_t i = 0;
	uint32_t len;

	if (in->len < 2) return false;

	*tag = in->p[i++];

	// The high-tag-number form (low five bits all set) means the tag
	// continues into following octets. X.509 uses no such tag, so
	// this is refused rather than parsed -- accepting a form nothing
	// legitimate uses only widens what a certificate can say.
	if ((*tag & 0x1F) == 0x1F) return false;

	{
		uint8_t l = in->p[i++];

		if (l < 0x80) {

			len = l;

		} else if (l == 0x80) {

			// Indefinite length: BER, not DER. See der.h.
			return false;

		} else if (l == 0xFF) {

			// Reserved by X.690.
			return false;

		} else {

			uint32_t n = l & 0x7F;

			// A length needing more than three octets would exceed
			// DER_MAX_LEN anyway, and refusing here means the
			// accumulation below cannot overflow rather than being
			// checked afterwards.
			if (n > 3) return false;
			if (in->len < i + n) return false;

			// A leading zero octet is a non-minimal encoding.
			if (in->p[i] == 0) return false;

			len = 0;
			while (n--) len = (len << 8) | in->p[i++];

			// DER requires the shortest form: anything under 128 must
			// have used the single-octet form. Two encodings of one
			// value is the shape of every signature-bypass bug in
			// this area.
			if (len < 0x80) return false;
		}
	}

	if (len > DER_MAX_LEN) return false;

	// Checked against the ENCLOSING view, and written so that neither
	// side of the comparison can wrap: i is at most 5, len is at most
	// 2^24, and in->len is whatever the caller had.
	if (len > in->len - i) return false;

	*hdr = i;
	*body_len = len;

	return true;

}

bool der_next(der_t *in, uint8_t *tag, der_t *body) {

	uint32_t hdr, len;

	if (!read_header(in, tag, &hdr, &len)) return false;

	body->p = in->p + hdr;
	body->len = len;

	in->p += hdr + len;
	in->len -= hdr + len;

	return true;

}

bool der_expect(der_t *in, uint8_t tag, der_t *body) {

	der_t save = *in;
	uint8_t got;

	if (!der_next(in, &got, body)) return false;

	if (got != tag) { *in = save; return false; }

	return true;

}

bool der_expect_raw(der_t *in, uint8_t tag, der_t *body, der_t *whole) {

	der_t save = *in;
	uint8_t got;
	uint32_t hdr, len;

	if (!read_header(in, &got, &hdr, &len)) return false;
	if (got != tag) return false;

	whole->p = in->p;
	whole->len = hdr + len;

	if (!der_next(in, &got, body)) { *in = save; return false; }

	return true;

}

bool der_skip(der_t *in) {
	uint8_t tag;
	der_t body;
	return der_next(in, &tag, &body);
}

bool der_uint(const der_t *v, uint32_t *out) {

	uint32_t i = 0;
	uint32_t val = 0;

	if (v->len == 0) return false;

	// The top bit of the first octet is the sign. Negative integers
	// exist in DER and have no meaning in any field this reads.
	if (v->p[0] & 0x80) return false;

	// A single leading zero is the padding that keeps a large positive
	// value from looking negative; two is non-minimal.
	if (v->len > 1 && v->p[0] == 0 && !(v->p[1] & 0x80)) return false;

	if (v->p[0] == 0) i = 1;

	if (v->len - i > 4) return false;

	for (; i < v->len; i++) val = (val << 8) | v->p[i];

	*out = val;
	return true;

}

bool der_oid_is(const der_t *v, const uint8_t *oid, uint32_t oid_len) {
	return v->len == oid_len && !memcmp(v->p, oid, oid_len);
}

bool der_bitstring(const der_t *v, der_t *out) {

	// At least the unused-bits octet AND one byte of payload. An
	// empty BIT STRING is legal DER and meaningless as a key or a
	// signature, and letting one through here would hand a caller a
	// zero-length signature to "verify".
	if (v->len < 2) return false;

	// The first octet counts the unused bits in the final byte. See
	// der.h on why anything but zero is refused here.
	if (v->p[0] != 0) return false;

	out->p = v->p + 1;
	out->len = v->len - 1;

	return true;

}
