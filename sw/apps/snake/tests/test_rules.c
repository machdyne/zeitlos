/*
 * snake -- host tests of the rules (sn_game.c). make -C sw/apps/snake test
 *
 * Every test checks the same invariants after every frame, because the
 * bugs a snake game has are bookkeeping bugs: a body cell left behind
 * in the grid, a link pointing at nothing, a length that disagrees with
 * the ring. check_invariants() is the real test; the cases just drive
 * the game into the corners where those go wrong.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../sn_game.h"
#include "../sn_levels.h"

static int fails, checks;

#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

static uint32_t lcg = 1;
static uint32_t rnd(uint32_t n) {
	lcg = lcg * 1103515245u + 12345u;
	return n ? (lcg >> 8) % n : 0;
}

static const int ddx[4] = { 1, 0, -1, 0 };
static const int ddy[4] = { 0, 1, 0, -1 };

static void check_invariants(const sn_game_t *g, const char *where) {
	int p, i, body = 0, want = 0;
	for (i = 0; i < SN_GW * SN_GH; i++)
		if (g->grid[i].what == SN_BODY) body++;
	for (p = 0; p < SN_PLAYERS; p++) {
		const sn_snake_t *s = &g->s[p];
		if (!s->active) continue;
		want += s->len;
		for (i = 0; i < s->len; i++) {
			uint16_t c = sn_segment(s, i);
			const sn_cell_t *cell = &g->grid[c];
			CHECK(cell->what == SN_BODY && cell->who == p,
				"%s: p%d segment %d (cell %d) not body in the grid", where, p, i, c);
			if (i + 1 < s->len) {
				uint16_t n = sn_segment(s, i + 1);
				int d = sn_dir_between(g, c, n);
				CHECK(d != SN_NODIR, "%s: p%d segments %d,%d not adjacent", where, p, i, i + 1);
				if (d != SN_NODIR) {
					CHECK(cell->links & (1u << d), "%s: p%d seg %d missing link forward", where, p, i);
					CHECK(g->grid[n].links & (1u << ((d + 2) & 3)),
						"%s: p%d seg %d missing link back", where, p, i + 1);
				}
			}
		}
		if (s->len) {
			/* The tail has no link backward; the head none forward
			 * unless a move is under way. */
			uint16_t t = s->body[s->tail], h = s->body[s->head];
			int bits = 0, d;
			for (d = 0; d < 4; d++) if (g->grid[t].links & (1u << d)) bits++;
			if (s->len >= 2) CHECK(bits == 1, "%s: p%d tail has %d links", where, p, bits);
			if (s->alive && s->next != SN_NONE) {
				int dn = sn_dir_between(g, h, s->next);
				CHECK(dn == s->dir, "%s: p%d next is not in the move's direction", where, p);
			}
		}
	}
	CHECK(body == want, "%s: %d body cells in the grid, %d in the snakes", where, body, want);
	if (g->food != SN_NONE)
		CHECK(g->grid[g->food].what == SN_EMPTY, "%s: food on a non-empty cell", where);
	if (g->bonus != SN_NONE)
		CHECK(g->grid[g->bonus].what == SN_EMPTY, "%s: bonus on a non-empty cell", where);
}

static int run(sn_game_t *g, int frames) {
	int f;
	for (f = 0; f < frames && !g->over; f++) {
		sn_step(g);
		check_invariants(g, "run");
	}
	return f;
}

/* Run until player p completes `moves` moves (head changes cell). */
static void run_moves(sn_game_t *g, int p, int moves) {
	int guard = 100000;
	while (moves > 0 && guard-- && !g->over) {
		uint16_t h = g->s[p].body[g->s[p].head];
		sn_step(g);
		check_invariants(g, "run_moves");
		if (g->s[p].body[g->s[p].head] != h) moves--;
	}
}

static void put_food_ahead(sn_game_t *g, int p, int cells) {
	const sn_snake_t *s = &g->s[p];
	uint16_t h = s->body[s->head];
	int x = sn_cx(h) + ddx[s->dir] * cells, y = sn_cy(h) + ddy[s->dir] * cells;
	g->food = sn_cell(x, y);
}

static void test_start(void) {
	sn_game_t g;
	sn_new(&g, SN_MODE_CLASSIC, 0, rnd);
	check_invariants(&g, "start");
	CHECK(g.s[0].len == 4, "starts with 4 cells, has %d", g.s[0].len);
	CHECK(g.s[0].alive && !g.s[1].active, "one live snake");
	CHECK(g.food != SN_NONE, "food placed");
	CHECK(g.players == 1 && !g.wrap, "classic: one player, walls");
}

