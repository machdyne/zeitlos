/*
 * blocks -- the picture. See docs/blocks_app.md, "How it is drawn".
 *
 * The static picture -- backdrop, the well and its stack, the boxes,
 * the text -- is painted by bk_paint() for whatever rectangle the
 * runtime says is dirty (sw/common/games/zarcade.h). The falling piece,
 * its ghost, the flashes, the hard-drop trails, sparks and the title's
 * rain are overlays.
 *
 * -- the drop after a clear is a copy --
 *
 * When rows go, every row above them falls: a few hundred blocks. They
 * are not redrawn. Each run of rows that falls the same distance is
 * moved down in video memory by the blitter (ar_copy(), all four
 * planes), a few pixels a frame, accelerating; only the strip each run
 * leaves behind is painted. The paint knows the same offsets
 * (bk.fall_px), so any repaint in between -- a window moved away, the
 * help box closed -- draws exactly what the copies made.
 *
 * That only holds if a block looks the same wherever it is drawn. Every
 * block in the well is the same sprite as the falling piece (one pass a
 * plane): in colour solid colours, in monochrome a texture made from the
 * shape's ink, and those inks are all patterns that are the same on
 * every row (vertical stripes of different widths) -- so a block moved
 * down by any number of pixels still matches a fresh paint. The small
 * blocks in the boxes are fills, which never move.
 */

#include <string.h>

#include "../../common/zfont.h"
#include "../../common/games/zarcade.h"
#include "../../common/games/zsfx.h"

#include "blocks.h"
#include "bk_sound.h"

/* ===================================================================
 * colour
 * =================================================================== */

const uint16_t bk_base_palette[16] = {
	0x000, 0x0cf, 0xfd0, 0xb4f, 0x3d4, 0xf34, 0x36f, 0xf82,
	0x226, 0xaff, 0xffb, 0xdaf, 0xbfb, 0xfbb, 0xabf, 0xfff
};

/* The theme -- the frame, the backdrop, the grey blocks -- changes
 * with the level. Dark, so the shapes stay the brightest thing. */
static const uint16_t themes[10] = {
	0x226, 0x245, 0x325, 0x242, 0x423, 0x332, 0x236, 0x434, 0x253, 0x522
};

static uint16_t theme_now = 0x226;

void bk_theme(int level) {
	theme_now = themes[(level - 1 + 10) % 10];
}

void bk_mono_inks(ar_ink_t *ink) {
	/* Every shape's texture is the same on every row, so a block moved
	 * down by the blitter matches a block painted where it lands (see
	 * the top of this file). */
	static const uint8_t col[8] = { 0x00, 0xff, 0xaa, 0x88, 0xee, 0xcc, 0x92, 0xdb };
	int i, r;
	for (i = 0; i < 16; i++) ink[i] = ar_ink_white;
	for (i = 0; i < 8; i++)
		for (r = 0; r < 8; r++) ink[i].row[r] = col[i];
	ink[BK_C_THEME] = ar_ink_grey50;
}

/* The colour each kind of cell is drawn in: a shape its own (1..7);
 * the little pieces borrow four of them; grey and the walls the theme. */
static const uint8_t colour_of[BK_TYPES] = {
	0, BK_I, BK_O, BK_T, BK_S, BK_Z, BK_J, BK_L, BK_C_THEME,
	BK_T, BK_O, BK_I, BK_L, BK_C_THEME
};
static int body_of(int t) {
	return (t > 0 && t < BK_TYPES) ? colour_of[t] : BK_C_THEME;
}
static int edge_of(int t) {
	if (t == BK_WALL) return BK_C_THEME;
	if (t == BK_GREY || t <= 0 || t >= BK_TYPES) return BK_C_WHITE;
	return colour_of[t] + 8;
}

/* ===================================================================
 * sprites
 * =================================================================== */

static ar_sprite_t cell_s[BK_TYPES + 1];     /* a block, 10x10 */
static ar_sprite_t ghost_s[BK_TYPES + 1];    /* its outline */

static bool make_cell(ar_sprite_t *s, int t, bool ghost) {
	uint8_t px[BK_CELL * BK_CELL];
	int x, y, e = edge_of(t), b = body_of(t);
	for (y = 0; y < BK_CELL; y++)
		for (x = 0; x < BK_CELL; x++) {
			uint8_t v = 0xff;
			if (x < 9 && y < 9) {
				if (ghost) {
					if (x == 0 || y == 0 || x == 8 || y == 8)
						v = (uint8_t)(((x + y) & 1) ? 0xff : e);
				} else {
					v = (uint8_t)((x == 0 || y == 0) ? e : b);
					if ((y == 2 && (x == 2 || x == 3)) || (y == 3 && x == 2))
						v = (uint8_t)e;     /* a glint */
				}
			}
			px[y * BK_CELL + x] = v;
		}
	return ar_sprite_from_pixels(s, px, BK_CELL, BK_CELL);
}

bool bk_draw_init(void) {
	int t;
	bool ok = true;
	for (t = BK_I; t < BK_TYPES; t++) {
		ok = make_cell(&cell_s[t], t, false) && ok;
		ok = make_cell(&ghost_s[t], t, true) && ok;
	}
	return ok;
}

/* A block drawn with fills, for the paint: the same pixels as the
 * sprite (an edge on the top and left, the glint, a one-pixel gap). */
static void block(int x, int y, int t, int size) {
	int e = edge_of(t), b = body_of(t);
	ar_fill(x, y, size - 1, size - 1, e);
	ar_fill(x + 1, y + 1, size - 2, size - 2, b);
	if (size >= 8) {
		ar_fill(x + 2, y + 2, 2, 1, e);
		ar_fill(x + 2, y + 3, 1, 1, e);
	}
}

/* A whole shape in a box, centred, at a cell size, in its own colours
 * or (as != 0) another's. */
