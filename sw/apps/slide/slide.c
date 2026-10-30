/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * slide -- cards, slides and pages: draw them, show them full screen,
 * and (in later phases) print and script them. docs/slide_app.md.
 *
 * This file is the window: it turns wm's messages into edit.c's calls
 * and draws what they leave behind. The card is rendered (render.c)
 * into an off-screen bitmap at the view's scale, only where something
 * changed; the selection, the caret and drag outlines are drawn over a
 * copy of it, so they never disturb the rendering; and the copy is put
 * on the screen with the blitter.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zwin.h"
#include "../../common/zwm.h"
#include "../../common/zgfx.h"
#include "../../common/zfont.h"
#include "../../common/zwidget.h"
#include "../../common/zdialog.h"
#include "../../common/zedit.h"
#include "../../common/zfsapp.h"
#include "../../common/zkbd.h"
#include "../../common/zsoc.h"
#include "../../common/zpaths.h"
#include "../../common/zutf8.h"
#include "../../common/zcanvas.h"
#include "../../common/zcfont.h"

#include "../../common/zpool.h"
#include "../../common/zprint.h"
#include "../../common/zpsvg.h"
#include "deck.h"
#include "deckio.h"
#include "render.h"
#include "edit.h"
#include "show.h"
#include "printjob.h"
#include "act.h"
#include "script.h"
#include "../../common/zspeak.h"

// -- layout --------------------------------------------------------------------

// A deck run as an app (deck->app, F7) shows just the card: no tools,
// no bar, no margin.
static bool app_mode;
static void set_app_mode(bool on);

#define TOOL_W      (app_mode ? 0 : 24)		// the tool column
#define TOOL_H      20
#define MARGIN      (app_mode ? 0 : 6)
#define BAR_H       (app_mode ? 0 : 30)		// the property strip and the status line
#define SW_SIZE     11		// a fill swatch
#define FONT        (&z_font_5x8)

static z_win_t win;
static z_dialog_ctx_t dlg;

#define BAR_Y()      (z_win_content_h(&win) - BAR_H)
static editor_t ed;

// The card is drawn at scale16 into "card pixels" (the whole card,
// card_pw x card_ph). The window shows view_w x view_h of them starting
// at (scroll_x, scroll_y), at content position (card_x, card_y): the
// whole card when it fits, a part of it when zoomed in. cbuf and dbuf
// hold just that part -- a zc target whose origin is the scroll.
static int card_x, card_y;		// the view's top-left, content coordinates
static int view_w, view_h;		// the view, pixels
static int card_pw, card_ph;	// the whole card at this scale, pixels
static int scroll_x, scroll_y;	// the view's top-left, card pixels
static int32_t scale16;			// pixels per unit, 16.16
static int zoom;				// 0: fit the window (at most 100%); else 100, 200, 400
static uint32_t *cbuf, *dbuf;	// the rendered view; it plus overlays
static int bwpl, bw, bh;		// their shape
static z_scrollbar_t sb_h, sb_v;
static bool sb_h_on, sb_v_on;

static uint8_t mods_now;
static char path[200];			// the deck's file, "" untitled
static char last_dir[160] = Z_DIR_HOME;

enum { MODE_EDIT = 0, MODE_SORTER, MODE_COMMAND };
static int mode;

static char cmd_buf[200];
static z_edit_t cmd_field;

// the last overlay drawn, device pixels in the card (x1 < x0: none)
static int ov_x0, ov_y0, ov_x1 = -1, ov_y1;

// -- small text helpers (no printf: newlib's costs ~100KB) ----------------------

typedef struct { char *p; int n, cap; } str_t;

static void s_add(str_t *s, const char *t) {
	while (*t && s->n < s->cap - 1) s->p[s->n++] = *t++;
	s->p[s->n] = 0;
}

static void s_int(str_t *s, long v) {
	char t[16];
	int k = 0;
	unsigned long u = v < 0 ? (unsigned long)-v : (unsigned long)v;
	do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u && k < 15);
	if (v < 0) t[k++] = '-';
	while (k && s->n < s->cap - 1) s->p[s->n++] = t[--k];
	s->p[s->n] = 0;
}

static void copy_str(char *dst, int cap, const char *src) {
	int i = 0;
	while (src && src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
	dst[i] = 0;
}

// -- memory ------------------------------------------------------------------------

static bool mem_setup(void) {
	static const uint32_t sizes[] = { 6u << 20, 3u << 20, 3u << 19, 3u << 18 };
	unsigned i;
	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		void *base = NULL;
		uint32_t got = 0;
		if (z_arena_set(sizes[i], &base, &got) == Z_ARENA_OK && base && got >= sizes[i]) {
			pool_init(base, got);
			return true;
		}
	}
	return false;
}

static uint8_t *load_file(const char *p, uint32_t *len) {
	int n = fs_size((char *)p);
	uint8_t *b;
	if (n <= 0) return NULL;
	b = pool_alloc((size_t)n + 1);
	if (!b) return NULL;
	if (fs_read_file((char *)p, (char *)b, n) != n) { pool_free(b); return NULL; }
	b[n] = 0;
	*len = (uint32_t)n;
	return b;
}

// -- coordinates ----------------------------------------------------------------------

static zc_xf_t view_xf(void) { return zc_xf_scale(scale16, 0, 0); }

static int to_units(int px) {
	return (int)((((int64_t)px << 16) + scale16 / 2) / scale16);
}

static int to_px(int u) {
	return (int)(((int64_t)u * scale16 + 32768) >> 16);
}

// -- buffers and layout ----------------------------------------------------------------

#define ZOOM_MAX 400

static void layout(void) {
	int cw = z_win_content_w(&win), ch = z_win_content_h(&win);
	int aw = cw - TOOL_W - 2 * MARGIN, ah = ch - BAR_H - 2 * MARGIN;
	int32_t sx, sy, s;
	deck_t *d = ed.d;
	if (!sb_h.win) {
		z_scrollbar_init(&sb_h, &win, Z_SB_HORZ);
		z_scrollbar_init(&sb_v, &win, Z_SB_VERT);
	}
	if (aw < 32) aw = 32;
	if (ah < 32) ah = 32;
	sx = (int32_t)(((int64_t)aw << 16) / d->page.gw);
	sy = (int32_t)(((int64_t)ah << 16) / d->page.gh);
	s = sx < sy ? sx : sy;
	if (s > ZC_XF_ONE) s = ZC_XF_ONE;		// fitted: never larger than 100%
	if (zoom) s = (int32_t)(((int64_t)zoom << 16) / 100);
	scale16 = s;
	card_pw = to_px(d->page.gw);
	card_ph = to_px(d->page.gh);
	// scrollbars where the card is larger than the room for it (one
	// takes room from the other's direction)
	sb_h_on = sb_v_on = false;
	if (card_pw > aw) { sb_h_on = true; ah -= Z_SB_THICK; }
	if (card_ph > ah) {
		sb_v_on = true;
		aw -= Z_SB_THICK;
		if (!sb_h_on && card_pw > aw) { sb_h_on = true; ah -= Z_SB_THICK; }
	}
	view_w = card_pw < aw ? card_pw : aw;
	view_h = card_ph < ah ? card_ph : ah;
	if (scroll_x > card_pw - view_w) scroll_x = card_pw - view_w;
	if (scroll_y > card_ph - view_h) scroll_y = card_ph - view_h;
	if (scroll_x < 0) scroll_x = 0;
	if (scroll_y < 0) scroll_y = 0;
	card_x = TOOL_W + MARGIN + (aw - view_w) / 2;
	card_y = MARGIN + (ah - view_h) / 2;
	if (card_x < TOOL_W + MARGIN) card_x = TOOL_W + MARGIN;
	if (card_y < MARGIN) card_y = MARGIN;
	// A deck run as an app: its card at the window's corner, whatever
	// the window's size -- the same place before wm has made the window
	// the card's size and after (a click meant for a button lands on it
	// either way; a demo's pointer relies on it)
	if (app_mode) card_x = card_y = 0;
	if (sb_h_on) {
		z_scrollbar_set_geom(&sb_h, card_x, card_y + view_h + 2, view_w);
		z_scrollbar_set_range(&sb_h, card_pw, view_w);
		z_scrollbar_set_value(&sb_h, scroll_x);
	}
	if (sb_v_on) {
		z_scrollbar_set_geom(&sb_v, card_x + view_w + 2, card_y, view_h);
		z_scrollbar_set_range(&sb_v, card_ph, view_h);
		z_scrollbar_set_value(&sb_v, scroll_y);
	}
	if (!cbuf || bw != view_w || bh != view_h) {
		int wpl = (view_w + 31) / 32;
		pool_free(cbuf);
		pool_free(dbuf);
		cbuf = pool_alloc((size_t)wpl * view_h * 4 + 8);
		dbuf = pool_alloc((size_t)wpl * view_h * 4 + 8);
		bwpl = wpl;
		bw = view_w;
		bh = view_h;
		edit_damage_all(&ed);
	}
}

// -- the card on screen ------------------------------------------------------------------

static void card_target(zc_target_t *t, uint32_t *b) {
	zc_target_bits(t, b, bwpl, bw, bh);
	zc_target_origin(t, scroll_x, scroll_y);
}

static void units_rect_to_px(int x0, int y0, int x1, int y1, int *a, int *b, int *c, int *d) {
	*a = to_px(x0) - 2;
	*b = to_px(y0) - 2;
	*c = to_px(x1) + 3;
	*d = to_px(y1) + 3;
}

static void xor_dotted(zc_target_t *t, int x0, int y0, int x1, int y1) {
	int x, y;
	zc_paint_t x_ = zc_xor();
	for (x = x0; x <= x1; x += 2) {
		zc_fill_rect_dev(t, x, y0, 1, 1, &x_);
		if (y1 != y0) zc_fill_rect_dev(t, x, y1, 1, 1, &x_);
	}
	for (y = y0 + 2; y < y1; y += 2) {
		zc_fill_rect_dev(t, x0, y, 1, 1, &x_);
		if (x1 != x0) zc_fill_rect_dev(t, x1, y, 1, 1, &x_);
	}
}

static void xor_line(zc_target_t *t, int x0, int y0, int x1, int y1) {
	zc_paint_t x_ = zc_xor();
	zc_line_dev(t, ZC_I(x0), ZC_I(y0), ZC_I(x1), ZC_I(y1), &x_);
}

// Draws (or measures, when t is NULL) the overlays: handles, the drag's
// outline, the caret, a polygon being drawn. Returns their device box.
static void overlays(zc_target_t *t, int *bx0, int *by0, int *bx1, int *by1) {
	int i;
	int a = 1 << 30, b = 1 << 30, c = -1, d = -1;
	zc_paint_t x_ = zc_xor();
#define GROW(p, q, r, s) do { \
		if ((p) < a) a = (p); \
		if ((q) < b) b = (q); \
		if ((r) > c) c = (r); \
		if ((s) > d) d = (s); \
	} while (0)

	if (ed.tool != TOOL_BROWSE && !app_mode) {
		bool one_group = ed.nsel > 1 && ed.sel[0]->gid;
		for (i = 1; i < ed.nsel && one_group; i++) if (ed.sel[i]->gid != ed.sel[0]->gid) one_group = false;
		if (one_group) {
			// a group is one thing: its corners only
			int gx0 = 32767, gy0 = 32767, gx1 = -32768, gy1 = -32768, k;
			for (i = 0; i < ed.nsel; i++) {
				int x0, y0, x1, y1;
				obj_bounds(ed.sel[i], &x0, &y0, &x1, &y1);
				if (x0 < gx0) gx0 = x0;
				if (y0 < gy0) gy0 = y0;
				if (x1 > gx1) gx1 = x1;
				if (y1 > gy1) gy1 = y1;
			}
			for (k = 0; k < 4; k++) {
				int px = to_px(k & 1 ? gx1 : gx0), py = to_px(k & 2 ? gy1 : gy0);
				GROW(px - 2, py - 2, px + 2, py + 2);
				if (t) zc_fill_rect_dev(t, px - 2, py - 2, 5, 5, &x_);
			}
		}
		for (i = 0; i < ed.nsel && !one_group; i++) {
			int hx[EDIT_HANDLES_MAX], hy[EDIT_HANDLES_MAX], n = edit_handles(ed.sel[i], hx, hy), k;
			// a polygon's corners are moved one selected at a time
			if (ed.nsel > 1 && n > 8) n = 8;
			for (k = 0; k < n; k++) {
				int px = to_px(hx[k]), py = to_px(hy[k]);
				GROW(px - 2, py - 2, px + 2, py + 2);
				if (t) zc_fill_rect_dev(t, px - 2, py - 2, 5, 5, &x_);
			}
		}
	}
	if (ed.text_obj) {
		int x0, y0, x1, y1;
		zc_fix cx, cy, chh;
		obj_bounds(ed.text_obj, &x0, &y0, &x1, &y1);
		x0 = to_px(x0); y0 = to_px(y0); x1 = to_px(x1); y1 = to_px(y1);
		GROW(x0 - 1, y0 - 1, x1 + 1, y1 + 1);
		if (t) xor_dotted(t, x0 - 1, y0 - 1, x1, y1);
		edit_caret(&ed, &cx, &cy, &chh);
		{
			int px = (int)(((int64_t)cx * scale16) >> 24);
			int py = (int)(((int64_t)cy * scale16) >> 24);
			int ph = (int)(((int64_t)chh * scale16) >> 24);
			if (ph < 4) ph = 4;
			GROW(px - 1, py, px + 1, py + ph);
			if (t) zc_fill_rect_dev(t, px, py, 1, ph, &x_);
		}
	}
	{
		int x0, y0, x1, y1, kind;
		if (edit_drag_box(&ed, &x0, &y0, &x1, &y1, &kind)) {
			int p = to_px(x0), q = to_px(y0), r = to_px(x1), s = to_px(y1);
			if (kind == OB_LINE) {
				GROW(p < r ? p : r, q < s ? q : s, p < r ? r : p, q < s ? s : q);
				if (t) xor_line(t, p, q, r, s);
			} else {
				GROW(p, q, r, s);
				if (t) {
					if (ed.drag == DRAG_BAND) xor_dotted(t, p, q, r, s);
					else if (kind == OB_OVAL) {
						static zc_pt_t pts[300];
						static uint16_t subs[4];
						zc_path_t pa;
						zc_path_init(&pa, pts, 300, subs, 4);
						zc_path_ellipse(&pa, ZC_I(p + r) / 2, ZC_I(q + s) / 2,
							ZC_I(r - p) / 2, ZC_I(s - q) / 2);
						zc_stroke(t, &pa, ZC_I(1), true, &pa, &x_);
					} else {
						xor_line(t, p, q, r, q);
						xor_line(t, r, q + 1, r, s);
						xor_line(t, r - 1, s, p, s);
						xor_line(t, p, s - 1, p, q + 1);
					}
				}
			}
		}
	}
	if (ed.npoly) {
		for (i = 0; i < ed.npoly; i++) {
			int px = to_px(ed.poly[2 * i]), py = to_px(ed.poly[2 * i + 1]);
			int nx, ny;
			if (i + 1 < ed.npoly) { nx = to_px(ed.poly[2 * i + 2]); ny = to_px(ed.poly[2 * i + 3]); }
			else { nx = to_px(ed.bx); ny = to_px(ed.by); }
			GROW(px < nx ? px : nx, py < ny ? py : ny, px < nx ? nx : px, py < ny ? ny : py);
			if (t) xor_line(t, px, py, nx, ny);
		}
	}
