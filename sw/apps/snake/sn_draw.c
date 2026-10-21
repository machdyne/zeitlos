/*
 * snake -- the picture. See docs/snake_app.md, "How it is drawn".
 *
 * The static picture is painted by sn_paint() for whatever rectangle
 * the runtime says is dirty (sw/common/games/zarcade.h): the floor, the
 * walls, every body cell, the status line and any panel. Everything
 * that slides -- the heads, the tails, the food's pulse in monochrome,
 * sparks, the score popups -- is an overlay, submitted every frame and
 * erased by the runtime.
 *
 * -- the body is a tube --
 *
 * A body cell is a 6x6 square in the middle of its 8x8 cell plus a
 * 1-pixel "arm" toward each neighbour it is joined to, so a run of
 * cells is a 6-pixel tube with a 2-pixel gap between parallel runs.
 * The head and the tail tip are sprites that slide along that tube by
 * the move's progress, which is what makes the movement smooth: the
 * cells only change once per move, the two ends every frame.
 *
 * -- colour --
 *
 * Each body cell gets one of four palette entries, in turn, as the head
 * enters it. Those four entries are recomputed every frame (a hue wheel
 * in one-player games, a band of greens or of reds in versus), so the
 * colour flows down the snake with not one cell redrawn. The food
 * pulses, the bonus glints and the sparks flicker the same way.
 */

#include <string.h>

#include "../../common/zfont.h"
#include "../../common/games/zarcade.h"

#include "snake.h"
#include "sn_sound.h"

static const int8_t ddx[4] = { 1, 0, -1, 0 };
static const int8_t ddy[4] = { 0, 1, 0, -1 };

/* ===================================================================
 * sprites
 * =================================================================== */

static const char *const head_t[8] = {
	"........",
	"ccccccc.",
	"cccff0cc",
	"cccccccc",
	"cccccccc",
	"cccff0cc",
	"ccccccc.",
	"........",
};

static const char *const tail_t[8] = {
	"........",
	"....cccc",
	"..cccccc",
	"cccccccc",
	"cccccccc",
	"..cccccc",
	"....cccc",
	"........",
};

static const char *const food_t[8] = {
	"....33..",
	"...3....",
	".ccc.cc.",
	"cfcccccc",
	"cfcccccc",
	"cccccccc",
	".cccccc.",
	"..cc.cc.",
};

static const char *const food_small_t[8] = {
	"........",
	"...33...",
	"..c3cc..",
	".cfcccc.",
	".cfcccc.",
	".cccccc.",
	"..cccc..",
	"........",
};

static const char *const bonus_t[8] = {
	"...d....",
	"...d....",
	"..ddd...",
	"ddfdddd.",
	".ddddd..",
	"..ddd...",
	".dd.dd..",
	"d.....d.",
};

static ar_sprite_t head_s[SN_PLAYERS][4][4];   /* [player][colour][dir] */
static ar_sprite_t tail_s[SN_PLAYERS][4][4];
static ar_sprite_t food_s, food_small_s, bonus_s;

static int hexv(char ch) {
	if (ch >= '0' && ch <= '9') return ch - '0';
	if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
	return -1;
}

/* A right-facing 8x8 template, turned to face `dir`, with 'c' as
 * colour `col`. */
static bool make_turned(ar_sprite_t *s, const char *const *t, int dir, int col) {
	uint8_t px[64];
	int x, y;
	for (y = 0; y < 8; y++)
		for (x = 0; x < 8; x++) {
			char ch;
			int v;
			switch (dir) {
			case SN_DOWN: ch = t[7 - x][y]; break;
			case SN_LEFT: ch = t[7 - y][7 - x]; break;
			case SN_UP:   ch = t[x][7 - y]; break;
			default:      ch = t[y][x]; break;
			}
			v = hexv(ch);
			if (v == 12) v = col;
			px[y * 8 + x] = (v < 0) ? 0xff : (uint8_t)v;
		}
	return ar_sprite_from_pixels(s, px, 8, 8);
}

bool sn_draw_init(void) {
	int p, c, d;
	bool ok = true;
	ar_ov_area(0, SN_PLAY_Y, AR_W, AR_H - SN_PLAY_Y);
	for (p = 0; p < SN_PLAYERS; p++)
		for (c = 0; c < 4; c++)
			for (d = 0; d < 4; d++) {
				int col = (p ? SN_C_P2 : SN_C_P1) + c;
				ok = make_turned(&head_s[p][c][d], head_t, d, col) && ok;
				ok = make_turned(&tail_s[p][c][d], tail_t, d, col) && ok;
			}
	ok = ar_sprite_make(&food_s, food_t, 8, 8, NULL) && ok;
	ok = ar_sprite_make(&food_small_s, food_small_t, 8, 8, NULL) && ok;
	ok = ar_sprite_make(&bonus_s, bonus_t, 8, 8, NULL) && ok;
	return ok;
}

/* ===================================================================
 * popups: "+10" rising from where the food was
 * =================================================================== */

#define POPUPS 6
#define POP_FRAMES 40

typedef struct {
	ar_label_t l;
	uint32_t buf[16];
	int x, y, t;
} popup_t;

