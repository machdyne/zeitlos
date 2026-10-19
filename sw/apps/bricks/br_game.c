/*
 * bricks -- the rules: the ball, the bricks, the capsules, the levels.
 * See br_game.h for how a ball moves, and docs/bricks_app.md.
 */

#include <string.h>

#include "br_game.h"

/* ===================================================================
 * the layouts
 *
 * Nineteen columns by up to fourteen rows, read top down; NULL ends a
 * level. '.' is no brick, '1'..'6' the six colours (one hit), 'S'
 * silver (two hits, more on later levels), 'G' gold (never breaks: a
 * level is clear when everything else is gone). After the last layout
 * the first comes round again, faster and with tougher silver.
 * =================================================================== */

static const char *const lv_rows[] = {
	/* FIRST LIGHT */
	"...................",
	"...................",
	"SSSSSSSSSSSSSSSSSSS",
	"1111111111111111111",
	"2222222222222222222",
	"3333333333333333333",
	"4444444444444444444",
	"5555555555555555555",
	"6666666666666666666",
	NULL,
	/* STAIRS */
	"1..................",
	"12.................",
	"123................",
	"1234...............",
	"12345..............",
	"123456.............",
	"1234561............",
	"12345612...........",
	"123456123..........",
	"1234561234.........",
	"12345612345........",
	"123456123456.......",
	"SSSSSSSSSSSSSSSSSS6",
	NULL,
	/* WINDOWS */
	"...................",
	"3333333333333333333",
	"2222222222222222222",
	"1111111111111111111",
	"...................",
	"GGG..GGGGGGGGG..GGG",
	"...................",
	"..4444.......4444..",
	"..5555.......5555..",
	"..6666.......6666..",
	NULL,
	/* DIAMOND */
	"...................",
	".........1.........",
	"........222........",
	".......43334.......",
	"......6544456......",
	".....216555612.....",
	"....43216S61234....",
	".....432111234.....",
	"......4322234......",
	".......43334.......",
	"........444........",
	".........5.........",
	NULL,
	/* PILLARS */
	"...................",
	"G.G.G.G.G.G.G.G.G.G",
	"1.2.3.4.5.6.1.2.3.4",
	"1.2.3.4.5.6.1.2.3.4",
	"1.2.3.4.5.6.1.2.3.4",
	"1.2.3.4.5.6.1.2.3.4",
	"S.S.S.S.S.S.S.S.S.S",
	"1.2.3.4.5.6.1.2.3.4",
	"1.2.3.4.5.6.1.2.3.4",
	NULL,
	/* CHECKERS */
	"...................",
	"1.3.5.1.3.5.1.3.5.1",
	".3.5.1.3.5.1.3.5.1.",
	"3.5.1.3.5.1.3.5.1.3",
	".5.1.3.5.1.3.5.1.3.",
	"5.1.3.5.1.3.5.1.3.5",
	".1.3.5.1.3.5.1.3.5.",
	"1.3.5.1.3.5.1.3.5.1",
	".3.5.1.3.5.1.3.5.1.",
	"3.5.1.3.5.1.3.5.1.3",
	".5.1.3.5.1.3.5.1.3.",
	"5.1.3.5.1.3.5.1.3.5",
	".1.3.5.1.3.5.1.3.5.",
	NULL,
	/* FORTRESS */
	"...................",
	"...G...........G...",
	"...SSSSSSSSSSSSS...",
	"...S11111111111S...",
	"...S12222222221S...",
	"...S12333333321S...",
	"...S12344444321S...",
	"...S12333333321S...",
	"...S12222222221S...",
	"...S11111111111S...",
	"...SSSSSS.SSSSSS...",
	"...G...........G...",
	NULL,
	/* VISITORS */
	"...................",
	"......4.....4......",
	".......4...4.......",
	"......4444444......",
	".....44.444.44.....",
	"....33333333333....",
	"....3.3333333.3....",
	"....3.3.....3.3....",
	".......33.33.......",
	"...................",
	"S.S.S.S.S.S.S.S.S.S",
	NULL,
	/* RAINBOW */
	"...................",
	"1111...........1111",
	".2222.........2222.",
	"..3333.......3333..",
	"...4444.....4444...",
	"....5555...5555....",
	".....6666.6666.....",
	"......SSSSSSS......",
	NULL,
	/* MAZE */
	"...................",
	"1111111111111111111",
	"GGGGGGGGGGGGGGG....",
	"2222222222222222222",
	"....GGGGGGGGGGGGGGG",
	"3333333333333333333",
	"GGGGGGGGGGGGGGG....",
	"4444444444444444444",
	"....GGGGGGGGGGGGGGG",
	"5555555555555555555",
	NULL,
	/* HEART */
	"...................",
	"....555.....555....",
	"...55555...55555...",
	"..5555555.5555555..",
	"..555555555555555..",
	"..555555555555555..",
	"...5555555555555...",
	"....55555555555....",
	".....555555555.....",
	"......5555555......",
	".......55555.......",
	"........555........",
	".........5.........",
	NULL,
	/* ARMOUR */
	"...................",
	"SSSSSSSSSSSSSSSSSSS",
	"S6S6S6S6S6S6S6S6S6S",
	"SSSSSSSSSSSSSSSSSSS",
	"5555555555555555555",
	"4G4G4G4G4G4G4G4G4G4",
	"3333333333333333333",
	"SSSSSSSSSSSSSSSSSSS",
	"2222222222222222222",
	"1111111111111111111",
	NULL,
};
static const char *const lv_names[] = {
	"FIRST LIGHT", "STAIRS", "WINDOWS", "DIAMOND", "PILLARS", "CHECKERS", "FORTRESS", "VISITORS", "RAINBOW", "MAZE", "HEART", "ARMOUR"
};

