/*
 * blocks -- the rules. See bk_game.h.
 */

#include <string.h>

#include "bk_game.h"

/* ===================================================================
 * shapes
 * =================================================================== */

/* Rotation 0 of each shape in its box, y down, and the box's size. */
static const int8_t base_cells[BK_TYPES][4][2] = {
	{ { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 } },
	{ { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 } },   /* I */
	{ { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } },   /* O */
	{ { 1, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } },   /* T */
	{ { 1, 0 }, { 2, 0 }, { 0, 1 }, { 1, 1 } },   /* S */
	{ { 0, 0 }, { 1, 0 }, { 1, 1 }, { 2, 1 } },   /* Z */
	{ { 0, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } },   /* J */
	{ { 2, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } },   /* L */
};

static const uint8_t box_size[BK_TYPES] = { 0, 4, 2, 3, 3, 3, 3, 3 };

void bk_cells(int type, int rot, int8_t out[4][2]) {
	int i, r, n;
	if (type < 1 || type > BK_L) type = BK_O;
	n = box_size[type];
	for (i = 0; i < 4; i++) {
		int x = base_cells[type][i][0], y = base_cells[type][i][1];
		/* clockwise, rot times: (x, y) -> (n-1-y, x) */
		for (r = 0; r < (rot & 3); r++) {
			int t = x;
			x = n - 1 - y;
			y = t;
		}
		out[i][0] = (int8_t)x;
		out[i][1] = (int8_t)y;
	}
}

/* -- wall kicks --
 *
 * Offsets to try, in order, for each turn: written with y UP as these
 * tables usually are, negated when applied (the well's y is down). The
 * first entry is "no kick". Index by from*2 + (clockwise ? 0 : 1).
 */
static const int8_t kick_jlstz[8][5][2] = {
	/* 0 -> R */ { { 0, 0 }, { -1, 0 }, { -1, 1 }, { 0, -2 }, { -1, -2 } },
	/* 0 -> L */ { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, -2 }, { 1, -2 } },
	/* R -> 2 */ { { 0, 0 }, { 1, 0 }, { 1, -1 }, { 0, 2 }, { 1, 2 } },
	/* R -> 0 */ { { 0, 0 }, { 1, 0 }, { 1, -1 }, { 0, 2 }, { 1, 2 } },
	/* 2 -> L */ { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, -2 }, { 1, -2 } },
	/* 2 -> R */ { { 0, 0 }, { -1, 0 }, { -1, 1 }, { 0, -2 }, { -1, -2 } },
	/* L -> 0 */ { { 0, 0 }, { -1, 0 }, { -1, -1 }, { 0, 2 }, { -1, 2 } },
	/* L -> 2 */ { { 0, 0 }, { -1, 0 }, { -1, -1 }, { 0, 2 }, { -1, 2 } },
};

static const int8_t kick_i[8][5][2] = {
	/* 0 -> R */ { { 0, 0 }, { -2, 0 }, { 1, 0 }, { -2, -1 }, { 1, 2 } },
	/* 0 -> L */ { { 0, 0 }, { -1, 0 }, { 2, 0 }, { -1, 2 }, { 2, -1 } },
	/* R -> 2 */ { { 0, 0 }, { -1, 0 }, { 2, 0 }, { -1, 2 }, { 2, -1 } },
	/* R -> 0 */ { { 0, 0 }, { 2, 0 }, { -1, 0 }, { 2, 1 }, { -1, -2 } },
	/* 2 -> L */ { { 0, 0 }, { 2, 0 }, { -1, 0 }, { 2, 1 }, { -1, -2 } },
	/* 2 -> R */ { { 0, 0 }, { 1, 0 }, { -2, 0 }, { 1, -2 }, { -2, 1 } },
	/* L -> 0 */ { { 0, 0 }, { 1, 0 }, { -2, 0 }, { 1, -2 }, { -2, 1 } },
	/* L -> 2 */ { { 0, 0 }, { -2, 0 }, { 1, 0 }, { -2, -1 }, { 1, 2 } },
};

/* ===================================================================
 * tuning
 * =================================================================== */

/* Rows a frame x256, from the familiar curve
 * seconds per row = (0.8 - (level-1) * 0.007) ^ (level-1). */
