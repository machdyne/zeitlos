/*
 * Zeitlos mahjong -- the board. See mj_board.h.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */

#include <string.h>

#include "mj_board.h"

#define FONT (&z_font_5x8)

/* -- colours ------------------------------------------------------------------
 *
 * A shadow is ONE BIT: the darker variant of colour c is c | 8 in 16
 * colours and c | 32 in 64, so darkening a strip is an OR into one
 * plane -- a word at a time -- instead of reading every pixel back and
 * looking it up. The layout below is what makes that true: 8..15 are
 * 0..7 in shadow, and 32..63 are 0..31 in shadow. Shading twice is
 * shading once (OR), which is also what a second shadow over the first
 * should look like. */

const uint16_t mj_palette[MJ_C_COLOURS] = {
	0x253,      /*  0 BG      the felt */
	0x111,      /*  1 INK */
	0xc21,      /*  2 RED     (and the keyboard's frame) */
	0x183,      /*  3 GREEN */
	0x24b,      /*  4 BLUE */
	0xcb8,      /*  5 SIDE    bone, the long side */
	0xffe,      /*  6 FACE    ivory */
	0xfd6,      /*  7 SEL     gold, light enough to read the face on */
	0x142,      /*  8 BG2     the felt in shadow */
	0x321,      /*  9 EDGE    outlines; ink in shadow */
	0x811,      /* 10 RED2    red in shadow */
	0x131,      /* 11 BAR     green in shadow; the bar */
	0x139,      /* 12 BLUE2   blue in shadow */
	0x986,      /* 13 SIDE2   the short side; the long side in shadow */
	0xdca,      /* 14 FACE0   a face in shadow; a face's dark edge */
	0xb94,      /* 15 HINT    gold in shadow; pulses while a hint is up */

	/* 16..21 faces by layer, the table darkest; 22..27 the long side by
	 * layer; 28..31 the short side by layer (from 3 up, the same) */
	0xdc9, 0xeda, 0xeeb, 0xfec, 0xffd, 0xfff,
	0xa97, 0xba8, 0xbb9, 0xcb9, 0xcca, 0xddb,
	0x765, 0x876, 0x986, 0xa98,

	/* 32..63: 0..31 in shadow, three quarters as bright */
	0x242, 0x111, 0x921, 0x162, 0x238, 0x986, 0xbbb, 0xba5,
	0x132, 0x221, 0x611, 0x121, 0x127, 0x765, 0xa98, 0x873,
	0xa97, 0xba8, 0xbb8, 0xbb9, 0xbba, 0xbbb, 0x875, 0x886,
	0x887, 0x987, 0x998, 0xaa8, 0x554, 0x655, 0x765, 0x876,
};

/* Monochrome: the felt and every ink black, faces white, the sides
 * greys, the selection and the hint two different light textures so
 * the black picture on them still reads. MSB leftmost. A shadow in
 * black and white is not an index: it is mj_cv_shade()'s checkerboard. */
#define SOLID   { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }
#define BLACK   { 0, 0, 0, 0, 0, 0, 0, 0 }
#define GREY50  { 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55 }
#define GREY25  { 0x88, 0x00, 0x22, 0x00, 0x88, 0x00, 0x22, 0x00 }
#define GREY75  { 0x77, 0xff, 0xdd, 0xff, 0x77, 0xff, 0xdd, 0xff }
#define STRIPES { 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee }

const uint8_t mj_inks[16][8] = {
	BLACK,      /* BG */
	BLACK,      /* INK */
	BLACK,      /* RED */
	BLACK,      /* GREEN */
	BLACK,      /* BLUE */
	GREY50,     /* SIDE */
	SOLID,      /* FACE */
	GREY75,     /* SEL */
	BLACK,      /* BG2 */
	BLACK,      /* EDGE */
	BLACK,      /* RED2 */
	BLACK,      /* BAR */
	BLACK,      /* BLUE2 */
	GREY25,     /* SIDE2 */
	SOLID,      /* FACE0 */
	STRIPES,    /* HINT */
};

const uint8_t mj_ink_map[MJ_C_COLOURS] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
	6, 6, 6, 6, 6, 6,  5, 5, 5, 5, 5, 5,  13, 13, 13, 13,
	8, 9, 10, 11, 12, 13, 14, 15, 8, 9, 10, 11, 12, 13, 14, 15,
	14, 14, 14, 14, 14, 14, 13, 13, 13, 13, 13, 13, 9, 9, 9, 9
};

static int zc(int z) { return z > 5 ? 5 : z; }

static int c_face(const mj_view_t *v, int z)
{
	return v->rich ? MJ_C_FACE_Z + zc(z) : MJ_C_FACE;
}

