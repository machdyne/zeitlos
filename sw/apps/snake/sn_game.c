/*
 * snake -- the rules. See sn_game.h.
 */

#include <string.h>

#include "sn_game.h"
#include "sn_levels.h"

static const int8_t ddx[4] = { 1, 0, -1, 0 };
static const int8_t ddy[4] = { 0, 1, 0, -1 };

#define OPP(d) (((d) + 2) & 3)
#define FULL (SN_CELL_PX << 8)

/* -- tuning: cells per second x10 -- */
#define SPEED_CLASSIC   70
#define SPEED_STEP       3      /* faster per food, classic and open */
#define SPEED_MAX      150
#define SPEED_MAZE      80
#define SPEED_MAZE_LVL   6
#define SPEED_VERSUS    85
#define SPEED_TITLE     95

#define GROW_PER_FOOD    2
#define BONUS_EVERY      5      /* foods */
#define BONUS_FRAMES   420      /* 7 seconds at 60Hz */
#define MAZE_GOAL       12

uint16_t sn_speed_for(int cps10) {
	/* 8 px a cell, 60 frames a second, 8.8 fixed point */
	return (uint16_t)((cps10 * 8 * 256) / 600);
}

uint16_t sn_neighbour(const sn_game_t *g, uint16_t c, int d) {
	int x = sn_cx(c) + ddx[d], y = sn_cy(c) + ddy[d];
	if (g->wrap) {
		if (x < 0) x += SN_GW;
		if (x >= SN_GW) x -= SN_GW;
		if (y < 0) y += SN_GH;
		if (y >= SN_GH) y -= SN_GH;
	} else if (x < 0 || y < 0 || x >= SN_GW || y >= SN_GH) {
		return SN_NONE;
	}
	return sn_cell(x, y);
}

int sn_dir_between(const sn_game_t *g, uint16_t a, uint16_t b) {
	int d;
	for (d = 0; d < 4; d++)
		if (sn_neighbour(g, a, d) == b) return d;
	return SN_NODIR;
}

uint16_t sn_segment(const sn_snake_t *s, int i) {
	return s->body[(s->tail + i) % SN_MAXLEN];
}

static void event(sn_game_t *g, int type, int p, uint16_t cell, int value) {
	sn_event_t *e;
	if (g->nev >= SN_MAX_EV) return;
	e = &g->ev[g->nev++];
	e->type = (uint8_t)type;
	e->p = (uint8_t)p;
	e->cell = cell;
	e->value = (uint16_t)value;
}

static bool cell_free(const sn_game_t *g, uint16_t c) {
	int p;
	if (g->grid[c].what != SN_EMPTY) return false;
	if (c == g->food || c == g->bonus) return false;
	for (p = 0; p < SN_PLAYERS; p++)
		if (g->s[p].active && g->s[p].alive && g->s[p].next == c)
			return false;
	return true;
}

/* A random free cell, not right in front of a head. Probing from a
 * random start keeps it uniform enough and bounded. */
static uint16_t place_item(sn_game_t *g) {
	uint32_t start = g->rand_below(SN_GW * SN_GH);
	uint32_t i;
	for (i = 0; i < SN_GW * SN_GH; i++) {
		uint16_t c = (uint16_t)((start + i) % (SN_GW * SN_GH));
		int p;
		bool near = false;
		if (!cell_free(g, c)) continue;
		for (p = 0; p < SN_PLAYERS; p++) {
			const sn_snake_t *s = &g->s[p];
			uint16_t h;
			int dx, dy;
			if (!s->active || !s->alive) continue;
			h = s->body[s->head];
			dx = sn_cx(c) - sn_cx(h);
			dy = sn_cy(c) - sn_cy(h);
			if (dx * dx + dy * dy < 5) near = true;
		}
		if (near && i < SN_GW * SN_GH / 2) continue;
		return c;
	}
	return SN_NONE;
}

static void place_food(sn_game_t *g) {
	g->food = SN_NONE;
	g->food = place_item(g);
	if (g->food != SN_NONE) event(g, SN_EV_FOOD, 0, g->food, 0);
}

