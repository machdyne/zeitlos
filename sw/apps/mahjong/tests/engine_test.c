/*
 * Zeitlos mahjong -- host tests for the layouts and the rules.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * What cannot be seen by playing:
 *
 *   - a neighbour list one tile short still plays; a tile that should
 *     be pinned is simply free, and nobody counts. So the free rule is
 *     checked against a SECOND implementation that works from the
 *     geometry directly, on every built-in layout, at every step of
 *     thousands of games.
 *   - a deal that cannot be won looks like any other until the end.
 *     So every deal's own solution is replayed to an empty table, over
 *     thousands of numbered deals per layout, and every shuffle's too.
 *   - a set one tile wrong is invisible among 144.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../mj_layout.h"
#include "../mj_game.h"

static int checks = 0;
static int failures = 0;

static void check(bool ok, const char *what)
{
	checks++;
	if (ok) return;
	failures++;
	printf("FAIL: %s\n", what);
}

static void check_eq(long got, long want, const char *what)
{
	checks++;
	if (got == want) return;
	failures++;
	printf("FAIL: %s -- got %ld, want %ld\n", what, got, want);
}

static mj_layout_t L;
static mj_game_t   G;

static void load(int i)
{
	char err[64];
	if (!mj_layout_parse(mj_layout_builtin_text(i), &L, err, sizeof(err))) {
		printf("built-in %d does not parse: %s\n", i, err);
		exit(1);
	}
}

/* -- the free rule, again, from scratch ---------------------------------
 *
 * Written the long way round on purpose: no neighbour lists, every
 * other tile examined every time. */
static bool free_slow(const mj_game_t *g, int t)
{
	const mj_layout_t *l = g->L;
	int u;
	bool lblock = false, rblock = false;

	if (g->face[t] == MJ_GONE) return false;
	for (u = 0; u < l->n; u++) {
		int dx, dy;
		if (u == t || g->face[u] == MJ_GONE) continue;
		dx = l->pos[u].x - l->pos[t].x;
		dy = l->pos[u].y - l->pos[t].y;
		if (dy <= -2 || dy >= 2) continue;
		if (l->pos[u].z > l->pos[t].z && dx > -2 && dx < 2) return false;
		if (l->pos[u].z == l->pos[t].z) {
			if (dx == -2) lblock = true;
			if (dx == 2) rblock = true;
		}
	}
	return !lblock || !rblock;
}

static int free_disagreements(const mj_game_t *g)
{
	int t, bad = 0;
	for (t = 0; t < g->L->n; t++)
		if (mj_free(g, t) != free_slow(g, t)) bad++;
	return bad;
}

/* -- layouts ------------------------------------------------------------- */

static void test_builtins(void)
{
	static const struct { const char *name; int n, w, h, d; } want[] = {
		{ "Turtle",   144, 30, 16, 5 },
		{ "Pyramid",  120, 16, 16, 4 },
		{ "Fortress", 144, 24, 16, 4 },
		{ "Waves",    126, 20, 16, 4 },
	};
	int i;

	check_eq(mj_layout_builtin_count(), 4, "four built-in layouts");
	check(mj_layout_builtin_text(-1) == 0 && mj_layout_builtin_text(4) == 0,
	      "built-in index out of range gives nothing");
	for (i = 0; i < 4; i++) {
		char what[80];
		load(i);
		snprintf(what, sizeof(what), "%s: name", want[i].name);
		check(strcmp(L.name, want[i].name) == 0, what);
		snprintf(what, sizeof(what), "%s: tiles", want[i].name);
		check_eq(L.n, want[i].n, what);
		snprintf(what, sizeof(what), "%s: width", want[i].name);
		check_eq(L.w, want[i].w, what);
		snprintf(what, sizeof(what), "%s: height", want[i].name);
		check_eq(L.h, want[i].h, what);
		snprintf(what, sizeof(what), "%s: layers", want[i].name);
		check_eq(L.d, want[i].d, what);
	}
}

static int find(int x, int y, int z)
{
	int t;
	for (t = 0; t < L.n; t++)
		if (L.pos[t].x == x && L.pos[t].y == y && L.pos[t].z == z) return t;
	return -1;
}