#undef GROW
	*bx0 = a; *by0 = b; *bx1 = c; *by1 = d;
}

// Copies the rendered card into the display buffer for a device
// rectangle, draws the overlays there, and puts it on the screen.
static void present(int x0, int y0, int x1, int y1) {
	zc_target_t t;
	z_clip_t content;
	int y, w0, w1;
	// card pixels to the view's
	x0 -= scroll_x; x1 -= scroll_x;
	y0 -= scroll_y; y1 -= scroll_y;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > bw) x1 = bw;
	if (y1 > bh) y1 = bh;
	if (x0 >= x1 || y0 >= y1 || !cbuf) return;
	w0 = x0 >> 5;
	w1 = (x1 - 1) >> 5;
	for (y = y0; y < y1; y++)
		memcpy(dbuf + (size_t)y * bwpl + w0, cbuf + (size_t)y * bwpl + w0,
			(size_t)(w1 - w0 + 1) * 4);
	card_target(&t, dbuf);
	zc_clip(&t, scroll_x + w0 * 32, scroll_y + y0, scroll_x + (w1 + 1) * 32, scroll_y + y1);
	if (mode != MODE_SORTER) {
		int a, b, c, d;
		overlays(&t, &a, &b, &c, &d);
	}
	if (z_win_frozen(&win)) return;
	z_win_content_rect(&win, &content);		// loads the visible region
	blit_to_screen(dbuf, bwpl, x0, y0, content.x0 + card_x + x0, content.y0 + card_y + y0,
		x1 - x0, y1 - y0);
}

// Renders what the editor marked damaged, then presents that and the
// overlays (old and new).
static void draw_menu(void);

static void update_card(void) {
	zc_target_t t;
	zc_xf_t xf = view_xf();
	int a, b, c, d;
	int px0 = 0, py0 = 0, px1 = -1, py1 = -1;
	if (!cbuf) return;
	edit_route(&ed);			// connectors follow what moved
	if (ed.all_dirty) {
		px0 = scroll_x; py0 = scroll_y; px1 = scroll_x + bw; py1 = scroll_y + bh;
		card_target(&t, cbuf);
		render_card(&t, &xf, ed.d, edit_card(&ed), (ed.tool == TOOL_BROWSE || app_mode) ? RENDER_SHOW : RENDER_EDIT);
	} else if (ed.dx1 >= ed.dx0) {
		units_rect_to_px(ed.dx0, ed.dy0, ed.dx1, ed.dy1, &px0, &py0, &px1, &py1);
		card_target(&t, cbuf);
		zc_clip(&t, px0, py0, px1, py1);
		if (!zc_clip_empty(&t))
			render_card(&t, &xf, ed.d, edit_card(&ed), (ed.tool == TOOL_BROWSE || app_mode) ? RENDER_SHOW : RENDER_EDIT);
	}
	edit_damage_clear(&ed);
	overlays(NULL, &a, &b, &c, &d);
	// present the damage, the old overlay and the new one
	if (px1 >= px0) present(px0, py0, px1, py1);
	if (ov_x1 >= ov_x0) present(ov_x0, ov_y0, ov_x1 + 1, ov_y1 + 1);
	if (c >= a) present(a, b, c + 1, d + 1);
	ov_x0 = a; ov_y0 = b; ov_x1 = c; ov_y1 = d;
	draw_menu();
}

// (While a background is edited, edit_card() is the background itself,
// a card with no background of its own: only its objects are drawn.)

// -- the tools and the bar -------------------------------------------------------------

static uint32_t icons[TOOL_COUNT * TOOL_H][1];		// 32 bits wide, one row each

static void make_icons(void) {
	static zc_pt_t pts[256], wpts[512];
	static uint16_t subs[16], wsubs[64];
	zc_path_t p, w;
	zc_target_t t;
	zc_paint_t ink = zc_black();
	int i;
	memset(icons, 0xFF, sizeof(icons));
	zc_target_bits(&t, &icons[0][0], 1, 32, TOOL_COUNT * TOOL_H);
	zc_path_init(&p, pts, 256, subs, 16);
	zc_path_init(&w, wpts, 512, wsubs, 64);
	for (i = 0; i < TOOL_COUNT; i++) {
		int oy = i * TOOL_H;
		zc_xf_t xf = zc_xf_scale(ZC_XF_ONE, ZC_I(3), ZC_I(oy + 2));	// a 16x16 box
		zc_path_set_xf(&p, &xf);
		zc_path_reset(&p);
		switch (i) {
		case TOOL_BROWSE:		// a pointing hand: a fingertip and a palm
			zc_path_rrect(&p, ZC_I(6), ZC_I(1), ZC_I(4), ZC_I(9), ZC_I(2));
			zc_path_rrect(&p, ZC_I(2), ZC_I(7), ZC_I(12), ZC_I(8), ZC_I(3));
			zc_fill(&t, &p, ZC_NONZERO, &ink);
			break;
		case TOOL_SELECT:		// the arrow
			zc_path_move(&p, ZC_I(4), ZC_I(1));
			zc_path_line(&p, ZC_I(4), ZC_I(14));
			zc_path_line(&p, ZC_I(7), ZC_I(11));
			zc_path_line(&p, ZC_I(10), ZC_I(16));
			zc_path_line(&p, ZC_I(12), ZC_I(15));
			zc_path_line(&p, ZC_I(9), ZC_I(10));
			zc_path_line(&p, ZC_I(13), ZC_I(10));
			zc_fill(&t, &p, ZC_NONZERO, &ink);
			break;
		case TOOL_TEXT: {
			zc_style_t st = { render_font_named("sansb"), ZC_I(19), 0, 0, 0, 0, 0 };
			zc_text_draw(&t, &xf, &st, ZC_I(2), ZC_I(14), "A", 1, &ink);
			break;
		}
		case TOOL_RECT:
			zc_frame_rect(&t, &p, ZC_I(1), ZC_I(3), ZC_I(14), ZC_I(10), ZC_I(1), &ink);
			break;
		case TOOL_RRECT:
			zc_frame_rrect(&t, &p, ZC_I(1), ZC_I(3), ZC_I(14), ZC_I(10), ZC_I(4), ZC_I(1), &ink);
			break;
		case TOOL_OVAL:
			zc_frame_ellipse(&t, &p, ZC_I(1), ZC_I(2), ZC_I(14), ZC_I(12), ZC_I(1), &ink);
			break;
		case TOOL_LINE:
			zc_line_dev(&t, ZC_I(4), ZC_I(oy + 15), ZC_I(17), ZC_I(oy + 3), &ink);
			break;
		case TOOL_ARROW:
			zc_line_dev(&t, ZC_I(4), ZC_I(oy + 15), ZC_I(14), ZC_I(oy + 5), &ink);
			zc_path_move(&p, ZC_I(14), ZC_I(1));
			zc_path_line(&p, ZC_I(14), ZC_I(8));
			zc_path_line(&p, ZC_I(7), ZC_I(1));
			zc_fill(&t, &p, ZC_NONZERO, &ink);
			break;
		case TOOL_POLY:
			zc_path_move(&p, ZC_I(1), ZC_I(13));
			zc_path_line(&p, ZC_I(5), ZC_I(3));
			zc_path_line(&p, ZC_I(10), ZC_I(9));
			zc_path_line(&p, ZC_I(15), ZC_I(2));
			zc_path_line(&p, ZC_I(14), ZC_I(14));
			zc_stroke(&t, &p, ZC_I(1), true, &w, &ink);
			break;
		case TOOL_PEN:			// a hand-drawn wave
			zc_path_move(&p, ZC_I(1), ZC_I(11));
			zc_path_cubic(&p, ZC_I(5), ZC_I(1), ZC_I(9), ZC_I(17), ZC_I(15), ZC_I(4));
			zc_stroke(&t, &p, ZC_I(2), false, &w, &ink);
			break;
		case TOOL_IMAGE:
			zc_frame_rect(&t, &p, ZC_I(1), ZC_I(2), ZC_I(14), ZC_I(12), ZC_I(1), &ink);
			zc_path_reset(&p);
			zc_path_move(&p, ZC_I(2), ZC_I(13));
			zc_path_line(&p, ZC_I(6), ZC_I(7));
			zc_path_line(&p, ZC_I(9), ZC_I(10));
			zc_path_line(&p, ZC_I(11), ZC_I(8));
			zc_path_line(&p, ZC_I(14), ZC_I(13));
			zc_fill(&t, &p, ZC_NONZERO, &ink);
			break;
		}
	}
}

static void draw_tools(void) {
	if (app_mode) return;
	int i;
	z_clip_t content;
	uint32_t mask = (1u << (TOOL_W - 2)) - 1;
	z_win_fill_rect(&win, 0, 0, TOOL_W, BAR_Y(), 0);
	z_win_content_rect(&win, &content);
	for (i = 0; i < TOOL_COUNT; i++) {
		static uint32_t row[TOOL_H + 1];
		int y = 2 + i * (TOOL_H + 1), r;
		// icons are ink on paper; on the dark desktop the ink is lit,
		// and a chosen tool is the other way round
		for (r = 0; r < TOOL_H; r++) {
			uint32_t v = icons[i * TOOL_H + r][0];
			row[r] = (i == ed.tool ? v : ~v) & mask;
		}
		blit_to_screen(row, 1, 0, 0, content.x0 + 1, content.y0 + y, TOOL_W - 2, TOOL_H);
	}
}

// What the bar shows, and where its parts are (content coordinates).
#define SWATCH_X(i)  (4 + (i) * (SW_SIZE + 2))
#define NSWATCH      17			// none + 16 patterns
#define WIDTH_X(i)   (SWATCH_X(NSWATCH) + 8 + (i) * 14)
static const uint8_t widths[] = { 0, 1, 2, 3, 4, 6 };
#define NWIDTH       6
#define FONTBTN_X    (WIDTH_X(NWIDTH) + 8)
#define FONTBTN_W    92
#define STYLE_X(i)   (FONTBTN_X + FONTBTN_W + 6 + (i) * 14)

static const char *const families[] = {
	"sans", "serif", "heavy", "script", "gothic", "m9x15", "m6x13", "m10x20", "mono5x8", "mono6x12",
};
#define NFAMILY ((int)(sizeof(families) / sizeof(families[0])))

static obj_t *one_sel(void) { return ed.nsel == 1 ? ed.sel[0] : NULL; }

static void draw_status(void);

// Set by repaint(): the strip is drawn whole, whatever it showed.
static bool bar_force = true;

static void draw_bar(void) {
	if (app_mode) return;
	int y = BAR_Y(), i;
	obj_t *o = one_sel();
	{
		// Drawn only when what it shows changed: it was redrawn -- cleared
		// and drawn again -- on every key typed into a text box, and
		// flashed (the status line under it is drawn either way).
		static uint32_t last[10];
		uint32_t now[10];
		now[0] = (uint32_t)(uintptr_t)o;
		now[1] = o ? o->fill : 0;
		now[2] = o ? o->fill_v : 0;
		now[3] = o ? o->line_w : 0;
		now[4] = o ? o->font : 0;
		now[5] = o ? o->size : 0;
		now[6] = o ? o->style : 0;
		now[7] = o ? o->kind : 0;
		now[8] = (uint32_t)z_win_content_w(&win);
		now[9] = (uint32_t)y;
		if (!bar_force && !memcmp(now, last, sizeof(now))) { draw_status(); return; }
		memcpy(last, now, sizeof(now));
		bar_force = false;
	}
	z_win_fill_rect(&win, TOOL_W, y, z_win_content_w(&win) - TOOL_W, BAR_H, 0);
	z_win_fill_rect(&win, 0, y, z_win_content_w(&win), 1, 1);
	y -= 1;		// the strip a row higher: a clear row above the command line
	for (i = 0; i < NSWATCH; i++) {
		int x = SWATCH_X(i) + TOOL_W;
		bool on = o && ((i == 0 && o->fill == FILL_NONE) ||
			(i > 0 && o->fill == FILL_PATTERN && o->fill_v == i - 1));
		z_win_fill_rect(&win, x - 1, y + 3, SW_SIZE + 2, SW_SIZE + 2, on ? 1 : 0);
		z_win_fill_rect(&win, x, y + 4, SW_SIZE, SW_SIZE, 1);
		if (i == 0) {
			// none: a cross on white
			int k;
			for (k = 1; k < SW_SIZE - 1; k++) {
				z_win_fill_rect(&win, x + k, y + 4 + k, 1, 1, 0);
				z_win_fill_rect(&win, x + SW_SIZE - 1 - k, y + 4 + k, 1, 1, 0);
			}
		} else {
			// ink is drawn dark on the light swatch, as it prints
			uint8_t inv[8];
			int k;
			for (k = 0; k < 8; k++) inv[k] = (uint8_t)~z_pattern_table[i - 1][k];
			z_win_fill_pattern(&win, x + 1, y + 5, SW_SIZE - 2, SW_SIZE - 2, inv);
		}
	}
	for (i = 0; i < NWIDTH; i++) {
		int x = WIDTH_X(i) + TOOL_W;
		bool on = o && o->line_w == widths[i];
		z_win_fill_rect(&win, x - 1, y + 3, 13, SW_SIZE + 2, on ? 1 : 0);
		z_win_fill_rect(&win, x, y + 4, 11, SW_SIZE, 1);
		if (widths[i]) z_win_fill_rect(&win, x + 1, y + 4 + (SW_SIZE - widths[i]) / 2, 9, widths[i], 0);
		else z_win_draw_text2(&win, x + 3, y + 6, "0", 0, 1, FONT);
	}
	{
		char t[40];
		str_t s = { t, 0, sizeof(t) };
		int x = FONTBTN_X + TOOL_W;
		if (o && OB_HAS_FONT(o->kind)) {
			s_add(&s, ed.d->font[o->font < ed.d->nfont ? o->font : 0]);
			s_add(&s, " ");
			s_int(&s, o->size);
		} else {
			s_add(&s, "font");
		}
		z_win_fill_rect(&win, x, y + 3, FONTBTN_W, SW_SIZE + 2, 1);
		z_win_draw_text2(&win, x + 3, y + 6, t, 0, 1, FONT);
		for (i = 0; i < 3; i++) {
			static const char *lab[3] = { "B", "I", "U" };
			int bx = STYLE_X(i) + TOOL_W;
			bool on = o && OB_HAS_FONT(o->kind) && (o->style & (1 << i));
			z_win_fill_rect(&win, bx, y + 3, 12, SW_SIZE + 2, on ? 1 : 0);
			if (!on) z_win_fill_rect(&win, bx + 1, y + 4, 10, SW_SIZE, 0);
			z_win_draw_text2(&win, bx + 4, y + 6, lab[i], on ? 0 : 1, on ? 1 : 0, FONT);
		}
	}
	draw_status();
}