static int base_speed(const sn_game_t *g) {
	switch (g->mode) {
	case SN_MODE_MAZE:   return SPEED_MAZE + SPEED_MAZE_LVL * (g->level % 6);
	case SN_MODE_VERSUS: return SPEED_VERSUS;
	case SN_MODE_TITLE:  return SPEED_TITLE;
	default:             return SPEED_CLASSIC;
	}
}

static void put_body(sn_game_t *g, int p, uint16_t c, uint8_t links) {
	sn_snake_t *s = &g->s[p];
	g->grid[c].what = SN_BODY;
	g->grid[c].who = (uint8_t)p;
	g->grid[c].col = s->colour_next;
	g->grid[c].links = links;
	s->colour_next = (uint8_t)((s->colour_next + 1) & 3);
}

static void spawn(sn_game_t *g, int p, int x, int y, int dir) {
	sn_snake_t *s = &g->s[p];
	int i;
	memset(s, 0, sizeof(*s));
	s->active = true;
	s->alive = true;
	s->dir = (uint8_t)dir;
	s->speed = sn_speed_for(base_speed(g));
	s->len = 0;
	s->tail = 0;
	/* Three cells behind the start, tail first. */
	for (i = 3; i >= 0; i--) {
		int cx = x - ddx[dir] * i, cy = y - ddy[dir] * i;
		uint16_t c = sn_cell(cx, cy);
		uint8_t links = 0;
		if (i < 3) links |= (uint8_t)(1u << OPP(dir));
		if (i > 0) links |= (uint8_t)(1u << dir);
		s->body[s->len] = c;
		s->len++;
		put_body(g, p, c, links);
	}
	s->head = (uint16_t)(s->len - 1);
	s->next = SN_NONE;
}

void sn_new(sn_game_t *g, sn_mode_t mode, int level,
	uint32_t (*rand_below)(uint32_t n)) {

	const char *const *lv;
	int x, y;

	memset(g, 0, sizeof(*g));
	g->mode = mode;
	g->level = (uint8_t)level;
	g->rand_below = rand_below;
	g->wrap = (mode == SN_MODE_OPEN);
	g->players = (mode == SN_MODE_VERSUS) ? 2 : 1;
	g->food = SN_NONE;
	g->bonus = SN_NONE;
	g->foods_to_bonus = BONUS_EVERY;
	g->level_goal = MAZE_GOAL;

	lv = sn_layout(mode, level);

	for (y = 0; y < SN_GH; y++)
		for (x = 0; x < SN_GW; x++) {
			char ch = lv[y][x];
			sn_cell_t *c = &g->grid[sn_cell(x, y)];
			if (ch == '#') c->what = SN_WALL;
			else if (ch == 'B') c->what = SN_BLOCKED;
		}

	for (y = 0; y < SN_GH; y++)
		for (x = 0; x < SN_GW; x++) {
			char ch = lv[y][x];
			if (ch == '1') spawn(g, 0, x, y, SN_RIGHT);
			else if (ch == '2' && g->players == 2) spawn(g, 1, x, y, SN_LEFT);
		}

	g->nev = 0;
	place_food(g);
	g->nev = 0;
}

void sn_turn(sn_game_t *g, int p, int dir) {
	sn_snake_t *s = &g->s[p];
	uint8_t last;
	if (p < 0 || p >= SN_PLAYERS || !s->active || !s->alive) return;
	if (dir < 0 || dir > 3) return;
	/* Compared with the last turn queued (or the current direction):
	 * a repeat or a reverse of that is not a turn. */
	last = s->nq ? s->queue[s->nq - 1] : s->dir;
	if (dir == last || dir == OPP(last)) return;
	if (s->nq < 2) s->queue[s->nq++] = (uint8_t)dir;
}

static void die(sn_game_t *g, int p, uint16_t where) {
	sn_snake_t *s = &g->s[p];
	if (!s->alive) return;
	s->alive = false;
	s->next = SN_NONE;
	s->progress = 0;
	event(g, SN_EV_DIE, p, where, 0);
}

