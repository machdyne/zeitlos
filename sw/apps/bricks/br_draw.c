/*
 * bricks -- the picture. See docs/bricks_app.md, "How it is drawn".
 *
 * The static picture -- the status line, the walls, the backdrop, the
 * bricks, the panels -- is painted by br_paint() for whatever rectangle
 * the runtime says is dirty (sw/common/games/zarcade.h). Everything that
 * moves is an overlay: the paddle, the balls and their trails, capsules,
 * bolts, the flash of a brick being hit, sparks, the words a capsule
 * says, and the title's bouncing balls. A brick that breaks is one dirty
 * 16x8 rectangle -- exactly two of the runtime's tiles.
 *
 * In colour the palette does the rest: the gold bricks glint now and
 * then, the paddle's light flashes when it hits, the bolts and capsules
 * gleam, the backdrop breathes with the music, flashes red when a
 * paddle is lost and white when a round is clear, and on the title the
 * six brick colours chase each other through the letters. None of that
 * redraws a pixel.
 */

#include <string.h>

#include "../../common/zfont.h"
#include "../../common/games/zarcade.h"
#include "../../common/games/zsfx.h"

#include "bricks.h"
#include "br_sound.h"

/* ===================================================================
 * colour
 * =================================================================== */

const uint16_t br_base_palette[16] = {
	0x000, 0x124, 0x568, 0x9bd, 0xe33, 0xf92, 0xed3, 0x4c4,
	0x3ce, 0x56f, 0xccd, 0xdb3, 0x47a, 0xfa3, 0x6ef, 0xfff
};

/* The backdrop's colour and pattern for each round, dark enough that a
 * ball is always the brightest thing on it. */
static const uint16_t themes[12] = {
	0x124, 0x214, 0x142, 0x231, 0x123, 0x312,
	0x132, 0x213, 0x321, 0x114, 0x411, 0x222
};

static const ar_ink_t patterns[4] = {
	{{ 0x80, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00 }},   /* dots */
	{{ 0xff, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 }},   /* grid */
	{{ 0x88, 0x00, 0x00, 0x00, 0x88, 0x00, 0x00, 0x00 }},   /* dense dots */
	{{ 0x10, 0x10, 0x28, 0xc6, 0x28, 0x10, 0x10, 0x00 }},   /* stars */
};

static int theme_i, pattern_i;

void br_draw_level(int level) {
	theme_i = (level - 1) % 12;
	pattern_i = (level - 1) % 4;
}

void br_mono_inks(ar_ink_t *ink) {
	int i;
	for (i = 0; i < 16; i++) ink[i] = ar_ink_white;
	ink[BR_C_BLACK] = ar_ink_black;
	ink[BR_C_WALL] = ar_ink_grey50;
	ink[BR_C_B1 + 1] = ar_ink_grey75;
	ink[BR_C_B1 + 2] = ar_ink_vlines;
	ink[BR_C_B1 + 3] = ar_ink_grey50;
	ink[BR_C_B1 + 4] = ar_ink_hlines;
	ink[BR_C_B1 + 5] = ar_ink_grey25;
	ink[BR_C_SILVER] = ar_ink_dots;
	ink[BR_C_GOLD] = ar_ink_diag;
	ink[BR_C_PADDLE] = ar_ink_grey75;
}

/* ===================================================================
 * sprites
 * =================================================================== */

static ar_sprite_t brick_s[BR_TYPES];      /* by type */
static ar_sprite_t cracked_s;              /* silver, hit */
static ar_sprite_t ball_s;
/* Three lengths (32 normal, 48 with E -- the normal one on easy -- and
 * 64, easy with E); the light green while catching, emitters while
 * firing, at the two shorter lengths. */
enum { PAD_NORMAL = 0, PAD_WIDE, PAD_CATCH, PAD_LASER,
	PAD_WIDE_CATCH, PAD_WIDE_LASER, PAD_EASY_WIDE, PADS };
static ar_sprite_t paddle_s[PADS];
static ar_sprite_t cap_s[BR_CAPS][2];      /* two frames: it rolls */
static ar_sprite_t mini_s;                 /* a spare paddle, status line */

static const uint8_t cap_colour[BR_CAPS] = {
	0, BR_C_B1 + 5, BR_C_B1 + 4, BR_C_B1 + 1, BR_C_B1 + 3, BR_C_B1, BR_C_SILVER
};
static const char *const cap_word[BR_CAPS] = {
	"", "WIDE", "MULTI", "SLOW", "CATCH", "LASER", "1UP"
};

static const char *const letter5[6][5] = {
	{ "11111", "1....", "1111.", "1....", "11111" },   /* E */
	{ "1...1", "11.11", "1.1.1", "1...1", "1...1" },   /* M */
	{ ".1111", "1....", ".111.", "....1", "1111." },   /* S */
	{ ".1111", "1....", "1....", "1....", ".1111" },   /* C */
	{ "1....", "1....", "1....", "1....", "11111" },   /* L */
	{ "1111.", "1...1", "1111.", "1....", "1...." },   /* P */
};

static int brick_colour(int t) {
	if (t >= BR_C1 && t <= BR_C6) return BR_C_B1 + t - BR_C1;
	return t == BR_SILVER ? BR_C_SILVER : BR_C_GOLD;
}

/* A brick: a light top and left edge, a dark bottom and right one (the
 * gap between bricks), a glint. Every pixel set, so a brick painted
 * over anything covers it. */
