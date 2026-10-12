#ifndef ZEXEC_LZ4_H
#define ZEXEC_LZ4_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The decoder for compressed executables (Z_EXEC_FLAG_COMPRESSED,
 * zexec.h): an LZ4 block (lz4_Block_format.md), unpacked straight into
 * the process image. docs/executables.md, "Compressed executables".
 *
 * -- why LZ4, not DEFLATE --
 *
 * DEFLATE came first and packs smaller (57% of the image to LZ4's 68%),
 * but on a PicoRV32 at 48MHz -- about 7 million instructions a second,
 * memory slower still -- it took 1,251ms to load wm, from 41
 * instructions a byte (Huffman decoding, table lookups). LZ4 has no
 * entropy coding: byte-aligned literal runs and back-references, about
 * 8 instructions a byte, and no tables at all.
 *
 * -- the shape --
 *
 * The output has a known size and one home, the new process's memory,
 * so the bytes already written are the history: no window. The input
 * comes through `fill`, so the same code reads flash a word at a time
 * (zar.c), a file through fs (kfs.c), or memory (the simulator). A long
 * literal run is filled straight into the image, not through the
 * buffer.
 *
 * -- untrusted input --
 *
 * A compressed binary can come off a card, so every length and offset
 * is checked: nothing is written past out_len, nothing read before
 * out[0], nothing read past what `fill` hands over. The image is
 * accepted only if it comes out exactly out_len bytes long, with the
 * input ending there too.
 */

#include <stdint.h>

#define Z_EXEC_LZ4_BUF	256

// Up to `cap` more compressed bytes into `buf`; 0 at the end of them.
typedef uint32_t (*z_exec_fill_t)(void *ctx, uint8_t *buf, uint32_t cap);

// The decoder's input state, the caller's (about 280 bytes).
typedef struct {
	uint8_t			buf[Z_EXEC_LZ4_BUF];
	const uint8_t	*p, *end;
	z_exec_fill_t	fill;
	void			*ctx;
} z_exec_lz4_t;

// The LZ4 block from `fill` into out[0..out_len). 0 when it ended,
// with the input, at exactly out_len bytes; -1 otherwise.
int z_exec_lz4(z_exec_lz4_t *s, uint8_t *out, uint32_t out_len,
	z_exec_fill_t fill, void *ctx);

#endif
