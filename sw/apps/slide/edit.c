/*
 * slide -- editing. See edit.h.
 */

#include "edit.h"
#include "act.h"
#include "../../common/zpool.h"

#include <string.h>

#include "../../common/zkbd.h"
#include "../../common/zutf8.h"
#include "../../common/zsvg.h"

static void msg(editor_t *e, const char *s) {
	size_t n = strlen(s);
	if (n >= sizeof(e->msg)) n = sizeof(e->msg) - 1;
	memcpy(e->msg, s, n);
	e->msg[n] = 0;
}

static void msg2(editor_t *e, const char *a, const char *b) {
	char t[96];
	size_t n = 0, k;
	for (k = 0; a[k] && n < sizeof(t) - 1; k++) t[n++] = a[k];
	for (k = 0; b && b[k] && n < sizeof(t) - 1; k++) t[n++] = b[k];
	t[n] = 0;
	msg(e, t);
}

static int iabs(int v) { return v < 0 ? -v : v; }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

// -- the editor ------------------------------------------------------------

void edit_init(editor_t *e, deck_t *d) {
	memset(e, 0, sizeof(*e));
	e->d = d;
	e->tool = TOOL_SELECT;
	e->caret_goal = -1;
	e->all_dirty = true;
	e->dx1 = -1;
}

static void undo_clear(undo_t *u, int *n) {
	int i;
	for (i = 0; i < *n; i++) pool_free(u[i].text);
	*n = 0;
}

void edit_reset(editor_t *e, deck_t *d) {
	undo_clear(e->undo, &e->nundo);
	undo_clear(e->redo, &e->nredo);
	{
		int snap = e->snap, tool = e->tool;
		edit_init(e, d);
		e->snap = snap;
		e->tool = tool == TOOL_BROWSE ? TOOL_BROWSE : TOOL_SELECT;
	}
}

card_t *edit_card(const editor_t *e) {
	card_t *c;
	if (!e->d || e->cur < 0 || e->cur >= e->d->ncard) return NULL;
	c = e->d->card[e->cur];
	if (e->on_bg && c->bg >= 0 && c->bg < e->d->nbg) return e->d->bg[c->bg];
	return c;
}

int edit_snap(const editor_t *e, int v) {
	if (e->snap <= 1) return v;
	return ((v + (v >= 0 ? e->snap / 2 : -e->snap / 2)) / e->snap) * e->snap;
}

// -- damage ----------------------------------------------------------------

void edit_damage(editor_t *e, int x0, int y0, int x1, int y1) {
	if (e->dx1 < e->dx0) { e->dx0 = x0; e->dy0 = y0; e->dx1 = x1; e->dy1 = y1; return; }
	e->dx0 = imin(e->dx0, x0);
	e->dy0 = imin(e->dy0, y0);
	e->dx1 = imax(e->dx1, x1);
	e->dy1 = imax(e->dy1, y1);
}

void edit_damage_obj(editor_t *e, const obj_t *o) {
	int x0, y0, x1, y1, m;
	obj_bounds(o, &x0, &y0, &x1, &y1);
	m = o->line_w + 3;
	if (o->arrows) m += render_arrow_len(o->line_w);
	if (OB_EDITS_TEXT(o->kind) && o->text && o->text[0]) {
		// text may run past its box: below it, and an italic to the right
		static text_layout_t L;
		zc_fix bottom;
		render_text_layout(e->d, o, &L);
		bottom = L.top + L.line_h * (L.n + 1);
		if ((bottom >> ZC_FRAC) > y1) y1 = bottom >> ZC_FRAC;
		if ((L.top >> ZC_FRAC) < y0) y0 = L.top >> ZC_FRAC;
		m += o->size / 3 + 2;
	}
	edit_damage(e, x0 - m, y0 - m, x1 + m, y1 + m);
}

void edit_damage_all(editor_t *e) { e->all_dirty = true; }

void edit_damage_clear(editor_t *e) {
	e->all_dirty = false;
	e->dx0 = 0;
	e->dx1 = -1;
}

static void damage_sel(editor_t *e) {
	int i;
	for (i = 0; i < e->nsel; i++) edit_damage_obj(e, e->sel[i]);
}

// -- selection -----------------------------------------------------------------

void edit_select_none(editor_t *e) {
	damage_sel(e);
	e->nsel = 0;
}

bool edit_is_selected(const editor_t *e, const obj_t *o) {
	int i;
	for (i = 0; i < e->nsel; i++) if (e->sel[i] == o) return true;
	return false;
}

static void select_one(editor_t *e, obj_t *o, bool add) {
	int i;
	for (i = 0; i < e->nsel; i++) {
		if (e->sel[i] == o) {
			if (add) {		// shift-click on a selected object deselects it
				edit_damage_obj(e, o);
				memmove(&e->sel[i], &e->sel[i + 1], (size_t)(e->nsel - i - 1) * sizeof(obj_t *));
				e->nsel--;
			}
			return;
		}
	}
	if (e->nsel < SEL_MAX) e->sel[e->nsel++] = o;
	edit_damage_obj(e, o);
}

void edit_select(editor_t *e, obj_t *o, bool add) {
	card_t *c = edit_card(e);
	int i;
	if (!add) edit_select_none(e);
	select_one(e, o, add);
	// the rest of its group with it
	if (o->gid && c)
		for (i = 0; i < c->nobj; i++)
			if (c->obj[i] != o && c->obj[i]->gid == o->gid) select_one(e, c->obj[i], add);
}

// Copies just made (a duplicate, a paste): their groups are new groups,
// and a connector copied with its shapes connects the copies.
static void fix_copies(card_t *c, obj_t **made, char (*old)[OB_NAME_MAX], int n) {
	int i, k;
	uint16_t top = 0, map_from[SEL_MAX], map_to[SEL_MAX];
	int nmap = 0;
	for (i = 0; i < c->nobj; i++) if (c->obj[i]->gid > top) top = c->obj[i]->gid;
	for (i = 0; i < n; i++) {
		obj_t *o = made[i];
		if (!o->gid) continue;
		for (k = 0; k < nmap && map_from[k] != o->gid; k++) ;
		if (k == nmap && nmap < SEL_MAX) { map_from[nmap] = o->gid; map_to[nmap] = ++top; nmap++; }
		if (k < nmap) o->gid = map_to[k];
	}
	for (i = 0; i < n; i++) {
		obj_t *o = made[i];
		if (!obj_is_connector(o)) continue;
		for (k = 0; k < n; k++) {
			if (!strcmp(o->from, old[k])) memcpy(o->from, made[k]->name, OB_NAME_MAX);
			if (!strcmp(o->to, old[k])) memcpy(o->to, made[k]->name, OB_NAME_MAX);
		}
	}
}

void edit_select_all(editor_t *e) {
	card_t *c = edit_card(e);
	int i;
	edit_select_none(e);
	if (!c) return;
	for (i = 0; i < c->nobj && e->nsel < SEL_MAX; i++) e->sel[e->nsel++] = c->obj[i];
	damage_sel(e);
}

// distance squared from (px, py) to the segment (ax, ay)-(bx, by)
static int64_t seg_d2(int px, int py, int ax, int ay, int bx, int by) {
	int64_t dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
	int64_t t, qx, qy;
	if (l2 == 0) {
		qx = px - ax; qy = py - ay;
		return qx * qx + qy * qy;
	}
	t = ((int64_t)(px - ax) * dx + (int64_t)(py - ay) * dy) * 1024 / l2;
	if (t < 0) t = 0;
	if (t > 1024) t = 1024;
	qx = (int64_t)px * 1024 - ((int64_t)ax * 1024 + t * dx);
	qy = (int64_t)py * 1024 - ((int64_t)ay * 1024 + t * dy);
	return (qx * qx + qy * qy) / (1024 * 1024);
}

static bool in_poly(const obj_t *o, int x, int y) {
	int i, j, n = o->npts;
	bool in = false;
	for (i = 0, j = n - 1; i < n; j = i++) {
		int xi = o->pts[2 * i], yi = o->pts[2 * i + 1];
		int xj = o->pts[2 * j], yj = o->pts[2 * j + 1];
		if (((yi > y) != (yj > y)) &&
			(x < (int)((int64_t)(xj - xi) * (y - yi) / (yj - yi)) + xi))
			in = !in;
	}
	return in;
}

static bool obj_hit(const obj_t *o, int x, int y, int tol) {
	int x0, y0, x1, y1;
	int r = tol + o->line_w / 2 + 1;
	obj_bounds(o, &x0, &y0, &x1, &y1);
	if (o->kind == OB_LINE)
		return seg_d2(x, y, o->x, o->y, o->x + o->w, o->y + o->h) <= (int64_t)r * r;
	if (o->kind == OB_POLY) {
		int i;
		for (i = 0; i + 1 < o->npts; i++)
			if (seg_d2(x, y, o->pts[2 * i], o->pts[2 * i + 1], o->pts[2 * i + 2], o->pts[2 * i + 3])
				<= (int64_t)r * r) return true;
		if (o->closed && o->npts > 2) {
			if (seg_d2(x, y, o->pts[0], o->pts[1], o->pts[2 * o->npts - 2], o->pts[2 * o->npts - 1])
				<= (int64_t)r * r) return true;
			return in_poly(o, x, y);
		}
		return false;
	}
	return x >= x0 - tol && x <= x1 + tol && y >= y0 - tol && y <= y1 + tol;
}

// The topmost shape (not a line, not a connector) at (x, y), for a line
// drawn from one to another.
static obj_t *shape_at(const editor_t *e, int x, int y) {
	card_t *c = edit_card(e);
	int i;
	if (!c) return NULL;
	for (i = c->nobj - 1; i >= 0; i--) {
		obj_t *o = c->obj[i];
		int x0, y0, x1, y1;
		if (o->kind == OB_LINE || o->kind == OB_POLY) continue;
		obj_bounds(o, &x0, &y0, &x1, &y1);
		if (x >= x0 && x <= x1 && y >= y0 && y <= y1) return o;
	}
	return NULL;
}

// Connectors laid again after whatever moved: what changed is redrawn.
void edit_route(editor_t *e) {
	card_t *c = edit_card(e), *bg = NULL;
	int chg[2 * 16][4], n, i;
	if (!c) return;
	if (!e->on_bg) {
		card_t *k = e->d->card[e->cur];
		if (k->bg >= 0 && k->bg < e->d->nbg) bg = e->d->bg[k->bg];
	}
	n = card_route(c, bg, chg, 16);
	if (n > 16) { edit_damage_all(e); return; }
	for (i = 0; i < 2 * n; i++) {
		// with room for an arrowhead and a thick line
		int m = 20;
		edit_damage(e, chg[i][0] - m, chg[i][1] - m, chg[i][2] + m, chg[i][3] + m);
	}
}

obj_t *edit_hit(const editor_t *e, int x, int y, int tol) {
	card_t *c = edit_card(e);
	int i;
	if (!c) return NULL;
	for (i = c->nobj - 1; i >= 0; i--)
		if (obj_hit(c->obj[i], x, y, tol)) return c->obj[i];
	return NULL;
}

int edit_handles(const obj_t *o, int hx[EDIT_HANDLES_MAX], int hy[EDIT_HANDLES_MAX]) {
	int x0, y0, x1, y1, xm, ym;
	if (o->kind == OB_LINE) {
		hx[0] = o->x; hy[0] = o->y;
		hx[1] = o->x + o->w; hy[1] = o->y + o->h;
		return 2;
	}
	obj_bounds(o, &x0, &y0, &x1, &y1);
	xm = (x0 + x1) / 2;
	ym = (y0 + y1) / 2;
	hx[0] = x0; hy[0] = y0;
	hx[1] = xm; hy[1] = y0;
	hx[2] = x1; hy[2] = y0;
	hx[3] = x1; hy[3] = ym;
	hx[4] = x1; hy[4] = y1;
	hx[5] = xm; hy[5] = y1;
	hx[6] = x0; hy[6] = y1;
	hx[7] = x0; hy[7] = ym;
	if (o->kind == OB_POLY && !obj_is_connector(o) && o->npts <= EDIT_POINTS_MAX) {
		int i;
		for (i = 0; i < o->npts; i++) { hx[8 + i] = o->pts[2 * i]; hy[8 + i] = o->pts[2 * i + 1]; }
		return 8 + o->npts;
	}
	return 8;
}

