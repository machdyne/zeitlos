/*
 * slide -- the deck model. See deck.h.
 */

#include "deck.h"
#include "../../common/zpool.h"

#include <string.h>

deck_t *g_deck;

static const char *kind_names[OB_KINDS] = {
	"text", "rect", "rrect", "oval", "line", "poly", "image",
	"button", "check", "radio", "field", "slider", "list",
	"diamond", "slant", "pill", "document", "cylinder"
};

const char *obj_kind_name(int kind) {
	return (kind >= 0 && kind < OB_KINDS) ? kind_names[kind] : "?";
}

int obj_kind_of(const char *name) {
	int i;
	for (i = 0; i < OB_KINDS; i++) if (!strcmp(name, kind_names[i])) return i;
	return -1;
}

// "prefix n", without printf (newlib's costs an app ~100KB).
static void name_n(char *out, int cap, const char *prefix, int n) {
	char num[12];
	int k = 0, i = 0;
	do { num[k++] = (char)('0' + n % 10); n /= 10; } while (n && k < 11);
	while (*prefix && i < cap - 1) out[i++] = *prefix++;
	if (i < cap - 1) out[i++] = ' ';
	while (k && i < cap - 1) out[i++] = num[--k];
	out[i] = 0;
}

static void copy_name(char *dst, int cap, const char *src) {
	int i = 0;
	if (src) for (; i < cap - 1 && src[i]; i++) dst[i] = src[i];
	dst[i] = 0;
}

// -- objects -------------------------------------------------------------

obj_t *obj_new(int kind) {
	obj_t *o = pool_zalloc(sizeof(obj_t));
	if (!o) return NULL;
	o->kind = (uint8_t)kind;
	o->w = 100;
	o->h = 60;
	o->line_tone = 16;
	o->text_tone = 16;
	o->size = 16;
	o->pad = 4;
	// a shape's text is in its middle
	if (OB_IS_SHAPE(kind)) { o->align = AL_CENTER; o->valign = VA_MIDDLE; }
	switch (kind) {
	case OB_TEXT:
		o->h = 24;
		o->w = 200;
		break;
	case OB_DIAMOND: case OB_SLANT: case OB_PILL: case OB_DOC: case OB_DRUM:
		o->w = 110; o->h = 60;
		o->line_w = 1;
		o->size = 14;
		break;
	case OB_RECT: case OB_OVAL:
		o->line_w = 1;
		break;
	case OB_RRECT:
		o->line_w = 1;
		o->radius = 10;
		break;
	case OB_LINE:
		o->line_w = 1;
		o->h = 0;
		break;
	case OB_POLY:
		o->line_w = 1;
		break;
	case OB_BUTTON:
		o->w = 90; o->h = 24;
		o->line_w = 1;
		o->radius = 6;
		o->align = AL_CENTER; o->valign = VA_MIDDLE;
		o->size = 14;
		break;
	case OB_CHECK: case OB_RADIO:
		o->w = 120; o->h = 18;
		o->valign = VA_MIDDLE;
		o->size = 14;
		break;
	case OB_FIELD:
		o->w = 180; o->h = 24;
		o->line_w = 1;
		o->size = 14;
		o->pad = 4;
		break;
	case OB_SLIDER:
		o->w = 160; o->h = 18;
		o->vmax = 100;
		break;
	case OB_LIST:
		o->w = 140; o->h = 80;
		o->line_w = 1;
		o->size = 13;
		o->pad = 3;
		o->value = -1;
		break;
	default:
		break;
	}
	return o;
}

void (*obj_free_hook)(obj_t *o);

void obj_free(obj_t *o) {
	if (!o) return;
	if (obj_free_hook) obj_free_hook(o);
	pool_free(o->text);
	pool_free(o->pts);
	pool_free(o->src);
	pool_free(o->action);
	pool_free(o->script);
	pool_free(o);
}

static bool dup_str(char **dst, const char *src) {
	*dst = NULL;
	if (!src) return true;
	*dst = pool_strdup(src);
	return *dst != NULL;
}

