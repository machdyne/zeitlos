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
 * -- scrolling --
 *
 * The document is a fixed 640x480 and no window can be that large once
 * the frame, titlebar, tool column and palette are accounted for, so
 * the viewport is always smaller than the drawing. A vertical
 * scrollbar down the right of the canvas and a horizontal one along
 * its bottom (z_scrollbar_t, sw/common/zwidget.h) pan it; scroll_x/
 * scroll_y hold the document coordinate at the viewport's top-left.
 * Before this the bottom and right of the document were simply
 * unreachable.
 *
 * -- undo --
 *
 * drawundo.h: a level per stroke or shape, the page as it was inside
 * the box the edit changed, in a ring taken from the arena -- or, with
 * no arena (a 1MB board), one level kept in `spare` at no cost. Ctrl+Z
 * and Ctrl+Y (or Ctrl+Shift+Z), the same keys as text and sheet.
 *
 * -- the paint bucket --
 *
 * drawfill.h: a span fill, a few spans a step between messages, so the
 * window answers throughout and Esc stops it. See the note above
 * tool_icons[] for the one that was here before and why it went.
 *
 * -- printing --
 *
 * Ctrl+P or the title bar's printer: the print dialog (zdialog.h), then
 * the page as one picture, fitted to the paper (do_print()).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zpaths.h"
#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"	// Z_TICK_HZ, for the idle wait in main()
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zgfx.h"
#include "../../common/zfont.h"
#include "../../common/zwidget.h"
#include "../../common/zflist.h"
#include "../../common/zdialog.h"
#include "../../common/zkbd.h"	// Z_KEY_HELP
#include "../../common/zfsapp.h"
#include "../../common/zbm.h"
#include "../../common/ztype.h"
#include "draw_icons.h"
#include "drawio.h"
#include "drawstamp.h"
#include "drawundo.h"
#include "drawfill.h"
#include "../../common/zprint.h"
#include "../../common/zspeak.h"

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
//
// Two of them, and `canvas` points at the one on screen. The other,
// `spare`, is where a file loads (drawio.h): the two swap only once the
// whole file is in, so stopping a load, or a damaged file, leaves the
// drawing as it was. That costs a second 37.5K of .bss; the cheaper
// ways out are not correct (reading into the canvas loses the old
// drawing on a stop) or not cheaper (keeping the old drawing in a /tmp
// file is the same 37.5K in the RAM disk, plus the time to write it).
//
// Between loads `spare` is undo's (drawundo.h): the page before the last
// edit, or a copy of the page kept for the levels in the arena.
static uint32_t canvas_a[CANVAS_H][CANVAS_WORDS];
static uint32_t canvas_b[CANVAS_H][CANVAS_WORDS];
static uint32_t (*canvas)[CANVAS_WORDS] = canvas_a;
static uint32_t (*spare)[CANVAS_WORDS] = canvas_b;

#define CANVAS_BYTES  ((int)sizeof(canvas_a))

// The page as drawstamp.c and drawundo.c take it: DRAW_PAGE_* words.
#define PAGE(c)  (&(c)[0][0])

#if CANVAS_W != DRAW_PAGE_W || CANVAS_H != DRAW_PAGE_H
#error "drawstamp.h's page is not the canvas"
#endif

// Whether this bitstream's blitter can read main memory. Probed once at
// startup (z_fb_hw_blit_mem_available(), zgfx.h) rather than per call,
// and kept as a plain flag so the fallback below reads as a deliberate
// alternative rather than an error path.
static bool use_hw_blit;

// Marks the document dirty (or clean) and refreshes the titlebar.
// Forward-declared: the drawing tools further down call it, and it is
// defined with the rest of the file handling near the bottom.
static void set_modified(bool m);

// Whether a file is loading or saving (see "files" near the bottom):
// the canvas takes no drawing meanwhile.
static bool busy(void);

// -- the file --
//
// Saved as ZBM (sw/common/zbm.h): a 16-byte header carrying a magic
// and the real dimensions, then the canvas array verbatim.
//
// The first version wrote the canvas with no header at all, since it
// is already exactly what the blitter reads. The header is worth its
// sixteen bytes: without one a file carried no record of its own
// shape, so changing CANVAS_W/CANVAS_H would have turned every
// existing file into unreadable garbage with only the byte count to
// notice from, and nothing outside draw could identify a saved image
// at all. Files written before it are still loadable -- see
// z_bm_is_legacy_size() and drawio.c.
static char filename[Z_FS_PATH_MAX];
static bool modified;

static z_dialog_ctx_t dlg_ctx;

// Directory the last dialog was in, so a second one starts where the
// first left off rather than back at the root.
static char last_dir[Z_FLIST_PATH_MAX] = Z_DIR_HOME;

// Filename handed to us at launch, if any -- see main().
static char launch_path[Z_FS_PATH_MAX];

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

// -- scrolling --
//
// The document is a fixed 640x480 and the window is smaller than that
// by default, so most of the drawing was simply unreachable: the
// viewport showed the document's top-left corner and there was no way
// to move it. Two scrollbars (z_scrollbar_t, sw/common/zwidget.h) now
// pan it.
//
// scroll_x/scroll_y are the document coordinate shown at the
// viewport's top-left, i.e. exactly the scrollbars' values. Every
// document-to-screen conversion in this file goes through
// doc_screen_x()/doc_screen_y() below rather than adding vc.x0
// directly, which is what to_doc()'s own comment anticipated when it
// said adding a scroll offset should be a change to one function --
// it turned out to be a change to one function plus the two
// conversions running the other way.
static z_scrollbar_t vsb, hsb;
static int scroll_x, scroll_y;

// Document coordinate -> absolute screen coordinate, within the
// viewport whose clip rect is `vc`.
static inline int doc_screen_x(const z_clip_t *vc, int dx) {
	return vc->x0 + dx - scroll_x;
}

static inline int doc_screen_y(const z_clip_t *vc, int dy) {
	return vc->y0 + dy - scroll_y;
}

// -- widgets --

// Nine, in a grid two wide: the freehand tools and the bucket, then the
// shapes, an outline and a filled one of each.
#define TOOL_COUNT   9
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
	TOOL_FILL,
	TOOL_LINE,
	TOOL_RECT,
	TOOL_RECTFILL,
	TOOL_OVAL,
	TOOL_OVALFILL,
} tool_t;

// Their names, for the help, speech and the title while one is chosen
// by key; and the key that chooses each (also 1..9 in this order).
static const char *const tool_names[TOOL_COUNT] = {
	"Pencil", "Brush", "Eraser", "Fill", "Line", "Rectangle",
	"Filled rectangle", "Oval", "Filled oval",
};
static const char tool_keys[TOOL_COUNT] = { 'p', 'b', 'e', 'f', 'l', 'r', 'R', 'o', 'O' };