static int c_side(const mj_view_t *v, int z)
{
	return v->rich ? MJ_C_SIDE_Z + zc(z) : MJ_C_SIDE;
}

static int c_side2(const mj_view_t *v, int z)
{
	return v->rich ? MJ_C_SIDE2_Z + (z > 3 ? 3 : z) : MJ_C_SIDE2;
}

/* A face's dark inner edge: the face in shadow. */
static int c_lowedge(const mj_view_t *v, int z)
{
	return v->rich ? (MJ_C_FACE_Z + zc(z)) | MJ_C_SHADE64 : MJ_C_FACE0;
}

static int c_light(const mj_view_t *v, int z)
{
	if (v->rich) return MJ_C_FACE_Z + (z + 2 > 5 ? 5 : z + 2);
	return MJ_C_FACE;
}

/* The bit a shadow sets on this canvas: 5 in 64 colours, 3 in 16, none
 * in 8 (the colour window on a six-plane board has no darker variant
 * of anything among its eight, so the stacks show by their sides). In
 * mono it is not a bit at all; mj_cv_shade() ignores it there. */
static int shade_bit(const mj_view_t *v, const mj_canvas_t *cv)
{
	if (v->rich && cv->planes >= 6) return 5;
	if (cv->planes >= 4) return 3;
	return cv->planes ? -1 : 0;
}

/* -- geometry ---------------------------------------------------------------- */

static int dir_x(const mj_view_t *v) { return (v->corner == 1 || v->corner == 2) ? -1 : 1; }
static int dir_y(const mj_view_t *v) { return v->corner >= 2 ? -1 : 1; }

/* A face's top-left in board space. */
static void face_board(const mj_view_t *v, int t, int *x, int *y)
{
	const mj_pos_t *p = &v->g->L->pos[t];
	*x = p->x * v->fw / 2 - dir_x(v) * p->z * v->d;
	*y = p->y * v->fh / 2 - dir_y(v) * p->z * v->d;
}

void mj_view_init(mj_view_t *v, const mj_game_t *g)
{
	memset(v, 0, sizeof(*v));
	v->g = g;
	v->size = MJ_ART_SMALL;
	v->sel = v->cursor = v->hint_a = v->hint_b = -1;
	v->buttons = (1 << MJ_B_NEW) | (1 << MJ_B_UNDO) | (1 << MJ_B_HINT) | (1 << MJ_B_SHUFFLE);
}

/* The place at exactly (x, y, z), or -1. A layout's places are in
 * canonical order -- layer, row, column (mj_layout.h) -- so a binary
 * search finds it. */
static int place_at(const mj_layout_t *L, int x, int y, int z)
{
	long want, lo = 0, hi = L->n - 1;
	if (x < 0 || y < 0 || z < 0) return -1;
	want = ((long)z << 16) | ((long)y << 8) | x;
	while (lo <= hi) {
		long mid = (lo + hi) / 2;
		const mj_pos_t *p = &L->pos[mid];
		long k = ((long)p->z << 16) | ((long)p->y << 8) | p->x;
		if (k == want) return (int)mid;
		if (k < want) lo = mid + 1;
		else hi = mid - 1;
	}
	return -1;
}

/* Which places hide which (mj_view_t.cover). A tile with a tile exactly
 * on top of it has its face covered: the upper one is drawn a side's
 * thickness towards the viewer, and its sides land exactly on the lower
 * face. Its own sides and its shadow lie under the faces of its
 * neighbours beside, below and diagonally on the sides' side, which are
 * drawn after it. With all four there, nothing it would draw survives,
 * so it is not drawn -- about a third of a full Turtle. tests/board_test.c
 * checks the picture is identical with and without. */
static void find_covers(mj_view_t *v)
{
	const mj_layout_t *L = v->g->L;
	int t, sx = 2 * dir_x(v), sy = 2 * dir_y(v);
	for (t = 0; t < L->n; t++) {
		const mj_pos_t *p = &L->pos[t];
		v->cover[t][0] = (int16_t)place_at(L, p->x, p->y, p->z + 1);
		v->cover[t][1] = (int16_t)place_at(L, p->x + sx, p->y, p->z);
		v->cover[t][2] = (int16_t)place_at(L, p->x, p->y + sy, p->z);
		v->cover[t][3] = (int16_t)place_at(L, p->x + sx, p->y + sy, p->z);
	}
}