static void draw_status(void) {
	if (app_mode) return;
	int y = BAR_Y() + 18;
	int w = z_win_content_w(&win);
	char t[160];
	str_t s = { t, 0, sizeof(t) };
	static bool was_command;
	if (mode == MODE_COMMAND) {
		// Typed into a key at a time: nothing cleared and drawn again
		// (it flashed), the field draws over itself (zedit.c). Its box
		// is 14 rows, so the text has its padding above and below.
		if (!was_command) z_win_fill_rect(&win, 0, y - 2, w, 14, 0);
		was_command = true;
		z_win_draw_text(&win, 4, y + 1, "/", 1, FONT);
		z_edit_draw(&win, &cmd_field, 12, y - 2, w - 16, 14, FONT);
		return;
	}
	was_command = false;
	z_win_fill_rect(&win, 0, y - 2, w, 14, 0);
	if (ed.msg[0]) {
		s_add(&s, ed.msg);
	} else {
		card_t *c = ed.d->card[ed.cur];
		s_add(&s, "Card ");
		s_int(&s, ed.cur + 1);
		s_add(&s, "/");
		s_int(&s, ed.d->ncard);
		s_add(&s, "  ");
		s_add(&s, c->name);
		if (ed.on_bg && c->bg >= 0) { s_add(&s, "  [background "); s_add(&s, ed.d->bg[c->bg]->name); s_add(&s, "]"); }
		if (mode == MODE_SORTER) s_add(&s, "  -- cards (F6 back)");
		else if (ed.nsel == 1) { s_add(&s, "  | "); s_add(&s, ed.sel[0]->name); }
		else if (ed.nsel > 1) { s_add(&s, "  | "); s_int(&s, ed.nsel); s_add(&s, " selected"); }
		if (ed.snap > 1) { s_add(&s, "  grid "); s_int(&s, ed.snap); }
		if (zoom) { s_add(&s, "  "); s_int(&s, zoom); s_add(&s, "%"); }
		s_add(&s, "   / for commands, F1 help, F5 show");
	}
	z_win_draw_utf8(&win, 4, y + 1, t, 1, FONT);
}

// -- the sorter ----------------------------------------------------------------------------

static int sort_sel, sort_top;
static int thumb_w, thumb_h, thumb_cols;
static uint32_t *tbuf;
static int twpl;

static void sorter_layout(void) {
	int aw = z_win_content_w(&win) - TOOL_W - 2 * MARGIN;
	thumb_w = ed.d->page.gw / 4;
	thumb_h = ed.d->page.gh / 4;
	if (thumb_w < 40) thumb_w = 40;
	thumb_cols = aw / (thumb_w + 12);
	if (thumb_cols < 1) thumb_cols = 1;
	twpl = (thumb_w + 31) / 32;
	if (!tbuf) tbuf = pool_alloc((size_t)twpl * (thumb_h + 1) * 4 + 8);
}

static void sorter_cell(int i, int *x, int *y) {
	int k = i - sort_top * thumb_cols;
	*x = TOOL_W + MARGIN + (k % thumb_cols) * (thumb_w + 12);
	*y = MARGIN + (k / thumb_cols) * (thumb_h + 16);
}

static int sorter_rows_visible(void) {
	int ah = z_win_content_h(&win) - BAR_H - 2 * MARGIN;
	int r = ah / (thumb_h + 16);
	return r < 1 ? 1 : r;
}

static void draw_sorter(void) {
	int i;
	z_clip_t content;
	int rows = sorter_rows_visible();
	sorter_layout();
	z_win_fill_rect(&win, TOOL_W, 0, z_win_content_w(&win) - TOOL_W, BAR_Y(), 0);
	if (!tbuf) return;
	z_win_content_rect(&win, &content);
	for (i = sort_top * thumb_cols; i < ed.d->ncard && i < (sort_top + rows) * thumb_cols; i++) {
		int x, y;
		zc_target_t t;
		zc_xf_t xf = zc_xf_scale(ZC_XF_ONE / 4, 0, 0);
		char lab[48];
		str_t s = { lab, 0, sizeof(lab) };
		sorter_cell(i, &x, &y);
		zc_target_bits(&t, tbuf, twpl, thumb_w, thumb_h);
		render_card(&t, &xf, ed.d, ed.d->card[i], RENDER_SHOW);
		if (i == sort_sel) {
			// the chosen card: a thick frame round it
			z_win_fill_rect(&win, x - 3, y - 3, thumb_w + 6, 2, 1);
			z_win_fill_rect(&win, x - 3, y + thumb_h + 1, thumb_w + 6, 2, 1);
			z_win_fill_rect(&win, x - 3, y - 1, 2, thumb_h + 2, 1);
			z_win_fill_rect(&win, x + thumb_w + 1, y - 1, 2, thumb_h + 2, 1);
		}
		z_win_content_rect(&win, &content);
		blit_to_screen(tbuf, twpl, 0, 0, content.x0 + x, content.y0 + y, thumb_w, thumb_h);
		s_int(&s, i + 1);
		s_add(&s, " ");
		s_add(&s, ed.d->card[i]->name);
		lab[(thumb_w + 10) / 5 < (int)sizeof(lab) - 1 ? (thumb_w + 10) / 5 : (int)sizeof(lab) - 1] = 0;
		z_win_draw_utf8(&win, x, y + thumb_h + 4, lab, 1, FONT);
	}
}

static int sorter_hit(int cx, int cy) {
	int i, rows = sorter_rows_visible();
	for (i = sort_top * thumb_cols; i < ed.d->ncard && i < (sort_top + rows) * thumb_cols; i++) {
		int x, y;
		sorter_cell(i, &x, &y);
		if (cx >= x - 3 && cx < x + thumb_w + 3 && cy >= y - 3 && cy < y + thumb_h + 12) return i;
	}
	return -1;
}

static void sorter_keep_visible(void) {
	int rows = sorter_rows_visible(), row = sort_sel / thumb_cols;
	if (row < sort_top) sort_top = row;
	if (row >= sort_top + rows) sort_top = row - rows + 1;
}

// -- painting --------------------------------------------------------------------------------

static void paint_desk(void) {
	static const uint8_t desk[8] = { 0x88, 0x00, 0x22, 0x00, 0x88, 0x00, 0x22, 0x00 };
	int cw = z_win_content_w(&win), ch = BAR_Y();
	// A deck run as an app is its card, at the window's corner: no desk.
	// Drawn at F7 it was drawn in the editor's larger window an instant
	// before wm made the window the card's size, and the strips of it
	// outside the new window stayed on the desktop.
	if (app_mode) return;
	z_win_fill_pattern(&win, TOOL_W, 0, cw - TOOL_W, card_y - 1, desk);
	z_win_fill_pattern(&win, TOOL_W, card_y + view_h + 1, cw - TOOL_W, ch - card_y - view_h - 1, desk);
	z_win_fill_pattern(&win, TOOL_W, card_y - 1, card_x - TOOL_W - 1, view_h + 2, desk);
	z_win_fill_pattern(&win, card_x + view_w + 1, card_y - 1, cw - card_x - view_w - 1, view_h + 2, desk);
	// a frame round the card
	z_win_fill_rect(&win, card_x - 1, card_y - 1, view_w + 2, 1, 1);
	z_win_fill_rect(&win, card_x - 1, card_y + view_h, view_w + 2, 1, 1);
	z_win_fill_rect(&win, card_x - 1, card_y, 1, view_h, 1);
	z_win_fill_rect(&win, card_x + view_w, card_y, 1, view_h, 1);
	if (sb_h_on) z_scrollbar_draw(&sb_h, true);
	if (sb_v_on) z_scrollbar_draw(&sb_v, true);
}

// -- zoom and scroll ---------------------------------------------------------------------
//
// Ctrl+= in, Ctrl+- out, Ctrl+0 back to fitting the window. Zoomed, the
// wheel scrolls (Shift: across) and the scrollbars move the view.

static void repaint(void);

// Moves the view (card pixels); true if it moved.
static bool scroll_to(int x, int y) {
	if (x > card_pw - view_w) x = card_pw - view_w;
	if (y > card_ph - view_h) y = card_ph - view_h;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x == scroll_x && y == scroll_y) return false;
	scroll_x = x;
	scroll_y = y;
	if (sb_h_on) z_scrollbar_set_value(&sb_h, scroll_x);
	if (sb_v_on) z_scrollbar_set_value(&sb_v, scroll_y);
	if (sb_h_on) z_scrollbar_draw(&sb_h, false);
	if (sb_v_on) z_scrollbar_draw(&sb_v, false);
	ov_x1 = -1;				// the old overlay scrolled away with the rest
	edit_damage_all(&ed);
	update_card();
	return true;
}

// The percentage on show (fitted: what fitting came to).
static int zoom_percent(void) { return (int)(((int64_t)scale16 * 100 + 32768) >> 16); }

// The zoom steps: fit, then 100, 200, 400.
static int zoom_step(int dir) {
	static const int steps[] = { 100, 200, 400 };
	int i, now = zoom ? zoom : zoom_percent();
	if (dir > 0) {
		for (i = 0; i < 3; i++) if (steps[i] > now) return steps[i];
		return ZOOM_MAX;
	}
	for (i = 2; i >= 0; i--) if (steps[i] < now) return steps[i];
	return 0;
}


// Zooms to `z` (0 fit), keeping the card point under view position
// (vx, vy) where it is.
static void set_zoom(int z, int vx, int vy) {
	int ux, uy;
	if (z == zoom) return;
	ux = to_units(scroll_x + vx);
	uy = to_units(scroll_y + vy);
	zoom = z;
	layout();
	if (z) {
		int nx = to_px(ux) - vx, ny = to_px(uy) - vy;
		scroll_x = nx; scroll_y = ny;
		layout();				// clamps, and sets the scrollbars
	} else {
		scroll_x = scroll_y = 0;
	}
	repaint();
}

// The view's centre, or the selection's when there is one in view.
static void zoom_anchor(int *vx, int *vy) {
	*vx = view_w / 2;
	*vy = view_h / 2;
	if (ed.nsel) {
		int x0, y0, x1, y1;
		obj_bounds(ed.sel[0], &x0, &y0, &x1, &y1);
		x0 = to_px((x0 + x1) / 2) - scroll_x;
		y0 = to_px((y0 + y1) / 2) - scroll_y;
		if (x0 >= 0 && x0 < view_w && y0 >= 0 && y0 < view_h) { *vx = x0; *vy = y0; }
	}
}

// Keeps a card-pixel box in view (the caret while typing at 400%).
static void keep_in_view(int x0, int y0, int x1, int y1) {
	int nx = scroll_x, ny = scroll_y, m = 8;
	if (!zoom) return;
	if (x1 + m > nx + view_w) nx = x1 + m - view_w;
	if (x0 - m < nx) nx = x0 - m;
	if (y1 + m > ny + view_h) ny = y1 + m - view_h;
	if (y0 - m < ny) ny = y0 - m;
	scroll_to(nx, ny);
}

static void keep_caret_in_view(void) {
	zc_fix cx, cy, chh;
	int px, py, ph;
	if (!ed.text_obj || !zoom) return;
	edit_caret(&ed, &cx, &cy, &chh);
	px = (int)(((int64_t)cx * scale16) >> 24);
	py = (int)(((int64_t)cy * scale16) >> 24);
	ph = (int)(((int64_t)chh * scale16) >> 24);
	keep_in_view(px, py, px + 1, py + ph);
}

static void repaint(void) {
	bar_force = true;
	layout();
	draw_tools();
	if (mode == MODE_SORTER) {
		draw_sorter();
	} else {
		paint_desk();
		edit_damage_all(&ed);
		ov_x1 = -1;
		update_card();
	}
	draw_bar();
}

static char sent_title[64];

static void update_title(void) {
	char t[64];
	z_win_doc_title(t, sizeof(t), path, ed.modified, "untitled deck");
	if (strcmp(t, sent_title)) {
		copy_str(sent_title, sizeof(sent_title), t);
		z_win_set_title(&win, t);
	}
}

// -- files --------------------------------------------------------------------------------------

static void forward_msg(z_msg_t *msg, void *user);

static void remember_dir(const char *p) {
	int cut = 0, i;
	for (i = 0; p[i]; i++) if (p[i] == '/') cut = i;
	if (cut <= 0) { copy_str(last_dir, sizeof(last_dir), "/"); return; }
	if (cut >= (int)sizeof(last_dir)) cut = (int)sizeof(last_dir) - 1;
	memcpy(last_dir, p, (size_t)cut);
	last_dir[cut] = 0;
}

// The window's outer size for the deck's page, at most the screen.
static void wanted_size(int *w, int *h) {
	*w = TOOL_W + 2 * MARGIN + ed.d->page.gw + Z_WIN_CHROME_W;
	*h = 2 * MARGIN + ed.d->page.gh + BAR_H + Z_WIN_CHROME_H;
	if (*w > Z_SCREEN_W) *w = Z_SCREEN_W;
	if (*h > Z_SCREEN_H - 8) *h = Z_SCREEN_H - 8;
}

static void set_deck(deck_t *d) {
	if (ed.d && ed.d != d) {
		deck_free(ed.d);			// stops its pictures loading, too
		render_drop_recent();
	}
	g_deck = d;
	scroll_x = scroll_y = 0;
	edit_reset(&ed, d);
	pool_free(tbuf);
	tbuf = NULL;
	sort_sel = sort_top = 0;
	layout();
	{
		int w, h;
		wanted_size(&w, &h);
		if (w != win.w || h != win.h) z_win_resize(&win, (uint32_t)w, (uint32_t)h);
	}
}

static bool load_path(const char *p) {
	uint32_t len = 0;
	uint8_t *b = load_file(p, &len);
	char err[80];
	deck_t *d;
	if (!b) {
		z_dialog_confirm(&dlg, "Open failed", "That file could not be read.", Z_DIALOG_OK_CANCEL);
		return false;
	}
	d = deckio_read((const char *)b, len, err, sizeof(err));
	pool_free(b);
	if (!d) {
		char m[120];
		str_t s = { m, 0, sizeof(m) };
		s_add(&s, "Not a deck slide can read:\n");
		s_add(&s, err);
		z_dialog_confirm(&dlg, "Open failed", m, Z_DIALOG_OK_CANCEL);
		return false;
	}
	copy_str(path, sizeof(path), p);
	remember_dir(p);
	render_set_dir(last_dir);
	set_deck(d);
	// a deck made to be used opens to be used (F7 edits it)
	set_app_mode(d->app);
	return true;
}

// Writes go to fs from memory in slide's own image, never from the
// pool: the pool is the arena, and a whole-sector write to the RAM disk
// (/tmp, /ram) from arena memory is refused on the board ("fs0 passed a
// bad pointer ...", FatFs 1) -- FatFs hands such a write the caller's
// own buffer. So everything written goes through this, a sector-sized
// piece at a time.
static char bounce[1024];

