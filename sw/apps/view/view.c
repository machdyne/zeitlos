/*
 * view -- an image viewer
 *
 *   > run wm
 *   > run view
 *
 * or launched from the file browser with a filename, which is how it
 * is normally reached (see Z_WM_SET_ARG in zwm.h).
 *
 * -- layout --
 *
 *   +---------------------------+-+
 *   |                           |V|
 *   |         viewport          | |
 *   |                           | |
 *   +---------------------------+-+
 *   |H                          |g|
 *   +---------------------------+-+
 *
 * The image occupies the viewport; scrollbars pan it when it is
 * larger than the window. The bottom-right square is deliberately
 * left unpainted: that is where wm draws the resize grip
 * (Z_WIN_GRIP_INSET, zwm.h), and a scrollbar or a fill extending into
 * it swallows the grip and the window can no longer be resized.
 *
 * -- the document, and what it costs --
 *
 * Decoded images live in a fixed 640x480 1bpp buffer, exactly like
 * sw/apps/draw's canvas and in exactly the same packing (zbm.h), so
 * getting one onto the screen is a single z_fb_hw_blit_mem() call.
 *
 * That buffer is the whole design. There is no dynamic memory in this
 * system and the kernel pool is 1MB shared between every process
 * (sw/os/mem.h), so nothing larger can exist -- a 640x480 truecolour
 * image would be 900KB on its own. Everything else follows:
 *
 *   - Images larger than the document are DOWNSCALED at decode time
 *     by a power of two, not stored at full size and panned. A
 *     2000x1500 dithered bitmap is 375KB and has nowhere to live.
 *   - Dithering happens during decode, into the document. There is no
 *     greyscale intermediate to re-dither from, so changing the scale
 *     means decoding the file again.
 *
 * The scrollbars therefore pan around the DOCUMENT, which is at most
 * 640x480 -- they matter because the window is nearly always smaller
 * than that, not because the image might be bigger.
 *
 * -- no printf --
 *
 * Deliberate. printf drags in ~100KB of stdio on the newlib toolchain
 * this tree targets (docs/app_runtime.md), which is more than this
 * app and all its decoders put together. Diagnostics go straight out
 * UART0 through dbg() below, which costs about eighty bytes.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zpaths.h"
#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zutf8.h"
#include "../../common/zgfx.h"
#include "../../common/zfont.h"
#include "../../common/zkbd.h"
#include "../../common/zwidget.h"
#include "../../common/zflist.h"
#include "../../common/zdialog.h"
#include "../../common/zfsapp.h"
#include "../../common/zimg.h"
#include "../../common/zimgjob.h"
#if VIEW_SVG
#include "../../common/zsvg.h"
#endif
#include "../../common/zprof.h"
#include "../../common/games/zarcade.h"

#include "vscreen.h"

#define VRAM        ((volatile uint32_t *)0x20000000)
#define VRAM_WPL    (Z_SCREEN_W / 32)

// The document. Matches the framebuffer, and therefore draw's canvas
// -- see zimg.h's Z_IMG_MAX_W/H.
#define DOC_W       Z_IMG_MAX_W
#define DOC_H       Z_IMG_MAX_H
#define DOC_WPL     (DOC_W / 32)

// Initial window size. Small enough to fit alongside the dock on a
// 640x480 screen, large enough to be useful; the window is resizable.
// Not its minimum (no Z_WIN_FLAG_MIN_IS_CREATE): the screen face makes
// the window 320x240 inside, smaller than this.
#define WIN_W       420
#define WIN_H       300

// How far the arrow keys pan, in pixels. A whole line of the 5x8 font
// plus a little, which is about the smallest step that still feels
// like it did something.
#define SCROLL_STEP 16

// ASCII escape. There is no Z_KEY_ESCAPE in zkbd.h because keysyms
// below 0x100 are ordinary ASCII (see that file's "keysyms" comment),
// and escape has an ASCII code of its own.
#define KEY_ESCAPE  27

// Two documents: the picture on screen, and the next one being decoded
// into the other, so stopping a load (Esc) leaves the picture that was
// there. `doc` is the one shown -- the new one while it loads, so its
// rows appear as they are decoded -- and `prev` what to go back to.
static uint32_t doc_mem[2][DOC_H][DOC_WPL] __attribute__((section(".bss")));
static uint32_t (*doc)[DOC_WPL] = doc_mem[0];

// -- loading without stopping (docs/responsive.md) --
//
// A picture is decoded a slice at a time from the main loop: zimgjob
// reads the file a chunk a step and runs the decoder in a coroutine,
// which dithers straight into the document (no grey image is held).
// The decoder's stack, its state and the chunk live in scratch.dec; on
// the board it uses about 2KB of the stack at most (PNG), so 8KB. They
// share their bytes with an SVG's text, which is only held while a
// drawing is rendered -- never while a picture loads.
#define DEC_STACK   (8 * 1024)
static union {
	uint8_t		dec[Z_IMGJOB_DOC_MEM_STACK(DEC_STACK)];
#if VIEW_SVG
	char		svg_text[VIEW_SVG_MAX];
#endif
} scratch __attribute__((section(".bss"), aligned(8)));
static z_imgjob_t job;
static bool loading;
static uint32_t shown_at;			// when the rows so far were last drawn
static int shown_rows;
static int shown_pct;				// in the title, in tens
static struct {						// what to go back to on Esc
	uint32_t (*doc)[DOC_WPL];
	int w, h, sx, sy;
} prev;
#if VIEW_INSTRUMENT
static uint32_t load_t0;
#endif

static z_win_t win;
static z_scrollbar_t vsb, hsb;
static z_dialog_ctx_t dlg_ctx;

// The file source. 1KB of buffer inside, which is why it is a single
// static rather than a local -- see z_img_file_t in zimg.h.
static z_img_file_t src __attribute__((section(".bss")));

static char filename[Z_FS_PATH_MAX];
static char last_dir[80] = Z_DIR_HOME;
static char title_buf[48];

// What is currently in `doc`. img_w/img_h are 0 when nothing is
// loaded, which is what every path tests for.
static int img_w, img_h;
static z_img_fmt_t img_fmt;
static bool img_ordered;

// content-relative viewport
static int view_x, view_y, view_w, view_h;
static int scroll_x, scroll_y;

static bool use_hw_blit;

// -- the screen face (vscreen.h; view is a dual app, docs/app_kinds.md) --
//
// F10, the colour icon or F11 lend the window to the arcade runtime for
// the picture on a 320x240 screen in colour; F10, the icon, a resize or
// Esc give it back. The screen's picture is made from the file again,
// in colour, once per picture (an SVG's from the one-bit document).
static char screen_for[Z_FS_PATH_MAX];
static int screen_w, screen_h;
static bool quit_now;
static void do_open(void);

// -- diagnostics --
//
// Straight to the 16550 at 0xf0000000 (reg_uart0_* in zeitlos.h).
// Polls LSR bit 5 (transmit holding register empty) exactly as
// sw/os/uart.c's tx_pump() does.

static void dbg_putc(char c) {
	while (!(reg_uart0_lsr & 0x20));
	reg_uart0_data = (uint8_t)c;
}

static void dbg(const char *s) {
	while (*s) {
		if (*s == '\n') dbg_putc('\r');
		dbg_putc(*s++);
	}
}

static void dbg_dec(int32_t v) {

	char tmp[12];
	int i = 0;

	if (v < 0) { dbg_putc('-'); v = -v; }
	if (v == 0) { dbg_putc('0'); return; }

	while (v > 0 && i < (int)sizeof(tmp)) { tmp[i++] = '0' + (v % 10); v /= 10; }
	while (i > 0) dbg_putc(tmp[--i]);

}

#if Z_PROF

// Left-pads a decimal to `w` columns so the report lines up without
// printf. Width, not precision -- anything wider simply overflows the
// column rather than being truncated, since a number that large is
// exactly the one worth seeing.
static void dbg_col(uint32_t v, int w) {

	uint32_t t = v;
	int n = 1;

	while (t >= 10) { t /= 10; n++; }
	while (n++ < w) dbg_putc(' ');

	dbg_dec((int32_t)v);

}

static const char *const prof_names[Z_IMGP_COUNT] = {
	"read", "entropy", "idct", "pixel", "dither", "blit"
};

/*
 * One report per image loaded.
 *
 * `min` is the number to optimise against -- the cheapest observed
 * call, i.e. the phase's cost when it was not interrupted by the
 * scheduler. `avg` includes whatever of another process's timeslice
 * landed in the middle, so a large avg/min ratio means "interrupted
 * often", not "expensive". See zprof.h.
 *
 * ipc is printed per-mille because there is no floating point here
 * and 0.17 would otherwise print as 0. Read 170 as 0.170.
 *
 *   ipc near 170    executing many instructions -- do less work.
 *   ipc well under  stalled on the bus, not busy. This SOC caches
 *                   instruction fetches only (rtl/cache.v), so every
 *                   data access is ~11 cycles on SDRAM and ~63 on
 *                   PSRAM. The fix is fewer memory transactions, or
 *                   gateware -- not a cleverer algorithm.
 */