static void sort_order(mj_view_t *v)
{
	const mj_layout_t *L = v->g->L;
	int sx = dir_x(v), sy = dir_y(v), i, j, n = L->n;

	/* Insertion sort on (layer, nearness, row): 144 places, once per
	 * change of layout or corner. */
	for (i = 0; i < n; i++) {
		int t = i;
		int kz = L->pos[t].z, kn = sx * L->pos[t].x + sy * L->pos[t].y;
		int kr = sy * L->pos[t].y;
		for (j = i; j > 0; j--) {
			int u = v->order[j - 1];
			int uz = L->pos[u].z, un = sx * L->pos[u].x + sy * L->pos[u].y;
			int ur = sy * L->pos[u].y;
			if (uz < kz || (uz == kz && (un < kn || (un == kn && ur <= kr)))) break;
			v->order[j] = v->order[j - 1];
		}
		v->order[j] = (uint16_t)t;
	}
	v->norder = n;
}

/* The board's extent in board space: every face and its sides. */
static void measure(const mj_game_t *g, int size, int corner,
		    int *x0, int *y0, int *w, int *h)
{
	mj_view_t tmp;
	int t, fx, fy, minx = 1 << 30, miny = 1 << 30, maxx = -(1 << 30), maxy = -(1 << 30);
	int sx, sy, ex0, ex1, ey0, ey1;

	memset(&tmp, 0, sizeof(tmp));
	tmp.g = g;
	tmp.corner = corner;
	tmp.fw = mj_art[size].face_w;
	tmp.fh = mj_art[size].face_h;
	tmp.d = mj_art[size].depth;
	sx = dir_x(&tmp);
	sy = dir_y(&tmp);
	/* the sides lie past the face on the viewer's side */
	ex0 = sx > 0 ? 0 : -tmp.d;
	ex1 = sx > 0 ? tmp.fw + tmp.d : tmp.fw;
	ey0 = sy > 0 ? 0 : -tmp.d;
	ey1 = sy > 0 ? tmp.fh + tmp.d : tmp.fh;
	for (t = 0; t < g->L->n; t++) {
		face_board(&tmp, t, &fx, &fy);
		if (fx + ex0 < minx) minx = fx + ex0;
		if (fy + ey0 < miny) miny = fy + ey0;
		if (fx + ex1 > maxx) maxx = fx + ex1;
		if (fy + ey1 > maxy) maxy = fy + ey1;
	}
	*x0 = minx;
	*y0 = miny;
	*w = maxx - minx;
	*h = maxy - miny;
}

/* A margin round the board, so the outermost shadows show. */
#define MARGIN 2

bool mj_view_fits(const mj_game_t *g, int size, int w, int h)
{
	int x0, y0, bw, bh;
	if (!g->L) return true;
	measure(g, size, 0, &x0, &y0, &bw, &bh);
	return bw + 2 * MARGIN <= w && bh + 2 * MARGIN <= h - MJ_BAR_H;
}

static void clamp_scroll(mj_view_t *v)
{
	int mx = v->bw + 2 * MARGIN - v->area_w;
	int my = v->bh + 2 * MARGIN - v->area_h;
	if (mx < 0) mx = 0;
	if (my < 0) my = 0;
	if (v->scroll_x > mx) v->scroll_x = mx;
	if (v->scroll_y > my) v->scroll_y = my;
	if (v->scroll_x < 0) v->scroll_x = 0;
	if (v->scroll_y < 0) v->scroll_y = 0;
}

static void place(mj_view_t *v)
{
	clamp_scroll(v);
	/* Centred along an axis that fits, scrolled along one that does
	 * not. */
	if (v->bw + 2 * MARGIN <= v->area_w)
		v->off_x = v->area_x + (v->area_w - v->bw) / 2 - v->bx0;
	else
		v->off_x = v->area_x + MARGIN - v->bx0 - v->scroll_x;
	if (v->bh + 2 * MARGIN <= v->area_h)
		v->off_y = v->area_y + (v->area_h - v->bh) / 2 - v->by0;
	else
		v->off_y = v->area_y + MARGIN - v->by0 - v->scroll_y;
	v->scrolls = v->bw + 2 * MARGIN > v->area_w || v->bh + 2 * MARGIN > v->area_h;
}

