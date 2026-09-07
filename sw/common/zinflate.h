#ifndef ZINFLATE_H
#define ZINFLATE_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * DEFLATE (RFC 1951), with zlib (RFC 1950) and gzip (RFC 1952)
 * wrappers.
 *
 * -- two callers, one decoder --
 *
 * HTTP `Content-Encoding: gzip` and PNG's IDAT stream are the same
 * algorithm behind different headers. Wikipedia's front page is 258KB
 * uncompressed and about a fifth of that gzipped, and on this machine
 * the body transfer is the largest single cost of a page load -- so
 * gzip is worth more than images are, and it is the same code.
 *
 * -- streaming, because nothing here can hold a file --
 *
 * There is no dynamic memory (docs/app_runtime.md) and the kernel pool
 * is shared by every process. The caller pushes compressed bytes in
 * and takes decompressed bytes out; neither side is ever fully
 * resident. That matches how sw/common/zimg.c already works and how
 * sw/apps/web already spools a response.
 *
 * -- THE MEMORY --
 *
 * 32KB, and it is not optional: DEFLATE back-references reach up to
 * 32768 bytes, so a decoder must keep that much history. It is the
 * single largest buffer in this tree after the framebuffer.
 *
 * The window lives in the CALLER's z_inflate_t rather than here, so
 * an app that never decompresses anything does not pay for it, and so
 * zimg.c can put it in the union it already shares between decoders
 * (GIF's LZW dictionary is 17.6KB of the same bytes).
 *
 * -- untrusted input --
 *
 * This parses attacker-controlled data with nothing authenticating
 * it, which makes it the highest-risk code in the tree after TLS.
 * Every length is bounds-checked against the window rather than
 * trusted, malformed streams end the decode rather than being
 * skipped, and the caller is expected to cap total output -- a few
 * hundred bytes of input can legitimately expand to megabytes, and
 * "legitimately" is exactly how a decompression bomb looks.
 */

#define Z_INFLATE_WINDOW 32768

typedef enum {
	Z_INFLATE_RAW = 0,		// bare DEFLATE (RFC 1951) -- PNG uses zlib
	Z_INFLATE_ZLIB,			// 2-byte header, Adler-32 trailer
	Z_INFLATE_GZIP,			// 10+ byte header, CRC-32 + size trailer
} z_inflate_wrap_t;

#define Z_INFLATE_OK        0	// output produced; more input welcome
#define Z_INFLATE_DONE      1	// stream ended cleanly
#define Z_INFLATE_E_DATA   -1	// malformed
#define Z_INFLATE_E_WRAP   -2	// header/trailer wrong for the wrapper
#define Z_INFLATE_E_LIMIT  -3	// output cap reached -- see z_inflate_init

typedef struct {

	// -- caller-owned window --
	//
	// Not embedded, so this struct can sit in an app's static state
	// while the 32KB lives wherever the app can afford it.
	uint8_t		*window;
	uint32_t	wpos;			// next write position, wraps at 32768
	bool		wfull;			// the window has wrapped at least once

	z_inflate_wrap_t wrap;

	// Stops runaway expansion. 0 means no limit, which is only
	// appropriate when the caller bounds the output some other way.
	uint32_t	limit;
	uint32_t	produced;

	// -- bit reader --
	uint32_t	bitbuf;
	int			bitcnt;

	// -- state --
	int			state;
	int			last_block;
	int			btype;

	uint32_t	len;			// stored-block bytes left, or copy length
	uint32_t	dist;

	// Header/trailer bytes still to consume.
	int			hdr_need;
	int			hdr_got;
	uint8_t		hdr[16];

	// -- dynamic Huffman tables --
	//
	// Canonical codes, decoded by walking code lengths. Slower than a
	// lookup table and far smaller; this is a 1bpp machine with 32KB
	// already committed to the window.
	uint16_t	lit_count[16];
	uint16_t	lit_sym[288];
	uint16_t	dist_count[16];
	uint16_t	dist_sym[32];

	// Code-length code, built first and used to read the other two.
	uint16_t	cl_count[8];
	uint16_t	cl_sym[19];

	uint16_t	lengths[288 + 32];
	int			nlen, ndist, ncode;
	int			lenpos;
	int			cl_prev;

	// A repeat code (16/17/18) whose extra bits have not arrived yet.
	//
	// Needed because the symbol and its extra bits are read in one
	// step, and input can run out between them: without this, the
	// resume decoded a FRESH symbol and silently dropped the pending
	// repeat. -1 when there is nothing pending.
	int			cl_hold;

	// A decoded literal with nowhere to go yet.
	//
	// Same hazard as cl_hold: the symbol is consumed from the bit
	// stream before the output has room, so suspending in between
	// dropped the byte and resumed by decoding a different one. -1
	// when there is nothing pending.
	int			lit_hold;

} z_inflate_t;

// `window` must be Z_INFLATE_WINDOW bytes and must outlive the
// decode. `limit` caps total output; 0 disables the cap.
void z_inflate_init(z_inflate_t *z, uint8_t *window,
	z_inflate_wrap_t wrap, uint32_t limit);

// Consumes up to `*inlen` bytes and produces up to `*outlen`.
//
// On return both are set to the amounts ACTUALLY used, so a caller
// that could not take everything simply calls again with the rest --
// which is what makes this usable from a network callback that has no
// say in how much arrives at once.
int z_inflate(z_inflate_t *z, const uint8_t *in, uint32_t *inlen,
	uint8_t *out, uint32_t *outlen);

const char *z_inflate_strerror(int rv);

#endif