static z_widget_t widgets[WIDGET_COUNT];
static z_widget_set_t wset;

// The paint bucket (drawfill.c).
//
// One was here before and was removed, because a pattern fill genuinely
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
// The one now is that different approach: it pushes runs rather than
// points; the region is found in one colour (a pixel changed is a pixel
// visited) and the pattern put in after, where the page differs from
// `spare`, so no visited mask is needed; a run the stack had no room
// for is found again by a sweep of the rows touched; and it runs a few
// spans at a time between messages, with Esc to stop it.
static const uint8_t *tool_icons[TOOL_COUNT] = {
	draw_icon_pencil, draw_icon_brush, draw_icon_eraser, draw_icon_bucket,
	draw_icon_line, draw_icon_rect, draw_icon_rectfill, draw_icon_oval,
	draw_icon_ovalfill,
};

// -- a fill in progress --
static draw_fill_t fill;
static bool filling;

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

// What the stroke may have touched, in document coordinates, for undo
// (stroke_end()). Grown by every stamp, padded for the brush.
static int touch_x0, touch_y0, touch_x1, touch_y1;

// -- brush shapes: drawstamp.c --

#define BRUSH_SIZE  DRAW_BRUSH_SIZE
#define ERASER_SIZE DRAW_ERASER_SIZE

// -- undo: drawundo.h --
//
// Its levels come from the arena (docs/kernel.md, "Arenas"), asked for
// once at start and never resized, so the base never moves. 256KB holds
// at least six page-sized strokes and DRAW_UNDO_LEVELS small ones; 64KB
// if that much is not there; none on a 1MB board, which refuses arenas,
// and then there is one level, in `spare`.
#define UNDO_WANT  (256u * 1024u)
#define UNDO_LESS  (64u * 1024u)

static draw_undo_t undo;

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

// Patterns are anchored to the DOCUMENT's 8x8 grid rather than to the
// shape being drawn (drawstamp.c does the filling). That means two
// shapes filled with the same pattern line up seamlessly where they
// meet, and a shape doesn't change appearance depending on where it
// was drawn -- the same anchoring rule z_fb_hw_fill_pattern() follows
// on the screen side, for the same reason.

static void canvas_clear(void) {
	memset(canvas, 0, CANVAS_BYTES);
}

// A word at a time (drawstamp.c): pixel by pixel, a page-sized fill
// took a few hundred ms -- longer than the main loop may stop
// (docs/responsive.md).
static void canvas_fill_rect(int x, int y, int w, int h, const uint8_t *pat) {
	draw_fill_rect(PAGE(canvas), x, y, w, h, pat);
}