void mj_view_layout(mj_view_t *v, int w, int h)
{
	int i, x;
	static const char *const names[MJ_NB] = { "New", "Undo", "Hint", "Shuffle", "Zoom" };

	v->fw = mj_art[v->size].face_w;
	v->fh = mj_art[v->size].face_h;
	v->d = mj_art[v->size].depth;
	v->cw = w;
	v->area_x = 0;
	v->area_y = 0;
	v->area_w = w;
	v->area_h = h - MJ_BAR_H;
	if (v->area_h < 0) v->area_h = 0;
	v->bar_y = v->area_h;

	if (v->g->L) {
		measure(v->g, v->size, v->corner, &v->bx0, &v->by0, &v->bw, &v->bh);
		sort_order(v);
		find_covers(v);
	} else {
		v->bx0 = v->by0 = v->bw = v->bh = 0;
		v->norder = 0;
	}
	place(v);

	/* The buttons, right to left from the bar's end. */
	x = w - 1;
	for (i = MJ_NB - 1; i >= 0; i--) {
		int bw = mj_cv_text_width(names[i], FONT) + 6;
		v->bwid[i] = 0;
		if (!(v->buttons & (1 << i))) continue;
		if (x - bw < w / 3) continue;      /* no room: the text comes first */
		x -= bw;
		v->bx[i] = x;
		v->bwid[i] = bw;
		x -= 2;
	}
}

bool mj_view_scroll(mj_view_t *v, int dx, int dy)
{
	int ox = v->scroll_x, oy = v->scroll_y;
	v->scroll_x += dx;
	v->scroll_y += dy;
	place(v);
	return ox != v->scroll_x || oy != v->scroll_y;
}

void mj_tile_face(const mj_view_t *v, int t, int *x, int *y)
{
	face_board(v, t, x, y);
	*x += v->off_x;
	*y += v->off_y;
}

void mj_tile_box(const mj_view_t *v, int t, int *x, int *y, int *w, int *h)
{
	int fx, fy, d2 = 2 * v->d;
	mj_tile_face(v, t, &fx, &fy);
	*x = dir_x(v) > 0 ? fx : fx - d2;
	*y = dir_y(v) > 0 ? fy : fy - d2;
	*w = v->fw + d2;
	*h = v->fh + d2;
}

bool mj_view_reveal(mj_view_t *v, int t)
{
	int x, y, w, h, dx = 0, dy = 0, m;
	if (!v->scrolls || t < 0) return false;
	/* The tile itself -- face and sides, not its shadow, which may lie
	 * past the board's edge and so past where scrolling can go. */
	mj_tile_face(v, t, &x, &y);
	if (dir_x(v) < 0) x -= v->d;
	if (dir_y(v) < 0) y -= v->d;
	w = v->fw + v->d;
	h = v->fh + v->d;
	m = MARGIN;
	if (x < v->area_x + m) dx = x - v->area_x - m;
	else if (x + w > v->area_x + v->area_w - m) dx = x + w - (v->area_x + v->area_w - m);
	if (y < v->area_y + m) dy = y - v->area_y - m;
	else if (y + h > v->area_y + v->area_h - m) dy = y + h - (v->area_y + v->area_h - m);
	if (!dx && !dy) return false;
	return mj_view_scroll(v, dx, dy);
}

/* -- drawing ---------------------------------------------------------------- */

/* A rectangle given in the tile's own frame -- face at (0, 0), sides
 * towards +x and +y -- put where it lands for this view. */
static void rect_local(const mj_view_t *v, int fx, int fy, int lx, int ly,
		       int w, int h, int *x, int *y)
{
	*x = dir_x(v) > 0 ? fx + lx : fx + v->fw - lx - w;
	*y = dir_y(v) > 0 ? fy + ly : fy + v->fh - ly - h;
}

static void fill_local(const mj_view_t *v, mj_canvas_t *cv, int fx, int fy,
		       int lx, int ly, int w, int h, int c)
{
	int x, y;
	rect_local(v, fx, fy, lx, ly, w, h, &x, &y);
	mj_cv_fill(cv, x, y, w, h, c);
}

static void shade_local(const mj_view_t *v, mj_canvas_t *cv, int fx, int fy,
			int lx, int ly, int w, int h)
{
	int x, y;
	rect_local(v, fx, fy, lx, ly, w, h, &x, &y);
	mj_cv_shade(cv, x, y, w, h, shade_bit(v, cv));
}

static void frame(mj_canvas_t *cv, int x, int y, int w, int h, int c)
{
	mj_cv_fill(cv, x, y, w, 1, c);
	mj_cv_fill(cv, x, y + h - 1, w, 1, c);
	mj_cv_fill(cv, x, y, 1, h, c);
	mj_cv_fill(cv, x + w - 1, y, 1, h, c);
}

/* A tile's body -- its two sides, its face, the face's light and dark
 * edges and its outline -- with the face at (fx, fy). With `mask` every
 * colour is 1: the same drawing, as the stamp's mask. */