static void prof_report(void) {

	int i;

	dbg("view: profile\n");
	dbg("  phase     calls      cycles       min       avg   ipc/1000\n");

	for (i = 0; i < Z_IMGP_COUNT; i++) {

		z_prof_slot_t *sl = &z_prof_slot[i];

		if (!sl->calls) continue;

		dbg("  ");
		dbg(prof_names[i]);
		{
			int pad = 8 - (int)strlen(prof_names[i]);
			while (pad-- > 0) dbg_putc(' ');
		}

		dbg_col(sl->calls, 7);
		dbg_col(sl->cyc, 12);
		dbg_col(sl->min, 10);
		dbg_col(sl->cyc / sl->calls, 10);
		dbg_col(sl->cyc ? (uint32_t)(((uint64_t)sl->instr * 1000u) / sl->cyc) : 0, 10);
		dbg("\n");

	}

}

#endif

// -- small string helpers --
//
// Hand-rolled rather than snprintf, for the reason in the file header.

static int str_copy(char *dst, int cap, const char *src_s) {

	int i = 0;

	if (cap <= 0) return 0;
	while (src_s[i] && i < cap - 1) { dst[i] = src_s[i]; i++; }
	dst[i] = 0;

	return i;

}

static int str_append(char *dst, int cap, int at, const char *s) {

	int i = at;

	while (*s && i < cap - 1) dst[i++] = *s++;
	dst[i] = 0;

	return i;

}