static void shape_in_box(int t, int bx, int by, int bw, int bh, int size,
	int as) {
	int8_t c[4][2];
	int i, minx = 9, maxx = -1, miny = 9, maxy = -1, w, h, ox, oy;
	if (t < BK_I || t > BK_V3 || t == BK_GREY) return;
	bk_cells(t, 0, c);
	for (i = 0; i < bk_ncells(t); i++) {
		if (c[i][0] < minx) minx = c[i][0];
		if (c[i][0] > maxx) maxx = c[i][0];
		if (c[i][1] < miny) miny = c[i][1];
		if (c[i][1] > maxy) maxy = c[i][1];
	}
	w = (maxx - minx + 1) * size;
	h = (maxy - miny + 1) * size;
	ox = bx + (bw - w) / 2;
	oy = by + (bh - h) / 2;
	for (i = 0; i < bk_ncells(t); i++)
		block(ox + (c[i][0] - minx) * size, oy + (c[i][1] - miny) * size,
			as ? as : t, size);
}

/* ===================================================================
 * the announcer: FOUR!, combos, levels
 * =================================================================== */

static char say_big[16], say_small[24];
static int say_colour, say_left;

void bk_say(const char *big, const char *small, int colour) {
	/* big may be say_big itself: keep the headline, change the line */
	if (big != say_big) {
		strncpy(say_big, big ? big : "", sizeof(say_big) - 1);
		say_big[sizeof(say_big) - 1] = 0;
	}
	if (small != say_small) {
		strncpy(say_small, small ? small : "", sizeof(say_small) - 1);
		say_small[sizeof(say_small) - 1] = 0;
	}
	say_colour = colour;
	say_left = 120;
	ar_dirty(SAY_X, SAY_Y, SAY_W, SAY_H);
}

/* ===================================================================
 * panels over the well
 * =================================================================== */

typedef struct {
	int y;
	const char *s;
	int c;
	const z_font_t *f;
	int scale;
} line_t;

#define MAX_LINES 16
static char tbuf[MAX_LINES][28];

const char *const bk_mode_names[] = { "ENDLESS", "40 LINES", "3 MINUTES", "LITTLE" };

static int panel(ar_rect_t *r, line_t *ln) {

	int n = 0, i;

#define LINE(yy, ss, cc, ff, sc) do { \
		ln[n].y = (yy); ln[n].s = (ss); ln[n].c = (cc); \
		ln[n].f = (ff); ln[n].scale = (sc); n++; } while (0)

	switch (bk.st) {

	case ST_TITLE:
		r->x = 64; r->y = 64; r->w = 192; r->h = 156;
		for (i = 0; i < MENU_ITEMS; i++) {
			static const char *const items[MENU_ITEMS] = {
				"", "ENDLESS", "LITTLE PIECES", "40 LINES", "3 MINUTES",
				"HIGH SCORES"
			};
			bool sel = (i == bk.menu);
			char *b = tbuf[i];
			if (i == MENU_SKILL) {
				/* "< EASY >": Left and Right change it */
				strcpy(b, sel ? "< " : "  ");
				strcat(b, ar_skill_name(ar_skill()));
				strcat(b, sel ? " >" : "  ");
				LINE(8, b, sel ? BK_C_WHITE : 10, &z_font_8x8, 1);
				continue;
			}
			b[0] = sel ? '>' : ' ';
			b[1] = ' ';
			strcpy(b + 2, items[i]);
			strcat(b, sel ? " <" : "  ");
			LINE(22 + (i - 1) * 12, b, sel ? BK_C_WHITE : 9, &z_font_8x8, 1);
		}
		{
			bool lv = (bk.menu == MENU_ENDLESS || bk.menu == MENU_LITTLE);
			strcpy(tbuf[6], "START LEVEL  ");
			ar_utoa(tbuf[6] + strlen(tbuf[6]), (uint32_t)bk.start_level, 2, ' ');
			LINE(86, tbuf[6], lv ? 10 : 14, &z_font_5x8, 1);
			LINE(95, lv ? "LEFT/RIGHT TO CHANGE" :
				bk.menu == MENU_SKILL ? "LEFT/RIGHT: HOW HARD" : "",
				14, &z_font_5x8, 1);
		}
		if (bk.menu >= MENU_ENDLESS && bk.menu <= MENU_3MIN) {
			strcpy(tbuf[7], "BEST ");
			if (bk.menu == MENU_40) {
				if (bk.best) bk_format_time(tbuf[7] + 5, 9999999u - bk.best);
				else strcat(tbuf[7], "--:--.-");
			} else {
				ar_utoa(tbuf[7] + 5, bk.best, 7, '0');
			}
			LINE(106, tbuf[7], BK_C_WHITE, &z_font_8x8, 1);
		}
		LINE(122, "ARROWS  Z X  SPACE  H", 9, &z_font_5x8, 1);
		LINE(132, "ENTER TO PLAY   M MUSIC", 9, &z_font_5x8, 1);
		LINE(142, "F1 HELP  F11 FULL SCREEN", 9, &z_font_5x8, 1);
		return n;

	case ST_SCORES:
		r->x = 64; r->y = 64; r->w = 192; r->h = 156;
		LINE(4, bk_mode_names[bk.score_view], 10, &z_font_8x8, 2);
		LINE(22, ar_skill_name(ar_skill()), 9, &z_font_5x8, 1);
		for (i = 0; i < AR_SCORES; i++) {
			char *b = tbuf[i];
			ar_utoa(b, (uint32_t)i + 1, 2, ' ');
			strcat(b, ". ");
			if (i < bk.table.n) {
				char v[16];
				strcat(b, bk.table.e[i].who);
				strcat(b, "  ");
				if (bk.score_view == BK_MODE_40)
					bk_format_time(v, 9999999u - bk.table.e[i].score);
				else
					ar_utoa(v, bk.table.e[i].score, 7, '0');
				strcat(b, v);
			} else {
				strcat(b, "---  -------");
			}
			LINE(32 + i * 10, b, i == bk.rank ? BK_C_WHITE : 9, &z_font_8x8, 1);
		}
		LINE(140, "LEFT/RIGHT MODE  ENTER BACK", 9, &z_font_5x8, 1);
		return n;

	case ST_READY: {
		static const char *const count[4] = { "3", "2", "1", "GO!" };
		int k = (int)(bk.st_frames / 30);
		if (k > 3) k = 3;
		r->x = 112; r->y = 100; r->w = 96; r->h = 48;
		LINE(8, count[k], k == 3 ? 4 : BK_C_WHITE, &z_font_8x8, 4);
		return n;
	}

	case ST_PAUSE:
		r->x = 80; r->y = 100; r->w = 160; r->h = 56;
		LINE(8, "PAUSED", 10, &z_font_8x8, 2);
		LINE(30, "P OR START: GO ON", 9, &z_font_5x8, 1);
		LINE(40, "ESC: MENU", 9, &z_font_5x8, 1);
		return n;

	case ST_OVER:
		r->x = 80; r->y = 92; r->w = 160; r->h = 72;
		if (bk.g.done)
			LINE(8, bk.mode == BK_MODE_40 ? "CLEAR!" : "TIME!", 10, &z_font_8x8, 2);
		else
			LINE(8, "GAME OVER", 13, &z_font_8x8, 2);
		if (bk.mode == BK_MODE_40) {
			strcpy(tbuf[0], bk.g.done ? "TIME " : "LINES ");
			if (bk.g.done) bk_format_time(tbuf[0] + 5, bk.g.frames);
			else ar_utoa(tbuf[0] + 6, bk.g.lines, 0, ' ');
		} else {
			strcpy(tbuf[0], "SCORE ");
			ar_utoa(tbuf[0] + 6, bk.g.score, 7, '0');
		}
		LINE(32, tbuf[0], BK_C_WHITE, &z_font_8x8, 1);
		LINE(50, "ENTER OR START", 9, &z_font_5x8, 1);
		return n;

	case ST_NAME:
		r->x = 72; r->y = 80; r->w = 176; r->h = 96;
		LINE(8, "NEW BEST", 10, &z_font_8x8, 1);
		strcpy(tbuf[0], "RANK ");
		ar_utoa(tbuf[0] + 5, (uint32_t)bk.rank + 1, 0, ' ');
		LINE(22, tbuf[0], BK_C_WHITE, &z_font_8x8, 1);
		tbuf[1][0] = bk.name[0];
		tbuf[1][1] = ' ';
		tbuf[1][2] = bk.name[1];
		tbuf[1][3] = ' ';
		tbuf[1][4] = bk.name[2];
		tbuf[1][5] = 0;
		LINE(38, tbuf[1], BK_C_WHITE, &z_font_8x8, 3);
		LINE(78, "UP/DOWN LETTER  ENTER DONE", 9, &z_font_5x8, 1);
		return n;

	default:
		return 0;
	}
