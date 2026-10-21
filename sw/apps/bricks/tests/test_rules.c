/*
 * bricks -- host tests for the rules (br_game.c). make -C sw/apps/bricks test
 *
 * The particular cases first (a bounce, a silver brick, each capsule),
 * then every level played by the autopilot for a long time, many times,
 * with the invariants checked after every frame: no ball inside a brick
 * or a wall, the brick count right, the score never going back.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../br_game.h"

static int checks, failed;

#define CHECK(c) do { checks++; if (!(c)) { failed++; \
	printf("%s:%d: FAILED: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t seed = 12345;
static uint32_t rnd(uint32_t n) {
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;
	return n ? seed % n : 0;
}
static uint32_t never(uint32_t n) { (void)n; return 1; }   /* no capsules */

static int count_ev(const br_game_t *g, int type) {
	int i, n = 0;
	for (i = 0; i < g->nev; i++) if (g->ev[i].type == type) n++;
	return n;
}

static int count_breakable(const br_game_t *g) {
	int r, c, n = 0;
	for (r = 0; r < BR_ROWS; r++)
		for (c = 0; c < BR_COLS; c++)
			if (g->brick[r][c] && g->brick[r][c] != BR_GOLD) n++;
	return n;
}

static bool ball_in_brick(const br_game_t *g, const br_ball_t *b) {
	int x = b->x >> 8, y = b->y >> 8, xx, yy, r, c;
	for (yy = y; yy < y + BR_BALL; yy++)
		for (xx = x; xx < x + BR_BALL; xx++)
			if (br_brick_at(g, xx, yy, &r, &c)) return true;
	return false;
}

/* A ball in flight at a pixel position. */
static void put_ball(br_game_t *g, int i, int x, int y, int ang, int sx, int sy) {
	br_ball_t *b = &g->ball[i];
	memset(b, 0, sizeof(*b));
	b->live = true;
	b->x = (int32_t)x << 8;
	b->y = (int32_t)y << 8;
	b->ang = (uint8_t)ang;
	b->sx = (int8_t)sx;
	b->sy = (int8_t)sy;
}

static const char *const empty_rows[] = { NULL };

/* Keep the paddle under ball 0, and send it back towards x: the
 * paddle's left half sends a ball left, the right half right. */
static void steer(br_game_t *g, int x) {
	int bc = (g->ball[0].x >> 8) + BR_BALL / 2;
	br_paddle_centre(g, bc > x ? bc + 1 : bc);
}

/* -- the particular cases -- */

static void test_layouts(void) {
	int l;
	br_game_t g;
	CHECK(br_levels() >= 10);
	for (l = 1; l <= br_levels(); l++) {
		br_new(&g, l, rnd);
		CHECK(g.bricks_left > 0);
		CHECK(g.bricks_left == count_breakable(&g));
		CHECK(br_level_name(l) != NULL);
		CHECK(br_balls(&g) == 1 && g.ball[0].stuck);
		CHECK(g.lives == BR_LIVES);
	}
	/* round again: the same layout, faster, tougher silver */
	br_new(&g, 1, rnd);
	{
		br_game_t g2;
		br_new(&g2, 1 + br_levels(), rnd);
		CHECK(g2.bricks_left == g.bricks_left);
		CHECK(g2.speed_base > g.speed_base);
		CHECK(g2.hp[2][0] > g.hp[2][0]);
		CHECK(strcmp(br_level_name(1), br_level_name(1 + br_levels())) == 0);
	}
}

static void test_bounce_and_break(void) {
	static const char *const rows[] = {
		"...................",
		"...................",
		"........1..........",
		NULL
	};
	br_game_t g;
	int f, broke = 0;
	br_new(&g, 1, never);
	br_test_layout(&g, rows);
	CHECK(g.bricks_left == 1);
	/* straight up into the brick at column 8 (x 136..151), row 2 (y 48..55) */
	memset(g.ball, 0, sizeof(g.ball));
	put_ball(&g, 0, 141, 120, 0, 1, -1);
	for (f = 0; f < 200 && !g.cleared; f++) {
		br_step(&g);
		broke += count_ev(&g, BR_EV_BREAK);
		br_clear_events(&g);
	}
	CHECK(broke == 1);
	CHECK(g.cleared);
	CHECK(g.bricks_left == 0);
	CHECK(g.score == br_points(BR_C1, 1));
	/* frozen once clear */
	{
		int32_t y = g.ball[0].y;
		br_step(&g);
		CHECK(g.ball[0].y == y);
	}
}