static void body(const mj_view_t *v, mj_canvas_t *cv, int fx, int fy, int z, bool mask)
{
	int i, fw = v->fw, fh = v->fh, d = v->d;
#define C(c) (mask ? 1 : (c))

	/* the short edge's side, then the long edge's over the corner they
	 * share; the outermost line of each, and the diagonal where each
	 * meets the face, in the outline colour */
	for (i = 1; i <= d; i++) {
		fill_local(v, cv, fx, fy, fw - 1 + i, i, 1, fh,
			   C(i == d ? MJ_C_EDGE : c_side2(v, z)));
		fill_local(v, cv, fx, fy, fw - 1 + i, i, 1, 1, C(MJ_C_EDGE));
	}
	for (i = 1; i <= d; i++) {
		fill_local(v, cv, fx, fy, i, fh - 1 + i, fw, 1,
			   C(i == d ? MJ_C_EDGE : c_side(v, z)));
		fill_local(v, cv, fx, fy, i, fh - 1 + i, 1, 1, C(MJ_C_EDGE));
	}

	/* The face, with its light and dark inner edges -- these in screen
	 * terms, not mirrored: the light comes from the top left whichever
	 * corner the board is seen from. */
	mj_cv_fill(cv, fx, fy, fw, fh, C(c_face(v, z)));
	mj_cv_fill(cv, fx + 1, fy + 1, fw - 2, 1, C(c_light(v, z)));
	mj_cv_fill(cv, fx + 1, fy + 1, 1, fh - 2, C(c_light(v, z)));
	mj_cv_fill(cv, fx + 1, fy + fh - 2, fw - 2, 1, C(c_lowedge(v, z)));
	mj_cv_fill(cv, fx + fw - 2, fy + 2, 1, fh - 3, C(c_lowedge(v, z)));
	frame(cv, fx, fy, fw, fh, C(MJ_C_EDGE));
#undef C
}

int mj_board_cull = 1;

/* -- stamps: each layer's tile body, drawn once ---------------------------------
 *
 * Every tile on a layer has the same body, so it is drawn once into a
 * stamp -- planes and a mask -- and each tile is then a masked copy of
 * two words a row a plane (mj_cv_stamp()), where drawing it costs a
 * fill for every one-pixel line of its sides and edges. That was most
 * of the time a whole table took: about 1.5 seconds at the board's 7
 * MIPS before, a tenth of that now.
 *
 * In 64 colours there is a stamp for each of six layer shades; in 16
 * and in mono every layer looks the same, so one. They are drawn again
 * when the size, the corner, the colours or the canvas's planes change.
 * In mono the inks are anchored to the stamp, so a tile's sides are
 * dithered the same on every tile, wherever it lands. */

#define ST_STRIDE   2           /* 64 pixels: the large tile and its sides */
#define ST_ROWS     48
#define ST_WORDS    ((MJ_CV_MAXP + 1) * ST_STRIDE * ST_ROWS)
#define ST_CLASSES  6

static uint32_t stamp_mem[ST_CLASSES][ST_WORDS];
static struct {
	int size, corner, planes, w, h;
	bool rich, valid;
	const uint8_t (*ink)[8];
	int depth;
} st;

static void stamps_make(const mj_view_t *v, const mj_canvas_t *cv)
{
	int k, np = cv->planes ? cv->planes : 1;
	int w = v->fw + v->d, h = v->fh + v->d;
	int fx = dir_x(v) > 0 ? 0 : v->d, fy = dir_y(v) > 0 ? 0 : v->d;
	int n = v->rich ? (v->g->L && v->g->L->d < ST_CLASSES ? v->g->L->d : ST_CLASSES) : 1;

	for (k = 0; k < n; k++) {
		mj_canvas_t sc, mc;
		/* the planes as a canvas of this canvas's kind, the mask as a
		 * one-plane canvas after them, laid out as mj_cv_stamp() reads */
		memset(stamp_mem[k], 0, sizeof(stamp_mem[k]));
		/* 64 wide whatever the tile, so every row is ST_STRIDE words:
		 * the layout mj_cv_stamp() is told */
		mj_cv_setup(&sc, stamp_mem[k], ST_WORDS, 32 * ST_STRIDE, h,
			    cv->planes, cv->ink, cv->ink_map);
		mj_cv_setup(&mc, stamp_mem[k] + np * ST_STRIDE * h, ST_STRIDE * h,
			    32 * ST_STRIDE, h, 1, 0, 0);
		body(v, &sc, fx, fy, k, false);
		body(v, &mc, fx, fy, k, true);
	}
	st.size = v->size;
	st.corner = v->corner;
	st.planes = cv->planes;
	st.rich = v->rich;
	st.ink = cv->ink;
	st.depth = v->g->L ? v->g->L->d : 0;
	st.w = w;
	st.h = h;
	st.valid = true;
}