static bool make_brick(ar_sprite_t *s, int t, bool cracked) {
	uint8_t px[BR_BW * BR_BH];
	int x, y, b = brick_colour(t);
	for (y = 0; y < BR_BH; y++)
		for (x = 0; x < BR_BW; x++) {
			uint8_t v = (uint8_t)b;
			if (y == BR_BH - 1 || x == BR_BW - 1) v = BR_C_BLACK;
			else if (y == 0 || x == 0) v = BR_C_WHITE;
			else if ((y == 2 && (x == 2 || x == 3 || x == 4)) || (y == 3 && x == 2))
				v = BR_C_WHITE;
			if (cracked && y > 0 && y < BR_BH - 1 &&
			    ((x == 9 && y < 4) || (x == 10 && y >= 3) || (x == 11 && y == 2)))
				v = BR_C_BLACK;
			px[y * BR_BW + x] = v;
		}
	return ar_sprite_from_pixels(s, px, BR_BW, BR_BH);
}

/* The paddle: a rounded steel bar with a light in the middle (amber;
 * green while it catches; the glow while it fires, with an emitter at
 * each quarter). The light flashes when the ball hits. */
static bool make_paddle(ar_sprite_t *s, int w, int light, bool emitters) {
	uint8_t px[BR_PADDLE_EASY_WIDE * BR_PADDLE_H];
	int x, y, l0 = w / 2 - 5, l1 = w / 2 + 5;
	for (y = 0; y < BR_PADDLE_H; y++)
		for (x = 0; x < w; x++) {
			uint8_t v = BR_C_PADDLE;
			int e = x < w - 1 - x ? x : w - 1 - x;    /* from the nearer end */
			bool edge_row = (y == 0 || y == BR_PADDLE_H - 1);
			if ((edge_row && e < 2) || ((y == 1 || y == BR_PADDLE_H - 2) && e < 1))
				v = 0xff;                              /* rounded ends */
			else if (y == 0) v = BR_C_WHITE;
			else if (y == BR_PADDLE_H - 1) v = BR_C_BLACK;
			else if (x >= l0 && x < l1) v = (uint8_t)light;
			else if (emitters && y < 4 && (x == w / 4 || x == w / 4 + 1 ||
			         x == w - 1 - w / 4 || x == w - 2 - w / 4))
				v = BR_C_GLOW;
			px[y * w + x] = v;
		}
	return ar_sprite_from_pixels(s, px, w, BR_PADDLE_H);
}

static bool make_cap(ar_sprite_t *s, int t, int frame) {
	uint8_t px[BR_BW * BR_BH];
	int x, y, c = cap_colour[t];
	const char *const *l = letter5[t - 1];
	for (y = 0; y < BR_BH; y++)
		for (x = 0; x < BR_BW; x++) {
			uint8_t v = (uint8_t)c;
			bool corner = (x == 0 || x == BR_BW - 1) && (y <= 1 || y >= BR_BH - 2);
			if (corner || ((x == 1 || x == BR_BW - 2) && (y == 0 || y == BR_BH - 1)))
				v = 0xff;
			else if (y == BR_BH - 1) v = BR_C_BLACK;
			else if (y == (frame ? 5 : 0)) v = BR_C_GLOW;
			if (x >= 5 && x < 10 && y >= 1 && y < 6 && l[y - 1][x - 5] == '1')
				v = BR_C_WHITE;
			px[y * BR_BW + x] = v;
		}
	return ar_sprite_from_pixels(s, px, BR_BW, BR_BH);
}

static const char *const ball_rows[BR_BALL] = {
	"..ff..",
	".ffff.",
	"ffffee",
	"fffeee",
	".feee.",
	"..ee..",
};

static const char *const mini_rows[3] = {
	"..ffffff..",
	".ccccddcc.",
	"..000000..",
};

/* the words a capsule says, rising off the paddle */
static ar_label_t word_l[BR_CAPS];
static uint32_t word_buf[BR_CAPS][20];   /* 5 letters: 8 x 8 bytes, + a word */

bool br_draw_init(void) {
	int t;
	bool ok = true;
	for (t = BR_C1; t <= BR_GOLD; t++) ok = make_brick(&brick_s[t], t, false) && ok;
	ok = make_brick(&cracked_s, BR_SILVER, true) && ok;
	ok = make_paddle(&paddle_s[PAD_NORMAL], BR_PADDLE_W, BR_C_LIGHT, false) && ok;
	ok = make_paddle(&paddle_s[PAD_WIDE], BR_PADDLE_WIDE, BR_C_LIGHT, false) && ok;
	ok = make_paddle(&paddle_s[PAD_CATCH], BR_PADDLE_W, BR_C_B1 + 3, false) && ok;
	ok = make_paddle(&paddle_s[PAD_LASER], BR_PADDLE_W, BR_C_LIGHT, true) && ok;
	ok = make_paddle(&paddle_s[PAD_WIDE_CATCH], BR_PADDLE_WIDE, BR_C_B1 + 3, false) && ok;
	ok = make_paddle(&paddle_s[PAD_WIDE_LASER], BR_PADDLE_WIDE, BR_C_LIGHT, true) && ok;
	ok = make_paddle(&paddle_s[PAD_EASY_WIDE], BR_PADDLE_EASY_WIDE, BR_C_LIGHT, false) && ok;
	for (t = 1; t < BR_CAPS; t++) {
		ok = make_cap(&cap_s[t][0], t, 0) && ok;
		ok = make_cap(&cap_s[t][1], t, 1) && ok;
		ok = ar_label_make(&word_l[t], word_buf[t], sizeof(word_buf[t]),
			cap_word[t], &z_font_8x8, 1) && ok;
	}
	ok = ar_sprite_make(&ball_s, ball_rows, BR_BALL, BR_BALL, NULL) && ok;
	ok = ar_sprite_make(&mini_s, mini_rows, 10, 3, NULL) && ok;
	return ok;
}

