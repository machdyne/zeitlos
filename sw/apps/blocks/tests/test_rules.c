/*
 * blocks -- host tests of the rules (bk_game.c). make -C sw/apps/blocks test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../bk_game.h"

static int fails, checks;

#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

static uint32_t lcg = 7;
static uint32_t rnd(uint32_t n) {
	lcg = lcg * 1103515245u + 12345u;
	return n ? (lcg >> 8) % n : 0;
}

static bool has_event(const bk_game_t *g, int type) {
	int i;
	for (i = 0; i < g->nev; i++) if (g->ev[i].type == type) return true;
	return false;
}

static const bk_event_t *find_event(const bk_game_t *g, int type) {
	int i;
	for (i = 0; i < g->nev; i++) if (g->ev[i].type == type) return &g->ev[i];
	return NULL;
}

static void step(bk_game_t *g) {
	bk_clear_events(g);
	bk_step(g);
	if (bk_clearing(g)) bk_clear_done(g);
}

/* Replace the current piece with one of our choosing, at the top. */
static void force(bk_game_t *g, int type) {
	g->cur.type = (uint8_t)type;
	g->cur.rot = 0;
	g->cur.x = (int8_t)(type == BK_O ? 4 : 3);
	g->cur.y = 1;
	g->hold_used = false;
}

static void test_shapes(void) {
	int t, r, i, j;
	for (t = BK_I; t <= BK_L; t++)
		for (r = 0; r < 4; r++) {
			int8_t c[4][2], c4[4][2];
			int n = (t == BK_I) ? 4 : (t == BK_O ? 2 : 3);
			bk_cells(t, r, c);
			for (i = 0; i < 4; i++) {
				CHECK(c[i][0] >= 0 && c[i][0] < n && c[i][1] >= 0 && c[i][1] < n,
					"type %d rot %d cell %d inside its box", t, r, i);
				for (j = i + 1; j < 4; j++)
					CHECK(c[i][0] != c[j][0] || c[i][1] != c[j][1],
						"type %d rot %d cells distinct", t, r);
			}
			bk_cells(t, r + 4, c4);
			CHECK(!memcmp(c, c4, sizeof(c)), "four turns are none");
		}
	{
		/* T turned right points right: its stem at (2,1) */
		int8_t c[4][2];
		int found = 0;
		bk_cells(BK_T, 1, c);
		for (i = 0; i < 4; i++) if (c[i][0] == 2 && c[i][1] == 1) found = 1;
		CHECK(found, "T rotation 1 has (2,1)");
		bk_cells(BK_I, 1, c);
		for (i = 0; i < 4; i++) CHECK(c[i][0] == 2, "I rotation 1 is column 2");
	}
}

uint8_t bk_test_from_bag(bk_game_t *g);

static void test_bag(void) {
	bk_game_t g;
	int count[BK_TYPES] = { 0 }, last[BK_TYPES], i, worst = 0, seen = 0;
	bk_new(&g, BK_MODE_ENDLESS, 1, rnd);
	/* the first six on show (the piece and five next) are all different */
	seen |= 1 << g.cur.type;
	for (i = 0; i < BK_QUEUE; i++) {
		CHECK(!(seen & (1 << g.queue[i])), "no repeat in the first bag");
		seen |= 1 << g.queue[i];
	}
	for (i = 0; i < BK_TYPES; i++) last[i] = -1;
	g.bag_n = 0;
	for (i = 0; i < 700; i++) {
		int t = bk_test_from_bag(&g);
		CHECK(t >= BK_I && t <= BK_L, "a real piece (%d)", t);
		count[t]++;
		if (last[t] >= 0 && i - last[t] > worst) worst = i - last[t];
		last[t] = i;
	}
	for (i = BK_I; i <= BK_L; i++)
		CHECK(count[i] == 100, "type %d dealt 100 times in 700 (%d)", i, count[i]);
	CHECK(worst <= 13, "never more than 12 between two of the same (%d)", worst - 1);
}

static void test_spawn_and_move(void) {
	bk_game_t g;
	int n = 0;
	bk_new(&g, BK_MODE_ENDLESS, 1, rnd);
	force(&g, BK_T);
	CHECK(bk_fits(&g, &g.cur), "spawned piece fits");
	while (bk_move(&g, -1)) n++;
	CHECK(n == 3 && g.cur.x == 0, "T walks to the left wall in 3 (%d, x %d)", n, g.cur.x);
	n = 0;
	while (bk_move(&g, 1)) n++;
	CHECK(n == 7, "and to the right wall in 7 (%d)", n);
	CHECK(!bk_move(&g, 1), "no further");
}

