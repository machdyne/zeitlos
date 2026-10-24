/*
 * Zeitlos -- the screen toolkit's boxes. See sui.h and
 * docs/screen_ui.md.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Pure arcade runtime: no Zeitlos headers beyond zarcade.h and the
 * font, so the host tests (tests/test_sui.c) run the same code on
 * tests/ar_host.c's software framebuffer.
 *
 * One kind of box does everything: a title and a rule, then any of
 * text (wrapped, with tab columns, scrolling), a folder's list, a line
 * to type into, and a row of buttons. What a call asks for decides
 * which parts it has.
 */

#include <string.h>

#include "sui.h"
#include "../zarcade.h"
#include "../../zpaths.h"

extern const z_font_t z_font_5x8;

/* ===================================================================
 * metrics
 * =================================================================== */

#define FONT        (&z_font_5x8)
#define CW          5             /* a character's advance */
#define ROW         9             /* a text row */
#define EDGE        2             /* the black ring and the white frame */
#define PAD         4             /* inside the frame */
#define BTN_H       13
#define BTN_MINW    40
#define BTN_GAP     6
#define FIELD_H     13
#define FIELD_MINW  180
#define SB_W        7
#define SB_GAP      3
#define TEXT_MAXW   280           /* the widest a line of text gets */
#define WIDE_W      288           /* help and file boxes: always this wide */
#define BOX_MINW    120
#define MAX_ROWS    10            /* a message's rows before it scrolls */
#define HELP_ROWS   19            /* help's */
#define TAB_COL     17            /* where '\t' goes, in characters */
#define MAX_SEGS    256
#define LIST_ROWS   12
#define LIST_MAX    192           /* entries in a folder's list */
#define LIST_POOL   3072          /* their names */
#define PATH_MAX    256
#define FIELD_MAX   128
#define DOUBLE_TICKS 24           /* a second click within this: open */

#define FOCUS_LIST  (-2)
#define FOCUS_FIELD (-1)

/* zkbd.h's keys, spelled numerically: this file has no Zeitlos headers */
#define K_UP        0x110000u
#define K_DOWN      0x110001u
#define K_LEFT      0x110002u
#define K_RIGHT     0x110003u
#define K_HOME      0x110004u
#define K_END       0x110005u
#define K_PGUP      0x110006u
#define K_PGDN      0x110007u
#define K_DELETE    0x110009u
#define K_F1        0x110010u
#define K_ESC       0x1bu
#define K_TAB       '\t'
#define K_BS        8u
#define MOD_SHIFT   0x22u

/* ===================================================================
 * colours
 * =================================================================== */

const uint16_t sui_rgb[SUI_NC] = {
	0x000,   /* face */
	0xfff,   /* text */
	0xbbc,   /* frame */
	0xfd2,   /* the focused button */
	0x000,   /* its label */
	0xf54,   /* a warning */
	0x334,   /* trough */
	0x99a,   /* thumb */
};

/* Monochrome: the same look in every program, whatever its inks. */
static const ar_ink_t sui_inks[16] = {
	{{ 0, 0, 0, 0, 0, 0, 0, 0 }},                                  /* face */
	{{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }},          /* text */
	{{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }},          /* frame */
	{{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }},          /* sel */
	{{ 0, 0, 0, 0, 0, 0, 0, 0 }},                                  /* sel text */
	{{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }},          /* warn */
	{{ 0x88, 0x22, 0x88, 0x22, 0x88, 0x22, 0x88, 0x22 }},          /* trough */
	{{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }},          /* thumb */
};

static uint8_t pal_idx[SUI_NC];
static bool pal_custom;            /* the program named its entries */
static bool pal_dim = true;

void sui_palette(const uint8_t *idx, bool dim) {
	if (!idx) {
		pal_custom = false;
		pal_dim = true;
		return;
	}
	memcpy(pal_idx, idx, sizeof(pal_idx));
	pal_custom = true;
	pal_dim = dim;
}

/* The index a role is drawn in, now: the role itself in monochrome
 * (the box's inks, ar_top), its palette entry in colour. */
static int C(int role) {
	return ar_color() ? pal_idx[role] : role;
}

/* What the box did to the palette, to be undone. */
static uint16_t pal_saved[256];
static int pal_saved_n;            /* 0: nothing changed */

static bool is_ui_entry(int i) {
	int r;
	for (r = 0; r < SUI_NC; r++) if (pal_idx[r] == i) return true;
	return false;
}

static int lum(uint16_t c) {
	return ((c >> 8) & 15) * 3 + ((c >> 4) & 15) * 6 + (c & 15);
}

/* In colour, by default: in 16 colours the art has every entry, so the
 * boxes draw in its darkest and lightest (pix's rule) and nothing is
 * written or dimmed; in 64 or 256 they take the top eight, set to the
 * toolkit's colours, and the rest is dimmed to half. A program's own
 * entries (sui_palette()) are used as they are, the rest dimmed if it
 * asked. Once per box (again after a mode change into colour); all of
 * it undone by colours_off(). */
static void colours_on(void) {
	int i, n, d = 0, l = 0;
	bool write = false, dim = pal_dim;
	if (!ar_color() || pal_saved_n) return;
	n = ar_colours();
	if (!pal_custom && n <= 16) {
		for (i = 1; i < n; i++) {
			if (lum(ar_pal_get(i)) < lum(ar_pal_get(d))) d = i;
			if (lum(ar_pal_get(i)) > lum(ar_pal_get(l))) l = i;
		}
		pal_idx[SUI_C_FACE] = pal_idx[SUI_C_SEL_TEXT] = (uint8_t)d;
		pal_idx[SUI_C_TROUGH] = (uint8_t)d;
		pal_idx[SUI_C_TEXT] = pal_idx[SUI_C_FRAME] = (uint8_t)l;
		pal_idx[SUI_C_SEL] = pal_idx[SUI_C_WARN] = (uint8_t)l;
		pal_idx[SUI_C_THUMB] = (uint8_t)l;
		return;
	}
	if (!pal_custom) {
		for (i = 0; i < SUI_NC; i++) pal_idx[i] = (uint8_t)(n - SUI_NC + i);
		write = true;
		dim = true;
	}
	if (!write && !dim) return;
	for (i = 0; i < n; i++) pal_saved[i] = ar_pal_get(i);
	pal_saved_n = n;
	if (dim)
		for (i = 0; i < n; i++)
			if (!is_ui_entry(i)) ar_pal(i, ar_rgb_mix(pal_saved[i], 0x000, 128));
	if (write)
		for (i = 0; i < SUI_NC; i++) ar_pal(pal_idx[i], sui_rgb[i]);
}

