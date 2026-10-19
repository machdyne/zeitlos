#ifndef ZPAPER_H
#define ZPAPER_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zpaper -- pages described, not drawn: what an app hands the print
 * service (sw/apps/print). docs/zpaper.md.
 *
 * -- why a description --
 *
 * An app that prints draws its pages the way it draws its window,
 * through zcanvas, onto a recording target (zc_target_rec()). Nothing
 * is rasterized in the app: each fill, stroke, picture and run of text
 * is written to a job file as it is drawn, in the page's own units.
 * The print service then turns that into whatever the printer wants --
 * a 300 dpi raster for an inkjet, PostScript for a laser, a PDF for a
 * file -- and lays the pages on sheets (fitted, n-up, turned, cut
 * marks), because only it knows the paper and the printer's margins.
 *
 * So an app pays for printing with a few KB (this file's writer), the
 * job stays small (vectors, text as text, pictures by their path), and
 * every app prints through one renderer that is the screen's own.
 *
 * -- the file --
 *
 *   "ZPP1", u32 version
 *   records: u8 op, u8 0, u16 0, u32 len, then len bytes, padded to 4
 *
 * little-endian throughout. A JOB record is one "key=value" option;
 * PAGE starts a page and END ends it; between them, drawing records in
 * DEVICE units: page units times ZP_SUB, 24.8 fixed point. A reader
 * skips what it does not know.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zcanvas.h"
#include "zcfont.h"
#include "zpage.h"

#define ZP_MAGIC	"ZPP1"
#define ZP_VERSION	1

// Device units per page unit while recording: curves are flattened at
// this resolution, which is finer than any printer gets them (a slide
// unit is 6.4 dots across Letter at 300 dpi).
#define ZP_SUB		16

enum {
	ZP_JOB = 1,			// "key=value"
	ZP_PAGE = 2,		// zp_page_t
	ZP_END = 3,			// end of a page
	ZP_CLIP = 4,		// the clip for what follows
	ZP_RECT = 5,
	ZP_FILL = 6,
	ZP_STROKE = 7,
	ZP_IMAGE = 8,		// pixels in the file
	ZP_IMAGE_FILE = 9,	// a picture by its path
	ZP_TEXT = 10,
};

#define ZP_NAME_MAX		16
#define ZP_FONT_MAX		20
#define ZP_PATH_MAX		200

typedef struct {
	uint16_t	gw, gh;			// page units
	uint8_t		kind;			// ZPAGE_PAPER or ZPAGE_RATIO
	uint8_t		landscape;
	uint16_t	w_mm10, h_mm10;	// the paper as oriented; 0 for a ratio
	char		name[ZP_NAME_MAX];	// zpage name: "letter", "16:9" ...
} zp_page_t;

// -- writing (the app) ---------------------------------------------------

typedef bool (*zp_write_fn)(void *ctx, const void *data, uint32_t len);

typedef struct {
	zp_write_fn	write;
	void		*ctx;
	bool		failed;			// a write failed: the job is no good
	bool		in_page;
	zc_target_t	t;
	int			clip[4];		// last clip written
	bool		clip_sent;
	uint32_t	pages;
} zp_writer_t;

void zp_writer_init(zp_writer_t *w, zp_write_fn write, void *ctx);

// A job option, before the first page: "title", "copies", "nup" ...
// (docs/print.md, "Job options").
bool zp_job(zp_writer_t *w, const char *key, const char *value);

// Starts a page shaped like `pg`. Returns the target to draw it on and,
// in *xf, the transform from page units to that target -- the drawing
// code is the screen's, with this target and transform.
zc_target_t *zp_page_begin(zp_writer_t *w, const zpage_t *pg, zc_xf_t *xf);
bool zp_page_end(zp_writer_t *w);

// A picture by its file, decoded by the print service at the size it
// prints: a photo travels as its path, not as megabytes of grey. `path`
// is absolute; contrast and bright -100..100 as slide keeps them. On
// any target but a recording one, nothing happens.
void zp_image_file(zc_target_t *t, const char *path, int contrast, int bright,
	zc_fix dx, zc_fix dy, zc_fix dw, zc_fix dh);

// -- reading (the print service) ------------------------------------------

typedef struct {
	uint8_t			op;
	uint32_t		len;
	const uint8_t	*p;
} zp_rec_t;

typedef struct {
	const uint8_t	*data;
	uint32_t		len, pos;
	bool			bad;		// a record ran past the end
} zp_reader_t;

// False if it is not a zpaper file this reads.
bool zp_open(zp_reader_t *r, const void *data, uint32_t len);
// The next record; false at the end (r->bad says whether that was the
// end of the file or a broken one).
bool zp_next(zp_reader_t *r, zp_rec_t *rec);

// The value of job option `key` ("" if not set); false if not set.
bool zp_job_get(const void *data, uint32_t len, const char *key, char *out, int outlen);

// Pages: where each starts (the PAGE record) and how long it is up to
// and including its END. Returns how many there are (all of them, even
// past `max`).
int zp_pages(const void *data, uint32_t len, uint32_t *start, uint32_t *bytes, int max);

bool zp_page_info(const zp_rec_t *rec, zp_page_t *out);
// The same page as zpage.h has it.
void zp_page_zpage(const zp_page_t *pi, zpage_t *pg);

// One drawing record, decoded. Pointers are into the file.
typedef struct {
	uint8_t			op;
	zc_paint_t		paint;
	int				clip[4];		// ZP_CLIP

	zc_fix			box[4];			// device box of what it draws (culling)

	// ZP_RECT: box; ZP_FILL / ZP_STROKE
	uint8_t			rule;			// ZC_NONZERO / ZC_EVENODD
	uint8_t			closed;
	zc_fix			width;
	int				nsub, npts;
	const uint8_t	*sub;			// u16 each
	const uint8_t	*pts;			// x, y 24.8 each

	// ZP_IMAGE / ZP_IMAGE_FILE
	uint8_t			kind;			// ZC_IMG_*
	int				sw, sh, rowbytes;
	const uint8_t	*pix;
	zc_fix			dx, dy, dw, dh;
	char			path[ZP_PATH_MAX];
	int				contrast, bright;

	// ZP_TEXT
	zc_xf_t			xf;
	zc_fix			x, y, size, weight, track;
	uint8_t			bold, italic;
	char			font[ZP_FONT_MAX];
	const char		*text;
	uint32_t		tlen;
} zp_op_t;

bool zp_decode(const zp_rec_t *rec, zp_op_t *op);
void zp_point(const zp_op_t *op, int i, zc_fix *x, zc_fix *y);
int zp_sub_at(const zp_op_t *op, int i);

// -- playing onto a raster ----------------------------------------------------

typedef struct {
	// A font by name (as recorded: "serifb", "m9x15", "mono6x12"), or
	// false; a run whose font is missing is drawn in `fallback`.
	bool	(*font)(void *ctx, const char *name, const zc_font_t **out);
	// A picture by path, decoded to grey no larger than maxw x maxh
	// (its printed size), contrast and bright applied. Called once per
	// picture per page and band, so it keeps what it decoded.
	bool	(*image)(void *ctx, const char *path, int contrast, int bright,
				int maxw, int maxh, const uint8_t **pix, int *w, int *h);
	void	*ctx;
	const zc_font_t	*fallback;
	// working paths, the caller's storage (sized for the largest path
	// in the job: zp_max_points())
	zc_path_t	*path, *work;
	// what could not be drawn (a path too long, a picture that would not
	// decode): counted, not fatal
	uint32_t	skipped;
} zp_play_t;

// The most points any path in the job has.
int zp_max_points(const void *data, uint32_t len);

// Draws one page (its bytes from zp_pages()) onto t through xf, page
// units -> device pixels; patterns are anchored at the page's origin,
// one pattern bit a page unit. t's clip on entry bounds everything
// (the page's cell, a band).
void zp_play_page(zp_play_t *pl, const uint8_t *page, uint32_t len,
	zc_target_t *t, const zc_xf_t *xf);

#endif