static popup_t pops[POPUPS];
static int pop_next;

static void popup(int x, int y, int value) {
	popup_t *p = &pops[pop_next];
	char s[12];
	pop_next = (pop_next + 1) % POPUPS;
	s[0] = '+';
	ar_utoa(s + 1, (uint32_t)value, 0, ' ');
	if (!ar_label_make(&p->l, p->buf, sizeof(p->buf), s, &z_font_5x8, 1))
		return;
	p->x = x + 4 - p->l.w / 2;
	if (p->x < 0) p->x = 0;
	if (p->x + p->l.w > AR_W) p->x = AR_W - p->l.w;
	p->y = y - 2;
	p->t = POP_FRAMES;
}

/* ===================================================================
 * colour
 * =================================================================== */

const uint16_t sn_base_palette[16] = {
	0x000, 0x123, 0x247, 0x6ad,
	0x0f4, 0x0dd, 0x48f, 0xd4f,
	0xf80, 0xfd0, 0xf40, 0xf08,
	0xf31, 0xfd3, 0xfa0, 0xfff
};

/* Hue 0..1535 (six sectors of 256), full value, near-full saturation. */
static uint16_t hue(int h, int sat) {
	int sec, f, r, g, b, lo;
	h %= 1536;
	if (h < 0) h += 1536;
	sec = h >> 8;
	f = h & 255;
	lo = 15 - (15 * sat) / 256;
	switch (sec) {
	case 0: r = 15; g = lo + ((15 - lo) * f) / 256; b = lo; break;
	case 1: r = 15 - ((15 - lo) * f) / 256; g = 15; b = lo; break;
	case 2: r = lo; g = 15; b = lo + ((15 - lo) * f) / 256; break;
	case 3: r = lo; g = 15 - ((15 - lo) * f) / 256; b = 15; break;
	case 4: r = lo + ((15 - lo) * f) / 256; g = lo; b = 15; break;
	default: r = 15; g = lo; b = 15 - ((15 - lo) * f) / 256; break;
	}
	return (uint16_t)((r << 8) | (g << 4) | b);
}

/* A triangle wave over 0..1023 -> 0..256. */
static int tri(int ph) {
	ph &= 1023;
	return ph < 512 ? ph / 2 : (1023 - ph) / 2;
}

static int flash_left, flash_total;
static int celebrate_left;
static uint32_t phase;

void sn_draw_flash(int frames) {
	flash_left = flash_total = frames;
}

void sn_draw_celebrate(int frames) {
	celebrate_left = frames;
}

static void palette_frame(uint32_t dt) {

	int k;
	uint32_t t = ar_ticks();
	int spd = sn.g.s[0].speed ? sn.g.s[0].speed : 100;

	/* The flow speed follows the snake: a fast snake's colours run. */
	phase += dt * (uint32_t)(spd / 8 + 4);

	for (k = 0; k < 4; k++) {
		if (sn.g.players == 2) {
			int a = tri((int)(phase / 2) + k * 256);
			ar_pal(SN_C_P1 + k, hue(400 + a, 220));
			ar_pal(SN_C_P2 + k, hue(1500 + a * 3 / 4, 230));
		} else {
			ar_pal(SN_C_P1 + k, hue((int)phase + k * 384, 210));
			ar_pal(SN_C_P2 + k, hue(1500 + tri((int)phase + k * 256), 230));
		}
	}

	/* the food breathes */
	ar_pal(SN_C_FOOD, ar_rgb_mix(0xe10, 0xf86, tri((int)t * 24)));
	/* the bonus glints */
	ar_pal(SN_C_GOLD, ar_rgb_mix(0xfb0, 0xffe, tri((int)t * 48)));
	/* sparks flicker */
	{
		static const uint16_t fire[4] = { 0xff4, 0xfa0, 0xf60, 0xfff };
		ar_pal(SN_C_FIRE, fire[ar_rand_below(4)]);
	}

	/* a crash: the floor and walls flash red and fade back */
	{
		uint16_t grid = sn_base_palette[SN_C_GRID], wall = sn_base_palette[SN_C_WALL];
		uint16_t light = sn_base_palette[SN_C_LIGHT];
		if (celebrate_left > 0) {
			celebrate_left -= (int)dt;
			wall = hue((int)t * 40, 200);
			light = hue((int)t * 40 + 768, 120);
			grid = ar_rgb_mix(0x123, hue((int)t * 40 + 384, 255), 60);
		}
		if (flash_left > 0) {
			int f = (256 * flash_left) / (flash_total ? flash_total : 1);
			flash_left -= (int)dt;
			grid = ar_rgb_mix(grid, 0x800, f);
			wall = ar_rgb_mix(wall, 0xf20, f);
		}
		ar_pal(SN_C_GRID, grid);
		ar_pal(SN_C_WALL, wall);
		ar_pal(SN_C_LIGHT, light);
	}
}

