/*
 * Zeitlos mahjong -- the game. See mj_game.h.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */

#include <string.h>

#include "mj_game.h"

/* -- the deal's own generator ---------------------------------------------
 *
 * xorshift32: no multiply, so it costs the same on rv32i as on rv32im,
 * and nothing about a deal depends on which one built it. Quality is
 * ample for placing tiles. The seed is stirred first so that deals 1,
 * 2 and 3 do not start from nearly the same state. */

static uint32_t rng_next(uint32_t *s)
{
	uint32_t x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *s = x;
}

static uint32_t rng_seed(uint32_t seed)
{
	uint32_t s = seed ^ 0x9e3779b9u;
	int i;
	if (!s) s = 0x6d2b79f5u;
	for (i = 0; i < 8; i++) rng_next(&s);
	return s;
}

/* Uniform in [0, n). The rejection threshold is 2^32 mod n, computed
 * as (0 - n) % n so that it never needs 2^32 itself: when n divides
 * 2^32 the threshold is 0 and nothing is rejected. That is the case
 * sw/common/games/zrand.h records z_rng_below() hanging on. */
static uint32_t rng_below(uint32_t *s, uint32_t n)
{
	uint32_t t, v;
	if (n < 2) return 0;
	t = (0u - n) % n;
	do v = rng_next(s); while (v < t);
	return v % n;
}

/* -- faces ------------------------------------------------------------- */

int mj_class(int f)
{
	if (f < MJ_F_FLOWER) return f;
	return f < MJ_F_SEASON ? 34 : 35;
}

bool mj_match(int fa, int fb)
{
	if (fa < 0 || fb < 0 || fa >= MJ_FACES || fb >= MJ_FACES) return false;
	return mj_class(fa) == mj_class(fb);
}

static void put(char *buf, int len, int *i, const char *s)
{
	while (*s && *i < len - 1) buf[(*i)++] = *s++;
}

void mj_face_name(int f, char *buf, int len)
{
	static const char *const num[9] = {
		"one", "two", "three", "four", "five",
		"six", "seven", "eight", "nine"
	};
	static const char *const suit[3] = { "dots", "bamboo", "characters" };
	static const char *const other[15] = {
		"east wind", "south wind", "west wind", "north wind",
		"red dragon", "green dragon", "white dragon",
		"plum", "orchid", "chrysanthemum", "bamboo flower",
		"spring", "summer", "autumn", "winter"
	};
	int i = 0;

	if (!buf || len <= 0) return;
	if (f >= 0 && f < MJ_F_WIND) {
		put(buf, len, &i, num[f % 9]);
		put(buf, len, &i, " of ");
		put(buf, len, &i, suit[f / 9]);
	} else if (f >= MJ_F_WIND && f < MJ_FACES) {
		put(buf, len, &i, other[f - MJ_F_WIND]);
	} else {
		put(buf, len, &i, "nothing");
	}
	buf[i] = 0;
}

/* -- the free rule ----------------------------------------------------- */

static bool free_in(const mj_layout_t *L, const uint8_t *on, int t)
{
	int k;
	bool lopen = true, ropen = true;

	for (k = 0; k < L->nabove[t]; k++)
		if (on[L->above[t][k]]) return false;
	for (k = 0; k < L->nleft[t]; k++)
		if (on[L->left[t][k]]) { lopen = false; break; }
	if (lopen) return true;
	for (k = 0; k < L->nright[t]; k++)
		if (on[L->right[t][k]]) { ropen = false; break; }
	return ropen;
}

/* Scratch for the emptying. Statics, not locals: a few hundred bytes
 * each, on a 16KB stack. */
static uint8_t  on_tmp[MJ_MAX];
static uint16_t free_tmp[MJ_MAX];

/* Empties the places marked in `on` (count of them in n) by taking
 * random pairs of free places, writing the order into out[]. False
 * when it ends with tiles left and fewer than two free. */