static void test_silver_and_gold(void) {
	static const char *const rows[] = {
		"...................",
		"...................",
		"........S.G........",
		"...................",
		"1..................",
		NULL
	};
	br_game_t g;
	int f, hits = 0, breaks = 0, golds = 0;
	br_new(&g, 1, never);
	br_test_layout(&g, rows);
	CHECK(g.bricks_left == 2);
	CHECK(g.hp[2][8] == 2);
	memset(g.ball, 0, sizeof(g.ball));
	put_ball(&g, 0, 141, 120, 0, 1, -1);
	/* the ball under the silver one, a wall of nothing: it comes back
	 * down past the paddle's place -- keep the paddle under it */
	for (f = 0; f < 600; f++) {
		steer(&g, 144);
		br_step(&g);
		hits += count_ev(&g, BR_EV_HIT);
		breaks += count_ev(&g, BR_EV_BREAK);
		golds += count_ev(&g, BR_EV_GOLD);
		br_clear_events(&g);
		if (breaks) break;
	}
	CHECK(hits == 1);
	CHECK(breaks == 1);
	CHECK(g.brick[2][8] == BR_EMPTY);
	CHECK(g.score == br_points(BR_SILVER, 1));

	/* gold: hit it a lot, it stays */
	memset(g.ball, 0, sizeof(g.ball));
	put_ball(&g, 0, 173, 120, 0, 1, -1);
	golds = 0;
	for (f = 0; f < 2000; f++) {
		steer(&g, 176);
		br_step(&g);
		golds += count_ev(&g, BR_EV_GOLD);
		br_clear_events(&g);
		if (g.cleared) break;
	}
	CHECK(golds >= 2);
	CHECK(g.brick[2][10] == BR_GOLD);
}

static void test_paddle_angles(void) {
	br_game_t g;
	int zone;
	br_new(&g, 1, never);
	br_test_layout(&g, empty_rows);
	g.bricks_left = 99;   /* nothing to clear */
	for (zone = 0; zone < 16; zone++) {
		int x = g.px + (zone * g.pw) / 16 + g.pw / 32;   /* middle of the zone */
		int f;
		memset(g.ball, 0, sizeof(g.ball));
		/* just above, nearly straight down: it lands where it is */
		put_ball(&g, 0, x - BR_BALL / 2, BR_PADDLE_Y - BR_BALL - 1, 0, 1, 1);
		for (f = 0; f < 30; f++) {
			br_step(&g);
			if (count_ev(&g, BR_EV_PADDLE)) break;
			br_clear_events(&g);
		}
		CHECK(count_ev(&g, BR_EV_PADDLE) == 1);
		br_clear_events(&g);
		CHECK(g.ball[0].sy == -1);
		CHECK(g.ball[0].sx == (zone < 8 ? -1 : 1));
		CHECK(g.ball[0].ang == (zone < 8 ? 7 - zone : zone - 8));
	}
	/* missing the paddle: the ball is lost, a life with it */
	memset(g.ball, 0, sizeof(g.ball));
	put_ball(&g, 0, g.px > 100 ? 20 : 280, 200, 0, 1, 1);
	{
		int f, lost = 0;
		for (f = 0; f < 100 && !lost; f++) {
			br_step(&g);
			lost = count_ev(&g, BR_EV_LOST);
			br_clear_events(&g);
		}
		CHECK(lost == 1);
		CHECK(g.lives == BR_LIVES - 1);
		CHECK(br_balls(&g) == 0);
	}
}

