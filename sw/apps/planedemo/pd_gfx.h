#ifndef PD_GFX_H
#define PD_GFX_H

/*
 * planedemo -- pictures: drawn on the CPU once, in 8 bits a pixel
 * ("chunky"), turned into bitplanes in RAM ("planar"), then blitted
 * into whichever planes of the screen a scene gives them. See
 * docs/planedemo.md, "Planes as layers".
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */

#include <stdint.h>
#include <stdbool.h>

/* -- chunky: one byte a pixel, for drawing and computing -- */

typedef struct {
	int w, h;
	uint8_t *px;
} pd_chunky_t;

bool pd_chunky_new(pd_chunky_t *c, int w, int h);
void pd_chunky_free(pd_chunky_t *c);
static inline void pdc_set(pd_chunky_t *c, int x, int y, int v) {
	if ((unsigned)x < (unsigned)c->w && (unsigned)y < (unsigned)c->h) c->px[y * c->w + x] = (uint8_t)v;
}
static inline int pdc_get(const pd_chunky_t *c, int x, int y) {
	return ((unsigned)x < (unsigned)c->w && (unsigned)y < (unsigned)c->h) ? c->px[y * c->w + x] : 0;
}
void pdc_fill(pd_chunky_t *c, int x, int y, int w, int h, int v);
void pdc_line(pd_chunky_t *c, int x0, int y0, int x1, int y1, int v);
void pdc_disc(pd_chunky_t *c, int cx, int cy, int r, int v);
/* Text in the 8x8 font, scaled; only the set pixels are written. */
void pdc_text(pd_chunky_t *c, int x, int y, const char *s, int v, int scale);
int pdc_text_w(const char *s, int scale);

/* -- planar: bitplanes in RAM, zgfx's layout (LSB leftmost, rows a
 * multiple of 4 bytes), ready for the blitter -- */

typedef struct {
	int w, h, stride, n;
	uint32_t *pl[8];
	uint32_t *mask;         /* where the picture is opaque, or NULL */
} pd_planar_t;

/* Bits bit0 .. bit0+n-1 of each pixel of `c` as n planes; with
 * `transparent` >= 0, a mask of the pixels that are not that value. */
bool pd_planar_from(pd_planar_t *p, const pd_chunky_t *c, int bit0, int n, int transparent);
void pd_planar_free(pd_planar_t *p);

/* Into screen planes base .. base+n-1, at (x, y), from (sx, sy) in the
 * picture: opaque, or through the mask (pd_put_masked). */
void pd_put(const pd_planar_t *p, int base, int sx, int sy, int x, int y, int w, int h);
void pd_put_masked(const pd_planar_t *p, int base, int sx, int sy, int x, int y, int w, int h);
/* A picture wider than the screen, scrolled: column `scroll` (any
 * value; it wraps at p->w) at the left edge, rows sy.. at y. */
void pd_put_wrap(const pd_planar_t *p, int base, int scroll, int sy, int y, int h);

/* -- a layer put together in RAM (docs/planedemo.md, "Racing the beam") --
 *
 * A sprite drawn over a layer on the screen is gone between the layer
 * being drawn and the sprite going back on: if the beam passes then,
 * it flashes. So the rows a sprite is in are put together here first
 * -- the layer, then the sprite -- and go on the screen in one pass,
 * each pixel written once. A canvas is a pd_planar_t of n planes,
 * full width; pd_put() puts it on the screen. */
bool pd_canvas_new(pd_planar_t *c, int w, int h, int n);
void pd_canvas_clear(pd_planar_t *c);
void pd_canvas_zero(pd_planar_t *c, int k, int y, int h);   /* rows of plane k to 0 */
/* planes 0..p->n-1 of p into the canvas's planes base.., at (x, y), from
 * (sx, sy); through p's mask if `masked` (and it has one), else opaque.
 * Clipped to both. */
void pd_canvas_put(pd_planar_t *c, int base, const pd_planar_t *p, bool masked,
	int sx, int sy, int x, int y, int w, int h);
/* pd_shape() into the canvas */
void pd_canvas_shape(pd_planar_t *c, const uint32_t *bits, int stride, int base, int n, int value,
	int sx, int sy, int x, int y, int w, int h);

/* Planes base .. base+n-1 of a rectangle to 0 (or 1). */
void pd_planes_fill(int base, int n, int x, int y, int w, int h, int bit);

/* A 1bpp bitmap (zgfx layout) into the planes whose bits are set in
 * `value` and out of the others, under its own shape: a shape in one
 * colour of a layer of n planes at base. */
void pd_shape(const uint32_t *bits, int stride, int base, int n, int value,
	int sx, int sy, int x, int y, int w, int h);