/* The monochrome picture: what each colour is in black and white. */
void sn_mono_inks(ar_ink_t *ink) {
	int i;
	for (i = 0; i < 16; i++) ink[i] = ar_ink_white;
	ink[SN_C_BG] = ar_ink_black;
	ink[SN_C_GRID] = ar_ink_black;
	ink[SN_C_WALL] = ar_ink_black;      /* walls: white mortar lines */
	for (i = 0; i < 4; i++) ink[SN_C_P2 + i] = ar_ink_grey75;
}

/* ===================================================================
 * the static picture
 * =================================================================== */

static const ar_ink_t brick = {{
	0xff, 0x80, 0x80, 0x80, 0xff, 0x08, 0x08, 0x08
}};

static const ar_ink_t floor_dots = {{
	0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00
}};

static int body_colour(const sn_cell_t *c) {
	return (c->who ? SN_C_P2 : SN_C_P1) + (c->col & 3);
}

static void wall_extent(int cx, int cy, int *x0, int *x1, int *y0, int *y1) {
	*x0 = cx * SN_CELL_PX;
	*x1 = *x0 + SN_CELL_PX;
	*y0 = SN_PLAY_Y + cy * SN_CELL_PX;
	*y1 = *y0 + SN_CELL_PX;
	/* The outer four pixels stay black: the shake moves the viewport
	 * by up to four, and what slides in from the other quadrants then
	 * reads as black too (zarcade.h, ar_shake()). */
	if (cx == 0) *x0 += 4;
	if (cx == SN_GW - 1) *x1 -= 4;
	if (cy == SN_GH - 1) *y1 -= 4;
}

static bool is_tail_cell(uint16_t c, const sn_cell_t *cell) {
	const sn_snake_t *s = &sn.g.s[cell->who];
	return s->len >= 2 && s->body[s->tail] == c;
}

static void paint_body(uint16_t c, const sn_cell_t *cell) {
	int x = SN_PX(c), y = SN_PY(c), col = body_colour(cell);
	/* The tail cell is the tail tip's: an overlay draws it as it
	 * slides (sn_draw_frame()). */
	if (is_tail_cell(c, cell)) return;
	ar_fill(x + 1, y + 1, 6, 6, col);
	if (cell->links & (1u << SN_RIGHT)) ar_fill(x + 7, y + 1, 1, 6, col);
	if (cell->links & (1u << SN_LEFT))  ar_fill(x,     y + 1, 1, 6, col);
	if (cell->links & (1u << SN_DOWN))  ar_fill(x + 1, y + 7, 6, 1, col);
	if (cell->links & (1u << SN_UP))    ar_fill(x + 1, y,     6, 1, col);
}

/* -- the status line -- */

/* A heart, 7x6, for easy's lives (zgfx layout: LSB leftmost). */
static const uint32_t heart_bits[6] = {
	0x36, 0x7f, 0x7f, 0x3e, 0x1c, 0x08
};

static void hud_hearts(int x, int n) {
	int i;
	for (i = 0; i < n && i < SN_HEARTS_MAX; i++)
		ar_bitmap(heart_bits, 4, 0, 0, x + i * 9, 5, 7, 6, SN_C_FOOD);
}

static void hud_text(int col, const char *s, int c) {
	ar_text(col * 8, 4, s, c, &z_font_8x8, 1);
}

static void paint_hud(void) {

	char v[12];
	const sn_game_t *g = &sn.g;

	ar_fill(0, 0, AR_W, SN_PLAY_Y, SN_C_BG);

	if (sn.st == ST_TITLE || sn.st == ST_SCORES) {
		hud_text(1, "HI", SN_C_LIGHT);
		ar_utoa(v, sn.hiscore, 6, '0');
		hud_text(4, v, SN_C_WHITE);
		hud_text(28, "F1 HELP", SN_C_LIGHT);
		return;
	}

	if (g->players == 2) {
		int i;
		char w[SN_WINS_NEEDED + 1];
		hud_text(1, "P1", SN_C_P1);
		for (i = 0; i < SN_WINS_NEEDED; i++) w[i] = i < g->s[0].wins ? '*' : '.';
		w[SN_WINS_NEEDED] = 0;
		hud_text(4, w, SN_C_WHITE);
		if (g->s[0].hearts || g->s[1].hearts) {
			/* easy: each player's hearts, inside their side */
			hud_hearts(64, g->s[0].hearts);
			hud_hearts(256 - 9 * g->s[1].hearts - 8, g->s[1].hearts);
		} else {
			hud_text(14, "FIRST TO 3", SN_C_LIGHT);
		}
		hud_text(32, "P2", SN_C_P2);
		for (i = 0; i < SN_WINS_NEEDED; i++) w[i] = i < g->s[1].wins ? '*' : '.';
		hud_text(35, w, SN_C_WHITE);
		return;
	}

	hud_text(1, "SCORE", SN_C_LIGHT);
	ar_utoa(v, g->s[0].score, 6, '0');
	hud_text(7, v, SN_C_WHITE);

	if (g->s[0].hearts) {
		/* easy: the hearts where the high score was */
		hud_hearts(120, g->s[0].hearts);
	} else {
		hud_text(15, "HI", SN_C_LIGHT);
		ar_utoa(v, g->s[0].score > sn.hiscore ? g->s[0].score : sn.hiscore, 6, '0');
		hud_text(18, v, SN_C_WHITE);
	}

	if (g->mode == SN_MODE_MAZE) {
		hud_text(27, "LEVEL", SN_C_LIGHT);
		ar_utoa(v, (uint32_t)g->level + 1, 2, ' ');
		hud_text(33, v, SN_C_WHITE);
		ar_utoa(v, (uint32_t)(g->level_goal > g->s[0].eaten ?
			g->level_goal - g->s[0].eaten : 0), 2, ' ');
		hud_text(36, v, SN_C_FOOD);
	} else {
		hud_text(28, "LEN", SN_C_LIGHT);
		ar_utoa(v, g->s[0].len, 4, ' ');
		hud_text(32, v, SN_C_WHITE);
	}
}