/* ===================================================================
 * small things
 * =================================================================== */

#define NLEVELS ((int)(sizeof(lv_names) / sizeof(lv_names[0])))

/* sin and cos of the eight angles, x256: 8, 16, 24, 32, 40, 48, 56 and
 * 63 degrees from straight up */
static const int16_t ang_sin[8] = { 36, 71, 104, 136, 165, 190, 212, 228 };
static const int16_t ang_cos[8] = { 254, 246, 234, 217, 196, 171, 143, 116 };

#define SPEED_MIN   416          /* the slow capsule: 1.6 px a frame */
#define SPEED_MAX   1088         /* 4.25 */
#define SPEED_STEP  12           /* every SPEED_HITS bricks hit */
#define SPEED_HITS  8
#define CAP_FALL    320          /* 1.25 px a frame */
#define SHOT_SPEED  5
#define SERVE_HOLD  360          /* a served ball goes by itself after 6 s */
#define CATCH_HOLD  150

int br_levels(void) { return NLEVELS; }

const char *br_level_name(int level) {
	return lv_names[(level - 1) % NLEVELS];
}

static void ev(br_game_t *g, int type, int r, int c, int value, int x, int y) {
	br_event_t *e;
	if (g->nev >= BR_MAX_EV) return;
	e = &g->ev[g->nev++];
	e->type = (uint8_t)type;
	e->r = (uint8_t)r;
	e->c = (uint8_t)c;
	e->value = (uint8_t)value;
	e->brick = 0;
	e->x = (int16_t)x;
	e->y = (int16_t)y;
}

void br_clear_events(br_game_t *g) { g->nev = 0; }

uint32_t br_points(int type, int level) {
	if (type >= BR_C1 && type <= BR_C6) return 50u + 10u * (uint32_t)(type - BR_C1);
	if (type == BR_SILVER) return 50u * (uint32_t)(level < 20 ? level : 20);
	return 0;
}