/* ===================================================================
 * panels
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

static bool in_game(void) {
	return br.st != ST_TITLE && br.st != ST_SCORES;
}

static int panel(ar_rect_t *r, line_t *ln) {

	int n = 0, i;

#define LINE(yy, ss, cc, ff, sc) do { \
		ln[n].y = (yy); ln[n].s = (ss); ln[n].c = (cc); \
		ln[n].f = (ff); ln[n].scale = (sc); n++; } while (0)

	switch (br.st) {

	case ST_TITLE:
		r->x = 72; r->y = 92; r->w = 176; r->h = 132;
		for (i = 0; i < MENU_ITEMS; i++) {
			static const char *const items[MENU_ITEMS] = { "", "PLAY", "HIGH SCORES" };
			bool sel = (i == br.menu);
			char *b = tbuf[i];
			if (i == MENU_SKILL) {
				/* "< EASY >": Left and Right change it */
				strcpy(b, sel ? "< " : "  ");
				strcat(b, ar_skill_name(ar_skill()));
				strcat(b, sel ? " >" : "  ");
				LINE(6, b, sel ? BR_C_WHITE : BR_C_GOLD, &z_font_8x8, 1);
				continue;
			}
			b[0] = sel ? '>' : ' ';
			b[1] = ' ';
			strcpy(b + 2, items[i]);
			strcat(b, sel ? " <" : "  ");
			LINE(20 + (i - 1) * 13, b, sel ? BR_C_WHITE : BR_C_WALL_HI, &z_font_8x8, 1);
		}
		strcpy(tbuf[4], "START ROUND  ");
		ar_utoa(tbuf[4] + strlen(tbuf[4]), (uint32_t)br.start_level, 2, ' ');
		LINE(50, tbuf[4], br.menu == MENU_PLAY ? BR_C_GOLD : BR_C_WALL_HI,
			&z_font_5x8, 1);
		LINE(60, br.menu == MENU_PLAY ? "LEFT/RIGHT TO CHANGE" :
			br.menu == MENU_SKILL ? "LEFT/RIGHT: HOW HARD" : "",
			BR_C_WALL_HI, &z_font_5x8, 1);
		strcpy(tbuf[5], "BEST ");
		ar_utoa(tbuf[5] + 5, br.best, 7, '0');
		LINE(74, tbuf[5], BR_C_WHITE, &z_font_8x8, 1);
		LINE(90, "MOUSE OR ARROWS, SPACE", BR_C_WALL_HI, &z_font_5x8, 1);
		LINE(102, "ENTER TO PLAY   M MUSIC", BR_C_WALL_HI, &z_font_5x8, 1);
		LINE(114, "F1 HELP  F11 FULL SCREEN", BR_C_WALL_HI, &z_font_5x8, 1);
		return n;

	case ST_SCORES:
		r->x = 72; r->y = 52; r->w = 176; r->h = 168;
		LINE(4, "HIGH SCORES", BR_C_GOLD, &z_font_8x8, 1);
		LINE(14, ar_skill_name(ar_skill()), BR_C_WALL_HI, &z_font_5x8, 1);
		for (i = 0; i < AR_SCORES; i++) {
			char *b = tbuf[i];
			ar_utoa(b, (uint32_t)i + 1, 2, ' ');
			strcat(b, ". ");
			if (i < br.table.n) {
				char v[12];
				strcat(b, br.table.e[i].who);
				strcat(b, "  ");
				ar_utoa(v, br.table.e[i].score, 7, '0');
				strcat(b, v);
			} else {
				strcat(b, "---  -------");
			}
			LINE(26 + i * 12, b, i == br.rank ? BR_C_WHITE : BR_C_WALL_HI,
				&z_font_8x8, 1);
		}
		LINE(150, "ENTER: BACK", BR_C_WALL_HI, &z_font_5x8, 1);
		return n;

	case ST_READY:
		/* once the bricks are all down */
		if (br.ready_rows < BR_ROWS) return 0;
		r->x = 80; r->y = 150; r->w = 160; r->h = 46;
		strcpy(tbuf[0], "ROUND ");
		ar_utoa(tbuf[0] + 6, (uint32_t)br.g.level, 0, ' ');
		LINE(8, tbuf[0], BR_C_WHITE, &z_font_8x8, 2);
		LINE(30, br_level_name(br.g.level), BR_C_GOLD, &z_font_5x8, 1);
		return n;

	case ST_PAUSE:
		r->x = 80; r->y = 120; r->w = 160; r->h = 56;
		LINE(8, "PAUSED", BR_C_GOLD, &z_font_8x8, 2);
		LINE(30, "P OR START: GO ON", BR_C_WALL_HI, &z_font_5x8, 1);
		LINE(40, "ESC: MENU", BR_C_WALL_HI, &z_font_5x8, 1);
		return n;

	case ST_CLEAR:
		r->x = 80; r->y = 150; r->w = 160; r->h = 46;
		LINE(8, "ROUND CLEAR", BR_C_GOLD, &z_font_8x8, 1);
		strcpy(tbuf[0], "NEXT: ");
		strcat(tbuf[0], br_level_name(br.g.level + 1));
		LINE(26, tbuf[0], BR_C_WALL_HI, &z_font_5x8, 1);
		return n;

	case ST_OVER:
		r->x = 80; r->y = 100; r->w = 160; r->h = 72;
		LINE(8, "GAME OVER", BR_C_LIGHT, &z_font_8x8, 2);
		strcpy(tbuf[0], "SCORE ");
		ar_utoa(tbuf[0] + 6, br.g.score, 7, '0');
		LINE(30, tbuf[0], BR_C_WHITE, &z_font_8x8, 1);
		strcpy(tbuf[1], "ROUND ");
		ar_utoa(tbuf[1] + 6, (uint32_t)br.g.level, 0, ' ');
		LINE(42, tbuf[1], BR_C_WALL_HI, &z_font_5x8, 1);
		LINE(56, "ENTER, START OR CLICK", BR_C_WALL_HI, &z_font_5x8, 1);
		return n;

	case ST_NAME:
		r->x = 72; r->y = 80; r->w = 176; r->h = 96;
		LINE(8, "NEW BEST", BR_C_GOLD, &z_font_8x8, 1);
		strcpy(tbuf[0], "RANK ");
		ar_utoa(tbuf[0] + 5, (uint32_t)br.rank + 1, 0, ' ');
		LINE(22, tbuf[0], BR_C_WHITE, &z_font_8x8, 1);
		tbuf[1][0] = br.name[0];
		tbuf[1][1] = ' ';
		tbuf[1][2] = br.name[1];
		tbuf[1][3] = ' ';
		tbuf[1][4] = br.name[2];
		tbuf[1][5] = 0;
		LINE(38, tbuf[1], BR_C_WHITE, &z_font_8x8, 3);
		LINE(78, "UP/DOWN LETTER  ENTER DONE", BR_C_WALL_HI, &z_font_5x8, 1);
		return n;

	default:
		return 0;
	}