// Outline: the top and bottom edges a word at a time too.
static void canvas_rect(int x0, int y0, int x1, int y1, int color) {
	draw_rect(PAGE(canvas), x0, y0, x1, y1, color);
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

// The inside of the oval inscribed in the rect, in `pat`, a row at a
// time (a span of draw_fill_rect() each). The outline goes over it.
static void canvas_oval_fill(int x0, int y0, int x1, int y1, const uint8_t *pat) {

	if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
	if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }

	int a = (x1 - x0) / 2, b = (y1 - y0) / 2;
	int cx = x0 + a, cy = y0 + b;
	if (a <= 0 || b <= 0) return;

	int64_t a2 = (int64_t)a * a, b2 = (int64_t)b * b;
	int x = a;
	// row by row from the middle out; the half width only shrinks, so
	// it is found by stepping down from the last one
	for (int y = 0; y <= b; y++) {
		while (x > 0 && b2 * x * x + a2 * y * y > a2 * b2) x--;
		draw_fill_rect(PAGE(canvas), cx - x, cy + y, 2 * x + 1, 1, pat);
		if (y) draw_fill_rect(PAGE(canvas), cx - x, cy - y, 2 * x + 1, 1, pat);
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

	// Both scrollbars come out of the viewport, not out of the window
	// chrome: the vertical one runs down the right edge of the canvas
	// and the horizontal one along its bottom, leaving a Z_SB_THICK
	// square where they meet.
	view_w = cw - view_x - Z_SB_THICK;
	view_h = ch - PALETTE_H - 1 - Z_SB_THICK;

	if (view_w < 0) view_w = 0;
	if (view_h < 0) view_h = 0;

	z_scrollbar_set_geom(&vsb, cw - Z_SB_THICK, view_y, view_h);
	z_scrollbar_set_geom(&hsb, view_x, view_y + view_h, view_w);

	// Range is in document PIXELS -- the unit the caller picks (see
	// zwidget.h). page is how much of the document is on screen, so
	// growing the window shows more of it and shrinks the thumb.
	z_scrollbar_set_range(&vsb, CANVAS_H, view_h);
	z_scrollbar_set_range(&hsb, CANVAS_W, view_w);

	// set_range() clamps the value, so a resize that reveals more of
	// the document can shift the viewport -- pick the clamped values
	// back up rather than letting scroll_x/y disagree with the bars.
	scroll_x = (int)hsb.value;
	scroll_y = (int)vsb.value;

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

	// Palette strip, below its rule -- in two pieces, because the
	// resize grip's corner falls inside this window's content area
	// and a full-width fill down to the bottom erases most of it.
	// See Z_WIN_GRIP_INSET in zwm.h; sw/apps/text does the same thing
	// for the same reason.
	int pal_y = ch - PALETTE_H + 1;

	// first row the grip occupies -- clamped up to the palette's own
	// top, so a window short enough for the grip to start above the
	// palette doesn't produce a negative-height fill below.
	int grip_y = ch - Z_WIN_GRIP_INSET;
	if (grip_y < pal_y) grip_y = pal_y;

	// full width, for the rows above the grip
	if (grip_y > pal_y)
		fill_content_rect(0, pal_y, cw, grip_y - pal_y, 0);

	// the grip's own rows, stopping short of its columns
	int grip_keep_w = cw - Z_WIN_GRIP_INSET;
	if (grip_keep_w > 0 && ch > grip_y)
		fill_content_rect(0, grip_y, grip_keep_w, ch - grip_y, 0);

	// The square where the two scrollbars meet. Neither of them owns
	// it, and the canvas blit stops short of both.
	fill_content_rect(view_x + view_w, view_y + view_h,
		Z_SB_THICK, Z_SB_THICK, 0);

	// viewport area the document doesn't reach. With scrolling this
	// only happens if the window is somehow larger than the document,
	// which it can't currently be -- kept because canvas_blit() still
	// clamps to the document rather than painting blank space itself,
	// so the day CANVAS_W/H shrinks this is what stops it leaving
	// stale pixels.
	if (view_w > CANVAS_W - scroll_x)
		fill_content_rect(view_x + (CANVAS_W - scroll_x), view_y,
			view_w - (CANVAS_W - scroll_x), view_h, 0);
	if (view_h > CANVAS_H - scroll_y)
		fill_content_rect(view_x, view_y + (CANVAS_H - scroll_y),
			view_w, view_h - (CANVAS_H - scroll_y), 0);

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

	// horizontal rule above the palette. Stops short of the grip
	// column for the same reason clear_panels() does -- the rule
	// itself sits above the grip today, but the window can be short
	// enough for PALETTE_H to put it inside the grip's rows.
	int rule_x1 = cw - 1;
	if (ch - PALETTE_H >= ch - Z_WIN_GRIP_INSET)
		rule_x1 = cw - Z_WIN_GRIP_INSET - 1;
	if (rule_x1 >= 0)
		z_fb_hw_line(c.x0, c.y0 + ch - PALETTE_H,
			c.x0 + rule_x1, c.y0 + ch - PALETTE_H, 1, &c);

}

// Re-blits the canvas viewport at the current scroll offset, and
// repaints the bars. Everything else this app draws -- tools,
// palette, rules -- is fixed to the window, so scrolling doesn't
// touch it; a full repaint() here would flash the whole panel set on
// every step of a scrollbar drag.
static void scroll_view(void) {

	z_clip_t vc;
	view_clip(&vc);

	canvas_blit(scroll_x, scroll_y, view_w, view_h, vc.x0, vc.y0, &vc);

	z_scrollbar_draw(&vsb, false);
	z_scrollbar_draw(&hsb, false);

	// Any rubber band on screen was drawn at the old offset and is
	// now in the wrong place; the blit above has already erased it.
	prev_valid = false;

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

	z_scrollbar_draw(&vsb, true);
	z_scrollbar_draw(&hsb, true);

	canvas_blit(scroll_x, scroll_y, view_w, view_h, vc.x0, vc.y0, &vc);

	prev_valid = false;

}

// ---------------------------------------------------------------
// tools
// ---------------------------------------------------------------

static const uint8_t *pattern(void) {
	return z_pattern_table[cur_pattern];
}

// Converts a content-relative point to document coordinates -- the
// viewport's own offset, plus however far the document is scrolled
// underneath it.
static void to_doc(int cx, int cy, int *dx, int *dy) {
	*dx = cx - view_x + scroll_x;
	*dy = cy - view_y + scroll_y;
}

static bool in_view(int cx, int cy) {
	return cx >= view_x && cx < view_x + view_w &&
		cy >= view_y && cy < view_y + view_h;
}

// -- freehand stamps, used as canvas_walk_line() callbacks --

static void stamp_pencil(int x, int y) {
	canvas_pset(x, y, pencil_ink);
}

// The brush and eraser a word at a time (drawstamp.c): per pixel they
// were 64 and 144 sets a point, a point per pixel along the line, and
// a quick eraser drag fell visibly behind the pointer.
static void stamp_brush(int x, int y) {
	draw_stamp_brush(PAGE(canvas), x, y, pattern());
}

static void stamp_eraser(int x, int y) {
	draw_stamp_eraser(PAGE(canvas), x, y);
}

static bool tool_is_freehand(tool_t t) {
	return t == TOOL_PENCIL || t == TOOL_BRUSH || t == TOOL_ERASER;
}

static bool tool_is_shape(tool_t t) {
	return t == TOOL_LINE || t == TOOL_RECT ||
		t == TOOL_RECTFILL || t == TOOL_OVAL || t == TOOL_OVALFILL;
}

// Modifiers from the last key event: Z_WM_MOUSE carries none (zwm.h),
// so a Shift held while dragging is known from the key that said so.
static uint32_t mods_now;

// Shift: a line at 0, 45 or 90 degrees; a rectangle a square, an oval a
// circle. Moves (x1, y1).
static void constrain(int x0, int y0, int *x1, int *y1) {

	if (!(mods_now & Z_KBD_MOD_SHIFT)) return;

	int dx = *x1 - x0, dy = *y1 - y0;
	int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;

	if (cur_tool == TOOL_LINE) {
		// nearer the axis than the diagonal: along it
		if (ax > 2 * ay) { *y1 = y0; return; }
		if (ay > 2 * ax) { *x1 = x0; return; }
	}
	int m = ax > ay ? ax : ay;
	*x1 = x0 + (dx < 0 ? -m : m);
	*y1 = y0 + (dy < 0 ? -m : m);

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
		doc_screen_x(&vc, prev_x0), doc_screen_y(&vc, prev_y0), &vc);

	prev_valid = false;

}

// Draws the in-progress shape on screen only.
static void preview_draw(int dx0, int dy0, int dx1, int dy1) {

	z_clip_t vc;
	view_clip(&vc);

	int ax0 = doc_screen_x(&vc, dx0), ay0 = doc_screen_y(&vc, dy0);
	int ax1 = doc_screen_x(&vc, dx1), ay1 = doc_screen_y(&vc, dy1);

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
		case TOOL_OVALFILL:
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

	// Every path through this function writes to the canvas, so one
	// flag here covers all four shape tools.
	set_modified(true);

	switch (cur_tool) {

		case TOOL_LINE:
			// always black: the pencil's ink is the last pencil
			// stroke's, white when that one started on black
			pencil_ink = 1;
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

		case TOOL_OVALFILL:
			canvas_oval_fill(dx0, dy0, dx1, dy1, pattern());
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
		doc_screen_x(&vc, x0), doc_screen_y(&vc, y0), &vc);

}

// ---------------------------------------------------------------
// undo
// ---------------------------------------------------------------

// Grows the box the stroke may have touched.
static void touch(int x0, int y0, int x1, int y1) {
	if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
	if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
	if (x0 < touch_x0) touch_x0 = x0;
	if (y0 < touch_y0) touch_y0 = y0;
	if (x1 > touch_x1) touch_x1 = x1;
	if (y1 > touch_y1) touch_y1 = y1;
}

// A stroke or shape starts: undo takes note of the page as it is.
static void stroke_begin(int dx, int dy) {
	draw_undo_begin(&undo);
	touch_x0 = touch_x1 = dx;
	touch_y0 = touch_y1 = dy;
}