obj_t *obj_dup(const obj_t *o) {
	obj_t *n = pool_alloc(sizeof(obj_t));
	if (!n) return NULL;
	*n = *o;
	n->img = NULL;
	n->pts = NULL;
	if (!dup_str(&n->text, o->text) || !dup_str(&n->src, o->src) ||
		!dup_str(&n->action, o->action) || !dup_str(&n->script, o->script))
		goto fail;
	if (o->npts) {
		n->pts = pool_alloc((size_t)o->npts * 4);
		if (!n->pts) goto fail;
		memcpy(n->pts, o->pts, (size_t)o->npts * 4);
	}
	return n;
fail:
	n->img = NULL;
	obj_free(n);
	return NULL;
}

bool obj_set_text(obj_t *o, const char *s, uint32_t len) {
	char *t = pool_strndup(s ? s : "", s ? len : 0);
	if (!t) return false;
	pool_free(o->text);
	o->text = t;
	return true;
}

bool obj_set_str(char **field, const char *s) {
	char *t = NULL;
	if (s && *s) {
		t = pool_strdup(s);
		if (!t) return false;
	}
	pool_free(*field);
	*field = t;
	return true;
}

void obj_bounds(const obj_t *o, int *x0, int *y0, int *x1, int *y1) {
	int a = o->x, b = o->y, c = o->x + o->w, d = o->y + o->h;
	*x0 = a < c ? a : c;
	*x1 = a < c ? c : a;
	*y0 = b < d ? b : d;
	*y1 = b < d ? d : b;
}

void obj_move(obj_t *o, int dx, int dy) {
	int i;
	o->x = (int16_t)(o->x + dx);
	o->y = (int16_t)(o->y + dy);
	for (i = 0; i < o->npts; i++) {
		o->pts[2 * i] = (int16_t)(o->pts[2 * i] + dx);
		o->pts[2 * i + 1] = (int16_t)(o->pts[2 * i + 1] + dy);
	}
}

void obj_set_box(obj_t *o, int x, int y, int w, int h) {
	if (o->kind == OB_POLY && o->npts && o->w && o->h) {
		int i;
		for (i = 0; i < o->npts; i++) {
			int px = o->pts[2 * i], py = o->pts[2 * i + 1];
			o->pts[2 * i] = (int16_t)(x + ((px - o->x) * w) / o->w);
			o->pts[2 * i + 1] = (int16_t)(y + ((py - o->y) * h) / o->h);
		}
	}
	o->x = (int16_t)x;
	o->y = (int16_t)y;
	o->w = (int16_t)w;
	o->h = (int16_t)h;
}

// -- connectors --------------------------------------------------------------

bool obj_is_connector(const obj_t *o) {
	return (o->kind == OB_LINE || o->kind == OB_POLY) && o->from[0] && o->to[0];
}

static const obj_t *find_on(const card_t *c, const card_t *bg, const char *name) {
	obj_t *o = c ? card_find_obj(c, name) : NULL;
	if (!o && bg) o = card_find_obj(bg, name);
	return o;
}

// Where the ray from o's centre towards (tx, ty) leaves o: its edge, as
// its shape has it (an oval's ellipse, a diamond's sides, else the box).
static void edge_toward(const obj_t *o, int tx, int ty, int *ex, int *ey) {
	int x0, y0, x1, y1;
	long cx, cy, dx, dy, hw, hh;
	obj_bounds(o, &x0, &y0, &x1, &y1);
	cx = (x0 + x1) / 2; cy = (y0 + y1) / 2;
	hw = (x1 - x0) / 2; hh = (y1 - y0) / 2;
	dx = tx - cx; dy = ty - cy;
	if ((!dx && !dy) || hw <= 0 || hh <= 0) { *ex = (int)cx; *ey = (int)cy; return; }
	if (o->kind == OB_OVAL) {
		// t such that (t dx / hw)^2 + (t dy / hh)^2 = 1, in 1/4096
		double a = (double)dx / hw, b = (double)dy / hh;
		double t = 1.0 / __builtin_sqrt(a * a + b * b);
		*ex = (int)(cx + dx * t + (dx > 0 ? 0.5 : -0.5));
		*ey = (int)(cy + dy * t + (dy > 0 ? 0.5 : -0.5));
		return;
	}
	if (o->kind == OB_DIAMOND) {
		// |x| / hw + |y| / hh = 1
		double t = 1.0 / ((dx < 0 ? -dx : dx) / (double)hw + (dy < 0 ? -dy : dy) / (double)hh);
		*ex = (int)(cx + dx * t + (dx > 0 ? 0.5 : -0.5));
		*ey = (int)(cy + dy * t + (dy > 0 ? 0.5 : -0.5));
		return;
	}
	// the box: whichever side the ray meets first
	if ((dx < 0 ? -dx : dx) * hh >= (dy < 0 ? -dy : dy) * hw) {
		*ex = (int)(cx + (dx > 0 ? hw : -hw));
		*ey = (int)(cy + dy * hw / (dx < 0 ? -dx : dx));
		if (o->kind == OB_SLANT) {
			// a side leans in by k over the height: the point on it at that height
			long k = hw / 2 < hh ? hw / 2 : hh, h = 2 * hh, down = *ey - y0;
			bool up = (dx > 0) != ((o->flags & OBF_FLIPX) != 0);
			*ex += (int)((dx > 0 ? -1 : 1) * (up ? k * down : k * (h - down)) / h);
		}
	} else {
		*ex = (int)(cx + dx * hh / (dy < 0 ? -dy : dy));
		*ey = (int)(cy + (dy > 0 ? hh : -hh));
	}
}