static bool write_from(int h, const char *d, uint32_t n) {
	while (n) {
		int k = (int)(n > sizeof(bounce) ? sizeof(bounce) : n);
		memcpy(bounce, d, (size_t)k);
		if (fs_write_chunk(h, bounce, k) != k) return false;
		d += k;
		n -= (uint32_t)k;
	}
	return true;
}

static bool write_whole_file(const char *p, const char *d, uint32_t n) {
	int h = fs_open_write(p);
	bool ok;
	if (h < 0) return false;
	ok = write_from(h, d, n);
	fs_close_handle(h);
	return ok;
}

static bool save_to(const char *p) {
	sbuf_t b;
	int h;
	bool ok = false;
	sb_init(&b);
	if (!deckio_write(&b, ed.d)) {
		sb_free(&b);
		z_dialog_confirm(&dlg, "Save failed", "Out of memory.", Z_DIALOG_OK_CANCEL);
		return false;
	}
	h = fs_open_write(p);
	if (h >= 0) {
		ok = write_from(h, b.buf, b.len);
		fs_close_handle(h);
	}
	sb_free(&b);
	if (!ok) {
		z_dialog_confirm(&dlg, "Save failed", "The file could not be written.", Z_DIALOG_OK_CANCEL);
		return false;
	}
	ed.modified = false;
	copy_str(path, sizeof(path), p);
	remember_dir(p);
	render_set_dir(last_dir);
	update_title();
	return true;
}

static bool ends_with_zsl(const char *p) {
	size_t n = strlen(p);
	return n >= 4 && p[n - 4] == '.' && (p[n - 3] | 32) == 'z' && (p[n - 2] | 32) == 's' &&
		(p[n - 1] | 32) == 'l';
}

// Save as: a file chosen in the dialog, or `given` (/save as PATH; a
// name without a folder goes beside the deck).
static bool do_save_as_path(const char *given) {
	char p[200];
	if (given && *given) {
		if (given[0] == '/') copy_str(p, sizeof(p), given);
		else {
			copy_str(p, sizeof(p), last_dir);
			if (p[0] && p[strlen(p) - 1] != '/') copy_str(p + strlen(p), (int)(sizeof(p) - strlen(p)), "/");
			copy_str(p + strlen(p), (int)(sizeof(p) - strlen(p)), given);
		}
	} else if (!z_dialog_save(&dlg, last_dir, path[0] ? path : "deck.zsl", p, sizeof(p))) return false;
	if (!ends_with_zsl(p) && strlen(p) + 4 < sizeof(p)) strcat(p, ".zsl");
	if (fs_size(p) > 0 && strcmp(p, path) &&
		z_dialog_confirm(&dlg, "Save as", "That file exists. Replace it?", Z_DIALOG_YES_NO) != Z_DIALOG_YES)
		return false;
	return save_to(p);
}

static bool do_save_as(void) { return do_save_as_path(NULL); }

static bool do_save(void) {
	if (!path[0]) return do_save_as();
	return save_to(path);
}

// -- printing (docs/slide_app.md, "Printing") ------------------------------------

static z_print_t printing;

// The deck's file name with another ending, in the deck's folder:
// "/home/talk.zsl" -> "/home/talk.pdf"; "deck.pdf" if untitled.
static void sibling(char *out, int n, const char *ext) {
	const char *base = path[0] ? path : "deck.zsl";
	const char *slash = strrchr(base, '/');
	const char *name = slash ? slash + 1 : base;
	const char *dot = strrchr(name, '.');
	int len = dot ? (int)(dot - name) : (int)strlen(name);
	char b[200];
	if (len > (int)sizeof(b) - 8) len = (int)sizeof(b) - 8;
	memcpy(b, name, (size_t)len);
	copy_str(b + len, (int)sizeof(b) - len, ext);
	copy_str(out, n, b);
}

// A file named on a /print line ("/print this talk.pdf"): taken out of
// `line` into `file`, with "pdf" or "ps" put in its place so the rest
// reads as usual. False if there is none.
static bool print_file_word(char *line, char *file, int n) {
	char *w = line;
	while (*w) {
		char *e;
		size_t l;
		while (*w == ' ') w++;
		e = w;
		while (*e && *e != ' ') e++;
		l = (size_t)(e - w);
		if ((l > 4 && !strncmp(e - 4, ".pdf", 4)) || (l > 3 && !strncmp(e - 3, ".ps", 3))) {
			bool pdf = e[-1] == 'f';
			char rest[200];
			if ((int)l >= n) return false;
			memcpy(file, w, l);
			file[l] = 0;
			copy_str(rest, sizeof(rest), e);
			copy_str(w, (int)(sizeof(rest)), pdf ? "pdf" : "ps");
			copy_str(w + strlen(w), (int)sizeof(rest), rest);
			return true;
		}
		w = e;
	}
	return false;
}

static void do_print(const char *line_in) {
	pj_settings_t s;
	char err[64], keep[DECK_PRINT_MAX], file[200], line[200], named[200];
	const char *ext;
	bool given;
	copy_str(line, sizeof(line), line_in);
	given = print_file_word(line, named, sizeof(named));
	if (!printjob_parse(&s, ed.d->print, line, ed.cur, ed.d->ncard, keep, sizeof(keep), err, sizeof(err))) {
		copy_str(ed.msg, sizeof(ed.msg), err);
		return;
	}
	if (strcmp(keep, ed.d->print)) {
		// how the deck prints is part of the deck
		copy_str(ed.d->print, sizeof(ed.d->print), keep);
		ed.modified = true;
		update_title();
	}
	if (s.dest == PJ_NONE) {
		copy_str(ed.msg, sizeof(ed.msg), keep[0] ? "Prints: " : "Prints one to a sheet, fitted");
		if (keep[0]) copy_str(ed.msg + 8, (int)sizeof(ed.msg) - 8, keep);
		return;
	}
	file[0] = 0;
	if (s.dest != PJ_PRINTER) {
		char p[200];
		ext = s.dest == PJ_PDF ? ".pdf" : ".ps";
		sibling(p, sizeof(p), ext);
		if (given) {
			// named on the line: no dialog, and a file there is replaced,
			// as printing it again means
			if (named[0] == '/') copy_str(file, sizeof(file), named);
			else {
				copy_str(file, sizeof(file), last_dir);
				if (file[0] && file[strlen(file) - 1] != '/') copy_str(file + strlen(file), (int)(sizeof(file) - strlen(file)), "/");
				copy_str(file + strlen(file), (int)(sizeof(file) - strlen(file)), named);
			}
		} else if (!z_dialog_save(&dlg, last_dir, p, file, sizeof(file))) return;
		{
			size_t l = strlen(file), e = strlen(ext);
			if ((l < e || strcmp(file + l - e, ext)) && l + e < sizeof(file)) strcat(file, ext);
		}
		if (!given && fs_size(file) > 0 &&
			z_dialog_confirm(&dlg, "Print to a file", "That file exists. Replace it?", Z_DIALOG_YES_NO) != Z_DIALOG_YES)
			return;
	}
	if (!z_print_begin(&printing, ed.d->title[0] ? ed.d->title : (path[0] ? path : "slide"))) {
		copy_str(ed.msg, sizeof(ed.msg), "Cannot print: no room in " Z_DIR_TMP);
		return;
	}
	printjob_options(&s, &printing.w);
	if (file[0]) {
		char d[208];
		copy_str(d, sizeof(d), s.dest == PJ_PDF ? "pdf:" : "ps:");
		copy_str(d + strlen(d), (int)(sizeof(d) - strlen(d)), file);
		zp_job(&printing.w, "dest", d);
	}
	copy_str(ed.msg, sizeof(ed.msg), "Getting the cards ready to print");
	draw_status();
	if (!printjob_pages(ed.d, s.first, s.count, &printing.w)) {
		z_print_abort(&printing);
		copy_str(ed.msg, sizeof(ed.msg), "Cannot print: no room in " Z_DIR_TMP " for the job");
		return;
	}
	if (!z_print_end(&printing)) {
		copy_str(ed.msg, sizeof(ed.msg), printing.result == Z_PRINT_E_GONE ?
			"Cannot print: print is not on the card" : "Cannot print: the job could not be written");
		return;
	}
	copy_str(ed.msg, sizeof(ed.msg), file[0] ? "Writing the file: see the print window" :
		"Printing: see the print window");
}

// Ctrl+P and the title bar's printer: the print dialog (zdialog.h),
// started from how the deck prints (its /print setup words), which the
// dialog's choices then override for this once. The deck's own words
// that the dialog has no row for -- cut marks, frames, actual size, the
// paper's kind -- still apply.
static void do_print_dialog(void) {

	pj_settings_t s;
	char err[64], keep[DECK_PRINT_MAX], base[64];
	z_print_opts_t o;

	if (!printjob_parse(&s, ed.d->print, "", ed.cur, ed.d->ncard, keep, sizeof(keep), err, sizeof(err))) {
		copy_str(ed.msg, sizeof(ed.msg), err);
		return;
	}

	z_print_opts_init(&o);
	o.npages = ed.d->ncard;
	o.current = ed.cur + 1;
	o.copies = s.copies > 0 ? s.copies : 1;
	o.nup = s.nup > 0 ? s.nup : 1;
	if (s.paper[0]) copy_str(o.paper_default, sizeof(o.paper_default), s.paper);
	copy_str(o.tray, sizeof(o.tray), s.tray);
	copy_str(o.quality, sizeof(o.quality), s.quality);

	sibling(base, sizeof(base), "");
	if (!z_dialog_print(&dlg, "Print", last_dir, base, &o)) return;

	// the cards: all, the one shown, or a range
	s.first = 0;
	s.count = -1;
	if (o.pages == Z_PRINT_PAGES_CURRENT) { s.first = ed.cur; s.count = 1; }
	else if (o.pages == Z_PRINT_PAGES_RANGE && o.first > 0) {
		int last = o.last > 0 && o.last <= ed.d->ncard ? o.last : ed.d->ncard;
		s.first = o.first - 1;
		s.count = last - o.first + 1;
		if (s.first >= ed.d->ncard || s.count < 1) {
			copy_str(ed.msg, sizeof(ed.msg), "Cannot print: no such cards");
			return;
		}
	}
	o.pages = Z_PRINT_PAGES_ALL;		// the cards sent are the ones chosen

	s.dest = o.dest == Z_PRINT_TO_PDF ? PJ_PDF : o.dest == Z_PRINT_TO_PS ? PJ_PS : PJ_PRINTER;
	s.nup = o.nup;
	s.copies = o.copies;
	if (o.paper[0]) copy_str(s.paper, sizeof(s.paper), o.paper);
	// a tray the dialog does not name (the deck's own) stays when the
	// dialog says "the printer's choice"
	if (o.tray[0] || !s.tray[0] || !strcmp(s.tray, "main") || !strcmp(s.tray, "rear") ||
		!strcmp(s.tray, "manual"))
		copy_str(s.tray, sizeof(s.tray), o.tray);
	copy_str(s.quality, sizeof(s.quality), o.quality);
	if (s.dest != PJ_PRINTER) { s.tray[0] = 0; s.quality[0] = 0; }

	if (!z_print_begin(&printing, ed.d->title[0] ? ed.d->title : (path[0] ? path : "slide"))) {
		copy_str(ed.msg, sizeof(ed.msg), "Cannot print: no room in " Z_DIR_TMP);
		return;
	}
	printjob_options(&s, &printing.w);
	if (o.dest != Z_PRINT_TO_PRINTER) {
		char d[208];
		copy_str(d, sizeof(d), o.dest == Z_PRINT_TO_PDF ? "pdf:" : "ps:");
		copy_str(d + strlen(d), (int)(sizeof(d) - strlen(d)), o.file);
		zp_job(&printing.w, "dest", d);
	}
	copy_str(ed.msg, sizeof(ed.msg), "Getting the cards ready to print");
	draw_status();
	if (!printjob_pages(ed.d, s.first, s.count, &printing.w)) {
		z_print_abort(&printing);
		copy_str(ed.msg, sizeof(ed.msg), "Cannot print: no room in " Z_DIR_TMP " for the job");
		return;
	}
	if (!z_print_end(&printing)) {
		copy_str(ed.msg, sizeof(ed.msg), printing.result == Z_PRINT_E_GONE ?
			"Cannot print: print is not on the card" : "Cannot print: the job could not be written");
		return;
	}
	copy_str(ed.msg, sizeof(ed.msg), o.dest != Z_PRINT_TO_PRINTER ?
		"Writing the file: see the print window" : "Printing: see the print window");

}

// -- /export svg (docs/slide_app.md, "Diagrams") -------------------------------
//
// A card as its print job would draw it (printjob_pages), the job kept
// in memory, then written as SVG (sw/common/zpsvg.h) a buffer at a time.

typedef struct { uint8_t *b; uint32_t n, cap; } membuf_t;

static bool mem_write(void *ctx, const void *d, uint32_t len) {
	membuf_t *m = ctx;
	if (m->n + len > m->cap) {
		uint32_t cap = m->cap ? m->cap * 2 : 16384;
		uint8_t *nb;
		while (cap < m->n + len) cap *= 2;
		nb = pool_realloc(m->b, cap);
		if (!nb) return false;
		m->b = nb;
		m->cap = cap;
	}
	memcpy(m->b + m->n, d, len);
	m->n += len;
	return true;
}

typedef struct { int fd; uint32_t n; char b[512]; bool ok; } svgout_t;

static bool svg_flush(svgout_t *o) {
	if (o->n && !write_from(o->fd, o->b, o->n)) o->ok = false;
	o->n = 0;
	return o->ok;
}

static bool svg_put(void *ctx, const char *s, uint32_t len) {
	svgout_t *o = ctx;
	while (len) {
		uint32_t k = sizeof(o->b) - o->n;
		if (k > len) k = len;
		memcpy(o->b + o->n, s, k);
		o->n += k; s += k; len -= k;
		if (o->n == sizeof(o->b) && !svg_flush(o)) return false;
	}
	return o->ok;
}

static uint8_t *svg_load(void *ctx, const char *p, uint32_t *len) { (void)ctx; return load_file(p, len); }
static void svg_release(void *ctx, uint8_t *p) { (void)ctx; pool_free(p); }
static bool svg_font(void *ctx, const char *name, const zc_font_t **out) {
	(void)ctx;
	*out = render_font_named(name);
	return *out != NULL;
}