static const uint16_t gravity_table[21] = {
	4, 4, 5, 7, 9, 12, 16, 22, 32, 45, 67,
	99, 151, 235, 373, 604, 1001, 1693, 2928, 5120, 5120
};

#define SOFT_RATE     128     /* half a row a frame */
#define LOCK_FRAMES    30
#define LOCK_RESETS    15
#define MAX_LEVEL      20

uint32_t bk_gravity(int level) {
	if (level < 1) level = 1;
	if (level > MAX_LEVEL) level = MAX_LEVEL;
	return gravity_table[level];
}

static const uint16_t line_points[5] = { 0, 100, 300, 500, 800 };
static const uint16_t twist_points[5] = { 400, 800, 1200, 1600, 1600 };

/* ===================================================================
 * the well
 * =================================================================== */

static void event(bk_game_t *g, int type, int value) {
	bk_event_t *e;
	if (g->nev >= BK_MAX_EV) return;
	e = &g->ev[g->nev++];
	memset(e, 0, sizeof(*e));
	e->type = (uint8_t)type;
	e->value = (int16_t)value;
	e->x = g->cur.x;
	e->y = g->cur.y;
	e->piece = g->cur.type;
	e->rot = g->cur.rot;
}

void bk_clear_events(bk_game_t *g) {
	g->nev = 0;
}

bool bk_fits(const bk_game_t *g, const bk_piece_t *p) {
	int8_t c[4][2];
	int i;
	if (!p->type) return false;
	bk_cells(p->type, p->rot, c);
	for (i = 0; i < 4; i++) {
		int x = p->x + c[i][0], y = p->y + c[i][1];
		if (x < 0 || x >= BK_W || y < 0 || y >= BK_H) return false;
		if (g->well[y][x]) return false;
	}
	return true;
}

static uint8_t from_bag(bk_game_t *g) {
	uint8_t t;
	if (g->bag_n == 0) {
		int i;
		for (i = 0; i < 7; i++) g->bag[i] = (uint8_t)(BK_I + i);
		/* Fisher-Yates */
		for (i = 6; i > 0; i--) {
			int j = (int)g->rand_below((uint32_t)i + 1);
			uint8_t tmp = g->bag[i];
			g->bag[i] = g->bag[j];
			g->bag[j] = tmp;
		}
		g->bag_n = 7;
	}
	t = g->bag[--g->bag_n];
	return t;
}

static uint8_t next_from_queue(bk_game_t *g) {
	uint8_t t = g->queue[0];
	memmove(g->queue, g->queue + 1, BK_QUEUE - 1);
	g->queue[BK_QUEUE - 1] = from_bag(g);
	return t;
}

static void reset_piece_state(bk_game_t *g) {
	g->fall = 0;
	g->lock_timer = 0;
	g->lock_resets = 0;
	g->landed = false;
	g->last_rotate = false;
	g->last_kick = 0;
}

static bool on_ground(const bk_game_t *g) {
	bk_piece_t p = g->cur;
	p.y++;
	return !bk_fits(g, &p);
}

/* A new piece at the top. If it cannot appear, the game is over. */
static void spawn(bk_game_t *g, uint8_t type) {
	bk_piece_t p;
	p.type = type;
	p.rot = 0;
	p.x = (int8_t)(type == BK_O ? 4 : 3);
	p.y = 0;
	reset_piece_state(g);
	if (!bk_fits(g, &p)) {
		g->cur = p;
		g->cur.type = 0;
		g->over = true;
		event(g, BK_EV_OVER, 0);
		return;
	}
	/* and straight down a row into view, if it can */
	p.y++;
	if (!bk_fits(g, &p)) p.y--;
	g->cur = p;
	g->hold_used = false;
	event(g, BK_EV_SPAWN, type);
}

void bk_new(bk_game_t *g, bk_mode_t mode, int start_level,
	uint32_t (*rand_below)(uint32_t n)) {
	int i;
	memset(g, 0, sizeof(*g));
	g->mode = mode;
	g->rand_below = rand_below;
	if (start_level < 1) start_level = 1;
	if (start_level > MAX_LEVEL) start_level = MAX_LEVEL;
	if (mode != BK_MODE_ENDLESS) start_level = 1;
	g->start_level = (uint8_t)start_level;
	g->level = (uint8_t)start_level;
	g->combo = -1;
	for (i = 0; i < BK_QUEUE; i++) g->queue[i] = from_bag(g);
	spawn(g, next_from_queue(g));
}