int br_balls(const br_game_t *g) {
	int i, n = 0;
	for (i = 0; i < BR_MAX_BALLS; i++) if (g->ball[i].live) n++;
	return n;
}

static int32_t vel_x(const br_game_t *g, const br_ball_t *b) {
	return b->sx * ((g->speed * ang_sin[b->ang]) >> 8);
}

static int32_t vel_y(const br_game_t *g, const br_ball_t *b) {
	return b->sy * ((g->speed * ang_cos[b->ang]) >> 8);
}

/* ===================================================================
 * levels
 * =================================================================== */

static void load_rows(br_game_t *g, const char *const *rows) {
	int r, c, silver_hp = 2 + (g->level - 1) / 6;
	if (silver_hp > 4) silver_hp = 4;
	memset(g->brick, 0, sizeof(g->brick));
	memset(g->hp, 0, sizeof(g->hp));
	g->bricks_left = 0;
	for (r = 0; r < BR_ROWS && rows[r]; r++) {
		for (c = 0; c < BR_COLS && rows[r][c]; c++) {
			char ch = rows[r][c];
			int t = BR_EMPTY;
			if (ch >= '1' && ch <= '6') t = BR_C1 + (ch - '1');
			else if (ch == 'S') t = BR_SILVER;
			else if (ch == 'G') t = BR_GOLD;
			g->brick[r][c] = (uint8_t)t;
			g->hp[r][c] = (uint8_t)(t == BR_SILVER ? silver_hp : 1);
			if (t != BR_EMPTY && t != BR_GOLD) g->bricks_left++;
		}
	}
}

static void level_setup(br_game_t *g) {
	g->speed_base = 480 + 32 * (g->level - 1);
	if (g->speed_base > 704) g->speed_base = 704;
	g->hits = 0;
	g->cleared = false;
	g->drop_wait = 2;
}

static void load_level(br_game_t *g) {
	const char *const *p = lv_rows;
	int k = (g->level - 1) % NLEVELS;
	while (k > 0) {
		while (*p) p++;
		p++;
		k--;
	}
	load_rows(g, p);
	level_setup(g);
}

#ifdef BR_TESTS
void br_test_layout(br_game_t *g, const char *const *rows) {
	load_rows(g, rows);
	level_setup(g);
	br_serve(g);
}
#endif

/* ===================================================================
 * the paddle and its powers
 * =================================================================== */

static void hold_balls(br_game_t *g) {
	int i;
	for (i = 0; i < BR_MAX_BALLS; i++) {
		br_ball_t *b = &g->ball[i];
		if (!b->live || !b->stuck) continue;
		if (b->stuck_off > g->pw - BR_BALL) b->stuck_off = (int16_t)(g->pw - BR_BALL);
		if (b->stuck_off < 0) b->stuck_off = 0;
		b->x = (int32_t)(g->px + b->stuck_off) << 8;
		b->y = (int32_t)(BR_PADDLE_Y - BR_BALL) << 8;
	}
}

static void clamp_paddle(br_game_t *g) {
	if (g->px < BR_FX0) g->px = BR_FX0;
	if (g->px > BR_FX1 - g->pw) g->px = BR_FX1 - g->pw;
	hold_balls(g);
}

void br_paddle_move(br_game_t *g, int dx) {
	g->px += dx;
	clamp_paddle(g);
}

void br_paddle_centre(br_game_t *g, int x) {
	g->px = x - g->pw / 2;
	clamp_paddle(g);
}

/* The angle for a ball whose middle is at x on the paddle: sixteen
 * zones, the outer ones steepest. */
static void aim(const br_game_t *g, br_ball_t *b, int x) {
	int zone = ((x - g->px) * 16) / g->pw;
	if (zone < 0) zone = 0;
	if (zone > 15) zone = 15;
	if (zone < 8) { b->ang = (uint8_t)(7 - zone); b->sx = -1; }
	else { b->ang = (uint8_t)(zone - 8); b->sx = 1; }
	b->sy = -1;
}