#undef LINE
}

bool bk_panel_rect(ar_rect_t *r) {
	line_t ln[MAX_LINES];
	return panel(r, ln) > 0;
}

static bool overlaps(const ar_rect_t *a, int x, int y, int w, int h) {
	return a->x < x + w && x < a->x + a->w && a->y < y + h && y < a->y + a->h;
}

static void text_if(const ar_rect_t *r, int x, int y, const char *s, int c,
	const z_font_t *f, int scale) {
	int w = ar_text_width(s, f, scale);
	if (!overlaps(r, x, y, w, f->h * scale)) return;
	ar_text(x, y, s, c, f, scale);
}

static void text_centre(const ar_rect_t *r, int x, int w, int y, const char *s,
	int c, const z_font_t *f, int scale) {
	text_if(r, x + (w - ar_text_width(s, f, scale)) / 2, y, s, c, f, scale);
}

static void paint_panel(const ar_rect_t *dirty) {
	ar_rect_t r;
	line_t ln[MAX_LINES];
	int n = panel(&r, ln), i;
	if (n == 0 || !overlaps(dirty, r.x, r.y, r.w, r.h)) return;
	ar_fill(r.x, r.y, r.w, r.h, BK_C_BLACK);
	ar_fill(r.x + 1, r.y + 1, r.w - 2, 1, BK_C_THEME);
	ar_fill(r.x + 1, r.y + r.h - 2, r.w - 2, 1, BK_C_THEME);
	ar_fill(r.x + 1, r.y + 1, 1, r.h - 2, BK_C_THEME);
	ar_fill(r.x + r.w - 2, r.y + 1, 1, r.h - 2, BK_C_THEME);
	for (i = 0; i < n; i++)
		text_centre(dirty, r.x, r.w, r.y + ln[i].y, ln[i].s, ln[i].c,
			ln[i].f, ln[i].scale);
}

/* ===================================================================
 * the static picture
 * =================================================================== */

static void box(int x, int y, int w, int h) {
	ar_fill(x, y, w, h, BK_C_THEME);
	ar_fill(x + 1, y + 1, w - 2, h - 2, BK_C_BLACK);
}

static bool in_game(void) {
	return bk.st != ST_TITLE && bk.st != ST_SCORES;
}