static bool empty_once(const mj_layout_t *L, const uint8_t *on, int n,
		       uint32_t *rng, uint16_t *out)
{
	int done = 0, t, nf;

	memcpy(on_tmp, on, (size_t)L->n);
	while (done < n) {
		uint32_t i = 0, j = 0;
		int look;
		nf = 0;
		for (t = 0; t < L->n; t++)
			if (on_tmp[t] && free_in(L, on_tmp, t)) free_tmp[nf++] = (uint16_t)t;
		if (nf < 2) return false;

		/* One step of lookahead: a pair whose taking leaves tiles but
		 * fewer than two free is a dead end on the spot, so another
		 * pair is drawn instead, a few times. It costs a scan per draw
		 * and roughly halves how often a whole attempt is thrown away;
		 * the rest of the dead ends are further off than one step. */
		for (look = 0; look < 8; look++) {
			int k, f2 = 0;
			i = rng_below(rng, (uint32_t)nf);
			j = rng_below(rng, (uint32_t)nf - 1);
			if (j >= i) j++;
			if (done + 2 >= n) break;
			on_tmp[free_tmp[i]] = on_tmp[free_tmp[j]] = 0;
			for (k = 0; k < L->n && f2 < 2; k++)
				if (on_tmp[k] && free_in(L, on_tmp, k)) f2++;
			on_tmp[free_tmp[i]] = on_tmp[free_tmp[j]] = 1;
			if (f2 >= 2) break;
		}
		out[done++] = free_tmp[i];
		out[done++] = free_tmp[j];
		on_tmp[free_tmp[i]] = 0;
		on_tmp[free_tmp[j]] = 0;
	}
	return true;
}

/* Tries a few times. A dead end is a matter of luck on most layouts
 * and a matter of shape on a few; 64 tries tells them apart well
 * enough (the tests measure how often the built-ins need a second). */
#define MJ_TRIES 64

int mj_tries;

static bool empty_order(const mj_layout_t *L, const uint8_t *on, int n,
			uint32_t *rng, uint16_t *out)
{
	for (mj_tries = 1; mj_tries <= MJ_TRIES; mj_tries++)
		if (empty_once(L, on, n, rng, out)) return true;
	return false;
}

/* -- dealing ------------------------------------------------------------ */

/* A face of class c, the k-th of that class handed out. Only the bonus
 * classes have more than one face. */
static uint8_t face_of(int c, int k)
{
	if (c < 34) return (uint8_t)c;
	return (uint8_t)((c == 34 ? MJ_F_FLOWER : MJ_F_SEASON) + (k & 3));
}

static uint8_t pair_class[MJ_MAX / 2];

/* npairs classes for a deal: the full set (each class twice, which is
 * 72 pairs) shuffled, as many sets as the layout needs, the last one
 * cut short. A layout smaller than 144 gets a random part of a set,
 * not always the dots. */
static void choose_pairs(int npairs, uint32_t *rng)
{
	int base, i, k;

	for (base = 0; base < npairs; base += 2 * MJ_CLASSES) {
		uint8_t set[2 * MJ_CLASSES];
		for (i = 0; i < 2 * MJ_CLASSES; i++) set[i] = (uint8_t)(i >> 1);
		for (i = 2 * MJ_CLASSES - 1; i > 0; i--) {
			uint8_t t;
			k = (int)rng_below(rng, (uint32_t)i + 1);
			t = set[i]; set[i] = set[k]; set[k] = t;
		}
		for (i = 0; i < 2 * MJ_CLASSES && base + i < npairs; i++)
			pair_class[base + i] = set[i];
	}
}

static void clear_play(mj_game_t *g)
{
	g->nhist = g->ntop = 0;
	g->shuffles = g->hints = g->undos = 0;
	g->hint_next = 0;
}