// It has ended (button up, or a save or load cuts it short): the level,
// and the title's '*' from what undo now says -- a stroke that changed
// nothing, or undoing back to the save, leaves the page unmodified.
static void stroke_end(void) {
	draw_undo_end(&undo, touch_x0, touch_y0, touch_x1, touch_y1);
	set_modified(!draw_undo_at_saved(&undo));
}

// Ctrl+Z, and Ctrl+Y / Ctrl+Shift+Z. Not mid-stroke (the stroke is not
// a level yet), and not while a file job runs: a save is reading the
// page, and a load is writing `spare`, which one-level undo keeps its
// level in. Only the box the level covers is re-blitted.
static void do_undo(bool redo) {

	if (drawing || busy() || filling) return;

	int box[4];
	if (!(redo ? draw_undo_redo(&undo, box) : draw_undo_undo(&undo, box)))
		return;

	canvas_refresh(box[0], box[1], box[2], box[3]);
	set_modified(!draw_undo_at_saved(&undo));

}

// ---------------------------------------------------------------
// the fill (drawfill.h)
// ---------------------------------------------------------------

static void update_title(void);

// From the main loop, between messages: spans for about 10 ms, then
// back. Done: the box it changed on screen, and an undo level.
static void fill_step(void) {

	uint32_t t0 = z_uptime_ticks();
	int r;
	do r = draw_fill_step(&fill, 32);
	while (r == DRAW_FILL_MORE && z_uptime_ticks() - t0 < Z_TICK_HZ / 100);
	if (r == DRAW_FILL_MORE) return;

	filling = false;
	touch(fill.x0, fill.y0, fill.x1, fill.y1);
	stroke_end();
	canvas_refresh(fill.x0, fill.y0, fill.x1, fill.y1);
	update_title();

}

// Esc, or the window closing: the page as it was.
static void fill_cancel(void) {
	if (!filling) return;
	draw_fill_stop(&fill);
	filling = false;
	stroke_end();
	update_title();
}

// ---------------------------------------------------------------
// input
// ---------------------------------------------------------------

static void handle_mouse(uint32_t packed) {

	int cx, cy;
	z_win_mouse_content_xy(&win, packed, &cx, &cy);

	uint8_t buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);
	bool down = (buttons & Z_MOUSE_BTN_LEFT) != 0;

	// Scrollbars before anything else, and they keep the pointer for
	// the whole of a drag they started -- otherwise sliding the thumb
	// sideways over the canvas would start painting. Checking
	// has_pointer() rather than the return value matters: the return
	// says whether the VALUE changed, and pressing the thumb changes
	// nothing (see zwidget.h).
	if (z_scrollbar_has_pointer(&vsb, cx, cy)) {
		if (z_scrollbar_mouse(&vsb, cx, cy, buttons)) {
			scroll_y = (int)vsb.value;
			scroll_view();
		}
		return;
	}

	if (z_scrollbar_has_pointer(&hsb, cx, cy)) {
		if (z_scrollbar_mouse(&hsb, cx, cy, buttons)) {
			scroll_x = (int)hsb.value;
			scroll_view();
		}
		return;
	}

	// Keep both edge detectors fed even when the pointer is
	// elsewhere, so a button released outside a bar still ends its
	// drag.
	z_scrollbar_mouse(&vsb, cx, cy, buttons);
	z_scrollbar_mouse(&hsb, cx, cy, buttons);

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

	// While a file loads or saves the canvas takes no drawing: a load is
	// about to replace it and a save is writing it (see "files" below).
	// Nor while a fill runs: it is reading the page.
	if (busy() || filling) return;

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

		// The bucket: a fill, run by the main loop a few spans at a
		// time (fill_step()). Not a drag: the button can come up.
		if (cur_tool == TOOL_FILL) {
			stroke_begin(dx, dy);
			if (draw_fill_start(&fill, PAGE(canvas), PAGE(spare), dx, dy, pattern())) {
				filling = true;
				z_win_set_title(&win, "Filling  (Esc stops)");
			} else stroke_end();
			return;
		}

		// The press itself already stamps a pixel for every freehand
		// tool, so the document is dirty from here on regardless of
		// whether a drag follows.
		set_modified(true);

		drawing = true;
		stroke_x0 = dx; stroke_y0 = dy;
		stroke_px = dx; stroke_py = dy;
		stroke_begin(dx, dy);

		switch (cur_tool) {

			case TOOL_PENCIL:
				pencil_ink = canvas_pget(dx, dy) ? 0 : 1;
				stamp_pencil(dx, dy);
				canvas_refresh(dx, dy, dx, dy);
				break;

			case TOOL_BRUSH:
				stamp_brush(dx, dy);
				touch(dx - BRUSH_SIZE, dy - BRUSH_SIZE,
					dx + BRUSH_SIZE, dy + BRUSH_SIZE);
				canvas_refresh(dx - BRUSH_SIZE, dy - BRUSH_SIZE,
					dx + BRUSH_SIZE, dy + BRUSH_SIZE);
				break;

			case TOOL_ERASER:
				stamp_eraser(dx, dy);
				touch(dx - ERASER_SIZE, dy - ERASER_SIZE,
					dx + ERASER_SIZE, dy + ERASER_SIZE);
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

			// Padded on both sides of the segment. This was
			// (px - pad, py - pad) to (dx + pad, dy + pad), which
			// canvas_refresh() reorders -- so dragging left or up by more
			// than twice the pad put the padding on the wrong side, and
			// part of the stroke stayed off screen until the next redraw.
			int pad = (cur_tool == TOOL_ERASER) ? ERASER_SIZE : BRUSH_SIZE;
			int sx0 = (stroke_px < dx ? stroke_px : dx) - pad;
			int sy0 = (stroke_py < dy ? stroke_py : dy) - pad;
			int sx1 = (stroke_px < dx ? dx : stroke_px) + pad;
			int sy1 = (stroke_py < dy ? dy : stroke_py) + pad;
			touch(sx0, sy0, sx1, sy1);
			canvas_refresh(sx0, sy0, sx1, sy1);

			stroke_px = dx; stroke_py = dy;

		} else if (tool_is_shape(cur_tool)) {

			constrain(stroke_x0, stroke_y0, &dx, &dy);
			preview_erase();
			preview_draw(stroke_x0, stroke_y0, dx, dy);

		}

		return;

	}

	if (!down && drawing) {

		// -- release --

		drawing = false;

		if (tool_is_shape(cur_tool)) {
			constrain(stroke_x0, stroke_y0, &dx, &dy);
			preview_erase();
			shape_commit(stroke_x0, stroke_y0, dx, dy);
			touch(stroke_x0, stroke_y0, dx, dy);
			canvas_refresh(stroke_x0, stroke_y0, dx, dy);
		}

		stroke_end();

	}

}