// Card `ci` to `file`; false with ed.msg said.
static bool export_card(int ci, const char *file) {
	membuf_t m = { NULL, 0, 0 };
	zp_writer_t w;
	svgout_t *o;
	zp_svg_t sv;
	bool ok;
	zp_writer_init(&w, mem_write, &m);
	if (!printjob_pages(ed.d, ci, 1, &w) || w.failed) {
		pool_free(m.b);
		copy_str(ed.msg, sizeof(ed.msg), "Cannot export: not enough memory for the card");
		return false;
	}
	o = pool_alloc(sizeof(svgout_t));
	if (!o) { pool_free(m.b); copy_str(ed.msg, sizeof(ed.msg), "Cannot export: not enough memory"); return false; }
	o->n = 0;
	o->ok = true;
	o->fd = fs_open_write(file);
	if (o->fd < 0) {
		pool_free(o); pool_free(m.b);
		copy_str(ed.msg, sizeof(ed.msg), "Cannot write the file");
		return false;
	}
	memset(&sv, 0, sizeof(sv));
	sv.put = svg_put;
	sv.load = svg_load;
	sv.release = svg_release;
	sv.font = svg_font;
	sv.ctx = o;
	ok = zp_svg_page(&sv, m.b, m.n, 0) && svg_flush(o);
	fs_close_handle(o->fd);
	pool_free(o);
	pool_free(m.b);
	if (!ok) copy_str(ed.msg, sizeof(ed.msg), "Cannot write the file (is the disk full?)");
	return ok;
}

// /export svg [all]: this card (a file chosen in the save dialog), or
// every card (deck-01.svg, deck-02.svg ... beside the name chosen).
static void do_export(const char *line) {
	char p[200], file[200];
	bool all = false, svg = false;
	const char *r = line;
	int i;
	size_t l;
	while (*r == '/' || *r == ' ') r++;
	r += 6;
	while (*r) {
		while (*r == ' ') r++;
		if (!strncmp(r, "svg", 3)) svg = true;
		else if (!strncmp(r, "all", 3)) all = true;
		else if (*r) { copy_str(ed.msg, sizeof(ed.msg), "export svg [all]"); return; }
		while (*r && *r != ' ') r++;
	}
	(void)svg;			// the only kind so far
	sibling(p, sizeof(p), ".svg");
	if (!z_dialog_save(&dlg, last_dir, p, file, sizeof(file))) return;
	l = strlen(file);
	if ((l < 4 || strcmp(file + l - 4, ".svg")) && l + 4 < sizeof(file)) { strcat(file, ".svg"); l += 4; }
	if (!all) {
		if (fs_size(file) > 0 &&
			z_dialog_confirm(&dlg, "Export", "That file exists. Replace it?", Z_DIALOG_YES_NO) != Z_DIALOG_YES)
			return;
		copy_str(ed.msg, sizeof(ed.msg), "Writing the card as SVG");
		draw_status();
		if (export_card(ed.cur, file)) {
			copy_str(ed.msg, sizeof(ed.msg), "Wrote ");
			copy_str(ed.msg + 6, (int)sizeof(ed.msg) - 6, file);
		}
		return;
	}
	for (i = 0; i < ed.d->ncard; i++) {
		char f[208];
		// name.svg -> name-01.svg
		memcpy(f, file, l - 4);
		f[l - 4] = '-';
		f[l - 3] = (char)('0' + (i + 1) / 10 % 10);
		f[l - 2] = (char)('0' + (i + 1) % 10);
		copy_str(f + l - 1, (int)sizeof(f) - (int)(l - 1), ".svg");
		copy_str(ed.msg, sizeof(ed.msg), "Writing card ");
		ed.msg[13] = (char)('0' + (i + 1) / 10 % 10);
		ed.msg[14] = (char)('0' + (i + 1) % 10);
		ed.msg[15] = 0;
		draw_status();
		if (!export_card(i, f)) return;
	}
	copy_str(ed.msg, sizeof(ed.msg), "Wrote every card as SVG");
}

// True when it is fine to throw the deck away (saved, or the person said so).
static bool may_discard(void) {
	int r;
	if (!ed.modified) return true;
	r = z_dialog_confirm(&dlg, "Unsaved changes", "Save the deck first?", Z_DIALOG_YES_NO_CANCEL);
	if (r == Z_DIALOG_CANCEL) return false;
	if (r == Z_DIALOG_YES) return do_save();
	return true;
}

// F1, /help: the short version (docs/app_help.md); Read docs opens
// docs/slide_app.md
static void do_help(void) {
	static const char help[] =
		"Cards, slides and pages: draw shapes, set text, place "
		"pictures, show the deck on the whole screen.\n\n"
		"b v t\tBrowse, select, text\n"
		"r u o\tRectangle, rounded, oval\n"
		"l a p\tLine, arrow, polygon\n"
		"f i\tFreehand, image\n"
		"Shift+drag\tSquares, circles, 45 degrees\n"
		"Enter\tEdit the text box; end a polygon\n"
		"/ or F4\tCommands: font, fill, line, align, "
		"page, go, name ... /tour shows them\n"
		"/ again\tEvery command, to pick from\n"
		"F2\tEdit the text box; name the selection\n"
		"Esc\tStop loading pictures (Enter: again)\n"
		"F5\tShow (Shift+F5: from card 1)\n"
		"F6\tAll cards: drag to reorder\n"
		"F7\tUse the deck as an app (F7: edit)\n"
		"/button\tControls; b (browse) uses them\n"
		"/(...)\tScheme, at once; /script edits scripts\n"
		"/diamond\tFlowcharts; /connect joins two shapes\n"
		"/export svg\tThe card for the web (/import a drawing)\n"
		"PgUp/PgDn\tPrevious, next card\n"
		"Ctrl+= - 0\tZoom in, out, fit\n"
		"Ctrl+M\tNew card\n"
		"Ctrl+Z/Y\tUndo, redo\n"
		"Ctrl+C/X/V\tCopy, cut, paste\n"
		"Ctrl+D\tDuplicate\n"
		"Ctrl+G\tGroup (Ctrl+Shift+G: ungroup)\n"
		"Ctrl+N/O/S\tNew, open, save\n"
		"Ctrl+P\tPrint: where, which cards, copies, paper\n"
		"\t(/print pdf: straight to a PDF)\n\n"
		"In a show: Space or a click goes on, Left goes back, "
		"B blanks, T speaks the notes, Esc ends.";
	z_dialog_help(&dlg, "Slide help", help, "slide_app");
}

static void do_open(void) {
	char p[200];
	if (!may_discard()) return;
	if (!z_dialog_open(&dlg, last_dir, p, sizeof(p))) return;
	load_path(p);
}

static void do_new(void) {
	deck_t *d;
	if (!may_discard()) return;
	d = deck_new();
	if (!d) return;
	path[0] = 0;
	set_deck(d);
}

// -- images ----------------------------------------------------------------------------------------

// The path to store for an image: relative to the deck's folder when it
// is inside it, so a deck and its pictures move together.
static void image_ref(const char *img, char *out, int cap) {
	size_t n = strlen(last_dir);
	if (path[0] && n > 1 && !strncmp(img, last_dir, n) && img[n] == '/') copy_str(out, cap, img + n + 1);
	else copy_str(out, cap, img);
}

// A picture just placed: put in at once, in a box of a likely shape,
// and fitted to its real shape when it has loaded -- unless it has been
// moved or sized meanwhile. Loading is the main loop's, a slice at a
// time (render_load_step()): nothing here waits for it.
static obj_t *fit_obj;
static int16_t fit_box[4];

// /import [FILE.svg]: a drawing's outlines as polygons (edit_import_svg)
static void import_file(const char *p) {
	uint32_t n;
	uint8_t *b = load_file(p, &n);
	if (!b) { copy_str(ed.msg, sizeof(ed.msg), "Cannot read the file"); return; }
	copy_str(ed.msg, sizeof(ed.msg), "Importing the drawing");
	draw_status();
	edit_import_svg(&ed, (const char *)b, n);
	pool_free(b);
	ed.tool = TOOL_SELECT;
	draw_tools();
	update_title();
}

static void do_import(const char *line) {
	char p[200];
	const char *r = line;
	while (*r == '/' || *r == ' ') r++;
	r += 6;
	while (*r == ' ') r++;
	if (*r) {
		if (*r == '/' || !path[0]) copy_str(p, sizeof(p), r);
		else {
			copy_str(p, sizeof(p), last_dir);
			if (p[0] && p[strlen(p) - 1] != '/') copy_str(p + strlen(p), (int)(sizeof(p) - strlen(p)), "/");
			copy_str(p + strlen(p), (int)(sizeof(p) - strlen(p)), r);
		}
	} else if (!z_dialog_open(&dlg, last_dir, p, sizeof(p))) return;
	import_file(p);
}

static bool is_svg_name(const char *p) {
	size_t n = strlen(p);
	return n >= 4 && p[n - 4] == '.' && (p[n - 3] | 32) == 's' && (p[n - 2] | 32) == 'v' && (p[n - 1] | 32) == 'g';
}

static void do_image(void) {
	char p[200], ref[200];
	obj_t *o;
	int w = ed.d->page.gw * 2 / 3, h = w * 3 / 4;
	ed.tool = TOOL_SELECT;
	if (!z_dialog_open(&dlg, last_dir, p, sizeof(p))) return;
	// a drawing comes in as shapes, to edit, not as a picture
	if (is_svg_name(p)) { import_file(p); return; }
	image_ref(p, ref, sizeof(ref));
	o = obj_new(OB_IMAGE);
	if (!o) return;
	o->line_w = 0;
	render_set_dir(path[0] ? last_dir : "");
	obj_set_str(&o->src, path[0] ? ref : p);
	if (h > ed.d->page.gh * 2 / 3) { h = ed.d->page.gh * 2 / 3; w = h * 4 / 3; }
	obj_set_box(o, (ed.d->page.gw - w) / 2, (ed.d->page.gh - h) / 2, w, h);
	{
		card_t *c = edit_card(&ed);
		edit_checkpoint(&ed, false);
		card_unique_obj_name(c, OB_IMAGE, o->name, OB_NAME_MAX);
		if (!card_add(c, -1, o)) { obj_free(o); return; }
		edit_select(&ed, o, false);
		edit_damage_obj(&ed, o);
	}
	fit_obj = o;
	fit_box[0] = o->x; fit_box[1] = o->y; fit_box[2] = o->w; fit_box[3] = o->h;
}

static bool on_shown_card(const obj_t *o) {
	card_t *c = edit_card(&ed);
	if (!c) return false;
	if (card_index_of(c, o) >= 0) return true;
	if (!ed.on_bg && c->bg >= 0 && c->bg < ed.d->nbg && card_index_of(ed.d->bg[c->bg], o) >= 0)
		return true;
	return false;
}

// A picture has finished loading, one way or another.
static void picture_done(obj_t *o) {
	int w, h;
	if (o == fit_obj) {
		fit_obj = NULL;
		if (render_image_state(o) == RENDER_IMG_FAILED) {
			// not a picture after all: it goes again, as if never placed
			card_t *c = edit_card(&ed);
			int i = c ? card_index_of(c, o) : -1;
			if (i >= 0) {
				edit_damage_obj(&ed, o);
				edit_select_none(&ed);
				card_take(c, i);
				obj_free(o);
				copy_str(ed.msg, sizeof(ed.msg), "That is not a picture slide can read (PNG, JPEG, GIF, BMP, PNM)");
				return;
			}
		} else if (render_image_size(o, &w, &h) && o->x == fit_box[0] && o->y == fit_box[1] &&
			o->w == fit_box[2] && o->h == fit_box[3]) {
			int maxw = ed.d->page.gw * 2 / 3, maxh = ed.d->page.gh * 2 / 3;
			int cx = o->x + o->w / 2, cy = o->y + o->h / 2;
			if (w > maxw) { h = h * maxw / w; w = maxw; }
			if (h > maxh) { w = w * maxh / h; h = maxh; }
			if (w < 1) w = 1;
			if (h < 1) h = 1;
			edit_damage_obj(&ed, o);
			obj_set_box(o, cx - w / 2, cy - h / 2, w, h);
		}
	}
	if (on_shown_card(o)) edit_damage_obj(&ed, o);
}

// The status line while pictures load: which, how far, how to stop.
static bool load_msg;
static void load_status(void) {
	const char *name;
	uint16_t pm = render_load_progress(&name);
	char b[96];
	int n;
	if (mode == MODE_COMMAND || (ed.msg[0] && !load_msg)) return;
	if (!name) { if (load_msg) { ed.msg[0] = 0; load_msg = false; draw_status(); } return; }
	copy_str(b, sizeof(b), "Loading ");
	copy_str(b + 8, (int)sizeof(b) - 8, name);
	n = (int)strlen(b);
	if (n < (int)sizeof(b) - 24) {
		b[n++] = ' ';
		if (pm >= 100) b[n++] = (char)('0' + pm / 100);
		b[n++] = (char)('0' + (pm / 10) % 10);
		b[n++] = '%';
		copy_str(b + n, (int)sizeof(b) - n, "  (Esc stops)");
	}
	copy_str(ed.msg, sizeof(ed.msg), b);
	load_msg = true;
	draw_status();
}

// -- the clipboard ------------------------------------------------------------------------------------

// The system clipboard holds 4KB of text. Objects that fit go there as
// their .zsl; larger ones stay here, with a marker there that says so.
static sbuf_t clip_internal;
#define CLIP_MARK "(slide-objects)"

static void do_copy(bool cut) {
	sbuf_t b;
	if (!ed.nsel) return;
	sb_init(&b);
	if (!edit_copy(&ed, &b)) { sb_free(&b); return; }
	sb_free(&clip_internal);
	clip_internal = b;
	if (b.len < 4000) z_clip_set(b.buf, (int)b.len);
	else z_clip_set(CLIP_MARK, (int)strlen(CLIP_MARK));
	if (cut) edit_delete(&ed);
}

static void do_paste(void) {
	static char t[4097];
	int n = z_clip_get(t, sizeof(t));
	if (n <= 0) return;
	t[n] = 0;
	if (!strcmp(t, CLIP_MARK) && clip_internal.buf)
		edit_paste(&ed, clip_internal.buf, clip_internal.len);
	else
		edit_paste(&ed, t, (uint32_t)n);
}

// -- the show ---------------------------------------------------------------------------------------------

// -- controls and actions in the window (act.h) -------------------------------------------
//
// With the browse tool, or a deck run as an app, the controls work:
// buttons, checks, fields ... and arriving at a card runs its action.

static player_t pl;
static int pl_card = -1;			// the card the player last saw
static deck_t *pl_deck;
static char open_wanted[200];		// a deck an action asked for, opened from the loop

static bool playing(void) { return app_mode || (ed.tool == TOOL_BROWSE && mode != MODE_SORTER && !ed.on_bg); }

static void pl_sync(void) {
	pl.d = ed.d;
	pl.cur = ed.cur;
}

static int next_card(void) {
	if (ed.cur + 1 < ed.d->ncard) return ed.cur + 1;
	return ed.d->loop ? 0 : ed.cur;
}