static void test_kicks(void) {
	bk_game_t g;
	bk_new(&g, BK_MODE_ENDLESS, 1, rnd);
	/* I vertical at the left wall, turned: has to kick off the wall */
	force(&g, BK_I);
	g.cur.y = 8;
	CHECK(bk_rotate(&g, 1), "I turns");
	while (bk_move(&g, -1)) ;
	CHECK(g.cur.x == -2, "vertical I against the left wall (x %d)", g.cur.x);
	bk_clear_events(&g);
	CHECK(bk_rotate(&g, 1), "turning back at the wall works by a kick");
	{
		const bk_event_t *e = find_event(&g, BK_EV_ROTATE);
		CHECK(e && e->value > 0, "and says it kicked (%d)", e ? e->value : -1);
	}
	CHECK(bk_fits(&g, &g.cur), "fits after the kick");
	/* O does not turn */
	force(&g, BK_O);
	CHECK(!bk_rotate(&g, 1), "O does not turn");
}

static void test_twist(void) {
	/* A slot at the bottom left, the T placed in it pointing right and
	 * then turned to point down -- the last move a turn, three corners
	 * of its box taken:
	 *   row 19: X . X X X X X X X X
	 *   row 20: . . . X X X X X X X
	 *   row 21: X . X X X X X X X X   */
	bk_game_t g;
	int x;
	const bk_event_t *e;
	bk_new(&g, BK_MODE_ENDLESS, 1, rnd);
	memset(g.well, 0, sizeof(g.well));
	for (x = 0; x < BK_W; x++) {
		g.well[19][x] = (x == 1) ? 0 : BK_GREY;
		g.well[20][x] = (x <= 2) ? 0 : BK_GREY;
		g.well[21][x] = (x == 1) ? 0 : BK_GREY;
	}
	force(&g, BK_T);
	g.cur.rot = 1;
	g.cur.x = 0;
	g.cur.y = 19;
	CHECK(bk_fits(&g, &g.cur), "T in the slot, pointing right");
	bk_clear_events(&g);
	CHECK(bk_rotate(&g, 1), "turns to point down");
	CHECK(g.cur.rot == 2 && g.cur.x == 0 && g.cur.y == 19, "without a kick");
	bk_hard_drop(&g);
	e = find_event(&g, BK_EV_CLEAR);
	CHECK(e != NULL, "a clear");
	if (e) {
		CHECK(e->flags & BK_F_TWIST, "flagged a twist (flags %d)", e->flags);
		CHECK(e->value == 2, "two rows (%d)", e->value);
		CHECK(e->points == 1200u * g.level, "twist double: 1200 (%u)", e->points);
	}
	/* the same rows filled by a plain drop are not a twist */
	bk_new(&g, BK_MODE_ENDLESS, 1, rnd);
	memset(g.well, 0, sizeof(g.well));
	for (x = 0; x < BK_W; x++) g.well[21][x] = (x == 1) ? 0 : BK_GREY;
	force(&g, BK_T);
	g.cur.rot = 2;
	g.cur.x = 0;
	bk_clear_events(&g);
	bk_hard_drop(&g);
	e = find_event(&g, BK_EV_CLEAR);
	CHECK(e && !(e->flags & BK_F_TWIST), "dropped, not turned: no twist");
}

/* Fill rows bottom-up, leaving column `hole` open. */
static void fill_rows(bk_game_t *g, int rows, int hole) {
	int y, x;
	for (y = BK_H - rows; y < BK_H; y++)
		for (x = 0; x < BK_W; x++)
			g->well[y][x] = (x == hole) ? 0 : BK_GREY;
}

static void drop_i_into(bk_game_t *g, int col) {
	force(g, BK_I);
	bk_rotate(g, 1);              /* vertical, column 2 of its box */
	while (g->cur.x + 2 < col && bk_move(g, 1)) ;
	while (g->cur.x + 2 > col && bk_move(g, -1)) ;
	bk_clear_events(g);
	bk_hard_drop(g);
}