static void stamps_check(const mj_view_t *v, const mj_canvas_t *cv)
{
	if (!st.valid || st.size != v->size || st.corner != v->corner ||
	    st.planes != cv->planes || st.rich != v->rich || st.ink != cv->ink ||
	    (v->rich && v->g->L && st.depth != v->g->L->d))
		stamps_make(v, cv);
}

/* The picture on face `face`, every colour at once, row by row. */
static void glyph(const mj_view_t *v, mj_canvas_t *cv, int fx, int fy, int face)
{
	static const uint8_t col[5] = { 0, MJ_C_INK, MJ_C_RED, MJ_C_GREEN, MJ_C_BLUE };
	const mj_art_size_t *a = &mj_art[v->size];
	int k0 = a->first[face], k1 = a->first[face + 1], y0 = 255, y1 = 0, y, k;

	for (k = k0; k < k1; k++) {
		const mj_art_part_t *p = &mj_art_parts[k];
		if (p->y < y0) y0 = p->y;
		if (p->y + p->h > y1) y1 = p->y + p->h;
	}
	for (y = y0; y < y1; y++) {
		uint32_t bits[4];
		uint8_t c[4];
		int n = 0;
		for (k = k0; k < k1 && n < 4; k++) {
			const mj_art_part_t *p = &mj_art_parts[k];
			if (y < p->y || y >= p->y + p->h) continue;
			bits[n] = mj_art_rows[p->off + (y - p->y)] << p->x;
			c[n] = col[p->col];
			n++;
		}
		if (n) mj_cv_row_multi(cv, fx, fy + y, bits, c, n, 32);
	}
}

/* A tile exactly on top of t covers t's whole face (find_covers() says
 * why), so its picture, colour and marks need not be drawn: only the
 * sides that may show past it. */
static bool face_covered(const mj_view_t *v, int t)
{
	int u = v->cover[t][0];
	return mj_board_cull && u >= 0 && mj_present(v->g, u);
}

static void draw_tile(const mj_view_t *v, mj_canvas_t *cv, int t, bool is_free)
{
	const mj_game_t *g = v->g;
	int z = g->L->pos[t].z, fw = v->fw, fh = v->fh, d = v->d;
	int fx, fy, face = g->face[t];

	mj_tile_face(v, t, &fx, &fy);

	/* The shadow past the sides, onto whatever is lower. */
	shade_local(v, cv, fx, fy, fw + d, 2 * d, d, fh);
	shade_local(v, cv, fx, fy, 2 * d, fh + d, fw - d, d);

	/* The body, from its stamp. */
	mj_cv_stamp(cv, stamp_mem[v->rich ? zc(z) : 0], ST_STRIDE, st.w, st.h,
		    dir_x(v) > 0 ? fx : fx - d, dir_y(v) > 0 ? fy : fy - d);

	if (face_covered(v, t)) return;

	/* Picked up, or hinted: the face in that colour, inside its outline. */
	if (t == v->sel)
		mj_cv_fill(cv, fx + 1, fy + 1, fw - 2, fh - 2, MJ_C_SEL);
	else if (t == v->hint_a || t == v->hint_b)
		mj_cv_fill(cv, fx + 1, fy + 1, fw - 2, fh - 2, MJ_C_HINT);

	if (face >= 0 && face < MJ_FACES) glyph(v, cv, fx, fy, face);

	if (v->show_free && !is_free)
		mj_cv_shade(cv, fx + 1, fy + 1, fw - 2, fh - 2, shade_bit(v, cv));

	if (t == v->cursor) {
		frame(cv, fx + 1, fy + 1, fw - 2, fh - 2, MJ_C_CURSOR);
		frame(cv, fx + 2, fy + 2, fw - 4, fh - 4, MJ_C_CURSOR);
	}
}

static bool hidden(const mj_view_t *v, int t)
{
	int k;
	if (!mj_board_cull) return false;
	for (k = 0; k < 4; k++) {
		int u = v->cover[t][k];
		if (u < 0 || !mj_present(v->g, u)) return false;
	}
	return true;
}