#undef LINE
}

bool br_panel_rect(ar_rect_t *r) {
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
	/* in monochrome a coloured word would be drawn in its brick's
	 * pattern and break up: all text is white there */
	if (!ar_color() && c != BR_C_BLACK) c = BR_C_WHITE;
	ar_text(x, y, s, c, f, scale);
}

static void paint_panel(const ar_rect_t *dirty) {
	ar_rect_t r;
	line_t ln[MAX_LINES];
	int n = panel(&r, ln), i;
	if (n == 0 || !overlaps(dirty, r.x, r.y, r.w, r.h)) return;
	ar_fill(r.x, r.y, r.w, r.h, BR_C_BLACK);
	ar_fill(r.x + 1, r.y + 1, r.w - 2, 1, BR_C_WALL_HI);
	ar_fill(r.x + 1, r.y + r.h - 2, r.w - 2, 1, BR_C_WALL_HI);
	ar_fill(r.x + 1, r.y + 1, 1, r.h - 2, BR_C_WALL_HI);
	ar_fill(r.x + r.w - 2, r.y + 1, 1, r.h - 2, BR_C_WALL_HI);
	for (i = 0; i < n; i++) {
		int w = ar_text_width(ln[i].s, ln[i].f, ln[i].scale);
		text_if(dirty, r.x + (r.w - w) / 2, r.y + ln[i].y, ln[i].s, ln[i].c,
			ln[i].f, ln[i].scale);
	}
}

/* ===================================================================
 * the static picture
 * =================================================================== */

/* -- the status line -- */

#define HUD_SCORE_X 8
#define HUD_HI_X    96
#define HUD_ROUND_X 188
#define HUD_LIVES_X 252

static void paint_hud(const ar_rect_t *r) {
	char v[12];
	int i, spare;
	if (!overlaps(r, 0, 0, AR_W, HUD_H)) return;
	ar_fill(0, 0, AR_W, HUD_H, BR_C_BLACK);
	ar_utoa(v, br.g.score, 7, '0');
	text_if(r, HUD_SCORE_X, 4, v, BR_C_WHITE, &z_font_8x8, 1);
	text_if(r, HUD_HI_X, 4, "HI", BR_C_WALL_HI, &z_font_5x8, 1);
	ar_utoa(v, br.best > br.g.score ? br.best : br.g.score, 7, '0');
	text_if(r, HUD_HI_X + 14, 4, v, BR_C_GOLD, &z_font_8x8, 1);
	if (!in_game()) return;
	text_if(r, HUD_ROUND_X, 4, "ROUND", BR_C_WALL_HI, &z_font_5x8, 1);
	ar_utoa(v, (uint32_t)br.g.level, 2, '0');
	text_if(r, HUD_ROUND_X + 32, 4, v, BR_C_WHITE, &z_font_8x8, 1);
	spare = br.g.lives - 1;
	if (br.st == ST_LOST) spare = br.g.lives;     /* not yet served */
	if (spare > 5) spare = 5;
	for (i = 0; i < spare; i++)
		ar_sprite(&mini_s, HUD_LIVES_X + i * 12, 6);
}

void br_draw_hud_dirty(void) {
	uint32_t best = br.best > br.g.score ? br.best : br.g.score;
	if (br.g.score != br.shown_score) {
		ar_dirty(HUD_SCORE_X, 0, 56, HUD_H);
		br.shown_score = br.g.score;
	}
	if (best != br.shown_best) {
		ar_dirty(HUD_HI_X + 14, 0, 56, HUD_H);
		br.shown_best = best;
	}
	if (br.g.level != br.shown_level) {
		ar_dirty(HUD_ROUND_X + 32, 0, 16, HUD_H);
		br.shown_level = br.g.level;
	}
	{
		int lives = br.g.lives + (br.st == ST_LOST ? 1 : 0);
		if (lives != br.shown_lives) {
			ar_dirty(HUD_LIVES_X, 0, AR_W - HUD_LIVES_X, HUD_H);
			br.shown_lives = lives;
		}
	}
}

/* -- the walls -- */