/* -- panels: the boxes of text over the field -- */

typedef struct {
	int y;
	const char *s;
	int c;
	const z_font_t *f;
	int scale;
} line_t;

#define MAX_LINES 16

static char tbuf[MAX_LINES][24];

static int panel(ar_rect_t *r, line_t *ln) {

	int n = 0, i;
	const sn_game_t *g = &sn.g;

#define LINE(yy, ss, cc, ff, sc) do { \
		ln[n].y = (yy); ln[n].s = (ss); ln[n].c = (cc); \
		ln[n].f = (ff); ln[n].scale = (sc); n++; } while (0)

	switch (sn.st) {

	case ST_TITLE:
		r->x = 56; r->y = 48; r->w = 208; r->h = 160;
		LINE(12, "SNAKE", SN_C_GOLD, &z_font_8x8, 4);
		for (i = 0; i < MENU_ITEMS; i++) {
			static const char *const items[MENU_ITEMS] = {
				"", "CLASSIC", "OPEN FIELD", "MAZES", "TWO PLAYERS",
				"HIGH SCORES"
			};
			bool sel = (i == sn.menu);
			char *b = tbuf[i];
			if (i == MENU_SKILL) {
				/* "< EASY >": Left and Right change it */
				strcpy(b, sel ? "< " : "  ");
				strcat(b, ar_skill_name(ar_skill()));
				strcat(b, sel ? " >" : "  ");
				LINE(52, b, sel ? SN_C_WHITE : SN_C_GOLD, &z_font_8x8, 1);
				continue;
			}
			b[0] = sel ? '>' : ' ';
			b[1] = ' ';
			strcpy(b + 2, items[i]);
			strcat(b, sel ? " <" : "  ");
			LINE(56 + i * 13, b, sel ? SN_C_WHITE : SN_C_LIGHT,
				&z_font_8x8, 1);
		}
		LINE(134, "ENTER OR START TO PLAY", SN_C_LIGHT, &z_font_5x8, 1);
		LINE(146, "F11 FULL SCREEN   M MUSIC", SN_C_LIGHT, &z_font_5x8, 1);
		return n;

	case ST_SCORES: {
		ar_table_t t;
		r->x = 56; r->y = 48; r->w = 208; r->h = 160;
		LINE(4, sn_mode_names[sn.score_view], SN_C_GOLD, &z_font_8x8, 2);
		LINE(22, ar_skill_name(ar_skill()), SN_C_LIGHT, &z_font_5x8, 1);
		t = sn.table;
		for (i = 0; i < AR_SCORES; i++) {
			char *b = tbuf[i];
			ar_utoa(b, (uint32_t)i + 1, 2, ' ');
			strcat(b, ". ");
			if (i < t.n) {
				char v[12];
				strcat(b, t.e[i].who);
				strcat(b, "  ");
				ar_utoa(v, t.e[i].score, 6, '0');
				strcat(b, v);
			} else {
				strcat(b, "---  ------");
			}
			LINE(32 + i * 10, b, i == sn.rank ? SN_C_WHITE : SN_C_LIGHT,
				&z_font_8x8, 1);
		}
		LINE(140, "LEFT/RIGHT MODE  ENTER BACK", SN_C_LIGHT, &z_font_5x8, 1);
		return n;
	}

	case ST_READY:
		r->x = 80; r->y = 104; r->w = 160; r->h = 48;
		if (g->mode == SN_MODE_MAZE) {
			strcpy(tbuf[0], "LEVEL ");
			ar_utoa(tbuf[0] + 6, (uint32_t)g->level + 1, 0, ' ');
			LINE(8, tbuf[0], SN_C_GOLD, &z_font_8x8, 2);
		} else if (g->players == 2) {
			strcpy(tbuf[0], "ROUND ");
			ar_utoa(tbuf[0] + 6, (uint32_t)(g->s[0].wins + g->s[1].wins) + 1, 0, ' ');
			LINE(8, tbuf[0], SN_C_GOLD, &z_font_8x8, 2);
		} else {
			LINE(8, "GET READY", SN_C_GOLD, &z_font_8x8, 2);
		}
		LINE(32, g->players == 2 ? "P1 ARROWS   P2 W A S D" : "ARROWS, W A S D OR PAD",
			SN_C_LIGHT, &z_font_5x8, 1);
		return n;

	case ST_PAUSE:
		r->x = 96; r->y = 104; r->w = 128; r->h = 48;
		LINE(8, "PAUSED", SN_C_GOLD, &z_font_8x8, 2);
		LINE(30, "P OR START: GO ON", SN_C_LIGHT, &z_font_5x8, 1);
		LINE(39, "ESC: MENU", SN_C_LIGHT, &z_font_5x8, 1);
		return n;

	case ST_LEVEL:
		r->x = 72; r->y = 104; r->w = 176; r->h = 48;
		LINE(8, "LEVEL CLEAR", SN_C_GOLD, &z_font_8x8, 2);
		LINE(32, "ON TO THE NEXT", SN_C_LIGHT, &z_font_5x8, 1);
		return n;

	case ST_ROUND:
		r->x = 56; r->y = 96; r->w = 208; r->h = 64;
		if (sn.round_winner < 0) strcpy(tbuf[0], "A DRAW");
		else {
			strcpy(tbuf[0], "PLAYER ");
			tbuf[0][7] = (char)('1' + sn.round_winner);
			tbuf[0][8] = 0;
		}
		LINE(8, tbuf[0], sn.round_winner == 1 ? SN_C_P2 :
			sn.round_winner == 0 ? SN_C_P1 : SN_C_GOLD, &z_font_8x8, 2);
		if (g->s[0].wins >= SN_WINS_NEEDED || g->s[1].wins >= SN_WINS_NEEDED)
			LINE(30, "WINS THE MATCH!", SN_C_GOLD, &z_font_8x8, 1);
		else
			LINE(30, sn.round_winner < 0 ? "NOBODY SCORES" : "WINS THE ROUND",
				SN_C_WHITE, &z_font_8x8, 1);
		strcpy(tbuf[1], "P1 ");
		ar_utoa(tbuf[1] + 3, g->s[0].wins, 0, ' ');
		strcat(tbuf[1], " - ");
		ar_utoa(tbuf[1] + strlen(tbuf[1]), g->s[1].wins, 0, ' ');
		strcat(tbuf[1], " P2");
		LINE(46, tbuf[1], SN_C_LIGHT, &z_font_8x8, 1);
		return n;

	case ST_OVER:
		r->x = 72; r->y = 96; r->w = 176; r->h = 64;
		LINE(8, "GAME OVER", SN_C_GOLD, &z_font_8x8, 2);
		strcpy(tbuf[0], "SCORE ");
		ar_utoa(tbuf[0] + 6, g->s[0].score, 6, '0');
		LINE(30, tbuf[0], SN_C_WHITE, &z_font_8x8, 1);
		LINE(46, "ENTER OR START", SN_C_LIGHT, &z_font_5x8, 1);
		return n;

	case ST_NAME:
		r->x = 64; r->y = 80; r->w = 192; r->h = 96;
		LINE(8, "NEW HIGH SCORE", SN_C_GOLD, &z_font_8x8, 1);
		strcpy(tbuf[0], "RANK ");
		ar_utoa(tbuf[0] + 5, (uint32_t)sn.rank + 1, 0, ' ');
		strcat(tbuf[0], "   ");
		ar_utoa(tbuf[0] + strlen(tbuf[0]), g->s[0].score, 6, '0');
		LINE(22, tbuf[0], SN_C_WHITE, &z_font_8x8, 1);
		tbuf[1][0] = sn.name[0];
		tbuf[1][1] = ' ';
		tbuf[1][2] = sn.name[1];
		tbuf[1][3] = ' ';
		tbuf[1][4] = sn.name[2];
		tbuf[1][5] = 0;
		LINE(38, tbuf[1], SN_C_WHITE, &z_font_8x8, 3);
		LINE(78, "UP/DOWN LETTER  ENTER DONE", SN_C_LIGHT, &z_font_5x8, 1);
		return n;

	default:
		return 0;
	}
