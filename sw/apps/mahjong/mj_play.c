/*
 * Zeitlos mahjong -- a session. See mj_play.h.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */

#include <string.h>

#include "mj_play.h"

/* -- small string helpers (no printf: see docs/app_runtime.md) -------------- */

static void scpy(char *d, int len, const char *s)
{
	int i = 0;
	while (s && s[i] && i < len - 1) { d[i] = s[i]; i++; }
	d[i] = 0;
}

static void scat(char *d, int len, const char *s)
{
	int i = 0;
	while (d[i] && i < len - 1) i++;
	while (s && *s && i < len - 1) d[i++] = *s++;
	d[i] = 0;
}

static void ucat(char *d, int len, uint32_t v)
{
	char t[12];
	int i = 0;
	do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v && i < 11);
	{
		char r[12];
		int k = 0;
		while (i) r[k++] = t[--i];
		r[k] = 0;
		scat(d, len, r);
	}
}

void mj_play_time(uint32_t s, char *buf, int len)
{
	uint32_t h = s / 3600, m = (s / 60) % 60, sec = s % 60;
	buf[0] = 0;
	if (h) {
		ucat(buf, len, h);
		scat(buf, len, ":");
		if (m < 10) scat(buf, len, "0");
	}
	ucat(buf, len, m);
	scat(buf, len, ":");
	if (sec < 10) scat(buf, len, "0");
	ucat(buf, len, sec);
}

/* -- out ----------------------------------------------------------------------- */

static void dirty(mj_play_t *p, int x, int y, int w, int h)
{
	if (p->all) return;
	if (p->nrect >= MJ_RECTS) { p->all = true; return; }
	p->rect[p->nrect].x = (int16_t)x;
	p->rect[p->nrect].y = (int16_t)y;
	p->rect[p->nrect].w = (int16_t)w;
	p->rect[p->nrect].h = (int16_t)h;
	p->nrect++;
}

static void dirty_tile(mj_play_t *p, int t)
{
	int x, y, w, h;
	if (t < 0 || !p->g.L || t >= p->g.L->n) return;
	mj_tile_box(&p->v, t, &x, &y, &w, &h);
	dirty(p, x, y, w, h);
}

static void say(mj_play_t *p, const char *s)
{
	scpy(p->say, sizeof(p->say), s);
}

/* A message in the bar for `secs` seconds, spoken too. */
static void msg(mj_play_t *p, const char *s, uint32_t secs)
{
	scpy(p->v.msg, MJ_TEXT_LEN, s);
	p->msg_until = p->now + secs;
	p->bar = true;
	say(p, s);
}

void mj_play_status(mj_play_t *p)
{
	char t[16];
	mj_view_t *v = &p->v;
	v->status[0] = 0;
	scat(v->status, MJ_TEXT_LEN, p->L.name);
	scat(v->status, MJ_TEXT_LEN, " #");
	ucat(v->status, MJ_TEXT_LEN, p->g.seed);
	if (p->g.mode == MJ_DEAL_RANDOM) scat(v->status, MJ_TEXT_LEN, "?");
	scat(v->status, MJ_TEXT_LEN, "  ");
	ucat(v->status, MJ_TEXT_LEN, (uint32_t)p->g.left);
	scat(v->status, MJ_TEXT_LEN, " left  ");
	mj_play_time(p->elapsed, t, sizeof(t));
	scat(v->status, MJ_TEXT_LEN, t);
	if (p->g.L && p->g.left && mj_state(&p->g) == MJ_STUCK)
		scat(v->status, MJ_TEXT_LEN, "  stuck");
	p->bar = true;
}

/* -- statistics ----------------------------------------------------------------- */

static mj_stat_t *stat_for(mj_play_t *p, const char *name, bool make)
{
	int i;
	for (i = 0; i < p->nstats; i++)
		if (!strcmp(p->stats[i].name, name)) return &p->stats[i];
	if (!make) return 0;
	if (p->nstats >= MJ_STATS) return 0;
	memset(&p->stats[p->nstats], 0, sizeof(mj_stat_t));
	scpy(p->stats[p->nstats].name, MJ_NAME, name);
	return &p->stats[p->nstats++];
}

const mj_stat_t *mj_play_stat(const mj_play_t *p, const char *name)
{
	return stat_for((mj_play_t *)p, name, false);
}

/* -- setting up ---------------------------------------------------------------- */