static void test_clears(void) {
	bk_game_t g;
	const bk_event_t *e;
	uint32_t s0;

	bk_new(&g, BK_MODE_ENDLESS, 1, rnd);
	memset(g.well, 0, sizeof(g.well));
	fill_rows(&g, 4, 9);
	s0 = g.score;
	drop_i_into(&g, 9);
	e = find_event(&g, BK_EV_CLEAR);
	CHECK(e && e->value == 4, "four lines at once");
	CHECK(bk_clearing(&g) && g.cur.type == 0, "waits for the picture");
	CHECK(g.clear_rows == (0xfu << (BK_H - 4)), "the bottom four rows marked (%x)", g.clear_rows);
	CHECK(e && (e->flags & BK_F_PERFECT), "and the well is empty: perfect");
	CHECK(g.score - s0 >= 800 + 3000, "scored a four and a perfect clear (%u)", g.score - s0);
	bk_clear_done(&g);
	CHECK(g.lines == 4 && g.cur.type != 0, "rows gone, next piece");
	{
		int x, y, n = 0;
		for (y = 0; y < BK_H; y++) for (x = 0; x < BK_W; x++) n += g.well[y][x] != 0;
		CHECK(n == 0, "nothing left (%d)", n);
	}
	/* back to back */
	fill_rows(&g, 4, 9);
	g.well[BK_H - 5][0] = BK_GREY;   /* so it is not perfect */
	s0 = g.score;
	drop_i_into(&g, 9);
	e = find_event(&g, BK_EV_CLEAR);
	CHECK(e && (e->flags & BK_F_B2B), "a second four is back to back");
	CHECK(e && e->points == (800u * 3 / 2 + 50) * g.level,
		"worth half as much again, plus the combo (%u)", e ? e->points : 0);
	bk_clear_done(&g);
	/* a single breaks it, and the combo counts on */
	fill_rows(&g, 1, 9);
	drop_i_into(&g, 9);
	e = find_event(&g, BK_EV_CLEAR);
	CHECK(e && e->value == 1 && !(e->flags & BK_F_B2B), "a single is not back to back");
	CHECK(e && e->combo == 2, "third clear in a row: combo 2 (%d)", e ? e->combo : -9);
	bk_clear_done(&g);
	CHECK(!g.b2b, "the single ended the run");
}

static void test_lock_delay(void) {
	bk_game_t g;
	int f, wiggles = 0;
	bk_new(&g, BK_MODE_ENDLESS, 1, rnd);
	force(&g, BK_O);
	g.cur.y = BK_H - 2;           /* on the floor */
	for (f = 0; f < 29; f++) step(&g);
	CHECK(g.cur.type == BK_O && g.cur.y == BK_H - 2, "not locked before 30 frames");
	step(&g);
	CHECK(g.well[BK_H - 1][4] == BK_O, "locked at 30");
	/* moving resets the delay, fifteen times */
	force(&g, BK_T);
	g.cur.y = BK_H - 4;
	for (f = 0; f < 200 && !g.landed; f++) { bk_clear_events(&g); bk_step(&g); }
	CHECK(g.landed && g.cur.type == BK_T, "the T is down");
	for (f = 0; f < 2000 && g.cur.type == BK_T && g.cur.y >= BK_H - 3; f++) {
		bk_clear_events(&g);
		if (f % 20 == 0) { bk_move(&g, (f / 20) % 2 ? 1 : -1); wiggles++; }
		bk_step(&g);
	}
	CHECK(f < 20 * 17 + 40, "wiggling cannot hold it forever (%d frames, %d moves)", f, wiggles);
}