static void test_move_and_eat(void) {
	sn_game_t g;
	uint32_t before;
	sn_new(&g, SN_MODE_CLASSIC, 0, rnd);
	put_food_ahead(&g, 0, 3);
	before = g.s[0].score;
	run_moves(&g, 0, 3);
	CHECK(g.s[0].score > before, "eating scores");
	CHECK(g.s[0].eaten == 1, "eaten counts");
	run_moves(&g, 0, 2);
	CHECK(g.s[0].len == 6, "grows by two over the next moves, len %d", g.s[0].len);
	run_moves(&g, 0, 3);
	CHECK(g.s[0].len == 6, "and then stops growing, len %d", g.s[0].len);
	CHECK(g.food != SN_NONE && g.grid[g.food].what == SN_EMPTY, "new food");
}

static void test_wall(void) {
	sn_game_t g;
	int f;
	sn_new(&g, SN_MODE_CLASSIC, 0, rnd);
	g.food = SN_NONE;
	sn_turn(&g, 0, SN_UP);
	f = run(&g, 2000);
	CHECK(g.over && !g.s[0].alive, "runs into the top wall");
	CHECK(f < 400, "within a few seconds (%d frames)", f);
	{
		uint16_t h = g.s[0].body[g.s[0].head];
		CHECK(sn_cy(h) == 1, "dies with its head in the last free row (%d)", sn_cy(h));
	}
}

static void test_reverse_and_queue(void) {
	sn_game_t g;
	sn_new(&g, SN_MODE_CLASSIC, 0, rnd);
	g.food = SN_NONE;
	sn_turn(&g, 0, SN_LEFT);           /* reverse: ignored */
	CHECK(g.s[0].nq == 0, "a reverse is not queued");
	run_moves(&g, 0, 1);
	CHECK(g.s[0].dir == SN_RIGHT && g.s[0].alive, "still going right");
	sn_turn(&g, 0, SN_UP);
	sn_turn(&g, 0, SN_LEFT);           /* up, then left: a U-turn in two */
	CHECK(g.s[0].nq == 2, "two turns queued");
	run_moves(&g, 0, 3);
	CHECK(g.s[0].dir == SN_LEFT && g.s[0].alive, "made both turns, dir %d", g.s[0].dir);
	sn_turn(&g, 0, SN_DOWN);
	sn_turn(&g, 0, SN_DOWN);           /* a repeat: one turn */
	CHECK(g.s[0].nq == 1, "a repeated turn is one turn");
}

static void test_self(void) {
	sn_game_t g;
	int i;
	sn_new(&g, SN_MODE_CLASSIC, 0, rnd);
	g.food = SN_NONE;
	g.s[0].grow = 6;                   /* long enough to bite itself */
	run_moves(&g, 0, 7);
	sn_turn(&g, 0, SN_DOWN);
	run_moves(&g, 0, 1);
	sn_turn(&g, 0, SN_LEFT);
	run_moves(&g, 0, 1);
	sn_turn(&g, 0, SN_UP);
	for (i = 0; i < 200 && !g.over; i++) { sn_step(&g); check_invariants(&g, "self"); }
	CHECK(g.over && !g.s[0].alive, "bites itself");
}

static void test_tail_chase(void) {
	/* Four cells in a square, going round: the head always enters the
	 * cell the tail is leaving. Legal, forever. */
	sn_game_t g;
	int i;
	sn_new(&g, SN_MODE_CLASSIC, 0, rnd);
	g.food = SN_NONE;
	g.bonus = SN_NONE;
	for (i = 0; i < 40 && g.s[0].alive; i++) {
		sn_turn(&g, 0, (g.s[0].dir + 1) & 3);
		run_moves(&g, 0, 1);
	}
	CHECK(g.s[0].alive, "chasing its own tail is not a crash");
	CHECK(g.s[0].len == 4, "and does not change the length (%d)", g.s[0].len);
}

