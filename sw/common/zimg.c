/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * ZIMG -- streaming image decoders. See zimg.h for the design and the
 * memory argument; this file is the implementation.
 *
 * Every decoder here has the same shape:
 *
 *   read a source row  ->  convert to 8-bit gray  ->  decimate
 *      ->  dither into the 1bpp document  ->  forget it
 *
 * and holds nothing between rows except what its compression scheme
 * genuinely requires (GIF's dictionary, JPEG's MCU row). That is what
 * makes an image viewer possible at all without dynamic memory.
 *
 * No printf anywhere in this file, deliberately. Pulling stdio into a
 * module that every app might link costs ~100KB on the newlib
 * toolchain this tree targets (docs/app_runtime.md) -- far more than
 * all the decoders combined. Errors come back as Z_IMG_E_* codes and
 * the app decides how to say so.
 */

#include <string.h>

#include "zimg.h"
#include "zprof.h"
#include "zfsapp.h"

// -- per-format scratch --
//
// A union, not a struct. Only one decoder runs at a time, so GIF's
// 16KB LZW dictionary and JPEG's 10KB MCU row occupy the same bytes;
// sizing by the largest member rather than the sum saves ~13KB of
// .bss today. If PNG is ever enabled its 32KB inflate window joins
// the union rather than adding to it, which is the whole point of
// arranging it this way now.
//
// Tagged into .bss explicitly. See docs/app_runtime.md on why large
// globals in this tree carry that attribute -- a zero-initialised
// array is .bss anyway by the letter of the standard, but being
// explicit here keeps it out of reach of the __global_pointer$
// relaxation questions that section placement can otherwise raise.

#define Z_IMG_MCUROW_MAX (Z_IMG_MAX_W * 16)

// Entropy-stream staging. The scan used to pull one byte at a time
// through im->read -- an indirect call into z_img_file_read plus a
// memcpy() of a single byte, measured at ~159 instructions PER BYTE on
// hardware. Reading a block at a time and handing out bytes from it
// keeps the byte-source abstraction while removing that entirely.
//
// Only the SCAN uses this. Header parsing still reads through im->read
// directly, which is safe because the two never interleave: the header
// loop exits at SOS and never runs again.
#define Z_JPG_INBUF 512

// Codes of 8 bits or fewer resolve in ONE indexed load through
// look_nbits/look_sym instead of the bit-at-a-time walk down
// mincode/maxcode. Baseline JPEG's tables are built so that the
// common symbols get the short codes, so this hits the great majority
// of the time; the walk stays for the 9..16-bit tail.
//
// 512 bytes per table, 2KB across the four. That is free: the scratch
// union (z_img_scratch_t) is sized by GIF's 17.6KB dictionary, and
// JPEG had headroom under it.
#define Z_JPG_LOOKBITS 8
#define Z_JPG_LOOKN    (1 << Z_JPG_LOOKBITS)

typedef struct {
	uint8_t		bits[17];		// bits[l] = number of codes of length l
	uint8_t		vals[256];
	int32_t		mincode[17];
	int32_t		maxcode[18];
	int32_t		valptr[17];
	uint8_t		look_nbits[Z_JPG_LOOKN];	// 0 = not resolvable in 8 bits
	uint8_t		look_sym[Z_JPG_LOOKN];
} z_jpg_huff_t;

typedef struct {
	uint8_t		id, hs, vs, tq, td, ta;
	int32_t		dcpred;
} z_jpg_comp_t;

typedef union {

	struct {
		uint8_t		pal[256 * 4];
	} bmp;

	struct {
		uint16_t	prefix[4096];
		uint8_t		suffix[4096];
		uint8_t		stack[4096];
		uint8_t		pal[256 * 3];
		uint8_t		pal_y[256];
		uint8_t		blk[256];
	} gif;

	struct {
		uint8_t			mcurow[Z_IMG_MCUROW_MAX];
		z_jpg_huff_t	hdc[2], hac[2];
		z_jpg_comp_t	comp[3];
		uint8_t			inbuf[Z_JPG_INBUF];
		uint16_t		qtab[4][64];
		int32_t			ws[64];
		int16_t			coef[64];
	} jpg;

} z_img_scratch_t;

static z_img_scratch_t S __attribute__((section(".bss")));

// One decoded row, 8-bit gray, already decimated to out_w. Shared by
// every decoder -- they never run concurrently.
static uint8_t grayrow[Z_IMG_MAX_W] __attribute__((section(".bss")));

// Floyd-Steinberg carries error into the next row, so two rows of it.
// Array index i holds the error destined for pixel i-1, which is what
// lets dither_row_fs() write each element exactly once and skip the
// per-row memset the previous version needed. int16_t rather than int: 2.5KB instead of 5KB at
// this width, and the accumulated value is bounded well inside the
// range (a pixel contributes at most +/-255, and the clamp below
// stops it compounding).
static int16_t err_a[Z_IMG_MAX_W + 2] __attribute__((section(".bss")));
static int16_t err_b[Z_IMG_MAX_W + 2] __attribute__((section(".bss")));
static int16_t *err_cur;
static int16_t *err_nxt;

// Which quantiser emit_row() uses. Set by the decoder after
// dither_reset(), which always restores DM_FS.
#define DM_FS         0		// Floyd-Steinberg error diffusion
#define DM_ORDERED    1		// Bayer 8x8, for out-of-order rows
#define DM_THRESHOLD  2		// straight compare, for bilevel sources
static int dither_mode;

// -- small shared helpers --

