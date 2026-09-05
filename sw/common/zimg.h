#ifndef ZIMG_H
#define ZIMG_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * ZIMG -- streaming image decoders for a 1bpp display.
 *
 * Written for sw/apps/view, but deliberately independent of it: the
 * only things this module touches are a byte source (a callback) and
 * a destination bitmap (a caller-owned pointer). sw/apps/draw can
 * link it to open images into its canvas without any of view's
 * furniture coming along -- see docs/view_app.md.
 *
 * -- the shape of this, and why --
 *
 * There is no dynamic memory in this system (docs/app_runtime.md), and
 * the kernel pool is 1MB total shared by every process (sw/os/mem.h),
 * so an app that holds a decoded truecolour image cannot exist: a
 * 640x480 RGB buffer alone is 900KB. Everything here is therefore
 * ROW-STREAMING. A decoder reads a source row, converts it to 8-bit
 * gray, decimates it horizontally, hands it to the ditherer, and
 * forgets it. Nothing larger than one source row (plus whatever the
 * compression scheme itself demands) is ever resident.
 *
 * The consequence worth knowing up front: the output is dithered at
 * DECODE time, into a fixed bitmap. There is no intermediate
 * greyscale image, so re-dithering at a different size means decoding
 * the file again. That is the trade that makes the whole thing fit.
 *
 * -- memory --
 *
 * Per-format scratch lives in a union (z_img_scratch_t in zimg.c):
 * only one decoder ever runs at a time, so GIF's LZW dictionary and
 * JPEG's MCU row share the same bytes. Sizing it by the largest
 * member rather than the sum saves ~13KB today, and considerably more
 * if PNG is added (its 32KB inflate window would otherwise be pure
 * addition).
 *
 * -- bit order --
 *
 * Output goes into the framebuffer's own packing: pixel x at bit
 * (x & 31) of word (x >> 5), LEAST significant bit LEFTMOST, exactly
 * as zbm.h describes. That is what z_fb_hw_blit_mem() reads, so a
 * decoded image reaches the screen in one blitter operation with no
 * reformatting. Note this is the OPPOSITE order from font and icon
 * data; getting it wrong produces an image mirrored within each
 * 32-pixel block, which zbm.h names as a symptom for exactly this
 * reason.
 */

#include <stdint.h>
#include <stdbool.h>

// Maximum decoded extent. Matches the framebuffer, and therefore
// draw's canvas -- see CANVAS_W in sw/apps/draw/draw.c and
// Z_SCREEN_W/H in zgfx.h. A source larger than this is downscaled by
// a power of two at decode time (z_img_t.shift below); it is NOT
// stored at full size and panned, because a 2000x1500 dithered bitmap
// is 375KB and cannot exist in this memory budget.
#define Z_IMG_MAX_W   640
#define Z_IMG_MAX_H   480

#define Z_IMG_MAX_WPL (Z_IMG_MAX_W / 32)

// Largest source the power-of-two downscale can bring into range.
// shift caps at 3 (1/8), so 640*8 across and 480*8 down.
#define Z_IMG_SRC_MAX_W (Z_IMG_MAX_W << 3)
#define Z_IMG_SRC_MAX_H (Z_IMG_MAX_H << 3)

typedef enum {
	Z_IMG_FMT_NONE = 0,
	Z_IMG_FMT_ZBM,			// draw's own raw bitmap (zbm.h)
	Z_IMG_FMT_BMP,
	Z_IMG_FMT_PNM,			// P1..P6
	Z_IMG_FMT_GIF,
	Z_IMG_FMT_JPG,
	Z_IMG_FMT_PNG,			// recognised; decoder not built yet
} z_img_fmt_t;

// Return codes. Negative is failure; z_img_strerror() renders any of
// them as a short line suitable for a dialog.
#define Z_IMG_OK            0
#define Z_IMG_E_FORMAT     -1	// not the format it claimed to be
#define Z_IMG_E_UNSUPPORTED -2	// valid, but a variant we don't decode
#define Z_IMG_E_IO         -3	// short read / file ended early
#define Z_IMG_E_TOOBIG     -4	// beyond Z_IMG_SRC_MAX_W/H
#define Z_IMG_E_NOTBUILT   -5	// decoder compiled out (see Z_IMG_HAVE_*)

// Which decoders are compiled in. PNG is off by default: its inflate
// window alone is 32KB of .bss, which is more than every other
// decoder here put together, and it is the slowest of them by a wide
// margin. Turn it on with -DZ_IMG_HAVE_PNG=1 once that cost has been
// measured against something real.
#ifndef Z_IMG_HAVE_BMP
#define Z_IMG_HAVE_BMP 1
#endif
#ifndef Z_IMG_HAVE_PNM
#define Z_IMG_HAVE_PNM 1
#endif
#ifndef Z_IMG_HAVE_GIF
#define Z_IMG_HAVE_GIF 1
#endif
#ifndef Z_IMG_HAVE_JPG
#define Z_IMG_HAVE_JPG 1
#endif
#ifndef Z_IMG_HAVE_PNG
#define Z_IMG_HAVE_PNG 0
#endif

