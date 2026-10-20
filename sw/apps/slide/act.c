/*
 * slide -- actions and the player. See act.h.
 */

#include "act.h"
#include "render.h"
#include "../../common/zpool.h"

#include <string.h>

static act_host_t host;

void act_init(const act_host_t *h) { host = *h; }

const act_host_t *act_host(void) { return &host; }

// -- reading forms -----------------------------------------------------------
//
// Just enough of Scheme's reader for the verbs: a list of atoms,
// numbers and strings; 'x is x.

enum { A_NONE = 0, A_ATOM, A_NUM, A_STR };

#define ARGS_MAX 4
#define ARG_LEN 160

typedef struct {
	int		kind;
	long	num;
	char	s[ARG_LEN];
} arg_t;

typedef struct {
	char	verb[24];
	arg_t	a[ARGS_MAX];
	int		n;
} form_t;

static const char *skip_ws(const char *p) {
	for (;;) {
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
		if (*p == ';') { while (*p && *p != '\n') p++; continue; }
		return p;
	}
}

// One form from p: false at the end, or when it is not one this reads
// (*bad then). *next is where the next form starts.
static bool read_form(const char *p, form_t *f, const char **next, bool *bad) {
	int k;
	*bad = false;
	memset(f, 0, sizeof(*f));
	p = skip_ws(p);
	if (!*p) return false;
	if (*p != '(') { *bad = true; return false; }
	p = skip_ws(p + 1);
	for (k = 0; *p && *p != ')' && *p != ' ' && *p != '(' && *p != '"' && k < (int)sizeof(f->verb) - 1; p++)
		f->verb[k++] = *p;
	for (;;) {
		arg_t *a;
		p = skip_ws(p);
		if (*p == ')') { *next = p + 1; return f->verb[0] != 0; }
		if (!*p || *p == '(') { *bad = true; return false; }
		if (f->n == ARGS_MAX) { *bad = true; return false; }
		a = &f->a[f->n++];
		if (*p == '\'') p++;
		if (*p == '"') {
			p++;
			for (k = 0; *p && *p != '"'; p++) {
				char c = *p;
				if (c == '\\' && p[1]) { p++; c = *p == 'n' ? '\n' : *p; }
				if (k < ARG_LEN - 1) a->s[k++] = c;
			}
			if (*p != '"') { *bad = true; return false; }
			p++;
			a->kind = A_STR;
		} else {
			bool num = true;
			for (k = 0; *p && *p != ')' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '('; p++) {
				if (!((*p >= '0' && *p <= '9') || (k == 0 && *p == '-'))) num = false;
				if (k < ARG_LEN - 1) a->s[k++] = *p;
			}
			a->kind = num && k && !(k == 1 && a->s[0] == '-') ? A_NUM : A_ATOM;
			if (a->kind == A_NUM) {
				const char *q = a->s;
				bool neg = *q == '-';
				if (neg) q++;
				while (*q) a->num = a->num * 10 + (*q++ - '0');
				if (neg) a->num = -a->num;
			}
		}
	}
}

// -- the verbs ----------------------------------------------------------------

#define HIST_MAX 32
static int hist[HIST_MAX], nhist;
static bool going_back;

static char *pending;			// forms after a (wait)
static uint32_t pending_at;
static deck_t *pending_deck;
static int pending_cur;

void act_reset(void) {
	pool_free(pending);
	pending = NULL;
	nhist = 0;
}

bool act_waiting(void) { return pending != NULL; }

obj_t *act_find(deck_t *d, int cur, const char *name) {
	obj_t *o;
	card_t *c;
	if (!d || cur < 0 || cur >= d->ncard) return NULL;
	c = d->card[cur];
	o = card_find_obj(c, name);
	if (!o && c->bg >= 0 && c->bg < d->nbg) o = card_find_obj(d->bg[c->bg], name);
	return o;
}