static void paint_well(const ar_rect_t *r) {

	int row, col;
	int r0, r1, c0, c1;

	/* the frame: two of theme, one of black, then the floor */
	/* the frame, only where the rectangle reaches it: the floor itself
	 * was filled black by backdrop_fill() */
	if (overlaps(r, WELL_X - 3, WELL_Y - 3, WELL_W + 6, WELL_H + 6) &&
	    !(r->x >= WELL_X && r->y >= WELL_Y && r->x + r->w <= WELL_X + WELL_W &&
	      r->y + r->h <= WELL_Y + WELL_H)) {
		ar_fill(WELL_X - 3, WELL_Y - 3, WELL_W + 6, 2, BK_C_THEME);
		ar_fill(WELL_X - 3, WELL_Y + WELL_H + 1, WELL_W + 6, 2, BK_C_THEME);
		ar_fill(WELL_X - 3, WELL_Y - 1, 2, WELL_H + 2, BK_C_THEME);
		ar_fill(WELL_X + WELL_W + 1, WELL_Y - 1, 2, WELL_H + 2, BK_C_THEME);
		ar_fill(WELL_X - 1, WELL_Y - 1, WELL_W + 2, 1, BK_C_BLACK);
		ar_fill(WELL_X - 1, WELL_Y + WELL_H, WELL_W + 2, 1, BK_C_BLACK);
		ar_fill(WELL_X - 1, WELL_Y, 1, WELL_H, BK_C_BLACK);
		ar_fill(WELL_X + WELL_W, WELL_Y, 1, WELL_H, BK_C_BLACK);
	}

	/* The rows the rectangle reaches -- a row that has fallen part of
	 * the way can reach one more, so a margin of a row either side. */
	r0 = BK_HIDDEN + (r->y - WELL_Y) / BK_CELL - 2;
	r1 = BK_HIDDEN + (r->y + r->h - WELL_Y) / BK_CELL + 1;
	if (r0 < BK_HIDDEN) r0 = BK_HIDDEN;
	if (r1 >= BK_H) r1 = BK_H - 1;
	c0 = (r->x - WELL_X) / BK_CELL;
	c1 = (r->x + r->w - 1 - WELL_X) / BK_CELL;
	if (c0 < 0) c0 = 0;
	if (c1 >= BK_W) c1 = BK_W - 1;
	if (r->x + r->w <= WELL_X || r->x >= WELL_X + WELL_W) return;

	/* During the fall the stack is drawn where the copies have it; the
	 * rows going are already gone. A fallen row is clipped to the well
	 * by hand: the copy never leaves it. */
	for (row = r0 - 5; row <= r1; row++) {
		int rr = row, y;
		if (rr < BK_HIDDEN) continue;
		if ((bk.st == ST_FALL) && (bk.rows & (1u << rr))) continue;
		y = ROW_Y(rr) + ((bk.st == ST_FALL) ? bk.fall_px[rr] : 0);
		if (y + BK_CELL <= r->y || y >= r->y + r->h) continue;
		if (y >= WELL_Y + WELL_H) continue;
		for (col = c0; col <= c1; col++) {
			int t = bk.g.well[rr][col];
			/* The sprite, not four fills: one blitter pass a plane,
			 * and the same pixels as the falling piece's overlay.
			 * Walls are drawn below, where they always are. */
			if (t && t != BK_WALL) ar_sprite(&cell_s[t], COL_X(col), y);
		}
	}

	/* little pieces: the walls, never moving, cleared rows or not */
	if (bk.g.little)
		for (row = r0; row <= r1; row++) {
			int y;
			if (row < BK_HIDDEN || row >= BK_H) continue;
			y = ROW_Y(row);
			if (y + BK_CELL <= r->y || y >= r->y + r->h) continue;
			if (c0 == 0 && bk.g.well[row][0] == BK_WALL)
				ar_sprite(&cell_s[BK_WALL], COL_X(0), y);
			if (c1 == BK_W - 1 && bk.g.well[row][BK_W - 1] == BK_WALL)
				ar_sprite(&cell_s[BK_WALL], COL_X(BK_W - 1), y);
		}
}

static void paint_boxes(const ar_rect_t *r) {

	char v[16];
	int y;

	/* hold */
	if (overlaps(r, HOLD_X, HOLD_Y - 10, HOLD_W, HOLD_H + 10)) {
		text_if(r, HOLD_X, HOLD_Y - 10, "HOLD", BK_C_WHITE, &z_font_8x8, 1);
		box(HOLD_X, HOLD_Y, HOLD_W, HOLD_H);
		/* greyed while it cannot be used again */
		if (bk.g.hold)
			shape_in_box(bk.g.hold, HOLD_X, HOLD_Y, HOLD_W, HOLD_H, 8,
				bk.g.hold_used ? BK_GREY : 0);
	}

	/* stats */
	if (overlaps(r, STATS_X, STATS_Y, 88, 80)) {
		y = STATS_Y;
		text_if(r, STATS_X, y, "SCORE", 9, &z_font_8x8, 1);
		ar_utoa(v, bk.g.score, 7, '0');
		text_if(r, STATS_X + 8, y + 10, v, BK_C_WHITE, &z_font_8x8, 1);
		y += 26;
		if (bk.mode == BK_MODE_40) {
			text_if(r, STATS_X, y, "LEFT", 9, &z_font_8x8, 1);
			ar_utoa(v, (uint32_t)bk_lines_goal(&bk.g), 3, ' ');
			text_if(r, STATS_X + 56, y, v, BK_C_WHITE, &z_font_8x8, 1);
		} else {
			text_if(r, STATS_X, y, "LEVEL", 9, &z_font_8x8, 1);
			ar_utoa(v, bk.g.level, 3, ' ');
			text_if(r, STATS_X + 56, y, v, BK_C_WHITE, &z_font_8x8, 1);
		}
		y += 12;
		text_if(r, STATS_X, y, "LINES", 9, &z_font_8x8, 1);
		ar_utoa(v, bk.g.lines, 3, ' ');
		text_if(r, STATS_X + 56, y, v, BK_C_WHITE, &z_font_8x8, 1);
		y += 12;
		text_if(r, STATS_X, y, "TIME", 9, &z_font_8x8, 1);
		bk_format_time(v, bk.mode == BK_MODE_3MIN ? bk_frames_left(&bk.g) : bk.g.frames);
		text_if(r, STATS_X + 32, y, v, BK_C_WHITE, &z_font_8x8, 1);
		/* easy: the rescues left, as hearts */
		if (bk.g.skill == BK_SKILL_EASY && overlaps(r, STATS_X, STATS_Y + 64, 88, 8)) {
			static const uint32_t heart[6] = { 0x36, 0x7f, 0x7f, 0x3e, 0x1c, 0x08 };
			int k;
			for (k = 0; k < bk.g.rescues; k++)
				ar_bitmap(heart, 4, 0, 0, STATS_X + k * 10, STATS_Y + 65, 7, 6, 5);
		}
	}

	/* next, the first big and the rest small */
	if (overlaps(r, NEXT_X, NEXT_Y - 10, NEXT_W, QUEUE_Y + QUEUE_H - NEXT_Y + 10)) {
		int i;
		text_if(r, NEXT_X, NEXT_Y - 10, "NEXT", BK_C_WHITE, &z_font_8x8, 1);
		box(NEXT_X, NEXT_Y, NEXT_W, NEXT_H);
		shape_in_box(bk.g.queue[0], NEXT_X, NEXT_Y, NEXT_W, NEXT_H, 8, 0);
		/* easy: only the next one -- less to look at */
		if (bk.g.skill != BK_SKILL_EASY) box(NEXT_X, QUEUE_Y, NEXT_W, QUEUE_H);
		for (i = 1; i < BK_QUEUE && bk.g.skill != BK_SKILL_EASY; i++)
			shape_in_box(bk.g.queue[i], NEXT_X, QUEUE_Y + 4 + (i - 1) * 33,
				NEXT_W, 33, 6, 0);
	}

	/* the announcer */
	if (overlaps(r, SAY_X, SAY_Y, SAY_W, SAY_H)) {
		box(SAY_X, SAY_Y, SAY_W, SAY_H);
		if (say_left > 0) {
			text_centre(r, SAY_X, SAY_W, SAY_Y + 12, say_big, say_colour,
				&z_font_8x8, 2);
			text_centre(r, SAY_X, SAY_W, SAY_Y + 36, say_small, BK_C_WHITE,
				&z_font_5x8, 1);
		}
	}

	/* the mode, over the well */
	text_centre(r, WELL_X - 20, WELL_W + 40, 8, bk_mode_names[bk.mode],
		BK_C_WHITE, &z_font_5x8, 1);
}