static void test_turtle_geometry(void)
{
	int t, nfree = 0, top, lone;

	load(0);
	/* Canonical order: layer, row, column. */
	for (t = 1; t < L.n; t++) {
		const mj_pos_t *a = &L.pos[t - 1], *b = &L.pos[t];
		int ka = (a->z << 16) | (a->y << 8) | a->x;
		int kb = (b->z << 16) | (b->y << 8) | b->x;
		if (ka >= kb) { check(false, "positions in canonical order"); break; }
	}

	top = find(13, 7, 4);
	check(top >= 0, "the Turtle's top tile sits on a half step");
	check_eq(L.nabove[top], 0, "nothing on the top tile");
	for (t = 0; t < L.n; t++)
		if (L.pos[t].z == 3) {
			int k, on = 0;
			for (k = 0; k < L.nabove[t]; k++) if (L.above[t][k] == top) on = 1;
			check(on, "the top tile pins all four under it");
		}

	lone = find(0, 7, 0);
	check(lone >= 0, "the left side tile is between rows");
	check_eq(L.nright[lone], 2, "the left side tile touches both rows beside it");
	check_eq(L.nleft[find(2, 6, 0)], 1, "row 3's first tile is held by the side tile");
	check_eq(L.nleft[find(26, 7, 0)], 2, "the right side tile touches both rows");
	check_eq(L.nright[find(26, 7, 0)], 1, "and the outermost tile");

	mj_new(&G, &L, 1, MJ_DEAL_SOLVABLE);
	for (t = 0; t < L.n; t++) if (mj_free(&G, t)) nfree++;
	/* top 1, the two outer side tiles, the ends of six rows of layer 0,
	 * the ends of six rows each on layers 1 and 2 (four rows there). */
	check_eq(nfree, 1 + 2 + 12 + 12 + 8, "free tiles on a full Turtle");
	check_eq(free_disagreements(&G), 0, "free rule agrees with geometry on a full Turtle");
}

static void parse_fails(const char *text, const char *want_err, const char *what)
{
	char err[64];
	bool ok = mj_layout_parse(text, &L, err, sizeof(err));
	check(!ok, what);
	if (!ok && want_err && !strstr(err, want_err)) {
		char m[160];
		snprintf(m, sizeof(m), "%s: error '%s' should mention '%s'", what, err, want_err);
		check(false, m);
	}
}

static void test_parser(void)
{
	char err[64];

	parse_fails("", "no 'layer'", "empty text");
	parse_fails("name X\n[]\n[]\n", "before the first", "tiles before a layer");
	parse_fails("layer\n[][][]\n[][][]\n", "odd", "odd count");
	parse_fails("layer\n[]\n", "bottom half", "half a tile");
	parse_fails("layer\n[]\n[.\n", "bottom half", "a broken bottom");
	parse_fails("layer\n[\n[\n", "without its ']'", "a lone '['");
	parse_fails("layer\n.]\n", "without its '['", "a lone ']'");
	parse_fails("layer\n[]x\n[]x\n", "not '['", "a stray letter");
	parse_fails("layer\n.\n.\n.\n[][]\n[]\n", "(line 5)", "the line number is the file's");
	parse_fails("layer\n\n", "fewer than two", "no tiles");

	check(mj_layout_parse("# two side by side\r\nname  Pair  \r\nlayer\r\n"
			      "....[][]\r\n....[][]\r\n", &L, err, sizeof(err)),
	      "CRLF, comments, padded name");
	check(strcmp(L.name, "Pair") == 0, "name trimmed");
	check_eq(L.pos[0].x, 0, "moved to column 0");
	check_eq(L.w, 4, "width of two tiles");

	check(mj_layout_parse("layer\n[][][]\n[][][]\nlayer\n.[]\n.[]\n", &L, err, sizeof(err)),
	      "a tile on a half step across two");
	check_eq(L.nabove[0], 1, "the left one is pinned");
	check_eq(L.nabove[1], 1, "the middle one is pinned");
	check_eq(L.nabove[2], 0, "the right one is not");
	check(mj_layout_parse("layer\n[][]\n[][]\n", &L, err, sizeof(err)) &&
	      strcmp(L.name, "Untitled") == 0, "a layout with no name");
}