static void say(const char *a, const char *b) {
	char m[96];
	int k = 0, i;
	for (i = 0; a[i] && k < (int)sizeof(m) - 1; i++) m[k++] = a[i];
	for (i = 0; b && b[i] && k < (int)sizeof(m) - 1; i++) m[k++] = b[i];
	m[k] = 0;
	if (host.message) host.message(host.u, m);
}

static int resolve_card(deck_t *d, int cur, const arg_t *a) {
	if (a->kind == A_NUM) return (int)a->num - 1;
	if (a->kind == A_STR) return deck_find_card(d, a->s);
	if (!strcmp(a->s, "next")) return cur + 1 < d->ncard ? cur + 1 : d->loop ? 0 : cur;
	if (!strcmp(a->s, "prev") || !strcmp(a->s, "previous"))
		return cur > 0 ? cur - 1 : d->loop ? d->ncard - 1 : cur;
	if (!strcmp(a->s, "first")) return 0;
	if (!strcmp(a->s, "last")) return d->ncard - 1;
	if (!strcmp(a->s, "back")) return nhist >= 2 ? hist[nhist - 2] : cur;
	return -1;
}

// Runs forms until a (wait) or the end. A card whose own action goes to
// a card whose own action goes ... stops after a few.
static int depth;

static void run_forms(deck_t *d, int cur, const char *p);

static void run_from(deck_t *d, int cur, const char *p) {
	if (depth >= 8) { say("actions going round in circles: stopped", NULL); return; }
	depth++;
	run_forms(d, cur, p);
	depth--;
}

static void run_forms(deck_t *d, int cur, const char *p) {
	form_t f;
	const char *next;
	bool bad;
	while (read_form(p, &f, &next, &bad)) {
		const char *v = f.verb;
		obj_t *o = NULL;
		p = next;
		if ((!strcmp(v, "show") || !strcmp(v, "hide") || !strcmp(v, "toggle") ||
			!strcmp(v, "set-text") || !strcmp(v, "set-value") || !strcmp(v, "speak-text")) &&
			f.n >= 1) {
			o = act_find(d, cur, f.a[0].s);
			if (!o) { say("no object called ", f.a[0].s); continue; }
		}
		if (!strcmp(v, "go") && f.n == 1) {
			int to = resolve_card(d, cur, &f.a[0]);
			if (to < 0 || to >= d->ncard) { say("no such card: ", f.a[0].s); continue; }
			if (!strcmp(f.a[0].s, "back") && f.a[0].kind == A_ATOM && nhist >= 2) {
				nhist--;					// the card left is not gone back to again
				going_back = true;
			}
			if (to != cur && host.go) {
				cur = to;
				host.go(host.u, to);
				// the card's own action ran (act_arrived); this goes on
				// with what follows the (go ...)
			}
		} else if (!strcmp(v, "show") && o) {
			o->flags &= (uint8_t)~OBF_HIDDEN;
			if (host.changed) host.changed(host.u);
		} else if (!strcmp(v, "hide") && o) {
			o->flags |= OBF_HIDDEN;
			if (host.changed) host.changed(host.u);
		} else if (!strcmp(v, "toggle") && o) {
			o->flags ^= OBF_HIDDEN;
			if (host.changed) host.changed(host.u);
		} else if (!strcmp(v, "set-text") && o && f.n == 2) {
			char num[16];
			const char *t = f.a[1].s;
			if (f.a[1].kind == A_NUM) {
				// the number as written
				int k = 0;
				const char *q = f.a[1].s;
				while (*q && k < 15) num[k++] = *q++;
				num[k] = 0;
				t = num;
			}
			obj_set_text(o, t, (uint32_t)strlen(t));
			if (host.changed) host.changed(host.u);
		} else if (!strcmp(v, "set-value") && o && f.n == 2 && f.a[1].kind == A_NUM) {
			o->value = (int16_t)f.a[1].num;
			if (o->kind == OB_SLIDER) {
				if (o->value < o->vmin) o->value = o->vmin;
				if (o->value > o->vmax) o->value = o->vmax;
			}
			if (host.changed) host.changed(host.u);
		} else if (!strcmp(v, "speak") && f.n == 1) {
			if (host.speak) host.speak(host.u, f.a[0].s);
		} else if (!strcmp(v, "speak-text") && o) {
			if (host.speak && o->text) host.speak(host.u, o->text);
		} else if (!strcmp(v, "run") && f.n == 1 && f.a[0].s[0]) {
			if (host.run) host.run(host.u, f.a[0].s);
		} else if (!strcmp(v, "open") && f.n == 1 && f.a[0].s[0]) {
			if (host.open_deck) {
				char path[ARG_LEN];
				memcpy(path, f.a[0].s, sizeof(path));
				host.open_deck(host.u, path);
			}
			return;						// another deck: nothing more of this one
		} else if (!strcmp(v, "wait") && f.n == 1 && f.a[0].kind == A_NUM) {
			uint32_t ms = f.a[0].num > 0 ? (uint32_t)f.a[0].num : 0;
			pool_free(pending);
			pending = pool_strdup(p);
			pending_deck = d;
			pending_cur = cur;
			pending_at = (host.now ? host.now(host.u) : 0) + ms * (host.tick_hz ? host.tick_hz : 1000) / 1000;
			if (!pending) say("out of memory", NULL);
			return;
		} else if (!strcmp(v, "beep")) {
			if (host.speak) host.speak(host.u, "beep");
		} else {
			say("needs scripting (not yet): ", v);
		}
	}
	if (bad) say("an action slide cannot read: ", p);
}

