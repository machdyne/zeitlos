#ifndef ZSVG_H
#define ZSVG_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * SVG rendering, for a 1bpp display.
 *
 * -- why this is worth having on this machine --
 *
 * A photograph dithered to one bit is mush. A DIAGRAM is nearly
 * lossless: line art, logos, charts and maps survive the trip to
 * monochrome almost perfectly, because they were already
 * shape-and-edge rather than tone. SVG is what the web uses for
 * exactly that material -- Wikipedia's diagrams, most site logos --
 * and it arrives as a few kilobytes of text rather than a few hundred
 * of pixels.
 *
 * It also scales. A 24-pixel logo and a full-screen diagram come from
 * the same file, which matters when the display is 640x480 and the
 * source was drawn for something else.
 *
 * -- how it draws --
 *
 * Filled shapes go through a SCANLINE rasterizer: the geometry is
 * flattened to edges, the edges are sorted, and each raster line is
 * filled as a set of horizontal spans. That maps directly onto
 * zgfx.h's z_fb_hw_span_begin()/z_fb_hw_span(), which is a hardware
 * span filler whose own header describes it as "the inner loop of
 * something that has already clipped". This is that something.
 *
 * Strokes are line segments and go to z_fb_hw_line().
 *
 * The renderer never touches the framebuffer itself. It calls back
 * with spans and lines, so the same code serves sw/apps/view,
 * sw/apps/web, an off-device test that writes a PBM, and a future
 * drawing app that wants the geometry rather than the pixels.
 *
 * -- what it is not --
 *
 * No CSS, no scripting, no animation, no filters, no gradients, no
 * text. Text is the significant omission and it is deliberate: laying
 * out a <text> element properly needs font metrics, and every
 * substitute produces a diagram whose labels are wrong rather than
 * missing. Files that need it are reported, not half-drawn.
 */

// -- geometry -------------------------------------------------------

// A 2x3 affine transform, row-major:  x' = a*x + c*y + e
//                                     y' = b*x + d*y + f
typedef struct {
	float a, b, c, d, e, f;
} z_svg_xf_t;

// One edge of a flattened outline, in device pixels.
//
// The caller owns the array. It is the single largest allocation this
// renderer needs and its size is a policy question -- how complex a
// drawing is worth rendering -- which belongs to the app, not here.
// The same argument as zinflate.h's window.
typedef struct {
	float		x0, y0, x1, y1;
	int8_t		dir;			// +1 downward, -1 upward, for winding
} z_svg_edge_t;

// -- output ---------------------------------------------------------
//
// The renderer produces spans and lines; the caller puts them on a
// screen, in a file, or in a display list.

typedef struct {

	// A filled run: y, from x0 to x1 inclusive, in device pixels,
	// already clipped to the target rectangle.
	//
	// `level` is 0..Z_SVG_LEVELS-1, from the fill colour's luminance:
	// 0 is white and does not need drawing, the top value is solid
	// ink. zgfx.h's z_fb_hw_span_begin() takes exactly such a level,
	// so a shaded span costs no more than a solid one.
	//
	// Without this every fill is ink, and a coloured illustration --
	// as opposed to line art -- comes out as one solid silhouette.
	void (*span)(void *user, int y, int x0, int x1, int level);

	// A stroked segment. May be called with the same point twice for
	// a degenerate segment; the callback should tolerate it.
	void (*line)(void *user, int x0, int y0, int x1, int y1);

	void *user;

} z_svg_sink_t;

// -- rendering ------------------------------------------------------

// Shading levels, matching zgfx.h's shaded fills.
#define Z_SVG_LEVELS  5

#define Z_SVG_OK             0
#define Z_SVG_E_FORMAT      -1	// not SVG, or malformed
#define Z_SVG_E_TOOCOMPLEX  -2	// more edges than the caller allowed
#define Z_SVG_E_UNSUPPORTED -3	// a feature this renderer does not have
#define Z_SVG_E_IO          -4

typedef struct {

	// The document, as text. SVG files are small -- kilobytes -- so
	// unlike the raster decoders this one takes the whole thing.
	// Streaming would buy nothing: a transform on a <g> applies to
	// children that appear later, so the tree has to be walked
	// anyway.
	const char		*doc;
	uint32_t		len;

	// Where to draw, in device pixels. The document's viewBox is
	// fitted into this, preserving aspect ratio and centring.
	int				x, y, w, h;

	z_svg_edge_t	*edges;
	int				max_edges;

	z_svg_sink_t	sink;

	// Filled in by z_svg_render().
	float			src_w, src_h;	// the document's own dimensions
	int				n_edges;		// peak edges used, for tuning
	int				n_shapes;

} z_svg_t;

int z_svg_render(z_svg_t *s);

// Renders into a 1bpp bitmap in FRAMEBUFFER packing -- pixel x at bit
// (x & 31) of word (x >> 5), least significant bit leftmost, the same
// layout zimg.c produces and z_fb_hw_blit_mem() consumes.
//
// Here rather than in each app because every caller so far wants
// exactly this: view and web both render into an off-screen bitmap so
// the result can be scrolled and blitted, and a drawing app would do
// the same before compositing. Writing the sink three times would be
// three chances to get the bit order wrong -- which is a mistake that
// looks like a broken rasterizer.
//
// `s->x` and `s->y` are the offset within the bitmap; `s->w`/`s->h`
// the area to fit into. The caller clears the bitmap first if it
// wants a clean background.
int z_svg_render_bitmap(z_svg_t *s, uint32_t *bits, int wpl);

// True if the buffer looks like SVG. Content, not filename: sites
// serve `.svg` as XML and XML as `.svg`.
bool z_svg_sniff(const char *buf, uint32_t n);

const char *z_svg_strerror(int rv);

#endif