int edit_handle_at(const obj_t *o, int x, int y, int tol) {
	int hx[EDIT_HANDLES_MAX], hy[EDIT_HANDLES_MAX], n = edit_handles(o, hx, hy), i;
	// a corner before the box's handles: they can be in the same place
	for (i = n - 1; i >= 8; i--)
		if (iabs(x - hx[i]) <= tol && iabs(y - hy[i]) <= tol) return i;
	if (n > 8) n = 8;
	for (i = 0; i < n; i++)
		if (iabs(x - hx[i]) <= tol && iabs(y - hy[i]) <= tol) return i;
	return -1;
}

// -- undo ----------------------------------------------------------------------

static void push(undo_t *stack, int *n, undo_t u) {
	if (*n == UNDO_MAX) {
		pool_free(stack[0].text);
		memmove(&stack[0], &stack[1], (UNDO_MAX - 1) * sizeof(undo_t));
		(*n)--;
	}
	stack[(*n)++] = u;
}

static bool snapshot(editor_t *e, bool whole, undo_t *u) {
	sbuf_t b;
	sb_init(&b);
	u->bg = false;
	if (whole) {
		u->card = -1;
		deckio_write(&b, e->d);
	} else {
		card_t *c = e->d->card[e->cur];
		u->card = e->cur;
		if (e->on_bg && c->bg >= 0 && c->bg < e->d->nbg) {
			u->bg = true;
			u->card = c->bg;
			deckio_write_card(&b, e->d, e->d->bg[c->bg], true);
		} else {
			deckio_write_card(&b, e->d, c, false);
		}
	}
	if (b.oom || !b.buf) { sb_free(&b); return false; }
	u->text = b.buf;
	return true;
}

void edit_checkpoint(editor_t *e, bool whole) {
	undo_t u;
	if (!snapshot(e, whole, &u)) { msg(e, "No memory left to undo that"); return; }
	push(e->undo, &e->nundo, u);
	undo_clear(e->redo, &e->nredo);
	e->modified = true;
}

static void forget_edit_state(editor_t *e) {
	e->nsel = 0;
	e->text_obj = NULL;
	e->drag = DRAG_NONE;
	e->npoly = 0;
}

// Puts the state u describes back. The current state is pushed onto
// `other` first (redo for an undo, undo for a redo).
static bool restore(editor_t *e, undo_t *u, undo_t *other, int *nother) {
	undo_t now;
	bool whole = u->card < 0;
	// what is there now, at the same scope
	if (whole) {
		if (!snapshot(e, true, &now)) return false;
	} else {
		sbuf_t b;
		card_t *c = u->bg ? (u->card < e->d->nbg ? e->d->bg[u->card] : NULL)
			: (u->card < e->d->ncard ? e->d->card[u->card] : NULL);
		if (!c) return false;
		sb_init(&b);
		deckio_write_card(&b, e->d, c, u->bg);
		if (b.oom) { sb_free(&b); return false; }
		now.text = b.buf;
		now.card = u->card;
		now.bg = u->bg;
	}
	forget_edit_state(e);
	if (whole) {
		char err[8];
		deck_t *nd = deckio_read(u->text, (uint32_t)strlen(u->text), err, sizeof(err));
		if (!nd) { pool_free(now.text); return false; }
		deck_free(e->d);
		e->d = nd;
		g_deck = nd;
		if (e->cur >= nd->ncard) e->cur = nd->ncard - 1;
	} else {
		card_t *nc = deckio_read_card(e->d, u->text, (uint32_t)strlen(u->text));
		if (!nc) { pool_free(now.text); return false; }
		if (u->bg) {
			card_free(e->d->bg[u->card]);
			e->d->bg[u->card] = nc;
		} else {
			card_free(e->d->card[u->card]);
			e->d->card[u->card] = nc;
			e->cur = u->card;
		}
	}
	push(other, nother, now);
	pool_free(u->text);
	edit_damage_all(e);
	e->modified = true;
	return true;
}

bool edit_undo(editor_t *e) {
	undo_t u;
	if (!e->nundo) { msg(e, "Nothing to undo"); return false; }
	u = e->undo[--e->nundo];
	if (!restore(e, &u, e->redo, &e->nredo)) {
		e->undo[e->nundo++] = u;
		msg(e, "Could not undo");
		return false;
	}
	msg(e, "Undone");
	return true;
}

bool edit_redo(editor_t *e) {
	undo_t u;
	if (!e->nredo) { msg(e, "Nothing to redo"); return false; }
	u = e->redo[--e->nredo];
	if (!restore(e, &u, e->undo, &e->nundo)) {
		e->redo[e->nredo++] = u;
		msg(e, "Could not redo");
		return false;
	}
	msg(e, "Redone");
	return true;
}

// -- adding --------------------------------------------------------------------

static void add_obj(editor_t *e, obj_t *o, bool checkpoint) {
	card_t *c = edit_card(e);
	if (!c) { obj_free(o); return; }
	if (checkpoint) edit_checkpoint(e, false);
	if (!o->name[0]) card_unique_obj_name(c, o->kind, o->name, OB_NAME_MAX);
	if (!card_add(c, -1, o)) { obj_free(o); msg(e, "Out of memory"); return; }
	edit_select(e, o, false);
	edit_damage_obj(e, o);
	e->modified = true;
}

// -- /import FILE.svg: a drawing's outlines as polygons ---------------------------
//
// zsvg (sw/common/zsvg.h) flattens the drawing's shapes, fitted into the
// middle of the card; each subpath becomes a polygon -- filled ones in
// the grey of their fill, strokes as lines -- all one group, selected,
// one undo step. A filled shape's later subpaths inside its first (the
// hole of an "o") are filled white.

static void simplify(const int16_t *p, int a, int b, int tol, uint8_t *keep);

#define IMPORT_MAX_OBJS SEL_MAX		// all of it selected, to move as one
#define IMPORT_MAX_PTS  1024

typedef struct {
	editor_t	*e;
	z_svg_t		*s;
	card_t		*c;
	uint16_t	gid;
	int			made, dropped;
	int			shape;				// s->n_shapes at the shape's first subpath
	int			bx0, by0, bx1, by1;	// that subpath's box
	int16_t		*p;					// IMPORT_MAX_PTS points
	uint8_t		*keep;
} import_t;

static void import_outline(void *user, const float *xy, int n, bool closed,
	bool filled, int level, bool stroked) {
	import_t *im = user;
	obj_t *o;
	int i, k = 0, x0 = 32767, y0 = 32767, x1 = -32768, y1 = -32768;
	bool first, hole = false;
	if (im->made >= IMPORT_MAX_OBJS) { im->dropped++; return; }
	if (!filled && !stroked) return;
	if (n > IMPORT_MAX_PTS) n = IMPORT_MAX_PTS;
	for (i = 0; i < n; i++) {
		int x = (int)(xy[2 * i] + (xy[2 * i] < 0 ? -0.5f : 0.5f));
		int y = (int)(xy[2 * i + 1] + (xy[2 * i + 1] < 0 ? -0.5f : 0.5f));
		if (k && im->p[2 * k - 2] == x && im->p[2 * k - 1] == y) continue;
		im->p[2 * k] = (int16_t)x;
		im->p[2 * k + 1] = (int16_t)y;
		k++;
		if (x < x0) x0 = x;
		if (x > x1) x1 = x;
		if (y < y0) y0 = y;
		if (y > y1) y1 = y;
	}
	if (k < 2 || (k < 3 && (closed || filled))) return;
	first = im->s->n_shapes != im->shape;
	if (first) { im->shape = im->s->n_shapes; im->bx0 = x0; im->by0 = y0; im->bx1 = x1; im->by1 = y1; }
	else if (filled && x0 >= im->bx0 && y0 >= im->by0 && x1 <= im->bx1 && y1 <= im->by1) hole = true;
	// a curve came as many short pieces: keep the ones that matter
	memset(im->keep, 0, (size_t)k);
	im->keep[0] = im->keep[k - 1] = 1;
	simplify(im->p, 0, k - 1, 1, im->keep);
	o = obj_new(OB_POLY);
	if (!o) { im->dropped++; return; }
	o->pts = pool_alloc((size_t)k * 4);
	if (!o->pts) { obj_free(o); im->dropped++; return; }
	for (i = 0, n = 0; i < k; i++) {
		if (!im->keep[i]) continue;
		o->pts[2 * n] = im->p[2 * i];
		o->pts[2 * n + 1] = im->p[2 * i + 1];
		n++;
	}
	o->npts = (uint16_t)n;
	o->closed = (closed || filled) ? 1 : 0;
	o->line_w = stroked ? 1 : 0;
	o->line_tone = 16;
	if (filled) {
		o->fill = FILL_SHADE;
		o->fill_v = hole ? 0 : (uint8_t)(level * 16 / (Z_SVG_LEVELS - 1));
	} else o->fill = FILL_NONE;
	o->gid = im->gid;
	obj_fit_box(o);
	card_unique_obj_name(im->c, OB_POLY, o->name, OB_NAME_MAX);
	if (!im->made) edit_checkpoint(im->e, false);		// the step undo takes back
	if (!card_add(im->c, -1, o)) { obj_free(o); im->dropped++; return; }
	im->made++;
}

int edit_import_svg(editor_t *e, const char *doc, uint32_t len) {
	import_t *im;
	z_svg_t *s;
	card_t *c = edit_card(e);
	int i, rv, gw = e->d->page.gw, gh = e->d->page.gh;
	static z_svg_edge_t no_edges[16];
	if (!c) return -1;
	if (!z_svg_sniff(doc, len)) { msg(e, "That is not an SVG drawing"); return -1; }
	im = pool_zalloc(sizeof(import_t));
	s = pool_zalloc(sizeof(z_svg_t));
	if (im) im->p = pool_alloc(IMPORT_MAX_PTS * 2 * sizeof(int16_t));
	if (im) im->keep = pool_alloc(IMPORT_MAX_PTS);
	if (s) s->pts = pool_alloc(IMPORT_MAX_PTS * 2 * sizeof(float));
	if (!im || !s || !im->p || !im->keep || !s->pts) {
		if (im) { pool_free(im->p); pool_free(im->keep); }
		if (s) pool_free(s->pts);
		pool_free(im); pool_free(s);
		msg(e, "Out of memory");
		return -1;
	}
	edit_select_none(e);
	for (i = 0; i < c->nobj; i++) if (c->obj[i]->gid > im->gid) im->gid = c->obj[i]->gid;
	im->gid++;
	im->e = e;
	im->s = s;
	im->c = c;
	im->shape = -1;
	s->doc = doc;
	s->len = len;
	// in the middle of the card, most of it
	s->x = gw / 10; s->y = gh / 10; s->w = gw - gw / 5; s->h = gh - gh / 5;
	s->edges = no_edges;
	s->max_edges = 16;
	s->max_pts = IMPORT_MAX_PTS;
	s->sink.outline = import_outline;
	s->sink.user = im;
	rv = z_svg_render(s);
	if (rv != Z_SVG_OK && !im->made) msg2(e, "Cannot import: ", z_svg_strerror(rv));
	else if (!im->made) msg(e, "Nothing to import: the drawing has no outlines");
	else if (im->dropped) msg(e, "Imported, but not all of it: the card had no room for more");
	else msg(e, "Imported as polygons, grouped (ungroup: Ctrl+Shift+G)");
	rv = im->made;
	if (rv) edit_select(e, c->obj[c->nobj - 1], false);		// and its group
	edit_damage_all(e);
	pool_free(s->pts); pool_free(s);
	pool_free(im->p); pool_free(im->keep); pool_free(im);
	return rv;
}

// A new object of the tool's kind, with the defaults the last one had.
static obj_t *new_for_tool(editor_t *e, int tool) {
	int kind;
	obj_t *o;
	switch (tool) {
	case TOOL_TEXT: kind = OB_TEXT; break;
	case TOOL_RECT: kind = OB_RECT; break;
	case TOOL_RRECT: kind = OB_RRECT; break;
	case TOOL_OVAL: kind = OB_OVAL; break;
	case TOOL_LINE: case TOOL_ARROW: kind = OB_LINE; break;
	case TOOL_POLY: kind = OB_POLY; break;
	default: return NULL;
	}
	o = obj_new(kind);
	if (o && tool == TOOL_ARROW) { o->arrows = AR_END; o->line_w = 2; }
	(void)e;
	return o;
}