static int str_append_dec(char *dst, int cap, int at, int v) {

	char tmp[12];
	int i = 0;

	if (v == 0) return str_append(dst, cap, at, "0");
	if (v < 0) { at = str_append(dst, cap, at, "-"); v = -v; }

	while (v > 0 && i < (int)sizeof(tmp)) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
	while (i > 0 && at < cap - 1) dst[at++] = tmp[--i];
	dst[at] = 0;

	return at;

}

static const char *basename_of(const char *path) {

	const char *b = path;

	for (const char *p = path; *p; p++)
		if (*p == '/') b = p + 1;

	return b;

}

static void remember_dir(const char *path) {

	int last = 0;
	int i;

	for (i = 0; path[i]; i++) if (path[i] == '/') last = i;

	if (last == 0) { last_dir[0] = '/'; last_dir[1] = 0; return; }

	for (i = 0; i < last && i < (int)sizeof(last_dir) - 1; i++)
		last_dir[i] = path[i];
	last_dir[i] = 0;

}

// -- title --
//
// "name 320x240 GIF", truncated to fit. Rebuilt only when something
// actually changed: z_win_set_title() is fire-and-forget and makes wm
// repair the titlebar, which forces every window underneath to
// repaint (see zwin.h), so sending it on every redraw would be
// visibly expensive.

static void update_title(void) {

	char next[sizeof(title_buf)];
	int n;

	if (loading) {
		// "name 40%  Esc stops": in tens, since each title costs a
		// repaint of the windows below it
		n = (int)z_utf8_copy(next, sizeof(next), basename_of(job.path));
		n = str_append(next, sizeof(next), n, " ");
		n = str_append_dec(next, sizeof(next), n, shown_pct * 10);
		str_append(next, sizeof(next), n, "%  Esc stops");
	} else if (!img_w) {
		str_copy(next, sizeof(next), "view");
	} else {
		// Cut at a character boundary: a long file name is UTF-8.
		n = (int)z_utf8_copy(next, sizeof(next), basename_of(filename));
		n = str_append(next, sizeof(next), n, " ");
		n = str_append_dec(next, sizeof(next), n, img_w);
		n = str_append(next, sizeof(next), n, "x");
		n = str_append_dec(next, sizeof(next), n, img_h);
		n = str_append(next, sizeof(next), n, " ");
		str_append(next, sizeof(next), n,
			img_fmt == Z_IMG_FMT_NONE ? "SVG" : z_img_fmt_name(img_fmt));
	}

	if (!strcmp(next, title_buf)) return;

	str_copy(title_buf, sizeof(title_buf), next);
	z_win_set_title(&win, title_buf);

}

// -- layout --
//
// Every content-relative rectangle is computed here and nowhere else.
// Called at startup and on every resize.

static void layout(void) {

	int cw = z_win_content_w(&win);
	int ch = z_win_content_h(&win);

	view_x = 0;
	view_y = 0;
	view_w = cw - Z_SB_THICK;
	view_h = ch - Z_SB_THICK;

	if (view_w < 0) view_w = 0;
	if (view_h < 0) view_h = 0;

	// The vertical bar stops at the horizontal one, and the
	// horizontal one stops at the vertical one, leaving the corner
	// square for wm's resize grip. See the file header.
	z_scrollbar_set_geom(&vsb, cw - Z_SB_THICK, view_y, view_h);
	z_scrollbar_set_geom(&hsb, view_x, ch - Z_SB_THICK, view_w);

	// `total` is the document extent, `page` how much is visible.
	// When the whole image fits, z_scrollbar_draw() draws nothing at
	// all rather than a full-length thumb (zwidget.h), which is
	// exactly the behaviour wanted here.
	if (z_scrollbar_set_range(&vsb, img_h, view_h)) scroll_y = vsb.value;
	if (z_scrollbar_set_range(&hsb, img_w, view_w)) scroll_x = hsb.value;

}

// The viewport in screen coordinates, intersected with the window's
// content area. Also loads this window's visible region into zgfx --
// see z_win_content_rect() in zwin.h -- which is what makes the
// hardware blit below clip correctly when another window overlaps us.
static void view_clip(z_clip_t *out) {

	z_clip_t content;

	z_win_content_rect(&win, &content);

	out->x0 = content.x0 + view_x;
	out->y0 = content.y0 + view_y;
	out->x1 = out->x0 + view_w - 1;
	out->y1 = out->y0 + view_h - 1;

	// A window can be small enough that the viewport is empty. Clamp
	// rather than letting an inverted rect through: everything
	// downstream treats these as inclusive bounds and would happily
	// draw an enormous region for x1 < x0.
	if (out->x1 > content.x1) out->x1 = content.x1;
	if (out->y1 > content.y1) out->y1 = content.y1;

}

static void fill_content_rect(int x, int y, int w, int h, int color) {

	if (w <= 0 || h <= 0) return;
	z_win_fill_rect(&win, x, y, w, h, color);

}