void mj_play_init(mj_play_t *p, mj_layout_src_fn src, void *user)
{
	memset(p, 0, sizeof(*p));
	p->src = src;
	p->src_user = user;
	mj_view_init(&p->v, &p->g);
	p->burst[0] = p->burst[1] = -1;
}

void mj_play_add_layout(mj_play_t *p, const char *name)
{
	int i;
	if (p->nlayouts >= MJ_LAYOUTS) return;
	for (i = 0; i < p->nlayouts; i++)
		if (!strcmp(p->lname[i], name)) return;
	scpy(p->lname[p->nlayouts++], MJ_NAME, name);
}

int mj_play_find_layout(const mj_play_t *p, const char *name)
{
	int i;
	for (i = 0; i < p->nlayouts; i++)
		if (!strcmp(p->lname[i], name)) return i;
	return -1;
}

static void clear_marks(mj_play_t *p)
{
	p->v.sel = p->v.hint_a = p->v.hint_b = -1;
	p->v.cursor = -1;
}

void mj_play_new(mj_play_t *p, uint32_t seed)
{
	if (!p->g.L) return;
	if (!mj_new(&p->g, &p->L, seed, p->random ? MJ_DEAL_RANDOM : MJ_DEAL_SOLVABLE)) {
		msg(p, "This layout cannot be played", 8);
		p->all = true;
		return;
	}
	clear_marks(p);
	if (p->keyboard) p->v.cursor = mj_nav_first(&p->v);
	p->timing = p->counted = false;
	p->elapsed = 0;
	p->v.msg[0] = 0;
	p->all = true;
	mj_play_status(p);
}

static bool load_layout(mj_play_t *p, int i)
{
	static char buf[4096];
	char err[48];
	const char *text;

	if (i < 0 || i >= p->nlayouts || !p->src) return false;
	text = p->src(p->src_user, i, buf, sizeof(buf));
	if (!text) {
		char m[MJ_TEXT_LEN];
		scpy(m, sizeof(m), "Cannot read ");
		scat(m, sizeof(m), p->lname[i]);
		msg(p, m, 6);
		return false;
	}
	if (!mj_layout_parse(text, &p->L, err, sizeof(err))) {
		char m[MJ_TEXT_LEN];
		scpy(m, sizeof(m), p->lname[i]);
		scat(m, sizeof(m), ": ");
		scat(m, sizeof(m), err);
		msg(p, m, 8);
		return false;
	}
	p->layout = i;
	return true;
}

bool mj_play_layout(mj_play_t *p, int i, uint32_t seed)
{
	int old = p->layout;
	bool had = p->g.L != 0;

	if (!load_layout(p, i)) {
		/* the parse wrote over the layout: put the old one back */
		if (had) {
			char keep[MJ_TEXT_LEN];
			scpy(keep, sizeof(keep), p->v.msg);
			load_layout(p, old);
			mj_play_new(p, seed);
			msg(p, keep, 8);
		}
		p->relayout = p->all = true;
		return false;
	}
	p->g.L = &p->L;
	mj_play_new(p, seed);
	p->relayout = p->all = true;
	p->save = true;
	return true;
}

/* -- play ------------------------------------------------------------------------ */

static uint8_t free_before[MJ_MAX], free_after[MJ_MAX];

/* What a change to the table repaints: the tiles that changed, and --
 * when blocked tiles are dimmed -- every tile whose freedom changed. */
static void before_change(mj_play_t *p)
{
	if (p->v.show_free) mj_free_flags(&p->g, free_before);
}

static void after_change(mj_play_t *p, int a, int b)
{
	int t;
	dirty_tile(p, a);
	dirty_tile(p, b);
	if (p->v.show_free) {
		mj_free_flags(&p->g, free_after);
		for (t = 0; t < p->L.n; t++)
			if (free_before[t] != free_after[t]) dirty_tile(p, t);
	}
	mj_play_status(p);
}

static void set_sel(mj_play_t *p, int t)
{
	if (p->v.sel == t) return;
	dirty_tile(p, p->v.sel);
	p->v.sel = t;
	dirty_tile(p, t);
}

static void clear_hint(mj_play_t *p)
{
	dirty_tile(p, p->v.hint_a);
	dirty_tile(p, p->v.hint_b);
	p->v.hint_a = p->v.hint_b = -1;
}

static void set_cursor(mj_play_t *p, int t)
{
	if (p->v.cursor == t) return;
	dirty_tile(p, p->v.cursor);
	p->v.cursor = t;
	dirty_tile(p, t);
	if (t >= 0 && mj_view_reveal(&p->v, t)) p->all = true;
}