static void paint_walls(const ar_rect_t *r) {
	int x, y;
	if (overlaps(r, 0, WALL_Y, AR_W, WALL_T)) {
		ar_fill(0, WALL_Y, AR_W, WALL_T, BR_C_WALL);
		ar_fill(0, WALL_Y + 1, AR_W, 1, BR_C_WALL_HI);
		ar_fill(0, WALL_Y + WALL_T - 1, AR_W, 1, BR_C_BLACK);
		for (x = 12; x < AR_W; x += 24)
			if (overlaps(r, x, WALL_Y + 3, 2, 2))
				ar_fill(x, WALL_Y + 3, 2, 2, BR_C_WALL_HI);
	}
	for (x = 0; x < 2; x++) {
		int wx = x ? BR_FX1 : 0;
		if (!overlaps(r, wx, WALL_Y, WALL_T, AR_H - WALL_Y)) continue;
		ar_fill(wx, WALL_Y + WALL_T, WALL_T, AR_H - WALL_Y - WALL_T, BR_C_WALL);
		ar_fill(wx + 1, WALL_Y + WALL_T, 1, AR_H - WALL_Y - WALL_T, BR_C_WALL_HI);
		ar_fill(x ? wx : wx + WALL_T - 1, WALL_Y + WALL_T, 1,
			AR_H - WALL_Y - WALL_T, BR_C_BLACK);
		for (y = 48; y < AR_H; y += 40)
			if (overlaps(r, wx, y, WALL_T, 3)) {
				ar_fill(wx, y, WALL_T, 3, BR_C_WALL_HI);
				ar_fill(wx, y + 2, WALL_T, 1, BR_C_BLACK);
			}
	}
}

/* -- the field: backdrop and bricks -- */

static void paint_field(const ar_rect_t *r) {
	int x0 = r->x, y0 = r->y, x1 = r->x + r->w, y1 = r->y + r->h;
	int rr, cc, r0, r1, c0, c1, shown;
	if (x0 < BR_FX0) x0 = BR_FX0;
	if (x1 > BR_FX1) x1 = BR_FX1;
	if (y0 < BR_FY0) y0 = BR_FY0;
	if (x1 <= x0 || y1 <= y0) return;
	ar_fill2(x0, y0, x1 - x0, y1 - y0, BR_C_BLACK, BR_C_BACK,
		&patterns[in_game() ? pattern_i : 0]);
	if (!in_game() || y1 <= BR_BY0 || y0 >= BR_BY0 + BR_ROWS * BR_BH) return;
	shown = (br.st == ST_READY) ? br.ready_rows : BR_ROWS;
	r0 = y0 < BR_BY0 ? 0 : (y0 - BR_BY0) / BR_BH;
	r1 = (y1 - 1 - BR_BY0) / BR_BH;
	c0 = (x0 - BR_FX0) / BR_BW;
	c1 = (x1 - 1 - BR_FX0) / BR_BW;
	if (r1 >= shown) r1 = shown - 1;
	for (rr = r0; rr <= r1; rr++)
		for (cc = c0; cc <= c1; cc++) {
			int t = br.g.brick[rr][cc];
			const ar_sprite_t *s;
			if (!t) continue;
			s = &brick_s[t];
			if (t == BR_SILVER && br.g.hp[rr][cc] < 2 + (br.g.level - 1) / 6 &&
			    br.g.hp[rr][cc] < 4)
				s = &cracked_s;
			ar_sprite(s, BR_BRICK_X(cc), BR_BRICK_Y(rr));
		}
}

/* -- the title: BRICKS, in small bricks -- */

static const char *const title_letters[6][5] = {
	{ "1111.", "1...1", "1111.", "1...1", "1111." },
	{ "1111.", "1...1", "1111.", "1..1.", "1...1" },
	{ "11111", "..1..", "..1..", "..1..", "11111" },
	{ ".1111", "1....", "1....", "1....", ".1111" },
	{ "1...1", "1..1.", "111..", "1..1.", "1...1" },
	{ ".1111", "1....", ".111.", "....1", "1111." },
};

#define TITLE_X 20
#define TITLE_Y 40
#define TITLE_W 280
#define TITLE_H 30

static void paint_title(const ar_rect_t *r) {
	int l, x, y;
	if (!overlaps(r, TITLE_X, TITLE_Y, TITLE_W, TITLE_H)) return;
	for (l = 0; l < 6; l++)
		for (y = 0; y < 5; y++)
			for (x = 0; x < 5; x++) {
				int px = TITLE_X + (l * 6 + x) * 8, py = TITLE_Y + y * 6;
				/* in monochrome the brick patterns make the letters
				 * hard to read: plain white */
				int c = ar_color() ? BR_C_B1 + (l + y) % 6 : BR_C_WHITE;
				if (title_letters[l][y][x] != '1' || !overlaps(r, px, py, 8, 6))
					continue;
				ar_fill(px, py, 7, 5, c);
				ar_fill(px, py, 7, 1, BR_C_WHITE);
				ar_fill(px + 1, py + 4, 6, 1, BR_C_BLACK);
			}
}

void br_paint(void *user, const ar_rect_t *r) {
	(void)user;
	paint_hud(r);
	paint_walls(r);
	paint_field(r);
	if (!in_game()) paint_title(r);
	paint_panel(r);
}

void br_draw_field_dirty(void) {
	ar_dirty(BR_FX0, BR_FY0, BR_FX1 - BR_FX0, AR_H - BR_FY0);
}

void br_draw_brick_dirty(int r, int c) {
	ar_dirty(BR_BRICK_X(c), BR_BRICK_Y(r), BR_BW, BR_BH);
}

/* ===================================================================
 * overlays, every frame
 * =================================================================== */