bool bk_clearing(const bk_game_t *g) {
	return g->clear_rows != 0;
}

static bool active(const bk_game_t *g) {
	return g->cur.type && !g->over && !g->done && !g->clear_rows;
}

/* A successful move or turn while on the ground restarts the lock
 * delay -- a limited number of times, or a piece could be kept alive
 * forever by wiggling it. */
static void moved(bk_game_t *g) {
	bool was = g->landed;
	g->landed = on_ground(g);
	if (g->landed && g->lock_resets < LOCK_RESETS) {
		g->lock_timer = 0;
		g->lock_resets++;
	}
	if (g->landed && !was) event(g, BK_EV_LAND, 0);
}

bool bk_move(bk_game_t *g, int dx) {
	bk_piece_t p;
	if (!active(g)) return false;
	p = g->cur;
	p.x = (int8_t)(p.x + dx);
	if (!bk_fits(g, &p)) return false;
	g->cur = p;
	g->last_rotate = false;
	moved(g);
	event(g, BK_EV_MOVE, dx);
	return true;
}

bool bk_rotate(bk_game_t *g, int dir) {
	bk_piece_t p;
	int k, idx;
	const int8_t (*tab)[5][2];

	if (!active(g)) return false;
	if (g->cur.type == BK_O) return false;     /* it looks the same */

	idx = g->cur.rot * 2 + (dir > 0 ? 0 : 1);
	tab = (g->cur.type == BK_I) ? kick_i : kick_jlstz;

	for (k = 0; k < 5; k++) {
		p = g->cur;
		p.rot = (uint8_t)((p.rot + (dir > 0 ? 1 : 3)) & 3);
		p.x = (int8_t)(p.x + tab[idx][k][0]);
		p.y = (int8_t)(p.y - tab[idx][k][1]);
		if (bk_fits(g, &p)) {
			g->cur = p;
			g->last_rotate = true;
			g->last_kick = (uint8_t)k;
			moved(g);
			event(g, BK_EV_ROTATE, k);
			return true;
		}
	}
	return false;
}

void bk_soft(bk_game_t *g, bool on) {
	g->soft = on;
}

/* A T is a twist when the last thing it did was turn, and three of the
 * four corners of its box are taken (the walls and the floor count). */
static bool is_twist(const bk_game_t *g) {
	static const int8_t corner[4][2] = { { 0, 0 }, { 2, 0 }, { 0, 2 }, { 2, 2 } };
	int i, n = 0;
	if (g->cur.type != BK_T || !g->last_rotate) return false;
	for (i = 0; i < 4; i++) {
		int x = g->cur.x + corner[i][0], y = g->cur.y + corner[i][1];
		if (x < 0 || x >= BK_W || y >= BK_H || (y >= 0 && g->well[y][x])) n++;
	}
	return n >= 3;
}

static void set_level(bk_game_t *g) {
	int lv;
	if (g->mode == BK_MODE_40) return;
	lv = g->start_level + g->lines / 10;
	if (lv > MAX_LEVEL) lv = MAX_LEVEL;
	if (lv > g->level) {
		g->level = (uint8_t)lv;
		event(g, BK_EV_LEVEL, lv);
	}
}