static void test_serve(void) {
	br_game_t g;
	int f, launched = -1;
	br_new(&g, 1, never);
	CHECK(g.ball[0].stuck);
	br_paddle_move(&g, -1000);
	CHECK(g.px == BR_FX0);
	CHECK((g.ball[0].x >> 8) >= g.px && (g.ball[0].x >> 8) + BR_BALL <= g.px + g.pw);
	br_paddle_move(&g, 1000);
	CHECK(g.px == BR_FX1 - g.pw);
	CHECK((g.ball[0].x >> 8) + BR_BALL <= g.px + g.pw);
	br_paddle_centre(&g, 160);
	br_fire(&g);
	CHECK(!g.ball[0].stuck);
	CHECK(count_ev(&g, BR_EV_LAUNCH) == 1);
	CHECK(g.ball[0].sy == -1);
	/* left alone, a served ball goes by itself */
	br_new(&g, 1, never);
	for (f = 0; f < 1000 && launched < 0; f++) {
		br_step(&g);
		if (count_ev(&g, BR_EV_LAUNCH)) launched = f;
		br_clear_events(&g);
	}
	CHECK(launched > 300 && launched < 400);
}

static void give(br_game_t *g, int type) {
	int f;
	g->cap[0].type = (uint8_t)type;
	g->cap[0].x = (int16_t)g->px;
	g->cap[0].y = (int32_t)(BR_PADDLE_Y - 12) << 8;
	for (f = 0; f < 10 && g->cap[0].type; f++) {
		/* keep the balls where they are */
		br_step(g);
	}
}

static void test_capsules(void) {
	br_game_t g;
	int centre, f, shots = 0;
	uint32_t s;

	/* wide, and back */
	br_new(&g, 1, never);
	centre = g.px + g.pw / 2;
	s = g.score;
	give(&g, BR_CAP_WIDE);
	CHECK(count_ev(&g, BR_EV_CAP_TAKE) == 1);
	CHECK(g.pw == BR_PADDLE_WIDE);
	CHECK(g.px + g.pw / 2 == centre);
	CHECK(g.score == s + 1000);
	br_clear_events(&g);
	give(&g, BR_CAP_LASER);
	CHECK(g.pw == BR_PADDLE_W);
	CHECK(g.power == BR_P_LASER);
	CHECK(g.px + g.pw / 2 == centre);

	/* the laser: the first press lets the ball go, the next fire */
	br_fire(&g);
	CHECK(!g.ball[0].stuck);
	br_clear_events(&g);
	br_fire(&g);
	CHECK(count_ev(&g, BR_EV_FIRE) == 1);
	br_fire(&g);     /* too soon */
	CHECK(count_ev(&g, BR_EV_FIRE) == 1);
	for (f = 0; f < BR_MAX_SHOTS; f++) if (g.shot[f].live) shots++;
	CHECK(shots == 2);
	br_clear_events(&g);
	{
		/* bolts break bricks: count breaks over the next frames */
		int breaks = 0, left = g.bricks_left;
		for (f = 0; f < 60; f++) {
			br_step(&g);
			breaks += count_ev(&g, BR_EV_BREAK) + count_ev(&g, BR_EV_HIT);
			br_clear_events(&g);
		}
		CHECK(breaks >= 2);
		CHECK(g.bricks_left <= left);
	}

	/* multi: three balls */
	br_new(&g, 1, never);
	br_fire(&g);
	br_step(&g);
	give(&g, BR_CAP_MULTI);
	CHECK(br_balls(&g) == 3);
	{
		int a = 0, i;
		for (i = 0; i < BR_MAX_BALLS; i++) if (g.ball[i].live && !g.ball[i].stuck) a++;
		CHECK(a == 3);
	}

	/* slow */
	g.speed = 900;
	give(&g, BR_CAP_SLOW);
	CHECK(g.speed < 500);

	/* catch: a ball coming down is held, then lets go by itself */
	br_new(&g, 1, never);
	give(&g, BR_CAP_CATCH);
	CHECK(g.power == BR_P_CATCH);
	br_fire(&g);
	br_clear_events(&g);
	memset(g.ball, 0, sizeof(g.ball));
	put_ball(&g, 0, g.px + 10, BR_PADDLE_Y - 30, 2, 1, 1);
	{
		int caught = -1, gone = -1;
		for (f = 0; f < 400 && gone < 0; f++) {
			br_step(&g);
			if (count_ev(&g, BR_EV_CATCH)) caught = f;
			if (caught >= 0 && count_ev(&g, BR_EV_LAUNCH)) gone = f;
			br_clear_events(&g);
			if (caught >= 0 && gone < 0) {
				CHECK(g.ball[0].stuck);
				br_paddle_move(&g, (f & 16) ? 3 : -3);
				CHECK((g.ball[0].x >> 8) >= g.px &&
				      (g.ball[0].x >> 8) + BR_BALL <= g.px + g.pw);
			}
		}
		CHECK(caught >= 0);
		CHECK(gone - caught > 100);
	}

	/* a life */
	br_new(&g, 1, never);
	give(&g, BR_CAP_LIFE);
	CHECK(g.lives == BR_LIVES + 1);
	CHECK(count_ev(&g, BR_EV_LIFE) == 1);

	/* a capsule missed */
	br_new(&g, 1, never);
	br_clear_events(&g);
	g.cap[1].type = BR_CAP_WIDE;
	g.cap[1].x = (int16_t)(g.px > 150 ? 20 : 280);
	g.cap[1].y = (int32_t)200 << 8;
	for (f = 0; f < 60 && g.cap[1].type; f++) br_step(&g);
	CHECK(!g.cap[1].type);
	CHECK(count_ev(&g, BR_EV_CAP_MISS) == 1);
	CHECK(g.pw == BR_PADDLE_W);
}

