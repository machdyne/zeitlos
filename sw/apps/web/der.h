#ifndef DER_H
#define DER_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A DER reader, sized for X.509 and nothing else.
 *
 * -- this parses bytes an attacker chose --
 *
 * A certificate arrives from whoever answered the connection, before
 * anything about them has been verified. It is the first
 * attacker-controlled structured input this browser touches, and
 * historically it is where TLS implementations get broken: the
 * bugs are integer overflows in length arithmetic, unbounded
 * recursion on nested structures, and readers that accept BER's
 * indefinite lengths and then disagree with the verifier about where
 * a field ended.
 *
 * So the rules here are stricter than the specification requires, and
 * each of them closes one of those:
 *
 *   - NO ALLOCATION. Everything is a (pointer, length) view into the
 *     caller's buffer. Nothing is copied, nothing is owned, nothing
 *     can leak.
 *   - NO RECURSION. Nesting is walked with explicit cursors. A
 *     certificate that is a thousand SEQUENCEs deep costs a thousand
 *     iterations, not a thousand stack frames on a 64KB stack.
 *   - Lengths are checked against the ENCLOSING view before use, and
 *     the arithmetic is done in a way that cannot wrap: the length is
 *     accumulated into a uint32_t that is bounded at 2^24 as it is
 *     read, rather than read first and checked afterwards.
 *   - Indefinite lengths are REFUSED. They are BER, not DER, and a
 *     certificate that uses one is malformed. Accepting it is how a
 *     signature ends up covering different bytes than the parse did.
 *   - Non-minimal lengths are REFUSED. DER requires the shortest
 *     form; two encodings of the same value are two ways to describe
 *     one certificate, which is the shape of every signature-bypass
 *     bug in this area.
 *   - Leading zero bytes in a length are REFUSED, for the same
 *     reason.
 *
 * -- what it does not do --
 *
 * No high-tag-number form (tags above 30). X.509 uses none. No BER,
 * no streaming, no encoder. If a certificate needs any of those, it
 * is not one this browser will accept.
 */

#include <stdint.h>
#include <stdbool.h>

// Universal tags used by X.509.
#define DER_BOOLEAN       0x01
#define DER_INTEGER       0x02
#define DER_BIT_STRING    0x03
#define DER_OCTET_STRING  0x04
#define DER_NULL          0x05
#define DER_OID           0x06
#define DER_UTF8STRING    0x0C
#define DER_SEQUENCE      0x30
#define DER_SET           0x31
#define DER_PRINTABLE     0x13
#define DER_T61STRING     0x14
#define DER_IA5STRING     0x16
#define DER_UTCTIME       0x17
#define DER_GENTIME       0x18

// Context-specific, constructed: [0] .. [3] as X.509 uses them.
#define DER_CTX(n)        (uint8_t)(0xA0 | (n))
// Context-specific, primitive: SAN entries are these.
#define DER_CTX_PRIM(n)   (uint8_t)(0x80 | (n))

// A view over a run of bytes. Never owns anything.
typedef struct {
	const uint8_t	*p;
	uint32_t		len;
} der_t;

// The largest single value accepted, and therefore the largest
// certificate. 2^24 rather than 2^32 so that every length addition
// below is provably free of overflow on a 32-bit machine, and because
// a 16MB certificate is not a certificate.
#define DER_MAX_LEN  (1u << 24)

static inline der_t der_view(const void *p, uint32_t len) {
	der_t v; v.p = (const uint8_t *)p; v.len = len; return v;
}

// Reads one TLV from the front of `in`, advancing it past the whole
// element. `tag` receives the identifier octet; `body` a view of the
// contents only.
//
// Returns false on any malformation, at which point `in` is left
// unmodified so a caller can report where it stopped.
bool der_next(der_t *in, uint8_t *tag, der_t *body);

// Reads one TLV and requires it to have exactly `tag`.
bool der_expect(der_t *in, uint8_t tag, der_t *body);

// Like der_expect, but also returns a view of the ENTIRE element,
// header included.
//
// X.509 needs this in two places that both matter: the signature
// covers the encoded tbsCertificate byte for byte, and a DN is
// compared between issuer and subject as encoded bytes. Re-encoding
// either from a parse is how a chain stops linking up.
bool der_expect_raw(der_t *in, uint8_t tag, der_t *body, der_t *whole);

// Steps over the next element without looking at it.
bool der_skip(der_t *in);

// True when nothing is left.
static inline bool der_done(const der_t *v) { return v->len == 0; }

// A non-negative INTEGER that fits in 32 bits. Refuses negatives, the
// non-minimal encodings DER forbids, and anything too large -- all of
// which are malformed rather than merely inconvenient.
bool der_uint(const der_t *v, uint32_t *out);

// Compares an OID's contents against a literal.
bool der_oid_is(const der_t *v, const uint8_t *oid, uint32_t oid_len);

#define DER_OID_IS(v, lit) der_oid_is((v), (lit), (uint32_t)sizeof(lit))

// A BIT STRING's payload, with the unused-bits octet removed.
// Refuses a non-zero unused-bit count: every bit string in a
// certificate this browser reads is a whole number of bytes, and one
// that is not is either malformed or something unexpected.
bool der_bitstring(const der_t *v, der_t *out);

#endif