// ---------------------------------------------------------------

// Titlebar text: the filename, with a leading '*' while there are
// unsaved changes. Same convention as sw/apps/text.
static void update_title(void) {

	char t[64];
	z_win_doc_title(t, (int)sizeof(t), filename, modified, "untitled");
	z_win_set_title(&win, t);

}

// ---------------------------------------------------------------
// files
// ---------------------------------------------------------------
//
// Opening and saving never stop the app (docs/responsive.md): each is a
// draw_io_t job (drawio.h) that the main loop steps ~20 ms at a time
// while nothing else is waiting, with its progress in the titlebar and
// Esc (or Ctrl+C) to stop it. While one runs:
//
//   - a load reads into `spare`, not the canvas; the two swap only when
//     the whole file is in, so a stop or a damaged file leaves the old
//     drawing. Drawing is ignored: the drawing on screen is about to go.
//   - a save writes NAME.part from the canvas, which is read-only until
//     it ends (drawing is ignored), and renames it when complete.
//   - New, Open and Save are ignored: one file job at a time, and the
//     title says which is running. Scrolling, the tools and F1 work.
//   - close: during a load, stops it and then closes as usual; during
//     a save, closes once the save has succeeded (Esc stops the save
//     and keeps the window).
//
// "Save changes first?" -> Yes starts a save and remembers what asked
// (after_save); that runs once the save is done, and is dropped if the
// save fails or is stopped.

static void forward_msg(z_msg_t *msg, void *user);

static draw_io_t io;

enum { AFTER_NONE = 0, AFTER_NEW, AFTER_OPEN, AFTER_CLOSE };
static int after_save;

static uint32_t io_shown_tick;
static int io_shown_pct = -1;

static void set_modified(bool m) {

	if (modified == m) return;

	modified = m;
	update_title();

}

static void copy_path(char *d, int cap, const char *s) {
	int i = 0;
	for (; s[i] && i < cap - 1; i++) d[i] = s[i];
	d[i] = 0;
}

static void remember_dir(const char *path) {

	int last = 0;
	for (int i = 0; path[i]; i++) if (path[i] == '/') last = i;

	if (last == 0) { last_dir[0] = '/'; last_dir[1] = 0; return; }

	int i = 0;
	for (; i < last && i < (int)sizeof(last_dir) - 1; i++) last_dir[i] = path[i];
	last_dir[i] = 0;

}

// Appends s to d (capacity cap, `*n` used so far). By hand rather than
// snprintf(), which would link a second printf engine (~17K).
static void put(char *d, int cap, int *n, const char *s, int max) {
	for (int i = 0; s[i] && i < max && *n < cap - 1; i++) d[(*n)++] = s[i];
	d[*n] = 0;
}

static bool busy(void) {
	return draw_io_running(&io);
}

// "Loading NAME 45%  (Esc stops)" in the titlebar, a few times a second.
static void show_progress(bool force) {

	int pct = draw_io_permille(&io) / 10;
	uint32_t now = z_uptime_ticks();

	if (!force && (pct == io_shown_pct || now - io_shown_tick < Z_TICK_HZ / 8))
		return;

	io_shown_pct = pct;
	io_shown_tick = now;

	const char *name = io.path;
	for (const char *p = io.path; *p; p++) if (*p == '/') name = p + 1;

	char t[64], num[5];
	int n = 0, k = 0;
	if (pct >= 100) num[k++] = (char)('0' + pct / 100);
	if (pct >= 10) num[k++] = (char)('0' + pct / 10 % 10);
	num[k++] = (char)('0' + pct % 10);
	num[k] = 0;

	put(t, sizeof(t), &n, io.kind == DRAW_IO_LOAD ? "Loading " : "Saving ", 99);
	put(t, sizeof(t), &n, name, 30);
	put(t, sizeof(t), &n, " ", 99);
	put(t, sizeof(t), &n, num, 99);
	put(t, sizeof(t), &n, "%  (Esc stops)", 99);
	z_win_set_title(&win, t);

}

static void report_failure(const char *title) {

	char m[Z_FS_PATH_MAX + 64];

	if (io.err == DRAW_IO_E_KEPT) {
		int n = 0;
		put(m, sizeof(m), &n, "The old file is gone;\nthe image is in\n", 99);
		put(m, sizeof(m), &n, io.part, 200);
		z_dialog_confirm(&dlg_ctx, title, m, Z_DIALOG_OK_CANCEL);
		return;
	}

	z_dialog_confirm(&dlg_ctx, title, draw_io_error(io.err), Z_DIALOG_OK_CANCEL);

}

// A stroke still in progress when a save or load starts. A shape that
// is still being dragged out is not part of the canvas yet, and goes; a
// freehand stroke is already in the canvas, so it becomes an undo level
// as if the button had come up.
static void stroke_cut(void) {

	if (!drawing) return;

	drawing = false;
	if (tool_is_shape(cur_tool)) preview_erase();
	stroke_end();

}

// Starts a save of the canvas to `path`; `after` runs when it is done.
static void start_save_to(const char *path, int after) {

	// the canvas is read-only from here on
	stroke_cut();

	if (!draw_io_save_start(&io, path, canvas, CANVAS_W, CANVAS_H)) {
		report_failure("Save failed");
		return;
	}

	after_save = after;
	show_progress(true);

}

static void start_save(int after, bool ask) {

	if (busy() || filling) return;

	if (ask || !filename[0]) {

		char path[Z_FS_PATH_MAX];
		const char *suggest = filename[0] ? filename : "";

		for (const char *p = filename; *p; p++)
			if (*p == '/') suggest = p + 1;

		if (!z_dialog_save(&dlg_ctx, last_dir, suggest, path, sizeof(path)))
			return;

		start_save_to(path, after);
		return;

	}

	start_save_to(filename, after);

}

static void start_load(const char *path) {

	stroke_cut();

	// the load goes into spare, which undo uses meanwhile
	draw_undo_spare_lost(&undo);

	if (!draw_io_load_start(&io, path, spare, CANVAS_W, CANVAS_H)) {
		report_failure("Open failed");
		update_title();
		return;
	}

	show_progress(true);

}

// Offers to save when there are unsaved changes. true: go ahead now.
// false: not now -- cancelled, or a save has started and `after` runs
// when it is done.
static bool confirm_discard(int after) {

	if (!modified) return true;

	int r = z_dialog_confirm(&dlg_ctx, "Unsaved changes",
		"Save changes before\ncontinuing?", Z_DIALOG_YES_NO_CANCEL);

	if (r == Z_DIALOG_CANCEL) return false;
	if (r == Z_DIALOG_NO) return true;

	start_save(after, false);
	return false;

}