static void launch(br_game_t *g, br_ball_t *b) {
	b->stuck = false;
	aim(g, b, g->px + b->stuck_off + BR_BALL / 2);
	ev(g, BR_EV_LAUNCH, 0, 0, 0, (b->x >> 8) + BR_BALL / 2, b->y >> 8);
}

static void release_all(br_game_t *g) {
	int i;
	for (i = 0; i < BR_MAX_BALLS; i++)
		if (g->ball[i].live && g->ball[i].stuck) launch(g, &g->ball[i]);
}

static void set_power(br_game_t *g, int p) {
	if (g->power == BR_P_WIDE && p != BR_P_WIDE) {
		g->px += (BR_PADDLE_WIDE - BR_PADDLE_W) / 2;
		g->pw = BR_PADDLE_W;
	} else if (p == BR_P_WIDE && g->power != BR_P_WIDE) {
		g->px -= (BR_PADDLE_WIDE - BR_PADDLE_W) / 2;
		g->pw = BR_PADDLE_WIDE;
	}
	if (g->power == BR_P_CATCH && p != BR_P_CATCH) release_all(g);
	g->power = p;
	clamp_paddle(g);
}

void br_fire(br_game_t *g) {
	int i, free_shots = 0;
	bool held = false;
	if (g->over || g->cleared) return;
	for (i = 0; i < BR_MAX_BALLS; i++)
		if (g->ball[i].live && g->ball[i].stuck) { launch(g, &g->ball[i]); held = true; }
	if (held || g->power != BR_P_LASER || g->fire_wait > 0) return;
	for (i = 0; i < BR_MAX_SHOTS; i++) if (!g->shot[i].live) free_shots++;
	if (free_shots < 2) return;
	{
		int xs[2], k = 0;
		xs[0] = g->px + 3;
		xs[1] = g->px + g->pw - 5;
		for (i = 0; i < BR_MAX_SHOTS && k < 2; i++) {
			if (g->shot[i].live) continue;
			g->shot[i].live = true;
			g->shot[i].x = (int16_t)xs[k++];
			g->shot[i].y = BR_PADDLE_Y - 6;
		}
	}
	g->fire_wait = 12;
	ev(g, BR_EV_FIRE, 0, 0, 0, g->px + g->pw / 2, BR_PADDLE_Y);
}

static void multi(br_game_t *g) {
	int i, src = -1, added = 0;
	for (i = 0; i < BR_MAX_BALLS; i++)
		if (g->ball[i].live) { src = i; if (!g->ball[i].stuck) break; }
	if (src < 0) return;
	if (g->ball[src].stuck) launch(g, &g->ball[src]);
	for (i = 0; i < BR_MAX_BALLS && added < 2; i++) {
		br_ball_t *b = &g->ball[i];
		if (b->live) continue;
		*b = g->ball[src];
		if (added == 0) b->sx = (int8_t)-b->sx;
		else b->ang = (uint8_t)(b->ang < 4 ? b->ang + 3 : b->ang - 3);
		added++;
	}
}

static void take(br_game_t *g, int type, int x) {
	switch (type) {
	case BR_CAP_WIDE:  set_power(g, BR_P_WIDE); break;
	case BR_CAP_CATCH: set_power(g, BR_P_CATCH); break;
	case BR_CAP_LASER: set_power(g, BR_P_LASER); break;
	case BR_CAP_MULTI: multi(g); break;
	case BR_CAP_SLOW:  g->speed = SPEED_MIN; g->hits = 0; break;
	case BR_CAP_LIFE:
		if (g->lives < 9) g->lives++;
		ev(g, BR_EV_LIFE, 0, 0, 0, x, 0);
		break;
	default: break;
	}
	g->score += 1000;
	ev(g, BR_EV_CAP_TAKE, 0, 0, type, x, BR_PADDLE_Y);
}