// -- the mouse ----------------------------------------------------------------

#define HANDLE_TOL 4

void edit_press(editor_t *e, int x, int y, bool shift, bool dbl) {
	card_t *c = edit_card(e);
	if (!c) return;
	e->constrain = shift;
	e->ax = e->bx = x;
	e->ay = e->by = y;

	if (e->text_obj) {
		// a click in the text being edited moves the caret; elsewhere ends it
		if (obj_hit(e->text_obj, x, y, 2)) {
			edit_text_begin(e, e->text_obj, x, y);
			return;
		}
		edit_text_end(e);
	}

	switch (e->tool) {
	case TOOL_SELECT: {
		obj_t *h;
		if (e->nsel == 1 && !(e->sel[0]->flags & OBF_LOCKED)) {
			int hi = edit_handle_at(e->sel[0], x, y, HANDLE_TOL);
			obj_t *s0 = e->sel[0];
			if (dbl && hi >= 8 && s0->npts > 2) {
				// a double-click on a corner takes it away
				int k = hi - 8;
				edit_checkpoint(e, false);
				edit_damage_obj(e, s0);
				memmove(&s0->pts[2 * k], &s0->pts[2 * k + 2], (size_t)(s0->npts - k - 1) * 2 * sizeof(int16_t));
				s0->npts--;
				obj_fit_box(s0);
				edit_damage_obj(e, s0);
				return;
			}
			if (dbl && hi < 0 && s0->kind == OB_POLY && !obj_is_connector(s0) && s0->npts < EDIT_POINTS_MAX &&
				obj_hit(s0, x, y, HANDLE_TOL)) {
				// on an edge: a new corner there
				int k, best = 0;
				long bd = -1;
				int16_t *np;
				for (k = 0; k < s0->npts - (s0->closed ? 0 : 1); k++) {
					int ax = s0->pts[2 * k], ay = s0->pts[2 * k + 1];
					int bx = s0->pts[2 * ((k + 1) % s0->npts)], by = s0->pts[2 * ((k + 1) % s0->npts) + 1];
					long d = (long)iabs(x - (ax + bx) / 2) + iabs(y - (ay + by) / 2);
					if (bd < 0 || d < bd) { bd = d; best = k; }
				}
				np = pool_alloc((size_t)(s0->npts + 1) * 2 * sizeof(int16_t));
				if (!np) return;
				edit_checkpoint(e, false);
				memcpy(np, s0->pts, (size_t)(best + 1) * 2 * sizeof(int16_t));
				np[2 * (best + 1)] = (int16_t)x;
				np[2 * (best + 1) + 1] = (int16_t)y;
				memcpy(&np[2 * (best + 2)], &s0->pts[2 * (best + 1)], (size_t)(s0->npts - best - 1) * 2 * sizeof(int16_t));
				pool_free(s0->pts);
				s0->pts = np;
				s0->npts++;
				obj_fit_box(s0);
				edit_damage_obj(e, s0);
				return;
			}
			if (hi >= 0) {
				int x0, y0, x1, y1;
				obj_bounds(e->sel[0], &x0, &y0, &x1, &y1);
				e->ox0 = x0; e->oy0 = y0; e->ox1 = x1; e->oy1 = y1;
				e->drag = DRAG_HANDLE;
				e->handle = hi;
				return;
			}
		}
		h = edit_hit(e, x, y, 2);
		if (!h) {
			if (!shift) edit_select_none(e);
			e->drag = DRAG_BAND;
			return;
		}
		if (dbl && OB_EDITS_TEXT(h->kind) && !(h->flags & OBF_LOCKED)) {
			edit_select(e, h, false);
			edit_text_begin(e, h, x, y);
			return;
		}
		if (shift) edit_select(e, h, true);
		else if (!edit_is_selected(e, h)) edit_select(e, h, false);
		e->drag = edit_is_selected(e, h) ? DRAG_MOVE : DRAG_NONE;
		return;
	}
	case TOOL_POLY:
		if (e->npoly == 0) edit_select_none(e);
		if (dbl) { edit_finish_poly(e); return; }
		if (e->npoly >= 2) {
			// a click on the first point closes it
			if (iabs(x - e->poly[0]) <= HANDLE_TOL && iabs(y - e->poly[1]) <= HANDLE_TOL) {
				edit_finish_poly(e);
				return;
			}
		}
		if (e->npoly < 128) {
			e->poly[2 * e->npoly] = (int16_t)edit_snap(e, x);
			e->poly[2 * e->npoly + 1] = (int16_t)edit_snap(e, y);
			e->npoly++;
			edit_damage(e, x - 4, y - 4, x + 4, y + 4);
		}
		return;
	case TOOL_PEN:
		edit_select_none(e);
		e->poly[0] = (int16_t)x;
		e->poly[1] = (int16_t)y;
		e->npoly = 1;
		e->drag = DRAG_PEN;
		return;
	case TOOL_TEXT: case TOOL_RECT: case TOOL_RRECT: case TOOL_OVAL:
	case TOOL_LINE: case TOOL_ARROW:
		edit_select_none(e);
		e->ax = e->bx = edit_snap(e, x);
		e->ay = e->by = edit_snap(e, y);
		e->drag = DRAG_CREATE;
		return;
	default:
		return;
	}
}

static void constrain_point(editor_t *e, int *x, int *y) {
	int dx = *x - e->ax, dy = *y - e->ay;
	if (!e->constrain) return;
	if (e->tool == TOOL_LINE || e->tool == TOOL_ARROW) {
		// horizontal, vertical or 45 degrees, whichever is nearest
		int ax = iabs(dx), ay = iabs(dy);
		if (ax > 2 * ay) dy = 0;
		else if (ay > 2 * ax) dx = 0;
		else { int m = imax(ax, ay); dx = dx < 0 ? -m : m; dy = dy < 0 ? -m : m; }
	} else {
		int m = imax(iabs(dx), iabs(dy));
		dx = dx < 0 ? -m : m;
		dy = dy < 0 ? -m : m;
	}
	*x = e->ax + dx;
	*y = e->ay + dy;
}

void edit_motion(editor_t *e, int x, int y, bool shift) {
	e->constrain = shift;
	if (e->drag == DRAG_PEN && e->npoly > 0) {
		// a point every few units; at the cap, the last one follows
		int lx = e->poly[2 * e->npoly - 2], ly = e->poly[2 * e->npoly - 1];
		if (iabs(x - lx) + iabs(y - ly) >= 3) {
			if (e->npoly < 128) e->npoly++;
			e->poly[2 * e->npoly - 2] = (int16_t)x;
			e->poly[2 * e->npoly - 1] = (int16_t)y;
			edit_damage(e, imin(x, lx) - 3, imin(y, ly) - 3, imax(x, lx) + 3, imax(y, ly) + 3);
		}
	}
	if (e->drag == DRAG_CREATE) {
		x = edit_snap(e, x);
		y = edit_snap(e, y);
		constrain_point(e, &x, &y);
	}
	if (e->drag == DRAG_MOVE && e->snap > 1 && e->nsel) {
		// the first selected object's corner lands on the grid
		int x0, y0, x1, y1, dx = x - e->ax, dy = y - e->ay;
		obj_bounds(e->sel[0], &x0, &y0, &x1, &y1);
		x = e->ax + edit_snap(e, x0 + dx) - x0;
		y = e->ay + edit_snap(e, y0 + dy) - y0;
	}
	if (e->drag == DRAG_HANDLE) {
		x = edit_snap(e, x);
		y = edit_snap(e, y);
	}
	e->bx = x;
	e->by = y;
}

// The box the dragged handle makes of the object's original box.
static void handle_box(const editor_t *e, int *x0, int *y0, int *x1, int *y1) {
	int a = e->ox0, b = e->oy0, c = e->ox1, d = e->oy1;
	switch (e->handle) {
	case 0: a = e->bx; b = e->by; break;
	case 1: b = e->by; break;
	case 2: c = e->bx; b = e->by; break;
	case 3: c = e->bx; break;
	case 4: c = e->bx; d = e->by; break;
	case 5: d = e->by; break;
	case 6: a = e->bx; d = e->by; break;
	case 7: a = e->bx; break;
	}
	if (e->constrain && (e->handle & 1) == 0) {
		// corners keep the proportions
		int ow = e->ox1 - e->ox0, oh = e->oy1 - e->oy0;
		if (ow > 0 && oh > 0) {
			int nw = c - a, nh = d - b;
			if (iabs(nw) * oh > iabs(nh) * ow) nh = (nh < 0 ? -1 : 1) * iabs(nw) * oh / ow;
			else nw = (nw < 0 ? -1 : 1) * iabs(nh) * ow / oh;
			if (e->handle == 0 || e->handle == 6) a = c - nw; else c = a + nw;
			if (e->handle == 0 || e->handle == 2) b = d - nh; else d = b + nh;
		}
	}
	*x0 = imin(a, c); *x1 = imax(a, c);
	*y0 = imin(b, d); *y1 = imax(b, d);
}

bool edit_drag_box(const editor_t *e, int *x0, int *y0, int *x1, int *y1, int *kind) {
	switch (e->drag) {
	case DRAG_CREATE: case DRAG_BAND:
		*x0 = imin(e->ax, e->bx); *x1 = imax(e->ax, e->bx);
		*y0 = imin(e->ay, e->by); *y1 = imax(e->ay, e->by);
		if (e->drag == DRAG_CREATE && (e->tool == TOOL_LINE || e->tool == TOOL_ARROW)) {
			*x0 = e->ax; *y0 = e->ay; *x1 = e->bx; *y1 = e->by;
			*kind = OB_LINE;
		} else if (e->drag == DRAG_CREATE && e->tool == TOOL_OVAL) {
			*kind = OB_OVAL;
		} else {
			*kind = OB_RECT;
		}
		return true;
	case DRAG_HANDLE: {
		obj_t *o = e->sel[0];
		if (o->kind == OB_LINE) {
			*x0 = o->x; *y0 = o->y; *x1 = o->x + o->w; *y1 = o->y + o->h;
			if (e->handle == 0) { *x0 = e->bx; *y0 = e->by; } else { *x1 = e->bx; *y1 = e->by; }
			*kind = OB_LINE;
			return true;
		}
		if (e->handle >= 8 && o->kind == OB_POLY) {
			// a corner: the side to it from the corner before
			int k = e->handle - 8, p = k > 0 ? k - 1 : (o->closed ? o->npts - 1 : 1);
			*x0 = o->pts[2 * p]; *y0 = o->pts[2 * p + 1];
			*x1 = e->bx; *y1 = e->by;
			*kind = OB_LINE;
			return true;
		}
		handle_box(e, x0, y0, x1, y1);
		*kind = o->kind == OB_OVAL ? OB_OVAL : OB_RECT;
		return true;
	}
	case DRAG_MOVE: {
		int i, a = 0, b = 0, c = 0, d = 0;
		for (i = 0; i < e->nsel; i++) {
			int p, q, r, s;
			obj_bounds(e->sel[i], &p, &q, &r, &s);
			if (!i) { a = p; b = q; c = r; d = s; }
			else { a = imin(a, p); b = imin(b, q); c = imax(c, r); d = imax(d, s); }
		}
		*x0 = a + e->bx - e->ax; *x1 = c + e->bx - e->ax;
		*y0 = b + e->by - e->ay; *y1 = d + e->by - e->ay;
		*kind = OB_RECT;
		return e->nsel > 0;
	}
	default:
		return false;
	}
}

static void release(editor_t *e, int drag);

void edit_release(editor_t *e, int x, int y) {
	int drag = e->drag;
	edit_motion(e, x, y, e->constrain);
	// the drag stays set while it is applied (edit_drag_box() reads it)
	release(e, drag);
	e->drag = DRAG_NONE;
}