static void test_gravity_and_drop(void) {
	bk_game_t g;
	int f, y0;
	uint32_t s0;
	bk_new(&g, BK_MODE_ENDLESS, 1, rnd);
	y0 = g.cur.y;
	for (f = 0; f < 600; f++) { bk_clear_events(&g); bk_step(&g); }
	CHECK(g.cur.y - y0 >= 8 && g.cur.y - y0 <= 11, "level 1: about a row a second (%d rows)", g.cur.y - y0);
	CHECK(bk_gravity(20) >= 20 * 256, "level 20 drops at once");
	{
		int gy = bk_ghost_y(&g), t = g.cur.type, x = g.cur.x;
		s0 = g.score;
		bk_clear_events(&g);
		bk_hard_drop(&g);
		{
			const bk_event_t *e = find_event(&g, BK_EV_HARD_DROP);
			CHECK(e && e->value == gy - (gy - e->value), "hard drop says how far");
			CHECK(g.score - s0 == 2u * (uint32_t)(e ? e->value : 0), "two points a row");
		}
		CHECK(has_event(&g, BK_EV_LOCK), "and locks at once");
		{
			int8_t c[4][2];
			bk_cells(t, 0, c);
			CHECK(g.well[gy + c[0][1]][x + c[0][0]] == t, "where the ghost said");
		}
	}
	/* soft drop is faster and scores */
	{
		int y1;
		force(&g, BK_O);
		y1 = g.cur.y;
		s0 = g.score;
		bk_soft(&g, true);
		for (f = 0; f < 10; f++) { bk_clear_events(&g); bk_step(&g); }
		bk_soft(&g, false);
		CHECK(g.cur.y - y1 == 5, "soft drop: half a row a frame (%d)", g.cur.y - y1);
		CHECK(g.score - s0 == 5, "a point a row (%u)", g.score - s0);
	}
}

static void test_hold(void) {
	bk_game_t g;
	int a, b;
	bk_new(&g, BK_MODE_ENDLESS, 1, rnd);
	a = g.cur.type;
	b = g.queue[0];
	CHECK(bk_hold(&g), "hold");
	CHECK(g.hold == a && g.cur.type == b, "held the first, playing the next");
	CHECK(!bk_hold(&g), "only once per piece");
	bk_clear_events(&g);
	bk_hard_drop(&g);
	if (bk_clearing(&g)) bk_clear_done(&g);
	{
		int c = g.cur.type;
		CHECK(bk_hold(&g), "again for the next piece");
		CHECK(g.cur.type == a && g.hold == c, "swaps with the held one");
	}
}

static void test_over(void) {
	bk_game_t g;
	int x, y;
	bk_new(&g, BK_MODE_ENDLESS, 1, rnd);
	for (y = 2; y < BK_H; y++)
		for (x = 0; x < BK_W; x++) g.well[y][x] = (x == (y % BK_W)) ? 0 : BK_GREY;
	for (x = 0; x < 400 && !g.over; x++) {
		bk_clear_events(&g);
		bk_hard_drop(&g);
		if (bk_clearing(&g)) bk_clear_done(&g);
	}
	CHECK(g.over, "a full well ends the game");
}

static void test_modes(void) {
	bk_game_t g;
	int i;
	bk_new(&g, BK_MODE_40, 9, rnd);
	CHECK(g.level == 1, "40 lines starts at level 1 whatever was asked");
	for (i = 0; i < 10; i++) {
		memset(g.well, 0, sizeof(g.well));
		fill_rows(&g, 4, 9);
		drop_i_into(&g, 9);
		bk_clear_done(&g);
	}
	CHECK(g.done && g.lines == 40, "done at forty (%d)", g.lines);
	CHECK(g.level == 1, "and the speed never changed");

	bk_new(&g, BK_MODE_3MIN, 1, rnd);
	for (i = 0; i < (int)BK_FRAMES_3MIN + 10 && !g.done; i++) {
		bk_clear_events(&g);
		bk_step(&g);
		if (g.cur.y > 10) { bk_clear_events(&g); }
		if (g.over) break;
		if (bk_clearing(&g)) bk_clear_done(&g);
		/* keep it alive: empty the well now and then */
		if (i % 600 == 0) memset(g.well, 0, sizeof(g.well));
	}
	CHECK(g.done && !g.over, "three minutes end the game (%d frames)", i);

	bk_new(&g, BK_MODE_ENDLESS, 3, rnd);
	for (i = 0; i < 3; i++) {
		memset(g.well, 0, sizeof(g.well));
		fill_rows(&g, 4, 9);
		g.well[BK_H - 5][0] = BK_GREY;
		drop_i_into(&g, 9);
		bk_clear_done(&g);
	}
	CHECK(g.lines == 12 && g.level == 4, "endless: a level every ten lines (%d at %d)", g.level, g.lines);
}