void obj_fit_box(obj_t *o) {
	int i, a = 30000, b = 30000, c = -30000, d = -30000;
	for (i = 0; i < o->npts; i++) {
		int x = o->pts[2 * i], y = o->pts[2 * i + 1];
		if (x < a) a = x;
		if (x > c) c = x;
		if (y < b) b = y;
		if (y > d) d = y;
	}
	o->x = (int16_t)a; o->y = (int16_t)b;
	o->w = (int16_t)(c - a); o->h = (int16_t)(d - b);
}

bool obj_make_connector(obj_t *o, const char *from, const char *to, bool elbow) {
	if (o->kind != OB_LINE && o->kind != OB_POLY) return false;
	copy_name(o->from, OB_NAME_MAX, from);
	copy_name(o->to, OB_NAME_MAX, to);
	if (elbow && (o->kind != OB_POLY || o->npts != 4)) {
		int16_t *p = pool_alloc(4 * 2 * sizeof(int16_t));
		if (!p) return false;
		pool_free(o->pts);
		o->pts = p;
		o->npts = 4;
		o->kind = OB_POLY;
		o->closed = 0;
		memset(p, 0, 8 * sizeof(int16_t));
	} else if (!elbow && o->kind == OB_POLY) {
		pool_free(o->pts);
		o->pts = NULL;
		o->npts = 0;
		o->kind = OB_LINE;
	}
	return true;
}

int card_route(card_t *c, const card_t *bg, int (*chg)[4], int max) {
	int i, n = 0;
	for (i = 0; i < c->nobj; i++) {
		obj_t *o = c->obj[i];
		const obj_t *a, *b;
		int ax0, ay0, ax1, ay1, bx0, by0, bx1, by1, acx, acy, bcx, bcy;
		int old[4];
		bool moved = false;
		if (!obj_is_connector(o)) continue;
		a = find_on(c, bg, o->from);
		b = find_on(c, bg, o->to);
		if (!a || !b || a == o || b == o) continue;
		obj_bounds(o, &old[0], &old[1], &old[2], &old[3]);
		obj_bounds(a, &ax0, &ay0, &ax1, &ay1);
		obj_bounds(b, &bx0, &by0, &bx1, &by1);
		acx = (ax0 + ax1) / 2; acy = (ay0 + ay1) / 2;
		bcx = (bx0 + bx1) / 2; bcy = (by0 + by1) / 2;
		if (o->kind == OB_LINE) {
			int sx, sy, ex, ey;
			edge_toward(a, bcx, bcy, &sx, &sy);
			edge_toward(b, acx, acy, &ex, &ey);
			if (o->x != sx || o->y != sy || o->w != ex - sx || o->h != ey - sy) {
				o->x = (int16_t)sx; o->y = (int16_t)sy;
				o->w = (int16_t)(ex - sx); o->h = (int16_t)(ey - sy);
				moved = true;
			}
		} else if (o->npts == 4) {
			int16_t q[8];
			int dx = bcx - acx, dy = bcy - acy;
			if ((dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy)) {
				// across, along, across: out of a's side, into b's
				// (the ends on the shapes' own outlines, straight out from their middles)
				int mx = (dx > 0 ? (ax1 + bx0) : (ax0 + bx1)) / 2, sx, sy, ex, ey;
				edge_toward(a, dx > 0 ? ax1 + 1000 : ax0 - 1000, acy, &sx, &sy);
				edge_toward(b, dx > 0 ? bx0 - 1000 : bx1 + 1000, bcy, &ex, &ey);
				q[0] = (int16_t)sx; q[1] = (int16_t)acy;
				q[2] = (int16_t)mx; q[3] = (int16_t)acy;
				q[4] = (int16_t)mx; q[5] = (int16_t)bcy;
				q[6] = (int16_t)ex; q[7] = (int16_t)bcy;
			} else {
				// down, across, down
				int my = (dy > 0 ? (ay1 + by0) : (ay0 + by1)) / 2, sx, sy, ex, ey;
				edge_toward(a, acx, dy > 0 ? ay1 + 1000 : ay0 - 1000, &sx, &sy);
				edge_toward(b, bcx, dy > 0 ? by0 - 1000 : by1 + 1000, &ex, &ey);
				q[0] = (int16_t)acx; q[1] = (int16_t)sy;
				q[2] = (int16_t)acx; q[3] = (int16_t)my;
				q[4] = (int16_t)bcx; q[5] = (int16_t)my;
				q[6] = (int16_t)bcx; q[7] = (int16_t)ey;
			}
			if (memcmp(q, o->pts, sizeof(q))) {
				memcpy(o->pts, q, sizeof(q));
				obj_fit_box(o);
				moved = true;
			}
		}
		if (moved) {
			if (n < max && chg) {
				int x0, y0, x1, y1;
				obj_bounds(o, &x0, &y0, &x1, &y1);
				chg[2 * n][0] = old[0]; chg[2 * n][1] = old[1]; chg[2 * n][2] = old[2]; chg[2 * n][3] = old[3];
				chg[2 * n + 1][0] = x0; chg[2 * n + 1][1] = y0; chg[2 * n + 1][2] = x1; chg[2 * n + 1][3] = y1;
			}
			n++;
		}
	}
	return n;
}