#undef LINE
}

/* The panel's rectangle, for marking it dirty from snake.c. */
bool sn_panel_rect(ar_rect_t *r) {
	line_t ln[MAX_LINES];
	return panel(r, ln) > 0;
}

static bool overlaps(const ar_rect_t *a, const ar_rect_t *b) {
	return a->x < b->x + b->w && b->x < a->x + a->w &&
		a->y < b->y + b->h && b->y < a->y + a->h;
}

static void paint_panel(const ar_rect_t *dirty) {
	ar_rect_t r;
	line_t ln[MAX_LINES];
	int n = panel(&r, ln), i;
	if (n == 0 || !overlaps(&r, dirty)) return;
	ar_fill(r.x, r.y, r.w, r.h, SN_C_BG);
	ar_fill(r.x + 1, r.y + 1, r.w - 2, 1, SN_C_LIGHT);
	ar_fill(r.x + 1, r.y + r.h - 2, r.w - 2, 1, SN_C_LIGHT);
	ar_fill(r.x + 1, r.y + 1, 1, r.h - 2, SN_C_LIGHT);
	ar_fill(r.x + r.w - 2, r.y + 1, 1, r.h - 2, SN_C_LIGHT);
	for (i = 0; i < n; i++) {
		int w = ar_text_width(ln[i].s, ln[i].f, ln[i].scale);
		int y = r.y + ln[i].y;
		ar_rect_t lr;
		lr.x = (int16_t)(r.x + (r.w - w) / 2); lr.y = (int16_t)y;
		lr.w = (int16_t)w; lr.h = (int16_t)(ln[i].f->h * ln[i].scale);
		/* Rendering text is the expensive part of a paint: skip lines
		 * the dirty rectangle does not reach. */
		if (!overlaps(&lr, dirty)) continue;
		ar_text(lr.x, y, ln[i].s, ln[i].c, ln[i].f, ln[i].scale);
	}
}