static bool is_verb(const char *v) {
	static const char *const verbs[] = { "go", "show", "hide", "toggle", "set-text", "set-value",
		"speak", "speak-text", "run", "open", "wait", "beep", NULL };
	int i;
	for (i = 0; verbs[i]; i++) if (!strcmp(v, verbs[i])) return true;
	return false;
}

bool act_simple(const char *forms) {
	form_t f;
	const char *p = forms, *next;
	bool bad;
	while (read_form(p, &f, &next, &bad)) {
		if (!is_verb(f.verb)) return false;
		p = next;
	}
	return !bad;
}

void act_run(deck_t *d, int cur, const char *forms) {
	pool_free(pending);
	pending = NULL;
	if (!forms || !*forms || !d) return;
	if (host.eval && !act_simple(forms)) {
		// more than the verbs: Scheme (Phase 4)
		host.eval(host.u, d, cur, forms);
		return;
	}
	run_from(d, cur, forms);
}

bool act_do(deck_t *d, int cur, const char *forms) {
	// no (wait) from Scheme: it has (after ...)
	if (!act_simple(forms) || strstr(forms, "(wait")) return false;
	run_from(d, cur, forms);
	return true;
}

void act_arrived(deck_t *d, int to) {
	if (!going_back) {
		if (nhist == HIST_MAX) { memmove(hist, hist + 1, sizeof(hist) - sizeof(hist[0])); nhist--; }
		if (!nhist || hist[nhist - 1] != to) hist[nhist++] = to;
	}
	going_back = false;
	if (d && to >= 0 && to < d->ncard && d->card[to]->action) {
		// the card's own action; whatever was waiting is the old card's
		char *keep = pool_strdup(d->card[to]->action);
		pool_free(pending);
		pending = NULL;
		if (keep) {
			if (host.eval && !act_simple(keep)) host.eval(host.u, d, to, keep);
			else run_from(d, to, keep);
			pool_free(keep);
		}
	}
}

void act_tick(void) {
	char *p;
	if (!pending) return;
	if (host.now && (int32_t)(host.now(host.u) - pending_at) < 0) return;
	p = pending;
	pending = NULL;
	run_from(pending_deck, pending_cur, p);
	pool_free(p);
}

// -- plain words to forms ----------------------------------------------------