// -- cards ---------------------------------------------------------------

card_t *card_new(const char *name) {
	card_t *c = pool_zalloc(sizeof(card_t));
	if (!c) return NULL;
	copy_name(c->name, CARD_NAME_MAX, name);
	c->bg = -1;
	c->trans_ms = 400;
	return c;
}

void card_free(card_t *c) {
	int i;
	if (!c) return;
	for (i = 0; i < c->nobj; i++) obj_free(c->obj[i]);
	pool_free(c->obj);
	pool_free(c->notes);
	pool_free(c->action);
	pool_free(c->script);
	pool_free(c);
}

card_t *card_dup(const card_t *c) {
	card_t *n = card_new(c->name);
	int i;
	if (!n) return NULL;
	n->bg = c->bg;
	n->trans = c->trans;
	n->trans_ms = c->trans_ms;
	n->advance_ms = c->advance_ms;
	if (!dup_str(&n->notes, c->notes) || !dup_str(&n->script, c->script) ||
		!dup_str(&n->action, c->action)) {
		card_free(n);
		return NULL;
	}
	for (i = 0; i < c->nobj; i++) {
		obj_t *o = obj_dup(c->obj[i]);
		if (!o || !card_add(n, -1, o)) {
			obj_free(o);
			card_free(n);
			return NULL;
		}
	}
	return n;
}

bool card_add(card_t *c, int at, obj_t *o) {
	if (c->nobj == c->cap) {
		int ncap = c->cap ? c->cap * 2 : 8;
		obj_t **n = pool_realloc(c->obj, (size_t)ncap * sizeof(obj_t *));
		if (!n) return false;
		c->obj = n;
		c->cap = ncap;
	}
	if (at < 0 || at > c->nobj) at = c->nobj;
	memmove(&c->obj[at + 1], &c->obj[at], (size_t)(c->nobj - at) * sizeof(obj_t *));
	c->obj[at] = o;
	c->nobj++;
	return true;
}

obj_t *card_take(card_t *c, int i) {
	obj_t *o;
	if (i < 0 || i >= c->nobj) return NULL;
	o = c->obj[i];
	memmove(&c->obj[i], &c->obj[i + 1], (size_t)(c->nobj - i - 1) * sizeof(obj_t *));
	c->nobj--;
	return o;
}

int card_index_of(const card_t *c, const obj_t *o) {
	int i;
	for (i = 0; i < c->nobj; i++) if (c->obj[i] == o) return i;
	return -1;
}