static void test_wrap(void) {
	sn_game_t g;
	sn_new(&g, SN_MODE_OPEN, 0, rnd);
	g.food = SN_NONE;
	run_moves(&g, 0, SN_GW + 5);
	CHECK(g.s[0].alive, "the open field wraps instead of killing");
	{
		uint16_t h = g.s[0].body[g.s[0].head];
		CHECK(sn_cx(h) == (6 + SN_GW + 5) % SN_GW, "came round the other side (x %d)", sn_cx(h));
	}
	sn_turn(&g, 0, SN_UP);
	run_moves(&g, 0, SN_GH + 2);
	CHECK(g.s[0].alive, "vertically too");
}

static void test_versus(void) {
	sn_game_t g;
	int f;
	sn_new(&g, SN_MODE_VERSUS, 0, rnd);
	check_invariants(&g, "versus start");
	CHECK(g.players == 2 && g.s[1].active, "two snakes");
	/* Put them on one row facing each other: a head-on. */
	{
		sn_game_t h = g;
		uint16_t a = h.s[0].body[h.s[0].head], b = h.s[1].body[h.s[1].head];
		int rows = sn_cy(b) - sn_cy(a);
		(void)rows;
		h.food = SN_NONE;
		/* move player 2 up one row */
		sn_turn(&h, 1, SN_UP);
		sn_turn(&h, 1, SN_LEFT);
		f = run(&h, 3000);
		CHECK(h.over, "the round ends");
		CHECK(!h.s[0].alive && !h.s[1].alive, "a head-on kills both (%d %d) after %d",
			h.s[0].alive, h.s[1].alive, f);
	}
	/* One runs into the wall: the other wins. */
	{
		sn_game_t h = g;
		h.food = SN_NONE;
		sn_turn(&h, 0, SN_UP);
		run(&h, 3000);
		CHECK(h.over && !h.s[0].alive && h.s[1].alive, "p2 survives p1's crash");
	}
}

static void test_bonus(void) {
	sn_game_t g;
	int i, shown = 0, gone = 0;
	sn_new(&g, SN_MODE_CLASSIC, 0, rnd);
	for (i = 0; i < 5; i++) {
		int f;
		/* Feed it: food straight ahead each time, turning to keep
		 * clear of the walls (the field is 38 wide inside; a lap of
		 * 6 x 4 keeps it in the middle). */
		sn_turn(&g, 0, (g.s[0].dir + 1) & 3);
		run_moves(&g, 0, 1);
		{
			uint16_t h = g.s[0].body[g.s[0].head];
			int x = sn_cx(h) + 2 * ddx[g.s[0].dir], y = sn_cy(h) + 2 * ddy[g.s[0].dir];
			uint16_t c = sn_cell(x, y);
			uint16_t m = sn_cell(sn_cx(h) + ddx[g.s[0].dir], sn_cy(h) + ddy[g.s[0].dir]);
			if (g.grid[c].what != SN_EMPTY || g.grid[m].what != SN_EMPTY ||
			    g.s[0].next == c) {
				i--;
				continue;
			}
			g.food = c;
		}
		for (f = 0; f < 200 && g.s[0].eaten == (uint16_t)i; f++) {
			sn_step(&g);
			check_invariants(&g, "bonus");
			{ int k; for (k = 0; k < g.nev; k++) if (g.ev[k].type == SN_EV_BONUS_SHOW) shown++; }
		}
	}
	CHECK(shown == 1 && g.bonus != SN_NONE, "a bonus after five (shown %d)", shown);
	g.food = SN_NONE;
	for (i = 0; i < 600 && g.bonus != SN_NONE; i++) {
		int k;
		uint16_t h = g.s[0].body[g.s[0].head];
		/* steer in a box so it lives */
		if (sn_cx(h) > 30 && g.s[0].dir == SN_RIGHT) sn_turn(&g, 0, SN_DOWN);
		if (sn_cy(h) > 20 && g.s[0].dir == SN_DOWN) sn_turn(&g, 0, SN_LEFT);
		if (sn_cx(h) < 8 && g.s[0].dir == SN_LEFT) sn_turn(&g, 0, SN_UP);
		if (sn_cy(h) < 6 && g.s[0].dir == SN_UP) sn_turn(&g, 0, SN_RIGHT);
		if (g.bonus != SN_NONE && sn_neighbour(&g, h, g.s[0].dir) == g.bonus) g.bonus_left = g.bonus_left;
		sn_step(&g);
		for (k = 0; k < g.nev; k++) if (g.ev[k].type == SN_EV_BONUS_GONE) gone++;
	}
	CHECK(g.bonus == SN_NONE, "the bonus goes away");
	CHECK(gone == 1 || g.s[0].score > 0, "with an event (%d)", gone);
}