void br_serve(br_game_t *g) {
	br_ball_t *b;
	memset(g->ball, 0, sizeof(g->ball));
	memset(g->cap, 0, sizeof(g->cap));
	memset(g->shot, 0, sizeof(g->shot));
	g->power = BR_P_NONE;
	g->pw = BR_PADDLE_W;
	g->px = (BR_FX0 + BR_FX1 - g->pw) / 2;
	g->speed = g->speed_base;
	g->top_boost = false;
	g->chain = 0;
	g->fire_wait = 0;
	b = &g->ball[0];
	b->live = true;
	b->stuck = true;
	b->stuck_off = (int16_t)(g->pw / 2 - BR_BALL / 2 + 4);
	b->stuck_left = SERVE_HOLD;
	hold_balls(g);
}

void br_new(br_game_t *g, int level, uint32_t (*rand_below)(uint32_t n)) {
	memset(g, 0, sizeof(*g));
	g->rand_below = rand_below;
	g->level = level < 1 ? 1 : level;
	g->lives = BR_LIVES;
	g->next_life = 20000;
	load_level(g);
	br_serve(g);
}

void br_next_level(br_game_t *g) {
	g->level++;
	load_level(g);
	br_serve(g);
}

/* ===================================================================
 * bricks
 * =================================================================== */

bool br_brick_at(const br_game_t *g, int x, int y, int *r, int *c) {
	int rr, cc;
	if (x < BR_FX0 || y < BR_BY0) return false;
	cc = (x - BR_FX0) / BR_BW;
	rr = (y - BR_BY0) / BR_BH;
	if (cc >= BR_COLS || rr >= BR_ROWS || !g->brick[rr][cc]) return false;
	*r = rr;
	*c = cc;
	return true;
}

/* The bricks a ball-sized box at (x, y) would overlap: up to four. */
static int box_bricks(const br_game_t *g, int x, int y, int w, int h,
	uint8_t *rs, uint8_t *cs) {
	int r0, r1, c0, c1, r, c, n = 0;
	if (y + h - 1 < BR_BY0 || x + w - 1 < BR_FX0 || x >= BR_FX1) return 0;
	r0 = y < BR_BY0 ? 0 : (y - BR_BY0) / BR_BH;
	r1 = (y + h - 1 - BR_BY0) / BR_BH;
	c0 = x < BR_FX0 ? 0 : (x - BR_FX0) / BR_BW;
	c1 = (x + w - 1 - BR_FX0) / BR_BW;
	if (r1 >= BR_ROWS) r1 = BR_ROWS - 1;
	if (c1 >= BR_COLS) c1 = BR_COLS - 1;
	for (r = r0; r <= r1; r++)
		for (c = c0; c <= c1; c++)
			if (g->brick[r][c] && n < 4) {
				rs[n] = (uint8_t)r;
				cs[n] = (uint8_t)c;
				n++;
			}
	return n;
}

static void maybe_drop(br_game_t *g, int r, int c) {
	static const uint8_t weight[BR_CAPS] = { 0, 22, 16, 18, 16, 20, 4 };
	int i, slot = -1, t, roll, total = 0;
	if (--g->drop_wait > 0) return;
	for (i = 0; i < BR_MAX_CAPS; i++) if (!g->cap[i].type) { slot = i; break; }
	if (slot < 0) return;
	if (g->rand_below(4) != 0) return;
	for (t = 1; t < BR_CAPS; t++) total += weight[t];
	roll = (int)g->rand_below((uint32_t)total);
	for (t = 1; t < BR_CAPS - 1 && roll >= weight[t]; t++) roll -= weight[t];
	/* not more of what the paddle already has, nor more balls while
	 * there are several */
	if ((t == BR_CAP_WIDE && g->power == BR_P_WIDE) ||
	    (t == BR_CAP_CATCH && g->power == BR_P_CATCH) ||
	    (t == BR_CAP_LASER && g->power == BR_P_LASER) ||
	    (t == BR_CAP_MULTI && br_balls(g) > 1))
		t = BR_CAP_SLOW;
	g->cap[slot].type = (uint8_t)t;
	g->cap[slot].x = (int16_t)BR_BRICK_X(c);
	g->cap[slot].y = (int32_t)BR_BRICK_Y(r) << 8;
	g->drop_wait = 3 + (int)g->rand_below(5);
	ev(g, BR_EV_CAP_DROP, r, c, t, BR_BRICK_X(c), BR_BRICK_Y(r));
}

