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
#include "deck.h"
#include "deckio.h"
#include "render.h"
#include "edit.h"
#include "show.h"
#include "printjob.h"

// -- layout --------------------------------------------------------------------

#define TOOL_W      24		// the tool column
#define TOOL_H      20
#define MARGIN      6
#define BAR_H       30		// the property strip and the status line
#define SW_SIZE     11		// a fill swatch
#define FONT        (&z_font_5x8)

static z_win_t win;
static z_dialog_ctx_t dlg;

#define BAR_Y()      (z_win_content_h(&win) - BAR_H)
static editor_t ed;

static int card_x, card_y;		// the card's top-left, content coordinates
static int view_w, view_h;		// the card on screen, pixels
static int32_t scale16;			// pixels per unit, 16.16
static uint32_t *cbuf, *dbuf;	// the rendered card; it plus overlays
static int bwpl, bw, bh;		// their shape

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

static void layout(void) {
	int cw = z_win_content_w(&win), ch = z_win_content_h(&win);
	int aw = cw - TOOL_W - 2 * MARGIN, ah = ch - BAR_H - 2 * MARGIN;
	int32_t sx, sy, s;
	deck_t *d = ed.d;
	if (aw < 32) aw = 32;
	if (ah < 32) ah = 32;
	sx = (int32_t)(((int64_t)aw << 16) / d->page.gw);
	sy = (int32_t)(((int64_t)ah << 16) / d->page.gh);
	s = sx < sy ? sx : sy;
	if (s > ZC_XF_ONE) s = ZC_XF_ONE;		// never larger than 100% in the editor
	scale16 = s;
	view_w = to_px(d->page.gw);
	view_h = to_px(d->page.gh);
	card_x = TOOL_W + MARGIN + (aw - view_w) / 2;
	card_y = MARGIN + (ah - view_h) / 2;
	if (card_x < TOOL_W + MARGIN) card_x = TOOL_W + MARGIN;
	if (card_y < MARGIN) card_y = MARGIN;
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

	if (ed.tool != TOOL_BROWSE) {
		for (i = 0; i < ed.nsel; i++) {
			int hx[8], hy[8], n = edit_handles(ed.sel[i], hx, hy), k;
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
	zc_clip(&t, w0 * 32, y0, (w1 + 1) * 32, y1);
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
	if (ed.all_dirty) {
		px0 = 0; py0 = 0; px1 = bw; py1 = bh;
		card_target(&t, cbuf);
		render_card(&t, &xf, ed.d, edit_card(&ed), ed.tool == TOOL_BROWSE ? RENDER_SHOW : RENDER_EDIT);
	} else if (ed.dx1 >= ed.dx0) {
		units_rect_to_px(ed.dx0, ed.dy0, ed.dx1, ed.dy1, &px0, &py0, &px1, &py1);
		card_target(&t, cbuf);
		zc_clip(&t, px0, py0, px1, py1);
		if (!zc_clip_empty(&t))
			render_card(&t, &xf, ed.d, edit_card(&ed), ed.tool == TOOL_BROWSE ? RENDER_SHOW : RENDER_EDIT);
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

static void draw_bar(void) {
	int y = BAR_Y(), i;
	obj_t *o = one_sel();
	z_win_fill_rect(&win, TOOL_W, y, z_win_content_w(&win) - TOOL_W, BAR_H, 0);
	z_win_fill_rect(&win, 0, y, z_win_content_w(&win), 1, 1);
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
		if (o && o->kind == OB_TEXT) {
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
			bool on = o && o->kind == OB_TEXT && (o->style & (1 << i));
			z_win_fill_rect(&win, bx, y + 3, 12, SW_SIZE + 2, on ? 1 : 0);
			if (!on) z_win_fill_rect(&win, bx + 1, y + 4, 10, SW_SIZE, 0);
			z_win_draw_text2(&win, bx + 4, y + 6, lab[i], on ? 0 : 1, on ? 1 : 0, FONT);
		}
	}
	draw_status();
}

static void draw_status(void) {
	int y = BAR_Y() + 18;
	int w = z_win_content_w(&win);
	char t[160];
	str_t s = { t, 0, sizeof(t) };
	z_win_fill_rect(&win, 0, y - 1, w, 12, 0);
	if (mode == MODE_COMMAND) {
		z_win_draw_text(&win, 4, y + 1, "/", 1, FONT);
		z_edit_draw(&win, &cmd_field, 12, y - 1, w - 16, 11, FONT);
		return;
	}
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
	z_win_fill_pattern(&win, TOOL_W, 0, cw - TOOL_W, card_y - 1, desk);
	z_win_fill_pattern(&win, TOOL_W, card_y + view_h + 1, cw - TOOL_W, ch - card_y - view_h - 1, desk);
	z_win_fill_pattern(&win, TOOL_W, card_y - 1, card_x - TOOL_W - 1, view_h + 2, desk);
	z_win_fill_pattern(&win, card_x + view_w + 1, card_y - 1, cw - card_x - view_w - 1, view_h + 2, desk);
	// a frame round the card
	z_win_fill_rect(&win, card_x - 1, card_y - 1, view_w + 2, 1, 1);
	z_win_fill_rect(&win, card_x - 1, card_y + view_h, view_w + 2, 1, 1);
	z_win_fill_rect(&win, card_x - 1, card_y, 1, view_h, 1);
	z_win_fill_rect(&win, card_x + view_w, card_y, 1, view_h, 1);
}

static void repaint(void) {
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
	return true;
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
		uint32_t off = 0;
		ok = true;
		while (off < b.len) {
			int n = (int)(b.len - off > 4096 ? 4096 : b.len - off);
			if (fs_write_chunk(h, b.buf + off, n) != n) { ok = false; break; }
			off += (uint32_t)n;
		}
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

static bool do_save_as(void) {
	char p[200];
	if (!z_dialog_save(&dlg, last_dir, path[0] ? path : "deck.zsl", p, sizeof(p))) return false;
	if (!ends_with_zsl(p) && strlen(p) + 4 < sizeof(p)) strcat(p, ".zsl");
	if (fs_size(p) > 0 && strcmp(p, path) &&
		z_dialog_confirm(&dlg, "Save as", "That file exists. Replace it?", Z_DIALOG_YES_NO) != Z_DIALOG_YES)
		return false;
	return save_to(p);
}

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

static void do_print(const char *line) {
	pj_settings_t s;
	char err[64], keep[DECK_PRINT_MAX], file[200];
	const char *ext;
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
		if (!z_dialog_save(&dlg, last_dir, p, file, sizeof(file))) return;
		{
			size_t l = strlen(file), e = strlen(ext);
			if ((l < e || strcmp(file + l - e, ext)) && l + e < sizeof(file)) strcat(file, ext);
		}
		if (fs_size(file) > 0 &&
			z_dialog_confirm(&dlg, "Print to a file", "That file exists. Replace it?", Z_DIALOG_YES_NO) != Z_DIALOG_YES)
			return;
	}
	if (!z_print_begin(&printing, ed.d->title[0] ? ed.d->title : (path[0] ? path : "slide"))) {
		copy_str(ed.msg, sizeof(ed.msg), "Cannot print: no room in /tmp");
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
		copy_str(ed.msg, sizeof(ed.msg), "Cannot print: no room in /tmp for the job");
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
		"l a p i\tLine, arrow, polygon, image\n"
		"Shift+drag\tSquares, circles, 45 degrees\n"
		"Enter\tEdit the text box; end a polygon\n"
		"/ or F4\tCommands: font, fill, line, align, "
		"page, go, name ... /tour shows them\n"
		"/ again\tEvery command, to pick from\n"
		"F2\tEdit the text box; name the selection\n"
		"Esc\tStop loading pictures (Enter: again)\n"
		"F5\tShow (Shift+F5: from card 1)\n"
		"F6\tAll cards: drag to reorder\n"
		"PgUp/PgDn\tPrevious, next card\n"
		"Ctrl+M\tNew card\n"
		"Ctrl+Z/Y\tUndo, redo\n"
		"Ctrl+C/X/V\tCopy, cut, paste\n"
		"Ctrl+D\tDuplicate\n"
		"Ctrl+N/O/S\tNew, open, save\n"
		"Ctrl+P\tPrint (/print pdf: a PDF file)\n\n"
		"In a show: Space or a click goes on, Left goes back, "
		"B blanks, Esc ends.";
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

static void do_image(void) {
	char p[200], ref[200];
	obj_t *o;
	int w = ed.d->page.gw * 2 / 3, h = w * 3 / 4;
	ed.tool = TOOL_SELECT;
	if (!z_dialog_open(&dlg, last_dir, p, sizeof(p))) return;
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

static void do_show(int from) {
	int end;
	edit_cancel(&ed);
	edit_text_end(&ed);
	end = show_run(ed.d, from, &win);
	if (mode != MODE_SORTER) edit_goto(&ed, end);
	edit_damage_all(&ed);
	// wm repaints the desktop and asks for this window's content
}

// -- commands ------------------------------------------------------------------------------------------------

static bool quit_now;

static void run_command(const char *line) {
	const char *app = NULL;
	int gw = ed.d->page.gw, gh = ed.d->page.gh;
	ed.msg[0] = 0;
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
		if (!strncmp(r, "save as", 7)) do_save_as(); else do_save();
	}
	else if (!strcmp(app, "new deck")) do_new();
	else if (!strcmp(app, "image")) do_image();
	else if (!strcmp(app, "help")) do_help();
	else if (!strcmp(app, "print")) do_print(line);
	else if (!strcmp(app, "tour")) { if (may_discard()) load_path(Z_PATH_SLIDE_TOUR); }
	else if (!strcmp(app, "quit")) { if (may_discard()) quit_now = true; }
}

// -- input ---------------------------------------------------------------------------------------------------

static const struct { char key; int tool; } tool_keys[] = {
	{ 'b', TOOL_BROWSE }, { 'v', TOOL_SELECT }, { 't', TOOL_TEXT }, { 'r', TOOL_RECT },
	{ 'u', TOOL_RRECT }, { 'o', TOOL_OVAL }, { 'l', TOOL_LINE }, { 'a', TOOL_ARROW },
	{ 'p', TOOL_POLY }, { 'i', TOOL_IMAGE },
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

static void handle_key(uint32_t k, uint8_t mods) {
	bool ctrl = (mods & Z_KBD_MOD_CTRL) != 0, shift = (mods & Z_KBD_MOD_SHIFT) != 0;
	ed.msg[0] = 0;

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

	if (k == Z_KEY_HELP) { do_help(); return; }
	if (k == Z_KEY_PROMPT) { open_prompt(""); return; }
	if (k == Z_KEY_RENAME && mode != MODE_SORTER) {
		// edit in place: the text box's text, else the name
		if (ed.text_obj) return;
		if (ed.nsel == 1 && ed.sel[0]->kind == OB_TEXT) {
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
		case 'a': edit_select_all(&ed); break;
		case 'm': edit_new_card(&ed, false); break;
		case 's': do_save(); break;
		case 'p': do_print(""); break;
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
		else if (ed.nsel == 1 && ed.sel[0]->kind == OB_TEXT) edit_text_begin(&ed, ed.sel[0], -1, -1);
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
	int y = BAR_Y(), i;
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
	if (cx >= FONTBTN_X + TOOL_W && cx < FONTBTN_X + TOOL_W + FONTBTN_W && o && o->kind == OB_TEXT) {
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
		if (ed.tool == TOOL_BROWSE) {
			edit_goto(&ed, shift ? ed.cur - 1 : ed.cur + 1);
			update_card();
			draw_bar();
			return;
		}
		edit_press(&ed, to_units(cx - card_x), to_units(cy - card_y), shift, dbl);
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
	if (ed.drag || ed.npoly) {
		edit_motion(&ed, to_units(cx - card_x), to_units(cy - card_y), shift);
		if (up) edit_release(&ed, to_units(cx - card_x), to_units(cy - card_y));
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
		Z_WIN_FLAG_OPEN_ICON | Z_WIN_FLAG_SAVE_ICON) != Z_OK)
		return 1;
	dlg.parent = &win;
	dlg.on_msg = forward_msg;
	dlg.user = NULL;

	if (have_arg) load_path(arg);
	layout();
	update_title();

	while (!quit_now) {
		z_msg_t msg;
		int mouse_pending = 0;
		uint32_t mouse_last = 0;

		while (z_msg_read(&msg) == Z_OK) {
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
		z_proc_wait(Z_TICK_HZ / 30);
	}
	z_win_destroy(&win);
	return 0;
}