/* -- the set ------------------------------------------------------------- */

static void count_faces(const mj_game_t *g, int *per_face)
{
	int t;
	memset(per_face, 0, sizeof(int) * MJ_FACES);
	for (t = 0; t < g->L->n; t++)
		if (g->face[t] != MJ_GONE) per_face[g->face[t]]++;
}

static void test_set(void)
{
	int f[MJ_FACES], i, ok = 1;

	load(0);
	mj_new(&G, &L, 4711, MJ_DEAL_SOLVABLE);
	count_faces(&G, f);
	for (i = 0; i < MJ_F_FLOWER; i++) if (f[i] != 4) ok = 0;
	for (i = MJ_F_FLOWER; i < MJ_FACES; i++) if (f[i] != 1) ok = 0;
	check(ok, "a 144-tile deal is exactly one set");

	mj_new(&G, &L, 4711, MJ_DEAL_RANDOM);
	count_faces(&G, f);
	ok = 1;
	for (i = 0; i < MJ_F_FLOWER; i++) if (f[i] != 4) ok = 0;
	for (i = MJ_F_FLOWER; i < MJ_FACES; i++) if (f[i] != 1) ok = 0;
	check(ok, "a random deal is the same set");
	check_eq(G.nsol, 0, "a random deal claims no solution");

	check(mj_match(MJ_F_FLOWER, MJ_F_FLOWER + 3), "plum matches bamboo flower");
	check(mj_match(MJ_F_SEASON + 1, MJ_F_SEASON + 2), "summer matches autumn");
	check(!mj_match(MJ_F_FLOWER, MJ_F_SEASON), "a flower is not a season");
	check(!mj_match(MJ_F_DOTS + 2, MJ_F_BAMBOO + 2), "three dots is not three bamboo");
	check(mj_match(MJ_F_DRAGON, MJ_F_DRAGON), "red dragon matches red dragon");
	check(!mj_match(0, MJ_GONE), "nothing matches a gone tile");
	{
		int seen[MJ_CLASSES], c;
		memset(seen, 0, sizeof(seen));
		for (i = 0; i < MJ_FACES; i++) seen[mj_class(i)]++;
		ok = 1;
		for (c = 0; c < 34; c++) if (seen[c] != 1) ok = 0;
		if (seen[34] != 4 || seen[35] != 4) ok = 0;
		check(ok, "36 classes: 34 single faces, flowers, seasons");
	}
	{
		char b[32];
		mj_face_name(2, b, sizeof(b));
		check(strcmp(b, "three of dots") == 0, "name: three of dots");
		mj_face_name(MJ_F_CHARS + 8, b, sizeof(b));
		check(strcmp(b, "nine of characters") == 0, "name: nine of characters");
		mj_face_name(MJ_F_WIND + 3, b, sizeof(b));
		check(strcmp(b, "north wind") == 0, "name: north wind");
		mj_face_name(MJ_F_DRAGON + 1, b, sizeof(b));
		check(strcmp(b, "green dragon") == 0, "name: green dragon");
		mj_face_name(MJ_F_FLOWER + 2, b, sizeof(b));
		check(strcmp(b, "chrysanthemum") == 0, "name: chrysanthemum");
		mj_face_name(MJ_F_SEASON + 3, b, sizeof(b));
		check(strcmp(b, "winter") == 0, "name: winter");
		mj_face_name(MJ_F_CHARS + 8, b, 5);
		check(strcmp(b, "nine") == 0, "name: cut to the buffer");
	}

	/* Two sets: a single layer of 20x10. */
	{
		static char text[2048];
		char err[64];
		int r, c, p = 0;
		p += sprintf(text + p, "layer\n");
		for (r = 0; r < 20; r++) {
			for (c = 0; c < 20; c++) p += sprintf(text + p, "[]");
			p += sprintf(text + p, "\n");
		}
		check(mj_layout_parse(text, &L, err, sizeof(err)), "a 200-tile layout parses");
		check_eq(L.n, 200, "200 tiles");
		check(mj_new(&G, &L, 9, MJ_DEAL_SOLVABLE), "a 200-tile layout deals");
		count_faces(&G, f);
		ok = 1;
		for (i = 0; i < MJ_FACES; i++) {
			int c2 = mj_class(i), tot = 0, j;
			for (j = 0; j < MJ_FACES; j++) if (mj_class(j) == c2) tot += f[j];
			if (tot % 2 || tot > 8) ok = 0;
		}
		check(ok, "a second set: every class even, at most eight");
	}
}