static void add_score(br_game_t *g, uint32_t pts) {
	g->score += pts;
	while (g->score >= g->next_life) {
		if (g->lives < 9) g->lives++;
		g->next_life += 50000;
		ev(g, BR_EV_LIFE, 0, 0, 0, 0, 0);
	}
}

static void hit(br_game_t *g, int r, int c, bool by_ball) {
	int t = g->brick[r][c];
	if (t == BR_EMPTY) return;
	if (t == BR_GOLD) { ev(g, BR_EV_GOLD, r, c, 0, BR_BRICK_X(c), BR_BRICK_Y(r)); return; }
	if (++g->hits % SPEED_HITS == 0 && g->speed < SPEED_MAX) {
		g->speed += SPEED_STEP;
		if (g->speed > SPEED_MAX) g->speed = SPEED_MAX;
	}
	if (t == BR_SILVER && --g->hp[r][c] > 0) {
		ev(g, BR_EV_HIT, r, c, g->hp[r][c], BR_BRICK_X(c), BR_BRICK_Y(r));
		return;
	}
	g->brick[r][c] = BR_EMPTY;
	g->hp[r][c] = 0;
	g->bricks_left--;
	if (by_ball && g->chain < 255) g->chain++;
	add_score(g, br_points(t, g->level));
	ev(g, BR_EV_BREAK, r, c, by_ball ? g->chain : 0, BR_BRICK_X(c), BR_BRICK_Y(r));
	if (g->nev > 0 && g->ev[g->nev - 1].type == BR_EV_BREAK) g->ev[g->nev - 1].brick = (uint8_t)t;
	if (g->bricks_left <= 0) {
		g->cleared = true;
		ev(g, BR_EV_CLEAR, 0, 0, 0, 0, 0);
		return;
	}
	if (t != BR_SILVER) maybe_drop(g, r, c);
}

/* Hit every brick in the box; true if there was any. */
static bool hit_box(br_game_t *g, int x, int y, int w, int h, bool by_ball) {
	uint8_t rs[4], cs[4];
	int n = box_bricks(g, x, y, w, h, rs, cs), i;
	for (i = 0; i < n; i++) hit(g, rs[i], cs[i], by_ball);
	return n > 0;
}

/* ===================================================================
 * the frame
 * =================================================================== */