static void start_move(sn_game_t *g, int p) {

	sn_snake_t *s = &g->s[p];
	uint16_t h = s->body[s->head], n;
	int q;

	if (s->nq) {
		uint8_t d = s->queue[0];
		s->queue[0] = s->queue[1];
		s->nq--;
		if (d != OPP(s->dir)) s->dir = d;
	}

	s->tail_moves = (s->grow == 0);
	n = sn_neighbour(g, h, s->dir);

	if (n == SN_NONE) { die(g, p, h); return; }

	{
		const sn_cell_t *c = &g->grid[n];
		bool ok = true;
		if (c->what == SN_WALL || c->what == SN_BLOCKED) ok = false;
		if (c->what == SN_BODY) {
			/* Its own tail, leaving as the head arrives: fine.
			 * Anything else that is a body is not. */
			ok = (c->who == p && n == s->body[s->tail] && s->tail_moves);
			/* Into the other's head while it comes the other way:
			 * a head-on, whichever of the two moved first. */
			if (!ok && c->who != p) {
				sn_snake_t *o = &g->s[c->who];
				if (o->alive && n == o->body[o->head] &&
				    o->dir == OPP(s->dir)) {
					die(g, c->who, n);
					die(g, p, n);
					return;
				}
			}
		}
		if (!ok) { die(g, p, n); return; }
	}

	/* Head on: both enter the same cell. */
	for (q = 0; q < SN_PLAYERS; q++) {
		sn_snake_t *o = &g->s[q];
		if (q == p || !o->active || !o->alive) continue;
		if (o->next == n) {
			die(g, q, n);
			die(g, p, n);
			return;
		}
	}

	s->next = n;
	g->grid[h].links |= (uint8_t)(1u << s->dir);
	event(g, SN_EV_MOVE, p, n, 0);
}

static void complete_move(sn_game_t *g, int p) {

	sn_snake_t *s = &g->s[p];
	uint16_t n = s->next;

	if (n == SN_NONE) return;

	if (s->tail_moves) {
		uint16_t t = s->body[s->tail];
		uint16_t t2;
		s->tail = (uint16_t)((s->tail + 1) % SN_MAXLEN);
		s->len--;
		t2 = s->body[s->tail];
		if (g->grid[t].what == SN_BODY && g->grid[t].who == p) {
			g->grid[t].what = SN_EMPTY;
			g->grid[t].links = 0;
		}
		{
			int d = sn_dir_between(g, t2, t);
			if (d != SN_NODIR) g->grid[t2].links &= (uint8_t)~(1u << d);
		}
	} else if (s->grow) {
		s->grow--;
	}

	s->head = (uint16_t)((s->head + 1) % SN_MAXLEN);
	s->body[s->head] = n;
	s->len++;
	put_body(g, p, n, (uint8_t)(1u << OPP(s->dir)));
	s->next = SN_NONE;

	if (n == g->food) {
		int value = 10 + 2 * (s->eaten / 5);
		if (g->mode == SN_MODE_MAZE) value += 5 * g->level;
		s->grow += GROW_PER_FOOD;
		s->score += (uint32_t)value;
		s->eaten++;
		event(g, SN_EV_EAT, p, n, value);

		if (g->mode == SN_MODE_CLASSIC || g->mode == SN_MODE_OPEN) {
			uint16_t mx = sn_speed_for(SPEED_MAX);
			uint16_t ns = (uint16_t)(s->speed + sn_speed_for(SPEED_STEP));
			if (ns > mx) ns = mx;
			if (ns != s->speed) {
				s->speed = ns;
				event(g, SN_EV_SPEEDUP, p, n, 0);
			}
		}

		place_food(g);

		if (g->mode != SN_MODE_TITLE && g->mode != SN_MODE_VERSUS) {
			if (g->foods_to_bonus) g->foods_to_bonus--;
			if (g->foods_to_bonus == 0 && g->bonus == SN_NONE) {
				g->bonus = place_item(g);
				g->bonus_left = g->bonus_total = BONUS_FRAMES;
				g->foods_to_bonus = BONUS_EVERY;
				if (g->bonus != SN_NONE)
					event(g, SN_EV_BONUS_SHOW, 0, g->bonus, 0);
			}
		}

		if (g->mode == SN_MODE_MAZE && s->eaten >= g->level_goal)
			event(g, SN_EV_LEVEL_DONE, p, n, 0);
	}

	if (n == g->bonus) {
		/* Worth more the sooner it is reached. */
		int value = 50 + (100 * g->bonus_left) / (g->bonus_total ? g->bonus_total : 1);
		s->score += (uint32_t)value;
		s->grow += 1;
		event(g, SN_EV_BONUS_EAT, p, n, value);
		g->bonus = SN_NONE;
	}
}