/* The paint callback: everything static inside r. */
void sn_paint(void *user, const ar_rect_t *r) {

	const sn_game_t *g = &sn.g;
	int cx0, cy0, cx1, cy1, cx, cy;
	int ry0 = r->y, ry1 = r->y + r->h;

	(void)user;

	if (ry0 < SN_PLAY_Y) paint_hud();

	if (ry1 > SN_PLAY_Y) {

		int fy0 = ry0 < SN_PLAY_Y ? SN_PLAY_Y : ry0;

		/* The floor in one fill: two colours through a sparse dot,
		 * anchored to the screen so every repaint matches. */
		ar_fill2(r->x, fy0, r->w, ry1 - fy0, SN_C_BG, SN_C_GRID, &floor_dots);

		cx0 = r->x / SN_CELL_PX;
		cx1 = (r->x + r->w - 1) / SN_CELL_PX;
		cy0 = (fy0 - SN_PLAY_Y) / SN_CELL_PX;
		cy1 = (ry1 - 1 - SN_PLAY_Y) / SN_CELL_PX;
		if (cx1 >= SN_GW) cx1 = SN_GW - 1;
		if (cy1 >= SN_GH) cy1 = SN_GH - 1;

		for (cy = cy0; cy <= cy1; cy++) {
			int run = -1;
			for (cx = cx0; cx <= cx1 + 1; cx++) {
				bool wall = cx <= cx1 &&
					g->grid[sn_cell(cx, cy)].what == SN_WALL;
				if (wall && run < 0) run = cx;
				if (!wall && run >= 0) {
					/* A run of wall cells is one fill. */
					int ax0, ax1, ay0, ay1, bx0, bx1, by0, by1;
					wall_extent(run, cy, &ax0, &ax1, &ay0, &ay1);
					wall_extent(cx - 1, cy, &bx0, &bx1, &by0, &by1);
					ar_fill2(ax0, ay0, bx1 - ax0, ay1 - ay0,
						SN_C_WALL, SN_C_LIGHT, &brick);
					run = -1;
				}
				if (cx <= cx1) {
					uint16_t c = sn_cell(cx, cy);
					const sn_cell_t *cell = &g->grid[c];
					if (cell->what == SN_BODY && sn.st != ST_SCORES)
						paint_body(c, cell);
				}
			}
		}
	}

	paint_panel(r);
}

/* ===================================================================
 * dirty marking
 * =================================================================== */

void sn_draw_cell_dirty(uint16_t c) {
	if (c == SN_NONE) return;
	ar_dirty(SN_PX(c), SN_PY(c), SN_CELL_PX, SN_CELL_PX);
}

static uint16_t last_head[SN_PLAYERS], last_tail[SN_PLAYERS];
static uint16_t last_t1[SN_PLAYERS], last_len[SN_PLAYERS];
static uint16_t last_next[SN_PLAYERS];

void sn_draw_dirty_snakes(void) {
	int p;
	for (p = 0; p < SN_PLAYERS; p++) {
		const sn_snake_t *s = &sn.g.s[p];
		uint16_t h, t, t1;
		if (s->len != last_len[p]) sn.hud_dirty = true;
		last_len[p] = s->len;
		/* A move starting adds the head cell's arm toward the next
		 * cell: the head did not change, but its cell did. */
		if (s->next != last_next[p]) {
			if (s->len) sn_draw_cell_dirty(s->body[s->head]);
			last_next[p] = s->next;
		}
		if (!s->active || s->len == 0) continue;
		h = s->body[s->head];
		t = s->body[s->tail];
		t1 = s->len >= 2 ? sn_segment(s, 1) : SN_NONE;
		/* Only when something moved on: a cell that changed what it
		 * is (became body, stopped being one, became the tail) is
		 * one of these, now or a frame ago. */
		if (h != last_head[p] || t != last_tail[p] || t1 != last_t1[p]) {
			sn_draw_cell_dirty(h);
			sn_draw_cell_dirty(last_head[p]);
			sn_draw_cell_dirty(t);
			sn_draw_cell_dirty(last_tail[p]);
			sn_draw_cell_dirty(t1);
			sn_draw_cell_dirty(last_t1[p]);
			if (s->len >= 2) sn_draw_cell_dirty(sn_segment(s, s->len - 2));
		}
		last_head[p] = h;
		last_tail[p] = t;
		last_t1[p] = t1;
	}
}

/* ===================================================================
 * overlays, every frame
 * =================================================================== */

/* A sprite at a cell's position moved `px` pixels in direction d; and
 * if the move crosses the wrap (a and b not side by side), the copy
 * coming in at the other edge as well. Nothing above the field: the
 * status line is not the field's to draw on. */