/* One step of at most a pixel on each axis. False if the ball is lost. */
static bool ball_substep(br_game_t *g, br_ball_t *b, int32_t dx, int32_t dy) {
	int32_t nx = b->x + dx, ny = b->y + dy;
	int ix = nx >> 8, iy = b->y >> 8, oy;

	/* across */
	if (dx != 0) {
		if (ix < BR_FX0 || ix + BR_BALL > BR_FX1) {
			b->sx = (int8_t)(ix < BR_FX0 ? 1 : -1);
			ev(g, BR_EV_WALL, 0, 0, 0, ix < BR_FX0 ? BR_FX0 : BR_FX1, iy + BR_BALL / 2);
		} else if (hit_box(g, ix, iy, BR_BALL, BR_BALL, true)) {
			b->sx = (int8_t)-b->sx;
		} else {
			b->x = nx;
		}
	}
	if (g->cleared) return true;

	/* down (or up) */
	ix = b->x >> 8;
	oy = b->y >> 8;
	iy = ny >> 8;
	if (dy == 0) return true;
	if (iy < BR_FY0) {
		b->sy = 1;
		ev(g, BR_EV_WALL, 0, 0, 1, ix + BR_BALL / 2, BR_FY0);
		if (!g->top_boost) {
			g->top_boost = true;
			if (g->speed < g->speed_base + 160) {
				g->speed = g->speed_base + 160;
				ev(g, BR_EV_SPEED, 0, 0, 0, 0, 0);
			}
		}
		return true;
	}
	if (hit_box(g, ix, iy, BR_BALL, BR_BALL, true)) {
		b->sy = (int8_t)-b->sy;
		return true;
	}
	/* the paddle: only from above, crossing its top edge */
	if (dy > 0 && oy + BR_BALL <= BR_PADDLE_Y && iy + BR_BALL > BR_PADDLE_Y &&
	    ix + BR_BALL > g->px && ix < g->px + g->pw) {
		int cx = ix + BR_BALL / 2;
		b->y = (int32_t)(BR_PADDLE_Y - BR_BALL) << 8;
		g->chain = 0;
		if (g->power == BR_P_CATCH) {
			b->stuck = true;
			b->stuck_off = (int16_t)(ix - g->px);
			b->stuck_left = CATCH_HOLD;
			hold_balls(g);
			ev(g, BR_EV_CATCH, 0, 0, 0, cx, BR_PADDLE_Y);
		} else {
			aim(g, b, cx);
			ev(g, BR_EV_PADDLE, 0, 0, b->ang, cx, BR_PADDLE_Y);
		}
		return true;
	}
	b->y = ny;
	if (iy >= BR_FLOOR) {
		b->live = false;
		return false;
	}
	return true;
}

static void ball_frame(br_game_t *g, br_ball_t *b) {
	int32_t ax, ay, n, i;
	if (b->stuck) {
		if (--b->stuck_left <= 0) launch(g, b);
		return;
	}
	ax = vel_x(g, b);
	ay = vel_y(g, b);
	if (ax < 0) ax = -ax;
	if (ay < 0) ay = -ay;
	n = ((ax > ay ? ax : ay) >> 8) + 1;
	for (i = 0; i < n; i++) {
		/* from the signs as they are now: a bounce turns the rest of
		 * the frame round */
		if (!ball_substep(g, b, vel_x(g, b) / n, vel_y(g, b) / n)) return;
		if (g->cleared || b->stuck) return;
	}
}

static void caps_frame(br_game_t *g) {
	int i;
	for (i = 0; i < BR_MAX_CAPS; i++) {
		br_cap_t *k = &g->cap[i];
		int y;
		if (!k->type) continue;
		k->y += CAP_FALL;
		y = k->y >> 8;
		if (y + BR_BH > BR_PADDLE_Y && y < BR_PADDLE_Y + BR_PADDLE_H &&
		    k->x + BR_BW > g->px && k->x < g->px + g->pw) {
			int t = k->type;
			k->type = BR_CAP_NONE;
			take(g, t, k->x + BR_BW / 2);
		} else if (y >= BR_FLOOR) {
			k->type = BR_CAP_NONE;
			ev(g, BR_EV_CAP_MISS, 0, 0, 0, k->x, BR_FLOOR);
		}
	}
}

static void shots_frame(br_game_t *g) {
	int i, s;
	for (i = 0; i < BR_MAX_SHOTS; i++) {
		br_shot_t *sh = &g->shot[i];
		if (!sh->live) continue;
		for (s = 0; s < SHOT_SPEED && sh->live; s++) {
			sh->y--;
			if (sh->y < BR_FY0) {
				sh->live = false;
				ev(g, BR_EV_SHOT_END, 0, 0, 0, sh->x, BR_FY0);
			} else if (hit_box(g, sh->x, sh->y, 2, 1, false)) {
				sh->live = false;
			}
		}
		if (g->cleared) return;
	}
}

