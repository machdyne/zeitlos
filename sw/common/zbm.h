#ifndef ZBM_H
#define ZBM_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * ZBM -- the Zeitlos bitmap file, written and read by sw/apps/draw.
 *
 * A 16-byte header followed by raw 1bpp pixel data:
 *
 *   offset  size  field
 *   0       4     magic "ZBM1"
 *   4       4     width in pixels
 *   8       4     height in pixels
 *   12      4     flags (0)
 *   16      ...   height * ((width + 31) / 32) * 4 bytes of pixels
 *
 * All integers little-endian, matching the CPU.
 *
 * -- pixel format --
 *
 * Exactly the framebuffer's own: pixel x lives at bit (x & 31) of word
 * (x >> 5), LEAST significant bit LEFTMOST. That is z_fb_set_pixel()'s
 * convention (zgfx.c) and the one z_fb_hw_blit_mem() reads, which is
 * the point -- a loaded image can go straight to the screen in one
 * blitter operation with no reformatting anywhere.
 *
 * Note this is the opposite bit order from font and icon data
 * (zfont.h, zicon.h), which is MSB-first. The two conventions exist
 * because glyphs are written by hand as binary literals and want to
 * read as pictures, while framebuffer words are addressed
 * arithmetically. Getting them confused produces a horizontally
 * mirrored image in 32-pixel blocks, which is a distinctive enough
 * symptom to be worth naming.
 *
 * -- why there is a header at all --
 *
 * draw's first version wrote the canvas array verbatim with no header,
 * on the grounds that it was already exactly what the blitter reads.
 * The cost was that a file carried no record of its own shape: change
 * CANVAS_W or CANVAS_H and every existing file becomes unreadable
 * garbage, with nothing but the byte count to notice from. Sixteen
 * bytes buys a positive identification and real dimensions, and lets
 * a file browser tell a ZBM from anything else before launching
 * anything.
 *
 * Files written before this header existed are still loadable -- see
 * z_bm_is_legacy_size() and draw's own loader.
 */

#include <stdint.h>
#include <stdbool.h>

#define Z_BM_MAGIC0 'Z'
#define Z_BM_MAGIC1 'B'
#define Z_BM_MAGIC2 'M'
#define Z_BM_MAGIC3 '1'

#define Z_BM_HEADER_SIZE 16

typedef struct {
	uint8_t		magic[4];
	uint32_t	width;
	uint32_t	height;
	uint32_t	flags;		// 0 -- reserved, must be written as 0
} z_bm_header_t;

// bytes of pixel data a width x height image occupies
static inline uint32_t z_bm_data_size(uint32_t w, uint32_t h) {
	return h * (((w + 31) / 32) * 4);
}

// total file size for a width x height image
static inline uint32_t z_bm_file_size(uint32_t w, uint32_t h) {
	return Z_BM_HEADER_SIZE + z_bm_data_size(w, h);
}

static inline void z_bm_header_init(z_bm_header_t *hdr,
	uint32_t w, uint32_t h) {

	hdr->magic[0] = Z_BM_MAGIC0;
	hdr->magic[1] = Z_BM_MAGIC1;
	hdr->magic[2] = Z_BM_MAGIC2;
	hdr->magic[3] = Z_BM_MAGIC3;
	hdr->width = w;
	hdr->height = h;
	hdr->flags = 0;

}

static inline bool z_bm_header_valid(const z_bm_header_t *hdr) {

	return hdr->magic[0] == Z_BM_MAGIC0 && hdr->magic[1] == Z_BM_MAGIC1 &&
		hdr->magic[2] == Z_BM_MAGIC2 && hdr->magic[3] == Z_BM_MAGIC3;

}

// True if `size` is exactly the headerless payload for w x h -- i.e.
// this looks like a file written before the header existed.
//
// A size test is all that is available for those, and it is a weak
// one: any file that happens to be that length passes. It is only
// applied after the magic check has already failed, and only for the
// one size draw ever wrote, so the exposure is narrow.
static inline bool z_bm_is_legacy_size(uint32_t size,
	uint32_t w, uint32_t h) {

	return size == z_bm_data_size(w, h);

}

#endif