static ar_rect_t panel_now;
static bool panel_up;

/* Overlays never draw over the panel: it is painted, they are drawn
 * after, and a star on top of NEW HIGH SCORE is not the effect. Nor
 * over the status line. */
static bool hidden(int x, int y, int w, int h) {
	/* Above the field: the runtime cuts overlays at the field's top
	 * (ar_ov_area(), set in sn_draw_init()), so a head sliding out of
	 * the top of an open field shows the part still inside it. */
	if (y + h <= SN_PLAY_Y) return true;
	if (!panel_up) return false;
	return x < panel_now.x + panel_now.w && panel_now.x < x + w &&
		y < panel_now.y + panel_now.h && panel_now.y < y + h;
}

static void ov_sprite(const ar_sprite_t *s, int x, int y) {
	if (!hidden(x, y, s->w, s->h)) ar_ov_sprite(s, x, y);
}

static void sliding(const ar_sprite_t *s, uint16_t a, uint16_t b, int d, int px) {
	int x = SN_PX(a) + ddx[d] * px, y = SN_PY(a) + ddy[d] * px;
	ov_sprite(s, x, y);
	if (b != SN_NONE) {
		int dx = sn_cx(b) - sn_cx(a), dy = sn_cy(b) - sn_cy(a);
		if (dx > 1 || dx < -1 || dy > 1 || dy < -1) {
			x = SN_PX(b) - ddx[d] * (SN_CELL_PX - px);
			y = SN_PY(b) - ddy[d] * (SN_CELL_PX - px);
			ov_sprite(s, x, y);
		}
	}
}

static void snake_overlays(int p) {

	const sn_snake_t *s = &sn.g.s[p];
	int px = (s->alive && s->next != SN_NONE) ? (s->progress >> 8) : 0;
	uint16_t h, t;

	if (!s->active || s->len == 0) return;
	if (px >= SN_CELL_PX) px = SN_CELL_PX - 1;

	h = s->body[s->head];
	t = s->body[s->tail];

	if (s->len >= 2) {
		uint16_t t1 = sn_segment(s, 1);
		int d = sn_dir_between(&sn.g, t, t1);
		const sn_cell_t *tc = &sn.g.grid[t];
		int tp = s->tail_moves ? px : 0;
		if (d != SN_NODIR)
			sliding(&tail_s[p][tc->col & 3][d], t, t1, d, tp);
	}

	{
		const sn_cell_t *hc = &sn.g.grid[h];
		int hd = s->dir;
		/* A dead snake's head is wherever the dissolve has reached:
		 * it faces away from the segment behind it. */
		if (!s->alive && s->len >= 2) {
			int d = sn_dir_between(&sn.g, sn_segment(s, s->len - 2), h);
			if (d != SN_NODIR) hd = d;
		}
		sliding(&head_s[p][hc->col & 3][hd], h, s->next, hd, px);

		/* A flick of the tongue when food is close. */
		if (s->alive && sn.g.food != SN_NONE &&
		    ((ar_ticks() >> 3) & 7) == 0) {
			int dx = sn_cx(sn.g.food) - sn_cx(h);
			int dy = sn_cy(sn.g.food) - sn_cy(h);
			if (dx * dx + dy * dy <= 16) {
				int x = SN_PX(h) + ddx[s->dir] * px + 3 + ddx[s->dir] * 5;
				int y = SN_PY(h) + ddy[s->dir] * px + 3 + ddy[s->dir] * 5;
				if (!hidden(x, y, 2, 2))
					ar_ov_fill(x, y, ddx[s->dir] ? 2 : 1, ddy[s->dir] ? 2 : 1,
						SN_C_FOOD);
			}
		}
	}
}