/* -- deals --------------------------------------------------------------- */

/* Plays the game's recorded solution from where it stands, checking the
 * free rule against geometry at every step. */
static bool replay(mj_game_t *g, int *disagree)
{
	int p;
	for (p = 0; p < g->nsol; p += 2) {
		if (disagree && (p & 15) == 0) *disagree += free_disagreements(g);
		if (!mj_take(g, g->sol[p], g->sol[p + 1])) return false;
	}
	return mj_state(g) == MJ_WON;
}

static void test_deals(void)
{
	static const int seeds[] = { 3000, 1000, 1000, 1000 };
	int i;

	for (i = 0; i < 4; i++) {
		int s, won = 0, retries = 0, disagree = 0, worst = 0;
		char what[96];
		load(i);
		for (s = 1; s <= seeds[i]; s++) {
			if (!mj_new(&G, &L, (uint32_t)s, MJ_DEAL_SOLVABLE)) continue;
			if (mj_tries > 1) retries++;
			if (mj_tries > worst) worst = mj_tries;
			if (replay(&G, &disagree)) won++;
		}
		snprintf(what, sizeof(what), "%s: every numbered deal clears", L.name);
		check_eq(won, seeds[i], what);
		snprintf(what, sizeof(what), "%s: free rule matches geometry throughout", L.name);
		check_eq(disagree, 0, what);
		/* A second try at emptying is luck; needing it on more than one
		 * deal in ten would mean the generator is fighting the shape. */
		snprintf(what, sizeof(what), "%s: deals rarely need a second try (%d of %d, worst %d)",
			 L.name, retries, seeds[i], worst);
		check(retries * 10 <= seeds[i], what);
		printf("  %-8s %d deals, %d needed a second try, worst %d\n",
		       L.name, seeds[i], retries, worst);
	}

	load(0);
	{
		static uint8_t a[MJ_MAX];
		mj_new(&G, &L, 77, MJ_DEAL_SOLVABLE);
		memcpy(a, G.face, sizeof(a));
		mj_new(&G, &L, 77, MJ_DEAL_SOLVABLE);
		check(memcmp(a, G.face, sizeof(a)) == 0, "a deal is its number");
		mj_new(&G, &L, 78, MJ_DEAL_SOLVABLE);
		check(memcmp(a, G.face, sizeof(a)) != 0, "the next number is another deal");
		mj_new(&G, &L, 0, MJ_DEAL_SOLVABLE);
		check(replay(&G, 0), "deal 0 is a deal like any other");
		mj_new(&G, &L, 0xffffffffu, MJ_DEAL_SOLVABLE);
		check(replay(&G, 0), "and so is the last number");
	}

	/* Shapes that cannot be emptied. */
	{
		char err[64];
		check(mj_layout_parse("layer\n[]\n[]\nlayer\n[]\n[]\n", &L, err, sizeof(err)),
		      "a stack of two parses");
		check(!mj_new(&G, &L, 1, MJ_DEAL_SOLVABLE), "a stack of two cannot be dealt");
		check(mj_layout_parse("layer\n[][]\n[][]\n", &L, err, sizeof(err)), "two side by side");
		check(mj_new(&G, &L, 1, MJ_DEAL_SOLVABLE) && replay(&G, 0), "two side by side clear");
	}
}

/* -- can these places be emptied at all? -----------------------------------
 *
 * Exhaustive, faces ignored: any two free places may go. For the
 * shuffles that said no -- each must be a shape that truly cannot be
 * emptied, not one the generator's 64 random attempts happened to
 * miss. Memoised on the set of places left; a budget keeps a bad case
 * from running forever, and running out of it is a failure too. */
#define EX_WORDS ((MJ_MAX + 31) / 32)
#define EX_HASH  (1 << 18)
static uint32_t ex_seen[EX_HASH][EX_WORDS];
static uint8_t  ex_used[EX_HASH];
static long     ex_nodes;

