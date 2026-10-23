/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * LZ4 block decoder for compressed executables -- see zexec_lz4.h.
 * tests/test_zexec_lz4.c checks it.
 *
 * A block is a run of sequences, each:
 *
 *   token          high nibble: literal count, low: match length - 4;
 *                  15 in either means "and more bytes follow", each
 *                  added, until one is not 255
 *   literals       that many bytes, verbatim
 *   offset         2 bytes, little-endian: how far back the match is
 *   match          (length) bytes copied from that far back
 *
 * and the last sequence stops after its literals.
 */

#include <stdint.h>
#include "zexec_lz4.h"

// The next input byte from a new piece, or -1 at the end of the input.
static int in_refill(z_exec_lz4_t *s) {
	uint32_t n = s->fill(s->ctx, s->buf, Z_EXEC_LZ4_BUF);
	if (!n || n > Z_EXEC_LZ4_BUF) return -1;
	s->p = s->buf + 1;
	s->end = s->buf + n;
	return s->buf[0];
}

#define IN_BYTE(s) ((s)->p != (s)->end ? (int)*(s)->p++ : in_refill(s))

// n input bytes to dst: what the buffer holds, then whole buffers' worth
// straight from `fill` into dst (a long literal run arrives without a
// second copy), the rest through the buffer. 0, or -1 if the input ends
// first.
static int in_copy(z_exec_lz4_t *s, uint8_t *dst, uint32_t n) {
	const uint8_t *p = s->p;
	uint32_t have = (uint32_t)(s->end - p);
	if (have > n) have = n;
	n -= have;
	while (have >= 4) {
		dst[0] = p[0]; dst[1] = p[1]; dst[2] = p[2]; dst[3] = p[3];
		dst += 4; p += 4; have -= 4;
	}
	while (have--) *dst++ = *p++;
	s->p = p;
	while (n >= Z_EXEC_LZ4_BUF) {
		uint32_t k = s->fill(s->ctx, dst, n);
		if (!k || k > n) return -1;
		dst += k;
		n -= k;
	}
	while (n--) {
		int c = IN_BYTE(s);
		if (c < 0) return -1;
		*dst++ = (uint8_t)c;
	}
	return 0;
}

// A length's extra bytes, added to *len; -1 at the end of the input or
// past `max` (which also bounds a run of 255s).
static int more(z_exec_lz4_t *s, uint32_t *len, uint32_t max) {
	int b;
	do {
		b = IN_BYTE(s);
		if (b < 0) return -1;
		*len += (uint32_t)b;
		if (*len > max) return -1;
	} while (b == 255);
	return 0;
}

// A word that may alias bytes (the image is written both ways).
typedef uint32_t __attribute__((may_alias)) lz4_word_t;

// A match: n bytes from dist back. In order, a byte at a time, unrolled:
// with dist < 4 it overlaps what it is writing (dist 1 repeats a byte).
// A longer one goes a word at a time where it can: a run of one byte
// (dist 1: the zeros of a program's tables and pictures), or a copy whose
// source is a whole number of words back (both ends then line up). On
// memory that is slower than the CPU, a store is a store: a quarter of
// them.
static __attribute__((noinline)) void match_words(uint8_t *q, const uint8_t *f, uint32_t dist, uint32_t n) {
	while ((uintptr_t)q & 3) { *q++ = *f++; n--; }
	if (dist == 1) {
		uint32_t v = (uint32_t)q[-1] * 0x01010101u;
		while (n >= 4) { *(lz4_word_t *)q = v; q += 4; n -= 4; }
		while (n--) *q++ = (uint8_t)v;
		return;
	}
	while (n >= 4) {
		*(lz4_word_t *)q = *(const lz4_word_t *)f;
		q += 4; f += 4; n -= 4;
	}
	while (n--) *q++ = *f++;
}

static inline __attribute__((always_inline)) void match(uint8_t *q, uint32_t dist, uint32_t n) {
	const uint8_t *f = q - dist;
	if (n >= 8 && (dist == 1 || !(dist & 3))) { match_words(q, f, dist, n); return; }
	while (n >= 4) {
		q[0] = f[0]; q[1] = f[1]; q[2] = f[2]; q[3] = f[3];
		q += 4; f += 4; n -= 4;
	}
	while (n--) *q++ = *f++;
}

int z_exec_lz4(z_exec_lz4_t *s, uint8_t *out, uint32_t out_len,
	z_exec_fill_t fill, void *ctx) {

	uint32_t o = 0;

	s->p = s->end = s->buf;
	s->fill = fill;
	s->ctx = ctx;

	for (;;) {
		// The fast path, nearly every sequence (wm: 9,777 of them, 4
		// literal bytes and a 7-byte match on average, so the cost is
		// per sequence, not per byte): no length bytes, and the whole
		// sequence -- token, up to 14 literals, offset -- already in the
		// buffer. Read with plain pointer steps, no refill checks.
		const uint8_t *p = s->p;
		if (s->end - p >= 17) {
			uint32_t t = *p, n = t >> 4, m = (t & 15) + 4;
			if (n < 15 && m < 19 && n + 4 <= out_len - o) {
				uint8_t *q = out + o;
				const uint8_t *e = ++p + n;
				while (p < e) *q++ = *p++;
				o += n;
				uint32_t dist = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
				p += 2;
				if (!dist || dist > o || m > out_len - o) return -1;
				s->p = p;
				match(out + o, dist, m);
				o += m;
				continue;
			}
		}

		// The general sequence: length bytes, a refill midway, the end.
		int t = IN_BYTE(s);
		if (t < 0) return -1;
		uint32_t n = (uint32_t)t >> 4;
		if (n) {
			if (n == 15 && more(s, &n, out_len - o)) return -1;
			if (n > out_len - o || in_copy(s, out + o, n)) return -1;
			o += n;
		}
		if (o == out_len) {
			// the last sequence: literals only, and the input ends here
			return (s->p == s->end && !s->fill(s->ctx, s->buf, Z_EXEC_LZ4_BUF)) ? 0 : -1;
		}
		int lo = IN_BYTE(s), hi = IN_BYTE(s);
		if (lo < 0 || hi < 0) return -1;
		uint32_t dist = (uint32_t)lo | ((uint32_t)hi << 8);
		n = ((uint32_t)t & 15) + 4;
		if (n == 19 && more(s, &n, out_len - o)) return -1;
		if (!dist || dist > o || n > out_len - o) return -1;
		match(out + o, dist, n);
		o += n;
	}
}