/*
 * Software blit, used only on bitstreams whose blitter predates the
 * memory-source mode (see z_fb_hw_blit_mem_available(), zgfx.h).
 *
 * Word-at-a-time with a shift, not per-pixel: the same 32-pixels-per-
 * memory-access argument the ditherer makes. Source and destination
 * are rarely aligned to each other -- the viewport starts wherever
 * the window happens to be -- so each destination word is assembled
 * from two source words.
 */
static void blit_sw(int src_x, int src_y, int dst_x, int dst_y, int w, int h) {

	int j;

	for (j = 0; j < h; j++) {

		volatile uint32_t *drow = &VRAM[(dst_y + j) * VRAM_WPL];
		const uint32_t *srow = &doc[src_y + j][0];
		int dx0 = dst_x;
		int dx1 = dst_x + w;
		int w0 = dx0 >> 5;
		int w1 = (dx1 - 1) >> 5;
		int wi;

		for (wi = w0; wi <= w1; wi++) {

			int first = wi * 32;			// dest pixel at bit 0
			int sx0 = src_x + (first - dst_x);
			int sw = sx0 >> 5;				// arithmetic shift: floor
			int s = sx0 & 31;
			uint32_t lo = (sw >= 0 && sw < DOC_WPL) ? srow[sw] : 0;
			uint32_t hi = (sw + 1 >= 0 && sw + 1 < DOC_WPL) ? srow[sw + 1] : 0;
			uint32_t val = s ? ((lo >> s) | (hi << (32 - s))) : lo;
			uint32_t mask = 0xFFFFFFFFu;

			if (dx0 > first) mask &= 0xFFFFFFFFu << (dx0 - first);
			if (dx1 < first + 32) mask &= 0xFFFFFFFFu >> (32 - (dx1 - first));

			drow[wi] = (drow[wi] & ~mask) | (val & mask);

		}

	}

}

// Puts the visible part of the document on screen, and paints
// whatever the image doesn't reach as background.
static void repaint_image(void) {

	z_clip_t vc;
	int sx, sy, w, h;

	view_clip(&vc);

	if (vc.x1 < vc.x0 || vc.y1 < vc.y0) return;

	sx = vc.x0;
	sy = vc.y0;

	// How much image there is to show, from the current scroll offset
	w = img_w - scroll_x;
	h = img_h - scroll_y;

	if (w > vc.x1 - vc.x0 + 1) w = vc.x1 - vc.x0 + 1;
	if (h > vc.y1 - vc.y0 + 1) h = vc.y1 - vc.y0 + 1;
	if (w < 0) w = 0;
	if (h < 0) h = 0;

	if (w > 0 && h > 0) {
		Z_PROF_BEGIN(Z_IMGP_BLIT);
		if (!use_hw_blit ||
			!z_fb_hw_blit_mem(doc, DOC_WPL * 4, scroll_x, scroll_y,
				sx, sy, w, h))
			blit_sw(scroll_x, scroll_y, sx, sy, w, h);
		Z_PROF_END(Z_IMGP_BLIT);
	}

	// Background to the right of and below the image. Content-
	// relative, since z_win_fill_rect() is.
	if (w < view_w)
		fill_content_rect(view_x + w, view_y, view_w - w, view_h, 0);
	if (h < view_h)
		fill_content_rect(view_x, view_y + h, w, view_h - h, 0);

}

// Full repaint, in response to Z_WM_REDRAW. wm has already cleared
// the region by the time this runs.
static void repaint(void) {

	repaint_image();

	z_scrollbar_draw(&vsb, true);
	z_scrollbar_draw(&hsb, true);

	if (!img_w && !loading) {
		// Nothing loaded. Say how to load something rather than
		// leaving an empty window that looks broken.
		z_win_draw_text(&win, 8, 8,
			"No image.\n\nUse the open icon in the\ntitlebar to choose a file.",
			1, &z_font_5x8);
	}

}

// -- loading --

static void show_error(const char *what, int rv) {

	dbg("view: ");
	dbg(what);
	dbg(" rv=");
	dbg_dec(rv);
	dbg("\n");

	z_dialog_confirm(&dlg_ctx, "Cannot open", z_img_strerror(rv),
		Z_DIALOG_OK_CANCEL);

}

#if VIEW_SVG