static bool ex_visit(const uint32_t *w)
{
	uint32_t h = 2166136261u;
	int i;
	for (i = 0; i < EX_WORDS; i++) h = (h ^ w[i]) * 16777619u;
	h &= EX_HASH - 1;
	while (ex_used[h]) {
		if (memcmp(ex_seen[h], w, sizeof(ex_seen[h])) == 0) return false;
		h = (h + 1) & (EX_HASH - 1);
	}
	ex_used[h] = 1;
	memcpy(ex_seen[h], w, sizeof(ex_seen[h]));
	return true;
}

static int ex_search(mj_game_t *g, uint32_t *w)
{
	int fr[MJ_MAX], nf = 0, i, j, t;

	if (g->left == 0) return 1;
	if (++ex_nodes > 2000000) return -1;
	if (!ex_visit(w)) return 0;
	for (t = 0; t < g->L->n; t++) if (free_slow(g, t)) fr[nf++] = t;
	for (i = 0; i < nf; i++)
		for (j = i + 1; j < nf; j++) {
			int r;
			uint8_t fa = g->face[fr[i]], fb = g->face[fr[j]];
			g->face[fr[i]] = g->face[fr[j]] = MJ_GONE;
			g->left -= 2;
			w[fr[i] >> 5] &= ~(1u << (fr[i] & 31));
			w[fr[j] >> 5] &= ~(1u << (fr[j] & 31));
			r = ex_search(g, w);
			g->face[fr[i]] = fa; g->face[fr[j]] = fb;
			g->left += 2;
			w[fr[i] >> 5] |= 1u << (fr[i] & 31);
			w[fr[j] >> 5] |= 1u << (fr[j] & 31);
			if (r) return r;
		}
	return 0;
}

/* 1 when the places left can be emptied, 0 when they cannot, -1 when
 * the search ran out of budget. */
static int emptiable(const mj_game_t *g0)
{
	static mj_game_t g;
	uint32_t w[EX_WORDS];
	int t;
	g = *g0;
	memset(w, 0, sizeof(w));
	for (t = 0; t < g.L->n; t++) if (g.face[t] != MJ_GONE) w[t >> 5] |= 1u << (t & 31);
	memset(ex_used, 0, sizeof(ex_used));
	ex_nodes = 0;
	return ex_search(&g, w);
}

/* -- play: undo, redo, hints, shuffles --------------------------------------- */

static uint32_t lcg = 12345;
static uint32_t pick(uint32_t n)
{
	lcg = lcg * 1103515245u + 12345u;
	return (lcg >> 8) % n;
}

/* Takes random legal pairs until there are none. */
static int play_badly(mj_game_t *g)
{
	static uint16_t a[512], b[512];
	int moves = 0, n;
	while ((n = mj_pairs(g, a, b, 512)) > 0) {
		int k = (int)pick((uint32_t)(n > 512 ? 512 : n));
		if (!mj_take(g, a[k], b[k])) { check(false, "a listed pair can be taken"); break; }
		moves++;
	}
	return moves;
}