static void won(mj_play_t *p)
{
	mj_stat_t *s = stat_for(p, p->L.name, true);
	char m[MJ_TEXT_LEN], t[16];
	bool best = false;

	p->timing = false;
	if (s) {
		s->won++;
		if (!s->best || p->elapsed < s->best) { s->best = p->elapsed; best = true; }
	}
	p->save = true;
	mj_play_time(p->elapsed, t, sizeof(t));
	scpy(m, sizeof(m), "Cleared in ");
	scat(m, sizeof(m), t);
	if (best) scat(m, sizeof(m), " -- your best!");
	else if (s && s->best) {
		mj_play_time(s->best, t, sizeof(t));
		scat(m, sizeof(m), " (best ");
		scat(m, sizeof(m), t);
		scat(m, sizeof(m), ")");
	}
	scat(m, sizeof(m), "  N: new deal");
	msg(p, m, 0x7fffffff);
	p->sound = MJ_SND_WIN;
}

static void take(mj_play_t *p, int a, int b)
{
	before_change(p);
	if (!mj_take(&p->g, a, b)) return;
	clear_hint(p);
	p->v.sel = -1;
	p->burst[0] = a;
	p->burst[1] = b;
	if (!p->timing && p->g.left) {
		p->timing = true;
		p->t0 = p->now - p->elapsed;
	}
	if (!p->counted) {
		mj_stat_t *s = stat_for(p, p->L.name, true);
		p->counted = true;
		if (s) s->played++;
		p->save = true;
	}
	after_change(p, a, b);
	p->sound = MJ_SND_TAKE;
	if (p->v.cursor == a || p->v.cursor == b) {
		p->v.cursor = -1;
		if (p->keyboard) set_cursor(p, mj_nav_first(&p->v));
	}

	switch (mj_state(&p->g)) {
	case MJ_WON:
		won(p);
		break;
	case MJ_STUCK:
		msg(p, "No pairs left: Shuffle, or Undo", 10);
		break;
	default:
		if (p->v.msg[0]) { p->v.msg[0] = 0; p->bar = true; }
		break;
	}
}

static void pick(mj_play_t *p, int t)
{
	char name[32];

	if (t < 0) {
		set_sel(p, -1);
		clear_hint(p);
		return;
	}
	if (!mj_free(&p->g, t)) {
		mj_face_name(p->g.face[t], name, sizeof(name));
		{
			char m[MJ_TEXT_LEN];
			scpy(m, sizeof(m), "The ");
			scat(m, sizeof(m), name);
			scat(m, sizeof(m), " is not free");
			msg(p, m, 3);
		}
		p->sound = MJ_SND_NOPE;
		return;
	}
	if (p->v.sel == t) {
		set_sel(p, -1);
		say(p, "put back");
		return;
	}
	if (p->v.sel >= 0 && mj_match(p->g.face[p->v.sel], p->g.face[t])) {
		take(p, p->v.sel, t);
		return;
	}
	if (p->v.sel >= 0) p->sound = MJ_SND_NOPE;
	else p->sound = MJ_SND_PICK;
	set_sel(p, t);
	mj_face_name(p->g.face[t], name, sizeof(name));
	say(p, name);
}

static void undo(mj_play_t *p)
{
	mj_move_t m;
	if (!p->g.nhist) {
		msg(p, p->g.shuffles ? "Nothing to undo since the shuffle" : "Nothing to undo", 3);
		return;
	}
	m = p->g.hist[p->g.nhist - 1];
	before_change(p);
	clear_hint(p);
	set_sel(p, -1);
	mj_undo(&p->g);
	if (p->g.left && !p->timing && p->counted) {
		/* undone out of a win: the clock runs on from where it stopped */
		p->timing = true;
		p->t0 = p->now - p->elapsed;
	}
	after_change(p, m.a, m.b);
	if (p->v.msg[0]) { p->v.msg[0] = 0; p->bar = true; }
	say(p, "undone");
}

static void redo(mj_play_t *p)
{
	mj_move_t m;
	if (p->g.nhist >= p->g.ntop) {
		msg(p, "Nothing to redo", 3);
		return;
	}
	m = p->g.hist[p->g.nhist];
	clear_hint(p);
	set_sel(p, -1);
	/* exactly as if taken: the clock, the win */
	{
		int top = p->g.ntop;
		take(p, m.a, m.b);
		p->g.ntop = top;
	}
}