// What actions, scripts and the player touched since last time (act.h):
// those areas drawn again, once -- a click redraws its button and what
// its action changed, not the card.
static void apply_dirty(void) {
	act_dirty_t dt[ACT_DIRTY_MAX];
	bool model;
	int n = act_dirty_take(dt, &model), i;
	card_t *c = edit_card(&ed);
	if (!n) return;
	if (model && !app_mode) ed.modified = true;		// a deck run as an app keeps no state
	if (mode == MODE_SORTER) return;
	if (n < 0 || !c) { edit_damage_all(&ed); update_card(); return; }
	// one at a time: a button and the display it changed are two small
	// areas, not the box around both
	for (i = 0; i < n; i++) {
		edit_damage(&ed, dt[i].r[0], dt[i].r[1], dt[i].r[2], dt[i].r[3]);
		// and where it is now (if it is still there: an undo may have
		// replaced it)
		if (render_card_has(ed.d, c, dt[i].o)) edit_damage_obj(&ed, dt[i].o);
		update_card();
	}
}

// The card's look changed (a value, a field's text, a shown object):
// act.h says which part, drawn by apply_dirty() after the event.
static void card_changed(void) { apply_dirty(); }

static void h_go(void *u, int to) {
	(void)u;
	edit_goto(&ed, to);
	player_reset(&pl);
	pl_sync();
	pl_card = to;
	update_card();
	draw_bar();
	act_arrived(ed.d, to);
	script_card(ed.d, to);
}
static void h_changed(void *u) { (void)u; }		// act_touch() says what, apply_dirty() draws it
static void h_eval(void *u, deck_t *d, int cur, const char *f) { (void)u; script_eval(d, cur, f, NULL, 0); }
static void h_clicked(void *u, deck_t *d, int cur, obj_t *o) { (void)u; script_clicked(d, cur, o); }
static int h_current(void *u) { (void)u; return ed.cur; }
static bool save_to(const char *p);
static bool h_save(void *u) {
	(void)u;
	if (!path[0]) { copy_str(ed.msg, sizeof(ed.msg), "save-deck: the deck has no file yet (/save as)"); draw_status(); return false; }
	return save_to(path);
}

static void forward_msg(z_msg_t *msg, void *user);

// While a script runs: Esc stops it; the window's own messages are
// answered (a redraw, its region); keys and the mouse wait for it to
// finish -- they are dropped.
static bool stop_check(void) {
	z_msg_t m;
	int n;
	for (n = 0; n < 8 && z_msg_read(&m) == Z_OK; n++) {
		if (m.subject == Z_WM_KEY) {
			if (m.obj.type == Z_UINT32 && Z_WM_UNPACK_KEY_PRESSED(m.obj.val.uint32) &&
				Z_WM_UNPACK_KEY_KEYSYM(m.obj.val.uint32) == 0x1b) {
				copy_str(ed.msg, sizeof(ed.msg), "Stopped the script (Esc)");
				return true;
			}
			continue;
		}
		if (m.subject == Z_WM_MOUSE || m.subject == Z_WM_WHEEL) continue;
		if (script_window_msg(&m)) continue;
		forward_msg(&m, NULL);
	}
	return false;
}
static void h_open(void *u, const char *p) { (void)u; copy_str(open_wanted, sizeof(open_wanted), p); }
static void h_msg(void *u, const char *m) { (void)u; copy_str(ed.msg, sizeof(ed.msg), m); draw_status(); }
static void h_speak(void *u, const char *t) { (void)u; z_speak(t, 0); }
static void h_run(void *u, const char *c) { (void)u; show_launch(c); }
static uint32_t h_now(void *u) { (void)u; return z_uptime_ticks(); }

static void act_host_editor(void) {
	act_host_t h;
	memset(&h, 0, sizeof(h));
	h.go = h_go; h.changed = h_changed; h.open_deck = h_open; h.message = h_msg;
	h.speak = h_speak; h.run = h_run; h.now = h_now; h.tick_hz = Z_TICK_HZ;
	h.eval = h_eval; h.clicked = h_clicked; h.current = h_current; h.save = h_save;
	act_init(&h);
	script_stop_check(stop_check);
}

// Called from the main loop: a new card (or deck) since last time, in
// browse or app mode, runs the card's action.
static deck_t *scripted;			// the deck whose script has run (in use)

static void pl_check_card(void) {
	if (pl_deck != ed.d) { act_reset(); player_reset(&pl); pl_deck = ed.d; pl_card = -1; scripted = NULL; }
	if (!playing()) {
		if (pl.focus || pl.pressed) player_reset(&pl);
		pl_card = -1;
		if (scripted) { script_stop_timers(); scripted = NULL; }
		return;
	}
	if (scripted != ed.d) {
		// the deck starts being used: its own script first
		scripted = ed.d;
		script_deck(ed.d, ed.cur);
	}
	if (pl_card != ed.cur) {
		player_reset(&pl);
		pl_sync();
		pl_card = ed.cur;
		act_arrived(ed.d, ed.cur);
		script_card(ed.d, ed.cur);
	}
}

static void wanted_size(int *w, int *h);

// F7: a deck run as an app, or edited.
static void set_app_mode(bool on) {
	int w, h;
	if (on == app_mode) return;
	edit_cancel(&ed);
	edit_text_end(&ed);
	edit_select_none(&ed);
	player_reset(&pl);
	pl_card = -1;
	app_mode = on;
	if (on) { zoom = 0; scroll_x = scroll_y = 0; mode = MODE_EDIT; }
	wanted_size(&w, &h);
	layout();
	z_win_resize(&win, (uint32_t)w, (uint32_t)h);
	repaint();
}

static void do_show(int from) {
	int end;
	const char *o;
	edit_cancel(&ed);
	edit_text_end(&ed);
	player_reset(&pl);
	end = show_run(ed.d, from, &win);
	act_host_editor();
	pl_card = -1;
	scripted = NULL;			// the show ran it; browsing again runs it again
	if (mode != MODE_SORTER) edit_goto(&ed, end);
	edit_damage_all(&ed);
	o = show_open_request();
	if (o) copy_str(open_wanted, sizeof(open_wanted), o);
	// wm repaints the desktop and asks for this window's content
}

// -- commands ------------------------------------------------------------------------------------------------

static bool quit_now;

// -- scripts, edited in `text` ---------------------------------------------------------
//
// /script writes the script (the deck's, the card's, or a control's
// action) to a file in /tmp and opens it in `text`. While it is open,
// slide looks at the file now and then; each time it is saved, the deck
// takes it -- checked first: its parentheses must close.

static struct {
	char		path[80];
	int			kind;			// 0 deck, 1 card, 2 object's action
	int			card;
	char		obj[OB_NAME_MAX];
	int			size;			// as last seen
	uint32_t	sum;
	uint32_t	looked;
	bool		on;
} sedit;

static uint32_t sum_of(const char *b, int n) {
	uint32_t h = 2166136261u;
	int i;
	for (i = 0; i < n; i++) h = (h ^ (uint8_t)b[i]) * 16777619u;
	return h;
}

static char **sedit_field(void) {
	card_t *c;
	if (sedit.kind == 0) return &ed.d->script;
	if (sedit.card < 0 || sedit.card >= ed.d->ncard) return NULL;
	c = ed.d->card[sedit.card];
	if (sedit.kind == 1) return &c->script;
	{
		obj_t *o = card_find_obj(c, sedit.obj);
		if (!o && c->bg >= 0) o = card_find_obj(ed.d->bg[c->bg], sedit.obj);
		return o ? &o->action : NULL;
	}
}

