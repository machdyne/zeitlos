/*
 * pix -- a pixel editor for sprites, icons, fonts and pictures. See
 * docs/pix_app.md.
 *
 *   > run pix                      a new sprite sheet
 *   > run pix /data/wm/dock.zic    the dock's icons
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Built on the arcade runtime (sw/common/games/zarcade.h): one 320x240
 * picture, the same in the window and full screen (F11). This file is
 * the screen and the input; the document is px_doc.c, the file formats
 * px_io.c, the tools and undo px_edit.c, animations px_anim.c.
 *
 * -- the screen --
 *
 *   +--------------------------------------+---------+
 *   | the canvas: the cell, zoomed,        | tools   |
 *   | 256 x 192                            | colours |
 *   |                                      | buttons |
 *   +----------+---------------------------+---------+
 *   | preview  | the cells, 1:1 or shrunk to fit     |
 *   +----------+-------------------------------------+
 *   | status                                         |
 *   +------------------------------------------------+
 *
 * Icons, fonts and pictures are two colours. A sprite sheet has a
 * palette of 2 to 256 colours, each with what stands in for it with
 * fewer (sw/common/zsprite.h): full screen shows the art in colour, as
 * many as the board has, through those tables; the window shows each
 * colour in a pattern of its own, and the preview the real
 * monochrome result.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "../../common/games/zarcade.h"
#include "../../common/games/ui/sui.h"
#include "../../common/zkbd.h"
#include "../../common/zpaths.h"
#ifndef AR_HOST_TESTS
#include "../../common/zeitlos.h"
#include "../../common/zwin.h"
#include "../../common/zfsapp.h"
#endif

#include "px_doc.h"
#include "px_io.h"
#include "px_edit.h"
#include "px_anim.h"
#include "px_import.h"

/* ===================================================================
 * the layout
 * =================================================================== */

#define CV_X 0
#define CV_Y 0
#define CV_W 256
#define CV_H 192

#define SD_X 258            /* the side column */
#define SD_W (AR_W - SD_X)

#define BT_Y 194            /* the bottom band */
#define PV_X 0              /* the preview */
#define PV_W 56
#define ST_X 58             /* the cell strip */
#define ST_W (AR_W - ST_X)
#define ST_H 36
#define SL_Y 232            /* the status line */

/* The controls' colours, as roles. ui_c[] is the index each is drawn
 * in right now (colours_setup()): 0..7 in monochrome, where the inks
 * below give them their look; in colour, entries the art leaves free at
 * the top of the palette, or the art's darkest and lightest when it
 * uses every one. */
enum { R_BG, R_INK, R_OUT, R_GRID, R_FRAME, R_HI, R_GUIDE, R_WARN, R_COUNT };
static int ui_c[R_COUNT] = { 0, 1, 2, 3, 4, 5, 6, 7 };
#define C_BG     ui_c[R_BG]      /* the panels */
#define C_INK    ui_c[R_INK]     /* text */
#define C_OUT    ui_c[R_OUT]     /* the canvas around the cell */
#define C_GRID   ui_c[R_GRID]
#define C_FRAME  ui_c[R_FRAME]
#define C_HI     ui_c[R_HI]      /* cursor, selection, the current cell */
#define C_GUIDE  ui_c[R_GUIDE]   /* a font's ascent, the pointer's centre */
#define C_WARN   ui_c[R_WARN]

static const uint16_t ui_rgb[R_COUNT] = {
	0x000, 0xfff, 0x223, 0x445, 0x667, 0xfd2, 0x4af, 0xf44
};

static const char help_text[] =
	"pix draws pixel art: dock icons (.zic), fonts (.zfn), sprite sheets "
	"(.zsp, the pointer's too) and pictures (.zbm, in up to 256 colours). "
	"Opening a PNG, GIF, BMP or JPEG makes a colour picture of it.\n\n"
	"Mouse\tleft draws the first colour, right the second\n"
	"Arrows\tmove the cursor; Space draws there\n"
	"Shift+arrows\tscroll\n"
	"P L R G K S\tpencil, line, rectangle, fill, pick, select\n"
	"R again\trectangle outline or filled\n"
	"0-9 A-F\ta colour of this page of 16; . transparent\n"
	"X\tswap the first and second colours\n"
	"Shift+PgUp PgDn\tthe page of colours (more than 16)\n"
	"F7\tshow as 64, 16, 4 or 2 colours, then as it is\n"
	"F8\tthe palette: RGB, what stands in with fewer colours, how many\n"
	"F5\tplay the cell's animation in the preview, or stop\n"
	"Shift+F5\tthe animation: its name, ms a frame, its cells\n"
	"F6\tonion skin: the frame before, then before and after, then off\n"
	"+ -\tzoom\n"
	"PgUp PgDn\tprevious and next cell (Home, End)\n"
	"Ins\tnew cell; Ctrl+D a copy of this one (the next frame)\n"
	"Shift+Del\tdelete this cell\n"
	"H V T\tflip across, flip down, turn (the selection or the cell)\n"
	"Ctrl+arrows\tshift with wrap\n"
	"Del\tclear the selection\n"
	"Ctrl+C X V A\tcopy, cut, paste, select all\n"
	"Ctrl+Z Y\tundo, redo\n"
	"F2\trename the cell\n"
	"F3\tgo to a cell by name or character\n"
	"Ctrl+N O S\tnew, open, save; Ctrl+Shift+S save as\n"
	"Ctrl+I\ta picture (PNG, GIF, BMP, JPEG) into this cell, in its colours\n"
	"F10\tcolour on or off, in the window too\n"
	"F11\tfull screen\n\n"
	"Ctrl+N asks what to make: icon, cursor, sprite 16 16 (and how many "
	"colours: sprite 16 16 64), font 6 12, picture 320 240 (in colour: "
	"picture 320 240 256).\n\n"
	"The dock's icons: open " Z_PATH_WM_ICONS ". Saving writes only the "
	"icons you changed or added, to " Z_PATH_WM_ICON_OVERRIDES ".";

static void pix_paint(void *user, const ar_rect_t *r);

static ar_desc_t desc = {
	"pix", "pix help", help_text, "pix_app",
	{ 0 },
	{ { { 0 } } },
	pix_paint, NULL,
	false,          /* full screen in colour where the board has it: C is a letter here */
	NULL,
	true            /* the pointer stays, full screen too */
};

/* ===================================================================
 * state
 * =================================================================== */

enum { T_PEN, T_LINE, T_RECT, T_FILL, T_PICK, T_SEL, T_COUNT };

static const char *const tool_name[T_COUNT] = {
	"Pencil", "Line", "Rect", "Fill", "Pick", "Select"
};
static const char tool_key[T_COUNT] = { 'p', 'l', 'r', 'g', 'k', 's' };

static px_doc_t doc;
static char path[160];          /* where it came from / goes; "" new */
static char suggest[160];       /* a name to save as: an imported picture's, as .zbm */
static bool overrides;          /* the dock: saving writes icons.zic */
static int cur;                 /* the cell */
static int zoom = 8;
static int vx, vy;              /* the view's top left, in cell pixels */
static int cx, cy;              /* the keyboard cursor */
static int tool = T_PEN;
static bool rect_filled;
static bool kbd_cursor = true;  /* the keyboard cursor shown: a key moved it last, not the mouse */
static int fg = 1;              /* the first colour (left button, Space) */
static int bg2 = 0;             /* the second (right button): ZSP_T, often */
static int page;                /* the 16 colours the chips and 0-F show */
static int view_tier;           /* F7: 0 as the sheet is; or 64, 16, 4, 2 */
static int shown_n = 16;        /* colours shown: full screen's, or the
                                   colour window's (32 or 8) */
static int art_tier = 16;       /* what art is drawn at, in colour */
static int planes_n = 4;

/* the palette editor (F8), over the canvas */
static bool editor;
static int ed_field;
static int differ_from = -1;

/* a stroke in progress: mouse button down, or a line/rect begun with
 * Space at the keyboard cursor */
static bool stroking;
static int stroke_v;
static int sx0, sy0, sx1, sy1;  /* line and rect: from, to */
static int lastx, lasty;        /* pencil: the last point */

/* the selection: a rectangle; and, while it is being moved, the lifted
 * pixels floating over the cell */
static bool sel;
static int selx, sely, selw, selh;
static bool floating;
static px_piece_t flt;
static int fltx, flty;
static int drag_dx, drag_dy;    /* where in the selection it was grabbed */
static bool dragging_sel;

static px_piece_t clip;

/* animation: F5 plays in the preview, F6 shows the frames around this
 * one under it */
static bool playing;
static int play_at;             /* the frame shown */
static int play_left;           /* screen frames until the next */
static int play_cell = -1;      /* the cell it shows */
static int onion;               /* 0 off, 1 the frame before, 2 before and after */

static char msg[64];            /* the status line's message, for a while */
static int msg_frames;
static bool quit;

/* ===================================================================
 * small helpers
 * =================================================================== */

static bool have_cell(void) { return cur >= 0 && cur < doc.ncells; }
static int cw(void) { return have_cell() ? doc.cell[cur].w : 1; }
static int ch(void) { return have_cell() ? doc.cell[cur].h : 1; }

static void say(const char *s) { ar_say(s); }
static void set_colour(int c);

static void message(const char *s) {
	strncpy(msg, s, sizeof(msg) - 1);
	msg[sizeof(msg) - 1] = 0;
	msg_frames = 60 * 4;
	ar_dirty(0, SL_Y, AR_W, AR_H - SL_Y);
	say(s);
}

/* The file's name for the status line: its last part, or "untitled",
 * and a "*" while there are changes not saved. */
static char shown_name[48];
static void file_label(char *out, int cap) {
	const char *base = path[0] ? strrchr(path, '/') : NULL;
	base = path[0] ? (base ? base + 1 : path) : "untitled";
	snprintf(out, (size_t)cap, "%.40s%s", base, doc.dirty ? "*" : "");
}

/* The canvas, and the preview, which shows what it shows. */
static void dirty_canvas(void) {
	ar_dirty(CV_X, CV_Y, CV_W, CV_H);
	ar_dirty(PV_X, BT_Y + 1, PV_W, ST_H);
}
static void dirty_side(void) { ar_dirty(SD_X - 2, 0, SD_W + 2, BT_Y); }
/* Parts of it: the tools; the colours (with the first and second, and
 * the page's line at the foot) */
static void dirty_zoom(void) { ar_dirty(SD_X - 2, 2 + 6 * 10 + 3 + 48 + 2 + 10, SD_W + 2, 9); }
static void dirty_tools(void) { ar_dirty(SD_X - 2, 0, SD_W + 2, 2 + 6 * 10 + 2); }
static void dirty_chips(void) {
	ar_dirty(SD_X - 2, 2 + 6 * 10, SD_W + 2, 48 + 16);
	ar_dirty(SD_X - 2, BT_Y - 20, SD_W + 2, 9);
}
static void dirty_bottom(void) { ar_dirty(0, BT_Y, AR_W, AR_H - BT_Y); }

static const char *kind_name(int k) {
	switch (k) {
	case PX_ZIC: return "icons";
	case PX_ZFN: return "font";
	case PX_ZBM: return "picture";
	case PX_ZSP: return doc.cursor ? "pointer" : "sprites";
	default: return "";
	}
}

/* The zoom that shows the whole cell, at most 16. */
static int fit_zoom(void) {
	int z = 16;
	while (z > 1 && (cw() * z > CV_W || ch() * z > CV_H)) z--;
	return z;
}

/* Where the cell's (0,0) is on the screen, and the view clamped so the
 * cell is centred when it fits and never scrolled past when it does
 * not. */
static void clamp_view(void) {
	int vw = CV_W / zoom, vh = CV_H / zoom;
	if (cw() <= vw) vx = 0;
	else { if (vx > cw() - vw) vx = cw() - vw; if (vx < 0) vx = 0; }
	if (ch() <= vh) vy = 0;
	else { if (vy > ch() - vh) vy = ch() - vh; if (vy < 0) vy = 0; }
}

static int org_x(void) {
	return cw() * zoom <= CV_W ? CV_X + (CV_W - cw() * zoom) / 2 : CV_X - vx * zoom;
}
static int org_y(void) {
	return ch() * zoom <= CV_H ? CV_Y + (CV_H - ch() * zoom) / 2 : CV_Y - vy * zoom;
}

/* Keep the keyboard cursor in view. */
static void follow_cursor(void) {
	int vw = CV_W / zoom, vh = CV_H / zoom;
	if (cx < vx) vx = cx;
	if (cx >= vx + vw) vx = cx - vw + 1;
	if (cy < vy) vy = cy;
	if (cy >= vy + vh) vy = cy - vh + 1;
	clamp_view();
}

static void set_cell(int i) {
	if (doc.ncells == 0) { cur = -1; return; }
	if (i < 0) i = 0;
	if (i >= doc.ncells) i = doc.ncells - 1;
	if (i != cur) {
		cur = i;
		sel = false;
		floating = false;
		px_piece_free(&flt);
		stroking = false;
		if (cx >= cw()) cx = cw() - 1;
		if (cy >= ch()) cy = ch() - 1;
	}
	zoom = fit_zoom();
	clamp_view();
	/* the canvas, the preview, the strip, the status, the zoom: not
	 * the tools and the colours */
	dirty_canvas();
	dirty_bottom();
	dirty_zoom();
	if (have_cell()) {
		char b[64];
		snprintf(b, sizeof(b), "%s, %d by %d",
			doc.cell[cur].spoken[0] ? doc.cell[cur].spoken : doc.cell[cur].name,
			cw(), ch());
		say(b);
	}
}

/* ===================================================================
 * colour: what the art and the controls are drawn in
 * =================================================================== */

static int sheet_n(void) { return doc.pal.colours; }

/* The colours the art is shown with: the sheet's own, or fewer (F7). */
static int tier(void) { return view_tier ? view_tier : sheet_n(); }

/* The palette index art value v is drawn in, in colour. */
static int disp(int v) { return zsp_reduce(&doc.pal, v, art_tier); }

/* The RGB of display index j: the palette's (a smaller palette is the
 * start of a larger one), a slot's, or black and white. */
static uint16_t disp_rgb(int j) {
	if (art_tier >= sheet_n() || art_tier >= 16) return doc.pal.pal[j & 255].rgb;
	if (art_tier == 4) return doc.pal.slot_rgb[j & 3];
	return j ? 0xfff : 0x000;
}

/* How many colours to ask the runtime for: room for the controls'
 * eight above the art's where the board has it. */
/* The most colours this board shows full screen (16, 64 or 256): what a
 * new sprite sheet has unless asked for fewer. */
static int board_colours = 16;

static void want_colours(void) {
	int n = sheet_n();
	ar_set_colours(n >= 64 ? 256 : n >= 16 ? 64 : 16);
}

/* The palette and the controls' indices for the mode and the sheet:
 * after a mode change, a load, a palette edit or F7. */
static void luts_setup(void);

static void boxes_colours(bool dim);