static void hint(mj_play_t *p)
{
	int a, b;
	char m[MJ_TEXT_LEN], name[32];
	clear_hint(p);
	if (!mj_hint(&p->g, &a, &b)) {
		msg(p, p->g.left ? "No pairs left: Shuffle, or Undo" : "Nothing left to find", 6);
		return;
	}
	p->v.hint_a = a;
	p->v.hint_b = b;
	dirty_tile(p, a);
	dirty_tile(p, b);
	if (mj_view_reveal(&p->v, a)) p->all = true;
	mj_face_name(p->g.face[a], name, sizeof(name));
	scpy(m, sizeof(m), "Hint: ");
	scat(m, sizeof(m), name);
	msg(p, m, 3);
}

static void shuffle(mj_play_t *p)
{
	if (!p->g.left) return;
	clear_marks(p);
	if (!mj_shuffle(&p->g)) {
		msg(p, "No shuffle can help: Undo, or N for a new deal", 8);
		p->sound = MJ_SND_NOPE;
		return;
	}
	if (p->keyboard) p->v.cursor = mj_nav_first(&p->v);
	p->all = true;
	p->sound = MJ_SND_SHUFFLE;
	mj_play_status(p);
	msg(p, "Shuffled", 3);
}

void mj_play_button(mj_play_t *p, int b, uint32_t seed)
{
	switch (b) {
	case MJ_B_NEW:     mj_play_new(p, seed); say(p, "new deal"); break;
	case MJ_B_UNDO:    undo(p); break;
	case MJ_B_HINT:    hint(p); break;
	case MJ_B_SHUFFLE: shuffle(p); break;
	case MJ_B_ZOOM:
		if (p->can_zoom) {
			p->zoom = !p->zoom;
			p->relayout = p->all = true;
			p->save = true;
			say(p, p->zoom ? "close up" : "whole table");
		}
		break;
	default: break;
	}
}

void mj_play_click(mj_play_t *p, int x, int y, uint32_t seed)
{
	int b = mj_bar_hit(&p->v, x, y);
	if (b >= 0) {
		mj_play_button(p, b, seed);
		return;
	}
	if (y >= p->v.bar_y) return;
	p->keyboard = false;
	set_cursor(p, -1);
	pick(p, mj_hit(&p->v, x, y));
}

void mj_play_key(mj_play_t *p, uint32_t sym, int mods, uint32_t seed)
{
	bool ctrl = (mods & 0x11) != 0;     /* Z_KBD_MOD_CTRL */
	bool shift = (mods & 0x22) != 0;    /* Z_KBD_MOD_SHIFT */
	int dir = -1;

	if (sym >= 'A' && sym <= 'Z') { sym += 'a' - 'A'; shift = true; }

	switch (sym) {
	case 0x110002: dir = 0; break;      /* Z_KEY_LEFT */
	case 0x110003: dir = 1; break;      /* Z_KEY_RIGHT */
	case 0x110000: dir = 2; break;      /* Z_KEY_UP */
	case 0x110001: dir = 3; break;      /* Z_KEY_DOWN */
	default: break;
	}
	if (dir >= 0) {
		int t;
		p->keyboard = true;
		t = p->v.cursor < 0 ? mj_nav_first(&p->v) : mj_nav(&p->v, p->v.cursor, dir);
		if (t >= 0) {
			char name[40];
			set_cursor(p, t);
			mj_face_name(p->g.face[t], name, sizeof(name));
			if (!mj_free(&p->g, t)) scat(name, sizeof(name), ", blocked");
			say(p, name);
		}
		return;
	}

	if (sym == '\t') {
		int t = mj_nav_tab(&p->v, p->v.cursor, shift ? -1 : 1);
		p->keyboard = true;
		if (t >= 0) {
			char name[40];
			set_cursor(p, t);
			mj_face_name(p->g.face[t], name, sizeof(name));
			say(p, name);
		} else {
			msg(p, "No tile is free", 3);
		}
		return;
	}

	switch (sym) {
	case '\r': case '\n': case ' ':
		p->keyboard = true;
		if (p->v.cursor < 0) set_cursor(p, mj_nav_first(&p->v));
		else pick(p, p->v.cursor);
		break;
	case 0x1b:
		if (p->v.sel >= 0 || p->v.hint_a >= 0) pick(p, -1);
		break;
	case 'h': hint(p); break;
	case 8: case 0x7f: case 'u': undo(p); break;
	case 'z':
		if (ctrl) undo(p);
		else if (p->can_zoom) mj_play_button(p, MJ_B_ZOOM, seed);
		else msg(p, "The window's size sets the tiles' size", 4);
		break;
	case 'r': case 'y': redo(p); break;
	case 's': shuffle(p); break;
	case 'n': mj_play_new(p, seed); say(p, "new deal"); break;
	case 'a':
		mj_restart(&p->g);
		clear_marks(p);
		if (p->keyboard) p->v.cursor = mj_nav_first(&p->v);
		p->timing = false;
		p->elapsed = 0;
		p->v.msg[0] = 0;
		p->all = true;
		mj_play_status(p);
		msg(p, "The same deal again", 3);
		break;
	case 'l':
		if (p->nlayouts > 1) {
			int i = (p->layout + (shift ? p->nlayouts - 1 : 1)) % p->nlayouts;
			if (mj_play_layout(p, i, seed)) {
				char m[MJ_TEXT_LEN];
				scpy(m, sizeof(m), "Layout: ");
				scat(m, sizeof(m), p->L.name);
				msg(p, m, 3);
			}
		}
		break;
	case 'd':
		p->random = !p->random;
		p->save = true;
		msg(p, p->random ? "Next deals: random, maybe unwinnable"
				 : "Next deals: always winnable", 4);
		break;
	case 'f':
		p->v.show_free = !p->v.show_free;
		p->all = true;
		p->save = true;
		msg(p, p->v.show_free ? "Blocked tiles dimmed" : "Blocked tiles not dimmed", 3);
		break;
	case 'v': {
		static const char *const from[4] = {
			"Seen from the front right", "Seen from the front left",
			"Seen from the back left", "Seen from the back right"
		};
		p->v.corner = (p->v.corner + (shift ? 3 : 1)) & 3;
		p->relayout = p->all = true;
		p->save = true;
		msg(p, from[p->v.corner], 3);
		break;
	}
	default:
		break;
	}
}