/* -- the title -- */

static const char *const letters[6][5] = {
	{ "1111.", "1...1", "1111.", "1...1", "1111." },
	{ "1....", "1....", "1....", "1....", "11111" },
	{ ".111.", "1...1", "1...1", "1...1", ".111." },
	{ ".1111", "1....", "1....", "1....", ".1111" },
	{ "1...1", "1..1.", "111..", "1..1.", "1...1" },
	{ ".1111", "1....", ".111.", "....1", "1111." },
};
static const uint8_t letter_type[6] = { BK_Z, BK_L, BK_O, BK_S, BK_I, BK_T };

#define TITLE_X 38
#define TITLE_Y 14
#define TITLE_W 245
#define TITLE_H 35

static void paint_title(const ar_rect_t *r) {
	int l, x, y;
	if (!overlaps(r, TITLE_X, TITLE_Y, TITLE_W, TITLE_H)) return;
	for (l = 0; l < 6; l++)
		for (y = 0; y < 5; y++)
			for (x = 0; x < 5; x++)
				if (letters[l][y][x] == '1')
					/* in monochrome the shapes' stripes make the
					 * letters hard to read: plain white blocks */
					block(TITLE_X + l * 42 + x * 7, TITLE_Y + y * 7,
						ar_color() ? letter_type[l] : BK_I, 7);
}

/* The backdrop: a field of staggered dots in the theme colour. A
 * pattern costs the blitter one operation per run of identical rows
 * (zgfx.h, z_fb_hw_fill_pattern()), per plane: this one has four runs
 * in eight rows. It used to be a diagonal -- a different byte on every
 * row, eight runs -- and on the title, where the falling shapes have
 * the backdrop repainted behind them every frame, that cost more than
 * a frame's time on the board, so the drawing fell behind the beam and
 * the shapes flickered. Most repaints in a game are inside the well,
 * whose floor is black anyway: fill the backdrop only around it. */
static const ar_ink_t backdrop = {{ 0x80, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00 }};

static void backdrop_fill(const ar_rect_t *r) {
	int x0 = r->x, y0 = r->y, x1 = r->x + r->w, y1 = r->y + r->h;
	int wx0 = WELL_X, wy0 = WELL_Y, wx1 = WELL_X + WELL_W, wy1 = WELL_Y + WELL_H;
	int iy0, iy1;
	if (!in_game() || x1 <= wx0 || x0 >= wx1 || y1 <= wy0 || y0 >= wy1) {
		ar_fill2(x0, y0, x1 - x0, y1 - y0, BK_C_BLACK, BK_C_THEME, &backdrop);
		return;
	}
	iy0 = y0 > wy0 ? y0 : wy0;
	iy1 = y1 < wy1 ? y1 : wy1;
	if (y0 < iy0) ar_fill2(x0, y0, x1 - x0, iy0 - y0, BK_C_BLACK, BK_C_THEME, &backdrop);
	if (y1 > iy1) ar_fill2(x0, iy1, x1 - x0, y1 - iy1, BK_C_BLACK, BK_C_THEME, &backdrop);
	if (x0 < wx0) ar_fill2(x0, iy0, wx0 - x0, iy1 - iy0, BK_C_BLACK, BK_C_THEME, &backdrop);
	if (x1 > wx1) ar_fill2(wx1, iy0, x1 - wx1, iy1 - iy0, BK_C_BLACK, BK_C_THEME, &backdrop);
	{
		int ix0 = x0 > wx0 ? x0 : wx0, ix1 = x1 < wx1 ? x1 : wx1;
		ar_fill(ix0, iy0, ix1 - ix0, iy1 - iy0, BK_C_BLACK);
	}
}

void bk_paint(void *user, const ar_rect_t *r) {
	(void)user;
	backdrop_fill(r);
	if (in_game()) {
		paint_well(r);
		paint_boxes(r);
	} else {
		paint_title(r);
	}
	paint_panel(r);
}

/* ===================================================================
 * dirty marking
 * =================================================================== */

void bk_draw_well_dirty(void) {
	ar_dirty(WELL_X, WELL_Y, WELL_W, WELL_H);
}

void bk_draw_rows_dirty(uint32_t rows) {
	int r;
	for (r = BK_HIDDEN; r < BK_H; r++)
		if (rows & (1u << r)) ar_dirty(WELL_X, ROW_Y(r), WELL_W, BK_CELL);
}

void bk_draw_cell_dirty(int x, int y) {
	if (y < BK_HIDDEN) return;
	ar_dirty(COL_X(x), ROW_Y(y), BK_CELL, BK_CELL);
}

void bk_draw_piece_cells_dirty(const bk_piece_t *p) {
	int8_t c[4][2];
	int i;
	if (!p->type) return;
	bk_cells(p->type, p->rot, c);
	for (i = 0; i < 4; i++) bk_draw_cell_dirty(p->x + c[i][0], p->y + c[i][1]);
}

/* ===================================================================
 * the clear: flash, then the copies
 * =================================================================== */

static int fall_new[BK_H];