static void do_script(const char *line) {
	const char *r = line;
	char **f;
	int n;
	while (*r == '/' || *r == ' ') r++;
	r += 6;						// "script"
	while (*r == ' ') r++;
	memset(&sedit, 0, sizeof(sedit));
	sedit.card = ed.cur;
	if (!strncmp(r, "deck", 4)) sedit.kind = 0;
	else if (!strncmp(r, "card", 4) || ed.nsel != 1) sedit.kind = 1;
	else { sedit.kind = 2; copy_str(sedit.obj, sizeof(sedit.obj), ed.sel[0]->name); }
	f = sedit_field();
	if (!f) return;
	{
		// a file name from what it is: /tmp/deck.scm, /tmp/card-intro.scm
		str_t s = { sedit.path, 0, sizeof(sedit.path) };
		const char *nm = sedit.kind == 0 ? "deck" : sedit.kind == 1 ? ed.d->card[ed.cur]->name : sedit.obj;
		int i;
		s_add(&s, Z_DIR_TMP "/");
		s_add(&s, sedit.kind == 0 ? "" : sedit.kind == 1 ? "card-" : "action-");
		for (i = 0; nm[i] && s.n < s.cap - 6; i++) {
			char c = nm[i];
			char t[2] = { (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? c : '-', 0 };
			s_add(&s, t);
		}
		s_add(&s, ".scm");
	}
	{
		const char *head = sedit.kind == 0 ?
			";; The deck's script: run when the deck starts being used.\n;; (when-clicked \"ok\" (go 'next))\n\n" :
			sedit.kind == 1 ? ";; This card's script: run on arriving at it.\n;; (every 1000 (set-text \"clock\" (current-time)))\n\n" :
			"";
		const char *body = *f ? *f : "";
		int hl = *f ? 0 : (int)strlen(head), bl = (int)strlen(body);
		char *b = pool_alloc((size_t)(hl + bl + 2));
		if (!b) { copy_str(ed.msg, sizeof(ed.msg), "Out of memory"); return; }
		memcpy(b, head, (size_t)hl);
		memcpy(b + hl, body, (size_t)bl);
		n = hl + bl;
		if (!n || b[n - 1] != '\n') b[n++] = '\n';
		write_whole_file(sedit.path, b, (uint32_t)n);
		sedit.size = n;
		sedit.sum = sum_of(b, n);
		pool_free(b);
	}
	sedit.on = true;
	sedit.looked = z_uptime_ticks();
	z_launch_arg_set(sedit.path);
	if (!z_proc_run("text")) { copy_str(ed.msg, sizeof(ed.msg), "Could not start text"); sedit.on = false; return; }
	copy_str(ed.msg, sizeof(ed.msg), "Editing the script in text: save it (Ctrl+S) and slide takes it");
}

// A second or so after the last look: has the file been saved?
static void sedit_poll(void) {
	int n;
	char *b, **f, err[64];
	if (!sedit.on || z_uptime_ticks() - sedit.looked < Z_TICK_HZ) return;
	sedit.looked = z_uptime_ticks();
	n = fs_size(sedit.path);
	if (n < 0) { sedit.on = false; return; }
	if (n > 16000) { copy_str(ed.msg, sizeof(ed.msg), "That script is too long (16 KB)"); draw_status(); return; }
	b = pool_alloc((size_t)n + 1);
	if (!b) return;
	if (fs_read_file(sedit.path, b, n) != n) { pool_free(b); return; }
	b[n] = 0;
	if (n == sedit.size && sum_of(b, n) == sedit.sum) { pool_free(b); return; }
	sedit.size = n;
	sedit.sum = sum_of(b, n);
	if (!script_check(b, err, sizeof(err))) {
		copy_str(ed.msg, sizeof(ed.msg), "The script was not taken: ");
		copy_str(ed.msg + strlen(ed.msg), (int)(sizeof(ed.msg) - strlen(ed.msg)), err);
		pool_free(b);
		draw_status();
		return;
	}
	f = sedit_field();
	if (f) {
		// trailing blank lines are not kept
		while (n && (b[n - 1] == '\n' || b[n - 1] == ' ')) b[--n] = 0;
		edit_checkpoint(&ed, sedit.kind == 0);
		obj_set_str(f, n ? b : NULL);
		ed.modified = true;
		// in use: the deck's script again, so its new definitions count
		if (sedit.kind == 0 && scripted == ed.d) scripted = NULL;
		copy_str(ed.msg, sizeof(ed.msg), sedit.kind == 0 ? "The deck's script was saved" :
			sedit.kind == 1 ? "The card's script was saved" : "The action was saved");
		update_title();
	}
	pool_free(b);
	draw_status();
}

static void run_command(const char *line) {
	const char *app = NULL;
	int gw = ed.d->page.gw, gh = ed.d->page.gh;
	ed.msg[0] = 0;
	{
		// the message box: Scheme, at once, on this card
		const char *r = line;
		while (*r == ' ' || *r == '/') r++;
		if (*r == '(') {
			char out[sizeof(ed.msg) - 4];
			bool ok = script_eval(ed.d, ed.cur, r, out, sizeof(out));
			copy_str(ed.msg, sizeof(ed.msg), ok ? "=> " : "Error: ");
			copy_str(ed.msg + strlen(ed.msg), (int)(sizeof(ed.msg) - strlen(ed.msg)), out);
			edit_damage_all(&ed);
			return;
		}
	}
	edit_command(&ed, line, &app);
	if (ed.d->page.gw != gw || ed.d->page.gh != gh) {
		// a new page shape: the window follows it
		int w, h;
		wanted_size(&w, &h);
		layout();
		z_win_resize(&win, (uint32_t)w, (uint32_t)h);
	}
	if (!app) return;
	if (!strcmp(app, "show")) do_show(ed.cur);
	else if (!strcmp(app, "open")) do_open();
	else if (!strcmp(app, "save")) {
		const char *r = line;
		while (*r == '/' || *r == ' ') r++;
		if (!strncmp(r, "save as", 7)) {
			r += 7;
			while (*r == ' ') r++;
			do_save_as_path(r);
		} else do_save();
	}
	else if (!strcmp(app, "new deck")) do_new();
	else if (!strcmp(app, "image")) do_image();
	else if (!strcmp(app, "help")) do_help();
	else if (!strcmp(app, "print")) do_print(line);
	else if (!strcmp(app, "export")) do_export(line);
	else if (!strcmp(app, "import")) do_import(line);
	else if (!strcmp(app, "script")) do_script(line);
	else if (!strcmp(app, "zoom")) {
		const char *r = line;
		int z = 0, vx, vy;
		while (*r == '/' || *r == ' ') r++;
		r += 4;
		while (*r == ' ') r++;
		while (*r >= '0' && *r <= '9') z = z * 10 + (*r++ - '0');
		if (*r == '%') r++;
		if (z && z != 100 && z != 200 && z != 400) {
			copy_str(ed.msg, sizeof(ed.msg), "zoom 100, 200, 400 or fit");
		} else {
			zoom_anchor(&vx, &vy);
			set_zoom(z, vx, vy);
		}
	}
	else if (!strcmp(app, "tour")) { if (may_discard()) load_path(Z_PATH_SLIDE_TOUR); }
	else if (!strcmp(app, "quit")) { if (may_discard()) quit_now = true; }
}

// An action's (open ...): another deck, from the main loop rather than
// from inside the action that asked.
static void take_open_request(void) {
	char p[200];
	if (!open_wanted[0]) return;
	copy_str(p, sizeof(p), open_wanted);
	open_wanted[0] = 0;
	if (may_discard()) load_path(p);
	repaint();
}

// -- input ---------------------------------------------------------------------------------------------------

static const struct { char key; int tool; } tool_keys[] = {
	{ 'b', TOOL_BROWSE }, { 'v', TOOL_SELECT }, { 't', TOOL_TEXT }, { 'r', TOOL_RECT },
	{ 'u', TOOL_RRECT }, { 'o', TOOL_OVAL }, { 'l', TOOL_LINE }, { 'a', TOOL_ARROW },
	{ 'p', TOOL_POLY }, { 'f', TOOL_PEN }, { 'i', TOOL_IMAGE },
};

static void set_tool(int t) {
	edit_cancel(&ed);
	if (t == TOOL_IMAGE) { do_image(); draw_tools(); return; }
	if (t != TOOL_SELECT && t != TOOL_BROWSE) edit_select_none(&ed);
	if ((t == TOOL_BROWSE) != (ed.tool == TOOL_BROWSE)) edit_damage_all(&ed);
	ed.tool = t;
	draw_tools();
}

static void sorter_key(uint32_t k) {
	int n = ed.d->ncard;
	if (k == Z_KEY_LEFT && sort_sel > 0) sort_sel--;
	else if (k == Z_KEY_RIGHT && sort_sel + 1 < n) sort_sel++;
	else if (k == Z_KEY_UP && sort_sel - thumb_cols >= 0) sort_sel -= thumb_cols;
	else if (k == Z_KEY_DOWN && sort_sel + thumb_cols < n) sort_sel += thumb_cols;
	else if (k == Z_KEY_HOME) sort_sel = 0;
	else if (k == Z_KEY_END) sort_sel = n - 1;
	else if (k == 0x0d || k == 0x1b || k == Z_KEY_F6) {
		edit_goto(&ed, sort_sel);
		mode = MODE_EDIT;
		repaint();
		return;
	} else if (k == Z_KEY_DELETE || k == 0x7f) {
		ed.cur = sort_sel;
		edit_delete_card(&ed);
		if (sort_sel >= ed.d->ncard) sort_sel = ed.d->ncard - 1;
	} else if (k == Z_KEY_F5) {
		do_show(sort_sel);
		return;
	} else return;
	sorter_keep_visible();
	draw_sorter();
	draw_bar();
}


// -- the / menu (docs/keys.md) -----------------------------------------------------------
//
// '/' at the start of the command line: every command in a list over the
// bottom of the card, Up / Down / PageUp / PageDown / Home / End to move,
// Enter (or a click) puts it on the line -- run at once if it takes
// nothing more -- and Esc or anything typed goes back to the line.

#define MENU_ROWS	12
#define MENU_LH		10
static bool menu_on;
static int menu_hot, menu_top;

static void menu_rect(int *x, int *y, int *w, int *h) {
	*x = TOOL_W + 2;
	*w = z_win_content_w(&win) - TOOL_W - 4;
	*h = MENU_ROWS * MENU_LH + 4;
	*y = BAR_Y() - *h - 1;
	if (*y < 0) *y = 0;
}

static void draw_menu(void) {
	int x, y, w, h, i, n = edit_command_count();
	if (!menu_on) return;
	menu_rect(&x, &y, &w, &h);
	z_win_fill_rect(&win, x - 1, y - 1, w + 2, h + 2, 1);
	z_win_fill_rect(&win, x, y, w, h, 0);
	for (i = 0; i < MENU_ROWS && menu_top + i < n; i++) {
		int k = menu_top + i, ry = y + 2 + i * MENU_LH;
		if (k == menu_hot) {
			z_win_fill_rect(&win, x + 1, ry - 1, w - 2, MENU_LH, 1);
			z_win_draw_text2(&win, x + 4, ry, edit_command_help(k), 0, 1, FONT);
		} else {
			z_win_draw_text(&win, x + 4, ry, edit_command_help(k), 1, FONT);
		}
	}
	if (n > MENU_ROWS) {
		// where in the list: a thin bar down the right edge
		int bh = h * MENU_ROWS / n, by = y + (h - bh) * menu_top / (n - MENU_ROWS);
		z_win_fill_rect(&win, x + w - 3, by, 2, bh, 1);
	}
}

static void menu_close(void) {
	if (!menu_on) return;
	menu_on = false;
	repaint();
}

static void menu_move(int to) {
	int n = edit_command_count();
	if (to < 0) to = 0;
	if (to >= n) to = n - 1;
	menu_hot = to;
	if (menu_hot < menu_top) menu_top = menu_hot;
	if (menu_hot >= menu_top + MENU_ROWS) menu_top = menu_hot - MENU_ROWS + 1;
	draw_menu();
}

// The command a help line starts with, cut before what it takes:
// "go N | NAME" puts "go " on the line; "dup card" runs; a list of
// commands ("undo | redo") runs the first.
static bool arg_word(const char *w) {
	return *w == '[' || *w == '.' || *w == '|' || (*w >= 'A' && *w <= 'Z');
}

static void menu_choose(void) {
	const char *h = edit_command_help(menu_hot);
	char word[48];
	int n = 0, end, t = 0;
	bool more = false;
	const char *p = h;
	// the part before the description (two spaces)
	end = 0;
	while (h[end] && !(h[end] == ' ' && h[end + 1] == ' ')) end++;
	while (p < h + end) {
		const char *q = p, *nx;
		while (q < h + end && *q != ' ') q++;		// this word: p..q
		nx = q;
		while (nx < h + end && *nx == ' ') nx++;	// the next
		if (arg_word(p)) { more = true; break; }
		if (nx < h + end && *nx == '|' && t > 0) { more = true; break; }
		if (n + (int)(q - p) + 1 >= (int)sizeof(word)) break;
		if (n) word[n++] = ' ';
		memcpy(word + n, p, (size_t)(q - p));
		n += (int)(q - p);
		t++;
		if (nx < h + end && *nx == '|') break;		// the first of several commands
		p = nx;
	}
	if (more) word[n++] = ' ';
	word[n] = 0;
	menu_on = false;
	repaint();
	if (more) {
		z_edit_init(&cmd_field, cmd_buf, sizeof(cmd_buf), word);
		cmd_field.focus = true;
		draw_status();
	} else {
		mode = MODE_EDIT;
		run_command(word);
		if (mode == MODE_SORTER) draw_sorter();
		update_card();
		draw_bar();
		update_title();
	}
}

static void menu_open(void) {
	menu_on = true;
	menu_hot = menu_top = 0;
	draw_menu();
}

// A key while the menu is up.
static void menu_key(uint32_t k) {
	switch (k) {
	case Z_KEY_UP: menu_move(menu_hot - 1); return;
	case Z_KEY_DOWN: menu_move(menu_hot + 1); return;
	case Z_KEY_PAGEUP: menu_move(menu_hot - MENU_ROWS); return;
	case Z_KEY_PAGEDOWN: menu_move(menu_hot + MENU_ROWS); return;
	case Z_KEY_HOME: menu_move(0); return;
	case Z_KEY_END: menu_move(edit_command_count() - 1); return;
	case 0x0d: menu_choose(); return;
	case 0x1b: menu_close(); return;
	default:
		// typing: back to the line, with what was typed
		menu_close();
		if (k >= 0x20 && k < 0x7f && k != '/' && z_edit_key(&cmd_field, k)) draw_status();
		return;
	}
}

// A click while the menu is up: a row chooses it, elsewhere closes it.
static void menu_click(int cx, int cy) {
	int x, y, w, h, row;
	menu_rect(&x, &y, &w, &h);
	if (cx < x || cx >= x + w || cy < y + 2 || cy >= y + h) { menu_close(); return; }
	row = (cy - y - 2) / MENU_LH;
	if (menu_top + row >= edit_command_count()) return;
	menu_hot = menu_top + row;
	menu_choose();
}

// F4 (docs/keys.md): the command line, from anywhere; `pre` already typed.
static void open_prompt(const char *pre) {
	edit_cancel(&ed);
	if (ed.text_obj) edit_text_end(&ed);
	if (mode == MODE_SORTER) { edit_goto(&ed, sort_sel); mode = MODE_EDIT; repaint(); }
	mode = MODE_COMMAND;
	menu_on = false;
	z_edit_init(&cmd_field, cmd_buf, sizeof(cmd_buf), pre);
	cmd_field.focus = true;
	draw_status();
}

// A key in a deck run as an app: only what a reader needs.
static void app_key(uint32_t k, bool shift) {
	int to = -1;
	if (k == Z_KEY_HELP) { do_help(); return; }
	if (k == Z_KEY_F5) { do_show(shift ? 0 : ed.cur); return; }
	if (k == Z_KEY_RIGHT || k == Z_KEY_DOWN || k == Z_KEY_PAGEDOWN) to = next_card();
	else if (k == Z_KEY_LEFT || k == Z_KEY_UP || k == Z_KEY_PAGEUP)
		to = ed.cur > 0 ? ed.cur - 1 : ed.d->loop ? ed.d->ncard - 1 : 0;
	else if (k == Z_KEY_HOME) to = 0;
	else if (k == Z_KEY_END) to = ed.d->ncard - 1;
	if (to >= 0 && to != ed.cur) {
		edit_goto(&ed, to);
		update_card();
	}
}

static void handle_key(uint32_t k, uint8_t mods) {
	bool ctrl = (mods & Z_KBD_MOD_CTRL) != 0, shift = (mods & Z_KBD_MOD_SHIFT) != 0;
	ed.msg[0] = 0;

	if (mode != MODE_COMMAND) {
		// F7, slide's own: the deck as an app, or back to editing it
		if (k == Z_KEY_F7) { set_app_mode(!app_mode); return; }
		if (playing() && !ctrl) {
			bool r;
			pl_sync();
			if (player_key(&pl, k, &r)) { if (r) card_changed(); return; }
		}
		if (app_mode) { app_key(k, shift); return; }
	}

	if (mode == MODE_COMMAND) {
		if (menu_on) { menu_key(k); return; }
		// '/' at the start of the line, or F4 again: the menu
		if ((k == '/' && !cmd_buf[0]) || k == Z_KEY_PROMPT) { menu_open(); return; }
		if (k == 0x1b) { mode = MODE_EDIT; draw_status(); return; }
		if (k == 0x0d) {
			char line[sizeof(cmd_buf)];
			copy_str(line, sizeof(line), cmd_buf);
			mode = MODE_EDIT;
			run_command(line);
			if (mode == MODE_SORTER) draw_sorter();
			update_card();
			draw_bar();
			update_title();
			return;
		}
		if (z_edit_key(&cmd_field, k)) draw_status();
		return;
	}

	// wm sends Ctrl+letter as its control code (Ctrl+M is 13, Ctrl+Z 26:
	// zkbd.c): the letter again, so the shortcuts below can say 'm'
	if (ctrl && k >= 1 && k <= 26) k = 'a' + k - 1;

	if (ctrl && mode != MODE_SORTER && (k == '=' || k == '+' || k == '-' || k == '_' || k == '0')) {
		int vx, vy;
		zoom_anchor(&vx, &vy);
		set_zoom(k == '0' ? 0 : zoom_step(k == '=' || k == '+' ? 1 : -1), vx, vy);
		keep_caret_in_view();
		draw_status();
		return;
	}
	if (k == Z_KEY_HELP) { do_help(); return; }
	if (k == Z_KEY_PROMPT) { open_prompt(""); return; }
	if (k == Z_KEY_RENAME && mode != MODE_SORTER) {
		// edit in place: the text box's text, else the name
		if (ed.text_obj) return;
		if (ed.nsel == 1 && OB_EDITS_TEXT(ed.sel[0]->kind)) {
			edit_text_begin(&ed, ed.sel[0], -1, -1);
			update_card();
		} else {
			open_prompt("name ");
		}
		return;
	}
	if (k == Z_KEY_F5) { do_show(shift ? 0 : (mode == MODE_SORTER ? sort_sel : ed.cur)); return; }
	if (k == Z_KEY_F6) {
		edit_cancel(&ed);
		edit_text_end(&ed);
		if (mode == MODE_SORTER) { edit_goto(&ed, sort_sel); mode = MODE_EDIT; }
		else { mode = MODE_SORTER; sort_sel = ed.cur; sorter_layout(); sorter_keep_visible(); }
		repaint();
		return;
	}
	if (mode == MODE_SORTER) { sorter_key(k); return; }

	if (ed.text_obj) {
		if (k == 0x1b) { edit_text_end(&ed); }
		else if (ctrl && (k == 'v' || k == 'V')) do_paste();
		else if (ctrl && (k == 'z' || k == 'Z')) { edit_text_end(&ed); edit_undo(&ed); }
		else if (ctrl && (k == 's' || k == 'S')) do_save();
		else edit_text_key(&ed, k, mods);
		update_card();
		keep_caret_in_view();
		draw_bar();
		update_title();
		return;
	}

	if (ctrl) {
		switch (k | 32) {
		case 'z': edit_undo(&ed); break;
		case 'y': edit_redo(&ed); break;
		case 'c': do_copy(false); break;
		case 'x': do_copy(true); break;
		case 'v': do_paste(); break;
		case 'd': edit_duplicate(&ed); break;
		case 'g': {
			const char *app;
			edit_command(&ed, shift ? "ungroup" : "group", &app);
			break;
		}
		case 'a': edit_select_all(&ed); break;
		case 'm': edit_new_card(&ed, false); break;
		case 's': do_save(); break;
		case 'p': do_print_dialog(); break;
		case 'o': do_open(); repaint(); break;
		case 'n': do_new(); repaint(); break;
		default: return;
		}
		update_card();
		draw_bar();
		update_title();
		return;
	}

	// Esc stops pictures loading before it does anything else
	if (k == 0x1b && render_loading()) {
		int n = render_load_stop();
		copy_str(ed.msg, sizeof(ed.msg), n == 1 ? "Stopped loading the picture: Enter on it loads it again" :
			"Stopped loading the pictures: Enter on one loads it again");
		edit_damage_all(&ed);
		update_card();
		draw_status();
		return;
	}

	switch (k) {
	case '/':
		open_prompt("");
		return;
	case 0x1b:
		if (ed.npoly || ed.drag) edit_cancel(&ed);
		else edit_select_none(&ed);
		break;
	case 0x0d:
		if (ed.npoly) edit_finish_poly(&ed);
		else if (ed.nsel == 1 && OB_EDITS_TEXT(ed.sel[0]->kind)) edit_text_begin(&ed, ed.sel[0], -1, -1);
		else if (ed.nsel == 1 && render_load_retry(ed.sel[0])) edit_damage_obj(&ed, ed.sel[0]);
		break;
	case Z_KEY_DELETE: case 0x7f:
		edit_delete(&ed);
		break;
	case Z_KEY_LEFT: edit_nudge(&ed, shift ? -10 : -1, 0); break;
	case Z_KEY_RIGHT: edit_nudge(&ed, shift ? 10 : 1, 0); break;
	case Z_KEY_UP: edit_nudge(&ed, 0, shift ? -10 : -1); break;
	case Z_KEY_DOWN: edit_nudge(&ed, 0, shift ? 10 : 1); break;
	case Z_KEY_PAGEUP: edit_goto(&ed, ed.cur - 1); break;
	case Z_KEY_PAGEDOWN: edit_goto(&ed, ed.cur + 1); break;
	case 0x09: {
		card_t *c = edit_card(&ed);
		if (c && c->nobj) {
			int i = ed.nsel ? card_index_of(c, ed.sel[0]) : -1;
			i = shift ? (i <= 0 ? c->nobj - 1 : i - 1) : (i + 1) % c->nobj;
			edit_select(&ed, c->obj[i], false);
		}
		break;
	}
	default: {
		unsigned i;
		for (i = 0; i < sizeof(tool_keys) / sizeof(tool_keys[0]); i++)
			if ((uint32_t)tool_keys[i].key == (k | 32) && k < 128) { set_tool(tool_keys[i].tool); break; }
		break;
	}
	}
	update_card();
	draw_bar();
	update_title();
}

static uint32_t last_click_tick;
static int last_click_x, last_click_y;
static uint8_t btn_prev;

static void bar_click(int cx, int cy) {
	int y = BAR_Y() - 1, i;		// as draw_bar() draws it
	obj_t *o = one_sel();
	if (cy < y + 3 || cy > y + 3 + SW_SIZE + 2) return;
	for (i = 0; i < NSWATCH; i++) {
		int x = SWATCH_X(i) + TOOL_W;
		if (cx >= x - 1 && cx < x + SW_SIZE + 1) {
			char c[24];
			str_t s = { c, 0, sizeof(c) };
			if (i == 0) s_add(&s, "fill none");
			else { s_add(&s, "fill "); s_int(&s, i - 1); }
			run_command(c);
			return;
		}
	}
	for (i = 0; i < NWIDTH; i++) {
		int x = WIDTH_X(i) + TOOL_W;
		if (cx >= x - 1 && cx < x + 12) {
			char c[16];
			str_t s = { c, 0, sizeof(c) };
			s_add(&s, "line ");
			s_int(&s, widths[i]);
			run_command(c);
			return;
		}
	}
	if (cx >= FONTBTN_X + TOOL_W && cx < FONTBTN_X + TOOL_W + FONTBTN_W && o && OB_HAS_FONT(o->kind)) {
		// the next family; shift-click... the size is the "size" command
		const char *cur = ed.d->font[o->font < ed.d->nfont ? o->font : 0];
		int k, next = 0;
		for (k = 0; k < NFAMILY; k++) if (!strcmp(families[k], cur)) next = (k + 1) % NFAMILY;
		for (k = 0; k < NFAMILY && !render_font_exists(families[next]); k++) next = (next + 1) % NFAMILY;
		{
			char c[32];
			str_t s = { c, 0, sizeof(c) };
			s_add(&s, "font ");
			s_add(&s, families[next]);
			run_command(c);
		}
		return;
	}
	for (i = 0; i < 3; i++) {
		int bx = STYLE_X(i) + TOOL_W;
		if (cx >= bx && cx < bx + 12) {
			static const char *cmds[3] = { "bold", "italic", "underline" };
			run_command(cmds[i]);
			return;
		}
	}
}

static void handle_mouse(uint32_t packed) {
	int cx, cy;
	uint8_t b = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);
	bool down = (b & Z_MOUSE_BTN_LEFT) && !(btn_prev & Z_MOUSE_BTN_LEFT);
	bool up = !(b & Z_MOUSE_BTN_LEFT) && (btn_prev & Z_MOUSE_BTN_LEFT);
	bool shift = (mods_now & Z_KBD_MOD_SHIFT) != 0;
	btn_prev = b;
	if (!z_win_mouse_content_xy(&win, packed, &cx, &cy)) {
		if (!up) return;
	}
	if (menu_on) {
		if (down) menu_click(cx, cy);
		return;
	}
	if (mode != MODE_SORTER && !ed.drag && !ed.npoly) {
		// the scrollbars, zoomed in
		if (sb_h_on && z_scrollbar_has_pointer(&sb_h, cx, cy)) {
			if (z_scrollbar_mouse(&sb_h, cx, cy, b)) scroll_to(sb_h.value, scroll_y);
			return;
		}
		if (sb_v_on && z_scrollbar_has_pointer(&sb_v, cx, cy)) {
			if (z_scrollbar_mouse(&sb_v, cx, cy, b)) scroll_to(scroll_x, sb_v.value);
			return;
		}
	}
	if (mode == MODE_COMMAND && down) { mode = MODE_EDIT; draw_status(); }

	if (down) {
		uint32_t now = z_uptime_ticks();
		bool dbl = now - last_click_tick < Z_DOUBLE_CLICK_TICKS &&
			cx - last_click_x < 4 && last_click_x - cx < 4 &&
			cy - last_click_y < 4 && last_click_y - cy < 4;
		last_click_tick = dbl ? 0 : now;
		last_click_x = cx;
		last_click_y = cy;
		ed.msg[0] = 0;

		if (cx < TOOL_W) {
			int t = (cy - 2) / (TOOL_H + 1);
			if (t >= 0 && t < TOOL_COUNT) set_tool(t);
			draw_bar();
			return;
		}
		if (cy >= BAR_Y()) {
			bar_click(cx, cy);
			update_card();
			draw_bar();
			update_title();
			return;
		}
		if (mode == MODE_SORTER) {
			int i = sorter_hit(cx, cy);
			if (i < 0) return;
			if (dbl) { edit_goto(&ed, i); mode = MODE_EDIT; repaint(); return; }
			sort_sel = i;
			draw_sorter();
			draw_bar();
			return;
		}
		if (playing()) {
			// a control takes the click; elsewhere it turns the card
			bool took;
			pl_sync();
			if (player_press(&pl, to_units(cx - card_x + scroll_x), to_units(cy - card_y + scroll_y), &took))
				card_changed();
			if (!took) {
				edit_goto(&ed, shift ? ed.cur - 1 : next_card());
				update_card();
				draw_bar();
			}
			return;
		}
		edit_press(&ed, to_units(cx - card_x + scroll_x), to_units(cy - card_y + scroll_y), shift, dbl);
		update_card();
		draw_bar();
		return;
	}
	if (mode == MODE_SORTER) {
		if (up && b == 0) {
			// a card dragged onto another moves there
			int i = sorter_hit(cx, cy);
			if (i >= 0 && i != sort_sel && last_click_tick) {
				edit_move_card(&ed, sort_sel, i);
				sort_sel = i;
				draw_sorter();
				draw_bar();
				update_title();
			}
		}
		return;
	}
	if (playing() && (pl.pressed || pl.drag)) {
		int ux = to_units(cx - card_x + scroll_x), uy = to_units(cy - card_y + scroll_y);
		if (up ? player_release(&pl, ux, uy) : player_motion(&pl, ux, uy)) card_changed();
		return;
	}
	if (ed.drag || ed.npoly) {
		edit_motion(&ed, to_units(cx - card_x + scroll_x), to_units(cy - card_y + scroll_y), shift);
		if (up) edit_release(&ed, to_units(cx - card_x + scroll_x), to_units(cy - card_y + scroll_y));
		update_card();
		if (up) { draw_bar(); update_title(); }
	}
}