static void lock_piece(bk_game_t *g) {

	int8_t c[4][2];
	int i, y, n = 0;
	bool twist, above = true;
	uint32_t rows = 0;

	bk_cells(g->cur.type, g->cur.rot, c);
	twist = is_twist(g);

	for (i = 0; i < 4; i++) {
		int x = g->cur.x + c[i][0], yy = g->cur.y + c[i][1];
		g->well[yy][x] = g->cur.type;
		if (yy >= BK_HIDDEN) above = false;
	}

	event(g, BK_EV_LOCK, g->cur.type);

	for (y = 0; y < BK_H; y++) {
		int x, full = 1;
		for (x = 0; x < BK_W; x++) if (!g->well[y][x]) { full = 0; break; }
		if (full) { rows |= 1u << y; n++; }
	}

	/* Scoring: at the level the clear was made at. */
	{
		uint32_t pts = 0;
		uint8_t flags = 0;
		bool hard = (n == 4) || (twist && n > 0);
		if (twist) { pts = twist_points[n]; flags |= BK_F_TWIST; }
		else pts = line_points[n];
		if (n > 0) {
			if (hard && g->b2b) { pts = pts * 3 / 2; flags |= BK_F_B2B; }
			g->b2b = hard;
			g->combo++;
			if (g->combo > 0) pts += 50u * (uint32_t)g->combo;
			/* perfect: nothing left once these rows go */
			{
				int cnt = 0, x;
				for (y = 0; y < BK_H; y++) {
					if (rows & (1u << y)) continue;
					for (x = 0; x < BK_W; x++) if (g->well[y][x]) cnt++;
				}
				if (cnt == 0) { pts += 3000; flags |= BK_F_PERFECT; }
			}
		} else {
			g->combo = -1;
		}
		pts *= g->level;
		g->score += pts;
		if (n > 0 || twist) {
			bk_event_t *e;
			event(g, BK_EV_CLEAR, n);
			e = &g->ev[g->nev - 1];
			e->flags = flags;
			e->combo = g->combo;
			e->points = pts;
		}
	}

	g->cur.type = 0;

	if (above && n == 0) {
		/* locked entirely above the well: out */
		g->over = true;
		event(g, BK_EV_OVER, 0);
		return;
	}

	if (n > 0) {
		g->clear_rows = rows;
		g->clear_count = (uint8_t)n;
		return;                       /* the picture's turn */
	}

	spawn(g, next_from_queue(g));
}

void bk_clear_done(bk_game_t *g) {
	int y, dst;
	if (!g->clear_rows) return;
	/* compact downward: from the bottom, keep the rows not cleared */
	dst = BK_H - 1;
	for (y = BK_H - 1; y >= 0; y--) {
		if (g->clear_rows & (1u << y)) continue;
		if (dst != y) memcpy(g->well[dst], g->well[y], BK_W);
		dst--;
	}
	for (; dst >= 0; dst--) memset(g->well[dst], 0, BK_W);

	g->lines = (uint16_t)(g->lines + g->clear_count);
	g->clear_rows = 0;
	g->clear_count = 0;

	set_level(g);

	if (g->mode == BK_MODE_40 && g->lines >= BK_GOAL_40) {
		g->done = true;
		event(g, BK_EV_DONE, 0);
		return;
	}
	spawn(g, next_from_queue(g));
}

void bk_hard_drop(bk_game_t *g) {
	int rows = 0;
	bk_piece_t p;
	if (!active(g)) return;
	p = g->cur;
	for (;;) {
		p.y++;
		if (!bk_fits(g, &p)) break;
		g->cur = p;
		rows++;
	}
	if (rows) g->last_rotate = false;
	g->score += 2u * (uint32_t)rows;
	event(g, BK_EV_HARD_DROP, rows);
	lock_piece(g);
}

bool bk_hold(bk_game_t *g) {
	uint8_t t;
	if (!active(g) || g->hold_used) return false;
	t = g->cur.type;
	event(g, BK_EV_HOLD, t);
	if (g->hold) {
		uint8_t h = g->hold;
		g->hold = t;
		spawn(g, h);
	} else {
		g->hold = t;
		spawn(g, next_from_queue(g));
	}
	g->hold_used = true;
	return true;
}

int bk_ghost_y(const bk_game_t *g) {
	bk_piece_t p = g->cur;
	if (!p.type) return p.y;
	while (1) {
		p.y++;
		if (!bk_fits(g, &p)) return p.y - 1;
	}
}

void bk_step(bk_game_t *g) {

	/* Events are NOT cleared here: the player's moves this frame have
	 * already added theirs. The caller clears once a frame
	 * (bk_clear_events()), before the input. */

	if (g->over || g->done) return;

	g->frames++;
	if (g->mode == BK_MODE_3MIN && g->frames >= BK_FRAMES_3MIN) {
		g->done = true;
		event(g, BK_EV_DONE, 0);
		return;
	}

	if (!active(g)) return;

	/* gravity */
	{
		uint32_t rate = bk_gravity(g->level);
		bool soft = g->soft && rate < SOFT_RATE;
		if (soft) rate = SOFT_RATE;
		g->fall += rate;
		while (g->fall >= 256) {
			bk_piece_t p = g->cur;
			g->fall -= 256;
			p.y++;
			if (!bk_fits(g, &p)) { g->fall = 0; break; }
			g->cur = p;
			g->last_rotate = false;
			if (soft) g->score += 1;
			/* a new lowest row: the lock delay starts over */
			g->lock_timer = 0;
			g->lock_resets = 0;
		}
	}

	/* the lock delay */
	{
		bool was = g->landed;
		g->landed = on_ground(g);
		if (g->landed && !was) event(g, BK_EV_LAND, 0);
		if (g->landed) {
			if (++g->lock_timer >= LOCK_FRAMES) lock_piece(g);
		} else {
			g->lock_timer = 0;
		}
	}
}