void bk_draw_fall_start(void) {
	int r, below = 0;
	for (r = BK_H - 1; r >= 0; r--) {
		if (bk.rows & (1u << r)) { below++; bk.fall_to[r] = 0; continue; }
		bk.fall_to[r] = below * BK_CELL;
		bk.fall_px[r] = 0;
		fall_new[r] = 0;
	}
	bk.copy_failed = false;
	/* the cleared rows go blank now */
	bk_draw_rows_dirty(bk.rows);
}

/* Advance the drop by a frame. The copies happen at the frame boundary
 * (bk_before_paint()); this only says where everything goes next. */
bool bk_draw_fall_step(void) {
	int r;
	uint32_t t = bk.st_frames;
	bool moving = false;
	if (t > FALL_FRAMES) t = FALL_FRAMES;
	for (r = 0; r < BK_H; r++) {
		if (bk.rows & (1u << r)) continue;
		/* accelerating: distance grows as t squared */
		fall_new[r] = (int)((bk.fall_to[r] * t * t + (FALL_FRAMES * FALL_FRAMES) / 2) /
			(FALL_FRAMES * FALL_FRAMES));
		if (fall_new[r] != bk.fall_to[r]) moving = true;
	}
	return moving;
}

void bk_before_paint(void *user) {

	int r;
	(void)user;

	if (bk.st != ST_FALL) return;

	/* Runs of rows falling together, from the bottom up: each moves
	 * down by what it gained this frame, and the strip it leaves is
	 * repainted (where the next run down is now, or the empty floor). */
	r = BK_H - 1;
	while (r >= BK_HIDDEN) {
		int bot, top, d, y, h;
		if ((bk.rows & (1u << r)) || bk.fall_to[r] == 0) { r--; continue; }
		bot = r;
		while (r >= BK_HIDDEN && !(bk.rows & (1u << r)) &&
		       bk.fall_to[r] == bk.fall_to[bot])
			r--;
		top = r + 1;
		d = fall_new[bot] - bk.fall_px[bot];
		if (d <= 0) continue;
		y = ROW_Y(top) + bk.fall_px[bot];
		h = (bot - top + 1) * BK_CELL;
		/* the part already below the well is not there to copy */
		if (y + h > WELL_Y + WELL_H) h = WELL_Y + WELL_H - y;
		if (y + h + d > WELL_Y + WELL_H) h = WELL_Y + WELL_H - y - d;
		/* little pieces: the walls do not fall -- only the columns
		 * between them move */
		{
			int x0 = WELL_X, w = WELL_W;
			if (bk.g.little) { x0 = COL_X(1); w = (BK_W - 2) * BK_CELL; }
			if (h > 0 && !bk.copy_failed &&
			    !ar_copy(x0, y, x0, y + d, w, h))
				bk.copy_failed = true;
			ar_dirty(x0, y, w, d);
		}
	}

	for (r = 0; r < BK_H; r++)
		if (!(bk.rows & (1u << r))) bk.fall_px[r] = fall_new[r];

	/* A copy the window would not allow: repaint the well instead. The
	 * paint draws the same offsets, so the picture is the same. */
	if (bk.copy_failed) bk_draw_well_dirty();
}

/* ===================================================================
 * overlays, every frame
 * =================================================================== */

#define MAX_TRAILS 4
typedef struct { int x, y0, y1, t, c; } trail_t;
static trail_t trails[MAX_TRAILS];

#define MAX_FLASH 4
typedef struct { int x, y, t; } lockflash_t;
static lockflash_t flashes[MAX_FLASH];

/* the title's rain */
#define RAIN 5
typedef struct { int type, x, y, speed, rot; } rain_t;
static rain_t rain[RAIN];

static void rain_reset(rain_t *d, bool anywhere) {
	d->type = BK_I + (int)ar_rand_below(7);
	d->rot = (int)ar_rand_below(4);
	d->x = (int)ar_rand_below(30) * BK_CELL;
	d->y = anywhere ? (int)ar_rand_below(240) - 40 : -40 - (int)ar_rand_below(60);
	d->speed = 96 + (int)ar_rand_below(256);
	d->y *= 256;
}

static bool hidden_by_panel(int x, int y, int w, int h) {
	ar_rect_t p;
	if (bk_panel_rect(&p) && overlaps(&p, x, y, w, h)) return true;
	if (!in_game() &&
	    x < TITLE_X + TITLE_W && TITLE_X < x + w && y < TITLE_Y + TITLE_H &&
	    TITLE_Y < y + h)
		return true;
	return false;
}

/* A sprite drawn only where it is not behind the panel or the title:
 * the parts outside them, as up to a few partial sprites. A falling
 * shape on the title passes behind the menu and the letters a pixel at
 * a time instead of losing a whole block at once, which looked like
 * flicker. */
static void ov_sprite_outside(const ar_sprite_t *s, int x, int y) {
	ar_rect_t block[2], part[16];
	int nb = 0, np = 1, i, j;
	if (bk_panel_rect(&block[nb])) nb++;
	if (!in_game()) {
		block[nb].x = TITLE_X; block[nb].y = TITLE_Y;
		block[nb].w = TITLE_W; block[nb].h = TITLE_H;
		nb++;
	}
	part[0].x = (int16_t)x; part[0].y = (int16_t)y;
	part[0].w = s->w; part[0].h = s->h;
	for (j = 0; j < nb; j++) {
		const ar_rect_t *b = &block[j];
		ar_rect_t out[16];
		int no = 0;
		for (i = 0; i < np; i++) {
			ar_rect_t r = part[i];
			int bx1 = b->x + b->w, by1 = b->y + b->h;
			int rx1 = r.x + r.w, ry1 = r.y + r.h;
			if (!overlaps(b, r.x, r.y, r.w, r.h)) { out[no++] = r; continue; }
			/* above, below, then left and right in between */
			if (r.y < b->y) {
				out[no] = r; out[no].h = (int16_t)(b->y - r.y); no++;
			}
			if (ry1 > by1) {
				out[no] = r; out[no].y = (int16_t)by1;
				out[no].h = (int16_t)(ry1 - by1); no++;
			}
			{
				int my0 = r.y > b->y ? r.y : b->y;
				int my1 = ry1 < by1 ? ry1 : by1;
				if (r.x < b->x) {
					out[no].x = r.x; out[no].y = (int16_t)my0;
					out[no].w = (int16_t)(b->x - r.x); out[no].h = (int16_t)(my1 - my0);
					no++;
				}
				if (rx1 > bx1) {
					out[no].x = (int16_t)bx1; out[no].y = (int16_t)my0;
					out[no].w = (int16_t)(rx1 - bx1); out[no].h = (int16_t)(my1 - my0);
					no++;
				}
			}
			if (no > 12) break;
		}
		for (i = 0; i < no; i++) part[i] = out[i];
		np = no;
	}
	for (i = 0; i < np; i++)
		ar_ov_sprite_part(s, part[i].x - x, part[i].y - y, part[i].w, part[i].h,
			part[i].x, part[i].y);
}