void sn_draw_frame(uint32_t dt) {

	const sn_game_t *g = &sn.g;
	int i;
	bool on_field = sn.st != ST_SCORES;

	palette_frame(dt);
	panel_up = sn_panel_rect(&panel_now) && sn.st != ST_TITLE;

	if (on_field && g->food != SN_NONE) {
		/* In colour the food pulses through the palette; in mono it
		 * swaps between two drawings. */
		const ar_sprite_t *f = &food_s;
		if (!ar_color() && ((ar_ticks() >> 4) & 1)) f = &food_small_s;
		ov_sprite(f, SN_PX(g->food), SN_PY(g->food));
	}

	if (on_field && g->bonus != SN_NONE) {
		/* Blinks in its last two seconds. */
		bool show = g->bonus_left > 120 || ((ar_ticks() >> 2) & 1);
		if (show) ov_sprite(&bonus_s, SN_PX(g->bonus), SN_PY(g->bonus));
		/* Its time left: a gold line across the top, shrinking to
		 * the middle. */
		if (sn.st == ST_PLAY && g->bonus_total) {
			int w = (312 * g->bonus_left) / g->bonus_total;
			if (w > 0) ar_ov_fill(4 + (312 - w) / 2, 1, w, 1, SN_C_GOLD);
		}
	}

	if (on_field)
		for (i = 0; i < SN_PLAYERS; i++) snake_overlays(i);

	/* Easy: a stopped snake shows where it could go -- blinking dots
	 * in the free cells beside its head. */
	if (sn.st == ST_PLAY && ((ar_ticks() >> 4) & 1))
		for (i = 0; i < SN_PLAYERS; i++) {
			const sn_snake_t *s = &g->s[i];
			int d;
			if (!s->active || !s->alive || !s->stopped) continue;
			for (d = 0; d < 4; d++) {
				uint16_t n = sn_neighbour(g, s->body[s->head], d);
				int x, y;
				if (n == SN_NONE || g->grid[n].what != SN_EMPTY) continue;
				x = SN_PX(n) + 3;
				y = SN_PY(n) + 3;
				if (!hidden(x, y, 2, 2)) ar_ov_fill(x, y, 2, 2, SN_C_WHITE);
			}
		}

	ar_parts_frame(dt);

	for (i = 0; i < POPUPS; i++) {
		popup_t *p = &pops[i];
		if (p->t <= 0) continue;
		p->t -= (int)dt;
		if (p->t <= 0) continue;
		{
			int y = p->y - (POP_FRAMES - p->t) / 2;
			if (!hidden(p->x, y, p->l.w, p->l.h))
				ar_ov_label(&p->l, p->x, y,
					p->t > 8 ? SN_C_GOLD : SN_C_LIGHT);
		}
	}

	/* The name entry's blinking cursor. */
	if (sn.st == ST_NAME && ((ar_ticks() >> 4) & 1)) {
		int x0 = 64 + (192 - ar_text_width("A A A", &z_font_8x8, 3)) / 2;
		ar_ov_fill(x0 + sn.name_pos * 48, 80 + 38 + 25, 24, 2, SN_C_WHITE);
	}
}

/* ===================================================================
 * effects for the rules' events
 * =================================================================== */

void sn_draw_events(void) {

	int i;
	const sn_game_t *g = &sn.g;
	bool quiet = (sn.st == ST_TITLE);

	for (i = 0; i < g->nev; i++) {
		const sn_event_t *e = &g->ev[i];
		int x = SN_PX(e->cell) + 4, y = SN_PY(e->cell) + 4;

		switch (e->type) {

		case SN_EV_EAT:
			ar_parts_burst(x, y, quiet ? 6 : 14, SN_C_FOOD, 3, 320, 24, 0);
			if (!quiet) {
				popup(x - 4, y - 4, e->value);
				sn_sound(SN_SND_EAT, x);
				sn.hud_dirty = true;
			}
			break;

		case SN_EV_BONUS_EAT:
			ar_parts_burst(x, y, 28, SN_C_GOLD, 2, 420, 34, 8);
			popup(x - 4, y - 4, e->value);
			sn_sound(SN_SND_BONUS_EAT, x);
			sn.hud_dirty = true;
			break;

		case SN_EV_BONUS_SHOW:
			ar_parts_burst(x, y, 10, SN_C_GOLD, 1, 200, 16, 0);
			sn_sound(SN_SND_BONUS_SHOW, x);
			break;

		case SN_EV_BONUS_GONE:
			sn_sound(SN_SND_BONUS_GONE, x);
			break;

		case SN_EV_DIE:
			ar_parts_burst(x, y, 30, SN_C_FIRE, 1, 480, 40, 10);
			if (!quiet) {
				ar_shake(4, 26);
				sn_draw_flash(30);
				sn_sound(SN_SND_DIE_CRASH, x);
				sn_sound(SN_SND_DIE_FALL, x);
			}
			break;

		case SN_EV_FOOD:
			sn_draw_cell_dirty(e->cell);
			break;

		case SN_EV_BUMP:
			/* easy: a bump, not a crash -- a few stars and a soft
			 * sound, and a heart less */
			ar_parts_burst(x, y, 12, SN_C_GOLD, 2, 260, 26, 0);
			sn_draw_flash(8);
			ar_shake(2, 10);
			sn_sound(SN_SND_BUMP, x);
			sn.hud_dirty = true;
			break;

		case SN_EV_BONK:
			sn_sound(SN_SND_BONK, x);
			break;

		case SN_EV_HEART:
			ar_parts_burst(x, y, 16, SN_C_FOOD, 1, 300, 30, 0);
			sn_sound(SN_SND_HEART, x);
			sn.hud_dirty = true;
			break;

		default:
			break;
		}
	}
}

void sn_draw_pop(uint16_t c, int p) {
	int x = SN_PX(c) + 4, y = SN_PY(c) + 4;
	const sn_cell_t *cell = &sn.g.grid[c];
	(void)cell;
	ar_parts_burst(x, y, 4, (p ? SN_C_P2 : SN_C_P1), 4, 260, 22, 12);
	sn_draw_cell_dirty(c);
}

void sn_draw_reset(void) {
	int i;
	ar_parts_clear();
	for (i = 0; i < POPUPS; i++) pops[i].t = 0;
	flash_left = 0;
	celebrate_left = 0;
	for (i = 0; i < SN_PLAYERS; i++)
		last_head[i] = last_tail[i] = last_t1[i] = last_next[i] = SN_NONE;
	ar_pal_load(sn_base_palette, 16);
}