static void test_play(void)
{
	static uint8_t start[MJ_MAX];
	int t, n, moves, k, stuck = 0, rescued = 0, fails = 0;

	load(0);
	mj_new(&G, &L, 2024, MJ_DEAL_SOLVABLE);
	memcpy(start, G.face, sizeof(start));

	check(!mj_take(&G, 0, 0), "a tile does not pair with itself");
	for (t = 0; t < L.n; t++) if (!mj_free(&G, t)) break;
	{
		int u;
		for (u = 0; u < L.n; u++)
			if (u != t && mj_match(G.face[t], G.face[u])) break;
		check(!mj_take(&G, t, u), "a pinned tile cannot be taken");
	}
	check(!mj_undo(&G), "nothing to undo at the start");

	moves = play_badly(&G);
	check(moves > 0, "some pairs could be taken");
	check_eq(G.left, L.n - 2 * moves, "two tiles per pair");
	check_eq(G.nhist, moves, "every pair recorded");
	for (k = 0; k < moves; k++) check(mj_undo(&G), "undo");
	check(memcmp(start, G.face, sizeof(start)) == 0, "undoing everything is the deal");
	check_eq(G.left, L.n, "all tiles back");
	for (k = 0; k < moves; k++) check(mj_redo(&G), "redo");
	check(!mj_redo(&G), "nothing more to redo");
	check_eq(G.left, L.n - 2 * moves, "redone");
	mj_undo(&G);
	{
		int a, b;
		check(mj_hint(&G, &a, &b), "a hint after an undo");
		mj_take(&G, a, b);
		check(!mj_redo(&G), "a new move ends redo");
	}

	/* Hints go round every pair, and every pair they give can be taken. */
	mj_new(&G, &L, 99, MJ_DEAL_SOLVABLE);
	{
		static uint16_t a[256], b[256];
		int seen = 0, i, ha, hb, okall = 1;
		n = mj_pairs(&G, a, b, 256);
		for (i = 0; i < n; i++) {
			if (!mj_hint(&G, &ha, &hb) || !mj_can_take(&G, ha, hb)) okall = 0;
			if (ha == a[i] && hb == b[i]) seen++;
		}
		check(okall, "every hint can be taken");
		check_eq(seen, n, "hints go round every pair in turn");
		mj_hint(&G, &ha, &hb);
		check(ha == a[0] && hb == b[0], "and start again");
		check_eq(G.hints, n + 1, "hints are counted");
	}

	/* Shuffles: play badly until stuck, shuffle, and the shuffle's own
	 * solution must clear what is left. Over many deals on every
	 * layout, with random deals among them. */
	for (k = 0; k < 4; k++) {
		int s;
		load(k);
		for (s = 1; s <= 300; s++) {
			int f0[MJ_FACES], f1[MJ_FACES], i, left, same = 1;
			mj_new(&G, &L, (uint32_t)s, (s & 1) ? MJ_DEAL_RANDOM : MJ_DEAL_SOLVABLE);
			play_badly(&G);
			if (mj_state(&G) == MJ_WON) continue;
			check_eq(mj_state(&G), MJ_STUCK, "no pairs means stuck");
			stuck++;
			count_faces(&G, f0);
			left = G.left;
			if (!mj_shuffle(&G)) {
				char m[96];
				fails++;
				snprintf(m, sizeof(m), "%s deal %d: a refused shuffle is truly hopeless (%d left)",
					 L.name, s, G.left);
				check_eq(emptiable(&G), 0, m);
				continue;
			}
			count_faces(&G, f1);
			for (i = 0; i < MJ_FACES; i++) if (f0[i] != f1[i]) same = 0;
			check(same, "a shuffle keeps the tiles");
			check_eq(G.left, left, "a shuffle keeps the count");
			check_eq(G.nhist, 0, "a shuffle clears undo");
			check(!mj_undo(&G), "nothing to undo after a shuffle");
			if (replay(&G, 0)) rescued++;
		}
	}
	printf("  shuffles: %d stuck games, %d cleared after one shuffle, %d could not shuffle\n",
	       stuck, rescued, fails);
	check(stuck > 100, "playing badly gets stuck often enough to test");
	check_eq(rescued + fails, stuck, "every shuffle that happens can be cleared");

	/* The same deal shuffled at the same point shuffles the same way. */
	{
		static uint8_t a[MJ_MAX];
		load(0);
		mj_new(&G, &L, 5, MJ_DEAL_SOLVABLE);
		lcg = 1; play_badly(&G);
		mj_shuffle(&G);
		memcpy(a, G.face, sizeof(a));
		mj_new(&G, &L, 5, MJ_DEAL_SOLVABLE);
		lcg = 1; play_badly(&G);
		mj_shuffle(&G);
		check(memcmp(a, G.face, sizeof(a)) == 0, "shuffles follow the deal number");
		mj_restart(&G);
		check_eq(G.left, L.n, "restart puts every tile back");
		check_eq(G.shuffles, 0, "restart forgets the shuffles");
		check(replay(&G, 0), "and the restarted deal clears");
	}

	check(!mj_shuffle(&G), "nothing to shuffle on an empty table");
}

int main(void)
{
	test_builtins();
	test_turtle_geometry();
	test_parser();
	test_set();
	test_deals();
	test_play();
	printf("engine_test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