// -- messages -----------------------------------------------------------------------------------------------------

static void forward_msg(z_msg_t *msg, void *user) {
	(void)user;
	switch (msg->subject) {
	case Z_WM_SET_CLIP:
		z_win_apply_clip(&win, &msg->obj);
		break;
	case Z_WM_REDRAW:
		if (msg->obj.type != Z_UINT32) break;
		if (z_win_redraw_id(msg->obj.val.uint32) != win.id) break;
		z_win_apply_redraw(&win, msg->obj.val.uint32);
		z_win_damage_ignore(&win);
		repaint();
		z_win_redraw_done(&win);
		break;
	case Z_WM_WINDOW_MOVED:
		z_win_parse_rect(&win, &msg->obj);
		break;
	case Z_WM_WINDOW_RESIZED:
		z_win_apply_resized(&win, &msg->obj);
		layout();
		// the card may sit elsewhere in the new size: drawn there now,
		// not when wm next asks
		repaint();
		break;
	default:
		break;
	}
}

// A first deck for someone who starts slide with nothing: one card that
// says how to begin.
static deck_t *starter_deck(void) {
	deck_t *d = deck_new();
	obj_t *o;
	if (!d) return NULL;
	o = obj_new(OB_TEXT);
	if (o) {
		obj_set_box(o, 20, 120, d->page.gw - 40, 60);
		o->font = (uint8_t)deck_font_index(d, "serif");
		o->size = 28;
		o->align = AL_CENTER;
		o->valign = VA_MIDDLE;
		copy_str(o->name, OB_NAME_MAX, "title");
		obj_set_text(o, "A new deck", 10);
		card_add(d->card[0], -1, o);
	}
	return d;
}

int main(void) {
	static uint8_t scratch[96 * 1024];
	char arg[sizeof(path)];
	bool have_arg;
	int cw, ch;

	have_arg = z_launch_path_take(arg, sizeof(arg));

	if (!mem_setup()) {
		// no arena on this board (1MB boards have none): say so where it
		// can be seen, without a window of our own
		dlg.parent = NULL;
		dlg.on_msg = NULL;
		z_dialog_confirm(&dlg, "slide", "slide needs a board with more\nmemory than this one has.", Z_DIALOG_OK_CANCEL);
		return 1;
	}
	render_init(scratch, sizeof(scratch), load_file);
	{
		void *gc = pool_alloc(128 * 1024);
		if (gc) zc_text_cache(gc, 128 * 1024);
	}
	make_icons();
	show_alloc();

	g_deck = starter_deck();
	edit_init(&ed, g_deck);

	wanted_size(&cw, &ch);
	if (z_win_create_flags(&win, "slide", (uint32_t)cw, (uint32_t)ch, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_RESIZABLE | Z_WIN_FLAG_NEW_ICON |
		Z_WIN_FLAG_OPEN_ICON | Z_WIN_FLAG_SAVE_ICON | Z_WIN_FLAG_PRINT_ICON) != Z_OK)
		return 1;
	dlg.parent = &win;
	dlg.on_msg = forward_msg;
	dlg.user = NULL;
	act_host_editor();

	if (have_arg) load_path(arg);
	layout();
	update_title();

	while (!quit_now) {
		z_msg_t msg;
		int mouse_pending = 0;
		uint32_t mouse_last = 0;

		while (z_msg_read(&msg) == Z_OK) {
			// a window a script made has its own messages
			if (script_window_msg(&msg)) continue;
			switch (msg.subject) {
			case Z_WM_KEY:
				if (msg.obj.type != Z_UINT32) break;
				mods_now = (uint8_t)Z_WM_UNPACK_KEY_MODIFIERS(msg.obj.val.uint32);
				if (!Z_WM_UNPACK_KEY_PRESSED(msg.obj.val.uint32)) break;
				if (mouse_pending) { handle_mouse(mouse_last); mouse_pending = 0; }
				handle_key(Z_WM_UNPACK_KEY_KEYSYM(msg.obj.val.uint32), mods_now);
				break;
			case Z_WM_MOUSE:
				if (msg.obj.type != Z_UINT32) break;
				// motion coalesces; a change of buttons is handled at once
				if (mouse_pending && Z_WM_UNPACK_MOUSE_BUTTONS(msg.obj.val.uint32) ==
					Z_WM_UNPACK_MOUSE_BUTTONS(mouse_last)) {
					mouse_last = msg.obj.val.uint32;
				} else {
					if (mouse_pending) handle_mouse(mouse_last);
					mouse_last = msg.obj.val.uint32;
					mouse_pending = 1;
					if (Z_WM_UNPACK_MOUSE_BUTTONS(mouse_last) != btn_prev) {
						handle_mouse(mouse_last);
						mouse_pending = 0;
					}
				}
				break;
			case Z_WM_WHEEL:
				if (msg.obj.type != Z_UINT32) break;
				if (mode == MODE_SORTER) {
					int rows = (ed.d->ncard + thumb_cols - 1) / thumb_cols;
					sort_top -= Z_WM_WHEEL_NOTCHES(msg.obj.val.uint32);
					if (sort_top > rows - sorter_rows_visible()) sort_top = rows - sorter_rows_visible();
					if (sort_top < 0) sort_top = 0;
					draw_sorter();
				} else if (zoom && (sb_h_on || sb_v_on)) {
					// zoomed in: the wheel scrolls (Shift: across)
					int n = Z_WM_WHEEL_NOTCHES(msg.obj.val.uint32) * 40;
					if ((mods_now & Z_KBD_MOD_SHIFT) || !sb_v_on) scroll_to(scroll_x - n, scroll_y);
					else scroll_to(scroll_x, scroll_y - n);
				} else {
					edit_goto(&ed, ed.cur - (Z_WM_WHEEL_NOTCHES(msg.obj.val.uint32) > 0 ? 1 : -1));
					update_card();
					draw_bar();
				}
				break;
			case Z_WM_TITLEBAR_ICON: {
				uint32_t v;
				if (msg.obj.type != Z_UINT32) break;
				v = msg.obj.val.uint32;
				if ((int)Z_WM_UNPACK_TBICON_ID(v) != win.id) break;
				switch (Z_WM_UNPACK_TBICON_KIND(v)) {
				case Z_WM_TBICON_NEW: do_new(); repaint(); break;
				case Z_WM_TBICON_OPEN: do_open(); repaint(); break;
				case Z_WM_TBICON_SAVE: do_save(); break;
				case Z_WM_TBICON_PRINT: do_print_dialog(); repaint(); break;
				default: break;
				}
				update_title();
				break;
			}
			case Z_WM_CLOSE:
				if (msg.obj.type == Z_UINT32 && (int32_t)msg.obj.val.uint32 == win.id && may_discard())
					quit_now = true;
				break;
			default:
				if (z_print_msg(&printing, &msg)) {
					if (!printing.running) {
						copy_str(ed.msg, sizeof(ed.msg), printing.result == 0 ? "Printed" :
							printing.result == Z_PRINT_E_CANCELLED ? "Printing cancelled" :
							"Printing failed: the print window says why");
						draw_status();
					}
					break;
				}
				forward_msg(&msg, NULL);
				break;
			}
			if (quit_now) break;
		}
		if (mouse_pending) handle_mouse(mouse_last);
		if (quit_now) break;
		apply_dirty();
		take_open_request();
		pl_check_card();
		sedit_poll();
		if (act_waiting()) act_tick();
		if (script_timers()) script_tick();
		apply_dirty();
		update_title();
		if (render_loading()) {
			// nothing waiting: a slice of a picture (docs/responsive.md),
			// then straight back to the messages
			static uint32_t last_shown;
			bool fin;
			obj_t *o = render_load_step(Z_TICK_HZ / 50, &fin);
			uint32_t now = z_uptime_ticks();
			if (o && fin) {
				picture_done(o);
				if (mode == MODE_SORTER) draw_sorter(); else update_card();
				load_status();
			} else if (o && now - last_shown >= Z_TICK_HZ / 8) {
				// its progress bar, a few times a second
				last_shown = now;
				if (mode != MODE_SORTER && on_shown_card(o)) { edit_damage_obj(&ed, o); update_card(); }
				load_status();
			}
			continue;
		}
		if (load_msg) load_status();
		z_proc_wait(act_waiting() || script_timers() || pl.pressed || pl.drag ? Z_TICK_HZ / 50 : Z_TICK_HZ / 30);
	}
	z_win_destroy(&win);
	return 0;
}