bool mj_new(mj_game_t *g, const mj_layout_t *L, uint32_t seed, int mode)
{
	static uint8_t all[MJ_MAX];
	int npairs = L->n / 2, p, t, cnt[MJ_CLASSES];

	memset(g, 0, sizeof(*g));
	g->L = L;
	g->seed = seed;
	g->mode = mode;
	g->rng = rng_seed(seed);
	memset(g->face, MJ_GONE, sizeof(g->face));

	memset(all, 0, sizeof(all));
	memset(all, 1, (size_t)L->n);
	if (!empty_order(L, all, L->n, &g->rng, g->sol)) return false;
	g->nsol = L->n;

	choose_pairs(npairs, &g->rng);
	memset(cnt, 0, sizeof(cnt));
	for (p = 0; p < npairs; p++) {
		int c = pair_class[p];
		uint8_t fa = face_of(c, cnt[c]++);
		uint8_t fb = face_of(c, cnt[c]++);
		/* Which of the two places gets which bonus face is a coin
		 * toss; for every other class the faces are the same. */
		if (rng_below(&g->rng, 2)) { uint8_t x = fa; fa = fb; fb = x; }
		g->face[g->sol[2 * p]] = fa;
		g->face[g->sol[2 * p + 1]] = fb;
	}

	if (mode == MJ_DEAL_RANDOM) {
		/* The same tiles, scattered. The order above is no longer a
		 * solution, so it is forgotten rather than kept to mislead. */
		for (t = L->n - 1; t > 0; t--) {
			int k = (int)rng_below(&g->rng, (uint32_t)t + 1);
			uint8_t x = g->face[t];
			g->face[t] = g->face[k];
			g->face[k] = x;
		}
		g->nsol = 0;
	}

	g->left = L->n;
	clear_play(g);
	return true;
}

void mj_restart(mj_game_t *g)
{
	/* A deal is its number: dealing it again is the same tiles, the
	 * same solution and the same shuffles to come. Cheaper in memory
	 * than keeping a copy, and it cannot drift from what was dealt. */
	mj_new(g, g->L, g->seed, g->mode);
}

/* -- play --------------------------------------------------------------- */

bool mj_present(const mj_game_t *g, int t)
{
	return g->L && t >= 0 && t < g->L->n && g->face[t] != MJ_GONE;
}

static uint8_t on_now[MJ_MAX];

static void mark_on(const mj_game_t *g)
{
	int t;
	for (t = 0; t < g->L->n; t++) on_now[t] = g->face[t] != MJ_GONE;
}

bool mj_free(const mj_game_t *g, int t)
{
	if (!mj_present(g, t)) return false;
	mark_on(g);
	return free_in(g->L, on_now, t);
}

void mj_free_all(const mj_game_t *g, uint8_t *out)
{
	int t;
	mark_on(g);
	for (t = 0; t < g->L->n; t++)
		out[t] = (uint8_t)(on_now[t] && free_in(g->L, on_now, t));
}

bool mj_can_take(const mj_game_t *g, int a, int b)
{
	if (a == b || !mj_present(g, a) || !mj_present(g, b)) return false;
	if (!mj_match(g->face[a], g->face[b])) return false;
	mark_on(g);
	return free_in(g->L, on_now, a) && free_in(g->L, on_now, b);
}

bool mj_take(mj_game_t *g, int a, int b)
{
	mj_move_t *m;

	if (!mj_can_take(g, a, b)) return false;
	m = &g->hist[g->nhist++];
	m->a = (uint16_t)a;
	m->b = (uint16_t)b;
	m->fa = g->face[a];
	m->fb = g->face[b];
	g->ntop = g->nhist;
	g->face[a] = g->face[b] = MJ_GONE;
	g->left -= 2;
	g->hint_next = 0;
	return true;
}

bool mj_undo(mj_game_t *g)
{
	mj_move_t *m;
	if (g->nhist == 0) return false;
	m = &g->hist[--g->nhist];
	g->face[m->a] = m->fa;
	g->face[m->b] = m->fb;
	g->left += 2;
	g->undos++;
	g->hint_next = 0;
	return true;
}