static void piece_overlays(const bk_piece_t *p, int dy_rows, bool ghost) {
	int8_t c[4][2];
	int i;
	bk_cells(p->type, p->rot, c);
	for (i = 0; i < bk_ncells(p->type); i++) {
		int row = p->y + dy_rows + c[i][1];
		int x = COL_X(p->x + c[i][0]);
		int y;
		if (row < BK_HIDDEN) continue;
		y = ROW_Y(row);
		if (hidden_by_panel(x, y, BK_CELL, BK_CELL)) continue;
		ar_ov_sprite(ghost ? &ghost_s[p->type] : &cell_s[p->type], x, y);
	}
}

void bk_draw_frame(uint32_t dt) {

	int i;
	const bk_game_t *g = &bk.g;

	/* -- palette: the theme, breathing with the music --
	 *
	 * Once a bar (16 rows), not on every beat, and gently: it rises
	 * and falls over about a second, to a quarter of the way to a pale
	 * blue. On every beat, sharp, it was a flashing that took the eye
	 * off the well. */
	{
		int32_t rows = zsfx_music_rows();
		uint16_t th = theme_now;
		static int32_t last_bar = -1;
		static int swell;            /* frames into the swell, 0 = none */
		if (rows >= 0 && rows / 16 != last_bar) {
			last_bar = rows / 16;
			swell = 1;
		}
		if (swell > 0) {
			/* up over 20 frames, down over 40 */
			int t = swell < 20 ? swell * 64 / 20 : (60 - swell) * 64 / 40;
			if (t > 0) th = ar_rgb_mix(theme_now, 0x88f, t);
			swell += (int)dt;
			if (swell >= 60) swell = 0;
		}
		ar_pal(BK_C_THEME, th);
	}

	if (say_left > 0) {
		say_left -= (int)dt;
		if (say_left <= 0) ar_dirty(SAY_X, SAY_Y, SAY_W, SAY_H);
	}

	/* -- the rain behind the title -- */
	if (!in_game()) {
		for (i = 0; i < RAIN; i++) {
			rain_t *d = &rain[i];
			int8_t c[4][2];
			int k;
			if (d->type == 0) rain_reset(d, true);
			d->y += d->speed * (int)dt;
			if ((d->y >> 8) > AR_H) rain_reset(d, false);
			bk_cells(d->type, d->rot, c);
			for (k = 0; k < 4; k++) {
				int x = d->x + c[k][0] * BK_CELL, y = (d->y >> 8) + c[k][1] * BK_CELL;
				if (x >= AR_W || y >= AR_H || y < -BK_CELL) continue;
				ov_sprite_outside(&cell_s[d->type], x, y);
			}
		}
	}

	/* -- the piece and its ghost -- */
	if (bk.st == ST_PLAY && g->cur.type) {
		int gy = bk_ghost_y(g);
		if (gy != g->cur.y) {
			bk_piece_t gp = g->cur;
			gp.y = (int8_t)gy;
			piece_overlays(&gp, 0, true);
		}
		piece_overlays(&g->cur, 0, false);
	}

	/* -- hard-drop trails: a streak from where it was to where it is,
	 *    shrinking down into the piece -- */
	for (i = 0; i < MAX_TRAILS; i++) {
		trail_t *t = &trails[i];
		int y0;
		if (t->t <= 0) continue;
		t->t -= (int)dt;
		if (t->t <= 0) continue;
		y0 = t->y1 - ((t->y1 - t->y0) * t->t) / 8;
		if (t->y1 > y0 && !hidden_by_panel(t->x, y0, 2, t->y1 - y0))
			ar_ov_fill(t->x, y0, 2, t->y1 - y0, t->c);
	}

	/* -- the white flash of a block locking -- */
	for (i = 0; i < MAX_FLASH; i++) {
		lockflash_t *f = &flashes[i];
		if (f->t <= 0) continue;
		f->t -= (int)dt;
		if (f->t > 0 && !hidden_by_panel(f->x, f->y, 9, 9))
			ar_ov_fill(f->x, f->y, 9, 9, BK_C_WHITE);
	}

	/* -- rows flashing before they go -- */
	if (bk.st == ST_FLASH && ((bk.st_frames >> 2) & 1) == 0) {
		int r;
		for (r = BK_HIDDEN; r < BK_H; r++)
			if (bk.rows & (1u << r))
				ar_ov_fill(WELL_X, ROW_Y(r), WELL_W - 1, BK_CELL - 1, BK_C_WHITE);
	}

	ar_parts_frame(dt);

	/* the name entry's cursor */
	if (bk.st == ST_NAME && ((ar_ticks() >> 4) & 1)) {
		int x0 = 72 + (176 - ar_text_width("A A A", &z_font_8x8, 3)) / 2;
		ar_ov_fill(x0 + bk.name_pos * 48, 80 + 38 + 25, 24, 2, BK_C_WHITE);
	}
}

/* ===================================================================
 * effects for the rules' events
 * =================================================================== */

static void lock_flash(const bk_piece_t *p);