static void colours_setup(void) {
	int i, a, d = 0, l = 0;
	static const int tiers[] = { 256, 64, 16, 4, 2 };
	shown_n = ar_colours();
	for (planes_n = 1; (1 << planes_n) < shown_n; planes_n++) ;
	/* the most of the sheet's tiers that fits: in the colour window 32
	 * colours show the art at 16 (the controls in the eight above), 8
	 * at 4 */
	a = tier() < shown_n ? tier() : shown_n;
	for (i = 0; tiers[i] > a; i++) ;
	art_tier = tiers[i];
	luts_setup();
	ar_dirty_all();
	if (!ar_color()) {
		for (i = 0; i < R_COUNT; i++) ui_c[i] = i;
		return;
	}
	a = art_tier < sheet_n() ? art_tier : sheet_n();
	for (i = 0; i < a; i++) ar_pal(i, disp_rgb(i));
	if (a + R_COUNT <= shown_n) {
		for (i = 0; i < R_COUNT; i++) {
			ui_c[i] = shown_n - R_COUNT + i;
			ar_pal(ui_c[i], ui_rgb[i]);
		}
		boxes_colours(true);
		return;
	}
	/* every entry is the art's: the controls in its darkest and its
	 * lightest */
	for (i = 1; i < a; i++) {
		if (zsp_lum(disp_rgb(i)) < zsp_lum(disp_rgb(d))) d = i;
		if (zsp_lum(disp_rgb(i)) > zsp_lum(disp_rgb(l))) l = i;
	}
	ui_c[R_BG] = ui_c[R_OUT] = ui_c[R_GRID] = d;
	ui_c[R_INK] = ui_c[R_FRAME] = ui_c[R_HI] = ui_c[R_GUIDE] = ui_c[R_WARN] = l;
	boxes_colours(false);
}

/* The screen toolkit's boxes (ui/sui.h) in pix's own controls'
 * colours: the panels' black, the text's white, the frame's grey, the
 * cursor's yellow for the focused button. Behind a box the art is
 * dimmed while those are pix's own entries; when they are the art's
 * darkest and lightest, nothing is. */
static void boxes_colours(bool dim) {
	uint8_t m[SUI_NC];
	m[SUI_C_FACE] = (uint8_t)C_BG;
	m[SUI_C_TEXT] = (uint8_t)C_INK;
	m[SUI_C_FRAME] = (uint8_t)C_FRAME;
	m[SUI_C_SEL] = (uint8_t)C_HI;
	m[SUI_C_SEL_TEXT] = (uint8_t)C_BG;
	m[SUI_C_WARN] = (uint8_t)C_WARN;
	m[SUI_C_TROUGH] = (uint8_t)C_GRID;
	m[SUI_C_THUMB] = (uint8_t)C_FRAME;
	sui_palette(m, dim);
}

/* -- monochrome --
 *
 * In the window each colour has a pattern of its own while it is being
 * edited (its 16-colour stand-in's, with more), so two colours are
 * never mistaken for each other; the preview and the strip show the
 * real result, each colour in its ink. */
static ar_ink_t edit_pat[16];
static ar_ink_t ink_pat[17];

static void make_patterns(void) {
	static const uint8_t fixed[16][8] = {
		{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },   /* 0 black */
		{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff },   /* 1 white */
		{ 0x88, 0x44, 0x22, 0x11, 0x88, 0x44, 0x22, 0x11 },   /* 2 diagonal */
		{ 0xee, 0xbb, 0xee, 0xbb, 0xee, 0xbb, 0xee, 0xbb },   /* 3 light */
		{ 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00 },   /* 4 lines */
		{ 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88 },   /* 5 columns */
		{ 0x88, 0x00, 0x22, 0x00, 0x88, 0x00, 0x22, 0x00 },   /* 6 sparse */
		{ 0xff, 0xdd, 0xff, 0x77, 0xff, 0xdd, 0xff, 0x77 },   /* 7 near white */
		{ 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55 },   /* 8 half */
		{ 0x80, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00 },   /* 9 dots */
		{ 0x11, 0x22, 0x44, 0x88, 0x11, 0x22, 0x44, 0x88 },   /* 10 the other diagonal */
		{ 0x00, 0x22, 0x00, 0x88, 0x00, 0x22, 0x00, 0x88 },   /* 11 dark */
		{ 0xff, 0x88, 0x88, 0x88, 0xff, 0x88, 0x88, 0x88 },   /* 12 grid */
		{ 0x00, 0xff, 0xff, 0xff, 0x00, 0xff, 0xff, 0xff },   /* 13 light lines */
		{ 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77 },   /* 14 light columns */
		{ 0x55, 0xff, 0xaa, 0xff, 0x55, 0xff, 0xaa, 0xff },   /* 15 lighter */
	};
	int i;
	for (i = 0; i < 16; i++) memcpy(edit_pat[i].row, fixed[i], 8);
	for (i = 0; i <= 16; i++) ar_ink_level(&ink_pat[i], i);
}

/* Art value v's pattern: `real`, its ink; else its editing pattern at
 * the tier shown -- 4 colours as four greys, 2 as black and white. */
static const ar_ink_t *mono_pat(int v, bool real) {
	int t = tier();
	if (v == ZSP_T) return &ar_ink_grey12;
	if (real) {
		int k = zsp_ink(&doc.pal, v);
		return &ink_pat[k > 16 ? 16 : k];
	}
	if (t <= 2) return &ink_pat[zsp_reduce(&doc.pal, v, 2) ? 16 : 0];
	/* shown as 4 of more: its slot, as a grey (a 4-colour sheet as it
	 * is keeps a pattern per colour, below) */
	if (t == 4 && doc.pal.colours > 4) return &ink_pat[zsp_reduce(&doc.pal, v, 4) * 16 / 3];
	v = zsp_reduce(&doc.pal, v, t);
	return &edit_pat[v < 16 ? v : doc.pal.pal[v & 255].to16 & 15];
}

/* ===================================================================
 * what a pixel shows: the cell, with what is being drawn over it
 * =================================================================== */

/* The line being drawn, a row at a time: on row y it covers x from
 * lrun0[y - ly0] to lrun1[...] (a Bresenham line is one run a row) --
 * worked out once a move, not by walking the line for every pixel the
 * canvas shows. */
#define LRUN_MAX 1024
static int16_t lrun0[LRUN_MAX], lrun1[LRUN_MAX];
static int ly0 = 0, ly1 = -1;

static void line_prep(int x0, int y0, int x1, int y1) {
	int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
	int dy = -(y1 > y0 ? y1 - y0 : y0 - y1), sy = y0 < y1 ? 1 : -1;
	int err = dx + dy, e2, i;
	ly0 = y0 < y1 ? y0 : y1;
	ly1 = y0 < y1 ? y1 : y0;
	if (ly1 - ly0 >= LRUN_MAX) { ly1 = ly0 - 1; return; }
	for (i = 0; i <= ly1 - ly0; i++) { lrun0[i] = 32767; lrun1[i] = -32768; }
	for (;;) {
		i = y0 - ly0;
		if (x0 < lrun0[i]) lrun0[i] = (int16_t)x0;
		if (x0 > lrun1[i]) lrun1[i] = (int16_t)x0;
		if (x0 == x1 && y0 == y1) break;
		e2 = 2 * err;
		if (e2 >= dy) { err += dy; x0 += sx; }
		if (e2 <= dx) { err += dx; y0 += sy; }
	}
}

static int shown(int x, int y) {
	if (floating && x >= fltx && y >= flty && x < fltx + flt.w && y < flty + flt.h) {
		int v = px_piece_get(&flt, x - fltx, y - flty);
		return v;
	}
	if (stroking && tool == T_LINE && y >= ly0 && y <= ly1 &&
	    x >= lrun0[y - ly0] && x <= lrun1[y - ly0])
		return stroke_v;
	if (stroking && tool == T_RECT && px_on_rect(sx0, sy0, sx1, sy1, x, y, rect_filled))
		return stroke_v;
	return px_get(&doc, cur, x, y);
}

/* ===================================================================
 * painting
 * =================================================================== */

/* A value in vbuf that is a control's colour (UI_V | its index), not
 * the art's: the canvas is put together whole -- art, grid, frames --
 * and drawn in one pass, so nothing on it is cleared and drawn again. */
#define UI_V 0x8000u

/* Art values to be drawn (an index or ZSP_T), and the bitmap each
 * plane -- or the monochrome picture -- is made in. */
static uint16_t vbuf[CV_H][CV_W];
static uint8_t ibuf[CV_H][CV_W];
static uint32_t cvbm[CV_H][CV_W / 32];

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* vbuf's top left w x h at (x, y): in colour a plane at a time; in
 * monochrome as patterns (`real`: the inks). Transparent is a checker,
 * anchored to the picture like every pattern. */
/* What each art value is drawn as, worked out once a palette, tier or
 * mode (colours_setup()) rather than a pixel at a time: its display
 * index in colour, its pattern in the window (editing and real). */
static uint8_t disp_lut[256];
static const ar_ink_t *pat_lut[2][257];

static void luts_setup(void) {
	int v;
	for (v = 0; v < 256; v++) disp_lut[v] = (uint8_t)disp(v);
	for (v = 0; v <= 256; v++) {
		int val = v == 256 ? ZSP_T : v;
		pat_lut[0][v] = mono_pat(val, false);
		pat_lut[1][v] = mono_pat(val, true);
	}
}

/* The planes of a picture row, eight pixels at a time: their eight
 * bytes are a matrix of bits, and its transpose is a byte a plane
 * (bit i is pixel i's, as the blitter wants: LSB leftmost). Three
 * swaps of blocks of bits, not a test of each bit of each plane. */
static uint32_t planebuf[8][CV_H][CV_W / 32];

static inline void c2p8(uint32_t x, uint32_t y, uint8_t *o) {
	uint32_t t;
	t = (x ^ (x >> 7)) & 0x00AA00AAu; x ^= t ^ (t << 7);
	t = (y ^ (y >> 7)) & 0x00AA00AAu; y ^= t ^ (t << 7);
	t = (x ^ (x >> 14)) & 0x0000CCCCu; x ^= t ^ (t << 14);
	t = (y ^ (y >> 14)) & 0x0000CCCCu; y ^= t ^ (t << 14);
	t = (x & 0x0F0F0F0Fu) | ((y & 0x0F0F0F0Fu) << 4);
	y = ((x >> 4) & 0x0F0F0F0Fu) | (y & 0xF0F0F0F0u);
	x = t;
	o[0] = (uint8_t)x; o[1] = (uint8_t)(x >> 8); o[2] = (uint8_t)(x >> 16); o[3] = (uint8_t)(x >> 24);
	o[4] = (uint8_t)y; o[5] = (uint8_t)(y >> 8); o[6] = (uint8_t)(y >> 16); o[7] = (uint8_t)(y >> 24);
}