// ITU-R BT.601 luma, integer. (77R + 150G + 29B) >> 8; the weights sum
// to 256 so there is no rounding drift across a large flat area.
//
// Not the "correct" perceptual formula for a monochrome CRT, and
// deliberately so: this is one multiply-add per channel with no
// division, and on a 48MHz core running a per-pixel loop that
// difference is worth more than the accuracy would be after the image
// has been reduced to two levels anyway.
static inline uint8_t luma(int r, int g, int b) {
	return (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
}

static inline uint32_t rd16le(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static inline uint32_t rd32le(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint32_t rd32be(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Reads and discards n bytes. Used to step over chunks and segments
// we don't care about, which every one of these formats has.
static int skip_bytes(z_img_t *im, uint32_t n) {
	uint8_t junk[64];
	while (n > 0) {
		uint32_t k = n > sizeof(junk) ? (uint32_t)sizeof(junk) : n;
		if (im->read(im->ctx, junk, (int)k) != (int)k) return Z_IMG_E_IO;
		n -= k;
	}
	return Z_IMG_OK;
}

// Chooses the power-of-two downscale that brings the source inside
// the document, and fills in out_w/out_h.
//
// Power-of-two only, and by point sampling rather than averaging.
// Averaging would be better looking, but it needs every source row in
// a group resident at once to combine them, and at 1/8 scale that is
// eight rows of a possibly-5120-pixel-wide image. Point sampling
// needs one. Given the result is then reduced to two levels by the
// ditherer, which destroys far more detail than the sampling choice
// does, this is the right place to spend nothing.
static int fit_scale(z_img_t *im, int w, int h) {

	int sh = 0;

	if (w <= 0 || h <= 0) return Z_IMG_E_FORMAT;

	while ((w >> sh) > im->doc_w || (h >> sh) > im->doc_h) {
		sh++;
		if (sh > 3) return Z_IMG_E_TOOBIG;
	}

	im->src_w = w;
	im->src_h = h;
	im->shift = sh;
	im->out_w = w >> sh;
	im->out_h = h >> sh;

	if (im->out_w <= 0 || im->out_h <= 0) return Z_IMG_E_FORMAT;

	return Z_IMG_OK;

}

// -- dithering --

static void dither_reset(void) {
	memset(err_a, 0, sizeof(err_a));
	memset(err_b, 0, sizeof(err_b));
	err_cur = err_a;
	err_nxt = err_b;
	dither_mode = DM_FS;
}

/*
 * Floyd-Steinberg, one row.
 *
 * Bits are accumulated into a 32-bit word and written once per 32
 * pixels rather than read-modify-written individually. That is not a
 * micro-optimisation: the per-pixel form is three memory operations
 * per pixel against main memory (there is no data cache on this
 * core), and at 640x480 that is the difference between a visible
 * pause and an imperceptible one.
 *
 * Bit order is the framebuffer's -- pixel x at bit (x & 31), LSB
 * leftmost -- so `acc |= 1 << (x & 31)`. See zimg.h.
 *
 * A row narrower than the document leaves the remaining words of that
 * row untouched, so the caller must have cleared the document first
 * (z_img_clear). The alternative, writing background out to the full
 * width on every row, costs more than clearing once.
 */
/*
 * Pinned to -Os even when the rest of this file is built -O2.
 *
 * Measured on hardware, not assumed: building zimg.c at -O2 improved
 * the JPEG entropy phase by 27% and made THIS function 13% worse
 * (7.62M -> 8.63M retired instructions for the same 99,072 pixels,
 * at unchanged IPC -- so genuinely more work, not more stalling).
 *
 * The loop is already tight and carries its state in registers; -O2's
 * unrolling buys nothing here and costs instruction-cache footprint,
 * which on this SOC is the term that dominates CPI (rtl/cache.v).
 *
 * The attribute is per-function so the two can differ without
 * splitting the file. At -Os it is a no-op.
 */
__attribute__((optimize("Os")))
static void dither_row_fs(z_img_t *im, const uint8_t *gray, int y) {

	uint32_t *dst = im->doc + (uint32_t)y * (uint32_t)im->doc_wpl;
	int16_t *cur = err_cur;
	int16_t *nxt = err_nxt;
	int w = im->out_w;
	uint32_t acc = 0;
	int nbits = 0;
	int wi = 0;
	int x;

	// The next row's three accumulators, held in registers.
	//   na -> pixel x-1   nb -> pixel x   nc -> pixel x+1
	// Pixel x-1 can receive nothing further once x is processed
	// (pixel x+1 diffuses into x, x+1, x+2), so na is final and gets
	// stored, then the window slides.
	int na = 0, nb = 0, nc = 0;

	// The 7/16 travelling right along THIS row, also a register: it is
	// produced at x and consumed at x+1, so it never needs to reach
	// memory at all.
	int e7 = 0;

	for (x = 0; x < w; x++) {

		int v = (int)gray[x] + (int)cur[x + 1] + e7;
		int on, e;

		if (v < 0) v = 0;
		else if (v > 255) v = 255;

		on = (v >= 128);
		e = v - (on ? 255 : 0);

		if (on) acc |= (uint32_t)1u << nbits;

		if (++nbits == 32) {
			dst[wi++] = acc;
			acc = 0;
			nbits = 0;
		}

		e7 = (e * 7) >> 4;			// right, stays in a register
		na += (e * 3) >> 4;			// below-left, now complete
		nb += (e * 5) >> 4;			// below
		nc  = e >> 4;				// below-right, fresh

		nxt[x] = (int16_t)na;		// the row's ONLY store to nxt[]
		na = nb;
		nb = nc;
		nc = 0;

	}

	if (nbits) dst[wi] = acc;

	// Flush the last complete accumulator (pixel w-1). nc belongs to
	// pixel w, which is off the edge, and is dropped.
	nxt[w] = (int16_t)na;

	err_cur = nxt;
	err_nxt = cur;

}

/*
 * Ordered (Bayer 8x8) dither, for sources whose rows do not arrive
 * top-to-bottom.
 *
 * Error diffusion is inherently sequential -- row y needs row y-1 to
 * have been quantised already -- so an interlaced GIF, which delivers
 * rows 0,8,16,... then 4,12,20,... cannot use it. Running
 * Floyd-Steinberg over those anyway produces horizontal streaking
 * that looks like a decoder bug. This is stateless per row and gives
 * a coarser but perfectly clean result.
 */
static const uint8_t bayer8[64] = {
	 0, 32,  8, 40,  2, 34, 10, 42,
	48, 16, 56, 24, 50, 18, 58, 26,
	12, 44,  4, 36, 14, 46,  6, 38,
	60, 28, 52, 20, 62, 30, 54, 22,
	 3, 35, 11, 43,  1, 33,  9, 41,
	51, 19, 59, 27, 49, 17, 57, 25,
	15, 47,  7, 39, 13, 45,  5, 37,
	63, 31, 55, 23, 61, 29, 53, 21
};

static void dither_row_ordered(z_img_t *im, const uint8_t *gray, int y) {

	uint32_t *dst = im->doc + (uint32_t)y * (uint32_t)im->doc_wpl;
	const uint8_t *b = bayer8 + ((y & 7) << 3);
	int w = im->out_w;
	uint32_t acc = 0;
	int nbits = 0;
	int wi = 0;
	int x;

	for (x = 0; x < w; x++) {

		// threshold spans 2..254 so pure black and pure white stay
		// solid rather than picking up a texture
		int t = ((int)b[x & 7] << 2) + 2;

		if ((int)gray[x] > t) acc |= (uint32_t)1u << nbits;

		if (++nbits == 32) {
			dst[wi++] = acc;
			acc = 0;
			nbits = 0;
		}

	}

	if (nbits) dst[wi] = acc;

}

/*
 * Straight threshold, for a source that is ALREADY bilevel.
 *
 * Dithering such an image is not merely wasted work, it is wrong: the
 * source has exactly two levels, and error diffusion renders them as
 * textures that were never in the picture. A compare reproduces it
 * exactly.
 *
 * Only used where the mapping is provably lossless -- see the palette
 * test in decode_gif().
 */
static void dither_row_threshold(z_img_t *im, const uint8_t *gray, int y) {

	uint32_t *dst = im->doc + (uint32_t)y * (uint32_t)im->doc_wpl;
	int w = im->out_w;
	uint32_t acc = 0;
	int nbits = 0;
	int wi = 0;
	int x;

	for (x = 0; x < w; x++) {

		if (gray[x] >= 128) acc |= (uint32_t)1u << nbits;

		if (++nbits == 32) {
			dst[wi++] = acc;
			acc = 0;
			nbits = 0;
		}

	}

	if (nbits) dst[wi] = acc;

}

// One decoded row into the document, whichever quantiser is in force.
static void emit_row(z_img_t *im, const uint8_t *gray, int y) {

	if (y < 0 || y >= im->out_h) return;

	Z_PROF_BEGIN(Z_IMGP_DITHER);

	if (dither_mode == DM_THRESHOLD)    dither_row_threshold(im, gray, y);
	else if (dither_mode == DM_ORDERED) dither_row_ordered(im, gray, y);
	else                                dither_row_fs(im, gray, y);

	Z_PROF_END(Z_IMGP_DITHER);

}

void z_img_clear(z_img_t *im) {
	memset(im->doc, 0,
		(size_t)im->doc_h * (size_t)im->doc_wpl * sizeof(uint32_t));
}

// -- format identification --

z_img_fmt_t z_img_sniff(const uint8_t *h, int n) {

	if (n >= 4 && h[0] == 'Z' && h[1] == 'B' && h[2] == 'M' && h[3] == '1')
		return Z_IMG_FMT_ZBM;

	if (n >= 2 && h[0] == 'B' && h[1] == 'M')
		return Z_IMG_FMT_BMP;

	if (n >= 4 && h[0] == 'G' && h[1] == 'I' && h[2] == 'F' && h[3] == '8')
		return Z_IMG_FMT_GIF;

	if (n >= 3 && h[0] == 0xFF && h[1] == 0xD8 && h[2] == 0xFF)
		return Z_IMG_FMT_JPG;

	if (n >= 8 && h[0] == 0x89 && h[1] == 'P' && h[2] == 'N' && h[3] == 'G' &&
		h[4] == 0x0D && h[5] == 0x0A && h[6] == 0x1A && h[7] == 0x0A)
		return Z_IMG_FMT_PNG;

	// PNM last: 'P' followed by a digit is a weak signature compared
	// with the magic numbers above, so anything that could be another
	// format has already been claimed by this point.
	if (n >= 2 && h[0] == 'P' && h[1] >= '1' && h[1] <= '6')
		return Z_IMG_FMT_PNM;

	return Z_IMG_FMT_NONE;

}

const char *z_img_fmt_name(z_img_fmt_t f) {
	switch (f) {
		case Z_IMG_FMT_ZBM: return "ZBM";
		case Z_IMG_FMT_BMP: return "BMP";
		case Z_IMG_FMT_PNM: return "PNM";
		case Z_IMG_FMT_GIF: return "GIF";
		case Z_IMG_FMT_JPG: return "JPEG";
		case Z_IMG_FMT_PNG: return "PNG";
		default: return "?";
	}
}

const char *z_img_strerror(int rv) {
	switch (rv) {
		case Z_IMG_OK:              return "ok";
		case Z_IMG_E_FORMAT:        return "The file is damaged or\nnot an image.";
		case Z_IMG_E_UNSUPPORTED:   return "This variant of the format\nis not supported.";
		case Z_IMG_E_IO:            return "The file could not be\nread completely.";
		case Z_IMG_E_TOOBIG:        return "The image is too large\nto display.";
		case Z_IMG_E_NOTBUILT:      return "Support for this format\nis not built in.";
		default:                    return "The image could not be\nopened.";
	}
}

// -- buffered file source --

int z_img_file_open(z_img_file_t *f, const char *path) {

	f->handle = fs_open_read(path);
	f->len = 0;
	f->pos = 0;
	f->eof = false;

	return (f->handle < 0) ? Z_IMG_E_IO : Z_IMG_OK;

}

int z_img_file_read(void *ctx, uint8_t *buf, int len) {

	z_img_file_t *f = (z_img_file_t *)ctx;
	int done = 0;

	Z_PROF_BEGIN(Z_IMGP_READ);

	while (done < len) {

		int avail = f->len - f->pos;

		if (avail <= 0) {

			if (f->eof) break;

			// Large requests bypass the buffer entirely and land in
			// the caller's memory directly -- one syscall instead of
			// a copy through a staging buffer. The row reads that
			// dominate BMP and PNM are exactly this shape.
			if (len - done >= Z_IMG_FILEBUF) {
				int got = fs_read_chunk(f->handle, buf + done, len - done);
				if (got <= 0) { f->eof = true; break; }
				done += got;
				continue;
			}

			f->len = fs_read_chunk(f->handle, f->buf, Z_IMG_FILEBUF);
			f->pos = 0;
			if (f->len <= 0) { f->len = 0; f->eof = true; break; }
			avail = f->len;

		}

		if (avail > len - done) avail = len - done;
		memcpy(buf + done, f->buf + f->pos, (size_t)avail);
		f->pos += avail;
		done += avail;

	}

	Z_PROF_END(Z_IMGP_READ);

	return done;

}

int z_img_file_rewind(z_img_file_t *f) {

	f->len = 0;
	f->pos = 0;
	f->eof = false;

	return (fs_seek(f->handle, 0) < 0) ? Z_IMG_E_IO : Z_IMG_OK;

}

void z_img_file_close(z_img_file_t *f) {

	if (f->handle >= 0) fs_close_handle(f->handle);
	f->handle = -1;

}

// -- ZBM (sw/common/zbm.h) --
//
// draw's own format. Already in the framebuffer's exact bit order and
// already 1bpp, so there is nothing to dither and nothing to convert:
// the rows go straight into the document. Included here so that view
// opens drawings as readily as photographs, which is the whole reason
// the two apps share a document format.

static int decode_zbm(z_img_t *im) {

	uint8_t hdr[16];
	uint32_t w, h;
	int src_wpl, y, rv;

	if (im->read(im->ctx, hdr, 16) != 16) return Z_IMG_E_IO;

	w = rd32le(hdr + 4);
	h = rd32le(hdr + 8);

	if (w == 0 || h == 0 || w > Z_IMG_SRC_MAX_W || h > Z_IMG_SRC_MAX_H)
		return Z_IMG_E_FORMAT;

	// No downscaling: a ZBM is already 1bpp, and point-sampling a
	// dithered bitmap produces aliasing far worse than simply
	// refusing. Anything larger than the document is cropped.
	im->src_w = (int)w;
	im->src_h = (int)h;
	im->shift = 0;
	im->out_w = (int)w > im->doc_w ? im->doc_w : (int)w;
	im->out_h = (int)h > im->doc_h ? im->doc_h : (int)h;

	src_wpl = (int)((w + 31) / 32);

	for (y = 0; y < (int)h; y++) {

		if (y >= im->out_h) break;

		uint32_t *dst = im->doc + (uint32_t)y * (uint32_t)im->doc_wpl;
		int words = src_wpl > im->doc_wpl ? im->doc_wpl : src_wpl;

		if (im->read(im->ctx, (uint8_t *)dst, words * 4) != words * 4)
			return Z_IMG_E_IO;

		// discard any source words past the document's width
		if (src_wpl > words) {
			rv = skip_bytes(im, (uint32_t)(src_wpl - words) * 4);
			if (rv != Z_IMG_OK) return rv;
		}

	}

	return Z_IMG_OK;

}

#if Z_IMG_HAVE_BMP

/*
 * BMP -- 1/4/8/24/32bpp, uncompressed.
 *
 * Rows are read in chunks rather than whole: a 32bpp source at the
 * maximum accepted width is 20KB per row, which there is no room to
 * hold. The chunk size is a multiple of the pixel group size for
 * every supported depth (480 = 2^5*3*5 divides by 1, 3 and 4) so a
 * pixel never straddles a chunk boundary and the decimation loop
 * needs no carry state.
 *
 * BMPs are usually stored bottom-up. The document row is flipped
 * rather than the read order, so error diffusion runs up the image
 * instead of down -- indistinguishable in the result, and it avoids
 * seeking backwards through the file, which the buffered reader would
 * handle badly.
 */

#define BMP_CHUNK 480

static int decode_bmp(z_img_t *im) {

	uint8_t hdr[54];
	uint8_t chunk[BMP_CHUNK];
	uint32_t off, dib, ncol, comp, pos;
	int w, h, bpp, stride, group, sy, rv, step;
	bool flip = true;

	if (im->read(im->ctx, hdr, 54) != 54) return Z_IMG_E_IO;
	if (hdr[0] != 'B' || hdr[1] != 'M') return Z_IMG_E_FORMAT;

	off  = rd32le(hdr + 10);
	dib  = rd32le(hdr + 14);
	w    = (int)rd32le(hdr + 18);
	h    = (int)(int32_t)rd32le(hdr + 22);
	bpp  = (int)rd16le(hdr + 28);
	comp = rd32le(hdr + 30);
	ncol = rd32le(hdr + 46);

	if (h < 0) { h = -h; flip = false; }

	// BI_RGB only. RLE4/RLE8 and BI_BITFIELDS are all rare enough on
	// files that actually get carried around that decoding them costs
	// more than it returns here.
	if (comp != 0) return Z_IMG_E_UNSUPPORTED;
	if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32)
		return Z_IMG_E_UNSUPPORTED;
	if (w <= 0 || h <= 0 || w > Z_IMG_SRC_MAX_W || h > Z_IMG_SRC_MAX_H)
		return Z_IMG_E_TOOBIG;

	rv = fit_scale(im, w, h);
	if (rv != Z_IMG_OK) return rv;

	pos = 14 + dib;

	if (bpp <= 8) {
		if (ncol == 0) ncol = 1u << bpp;
		if (ncol > 256) return Z_IMG_E_FORMAT;
		if (im->read(im->ctx, S.bmp.pal, (int)(ncol * 4)) != (int)(ncol * 4))
			return Z_IMG_E_IO;
		pos += ncol * 4;
	}

	if (off < pos) return Z_IMG_E_FORMAT;
	rv = skip_bytes(im, off - pos);
	if (rv != Z_IMG_OK) return rv;

	stride = ((w * bpp + 31) / 32) * 4;
	group  = (bpp >= 8) ? (bpp / 8) : 1;
	step   = 1 << im->shift;

	dither_reset();

	for (sy = 0; sy < h; sy++) {

		int left = stride;
		int px = 0;			// source pixel index reached so far
		int ox = 0;			// output pixel index

		bool want = ((sy & (step - 1)) == 0);

		while (left > 0) {

			int n = left > BMP_CHUNK ? BMP_CHUNK : left;
			int i;

			// keep chunks whole in pixel-group terms
			if (n > group) n -= (n % group);

			if (im->read(im->ctx, chunk, n) != n) return Z_IMG_E_IO;
			left -= n;

			if (!want) continue;

			Z_PROF_BEGIN(Z_IMGP_PIXEL);

			for (i = 0; i < n && px < w; ) {

				int r, g, b, take;

				switch (bpp) {

					case 1: {
						// eight pixels in this byte
						int k;
						for (k = 0; k < 8 && px < w; k++, px++) {
							if ((px & (step - 1)) == 0 && ox < im->out_w) {
								int idx = (chunk[i] >> (7 - k)) & 1;
								grayrow[ox++] = luma(S.bmp.pal[idx * 4 + 2],
									S.bmp.pal[idx * 4 + 1], S.bmp.pal[idx * 4]);
							}
						}
						i++;
						continue;
					}

					case 4: {
						int k;
						for (k = 0; k < 2 && px < w; k++, px++) {
							if ((px & (step - 1)) == 0 && ox < im->out_w) {
								int idx = k ? (chunk[i] & 15) : (chunk[i] >> 4);
								grayrow[ox++] = luma(S.bmp.pal[idx * 4 + 2],
									S.bmp.pal[idx * 4 + 1], S.bmp.pal[idx * 4]);
							}
						}
						i++;
						continue;
					}

					case 8:
						r = S.bmp.pal[chunk[i] * 4 + 2];
						g = S.bmp.pal[chunk[i] * 4 + 1];
						b = S.bmp.pal[chunk[i] * 4];
						take = 1;
						break;

					case 24:
						b = chunk[i]; g = chunk[i + 1]; r = chunk[i + 2];
						take = 3;
						break;

					default:
						b = chunk[i]; g = chunk[i + 1]; r = chunk[i + 2];
						take = 4;
						break;

				}

				if ((px & (step - 1)) == 0 && ox < im->out_w)
					grayrow[ox++] = luma(r, g, b);

				px++;
				i += take;

			}

			Z_PROF_END(Z_IMGP_PIXEL);

		}

		if (want) {
			int oy = sy >> im->shift;
			if (flip) oy = (im->out_h - 1) - oy;
			emit_row(im, grayrow, oy);
		}

	}

	return Z_IMG_OK;

}

#endif	// Z_IMG_HAVE_BMP

#if Z_IMG_HAVE_PNM

/*
 * PNM -- P1..P6.
 *
 * The ASCII variants (P1/P2/P3) are handled because they cost almost
 * nothing on top of the binary ones and are what anything
 * script-generated tends to produce. They are slow, but the slowness
 * is in the file being large rather than in the decoding.
 */

static int pnm_getc(z_img_t *im) {
	uint8_t c;
	if (im->read(im->ctx, &c, 1) != 1) return -1;
	return (int)c;
}

// Next whitespace-separated integer, skipping '#' comments. Returns
// -1 at end of file or on anything that isn't a number.
static int pnm_num(z_img_t *im) {

	int c, v = 0;

	for (;;) {
		c = pnm_getc(im);
		if (c < 0) return -1;
		if (c == '#') {
			while (c != '\n' && c >= 0) c = pnm_getc(im);
			continue;
		}
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
		break;
	}

	if (c < '0' || c > '9') return -1;

	while (c >= '0' && c <= '9') {
		v = v * 10 + (c - '0');
		if (v > (1 << 24)) return -1;
		c = pnm_getc(im);
	}

	return v;

}

#define PNM_CHUNK 480

static int decode_pnm(z_img_t *im) {

	uint8_t magic[2];
	uint8_t chunk[PNM_CHUNK];
	int type, w, h, maxv, bypp, stride, sy, rv, step;

	if (im->read(im->ctx, magic, 2) != 2) return Z_IMG_E_IO;
	if (magic[0] != 'P') return Z_IMG_E_FORMAT;

	type = magic[1] - '0';
	if (type < 1 || type > 6) return Z_IMG_E_FORMAT;

	w = pnm_num(im);
	h = pnm_num(im);
	if (w <= 0 || h <= 0) return Z_IMG_E_FORMAT;
	if (w > Z_IMG_SRC_MAX_W || h > Z_IMG_SRC_MAX_H) return Z_IMG_E_TOOBIG;

	// P1/P4 are bilevel and carry no maxval
	maxv = (type == 1 || type == 4) ? 1 : pnm_num(im);
	if (maxv <= 0 || maxv > 65535) return Z_IMG_E_FORMAT;
	if (maxv > 255) return Z_IMG_E_UNSUPPORTED;		// 16-bit samples

	rv = fit_scale(im, w, h);
	if (rv != Z_IMG_OK) return rv;

	bypp   = (type == 3 || type == 6) ? 3 : 1;
	stride = (type == 4) ? ((w + 7) >> 3) : (w * bypp);
	step   = 1 << im->shift;

	dither_reset();

	for (sy = 0; sy < h; sy++) {

		bool want = ((sy & (step - 1)) == 0);
		int ox = 0;

		if (type <= 3) {

			// ASCII: one value at a time, no row buffer at all
			int px, ch;

			for (px = 0; px < w; px++) {

				int r = 0, g = 0, b = 0;

				if (bypp == 3) {
					r = pnm_num(im); g = pnm_num(im); b = pnm_num(im);
					if (r < 0 || g < 0 || b < 0) return Z_IMG_E_IO;
				} else {
					r = pnm_num(im);
					if (r < 0) return Z_IMG_E_IO;
					// P1 is 1=black, the opposite of every other
					// greyscale convention here
					if (type == 1) r = r ? 0 : maxv;
					g = b = r;
				}

				if (!want) continue;
				if ((px & (step - 1)) != 0 || ox >= im->out_w) continue;

				if (maxv == 255) {
					grayrow[ox++] = luma(r, g, b);
				} else {
					ch = luma(r * 255 / maxv, g * 255 / maxv, b * 255 / maxv);
					grayrow[ox++] = (uint8_t)ch;
				}

			}

		} else {

			int left = stride;
			int px = 0;

			while (left > 0) {

				int n = left > PNM_CHUNK ? PNM_CHUNK : left;
				int i;

				if (n > bypp) n -= (n % bypp);

				if (im->read(im->ctx, chunk, n) != n) return Z_IMG_E_IO;
				left -= n;

				if (!want) continue;

				if (type == 4) {

					for (i = 0; i < n && px < w; i++) {
						int k;
						for (k = 0; k < 8 && px < w; k++, px++) {
							if ((px & (step - 1)) == 0 && ox < im->out_w) {
								int bit = (chunk[i] >> (7 - k)) & 1;
								grayrow[ox++] = bit ? 0 : 255;
							}
						}
					}

				} else {

					for (i = 0; i + bypp <= n && px < w; i += bypp, px++) {
						if ((px & (step - 1)) != 0 || ox >= im->out_w) continue;
						if (bypp == 3) {
							grayrow[ox++] = (maxv == 255)
								? luma(chunk[i], chunk[i+1], chunk[i+2])
								: luma(chunk[i] * 255 / maxv,
								       chunk[i+1] * 255 / maxv,
								       chunk[i+2] * 255 / maxv);
						} else {
							grayrow[ox++] = (maxv == 255)
								? chunk[i]
								: (uint8_t)(chunk[i] * 255 / maxv);
						}
					}

				}

			}

		}

		if (want) emit_row(im, grayrow, sy >> im->shift);

	}

	return Z_IMG_OK;

}

#endif	// Z_IMG_HAVE_PNM

#if Z_IMG_HAVE_GIF

/*
 * GIF87a/89a, first frame only.
 *
 * Animation is not decoded past frame one: subsequent frames need the
 * previous frame kept as 8-bit indices to composite against (a full
 * 640x480 index plane, 300KB), which does not fit. The first frame of
 * an animated GIF is a complete image by definition, so this
 * degrades to "shows the first frame" rather than failing.
 *
 * Palette luma is precomputed once into pal_y, so the per-pixel path
 * is a single table lookup rather than three multiplies -- worth it
 * when the dictionary can emit thousands of pixels per code.
 */

static int gif_blk_len, gif_blk_pos;
static int gif_bitbuf, gif_bitcnt;

// GIF image data is a chain of length-prefixed sub-blocks, terminated
// by a zero-length one.
static int gif_byte(z_img_t *im) {

	if (gif_blk_pos >= gif_blk_len) {
		uint8_t n;
		if (im->read(im->ctx, &n, 1) != 1) return -1;
		if (n == 0) return -1;
		if (im->read(im->ctx, S.gif.blk, n) != n) return -1;
		gif_blk_len = n;
		gif_blk_pos = 0;
	}

	return S.gif.blk[gif_blk_pos++];

}

static int gif_code(z_img_t *im, int nbits) {

	int v;

	while (gif_bitcnt < nbits) {
		int c = gif_byte(im);
		if (c < 0) return -1;
		gif_bitbuf |= c << gif_bitcnt;
		gif_bitcnt += 8;
	}

	v = gif_bitbuf & ((1 << nbits) - 1);
	gif_bitbuf >>= nbits;
	gif_bitcnt -= nbits;

	return v;

}

static int decode_gif(z_img_t *im) {

	uint8_t hdr[13], idsc[9], minbits;
	int gct_n, w, h, iflags, i, rv;
	int clear, eoi, next, nbits, prev, first;
	int x, ox, row, pass;
	int step;
	bool interlaced;

	static const int il_start[4] = { 0, 4, 2, 1 };
	static const int il_step[4]  = { 8, 8, 4, 2 };

	if (im->read(im->ctx, hdr, 13) != 13) return Z_IMG_E_IO;
	if (hdr[0] != 'G' || hdr[1] != 'I' || hdr[2] != 'F') return Z_IMG_E_FORMAT;

	gct_n = (hdr[10] & 0x80) ? (2 << (hdr[10] & 7)) : 0;

	if (gct_n) {
		if (im->read(im->ctx, S.gif.pal, gct_n * 3) != gct_n * 3)
			return Z_IMG_E_IO;
	}

	// walk extensions until the first image descriptor
	for (;;) {

		uint8_t b;

		if (im->read(im->ctx, &b, 1) != 1) return Z_IMG_E_IO;

		if (b == 0x2C) break;					// image descriptor
		if (b == 0x3B) return Z_IMG_E_FORMAT;	// trailer, no image at all

		if (b == 0x21) {
			uint8_t label;
			if (im->read(im->ctx, &label, 1) != 1) return Z_IMG_E_IO;
			for (;;) {
				uint8_t n;
				if (im->read(im->ctx, &n, 1) != 1) return Z_IMG_E_IO;
				if (n == 0) break;
				if (im->read(im->ctx, S.gif.blk, n) != n) return Z_IMG_E_IO;
			}
			continue;
		}

		// anything else is a corrupt stream
		return Z_IMG_E_FORMAT;

	}

	if (im->read(im->ctx, idsc, 9) != 9) return Z_IMG_E_IO;

	w = (int)(idsc[4] | (idsc[5] << 8));
	h = (int)(idsc[6] | (idsc[7] << 8));
	iflags = idsc[8];
	interlaced = (iflags & 0x40) != 0;

	if (iflags & 0x80) {						// local colour table wins
		int n = 2 << (iflags & 7);
		if (im->read(im->ctx, S.gif.pal, n * 3) != n * 3) return Z_IMG_E_IO;
		gct_n = n;
	}

	if (gct_n == 0) return Z_IMG_E_FORMAT;
	if (w <= 0 || h <= 0) return Z_IMG_E_FORMAT;
	if (w > Z_IMG_SRC_MAX_W || h > Z_IMG_SRC_MAX_H) return Z_IMG_E_TOOBIG;

	rv = fit_scale(im, w, h);
	if (rv != Z_IMG_OK) return rv;

	for (i = 0; i < gct_n; i++)
		S.gif.pal_y[i] = luma(S.gif.pal[i * 3], S.gif.pal[i * 3 + 1],
			S.gif.pal[i * 3 + 2]);

	if (im->read(im->ctx, &minbits, 1) != 1) return Z_IMG_E_IO;
	if (minbits < 2 || minbits > 11) return Z_IMG_E_FORMAT;

	gif_blk_len = gif_blk_pos = 0;
	gif_bitbuf = gif_bitcnt = 0;

	clear = 1 << minbits;
	eoi   = clear + 1;
	next  = eoi + 1;
	nbits = minbits + 1;
	prev  = -1;
	first = 0;

	for (i = 0; i < clear; i++) {
		S.gif.prefix[i] = 0xFFFF;
		S.gif.suffix[i] = (uint8_t)i;
	}

	// Interlaced rows arrive out of order, which error diffusion
	// cannot follow -- see dither_row_ordered().
	im->was_ordered = interlaced;
	dither_reset();

	// dither_reset() restores DM_FS, so the ordered fallback has to
	// be re-selected AFTER it, not before -- and it must be kept in
	// step with im->was_ordered, which is only the outward report.
	if (interlaced) dither_mode = DM_ORDERED;

	/*
	 * Bilevel fast path.
	 *
	 * A two-colour GIF is already a 1bpp image wearing a palette, and
	 * this display is 1bpp -- so the right rendering is a direct
	 * mapping, not a dither. Running Floyd-Steinberg over it is both
	 * the single most expensive phase in the decode (measured at 77%
	 * of bracketed time on hardware, against 27% for JPEG) and
	 * actively wrong: it renders two flat tones as textures that were
	 * never in the source.
	 *
	 * The guard matters. Thresholding is only lossless when the two
	 * entries fall on OPPOSITE sides of it -- a palette of two dark
	 * greys (say luma 100 and 120) would collapse to solid black,
	 * losing the picture entirely, where dithering renders them as
	 * two distinguishable textures. So the test is not "are there two
	 * colours" but "do the two colours separate cleanly", and
	 * anything else keeps the dither.
	 *
	 * Interlacing does not disqualify it: a threshold is stateless,
	 * so row order is irrelevant, and this takes precedence over the
	 * ordered fallback above.
	 */
	if (gct_n == 2) {
		int y0 = S.gif.pal_y[0] >= 128;
		int y1 = S.gif.pal_y[1] >= 128;
		if (y0 != y1) dither_mode = DM_THRESHOLD;
	}

	step = 1 << im->shift;
	x = ox = row = pass = 0;

	for (;;) {

		int code = gif_code(im, nbits);
		int sp = 0;
		int c;

		if (code < 0) break;
		if (code == eoi) break;

		if (code == clear) {
			next  = eoi + 1;
			nbits = minbits + 1;
			prev  = -1;
			continue;
		}

		c = code;

		// The KwKwK case: a code referring to the entry currently
		// being built. Legal, and produced by every real encoder.
		if (c >= next) {
			if (prev < 0) break;
			S.gif.stack[sp++] = (uint8_t)first;
			c = prev;
		}

		while (c >= clear) {
			if (sp >= 4096) return Z_IMG_E_FORMAT;
			S.gif.stack[sp++] = S.gif.suffix[c];
			c = S.gif.prefix[c];
			if (c == 0xFFFF) return Z_IMG_E_FORMAT;
		}

		S.gif.stack[sp++] = (uint8_t)c;
		first = c;

		while (sp > 0) {

			uint8_t pix = S.gif.stack[--sp];

			if ((x & (step - 1)) == 0 && ox < im->out_w)
				grayrow[ox++] = S.gif.pal_y[pix];

			if (++x >= w) {

				int oy = row;

				x = 0;

				if (interlaced) {
					row += il_step[pass];
					while (row >= h && pass < 3) {
						pass++;
						row = il_start[pass];
					}
				} else {
					row++;
				}

				if ((oy & (step - 1)) == 0)
					emit_row(im, grayrow, oy >> im->shift);

				ox = 0;

			}

		}

		if (prev >= 0 && next < 4096) {
			S.gif.prefix[next] = (uint16_t)prev;
			S.gif.suffix[next] = (uint8_t)first;
			next++;
			if (next == (1 << nbits) && nbits < 12) nbits++;
		}

		prev = code;

	}

	return Z_IMG_OK;

}

#endif	// Z_IMG_HAVE_GIF

#if Z_IMG_HAVE_JPG

/*
 * Baseline sequential JPEG, LUMA ONLY.
 *
 * The display has no colour, so the chroma components are Huffman-
 * decoded (they must be, or the bitstream desynchronises) and then
 * discarded without dequantisation or IDCT. On a 4:2:0 image that is
 * a third of the blocks skipped, plus all of the upsampling and all
 * of the YCbCr conversion -- easily the largest single saving
 * available in this decoder, and it comes from the display being
 * monochrome rather than from any cleverness.
 *
 * Progressive JPEG is refused. It needs every coefficient of the
 * whole image resident across multiple scans -- roughly 600KB for
 * luma alone at 640x480 -- which is not a tuning problem but a
 * structural impossibility without dynamic memory.
 *
 * Output is produced one MCU row at a time (8 or 16 source rows),
 * decimated into mcurow and then flushed through the ditherer in
 * order, so error diffusion still sees rows top-to-bottom.
 */

static const uint8_t jpg_zigzag[64] = {
	 0, 1, 8,16, 9, 2, 3,10, 17,24,32,25,18,11, 4, 5,
	12,19,26,33,40,48,41,34, 27,20,13, 6, 7,14,21,28,
	35,42,49,56,57,50,43,36, 29,22,15,23,30,37,44,51,
	58,59,52,45,38,31,39,46, 53,60,61,54,47,55,62,63
};

/*
 * -- bit reader --
 *
 * A 32-bit MSB-aligned barrel. Bits are consumed from bit 31 down and
 * the buffer is refilled to >=25 bits whenever it runs low, so a
 * 16-bit magnitude field is always satisfiable from the register
 * without touching memory.
 *
 * The previous version fetched one byte per BIT request through the
 * byte-source callback. On hardware that measured ~159 instructions
 * per byte and made the entropy phase 52% of total decode time, which
 * is what this exists to fix.
 */

// Whether the block just decoded has any AC coefficient at all. A
// block without one is a flat 8x8 of its DC value, so the whole IDCT
// -- 256 multiplies -- collapses to a fill. Roughly a quarter of
// blocks in ordinary photographic material.
static bool jpg_ac_present;

static uint32_t jpg_bitbuf;		// MSB-aligned; next bit is bit 31
static int jpg_bitcnt;			// valid bits held
static int jpg_marker;			// marker seen by the reader, 0 if none
static int jpg_inpos, jpg_inlen;

// Next raw byte of the scan, from the staging buffer.
static int jpg_raw(z_img_t *im) {

	if (jpg_inpos >= jpg_inlen) {
		jpg_inlen = im->read(im->ctx, S.jpg.inbuf, Z_JPG_INBUF);
		jpg_inpos = 0;
		if (jpg_inlen <= 0) { jpg_inlen = 0; return -1; }
	}

	return S.jpg.inbuf[jpg_inpos++];

}

// Entropy-coded data stuffs 0xFF as 0xFF 0x00. An 0xFF followed by
// anything else is a marker: the segment has ended, and from here the
// reader feeds zeros until the caller notices.
static int jpg_byte(z_img_t *im) {

	int c, d;

	if (jpg_marker) return -1;

	c = jpg_raw(im);
	if (c < 0) return -1;

	if (c == 0xFF) {
		d = jpg_raw(im);
		if (d < 0) return -1;
		if (d != 0x00) { jpg_marker = d; return -1; }
	}

	return c;

}

static void jpg_refill(z_img_t *im) {

	while (jpg_bitcnt <= 24) {
		int c = jpg_byte(im);
		if (c < 0) c = 0;			// past the end: pad with zeros
		jpg_bitbuf |= (uint32_t)c << (24 - jpg_bitcnt);
		jpg_bitcnt += 8;
	}

}

static inline int jpg_bit(z_img_t *im) {

	int b;

	if (jpg_bitcnt < 1) jpg_refill(im);

	b = (int)(jpg_bitbuf >> 31);
	jpg_bitbuf <<= 1;
	jpg_bitcnt--;

	return b;

}

static int jpg_bits(z_img_t *im, int n) {

	int v;

	if (n <= 0) return 0;			// shifting by 32 is undefined
	if (jpg_bitcnt < n) jpg_refill(im);

	v = (int)(jpg_bitbuf >> (32 - n));
	jpg_bitbuf <<= n;
	jpg_bitcnt -= n;

	return v;

}

static void jpg_huff_build(z_jpg_huff_t *h) {

	int code = 0, k = 0, l, i, j;

	for (l = 1; l <= 16; l++) {
		h->valptr[l] = k;
		h->mincode[l] = code;
		if (h->bits[l]) {
			k += h->bits[l];
			code += h->bits[l];
			h->maxcode[l] = code - 1;
		} else {
			h->maxcode[l] = -1;
		}
		code <<= 1;
	}

	h->maxcode[17] = 0x7FFFFFFF;

	// 8-bit peek table. Every code of length l <= 8 owns the 2^(8-l)
	// entries that share its prefix, so one indexed load resolves both
	// the symbol and how many bits to drop. 0 in look_nbits means
	// "longer than 8 bits, take the slow path".
	memset(h->look_nbits, 0, sizeof(h->look_nbits));

	code = 0;
	k = 0;

	for (l = 1; l <= 16; l++) {
		for (i = 0; i < h->bits[l]; i++) {
			if (l <= Z_JPG_LOOKBITS) {
				int base = code << (Z_JPG_LOOKBITS - l);
				int n = 1 << (Z_JPG_LOOKBITS - l);
				for (j = 0; j < n; j++) {
					h->look_nbits[base + j] = (uint8_t)l;
					h->look_sym[base + j] = h->vals[k];
				}
			}
			code++;
			k++;
		}
		code <<= 1;
	}

}

static int jpg_huff_decode(z_img_t *im, z_jpg_huff_t *h) {

	int code, l, nb, peek;

	if (jpg_bitcnt < 16) jpg_refill(im);

	// Fast path: the next 8 bits identify the symbol outright for any
	// code of 8 bits or fewer, which in a baseline table is most of
	// them.
	peek = (int)((jpg_bitbuf >> 24) & 0xFF);
	nb = h->look_nbits[peek];

	if (nb) {
		int sym = h->look_sym[peek];
		jpg_bitbuf <<= nb;
		jpg_bitcnt -= nb;
		return sym;
	}

	// 9..16 bits: walk it the long way.
	code = 0;

	for (l = 1; l <= 16; l++) {
		code = (code << 1) | jpg_bit(im);
		if (h->maxcode[l] >= 0 && code <= h->maxcode[l])
			return h->vals[h->valptr[l] + (code - h->mincode[l])];
	}

	return 0;

}

static inline int jpg_extend(int v, int n) {
	return (n == 0) ? 0 : ((v < (1 << (n - 1))) ? (v - (1 << n) + 1) : v);
}

/*
 * Integer IDCT, the AAN-derived form with constants scaled by 4096.
 *
 * Fixed point throughout: there is no FPU on this core, so a float
 * IDCT would become soft-float calls and dominate the entire decode.
 * The column pass folds in the +128 level shift and the final
 * rounding, so the output is clamped bytes directly with no separate
 * pass over the block.
 */

#define JF(x) ((int32_t)(x))

static void jpg_idct(const int16_t *coef, const uint16_t *q,
	uint8_t *out, int out_stride) {

	int32_t *v = S.jpg.ws;
	int i;

	Z_PROF_BEGIN(Z_IMGP_IDCT);

	// DC-only: every output sample is the same value, so skip both
	// passes entirely. Derived from the full path rather than guessed
	// -- with only coef[0] nonzero the column pass yields (s0 << 2)
	// and the row pass turns that into ((r0 * 4096) + rounding) >> 17.
	if (!jpg_ac_present) {

		int32_t dc = (int32_t)coef[0] * (int32_t)q[0];
		int32_t val = ((dc << 14) + 65536 + (128 << 17)) >> 17;
		uint8_t b;

		if (val < 0) val = 0;
		else if (val > 255) val = 255;
		b = (uint8_t)val;

		for (i = 0; i < 8; i++) memset(out + i * out_stride, b, 8);

		Z_PROF_END(Z_IMGP_IDCT);
		return;

	}

	// columns
	for (i = 0; i < 8; i++) {

		int32_t s0 = (int32_t)coef[i +  0] * (int32_t)q[i +  0];
		int32_t s1 = (int32_t)coef[i +  8] * (int32_t)q[i +  8];
		int32_t s2 = (int32_t)coef[i + 16] * (int32_t)q[i + 16];
		int32_t s3 = (int32_t)coef[i + 24] * (int32_t)q[i + 24];
		int32_t s4 = (int32_t)coef[i + 32] * (int32_t)q[i + 32];
		int32_t s5 = (int32_t)coef[i + 40] * (int32_t)q[i + 40];
		int32_t s6 = (int32_t)coef[i + 48] * (int32_t)q[i + 48];
		int32_t s7 = (int32_t)coef[i + 56] * (int32_t)q[i + 56];

		int32_t p1, p2, p3, p4, p5, t0, t1, t2, t3, x0, x1, x2, x3;

		// A column with no AC content at all is flat. Very common in
		// smooth areas, and skipping the butterflies there is a large
		// win on photographic material.
		if (!(s1 | s2 | s3 | s4 | s5 | s6 | s7)) {
			int32_t dc = s0 << 2;
			v[i + 0] = v[i + 8] = v[i + 16] = v[i + 24] =
			v[i + 32] = v[i + 40] = v[i + 48] = v[i + 56] = dc;
			continue;
		}

		p2 = s2; p3 = s6;
		p1 = (p2 + p3) * JF(2217);
		t2 = p1 + p3 * JF(-7567);
		t3 = p1 + p2 * JF(3135);
		p2 = s0; p3 = s4;
		t0 = (p2 + p3) * 4096;
		t1 = (p2 - p3) * 4096;
		x0 = t0 + t3; x3 = t0 - t3;
		x1 = t1 + t2; x2 = t1 - t2;

		t0 = s7; t1 = s5; t2 = s3; t3 = s1;
		p3 = t0 + t2; p4 = t1 + t3;
		p1 = t0 + t3; p2 = t1 + t2;
		p5 = (p3 + p4) * JF(4816);
		t0 = t0 * JF(1223);
		t1 = t1 * JF(8410);
		t2 = t2 * JF(12586);
		t3 = t3 * JF(6149);
		p1 = p5 + p1 * JF(-3685);
		p2 = p5 + p2 * JF(-10497);
		p3 = p3 * JF(-8034);
		p4 = p4 * JF(-1597);
		t3 += p1 + p4; t2 += p2 + p3;
		t1 += p2 + p4; t0 += p1 + p3;

		x0 += 512; x1 += 512; x2 += 512; x3 += 512;

		v[i +  0] = (x0 + t3) >> 10;
		v[i + 56] = (x0 - t3) >> 10;
		v[i +  8] = (x1 + t2) >> 10;
		v[i + 48] = (x1 - t2) >> 10;
		v[i + 16] = (x2 + t1) >> 10;
		v[i + 40] = (x2 - t1) >> 10;
		v[i + 24] = (x3 + t0) >> 10;
		v[i + 32] = (x3 - t0) >> 10;

	}

	// rows
	for (i = 0; i < 8; i++) {

		int32_t *r = v + i * 8;
		uint8_t *o = out + i * out_stride;
		int32_t p1, p2, p3, p4, p5, t0, t1, t2, t3, x0, x1, x2, x3;
		int k;

		// Flat row: same shortcut as the DC-only block above, one row
		// at a time. After the column pass a block with few
		// coefficients leaves most of its rows with nothing but r[0],
		// and each of those otherwise costs a full 1-D pass -- ~12
		// multiplies and the whole butterfly -- to produce eight
		// copies of one number.
		//
		// This catches what the whole-block test cannot: a block with
		// a couple of low-frequency coefficients is not DC-only, but
		// is still mostly flat rows.
		if (!(r[1] | r[2] | r[3] | r[4] | r[5] | r[6] | r[7])) {

			int32_t val = ((r[0] * 4096) + 65536 + (128 << 17)) >> 17;

			if (val < 0) val = 0;
			else if (val > 255) val = 255;

			memset(o, (uint8_t)val, 8);
			continue;

		}

		p2 = r[2]; p3 = r[6];
		p1 = (p2 + p3) * JF(2217);
		t2 = p1 + p3 * JF(-7567);
		t3 = p1 + p2 * JF(3135);
		p2 = r[0]; p3 = r[4];
		t0 = (p2 + p3) * 4096;
		t1 = (p2 - p3) * 4096;
		x0 = t0 + t3; x3 = t0 - t3;
		x1 = t1 + t2; x2 = t1 - t2;

		t0 = r[7]; t1 = r[5]; t2 = r[3]; t3 = r[1];
		p3 = t0 + t2; p4 = t1 + t3;
		p1 = t0 + t3; p2 = t1 + t2;
		p5 = (p3 + p4) * JF(4816);
		t0 = t0 * JF(1223);
		t1 = t1 * JF(8410);
		t2 = t2 * JF(12586);
		t3 = t3 * JF(6149);
		p1 = p5 + p1 * JF(-3685);
		p2 = p5 + p2 * JF(-10497);
		p3 = p3 * JF(-8034);
		p4 = p4 * JF(-1597);
		t3 += p1 + p4; t2 += p2 + p3;
		t1 += p2 + p4; t0 += p1 + p3;

		// +128 level shift and rounding folded into the constant
		x0 += 65536 + (128 << 17);
		x1 += 65536 + (128 << 17);
		x2 += 65536 + (128 << 17);
		x3 += 65536 + (128 << 17);

		{
			int32_t out8[8];
			out8[0] = (x0 + t3) >> 17;
			out8[7] = (x0 - t3) >> 17;
			out8[1] = (x1 + t2) >> 17;
			out8[6] = (x1 - t2) >> 17;
			out8[2] = (x2 + t1) >> 17;
			out8[5] = (x2 - t1) >> 17;
			out8[3] = (x3 + t0) >> 17;
			out8[4] = (x3 - t0) >> 17;
			for (k = 0; k < 8; k++) {
				int32_t val = out8[k];
				if (val < 0) val = 0;
				else if (val > 255) val = 255;
				o[k] = (uint8_t)val;
			}
		}

	}

	Z_PROF_END(Z_IMGP_IDCT);

}

// One 8x8 block. `keep` false means "consume the bits and throw the
// result away" -- the chroma path.
static void jpg_block(z_img_t *im, z_jpg_comp_t *c, bool keep) {

	int t, diff, k;

	Z_PROF_BEGIN(Z_IMGP_ENTROPY);

	jpg_ac_present = false;

	if (keep) memset(S.jpg.coef, 0, sizeof(S.jpg.coef));

	t = jpg_huff_decode(im, &S.jpg.hdc[c->td]);
	diff = jpg_extend(jpg_bits(im, t), t);
	c->dcpred += diff;

	if (keep) S.jpg.coef[0] = (int16_t)c->dcpred;

	for (k = 1; k < 64; ) {

		int rs = jpg_huff_decode(im, &S.jpg.hac[c->ta]);
		int r = rs >> 4;
		int s = rs & 15;

		if (s == 0) {
			if (r != 15) break;		// EOB
			k += 16;				// ZRL
			continue;
		}

		k += r;
		if (k > 63) break;

		{
			int v = jpg_extend(jpg_bits(im, s), s);
			if (keep) S.jpg.coef[jpg_zigzag[k]] = (int16_t)v;
			jpg_ac_present = true;
		}

		k++;

	}

	Z_PROF_END(Z_IMGP_ENTROPY);

}

static int decode_jpg(z_img_t *im) {

	uint8_t b[2];
	int w = 0, h = 0, ncomp = 0, restart = 0;
	int hmax = 1, vmax = 1, mcuw, mcuh, mcux, mcuy;
	int my, mx, ci, i, rv, step, mcu_count = 0;

	if (im->read(im->ctx, b, 2) != 2) return Z_IMG_E_IO;
	if (b[0] != 0xFF || b[1] != 0xD8) return Z_IMG_E_FORMAT;

	jpg_marker = 0;

	// -- header parse, up to and including SOS --

	for (;;) {

		int m, len;

		do {
			if (im->read(im->ctx, b, 1) != 1) return Z_IMG_E_IO;
		} while (b[0] != 0xFF);

		do {
			if (im->read(im->ctx, b, 1) != 1) return Z_IMG_E_IO;
			m = b[0];
		} while (m == 0xFF);

		if (m == 0xD9) return Z_IMG_E_FORMAT;			// EOI before any scan
		if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;	// no payload

		if (im->read(im->ctx, b, 2) != 2) return Z_IMG_E_IO;
		len = (int)((b[0] << 8) | b[1]) - 2;
		if (len < 0) return Z_IMG_E_FORMAT;

		if (m == 0xC0 || m == 0xC1) {					// SOF0 / SOF1

			uint8_t s[6 + 3 * 3];

			if (len > (int)sizeof(s)) return Z_IMG_E_UNSUPPORTED;
			if (im->read(im->ctx, s, len) != len) return Z_IMG_E_IO;

			if (s[0] != 8) return Z_IMG_E_UNSUPPORTED;	// 12-bit samples
			h = (int)((s[1] << 8) | s[2]);
			w = (int)((s[3] << 8) | s[4]);
			ncomp = s[5];

			if (ncomp < 1 || ncomp > 3) return Z_IMG_E_UNSUPPORTED;

			for (i = 0; i < ncomp; i++) {
				S.jpg.comp[i].id = s[6 + i * 3];
				S.jpg.comp[i].hs = s[7 + i * 3] >> 4;
				S.jpg.comp[i].vs = s[7 + i * 3] & 15;
				S.jpg.comp[i].tq = s[8 + i * 3];
				if (S.jpg.comp[i].hs < 1 || S.jpg.comp[i].hs > 2 ||
					S.jpg.comp[i].vs < 1 || S.jpg.comp[i].vs > 2)
					return Z_IMG_E_UNSUPPORTED;
				if (S.jpg.comp[i].tq > 3) return Z_IMG_E_FORMAT;
			}

		} else if (m == 0xC2) {

			// Progressive. See this decoder's header comment.
			return Z_IMG_E_UNSUPPORTED;

		} else if (m == 0xC3 || (m >= 0xC5 && m <= 0xCF && m != 0xC8)) {

			// Lossless, differential, arithmetic-coded variants
			return Z_IMG_E_UNSUPPORTED;

		} else if (m == 0xC4) {							// DHT

			while (len > 0) {

				uint8_t tc;
				z_jpg_huff_t *ht;
				int n = 0;

				if (im->read(im->ctx, &tc, 1) != 1) return Z_IMG_E_IO;
				if ((tc & 15) > 1) return Z_IMG_E_UNSUPPORTED;

				ht = (tc >> 4) ? &S.jpg.hac[tc & 15] : &S.jpg.hdc[tc & 15];

				ht->bits[0] = 0;
				if (im->read(im->ctx, ht->bits + 1, 16) != 16)
					return Z_IMG_E_IO;

				for (i = 1; i <= 16; i++) n += ht->bits[i];
				if (n > 256) return Z_IMG_E_FORMAT;
				if (im->read(im->ctx, ht->vals, n) != n) return Z_IMG_E_IO;

				jpg_huff_build(ht);
				len -= 17 + n;

			}

		} else if (m == 0xDB) {							// DQT

			while (len > 0) {

				uint8_t pq;
				int prec, id;

				if (im->read(im->ctx, &pq, 1) != 1) return Z_IMG_E_IO;
				prec = pq >> 4;
				id = pq & 15;
				if (id > 3) return Z_IMG_E_FORMAT;

				for (i = 0; i < 64; i++) {
					uint8_t v[2];
					int nb = prec ? 2 : 1;
					if (im->read(im->ctx, v, nb) != nb) return Z_IMG_E_IO;
					S.jpg.qtab[id][jpg_zigzag[i]] = prec
						? (uint16_t)((v[0] << 8) | v[1]) : (uint16_t)v[0];
				}

				len -= 1 + (prec ? 128 : 64);

			}

		} else if (m == 0xDD) {							// DRI

			uint8_t r[2];
			if (im->read(im->ctx, r, 2) != 2) return Z_IMG_E_IO;
			restart = (int)((r[0] << 8) | r[1]);
			len -= 2;
			if (len > 0) { rv = skip_bytes(im, (uint32_t)len); if (rv) return rv; }

		} else if (m == 0xDA) {							// SOS

			uint8_t s[1 + 3 * 2 + 3];
			int ns, j;

			if (len > (int)sizeof(s)) return Z_IMG_E_UNSUPPORTED;
			if (im->read(im->ctx, s, len) != len) return Z_IMG_E_IO;

			ns = s[0];
			if (ns != ncomp) return Z_IMG_E_UNSUPPORTED;	// non-interleaved

			for (i = 0; i < ns; i++) {
				for (j = 0; j < ncomp; j++) {
					if (S.jpg.comp[j].id == s[1 + i * 2]) {
						S.jpg.comp[j].td = s[2 + i * 2] >> 4;
						S.jpg.comp[j].ta = s[2 + i * 2] & 15;
						if (S.jpg.comp[j].td > 1 || S.jpg.comp[j].ta > 1)
							return Z_IMG_E_UNSUPPORTED;
					}
				}
			}

			break;

		} else {

			rv = skip_bytes(im, (uint32_t)len);
			if (rv != Z_IMG_OK) return rv;

		}

	}

	if (w <= 0 || h <= 0 || ncomp == 0) return Z_IMG_E_FORMAT;
	if (w > Z_IMG_SRC_MAX_W || h > Z_IMG_SRC_MAX_H) return Z_IMG_E_TOOBIG;

	rv = fit_scale(im, w, h);
	if (rv != Z_IMG_OK) return rv;

	for (i = 0; i < ncomp; i++) {
		if (S.jpg.comp[i].hs > hmax) hmax = S.jpg.comp[i].hs;
		if (S.jpg.comp[i].vs > vmax) vmax = S.jpg.comp[i].vs;
		S.jpg.comp[i].dcpred = 0;
	}

	mcuw = hmax * 8;
	mcuh = vmax * 8;
	mcux = (w + mcuw - 1) / mcuw;
	mcuy = (h + mcuh - 1) / mcuh;

	jpg_bitbuf = 0;
	jpg_bitcnt = 0;
	jpg_inpos = 0;
	jpg_inlen = 0;
	im->was_ordered = false;
	dither_reset();

	step = 1 << im->shift;

	// -- scan --

	for (my = 0; my < mcuy; my++) {

		int yy;

		memset(S.jpg.mcurow, 0, (size_t)im->out_w * (size_t)mcuh);

		for (mx = 0; mx < mcux; mx++) {

			if (restart && mcu_count && (mcu_count % restart) == 0) {

				// Resynchronise at a restart marker: the bit buffer is
				// discarded and every DC predictor resets. If the
				// reader has not already tripped over the marker,
				// scan forward for it.
				// Discard the partially-consumed word AND the
				// staging bytes it came from -- both are part of
				// the segment being abandoned.
				jpg_bitbuf = 0;
				jpg_bitcnt = 0;

				// Scan forward for the RSTn through jpg_raw(), not
				// im->read(): the staging buffer already holds bytes
				// the byte source has handed over, and reading around
				// it would skip them.
				if (!jpg_marker) {
					int c, guard = 0;
					for (;;) {
						c = jpg_raw(im);
						if (c < 0) break;
						if (c != 0xFF) { if (++guard > 65536) break; continue; }
						c = jpg_raw(im);
						if (c < 0) break;
						if (c == 0x00) { if (++guard > 65536) break; continue; }
						jpg_marker = c;
						break;
					}
				}

				if (jpg_marker >= 0xD0 && jpg_marker <= 0xD7) jpg_marker = 0;

				for (i = 0; i < ncomp; i++) S.jpg.comp[i].dcpred = 0;

			}

			mcu_count++;

			for (ci = 0; ci < ncomp; ci++) {

				z_jpg_comp_t *c = &S.jpg.comp[ci];
				bool keep = (ci == 0);
				int by, bx;

				for (by = 0; by < c->vs; by++) {
					for (bx = 0; bx < c->hs; bx++) {

						uint8_t px[64];
						int ox0, oy0, sy2, sx2;

						jpg_block(im, c, keep);
						if (!keep) continue;

						jpg_idct(S.jpg.coef, S.jpg.qtab[c->tq], px, 8);

						ox0 = mx * mcuw + bx * 8;
						oy0 = by * 8;

						for (sy2 = 0; sy2 < 8; sy2++) {

							int oy = oy0 + sy2;
							if (oy >= mcuh) break;

							for (sx2 = 0; sx2 < 8; sx2++) {
								int sx = ox0 + sx2;
								int dx;
								if (sx >= w) break;
								if ((sx & (step - 1)) != 0) continue;
								dx = sx >> im->shift;
								if (dx < im->out_w)
									S.jpg.mcurow[oy * im->out_w + dx] =
										px[sy2 * 8 + sx2];
							}

						}

					}
				}

			}

		}

		// flush this MCU row, in order, through the ditherer
		for (yy = 0; yy < mcuh; yy++) {

			int sy = my * mcuh + yy;

			if (sy >= h) break;
			if ((sy & (step - 1)) != 0) continue;

			memcpy(grayrow, S.jpg.mcurow + yy * im->out_w,
				(size_t)im->out_w);

			emit_row(im, grayrow, sy >> im->shift);

		}

	}

	return Z_IMG_OK;

}

#endif	// Z_IMG_HAVE_JPG

// -- dispatch --

int z_img_decode(z_img_t *im, z_img_fmt_t fmt) {

	im->src_w = im->src_h = 0;
	im->out_w = im->out_h = 0;
	im->shift = 0;
	im->was_ordered = false;

	if (!im->read || !im->doc) return Z_IMG_E_FORMAT;

	switch (fmt) {

		case Z_IMG_FMT_ZBM:
			return decode_zbm(im);

#if Z_IMG_HAVE_BMP
		case Z_IMG_FMT_BMP:
			return decode_bmp(im);
#endif
#if Z_IMG_HAVE_PNM
		case Z_IMG_FMT_PNM:
			return decode_pnm(im);
#endif
#if Z_IMG_HAVE_GIF
		case Z_IMG_FMT_GIF:
			return decode_gif(im);
#endif
#if Z_IMG_HAVE_JPG
		case Z_IMG_FMT_JPG:
			return decode_jpg(im);
#endif

		case Z_IMG_FMT_PNG:
			// Recognised by z_img_sniff() whether or not the decoder
			// is built, so the app can say "PNG is not supported"
			// rather than "this is not an image".
			return Z_IMG_E_NOTBUILT;

		default:
			return Z_IMG_E_FORMAT;

	}

}