static bool overlaps(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
	return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

void mj_render(const mj_view_t *v, mj_canvas_t *cv, int x, int y, int w, int h)
{
	static uint8_t on[MJ_MAX];
	int i, x0 = x, y0 = y, x1 = x + w, y1 = y + h;

	if (x0 < v->area_x) x0 = v->area_x;
	if (y0 < v->area_y) y0 = v->area_y;
	if (x1 > v->area_x + v->area_w) x1 = v->area_x + v->area_w;
	if (y1 > v->area_y + v->area_h) y1 = v->area_y + v->area_h;
	if (x1 <= x0 || y1 <= y0) return;

	stamps_check(v, cv);
	mj_cv_clip(cv, x0, y0, x1 - x0, y1 - y0);
	mj_cv_fill(cv, x0, y0, x1 - x0, y1 - y0, MJ_C_BG);

	if (v->g->L && v->show_free) mj_free_flags(v->g, on);

	for (i = 0; i < v->norder; i++) {
		int t = v->order[i], bx, by, bw, bh;
		if (!mj_present(v->g, t)) continue;
		mj_tile_box(v, t, &bx, &by, &bw, &bh);
		if (!overlaps(bx, by, bw, bh, x0, y0, x1 - x0, y1 - y0)) continue;
		if (hidden(v, t)) continue;
		draw_tile(v, cv, t, v->show_free ? on[t] != 0 : true);
	}
	mj_cv_noclip(cv);
}

void mj_render_bar(mj_view_t *v, mj_canvas_t *cv)
{
	static const char *const names[MJ_NB] = { "New", "Undo", "Hint", "Shuffle", "Zoom" };
	int i, y = v->bar_y, room, n;
	const char *s = v->msg[0] ? v->msg : v->status;
	char cut[MJ_TEXT_LEN];

	if (y >= cv->h) return;
	mj_cv_noclip(cv);
	mj_cv_fill(cv, 0, y, v->cw, MJ_BAR_H, MJ_C_BAR);
	mj_cv_fill(cv, 0, y, v->cw, 1, MJ_C_EDGE);

	/* The text, cut where the first button starts. */
	room = v->cw - 4;
	for (i = 0; i < MJ_NB; i++)
		if (v->bwid[i] && v->bx[i] - 6 < room) room = v->bx[i] - 6;
	n = room / FONT->w;
	if (n < 0) n = 0;
	if (n > MJ_TEXT_LEN - 1) n = MJ_TEXT_LEN - 1;
	for (i = 0; i < n && s[i]; i++) cut[i] = s[i];
	cut[i] = 0;
	mj_cv_text(cv, 3, y + 2, cut, MJ_C_TEXT, FONT);

	for (i = 0; i < MJ_NB; i++) {
		if (!v->bwid[i]) continue;
		frame(cv, v->bx[i], y + 1, v->bwid[i], MJ_BAR_H - 1, MJ_C_SIDE);
		mj_cv_text(cv, v->bx[i] + 3, y + 2, names[i], MJ_C_TEXT, FONT);
	}
}

bool mj_render_scrolled(mj_view_t *v, mj_canvas_t *cv, int dx, int dy)
{
	int ax = v->area_x, ay = v->area_y, aw = v->area_w, ah = v->area_h;

	if (!dx && !dy) return true;
	if (dx >= aw || -dx >= aw || dy >= ah || -dy >= ah) return false;
	/* mono's inks repeat every 4 pixels at most (mj_inks) and the
	 * shadows' checkerboard every 2, anchored to the canvas */
	if (!cv->planes && ((dx & 3) || (dy & 3))) return false;
	if (ax != 0 || aw != cv->w) return false;   /* the shift is whole rows */

	/* the view moved by (dx, dy): the picture by (-dx, -dy) */
	mj_cv_scroll(cv, ay, ay + ah, -dx, -dy);
	if (dx > 0) mj_render(v, cv, ax + aw - dx, ay, dx, ah);
	else if (dx < 0) mj_render(v, cv, ax, ay, -dx, ah);
	if (dy > 0) mj_render(v, cv, ax, ay + ah - dy, aw, dy);
	else if (dy < 0) mj_render(v, cv, ax, ay, aw, -dy);
	return true;
}

void mj_render_all(mj_view_t *v, mj_canvas_t *cv)
{
	mj_render(v, cv, 0, 0, cv->w, cv->h);
	mj_render_bar(v, cv);
}

void mj_free_flags(const mj_game_t *g, uint8_t *out)
{
	if (g->L) mj_free_all(g, out);
}

/* -- the pointer and the keyboard -------------------------------------------- */

int mj_hit(const mj_view_t *v, int x, int y)
{
	int i;
	if (y < v->area_y || y >= v->area_y + v->area_h) return -1;
	for (i = v->norder - 1; i >= 0; i--) {
		int t = v->order[i], fx, fy, bx, by;
		if (!mj_present(v->g, t)) continue;
		mj_tile_face(v, t, &fx, &fy);
		/* the face and the sides: what a click can land on */
		bx = dir_x(v) > 0 ? fx : fx - v->d;
		by = dir_y(v) > 0 ? fy : fy - v->d;
		if (x >= bx && x < bx + v->fw + v->d && y >= by && y < by + v->fh + v->d) {
			/* the corners the sides' diagonals leave empty */
			int lx = dir_x(v) > 0 ? x - fx : fx + v->fw - 1 - x;
			int ly = dir_y(v) > 0 ? y - fy : fy + v->fh - 1 - y;
			if (lx >= v->fw && ly < lx - v->fw + 1) continue;
			if (ly >= v->fh && lx < ly - v->fh + 1) continue;
			return t;
		}
	}
	return -1;
}

int mj_bar_hit(const mj_view_t *v, int x, int y)
{
	int i;
	if (y < v->bar_y || y >= v->bar_y + MJ_BAR_H) return -1;
	for (i = 0; i < MJ_NB; i++)
		if (v->bwid[i] && x >= v->bx[i] && x < v->bx[i] + v->bwid[i]) return i;
	return -1;
}

static bool exposed(const mj_game_t *g, int t)
{
	int k;
	if (!mj_present(g, t)) return false;
	for (k = 0; k < g->L->nabove[t]; k++)
		if (mj_present(g, g->L->above[t][k])) return false;
	return true;
}

static void centre(const mj_view_t *v, int t, int *x, int *y)
{
	mj_tile_face(v, t, x, y);
	*x += v->fw / 2;
	*y += v->fh / 2;
}

int mj_nav(const mj_view_t *v, int from, int dir)
{
	int t, best = -1, wide = -1, fx, fy;
	long bestd = 0, wided = 0;

	if (!v->g->L) return -1;
	if (from < 0 || !mj_present(v->g, from)) return mj_nav_first(v);
	centre(v, from, &fx, &fy);
	for (t = 0; t < v->g->L->n; t++) {
		int x, y, along, across;
		long dist;
		if (t == from || !exposed(v->g, t)) continue;
		centre(v, t, &x, &y);
		switch (dir) {
		case 0:  along = fx - x; across = y - fy; break;
		case 1:  along = x - fx; across = y - fy; break;
		case 2:  along = fy - y; across = x - fx; break;
		default: along = y - fy; across = x - fx; break;
		}
		if (across < 0) across = -across;
		if (along <= 0) continue;
		dist = (long)along + 3L * across;
		/* Ahead and within a cone of the direction first: a tile
		 * beside the row is not "to the right" of it. Failing that,
		 * the nearest anywhere on that side, so that a press always
		 * moves when there is somewhere to go. */
		if (across <= 2 * along) {
			if (best < 0 || dist < bestd) { best = t; bestd = dist; }
		} else if (wide < 0 || dist < wided) {
			wide = t;
			wided = dist;
		}
	}
	return best >= 0 ? best : wide;
}

/* Tab: the free tiles in reading order -- rows of the picture, top to
 * bottom, left to right -- so every tile that can be taken is a few
 * presses away whatever the arrows make of the layout's shape. Centres
 * within half a face's height read as one row (the Turtle's side tiles
 * sit half a row down). */
static long read_key(const mj_view_t *v, int t)
{
	int x, y, row = v->fh / 2 ? v->fh / 2 : 1;
	centre(v, t, &x, &y);
	return (long)((y + row / 2) / row) * 65536L + (x + 32768);
}

int mj_nav_tab(const mj_view_t *v, int from, int step)
{
	int t, best = -1, wrap = -1;
	long bestk = 0, wrapk = 0, fk;
	bool have = from >= 0 && mj_present(v->g, from);

	if (!v->g->L) return -1;
	fk = have ? read_key(v, from) : (step > 0 ? -1L : 0x7fffffffL);
	for (t = 0; t < v->g->L->n; t++) {
		long k;
		if (t == from || !mj_free(v->g, t)) continue;
		k = read_key(v, t);
		/* ties (same key) are broken by index, so none is skipped */
		if (step > 0) {
			if ((k > fk || (k == fk && t > from)) && (best < 0 || k < bestk))
				{ best = t; bestk = k; }
			if (wrap < 0 || k < wrapk) { wrap = t; wrapk = k; }
		} else {
			if ((k < fk || (k == fk && t < from)) && (best < 0 || k > bestk))
				{ best = t; bestk = k; }
			if (wrap < 0 || k > wrapk) { wrap = t; wrapk = k; }
		}
	}
	return best >= 0 ? best : wrap;
}

int mj_nav_first(const mj_view_t *v)
{
	int t, best = -1, bestk = 0;
	if (!v->g->L) return -1;
	for (t = 0; t < v->g->L->n; t++) {
		int x, y, k;
		if (!exposed(v->g, t)) continue;
		centre(v, t, &x, &y);
		k = x + y + (mj_free(v->g, t) ? 0 : 100000);
		if (best < 0 || k < bestk) { best = t; bestk = k; }
	}
	return best;
}