static void put_art(int x, int y, int w, int h, bool real) {
	int a, b, k;
	if (w <= 0 || h <= 0) return;
	if (ar_color()) {
		int w8 = (w + 7) & ~7;
		uint8_t cg = (uint8_t)C_GRID, co = (uint8_t)C_OUT;
		for (b = 0; b < h; b++) {
			const uint16_t *vr = vbuf[b];
			uint8_t *ir = ibuf[b], *pb[8];
			for (a = 0; a < w; a++) {
				unsigned v = vr[a];
				ir[a] = v & UI_V ? (uint8_t)v : v == ZSP_T ?
					((((x + a) >> 2) ^ ((y + b) >> 2)) & 1 ? cg : co) : disp_lut[v & 255];
			}
			for (; a < w8; a++) ir[a] = 0;
			for (k = 0; k < 8; k++) pb[k] = (uint8_t *)planebuf[k][b];
			for (a = 0; a < w8; a += 8) {
				uint8_t o[8];
				const uint8_t *q = ir + a;
				c2p8((uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24,
					(uint32_t)q[4] | (uint32_t)q[5] << 8 | (uint32_t)q[6] << 16 | (uint32_t)q[7] << 24, o);
				for (k = 0; k < planes_n; k++) pb[k][a >> 3] = o[k];
			}
		}
		for (k = 0; k < planes_n; k++)
			ar_plane(k, planebuf[k], CV_W / 8, 0, 0, x, y, w, h);
		return;
	}
	for (b = 0; b < h; b++) {
		uint32_t *row = cvbm[b];
		const uint16_t *vr = vbuf[b];
		int yy = (y + b) & 7;
		memset(row, 0, sizeof(cvbm[0]));
		for (a = 0; a < w; a++) {
			unsigned v = vr[a];
			const ar_ink_t *p = v & UI_V ? &desc.ink[v & 15] : pat_lut[real][v > 256 ? 256 : v];
			if (p->row[yy] & (0x80u >> ((x + a) & 7))) row[a >> 5] |= 1u << (a & 31);
		}
	}
	/* (the bitmap in R_INK over R_BG: the patterns are the picture) */
	ar_bitmap_opaque(cvbm, CV_W / 8, 0, 0, x, y, w, h);
}

/* One colour as a swatch: its index in colour, its pattern in mono. */
static void chip(int x, int y, int w, int h, int v) {
	if (v == ZSP_T) {
		if (ar_color()) ar_fill2(x, y, w, h, C_OUT, C_GRID, &ar_ink_grey50);
		else ar_fill2(x, y, w, h, C_BG, C_INK, &ar_ink_grey12);
	} else if (ar_color()) {
		ar_fill(x, y, w, h, disp(v));
	} else {
		ar_fill2(x, y, w, h, C_BG, C_INK, mono_pat(v, false));
	}
}

/* intersect (x,y,w,h) with r; false if nothing is left */
static bool clip_to(const ar_rect_t *r, int *x, int *y, int *w, int *h) {
	int x0 = *x > r->x ? *x : r->x, y0 = *y > r->y ? *y : r->y;
	int x1 = *x + *w < r->x + r->w ? *x + *w : r->x + r->w;
	int y1 = *y + *h < r->y + r->h ? *y + *h : r->y + r->h;
	if (x1 <= x0 || y1 <= y0) return false;
	*x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
	return true;
}

static void frame_rect(int x, int y, int w, int h, int c) {
	ar_fill(x, y, w, 1, c);
	ar_fill(x, y + h - 1, w, 1, c);
	ar_fill(x, y, 1, h, c);
	ar_fill(x + w - 1, y, 1, h, c);
}

/* Onion skin, over vbuf (aw x ah at ax, ay; the cell's 0,0 at ox,
 * oy): where this frame is clear, a sparse dot of the frame before in
 * its own colour -- in the middle of each zoomed pixel -- and with
 * onion 2 of the frame after, nearer the corner. Below 3x, every
 * fourth pixel, on two different diagonals. */
static bool onion_dot(int cx_, int cy_, int sx_, int sy_, int which) {
	if (zoom >= 3) {
		int m = which ? zoom / 4 : zoom / 2, big = zoom >= 8;
		return (sx_ == m || (big && sx_ == m - 1 + 2 * which)) && (sy_ == m || (big && sy_ == m - 1 + 2 * which));
	}
	return sx_ == 0 && sy_ == 0 && (cx_ & 1) == which && (cy_ & 1) == which;
}

static void onion_skin(int ax, int ay, int aw, int ah, int ox, int oy, int bx, int by) {
	int prev, next, b, a, clear = px_clear(&doc);
	px_anim_neighbours(&doc, cur, &prev, &next);
	if (onion < 2) next = -1;
	if (prev < 0 && next < 0) return;
	for (b = 0; b < ah; b++) {
		int cy_ = (ay + b - oy) / zoom, sy_ = (ay + b - oy) % zoom;
		uint16_t *row = vbuf[by + b] + bx;
		for (a = 0; a < aw; a++) {
			int cx_ = (ax + a - ox) / zoom, sx_ = (ax + a - ox) % zoom, v;
			if (row[a] != clear) continue;
			if (prev >= 0 && onion_dot(cx_, cy_, sx_, sy_, 0) && (v = px_get(&doc, prev, cx_, cy_)) != clear)
				row[a] = (uint16_t)v;
			else if (next >= 0 && onion_dot(cx_, cy_, sx_, sy_, 1) && (v = px_get(&doc, next, cx_, cy_)) != clear)
				row[a] = (uint16_t)v;
		}
	}
}

/* Into vbuf (whose 0,0 is the screen's x0,y0, w x h): a rectangle of
 * screen pixels in control colour c, clipped. */
static void vb_rect(int x0, int y0, int w, int h, int rx, int ry, int rw, int rh, int c) {
	int a, b;
	int ax = rx > x0 ? rx : x0, ay = ry > y0 ? ry : y0;
	int ax1 = rx + rw < x0 + w ? rx + rw : x0 + w, ay1 = ry + rh < y0 + h ? ry + rh : y0 + h;
	for (b = ay; b < ay1; b++)
		for (a = ax; a < ax1; a++) vbuf[b - y0][a - x0] = (uint16_t)(UI_V | (unsigned)c);
}

static void vb_frame(int x0, int y0, int w, int h, int fx, int fy, int fw, int fh, int c) {
	vb_rect(x0, y0, w, h, fx, fy, fw, 1, c);
	vb_rect(x0, y0, w, h, fx, fy + fh - 1, fw, 1, c);
	vb_rect(x0, y0, w, h, fx, fy, 1, fh, c);
	vb_rect(x0, y0, w, h, fx + fw - 1, fy, 1, fh, c);
}

/* The canvas's part of r, put together in vbuf -- the space round the
 * cell, its edge, the art (with the onion skin), the grid, the guides,
 * the selection and the cursor -- and drawn once (put_art()). Drawn a
 * layer at a time, each pixel was cleared and drawn again, and in 256
 * colours that is eight planes of each; the screen showed it. */
static void paint_canvas(const ar_rect_t *r) {
	int ox, oy, x, y, w, h, px0, px1, py0, py1, py, px, b;

	x = CV_X; y = CV_Y; w = CV_W; h = CV_H;
	if (!clip_to(r, &x, &y, &w, &h)) return;
	for (b = 0; b < h; b++) {
		uint16_t *row = vbuf[b];
		int a;
		for (a = 0; a < w; a++) row[a] = (uint16_t)(UI_V | (unsigned)C_OUT);
	}
	if (!have_cell()) { put_art(x, y, w, h, false); return; }

	ox = org_x();
	oy = org_y();

	/* the cell's edge, just outside it */
	vb_frame(x, y, w, h, ox - 1, oy - 1, cw() * zoom + 2, ch() * zoom + 2, C_FRAME);

	/* the cell's part of this rectangle, in cell pixels */
	px0 = clampi((x - ox) / zoom, 0, cw() - 1);
	px1 = clampi((x + w - 1 - ox) / zoom, 0, cw() - 1);
	py0 = clampi((y - oy) / zoom, 0, ch() - 1);
	py1 = clampi((y + h - 1 - oy) / zoom, 0, ch() - 1);

	{
		int ax = ox + px0 * zoom, ay = oy + py0 * zoom;
		int aw = (px1 - px0 + 1) * zoom, ah = (py1 - py0 + 1) * zoom;
		int s, bx, by;
		if (clip_to(r, &ax, &ay, &aw, &ah)) {
			if (ax < x) { aw -= x - ax; ax = x; }
			if (ay < y) { ah -= y - ay; ay = y; }
			if (ax + aw > x + w) aw = x + w - ax;
			if (ay + ah > y + h) ah = y + h - ay;
		} else aw = 0;
		bx = ax - x; by = ay - y;
		if (aw > 0 && ah > 0) {
			/* the art, a row of cell pixels worked out once and
			 * copied down `zoom` screen rows */
			for (b = 0; b < ah; b++) {
				uint16_t *row = vbuf[by + b] + bx;
				int cy_ = (ay + b - oy) / zoom;
				if (b > 0 && (ay + b - oy) % zoom != 0) {
					memcpy(row, vbuf[by + b - 1] + bx, sizeof(uint16_t) * (size_t)aw);
					continue;
				}
				for (s = 0; s < aw; s++) {
					int cx_ = (ax + s - ox) / zoom;
					if (s > 0 && (ax + s - ox) % zoom != 0) row[s] = row[s - 1];
					else row[s] = (uint16_t)shown(cx_, cy_);
				}
			}
			if (onion) onion_skin(ax, ay, aw, ah, ox, oy, bx, by);

			/* the grid, from 6x */
			if (zoom >= 6) {
				for (px = px0; px <= px1 + 1; px++) {
					int gx = ox + px * zoom;
					if (gx >= ax && gx < ax + aw) vb_rect(x, y, w, h, gx, ay, 1, ah, C_GRID);
				}
				for (py = py0; py <= py1 + 1; py++) {
					int gy = oy + py * zoom;
					if (gy >= ay && gy < ay + ah) vb_rect(x, y, w, h, ax, gy, aw, 1, C_GRID);
				}
			}
		}
	}
	/* guides: a font's baseline (the ascent), the pointer's centre */
	if (doc.kind == PX_ZFN && doc.ascent > 0 && doc.ascent < ch())
		vb_rect(x, y, w, h, ox, oy + doc.ascent * zoom, cw() * zoom, 1, C_GUIDE);
	if (doc.cursor) {
		int gx = ox + 8 * zoom + zoom / 2, gy = oy + 8 * zoom + zoom / 2;
		int a = zoom / 2 + 1;
		vb_rect(x, y, w, h, gx - a, gy, 2 * a + 1, 1, C_GUIDE);
		vb_rect(x, y, w, h, gx, gy - a, 1, 2 * a + 1, C_GUIDE);
	}
	/* the selection, and the keyboard cursor */
	if (sel) {
		int fx = floating ? fltx : selx, fy = floating ? flty : sely;
		vb_frame(x, y, w, h, ox + fx * zoom - 1, oy + fy * zoom - 1, selw * zoom + 2, selh * zoom + 2, C_HI);
	}
	if (zoom >= 3 && kbd_cursor)
		vb_frame(x, y, w, h, ox + cx * zoom, oy + cy * zoom, zoom + 1, zoom + 1, C_HI);

	put_art(x, y, w, h, false);
}

static void button(int x, int y, int w, const char *label, bool on) {
	ar_fill(x, y, w, 9, on ? C_INK : C_BG);
	if (!on) frame_rect(x, y, w, 9, C_FRAME);
	ar_text(x + 2, y + 1, label, on ? C_BG : C_INK, &z_font_5x8, 1);
}

#define SD_TOOLS_Y 2
#define SD_ROW 10
#define SD_COL_Y (SD_TOOLS_Y + T_COUNT * SD_ROW + 3)   /* 4 x 4 colour chips */
#define CHIP_W 14
#define CHIP_H 11
#define SD_COL_H 48             /* the chips, or every colour (chips_grid()) */
#define SD_FGBG_Y (SD_COL_Y + SD_COL_H + 2)
#define SD_ZOOM_Y (SD_FGBG_Y + 10)
#define SD_FILE_Y (SD_ZOOM_Y + 12)

/* A colour as words: "9", "f0", "." */
static void colour_word(char *b, int n, int v) {
	if (v == ZSP_T) snprintf(b, (size_t)n, ".");
	else if (sheet_n() > 16) snprintf(b, (size_t)n, "%02x", v);
	else snprintf(b, (size_t)n, "%x", v);
}

/* More than 16 colours, full screen in colour: every colour shown at
 * once, as small squares (8 x 8 of 7 x 6, or 16 x 16 of 3 x 3), rather
 * than a page of 16 chips. In the window the chips are patterns, and
 * stay a page at a time. */
static bool chips_grid(void) { return ar_color() && sheet_n() > 16; }

static void grid_geom(int *cols, int *gw, int *gh, int *gx) {
	if (sheet_n() == 64) { *cols = 8; *gw = 7; *gh = 6; }
	else { *cols = 16; *gw = 3; *gh = 3; }
	*gx = SD_X + 1 + (SD_W - 2 - *cols * *gw) / 2;
}

/* How many chips this page has. */
static int page_chips(void) {
	int n = sheet_n() - page * 16;
	return n > 16 ? 16 : n < 0 ? 0 : n;
}

static bool hits(const ar_rect_t *r, int x, int y, int w, int h);

static void paint_side(const ar_rect_t *r) {
	int x = SD_X - 2, y = 0, w = SD_W + 2, h = BT_Y, i;
	char b[48], f1[4], f2[4];
	int gcols = 0, ggw = 0, ggh = 0, ggx = 0, gx0 = 0, gy0 = 0, gw0 = 0, gh0 = 0;
	if (!clip_to(r, &x, &y, &w, &h)) return;
	if (chips_grid()) {
		/* the colours' square: put together, not drawn over the
		 * background (below) */
		grid_geom(&gcols, &ggw, &ggh, &ggx);
		gx0 = ggx - 3; gy0 = SD_COL_Y - 1;
		gw0 = gcols * ggw + 4; gh0 = sheet_n() / gcols * ggh + 2;
		ar_fill(SD_X - 2, 0, SD_W + 2, gy0, C_BG);
		ar_fill(SD_X - 2, gy0 + gh0, SD_W + 2, BT_Y - gy0 - gh0, C_BG);
		ar_fill(SD_X - 2, gy0, gx0 - (SD_X - 2), gh0, C_BG);
		ar_fill(gx0 + gw0, gy0, SD_X + SD_W - gx0 - gw0, gh0, C_BG);
	} else ar_fill(SD_X - 2, 0, SD_W + 2, BT_Y, C_BG);
	ar_fill(SD_X - 2, 0, 1, BT_Y, C_FRAME);

	for (i = 0; i < T_COUNT; i++) {
		char l[16];
		snprintf(l, sizeof(l), "%c %s", toupper((unsigned char)tool_key[i]), tool_name[i]);
		if (i == T_RECT && rect_filled) strcpy(l, "R Box");
		button(SD_X, SD_TOOLS_Y + i * SD_ROW, SD_W - 2, l, tool == i);
	}

	if (chips_grid() && hits(r, gx0, gy0, gw0, gh0)) {
		/* every colour, a small square each; this page (what 0-9 A-F
		 * pick from) marked at the left, the first colour framed, the
		 * second marked -- put together and drawn in one pass */
		int a, b2, rows = sheet_n() / gcols;
		for (b2 = 0; b2 < gh0; b2++)
			for (a = 0; a < gw0; a++) {
				int gxx = a - 3, gyy = b2 - 1;
				vbuf[b2][a] = (uint16_t)(gxx >= 0 && gyy >= 0 && gxx < gcols * ggw && gyy < rows * ggh ?
					(unsigned)((gyy / ggh) * gcols + gxx / ggw) : UI_V | (unsigned)C_BG);
			}
		vb_rect(gx0, gy0, gw0, gh0, gx0, SD_COL_Y + page * 16 / gcols * ggh, 2, 16 / gcols * ggh, C_HI);
		if (bg2 != ZSP_T && bg2 < sheet_n())
			vb_frame(gx0, gy0, gw0, gh0, ggx + (bg2 % gcols) * ggw - 1, SD_COL_Y + (bg2 / gcols) * ggh - 1, ggw + 2, ggh + 2, C_GUIDE);
		if (fg != ZSP_T && fg < sheet_n())
			vb_frame(gx0, gy0, gw0, gh0, ggx + (fg % gcols) * ggw - 1, SD_COL_Y + (fg / gcols) * ggh - 1, ggw + 2, ggh + 2, C_HI);
		put_art(gx0, gy0, gw0, gh0, false);
	} else if (!chips_grid()) for (i = 0; i < page_chips(); i++) {
		/* this page's colours; the first colour framed, the second
		 * marked */
		int v = page * 16 + i;
		int bx = SD_X + (i & 3) * (CHIP_W + 1), by = SD_COL_Y + (i >> 2) * (CHIP_H + 1);
		chip(bx + 1, by + 1, CHIP_W - 2, CHIP_H - 2, v);
		frame_rect(bx, by, CHIP_W, CHIP_H, v == fg ? C_HI : v == bg2 ? C_GUIDE : C_FRAME);
	}
	colour_word(f1, sizeof(f1), fg);
	colour_word(f2, sizeof(f2), bg2);
	chip(SD_X, SD_FGBG_Y, 6, 7, fg);
	chip(SD_X + 30, SD_FGBG_Y, 6, 7, bg2);
	ar_text(SD_X + 8, SD_FGBG_Y, f1, C_INK, &z_font_5x8, 1);
	ar_text(SD_X + 38, SD_FGBG_Y, f2, C_INK, &z_font_5x8, 1);

	snprintf(b, sizeof(b), "Zoom %dx", zoom);
	ar_text(SD_X + 2, SD_ZOOM_Y, b, C_INK, &z_font_5x8, 1);

	button(SD_X, SD_FILE_Y, SD_W - 2, "New", false);
	button(SD_X, SD_FILE_Y + SD_ROW, SD_W - 2, "Open", false);
	button(SD_X, SD_FILE_Y + 2 * SD_ROW, SD_W - 2, doc.dirty ? "Save *" : "Save", false);

	if (sheet_n() > 16) snprintf(b, sizeof(b), "%d: p%d/%d", sheet_n(), page + 1, sheet_n() / 16);
	else snprintf(b, sizeof(b), "%d colours", sheet_n());
	ar_text(SD_X + 2, BT_Y - 20, b, C_GRID == C_BG ? C_INK : C_GRID, &z_font_5x8, 1);
	/* F7's choice, or fewer where the colour shown has no more (a colour
	 * window, a smaller board) */
	i = ar_color() && art_tier < tier() ? art_tier : view_tier;
	if (i) snprintf(b, sizeof(b), "shown as %d", i);   /* 12 letters fit */
	else snprintf(b, sizeof(b), "%s", kind_name(doc.kind));
	ar_text(SD_X + 2, BT_Y - 10, b, C_GRID == C_BG ? C_INK : C_GRID, &z_font_5x8, 1);
}

/* A cell drawn small, as it really looks: 1:1 if it fits the box, else
 * every n-th pixel. */
static void draw_thumb(int i, int x, int y, int bw, int bh) {
	const px_cell_t *c = &doc.cell[i];
	int step = 1, w, h, a, b;
	while ((c->w + step - 1) / step > bw || (c->h + step - 1) / step > bh) step++;
	w = (c->w + step - 1) / step;
	h = (c->h + step - 1) / step;
	if (w > 64) w = 64;
	if (h > 48) h = 48;
	for (b = 0; b < h; b++)
		for (a = 0; a < w; a++)
			vbuf[b][a] = (uint16_t)px_get(&doc, i, a * step, b * step);
	put_art(x + (bw - w) / 2, y + (bh - h) / 2, w, h, true);
}

/* The strip's slots: as many cells as fit, the current one among them. */
static int strip_slot_w(void) {
	int w = have_cell() ? doc.cell[cur].w : 16;
	if (w > ST_H - 4) w = ST_H - 4;
	if (w < 8) w = 8;
	return w + 6;
}

static int strip_first(void) {
	int n = ST_W / strip_slot_w(), f = cur - n / 2;
	if (f > doc.ncells - n) f = doc.ncells - n;
	if (f < 0) f = 0;
	return f;
}

/* Whether the rectangle being painted meets this one: what it misses
 * is not worked out at all (a thumbnail is a pass over its cell). */
static bool hits(const ar_rect_t *r, int x, int y, int w, int h) {
	return x < r->x + r->w && r->x < x + w && y < r->y + r->h && r->y < y + h;
}

static void paint_bottom(const ar_rect_t *r) {
	int x = 0, y = BT_Y, w = AR_W, h = AR_H - BT_Y, i, sw, f, n;
	char b[96];
	if (!clip_to(r, &x, &y, &w, &h)) return;
	ar_fill(0, BT_Y, AR_W, AR_H - BT_Y, C_BG);
	ar_fill(0, BT_Y - 1, CV_W, 1, C_FRAME);

	/* the preview, 1:1, as it really looks; playing, the animation's
	 * frame, at 2:1 where it fits */
	frame_rect(PV_X, BT_Y + 1, PV_W, ST_H, C_FRAME);
	if (playing && hits(r, PV_X, BT_Y + 1, PV_W, ST_H)) {
		if (play_cell >= 0 && play_cell < doc.ncells) {
			const px_cell_t *pc = &doc.cell[play_cell];
			int sc = pc->w * 2 <= PV_W - 4 && pc->h * 2 <= ST_H - 4 ? 2 : 1;
			int pw = pc->w * sc < PV_W - 4 ? pc->w * sc : PV_W - 4;
			int ph = pc->h * sc < ST_H - 4 ? pc->h * sc : ST_H - 4;
			int a, c2;
			for (c2 = 0; c2 < ph; c2++)
				for (a = 0; a < pw; a++)
					vbuf[c2][a] = (uint16_t)px_get(&doc, play_cell, a / sc, c2 / sc);
			put_art(PV_X + (PV_W - pw) / 2, BT_Y + 1 + (ST_H - ph) / 2, pw, ph, true);
		}
	} else if (have_cell() && hits(r, PV_X, BT_Y + 1, PV_W, ST_H)) {
		int pw = cw() < PV_W - 4 ? cw() : PV_W - 4, ph = ch() < ST_H - 4 ? ch() : ST_H - 4;
		int a, c2;
		for (c2 = 0; c2 < ph; c2++)
			for (a = 0; a < pw; a++)
				vbuf[c2][a] = (uint16_t)shown(a + (cw() > pw ? vx : 0), c2 + (ch() > ph ? vy : 0));
		put_art(PV_X + (PV_W - pw) / 2, BT_Y + 1 + (ST_H - ph) / 2, pw, ph, true);
	}

	/* the strip */
	sw = strip_slot_w();
	f = strip_first();
	n = ST_W / sw;
	for (i = f; i < doc.ncells && i < f + n; i++) {
		int sx = ST_X + (i - f) * sw;
		if (!hits(r, sx, BT_Y, sw, ST_H + 2)) continue;
		draw_thumb(i, sx + 3, BT_Y + 3, sw - 6, ST_H - 4);
		if (i == cur) frame_rect(sx + 1, BT_Y + 1, sw - 2, ST_H, C_HI);
		if (doc.cell[i].flags & PX_CELL_CHANGED) ar_fill(sx + sw - 4, BT_Y + 2, 2, 2, C_WARN);
	}

	/* the status line */
	if (msg_frames > 0) {
		snprintf(b, sizeof(b), "%s", msg);
	} else if (have_cell()) {
		const char *nm = doc.cell[cur].name;
		int v = px_get(&doc, cur, cx, cy);
		char cw_[8];
		if (doc.depth == 1) snprintf(cw_, sizeof(cw_), "%s", v ? "ink" : "clear");
		else if (v == ZSP_T) snprintf(cw_, sizeof(cw_), "clear");
		else snprintf(cw_, sizeof(cw_), "c%d", v);
		snprintf(b, sizeof(b), "%d/%d %s  %dx%d  %d,%d  %s", cur + 1, doc.ncells, nm,
			cw(), ch(), cx, cy, cw_);
	} else {
		snprintf(b, sizeof(b), "No cells: Ins adds one");
	}
	ar_text(2, SL_Y, b, msg_frames > 0 ? C_HI : C_INK, &z_font_5x8, 1);

	/* the file, at the right: full screen has no titlebar, and the
	 * window's title is just "pix" (docs/app_kinds.md) */
	{
		int used = (int)strlen(b) * 5 + 2 + 10;
		int room = (AR_W - 2 - used) / 5, n;
		const char *nm;
		file_label(shown_name, sizeof(shown_name));
		n = (int)strlen(shown_name);
		nm = shown_name;
		if (room >= 6) {
			char cut[64];
			if (n > room) {
				snprintf(cut, sizeof(cut), "..%s", shown_name + n - (room - 2));
				nm = cut;
				n = room;
			}
			ar_text(AR_W - 2 - n * 5, SL_Y, nm, C_INK, &z_font_5x8, 1);
		}
	}
}

/* ===================================================================
 * the palette editor (F8): over the canvas
 * =================================================================== */

enum { ED_COLOURS, ED_ENTRY, ED_R, ED_G, ED_B, ED_TO64, ED_TO16, ED_SLOT, ED_INK, ED_N };
static const char *const ed_name[ED_N] = {
	"Colours", "Entry", "Red", "Green", "Blue", "to 64", "to 16", "Slot", "Ink"
};
#define ED_X0 128               /* the fields */
#define ED_Y0 14
#define ED_ROW 11

/* Whether this sheet's size has the field. */
static bool ed_has(int f) {
	switch (f) {
	case ED_TO64: return sheet_n() == 256;
	case ED_TO16: return sheet_n() >= 64;
	case ED_SLOT: return sheet_n() > 4;
	case ED_INK: return sheet_n() > 2;
	default: return true;
	}
}

/* The entry being edited: the first colour, or 0 while it is clear. */
static int ed_entry(void) { return fg == ZSP_T ? 0 : fg; }

/* The grid of entries: columns, and each cell's size. */
static void ed_grid(int *cols, int *gw, int *gh) {
	int n = sheet_n();
	*cols = n <= 16 ? 4 : n == 64 ? 8 : 16;
	*gw = n <= 16 ? 26 : n == 64 ? 14 : 7;
	*gh = n <= 16 ? 20 : n == 64 ? 11 : 6;
}

static int ed_value(int f, char *given) {
	const zsp_colour_t *c = &doc.pal.pal[ed_entry()];
	*given = ' ';
	switch (f) {
	case ED_COLOURS: return sheet_n();
	case ED_ENTRY: return ed_entry();
	case ED_R: return (c->rgb >> 8) & 15;
	case ED_G: return (c->rgb >> 4) & 15;
	case ED_B: return c->rgb & 15;
	case ED_TO64: if (c->given & ZSP_GIVEN_TO64) *given = '!'; return c->to64;
	case ED_TO16: if (c->given & ZSP_GIVEN_TO16) *given = '!'; return c->to16;
	case ED_SLOT: if (c->given & ZSP_GIVEN_SLOT) *given = '!'; return c->slot;
	default: if (c->given & ZSP_GIVEN_INK) *given = '!'; return c->ink;
	}
}

static void paint_editor(const ar_rect_t *r) {
	int x = CV_X, y = CV_Y, w = CV_W, h = CV_H, i, cols, gw, gh, row, a, b2, tier_y;
	char b[48];
	if (!clip_to(r, &x, &y, &w, &h)) return;
	ar_fill(CV_X, CV_Y, CV_W, CV_H, C_BG);
	snprintf(b, sizeof(b), "Palette: %d colours", sheet_n());
	ar_text(4, 3, b, C_INK, &z_font_5x8, 1);

	/* every entry */
	ed_grid(&cols, &gw, &gh);
	for (i = 0; i < sheet_n(); i++) {
		int gx = 4 + (i % cols) * gw, gy = ED_Y0 + (i / cols) * gh;
		chip(gx + 1, gy + 1, gw - 2, gh - 2, i);
		if (i == ed_entry()) frame_rect(gx, gy, gw, gh, C_HI);
	}

	/* the fields */
	row = 0;
	for (i = 0; i < ED_N; i++) {
		char g;
		int v, fy;
		if (!ed_has(i)) continue;
		fy = ED_Y0 + row * ED_ROW;
		v = ed_value(i, &g);
		snprintf(b, sizeof(b), "%-7s %3d%c", ed_name[i], v, g);
		if (i == ed_field) ar_fill(ED_X0 - 2, fy - 1, CV_W - ED_X0, 10, C_FRAME);
		/* (where the controls are the art's lightest and darkest, the
		 * bar is the text's colour: the field in the dark one) */
		ar_text(ED_X0, fy, b, i == ed_field && C_FRAME == C_INK ? C_BG : C_INK,
			&z_font_5x8, 1);
		row++;
	}

	/* what this colour becomes with fewer */
	tier_y = ED_Y0 + row * ED_ROW + 4;
	ar_text(ED_X0, tier_y, "with fewer:", C_INK, &z_font_5x8, 1);
	a = ED_X0;
	{
		static const int tiers[5] = { 256, 64, 16, 4, 2 };
		for (i = 0; i < 5; i++) {
			int t = tiers[i], v;
			if (t > sheet_n()) continue;
			v = zsp_reduce(&doc.pal, ed_entry(), t);
			if (t >= 16 || t >= sheet_n()) snprintf(b, sizeof(b), "%d:%d", t, v);
			else if (t == 4) snprintf(b, sizeof(b), "4:s%d", v);
			else snprintf(b, sizeof(b), "2:i%d", zsp_ink(&doc.pal, ed_entry()));
			ar_text(a, tier_y + 10, b, C_INK, &z_font_5x8, 1);
			a += (int)strlen(b) * 5 + 4;
			if (a > CV_W - 20) { a = ED_X0; tier_y += 9; }
		}
	}

	/* must differ, and what collides */
	b2 = 0;
	{
		char l[48] = "differs from:";
		int n = (int)strlen(l), k;
		for (k = 0; k < doc.pal.ndiffer && n < 40; k++) {
			int p = doc.pal.differ[k][0], q = doc.pal.differ[k][1];
			if (p == ed_entry()) n += snprintf(l + n, sizeof(l) - (size_t)n, " %d", q), b2++;
			else if (q == ed_entry()) n += snprintf(l + n, sizeof(l) - (size_t)n, " %d", p), b2++;
		}
		if (b2) ar_text(ED_X0, tier_y + 22, l, C_INK, &z_font_5x8, 1);
	}
	{
		int p, q, t;
		if (zsp_collision(&doc.pal, &p, &q, &t)) {
			snprintf(b, sizeof(b), "%d and %d meet at %d!", p, q, t);
			ar_text(ED_X0, tier_y + 32, b, C_WARN, &z_font_5x8, 1);
		}
	}
	if (differ_from >= 0) {
		snprintf(b, sizeof(b), "M: differ from %d", differ_from);
		ar_text(4, CV_H - 22, b, C_HI, &z_font_5x8, 1);
	}
	ar_text(4, CV_H - 11, "Arrows: field, value  D default  M differ  F8 done",
		C_INK, &z_font_5x8, 1);
}

static void pix_paint(void *user, const ar_rect_t *r) {
	(void)user;
	if (editor) paint_editor(r);
	else paint_canvas(r);
	paint_side(r);
	paint_bottom(r);
}

/* ===================================================================
 * editing
 * =================================================================== */

/* About to change the cell: one undo step. */
static void begin_change(void) {
	if (have_cell() && !px_undo_save(&doc, cur)) message("Not enough memory to undo this");
}

static void changed(void) {
	if (!have_cell()) return;
	px_touch(&doc, cur);
	dirty_canvas();
	dirty_bottom();
	dirty_side();
}

/* The status line, alone. */
static void dirty_status(void) { ar_dirty(0, SL_Y - 1, AR_W, AR_H - SL_Y + 1); }

/* Cell pixels x0..x1, y0..y1 on the canvas (with their grid lines and
 * the keyboard cursor's frame around them). */
static void dirty_px(int x0, int y0, int x1, int y1) {
	int t;
	if (x0 > x1) { t = x0; x0 = x1; x1 = t; }
	if (y0 > y1) { t = y0; y0 = y1; y1 = t; }
	ar_dirty(org_x() + x0 * zoom - 2, org_y() + y0 * zoom - 2,
		(x1 - x0 + 1) * zoom + 4, (y1 - y0 + 1) * zoom + 4);
}

/* Cell pixels x0..x1, y0..y1 in the preview (1:1, the view's corner
 * when the cell is bigger than it). */
static void dirty_prev(int x0, int y0, int x1, int y1) {
	int pw = cw() < PV_W - 4 ? cw() : PV_W - 4, ph = ch() < ST_H - 4 ? ch() : ST_H - 4;
	int bx = PV_X + (PV_W - pw) / 2 - (cw() > pw ? vx : 0);
	int by = BT_Y + 1 + (ST_H - ph) / 2 - (ch() > ph ? vy : 0);
	int t;
	if (playing) return;            /* it shows the animation */
	if (x0 > x1) { t = x0; x0 = x1; x1 = t; }
	if (y0 > y1) { t = y0; y0 = y1; y1 = t; }
	ar_dirty(bx + x0, by + y0, x1 - x0 + 1, y1 - y0 + 1);
}

/* Both: the canvas and the preview. */
static void dirty_cells(int x0, int y0, int x1, int y1) {
	dirty_px(x0, y0, x1, y1);
	dirty_prev(x0, y0, x1, y1);
}

/* What a line or a rectangle being drawn covers, on the canvas and
 * the preview: the line's pixels, the outline's four edges, a filled
 * one's box -- not the whole canvas, at every move of the mouse. */
static void dirty_shape(int t, int x0, int y0, int x1, int y1) {
	if (t == T_LINE) {
		int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
		int dy = -(y1 > y0 ? y1 - y0 : y0 - y1), sy = y0 < y1 ? 1 : -1;
		int err = dx + dy, e2;
		for (;;) {
			dirty_cells(x0, y0, x0, y0);
			if (x0 == x1 && y0 == y1) break;
			e2 = 2 * err;
			if (e2 >= dy) { err += dy; x0 += sx; }
			if (e2 <= dx) { err += dx; y0 += sy; }
		}
	} else if (t == T_RECT && !rect_filled) {
		dirty_cells(x0, y0, x1, y0);
		dirty_cells(x0, y1, x1, y1);
		dirty_cells(x0, y0, x0, y1);
		dirty_cells(x1, y0, x1, y1);
	} else dirty_cells(x0, y0, x1, y1);
}

/* A selection's frame: its four edges. */
static void dirty_frame(int x, int y, int w, int h) {
	dirty_px(x, y, x + w - 1, y);
	dirty_px(x, y + h - 1, x + w - 1, y + h - 1);
	dirty_px(x, y, x, y + h - 1);
	dirty_px(x + w - 1, y, x + w - 1, y + h - 1);
}

/* The current cell's slot in the strip. */
static void dirty_slot(void) {
	int sw = strip_slot_w(), f = strip_first();
	if (cur >= f && cur < f + ST_W / sw) ar_dirty(ST_X + (cur - f) * sw, BT_Y, sw, ST_H + 2);
}

/* A change to pixels x0..x1, y0..y1 of the cell only -- what a pencil
 * makes, many times a second: that part of the canvas, the preview,
 * the cell's thumbnail and the status line, not the whole screen. In
 * 256 colours a whole canvas is eight planes of work. */
static void changed_px(int x0, int y0, int x1, int y1) {
	bool was = doc.dirty;
	if (!have_cell()) return;
	px_touch(&doc, cur);
	dirty_cells(x0, y0, x1, y1);
	dirty_slot();
	/* the status line shows the colour under the cursor */
	if (cx >= (x0 < x1 ? x0 : x1) && cx <= (x0 < x1 ? x1 : x0) &&
	    cy >= (y0 < y1 ? y0 : y1) && cy <= (y0 < y1 ? y1 : y0)) dirty_status();
	if (!was) ar_dirty(SD_X - 2, SD_FILE_Y + 2 * SD_ROW, SD_W + 2, 9);   /* "Save *" */
}

/* Put a floating selection down where it is. */
static void drop_float(void) {
	if (!floating) return;
	px_piece_put(&flt, &doc, cur, fltx, flty, false);
	selx = fltx;
	sely = flty;
	floating = false;
	px_piece_free(&flt);
	changed();
}

/* Lift the selection off the cell to move it: its place is cleared. */
static bool lift(void) {
	if (!sel || floating) return floating;
	if (!px_piece_take(&flt, &doc, cur, selx, sely, selw, selh)) return false;
	begin_change();
	px_rect(&doc, cur, selx, sely, selx + selw - 1, sely + selh - 1, px_clear(&doc), true);
	fltx = selx;
	flty = sely;
	floating = true;
	dirty_slot();                   /* the strip shows the cell without it */
	dirty_prev(selx, sely, selx + selw - 1, sely + selh - 1);
	return true;
}

static void select_none(void) {
	drop_float();
	if (sel) dirty_canvas();
	sel = false;
}

/* The selection's pixels, or the whole cell's when there is none, as
 * a piece for flip/turn/shift; and putting it back. */
static bool with_target(px_piece_t *p, int *x, int *y) {
	if (sel) {
		if (!lift()) return false;
		*p = flt;
		*x = fltx;
		*y = flty;
		return true;
	}
	memset(p, 0, sizeof(*p));
	if (!px_piece_take(p, &doc, cur, 0, 0, cw(), ch())) return false;
	begin_change();
	*x = *y = 0;
	return true;
}

static void put_target(px_piece_t *p, int x, int y) {
	if (sel) {
		flt = *p;
		fltx = x;
		flty = y;
		selw = flt.w;
		selh = flt.h;
		dirty_canvas();
		return;
	}
	if (p->w != cw() || p->h != ch()) {
		if (!px_cell_resize(&doc, cur, p->w, p->h)) {
			message("Out of memory");
			px_piece_free(p);
			return;
		}
	}
	px_piece_put(p, &doc, cur, x, y, false);
	px_piece_free(p);
	zoom = fit_zoom();
	clamp_view();
	if (cx >= cw()) cx = cw() - 1;
	if (cy >= ch()) cy = ch() - 1;
	changed();
	ar_dirty_all();
}

static void flip(int how) {
	px_piece_t p;
	int x, y;
	if (!have_cell() || !with_target(&p, &x, &y)) return;
	if (how == 0) px_piece_flip_h(&p);
	else if (how == 1) px_piece_flip_v(&p);
	else {
		/* a whole font glyph or icon keeps its size: only square
		 * cells turn on their own */
		if (!sel && p.w != p.h && (doc.kind == PX_ZFN || doc.kind == PX_ZIC || doc.cursor)) {
			px_piece_free(&p);
			message("Only a square cell turns; select a square to turn part");
			return;
		}
		px_piece_rotate(&p);
	}
	put_target(&p, x, y);
	say(how == 0 ? "Flipped across" : how == 1 ? "Flipped down" : "Turned");
}

static void shift(int dx, int dy) {
	px_piece_t p;
	int x, y;
	if (!have_cell() || !with_target(&p, &x, &y)) return;
	px_piece_shift(&p, dx, dy);
	put_target(&p, x, y);
}

static void copy_sel(bool cut) {
	if (!have_cell()) return;
	if (floating) {
		px_piece_copy(&clip, &flt);
	} else if (sel) {
		px_piece_take(&clip, &doc, cur, selx, sely, selw, selh);
	} else {
		px_piece_take(&clip, &doc, cur, 0, 0, cw(), ch());
	}
	if (cut) {
		if (floating) {
			floating = false;
			px_piece_free(&flt);
			sel = false;
			changed();
		} else if (sel) {
			begin_change();
			px_rect(&doc, cur, selx, sely, selx + selw - 1, sely + selh - 1, px_clear(&doc), true);
			sel = false;
			changed();
		} else {
			begin_change();
			px_rect(&doc, cur, 0, 0, cw() - 1, ch() - 1, px_clear(&doc), true);
			changed();
		}
	}
	message(cut ? "Cut" : "Copied");
}

static void paste(void) {
	if (!have_cell() || !clip.px) return;
	drop_float();
	begin_change();
	px_piece_free(&flt);
	if (!px_piece_copy(&flt, &clip)) return;
	/* from a sheet of more colours: what stands in for each here */
	{
		int k;
		for (k = 0; k < flt.w * flt.h; k++) {
			int v = flt.px[k];
			if (doc.depth == 1) flt.px[k] = (uint16_t)(v == 1);
			else if (v != ZSP_T && v >= sheet_n()) flt.px[k] = (uint16_t)(sheet_n() - 1);
		}
	}
	/* at the keyboard cursor, so it lands where the person is looking */
	fltx = cx;
	flty = cy;
	selw = flt.w;
	selh = flt.h;
	sel = floating = true;
	tool = T_SEL;
	dirty_side();
	dirty_canvas();
	say("Pasted; move it, then click or press Enter");
}

static void do_undo(bool redo) {
	int i, w0 = cw(), h0 = ch();
	drop_float();
	sel = false;
	i = px_undo(&doc, redo);
	if (i < 0) { message(redo ? "Nothing to redo" : "Nothing to undo"); return; }
	if (i != cur) set_cell(i);          /* another cell: fitted afresh */
	else if (cw() != w0 || ch() != h0) {
		/* a turn undone: the zoom fits the new shape */
		if (cx >= cw()) cx = cw() - 1;
		if (cy >= ch()) cy = ch() - 1;
		zoom = fit_zoom();
	}
	clamp_view();
	ar_dirty_all();
	say(redo ? "Redo" : "Undo");
}

/* ===================================================================
 * strokes
 * =================================================================== */

static void stroke_begin(int x, int y, int v) {
	if (!have_cell()) return;
	switch (tool) {
	case T_PEN:
		begin_change();
		px_set(&doc, cur, x, y, v);
		lastx = x; lasty = y;
		stroking = true;
		stroke_v = v;
		changed_px(x, y, x, y);
		break;
	case T_LINE:
	case T_RECT:
		sx0 = sx1 = x; sy0 = sy1 = y;
		stroking = true;
		stroke_v = v;
		line_prep(sx0, sy0, sx1, sy1);
		dirty_cells(x, y, x, y);
		break;
	case T_FILL:
		begin_change();
		if (px_flood(&doc, cur, x, y, v)) changed();
		break;
	case T_PICK:
		set_colour(px_get(&doc, cur, x, y));
		break;
	case T_SEL:
		if (sel && x >= (floating ? fltx : selx) && y >= (floating ? flty : sely) &&
		    x < (floating ? fltx : selx) + selw && y < (floating ? flty : sely) + selh) {
			/* grabbed the selection: move it */
			if (lift()) {
				drag_dx = x - fltx;
				drag_dy = y - flty;
				dragging_sel = true;
			}
		} else {
			drop_float();
			if (sel) dirty_frame(selx, sely, selw, selh);
			sx0 = sx1 = x; sy0 = sy1 = y;
			sel = true;
			selx = x; sely = y; selw = selh = 1;
			stroking = true;
			dirty_frame(selx, sely, selw, selh);
		}
		break;
	}
}

static void stroke_move(int x, int y) {
	if (dragging_sel) {
		if (x - drag_dx != fltx || y - drag_dy != flty) {
			dirty_cells(fltx, flty, fltx + selw - 1, flty + selh - 1);
			fltx = x - drag_dx;
			flty = y - drag_dy;
			dirty_cells(fltx, flty, fltx + selw - 1, flty + selh - 1);
		}
		return;
	}
	if (!stroking) return;
	switch (tool) {
	case T_PEN:
		if (x != lastx || y != lasty) {
			px_line(&doc, cur, lastx, lasty, x, y, stroke_v);
			changed_px(lastx, lasty, x, y);
			lastx = x; lasty = y;
		}
		break;
	case T_LINE:
	case T_RECT:
		if (x != sx1 || y != sy1) {
			dirty_shape(tool, sx0, sy0, sx1, sy1);     /* where it was */
			sx1 = x; sy1 = y;
			line_prep(sx0, sy0, sx1, sy1);
			dirty_shape(tool, sx0, sy0, sx1, sy1);     /* and is */
		}
		break;
	case T_SEL: {
		int x0 = sx0 < x ? sx0 : x, y0 = sy0 < y ? sy0 : y;
		int x1 = sx0 < x ? x : sx0, y1 = sy0 < y ? y : sy0;
		x0 = clampi(x0, 0, cw() - 1); x1 = clampi(x1, 0, cw() - 1);
		y0 = clampi(y0, 0, ch() - 1); y1 = clampi(y1, 0, ch() - 1);
		if (x0 != selx || y0 != sely || x1 - x0 + 1 != selw || y1 - y0 + 1 != selh) {
			dirty_frame(selx, sely, selw, selh);
			selx = x0; sely = y0; selw = x1 - x0 + 1; selh = y1 - y0 + 1;
			dirty_frame(selx, sely, selw, selh);
		}
		break;
	}
	default:
		break;
	}
}

static void stroke_end(void) {
	if (dragging_sel) {
		dragging_sel = false;
		return;
	}
	if (!stroking) return;
	stroking = false;
	if (tool == T_LINE) {
		begin_change();
		px_line(&doc, cur, sx0, sy0, sx1, sy1, stroke_v);
		changed_px(sx0, sy0, sx1, sy1);
	} else if (tool == T_RECT) {
		begin_change();
		px_rect(&doc, cur, sx0, sy0, sx1, sy1, stroke_v, rect_filled);
		changed_px(sx0, sy0, sx1, sy1);
	} else if (tool == T_SEL) {
		char b[40];
		snprintf(b, sizeof(b), "Selected %d by %d", selw, selh);
		say(b);
	}
}

/* ===================================================================
 * files
 * =================================================================== */

static uint8_t *read_file(const char *p, uint32_t *len) {
#ifdef AR_HOST_TESTS
	FILE *f = fopen(p, "rb");
	uint8_t *b;
	long n;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc((size_t)n + 1);
	if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
	fclose(f);
	*len = (uint32_t)n;
	return b;
#else
	int n = fs_size((char *)p);
	if (n <= 0) return NULL;
	*len = (uint32_t)n;
	return (uint8_t *)fs_mallocfile((char *)p);
#endif
}

static bool write_file(const char *p, const uint8_t *data, uint32_t len) {
#ifdef AR_HOST_TESTS
	FILE *f = fopen(p, "wb");
	bool ok;
	if (!f) return false;
	ok = fwrite(data, 1, len, f) == len;
	fclose(f);
	return ok;
#else
	return fs_write_file((char *)p, (char *)data, (int)len) == (int)len;
#endif
}

static void after_load(void) {
	px_undo_clear();
	sel = floating = stroking = false;
	px_piece_free(&flt);
	cur = -1;
	cx = cy = 0;
	editor = false;
	view_tier = 0;
	page = 0;
	fg = 1;
	bg2 = px_clear(&doc);
	want_colours();
	colours_setup();
	set_cell(0);
	ar_dirty_all();
}

static bool load(const char *p) {
	uint32_t len = 0;
	uint8_t *data = read_file(p, &len);
	char err[80] = "";
	int kind;
	px_doc_t n;

	if (!data) { message("Cannot read that file"); return false; }
	kind = px_detect(data, len, p);
	px_doc_init(&n, 0);
	if (!kind && px_import_sniff(data, len < 16 ? (int)len : 16)) {
		/* a PNG, GIF, BMP, PNM or JPEG: a new colour picture of it */
		px_src_t src;
		px_mem_t m;
		int seen = 0;
		char b[80];
		const char *dot;
		px_src_mem(&src, &m, data, len);
		if (!px_import_picture(&n, &src, &seen, err, sizeof(err))) {
			free(data);
			px_doc_free(&n);
			message(err);
			return false;
		}
		free(data);
		px_doc_free(&doc);
		doc = n;
		path[0] = 0;
		/* its name, to save as a ZBM */
		dot = strrchr(p, '.');
		snprintf(suggest, sizeof(suggest), "%.*s.zbm", dot && dot > strrchr(p, '/') ? (int)(dot - p) : (int)strlen(p), p);
		overrides = false;
		after_load();
		doc.dirty = true;
		if (seen > 256) snprintf(b, sizeof(b), "Imported %d x %d: %d colours, made 256", doc.cell[0].w, doc.cell[0].h, seen);
		else snprintf(b, sizeof(b), "Imported %d x %d in %d colours", doc.cell[0].w, doc.cell[0].h, seen);
		message(b);
		return true;
	}
	if (!kind || !px_read(&n, kind, data, len, err, sizeof(err))) {
		free(data);
		px_doc_free(&n);
		message(kind ? err : "Not a kind of file pix opens");
		return false;
	}
	free(data);

	overrides = kind == PX_ZIC && !strcmp(p, Z_PATH_WM_ICONS);
	if (overrides) {
		/* the person's own icons over the release's (docs/pix_app.md) */
		data = read_file(Z_PATH_WM_ICON_OVERRIDES, &len);
		if (data) {
			if (!px_zic_overlay(&n, data, len, err, sizeof(err)))
				message("icons.zic is damaged; showing the release's icons");
			free(data);
		}
	}
	if (!strcmp(p, Z_PATH_WM_CURSOR)) n.cursor = true;

	px_doc_free(&doc);
	doc = n;
	strncpy(path, p, sizeof(path) - 1);
	path[sizeof(path) - 1] = 0;
	suggest[0] = 0;
	after_load();
	{
		char b[64];
		if (doc.kind == PX_ZFN)
			snprintf(b, sizeof(b), "Opened a font: %d glyphs", doc.ncells);
		else if (doc.kind == PX_ZBM)
			snprintf(b, sizeof(b), "Opened a picture, %d x %d", doc.cell[0].w, doc.cell[0].h);
		else if (doc.cursor)
			snprintf(b, sizeof(b), "Opened the pointer");
		else
			snprintf(b, sizeof(b), "Opened %d %s", doc.ncells, kind_name(doc.kind));
		message(b);
	}
	return true;
}

static bool save_to(const char *p) {
	uint32_t len = 0;
	uint8_t *data;
	const char *target = p;
	bool changed_only = false;

	drop_float();
	if (overrides && !strcmp(p, Z_PATH_WM_ICONS)) {
		target = Z_PATH_WM_ICON_OVERRIDES;
		changed_only = true;
	}
	data = px_write(&doc, changed_only, &len);
	if (!data) { message("Out of memory"); return false; }
	if (!write_file(target, data, len)) {
		free(data);
		message("Could not save");
		return false;
	}
	free(data);
	doc.dirty = false;
	if (target != p) message("Saved your icons to icons.zic");
	else if (!strcmp(p, Z_PATH_WM_CURSOR)) message("Saved; cfg reload shows the pointer");
	else message("Saved");
	dirty_side();
	return true;
}

/* -- the boxes (dialogs) -- */

#ifndef AR_HOST_TESTS
/* On pix's own screen, in the window and full screen alike: the screen
 * toolkit's boxes (ui/sui.h, docs/screen_ui.md). F11 stays where it
 * is. Same answers as zdialog's: Cancel 0, Yes 1, No 2. */
static bool ask(const char *title, const char *q, const char *initial, char *out, int len) {
	return sui_prompt(title, q, initial, out, len);
}

static int confirm(const char *title, const char *q, int buttons) {
	return sui_confirm(title, q, buttons);
}

static bool pick_open(char *out, int len) {
	return sui_open(NULL, out, len);
}

static bool pick_save(const char *suggest, char *out, int len) {
	return sui_save(NULL, suggest, out, len);
}
#else
/* The host tests answer the boxes from pix_host_answer (tests/render.c)
 * -- or, with PIX_BOXES set, run the screen toolkit's real boxes, which
 * the script answers with keys ("boxes" scene). */
const char *pix_host_answer;
static bool real_boxes(void) { return getenv("PIX_BOXES") != NULL; }
static bool ask(const char *title, const char *q, const char *initial, char *out, int len) {
	if (real_boxes()) return sui_prompt(title, q, initial, out, len);
	if (!pix_host_answer) return false;
	/* "=": OK on what the box was opened with */
	strncpy(out, !strcmp(pix_host_answer, "=") && initial ? initial : pix_host_answer, (size_t)len - 1);
	out[len - 1] = 0;
	return true;
}
static int confirm(const char *title, const char *q, int buttons) {
	if (real_boxes()) return sui_confirm(title, q, buttons);
	return 2;           /* "no": a script never saves over its inputs */
}
static bool pick_open(char *out, int len) {
	if (real_boxes()) return sui_open(NULL, out, len);
	return ask(NULL, NULL, NULL, out, len);
}
static bool pick_save(const char *s, char *out, int len) {
	if (real_boxes()) return sui_save(NULL, s, out, len);
	return ask(NULL, NULL, NULL, out, len);
}
#endif

/* Unsaved work: ask before it goes. True to go on. */
static bool may_discard(void) {
	int r;
	if (!doc.dirty) return true;
	r = confirm("pix", "Save your changes first?", 2);
	if (r == 0) return false;            /* cancel */
	if (r == 1) {                        /* yes */
		if (!path[0]) {
			char p[160];
			if (!pick_save(NULL, p, sizeof(p))) return false;
			if (!save_to(p)) return false;
			strcpy(path, p);
			return true;
		}
		return save_to(path);
	}
	return true;                         /* no */
}

static void do_save(bool as) {
	char p[160];
	if (!as && path[0]) { save_to(path); return; }
	if (!pick_save(path[0] ? path : suggest[0] ? suggest : NULL, p, sizeof(p))) return;
	if (save_to(p)) {
		strcpy(path, p);
		overrides = doc.kind == PX_ZIC && !strcmp(p, Z_PATH_WM_ICONS);
	}
}

/* Ctrl+I: a picture from elsewhere into this cell, in its colours, as
 * a pasted selection to move and put down. */
static void import_into(void) {
	char p[160], err[80];
	uint8_t *data;
	uint32_t len = 0;
	px_src_t src;
	px_mem_t m;
	px_piece_t pc = { 0, 0, NULL };
	if (!have_cell()) return;
	if (!pick_open(p, sizeof(p))) return;
	data = read_file(p, &len);
	if (!data) { message("Cannot read that file"); return; }
	if (!px_import_sniff(data, len < 16 ? (int)len : 16)) { free(data); message("Not a picture pix can bring in"); return; }
	px_src_mem(&src, &m, data, len);
	if (!px_import_piece(&doc, &src, &pc, err, sizeof(err))) { free(data); message(err); return; }
	free(data);
	drop_float();
	begin_change();
	px_piece_free(&flt);
	flt = pc;
	fltx = cx;
	flty = cy;
	selw = flt.w;
	selh = flt.h;
	sel = floating = true;
	tool = T_SEL;
	dirty_side();
	dirty_canvas();
	{
		char b[64];
		snprintf(b, sizeof(b), "Imported %d x %d: move it, then click or Enter", flt.w, flt.h);
		message(b);
	}
}

static void do_open(void) {
	char p[160];
	if (!may_discard()) return;
	if (pick_open(p, sizeof(p))) load(p);
}

/* A new document from a few words: "icon", "cursor", "sprite W H",
 * "font W H", "picture W H". */
static bool make_new(const char *what) {
	char word[16] = "";
	int w = 0, h = 0, n, i;
	px_doc_t d;

	int colours = board_colours;

	n = sscanf(what, "%15s %d %d %d", word, &w, &h, &colours);
	if (n < 1) return false;
	for (i = 0; word[i]; i++) word[i] = (char)tolower((unsigned char)word[i]);
	if (colours != 2 && colours != 4 && colours != 16 && colours != 64 && colours != 256) {
		message("Colours: 2, 4, 16, 64 or 256");
		return false;
	}

	px_doc_init(&d, 0);
	if (!strcmp(word, "icon")) {
		px_doc_init(&d, PX_ZIC);
		i = px_cell_insert(&d, -1, 32, 32, "new");
		if (i >= 0) strcpy(d.cell[i].spoken, "New");
	} else if (!strcmp(word, "cursor") || !strcmp(word, "pointer")) {
		px_doc_init(&d, PX_ZSP);
		d.cursor = true;
		px_cell_insert(&d, -1, 16, 16, "normal");
		px_cell_insert(&d, -1, 16, 16, "busy");
	} else if (!strcmp(word, "sprite") || !strcmp(word, "sprites")) {
		if (n < 3) { w = 16; h = 16; }
		px_doc_init(&d, PX_ZSP);
		zsp_palette_init(&d.pal, colours);
		px_cell_insert(&d, -1, w, h, "sprite0");
	} else if (!strcmp(word, "font")) {
		if (n < 3) { w = 6; h = 12; }
		if (w > 16 || h > 32) { px_doc_free(&d); message("A font cell is at most 16 x 32"); return false; }
		px_doc_init(&d, PX_ZFN);
		d.ascent = h - h / 4;
		i = px_cell_insert(&d, -1, w, h, NULL);
		if (i >= 0) { d.cell[i].code = 'A'; px_glyph_name(d.cell[i].name, 'A'); }
	} else if (!strcmp(word, "picture")) {
		if (n < 3) { w = 320; h = 240; }
		px_doc_init(&d, PX_ZBM);
		if (n >= 4 && colours > 2) {
			/* a colour picture: a byte a pixel, a palette */
			if ((uint32_t)w * (uint32_t)h > PX_MAX_COLOUR_PX) {
				px_doc_free(&d);
				message("A colour picture is at most 512K pixels (1024 x 512)");
				return false;
			}
			d.depth = 8;
			zsp_palette_init(&d.pal, colours);
		}
		px_cell_insert(&d, -1, w, h, "picture");
	} else {
		px_doc_free(&d);
		return false;
	}
	if (d.ncells == 0) { px_doc_free(&d); message("Too big"); return false; }

	px_doc_free(&doc);
	doc = d;
	doc.dirty = false;
	path[0] = 0;
	overrides = false;
	after_load();
	message(doc.cursor ? "New pointer: save as " Z_PATH_WM_CURSOR : "New");
	return true;
}

static void do_new(void) {
	char a[40];
	if (!may_discard()) return;
	if (!ask("New", "What? icon, cursor, sprite 16 16 (64),\nfont 6 12, picture 320 240 (256)",
	    "sprite 16 16", a, sizeof(a))) return;
	if (!make_new(a)) message("Not one of: icon, cursor, sprite, font, picture");
}

/* -- cells -- */

/* A codepoint from "A", "U+00C4", "0x41" or "65". -1 if none. */
static long parse_code(const char *s) {
	char *e;
	long v;
	if (!s[0]) return -1;
	if ((s[0] == 'U' || s[0] == 'u') && s[1] == '+') return strtol(s + 2, &e, 16);
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return strtol(s + 2, &e, 16);
	if (s[1] == 0) return (unsigned char)s[0];
	v = strtol(s, &e, 10);
	return *e ? -1 : v;
}

static void new_cell(void) {
	char a[40];
	int i;
	if (doc.kind == PX_ZBM) { message("A picture is one cell"); return; }
	if (doc.kind == PX_ZFN) {
		long code;
		bool ex;
		if (!ask("New glyph", "Which character? (A, U+00C4, 65)", NULL, a, sizeof(a))) return;
		code = parse_code(a);
		if (code < 0 || code > 0xffff) { message("Not a character this font can hold"); return; }
		i = px_glyph_slot(&doc, (uint32_t)code, &ex);
		if (ex) { set_cell(i); message("That glyph is already here"); return; }
		i = px_cell_insert(&doc, i, cw(), ch(), NULL);
		if (i < 0) { message("Out of memory"); return; }
		doc.cell[i].code = (uint32_t)code;
		px_glyph_name(doc.cell[i].name, (uint32_t)code);
	} else if (doc.kind == PX_ZIC) {
		if (!ask("New icon", "The app's name, as in " Z_DIR_APPS, NULL, a, sizeof(a))) return;
		if (strlen(a) > 15) { message("At most 15 letters"); return; }
		if (px_find(&doc, a) >= 0) { set_cell(px_find(&doc, a)); message("There is one"); return; }
		i = 0;
		while (i < doc.ncells && strcmp(doc.cell[i].name, a) < 0) i++;
		i = px_cell_insert(&doc, i, 32, 32, a);
		if (i < 0) { message("Out of memory"); return; }
		strncpy(doc.cell[i].spoken, a, PX_NAME_MAX - 1);
		doc.cell[i].spoken[0] = (char)toupper((unsigned char)doc.cell[i].spoken[0]);
	} else {
		if (!ask("New sprite", "Name, and size if not this one's (walk 16 16)", NULL, a, sizeof(a))) return;
		{
			char nm[PX_NAME_MAX];
			int w = cw(), h = ch();
			if (sscanf(a, "%23s %d %d", nm, &w, &h) < 1) return;
			if (strchr(nm, ' ')) return;
			i = px_cell_insert(&doc, cur + 1, w, h, nm);
			if (i < 0) { message("Too big, or out of memory"); return; }
		}
	}
	px_touch(&doc, i);
	px_undo_clear();
	set_cell(i);
}

static void dup_cell(void) {
	char nm[PX_NAME_MAX];
	int i;
	if (!have_cell() || doc.kind == PX_ZBM || doc.kind == PX_ZFN) {
		message(doc.kind == PX_ZFN ? "A glyph is one character: Ins adds one" : "A picture is one cell");
		return;
	}
	/* the next frame's name: walk1 -> walk2 */
	px_anim_next_name(&doc, doc.cell[cur].name, nm);
	if (doc.kind == PX_ZIC) {
		/* a copy for another app: its own name, in the name order the
		 * dock reads in */
		char a[40];
		uint8_t icon[128];
		px_cell_t was = doc.cell[cur];
		if (!ask("Copy icon", "The new app's name", nm, a, sizeof(a))) return;
		if (strlen(a) > 15 || px_find(&doc, a) >= 0) { message("Too long, or taken"); return; }
		memcpy(icon, px_cell_rows(&doc, cur), sizeof(icon));
		i = 0;
		while (i < doc.ncells && strcmp(doc.cell[i].name, a) < 0) i++;
		i = px_cell_insert(&doc, i, 32, 32, a);
		if (i < 0) { message("Out of memory"); return; }
		memcpy(px_cell_rows(&doc, i), icon, sizeof(icon));
		strcpy(doc.cell[i].spoken, was.spoken);
		doc.cell[i].feat1 = was.feat1;
		doc.cell[i].feat2 = was.feat2;
		px_touch(&doc, i);
	} else {
		char was[PX_NAME_MAX];
		strcpy(was, doc.cell[cur].name);
		i = px_cell_dup(&doc, cur, nm);
		if (i < 0) { message("Out of memory"); return; }
		/* the next frame: after this one in its animations too */
		px_anim_insert_after(&doc, was, nm);
	}
	px_undo_clear();
	set_cell(i);
}

static void delete_cell(void) {
	char q[64];
	if (!have_cell()) return;
	if (doc.kind == PX_ZBM) { message("A picture is one cell"); return; }
	snprintf(q, sizeof(q), "Delete %s?", doc.cell[cur].name);
	if (confirm("pix", q, 1) != 1) return;
	px_anim_forget(&doc, doc.cell[cur].name);
	px_cell_delete(&doc, cur);
	px_undo_clear();
	{
		int i = cur;
		cur = -1;
		set_cell(i);
	}
	ar_dirty_all();
}

static void rename_cell(void) {
	char a[40];
	if (!have_cell()) return;
	if (doc.kind == PX_ZFN) {
		message("A glyph's name is its character");
		return;
	}
	if (doc.kind == PX_ZIC) {
		if (!ask("Spoken name", "What the dock says for this icon", doc.cell[cur].spoken, a, sizeof(a))) return;
		strncpy(doc.cell[cur].spoken, a, PX_NAME_MAX - 1);
		doc.cell[cur].spoken[PX_NAME_MAX - 1] = 0;
		px_touch(&doc, cur);
		dirty_bottom();
		return;
	}
	if (!ask("Rename", "The cell's name", doc.cell[cur].name, a, sizeof(a))) return;
	if (!a[0] || strchr(a, ' ') || strlen(a) >= PX_NAME_MAX) { message("A name has no spaces, up to 23 letters"); return; }
	if (px_find(&doc, a) >= 0 && px_find(&doc, a) != cur) { message("Taken"); return; }
	px_anim_rename(&doc, doc.cell[cur].name, a);
	strncpy(doc.cell[cur].name, a, PX_NAME_MAX - 1);
	doc.cell[cur].name[PX_NAME_MAX - 1] = 0;
	px_touch(&doc, cur);
	dirty_bottom();
}

static void goto_cell(void) {
	char a[40];
	int i;
	if (!ask("Go to", doc.kind == PX_ZFN ? "Which character? (A, U+00C4, 65)" : "Which cell? (name or number)",
	    NULL, a, sizeof(a))) return;
	if (doc.kind == PX_ZFN) {
		long code = parse_code(a);
		bool ex;
		if (code < 0) return;
		i = px_glyph_slot(&doc, (uint32_t)code, &ex);
		if (!ex) { message("No glyph for that character: Ins adds one"); return; }
	} else {
		i = px_find(&doc, a);
		if (i < 0 && isdigit((unsigned char)a[0])) i = atoi(a) - 1;
		if (i < 0 || i >= doc.ncells) { message("No such cell"); return; }
	}
	set_cell(i);
}

/* ===================================================================
 * input
 * =================================================================== */

static void set_tool(int t) {
	if (t == T_RECT && tool == T_RECT) rect_filled = !rect_filled;
	if (t != T_SEL) select_none();
	tool = t;
	stroking = false;
	dirty_tools();
	say(t == T_RECT && rect_filled ? "Filled rectangle" : tool_name[t]);
}

/* The Commodore 64's names, for the colours a new sheet starts with. */
static const char *const c64_name[16] = {
	"black", "white", "red", "cyan", "purple", "green", "blue", "yellow",
	"orange", "brown", "light red", "dark grey", "grey", "light green",
	"light blue", "light grey"
};
static const uint16_t c64_rgb[16] = {
	0x000, 0xfff, 0x633, 0x7aa, 0x748, 0x584, 0x327, 0xbc7,
	0x752, 0x430, 0x965, 0x444, 0x666, 0x9c8, 0x66b, 0x999
};

/* "colour 9, brown", "clear", "ink" */
static void colour_say(char *b, int n, int v) {
	int i;
	if (v == ZSP_T) { snprintf(b, (size_t)n, "clear"); return; }
	if (doc.depth == 1) { snprintf(b, (size_t)n, "%s", v ? "ink" : "background"); return; }
	for (i = 0; i < 16; i++)
		if (doc.pal.pal[v & 255].rgb == c64_rgb[i]) {
			snprintf(b, (size_t)n, "colour %d, %s", v, c64_name[i]);
			return;
		}
	snprintf(b, (size_t)n, "colour %d", v);
}

static void set_colour(int c) {
	char b[40];
	if (c == ZSP_T && doc.depth != 16) c = 0;     /* only a sheet has see-through */
	if (c != ZSP_T && c >= sheet_n()) return;
	fg = c;
	if (c != ZSP_T) page = c / 16;
	dirty_chips();
	if (editor) dirty_canvas();
	colour_say(b, sizeof(b), c);
	say(b);
}

/* ===================================================================
 * animation
 * =================================================================== */

/* Screen frames a frame of the animation shows: ms at 60 a second. */
static int frames_of(int ms) { int f = (ms * 60 + 500) / 1000; return f < 1 ? 1 : f; }

/* The frame shown, from what F5 plays for the current cell now: the
 * animation is looked up again each step, so a cell drawn, added or
 * renamed while it plays is in it at once. */
static void play_step(bool advance) {
	px_anim_t a;
	int at, was = play_cell;
	px_anim_for(&doc, cur, &a, &at);
	if (a.n < 1) { play_cell = -1; return; }
	if (advance) play_at = (play_at + 1) % a.n;
	if (play_at >= a.n) play_at = 0;
	play_cell = a.cell[play_at];
	play_left = frames_of(a.ms);
	if (play_cell != was) ar_dirty(PV_X, BT_Y + 1, PV_W, ST_H);
}

/* F5 */
static void toggle_play(void) {
	px_anim_t a;
	int at;
	char b[64];
	if (playing) {
		playing = false;
		play_cell = -1;
		ar_dirty(PV_X, BT_Y + 1, PV_W, ST_H);
		message("Stopped");
		return;
	}
	if (!have_cell()) return;
	px_anim_for(&doc, cur, &a, &at);
	if (a.n < 2) { message("One frame: nothing to play (Ctrl+D makes the next)"); return; }
	playing = true;
	play_at = at;
	play_cell = -1;
	play_step(false);
	if (a.name[0]) snprintf(b, sizeof(b), "Playing %s: %d frames, %d ms", a.name, a.n, a.ms);
	else snprintf(b, sizeof(b), "Playing the %d cells this size, %d ms", a.n, a.ms);
	message(b);
}

/* F6 */
static void toggle_onion(void) {
	static const char *const said[3] = { "Onion skin off", "Onion skin: the frame before",
		"Onion skin: before and after" };
	onion = (onion + 1) % 3;
	dirty_canvas();
	message(said[onion]);
}

/* Shift+F5: the animation the current cell is in, as words to edit --
 * or a new one, of the cells this size. */
static void edit_anim(void) {
	px_anim_t a;
	int at, k, i, n;
	char init[160], out[160], stem[PX_NAME_MAX];
	const char *err;
	if (!have_cell()) return;
	if (doc.kind != PX_ZSP) { message("Animations are kept in sprite sheets"); return; }
	k = px_anim_for(&doc, cur, &a, &at);
	if (k >= 0) px_anim_words(&doc, k, init, sizeof(init));
	else {
		/* a name from the cell's, without its number */
		snprintf(stem, sizeof(stem), "%s", doc.cell[cur].name);
		i = (int)strlen(stem);
		while (i > 1 && stem[i - 1] >= '0' && stem[i - 1] <= '9') stem[--i] = 0;
		n = snprintf(init, sizeof(init), "%s %d", stem, a.ms);
		for (i = 0; i < a.n && n < (int)sizeof(init) - PX_NAME_MAX - 2; i++)
			n += snprintf(init + n, sizeof(init) - (size_t)n, " %s", doc.cell[a.cell[i]].name);
	}
	if (!ask("Animation", "Name, ms a frame, the cells (a name alone deletes)", init, out, sizeof(out))) return;
	if (!out[0]) return;
	if (!px_anim_set(&doc, out, &err)) { message(err); return; }
	dirty_canvas();
	dirty_side();
	{
		char b[64], nm[PX_NAME_MAX];
		sscanf(out, "%23s", nm);
		for (k = 0; k < px_anim_count(&doc); k++)
			if (px_anim_get(&doc, k, &a) && !strcmp(a.name, nm)) break;
		if (k < px_anim_count(&doc)) snprintf(b, sizeof(b), "%s: %d frames, %d ms", a.name, a.n, a.ms);
		else snprintf(b, sizeof(b), "%s deleted", nm);
		message(b);
	}
}

/* F7: as the sheet is, then each smaller form in turn. */
static void next_view(void) {
	static const int tiers[4] = { 64, 16, 4, 2 };
	int i, t = tier(), nt = 0;
	char b[40];
	if (sheet_n() <= 2) { message("Two colours: there is nothing fewer"); return; }
	for (i = 0; i < 4; i++) if (tiers[i] < t) { nt = tiers[i]; break; }
	view_tier = nt;
	colours_setup();
	if (view_tier) snprintf(b, sizeof(b), "Shown as %d colours", view_tier);
	else snprintf(b, sizeof(b), "Shown as it is, %d colours", sheet_n());
	message(b);
}

/* The sheet's number of colours. Fewer maps every pixel through the
 * tables (and 4 and 2 take the slots' colours, and black and white);
 * more changes no pixel. Neither can be undone. */
static void set_sheet_colours(int m) {
	int n = sheet_n(), i, x, y, k;
	if (!px_colour(&doc)) { message("Only a sprite sheet or a colour picture has a palette"); return; }
	if (m == n) return;
	if (m < n) {
		char q[96];
		snprintf(q, sizeof(q), "Reduce to %d colours?\nEvery pixel takes the colour\nthat stands in for it.", m);
		if (confirm("pix", q, 1) != 1) return;
		drop_float();
		for (i = 0; i < doc.ncells; i++) {
			for (y = 0; y < doc.cell[i].h; y++)
				for (x = 0; x < doc.cell[i].w; x++)
					px_set(&doc, i, x, y, zsp_reduce(&doc.pal, px_get(&doc, i, x, y), m));
			doc.cell[i].flags |= PX_CELL_CHANGED;
		}
		if (m == 4)
			for (k = 0; k < 4; k++) { doc.pal.pal[k].rgb = doc.pal.slot_rgb[k]; doc.pal.pal[k].given = 0; }
		if (m == 2) {
			doc.pal.pal[0].rgb = 0x000; doc.pal.pal[1].rgb = 0xfff;
			doc.pal.pal[0].given = doc.pal.pal[1].given = 0;
		}
		doc.pal.colours = m;
		for (k = 0; k < m; k++) zsp_defaults(&doc.pal, k);
		for (k = 0; k < doc.pal.ndiffer; )
			if (doc.pal.differ[k][0] >= m || doc.pal.differ[k][1] >= m) {
				doc.pal.differ[k][0] = doc.pal.differ[doc.pal.ndiffer - 1][0];
				doc.pal.differ[k][1] = doc.pal.differ[doc.pal.ndiffer - 1][1];
				doc.pal.ndiffer--;
			} else k++;
	} else {
		zsp_palette_t fresh;
		zsp_palette_init(&fresh, m);
		for (k = n; k < 256; k++) doc.pal.pal[k] = fresh.pal[k];
		doc.pal.colours = m;
		for (k = n; k < m; k++) zsp_defaults(&doc.pal, k);
	}
	px_undo_clear();
	doc.dirty = true;
	if (fg != ZSP_T && fg >= m) fg = m - 1;
	if (bg2 != ZSP_T && bg2 >= m) bg2 = 0;
	page = fg == ZSP_T ? 0 : fg / 16;
	view_tier = 0;
	want_colours();
	colours_setup();
	{
		char b[40];
		snprintf(b, sizeof(b), "%d colours", m);
		message(b);
	}
}

/* A palette field one step up or down. */
static void ed_change(int delta) {
	zsp_colour_t *c = &doc.pal.pal[ed_entry()];
	int v, comp;
	char g;
	if (ed_field == ED_COLOURS) {
		static const int counts[5] = { 2, 4, 16, 64, 256 };
		int k = 0;
		while (k < 4 && counts[k] != sheet_n()) k++;
		k = clampi(k + delta, 0, 4);
		set_sheet_colours(counts[k]);
		return;
	}
	if (ed_field == ED_ENTRY) {
		set_colour(clampi(ed_entry() + delta, 0, sheet_n() - 1));
		return;
	}
	v = ed_value(ed_field, &g) + delta;
	switch (ed_field) {
	case ED_R: case ED_G: case ED_B:
		comp = ed_field == ED_R ? 8 : ed_field == ED_G ? 4 : 0;
		v = clampi(v, 0, 15);
		c->rgb = (uint16_t)((c->rgb & ~(15u << comp)) | (unsigned)(v << comp));
		zsp_defaults(&doc.pal, ed_entry());   /* what was not set by hand follows */
		break;
	case ED_TO64: c->to64 = (uint8_t)clampi(v, 0, 63); c->given |= ZSP_GIVEN_TO64; break;
	case ED_TO16: c->to16 = (uint8_t)clampi(v, 0, 15); c->given |= ZSP_GIVEN_TO16; break;
	case ED_SLOT: c->slot = (uint8_t)clampi(v, 0, 3); c->given |= ZSP_GIVEN_SLOT; break;
	case ED_INK: c->ink = (uint8_t)clampi(v, 0, 16); c->given |= ZSP_GIVEN_INK; break;
	}
	doc.dirty = true;
	colours_setup();
}

/* D: the field back to what the RGB says. */
static void ed_default(void) {
	zsp_colour_t *c = &doc.pal.pal[ed_entry()];
	static const uint8_t bit[ED_N] = { 0, 0, 0, 0, 0, ZSP_GIVEN_TO64, ZSP_GIVEN_TO16,
		ZSP_GIVEN_SLOT, ZSP_GIVEN_INK };
	if (!bit[ed_field]) return;
	c->given &= (uint8_t)~bit[ed_field];
	zsp_defaults(&doc.pal, ed_entry());
	doc.dirty = true;
	colours_setup();
	say("Default");
}

/* M: this entry and the next one M is pressed on must differ. */
static void ed_differ(void) {
	int e = ed_entry(), k;
	char b[48];
	if (differ_from < 0) {
		differ_from = e;
		snprintf(b, sizeof(b), "Must differ from %d: pick the other, M again", e);
		message(b);
		dirty_canvas();
		return;
	}
	if (differ_from != e) {
		for (k = 0; k < doc.pal.ndiffer; k++)
			if ((doc.pal.differ[k][0] == differ_from && doc.pal.differ[k][1] == e) ||
			    (doc.pal.differ[k][1] == differ_from && doc.pal.differ[k][0] == e)) break;
		if (k == doc.pal.ndiffer) {
			if (doc.pal.ndiffer < ZSP_DIFFER_MAX) {
				doc.pal.differ[k][0] = (uint8_t)differ_from;
				doc.pal.differ[k][1] = (uint8_t)e;
				doc.pal.ndiffer++;
				snprintf(b, sizeof(b), "%d and %d must differ", differ_from, e);
			} else snprintf(b, sizeof(b), "At most %d pairs", ZSP_DIFFER_MAX);
		} else {
			/* the same pair again: no longer */
			doc.pal.differ[k][0] = doc.pal.differ[doc.pal.ndiffer - 1][0];
			doc.pal.differ[k][1] = doc.pal.differ[doc.pal.ndiffer - 1][1];
			doc.pal.ndiffer--;
			snprintf(b, sizeof(b), "%d and %d may meet", differ_from, e);
		}
		doc.dirty = true;
		message(b);
	}
	differ_from = -1;
	dirty_canvas();
}

static void ed_move(int d) {
	int f = ed_field;
	do { f = (f + d + ED_N) % ED_N; } while (!ed_has(f));
	ed_field = f;
	dirty_canvas();
	{
		char b[32], g;
		snprintf(b, sizeof(b), "%s %d", ed_name[f], ed_value(f, &g));
		say(b);
	}
}

static void toggle_editor(void) {
	if (!px_colour(&doc)) { message("Icons, fonts and two-colour pictures have no palette"); return; }
	editor = !editor;
	differ_from = -1;
	if (!ed_has(ed_field)) ed_field = ED_ENTRY;
	dirty_canvas();
	say(editor ? "Palette" : "Done");
}

/* Keys while the palette is open; true if one was used. */
static bool editor_key(uint32_t k, bool shift_k) {
	int step = shift_k ? 4 : 1;
	switch (k) {
	case Z_KEY_UP: ed_move(-1); return true;
	case Z_KEY_DOWN: ed_move(1); return true;
	case Z_KEY_LEFT: ed_change(-step); return true;
	case Z_KEY_RIGHT: ed_change(step); return true;
	case 'd': case 'D': ed_default(); return true;
	case 'm': case 'M': ed_differ(); return true;
	case 0x1b: case Z_KEY_F8: toggle_editor(); return true;
	default: return false;
	}
}

/* A click on the palette: an entry (left: the first colour, right: the
 * second), or a field (its left half down, its right half up). */
static void editor_click(int x, int y, bool right) {
	int cols, gw, gh, i, row = 0;
	ed_grid(&cols, &gw, &gh);
	if (x >= 4 && x < 4 + cols * gw && y >= ED_Y0) {
		i = (y - ED_Y0) / gh * cols + (x - 4) / gw;
		if (i < sheet_n()) {
			if (right) { bg2 = i; dirty_side(); }
			else set_colour(i);
		}
		return;
	}
	for (i = 0; i < ED_N; i++) {
		int fy;
		if (!ed_has(i)) continue;
		fy = ED_Y0 + row * ED_ROW;
		if (x >= ED_X0 && y >= fy - 1 && y < fy + 9) {
			ed_field = i;
			ed_change(x < ED_X0 + 60 ? -1 : 1);
			dirty_canvas();
			return;
		}
		row++;
	}
}

static void set_zoom(int z) {
	int ccx, ccy;
	if (z < 1) z = 1;
	if (z > 16) z = 16;
	if (z == zoom) return;
	/* keep the middle of the view where it was */
	ccx = vx + CV_W / zoom / 2;
	ccy = vy + CV_H / zoom / 2;
	zoom = z;
	vx = ccx - CV_W / zoom / 2;
	vy = ccy - CV_H / zoom / 2;
	clamp_view();
	dirty_canvas();
	dirty_zoom();
}

static void move_cursor(int dx, int dy) {
	if (!have_cell()) return;
	kbd_cursor = true;
	cx = clampi(cx + dx, 0, cw() - 1);
	cy = clampi(cy + dy, 0, ch() - 1);
	if (floating && tool == T_SEL) {
		fltx += dx;
		flty += dy;
	}
	{
		/* only the cursor's old and new places, unless the view
		 * scrolled or something is being dragged with it */
		int ovx = vx, ovy = vy, ox_ = cx - dx, oy_ = cy - dy;
		stroke_move(cx, cy);
		follow_cursor();
		if (vx != ovx || vy != ovy || floating || stroking) {
			dirty_canvas();
			dirty_bottom();
		} else {
			dirty_px(ox_, oy_, ox_, oy_);
			dirty_px(cx, cy, cx, cy);
			dirty_status();
		}
	}
	{
		char b[32];
		snprintf(b, sizeof(b), "%d, %d, %s", cx, cy, shown(cx, cy) ? "ink" : "clear");
		say(b);
	}
}

/* Space: the tool at the keyboard cursor. A line or rectangle is two
 * presses, start and end; a selection too. */
static void space(void) {
	if (!have_cell()) return;
	if ((tool == T_LINE || tool == T_RECT || tool == T_SEL) && stroking) {
		stroke_end();
		return;
	}
	if (tool == T_SEL && floating) {
		drop_float();
		say("Put down");
		return;
	}
	stroke_begin(cx, cy, fg);
	if (tool == T_PEN) stroking = false;
}

static void key(uint32_t k, uint8_t mods) {
	bool ctrl = (mods & Z_KBD_MOD_CTRL) != 0, shift_k = (mods & Z_KBD_MOD_SHIFT) != 0;

	if (ctrl) {
		switch (k) {
		case 0x1a: do_undo(shift_k); return;            /* Ctrl+Z */
		case 0x19: do_undo(true); return;               /* Ctrl+Y */
		case 0x03: copy_sel(false); return;             /* Ctrl+C */
		case 0x18: copy_sel(true); return;              /* Ctrl+X */
		case 0x16: paste(); return;                     /* Ctrl+V */
		case 0x01:                                      /* Ctrl+A */
			if (!have_cell()) return;
			drop_float();
			tool = T_SEL;
			sel = true;
			selx = sely = 0;
			selw = cw();
			selh = ch();
			dirty_canvas();
			dirty_side();
			say("Selected all");
			return;
		case 0x04: dup_cell(); return;                  /* Ctrl+D */
		case 0x0e: do_new(); return;                    /* Ctrl+N */
		case 0x0f: do_open(); return;                   /* Ctrl+O */
		case 0x09: import_into(); return;               /* Ctrl+I */
		case 0x13: do_save(shift_k); return;            /* Ctrl+S */
		case 0x11: if (may_discard()) quit = true; return;   /* Ctrl+Q */
		case Z_KEY_LEFT: shift(-1, 0); return;
		case Z_KEY_RIGHT: shift(1, 0); return;
		case Z_KEY_UP: shift(0, -1); return;
		case Z_KEY_DOWN: shift(0, 1); return;
		default: return;
		}
	}

	if (editor && editor_key(k, shift_k)) return;

	/* 0-9, a-f: a colour of this page (not the tool letters) */
	{
		int h = (k >= '0' && k <= '9') ? (int)(k - '0') :
			(k >= 'a' && k <= 'f') ? (int)(k - 'a' + 10) : -1;
		if (h >= 0 && h < 16) { set_colour(page * 16 + h); return; }
	}

	if (shift_k) {
		int s = (CV_W / zoom) / 4 + 1;
		switch (k) {
		case Z_KEY_PAGEUP:
		case Z_KEY_PAGEDOWN:
			if (sheet_n() <= 16) return;
			page = (page + (k == Z_KEY_PAGEUP ? -1 : 1) + sheet_n() / 16) % (sheet_n() / 16);
			dirty_side();
			{
				char b[40];
				snprintf(b, sizeof(b), "Colours %d to %d", page * 16, page * 16 + 15);
				say(b);
			}
			return;
		case Z_KEY_LEFT: vx -= s; clamp_view(); dirty_canvas(); return;
		case Z_KEY_RIGHT: vx += s; clamp_view(); dirty_canvas(); return;
		case Z_KEY_UP: vy -= s; clamp_view(); dirty_canvas(); return;
		case Z_KEY_DOWN: vy += s; clamp_view(); dirty_canvas(); return;
		case Z_KEY_DELETE: delete_cell(); return;
		default: break;
		}
	}

	switch (k) {
	case Z_KEY_LEFT: move_cursor(-1, 0); return;
	case Z_KEY_RIGHT: move_cursor(1, 0); return;
	case Z_KEY_UP: move_cursor(0, -1); return;
	case Z_KEY_DOWN: move_cursor(0, 1); return;
	case Z_KEY_PAGEUP: if (cur > 0) set_cell(cur - 1); return;
	case Z_KEY_PAGEDOWN: if (cur < doc.ncells - 1) set_cell(cur + 1); return;
	case Z_KEY_HOME: set_cell(0); return;
	case Z_KEY_END: set_cell(doc.ncells - 1); return;
	case Z_KEY_INSERT: new_cell(); return;
	case Z_KEY_DELETE:
		if (floating) {
			floating = false;
			px_piece_free(&flt);
			sel = false;
			changed();
		} else if (sel) {
			begin_change();
			px_rect(&doc, cur, selx, sely, selx + selw - 1, sely + selh - 1, px_clear(&doc), true);
			sel = false;
			changed();
		}
		return;
	case Z_KEY_RENAME: rename_cell(); return;
	case Z_KEY_FIND: goto_cell(); return;
	case ' ': space(); return;
	case '\r':
		if (floating) { drop_float(); say("Put down"); }
		else if (stroking) stroke_end();
		return;
	case 0x1b:
		/* in the window: drop what is under way */
		if (stroking) { stroking = false; dirty_canvas(); }
		else select_none();
		return;
	case 'x': case 'X': {
		int t = fg;
		fg = bg2;
		bg2 = t;
		dirty_side();
		set_colour(fg);
		return;
	}
	case '.': if (doc.depth == 16) set_colour(ZSP_T); return;
	case Z_KEY_F5: if (shift_k) edit_anim(); else toggle_play(); return;
	case Z_KEY_F6: toggle_onion(); return;
	case Z_KEY_F7: next_view(); return;
	case Z_KEY_F8: toggle_editor(); return;
	case '+': case '=': set_zoom(zoom + (zoom < 4 ? 1 : 2)); return;
	case '-': set_zoom(zoom - (zoom <= 4 ? 1 : 2)); return;
	case 'h': case 'H': flip(0); return;
	case 'v': case 'V': flip(1); return;
	case 't': case 'T': flip(2); return;
	default: break;
	}
	{
		int t;
		for (t = 0; t < T_COUNT; t++)
			if (k == (uint32_t)tool_key[t] || k == (uint32_t)toupper((unsigned char)tool_key[t])) {
				set_tool(t);
				return;
			}
	}
}

static uint8_t mouse_buttons;

/* A click in the side column or the strip. True if it was one. */
static bool click_ui(int x, int y, bool right) {
	if (x >= SD_X && y < BT_Y) {
		int i;
		for (i = 0; i < T_COUNT; i++)
			if (y >= SD_TOOLS_Y + i * SD_ROW && y < SD_TOOLS_Y + i * SD_ROW + 9) { set_tool(i); return true; }
		if (y >= SD_COL_Y && y < SD_COL_Y + SD_COL_H && chips_grid()) {
			int cols, gw, gh, gx, c, r, v;
			grid_geom(&cols, &gw, &gh, &gx);
			c = (x - gx) / gw; r = (y - SD_COL_Y) / gh;
			v = r * cols + c;
			if (x >= gx && c < cols && v < sheet_n()) {
				if (right) { bg2 = v; dirty_chips(); }
				else set_colour(v);
			}
			return true;
		}
		if (y >= SD_COL_Y && y < SD_COL_Y + 4 * (CHIP_H + 1)) {
			int c = (x - SD_X) / (CHIP_W + 1), r = (y - SD_COL_Y) / (CHIP_H + 1);
			if (c >= 0 && c < 4 && r * 4 + c < page_chips()) {
				if (right) { bg2 = page * 16 + r * 4 + c; dirty_chips(); }
				else set_colour(page * 16 + r * 4 + c);
			}
			return true;
		}
		if (y >= SD_FILE_Y && y < SD_FILE_Y + 9) { do_new(); return true; }
		if (y >= SD_FILE_Y + SD_ROW && y < SD_FILE_Y + SD_ROW + 9) { do_open(); return true; }
		if (y >= SD_FILE_Y + 2 * SD_ROW && y < SD_FILE_Y + 2 * SD_ROW + 9) { do_save(false); return true; }
		return true;
	}
	if (y >= BT_Y && y < BT_Y + ST_H + 2 && x >= ST_X) {
		int i = strip_first() + (x - ST_X) / strip_slot_w();
		if (i < doc.ncells) set_cell(i);
		return true;
	}
	return y >= BT_Y || x >= CV_X + CV_W;
}

static void mouse(const ar_event_t *e) {
	uint8_t was = mouse_buttons, now = e->buttons;
	bool down = (now & 3) && !(was & 3), up = !(now & 3) && (was & 3);
	int px, py;
	mouse_buttons = now;

	if (!have_cell()) {
		if (down) click_ui(e->x, e->y, (now & 2) != 0);
		return;
	}
	if (editor) {
		if (down) {
			if (e->x < CV_X + CV_W && e->y < CV_Y + CV_H) editor_click(e->x, e->y, (now & 2) != 0);
			else click_ui(e->x, e->y, (now & 2) != 0);
		}
		return;
	}
	px = (e->x - org_x()) < 0 ? -1 - (org_x() - e->x - 1) / zoom : (e->x - org_x()) / zoom;
	py = (e->y - org_y()) < 0 ? -1 - (org_y() - e->y - 1) / zoom : (e->y - org_y()) / zoom;

	if (down) {
		if (e->x >= CV_X + CV_W || e->y >= CV_Y + CV_H) { click_ui(e->x, e->y, (now & 2) != 0); return; }
		if (px < 0 || py < 0 || px >= cw() || py >= ch()) {
			if (tool == T_SEL) select_none();
			return;
		}
		/* the keyboard cursor comes here, and is not shown until a
		 * key moves it (a frame where the mouse went down looked like
		 * a pixel selected) */
		if (kbd_cursor) dirty_px(cx, cy, cx, cy);
		kbd_cursor = false;
		cx = px; cy = py;
		stroke_begin(px, py, (now & 2) ? bg2 : fg);
		return;
	}
	if (now & 3) {
		/* dragging: follow, clamped to the cell for the shapes */
		if (tool == T_PEN || dragging_sel) stroke_move(px, py);
		else stroke_move(clampi(px, 0, cw() - 1), clampi(py, 0, ch() - 1));
		return;
	}
	if (up) {
		stroke_end();
		dirty_status();
	}
}

static void handle(const ar_event_t *e) {
	switch (e->type) {
	case AR_EV_KEY:
		if (e->pressed) key(e->key, e->mods);
		break;
	case AR_EV_MOUSE:
		mouse(e);
		break;
	case AR_EV_WHEEL:
		set_zoom(zoom + (e->notches > 0 ? 1 : -1) * (zoom < 4 ? 1 : 2));
		break;
	case AR_EV_MODE:
		colours_setup();
		break;
	case AR_EV_QUIT:
		if (may_discard()) quit = true;
		break;
	default:
		break;
	}
}

int main(void) {
	char arg[160];

	memcpy(desc.palette, ui_rgb, sizeof(ui_rgb));
	ar_ink_level(&desc.ink[R_BG], 0);
	ar_ink_level(&desc.ink[R_INK], 16);
	desc.ink[R_OUT] = ar_ink_dots;
	desc.ink[R_GRID] = ar_ink_grey12;
	desc.ink[R_FRAME] = ar_ink_grey50;
	desc.ink[R_HI] = ar_ink_grey75;
	desc.ink[R_GUIDE] = ar_ink_grey50;
	desc.ink[R_WARN] = ar_ink_white;
	make_patterns();

	px_doc_init(&doc, PX_ZSP);
	cur = -1;

#ifndef AR_HOST_TESTS
	arg[0] = 0;
	z_launch_path_take(arg, sizeof(arg));
#else
	/* the host tests name a file in the environment */
	arg[0] = 0;
	if (getenv("PIX_OPEN")) snprintf(arg, sizeof(arg), "%s", getenv("PIX_OPEN"));
#endif

	/* colour in the window, where the board has it and wm gives it
	 * (docs/color.md, "The colour window") */
	ar_window_colour(true);
	if (!ar_init(&desc)) return 1;
	board_colours = ar_set_colours(256);
	/* every key is the program's: C and M are letters here */
	ar_text_input(true);

#ifdef AR_HOST_TESTS
	if (getenv("PIX_NEW")) { make_new(getenv("PIX_NEW")); arg[0] = 0; }
	else
#endif
	if (!arg[0] || !load(arg)) make_new("sprite 16 16");

	while (!quit) {
		ar_event_t e;
		while (ar_poll(&e)) handle(&e);
		if (quit) break;
		if (msg_frames > 0 && --msg_frames == 0) dirty_bottom();
		/* the last edit's undo step cut down to what it changed, once
		 * it is over (not mid-stroke, nor with a selection lifted) */
		if (!stroking && !floating && !(mouse_buttons & 3)) px_undo_settle(&doc);
		/* a new file, a save, the first change by any route (a
		 * rename too): the name at the right of the status line, and
		 * the side's "Save *" */
		{
			char nm[sizeof(shown_name)];
			file_label(nm, sizeof(nm));
			if (strcmp(nm, shown_name)) {
				dirty_status();
				ar_dirty(SD_X - 2, SD_FILE_Y + 2 * SD_ROW, SD_W + 2, 9);
			}
		}
		{
			uint32_t n = ar_frame();
			if (playing && (play_left -= (int)n) <= 0) play_step(true);
		}
	}

	ar_shutdown();
	return 0;
}