static void colours_off(void) {
	int i;
	for (i = 0; i < pal_saved_n; i++) ar_pal(i, pal_saved[i]);
	pal_saved_n = 0;
}

/* ===================================================================
 * the box
 * =================================================================== */

enum { KIND_PLAIN = 0, KIND_HELP, KIND_OPEN, KIND_SAVE };

typedef struct {
	uint16_t off;           /* into the text */
	uint8_t len, col;       /* characters, and the column it starts at */
	uint16_t row;
} seg_t;

typedef struct {
	int kind;
	const char *title;
	bool warn;
	const char *text;

	/* the text, wrapped: segments on rows (two on a row where a tab
	 * puts a key and its meaning side by side) */
	seg_t seg[MAX_SEGS];
	int nseg, nrows, rows, first, max_rows;
	bool scroll;

	/* buttons */
	int nbtn;
	const char *label[3];
	int value[3];
	int esc_value;
	int focus;                    /* a button, FOCUS_FIELD, FOCUS_LIST */

	/* the line */
	bool field, secret, empty_ok;
	const char *field_label;
	char fbuf[FIELD_MAX];
	int flen, fcur, fscroll;

	/* the list (file boxes) */
	bool list;
	int lcount, ltop, lsel;       /* rows, ".." included */

	/* layout */
	ar_rect_t r;
	int ix, iy, iw;
	int title_y, rule_y, text_y, path_y, list_y, field_y, field_x, field_w;
	int btn_y;
	int bx[3], bw[3];
	int text_w;                   /* what a text row may use */
	int field_cols;
	bool layout_ok;

	/* the mouse */
	bool mouse_was;
	int pressed;                  /* the button pressed on, or -1 */
	int drag;                     /* 0, or 1 the text's thumb, 2 the list's */
	int drag_off;
	int last_click_row;
	uint32_t last_click_tick;

	bool dirty, done;
	int result;
} box_t;

static box_t box;
static bool last_ok;
static ar_rect_t last_r;

/* The folder's list, only in a file box: reached through these, so a
 * program that opens no file box (a game with F1 help) links none of
 * it -- its code, nor its 4 KB of names (--gc-sections). */
typedef struct {
	void (*draw)(const box_t *b);
	void (*say)(const box_t *b);
	void (*key)(box_t *b, uint32_t k);
	void (*click)(box_t *b, int x, int y);
	void (*ok)(box_t *b);
} list_ops_t;
static const list_ops_t *lops;

bool sui_last_layout_ok(int *x, int *y, int *w, int *h) {
	if (x) *x = last_r.x;
	if (y) *y = last_r.y;
	if (w) *w = last_r.w;
	if (h) *h = last_r.h;
	return last_ok;
}

static int tw(const char *s) { return (int)strlen(s) * CW; }

/* ===================================================================
 * wrapping
 * =================================================================== */

static void seg_add(box_t *b, int off, int len, int col, int row) {
	seg_t *s;
	if (b->nseg >= MAX_SEGS) return;
	s = &b->seg[b->nseg++];
	s->off = (uint16_t)off;
	s->len = (uint8_t)len;
	s->col = (uint8_t)col;
	s->row = (uint16_t)row;
}

/* Text from t[i] up to `end`, wrapped to `cols` characters from column
 * `col`, starting on row *row; *row ends on the row after the last. */
static void wrap_run(box_t *b, const char *t, int i, int end, int col,
	int cols, int *row) {
	if (i >= end) {
		(*row)++;
		return;
	}
	while (i < end) {
		int start = i, n = 0, brk = -1, stop;
		while (i < end && n < cols) {
			if (t[i] == ' ') brk = i;
			i++;
			n++;
		}
		stop = i;
		if (i < end) {
			/* too long: back to the last space, if there was one,
			 * unless the next character is itself a space */
			if (t[i] == ' ') i++;
			else if (brk > start) { stop = brk; i = brk + 1; }
		}
		seg_add(b, start, stop - start, col, *row);
		(*row)++;
	}
}

/* Word-wrap the text into segments, `cols` characters wide. */
static void wrap(box_t *b, int cols) {
	const char *t = b->text ? b->text : "";
	int i = 0, row = 0;
	b->nseg = 0;
	if (cols < TAB_COL + 8) cols = TAB_COL + 8;
	while (t[i]) {
		int end = i, tab = -1;
		while (t[end] && t[end] != '\n') {
			if (t[end] == '\t' && tab < 0) tab = end;
			end++;
		}
		if (tab >= 0) {
			/* "key\tmeaning": the key at column 0, the meaning at
			 * TAB_COL, wrapped there; a long key takes its own row */
			int krow = row;
			wrap_run(b, t, i, tab, 0, TAB_COL - 1, &krow);
			if (tab - i >= TAB_COL) row = krow;
			wrap_run(b, t, tab + 1, end, TAB_COL, cols - TAB_COL, &row);
			if (krow > row) row = krow;
		} else {
			wrap_run(b, t, i, end, 0, cols, &row);
		}
		i = end;
		if (t[i] == '\n') i++;
	}
	b->nrows = row;
}