// -- byte source --
//
// Returns the number of bytes actually read, which the decoders treat
// as fatal if it is short of what was asked for. Deliberately a
// callback rather than a file handle: it keeps this module free of
// any dependency on the filesystem, so the same decoders can run over
// a buffer in memory, or eventually a network stream, without change.
typedef int (*z_img_read_fn)(void *ctx, uint8_t *buf, int len);

typedef struct {

	// -- set by the caller before z_img_decode() --

	z_img_read_fn	read;
	void			*ctx;

	// Destination bitmap, in framebuffer packing (see the header
	// comment). Caller-owned: this module never allocates.
	uint32_t		*doc;
	int				doc_wpl;	// words per line
	int				doc_w, doc_h;	// capacity, in pixels

	// -- filled in by z_img_decode() --

	int				src_w, src_h;	// as recorded in the file
	int				out_w, out_h;	// after downscaling
	int				shift;			// 0=1:1, 1=1/2, 2=1/4, 3=1/8

	// True if the image had to be dithered without error diffusion.
	// Interlaced GIF delivers rows out of order, and Floyd-Steinberg
	// needs row y-1 finished before row y -- so those fall back to an
	// ordered (Bayer) dither. Exposed so the app can say so rather
	// than leaving the user wondering why one file looks coarser.
	bool			was_ordered;

} z_img_t;

// Identifies a format from the first bytes of a file. Needs at least
// 16 bytes to distinguish every format it knows; fewer is safe but
// may return Z_IMG_FMT_NONE for something it would otherwise have
// recognised.
//
// Sniffs CONTENT, not the filename extension. A .jpg that is really a
// PNG is common enough on real media to be worth handling, and costs
// nothing here.
z_img_fmt_t z_img_sniff(const uint8_t *hdr, int n);

// Decodes into im->doc. The source must be positioned at byte 0 --
// call z_img_file_rewind() (or the equivalent) after sniffing.
//
// The destination is NOT cleared first: z_img_clear() is separate, so
// a caller that is about to fill the whole thing can skip it. On
// failure partway through, whatever was decoded so far stays in the
// bitmap, which is usually more useful than a blank window.
int z_img_decode(z_img_t *im, z_img_fmt_t fmt);

// Short human-readable form of a Z_IMG_E_* code, for a dialog.
const char *z_img_strerror(int rv);

// Short name of a format ("GIF", "JPEG", ...), for a title bar.
const char *z_img_fmt_name(z_img_fmt_t fmt);

// Fills the whole bitmap with 0. Word-at-a-time, so considerably
// faster than the per-pixel path.
void z_img_clear(z_img_t *im);

// -- profiling --
//
// Phase timing is cycle-accurate via zprof.h, which follows the
// pattern sw/apps/play/prof.h established: rdcycle/rdinstret read as
// raw instruction words, no syscall, 1-cycle resolution.
//
// The earlier plan here was to time phases with z_uptime_ticks() and
// subtract. That does not work, for the reason play's prof.h already
// documents: one tick is 1.37ms (65664 cycles), and every phase worth
// optimising in this file is far shorter than that.
//
// It would also have been actively misleading. There is no data cache
// on this SOC -- rtl/cache.v caches instruction fetches only -- so a
// counter in .bss costs a load and a store at full main-memory
// latency, ~11 cycles per word on SDRAM and ~63 on PSRAM. Per-pixel
// instrumentation would have cost more than the arithmetic it was
// measuring. Every bracket below is therefore per ROW or per BLOCK,
// never per pixel, so the instrument amortises to well under a cycle
// per pixel.
//
// Phases. Keep in step with z_img_prof_names[] in zimg.c.
enum {
	Z_IMGP_READ = 0,	// the byte source: buffer copies and fs_read_chunk
	Z_IMGP_ENTROPY,		// JPEG Huffman decode of one block
	Z_IMGP_IDCT,		// one 8x8 inverse DCT
	Z_IMGP_PIXEL,		// one row converted to 8-bit gray (luma etc.)
	Z_IMGP_DITHER,		// one row dithered into the document
	Z_IMGP_BLIT,		// document to screen (view.c)
	Z_IMGP_COUNT
};

// -- buffered file source --
//
// A z_img_read_fn over zfsapp.h's file handles. Lives here rather
// than in the app because every caller needs exactly the same thing
// and the buffering is not optional: the decoders read a byte at a
// time in places (GIF sub-blocks, JPEG's entropy stream), and one
// syscall per byte through the kernel's filesystem API would dominate
// the decode by orders of magnitude.
#define Z_IMG_FILEBUF 1024

typedef struct {
	int		handle;
	int		len, pos;
	bool	eof;
	uint8_t	buf[Z_IMG_FILEBUF];
} z_img_file_t;

// Returns Z_IMG_OK or Z_IMG_E_IO. On success the caller must
// z_img_file_close() -- a leaked handle permanently consumes one of
// only Z_FS_MAX_OPEN (8) slots and nothing sweeps them at process
// exit. See zfs.h.
int  z_img_file_open(z_img_file_t *f, const char *path);
int  z_img_file_read(void *ctx, uint8_t *buf, int len);	// z_img_read_fn
int  z_img_file_rewind(z_img_file_t *f);
void z_img_file_close(z_img_file_t *f);

#endif