static void reset_view(void) {
	scroll_x = scroll_y = 0;
	z_scrollbar_set_value(&vsb, 0);
	z_scrollbar_set_value(&hsb, 0);
}

static void new_now(void) {

	canvas_clear();
	draw_undo_clear(&undo, PAGE(canvas), PAGE(spare));

	filename[0] = 0;
	reset_view();

	set_modified(false);
	update_title();
	repaint();

}

static void open_now(void) {

	char path[Z_FS_PATH_MAX];

	if (!z_dialog_open(&dlg_ctx, last_dir, path, sizeof(path))) return;

	start_load(path);

}

static void close_now(void) {

	z_win_destroy(&win);

	printf("draw: exiting\n");
	exit(0);

}

static void do_new(void) {
	if (busy() || filling) return;
	if (confirm_discard(AFTER_NEW)) new_now();
}

static void do_open(void) {
	if (busy() || filling) return;
	if (confirm_discard(AFTER_OPEN)) open_now();
}

static void do_close(void) {

	fill_cancel();

	if (busy() && io.kind == DRAW_IO_SAVE) {
		// quit once it is saved; Esc stops the save and keeps the window
		after_save = AFTER_CLOSE;
		return;
	}

	if (busy()) {
		draw_io_stop(&io);
		update_title();
	}

	if (confirm_discard(AFTER_CLOSE)) close_now();

}

// Esc / Ctrl+C: the running job stops; the app is as it was before it.
static void stop_io(void) {

	if (!busy()) return;

	draw_io_stop(&io);
	after_save = AFTER_NONE;
	update_title();

}

// A step of the running job, from the main loop; and what follows when
// it ends.
static void io_step(void) {

	int s = draw_io_step(&io, Z_TICK_HZ / 50);

	if (s == DRAW_IO_RUNNING) { show_progress(false); return; }

	io_shown_pct = -1;

	if (io.kind == DRAW_IO_LOAD) {

		if (s != DRAW_IO_DONE) {
			update_title();
			report_failure("Open failed");
			return;
		}

		if (io.legacy) printf("draw: loaded a headerless (pre-ZBM) file\n");

		uint32_t (*t)[CANVAS_WORDS] = canvas;
		canvas = spare;
		spare = t;

		copy_path(filename, (int)sizeof(filename), io.path);
		remember_dir(filename);
		reset_view();

		// another picture: nothing of the old one to undo into
		draw_undo_clear(&undo, PAGE(canvas), PAGE(spare));

		modified = false;
		update_title();
		repaint();
		return;

	}

	int after = after_save;
	after_save = AFTER_NONE;

	if (s != DRAW_IO_DONE) {
		update_title();
		report_failure("Save failed");
		return;
	}

	copy_path(filename, (int)sizeof(filename), io.path);
	remember_dir(filename);
	modified = false;
	draw_undo_mark_saved(&undo);	// undoing back to here is unmodified
	update_title();

	switch (after) {
		case AFTER_NEW:   new_now(); break;
		case AFTER_OPEN:  open_now(); break;
		case AFTER_CLOSE: close_now(); break;
		default: break;
	}

}

// The Open and Save dialogs list a big folder after undo's memory,
// which can move it (zdialog.h, arena_moved). The levels are offsets.
static void undo_moved(void *base, void *user) {
	(void)user;
	if (undo.mem) undo.mem = base;
}

// Undo's memory, once at start (see UNDO_WANT). draw itself never
// resizes it; only a dialog does, and says so (undo_moved()).
static void undo_init(void) {

	static const uint32_t want[] = { UNDO_WANT, UNDO_LESS };
	void *base = NULL;
	uint32_t got = 0;

	for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++) {
		if (z_arena_set(want[i], &base, &got) == Z_ARENA_OK && base && got)
			break;
		base = NULL;
		got = 0;
	}

	draw_undo_init(&undo, PAGE(canvas), PAGE(spare), base, got);

	if (base) printf("draw: undo: %lu KB of arena\n", (unsigned long)(got / 1024));
	else printf("draw: undo: no arena, one level\n");

}

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

// Handles the messages that belong to this app's own window, from
// both the main loop and -- through z_dialog_ctx_t -- from inside any
// dialog that happens to be open.
//
// One function for both is not a convenience: while a dialog is up wm
// carries on asking THIS window to redraw and blocks waiting for the
// ack, so an app that only serviced redraws from its main loop would
// freeze the screen every time it opened a dialog. See zdialog.h.
static void forward_msg(z_msg_t *msg, void *user) {

	(void)user;

	switch (msg->subject) {

		// The part of this window not covered by the windows in front
		// of it. Confines every subsequent draw to it -- see
		// z_win_apply_clip() in zwin.c. The ack it sends is not
		// optional: wm waits for it when a region narrows.
		case Z_WM_SET_CLIP:
			z_win_apply_clip(&win, &msg->obj);
			break;

		case Z_WM_REDRAW:

			if (msg->obj.type != Z_UINT32) break;

			// Only ours -- a dialog's own redraws are handled inside
			// zdialog.c, and this is also the message that can arrive
			// while a dialog is still being created.
			if (z_win_redraw_id(msg->obj.val.uint32) != win.id) break;

			z_win_apply_redraw(&win, msg->obj.val.uint32);
			repaint();
			z_win_redraw_done(&win);

			break;

		case Z_WM_WINDOW_MOVED:

			// No layout() -- moving doesn't change our size, and
			// every rect we hold is content-relative.
			z_win_parse_rect(&win, &msg->obj);
			break;

		case Z_WM_WINDOW_RESIZED:

			// Arrives BEFORE the Z_WM_REDRAW that follows a resize
			// (zwm.h guarantees that ordering), so the layout is
			// already correct by the time we're asked to repaint at
			// the new size.
			z_win_apply_resized(&win, &msg->obj);
			layout();
			break;

		default:
			break;

	}

}

// ---------------------------------------------------------------
// printing
// ---------------------------------------------------------------

static z_print_t printing;