// SVG is TEXT, so unlike the raster formats the whole file is read
// before anything is drawn: a transform on a <g> applies to children
// that appear later, so the document has to be walked as a tree.
//
// That is affordable because SVG files are kilobytes. A file larger
// than the buffer is refused rather than truncated -- half a drawing
// is not a drawing.
static void load_svg(z_img_file_t *src, const char *path) {

	char *text = scratch.svg_text;	// no picture is loading (load_file())
	static z_svg_edge_t edges[VIEW_SVG_EDGES]
		__attribute__((section(".bss")));

	z_svg_t sv;
	int n, rv;

	if (z_img_file_rewind(src) != Z_IMG_OK) {
		show_error("rewind", Z_IMG_E_IO);
		return;
	}

	n = z_img_file_read(src, (uint8_t *)text, VIEW_SVG_MAX);
	if (n <= 0) { show_error("read", Z_IMG_E_IO); return; }

	if (n >= VIEW_SVG_MAX) {
		z_dialog_confirm(&dlg_ctx, "Cannot open",
			"This SVG is larger than\nview can hold.",
			Z_DIALOG_OK_CANCEL);
		return;
	}

	memset(&sv, 0, sizeof(sv));
	sv.doc = text;
	sv.len = (uint32_t)n;
	sv.x = 0;
	sv.y = 0;

	// Rendered at the SCREEN size, not the document's. A vector
	// drawing has no natural pixel size, and the point of it is that
	// it can be drawn at whatever size is wanted -- so it is fitted
	// to the window rather than scaled after the fact like a bitmap.
	sv.w = DOC_W;
	sv.h = DOC_H;
	sv.edges = edges;
	sv.max_edges = VIEW_SVG_EDGES;

	// Into the other document, so a drawing that fails leaves the
	// picture that was there.
	{
		uint32_t (*other)[DOC_WPL] = doc == doc_mem[0] ? doc_mem[1] : doc_mem[0];

		memset(other, 0, sizeof(doc_mem[0]));

		rv = z_svg_render_bitmap(&sv, &other[0][0], DOC_WPL);

		if (rv != Z_SVG_OK) {
			z_dialog_confirm(&dlg_ctx, "Cannot draw",
				z_svg_strerror(rv), Z_DIALOG_OK_CANCEL);
			return;
		}

		doc = other;
	}

	str_copy(filename, sizeof(filename), path);
	remember_dir(filename);
	img_fmt = Z_IMG_FMT_NONE;

	img_w = DOC_W;
	img_h = DOC_H;
	scroll_x = 0;
	scroll_y = 0;
	z_scrollbar_set_value(&vsb, 0);
	z_scrollbar_set_value(&hsb, 0);

	layout();
	update_title();
	repaint();

}

#endif

// Back to the picture that was there before a load began.
static void load_back(void) {

	doc = prev.doc;
	img_w = prev.w;
	img_h = prev.h;
	layout();
	z_scrollbar_set_value(&hsb, prev.sx);
	z_scrollbar_set_value(&vsb, prev.sy);
	scroll_x = hsb.value;
	scroll_y = vsb.value;

}

// Esc: the load stops at once -- the file closed -- and the picture
// that was there comes back.
static void load_stop(void) {

	if (!loading) return;

	z_imgjob_stop(&job);
	z_imgjob_end(&job);
	loading = false;
	load_back();
	update_title();
	repaint();
	dbg("view: load stopped\n");

}

// A slice of the load, from the main loop when no message is waiting.
static void load_step(void) {

	int st = z_imgjob_step(&job, Z_TICK_HZ / 50);
	uint32_t now = z_uptime_ticks();
	int pct;

	// The picture's size is known from its first row: the scrollbars
	// follow it from then on.
	if (job.w && (job.w != img_w || job.h != img_h)) {
		img_w = job.w;
		img_h = job.h;
		layout();
		z_scrollbar_draw(&vsb, true);
		z_scrollbar_draw(&hsb, true);
	}

	if (st == Z_IMGJOB_RUNNING) {

		// The rows so far, a few times a second.
		if (job.rows_done != shown_rows && now - shown_at >= Z_TICK_HZ / 8) {
			shown_rows = job.rows_done;
			shown_at = now;
			repaint_image();
		}

		pct = z_imgjob_permille(&job) / 100;
		if (pct != shown_pct) {
			shown_pct = pct;
			update_title();
		}
		return;

	}

	loading = false;

#if VIEW_INSTRUMENT
	{
		uint32_t t1 = z_uptime_ticks();
		dbg("view: decode ");
		dbg(z_img_fmt_name(job.fmt));
		dbg(" -> ");
		dbg_dec((int32_t)job.w);
		dbg("x");
		dbg_dec((int32_t)job.h);
		dbg(" in ");
		dbg_dec((int32_t)(t1 - load_t0));
		dbg(" ticks (");
		dbg_dec((int32_t)(((t1 - load_t0) * 1000u) / Z_TICK_HZ));
		dbg(" ms)\n");
	}
#endif
#if Z_PROF
	prof_report();
#endif

	if (st != Z_IMGJOB_DONE) {

		int err = job.err;

		// Nothing of it could be shown: the picture that was there
		// stays.
		z_imgjob_end(&job);
		load_back();
		update_title();
		repaint();
		if (err == Z_IMGJOB_E_OPEN)
			z_dialog_confirm(&dlg_ctx, "Cannot open",
				"The file could not be\nopened.", Z_DIALOG_OK_CANCEL);
		else if (err == Z_IMGJOB_E_MEM)
			z_dialog_confirm(&dlg_ctx, "Cannot open",
				"Not enough memory.", Z_DIALOG_OK_CANCEL);
		else
			show_error("decode", err);
		return;

	}

	// A picture cut short is still worth showing -- what there was,
	// with the scroll range matching it -- and says why.
	img_w = job.w;
	img_h = job.h;
	img_fmt = job.fmt;
	img_ordered = job.ordered;

	str_copy(filename, sizeof(filename), job.path);
	remember_dir(filename);

	z_imgjob_end(&job);

	layout();
	update_title();
	repaint();

	if (job.err != Z_IMG_OK) show_error("decode", job.err);

}