obj_t *card_find_obj(const card_t *c, const char *name) {
	int i;
	for (i = 0; i < c->nobj; i++) if (!strcmp(c->obj[i]->name, name)) return c->obj[i];
	return NULL;
}

void card_unique_obj_name(const card_t *c, int kind, char *out, int cap) {
	int n;
	for (n = 1; n < 10000; n++) {
		name_n(out, cap, obj_kind_name(kind), n);
		if (!card_find_obj(c, out)) return;
	}
}

// -- the deck ------------------------------------------------------------

static bool grow(void ***arr, int *cap, int n) {
	if (n < *cap) return true;
	{
		int nc = *cap ? *cap * 2 : 16;
		void **na = pool_realloc(*arr, (size_t)nc * sizeof(void *));
		if (!na) return false;
		*arr = na;
		*cap = nc;
	}
	return true;
}

deck_t *deck_new(void) {
	deck_t *d = pool_zalloc(sizeof(deck_t));
	card_t *c;
	if (!d) return NULL;
	zpage_set(&d->page, "classic", true);
	deck_font_index(d, "sans");
	c = card_new("card 1");
	if (!c || !deck_insert_card(d, 0, c)) {
		card_free(c);
		deck_free(d);
		return NULL;
	}
	return d;
}

void deck_free(deck_t *d) {
	int i;
	if (!d) return;
	for (i = 0; i < d->ncard; i++) card_free(d->card[i]);
	for (i = 0; i < d->nbg; i++) card_free(d->bg[i]);
	for (i = 0; i < d->nfont; i++) pool_free(d->font[i]);
	pool_free(d->card);
	pool_free(d->bg);
	pool_free(d->script);
	pool_free(d);
}

bool deck_insert_card(deck_t *d, int at, card_t *c) {
	if (!grow((void ***)&d->card, &d->capcard, d->ncard)) return false;
	if (at < 0 || at > d->ncard) at = d->ncard;
	memmove(&d->card[at + 1], &d->card[at], (size_t)(d->ncard - at) * sizeof(card_t *));
	d->card[at] = c;
	d->ncard++;
	return true;
}

card_t *deck_remove_card(deck_t *d, int at) {
	card_t *c;
	if (at < 0 || at >= d->ncard) return NULL;
	c = d->card[at];
	memmove(&d->card[at], &d->card[at + 1], (size_t)(d->ncard - at - 1) * sizeof(card_t *));
	d->ncard--;
	return c;
}

bool deck_move_card(deck_t *d, int from, int to) {
	card_t *c;
	if (from < 0 || from >= d->ncard || to < 0 || to >= d->ncard) return false;
	if (from == to) return true;
	c = d->card[from];
	if (from < to)
		memmove(&d->card[from], &d->card[from + 1], (size_t)(to - from) * sizeof(card_t *));
	else
		memmove(&d->card[to + 1], &d->card[to], (size_t)(from - to) * sizeof(card_t *));
	d->card[to] = c;
	return true;
}

int deck_find_card(const deck_t *d, const char *name) {
	int i;
	for (i = 0; i < d->ncard; i++) if (!strcmp(d->card[i]->name, name)) return i;
	return -1;
}

bool deck_insert_bg(deck_t *d, int at, card_t *c) {
	if (!grow((void ***)&d->bg, &d->capbg, d->nbg)) return false;
	if (at < 0 || at > d->nbg) at = d->nbg;
	memmove(&d->bg[at + 1], &d->bg[at], (size_t)(d->nbg - at) * sizeof(card_t *));
	d->bg[at] = c;
	d->nbg++;
	return true;
}

int deck_find_bg(const deck_t *d, const char *name) {
	int i;
	for (i = 0; i < d->nbg; i++) if (!strcmp(d->bg[i]->name, name)) return i;
	return -1;
}

void deck_unique_card_name(const deck_t *d, char *out, int cap) {
	int n;
	for (n = d->ncard + 1; n < 100000; n++) {
		name_n(out, cap, "card", n);
		if (deck_find_card(d, out) < 0) return;
	}
}

int deck_font_index(deck_t *d, const char *name) {
	int i;
	for (i = 0; i < d->nfont; i++) if (!strcmp(d->font[i], name)) return i;
	if (d->nfont >= DECK_FONTS_MAX) return 0;
	d->font[d->nfont] = pool_strdup(name);
	if (!d->font[d->nfont]) return 0;
	return d->nfont++;
}