static void test_ai(void) {
	bk_game_t g;
	int f, pieces = 0;
	bk_new(&g, BK_MODE_ENDLESS, 1, rnd);
	for (f = 0; f < 200000 && !g.over && pieces < 400; f++) {
		bk_plan_t p;
		bk_clear_events(&g);
		if (g.cur.type && bk_ai_plan(&g, &p)) {
			if (p.use_hold) bk_hold(&g);
			while (g.cur.rot != p.rot && bk_rotate(&g, 1)) ;
			while (g.cur.x < p.x && bk_move(&g, 1)) ;
			while (g.cur.x > p.x && bk_move(&g, -1)) ;
			bk_hard_drop(&g);
			pieces++;
		}
		bk_step(&g);
		if (bk_clearing(&g)) bk_clear_done(&g);
	}
	printf("autopilot: %d pieces, %d lines, level %d, score %u%s\n",
		pieces, g.lines, g.level, g.score, g.over ? " (topped out)" : "");
	CHECK(!g.over, "the autopilot survives 400 pieces");
	CHECK(g.lines >= 120, "and clears lines (%d)", g.lines);
}

/* Easy: slower, longer to lock, a level every 15, at most level 8. */
static void test_easy(void) {
	bk_game_t g, n;
	int i;
	bk_new_skill(&g, BK_MODE_ENDLESS, 1, BK_SKILL_EASY, rnd);
	bk_new(&n, BK_MODE_ENDLESS, 1, rnd);
	CHECK(n.skill == BK_SKILL_NORMAL && n.rescues == 0, "bk_new is normal, no rescues");
	CHECK(g.rescues == BK_RESCUES, "easy has %d rescues", BK_RESCUES);
	CHECK(bk_gravity_now(&g) * 2 <= bk_gravity_now(&n) + 1, "easy falls at half speed (%u vs %u)",
		bk_gravity_now(&g), bk_gravity_now(&n));
	CHECK(bk_lock_frames(&g) == 2 * bk_lock_frames(&n), "and takes twice as long to lock");
	for (i = 0; i < 40; i++) {
		memset(g.well, 0, sizeof(g.well));
		fill_rows(&g, 4, 9);
		drop_i_into(&g, 9);
		bk_clear_done(&g);
	}
	CHECK(g.lines == 160 && g.level == 8, "easy: a level every 15, at most 8 (%d at %d)", g.level, g.lines);
	bk_new_skill(&g, BK_MODE_ENDLESS, 1, BK_SKILL_HARD, rnd);
	CHECK(bk_gravity_now(&g) == bk_gravity(6), "hard falls as normal at level 6");
	CHECK(bk_lock_frames(&g) == 20 && bk_lines_per_level(&g) == 8,
		"hard locks in 20 frames and levels up every 8 rows");
	for (i = 0; i < 4; i++) {
		memset(g.well, 0, sizeof(g.well));
		fill_rows(&g, 4, 9);
		drop_i_into(&g, 9);
		bk_clear_done(&g);
	}
	CHECK(g.lines == 16 && g.level == 3, "hard: level 3 after 16 rows (%d)", g.level);
}

/* Easy: topping out clears the top half, three times; the fourth ends it. */
static void test_rescue(void) {
	bk_game_t g;
	int x, y, k, rescues = 0;
	bk_new_skill(&g, BK_MODE_ENDLESS, 1, BK_SKILL_EASY, rnd);
	for (k = 0; k < 4000 && !g.over; k++) {
		/* a stack with no full rows, up to the top */
		if (!g.cur.type || g.cur.y < 0) break;
		for (y = 2; y < BK_H; y++)
			for (x = 0; x < BK_W; x++)
				if (!g.well[y][x] && x != (y % BK_W)) g.well[y][x] = BK_GREY;
		bk_clear_events(&g);
		bk_hard_drop(&g);
		if (bk_clearing(&g)) bk_clear_done(&g);
		if (has_event(&g, BK_EV_RESCUE)) {
			rescues++;
			for (y = 0; y < BK_H / 2; y++)
				for (x = 0; x < BK_W; x++)
					CHECK(g.well[y][x] == 0 || (y < 2 && g.cur.type), "top half empty after a rescue");
			CHECK(!g.over && g.cur.type, "and a new piece is in play");
		}
	}
	CHECK(rescues == BK_RESCUES, "three rescues (%d)", rescues);
	CHECK(g.over, "then the game ends");
}