typedef struct { char *p; int n, cap; bool full; } ob_t;

static void ob_put(ob_t *b, const char *s, int n) {
	int i;
	if (n < 0) n = (int)strlen(s);
	for (i = 0; i < n; i++) {
		if (b->n >= b->cap - 1) { b->full = true; break; }
		b->p[b->n++] = s[i];
	}
	b->p[b->n] = 0;
}

static void ob_str(ob_t *b, const char *s, int n) {
	int i;
	ob_put(b, "\"", 1);
	for (i = 0; i < n; i++) {
		if (s[i] == '"' || s[i] == '\\') ob_put(b, "\\", 1);
		ob_put(b, s + i, 1);
	}
	ob_put(b, "\"", 1);
}

static bool all_digits(const char *s, int n) {
	int i = (n > 0 && s[0] == '-') ? 1 : 0;
	if (i >= n) return false;
	for (; i < n; i++) if (s[i] < '0' || s[i] > '9') return false;
	return true;
}

bool act_from_words(const char *in, char *out, int cap) {
	ob_t b = { out, 0, cap, false };
	const char *p = in;
	out[0] = 0;
	while (*p == ' ') p++;
	if (*p == '(') { ob_put(&b, p, -1); return !b.full; }
	while (*p) {
		const char *v, *e, *a, *ae;
		int vl;
		while (*p == ' ' || *p == ';') p++;
		if (!*p) break;
		// one step: up to ';'
		e = p;
		while (*e && *e != ';') e++;
		ae = e;
		while (ae > p && ae[-1] == ' ') ae--;
		v = p;
		while (p < ae && *p != ' ') p++;
		vl = (int)(p - v);
		while (p < ae && *p == ' ') p++;
		a = p;							// the rest of the step: a .. ae
		if (b.n) ob_put(&b, " ", 1);
		ob_put(&b, "(", 1);
		ob_put(&b, v, vl);
		if (vl == 2 && !strncmp(v, "go", 2)) {
			static const char *const named[] = { "next", "prev", "previous", "first", "last", "back", NULL };
			int i, al = (int)(ae - a);
			bool done = false;
			ob_put(&b, " ", 1);
			for (i = 0; named[i]; i++)
				if (al == (int)strlen(named[i]) && !strncmp(a, named[i], (size_t)al)) {
					ob_put(&b, "'", 1);
					ob_put(&b, a, al);
					done = true;
				}
			if (!done) {
				if (all_digits(a, al)) ob_put(&b, a, al);
				else ob_str(&b, a, al);
			}
		} else if ((vl == 4 && !strncmp(v, "show", 4)) || (vl == 4 && !strncmp(v, "hide", 4)) ||
			(vl == 6 && !strncmp(v, "toggle", 6)) || (vl == 10 && !strncmp(v, "speak-text", 10)) ||
			(vl == 5 && !strncmp(v, "speak", 5)) || (vl == 3 && !strncmp(v, "run", 3)) ||
			(vl == 4 && !strncmp(v, "open", 4))) {
			ob_put(&b, " ", 1);
			ob_str(&b, a, (int)(ae - a));
		} else if ((vl == 8 && !strncmp(v, "set-text", 8)) || (vl == 9 && !strncmp(v, "set-value", 9))) {
			// a name, then the rest
			const char *n = a, *ne;
			while (a < ae && *a != ' ') a++;
			ne = a;
			while (a < ae && *a == ' ') a++;
			ob_put(&b, " ", 1);
			ob_str(&b, n, (int)(ne - n));
			ob_put(&b, " ", 1);
			if (v[4] == 'v' && all_digits(a, (int)(ae - a))) ob_put(&b, a, (int)(ae - a));
			else ob_str(&b, a, (int)(ae - a));
		} else if (vl == 4 && !strncmp(v, "wait", 4)) {
			if (!all_digits(a, (int)(ae - a))) {
				b.n = 0;
				ob_put(&b, "wait MS: a number of milliseconds", -1);
				return false;
			}
			ob_put(&b, " ", 1);
			ob_put(&b, a, (int)(ae - a));
		} else if (vl == 4 && !strncmp(v, "beep", 4)) {
			// nothing more
		} else {
			char verb[24];
			int k = vl < 23 ? vl : 23;
			memcpy(verb, v, (size_t)k);
			verb[k] = 0;
			b.n = 0;
			ob_put(&b, "Not an action: ", -1);
			ob_put(&b, verb, -1);
			ob_put(&b, " (go show hide toggle set-text set-value speak speak-text run open wait)", -1);
			return false;
		}
		ob_put(&b, ")", 1);
		p = e;
	}
	return !b.full;
}