bool mj_redo(mj_game_t *g)
{
	mj_move_t m;
	int top;
	if (g->nhist >= g->ntop) return false;
	m = g->hist[g->nhist];
	top = g->ntop;
	if (!mj_take(g, m.a, m.b)) return false;
	g->ntop = top;          /* mj_take() cut the redo list; restore it */
	return true;
}

int mj_pairs(const mj_game_t *g, uint16_t *a, uint16_t *b, int max)
{
	int nf = 0, i, j, n = 0;

	if (!g->L) return 0;
	mark_on(g);
	for (i = 0; i < g->L->n; i++)
		if (on_now[i] && free_in(g->L, on_now, i)) free_tmp[nf++] = (uint16_t)i;
	for (i = 0; i < nf; i++)
		for (j = i + 1; j < nf; j++)
			if (mj_match(g->face[free_tmp[i]], g->face[free_tmp[j]])) {
				if (n < max) { a[n] = free_tmp[i]; b[n] = free_tmp[j]; }
				n++;
			}
	return n;
}

/* At most 4*4/2 pairs per class in play on a full set, times two sets;
 * a list this long is never cut short in practice, and if it were the
 * hint would still go round the first MJ_HINTS. */
#define MJ_HINTS 128

bool mj_hint(mj_game_t *g, int *a, int *b)
{
	static uint16_t ha[MJ_HINTS], hb[MJ_HINTS];
	int n = mj_pairs(g, ha, hb, MJ_HINTS), k;

	if (n == 0) return false;
	if (n > MJ_HINTS) n = MJ_HINTS;
	k = g->hint_next % n;
	g->hint_next = k + 1;
	*a = ha[k];
	*b = hb[k];
	g->hints++;
	return true;
}

int mj_state(const mj_game_t *g)
{
	uint16_t a, b;
	if (g->left == 0) return MJ_WON;
	return mj_pairs(g, &a, &b, 1) ? MJ_PLAYING : MJ_STUCK;
}

bool mj_shuffle(mj_game_t *g)
{
	static uint16_t order[MJ_MAX];
	static uint8_t  fpair[MJ_MAX];      /* faces, two by two */
	int t, c, n = 0, np = 0, p;

	if (g->left == 0) return false;
	mark_on(g);
	if (!empty_order(g->L, on_now, g->left, &g->rng, order)) return false;

	/* The faces left, paired by class. Every class has an even number
	 * left -- tiles only ever go two of a class at a time -- so pairing
	 * them in any order gives pairs that match. */
	for (c = 0; c < MJ_CLASSES; c++)
		for (t = 0; t < g->L->n; t++)
			if (g->face[t] != MJ_GONE && mj_class(g->face[t]) == c)
				fpair[n++] = g->face[t];

	/* Shuffle the pairs as units, then lay them along the order. */
	np = n / 2;
	for (p = np - 1; p > 0; p--) {
		int k = (int)rng_below(&g->rng, (uint32_t)p + 1);
		uint8_t x0 = fpair[2 * p], x1 = fpair[2 * p + 1];
		fpair[2 * p] = fpair[2 * k];
		fpair[2 * p + 1] = fpair[2 * k + 1];
		fpair[2 * k] = x0;
		fpair[2 * k + 1] = x1;
	}
	for (p = 0; p < np; p++) {
		uint8_t fa = fpair[2 * p], fb = fpair[2 * p + 1];
		if (rng_below(&g->rng, 2)) { uint8_t x = fa; fa = fb; fb = x; }
		g->face[order[2 * p]] = fa;
		g->face[order[2 * p + 1]] = fb;
	}

	memcpy(g->sol, order, (size_t)g->left * sizeof(order[0]));
	g->nsol = g->left;
	g->nhist = g->ntop = 0;
	g->hint_next = 0;
	g->shuffles++;
	return true;
}