// Ramer-Douglas-Peucker: keeps the points that matter for a line drawn
// by hand, dropping the jitter (tol units). Marks what stays in keep[].
static void simplify(const int16_t *p, int a, int b, int tol, uint8_t *keep) {
	int i, best = -1;
	long bd = 0, dx = p[2 * b] - p[2 * a], dy = p[2 * b + 1] - p[2 * a + 1];
	long len2 = dx * dx + dy * dy;
	for (i = a + 1; i < b; i++) {
		long ex = p[2 * i] - p[2 * a], ey = p[2 * i + 1] - p[2 * a + 1], d;
		// the distance from the chord, squared and scaled by its length squared
		if (len2) { long c = ex * dy - ey * dx; d = c * c / len2; }
		else d = ex * ex + ey * ey;
		if (d > bd) { bd = d; best = i; }
	}
	if (best >= 0 && bd > (long)tol * tol) {
		keep[best] = 1;
		simplify(p, a, best, tol, keep);
		simplify(p, best, b, tol, keep);
	}
}

static void finish_pen(editor_t *e) {
	static uint8_t keep[128];
	obj_t *o;
	int i, n = 0;
	bool closed;
	if (e->npoly < 3) { e->npoly = 0; edit_damage_all(e); return; }
	memset(keep, 0, sizeof(keep));
	keep[0] = keep[e->npoly - 1] = 1;
	simplify(e->poly, 0, e->npoly - 1, 2, keep);
	closed = e->npoly >= 6 && iabs(e->poly[0] - e->poly[2 * e->npoly - 2]) +
		iabs(e->poly[1] - e->poly[2 * e->npoly - 1]) <= 8;
	o = obj_new(OB_POLY);
	if (!o) { e->npoly = 0; return; }
	o->pts = pool_alloc((size_t)e->npoly * 4);
	if (!o->pts) { obj_free(o); e->npoly = 0; return; }
	for (i = 0; i < e->npoly; i++) {
		if (!keep[i]) continue;
		if (closed && i == e->npoly - 1 && n > 2) break;	// the end is the start
		o->pts[2 * n] = e->poly[2 * i];
		o->pts[2 * n + 1] = e->poly[2 * i + 1];
		n++;
	}
	o->npts = (uint16_t)n;
	o->closed = closed ? 1 : 0;
	o->smooth = 1;
	obj_fit_box(o);
	e->npoly = 0;
	add_obj(e, o, true);
	edit_damage_all(e);
}

static void release(editor_t *e, int drag) {
	card_t *c = edit_card(e);
	if (!c) return;
	switch (drag) {
	case DRAG_PEN:
		finish_pen(e);
		return;
	case DRAG_MOVE: {
		int dx = e->bx - e->ax, dy = e->by - e->ay, i;
		bool any = false;
		if (!dx && !dy) return;
		for (i = 0; i < e->nsel; i++) if (!(e->sel[i]->flags & OBF_LOCKED)) any = true;
		if (!any) { msg(e, "Locked"); return; }
		edit_checkpoint(e, false);
		for (i = 0; i < e->nsel; i++) {
			if (e->sel[i]->flags & OBF_LOCKED) continue;
			edit_damage_obj(e, e->sel[i]);
			obj_move(e->sel[i], dx, dy);
			edit_damage_obj(e, e->sel[i]);
		}
		return;
	}
	case DRAG_HANDLE: {
		obj_t *o = e->sel[0];
		int x0, y0, x1, y1, k;
		if (e->bx == e->ax && e->by == e->ay) return;
		edit_checkpoint(e, false);
		edit_damage_obj(e, o);
		if (e->handle >= 8 && o->kind == OB_POLY && e->handle - 8 < o->npts) {
			o->pts[2 * (e->handle - 8)] = (int16_t)e->bx;
			o->pts[2 * (e->handle - 8) + 1] = (int16_t)e->by;
			obj_fit_box(o);
			edit_damage_obj(e, o);
			return;
		}
		edit_drag_box(e, &x0, &y0, &x1, &y1, &k);
		(void)k;
		if (o->kind == OB_LINE) obj_set_box(o, x0, y0, x1 - x0, y1 - y0);
		else obj_set_box(o, x0, y0, imax(x1 - x0, 1), imax(y1 - y0, 1));
		edit_damage_obj(e, o);
		return;
	}
	case DRAG_BAND: {
		int x0, y0, x1, y1, k, i;
		edit_drag_box(e, &x0, &y0, &x1, &y1, &k);
		if (x1 - x0 < 2 && y1 - y0 < 2) return;
		for (i = 0; i < c->nobj; i++) {
			int a, b, cc, d;
			obj_bounds(c->obj[i], &a, &b, &cc, &d);
			if (a >= x0 && cc <= x1 && b >= y0 && d <= y1 && !edit_is_selected(e, c->obj[i]))
				edit_select(e, c->obj[i], true);
		}
		return;
	}
	case DRAG_CREATE: {
		obj_t *o = new_for_tool(e, e->tool);
		int x0 = imin(e->ax, e->bx), y0 = imin(e->ay, e->by);
		int w = iabs(e->bx - e->ax), h = iabs(e->by - e->ay);
		bool tiny = w < 4 && h < 4;
		if (!o) return;
		if (o->kind == OB_LINE) {
			obj_t *a, *b;
			if (tiny) { obj_free(o); msg(e, "Drag to draw a line"); return; }
			obj_set_box(o, e->ax, e->ay, e->bx - e->ax, e->by - e->ay);
			// from one shape to another: a connector, which follows them
			a = shape_at(e, e->ax, e->ay);
			b = shape_at(e, e->bx, e->by);
			if (a && b && a != b) {
				obj_make_connector(o, a->name, b->name, false);
				msg2(e, "Connected; /route elbow bends it, /connect none lets go: ", b->name);
			}
		} else if (tiny) {
			// a click: the default size, from the click
			obj_set_box(o, e->ax, e->ay, o->w, o->h);
		} else {
			obj_set_box(o, x0, y0, imax(w, 4), imax(h, 4));
		}
		add_obj(e, o, true);
		if (o->kind == OB_TEXT) {
			edit_text_begin(e, o, -1, -1);
			e->tool = TOOL_SELECT;
		}
		return;
	}
	default:
		return;
	}
}

void edit_finish_poly(editor_t *e) {
	obj_t *o;
	int i, x0, y0, x1, y1;
	bool closed = false;
	if (e->npoly < 2) { e->npoly = 0; edit_damage_all(e); return; }
	// the last click on the first point means closed
	if (e->npoly >= 3 && iabs(e->poly[2 * e->npoly - 2] - e->poly[0]) <= HANDLE_TOL &&
		iabs(e->poly[2 * e->npoly - 1] - e->poly[1]) <= HANDLE_TOL) {
		closed = true;
		e->npoly--;
	} else if (e->npoly >= 3) {
		closed = iabs(e->bx - e->poly[0]) <= HANDLE_TOL && iabs(e->by - e->poly[1]) <= HANDLE_TOL;
	}
	o = obj_new(OB_POLY);
	if (!o) return;
	o->pts = pool_alloc((size_t)e->npoly * 4);
	if (!o->pts) { obj_free(o); return; }
	memcpy(o->pts, e->poly, (size_t)e->npoly * 4);
	o->npts = (uint16_t)e->npoly;
	o->closed = closed ? 1 : 0;
	x0 = x1 = o->pts[0];
	y0 = y1 = o->pts[1];
	for (i = 1; i < o->npts; i++) {
		x0 = imin(x0, o->pts[2 * i]); x1 = imax(x1, o->pts[2 * i]);
		y0 = imin(y0, o->pts[2 * i + 1]); y1 = imax(y1, o->pts[2 * i + 1]);
	}
	o->x = (int16_t)x0; o->y = (int16_t)y0;
	o->w = (int16_t)imax(x1 - x0, 1); o->h = (int16_t)imax(y1 - y0, 1);
	e->npoly = 0;
	add_obj(e, o, true);
	edit_damage_all(e);
}

void edit_cancel(editor_t *e) {
	if (e->npoly) { e->npoly = 0; edit_damage_all(e); }
	e->drag = DRAG_NONE;
	if (e->text_obj) edit_text_end(e);
}

// -- text ----------------------------------------------------------------------

static text_layout_t g_lay;		// one at a time; big

static int caret_line(const text_layout_t *L, uint32_t caret) {
	int i;
	for (i = 0; i < L->n; i++) {
		uint32_t end = L->line[i].start + L->line[i].len;
		uint32_t next = (i + 1 < L->n) ? L->line[i + 1].start : 0xFFFFFFFFu;
		if (caret < next || i == L->n - 1) {
			(void)end;
			return i;
		}
	}
	return 0;
}

// x (units, 24.8) of the caret on its line
static zc_fix caret_x(const obj_t *o, const text_layout_t *L, int line, uint32_t caret) {
	uint32_t s = L->line[line].start, n = caret - s;
	if (caret < s) n = 0;
	if (n > L->line[line].len) n = L->line[line].len;
	return render_line_x(o, L, line) + zc_text_width(&L->st, o->text + s, n);
}

// The byte in line `line` whose position is nearest x.
static uint32_t index_at_x(const obj_t *o, const text_layout_t *L, int line, zc_fix x) {
	const char *t = o->text;
	uint32_t s = L->line[line].start, e = s + L->line[line].len, i = s, best = s;
	zc_fix cx = render_line_x(o, L, line), bestd = 0x7FFFFFFF;
	for (;;) {
		zc_fix d = x > cx ? x - cx : cx - x;
		if (d < bestd) { bestd = d; best = i; }
		if (i >= e) break;
		{
			const char *p = t + i;
			uint32_t cp = z_utf8_next(&p, t + e);
			cx += zc_text_advance(&L->st, cp);
			i = (uint32_t)(p - t);
		}
	}
	return best;
}

void edit_text_begin(editor_t *e, obj_t *o, int x, int y) {
	if (!o->text && !obj_set_text(o, "", 0)) return;
	e->text_obj = o;
	e->caret_goal = -1;
	render_text_layout(e->d, o, &g_lay);
	if (x < 0 || g_lay.n == 0) {
		e->caret = (uint32_t)strlen(o->text);
	} else {
		int line = (int)((ZC_I(y) - g_lay.top) / (g_lay.line_h ? g_lay.line_h : 1));
		if (ZC_I(y) < g_lay.top) line = 0;
		if (line >= g_lay.n) line = g_lay.n - 1;
		if (line < 0) line = 0;
		e->caret = index_at_x(o, &g_lay, line, ZC_I(x));
	}
	edit_damage_obj(e, o);
}

void edit_text_end(editor_t *e) {
	if (!e->text_obj) return;
	edit_damage_obj(e, e->text_obj);
	e->text_obj = NULL;
}

void edit_caret(const editor_t *e, zc_fix *x, zc_fix *y, zc_fix *h) {
	const obj_t *o = e->text_obj;
	int line;
	if (!o) { *x = *y = *h = 0; return; }
	render_text_layout(e->d, o, &g_lay);
	if (g_lay.n == 0) {
		*x = render_line_x(o, &g_lay, 0);
		*y = g_lay.top;
		*h = g_lay.line_h;
		return;
	}
	line = caret_line(&g_lay, e->caret);
	*x = caret_x(o, &g_lay, line, e->caret);
	*y = g_lay.top + g_lay.line_h * line;
	*h = g_lay.line_h;
}

static bool typing_started;

static void text_change(editor_t *e) {
	if (!typing_started) {
		edit_checkpoint(e, false);
		typing_started = true;
	}
	edit_damage_obj(e, e->text_obj);
	e->modified = true;
}

void edit_text_insert(editor_t *e, const char *s, uint32_t n) {
	obj_t *o = e->text_obj;
	uint32_t len;
	char *nt;
	if (!o || !n) return;
	text_change(e);
	len = (uint32_t)strlen(o->text);
	if (e->caret > len) e->caret = len;
	if (len + n > 16000) { msg(e, "That text box is full"); return; }
	nt = pool_alloc(len + n + 1);
	if (!nt) { msg(e, "Out of memory"); return; }
	memcpy(nt, o->text, e->caret);
	memcpy(nt + e->caret, s, n);
	memcpy(nt + e->caret + n, o->text + e->caret, len - e->caret + 1);
	pool_free(o->text);
	o->text = nt;
	e->caret += n;
	e->caret_goal = -1;
	edit_damage_obj(e, o);
}