// -- the player ---------------------------------------------------------------

static bool inside(const obj_t *o, int x, int y) {
	int x0, y0, x1, y1;
	obj_bounds(o, &x0, &y0, &x1, &y1);
	return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

obj_t *player_hit(player_t *p, int x, int y) {
	card_t *c;
	int i;
	if (!p->d || p->cur < 0 || p->cur >= p->d->ncard) return NULL;
	c = p->d->card[p->cur];
	for (i = c->nobj - 1; i >= 0; i--) {
		obj_t *o = c->obj[i];
		if ((o->flags & OBF_HIDDEN) || !OB_IS_CONTROL(o->kind)) continue;
		if (inside(o, x, y)) return o;
	}
	if (c->bg >= 0 && c->bg < p->d->nbg) {
		card_t *b = p->d->bg[c->bg];
		for (i = b->nobj - 1; i >= 0; i--) {
			obj_t *o = b->obj[i];
			if ((o->flags & OBF_HIDDEN) || !OB_IS_CONTROL(o->kind)) continue;
			if (inside(o, x, y)) return o;
		}
	}
	return NULL;
}

void player_reset(player_t *p) {
	p->pressed = p->focus = p->drag = NULL;
	p->inside = false;
	render_ui_state(NULL, NULL);
}

static void ui(player_t *p) {
	render_ui_state(p->pressed && p->inside ? p->pressed : NULL, p->focus);
}

// The radios of o's group on its card (and background) off, o on.
static void radio_set(player_t *p, obj_t *o) {
	card_t *cs[2];
	int k, i, n = 1;
	card_t *c = p->d->card[p->cur];
	cs[0] = c;
	if (c->bg >= 0 && c->bg < p->d->nbg) cs[n++] = p->d->bg[c->bg];
	for (k = 0; k < n; k++)
		for (i = 0; i < cs[k]->nobj; i++) {
			obj_t *r = cs[k]->obj[i];
			if (r->kind == OB_RADIO && !strcmp(r->group, o->group)) r->value = 0;
		}
	o->value = 1;
}

static void fire(player_t *p, obj_t *o) {
	if (host.clicked) host.clicked(host.u, p->d, p->cur, o);
	if (o->action) act_run(p->d, p->cur, o->action);
}

bool player_press(player_t *p, int x, int y, bool *took) {
	obj_t *o = player_hit(p, x, y);
	bool redraw = false;
	*took = o != NULL;
	if (p->focus && p->focus != o) { p->focus = NULL; redraw = true; }
	if (!o) { ui(p); return redraw; }
	switch (o->kind) {
	case OB_BUTTON: case OB_CHECK: case OB_RADIO:
		p->pressed = o;
		p->inside = true;
		redraw = true;
		break;
	case OB_SLIDER: {
		int v = render_slider_value(o, x);
		p->drag = o;
		if (v != o->value) { o->value = (int16_t)v; redraw = true; }
		p->pressed = o;
		p->inside = true;
		redraw = true;
		break;
	}
	case OB_LIST: {
		int rh = render_list_row_h(p->d, o), i;
		i = (y - o->y - o->pad) / (rh > 0 ? rh : 1);
		if (y - o->y - o->pad >= 0 && i < render_list_items(o) && i != o->value) {
			o->value = (int16_t)i;
			ui(p);
			fire(p, o);
			return true;
		}
		break;
	}
	case OB_FIELD:
		if (p->focus != o) { p->focus = o; redraw = true; }
		break;
	}
	ui(p);
	return redraw;
}

bool player_motion(player_t *p, int x, int y) {
	if (p->drag) {
		int v = render_slider_value(p->drag, x);
		if (v != p->drag->value) { p->drag->value = (int16_t)v; return true; }
		return false;
	}
	if (p->pressed) {
		bool in = inside(p->pressed, x, y);
		if (in != p->inside) { p->inside = in; ui(p); return true; }
	}
	return false;
}

bool player_release(player_t *p, int x, int y) {
	obj_t *o = p->pressed;
	bool redraw = false;
	if (p->drag) {
		obj_t *s = p->drag;
		player_motion(p, x, y);
		p->drag = NULL;
		p->pressed = NULL;
		ui(p);
		fire(p, s);
		return true;
	}
	if (!o) return false;
	p->pressed = NULL;
	if (inside(o, x, y)) {
		if (o->kind == OB_CHECK) o->value = !o->value;
		else if (o->kind == OB_RADIO) radio_set(p, o);
		ui(p);
		fire(p, o);
		return true;
	}
	ui(p);
	redraw = true;
	return redraw;
}

// The next field on the card after `o` (Tab), wrapping; NULL if none.
static obj_t *next_field(player_t *p, obj_t *o) {
	card_t *c = p->d->card[p->cur];
	int i, at = card_index_of(c, o), n = c->nobj;
	for (i = 1; i <= n; i++) {
		obj_t *f = c->obj[(at + i + n) % n];
		if (f->kind == OB_FIELD && !(f->flags & OBF_HIDDEN)) return f;
	}
	return NULL;
}

bool player_key(player_t *p, uint32_t k, bool *redraw) {
	obj_t *f = p->focus;
	*redraw = false;
	if (!f) return false;
	if (k == 0x1b) { p->focus = NULL; ui(p); *redraw = true; return true; }
	if (k == '\t') { p->focus = next_field(p, f); ui(p); *redraw = true; return true; }
	{
		const char *t = f->text ? f->text : "";
		uint32_t n = (uint32_t)strlen(t);
		char *b;
		if (k == 0x08 || k == 0x7f) {
			if (!n) return true;
			// a whole UTF-8 character
			n--;
			while (n && ((uint8_t)t[n] & 0xC0) == 0x80) n--;
			obj_set_text(f, t, n);
			*redraw = true;
			return true;
		}
		if (k == 0x0d) {
			// a field one line tall runs its action; a taller one takes
			// the line
			int rows = (f->h - 2 * f->pad) / (f->size ? f->size : 16);
			if (rows < 2) { fire(p, f); *redraw = true; return true; }
			k = '\n';
		}
		if (k != '\n' && (k < 0x20 || k >= 0x110000 || (k >= 0x7f && k < 0xa0))) return true;
		b = pool_alloc(n + 5);
		if (!b) return true;
		memcpy(b, t, n);
		if (k < 0x80) b[n++] = (char)k;
		else if (k < 0x800) { b[n++] = (char)(0xC0 | (k >> 6)); b[n++] = (char)(0x80 | (k & 63)); }
		else if (k < 0x10000) { b[n++] = (char)(0xE0 | (k >> 12)); b[n++] = (char)(0x80 | ((k >> 6) & 63)); b[n++] = (char)(0x80 | (k & 63)); }
		else { b[n++] = (char)(0xF0 | (k >> 18)); b[n++] = (char)(0x80 | ((k >> 12) & 63)); b[n++] = (char)(0x80 | ((k >> 6) & 63)); b[n++] = (char)(0x80 | (k & 63)); }
		obj_set_text(f, b, n);
		pool_free(b);
		*redraw = true;
		return true;
	}
}