static void test_lives_and_over(void) {
	br_game_t g;
	int k, f;
	br_new(&g, 1, never);
	g.score = 19990;
	{
		/* points over 20000: an extra paddle */
		static const char *const rows[] = { "...................", "...................",
			"........1.........1", NULL };
		br_test_layout(&g, rows);
		memset(g.ball, 0, sizeof(g.ball));
		put_ball(&g, 0, 141, 120, 0, 1, -1);
		for (f = 0; f < 200 && g.bricks_left == 2; f++) { br_step(&g); }
		CHECK(g.lives == BR_LIVES + 1);
		CHECK(count_ev(&g, BR_EV_LIFE) == 1);
		CHECK(g.next_life == 70000);
		br_clear_events(&g);
	}
	for (k = 0; k < BR_LIVES + 1; k++) {
		CHECK(!g.over);
		br_serve(&g);
		br_paddle_centre(&g, 40);
		br_fire(&g);
		g.ball[0].x = 250 << 8;
		g.ball[0].y = 200 << 8;
		g.ball[0].sy = 1;
		for (f = 0; f < 200 && !count_ev(&g, BR_EV_LOST); f++) {
			br_clear_events(&g);
			br_step(&g);
		}
		CHECK(count_ev(&g, BR_EV_LOST) == 1);
		br_clear_events(&g);
	}
	CHECK(g.over);
	CHECK(g.lives == 0);
}

/* -- every level, the autopilot playing, with the invariants -- */

static void soak(int level, uint32_t s, int frames, int *clears, int *losses,
	int skill) {
	br_game_t g;
	int f, i;
	uint32_t last_score = 0;
	int failed0 = failed;
	seed = s;
	br_new_skill(&g, level, skill, rnd);
	for (f = 0; f < frames; f++) {
		bool fire;
		int t = br_ai_target(&g, &fire), c = g.px + g.pw / 2, d = t - c;
		/* a keyboard's speed, not a mouse's */
		if (d > 5) d = 5;
		if (d < -5) d = -5;
		br_paddle_move(&g, d);
		if (fire) br_fire(&g);
		br_step(&g);

		for (i = 0; i < BR_MAX_BALLS; i++) {
			const br_ball_t *b = &g.ball[i];
			int x, y;
			if (!b->live) continue;
			x = b->x >> 8;
			y = b->y >> 8;
			CHECK(x >= BR_FX0 && x + BR_BALL <= BR_FX1);
			CHECK(y >= BR_FY0);
			CHECK(b->ang < 8);
			CHECK(!ball_in_brick(&g, b));
			if (b->stuck) CHECK(x >= g.px && x + BR_BALL <= g.px + g.pw);
			if (failed > failed0 + 20) { printf("level %d frame %d\n", level, f); exit(1); }
		}
		CHECK(g.speed >= (skill == BR_SKILL_EASY ? 240 : 400) && g.speed <= br_speed_max(&g));
		CHECK(g.px >= BR_FX0 && g.px + g.pw <= BR_FX1);
		CHECK(g.score >= last_score);
		last_score = g.score;
		if ((f & 63) == 0) CHECK(g.bricks_left == count_breakable(&g));
		if (count_ev(&g, BR_EV_LOST)) (*losses)++;
		br_clear_events(&g);

		if (g.cleared) {
			(*clears)++;
			br_next_level(&g);
			CHECK(g.bricks_left == count_breakable(&g));
		} else if (g.over) {
			br_new_skill(&g, level, skill, rnd);
			last_score = 0;
		} else if (br_balls(&g) == 0) {
			br_serve(&g);
		}
	}
}