/* -- fewer planes (docs/planedemo.md, "On fewer planes") --
 *
 * Scenes draw in eight logical planes and write 256 palette entries
 * as if the board had them all. On a board with 6 or 4, each scene says
 * in begin() which logical planes to keep (pd_keep()); those land, in
 * order, on the physical ones, the others are not drawn at all, and the
 * palette shown is, for each physical index, the entry of the logical
 * index it stands for (pd_logical()). Every drawing call here goes
 * through the map; a scene that draws in planes itself uses pdp_*. */
extern int pd_planes;                   /* 8, 6 or 4: what the board shows */
void pd_keep(unsigned m6, unsigned m4); /* the planes kept with 6 and with 4 */
int pd_phys(int k);                     /* logical plane k's physical one; -1 dropped */
unsigned pd_logical(unsigned j);        /* physical index j's logical one */
void pdp_plane(int k, const void *bits, int stride, int sx, int sy, int x, int y, int w, int h);
void pdp_or(int k, const void *bits, int stride, int sx, int sy, int x, int y, int w, int h);
void pdp_andn(int k, const void *bits, int stride, int sx, int sy, int x, int y, int w, int h);
void pdp_fill(int k, int x, int y, int w, int h, int bit);
void pdp_line(int k, int x0, int y0, int x1, int y1, int op);

/* -- racing the beam (docs/planedemo.md) --
 *
 * With the queue on, every drawing call above is kept, not done, and
 * the main loop runs the frame's at the frame boundary, straight after
 * the palette: pd_queue_prepare(), pd_queue_band() for each band of rows from the top,
 * then pd_queue_done(). The pictures drawn from must stay as they are
 * until then (they do: they live for the film). */
void pd_queue(bool on);            /* off: runs what is queued first */
void pd_queue_prepare(int nbands); /* before the first band: up to 16 */
int pd_queue_bands(void);          /* how many there are (fewer if the lists overflowed) */
void pd_queue_band(int b, int *row0); /* band b's part; *row0 its first row */
void pd_queue_done(void);
void pd_queue_flush(void);         /* all of it now, in order */
int pd_queue_len(void);

/* An ordered-dither threshold, 0..15, for (x, y): a value in 16ths
 * becomes (v + pd_dither(x, y)) >> 4, and a gradient of few colours
 * shows no bands. */
int pd_dither(int x, int y);

/* -- angles: 4096 to a turn, Q14 results -- */

void pd_trig_init(void);
int pd_sin(int a);
int pd_cos(int a);
/* The angle of (x, y), 0..4095, 0 along +x, turning toward +y. */
int pd_atan2(int y, int x);
/* sqrt of a non-negative integer */
uint32_t pd_isqrt(uint32_t v);

/* -- time, for spreading work over frames -- */

/* Something only a little more than "now" in a fine unit; the host's
 * counts calls. */
uint32_t pd_ticks(void);
#define PD_TICKS_PER_FRAME 12      /* about: the board's 732 Hz */

/* -- made on the build host (docs/planedemo.md, "Made before it plays") --
 *
 * Everything a scene computes before it can show it -- pictures, the
 * donut's frames, the tunnel's map, the instruments -- is made on the
 * build host by the same code (tests/mkfilm.c) and linked into the
 * program as the film's data (pd_blob.c). Each part says what it has
 * in a visitor, pd_store_*(), run once to write it there and once on
 * the board to read it back, in the same order: on the board nothing
 * is copied, the pictures point into the data. */
typedef struct {
	bool writing;
	uint8_t *buf;                   /* writing: the data so far */
	uint32_t len, cap;
	const uint8_t *in;              /* reading */
	uint32_t pos, in_len;
	bool bad;
} pd_store_t;

void pd_store_planar(pd_store_t *st, pd_planar_t *p);
void pd_store_bytes(pd_store_t *st, void *data, uint32_t len);
/* a block the reader keeps where it is (no copy): *data points at it */
void pd_store_block(pd_store_t *st, void **data, uint32_t *len);
void pd_store_u32(pd_store_t *st, uint32_t *v);

/* the film's data, linked in (pd_blob.c); NULL if this build has none */
extern const uint8_t *pd_blob;
extern uint32_t pd_blob_len;

typedef bool (*pd_job_fn)(void);  /* one step; true when done */
void pd_job_add(pd_job_fn fn);
/* Steps of the queued jobs for up to `ticks`; true when none is left. */
bool pd_jobs_run(uint32_t ticks);
/* Finish one job now (a scene that needs it). */
void pd_job_finish(pd_job_fn fn);

#endif