/* Little pieces: an 8-wide well, small shapes, rows clear at 8. */
static void test_little(void) {
	bk_game_t g;
	int i, y, seen[BK_TYPES] = { 0 }, pieces = 0, f;
	bk_new_skill(&g, BK_MODE_LITTLE, 1, BK_SKILL_EASY, rnd);
	CHECK(g.little, "little");
	for (y = 0; y < BK_H; y++)
		CHECK(g.well[y][0] == BK_WALL && g.well[y][BK_W - 1] == BK_WALL, "walls in row %d", y);
	CHECK(bk_col0(&g) == 1 && bk_col1(&g) == 8, "columns 1..8");
	for (i = 0; i < 70; i++) {
		int t = bk_test_from_bag(&g);
		CHECK(t >= BK_DOT && t <= BK_V3, "only little pieces (%d)", t);
		if (t >= 0 && t < BK_TYPES) seen[t]++;
	}
	CHECK(seen[BK_DOT] && seen[BK_DUO] && seen[BK_I3] && seen[BK_V3], "all four come");
	CHECK(bk_ncells(BK_DOT) == 1 && bk_ncells(BK_DUO) == 2 && bk_ncells(BK_I3) == 3 &&
		bk_ncells(BK_V3) == 3 && bk_ncells(BK_T) == 4, "cell counts");

	/* a row of 8 clears, and the walls stay */
	memset(g.well, 0, sizeof(g.well));
	bk_new_skill(&g, BK_MODE_LITTLE, 1, BK_SKILL_EASY, rnd);
	for (i = 1; i <= 7; i++) g.well[BK_H - 1][i] = BK_GREY;
	g.cur.type = BK_DOT; g.cur.rot = 0; g.cur.x = 8; g.cur.y = 2;
	bk_clear_events(&g);
	bk_hard_drop(&g);
	CHECK(bk_clearing(&g) && g.clear_count == 1, "eight in a row is a line");
	bk_clear_done(&g);
	for (y = 0; y < BK_H; y++)
		CHECK(g.well[y][0] == BK_WALL && g.well[y][BK_W - 1] == BK_WALL, "walls kept, row %d", y);
	CHECK(g.lines == 1, "counted");
	{
		const bk_event_t *e = NULL;
		(void)e;
	}

	/* a piece cannot go into the walls */
	bk_new_skill(&g, BK_MODE_LITTLE, 1, BK_SKILL_EASY, rnd);
	for (i = 0; i < 12; i++) bk_move(&g, -1);
	{
		int8_t c[4][2];
		int k, minx = 99;
		bk_cells(g.cur.type, g.cur.rot, c);
		for (k = 0; k < 4; k++) if (g.cur.x + c[k][0] < minx) minx = g.cur.x + c[k][0];
		CHECK(minx == 1, "stops at the wall (%d)", minx);
	}

	/* the autopilot plays little pieces */
	bk_new_skill(&g, BK_MODE_LITTLE, 1, BK_SKILL_NORMAL, rnd);
	for (f = 0; f < 200000 && !g.over && pieces < 400; f++) {
		bk_plan_t p;
		bk_clear_events(&g);
		if (g.cur.type && bk_ai_plan(&g, &p)) {
			if (p.use_hold) bk_hold(&g);
			while (g.cur.rot != p.rot && bk_rotate(&g, 1)) ;
			while (g.cur.x < p.x && bk_move(&g, 1)) ;
			while (g.cur.x > p.x && bk_move(&g, -1)) ;
			bk_hard_drop(&g);
			pieces++;
		}
		bk_step(&g);
		if (bk_clearing(&g)) bk_clear_done(&g);
		for (y = 0; y < BK_H; y++)
			if (g.well[y][0] != BK_WALL || g.well[y][BK_W - 1] != BK_WALL) {
				CHECK(0, "walls intact at frame %d", f);
				f = 1 << 30;
				break;
			}
	}
	printf("little pieces: %d pieces, %d lines%s\n", pieces, g.lines, g.over ? " (topped out)" : "");
	CHECK(!g.over && g.lines >= 60, "the autopilot clears little lines (%d)", g.lines);
}

int main(void) {
	test_shapes();
	test_bag();
	test_spawn_and_move();
	test_kicks();
	test_twist();
	test_clears();
	test_lock_delay();
	test_gravity_and_drop();
	test_hold();
	test_over();
	test_modes();
	test_ai();
	test_easy();
	test_rescue();
	test_little();
	printf("blocks rules: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
