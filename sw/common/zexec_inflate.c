/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Inflate for ZEXE loaders -- see zexec_inflate.h. The Huffman decoding
 * is canonical, as in zlib's puff.c, with a lookup table for every code
 * of up to Z_EXEC_INFLATE_FAST bits -- nearly all of them -- since a
 * bit-at-a-time walk alone cost 163 instructions a byte. tests/test_zexec_inflate.c checks it against zlib.
 */

#include <stdint.h>
#include "zexec_inflate.h"

static const uint16_t len_base[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
	35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
	3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
	257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
	8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
	7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static const uint8_t cl_order[19] = {
	16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

// The next input byte, or a made-up 0 past the end of it (counted in
// `over`): the bit buffer reads ahead, and the end is checked against
// what was actually consumed (input_ok()), not against what was read.
static uint32_t next_byte(z_exec_inflate_t *s) {
	if (s->p == s->end) {
		uint32_t n = 0;
		if (!s->over) {
			n = s->fill(s->ctx, s->buf, Z_EXEC_INFLATE_BUF);
			if (n > Z_EXEC_INFLATE_BUF) { s->err = 1; n = 0; }
		}
		if (!n) { s->over++; return 0; }
		s->p = s->buf;
		s->end = s->buf + n;
	}
	return *s->p++;
}

static void refill(z_exec_inflate_t *s) {
	while (s->bitcnt <= 24) {
		s->bitbuf |= next_byte(s) << s->bitcnt;
		s->bitcnt += 8;
	}
}

// Nothing consumed from the made-up bytes: the stream is within its input.
static int input_ok(const z_exec_inflate_t *s) {
	return !s->err && s->over * 8 <= (uint32_t)s->bitcnt;
}

// n <= 16.
static uint32_t bits(z_exec_inflate_t *s, int n) {
	if (s->bitcnt < n) refill(s);
	uint32_t v = s->bitbuf & ((1u << n) - 1);
	s->bitbuf >>= n;
	s->bitcnt -= n;
	return v;
}

// Counts and sorted symbols from n code lengths, and the lookup table
// for the codes of up to FAST bits. -1 if over-subscribed; an incomplete
// code is accepted, and an unused code fails in decode().
static int build(uint16_t *cnt, uint16_t *sym, uint16_t *fast, const uint8_t *len, int n) {
	uint16_t offs[16], next[16];
	int left = 1, code = 0;
	for (int i = 0; i < 16; i++) cnt[i] = 0;
	for (int i = 0; i < n; i++) cnt[len[i]]++;
	for (int l = 1; l < 16; l++) {
		left <<= 1;
		left -= cnt[l];
		if (left < 0) return -1;
	}
	offs[1] = 0;
	for (int l = 1; l < 15; l++) offs[l + 1] = (uint16_t)(offs[l] + cnt[l]);
	next[1] = 0;
	for (int l = 2; l < 16; l++) {
		code = (code + cnt[l - 1]) << 1;
		next[l] = (uint16_t)code;
	}
	for (int i = 0; i < (1 << Z_EXEC_INFLATE_FAST); i++) fast[i] = 0;
	for (int i = 0; i < n; i++) {
		int l = len[i];
		if (!l) continue;
		sym[offs[l]++] = (uint16_t)i;
		uint32_t c = next[l]++, r = 0;
		if (l > Z_EXEC_INFLATE_FAST) continue;
		for (int k = 0; k < l; k++) r |= ((c >> k) & 1u) << (l - 1 - k);	// first bit lowest
		for (; r < (1u << Z_EXEC_INFLATE_FAST); r += 1u << l)
			fast[r] = (uint16_t)(i | (l << 12));
	}
	return 0;
}

// The slow way, a bit at a time: a code longer than FAST bits.
static int decode_slow(z_exec_inflate_t *s, const uint16_t *cnt, const uint16_t *sym) {
	int code = 0, first = 0, index = 0;
	for (int l = 1; l < 16; l++) {
		code |= (int)bits(s, 1);
		int c = cnt[l];
		if (code - c < first) return sym[index + (code - first)];
		index += c;
		first += c;
		first <<= 1;
		code <<= 1;
	}
	s->err = 1;
	return -1;
}

static inline int decode(z_exec_inflate_t *s, const uint16_t *fast,
	const uint16_t *cnt, const uint16_t *sym) {
	if (s->bitcnt < Z_EXEC_INFLATE_FAST) refill(s);
	uint32_t e = fast[s->bitbuf & ((1u << Z_EXEC_INFLATE_FAST) - 1)];
	if (e) {
		int l = (int)(e >> 12);
		s->bitbuf >>= l;
		s->bitcnt -= l;
		return (int)(e & 0xfff);
	}
	return decode_slow(s, cnt, sym);
}

static int fixed_tables(z_exec_inflate_t *s) {
	int i = 0;
	for (; i < 144; i++) s->len[i] = 8;
	for (; i < 256; i++) s->len[i] = 9;
	for (; i < 280; i++) s->len[i] = 7;
	for (; i < 288; i++) s->len[i] = 8;
	if (build(s->lcnt, s->lsym, s->lfast, s->len, 288)) return -1;
	for (i = 0; i < 30; i++) s->len[i] = 5;
	return build(s->dcnt, s->dsym, s->dfast, s->len, 30);
}

static int dynamic_tables(z_exec_inflate_t *s) {
	int nlen = (int)bits(s, 5) + 257, ndist = (int)bits(s, 5) + 1;
	int ncode = (int)bits(s, 4) + 4, i;
	if (nlen > 286 || ndist > 30) return -1;
	for (i = 0; i < 19; i++) s->len[cl_order[i]] = i < ncode ? (uint8_t)bits(s, 3) : 0;
	// the code-length code, in the literal tables until they are built
	if (build(s->lcnt, s->lsym, s->lfast, s->len, 19)) return -1;
	for (i = 0; i < nlen + ndist && !s->err; ) {
		int sym = decode(s, s->lfast, s->lcnt, s->lsym), rep;
		uint8_t val = 0;
		if (sym < 0) return -1;
		if (sym < 16) { s->len[i++] = (uint8_t)sym; continue; }
		if (sym == 16) {
			if (!i) return -1;
			val = s->len[i - 1];
			rep = 3 + (int)bits(s, 2);
		} else if (sym == 17) rep = 3 + (int)bits(s, 3);
		else rep = 11 + (int)bits(s, 7);
		if (i + rep > nlen + ndist) return -1;
		while (rep--) s->len[i++] = val;
	}
	if (s->err || !s->len[256]) return -1;			// no end-of-block code
	if (build(s->lcnt, s->lsym, s->lfast, s->len, nlen)) return -1;
	return build(s->dcnt, s->dsym, s->dfast, s->len + nlen, ndist);
}

// One block's codes, the hot loop: the bit buffer and the input pointer
// in locals, synced back to `s` around the slow paths. Returns 0 at the
// end-of-block code.
__attribute__((optimize("O2")))
static int codes(z_exec_inflate_t *s, uint8_t *out, uint32_t *op, uint32_t out_len) {
	const uint16_t *lfast = s->lfast, *dfast = s->dfast;
	const uint8_t *p = s->p, *end = s->end;
	uint32_t bb = s->bitbuf, o = *op;
	int bc = s->bitcnt, rv = -1;

// At least 25 bits in bb: bytes from the buffer while it has them, the
// rest (a new piece, or made-up zeros past the end) through next_byte().
#define REFILL() do { \
		while (bc <= 24) { \
			if (p != end) bb |= (uint32_t)*p++ << bc; \
			else { s->p = p; bb |= next_byte(s) << bc; p = s->p; end = s->end; } \
			bc += 8; \
		} \
	} while (0)
#define SYNC_OUT() do { s->p = p; s->end = end; s->bitbuf = bb; s->bitcnt = bc; } while (0)
#define SYNC_IN() do { p = s->p; end = s->end; bb = s->bitbuf; bc = s->bitcnt; } while (0)

	for (;;) {
		uint32_t e, n, dist;
		int sym;
		if (bc < 15) REFILL();
		e = lfast[bb & ((1u << Z_EXEC_INFLATE_FAST) - 1)];
		if (e) {
			bb >>= e >> 12;
			bc -= (int)(e >> 12);
			sym = (int)(e & 0xfff);
		} else {
			SYNC_OUT();
			sym = decode_slow(s, s->lcnt, s->lsym);
			SYNC_IN();
			if (sym < 0) break;
		}
		if (sym < 256) {
			if (o >= out_len) break;
			out[o++] = (uint8_t)sym;
			continue;
		}
		if (sym == 256) { rv = 0; break; }
		sym -= 257;
		if (sym >= 29) break;
		if (bc < 20) REFILL();
		n = len_base[sym] + (bb & ((1u << len_extra[sym]) - 1));
		bb >>= len_extra[sym];
		bc -= len_extra[sym];
		if (bc < 15) REFILL();
		e = dfast[bb & ((1u << Z_EXEC_INFLATE_FAST) - 1)];
		if (e) {
			bb >>= e >> 12;
			bc -= (int)(e >> 12);
			sym = (int)(e & 0xfff);
		} else {
			SYNC_OUT();
			sym = decode_slow(s, s->dcnt, s->dsym);
			SYNC_IN();
			if (sym < 0) break;
		}
		if (sym >= 30) break;
		if (bc < 13) REFILL();
		dist = dist_base[sym] + (bb & ((1u << dist_extra[sym]) - 1));
		bb >>= dist_extra[sym];
		bc -= dist_extra[sym];
		if (dist > o || n > out_len - o) break;
		{
			uint8_t *q = out + o;
			const uint8_t *f = q - dist;
			o += n;
			while (n--) *q++ = *f++;
		}
	}
	SYNC_OUT();
	*op = o;
	return rv;
#undef REFILL
#undef SYNC_OUT
#undef SYNC_IN
}

int z_exec_inflate(z_exec_inflate_t *s, uint8_t *out, uint32_t out_len,
	z_exec_fill_t fill, void *ctx) {

	uint32_t o = 0;
	int last;

	s->p = s->end = s->buf;
	s->bitbuf = 0;
	s->bitcnt = 0;
	s->err = 0;
	s->over = 0;
	s->fill = fill;
	s->ctx = ctx;

	do {
		last = (int)bits(s, 1);
		int type = (int)bits(s, 2);
		if (type == 0) {
			// stored: the rest of this byte is padding
			bits(s, s->bitcnt & 7);
			uint32_t n = bits(s, 16), nn = bits(s, 16);
			if (n != (~nn & 0xffffu) || n > out_len - o) return -1;
			while (n--) out[o++] = (uint8_t)bits(s, 8);
		} else if (type == 1 || type == 2) {
			if ((type == 1 ? fixed_tables(s) : dynamic_tables(s)) || s->err) return -1;
			if (codes(s, out, &o, out_len)) return -1;
		} else {
			return -1;
		}
		if (!input_ok(s)) return -1;
	} while (!last);

	return o == out_len ? 0 : -1;
}