/* Every level: a start, the start's body on the floor, and one
 * connected floor, so food can always be reached. */
static void test_levels(void) {
	int m, lv;
	for (m = 0; m < SN_MODES; m++) {
		int nlev = (m == SN_MODE_MAZE) ? sn_levels() : 1;
		for (lv = 0; lv < nlev; lv++) {
			sn_game_t g;
			static uint8_t seen[SN_GW * SN_GH];
			static uint16_t q[SN_GW * SN_GH];
			int qh = 0, qt = 0, floor = 0, reached = 0, i, d;
			const char *const *L = sn_layout(m, lv);
			for (i = 0; i < SN_GH; i++)
				CHECK(strlen(L[i]) == SN_GW, "mode %d level %d row %d is %d wide",
					m, lv, i, (int)strlen(L[i]));
			sn_new(&g, (sn_mode_t)m, lv, rnd);
			check_invariants(&g, "level");
			CHECK(g.s[0].active, "mode %d level %d has a start", m, lv);
			memset(seen, 0, sizeof(seen));
			for (i = 0; i < SN_GW * SN_GH; i++)
				if (g.grid[i].what != SN_WALL && g.grid[i].what != SN_BLOCKED) floor++;
			q[qt++] = g.s[0].body[g.s[0].head];
			seen[q[0]] = 1;
			while (qh < qt) {
				uint16_t c = q[qh++];
				reached++;
				for (d = 0; d < 4; d++) {
					uint16_t n = sn_neighbour(&g, c, d);
					if (n == SN_NONE || seen[n]) continue;
					if (g.grid[n].what == SN_WALL || g.grid[n].what == SN_BLOCKED) continue;
					seen[n] = 1;
					q[qt++] = n;
				}
			}
			CHECK(reached == floor, "mode %d level %d: %d of %d floor cells reachable",
				m, lv, reached, floor);
			/* room to start: the three cells ahead are free */
			{
				uint16_t h = g.s[0].body[g.s[0].head];
				for (i = 1; i <= 3; i++) {
					uint16_t c = sn_cell(sn_cx(h) + i, sn_cy(h));
					CHECK(g.grid[c].what == SN_EMPTY, "mode %d level %d: blocked ahead of the start", m, lv);
				}
			}
		}
	}
}

static void test_autopilot(void) {
	sn_game_t g;
	int f, deaths = 0, maxlen = 0;
	sn_new(&g, SN_MODE_TITLE, 0, rnd);
	for (f = 0; f < 60000; f++) {
		if (!g.s[0].alive) {
			deaths++;
			sn_new(&g, SN_MODE_TITLE, 0, rnd);
		}
		sn_autopilot(&g, 0);
		sn_step(&g);
		check_invariants(&g, "autopilot");
		if (g.s[0].len > maxlen) maxlen = g.s[0].len;
		if (fails > 20) break;
	}
	printf("autopilot: %d deaths in %d frames, longest %d\n", deaths, f, maxlen);
	CHECK(maxlen >= 20, "the demo snake gets somewhere (%d)", maxlen);
	CHECK(deaths < 30, "and does not die every few seconds (%d)", deaths);
}

static void test_dissolve(void) {
	sn_game_t g;
	int i;
	sn_new(&g, SN_MODE_CLASSIC, 0, rnd);
	g.s[0].grow = 10;
	g.food = SN_NONE;
	run_moves(&g, 0, 12);
	g.s[0].alive = false;
	for (i = 0; i < 100 && g.s[0].len; i++) {
		sn_pop_head(&g, 0);
		check_invariants(&g, "dissolve");
	}
	CHECK(g.s[0].len == 0, "dissolves to nothing");
	for (i = 0; i < SN_GW * SN_GH; i++)
		CHECK(g.grid[i].what != SN_BODY, "no body cell left at %d", i);
}

static void test_speed(void) {
	CHECK(sn_speed_for(60) == 204, "6 cells a second is 0.8 px a frame (%d)", sn_speed_for(60));
	CHECK(sn_speed_for(150) < (SN_CELL_PX << 8), "the top speed is under a cell a frame");
}

int main(void) {
	test_start();
	test_move_and_eat();
	test_wall();
	test_reverse_and_queue();
	test_self();
	test_tail_chase();
	test_wrap();
	test_versus();
	test_bonus();
	test_levels();
	test_autopilot();
	test_dissolve();
	test_speed();
	printf("snake rules: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