static void text_remove(editor_t *e, uint32_t a, uint32_t b) {
	obj_t *o = e->text_obj;
	uint32_t len = (uint32_t)strlen(o->text);
	if (b > len) b = len;
	if (a >= b) return;
	text_change(e);
	memmove(o->text + a, o->text + b, len - b + 1);
	e->caret = a;
	e->caret_goal = -1;
	edit_damage_obj(e, o);
}

bool edit_text_key(editor_t *e, uint32_t k, uint8_t mods) {
	obj_t *o = e->text_obj;
	uint32_t len;
	bool ctrl = (mods & Z_KBD_MOD_CTRL) != 0;
	if (!o) return false;
	len = (uint32_t)strlen(o->text);
	if (e->caret > len) e->caret = len;
	switch (k) {
	case Z_KEY_LEFT:
		if (e->caret) e->caret = (uint32_t)z_utf8_prev_off(o->text, (int)len, (int)e->caret);
		e->caret_goal = -1;
		typing_started = false;
		break;
	case Z_KEY_RIGHT:
		if (e->caret < len) e->caret = (uint32_t)z_utf8_next_off(o->text, (int)len, (int)e->caret);
		e->caret_goal = -1;
		typing_started = false;
		break;
	case Z_KEY_UP: case Z_KEY_DOWN: case Z_KEY_HOME: case Z_KEY_END: {
		int line;
		zc_fix x;
		render_text_layout(e->d, o, &g_lay);
		if (!g_lay.n) break;
		typing_started = false;
		if (ctrl && k == Z_KEY_HOME) { e->caret = 0; break; }
		if (ctrl && k == Z_KEY_END) { e->caret = len; break; }
		line = caret_line(&g_lay, e->caret);
		if (k == Z_KEY_HOME) { e->caret = g_lay.line[line].start; e->caret_goal = -1; break; }
		if (k == Z_KEY_END) {
			e->caret = g_lay.line[line].start + g_lay.line[line].len;
			e->caret_goal = -1;
			break;
		}
		x = e->caret_goal >= 0 ? ZC_I(e->caret_goal) : caret_x(o, &g_lay, line, e->caret);
		if (e->caret_goal < 0) e->caret_goal = x >> ZC_FRAC;
		line += (k == Z_KEY_UP) ? -1 : 1;
		if (line < 0) { e->caret = 0; break; }
		if (line >= g_lay.n) { e->caret = len; break; }
		e->caret = index_at_x(o, &g_lay, line, x);
		break;
	}
	case 0x7f:		// Backspace
		if (e->caret) text_remove(e, (uint32_t)z_utf8_prev_off(o->text, (int)len, (int)e->caret), e->caret);
		break;
	case Z_KEY_DELETE:
		if (e->caret < len) text_remove(e, e->caret, (uint32_t)z_utf8_next_off(o->text, (int)len, (int)e->caret));
		break;
	case 0x0d:
		edit_text_insert(e, "\n", 1);
		break;
	case 0x09:
		edit_text_insert(e, "    ", 4);
		break;
	default:
		if (ctrl || (mods & (Z_KBD_MOD_LALT | Z_KBD_MOD_LGUI | Z_KBD_MOD_RGUI))) return false;
		if (Z_KEY_IS_TEXT(k) && !Z_KEY_IS_NAMED(k)) {
			char u[4];
			int n = z_utf8_put(k, u);
			if (n > 0) edit_text_insert(e, u, (uint32_t)n);
			return true;
		}
		return false;
	}
	edit_damage_obj(e, o);
	return true;
}

// -- objects ---------------------------------------------------------------------

void edit_nudge(editor_t *e, int dx, int dy) {
	int i;
	if (!e->nsel) return;
	edit_checkpoint(e, false);
	for (i = 0; i < e->nsel; i++) {
		if (e->sel[i]->flags & OBF_LOCKED) continue;
		edit_damage_obj(e, e->sel[i]);
		obj_move(e->sel[i], dx, dy);
		edit_damage_obj(e, e->sel[i]);
	}
}

void edit_delete(editor_t *e) {
	card_t *c = edit_card(e);
	int i, n = 0;
	if (!c || !e->nsel) return;
	edit_checkpoint(e, false);
	for (i = 0; i < e->nsel; i++) {
		int k = card_index_of(c, e->sel[i]);
		if (k < 0 || (e->sel[i]->flags & OBF_LOCKED)) continue;
		edit_damage_obj(e, e->sel[i]);
		obj_free(card_take(c, k));
		n++;
	}
	e->nsel = 0;
	if (!n) msg(e, "Locked");
}

void edit_duplicate(editor_t *e) {
	card_t *c = edit_card(e);
	obj_t *made[SEL_MAX];
	static char old[SEL_MAX][OB_NAME_MAX];
	int i, n = 0;
	if (!c || !e->nsel) return;
	edit_checkpoint(e, false);
	for (i = 0; i < e->nsel; i++) {
		obj_t *o = obj_dup(e->sel[i]);
		if (!o) break;
		obj_move(o, 10, 10);
		o->flags &= (uint8_t)~OBF_LOCKED;
		memcpy(old[n], o->name, OB_NAME_MAX);
		card_unique_obj_name(c, o->kind, o->name, OB_NAME_MAX);
		if (!card_add(c, -1, o)) { obj_free(o); break; }
		made[n++] = o;
	}
	fix_copies(c, made, old, n);
	edit_select_none(e);
	for (i = 0; i < n; i++) select_one(e, made[i], true);
}

void edit_order(editor_t *e, int how) {
	card_t *c = edit_card(e);
	int i;
	if (!c || !e->nsel) return;
	edit_checkpoint(e, false);
	// each selected object, in its own stacking order
	if (how >= 2) {
		for (i = c->nobj - 1; i >= 0; i--) {
			obj_t *o = c->obj[i];
			if (!edit_is_selected(e, o)) continue;
			if (how == 3) { card_take(c, i); card_add(c, -1, o); }
			else if (i + 1 < c->nobj && !edit_is_selected(e, c->obj[i + 1])) {
				c->obj[i] = c->obj[i + 1];
				c->obj[i + 1] = o;
			}
		}
	} else {
		int placed = 0;
		for (i = 0; i < c->nobj; i++) {
			obj_t *o = c->obj[i];
			if (!edit_is_selected(e, o)) continue;
			if (how == 0) { card_take(c, i); card_add(c, placed++, o); }
			else if (i > 0 && !edit_is_selected(e, c->obj[i - 1])) {
				c->obj[i] = c->obj[i - 1];
				c->obj[i - 1] = o;
			}
		}
	}
	damage_sel(e);
}

// -- cards -----------------------------------------------------------------------

void edit_goto(editor_t *e, int card) {
	if (card < 0 || card >= e->d->ncard || card == e->cur) return;
	if (e->text_obj) edit_text_end(e);
	e->nsel = 0;
	e->npoly = 0;
	e->drag = DRAG_NONE;
	e->cur = card;
	e->on_bg = false;
	edit_damage_all(e);
}

void edit_new_card(editor_t *e, bool duplicate) {
	card_t *c;
	char nm[CARD_NAME_MAX];
	edit_checkpoint(e, true);
	deck_unique_card_name(e->d, nm, sizeof(nm));
	if (duplicate) {
		c = card_dup(e->d->card[e->cur]);
		if (c) {
			size_t i;
			for (i = 0; i < sizeof(c->name); i++) c->name[i] = nm[i];
		}
	} else {
		c = card_new(nm);
		// a new card shares the current one's background
		if (c) c->bg = e->d->card[e->cur]->bg;
	}
	if (!c || !deck_insert_card(e->d, e->cur + 1, c)) {
		card_free(c);
		msg(e, "Out of memory");
		return;
	}
	edit_goto(e, e->cur + 1);
	msg2(e, "New card: ", c->name);
}

void edit_delete_card(editor_t *e) {
	if (e->d->ncard <= 1) { msg(e, "A deck keeps at least one card"); return; }
	edit_checkpoint(e, true);
	card_free(deck_remove_card(e->d, e->cur));
	if (e->cur >= e->d->ncard) e->cur = e->d->ncard - 1;
	e->nsel = 0;
	e->text_obj = NULL;
	edit_damage_all(e);
	msg(e, "Card deleted (Ctrl+Z brings it back)");
}

void edit_move_card(editor_t *e, int from, int to) {
	if (from == to || from < 0 || to < 0 || from >= e->d->ncard || to >= e->d->ncard) return;
	edit_checkpoint(e, true);
	deck_move_card(e->d, from, to);
	if (e->cur == from) e->cur = to;
	else if (from < e->cur && to >= e->cur) e->cur--;
	else if (from > e->cur && to <= e->cur) e->cur++;
	edit_damage_all(e);
}

// -- the clipboard ---------------------------------------------------------------

bool edit_copy(editor_t *e, sbuf_t *out) {
	if (!e->nsel) return false;
	return deckio_write_objs(out, e->d, e->sel, e->nsel);
}

int edit_paste(editor_t *e, const char *text, uint32_t len) {
	card_t *c = edit_card(e);
	int before, n, i;
	if (!c) return 0;
	if (e->text_obj) {
		// into the text being typed: as text, whatever it is
		edit_text_insert(e, text, len);
		return 0;
	}
	edit_checkpoint(e, false);
	before = c->nobj;
	n = deckio_read_objs(e->d, c, text, len);
	if (n < 0) {
		// plain text: a new text box with it
		obj_t *o = obj_new(OB_TEXT);
		if (!o || !obj_set_text(o, text, len)) { obj_free(o); return 0; }
		obj_set_box(o, 20, 20, e->d->page.gw - 40, 40);
		card_unique_obj_name(c, OB_TEXT, o->name, OB_NAME_MAX);
		if (!card_add(c, -1, o)) { obj_free(o); return 0; }
		n = 1;
	}
	edit_select_none(e);
	{
		static char old[SEL_MAX][OB_NAME_MAX];
		obj_t *made[SEL_MAX];
		int m = 0;
		for (i = before; i < c->nobj && m < SEL_MAX; i++) {
			memcpy(old[m], c->obj[i]->name, OB_NAME_MAX);
			made[m++] = c->obj[i];
		}
		for (i = 0; i < m; i++) {
			int k;
			for (k = 0; k < before; k++)
				if (!strcmp(c->obj[k]->name, made[i]->name)) {
					card_unique_obj_name(c, made[i]->kind, made[i]->name, OB_NAME_MAX);
					break;
				}
		}
		fix_copies(c, made, old, m);
	}
	for (i = before; i < c->nobj; i++) {
		obj_t *o = c->obj[i];
		int k;
		// one pasted where its original still is moves aside
		for (k = 0; k < before; k++)
			if (c->obj[k]->x == o->x && c->obj[k]->y == o->y && c->obj[k]->kind == o->kind) {
				obj_move(o, 10, 10);
				break;
			}
		select_one(e, o, true);
	}
	return n;
}

// -- commands ----------------------------------------------------------------------

#define ARGS_MAX 12

typedef struct {
	char	*v[ARGS_MAX];
	int		n;
	char	buf[256];
} args_t;

static void split(const char *line, args_t *a) {
	size_t i = 0, k = 0;
	a->n = 0;
	while (line[i] && k < sizeof(a->buf) - 1) {
		while (line[i] == ' ') i++;
		if (!line[i] || a->n == ARGS_MAX) break;
		a->v[a->n++] = &a->buf[k];
		if (line[i] == '"') {
			i++;
			while (line[i] && line[i] != '"' && k < sizeof(a->buf) - 1) a->buf[k++] = line[i++];
			if (line[i] == '"') i++;
		} else {
			while (line[i] && line[i] != ' ' && k < sizeof(a->buf) - 1) {
				char ch = line[i++];
				a->buf[k++] = ch;
			}
		}
		a->buf[k++] = 0;
	}
	a->buf[k < sizeof(a->buf) ? k : sizeof(a->buf) - 1] = 0;
}

// everything after the first word, as typed (for names and text)
static const char *rest_of(const char *line) {
	while (*line == ' ') line++;
	while (*line && *line != ' ') line++;
	while (*line == ' ') line++;
	return line;
}

static bool is_num(const char *s) {
	int i = (s[0] == '-') ? 1 : 0;
	if (!s[i]) return false;
	for (; s[i]; i++) if (s[i] < '0' || s[i] > '9') return false;
	return true;
}