void br_step(br_game_t *g) {
	int i, had = 0, lost_x = 0;

	if (g->over || g->cleared) return;
	g->frame++;
	if (g->fire_wait > 0) g->fire_wait--;

	for (i = 0; i < BR_MAX_BALLS; i++) {
		br_ball_t *b = &g->ball[i];
		if (!b->live) continue;
		had++;
		ball_frame(g, b);
		if (!b->live) lost_x = (b->x >> 8) + BR_BALL / 2;
		if (g->cleared) return;
	}
	if (!had) return;

	caps_frame(g);
	if (!g->cleared) shots_frame(g);
	if (g->cleared) return;

	if (br_balls(g) == 0) {
		g->lives--;
		memset(g->cap, 0, sizeof(g->cap));
		memset(g->shot, 0, sizeof(g->shot));
		if (g->power == BR_P_WIDE) { g->px += (BR_PADDLE_WIDE - BR_PADDLE_W) / 2; g->pw = BR_PADDLE_W; }
		g->power = BR_P_NONE;
		clamp_paddle(g);
		ev(g, BR_EV_LOST, 0, 0, 0, lost_x, BR_FLOOR);
		if (g->lives <= 0) { g->lives = 0; g->over = true; }
	} else if (lost_x) {
		ev(g, BR_EV_BALL_LOST, 0, 0, 0, lost_x, BR_FLOOR);
	}
}

/* ===================================================================
 * the autopilot
 * =================================================================== */

/* Where a ball's middle will be when it comes down to the paddle,
 * bouncing off the side walls (bricks ignored). */
static int predict_x(const br_game_t *g, const br_ball_t *b) {
	int32_t vx = vel_x(g, b), vy = vel_y(g, b);
	int32_t dist = ((int32_t)(BR_PADDLE_Y - BR_BALL) << 8) - b->y;
	int32_t span = (int32_t)(BR_FX1 - BR_BALL - BR_FX0) << 8;
	int32_t u;
	if (vy <= 0) {
		/* going up: as far again up and back down, roughly */
		dist += 2 * (b->y - ((int32_t)BR_FY0 << 8));
		vy = -vy;
	}
	if (vy == 0) vy = 1;
	u = b->x - ((int32_t)BR_FX0 << 8) + (int32_t)(((int64_t)vx * dist) / vy);
	u %= 2 * span;
	if (u < 0) u += 2 * span;
	if (u > span) u = 2 * span - u;
	return BR_FX0 + (u >> 8) + BR_BALL / 2;
}

int br_ai_target(const br_game_t *g, bool *fire) {
	int i, best = -1, target, offset;
	int32_t besty = -1;
	bool held = false;

	*fire = false;
	for (i = 0; i < BR_MAX_BALLS; i++) {
		const br_ball_t *b = &g->ball[i];
		int32_t score;
		if (!b->live) continue;
		if (b->stuck) { held = true; continue; }
		/* the most urgent: coming down, and lowest */
		score = b->y + (b->sy > 0 ? (1 << 24) : 0);
		if (score > besty) { besty = score; best = i; }
	}

	if (best < 0) {
		*fire = held && (g->frame % 50) == 49;
		return g->px + g->pw / 2 + (held ? ((int)(g->frame / 50) % 3 - 1) * 4 : 0);
	}

	/* nothing coming down soon: fetch a capsule */
	if (g->ball[best].sy < 0 || (g->ball[best].y >> 8) < 120) {
		for (i = 0; i < BR_MAX_CAPS; i++)
			if (g->cap[i].type && (g->cap[i].y >> 8) > 150)
				return g->cap[i].x + BR_BW / 2;
	}

	target = predict_x(g, &g->ball[best]);
	/* hit it off-centre, the side changing now and then, so the ball
	 * goes somewhere new */
	offset = ((int)((g->frame / 211) % 5) - 2) * (g->pw / 6);
	if (g->power == BR_P_LASER)
		*fire = (g->frame & 7) == 0;
	return target - offset;
}
