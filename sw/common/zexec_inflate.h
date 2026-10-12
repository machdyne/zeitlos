#ifndef ZEXEC_INFLATE_H
#define ZEXEC_INFLATE_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Inflate for ZEXE loaders: a compressed executable (Z_EXEC_FLAG_COMPRESSED,
 * zexec.h) decoded straight into the process image. docs/executables.md,
 * "Compressed executables".
 *
 * -- why not zinflate.c --
 *
 * zinflate.c streams: its output leaves through a 32KB ring that holds
 * DEFLATE's history, and it can stop and resume anywhere, for a network
 * callback. A loader has neither need. The output has a known size and
 * one contiguous home -- the new process's memory -- so the bytes
 * already written ARE the history: no window, no allocation, and a
 * decoder of about 1.5KB, which is what the kernel can afford. It runs
 * from start to finish in one call; only the input comes in pieces,
 * through `fill`, so the same code reads flash a word at a time (zar.c),
 * a file through fs (kfs.c), or a buffer (the simulator).
 *
 * -- untrusted input --
 *
 * A compressed binary can come off a card, so every length and distance
 * is checked: nothing is written past out_len, nothing is read before
 * out[0], and nothing is read past what `fill` hands over. A malformed
 * stream fails; it never runs past its buffers. The output is accepted
 * only if the stream ends exactly at out_len.
 */

#include <stdint.h>

#define Z_EXEC_INFLATE_BUF	256

// Up to `cap` more compressed bytes into `buf`; 0 at the end of them.
typedef uint32_t (*z_exec_fill_t)(void *ctx, uint8_t *buf, uint32_t cap);

// Codes up to this many bits decode with one table lookup; longer ones
// (rare: a literal/length code past 9 bits) walk the canonical code.
#define Z_EXEC_INFLATE_FAST	9

// The decoder's state: about 3.3KB, the caller's (the kernel keeps one,
// a host tool can keep it on its stack).
typedef struct {
	uint8_t			buf[Z_EXEC_INFLATE_BUF];
	const uint8_t	*p, *end;
	uint32_t		bitbuf;
	int				bitcnt;
	int				err;
	uint32_t		over;			// zero bytes made up past the input's end
	z_exec_fill_t	fill;
	void			*ctx;
	uint16_t		lcnt[16], lsym[288];
	uint16_t		dcnt[16], dsym[30];
	// symbol | length << 12, by the next FAST bits; 0: a longer code
	uint16_t		lfast[1 << Z_EXEC_INFLATE_FAST];
	uint16_t		dfast[1 << Z_EXEC_INFLATE_FAST];
	uint8_t			len[288 + 32];
} z_exec_inflate_t;

// Raw DEFLATE (RFC 1951) from `fill` into out[0..out_len). 0 when the
// stream ended having produced exactly out_len bytes; -1 otherwise.
int z_exec_inflate(z_exec_inflate_t *s, uint8_t *out, uint32_t out_len,
	z_exec_fill_t fill, void *ctx);

#endif