static int num(const char *s) {
	int v = 0, i = (s[0] == '-') ? 1 : 0;
	for (; s[i] >= '0' && s[i] <= '9' && v < 1000000; i++) v = v * 10 + (s[i] - '0');
	return s[0] == '-' ? -v : v;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// Applies fn to every selected object, with one checkpoint; false (and
// a message) when nothing is selected.
static bool each_sel(editor_t *e, void (*fn)(editor_t *, obj_t *, void *), void *arg) {
	int i;
	if (!e->nsel) { msg(e, "Select something first"); return false; }
	edit_checkpoint(e, false);
	for (i = 0; i < e->nsel; i++) {
		edit_damage_obj(e, e->sel[i]);
		fn(e, e->sel[i], arg);
		if (e->sel[i]->img) render_forget(e->sel[i]);
		edit_damage_obj(e, e->sel[i]);
	}
	return true;
}

typedef struct { args_t *a; int i0, i1, i2; } cmd_arg_t;

static void set_font(editor_t *e, obj_t *o, void *p) {
	cmd_arg_t *c = p;
	args_t *a = c->a;
	int i;
	for (i = 1; i < a->n; i++) {
		const char *w = a->v[i];
		if (is_num(w)) o->size = (uint8_t)clampi(num(w), 4, 255);
		else if (!strcmp(w, "bold")) o->style |= ST_BOLD;
		else if (!strcmp(w, "italic")) o->style |= ST_ITALIC;
		else if (!strcmp(w, "underline")) o->style |= ST_UNDERLINE;
		else if (!strcmp(w, "plain")) o->style = 0;
		else o->font = (uint8_t)deck_font_index(e->d, w);
	}
}

static void set_size(editor_t *e, obj_t *o, void *p) { (void)e; o->size = (uint8_t)((cmd_arg_t *)p)->i0; }
static void set_style(editor_t *e, obj_t *o, void *p) {
	cmd_arg_t *c = p;
	(void)e;
	if (c->i0 < 0) o->style = 0; else o->style ^= (uint8_t)c->i0;
}
static void set_align(editor_t *e, obj_t *o, void *p) {
	cmd_arg_t *c = p;
	(void)e;
	if (c->i0 >= 0) o->align = (uint8_t)c->i0;
	if (c->i1 >= 0) o->valign = (uint8_t)c->i1;
}
static void set_fill(editor_t *e, obj_t *o, void *p) {
	cmd_arg_t *c = p;
	(void)e;
	o->fill = (uint8_t)c->i0;
	o->fill_v = (uint8_t)c->i1;
}
static void set_line(editor_t *e, obj_t *o, void *p) {
	cmd_arg_t *c = p;
	(void)e;
	o->line_w = (uint8_t)c->i0;
	if (c->i1 >= 0) o->line_tone = (uint8_t)c->i1;
}
static void set_color(editor_t *e, obj_t *o, void *p) { (void)e; o->text_tone = (uint8_t)((cmd_arg_t *)p)->i0; }
static void set_radius(editor_t *e, obj_t *o, void *p) { (void)e; o->radius = (uint8_t)((cmd_arg_t *)p)->i0; }
static void set_pad(editor_t *e, obj_t *o, void *p) { (void)e; o->pad = (uint8_t)((cmd_arg_t *)p)->i0; }
static void set_arrows(editor_t *e, obj_t *o, void *p) { (void)e; o->arrows = (uint8_t)((cmd_arg_t *)p)->i0; }
static void set_flag(editor_t *e, obj_t *o, void *p) {
	cmd_arg_t *c = p;
	(void)e;
	if (c->i1) o->flags |= (uint8_t)c->i0; else o->flags &= (uint8_t)~c->i0;
}
static void set_contrast(editor_t *e, obj_t *o, void *p) { (void)e; o->contrast = (int8_t)((cmd_arg_t *)p)->i0; }
static void set_bright(editor_t *e, obj_t *o, void *p) { (void)e; o->bright = (int8_t)((cmd_arg_t *)p)->i0; }

// The middle of the selection's box.
static void sel_middle(const editor_t *e, int *cx, int *cy) {
	int i, a = 0, b = 0, c = 0, d = 0;
	for (i = 0; i < e->nsel; i++) {
		int p, q, r, s;
		obj_bounds(e->sel[i], &p, &q, &r, &s);
		if (!i) { a = p; b = q; c = r; d = s; }
		else { a = imin(a, p); b = imin(b, q); c = imax(c, r); d = imax(d, s); }
	}
	*cx = (a + c) / 2;
	*cy = (b + d) / 2;
}

static void flip_sel(editor_t *e, bool down) {
	int cx, cy, i, k;
	sel_middle(e, &cx, &cy);
	edit_checkpoint(e, false);
	for (i = 0; i < e->nsel; i++) {
		obj_t *o = e->sel[i];
		if (o->flags & OBF_LOCKED) continue;
		edit_damage_obj(e, o);
		if (o->kind == OB_LINE) {
			if (!down) { o->x = (int16_t)(2 * cx - o->x); o->w = (int16_t)-o->w; }
			else { o->y = (int16_t)(2 * cy - o->y); o->h = (int16_t)-o->h; }
		} else if (o->kind == OB_POLY) {
			for (k = 0; k < o->npts; k++) {
				if (!down) o->pts[2 * k] = (int16_t)(2 * cx - o->pts[2 * k]);
				else o->pts[2 * k + 1] = (int16_t)(2 * cy - o->pts[2 * k + 1]);
			}
			obj_fit_box(o);
		} else {
			if (!down) o->x = (int16_t)(2 * cx - o->x - o->w);
			else o->y = (int16_t)(2 * cy - o->y - o->h);
			// a shape that is not symmetric shows it
			o->flags ^= (uint8_t)(down ? OBF_FLIPY : OBF_FLIPX);
		}
		edit_damage_obj(e, o);
	}
}

static void rotate_sel(editor_t *e, bool left) {
	int cx, cy, i, k;
	sel_middle(e, &cx, &cy);
	edit_checkpoint(e, false);
#define ROT_X(x, y) (left ? cx + ((y) - cy) : cx - ((y) - cy))
#define ROT_Y(x, y) (left ? cy - ((x) - cx) : cy + ((x) - cx))
	for (i = 0; i < e->nsel; i++) {
		obj_t *o = e->sel[i];
		if (o->flags & OBF_LOCKED) continue;
		edit_damage_obj(e, o);
		if (o->kind == OB_LINE) {
			int ax = o->x, ay = o->y, bx = o->x + o->w, by = o->y + o->h;
			int nax = ROT_X(ax, ay), nay = ROT_Y(ax, ay), nbx = ROT_X(bx, by), nby = ROT_Y(bx, by);
			o->x = (int16_t)nax; o->y = (int16_t)nay;
			o->w = (int16_t)(nbx - nax); o->h = (int16_t)(nby - nay);
		} else if (o->kind == OB_POLY) {
			for (k = 0; k < o->npts; k++) {
				int x = o->pts[2 * k], y = o->pts[2 * k + 1];
				o->pts[2 * k] = (int16_t)ROT_X(x, y);
				o->pts[2 * k + 1] = (int16_t)ROT_Y(x, y);
			}
			obj_fit_box(o);
		} else {
			// the box turned about its middle: its middle moves, its
			// sides swap
			int mx = o->x + o->w / 2, my = o->y + o->h / 2;
			int nmx = ROT_X(mx, my), nmy = ROT_Y(mx, my), w = o->w, h = o->h;
			o->w = (int16_t)h; o->h = (int16_t)w;
			o->x = (int16_t)(nmx - h / 2); o->y = (int16_t)(nmy - w / 2);
		}
		edit_damage_obj(e, o);
	}
#undef ROT_X
#undef ROT_Y
}

static void set_smooth(editor_t *e, obj_t *o, void *p) {
	(void)e;
	if (o->kind == OB_POLY) o->smooth = (uint8_t)((cmd_arg_t *)p)->i0;
}

static void set_group(editor_t *e, obj_t *o, void *p) {
	cmd_arg_t *c = p;
	(void)e;
	strncpy(o->group, c->a->v[1], sizeof(o->group) - 1);
	o->group[sizeof(o->group) - 1] = 0;
}
static void set_range(editor_t *e, obj_t *o, void *p) {
	cmd_arg_t *c = p;
	(void)e;
	o->vmin = (int16_t)c->i0;
	o->vmax = (int16_t)c->i1;
	if (o->value < o->vmin) o->value = o->vmin;
	if (o->value > o->vmax) o->value = o->vmax;
}
static void set_value(editor_t *e, obj_t *o, void *p) {
	cmd_arg_t *c = p;
	(void)e;
	o->value = (int16_t)c->i0;
	if (o->kind == OB_SLIDER) {
		if (o->value < o->vmin) o->value = o->vmin;
		if (o->value > o->vmax) o->value = o->vmax;
	} else if (o->kind == OB_CHECK || o->kind == OB_RADIO) {
		o->value = o->value ? 1 : 0;
	}
}

// A control in the middle of the card, selected: "button Next",
// "list red, green, blue", "slider 0 10".
static bool add_control(editor_t *e, int kind, const char *rest, const args_t *a) {
	obj_t *o = obj_new(kind);
	int gw = e->d->page.gw, gh = e->d->page.gh;
	if (!o) { msg(e, "Out of memory"); return false; }
	if (kind == OB_SLIDER) {
		if (a->n >= 3 && is_num(a->v[1]) && is_num(a->v[2]) && num(a->v[2]) > num(a->v[1])) {
			o->vmin = (int16_t)clampi(num(a->v[1]), -9999, 9998);
			o->vmax = (int16_t)clampi(num(a->v[2]), o->vmin + 1, 9999);
		}
		o->value = o->vmin;
	} else if (kind == OB_LIST) {
		// "a, b, c": a line each
		char b[200];
		int k = 0;
		const char *p = *rest ? rest : "One, Two, Three";
		while (*p && k < (int)sizeof(b) - 1) {
			if (*p == ',') { b[k++] = '\n'; p++; while (*p == ' ') p++; continue; }
			b[k++] = *p++;
		}
		b[k] = 0;
		obj_set_text(o, b, (uint32_t)k);
	} else if (OB_IS_FLOW(kind)) {
		if (*rest) obj_set_text(o, rest, (uint32_t)strlen(rest));
	} else {
		static const char *const def[] = { "OK", "Check", "Option", "", "", "" };
		const char *t = *rest ? rest : def[kind - OB_BUTTON];
		obj_set_text(o, t, (uint32_t)strlen(t));
	}
	o->x = (int16_t)((gw - o->w) / 2);
	o->y = (int16_t)((gh - o->h) / 2);
	if (e->snap > 1) {
		o->x = (int16_t)(o->x / e->snap * e->snap);
		o->y = (int16_t)(o->y / e->snap * e->snap);
	}
	o->font = (uint8_t)deck_font_index(e->d, "sans");
	add_obj(e, o, true);
	msg2(e, "Placed: ", obj_kind_name(kind));
	return true;
}

static void lineup(editor_t *e, const char *how) {
	int i, a, b, c, d;
	if (e->nsel < 2) { msg(e, "Select two or more to line up"); return; }
	obj_bounds(e->sel[0], &a, &b, &c, &d);
	for (i = 1; i < e->nsel; i++) {
		int p, q, r, s;
		obj_bounds(e->sel[i], &p, &q, &r, &s);
		a = imin(a, p); b = imin(b, q); c = imax(c, r); d = imax(d, s);
	}
	edit_checkpoint(e, false);
	for (i = 0; i < e->nsel; i++) {
		obj_t *o = e->sel[i];
		int p, q, r, s, dx = 0, dy = 0;
		if (o->flags & OBF_LOCKED) continue;
		obj_bounds(o, &p, &q, &r, &s);
		if (!strcmp(how, "left")) dx = a - p;
		else if (!strcmp(how, "right")) dx = c - r;
		else if (!strcmp(how, "center")) dx = (a + c) / 2 - (p + r) / 2;
		else if (!strcmp(how, "top")) dy = b - q;
		else if (!strcmp(how, "bottom")) dy = d - s;
		else if (!strcmp(how, "middle")) dy = (b + d) / 2 - (q + s) / 2;
		edit_damage_obj(e, o);
		obj_move(o, dx, dy);
		edit_damage_obj(e, o);
	}
}

// Changes the page; objects are scaled with the grid so the card keeps
// its layout.
static bool set_page(editor_t *e, const char *name, int orient) {
	zpage_t np;
	int ow = e->d->page.gw, oh = e->d->page.gh, i, k;
	bool land = orient < 0 ? e->d->page.landscape : orient == 1;
	if (!zpage_set(&np, name, land)) { msg2(e, "Not a page size: ", name); return false; }
	edit_checkpoint(e, true);
	for (k = 0; k < e->d->ncard + e->d->nbg; k++) {
		card_t *c = k < e->d->ncard ? e->d->card[k] : e->d->bg[k - e->d->ncard];
		for (i = 0; i < c->nobj; i++) {
			obj_t *o = c->obj[i];
			int j;
			o->x = (int16_t)(o->x * np.gw / ow);
			o->y = (int16_t)(o->y * np.gh / oh);
			o->w = (int16_t)(o->w * np.gw / ow);
			o->h = (int16_t)(o->h * np.gh / oh);
			for (j = 0; j < o->npts; j++) {
				o->pts[2 * j] = (int16_t)(o->pts[2 * j] * np.gw / ow);
				o->pts[2 * j + 1] = (int16_t)(o->pts[2 * j + 1] * np.gh / oh);
			}
			if (o->img) render_forget(o);
		}
	}
	e->d->page = np;
	edit_damage_all(e);
	return true;
}

static const char *const cmd_help[] = {
	"new                 a new card after this one",
	"dup card            a copy of this card",
	"delete card         this card (undo brings it back)",
	"name NAME           names the selection, or the card if nothing is selected",
	"go N | NAME | first | last | next | prev",
	"background NAME | none   this card's background (made if new)",
	"edit background     edit the background instead (again: the card)",
	"page SIZE [landscape | portrait]   classic 4:3 16:9 letter a4 4x6 3x5 ...",
	"title TEXT          the deck's title",
	"font NAME [SIZE] [bold] [italic] [underline] [plain]",
	"size N              text size",
	"bold | italic | underline | plain",
	"align left | center | right | top | middle | bottom",
	"color N             text tone, 0 white .. 16 black",
	"fill none | N | shade N   pattern 0-15, or a shade 0-16",
	"line W [TONE]       frame or stroke width (0 none) and tone",
	"radius N            corner radius of a rounded rectangle",
	"arrows none | start | end | both",
	"pad N               space inside a text box",
	"lineup left | center | right | top | middle | bottom",
	"front | back | forward | backward",
	"hide | unhide | lock | unlock",
	"contrast N | bright N   an image's, -100..100",
	"notes TEXT          the speaker's notes for this card",
	"transition cut | wipe | push | iris | dissolve [MS]",
	"advance SECONDS     go on by itself in a show (0: never)",
	"grid N              snap to N units (0: off)",
	"zoom 100 | 200 | 400 | fit   (Ctrl+= Ctrl+- Ctrl+0)",
	"select all | delete | duplicate",
	"undo | redo",
	"image               place a picture (a file dialog)",
	"print [pdf | ps] [this | N-M] [4up] [cut] [frame] [paper a4] [tray rear] [actual] [copies N]",
	"print setup ...     only the settings, which the deck keeps",
	"export svg [all]    this card (or every card) as SVG, for a drawing program or a web page",
	"import FILE.svg     a drawing's outlines as polygons on this card",
	"button LABEL        a button (its action: action ...)",
	"check LABEL | radio LABEL   a check box, a radio button",
	"field               a field to type into",
	"slider [MIN MAX]    a slider",
	"list A, B, C        a list to choose from",
	"diamond | slant | pill | document | cylinder [TEXT]   flowchart shapes",
	"connect [elbow] | none   an arrow from the first selected shape to the second; or let go",
	"route straight | elbow   a connector's way",
	"smooth | sharp      a polygon's corners: a curve through them, or straight",
	"action go next | show NAME | speak TEXT ...   what a click does; nothing selected: on arriving",
	"text WORDS          a check's or radio's label; a list's items: A, B, C",
	"group NAME          radios that exclude each other",
	"group | ungroup     the selection as one (Ctrl+G, Ctrl+Shift+G)",
	"flip across | down   mirror the selection, about its middle",
	"rotate [left]       a quarter turn, right (or left), about the selection's middle",
	"range MIN MAX | value N   a slider's range, a control's value",
	"app on | off        the deck opens as an app (F7: edit it)",
	"loop on | off       the show goes round",
	"script [deck | card]   edit a script in text: the deck's, the card's, or the selection's action",
	"(go 'next) ...      Scheme, run at once: the message box",
	"show | open | save | save as | new deck | help | tour",
};

int edit_command_count(void) { return (int)(sizeof(cmd_help) / sizeof(cmd_help[0])); }
const char *edit_command_help(int i) { return (i >= 0 && i < edit_command_count()) ? cmd_help[i] : ""; }

bool edit_command(editor_t *e, const char *line, const char **app) {
	args_t a;
	cmd_arg_t ca;
	const char *w;
	static const char *app_words[] = {
		"show", "open", "save", "image", "print", "help", "tour", "quit", "zoom", "script",
		"export", "import", NULL
	};
	int i;

	*app = NULL;
	e->msg[0] = 0;
	while (*line == '/' || *line == ' ') line++;
	split(line, &a);
	if (a.n == 0) return false;
	w = a.v[0];
	memset(&ca, 0, sizeof(ca));
	ca.a = &a;
	ca.i1 = -1;

	for (i = 0; app_words[i]; i++) {
		if (!strcmp(w, app_words[i])) {
			// "save as", "new deck" handled by the app too
			*app = app_words[i];
			return false;
		}
	}
	if (!strcmp(w, "new")) {
		if (a.n > 1 && !strcmp(a.v[1], "deck")) { *app = "new deck"; return false; }
		edit_new_card(e, false);
		return true;
	}
	if (!strcmp(w, "dup") || !strcmp(w, "duplicate")) {
		if (a.n > 1 && !strcmp(a.v[1], "card")) edit_new_card(e, true);
		else edit_duplicate(e);
		return true;
	}
	if (!strcmp(w, "delete")) {
		if (a.n > 1 && !strcmp(a.v[1], "card")) edit_delete_card(e);
		else edit_delete(e);
		return true;
	}
	if (!strcmp(w, "undo")) return edit_undo(e);
	if (!strcmp(w, "redo")) return edit_redo(e);
	if (!strcmp(w, "select")) { edit_select_all(e); return true; }
	if (!strcmp(w, "name")) {
		const char *n = rest_of(line);
		if (!*n) { msg(e, "name NAME"); return false; }
		if (e->nsel == 1) {
			card_t *c = edit_card(e);
			if (card_find_obj(c, n)) { msg2(e, "Already a name here: ", n); return false; }
			edit_checkpoint(e, false);
			strncpy(e->sel[0]->name, n, OB_NAME_MAX - 1);
			e->sel[0]->name[OB_NAME_MAX - 1] = 0;
		} else if (e->nsel == 0) {
			if (deck_find_card(e->d, n) >= 0) { msg2(e, "Already a card: ", n); return false; }
			edit_checkpoint(e, true);
			strncpy(e->d->card[e->cur]->name, n, CARD_NAME_MAX - 1);
			e->d->card[e->cur]->name[CARD_NAME_MAX - 1] = 0;
		} else {
			msg(e, "One object at a time");
			return false;
		}
		msg2(e, "Named ", n);
		return true;
	}
	if (!strcmp(w, "go")) {
		int t = -1;
		const char *n = rest_of(line);
		if (a.n < 2) { msg(e, "go N | NAME"); return false; }
		if (!strcmp(a.v[1], "first")) t = 0;
		else if (!strcmp(a.v[1], "last")) t = e->d->ncard - 1;
		else if (!strcmp(a.v[1], "next")) t = e->cur + 1;
		else if (!strcmp(a.v[1], "prev")) t = e->cur - 1;
		else if (is_num(a.v[1])) t = num(a.v[1]) - 1;
		else t = deck_find_card(e->d, n);
		if (t < 0 || t >= e->d->ncard) { msg(e, "No such card"); return false; }
		edit_goto(e, t);
		return true;
	}
	if (!strcmp(w, "background")) {
		const char *n = rest_of(line);
		card_t *c = e->d->card[e->cur];
		if (!*n) { msg(e, "background NAME | none"); return false; }
		edit_checkpoint(e, true);
		if (!strcmp(n, "none")) {
			c->bg = -1;
		} else {
			int b = deck_find_bg(e->d, n);
			if (b < 0) {
				card_t *nb = card_new(n);
				if (!nb || !deck_insert_bg(e->d, -1, nb)) { card_free(nb); msg(e, "Out of memory"); return false; }
				b = e->d->nbg - 1;
			}
			c->bg = b;
		}
		e->on_bg = false;
		e->nsel = 0;
		edit_damage_all(e);
		return true;
	}
	if (!strcmp(w, "edit")) {
		card_t *c = e->d->card[e->cur];
		if (c->bg < 0) { msg(e, "This card has no background (background NAME)"); return false; }
		e->on_bg = !e->on_bg;
		e->nsel = 0;
		e->text_obj = NULL;
		edit_damage_all(e);
		msg(e, e->on_bg ? "Editing the background" : "Editing the card");
		return true;
	}
	if (!strcmp(w, "page")) {
		int orient = -1;
		if (a.n < 2) { msg(e, "page SIZE [landscape|portrait]"); return false; }
		if (a.n > 2) orient = !strcmp(a.v[2], "portrait") ? 0 : 1;
		if (!strcmp(a.v[1], "landscape") || !strcmp(a.v[1], "portrait"))
			return set_page(e, e->d->page.size.name, !strcmp(a.v[1], "landscape"));
		return set_page(e, a.v[1], orient);
	}
	if (!strcmp(w, "title")) {
		edit_checkpoint(e, true);
		strncpy(e->d->title, rest_of(line), DECK_TITLE_MAX - 1);
		e->d->title[DECK_TITLE_MAX - 1] = 0;
		return true;
	}
	if (!strcmp(w, "font")) {
		if (a.n < 2) { msg(e, "font NAME [SIZE] [bold] [italic]"); return false; }
		if (!is_num(a.v[1]) && strcmp(a.v[1], "bold") && strcmp(a.v[1], "italic") &&
			strcmp(a.v[1], "plain") && strcmp(a.v[1], "underline") &&
			!render_font_exists(a.v[1])) { msg2(e, "No such font: ", a.v[1]); return false; }
		return each_sel(e, set_font, &ca);
	}
	if (!strcmp(w, "size")) {
		if (a.n < 2 || !is_num(a.v[1])) { msg(e, "size N"); return false; }
		ca.i0 = clampi(num(a.v[1]), 4, 255);
		return each_sel(e, set_size, &ca);
	}
	if (!strcmp(w, "bold") || !strcmp(w, "italic") || !strcmp(w, "underline") || !strcmp(w, "plain")) {
		ca.i0 = w[0] == 'b' ? ST_BOLD : w[0] == 'i' ? ST_ITALIC : w[0] == 'u' ? ST_UNDERLINE : -1;
		return each_sel(e, set_style, &ca);
	}
	if (!strcmp(w, "align")) {
		ca.i0 = -1;
		for (i = 1; i < a.n; i++) {
			if (!strcmp(a.v[i], "left")) ca.i0 = AL_LEFT;
			else if (!strcmp(a.v[i], "center") || !strcmp(a.v[i], "centre")) ca.i0 = AL_CENTER;
			else if (!strcmp(a.v[i], "right")) ca.i0 = AL_RIGHT;
			else if (!strcmp(a.v[i], "top")) ca.i1 = VA_TOP;
			else if (!strcmp(a.v[i], "middle")) ca.i1 = VA_MIDDLE;
			else if (!strcmp(a.v[i], "bottom")) ca.i1 = VA_BOTTOM;
		}
		if (ca.i0 < 0 && ca.i1 < 0) { msg(e, "align left|center|right|top|middle|bottom"); return false; }
		return each_sel(e, set_align, &ca);
	}
	if (!strcmp(w, "color") || !strcmp(w, "colour")) {
		if (a.n < 2 || !is_num(a.v[1])) { msg(e, "color 0..16"); return false; }
		ca.i0 = clampi(num(a.v[1]), 0, 16);
		return each_sel(e, set_color, &ca);
	}
	if (!strcmp(w, "fill")) {
		if (a.n < 2) { msg(e, "fill none | N | shade N"); return false; }
		if (!strcmp(a.v[1], "none")) { ca.i0 = FILL_NONE; ca.i1 = 0; }
		else if (!strcmp(a.v[1], "shade") && a.n > 2 && is_num(a.v[2])) { ca.i0 = FILL_SHADE; ca.i1 = clampi(num(a.v[2]), 0, 16); }
		else if (is_num(a.v[1])) { ca.i0 = FILL_PATTERN; ca.i1 = clampi(num(a.v[1]), 0, 15); }
		else { msg(e, "fill none | N | shade N"); return false; }
		return each_sel(e, set_fill, &ca);
	}
	if (!strcmp(w, "line")) {
		if (a.n < 2 || !is_num(a.v[1])) { msg(e, "line W [TONE]"); return false; }
		ca.i0 = clampi(num(a.v[1]), 0, 64);
		if (a.n > 2 && is_num(a.v[2])) ca.i1 = clampi(num(a.v[2]), 0, 16);
		return each_sel(e, set_line, &ca);
	}
	if (!strcmp(w, "radius") || !strcmp(w, "pad") || !strcmp(w, "contrast") || !strcmp(w, "bright")) {
		if (a.n < 2 || !is_num(a.v[1])) { msg2(e, w, " N"); return false; }
		ca.i0 = num(a.v[1]);
		if (w[0] == 'r') { ca.i0 = clampi(ca.i0, 0, 255); return each_sel(e, set_radius, &ca); }
		if (w[0] == 'p') { ca.i0 = clampi(ca.i0, 0, 100); return each_sel(e, set_pad, &ca); }
		ca.i0 = clampi(ca.i0, -100, 100);
		return each_sel(e, w[0] == 'c' ? set_contrast : set_bright, &ca);
	}
	if (!strcmp(w, "arrows")) {
		if (a.n < 2) { msg(e, "arrows none|start|end|both"); return false; }
		ca.i0 = !strcmp(a.v[1], "start") ? AR_START : !strcmp(a.v[1], "end") ? AR_END :
			!strcmp(a.v[1], "both") ? (AR_START | AR_END) : 0;
		return each_sel(e, set_arrows, &ca);
	}
	if (!strcmp(w, "hide") || !strcmp(w, "unhide") || !strcmp(w, "lock") || !strcmp(w, "unlock")) {
		ca.i0 = (w[0] == 'h' || w[2] == 'h') ? OBF_HIDDEN : OBF_LOCKED;
		ca.i1 = (w[0] == 'h' || w[0] == 'l') ? 1 : 0;
		return each_sel(e, set_flag, &ca);
	}
	if (!strcmp(w, "lineup")) {
		if (a.n < 2) { msg(e, "lineup left|center|right|top|middle|bottom"); return false; }
		lineup(e, a.v[1]);
		return true;
	}
	if (!strcmp(w, "front")) { edit_order(e, 3); return true; }
	if (!strcmp(w, "back")) { edit_order(e, 0); return true; }
	if (!strcmp(w, "forward")) { edit_order(e, 2); return true; }
	if (!strcmp(w, "backward")) { edit_order(e, 1); return true; }
	if (!strcmp(w, "notes")) {
		edit_checkpoint(e, false);
		obj_set_str(&e->d->card[e->cur]->notes, rest_of(line));
		msg(e, "Notes set");
		return true;
	}
	if (!strcmp(w, "transition")) {
		card_t *c = e->d->card[e->cur];
		static const char *tn[TR_KINDS] = { "cut", "wipe", "push", "iris", "dissolve" };
		int t = -1;
		if (a.n < 2) { msg(e, "transition cut|wipe|push [MS]"); return false; }
		for (i = 0; i < TR_KINDS; i++) if (!strcmp(a.v[1], tn[i])) t = i;
		if (t < 0) { msg(e, "transition cut|wipe|push [MS]"); return false; }
		edit_checkpoint(e, false);
		c->trans = (uint8_t)t;
		if (a.n > 2 && is_num(a.v[2])) c->trans_ms = (uint16_t)clampi(num(a.v[2]), 0, 10000);
		return true;
	}
	if (!strcmp(w, "advance")) {
		if (a.n < 2 || !is_num(a.v[1])) { msg(e, "advance SECONDS"); return false; }
		edit_checkpoint(e, false);
		e->d->card[e->cur]->advance_ms = (uint32_t)clampi(num(a.v[1]), 0, 3600) * 1000u;
		return true;
	}
	if (!strcmp(w, "grid")) {
		e->snap = (a.n > 1 && is_num(a.v[1])) ? clampi(num(a.v[1]), 0, 100) : 0;
		msg(e, e->snap > 1 ? "Snapping to the grid" : "Grid off");
		return true;
	}
	if (!strcmp(w, "smooth") || !strcmp(w, "sharp")) {
		ca.i0 = w[0] == 's' && w[1] == 'm';
		return each_sel(e, set_smooth, &ca);
	}
	if (!strcmp(w, "connect")) {
		// two shapes selected: an arrow from the first to the second;
		// a line selected: "connect none" lets it go
		bool elbow = a.n > 1 && !strcmp(a.v[1], "elbow");
		if (a.n > 1 && !strcmp(a.v[1], "none")) {
			int k;
			edit_checkpoint(e, false);
			for (k = 0; k < e->nsel; k++) { e->sel[k]->from[0] = 0; e->sel[k]->to[0] = 0; }
			msg(e, "Not connected: the line stays where it is");
			return true;
		}
		if (e->nsel != 2 || e->sel[0]->kind == OB_LINE || e->sel[1]->kind == OB_LINE) {
			msg(e, "Select two shapes (Shift+click), the first where the arrow starts");
			return false;
		}
		{
			obj_t *o = obj_new(OB_LINE), *from = e->sel[0], *to = e->sel[1];
			if (!o) { msg(e, "Out of memory"); return false; }
			o->arrows = AR_END;
			if (!obj_make_connector(o, from->name, to->name, elbow)) { obj_free(o); msg(e, "Out of memory"); return false; }
			edit_select_none(e);
			add_obj(e, o, true);
			edit_route(e);
			msg2(e, "Connected to ", to->name);
		}
		return true;
	}
	if (!strcmp(w, "route")) {
		bool elbow = a.n > 1 && !strcmp(a.v[1], "elbow");
		int k, done = 0;
		if (a.n < 2 || (strcmp(a.v[1], "elbow") && strcmp(a.v[1], "straight"))) { msg(e, "route straight | elbow"); return false; }
		edit_checkpoint(e, false);
		for (k = 0; k < e->nsel; k++) {
			obj_t *o = e->sel[k];
			if (!obj_is_connector(o)) continue;
			edit_damage_obj(e, o);
			if (obj_make_connector(o, o->from, o->to, elbow)) done++;
		}
		edit_route(e);
		edit_damage_all(e);
		if (!done) { msg(e, "Select a connector (a line from shape to shape)"); return false; }
		return true;
	}
	if (!strcmp(w, "text")) {
		// the selection's text: a label, a list's items ("a, b, c")
		const char *r = rest_of(line);
		char b[400];
		int k = 0;
		obj_t *o;
		if (e->nsel != 1) { msg(e, "Select one object"); return false; }
		o = e->sel[0];
		while (*r && k < (int)sizeof(b) - 1) {
			if (o->kind == OB_LIST && *r == ',') { b[k++] = '\n'; r++; while (*r == ' ') r++; continue; }
			b[k++] = *r++;
		}
		b[k] = 0;
		edit_checkpoint(e, false);
		edit_damage_obj(e, o);
		obj_set_text(o, b, (uint32_t)k);
		edit_damage_obj(e, o);
		return true;
	}
	// -- controls (Phase 3), diagram shapes (Phase 5) --
	{
		static const char *const kinds[] = { "button", "check", "radio", "field", "slider", "list",
			"diamond", "slant", "pill", "document", "cylinder", NULL };
		for (i = 0; kinds[i]; i++) if (!strcmp(w, kinds[i])) break;
		if (kinds[i]) return add_control(e, OB_BUTTON + i, rest_of(line), &a);
	}
	if (!strcmp(w, "action")) {
		const char *r = rest_of(line);
		char forms[400];
		char **field = NULL;
		if (e->nsel == 1) field = &e->sel[0]->action;
		else if (e->nsel == 0) field = &e->d->card[e->cur]->action;
		else { msg(e, "One object at a time (or none: the card's)"); return false; }
		if (!*r) {
			msg2(e, e->nsel ? "Its action: " : "On arriving: ", *field ? *field : "none");
			return false;
		}
		if (!strcmp(r, "none")) {
			edit_checkpoint(e, false);
			obj_set_str(field, NULL);
			msg(e, "No action");
			return true;
		}
		if (!act_from_words(r, forms, sizeof(forms))) { msg(e, forms); return false; }
		edit_checkpoint(e, false);
		obj_set_str(field, forms);
		msg2(e, e->nsel ? "Action: " : "On arriving: ", forms);
		return true;
	}
	if (!strcmp(w, "group")) {
		if (a.n < 2) {
			// the selection made one: clicked, moved, copied together
			card_t *c = edit_card(e);
			uint16_t top = 0;
			if (e->nsel < 2 || !c) { msg(e, "Select two or more to group (group NAME: radios)"); return false; }
			for (i = 0; i < c->nobj; i++) if (c->obj[i]->gid > top) top = c->obj[i]->gid;
			edit_checkpoint(e, false);
			for (i = 0; i < e->nsel; i++) e->sel[i]->gid = (uint16_t)(top + 1);
			msg(e, "Grouped (ungroup: Ctrl+Shift+G)");
			return true;
		}
		ca.a = &a;
		return each_sel(e, set_group, &ca);
	}
	if (!strcmp(w, "ungroup")) {
		if (!e->nsel) { msg(e, "Select a group"); return false; }
		edit_checkpoint(e, false);
		for (i = 0; i < e->nsel; i++) e->sel[i]->gid = 0;
		msg(e, "Ungrouped");
		return true;
	}
	if (!strcmp(w, "flip")) {
		bool down = a.n > 1 && (!strcmp(a.v[1], "down") || !strcmp(a.v[1], "vertical") || !strcmp(a.v[1], "v"));
		if (!e->nsel) { msg(e, "Select something to flip"); return false; }
		flip_sel(e, down);
		return true;
	}
	if (!strcmp(w, "rotate")) {
		if (!e->nsel) { msg(e, "Select something to turn"); return false; }
		rotate_sel(e, a.n > 1 && (!strcmp(a.v[1], "left") || !strcmp(a.v[1], "-90")));
		return true;
	}
	if (!strcmp(w, "range")) {
		if (a.n < 3 || !is_num(a.v[1]) || !is_num(a.v[2]) || num(a.v[2]) <= num(a.v[1])) {
			msg(e, "range MIN MAX");
			return false;
		}
		ca.i0 = clampi(num(a.v[1]), -9999, 9998);
		ca.i1 = clampi(num(a.v[2]), ca.i0 + 1, 9999);
		return each_sel(e, set_range, &ca);
	}
	if (!strcmp(w, "value")) {
		if (a.n < 2 || !is_num(a.v[1])) { msg(e, "value N"); return false; }
		ca.i0 = clampi(num(a.v[1]), -9999, 9999);
		return each_sel(e, set_value, &ca);
	}
	if (!strcmp(w, "app") || !strcmp(w, "loop")) {
		bool on = a.n < 2 || strcmp(a.v[1], "off");
		edit_checkpoint(e, true);
		if (w[0] == 'a') e->d->app = on; else e->d->loop = on;
		msg(e, w[0] == 'a' ? (on ? "Opens as an app (F7 edits it)" : "Opens to edit") :
			(on ? "The show goes round" : "The show stops at the end"));
		return true;
	}
	msg2(e, "Not a command: ", w);
	return false;
}