void sn_step(sn_game_t *g) {

	int p, alive = 0, active = 0;

	g->nev = 0;
	g->frame++;

	for (p = 0; p < SN_PLAYERS; p++) {
		sn_snake_t *s = &g->s[p];
		if (!s->active || !s->alive) continue;

		if (s->next == SN_NONE) {
			start_move(g, p);
			if (!s->alive) continue;
		}

		s->progress = (uint16_t)(s->progress + s->speed);
		while (s->alive && s->progress >= FULL) {
			s->progress = (uint16_t)(s->progress - FULL);
			complete_move(g, p);
			start_move(g, p);
		}
		if (!s->alive) s->progress = 0;
	}

	if (g->bonus != SN_NONE) {
		if (g->bonus_left) g->bonus_left--;
		if (g->bonus_left == 0) {
			event(g, SN_EV_BONUS_GONE, 0, g->bonus, 0);
			g->bonus = SN_NONE;
		}
	}

	for (p = 0; p < SN_PLAYERS; p++) {
		if (!g->s[p].active) continue;
		active++;
		if (g->s[p].alive) alive++;
	}
	if (g->players == 1) g->over = (alive == 0);
	else g->over = (alive <= 1 && active == 2);
}

uint16_t sn_pop_head(sn_game_t *g, int p) {
	sn_snake_t *s = &g->s[p];
	uint16_t c, prev;
	int d;
	if (!s->active || s->len == 0) return SN_NONE;
	c = s->body[s->head];
	if (g->grid[c].what == SN_BODY && g->grid[c].who == p) {
		g->grid[c].what = SN_EMPTY;
		g->grid[c].links = 0;
	}
	s->len--;
	if (s->len == 0) return c;
	s->head = (uint16_t)((s->head + SN_MAXLEN - 1) % SN_MAXLEN);
	prev = s->body[s->head];
	d = sn_dir_between(g, prev, c);
	if (d != SN_NODIR) g->grid[prev].links &= (uint8_t)~(1u << d);
	return c;
}

/* -- the autopilot --
 *
 * For the title screen's snake: of the three ways it can go, prefer one
 * that is not instant death, then one with room behind it (a small
 * flood fill, so it does not drive into a pocket), then the one nearest
 * the food, then straight on. Decided once per move, which is a few
 * times a second. */

static int room(const sn_game_t *g, uint16_t from, int cap) {
	static uint16_t q[128];
	static uint8_t seen[SN_GW * SN_GH];
	int qh = 0, qt = 0, n = 0, d;
	memset(seen, 0, sizeof(seen));
	q[qt++] = from;
	seen[from] = 1;
	while (qh < qt && n < cap) {
		uint16_t c = q[qh++];
		n++;
		for (d = 0; d < 4; d++) {
			uint16_t m = sn_neighbour(g, c, d);
			if (m == SN_NONE || seen[m]) continue;
			if (g->grid[m].what != SN_EMPTY) continue;
			seen[m] = 1;
			if (qt < 128) q[qt++] = m;
		}
	}
	return n;
}

void sn_autopilot(sn_game_t *g, int p) {

	sn_snake_t *s = &g->s[p];
	uint16_t from;
	int d, best = -1, best_score = -100000;
	int cur;

	if (!s->active || !s->alive || s->nq) return;

	/* Plan from where the head will be after this move. */
	from = s->next != SN_NONE ? s->next : s->body[s->head];
	cur = s->dir;

	for (d = 0; d < 4; d++) {
		uint16_t m;
		int score = 0, r;
		if (d == OPP(cur)) continue;
		m = sn_neighbour(g, from, d);
		if (m == SN_NONE) continue;
		if (g->grid[m].what != SN_EMPTY) continue;
		r = room(g, m, 40);
		score += r * 100;
		if (g->food != SN_NONE) {
			int dx = sn_cx(m) - sn_cx(g->food), dy = sn_cy(m) - sn_cy(g->food);
			if (dx < 0) dx = -dx;
			if (dy < 0) dy = -dy;
			score -= (dx + dy) * 10;
		}
		if (d == cur) score += 5;
		if (score > best_score) { best_score = score; best = d; }
	}

	if (best >= 0 && best != cur) sn_turn(g, p, best);
}