#define MAX_FLASH 12
typedef struct { int16_t x, y, t, c; } flash_t;
static flash_t flashes[MAX_FLASH];

typedef struct { int16_t x[4], y[4]; uint8_t n; } trail_t;
static trail_t trails[BR_MAX_BALLS];

static struct { int word, x, t; int32_t y; } popup;

/* the title's balls */
#define TBALLS 3
typedef struct { int32_t x, y, vx, vy; } tball_t;
static tball_t tballs[TBALLS];
static bool tballs_set;

static int paddle_flash, back_flash, back_flash_c;

static bool hidden_by_panel(int x, int y, int w, int h) {
	ar_rect_t p;
	if (br_panel_rect(&p) && overlaps(&p, x, y, w, h)) return true;
	if (!in_game() && x < TITLE_X + TITLE_W && TITLE_X < x + w &&
	    y < TITLE_Y + TITLE_H && TITLE_Y < y + h)
		return true;
	return false;
}

static void flash(int x, int y, int t, int c) {
	int i, best = 0;
	for (i = 0; i < MAX_FLASH; i++) {
		if (flashes[i].t <= 0) { best = i; break; }
		if (flashes[i].t < flashes[best].t) best = i;
	}
	flashes[best].x = (int16_t)x;
	flashes[best].y = (int16_t)y;
	flashes[best].t = (int16_t)t;
	flashes[best].c = (int16_t)c;
}

/* Is (x, y, w, h) clear of the title and the panel? */
static bool tball_free(int x, int y) {
	return x >= BR_FX0 && x + BR_BALL <= BR_FX1 && y >= BR_FY0 &&
		y + BR_BALL <= AR_H && !hidden_by_panel(x, y, BR_BALL, BR_BALL);
}

static void title_balls(uint32_t dt) {
	uint32_t f;
	int i;
	if (!tballs_set) {
		static const int16_t start[TBALLS][4] = {
			{ 30, 200, 300, -220 }, { 270, 80, -260, 180 }, { 150, 220, 200, -280 }
		};
		for (i = 0; i < TBALLS; i++) {
			tballs[i].x = start[i][0] << 8;
			tballs[i].y = start[i][1] << 8;
			tballs[i].vx = start[i][2];
			tballs[i].vy = start[i][3];
		}
		tballs_set = true;
	}
	for (i = 0; i < TBALLS; i++) {
		tball_t *b = &tballs[i];
		for (f = 0; f < dt; f++) {
			int nx = (b->x + b->vx) >> 8, ny = (b->y + b->vy) >> 8;
			if (tball_free(nx, b->y >> 8)) b->x += b->vx;
			else b->vx = -b->vx;
			if (tball_free(b->x >> 8, ny)) b->y += b->vy;
			else b->vy = -b->vy;
		}
		/* a panel that appeared on top of it: let it be */
		if (!hidden_by_panel(b->x >> 8, b->y >> 8, BR_BALL, BR_BALL))
			ar_ov_sprite(&ball_s, b->x >> 8, b->y >> 8);
	}
}

static void palette_frame(uint32_t dt) {
	uint32_t t = ar_ticks();
	int k;

	/* gold glints every two seconds */
	k = (int)(t % 120);
	ar_pal(BR_C_GOLD, k < 16 ? ar_rgb_mix(br_base_palette[BR_C_GOLD], 0xfff,
		(8 - (k > 8 ? k - 8 : 8 - k)) * 28) : br_base_palette[BR_C_GOLD]);

	/* the glow breathes */
	k = (int)(t % 32);
	ar_pal(BR_C_GLOW, ar_rgb_mix(br_base_palette[BR_C_GLOW], 0xfff,
		(k < 16 ? k : 32 - k) * 12));

	/* the paddle's light, after a hit */
	ar_pal(BR_C_LIGHT, paddle_flash > 0 ?
		ar_rgb_mix(br_base_palette[BR_C_LIGHT], 0xfff, paddle_flash * 32) :
		br_base_palette[BR_C_LIGHT]);
	if (paddle_flash > 0) paddle_flash -= (int)dt;

	/* the backdrop: the round's colour, a pulse on the beat, flashes */
	{
		uint16_t th = in_game() ? themes[theme_i] : themes[0];
		int32_t rows = zsfx_music_rows();
		static int32_t last_beat = -1;
		static int pulse;
		if (rows >= 0 && rows / 4 != last_beat) {
			last_beat = rows / 4;
			pulse = 6;
		}
		if (pulse > 0) {
			th = ar_rgb_mix(th, 0x448, pulse * 12);
			pulse -= (int)dt;
		}
		if (back_flash > 0) {
			th = ar_rgb_mix(th, (uint16_t)back_flash_c, back_flash * 16);
			back_flash -= (int)dt;
		}
		ar_pal(BR_C_BACK, th);
	}

	/* the title: the brick colours chase each other */
	for (k = 0; k < 6; k++) {
		int from = in_game() ? k : (k + (int)(t / 6)) % 6;
		ar_pal(BR_C_B1 + k, br_base_palette[BR_C_B1 + from]);
	}
}

static const ar_sprite_t *paddle_sprite(void) {
	if (br.g.pw == BR_PADDLE_EASY_WIDE) return &paddle_s[PAD_EASY_WIDE];
	if (br.g.pw == BR_PADDLE_WIDE) {
		if (br.g.power == BR_P_CATCH) return &paddle_s[PAD_WIDE_CATCH];
		if (br.g.power == BR_P_LASER) return &paddle_s[PAD_WIDE_LASER];
		return &paddle_s[PAD_WIDE];
	}
	if (br.g.power == BR_P_CATCH) return &paddle_s[PAD_CATCH];
	if (br.g.power == BR_P_LASER) return &paddle_s[PAD_LASER];
	return &paddle_s[PAD_NORMAL];
}

