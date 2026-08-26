/*
 * draw -- a monochrome painting program
 *
 * Replaces the pre-window version of this app entirely. That one drew
 * straight to the framebuffer at a hardcoded 512x384, polled the mouse
 * registers itself, and had no idea the window manager existed; none
 * of it survives here beyond the idea.
 *
 * The reference is early MacPaint, which is the right model for a 1bpp
 * display: there is no colour, so texture does the work colour
 * normally would, and the pattern palette is therefore not a
 * decorative extra but the main way of telling one filled area from
 * another.
 *
 *   > run wm
 *   > run draw
 *
 * -- layout --
 *
 *   +----+---------------------------+
 *   | to |                           |
 *   | ol |          canvas           |
 *   | s  |                           |
 *   +----+---------------------------+
 *   |       pattern palette          |
 *   +--------------------------------+
 *
 * Tools down the left in a 2-wide grid, patterns across the bottom,
 * canvas gets the rest. Recomputed on every resize -- see layout().
 *
 * -- the canvas is a document, not a viewport --
 *
 * The drawing lives in a fixed CANVAS_W x CANVAS_H 1bpp buffer in
 * ordinary memory -- the full 640x480 of the framebuffer, see
 * CANVAS_W's own comment -- and the window shows part of it. It is NOT
 * stored in the framebuffer.
 *
 * This is the most important structural decision in the app, and it is
 * forced: an app must redraw its own content whenever the wm says so
 * (Z_WM_REDRAW -- window moved, uncovered, resized, focus changed), and
 * the wm's repair clears the region first. A paint program whose only
 * copy of the drawing is the pixels on screen loses the entire drawing
 * the first time anything overlaps it. So there has to be a backing
 * store -- and once there is one, making it a fixed-size document
 * rather than a window-sized bitmap also makes resizing trivial:
 * growing the window reveals more of the document, instead of forcing a
 * decision about what happens to the existing pixels.
 *
 * Getting the document onto the screen is one blitter operation --
 * z_fb_hw_blit_mem() (zgfx.h), the hardware's memory-copy mode. That
 * mode exists because of this app: the blitter could previously only
 * read from VRAM, and there is no spare VRAM to keep a document in
 * (rtl/mem/vram.v is exactly 640*480/32 words, with nothing left over),
 * so the document has to live in main memory and the blitter has to be
 * able to reach it. See docs/gpu_blitter.md, "Copy modes".
 *
 * A software word-at-a-time fallback is kept for bitstreams whose
 * blitter predates that mode -- see canvas_blit(). It is not dead code:
 * the same app binary runs on both, and the difference is a silent
 * no-op rather than an error, which is exactly the sort of thing that
 * gets misdiagnosed as an app bug.
 *
 * Everything that is NOT the canvas -- widget bodies, palette swatches,
 * frames, rubber-band previews -- does go through the GPU, via
 * zwidget.c and zgfx.h.
 *
 * -- not implemented yet --
 *
 * No new/save/load: the titlebar buttons for those are the intended
 * next step, and want a wm-side titlebar-icon API more general than
 * today's single close icon. No scrolling -- the window shows the
 * document's top-left corner, and since the document is now the full
 * size of the screen, every window is smaller than it and the bottom
 * and right of the document are currently unreachable. No undo. No
 * flood fill -- see the note
 * above tool_icons[] for why the one that was here got removed and what
 * bringing it back would need.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zgfx.h"
#include "../../common/zfont.h"
#include "../../common/zwidget.h"
#include "draw_icons.h"

#define VRAM        ((volatile uint32_t *)0x20000000)
#define VRAM_WPL    (Z_SCREEN_W / 32)	// framebuffer words per line

// -- the document --

// Sized to the full framebuffer, deliberately.
//
// It was 512x384 at first, which is what this system's framebuffer used
// to be and what the original MacPaint page size evokes. The
// framebuffer is 640x480 now (rtl/mem/vram.v, Z_SCREEN_W/H in zgfx.h),
// and a document smaller than the screen means a maximised window has
// dead space along two edges that can never be drawn on. Matching the
// framebuffer means the document is always at least as large as any
// viewport that can exist -- the window frame, titlebar, tool column
// and palette all take space, so the visible area is strictly smaller
// than 640x480 no matter how big the window gets.
//
// Cost is 640*480/8 = 37.5K of .bss. That is the app's whole memory
// footprint of consequence and the reason its Makefile emits a .zexe
// rather than a --pad-to binary (see sw/common/zexec.h): the older
// format would write all 37.5K out as literal zeros to be read off the
// SD card on every launch.
//
// CANVAS_WORDS works out to 20, which is also the framebuffer's own
// words-per-line. That is not relied upon anywhere -- the blit passes
// an explicit stride -- but it does mean a full-document blit needs no
// bit shifting at all when the viewport happens to land on a word
// boundary.
#define CANVAS_W      Z_SCREEN_W
#define CANVAS_H      Z_SCREEN_H
#define CANVAS_WORDS  (CANVAS_W / 32)

// Bit order matches the framebuffer exactly: pixel x lives at bit
// (x & 31) of word (x >> 5), least significant bit LEFTMOST. That is
// z_fb_set_pixel()'s convention (zgfx.c), and matching it is what lets
// canvas_blit() move whole words at a time instead of reformatting
// every pixel on the way to the screen.
static uint32_t canvas[CANVAS_H][CANVAS_WORDS];

// Whether this bitstream's blitter can read main memory. Probed once at
// startup (z_fb_hw_blit_mem_available(), zgfx.h) rather than per call,
// and kept as a plain flag so the fallback below reads as a deliberate
// alternative rather than an error path.
static bool use_hw_blit;

// -- window --

// Created at exactly the size we want as the MINIMUM, because
// Z_WIN_FLAG_MIN_IS_CREATE (zwm.h) ties the two together: whatever we
// ask for here becomes the floor the user can't shrink past. Chosen so
// the full pattern palette fits across the bottom without clipping,
// which is the binding constraint -- Z_PATTERN_COUNT swatches at
// SWATCH px each, plus the frame.
#define WIN_W  340
#define WIN_H  280

static z_win_t win;

// -- layout, recomputed on every resize --

#define TOOL_BTN    20
#define TOOL_COLS    2
#define TOOLS_W     (TOOL_COLS * TOOL_BTN + 2)
#define SWATCH      16
#define PALETTE_H   (SWATCH + 2)

// canvas viewport, content-relative
static int view_x, view_y, view_w, view_h;

// -- widgets --

// Seven, not eight: the 2-wide grid therefore has one empty slot in its
// last row. See the flood-fill note above tool_icons[] for why the
// bucket is gone.
#define TOOL_COUNT   7
#define GROUP_TOOL   1
#define GROUP_PAT    2

// Tools first in widgets[], then swatches, so a widget index maps to a
// tool id or a pattern id by subtraction. Keeping both panels in ONE
// array means one hit test and one dirty-tracking pass covers them
// both, rather than two of everything.
#define W_TOOL0      0
#define W_PAT0       TOOL_COUNT
#define WIDGET_COUNT (TOOL_COUNT + Z_PATTERN_COUNT)

typedef enum {
	TOOL_PENCIL = 0,
	TOOL_BRUSH,
	TOOL_ERASER,
	TOOL_LINE,
	TOOL_RECT,
	TOOL_RECTFILL,
	TOOL_OVAL,
} tool_t;

static z_widget_t widgets[WIDGET_COUNT];
static z_widget_set_t wset;

// No flood fill / paint bucket.
//
// It was here and it was removed, because a pattern fill genuinely
// needs a visited mask -- the usual "filled pixels no longer match the
// target colour" shortcut breaks when roughly half the pixels of a 50%
// pattern get written WITH the target colour -- and a bounded seed
// stack on top of that. On anything but a simple region the stack ran
// out, leaving the fill half done, and the two-pass sweep over the
// whole document that followed took long enough that wm gave up waiting
// for the redraw ack ("timed out waiting for pid N to ack a redraw").
// A tool that regularly half-works and stalls the window manager is
// worse than no tool.
//
// Bringing it back wants a different approach, not a bigger stack:
// span-based filling that pushes runs rather than points, a fill
// bounded to a computed region rather than sweeping the full document,
// and yielding to the message loop mid-fill so the redraw ack isn't
// blocked. draw_icon_bucket (draw_icons.h) is deliberately left in
// place for that day; --gc-sections drops it meanwhile.
static const uint8_t *tool_icons[TOOL_COUNT] = {
	draw_icon_pencil, draw_icon_brush, draw_icon_eraser, draw_icon_line,
	draw_icon_rect, draw_icon_rectfill, draw_icon_oval,
};

static tool_t cur_tool = TOOL_PENCIL;
static int cur_pattern = 1;	// index into z_pattern_table -- 1 is solid black

// -- stroke state --

static bool drawing = false;
// document coordinates of the stroke's origin (shape tools) and of the
// previous sample (freehand tools)
static int stroke_x0, stroke_y0, stroke_px, stroke_py;

// bounding box of the rubber-band preview currently on screen, in
// document coordinates. Tracked so erasing the preview only re-blits
// the region it actually covered -- re-blitting the whole viewport per
// mouse sample flickers badly and is most of the cost of a drag.
static bool prev_valid = false;
static int prev_x0, prev_y0, prev_x1, prev_y1;

// The pencil draws black normally, but white when the stroke STARTED on
// a black pixel -- the original MacPaint behaviour, and the only way to
// erase a single pixel without switching tools. Latched at press time
// so the whole stroke is one colour rather than flickering per pixel as
// it crosses existing artwork.
static int pencil_ink = 1;

// -- brush shapes, MSB-first rows --

static const uint8_t brush_mask[8] = {
	0x3C, 0x7E, 0xFF, 0xFF, 0xFF, 0xFF, 0x7E, 0x3C
};
#define BRUSH_SIZE  8
#define ERASER_SIZE 12

// ---------------------------------------------------------------
// canvas primitives -- document coordinates, all software
// ---------------------------------------------------------------

static inline void canvas_pset(int x, int y, int color) {
	if ((unsigned)x >= CANVAS_W || (unsigned)y >= CANVAS_H) return;
	uint32_t m = 1u << (x & 31);
	if (color) canvas[y][x >> 5] |= m;
	else canvas[y][x >> 5] &= ~m;
}

static inline int canvas_pget(int x, int y) {
	if ((unsigned)x >= CANVAS_W || (unsigned)y >= CANVAS_H) return 0;
	return (int)((canvas[y][x >> 5] >> (x & 31)) & 1u);
}

// Pattern bit for a document pixel, anchored to the DOCUMENT's 8x8
// grid rather than to the shape being drawn. That means two shapes
// filled with the same pattern line up seamlessly where they meet, and
// a shape doesn't change appearance depending on where it was drawn --
// the same anchoring rule z_fb_hw_fill_pattern() follows on the screen
// side, for the same reason.
static inline int pat_bit(const uint8_t *pat, int x, int y) {
	return (pat[y & 7] >> (7 - (x & 7))) & 1;
}

static inline void canvas_pset_pat(int x, int y, const uint8_t *pat) {
	canvas_pset(x, y, pat_bit(pat, x, y));
}

static void canvas_clear(void) {
	memset(canvas, 0, sizeof(canvas));
}

static void canvas_fill_rect(int x, int y, int w, int h, const uint8_t *pat) {
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++)
			canvas_pset_pat(x + i, y + j, pat);
}

static void canvas_rect(int x0, int y0, int x1, int y1, int color) {

	if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
	if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }

	for (int x = x0; x <= x1; x++) {
		canvas_pset(x, y0, color);
		canvas_pset(x, y1, color);
	}
	for (int y = y0; y <= y1; y++) {
		canvas_pset(x0, y, color);
		canvas_pset(x1, y, color);
	}

}

// Bresenham. `stamp` is called per point rather than setting a pixel
// directly, so one walk serves the 1px line tool, the brush and the
// eraser -- the three differ only in what they leave behind at each
// step, not in how they get from A to B.
static void canvas_walk_line(int x0, int y0, int x1, int y1,
	void (*stamp)(int, int)) {

	int dx = x1 - x0, dy = y1 - y0;
	int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
	if (dx < 0) dx = -dx;
	if (dy < 0) dy = -dy;

	int err = dx - dy;

	for (;;) {
		stamp(x0, y0);
		if (x0 == x1 && y0 == y1) break;
		int e2 = err * 2;
		if (e2 > -dy) { err -= dy; x0 += sx; }
		if (e2 < dx) { err += dx; y0 += sy; }
	}

}

// Midpoint ellipse, inscribed in the given rect. Outline only -- a
// filled-oval tool would be a fourth shape tool, and the palette is
// already the more useful way to get a filled shape.
static void canvas_oval(int x0, int y0, int x1, int y1, int color) {

	if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
	if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }

	int a = (x1 - x0) / 2, b = (y1 - y0) / 2;
	int cx = x0 + a, cy = y0 + b;

	if (a <= 0 || b <= 0) {
		// Degenerate -- a zero-width or zero-height drag. Draw the
		// straight line it geometrically describes rather than
		// nothing, which is what the user just asked for.
		canvas_rect(x0, y0, x1, y1, color);
		return;
	}

	int a2 = a * a, b2 = b * b;

	// region 1
	int x = 0, y = b;
	int sigma = 2 * b2 + a2 * (1 - 2 * b);

	while (b2 * x <= a2 * y) {
		canvas_pset(cx + x, cy + y, color);
		canvas_pset(cx - x, cy + y, color);
		canvas_pset(cx + x, cy - y, color);
		canvas_pset(cx - x, cy - y, color);
		if (sigma >= 0) { sigma += 4 * a2 * (1 - y); y--; }
		sigma += b2 * (4 * x + 6);
		x++;
	}

	// region 2
	x = a; y = 0;
	sigma = 2 * a2 + b2 * (1 - 2 * a);

	while (a2 * y <= b2 * x) {
		canvas_pset(cx + x, cy + y, color);
		canvas_pset(cx - x, cy + y, color);
		canvas_pset(cx + x, cy - y, color);
		canvas_pset(cx - x, cy - y, color);
		if (sigma >= 0) { sigma += 4 * b2 * (1 - x); x--; }
		sigma += a2 * (4 * y + 6);
		y++;
	}

}

// ---------------------------------------------------------------
// canvas -> screen
// ---------------------------------------------------------------

static uint32_t canvas_word_at(int row, int wi) {
	if (wi < 0 || wi >= CANVAS_WORDS) return 0;
	return canvas[row][wi];
}

// Fetches the 32 document bits starting at bit position `bitpos` of
// `row`, in framebuffer bit order.
//
// bitpos may be negative: the document can be positioned so that a
// screen word's leftmost pixel maps to a document pixel left of the
// origin. Both `>> 5` and `& 31` do the right thing there on this
// toolchain -- the arithmetic shift floors, and the mask yields a
// non-negative remainder -- which is what lets one expression cover
// both signs rather than needing a separate case.
static uint32_t canvas_fetch32(int row, int bitpos) {

	int wi = bitpos >> 5;
	int b = bitpos & 31;

	uint32_t lo = canvas_word_at(row, wi);

	// Not a fast path -- required. Shifting a 32-bit value by 32 is
	// undefined, and that is exactly what (32 - b) is when b is 0.
	if (!b) return lo;

	uint32_t hi = canvas_word_at(row, wi + 1);
	return (lo >> b) | (hi << (32 - b));

}

// Copies a document rect to the screen, whole words at a time.
//
// dx/dy are document coordinates, sx/sy absolute screen coordinates.
// The rect is clipped to `clip` (the viewport) BEFORE anything is
// written: the framebuffer access below is a raw read-modify-write with
// no clipping of its own, so getting this wrong paints over other
// windows rather than merely looking odd.
static void canvas_blit(int dx, int dy, int w, int h, int sx, int sy,
	const z_clip_t *clip) {

	// Clip against the viewport, moving the document origin in step so
	// the two stay aligned. Done here rather than left to the hardware
	// because the blitter clips to the SCREEN, not to an arbitrary rect
	// -- it has no idea this window has a tool column and a palette that
	// the canvas must not spill over.
	if (sx < clip->x0) { int d = clip->x0 - sx; sx += d; dx += d; w -= d; }
	if (sy < clip->y0) { int d = clip->y0 - sy; sy += d; dy += d; h -= d; }
	if (sx + w - 1 > clip->x1) w = clip->x1 - sx + 1;
	if (sy + h - 1 > clip->y1) h = clip->y1 - sy + 1;
	if (w <= 0 || h <= 0) return;

	// Clamp to the document as well. The viewport can be larger than the
	// document, and the hardware has no notion of the document's extent
	// -- it would happily read past the end of the canvas array. The
	// software path below tolerates that (canvas_word_at() returns zero
	// out of range); the blitter does not.
	if (dx < 0) { sx -= dx; w += dx; dx = 0; }
	if (dy < 0) { sy -= dy; h += dy; dy = 0; }
	if (dx + w > CANVAS_W) w = CANVAS_W - dx;
	if (dy + h > CANVAS_H) h = CANVAS_H - dy;
	if (w <= 0 || h <= 0) return;

	if (use_hw_blit &&
		z_fb_hw_blit_mem(canvas, CANVAS_WORDS * 4, dx, dy, sx, sy, w, h))
		return;

	int shift = dx - sx;	// document x corresponding to screen x = 0

	for (int j = 0; j < h; j++) {

		int row = dy + j;
		int scr_y = sy + j;

		if ((unsigned)scr_y >= Z_SCREEN_H) continue;

		volatile uint32_t *dst = &VRAM[scr_y * VRAM_WPL];

		int wa = sx >> 5;
		int wb = (sx + w - 1) >> 5;

		// Off the top or bottom of the document reads as blank rather
		// than being skipped -- the viewport can be taller than the
		// document, and leaving those rows untouched would show
		// whatever the wm's repair happened to leave there.
		bool row_ok = (row >= 0 && row < CANVAS_H);

		for (int wi = wa; wi <= wb; wi++) {

			uint32_t src = row_ok ? canvas_fetch32(row, wi * 32 + shift) : 0;

			uint32_t mask = 0xFFFFFFFFu;
			if (wi == wa) mask &= 0xFFFFFFFFu << (sx & 31);
			if (wi == wb) {
				int e = (sx + w - 1) & 31;
				// same shift-by-32 hazard as canvas_fetch32()
				if (e != 31) mask &= ~(0xFFFFFFFFu << (e + 1));
			}

			dst[wi] = (dst[wi] & ~mask) | (src & mask);

		}

	}

}

// ---------------------------------------------------------------
// layout and painting
// ---------------------------------------------------------------

// Absolute screen rect of the canvas viewport -- what canvas_blit() and
// the rubber-band preview both clip to. Deliberately NOT the window's
// whole content rect: the tools and palette live in that too, and a
// preview line allowed to run across them would read as a bug.
static void view_clip(z_clip_t *out) {

	z_clip_t content;
	z_win_content_rect(&win, &content);

	out->x0 = content.x0 + view_x;
	out->y0 = content.y0 + view_y;
	out->x1 = out->x0 + view_w - 1;
	out->y1 = out->y0 + view_h - 1;

	// The window can be small enough that the viewport is empty --
	// clamp rather than letting an inverted rect through, since
	// everything downstream treats these bounds as inclusive and would
	// happily draw an enormous region for x1 < x0.
	if (out->x1 > content.x1) out->x1 = content.x1;
	if (out->y1 > content.y1) out->y1 = content.y1;

}

// Recomputes every content-relative rectangle from the window's current
// size. Called once at startup and again after every resize -- there is
// no other copy of these numbers anywhere, which is the point.
static void layout(void) {

	int cw = z_win_content_w(&win);
	int ch = z_win_content_h(&win);

	view_x = TOOLS_W + 1;
	view_y = 0;
	view_w = cw - view_x;
	view_h = ch - PALETTE_H - 1;

	if (view_w < 0) view_w = 0;
	if (view_h < 0) view_h = 0;

	for (int i = 0; i < TOOL_COUNT; i++) {
		widgets[W_TOOL0 + i].x = (int16_t)((i % TOOL_COLS) * TOOL_BTN);
		widgets[W_TOOL0 + i].y = (int16_t)((i / TOOL_COLS) * TOOL_BTN);
	}

	for (int i = 0; i < Z_PATTERN_COUNT; i++) {
		widgets[W_PAT0 + i].x = (int16_t)(i * SWATCH);
		widgets[W_PAT0 + i].y = (int16_t)(ch - PALETTE_H + 1);
	}

	z_widget_invalidate(&wset);

}

// Fills a content-relative rectangle, clipped to the content area.
//
// z_fb_hw_fill_rect() clamps to the SCREEN, not to this window, so it
// cannot be handed a content-relative rect directly -- doing so would
// paint over other windows the moment any of our furniture extended
// past our own edge. This converts and clamps first.
static void fill_content_rect(int cx, int cy, int w, int h, int color) {

	z_clip_t c;
	z_win_content_rect(&win, &c);

	int x0 = c.x0 + cx, y0 = c.y0 + cy;
	int x1 = x0 + w - 1, y1 = y0 + h - 1;

	if (x0 < c.x0) x0 = c.x0;
	if (y0 < c.y0) y0 = c.y0;
	if (x1 > c.x1) x1 = c.x1;
	if (y1 > c.y1) y1 = c.y1;
	if (x1 < x0 || y1 < y0) return;

	z_fb_hw_fill_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, color);

}

// Blanks everything repaint() does not subsequently draw over.
//
// This is not belt-and-braces, it is required, and the reason is worth
// stating because it is easy to assume otherwise: wm clears a region
// before asking for a redraw in MOST cases, but not after a window
// move. repair_drag() (sw/apps/wm/wm.c) deliberately repairs only the
// strips the window swept THROUGH, excluding the window's own final
// footprint -- the border there is already correct and reclearing it
// would flash. So on a move, whatever this app last drew inside its own
// footprint is still on screen, at the old position relative to the
// window, and only the pixels we actively rewrite get corrected.
//
// The canvas hides this (the blit rewrites every pixel of it) and so do
// the widgets (each fills its own body). What is left over is precisely
// the gaps: the tool column below the last button, the palette strip
// past the last swatch, the pixel between adjacent widgets, and any
// part of the viewport beyond the edge of the document. Those kept
// pre-move artefacts.
static void clear_panels(void) {

	int cw = z_win_content_w(&win);
	int ch = z_win_content_h(&win);

	// tool column, full height above the palette
	fill_content_rect(0, 0, TOOLS_W, ch - PALETTE_H, 0);

	// palette strip, full width below its rule
	fill_content_rect(0, ch - PALETTE_H + 1, cw, PALETTE_H - 1, 0);

	// viewport area the document doesn't reach. The window can be
	// larger than the document in either axis, and canvas_blit() clamps
	// to the document rather than painting blank space itself.
	if (view_w > CANVAS_W)
		fill_content_rect(view_x + CANVAS_W, view_y,
			view_w - CANVAS_W, view_h, 0);
	if (view_h > CANVAS_H)
		fill_content_rect(view_x, view_y + CANVAS_H,
			view_w, view_h - CANVAS_H, 0);

}

// The two rules separating the three panels. Hardware rasterizer, like
// all the other chrome in this system.
static void draw_frame(void) {

	int cw = z_win_content_w(&win);
	int ch = z_win_content_h(&win);

	z_clip_t c;
	z_win_content_rect(&win, &c);

	// vertical rule between tools and canvas
	z_fb_hw_line(c.x0 + TOOLS_W, c.y0,
		c.x0 + TOOLS_W, c.y0 + ch - PALETTE_H - 2, 1, &c);

	// horizontal rule above the palette
	z_fb_hw_line(c.x0, c.y0 + ch - PALETTE_H,
		c.x0 + cw - 1, c.y0 + ch - PALETTE_H, 1, &c);

}

// Full repaint of everything this app owns. Called on Z_WM_REDRAW,
// which the wm sends after it has already cleared the region -- so this
// must not assume anything about what is currently on screen.
static void repaint(void) {

	z_clip_t vc;
	view_clip(&vc);

	// order matters: blank the gaps first, then draw over them. See
	// clear_panels() for why this can't be left to wm.
	clear_panels();

	draw_frame();
	z_widget_draw_all(&wset, true);

	canvas_blit(0, 0, view_w, view_h, vc.x0, vc.y0, &vc);

	prev_valid = false;

}

// ---------------------------------------------------------------
// tools
// ---------------------------------------------------------------

static const uint8_t *pattern(void) {
	return z_pattern_table[cur_pattern];
}

// Converts a content-relative point to document coordinates. The
// document's origin currently sits at the viewport's top-left (no
// scrolling yet), so this is just a subtraction -- isolated in one
// function anyway, since adding a scroll offset later should be a
// change to this function and nothing else.
static void to_doc(int cx, int cy, int *dx, int *dy) {
	*dx = cx - view_x;
	*dy = cy - view_y;
}

static bool in_view(int cx, int cy) {
	return cx >= view_x && cx < view_x + view_w &&
		cy >= view_y && cy < view_y + view_h;
}

// -- freehand stamps, used as canvas_walk_line() callbacks --

static void stamp_pencil(int x, int y) {
	canvas_pset(x, y, pencil_ink);
}

static void stamp_brush(int x, int y) {
	const uint8_t *pat = pattern();
	for (int j = 0; j < BRUSH_SIZE; j++)
		for (int i = 0; i < BRUSH_SIZE; i++)
			if (brush_mask[j] & (0x80u >> i))
				canvas_pset_pat(x - BRUSH_SIZE / 2 + i,
					y - BRUSH_SIZE / 2 + j, pat);
}

static void stamp_eraser(int x, int y) {
	for (int j = 0; j < ERASER_SIZE; j++)
		for (int i = 0; i < ERASER_SIZE; i++)
			canvas_pset(x - ERASER_SIZE / 2 + i,
				y - ERASER_SIZE / 2 + j, 0);
}

static bool tool_is_freehand(tool_t t) {
	return t == TOOL_PENCIL || t == TOOL_BRUSH || t == TOOL_ERASER;
}

static bool tool_is_shape(tool_t t) {
	return t == TOOL_LINE || t == TOOL_RECT ||
		t == TOOL_RECTFILL || t == TOOL_OVAL;
}

// -- rubber band preview --
//
// Drawn straight to the framebuffer, never into the document: the shape
// isn't committed until the button comes up, and putting it in the
// document would mean undoing it on every mouse move.
//
// Erasing works by re-blitting the document over the region the preview
// covered, which is why prev_x0..prev_y1 exists. Drawing the preview a
// second time to cancel it (an XOR-style erase) isn't available here --
// the framebuffer has no XOR write mode, and the preview crosses
// arbitrary existing artwork.

static void preview_bounds(int dx0, int dy0, int dx1, int dy1) {

	prev_x0 = dx0 < dx1 ? dx0 : dx1;
	prev_x1 = dx0 < dx1 ? dx1 : dx0;
	prev_y0 = dy0 < dy1 ? dy0 : dy1;
	prev_y1 = dy0 < dy1 ? dy1 : dy0;

	// One pixel of slack each side, so an outline drawn exactly on the
	// bounding box is fully covered when it's erased.
	prev_x0--; prev_y0--; prev_x1++; prev_y1++;

	prev_valid = true;

}

static void preview_erase(void) {

	if (!prev_valid) return;

	z_clip_t vc;
	view_clip(&vc);

	canvas_blit(prev_x0, prev_y0,
		prev_x1 - prev_x0 + 1, prev_y1 - prev_y0 + 1,
		vc.x0 + prev_x0, vc.y0 + prev_y0, &vc);

	prev_valid = false;

}

// Draws the in-progress shape on screen only.
static void preview_draw(int dx0, int dy0, int dx1, int dy1) {

	z_clip_t vc;
	view_clip(&vc);

	int ax0 = vc.x0 + dx0, ay0 = vc.y0 + dy0;
	int ax1 = vc.x0 + dx1, ay1 = vc.y0 + dy1;

	switch (cur_tool) {

		case TOOL_LINE:
			z_fb_hw_line(ax0, ay0, ax1, ay1, 1, &vc);
			break;

		case TOOL_RECT:
		case TOOL_RECTFILL:
			// Even the filled variant previews as an outline: filling
			// it live would mean a full pattern fill per mouse sample,
			// and the outline already communicates the result.
			z_fb_hw_box(ax0, ay0, ax1, ay1, 1, &vc);
			break;

		case TOOL_OVAL:
			// No hardware ellipse, so this previews as its bounding
			// box -- which is also the affordance the original
			// MacPaint's oval tool gave.
			z_fb_hw_box(ax0, ay0, ax1, ay1, 1, &vc);
			break;

		default:
			return;

	}

	preview_bounds(dx0, dy0, dx1, dy1);

}

// Commits a finished shape into the document.
static void shape_commit(int dx0, int dy0, int dx1, int dy1) {

	switch (cur_tool) {

		case TOOL_LINE:
			canvas_walk_line(dx0, dy0, dx1, dy1, stamp_pencil);
			break;

		case TOOL_RECT:
			canvas_rect(dx0, dy0, dx1, dy1, 1);
			break;

		case TOOL_RECTFILL: {
			int x0 = dx0 < dx1 ? dx0 : dx1, x1 = dx0 < dx1 ? dx1 : dx0;
			int y0 = dy0 < dy1 ? dy0 : dy1, y1 = dy0 < dy1 ? dy1 : dy0;
			canvas_fill_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, pattern());
			// Outline the fill in solid black. Without it a light
			// pattern has no discernible edge at all, which makes a
			// filled rectangle look like a smudge rather than a shape.
			canvas_rect(x0, y0, x1, y1, 1);
			break;
		}

		case TOOL_OVAL:
			canvas_oval(dx0, dy0, dx1, dy1, 1);
			break;

		default:
			return;

	}

}

// Re-blits just the region a finished operation touched.
static void canvas_refresh(int dx0, int dy0, int dx1, int dy1) {

	z_clip_t vc;
	view_clip(&vc);

	int x0 = (dx0 < dx1 ? dx0 : dx1) - 2;
	int x1 = (dx0 < dx1 ? dx1 : dx0) + 2;
	int y0 = (dy0 < dy1 ? dy0 : dy1) - 2;
	int y1 = (dy0 < dy1 ? dy1 : dy0) + 2;

	canvas_blit(x0, y0, x1 - x0 + 1, y1 - y0 + 1,
		vc.x0 + x0, vc.y0 + y0, &vc);

}

// ---------------------------------------------------------------
// input
// ---------------------------------------------------------------

static void handle_mouse(uint32_t packed) {

	int cx, cy;
	z_win_mouse_content_xy(&win, packed, &cx, &cy);

	uint8_t buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);
	bool down = (buttons & Z_MOUSE_BTN_LEFT) != 0;

	// Widgets first. z_widget_mouse() only claims events landing on a
	// widget, so a click on the canvas falls through -- but a press
	// that STARTED on a widget stays with the widget, which is what
	// stops a slip off the edge of the palette from suddenly painting.
	int act = z_widget_mouse(&wset, cx, cy, buttons);

	if (act >= 0) {
		if (act < W_PAT0) cur_tool = (tool_t)(act - W_TOOL0);
		else cur_pattern = act - W_PAT0;
		return;
	}

	// a widget gesture in progress owns the pointer
	if (wset.pressed >= 0) return;

	int dx, dy;
	to_doc(cx, cy, &dx, &dy);

	if (down && !drawing) {

		// -- press --

		// Only start a stroke if the press landed on the canvas. A
		// press elsewhere that isn't a widget (the gap between panels,
		// say) should do nothing at all, rather than starting an
		// invisible stroke that springs into life the moment the
		// cursor wanders over the canvas.
		if (!in_view(cx, cy)) return;

		drawing = true;
		stroke_x0 = dx; stroke_y0 = dy;
		stroke_px = dx; stroke_py = dy;

		switch (cur_tool) {

			case TOOL_PENCIL:
				pencil_ink = canvas_pget(dx, dy) ? 0 : 1;
				stamp_pencil(dx, dy);
				canvas_refresh(dx, dy, dx, dy);
				break;

			case TOOL_BRUSH:
				stamp_brush(dx, dy);
				canvas_refresh(dx - BRUSH_SIZE, dy - BRUSH_SIZE,
					dx + BRUSH_SIZE, dy + BRUSH_SIZE);
				break;

			case TOOL_ERASER:
				stamp_eraser(dx, dy);
				canvas_refresh(dx - ERASER_SIZE, dy - ERASER_SIZE,
					dx + ERASER_SIZE, dy + ERASER_SIZE);
				break;

			default:
				// shape tools: nothing happens until the drag starts
				break;

		}

		return;

	}

	if (down && drawing) {

		// -- drag --

		if (tool_is_freehand(cur_tool)) {

			// Interpolate from the previous sample. The pointer moves
			// far more than one pixel between samples, so stamping
			// only at sample positions leaves a dotted trail rather
			// than a stroke.
			if (dx == stroke_px && dy == stroke_py) return;

			if (cur_tool == TOOL_PENCIL)
				canvas_walk_line(stroke_px, stroke_py, dx, dy, stamp_pencil);
			else if (cur_tool == TOOL_BRUSH)
				canvas_walk_line(stroke_px, stroke_py, dx, dy, stamp_brush);
			else
				canvas_walk_line(stroke_px, stroke_py, dx, dy, stamp_eraser);

			int pad = (cur_tool == TOOL_ERASER) ? ERASER_SIZE : BRUSH_SIZE;
			canvas_refresh(stroke_px - pad, stroke_py - pad,
				dx + pad, dy + pad);

			stroke_px = dx; stroke_py = dy;

		} else if (tool_is_shape(cur_tool)) {

			preview_erase();
			preview_draw(stroke_x0, stroke_y0, dx, dy);

		}

		return;

	}

	if (!down && drawing) {

		// -- release --

		drawing = false;

		if (tool_is_shape(cur_tool)) {
			preview_erase();
			shape_commit(stroke_x0, stroke_y0, dx, dy);
			canvas_refresh(stroke_x0, stroke_y0, dx, dy);
		}

	}

}

// ---------------------------------------------------------------

static void widgets_init(void) {

	memset(widgets, 0, sizeof(widgets));

	for (int i = 0; i < TOOL_COUNT; i++) {
		widgets[W_TOOL0 + i].type = Z_WIDGET_TOGGLE;
		widgets[W_TOOL0 + i].w = TOOL_BTN;
		widgets[W_TOOL0 + i].h = TOOL_BTN;
		widgets[W_TOOL0 + i].group = GROUP_TOOL;
		widgets[W_TOOL0 + i].icon = tool_icons[i];
		widgets[W_TOOL0 + i].enabled = true;
	}

	for (int i = 0; i < Z_PATTERN_COUNT; i++) {
		widgets[W_PAT0 + i].type = Z_WIDGET_SWATCH;
		widgets[W_PAT0 + i].w = SWATCH;
		widgets[W_PAT0 + i].h = SWATCH;
		widgets[W_PAT0 + i].group = GROUP_PAT;
		widgets[W_PAT0 + i].pattern = z_pattern_table[i];
		widgets[W_PAT0 + i].enabled = true;
	}

	z_widget_set_init(&wset, widgets, WIDGET_COUNT, &win);

	z_widget_select(&wset, W_TOOL0 + TOOL_PENCIL);
	z_widget_select(&wset, W_PAT0 + cur_pattern);

}

int main(void) {

	printf("draw: starting\n");

	use_hw_blit = z_fb_hw_blit_mem_available();
	printf("draw: canvas blit path: %s\n",
		use_hw_blit ? "hardware (blitter memory copy)" : "software");

	canvas_clear();
	widgets_init();

	// Z_WIN_FLAG_RESIZABLE gives us the corner grip;
	// Z_WIN_FLAG_MIN_IS_CREATE pins the minimum size to WIN_W/WIN_H so
	// the tool column and pattern palette can never be shrunk out of
	// the window. See both flags' comments in zwm.h.
	//
	// CLOSE_KILLS_OWNER is correct here specifically because this app
	// owns exactly one window for its whole lifetime -- see that flag's
	// own warning about apps that own several.
	if (z_win_create_flags(&win, "draw", WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER |
		Z_WIN_FLAG_RESIZABLE | Z_WIN_FLAG_MIN_IS_CREATE) != Z_OK) {
		printf("draw: failed to create window -- is wm running?\n");
		return 1;
	}

	layout();
	repaint();

	for (;;) {

		z_msg_t msg;

		// Drain the whole queue each pass rather than one message per
		// iteration. Mouse events arrive at pointer rates and each can
		// trigger a canvas blit, so handling one per loop lets the app
		// fall progressively further behind the real cursor -- the
		// stroke visibly lags, then catches up in a rush. See
		// Z_WM_MOUSE's own note on this in zwm.h.
		while (z_msg_read(&msg) == Z_OK) {

			switch (msg.subject) {

				case Z_WM_MOUSE:
					if (msg.obj.type == Z_UINT32)
						handle_mouse(msg.obj.val.uint32);
					break;

				case Z_WM_REDRAW:
					if (msg.obj.type == Z_UINT32)
						z_win_apply_redraw(&win, msg.obj.val.uint32);
					repaint();
					z_win_redraw_done(&win);
					break;

				case Z_WM_WINDOW_MOVED:
					// No layout() -- moving doesn't change our size,
					// and every rect we hold is content-relative.
					z_win_parse_rect(&win, &msg.obj);
					break;

				case Z_WM_WINDOW_RESIZED:
					// Arrives BEFORE the Z_WM_REDRAW that follows a
					// resize (zwm.h guarantees that ordering), so the
					// layout is already correct by the time we're asked
					// to repaint at the new size.
					z_win_apply_resized(&win, &msg.obj);
					layout();
					break;

				default:
					break;

			}

		}

		for (volatile int i = 0; i < 200; i++);	// light throttle

	}

	return 0;

}