static void load_file(const char *path) {

	// 1KB, not the handful of bytes a raster magic number needs.
	//
	// SVG is text: an XML declaration, a DOCTYPE and a generator
	// comment all come before the <svg> tag. Inkscape puts it at
	// offset 114 in an ordinary file, so a 16-byte sniff reported
	// every real SVG as unrecognised.
	//
	// Static rather than on the stack: an app's stack is not the
	// place for a kilobyte that is used once per file open.
	static uint8_t hdr[1024];
	z_img_fmt_t fmt;
	int n;

	// One picture at a time: a load under way gives way to this one.
	load_stop();

	if (z_img_file_open(&src, path) != Z_IMG_OK) {
		z_dialog_confirm(&dlg_ctx, "Cannot open",
			"The file could not be\nopened.", Z_DIALOG_OK_CANCEL);
		return;
	}

	// Sniff on content. Identifying by extension instead would be
	// shorter but gets a surprising number of real files wrong.
	n = z_img_file_read(&src, hdr, sizeof(hdr));

#if VIEW_SVG
	// SVG before the raster sniff, because it is not one of zimg's
	// formats: it is text, and it is rendered rather than decoded.
	if (z_svg_sniff((const char *)hdr, (uint32_t)n)) {
		load_svg(&src, path);
		z_img_file_close(&src);
		return;
	}
#endif

	z_img_file_close(&src);

	fmt = z_img_sniff(hdr, n);

	if (fmt == Z_IMG_FMT_NONE) {
		z_dialog_confirm(&dlg_ctx, "Cannot open",
			"This is not an image file\nview recognises.",
			Z_DIALOG_OK_CANCEL);
		return;
	}

	// The new picture goes into the other document, and is what the
	// window shows while it loads: its rows appear as they are
	// decoded. The one there now is kept to go back to.
	prev.doc = doc;
	prev.w = img_w;
	prev.h = img_h;
	prev.sx = scroll_x;
	prev.sy = scroll_y;
	doc = doc == doc_mem[0] ? doc_mem[1] : doc_mem[0];

	z_prof_reset();
#if VIEW_INSTRUMENT
	load_t0 = z_uptime_ticks();
#endif

	// Cleared by the job, not after failing: a decoder that stops
	// partway leaves what it managed on screen, which is more use than
	// a blank window. The ditherer also relies on this -- rows narrower
	// than the document leave the remaining words untouched (see
	// dither_row_fs() in zimg.c).
	if (!z_imgjob_start_doc(&job, path, &doc[0][0], DOC_WPL, DOC_W, DOC_H,
		scratch.dec, sizeof(scratch.dec))) {
		int err = job.err;
		z_imgjob_end(&job);
		doc = prev.doc;
		z_dialog_confirm(&dlg_ctx, "Cannot open",
			err == Z_IMGJOB_E_MEM ? "Not enough memory."
				: "The file could not be\nopened.", Z_DIALOG_OK_CANCEL);
		return;
	}

	loading = true;
	shown_rows = 0;
	shown_pct = 0;
	shown_at = z_uptime_ticks();

	img_w = img_h = 0;
	scroll_x = scroll_y = 0;
	z_scrollbar_set_value(&vsb, 0);
	z_scrollbar_set_value(&hsb, 0);

	layout();
	update_title();

}

static void do_open(void) {

	char path[80];

	if (!z_dialog_open(&dlg_ctx, last_dir, path, sizeof(path))) return;

	load_file(path);
	repaint();

}

// -- scrolling --

static void scroll_to(int nx, int ny) {

	bool changed = false;

	if (z_scrollbar_set_value(&hsb, nx)) changed = true;
	if (z_scrollbar_set_value(&vsb, ny)) changed = true;

	if (!changed) return;

	scroll_x = hsb.value;
	scroll_y = vsb.value;

	repaint_image();
	z_scrollbar_draw(&vsb, false);
	z_scrollbar_draw(&hsb, false);

}

static bool file_rewind(void *ctx) {
	return z_img_file_rewind((z_img_file_t *)ctx) == Z_IMG_OK;
}

static void screen(bool full) {

	int r;

	if (loading) return;

	if (strcmp(screen_for, filename) || screen_w != img_w || screen_h != img_h) {
		bool ok = false;
		if (img_w && img_fmt != Z_IMG_FMT_NONE &&
		    z_img_file_open(&src, filename) == Z_IMG_OK) {
			vs_src_t s;
			s.read = z_img_file_read;
			s.rewind = file_rewind;
			s.ctx = &src;
			ok = vs_load(&s);
			z_img_file_close(&src);
		}
		if (!ok) {
			if (img_w) vs_from_bits(&doc[0][0], DOC_WPL, img_w, img_h);
			else vs_clear();
		}
		str_copy(screen_for, sizeof(screen_for), filename);
		screen_w = img_w;
		screen_h = img_h;
	}

	r = vs_run(&win, full, filename);
	if (r == VS_QUIT) quit_now = true;
	else if (r == VS_OPEN) do_open();

}