/* A scroll and nothing else: reported as how far, so the face can move
 * the picture and draw the strip that came into view. With other
 * changes in the same breath it is simplest, and right, to draw it
 * all. */
static void scrolled(mj_play_t *p, int dx, int dy)
{
	int ox = p->v.scroll_x, oy = p->v.scroll_y;
	if (!p->v.scrolls || !mj_view_scroll(&p->v, dx, dy)) return;
	if (p->nrect || p->bar) { p->all = true; return; }
	p->sdx += p->v.scroll_x - ox;
	p->sdy += p->v.scroll_y - oy;
}

void mj_play_wheel(mj_play_t *p, int notches, bool across)
{
	int ox = p->v.scroll_x, oy = p->v.scroll_y;
	if (!across) {
		scrolled(p, 0, notches * 24);
		/* nothing to scroll down: the board is only too wide */
		if (p->v.scroll_y != oy || p->v.scroll_x != ox || p->all) return;
		if (p->v.bh + 4 > p->v.area_h) return;     /* too tall, at the end */
	}
	scrolled(p, notches * 24, 0);
}

void mj_play_hover(mj_play_t *p, int x, int y)
{
	const mj_view_t *v = &p->v;
	int dx = 0, dy = 0, e = 6;
	if (!v->scrolls) return;
	if (y < v->area_y || y >= v->area_y + v->area_h || x < 0 || x >= v->cw) return;
	if (x < v->area_x + e) dx = -4;
	else if (x >= v->area_x + v->area_w - e) dx = 4;
	if (y < v->area_y + e) dy = -4;
	else if (y >= v->area_y + v->area_h - e) dy = 4;
	if (dx || dy) scrolled(p, dx, dy);
}

void mj_play_tick(mj_play_t *p, uint32_t now_s)
{
	p->now = now_s;
	if (p->timing) {
		uint32_t e = now_s - p->t0;
		if (e != p->elapsed) {
			p->elapsed = e;
			mj_play_status(p);
		}
	}
	if (p->v.msg[0] && (int32_t)(now_s - p->msg_until) >= 0) {
		p->v.msg[0] = 0;
		p->bar = true;
	}
}

/* -- stats.txt ------------------------------------------------------------------- */

static uint32_t num(const char **s)
{
	uint32_t v = 0;
	while (**s == ' ') (*s)++;
	while (**s >= '0' && **s <= '9') v = v * 10 + (uint32_t)(*(*s)++ - '0');
	return v;
}

static bool word(const char *s, int len, const char *w, const char **rest)
{
	int n = (int)strlen(w);
	if (len < n || memcmp(s, w, (size_t)n) || (len > n && s[n] != ' ')) return false;
	*rest = s + n;
	while (**rest == ' ') (*rest)++;
	return true;
}

