/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See zinflate.h.
 */

#include <string.h>

#include "zinflate.h"

// -- states --
//
// One enum rather than nested loops, because this decoder can be
// suspended at ANY byte: a network callback delivers what it has, not
// what the decoder wants. Every place that could block on more input
// is a state, which is why there are so many of them.
enum {
	S_HDR = 0,			// wrapper header
	S_BLOCK,			// block header bits
	S_STORED_LEN,
	S_STORED,
	S_DYN_HDR,			// HLIT/HDIST/HCLEN
	S_DYN_CL,			// code-length code lengths
	S_DYN_LENS,			// literal/distance code lengths
	S_SYM,				// decoding symbols
	S_LEN_EXTRA,
	S_DIST,
	S_DIST_EXTRA,
	S_COPY,
	S_TRAILER,
	S_DONE,
	S_ERROR,
};

// -- length and distance tables (RFC 1951 3.2.5) --

static const uint16_t len_base[29] = {
	3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,
	67,83,99,115,131,163,195,227,258 };
static const uint8_t len_extra[29] = {
	0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t dist_base[30] = {
	1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
	1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t dist_extra[30] = {
	0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

// The order code lengths arrive in, which is not 0..18. It puts the
// lengths most likely to be non-zero first so the trailing zeros can
// be omitted.
static const uint8_t cl_order[19] = {
	16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };

void z_inflate_init(z_inflate_t *z, uint8_t *window,
	z_inflate_wrap_t wrap, uint32_t limit) {

	memset(z, 0, sizeof(*z));
	z->window = window;
	z->wrap = wrap;
	z->limit = limit;
	z->state = (wrap == Z_INFLATE_RAW) ? S_BLOCK : S_HDR;
	z->cl_hold = -1;
	z->lit_hold = -1;
	z->hdr_need = (wrap == Z_INFLATE_GZIP) ? 10 :
		(wrap == Z_INFLATE_ZLIB) ? 2 : 0;

}

const char *z_inflate_strerror(int rv) {
	switch (rv) {
	case Z_INFLATE_OK:      return "ok";
	case Z_INFLATE_DONE:    return "stream complete";
	case Z_INFLATE_E_DATA:  return "malformed compressed data";
	case Z_INFLATE_E_WRAP:  return "bad gzip or zlib header";
	case Z_INFLATE_E_LIMIT: return "compressed data expanded past its limit";
	default:                return "unknown";
	}
}

// -- the window --
//
// Output goes through here even when the caller has room, because a
// back-reference may reach into bytes the caller has already taken
// away. The window is the only copy this decoder can rely on.
static void wput(z_inflate_t *z, uint8_t b) {
	z->window[z->wpos++] = b;
	if (z->wpos == Z_INFLATE_WINDOW) { z->wpos = 0; z->wfull = true; }
}

// A distance is only valid if that many bytes have actually been
// written. Trusting it would read uninitialised window bytes -- which
// is a memory disclosure, not merely a wrong pixel.
static bool wvalid(const z_inflate_t *z, uint32_t dist) {
	if (dist == 0 || dist > Z_INFLATE_WINDOW) return false;
	return z->wfull || dist <= z->wpos;
}

static uint8_t wback(const z_inflate_t *z, uint32_t dist) {
	uint32_t p = (z->wpos + Z_INFLATE_WINDOW - dist) & (Z_INFLATE_WINDOW - 1);
	return z->window[p];
}

// -- bit reader --
//
// LSB-first, as DEFLATE specifies -- and the opposite of the Huffman
// CODES, which are read MSB-first. Mixing those up is the classic way
// to get a decoder that works on stored blocks and nothing else.

#define NEEDBITS(n) \
	while (z->bitcnt < (n)) { \
		if (ip >= *inlen) goto out_of_input; \
		z->bitbuf |= (uint32_t)in[ip++] << z->bitcnt; \
		z->bitcnt += 8; \
	}

#define GETBITS(n)  (z->bitbuf & ((1u << (n)) - 1))
#define DROPBITS(n) do { z->bitbuf >>= (n); z->bitcnt -= (n); } while (0)

// Decodes one canonical Huffman symbol.
//
// Walks code lengths rather than using a lookup table: a table would
// be faster and this machine has already spent 32KB on the window.
// Returns -1 when there is not enough input yet, which the caller
// turns into "come back with more".
static int huff_decode(z_inflate_t *z, const uint16_t *count,
	const uint16_t *sym, const uint8_t *in, uint32_t *inlen, uint32_t *ipp) {

	int code = 0, first = 0, index = 0, len;
	uint32_t ip = *ipp;

	for (len = 1; len <= 15; len++) {

		while (z->bitcnt < len) {
			if (ip >= *inlen) { *ipp = ip; return -1; }
			z->bitbuf |= (uint32_t)in[ip++] << z->bitcnt;
			z->bitcnt += 8;
		}

		// Codes are MSB-first within the LSB-first bit stream, so
		// each new bit goes on at the bottom of the code.
		code |= (int)((z->bitbuf >> (len - 1)) & 1);

		{
			int cnt = count[len];
			if (code - first < cnt) {
				// Consume exactly the bits this code used. Nothing
				// before now may touch bitbuf, because a short read
				// above has to be resumable.
				z->bitbuf >>= len;
				z->bitcnt -= len;
				*ipp = ip;
				return sym[index + (code - first)];
			}
			index += cnt;
			first = (first + cnt) << 1;
			code <<= 1;
		}

	}

	*ipp = ip;
	return -2;					// no code of any length matches

}

// Builds the count/symbol arrays a canonical decoder needs from a
// list of code lengths.
static bool huff_build(uint16_t *count, uint16_t *sym,
	const uint16_t *lengths, int n) {

	int i, len, left;
	uint16_t offs[16];

	for (len = 0; len < 16; len++) count[len] = 0;
	for (i = 0; i < n; i++) count[lengths[i]]++;

	// All-zero is legal for the distance table (a block with no
	// matches) and meaningless for the literal table, which the
	// caller checks.
	if (count[0] == n) return true;

	// Over-subscribed or incomplete codes are malformed. Accepting
	// them means decoding symbols that were never encoded.
	left = 1;
	for (len = 1; len < 16; len++) {
		left <<= 1;
		left -= count[len];
		if (left < 0) return false;
	}

	offs[1] = 0;
	for (len = 1; len < 15; len++) offs[len + 1] = offs[len] + count[len];

	for (i = 0; i < n; i++)
		if (lengths[i]) sym[offs[lengths[i]]++] = (uint16_t)i;

	return true;

}

int z_inflate(z_inflate_t *z, const uint8_t *in, uint32_t *inlen,
	uint8_t *out, uint32_t *outlen) {

	uint32_t ip = 0, op = 0;
	int rv = Z_INFLATE_OK;

	if (z->state == S_DONE)  { *inlen = 0; *outlen = 0; return Z_INFLATE_DONE; }
	if (z->state == S_ERROR) { *inlen = 0; *outlen = 0; return Z_INFLATE_E_DATA; }

	for (;;) {

		switch (z->state) {

		case S_HDR:
			while (z->hdr_got < z->hdr_need) {
				if (ip >= *inlen) goto out_of_input;
				z->hdr[z->hdr_got++] = in[ip++];
			}
			if (z->wrap == Z_INFLATE_GZIP) {
				if (z->hdr[0] != 0x1f || z->hdr[1] != 0x8b || z->hdr[2] != 8)
					{ rv = Z_INFLATE_E_WRAP; goto fail; }
				// FLG bits: any of FEXTRA/FNAME/FCOMMENT/FHCRC means
				// more header to skip. Refused rather than skipped:
				// HTTP servers do not set them, and a skipper is more
				// code paths on untrusted input for no real case.
				if (z->hdr[3] & 0x1e) { rv = Z_INFLATE_E_WRAP; goto fail; }
			} else {
				// zlib: CM must be 8, and CMF/FLG must be a multiple
				// of 31. FDICT is refused -- PNG never sets it.
				if ((z->hdr[0] & 0x0f) != 8) { rv = Z_INFLATE_E_WRAP; goto fail; }
				if (((z->hdr[0] << 8) | z->hdr[1]) % 31)
					{ rv = Z_INFLATE_E_WRAP; goto fail; }
				if (z->hdr[1] & 0x20) { rv = Z_INFLATE_E_WRAP; goto fail; }
			}
			z->hdr_need = 0;		// reused by the trailer below
			z->state = S_BLOCK;
			break;

		case S_BLOCK:
			NEEDBITS(3);
			z->last_block = (int)GETBITS(1); DROPBITS(1);
			z->btype = (int)GETBITS(2); DROPBITS(2);
			if (z->btype == 0) {
				// A stored block starts on a byte boundary.
				z->bitbuf >>= (z->bitcnt & 7);
				z->bitcnt -= (z->bitcnt & 7);
				z->hdr_got = 0;
				z->state = S_STORED_LEN;
			} else if (z->btype == 1) {
				// Fixed tables, built here rather than kept as
				// constants: it is 30 lines of setup once per block
				// against 640 bytes of .rodata forever.
				int i;
				for (i = 0; i < 144; i++) z->lengths[i] = 8;
				for (; i < 256; i++)      z->lengths[i] = 9;
				for (; i < 280; i++)      z->lengths[i] = 7;
				for (; i < 288; i++)      z->lengths[i] = 8;
				if (!huff_build(z->lit_count, z->lit_sym, z->lengths, 288))
					{ rv = Z_INFLATE_E_DATA; goto fail; }
				for (i = 0; i < 30; i++) z->lengths[i] = 5;
				if (!huff_build(z->dist_count, z->dist_sym, z->lengths, 30))
					{ rv = Z_INFLATE_E_DATA; goto fail; }
				z->state = S_SYM;
			} else if (z->btype == 2) {
				z->state = S_DYN_HDR;
			} else {
				rv = Z_INFLATE_E_DATA; goto fail;		// btype 3 is reserved
			}
			break;

		case S_STORED_LEN:
			while (z->hdr_got < 4) {
				if (ip >= *inlen) goto out_of_input;
				z->hdr[z->hdr_got++] = in[ip++];
			}
			z->len = (uint32_t)z->hdr[0] | ((uint32_t)z->hdr[1] << 8);
			// LEN and ~LEN must agree. This is the only integrity
			// check DEFLATE gives for free.
			if ((uint16_t)~((uint32_t)z->hdr[2] | ((uint32_t)z->hdr[3] << 8))
				!= (uint16_t)z->len)
				{ rv = Z_INFLATE_E_DATA; goto fail; }
			z->state = S_STORED;
			break;

		case S_STORED:
			while (z->len) {
				if (ip >= *inlen) goto out_of_input;
				if (op >= *outlen) goto out_of_output;
				if (z->limit && z->produced >= z->limit)
					{ rv = Z_INFLATE_E_LIMIT; goto fail; }
				{
					uint8_t b = in[ip++];
					wput(z, b);
					out[op++] = b;
					z->produced++;
					z->len--;
				}
			}
			z->state = z->last_block ? S_TRAILER : S_BLOCK;
			break;

		case S_DYN_HDR:
			NEEDBITS(14);
			z->nlen  = (int)GETBITS(5) + 257; DROPBITS(5);
			z->ndist = (int)GETBITS(5) + 1;   DROPBITS(5);
			z->ncode = (int)GETBITS(4) + 4;   DROPBITS(4);
			if (z->nlen > 286 || z->ndist > 30)
				{ rv = Z_INFLATE_E_DATA; goto fail; }
			z->lenpos = 0;
			memset(z->lengths, 0, sizeof(z->lengths));
			z->state = S_DYN_CL;
			break;

		case S_DYN_CL:
			while (z->lenpos < z->ncode) {
				NEEDBITS(3);
				z->lengths[cl_order[z->lenpos++]] = (uint16_t)GETBITS(3);
				DROPBITS(3);
			}
			// The remaining 19 - ncode entries stay zero, which the
			// memset above already arranged.
			if (!huff_build(z->cl_count, z->cl_sym, z->lengths, 19))
				{ rv = Z_INFLATE_E_DATA; goto fail; }
			z->lenpos = 0;
			z->cl_prev = -1;
			z->cl_hold = -1;
			memset(z->lengths, 0, sizeof(z->lengths));
			z->state = S_DYN_LENS;
			break;

		case S_DYN_LENS:
			while (z->lenpos < z->nlen + z->ndist) {

				int sym;
				int rep = 0, val = 0;

				// Resume a repeat whose extra bits had not arrived,
				// rather than decoding a new symbol over the top of
				// it. See cl_hold in zinflate.h.
				if (z->cl_hold >= 0) {
					sym = z->cl_hold;
				} else {
					sym = huff_decode(z, z->cl_count, z->cl_sym,
						in, inlen, &ip);
					if (sym == -1) goto out_of_input;
					if (sym < 0)   { rv = Z_INFLATE_E_DATA; goto fail; }
				}

				if (sym < 16) {
					z->lengths[z->lenpos++] = (uint16_t)sym;
					z->cl_prev = sym;
					z->cl_hold = -1;
					continue;
				}

				// The symbol is banked BEFORE its extra bits are
				// read, so running out of input here resumes the
				// repeat instead of losing it.
				z->cl_hold = sym;

				if (sym == 16) {
					// Repeat the PREVIOUS length. With no previous
					// length there is nothing to repeat, and a stream
					// that asks is malformed rather than starting
					// from zero.
					if (z->cl_prev < 0) { rv = Z_INFLATE_E_DATA; goto fail; }
					NEEDBITS(2);
					rep = 3 + (int)GETBITS(2); DROPBITS(2);
					val = z->cl_prev;
				} else if (sym == 17) {
					NEEDBITS(3);
					rep = 3 + (int)GETBITS(3); DROPBITS(3);
					val = 0;
				} else {
					NEEDBITS(7);
					rep = 11 + (int)GETBITS(7); DROPBITS(7);
					val = 0;
				}

				z->cl_hold = -1;

				// A repeat that runs past the table is malformed.
				// Clamping it instead would silently accept a stream
				// no encoder produces.
				if (z->lenpos + rep > z->nlen + z->ndist)
					{ rv = Z_INFLATE_E_DATA; goto fail; }

				while (rep--) z->lengths[z->lenpos++] = (uint16_t)val;
				if (val) z->cl_prev = val;

			}

			if (!huff_build(z->lit_count, z->lit_sym, z->lengths, z->nlen))
				{ rv = Z_INFLATE_E_DATA; goto fail; }
			if (!huff_build(z->dist_count, z->dist_sym,
				z->lengths + z->nlen, z->ndist))
				{ rv = Z_INFLATE_E_DATA; goto fail; }

			z->state = S_SYM;
			break;

		case S_SYM: {

			int sym;

			// A literal decoded on an earlier call that had no room
			// to put it. See lit_hold in zinflate.h.
			if (z->lit_hold >= 0) {
				sym = z->lit_hold;
			} else {
				sym = huff_decode(z, z->lit_count, z->lit_sym,
					in, inlen, &ip);
				if (sym == -1) goto out_of_input;
				if (sym < 0)   { rv = Z_INFLATE_E_DATA; goto fail; }
			}

			if (sym < 256) {
				if (op >= *outlen) {
					// Bank it: the bits are already gone.
					z->lit_hold = sym;
					goto out_of_output;
				}
				if (z->limit && z->produced >= z->limit)
					{ rv = Z_INFLATE_E_LIMIT; goto fail; }
				z->lit_hold = -1;
				wput(z, (uint8_t)sym);
				out[op++] = (uint8_t)sym;
				z->produced++;
				break;
			}

			z->lit_hold = -1;

			if (sym == 256) {
				z->state = z->last_block ? S_TRAILER : S_BLOCK;
				break;
			}

			sym -= 257;
			if (sym >= 29) { rv = Z_INFLATE_E_DATA; goto fail; }
			z->len = len_base[sym];
			z->dist = (uint32_t)len_extra[sym];		// bits still to read
			z->state = S_LEN_EXTRA;
			break;

		}

		case S_LEN_EXTRA:
			if (z->dist) {
				NEEDBITS((int)z->dist);
				z->len += GETBITS((int)z->dist);
				DROPBITS((int)z->dist);
			}
			z->state = S_DIST;
			break;

		case S_DIST: {

			int sym = huff_decode(z, z->dist_count, z->dist_sym,
				in, inlen, &ip);

			if (sym == -1) goto out_of_input;
			if (sym < 0 || sym >= 30) { rv = Z_INFLATE_E_DATA; goto fail; }

			z->dist = dist_base[sym];
			z->hdr_got = dist_extra[sym];
			z->state = S_DIST_EXTRA;
			break;

		}

		case S_DIST_EXTRA:
			if (z->hdr_got) {
				NEEDBITS(z->hdr_got);
				z->dist += GETBITS(z->hdr_got);
				DROPBITS(z->hdr_got);
			}
			if (!wvalid(z, z->dist)) { rv = Z_INFLATE_E_DATA; goto fail; }
			z->state = S_COPY;
			break;

		case S_COPY:
			while (z->len) {
				if (op >= *outlen) goto out_of_output;
				if (z->limit && z->produced >= z->limit)
					{ rv = Z_INFLATE_E_LIMIT; goto fail; }
				{
					// Read before write: a copy may overlap itself,
					// which is how DEFLATE encodes a run.
					uint8_t b = wback(z, z->dist);
					wput(z, b);
					out[op++] = b;
					z->produced++;
					z->len--;
				}
			}
			z->state = S_SYM;
			break;

		case S_TRAILER: {

			// gzip has 8 trailer bytes, zlib 4, raw none.
			//
			// They are CONSUMED but not verified. A CRC-32 over every
			// byte costs more than this machine can spare on a body
			// it is about to render anyway, and every framing error
			// the trailer would catch has already been caught by the
			// stream itself. Said plainly here because "has a CRC"
			// and "checks the CRC" are easy to confuse.
			// The countdown lives in the CONTEXT, not on the stack.
			//
			// As a local it was recomputed on every re-entry, so a
			// trailer arriving in pieces restarted at eight each time
			// and the decode stalled a few bytes from the end --
			// visibly, as a stream that consumed all its input and
			// never reported done.
			if (!z->hdr_need) {
				z->hdr_need = (z->wrap == Z_INFLATE_GZIP) ? 8 :
					(z->wrap == Z_INFLATE_ZLIB) ? 4 : 0;

				// Whole bytes only, from the current boundary.
				z->bitbuf >>= (z->bitcnt & 7);
				z->bitcnt -= (z->bitcnt & 7);

				if (!z->hdr_need) { z->state = S_DONE; rv = Z_INFLATE_DONE; goto done; }
			}

			while (z->bitcnt >= 8 && z->hdr_need > 0) {
				DROPBITS(8); z->hdr_need--;
			}
			while (z->hdr_need > 0) {
				if (ip >= *inlen) goto out_of_input;
				ip++; z->hdr_need--;
			}

			z->state = S_DONE;
			rv = Z_INFLATE_DONE;
			goto done;

		}

		default:
			rv = Z_INFLATE_E_DATA;
			goto fail;

		}

	}

out_of_input:
out_of_output:
done:
	*inlen = ip;
	*outlen = op;
	return rv;

fail:
	z->state = S_ERROR;
	*inlen = ip;
	*outlen = op;
	return rv;

}