void br_draw_frame(uint32_t dt) {

	int i, k;
	const br_game_t *g = &br.g;
	bool play = (br.st == ST_READY || br.st == ST_PLAY || br.st == ST_PAUSE ||
		br.st == ST_CLEAR);

	palette_frame(dt);

	if (!in_game()) title_balls(dt);

	/* -- flashes: a brick taking a hit -- */
	for (i = 0; i < MAX_FLASH; i++) {
		flash_t *f = &flashes[i];
		if (f->t <= 0) continue;
		f->t = (int16_t)(f->t - (int)dt);
		if (f->t > 0 && !hidden_by_panel(f->x, f->y, BR_BW - 1, BR_BH - 1))
			ar_ov_fill(f->x, f->y, BR_BW - 1, BR_BH - 1, f->c);
	}

	/* -- round clear: the gold that is left flashes -- */
	if (br.st == ST_CLEAR && ((br.st_frames >> 3) & 1)) {
		int r, c;
		for (r = 0; r < BR_ROWS; r++)
			for (c = 0; c < BR_COLS; c++)
				if (g->brick[r][c] == BR_GOLD &&
				    !hidden_by_panel(BR_BRICK_X(c), BR_BRICK_Y(r), BR_BW, BR_BH))
					ar_ov_fill(BR_BRICK_X(c), BR_BRICK_Y(r), BR_BW - 1, BR_BH - 1,
						BR_C_WHITE);
	}

	if (play) {
		/* -- capsules, rolling -- */
		for (i = 0; i < BR_MAX_CAPS; i++) {
			const br_cap_t *c = &g->cap[i];
			int y = c->y >> 8;
			if (!c->type || y >= AR_H) continue;
			if (hidden_by_panel(c->x, y, BR_BW, BR_BH)) continue;
			ar_ov_sprite(&cap_s[c->type][(ar_ticks() >> 3) & 1], c->x, y);
		}

		/* -- bolts -- */
		for (i = 0; i < BR_MAX_SHOTS; i++) {
			const br_shot_t *s = &g->shot[i];
			if (s->live && !hidden_by_panel(s->x, s->y, 2, 6))
				ar_ov_fill(s->x, s->y, 2, 6, BR_C_GLOW);
		}

		/* -- balls, each with a fading trail -- */
		for (i = 0; i < BR_MAX_BALLS; i++) {
			const br_ball_t *b = &g->ball[i];
			trail_t *t = &trails[i];
			int bx = b->x >> 8, by = b->y >> 8;
			if (!b->live || by >= AR_H) { t->n = 0; continue; }
			if (b->stuck || br.st != ST_PLAY) {
				t->n = 0;
			} else {
				/* every other frame's position, newest first */
				if ((ar_ticks() & 1) == 0 || t->n == 0) {
					for (k = 3; k > 0; k--) { t->x[k] = t->x[k - 1]; t->y[k] = t->y[k - 1]; }
					t->x[0] = (int16_t)(bx + BR_BALL / 2);
					t->y[0] = (int16_t)(by + BR_BALL / 2);
					if (t->n < 4) t->n++;
				}
				for (k = 1; k < t->n; k++) {
					int s = 4 - k;     /* 3, 2, 1 pixels */
					int tx = t->x[k] - s / 2, ty = t->y[k] - s / 2;
					if (ty + s > AR_H || hidden_by_panel(tx, ty, s, s)) continue;
					ar_ov_fill(tx, ty, s, s, BR_C_GLOW);
				}
			}
			if (!hidden_by_panel(bx, by, BR_BALL, BR_BALL))
				ar_ov_sprite(&ball_s, bx, by);
		}

		/* -- the paddle -- */
		if (!hidden_by_panel(g->px, BR_PADDLE_Y, g->pw, BR_PADDLE_H))
			ar_ov_sprite(paddle_sprite(), g->px, BR_PADDLE_Y);

		/* -- easy: the safety floor under it, a glowing bar -- */
		if (g->floor_up && !hidden_by_panel(BR_FX0, BR_FLOOR_Y, BR_FX1 - BR_FX0, 2))
			ar_ov_fill(BR_FX0, BR_FLOOR_Y, BR_FX1 - BR_FX0, 2, BR_C_GLOW);
	}

	/* -- what the capsule was, rising -- */
	if (popup.t > 0) {
		const ar_label_t *l = &word_l[popup.word];
		int x = popup.x - l->w / 2, y;
		popup.t -= (int)dt;
		popup.y -= 160 * (int32_t)dt;
		y = popup.y >> 8;
		if (x < BR_FX0) x = BR_FX0;
		if (x + l->w > BR_FX1) x = BR_FX1 - l->w;
		if (popup.t > 0 && !hidden_by_panel(x, y, l->w, l->h))
			ar_ov_label(l, x, y, (popup.t >> 2) & 1 ? BR_C_WHITE : BR_C_GLOW);
	}

	ar_parts_frame(dt);

	/* the name entry's cursor */
	if (br.st == ST_NAME && ((ar_ticks() >> 4) & 1)) {
		int x0 = 72 + (176 - ar_text_width("A A A", &z_font_8x8, 3)) / 2;
		ar_ov_fill(x0 + br.name_pos * 48, 80 + 38 + 25, 24, 2, BR_C_WHITE);
	}
}

/* ===================================================================
 * effects for the rules' events
 * =================================================================== */