/* Easy: a slow ball, a long paddle, five of them, and a floor. */
static void test_easy(void) {
	br_game_t g, n;
	int f, bounced = 0;
	br_new_skill(&g, 1, BR_SKILL_EASY, never);
	br_new(&n, 1, never);
	CHECK(n.skill == BR_SKILL_NORMAL && n.lives == BR_LIVES && n.pw == BR_PADDLE_W && !n.floor_up);
	CHECK(n.speed_base == 480);
	CHECK(g.lives == BR_LIVES_EASY);
	CHECK(g.pw == BR_PADDLE_WIDE && g.pw_wide == BR_PADDLE_EASY_WIDE);
	CHECK(g.speed_base == 288);
	CHECK(g.speed_base * 10 <= n.speed_base * 6);
	CHECK(br_speed_max(&g) == 512);
	CHECK(g.floor_up);
	/* rounds 1-3: every other row */
	CHECK(count_breakable(&g) * 2 <= count_breakable(&n) + 19);
	CHECK(count_breakable(&g) == g.bricks_left);
	{
		br_game_t g4;
		br_new_skill(&g4, 4, BR_SKILL_EASY, never);
		br_new(&n, 4, never);
		CHECK(count_breakable(&g4) == count_breakable(&n));
	}

	/* the floor: a ball missed by the paddle comes back once */
	br_fire(&g);
	br_paddle_centre(&g, 40);
	g.ball[0].x = 250 << 8;
	g.ball[0].y = 200 << 8;
	g.ball[0].sy = 1;
	for (f = 0; f < 600 && !count_ev(&g, BR_EV_LOST); f++) {
		br_clear_events(&g);
		br_step(&g);
		bounced += count_ev(&g, BR_EV_FLOOR);
		if (bounced && g.ball[0].sy < 0 && (g.ball[0].y >> 8) < 120) {
			/* on its way up again: send it straight back down,
			 * away from the paddle */
			g.ball[0].sy = 1;
			g.ball[0].x = 250 << 8;
		}
	}
	CHECK(bounced == 1);
	CHECK(!g.floor_up);
	CHECK(count_ev(&g, BR_EV_LOST) == 1);
	CHECK(g.lives == BR_LIVES_EASY - 1);
	br_clear_events(&g);
	br_serve(&g);
	CHECK(g.floor_up);                 /* back with the next ball */
	CHECK(g.pw == BR_PADDLE_WIDE);

	/* E on easy: 64 */
	give(&g, BR_CAP_WIDE);
	CHECK(g.pw == BR_PADDLE_EASY_WIDE);

	/* hard: faster than normal */
	br_new_skill(&g, 1, BR_SKILL_HARD, never);
	CHECK(g.speed_base > 480 && g.lives == BR_LIVES && !g.floor_up);
}

int main(void) {
	int l, k, clears = 0, losses = 0;

	test_layouts();
	test_bounce_and_break();
	test_silver_and_gold();
	test_paddle_angles();
	test_serve();
	test_capsules();
	test_lives_and_over();
	test_easy();

	for (l = 1; l <= br_levels(); l++)
		for (k = 0; k < 4; k++)
			soak(l, 1000u + (uint32_t)(l * 77 + k * 1013), 30000, &clears, &losses,
				BR_SKILL_NORMAL);
	printf("soak: %d levels cleared, %d paddles lost\n", clears, losses);
	CHECK(clears > br_levels());

	{
		int ec = 0, el = 0, hc = 0, hl = 0;
		for (l = 1; l <= br_levels(); l++) {
			soak(l, 5000u + (uint32_t)(l * 31), 30000, &ec, &el, BR_SKILL_EASY);
			soak(l, 7000u + (uint32_t)(l * 31), 30000, &hc, &hl, BR_SKILL_HARD);
		}
		printf("soak, easy: %d levels cleared, %d paddles lost; hard: %d, %d\n",
			ec, el, hc, hl);
		CHECK(el <= hl);
	}

	printf("bricks rules: %d checks, %d failed\n", checks, failed);
	return failed != 0;
}