void mj_play_load_stats(mj_play_t *p, const char *text)
{
	const char *s = text;
	while (s && *s) {
		const char *e = s, *r;
		char line[96];
		int len;
		while (*e && *e != '\n') e++;
		len = (int)(e - s);
		if (len && s[len - 1] == '\r') len--;
		if (len > (int)sizeof(line) - 1) len = (int)sizeof(line) - 1;
		memcpy(line, s, (size_t)len);
		line[len] = 0;

		if (word(line, len, "stat", &r)) {
			uint32_t pl = num(&r), wo = num(&r), be = num(&r);
			mj_stat_t *st;
			while (*r == ' ') r++;
			if (*r && (st = stat_for(p, r, true)) != 0) {
				st->played = pl;
				st->won = wo;
				st->best = be;
			}
		} else if (word(line, len, "layout", &r)) {
			scpy(p->want, MJ_NAME, r);
		} else if (word(line, len, "random", &r)) {
			p->random = num(&r) != 0;
		} else if (word(line, len, "free", &r)) {
			p->v.show_free = num(&r) != 0;
		} else if (word(line, len, "corner", &r)) {
			p->v.corner = (int)(num(&r) & 3);
		} else if (word(line, len, "zoom", &r)) {
			p->zoom = num(&r) ? 1 : 0;
		}
		s = *e ? e + 1 : e;
	}
}

int mj_play_save_stats(const mj_play_t *p, char *out, int cap)
{
	int i;
	if (cap <= 0) return 0;
	out[0] = 0;
	scat(out, cap, "# mahjong: settings, and per layout: played won best-seconds name\n");
	scat(out, cap, "layout ");
	scat(out, cap, p->L.name[0] ? p->L.name : p->want);
	scat(out, cap, "\nrandom ");
	ucat(out, cap, p->random);
	scat(out, cap, "\nfree ");
	ucat(out, cap, p->v.show_free);
	scat(out, cap, "\ncorner ");
	ucat(out, cap, (uint32_t)p->v.corner);
	scat(out, cap, "\nzoom ");
	ucat(out, cap, (uint32_t)p->zoom);
	scat(out, cap, "\n");
	for (i = 0; i < p->nstats; i++) {
		scat(out, cap, "stat ");
		ucat(out, cap, p->stats[i].played);
		scat(out, cap, " ");
		ucat(out, cap, p->stats[i].won);
		scat(out, cap, " ");
		ucat(out, cap, p->stats[i].best);
		scat(out, cap, " ");
		scat(out, cap, p->stats[i].name);
		scat(out, cap, "\n");
	}
	return (int)strlen(out);
}

const char *mj_play_help(void)
{
	/* app_help.md's rules: a key, a tab, what it does; the same text
	 * in the desktop's box and on the screen (screen_ui.md, "Help") */
	return
	"Mahjong solitaire: clear the table two tiles at a time. "
	"Take two with the same picture that are both free: nothing "
	"on them, and the left or the right side open. Any flower goes "
	"with any flower, any season with any season. Every deal can "
	"be won, unless D has made it a random one.\n"
	"\n"
	"Click\tpick up a tile, then click its match\n"
	"Arrows\tmove between the tiles on top\n"
	"Tab\tthe next free tile; shift-Tab the one before\n"
	"Enter\tpick up, or take the pair\n"
	"Esc\tput it back\n"
	"H\thint; again for the next pair\n"
	"U\tundo\n"
	"R\tredo\n"
	"S\tshuffle what is left\n"
	"N\tnew deal\n"
	"A\tthe same deal again\n"
	"L\tnext layout; shift-L the one before\n"
	"V\tsee the table from another corner\n"
	"F\tdim the tiles that are not free\n"
	"D\trandom deals on or off\n"
	"Z\tzoom, on the screen\n"
	"Wheel\tscroll a board too big to show whole; with Shift, Ctrl or Alt, across\n"
	"M\tmusic on or off, on the screen\n"
	"C\tfull screen: black and white, or colour\n"
	"Gamepad\tstick moves, A picks up, B puts back, X hint, "
	"Y undo, Start shuffle, Select new deal\n"
	"F10\tcolour: the table in colour, and back\n"
	"F11\tfull screen\n"
	"\n"
	"Layouts of your own go in the layouts folder of mahjong's "
	"data; Read docs shows how to draw one.";
}