// The print dialog, then the page as one picture: a 4:3 page, fitted to
// the paper and turned if it fits better (zpage.h). The picture is a
// mask over black: white where the page is white (zc_blit_mask() paints
// where its source is 0, and the page's ink is 1).
static void do_print(void) {

	if (busy() || filling || drawing) return;

	char base[64];
	const char *name = filename[0] ? filename : "untitled";
	for (const char *p = name; *p; p++) if (*p == '/') name = p + 1;
	int n = 0;
	put(base, sizeof(base), &n, name, 60);
	for (int i = n - 1; i > 0; i--) if (base[i] == '.') { base[i] = 0; break; }

	z_print_opts_t o;
	z_print_opts_init(&o);
	o.npages = 1;
	if (!z_dialog_print(&dlg_ctx, "Print", last_dir, base, &o)) return;

	if (!z_print_begin(&printing, base)) {
		z_dialog_confirm(&dlg_ctx, "Print", "The print job could not\nbe made in " Z_DIR_TMP ".",
			Z_DIALOG_OK_CANCEL);
		return;
	}
	z_print_opts_job(&o, &printing.w);

	zpage_t pg;
	zpage_custom(&pg, ZPAGE_RATIO, 4, 3, true);
	zc_xf_t xf;
	zc_target_t *t = zp_page_begin(&printing.w, &pg, &xf);
	if (t) {
		zc_fix x0, y0, x1, y1;
		zc_xf_apply(&xf, 0, 0, &x0, &y0);
		zc_xf_apply(&xf, (zc_fix)pg.gw << 8, (zc_fix)pg.gh << 8, &x1, &y1);
		if (x1 < x0) { zc_fix k = x0; x0 = x1; x1 = k; }
		if (y1 < y0) { zc_fix k = y0; y0 = y1; y1 = k; }
		zc_paint_t black = zc_black(), white = zc_white();
		zc_fill_rect_dev(t, x0 >> 8, y0 >> 8, (x1 - x0) >> 8, (y1 - y0) >> 8, &black);
		zc_blit_mask(t, PAGE(canvas), CANVAS_WORDS, CANVAS_W, CANVAS_H,
			x0, y0, x1 - x0, y1 - y0, &white);
		zp_page_end(&printing.w);
	}

	// print shows its own window from here: progress, Cancel, how it ended
	if (!z_print_end(&printing))
		z_dialog_confirm(&dlg_ctx, "Print", printing.result == Z_PRINT_E_GONE ?
			"print is not on the card." : "The print job could not\nbe written.",
			Z_DIALOG_OK_CANCEL);

}

// A tool by its key, or by 1..9; [ and ] the pattern before and after.
// True if it was one.
static bool tool_key(uint32_t k, uint32_t mods) {

	if (mods & (Z_KBD_MOD_CTRL | Z_KBD_MOD_ALT)) return false;
	int t = -1;
	if (k >= '1' && k <= '0' + TOOL_COUNT) t = (int)(k - '1');
	for (int i = 0; i < TOOL_COUNT && t < 0; i++) if ((uint32_t)tool_keys[i] == k) t = i;
	if (t >= 0) {
		if (drawing || filling) return true;
		cur_tool = (tool_t)t;
		z_widget_select(&wset, W_TOOL0 + t);
		z_widget_draw_all(&wset, false);
		z_speak(tool_names[t], Z_TTS_F_INTERRUPT);
		return true;
	}
	if (k == '[' || k == ']') {
		cur_pattern = (cur_pattern + (k == ']' ? 1 : Z_PATTERN_COUNT - 1)) % Z_PATTERN_COUNT;
		z_widget_select(&wset, W_PAT0 + cur_pattern);
		z_widget_draw_all(&wset, false);
		return true;
	}
	return false;

}

// Arrow keys, PageUp/PageDown, Home/End: pan the page. True if one.
static bool pan_key(uint32_t k) {
	int dx = 0, dy = 0;
	switch (k) {
		case Z_KEY_LEFT:  dx = -32; break;
		case Z_KEY_RIGHT: dx = 32; break;
		case Z_KEY_UP:    dy = -32; break;
		case Z_KEY_DOWN:  dy = 32; break;
		case Z_KEY_PAGEUP:  dy = -view_h; break;
		case Z_KEY_PAGEDOWN:  dy = view_h; break;
		case Z_KEY_HOME:  dx = -CANVAS_W; dy = -CANVAS_H; break;
		case Z_KEY_END:   dx = CANVAS_W; dy = CANVAS_H; break;
		default: return false;
	}
	bool moved = false;
	if (dx && z_scrollbar_set_value(&hsb, scroll_x + dx)) { scroll_x = (int)hsb.value; moved = true; }
	if (dy && z_scrollbar_set_value(&vsb, scroll_y + dy)) { scroll_y = (int)vsb.value; moved = true; }
	if (moved) scroll_view();
	return true;
}

// Ctrl+letter. True if it was one of ours.
//
// Ctrl+letter arrives as its control code, 0x01..0x1a (zkbd.h), with
// the modifier set -- hence checking the modifier rather than the code
// alone, which Enter and Tab also produce. The letter itself is taken
// too, as sheet does, in case a keymap delivers that. Same keys as text
// and sheet. New, Open and Save go the way the title icons do, so the
// unsaved-changes question and the one-file-job-at-a-time rule apply
// the same.
static bool ctrl_key(uint32_t k, uint32_t mods) {

	if (!(mods & Z_KBD_MOD_CTRL)) return false;

	bool shift = (mods & Z_KBD_MOD_SHIFT) != 0;
	if (k >= 'a' && k <= 'z') k -= 'a' - 1;
	else if (k >= 'A' && k <= 'Z') k -= 'A' - 1;

	switch (k) {
		case 0x0e: do_new(); return true;						// Ctrl+N
		case 0x0f: do_open(); return true;						// Ctrl+O
		case 0x13: start_save(AFTER_NONE, shift); return true;	// Ctrl+S, +Shift: save as
		case 0x1a: do_undo(shift); return true;					// Ctrl+Z, +Shift: redo
		case 0x19: do_undo(true); return true;					// Ctrl+Y
		case 0x10: do_print(); return true;						// Ctrl+P
		default: return false;
	}

}

// F1 (docs/app_help.md). The manual is docs/draw_app.md.
static void show_help(void) {
	static const char help[] =
		"Paint in black and white on a 640x480 page.\n\n"
		"Left column\tPick a tool: pencil, brush, eraser, fill, line, "
		"rectangle, filled rectangle, oval, filled oval\n"
		"Keys\tP B E F L R O pick them too; Shift+R and Shift+O the "
		"filled shapes; or 1 to 9\n"
		"Bottom row\tPick the pattern for the brush, the fill and the "
		"filled shapes ([ and ] step through them)\n"
		"Drag\tDraw with the tool\n"
		"Shift\tWhile dragging: lines straight or at 45 degrees, "
		"squares, circles\n"
		"Fill\tClick inside a shape: it fills with the pattern\n"
		"Arrows\tPan the page (or the scrollbars, or the wheel)\n"
		"Title icons\tNew, open, save, print\n"
		"Ctrl+N/O/S\tNew, open, save (Ctrl+Shift+S: save as)\n"
		"Ctrl+P\tPrint\n"
		"Ctrl+Z\tUndo the last stroke, shape or fill\n"
		"Ctrl+Y\tRedo (also Ctrl+Shift+Z)\n"
		"Esc\tStop a fill, a load or a save: the drawing and the "
		"file stay as they were (also Ctrl+C)\n\n"
		"The pencil draws white when you start on black.";
	z_dialog_help(&dlg_ctx, "Draw help", help, "draw_app");
}