int bk_lines_goal(const bk_game_t *g) {
	int left = BK_GOAL_40 - (int)g->lines;
	return left > 0 ? left : 0;
}

uint32_t bk_frames_left(const bk_game_t *g) {
	return g->frames >= BK_FRAMES_3MIN ? 0 : BK_FRAMES_3MIN - g->frames;
}

/* ===================================================================
 * the autopilot
 *
 * Every rotation at every column, dropped straight down from the top,
 * scored by the well it leaves: lines cleared (good), total height,
 * holes and bumpiness (bad). The weights are a well-known hand-tuned
 * set, in hundredths. Good enough to clear lines for minutes; no
 * look-ahead, no twists. For the demo and the host renders.
 * =================================================================== */

static int evaluate(uint8_t w[BK_H][BK_W], int *lines_out) {
	int x, y, lines = 0, height = 0, holes = 0, bump = 0, prev = -1;
	int top[BK_W];

	for (y = 0; y < BK_H; y++) {
		int full = 1;
		for (x = 0; x < BK_W; x++) if (!w[y][x]) { full = 0; break; }
		lines += full;
	}

	for (x = 0; x < BK_W; x++) {
		int h = 0, seen = 0;
		for (y = 0; y < BK_H; y++) {
			if (w[y][x]) {
				if (!seen) { h = BK_H - y; seen = 1; }
			} else if (seen) {
				holes++;
			}
		}
		top[x] = h;
		height += h;
		if (prev >= 0) bump += (h > prev) ? h - prev : prev - h;
		prev = h;
	}
	(void)top;

	*lines_out = lines;
	return 76 * lines - 51 * height - 36 * holes - 18 * bump;
}

static bool best_for(const bk_game_t *g, int type, int *best_score,
	bk_plan_t *plan) {

	static uint8_t w[BK_H][BK_W];
	bool found = false;
	int rot, x;

	for (rot = 0; rot < 4; rot++) {
		if (type == BK_O && rot) break;
		for (x = -2; x < BK_W; x++) {
			bk_piece_t p;
			int8_t c[4][2];
			int i, lines, s;
			p.type = (uint8_t)type;
			p.rot = (uint8_t)rot;
			p.x = (int8_t)x;
			p.y = 0;
			if (!bk_fits(g, &p)) continue;
			while (1) {
				p.y++;
				if (!bk_fits(g, &p)) { p.y--; break; }
			}
			memcpy(w, g->well, sizeof(w));
			bk_cells(type, rot, c);
			for (i = 0; i < 4; i++) w[p.y + c[i][1]][p.x + c[i][0]] = (uint8_t)type;
			s = evaluate(w, &lines);
			if (!found || s > *best_score) {
				*best_score = s;
				plan->rot = (uint8_t)rot;
				plan->x = (int8_t)x;
				found = true;
			}
		}
	}
	return found;
}

bool bk_ai_plan(const bk_game_t *g, bk_plan_t *out) {
	int s1 = 0, s2 = 0;
	bk_plan_t a, b;
	bool fa, fb = false;
	if (!g->cur.type) return false;
	fa = best_for(g, g->cur.type, &s1, &a);
	if (!g->hold_used) {
		int other = g->hold ? g->hold : g->queue[0];
		fb = best_for(g, other, &s2, &b);
	}
	a.use_hold = false;
	if (fb && (!fa || s2 > s1 + 20)) {
		b.use_hold = true;
		*out = b;
		return true;
	}
	if (!fa) return false;
	*out = a;
	return true;
}

#ifdef BK_TESTS
uint8_t bk_test_from_bag(bk_game_t *g) { return from_bag(g); }
#endif