static int longest(const box_t *b) {
	int i, m = 0;
	for (i = 0; i < b->nseg; i++) {
		int w = (b->seg[i].col + b->seg[i].len) * CW;
		if (w > m) m = w;
	}
	return m;
}

/* ===================================================================
 * the folder's list
 * =================================================================== */

static char path[PATH_MAX];
static char pool[LIST_POOL];
static uint16_t name_off[LIST_MAX];
static uint8_t name_dir[LIST_MAX];
static uint8_t order[LIST_MAX];
static int nnames;

static bool at_root(void) { return path[0] == '/' && path[1] == 0; }

static int ci(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int name_cmp(int a, int b) {
	const char *p = pool + name_off[a], *q = pool + name_off[b];
	if (name_dir[a] != name_dir[b]) return name_dir[a] ? -1 : 1;
	while (*p && ci((uint8_t)*p) == ci((uint8_t)*q)) p++, q++;
	return ci((uint8_t)*p) - ci((uint8_t)*q);
}

/* Row r of the list: -1 for "..", else the index of a name. */
static int row_name(int r) {
	if (!at_root()) {
		if (r == 0) return -1;
		r--;
	}
	return order[r];
}

/* False if the folder could not be listed (it shows empty). */
static bool list_load(box_t *b, const char *select) {
	int i, j;
	bool trunc, ok;
	nnames = ar_dir_list(path, pool, LIST_POOL, name_dir, LIST_MAX, &trunc);
	ok = nnames >= 0;
	if (nnames < 0) nnames = 0;
	for (i = 0, j = 0; i < nnames; i++) {
		name_off[i] = (uint16_t)j;
		j += (int)strlen(pool + j) + 1;
		order[i] = (uint8_t)i;
	}
	/* insertion sort: a few hundred at most */
	for (i = 1; i < nnames; i++) {
		uint8_t v = order[i];
		j = i - 1;
		while (j >= 0 && name_cmp(order[j], v) > 0) {
			order[j + 1] = order[j];
			j--;
		}
		order[j + 1] = v;
	}
	b->lcount = nnames + (at_root() ? 0 : 1);
	b->ltop = 0;
	b->lsel = 0;
	if (select)
		for (i = 0; i < b->lcount; i++) {
			int n = row_name(i);
			if (n >= 0 && !strcmp(pool + name_off[n], select)) {
				b->lsel = i;
				break;
			}
		}
	if (b->lsel >= LIST_ROWS) b->ltop = b->lsel - LIST_ROWS / 2;
	if (b->ltop > b->lcount - LIST_ROWS) b->ltop = b->lcount - LIST_ROWS;
	if (b->ltop < 0) b->ltop = 0;
	b->dirty = true;
	return ok;
}

static void path_join(char *out, int cap, const char *dir, const char *name) {
	int n = (int)strlen(dir), m = (int)strlen(name);
	bool slash = !(n == 1 && dir[0] == '/');
	if (n + (slash ? 1 : 0) + m + 1 > cap) {
		out[0] = 0;
		return;
	}
	memcpy(out, dir, (size_t)n);
	if (slash) out[n++] = '/';
	memcpy(out + n, name, (size_t)m + 1);
}

static void go_up(box_t *b) {
	char was[PATH_MAX];
	char *s;
	if (at_root()) return;
	s = strrchr(path, '/');
	strcpy(was, s + 1);
	if (s == path) s[1] = 0;
	else *s = 0;
	list_load(b, was);
}

static void go_into(box_t *b, const char *name) {
	char p[PATH_MAX];
	path_join(p, sizeof p, path, name);
	if (!p[0]) return;
	strcpy(path, p);
	list_load(b, NULL);
}

/* ===================================================================
 * layout
 * =================================================================== */

static void layout(box_t *b) {
	int i, inner_w, inner_h, btns_w = 0, w, h, x, max_text_w;
	bool wide = b->kind != KIND_PLAIN;

	b->layout_ok = true;
	b->max_rows = b->kind == KIND_HELP ? HELP_ROWS : MAX_ROWS;
	max_text_w = wide ? WIDE_W : TEXT_MAXW;

	wrap(b, max_text_w / CW);
	b->scroll = b->nrows > b->max_rows;
	if (b->scroll) wrap(b, (max_text_w - SB_W - SB_GAP) / CW);
	b->rows = b->nrows < b->max_rows ? b->nrows : b->max_rows;
	b->first = 0;

	for (i = 0; i < b->nbtn; i++) {
		b->bw[i] = tw(b->label[i]) + 12;
		if (b->bw[i] < BTN_MINW) b->bw[i] = BTN_MINW;
		btns_w += b->bw[i] + (i ? BTN_GAP : 0);
	}

	inner_w = longest(b) + (b->scroll ? SB_W + SB_GAP : 0);
	if (wide) inner_w = WIDE_W;
	if (b->title && tw(b->title) > inner_w) inner_w = tw(b->title);
	if (btns_w > inner_w) inner_w = btns_w;
	if (b->field && inner_w < FIELD_MINW) inner_w = FIELD_MINW;
	if (inner_w < BOX_MINW - 2 * (EDGE + PAD))
		inner_w = BOX_MINW - 2 * (EDGE + PAD);
	if (inner_w > max_text_w) {
		/* only a title can do this: it is cut, and that is a fault */
		inner_w = max_text_w;
		b->layout_ok = false;
	}

	inner_h = ROW + 4;                              /* title and rule */
	if (b->rows) inner_h += b->rows * ROW + PAD;
	if (b->list) inner_h += ROW + LIST_ROWS * ROW + 2 + PAD;
	if (b->field) inner_h += FIELD_H + PAD;
	inner_h += BTN_H;

	w = inner_w + 2 * (EDGE + PAD);
	h = inner_h + 2 * (EDGE + PAD);
	b->r.x = (int16_t)((AR_W - w) / 2);
	b->r.y = (int16_t)((AR_H - h) / 2);
	b->r.w = (int16_t)w;
	b->r.h = (int16_t)h;
	if (b->r.x < 0 || b->r.y < 0 || w > AR_W || h > AR_H)
		b->layout_ok = false;

	b->ix = b->r.x + EDGE + PAD;
	b->iy = b->r.y + EDGE + PAD;
	b->iw = inner_w;
	b->title_y = b->iy;
	b->rule_y = b->iy + ROW + 1;
	b->text_y = b->iy + ROW + 4;
	b->text_w = inner_w - (b->scroll ? SB_W + SB_GAP : 0);
	b->path_y = b->text_y + (b->rows ? b->rows * ROW + PAD : 0);
	b->list_y = b->path_y + ROW;
	b->field_y = b->list ? b->list_y + LIST_ROWS * ROW + 2 + PAD : b->path_y;
	b->field_x = b->ix + (b->field_label ? tw(b->field_label) + 4 : 0);
	b->field_w = b->ix + inner_w - b->field_x;
	b->btn_y = b->field_y + (b->field ? FIELD_H + PAD : 0);
	b->field_cols = (b->field_w - 4) / CW;

	x = b->ix + inner_w - btns_w;
	for (i = 0; i < b->nbtn; i++) {
		b->bx[i] = x;
		x += b->bw[i] + BTN_GAP;
	}

	/* every segment within its row */
	if (longest(b) > b->text_w) b->layout_ok = false;
}

/* ===================================================================
 * drawing (ar_top's paint: everything is clipped to the dirty part)
 * =================================================================== */

static void outline(int x, int y, int w, int h, int c, int t) {
	ar_fill(x, y, w, t, c);
	ar_fill(x, y + h - t, w, t, c);
	ar_fill(x, y, t, h, c);
	ar_fill(x + w - t, y, t, h, c);
}

/* Text of at most `n` characters from `s`, cut to `maxw` pixels. */
static void text_n(int x, int y, const char *s, int n, int maxw, int c) {
	char tmp[WIDE_W / CW + 2];
	int fit = maxw / CW;
	if (n > fit) n = fit;
	if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1;
	if (n <= 0) return;
	memcpy(tmp, s, (size_t)n);
	tmp[n] = 0;
	ar_text(x, y, tmp, c, FONT, 1);
}

static void draw_field(const box_t *b) {
	int x = b->field_x, y = b->field_y, w = b->field_w;
	int n = b->flen - b->fscroll, i;
	char shown[FIELD_MAX];

	if (b->field_label)
		ar_text(b->ix, y + 3, b->field_label, C(SUI_C_TEXT), FONT, 1);
	outline(x, y, w, FIELD_H, C(SUI_C_FRAME), 1);
	if (n > b->field_cols) n = b->field_cols;
	for (i = 0; i < n; i++)
		shown[i] = b->secret ? '*' : b->fbuf[b->fscroll + i];
	text_n(x + 2, y + 3, shown, n, w - 4, C(SUI_C_TEXT));
	if (b->focus == FOCUS_FIELD) {
		int cx = x + 2 + (b->fcur - b->fscroll) * CW - 1;
		if (cx < x + 1) cx = x + 1;
		ar_fill(cx, y + 2, 1, FIELD_H - 4, C(SUI_C_TEXT));
	}
}

/* The button Enter takes while a line or a list has focus. */
static bool is_default(const box_t *b, int i) {
	return b->focus < 0 && i == 0;
}

static void draw_button(const box_t *b, int i) {
	int x = b->bx[i], y = b->btn_y, w = b->bw[i];
	int tx = x + (w - tw(b->label[i])) / 2;
	if (b->focus == i) {
		ar_fill(x, y, w, BTN_H, C(SUI_C_SEL));
		ar_text(tx, y + 3, b->label[i], C(SUI_C_SEL_TEXT), FONT, 1);
	} else {
		outline(x, y, w, BTN_H, C(SUI_C_FRAME), is_default(b, i) ? 2 : 1);
		ar_text(tx, y + 3, b->label[i], C(SUI_C_TEXT), FONT, 1);
	}
}

static void thumb_geom(int sb_y, int sb_h, int total, int vis, int first,
	int *ty, int *th) {
	int span = total - vis;
	*th = total > 0 ? sb_h * vis / total : sb_h;
	if (*th < 6) *th = 6;
	if (*th > sb_h) *th = sb_h;
	*ty = sb_y + (span > 0 ? (sb_h - *th) * first / span : 0);
}

static void draw_scrollbar(int x, int y, int h, int total, int vis,
	int first) {
	int ty, th;
	ar_fill(x, y, SB_W, h, C(SUI_C_TROUGH));
	thumb_geom(y, h, total, vis, first, &ty, &th);
	ar_fill(x + 1, ty, SB_W - 2, th, C(SUI_C_THUMB));
}

static void draw_list(const box_t *b) {
	int x = b->ix, y = b->list_y, w = b->iw, h = LIST_ROWS * ROW + 2;
	int rw = w - 2 - SB_W, i;
	char row[WIDE_W / CW + 2];

	/* the folder, its left end cut if it is long */
	{
		int n = (int)strlen(path), fit = w / CW;
		const char *p = path;
		if (n > fit) {
			p = path + n - (fit - 3);
			memcpy(row, "...", 3);
			strcpy(row + 3, p);
			p = row;
		}
		ar_text(x, b->path_y, p, C(SUI_C_TEXT), FONT, 1);
	}

	outline(x, y, w, h, C(SUI_C_FRAME), 1);
	for (i = 0; i < LIST_ROWS; i++) {
		int r = b->ltop + i, n, len, ry = y + 1 + i * ROW;
		bool sel = r == b->lsel;
		int tc = C(SUI_C_TEXT);
		if (r >= b->lcount) break;
		n = row_name(r);
		if (n < 0) {
			strcpy(row, "..");
		} else {
			const char *nm = pool + name_off[n];
			len = (int)strlen(nm);
			if (len > (int)sizeof(row) - 2) len = (int)sizeof(row) - 2;
			memcpy(row, nm, (size_t)len);
			row[len] = 0;
			if (name_dir[n]) strcat(row, "/");
		}
		if (sel && b->focus == FOCUS_LIST) {
			ar_fill(x + 1, ry, rw, ROW, C(SUI_C_SEL));
			tc = C(SUI_C_SEL_TEXT);
		} else if (sel) {
			outline(x + 1, ry, rw, ROW, C(SUI_C_FRAME), 1);
		}
		text_n(x + 3, ry + 1, row, (int)strlen(row), rw - 4, tc);
	}
	draw_scrollbar(x + w - 1 - SB_W, y + 1, h - 2, b->lcount, LIST_ROWS,
		b->ltop);
}

static void paint(void *user, const ar_rect_t *clip) {
	const box_t *b = (const box_t *)user;
	const char *t = b->text ? b->text : "";
	int i;
	(void)clip;

	ar_fill(b->r.x, b->r.y, b->r.w, b->r.h, C(SUI_C_FACE));
	outline(b->r.x + 1, b->r.y + 1, b->r.w - 2, b->r.h - 2,
		C(SUI_C_FRAME), 1);

	if (b->title)
		text_n(b->ix, b->title_y, b->title, (int)strlen(b->title), b->iw,
			C(b->warn ? SUI_C_WARN : SUI_C_TEXT));
	ar_fill(b->ix, b->rule_y, b->iw, 1, C(SUI_C_FRAME));

	for (i = 0; i < b->nseg; i++) {
		const seg_t *s = &b->seg[i];
		int r = (int)s->row - b->first;
		if (r < 0 || r >= b->rows) continue;
		text_n(b->ix + s->col * CW, b->text_y + r * ROW, t + s->off,
			s->len, b->text_w - s->col * CW, C(SUI_C_TEXT));
	}
	if (b->scroll)
		draw_scrollbar(b->ix + b->iw - SB_W, b->text_y, b->rows * ROW,
			b->nrows, b->rows, b->first);
	if (b->list) lops->draw(b);
	if (b->field) draw_field(b);
	for (i = 0; i < b->nbtn; i++) draw_button(b, i);
}

/* ===================================================================
 * input
 * =================================================================== */

static void say_row(const box_t *b) {
	int n = row_name(b->lsel);
	if (b->lsel >= b->lcount) return;
	ar_say(n < 0 ? "up a folder" : pool + name_off[n]);
}

static void say_focus(const box_t *b) {
	if (b->focus >= 0) ar_say(b->label[b->focus]);
	else if (b->focus == FOCUS_LIST) lops->say(b);
	else ar_say(b->secret ? "password" : b->field_label ? "name" : "text");
}

static void set_focus(box_t *b, int f) {
	if (f == b->focus) return;
	b->focus = f;
	b->dirty = true;
	say_focus(b);
}

static void answer(box_t *b, int value) {
	b->result = value;
	b->done = true;
}

static void scroll_to(box_t *b, int first) {
	int max = b->nrows - b->rows;
	if (!b->scroll) return;
	if (first > max) first = max;
	if (first < 0) first = 0;
	if (first != b->first) {
		b->first = first;
		b->dirty = true;
	}
}

static void list_top(box_t *b, int top) {
	int max = b->lcount - LIST_ROWS;
	if (top > max) top = max;
	if (top < 0) top = 0;
	if (top != b->ltop) {
		b->ltop = top;
		b->dirty = true;
	}
}

static void list_select(box_t *b, int r) {
	if (r >= b->lcount) r = b->lcount - 1;
	if (r < 0) r = 0;
	if (r == b->lsel) return;
	b->lsel = r;
	if (r < b->ltop) list_top(b, r);
	if (r >= b->ltop + LIST_ROWS) list_top(b, r - LIST_ROWS + 1);
	b->dirty = true;
	say_row(b);
}

static void field_show_caret(box_t *b) {
	if (b->fcur < b->fscroll) b->fscroll = b->fcur;
	if (b->field_cols > 0 && b->fcur > b->fscroll + b->field_cols - 1)
		b->fscroll = b->fcur - b->field_cols + 1;
	if (b->fscroll < 0) b->fscroll = 0;
}

static void field_set(box_t *b, const char *s) {
	size_t n = s ? strlen(s) : 0;
	if (n > FIELD_MAX - 1) n = FIELD_MAX - 1;
	memcpy(b->fbuf, s ? s : "", n);
	b->fbuf[n] = 0;
	b->flen = b->fcur = (int)n;
	b->fscroll = 0;
	field_show_caret(b);
	b->dirty = true;
}

static bool field_char_ok(const box_t *b, uint32_t k) {
	if (k >= 0x20 && k <= 0x7e) return !(b->list && k == '/');
	return !b->secret && k >= 0xa0 && k <= 0xff;
}

static bool field_key(box_t *b, uint32_t k) {
	switch (k) {
	case K_LEFT:  if (b->fcur > 0) b->fcur--; break;
	case K_RIGHT: if (b->fcur < b->flen) b->fcur++; break;
	case K_HOME:  b->fcur = 0; break;
	case K_END:   b->fcur = b->flen; break;
	case K_BS:
		if (b->fcur == 0) return true;
		memmove(b->fbuf + b->fcur - 1, b->fbuf + b->fcur,
			(size_t)(b->flen - b->fcur));
		b->fcur--;
		b->flen--;
		break;
	case K_DELETE:
		if (b->fcur == b->flen) return true;
		memmove(b->fbuf + b->fcur, b->fbuf + b->fcur + 1,
			(size_t)(b->flen - b->fcur - 1));
		b->flen--;
		break;
	default:
		if (!field_char_ok(b, k)) return false;
		if (b->flen >= FIELD_MAX - 1) return true;
		memmove(b->fbuf + b->fcur + 1, b->fbuf + b->fcur,
			(size_t)(b->flen - b->fcur));
		b->fbuf[b->fcur++] = (char)k;
		b->flen++;
		break;
	}
	b->fbuf[b->flen] = 0;
	field_show_caret(b);
	b->dirty = true;
	return true;
}

/* A file box's OK: on a folder, into it; on a file (or a typed name),
 * the answer. */
static void file_ok(box_t *b) {
	int n = b->lsel < b->lcount ? row_name(b->lsel) : -2;
	if (b->kind == KIND_OPEN || b->focus == FOCUS_LIST) {
		if (n == -1) { go_up(b); return; }
		if (n >= 0 && name_dir[n]) { go_into(b, pool + name_off[n]); return; }
		if (b->kind == KIND_SAVE && n >= 0) {
			/* a file picked: its name to the line */
			field_set(b, pool + name_off[n]);
			set_focus(b, FOCUS_FIELD);
			return;
		}
		if (n < 0) return;
	} else if (b->flen == 0) {
		return;
	}
	answer(b, SUI_YES);
}

/* The next name after the selection starting with letter k. */
static void list_jump(box_t *b, uint32_t k) {
	int i;
	for (i = 1; i <= b->lcount; i++) {
		int r = (b->lsel + i) % b->lcount, n = row_name(r);
		if (n >= 0 && ci((uint8_t)pool[name_off[n]]) == ci((int)k)) {
			list_select(b, r);
			return;
		}
	}
}

/* The button whose label starts with a letter, for Y N O C. */
static int button_for(const box_t *b, uint32_t k) {
	int i;
	for (i = 0; i < b->nbtn; i++)
		if (ci((uint8_t)b->label[i][0]) == ci((int)k)) return i;
	return -1;
}

/* Focus order: the list, the line, then the buttons. */
static int focus_step(const box_t *b, int f, int d) {
	int lo = b->list ? FOCUS_LIST : b->field ? FOCUS_FIELD : 0;
	do {
		f += d;
		if (f < lo) f = b->nbtn - 1;
		if (f > b->nbtn - 1) f = lo;
	} while ((f == FOCUS_FIELD && !b->field) || (f == FOCUS_LIST && !b->list));
	return f;
}

static void key(box_t *b, uint32_t k, uint8_t mods) {
	bool shift = (mods & MOD_SHIFT) != 0;

	if (k == K_ESC || (k == K_F1 && b->kind == KIND_HELP)) {
		answer(b, b->esc_value);
		return;
	}
	if (k == K_TAB) {
		set_focus(b, focus_step(b, b->focus, shift ? -1 : 1));
		return;
	}

	if (b->focus == FOCUS_FIELD) {
		if (k == '\r' || k == '\n') {
			if (b->list) lops->ok(b);
			else answer(b, b->value[0]);
			return;
		}
		field_key(b, k);
		return;
	}

	if (b->focus == FOCUS_LIST) {
		lops->key(b, k);
		return;
	}

	switch (k) {
	case '\r': case '\n': case ' ':
		if (b->list && b->value[b->focus] == SUI_YES) lops->ok(b);
		else answer(b, b->value[b->focus]);
		return;
	case K_LEFT:
		if (b->focus > 0) set_focus(b, b->focus - 1);
		else if (b->field) set_focus(b, FOCUS_FIELD);
		return;
	case K_RIGHT:
		if (b->focus < b->nbtn - 1) set_focus(b, b->focus + 1);
		return;
	case K_UP:   scroll_to(b, b->first - 1); return;
	case K_DOWN: scroll_to(b, b->first + 1); return;
	case K_PGUP: scroll_to(b, b->first - (b->rows - 1)); return;
	case K_PGDN: scroll_to(b, b->first + (b->rows - 1)); return;
	case K_HOME: scroll_to(b, 0); return;
	case K_END:  scroll_to(b, b->nrows); return;
	default:
		break;
	}

	if (b->field && field_char_ok(b, k)) {
		/* typing on a button goes back to the line */
		set_focus(b, FOCUS_FIELD);
		field_key(b, k);
		return;
	}
	if (!b->list) {
		int i = button_for(b, k);
		if (i >= 0) answer(b, b->value[i]);
	}
}

static bool inside(int x, int y, int rx, int ry, int rw, int rh) {
	return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

static int button_at(const box_t *b, int x, int y) {
	int i;
	for (i = 0; i < b->nbtn; i++)
		if (inside(x, y, b->bx[i], b->btn_y, b->bw[i], BTN_H)) return i;
	return -1;
}

/* the scrollbars: the text's, and the list's */
static void sb_geom(const box_t *b, int which, int *x, int *y, int *h,
	int *total, int *vis, int *first) {
	if (which == 1) {
		*x = b->ix + b->iw - SB_W; *y = b->text_y; *h = b->rows * ROW;
		*total = b->nrows; *vis = b->rows; *first = b->first;
	} else {
		*x = b->ix + b->iw - 1 - SB_W; *y = b->list_y + 1;
		*h = LIST_ROWS * ROW; *total = b->lcount; *vis = LIST_ROWS;
		*first = b->ltop;
	}
}

static void sb_to(box_t *b, int which, int first) {
	if (which == 1) scroll_to(b, first);
	else list_top(b, first);
}

static bool sb_press(box_t *b, int which, int px, int py) {
	int x, y, h, total, vis, first, ty, th;
	sb_geom(b, which, &x, &y, &h, &total, &vis, &first);
	if (!inside(px, py, x, y, SB_W, h)) return false;
	thumb_geom(y, h, total, vis, first, &ty, &th);
	if (py < ty) sb_to(b, which, first - (vis - 1));
	else if (py >= ty + th) sb_to(b, which, first + (vis - 1));
	else {
		b->drag = which;
		b->drag_off = py - ty;
	}
	return true;
}

static void sb_drag(box_t *b, int py) {
	int x, y, h, total, vis, first, ty, th, room;
	sb_geom(b, b->drag, &x, &y, &h, &total, &vis, &first);
	thumb_geom(y, h, total, vis, first, &ty, &th);
	room = h - th;
	if (room <= 0) return;
	sb_to(b, b->drag, ((py - b->drag_off - y) * (total - vis) + room / 2) / room);
}

static void mouse(box_t *b, const ar_event_t *e) {
	bool down = (e->buttons & 1) != 0;
	int x = e->x, y = e->y;

	if (down && !b->mouse_was) {
		int i = button_at(b, x, y);
		if (i >= 0) {
			b->pressed = i;
			set_focus(b, i);
		} else if (b->field &&
			inside(x, y, b->field_x, b->field_y, b->field_w, FIELD_H)) {
			int col = (x - b->field_x - 2 + CW / 2) / CW + b->fscroll;
			if (col > b->flen) col = b->flen;
			if (col < 0) col = 0;
			b->fcur = col;
			set_focus(b, FOCUS_FIELD);
			b->dirty = true;
		} else if (b->scroll && sb_press(b, 1, x, y)) {
		} else if (b->list && sb_press(b, 2, x, y)) {
		} else if (b->list && inside(x, y, b->ix, b->list_y + 1, b->iw,
			LIST_ROWS * ROW)) {
			lops->click(b, x, y);
		}
	} else if (down && b->drag) {
		sb_drag(b, y);
	} else if (!down && b->mouse_was) {
		if (b->pressed >= 0 && button_at(b, x, y) == b->pressed) {
			if (b->list && b->value[b->pressed] == SUI_YES) lops->ok(b);
			else answer(b, b->value[b->pressed]);
		}
		b->pressed = -1;
		b->drag = 0;
	}
	b->mouse_was = down;
}

static void wheel(box_t *b, int notches) {
	if (b->list) list_top(b, b->ltop - notches * 3);
	else scroll_to(b, b->first - notches * 3);
}

/* ===================================================================
 * running a box
 * =================================================================== */

static void speak_open(const box_t *b) {
	char s[160];
	size_t n = 0, m;
	if (b->title) {
		m = strlen(b->title);
		if (m > sizeof(s) - 3) m = sizeof(s) - 3;
		memcpy(s, b->title, m);
		n = m;
		s[n++] = '.';
		s[n++] = ' ';
	}
	if (b->text) {
		m = strlen(b->text);
		if (m > sizeof(s) - 1 - n) m = sizeof(s) - 1 - n;
		memcpy(s + n, b->text, m);
		n += m;
	}
	s[n] = 0;
	ar_say(s);
}

static int run(box_t *b) {
	ar_top_t top;
	ar_event_t e;
	bool mode_seen = false, quit_seen = false;

	layout(b);
	field_show_caret(b);
	last_ok = b->layout_ok;
	last_r = b->r;
	b->pressed = -1;
	b->last_click_row = -1;
	b->mouse_was = true;          /* a button already down opened us */
	b->done = false;
	b->dirty = false;

	colours_on();
	memset(&top, 0, sizeof(top));
	top.paint = paint;
	top.user = b;
	top.area = b->r;
	top.ink = sui_inks;
	top.opaque = true;
	ar_top(&top);
	speak_open(b);

	while (!b->done) {
		while (!b->done && ar_poll(&e)) {
			switch (e.type) {
			case AR_EV_KEY:
				if (e.pressed) key(b, e.key, e.mods);
				break;
			case AR_EV_MOUSE:
				mouse(b, &e);
				break;
			case AR_EV_WHEEL:
				wheel(b, e.notches);
				break;
			case AR_EV_MODE:
				mode_seen = true;
				colours_on();
				b->dirty = true;
				break;
			case AR_EV_QUIT:
				quit_seen = true;
				answer(b, b->esc_value);
				break;
			default:
				break;
			}
		}
		if (b->dirty) {
			ar_dirty(b->r.x, b->r.y, b->r.w, b->r.h);
			b->dirty = false;
		}
		if (!b->done) ar_frame();
	}

	ar_top(NULL);
	colours_off();
	if (mode_seen) {
		memset(&e, 0, sizeof(e));
		e.type = AR_EV_MODE;
		ar_push(&e);
	}
	if (quit_seen) {
		memset(&e, 0, sizeof(e));
		e.type = AR_EV_QUIT;
		ar_push(&e);
	}
	return b->result;
}

static void box_new(int kind, const char *title, const char *text) {
	memset(&box, 0, sizeof(box));
	box.kind = kind;
	box.title = title;
	box.text = text;
}

static void button(const char *label, int value) {
	if (box.nbtn >= 3) return;
	box.label[box.nbtn] = label;
	box.value[box.nbtn] = value;
	box.nbtn++;
}

/* ===================================================================
 * the calls
 * =================================================================== */

static void message(const char *title, const char *text, bool warn) {
	box_new(KIND_PLAIN, title, text);
	box.warn = warn;
	button("OK", SUI_YES);
	box.esc_value = SUI_YES;
	run(&box);
}

void sui_message(const char *title, const char *text) {
	message(title, text, false);
}

void sui_warn(const char *title, const char *text) {
	message(title, text, true);
}

int sui_confirm(const char *title, const char *text, int buttons) {
	box_new(KIND_PLAIN, title, text);
	switch (buttons) {
	case SUI_YES_NO:
		button("Yes", SUI_YES);
		button("No", SUI_NO);
		box.esc_value = SUI_NO;
		break;
	case SUI_YES_NO_CANCEL:
		button("Yes", SUI_YES);
		button("No", SUI_NO);
		button("Cancel", SUI_CANCEL);
		box.esc_value = SUI_CANCEL;
		break;
	default:
		button("OK", SUI_YES);
		button("Cancel", SUI_CANCEL);
		box.esc_value = SUI_CANCEL;
		break;
	}
	return run(&box);
}

static void wipe_field(void) {
	volatile char *p = box.fbuf;
	int i;
	for (i = 0; i < FIELD_MAX; i++) p[i] = 0;
	box.flen = box.fcur = 0;
}

static void copy_out(char *out, int outlen, const char *s, int n, bool ok) {
	if (!out || outlen <= 0) return;
	if (!ok) {
		out[0] = 0;
		return;
	}
	if (n > outlen - 1) n = outlen - 1;
	memcpy(out, s, (size_t)n);
	out[n] = 0;
}

static bool prompt(const char *title, const char *text, const char *initial,
	char *out, int outlen, bool empty_ok, bool secret) {
	bool ok;
	box_new(KIND_PLAIN, title, text);
	box.field = true;
	box.secret = secret;
	box.empty_ok = empty_ok;
	box.focus = FOCUS_FIELD;
	button("OK", SUI_YES);
	button("Cancel", SUI_CANCEL);
	box.esc_value = SUI_CANCEL;
	field_set(&box, secret ? NULL : initial);

	ok = run(&box) == SUI_YES && (box.flen > 0 || empty_ok);
	copy_out(out, outlen, box.fbuf, box.flen, ok);
	if (secret) wipe_field();
	return ok;
}

bool sui_prompt(const char *title, const char *text, const char *initial,
	char *out, int outlen) {
	return prompt(title, text, initial, out, outlen, false, false);
}

bool sui_prompt_empty_ok(const char *title, const char *text,
	const char *initial, char *out, int outlen) {
	return prompt(title, text, initial, out, outlen, true, false);
}

bool sui_prompt_secret(const char *title, const char *text, char *out,
	int outlen) {
	return prompt(title, text, NULL, out, outlen, false, true);
}

/* -- files -- */

static void list_key(box_t *b, uint32_t k) {
	switch (k) {
	case '\r': case '\n': file_ok(b); return;
	case K_UP:    list_select(b, b->lsel - 1); return;
	case K_DOWN:  list_select(b, b->lsel + 1); return;
	case K_PGUP:  list_select(b, b->lsel - (LIST_ROWS - 1)); return;
	case K_PGDN:  list_select(b, b->lsel + (LIST_ROWS - 1)); return;
	case K_HOME:  list_select(b, 0); return;
	case K_END:   list_select(b, b->lcount - 1); return;
	case K_BS:    go_up(b); return;
	default: break;
	}
	if (b->field && field_char_ok(b, k)) {
		set_focus(b, FOCUS_FIELD);
		field_key(b, k);
	} else if (k > ' ' && k < 0x7f) {
		list_jump(b, k);
	}
}

/* A row clicked; a second click on it within DOUBLE_TICKS opens it. */
static void list_click(box_t *b, int x, int y) {
	int r = b->ltop + (y - b->list_y - 1) / ROW;
	uint32_t now = ar_ticks();
	(void)x;
	if (r >= b->lcount) return;
	{
		bool again = r == b->last_click_row &&
			now - b->last_click_tick <= DOUBLE_TICKS;
		set_focus(b, FOCUS_LIST);
		list_select(b, r);
		b->last_click_row = r;
		b->last_click_tick = now;
		if (again) {
			b->last_click_row = -1;
			file_ok(b);
		}
	}
}

static const list_ops_t file_ops = {
	draw_list, say_row, list_key, list_click, file_ok
};

static bool file_box(int kind, const char *start_dir, const char *suggested,
	char *out, int outlen) {
	char res[PATH_MAX];
	bool ok;
	int n;

	box_new(kind, kind == KIND_OPEN ? "Open" : "Save As", NULL);
	lops = &file_ops;
	box.list = true;
	box.focus = FOCUS_LIST;
	if (kind == KIND_SAVE) {
		box.field = true;
		box.field_label = "Name";
		field_set(&box, suggested);
		box.focus = FOCUS_FIELD;
	}
	button(kind == KIND_OPEN ? "Open" : "Save", SUI_YES);
	button("Cancel", SUI_CANCEL);
	box.esc_value = SUI_CANCEL;

	n = start_dir ? (int)strlen(start_dir) : 0;
	if (n <= 0 || n >= PATH_MAX || start_dir[0] != '/') strcpy(path, Z_DIR_HOME);
	else {
		strcpy(path, start_dir);
		while (n > 1 && path[n - 1] == '/') path[--n] = 0;
	}
	if (!list_load(&box, NULL)) {
		strcpy(path, "/");
		list_load(&box, NULL);
	}

	ok = run(&box) == SUI_YES;
	res[0] = 0;
	if (ok) {
		if (kind == KIND_SAVE) {
			path_join(res, sizeof res, path, box.fbuf);
		} else {
			int r = row_name(box.lsel);
			if (r >= 0) path_join(res, sizeof res, path, pool + name_off[r]);
		}
		ok = res[0] != 0;
	}
	copy_out(out, outlen, res, (int)strlen(res), ok);
	return ok;
}

bool sui_open(const char *start_dir, char *out, int outlen) {
	return file_box(KIND_OPEN, start_dir, NULL, out, outlen);
}

bool sui_save(const char *start_dir, const char *suggested, char *out,
	int outlen) {
	return file_box(KIND_SAVE, start_dir, suggested, out, outlen);
}

/* -- help -- */

void sui_help(const char *title, const char *text) {
	box_new(KIND_HELP, title, text);
	button("Close", SUI_YES);
	box.esc_value = SUI_YES;
	run(&box);
}