int main(void) {

	printf("draw: starting\n");

	use_hw_blit = z_fb_hw_blit_mem_available();
	printf("draw: canvas blit path: %s\n",
		use_hw_blit ? "hardware (blitter memory copy)" : "software");

	canvas_clear();
	widgets_init();
	undo_init();

	// A file to open, if something launched us with one (the file
	// browser does -- see Z_WM_SET_ARG in zwm.h). Asked for BEFORE the
	// window exists, as view, text and sheet do: waiting for wm's
	// answer reads the mailbox, and zwin_absorb() (zwin.c) takes the
	// window's first SET_CLIP and REDRAW out of it on the way. The
	// REDRAW was then never answered -- draw had painted once already,
	// into a window with no region yet, which draws nothing -- so it
	// came up without its tools and patterns until it was dragged.
	// Loaded by the main loop like any other open, after the dialog
	// context is set up, since a failure is reported through a dialog.
	bool have_launch = z_launch_path_take(launch_path, sizeof(launch_path));

	// Z_WIN_FLAG_RESIZABLE gives us the corner grip;
	// Z_WIN_FLAG_MIN_IS_CREATE pins the minimum size to WIN_W/WIN_H so
	// the tool column and pattern palette can never be shrunk out of
	// the window. See both flags' comments in zwm.h.
	//
	// CLOSE_ICON WITHOUT CLOSE_KILLS_OWNER. This app used to have the
	// killing form, which was correct while it owned exactly one
	// window for its whole lifetime. It no longer does -- a dialog is
	// a window -- and that flag takes down every window of a pid the
	// instant any one of them is clicked closed (see its own warning
	// in zwm.h). It also has to be the non-killing form so that
	// closing with an unsaved drawing gets a chance to ask.
	if (z_win_create_flags(&win, "untitled", WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_RESIZABLE |
		Z_WIN_FLAG_MIN_IS_CREATE |
		Z_WIN_FLAG_NEW_ICON | Z_WIN_FLAG_OPEN_ICON |
		Z_WIN_FLAG_SAVE_ICON | Z_WIN_FLAG_PRINT_ICON) != Z_OK) {
		printf("draw: failed to create window -- is wm running?\n");
		return 1;
	}

	z_scrollbar_init(&vsb, &win, Z_SB_VERT);
	z_scrollbar_init(&hsb, &win, Z_SB_HORZ);

	// The dialogs need somewhere to send messages that aren't theirs
	// -- above all this window's own Z_WM_REDRAW, which wm blocks on
	// an ack for while a dialog is up. See zdialog.h.
	dlg_ctx.parent = &win;
	dlg_ctx.on_msg = forward_msg;
	dlg_ctx.user = NULL;
	dlg_ctx.arena_moved = undo_moved;

	layout();

	update_title();
	repaint();

	if (have_launch) start_load(launch_path);

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

				case Z_WM_WHEEL:
					// 48 pixels a notch, up = back -- the same distance
					// as in view. The scrollbar clamps it to the canvas.
					if (msg.obj.type == Z_UINT32 &&
					    z_scrollbar_set_value(&vsb, scroll_y -
							48 * Z_WM_WHEEL_NOTCHES(msg.obj.val.uint32))) {
						scroll_y = (int)vsb.value;
						scroll_view();
					}
					break;

				case Z_WM_KEY: {
					// F1, Esc / Ctrl+C to stop a load or save, and the
					// Ctrl shortcuts (ctrl_key()): everything else is
					// the mouse.
					if (msg.obj.type != Z_UINT32) break;
					// pressed or let go: what Shift is now (constrain())
					mods_now = Z_WM_UNPACK_KEY_MODIFIERS(msg.obj.val.uint32);
					if (!Z_WM_UNPACK_KEY_PRESSED(msg.obj.val.uint32)) break;
					uint32_t k = Z_WM_UNPACK_KEY_KEYSYM(msg.obj.val.uint32);
					uint32_t mods = mods_now;
					if (k == Z_KEY_HELP) show_help();
					else if (k == 0x1b || (k == 0x03 && (mods & Z_KBD_MOD_CTRL))) {
						if (filling) fill_cancel();
						else stop_io();
					} else if (!ctrl_key(k, mods) && !tool_key(k, mods)) pan_key(k);
					break;
				}

				case Z_WM_MOUSE:
					if (msg.obj.type == Z_UINT32)
						handle_mouse(msg.obj.val.uint32);
					break;

				case Z_WM_TITLEBAR_ICON: {

					if (msg.obj.type != Z_UINT32) break;

					uint32_t v = msg.obj.val.uint32;
					if ((int)Z_WM_UNPACK_TBICON_ID(v) != win.id) break;

					switch (Z_WM_UNPACK_TBICON_KIND(v)) {
						case Z_WM_TBICON_NEW:  do_new(); break;
						case Z_WM_TBICON_OPEN: do_open(); break;
						case Z_WM_TBICON_SAVE: start_save(AFTER_NONE, false); break;
						case Z_WM_TBICON_PRINT: do_print(); break;
						default: break;
					}

					break;

				}

				case Z_WM_CLOSE:

					if (msg.obj.type == Z_UINT32 &&
						(int32_t)msg.obj.val.uint32 == win.id)
						do_close();

					break;

				default:

					forward_msg(&msg, NULL);
					break;

			}

		}

		// Nothing waiting: a slice of a load or save, and straight back
		// to the messages (docs/responsive.md).
		if (busy()) {
			io_step();
			continue;
		}
		if (filling) {
			fill_step();
			continue;
		}

		for (volatile int i = 0; i < 200; i++);	// light throttle

		/* Yield. This loop used to spin, so the app was RUNNABLE
		 * forever and took a full scheduler share from whatever was
		 * in the foreground -- see docs/app_runtime.md. a paint program only changes on input. */
		z_proc_wait(Z_TICK_HZ / 30);
	}

	return 0;

}