void br_draw_lost(void) {
	const br_game_t *g = &br.g;
	int x;
	for (x = 0; x < g->pw; x += 4)
		ar_parts_burst(g->px + x, BR_PADDLE_Y + 2, 3, BR_C_PADDLE, 2, 520, 34, 22);
	ar_parts_burst(g->px + g->pw / 2, BR_PADDLE_Y, 10, BR_C_GLOW, 2, 800, 24, 10);
	ar_shake(4, 28);
	back_flash = 12;
	back_flash_c = 0xf00;
}

void br_draw_events(void) {

	int i;
	const br_game_t *g = &br.g;

	for (i = 0; i < g->nev; i++) {
		const br_event_t *e = &g->ev[i];
		switch (e->type) {

		case BR_EV_FLOOR: {
			/* the floor bounced the ball and goes, in sparks */
			int x;
			for (x = BR_FX0 + 4; x < BR_FX1; x += 12)
				ar_parts_burst(x, BR_FLOOR_Y, 2, BR_C_GLOW, 1, 300, 20, 14);
			ar_parts_burst(e->x, BR_FLOOR_Y, 10, BR_C_WHITE, 1, 380, 18, 10);
			br_sound(BR_SND_FLOOR, e->x);
			break;
		}

		case BR_EV_WALL:
			br_sound(BR_SND_WALL, e->x);
			ar_parts_burst(e->x, e->y, 2, BR_C_WALL_HI, 1, 200, 8, 0);
			break;

		case BR_EV_PADDLE:
			br_sound(BR_SND_PADDLE, e->x);
			paddle_flash = 8;
			ar_parts_burst(e->x, BR_PADDLE_Y, 3, BR_C_GLOW, 1, 260, 10, 12);
			break;

		case BR_EV_HIT:
			br_sound(BR_SND_SILVER, e->x);
			br_draw_brick_dirty(e->r, e->c);
			flash(e->x, e->y, 4, BR_C_WHITE);
			ar_parts_burst(e->x + BR_BW / 2, e->y + BR_BH / 2, 3, BR_C_SILVER, 1,
				240, 12, 10);
			break;

		case BR_EV_GOLD:
			br_sound(BR_SND_GOLD, e->x);
			flash(e->x, e->y, 6, BR_C_GOLD);
			ar_parts_burst(e->x + BR_BW / 2, e->y + BR_BH / 2, 2, BR_C_GOLD, 1,
				200, 10, 0);
			break;

		case BR_EV_BREAK: {
			int c = brick_colour(e->brick);
			int n = e->value >= 6 ? 10 : 6;
			br_sound_break(e->value ? e->value : 1, e->x);
			br_draw_brick_dirty(e->r, e->c);
			flash(e->x, e->y, 3, BR_C_WHITE);
			ar_parts_burst(e->x + BR_BW / 2, e->y + BR_BH / 2, n, c, 1, 330, 24, 16);
			if (e->brick == BR_SILVER) {
				ar_parts_burst(e->x + BR_BW / 2, e->y + BR_BH / 2, 6, BR_C_WHITE, 1,
					420, 16, 10);
				ar_shake(2, 8);
			} else if (e->value >= 8) {
				ar_shake(1, 4);
			}
			break;
		}

		case BR_EV_CAP_DROP:
			br_sound(BR_SND_DROP, e->x);
			break;

		case BR_EV_CAP_TAKE: {
			static const uint8_t snd[BR_CAPS] = {
				0, BR_SND_WIDE, BR_SND_MULTI, BR_SND_SLOW, BR_SND_CATCH_POWER,
				BR_SND_LASER_POWER, BR_SND_LIFE
			};
			if (e->value > 0 && e->value < BR_CAPS) {
				if (e->value != BR_CAP_LIFE) br_sound(snd[e->value], e->x);
				popup.word = e->value;
				popup.x = e->x;
				popup.y = (int32_t)(BR_PADDLE_Y - 12) << 8;
				popup.t = 50;
				ar_parts_burst(e->x, BR_PADDLE_Y, 10, cap_colour[e->value], 1,
					380, 20, 6);
				back_flash = 6;
				back_flash_c = br_base_palette[cap_colour[e->value]];
			}
			break;
		}

		case BR_EV_LAUNCH:
			br_sound(BR_SND_LAUNCH, e->x);
			break;

		case BR_EV_CATCH:
			br_sound(BR_SND_CATCH, e->x);
			paddle_flash = 6;
			break;

		case BR_EV_FIRE:
			br_sound(BR_SND_FIRE, e->x);
			ar_parts_burst(g->px + 4, BR_PADDLE_Y - 2, 2, BR_C_GLOW, 1, 200, 6, 0);
			ar_parts_burst(g->px + g->pw - 4, BR_PADDLE_Y - 2, 2, BR_C_GLOW, 1, 200, 6, 0);
			break;

		case BR_EV_SHOT_END:
			br_sound(BR_SND_BOLT_END, e->x);
			ar_parts_burst(e->x, e->y, 3, BR_C_GLOW, 1, 220, 8, 0);
			break;

		case BR_EV_BALL_LOST:
			br_sound(BR_SND_BALL_LOST, e->x);
			break;

		case BR_EV_LIFE:
			br_sound(BR_SND_LIFE, -1);
			break;

		case BR_EV_CLEAR:
			back_flash = 16;
			back_flash_c = 0xfff;
			break;

		default:
			break;
		}
	}
}

void br_draw_reset(void) {
	int i;
	ar_parts_clear();
	for (i = 0; i < MAX_FLASH; i++) flashes[i].t = 0;
	for (i = 0; i < BR_MAX_BALLS; i++) trails[i].n = 0;
	popup.t = 0;
	paddle_flash = back_flash = 0;
	ar_pal_load(br_base_palette, 16);
}