static void handle_key(uint32_t keysym) {

	if (keysym == Z_KEY_HELP) {	// docs/app_help.md
		static const char help[] =
			"An image viewer: BMP, PNM, GIF, JPEG, PNG, ZBM and SVG, "
			"shown in black and white.\n\n"
			"o\tOpen an image\n"
			"F10\tColour: the picture on a 320x240 colour screen\n"
			"F11\tThe same, full screen\n"
			"Esc\tStop loading one\n"
			"Arrows\tScroll\n"
			"PgUp/PgDn\tScroll a page\n"
			"Home\tBack to the top left\n"
			"End\tTo the bottom\n\n"
			"Open with the icon in the title bar, or double-click an "
			"image in files. Large images are scaled down to fit "
			"640x480; drag the scrollbars or use the wheel to pan.";
		z_dialog_help(&dlg_ctx, "View help", help, "view_app");
		return;
	}

	switch (keysym) {

		case Z_KEY_LEFT:     scroll_to(scroll_x - SCROLL_STEP, scroll_y); break;
		case Z_KEY_RIGHT:    scroll_to(scroll_x + SCROLL_STEP, scroll_y); break;
		case Z_KEY_UP:       scroll_to(scroll_x, scroll_y - SCROLL_STEP); break;
		case Z_KEY_DOWN:     scroll_to(scroll_x, scroll_y + SCROLL_STEP); break;
		case Z_KEY_PAGEUP:   scroll_to(scroll_x, scroll_y - view_h); break;
		case Z_KEY_PAGEDOWN: scroll_to(scroll_x, scroll_y + view_h); break;
		case Z_KEY_HOME:     scroll_to(0, 0); break;
		case Z_KEY_END:      scroll_to(scroll_x, img_h); break;

		case 'o':
		case 'O':
			do_open();
			break;

		case Z_KEY_COLOUR:		// the screen face, in the window
			screen(false);
			break;

		case Z_KEY_FULL_SCREEN:	// the screen face, full screen
			screen(true);
			break;

		case KEY_ESCAPE:
			// Stops a picture loading, and leaves the one that was
			// there (docs/view_app.md).
			if (loading) { load_stop(); break; }
			// Otherwise reserved for leaving full-screen mode, which needs wm
			// support that does not exist yet (an undecorated-window
			// flag and a way to suppress the dock). Swallowed rather
			// than ignored so the binding is not quietly claimed by
			// something else in the meantime. See docs/view_app.md.
			break;

		default:
			break;

	}

}

static void handle_mouse(uint32_t packed) {

	int cx, cy;
	uint8_t buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);

	z_win_mouse_content_xy(&win, packed, &cx, &cy);

	// Order matters: a scrollbar that owns the pointer keeps it even
	// when the cursor has wandered outside its rect mid-drag, which
	// is what z_scrollbar_has_pointer() reports. Testing the value
	// change alone would drop the drag the moment the pointer left.
	if (z_scrollbar_has_pointer(&vsb, cx, cy)) {
		if (z_scrollbar_mouse(&vsb, cx, cy, buttons)) {
			scroll_y = vsb.value;
			repaint_image();
		}
		return;
	}

	if (z_scrollbar_has_pointer(&hsb, cx, cy)) {
		if (z_scrollbar_mouse(&hsb, cx, cy, buttons)) {
			scroll_x = hsb.value;
			repaint_image();
		}
		return;
	}

	// Feed both anyway so a drag that started on one and has since
	// left its rect still gets its button-release.
	z_scrollbar_mouse(&vsb, cx, cy, buttons);
	z_scrollbar_mouse(&hsb, cx, cy, buttons);

}

// -- message handling --
//
// One function, used both by the main loop and, through
// z_dialog_ctx_t, by any dialog that happens to be open. Not a
// convenience: while a dialog is up, wm carries on asking THIS window
// to redraw and blocks waiting for the ack (zdialog.h). An app that
// only serviced redraws from its own loop would freeze the screen
// every time it opened a file.

static void forward_msg(z_msg_t *msg, void *user) {

	(void)user;

	switch (msg->subject) {

		case Z_WM_SET_CLIP:
			z_win_apply_clip(&win, &msg->obj);
			break;

		case Z_WM_REDRAW:

			if (msg->obj.type != Z_UINT32) break;

			// Only ours. A dialog's own redraws are handled inside
			// zdialog.c and never reach here, but this is also the
			// message that arrives while a dialog is being created,
			// before it has an id to compare against.
			if (z_win_redraw_id(msg->obj.val.uint32) != win.id) break;

			z_win_apply_redraw(&win, msg->obj.val.uint32);
			repaint();
			z_win_redraw_done(&win);

			break;

		case Z_WM_WINDOW_MOVED:

			// No layout(): moving doesn't change our size, and every
			// rect we hold is content-relative.
			z_win_parse_rect(&win, &msg->obj);
			break;

		case Z_WM_WINDOW_RESIZED:

			// Arrives BEFORE the Z_WM_REDRAW that follows a resize
			// (zwm.h guarantees the ordering), so the layout is
			// already right by the time we repaint.
			z_win_apply_resized(&win, &msg->obj);
			layout();
			break;

		default:
			break;

	}

}