static void clear_effects(const bk_event_t *e) {

	static const char *const names[5] = { "", "SINGLE", "DOUBLE", "TRIPLE", "FOUR!" };
	char small[24];
	int r, x;

	/* sparks from every block going, in its own colour */
	for (r = BK_HIDDEN; r < BK_H; r++) {
		if (!(bk.g.clear_rows & (1u << r))) continue;
		for (x = bk_col0(&bk.g); x <= bk_col1(&bk.g); x += (e->value >= 3 ? 1 : 2)) {
			int t = bk.g.well[r][x];
			ar_parts_burst(COL_X(x) + 5, ROW_Y(r) + 5, 1, edge_of(t), 1, 380, 14, 18);
		}
	}

	small[0] = '+';
	ar_utoa(small + 1, e->points, 0, ' ');

	if (e->flags & BK_F_PERFECT) {
		bk_say("CLEAR!", small, BK_C_WHITE);
		bk_sound(BK_SND_PERFECT);
		ar_shake(6, 30);
	} else if (e->flags & BK_F_TWIST) {
		bk_say("TWIST", small, 11);
		bk_sound(BK_SND_TWIST);
		ar_shake(3, 16);
	} else if (e->value == 4) {
		bk_say(names[4], small, 10);
		bk_sound(BK_SND_FOUR);
		ar_shake(5, 24);
	} else if (e->value > 0) {
		bk_say(names[e->value], small, 9);
		bk_sound(BK_SND_CLEAR1 + e->value - 1);
	}

	if (e->flags & BK_F_B2B) {
		strcpy(small, "BACK TO BACK");
		bk_say(say_big, small, say_colour);
	} else if (e->combo >= 1) {
		strcpy(small, "COMBO ");
		ar_utoa(small + 6, (uint32_t)e->combo, 0, ' ');
		bk_say(say_big, small, say_colour);
		bk_sound_combo(e->combo);
	}
}

void bk_draw_events(void) {

	int i;
	const bk_game_t *g = &bk.g;

	for (i = 0; i < g->nev; i++) {
		const bk_event_t *e = &g->ev[i];
		switch (e->type) {

		case BK_EV_MOVE:
			bk_sound(BK_SND_MOVE);
			break;

		case BK_EV_ROTATE:
			bk_sound(e->value ? BK_SND_KICK : BK_SND_ROTATE);
			break;

		case BK_EV_HARD_DROP: {
			/* A streak above each column of the piece, from where it
			 * was to where it landed; e has the piece as it landed. */
			int8_t c[4][2];
			int k, n = 0;
			if (e->value > 0 && e->piece) {
				bk_cells(e->piece, e->rot, c);
				for (k = 0; k < 4; k++) {
					int j, top = 99, row;
					for (j = 0; j < 4; j++)
						if (c[j][0] == c[k][0] && c[j][1] < top) top = c[j][1];
					if (c[k][1] != top || n >= MAX_TRAILS) continue;
					row = e->y + top;
					{
						trail_t *t = &trails[n++];
						t->x = COL_X(e->x + c[k][0]) + 4;
						t->y1 = row >= BK_HIDDEN ? ROW_Y(row) : WELL_Y;
						t->y0 = ROW_Y(row - e->value);
						if (t->y0 < WELL_Y) t->y0 = WELL_Y;
						t->t = 8;
						t->c = edge_of(e->piece);
					}
				}
				ar_shake(2, 8);
			}
			bk_sound(BK_SND_HARD);
			break;
		}

		case BK_EV_LOCK: {
			bk_piece_t p;
			p.type = e->piece;
			p.rot = e->rot;
			p.x = e->x;
			p.y = e->y;
			bk_draw_piece_cells_dirty(&p);
			lock_flash(&p);
			bk_sound(BK_SND_LOCK);
			break;
		}

		case BK_EV_HOLD:
			bk_sound(BK_SND_HOLD);
			bk.hud_dirty = true;
			ar_dirty(HOLD_X, HOLD_Y, HOLD_W, HOLD_H);
			break;

		case BK_EV_SPAWN:
			bk.queue_dirty = true;
			ar_dirty(HOLD_X, HOLD_Y, HOLD_W, HOLD_H);
			break;

		case BK_EV_CLEAR:
			clear_effects(e);
			break;

		case BK_EV_LEVEL: {
			char s[12];
			strcpy(s, "LEVEL ");
			ar_utoa(s + 6, (uint32_t)e->value, 0, ' ');
			bk_say("LEVEL", s + 6, 12);
			bk_theme(e->value);
			bk_sound(BK_SND_LEVEL);
			bk_music();
			break;
		}

		case BK_EV_RESCUE: {
			/* easy: the top half swept away -- a wind of sparks
			 * across the well, and the well drawn again */
			char s[12];
			int x;
			for (x = 0; x < BK_W; x++)
				ar_parts_burst(COL_X(x) + 5, ROW_Y(BK_H / 2), 3, 10, 2, 420, 30, -8);
			ar_utoa(s, (uint32_t)e->value, 0, ' ');
			strcat(s, " MORE");
			bk_say("SAVED", e->value ? s : "LAST ONE", 10);
			bk_draw_well_dirty();
			bk.hud_dirty = true;
			bk_sound(BK_SND_RESCUE);
			break;
		}

		default:
			break;
		}
	}
}

/* A locked piece flashes white for a moment. */
static void lock_flash(const bk_piece_t *p) {
	int8_t c[4][2];
	int i;
	bk_cells(p->type, p->rot, c);
	for (i = 0; i < MAX_FLASH; i++) flashes[i].t = 0;
	for (i = 0; i < bk_ncells(p->type) && i < MAX_FLASH; i++) {
		int row = p->y + c[i][1];
		flashes[i].x = COL_X(p->x + c[i][0]);
		flashes[i].y = ROW_Y(row);
		flashes[i].t = row >= BK_HIDDEN ? 4 : 0;
	}
}

void bk_draw_reset(void) {
	int i;
	ar_parts_clear();
	for (i = 0; i < MAX_TRAILS; i++) trails[i].t = 0;
	for (i = 0; i < MAX_FLASH; i++) flashes[i].t = 0;
	for (i = 0; i < BK_H; i++) bk.fall_px[i] = bk.fall_to[i] = 0;
	say_left = 0;
	ar_pal_load(bk_base_palette, 16);
	theme_now = themes[0];
}
