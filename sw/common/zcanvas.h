#ifndef ZCANVAS_H
#define ZCANVAS_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zcanvas -- a 2D rasterizer for pages: fills, thick strokes, patterns,
 * shades and scaled images, into a 1bpp bitmap (the screen's packing)
 * or an 8-bit grey band (a printer's). docs/zcanvas.md.
 *
 * -- why it exists --
 *
 * slide draws a card at 100% in its window, at 200% when zoomed, at
 * 640x480 in a show and at 300 dpi on paper, and every one of those
 * must come from the same description of the card, never from a
 * scaled copy of its pixels. That needs a rasterizer that takes the
 * description at any scale. zsvg has the right shape (spans out of
 * flattened edges) but works in floats -- software floating point on
 * this CPU -- draws 1-px strokes only, and stops at the screen. This
 * is the same idea in fixed point, with the things a page needs.
 *
 * -- the model --
 *
 * Colour is LIGHT, not ink: a 1 bit is white, a 0 bit black, a grey
 * byte 255 white. That is how zimg already decodes images (bright
 * pixels set), so a photo on a card is not a negative, and it is what
 * paper is. A slide card on the dark desktop is therefore a white page
 * with black ink, as HyperCard's were.
 *
 * Tone is darkness, 0 (white) to 16 (black): the 17 levels of
 * zgfx.h's shaded fills, with the same 4x4 Bayer matrix, so a shade
 * drawn here and one the blitter draws look the same. On a grey target
 * a tone is a real grey and the printer halftones it.
 *
 * Coordinates are 24.8 fixed point (zc_fix). Paths are built in USER
 * units through an affine transform (16.16) into DEVICE pixels; a
 * target is a window onto device space (a band of a page is a target
 * whose origin is the band's first row), so the same calls draw any
 * band of any page.
 *
 * Coverage is by pixel centre, no antialiasing: a pixel is inside a
 * shape when its centre is. Shapes that share an edge therefore never
 * both paint, nor both miss, a pixel -- which is what keeps a page
 * rendered in bands identical to the page rendered whole.
 *
 * No dynamic memory. Fills need scratch for their edge table, handed
 * over once with zc_scratch(); a path's storage is the caller's.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef int32_t zc_fix;			// 24.8

#define ZC_FRAC     8
#define ZC_ONE      (1 << ZC_FRAC)
#define ZC_I(i)     ((zc_fix)((i) * ZC_ONE))
#define ZC_HALF     (ZC_ONE / 2)
#define ZC_TOINT(f) ((int)((f) >> ZC_FRAC))		// floor

#define ZC_TONE_WHITE 0
#define ZC_TONE_BLACK 16

// -- targets -----------------------------------------------------------

enum {
	ZC_BITS = 0,		// 1bpp, framebuffer packing: pixel x at bit (x & 31)
						// of word (x >> 5), least significant bit leftmost
	ZC_GRAY = 1,		// 8 bits a pixel, 0 black .. 255 white
	ZC_REC = 2,			// nothing drawn: every call is described to a
						// recorder instead (zpaper.h), for printing
};

typedef struct zc_rec_ops zc_rec_ops_t;

typedef struct {

	uint8_t		fmt;
	// Device pixels per pattern bit and per shade cell. 1 on screen; a
	// printed page uses its scale, so patterns keep their on-screen
	// size on paper instead of turning to a grey fuzz at 300 dpi.
	uint8_t		pat_scale;

	void		*pix;
	int			stride;		// words a row (ZC_BITS), bytes a row (ZC_GRAY)
	int			w, h;		// buffer size, pixels

	// Device coordinates of the buffer's pixel (0,0). A band is a
	// buffer with oy at its first row.
	int			ox, oy;

	// Where patterns and shades are anchored, device coordinates: the
	// page's origin, so a pattern does not shift between bands or
	// when the page is drawn somewhere else.
	int			px, py;

	// Clip, device coordinates, [x0, x1) x [y0, y1). Always inside
	// the buffer.
	int			cx0, cy0, cx1, cy1;

	// ZC_REC: who is told (zc_target_rec())
	const zc_rec_ops_t	*rec;
	void				*rec_ctx;

} zc_target_t;

void zc_target_bits(zc_target_t *t, uint32_t *bits, int wpl, int w, int h);
void zc_target_gray(zc_target_t *t, uint8_t *pix, int stride, int w, int h);
// A recording target of w x h device units: every drawing call goes to
// `ops` as a description (fills, strokes, images, text runs), with the
// clip as it stands, and nothing is rasterized. zpaper.c is the
// recorder; the device units are whatever it says (zpaper.h).
void zc_target_rec(zc_target_t *t, const zc_rec_ops_t *ops, void *ctx, int w, int h);

// Moves a target's window onto device space; the clip becomes the
// whole buffer there.
void zc_target_origin(zc_target_t *t, int ox, int oy);

// Narrows the clip (device coordinates). Intersects with the buffer;
// an empty result draws nothing.
void zc_clip(zc_target_t *t, int x0, int y0, int x1, int y1);
void zc_clip_reset(zc_target_t *t);
static inline bool zc_clip_empty(const zc_target_t *t) {
	return t->cx0 >= t->cx1 || t->cy0 >= t->cy1;
}

// -- paint -------------------------------------------------------------

enum {
	ZC_P_SHADE = 0,		// tone: 0 white .. 16 black
	ZC_P_PATTERN,		// an 8x8 pattern, 1 bits in `tone`, 0 bits white
						// (opaque) or left alone
	ZC_P_XOR,			// inverts what is there (outlines, selections)
};

typedef struct {
	uint8_t		kind;
	uint8_t		tone;
	uint8_t		opaque;
	uint8_t		pat[8];		// rows, MSB = leftmost (zwidget.h's table)
} zc_paint_t;

static inline zc_paint_t zc_shade(int tone) {
	zc_paint_t p = { ZC_P_SHADE, (uint8_t)(tone < 0 ? 0 : tone > 16 ? 16 : tone),
		0, { 0 } };
	return p;
}
#define zc_black() zc_shade(ZC_TONE_BLACK)
#define zc_white() zc_shade(ZC_TONE_WHITE)
zc_paint_t zc_pattern(const uint8_t pat[8], int tone, bool opaque);
static inline zc_paint_t zc_xor(void) {
	zc_paint_t p = { ZC_P_XOR, 0, 0, { 0 } };
	return p;
}

// One run of a row, device coordinates [x0, x1), clipped here. The
// primitive everything else ends in; exported for callers with their
// own geometry.
void zc_span(zc_target_t *t, int y, int x0, int x1, const zc_paint_t *p);

// Rectangle in device pixels, clipped.
void zc_fill_rect_dev(zc_target_t *t, int x, int y, int w, int h,
	const zc_paint_t *p);

// Paints the whole clip.
void zc_clear(zc_target_t *t, const zc_paint_t *p);

// -- transforms --------------------------------------------------------

// x' = a*x + c*y + e,  y' = b*x + d*y + f.  a..d 16.16, e/f 24.8
// device. User coordinates are 24.8.
typedef struct {
	int32_t		a, b, c, d;
	zc_fix		e, f;
} zc_xf_t;

#define ZC_XF_ONE 65536

zc_xf_t zc_xf_identity(void);
// scale (16.16, device pixels per user unit) then offset (device 24.8)
zc_xf_t zc_xf_scale(int32_t s16, zc_fix ox, zc_fix oy);
// m * n: apply n first, then m
zc_xf_t zc_xf_mul(const zc_xf_t *m, const zc_xf_t *n);
// a quarter turn clockwise about the origin, then (ox, oy); used to lay
// a landscape page on portrait paper
zc_xf_t zc_xf_rot90(int32_t s16, zc_fix ox, zc_fix oy);
void zc_xf_apply(const zc_xf_t *m, zc_fix x, zc_fix y, zc_fix *ox, zc_fix *oy);
// how long one user unit is in device pixels, 16.16 (geometric mean
// of the axes: what a stroke width scales by)
int32_t zc_xf_scale_of(const zc_xf_t *m);
// true when the transform is a scale and an offset, nothing turned
bool zc_xf_is_axis(const zc_xf_t *m);

// -- paths -------------------------------------------------------------

typedef struct { zc_fix x, y; } zc_pt_t;

typedef struct {

	zc_pt_t		*pt;		// device coordinates, after the transform
	int			n, cap;
	uint16_t	*sub;		// first point of each subpath
	int			nsub, subcap;

	zc_xf_t		xf;

	// Device bounding box of everything added, valid when n > 0.
	zc_fix		bx0, by0, bx1, by1;

	// Set when a point or subpath did not fit; the path draws what it
	// has, and the caller can tell.
	bool		overflow;

	zc_fix		cur_x, cur_y;	// user coordinates of the pen

} zc_path_t;

void zc_path_init(zc_path_t *p, zc_pt_t *pt, int cap, uint16_t *sub, int subcap);
void zc_path_reset(zc_path_t *p);			// keeps the storage and transform
void zc_path_set_xf(zc_path_t *p, const zc_xf_t *xf);

void zc_path_move(zc_path_t *p, zc_fix x, zc_fix y);
void zc_path_line(zc_path_t *p, zc_fix x, zc_fix y);
// device-space point, no transform (glyph caches, outlines)
void zc_path_line_dev(zc_path_t *p, zc_fix x, zc_fix y);
void zc_path_move_dev(zc_path_t *p, zc_fix x, zc_fix y);
void zc_path_close(zc_path_t *p);
void zc_path_cubic(zc_path_t *p, zc_fix x1, zc_fix y1, zc_fix x2, zc_fix y2,
	zc_fix x3, zc_fix y3);
void zc_path_quad(zc_path_t *p, zc_fix x1, zc_fix y1, zc_fix x2, zc_fix y2);

// Angles in 1/1024 of a turn, 0 = +x, increasing clockwise on screen
// (y down). Adds the arc's points from the current pen; a = a0 .. a1.
void zc_path_arc(zc_path_t *p, zc_fix cx, zc_fix cy, zc_fix rx, zc_fix ry,
	int a0, int a1);

// Closed shapes, each its own subpath, all drawn clockwise on screen
// (so with nonzero winding they add, never cancel).
void zc_path_rect(zc_path_t *p, zc_fix x, zc_fix y, zc_fix w, zc_fix h);
void zc_path_ellipse(zc_path_t *p, zc_fix cx, zc_fix cy, zc_fix rx, zc_fix ry);
void zc_path_rrect(zc_path_t *p, zc_fix x, zc_fix y, zc_fix w, zc_fix h,
	zc_fix r);

// -- drawing -----------------------------------------------------------

enum { ZC_NONZERO = 0, ZC_EVENODD = 1 };

// Scratch for fills: edges and the active list. 16 bytes an edge; a
// card's busiest shape is a glyph run or a stroked polyline. Returns
// the number of edges it can hold. Without scratch nothing fills.
int zc_scratch(void *mem, size_t bytes);

// Fills every subpath (each closed implicitly).
// Returns false if the path had more edges than the scratch holds; the
// shape is then not drawn at all rather than drawn wrong.
bool zc_fill(zc_target_t *t, const zc_path_t *path, int rule,
	const zc_paint_t *p);

// Strokes every subpath with a round pen `width` user units wide (round
// caps and joins: the union of a disc swept along the path). Widths
// under ~1.25 device pixels draw as 1-pixel lines. `closed` joins each
// subpath's last point to its first.
//
// Uses a second path as working space (the capsules), which must not be
// `path`; its own storage is used and its contents replaced.
bool zc_stroke(zc_target_t *t, const zc_path_t *path, zc_fix width,
	bool closed, zc_path_t *work, const zc_paint_t *p);

// Outlines of the closed shapes, drawn INSIDE the shape's box: the band
// from its edge inward by `width` user units, as one even-odd fill of
// the shape and the shape shrunk by `width`. That is QuickDraw's pen,
// and HyperCard's: a frame never sticks out of the box its fill covers,
// a 1-pixel frame on whole coordinates lands on whole pixels, and a
// rectangle keeps square corners. Also much cheaper than zc_stroke().
// `p` is working space (its contents are replaced). A width at least
// half the shape fills it.
bool zc_frame_rect(zc_target_t *t, zc_path_t *p, zc_fix x, zc_fix y, zc_fix w,
	zc_fix h, zc_fix width, const zc_paint_t *paint);
bool zc_frame_ellipse(zc_target_t *t, zc_path_t *p, zc_fix x, zc_fix y,
	zc_fix w, zc_fix h, zc_fix width, const zc_paint_t *paint);
bool zc_frame_rrect(zc_target_t *t, zc_path_t *p, zc_fix x, zc_fix y,
	zc_fix w, zc_fix h, zc_fix r, zc_fix width, const zc_paint_t *paint);

// A 1-pixel line between two device points (24.8), clipped.
void zc_line_dev(zc_target_t *t, zc_fix x0, zc_fix y0, zc_fix x1, zc_fix y1,
	const zc_paint_t *p);

// -- images ------------------------------------------------------------

// A 1bpp source in framebuffer packing (1 = white), drawn into the
// device rectangle (dx, dy, dw, dh), 24.8, nearest pixel. Opaque.
void zc_blit_bits(zc_target_t *t, const uint32_t *src, int swpl, int sw,
	int sh, zc_fix dx, zc_fix dy, zc_fix dw, zc_fix dh);

// The same source used as a MASK: where a source pixel is 0 (black),
// the paint is drawn; where it is 1, nothing. Glyph caches and
// monochrome art with a transparent background.
void zc_blit_mask(zc_target_t *t, const uint32_t *src, int swpl, int sw,
	int sh, zc_fix dx, zc_fix dy, zc_fix dw, zc_fix dh, const zc_paint_t *p);

// An 8-bit grey source. On a grey target it is resampled (bilinear
// when enlarging, box when reducing); on a 1bpp target it is dithered
// with the shade matrix -- a preview, not a photograph (zc_dither()
// makes those).
void zc_blit_gray(zc_target_t *t, const uint8_t *src, int sstride, int sw,
	int sh, zc_fix dx, zc_fix dy, zc_fix dw, zc_fix dh);

// Any source (ZC_IMG_BITS, _MASK, _GRAY; `stride` in words or bytes)
// through a transform `m` from source pixels to device: a quarter turn
// for a card laid sideways on a sheet, or anything else. An unturned
// `m` is the ordinary blit above; a turned one samples each device
// pixel's centre (bilinear for grey, nearest for bits). `p` paints a
// mask.
void zc_blit_xf(zc_target_t *t, int kind, const void *src, int stride, int sw,
	int sh, const zc_xf_t *m, const zc_paint_t *p);

// Resamples a grey image to dw x dh and dithers it (Floyd-Steinberg,
// serpentine) into 1bpp framebuffer packing. This is how an image on a
// card is made: decoded to grey once, scaled to the size it is shown
// at, dithered once. `row` is scratch for two error rows and the
// resampled row: 3 * (dw + 2) * 2 bytes.
//
// `contrast` and `bright` adjust before dithering (0 and 0 leave it):
// contrast 16.16 gain about mid-grey, bright -255..255.
void zc_dither(const uint8_t *src, int sstride, int sw, int sh,
	uint32_t *dst, int dwpl, int dw, int dh, int16_t *row,
	int32_t contrast, int bright);

// Resamples grey to grey (box/bilinear), for print bands and previews.
// `row` is scratch, dw * 4 bytes.
void zc_resample(const uint8_t *src, int sstride, int sw, int sh,
	uint8_t *dst, int dstride, int dw, int dh, uint32_t *row);

// -- recording ---------------------------------------------------------

enum { ZC_IMG_BITS = 0, ZC_IMG_MASK = 1, ZC_IMG_GRAY = 2 };

// What a ZC_REC target is told. Coordinates are device units (24.8),
// after the transform, exactly what the rasterizer would have used;
// the clip is t->cx0..cy1. Any member may be NULL (that kind of call is
// then dropped).
struct zc_rec_ops {
	// a device rectangle [x0, x1) x [y0, y1)
	void	(*rect)(zc_target_t *t, int x0, int y0, int x1, int y1,
				const zc_paint_t *p);
	void	(*fill)(zc_target_t *t, const zc_path_t *path, int rule,
				const zc_paint_t *p);
	// round pen, `width` in device units (0: the thinnest line)
	void	(*stroke)(zc_target_t *t, const zc_path_t *path, zc_fix width,
				bool closed, const zc_paint_t *p);
	// ZC_IMG_*; `stride` in words (bits, mask) or bytes (grey); `p` for
	// a mask
	void	(*image)(zc_target_t *t, int kind, const void *src, int stride,
				int sw, int sh, zc_fix dx, zc_fix dy, zc_fix dw, zc_fix dh,
				const zc_paint_t *p);
	// a run of text: `style` is a zc_style_t (zcfont.h), drawn through
	// `xf` at user (x, y); its underline, if any, follows as a stroke
	void	(*text)(zc_target_t *t, const zc_xf_t *xf, const void *style,
				zc_fix x, zc_fix y, const char *s, uint32_t len,
				const zc_paint_t *p);
};

// -- helpers -----------------------------------------------------------

// 1/1024-turn sine and cosine, Q14 (16384 = 1.0).
int zc_sin(int a);
int zc_cos(int a);
uint32_t zc_isqrt64(uint64_t v);

// Bit of a 1bpp target at device (x, y): 1 white, 0 black; -1 outside.
int zc_get_bit(const zc_target_t *t, int x, int y);

#endif