int main(void) {

	char launch_arg[sizeof(filename)];
	bool have_arg;

	dbg("view: starting\n");

	// Explicit, not left to .bss zero-init: that has been shown
	// unreliable on this hardware (docs/app_runtime.md), and a
	// document full of garbage would be blitted to the screen before
	// anything else had a chance to run.
	memset(doc_mem, 0, sizeof(doc_mem));

	img_w = img_h = 0;
	img_fmt = Z_IMG_FMT_NONE;
	img_ordered = false;
	scroll_x = scroll_y = 0;
	src.handle = -1;

	// Taken BEFORE the window is created, so the window can be made
	// with its final title. z_launch_arg_take() blocks on wm's reply
	// via z_msg_wait(), which discards anything else that arrives
	// meanwhile -- safe here only because nothing else is in flight
	// yet. See zwin.h.
	have_arg = z_launch_path_take(launch_arg, sizeof(launch_arg));

	// CLOSE_ICON without CLOSE_KILLS_OWNER, deliberately: this app
	// owns more than one window at a time as soon as a dialog opens,
	// and the killing form takes every window of a pid down the
	// instant any one of them is closed (zwm.h).
	// The screen face's flags too: game mode (F11 is view's: the
	// picture full screen), the colour window and its icon.
	ar_window_colour(true);
	if (z_win_create_flags(&win, "view", WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_RESIZABLE |
		Z_WIN_FLAG_OPEN_ICON |
		vs_window_flags()) != Z_OK) {
		dbg("view: failed to create window -- is wm running?\n");
		return 1;
	}

	str_copy(title_buf, sizeof(title_buf), "view");

	// Probed once. The same binary runs on bitstreams whose blitter
	// has no memory-source mode, where this silently returns false
	// and blit_sw() takes over -- see z_fb_hw_blit_mem() in zgfx.h.
	use_hw_blit = z_fb_hw_blit_mem_available();
	if (!use_hw_blit) dbg("view: no hw mem blit, using software path\n");

	z_scrollbar_init(&vsb, &win, Z_SB_VERT);
	z_scrollbar_init(&hsb, &win, Z_SB_HORZ);

	dlg_ctx.parent = &win;
	dlg_ctx.on_msg = forward_msg;
	dlg_ctx.user = NULL;

	layout();

	if (have_arg) load_file(launch_arg);

	update_title();
	repaint();

	for (;;) {

		z_msg_t msg;

		// Drain the whole queue each pass rather than one message per
		// iteration -- see Z_WM_MOUSE's own note in zwm.h on why
		// handling one per loop makes an app fall progressively
		// behind the real cursor.
		while (z_msg_read(&msg) == Z_OK) {

			switch (msg.subject) {

				case Z_WM_KEY:

					if (msg.obj.type != Z_UINT32) break;
					if (!Z_WM_UNPACK_KEY_PRESSED(msg.obj.val.uint32)) break;

					handle_key(Z_WM_UNPACK_KEY_KEYSYM(msg.obj.val.uint32));
					break;

				case Z_WM_WHEEL:
					// Three arrow steps a notch, up = back, like the
					// other apps; scroll_to() clamps to the image.
					if (msg.obj.type == Z_UINT32)
						scroll_to(scroll_x, scroll_y -
							3 * SCROLL_STEP * Z_WM_WHEEL_NOTCHES(msg.obj.val.uint32));
					break;

				case Z_WM_MOUSE:

					if (msg.obj.type == Z_UINT32)
						handle_mouse(msg.obj.val.uint32);
					break;

				case Z_WM_TITLEBAR_ICON: {

					uint32_t v;

					if (msg.obj.type != Z_UINT32) break;

					v = msg.obj.val.uint32;
					if ((int)Z_WM_UNPACK_TBICON_ID(v) != win.id) break;

					if (Z_WM_UNPACK_TBICON_KIND(v) == Z_WM_TBICON_OPEN)
						do_open();
					else if (Z_WM_UNPACK_TBICON_KIND(v) == Z_WM_TBICON_COLOUR)
						screen(false);

					break;

				}

				case Z_WM_CLOSE:

					if (msg.obj.type == Z_UINT32 &&
						(int32_t)msg.obj.val.uint32 == win.id) {
						// its file closed: nothing sweeps a handle
						// at exit (zimg.h)
						if (loading) { z_imgjob_stop(&job); loading = false; }
						z_win_destroy(&win);
						dbg("view: exiting\n");
						return 0;
					}

					break;

				default:

					forward_msg(&msg, NULL);
					break;

			}

		}

		if (quit_now) {
			z_win_destroy(&win);
			dbg("view: exiting\n");
			return 0;
		}

		// A picture loading: a slice of it, then back for messages.
		if (loading) { load_step(); continue; }

		// Yield. Without this the loop spins, the process stays
		// RUNNABLE forever and takes a full scheduler share from
		// whatever is in the foreground -- see docs/app_runtime.md.
		//
		// z_proc_wait() rather than z_msg_wait(): the latter waits
		// for one specific subject and DISCARDS everything else
		// (zeitlos.h), which here would throw away redraws and
		// clip updates. A viewer only changes on input, so the same
		// ~30Hz poll draw uses is more than enough.
		z_proc_wait(Z_TICK_HZ / 30);

	}

	return 0;

}
