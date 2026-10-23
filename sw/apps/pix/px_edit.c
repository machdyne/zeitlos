/*
 * pix -- the tools and undo. See px_edit.h.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */

#include <stdlib.h>
#include <string.h>

#include "px_edit.h"

static int iabs(int v) { return v < 0 ? -v : v; }

/* -- shapes -- */

void px_line(px_doc_t *d, int i, int x0, int y0, int x1, int y1, int v) {
	int dx = iabs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = -iabs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	int err = dx + dy, e2;
	for (;;) {
		px_set(d, i, x0, y0, v);
		if (x0 == x1 && y0 == y1) break;
		e2 = 2 * err;
		if (e2 >= dy) { err += dy; x0 += sx; }
		if (e2 <= dx) { err += dx; y0 += sy; }
	}
}

bool px_on_line(int x0, int y0, int x1, int y1, int x, int y) {
	int dx = iabs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = -iabs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	int err = dx + dy, e2;
	/* outside the box: not on it, without walking it */
	if (x < (x0 < x1 ? x0 : x1) || x > (x0 < x1 ? x1 : x0) ||
	    y < (y0 < y1 ? y0 : y1) || y > (y0 < y1 ? y1 : y0)) return false;
	for (;;) {
		if (x0 == x && y0 == y) return true;
		if (x0 == x1 && y0 == y1) return false;
		e2 = 2 * err;
		if (e2 >= dy) { err += dy; x0 += sx; }
		if (e2 <= dx) { err += dx; y0 += sy; }
	}
}

static void order(int *a, int *b) {
	if (*a > *b) { int t = *a; *a = *b; *b = t; }
}

bool px_on_rect(int x0, int y0, int x1, int y1, int x, int y, bool filled) {
	order(&x0, &x1);
	order(&y0, &y1);
	if (x < x0 || x > x1 || y < y0 || y > y1) return false;
	return filled || x == x0 || x == x1 || y == y0 || y == y1;
}

void px_rect(px_doc_t *d, int i, int x0, int y0, int x1, int y1, int v, bool filled) {
	int x, y;
	order(&x0, &x1);
	order(&y0, &y1);
	for (y = y0; y <= y1; y++)
		for (x = x0; x <= x1; x++)
			if (filled || x == x0 || x == x1 || y == y0 || y == y1)
				px_set(d, i, x, y, v);
}

/* Scanline flood fill with a small explicit stack of seeds: no
 * recursion, so a 1024 x 1024 picture cannot run the stack out. */
bool px_flood(px_doc_t *d, int i, int x, int y, int v) {
	const px_cell_t *c = &d->cell[i];
	int from = px_get(d, i, x, y);
	int *stack, sp = 0, cap;
	bool any = false;

	if (x < 0 || y < 0 || x >= c->w || y >= c->h || from == v) return false;
	cap = c->w * 2 + c->h * 2 + 64;
	stack = malloc(sizeof(int) * 2 * (size_t)cap);
	if (!stack) return false;
	stack[sp++] = x;
	stack[sp++] = y;
	while (sp) {
		int l, r, k;
		y = stack[--sp];
		x = stack[--sp];
		if (px_get(d, i, x, y) != from) continue;
		l = x;
		while (l > 0 && px_get(d, i, l - 1, y) == from) l--;
		r = x;
		while (r < c->w - 1 && px_get(d, i, r + 1, y) == from) r++;
		for (k = l; k <= r; k++) px_set(d, i, k, y, v);
		any = true;
		/* one seed per run above and below */
		for (k = -1; k <= 1; k += 2) {
			int ny = y + k, xx;
			bool in = false;
			if (ny < 0 || ny >= c->h) continue;
			for (xx = l; xx <= r; xx++) {
				bool m = px_get(d, i, xx, ny) == from;
				if (m && !in) {
					if (sp + 2 > cap * 2) {
						int *n = realloc(stack, sizeof(int) * 4 * (size_t)cap);
						if (!n) { free(stack); return any; }
						stack = n;
						cap *= 2;
					}
					stack[sp++] = xx;
					stack[sp++] = ny;
				}
				in = m;
			}
		}
	}
	free(stack);
	return any;
}

/* -- pieces -- */

#define PXB(w, h) ((size_t)(w) * (size_t)(h) * sizeof(uint16_t))

bool px_piece_take(px_piece_t *p, const px_doc_t *d, int i, int x, int y, int w, int h) {
	int a, b;
	uint16_t *n;
	if (w < 1 || h < 1) return false;
	n = malloc(PXB(w, h));
	if (!n) return false;
	free(p->px);
	p->px = n;
	p->w = w;
	p->h = h;
	for (b = 0; b < h; b++)
		for (a = 0; a < w; a++)
			p->px[b * w + a] = (uint16_t)px_get(d, i, x + a, y + b);
	return true;
}

void px_piece_put(const px_piece_t *p, px_doc_t *d, int i, int x, int y, bool transparent) {
	int a, b, clr = px_clear(d);
	for (b = 0; b < p->h; b++)
		for (a = 0; a < p->w; a++) {
			int v = p->px[b * p->w + a];
			if (transparent && v == clr) continue;
			px_set(d, i, x + a, y + b, v);
		}
}

void px_piece_free(px_piece_t *p) {
	free(p->px);
	p->px = NULL;
	p->w = p->h = 0;
}

bool px_piece_copy(px_piece_t *p, const px_piece_t *q) {
	uint16_t *n;
	if (!q->px) return false;
	n = malloc(PXB(q->w, q->h));
	if (!n) return false;
	memcpy(n, q->px, PXB(q->w, q->h));
	free(p->px);
	p->px = n;
	p->w = q->w;
	p->h = q->h;
	return true;
}

void px_piece_flip_h(px_piece_t *p) {
	int a, b;
	for (b = 0; b < p->h; b++)
		for (a = 0; a < p->w / 2; a++) {
			uint16_t *l = &p->px[b * p->w + a], *r = &p->px[b * p->w + p->w - 1 - a];
			uint16_t t = *l; *l = *r; *r = t;
		}
}

void px_piece_flip_v(px_piece_t *p) {
	int a, b;
	for (b = 0; b < p->h / 2; b++)
		for (a = 0; a < p->w; a++) {
			uint16_t *u = &p->px[b * p->w + a], *v = &p->px[(p->h - 1 - b) * p->w + a];
			uint16_t t = *u; *u = *v; *v = t;
		}
}

bool px_piece_rotate(px_piece_t *p) {
	int a, b, w = p->w, h = p->h;
	uint16_t *n = malloc(PXB(w, h));
	if (!n) return false;
	/* (a, b) goes to (h - 1 - b, a) in an h-wide piece */
	for (b = 0; b < h; b++)
		for (a = 0; a < w; a++)
			n[a * h + (h - 1 - b)] = p->px[b * w + a];
	free(p->px);
	p->px = n;
	p->w = h;
	p->h = w;
	return true;
}

void px_piece_shift(px_piece_t *p, int dx, int dy) {
	int a, b;
	uint16_t *n = malloc(PXB(p->w, p->h));
	if (!n) return;
	dx = ((dx % p->w) + p->w) % p->w;
	dy = ((dy % p->h) + p->h) % p->h;
	for (b = 0; b < p->h; b++)
		for (a = 0; a < p->w; a++)
			n[((b + dy) % p->h) * p->w + (a + dx) % p->w] = p->px[b * p->w + a];
	free(p->px);
	p->px = n;
}

/* -- undo --
 *
 * Two stacks of steps; a new edit clears redo. When either limit is
 * reached the oldest undo step goes.
 *
 * A step is taken whole -- the cell's every byte, and its size -- just
 * before an edit, because what the edit will touch is not known yet (a
 * pencil stroke goes where the hand goes). It is then *settled* the
 * next time undo is used or another step is taken: compared with the
 * cell as it is now, and cut down to the rows and the bytes across
 * that differ. A pixel set in a 640 x 480 picture keeps a few bytes,
 * not 300 KB, so a big picture has as many steps as a small one. A
 * step across a change of size (a turn) stays whole. */

typedef struct {
	int cell, w, h;             /* the cell, and its size then */
	bool part;                  /* only rows y0..y1, bytes b0..b1 of each */
	int y0, y1, b0, b1;
	uint8_t *bits;
	uint32_t n;
} step_t;

/* one over: the newest, before it is settled, may be one too many */
static step_t undo_st[PX_UNDO_STEPS + 1], redo_st[PX_UNDO_STEPS + 1];
static int undo_n, redo_n;
static uint32_t held;
static bool pending;            /* the newest undo step is not settled */

static void step_free(step_t *s) {
	held -= s->n;
	free(s->bits);
	memset(s, 0, sizeof(*s));
}

static void clear(step_t *st, int *n) {
	while (*n) step_free(&st[--(*n)]);
}

void px_undo_clear(void) {
	clear(undo_st, &undo_n);
	clear(redo_st, &redo_n);
	pending = false;
}

static void drop_oldest(void) {
	step_free(&undo_st[0]);
	memmove(&undo_st[0], &undo_st[1], sizeof(step_t) * (size_t)(undo_n - 1));
	undo_n--;
	memset(&undo_st[undo_n], 0, sizeof(step_t));
}

static uint32_t row_bytes(const px_doc_t *d, int w) {
	return px_cell_bytes(d, w, 1);
}

static bool snap(step_t *s, const px_doc_t *d, int i) {
	const px_cell_t *c = &d->cell[i];
	s->n = px_cell_bytes(d, c->w, c->h);
	s->bits = malloc(s->n ? s->n : 1);
	if (!s->bits) { s->n = 0; return false; }
	memcpy(s->bits, d->bits + c->off, s->n);
	s->cell = i;
	s->w = c->w;
	s->h = c->h;
	s->part = false;
	held += s->n;
	return true;
}

/* A part of cell i, rows y0..y1 and bytes b0..b1 across: what is there
 * now (for redo or undo of a part). */
static bool snap_part(step_t *s, const px_doc_t *d, int i, int y0, int y1, int b0, int b1) {
	const px_cell_t *c = &d->cell[i];
	uint32_t rb = row_bytes(d, c->w), wb = (uint32_t)(b1 - b0 + 1);
	int y;
	s->n = wb * (uint32_t)(y1 - y0 + 1);
	s->bits = malloc(s->n ? s->n : 1);
	if (!s->bits) { s->n = 0; return false; }
	for (y = y0; y <= y1; y++)
		memcpy(s->bits + (uint32_t)(y - y0) * wb, d->bits + c->off + (uint32_t)y * rb + (uint32_t)b0, wb);
	s->cell = i;
	s->w = c->w;
	s->h = c->h;
	s->part = true;
	s->y0 = y0; s->y1 = y1; s->b0 = b0; s->b1 = b1;
	held += s->n;
	return true;
}

/* The newest step, cut down to what the edit changed; gone if it
 * changed nothing; and the oldest dropped while over the byte limit. */
static void settle(const px_doc_t *d) {
	step_t *s;
	const px_cell_t *c;
	uint32_t rb;
	int y, x, y0 = -1, y1 = -1, b0 = 0, b1 = 0;
	if (!pending || !undo_n) { pending = false; return; }
	pending = false;
	s = &undo_st[undo_n - 1];
	if (s->part || s->cell < 0 || s->cell >= d->ncells) return;
	c = &d->cell[s->cell];
	if (c->w == s->w && c->h == s->h) {
		const uint8_t *now = d->bits + c->off;
		rb = row_bytes(d, c->w);
		for (y = 0; y < c->h; y++) {
			const uint8_t *a = s->bits + (uint32_t)y * rb, *b = now + (uint32_t)y * rb;
			int l = -1, r = -1;
			if (!memcmp(a, b, rb)) continue;
			for (x = 0; x < (int)rb; x++) if (a[x] != b[x]) { l = x; break; }
			for (x = (int)rb - 1; x >= 0; x--) if (a[x] != b[x]) { r = x; break; }
			if (y0 < 0) { y0 = y; b0 = l; b1 = r; }
			y1 = y;
			if (l < b0) b0 = l;
			if (r > b1) b1 = r;
		}
		/* nothing changed: no step, and what redo had stays */
		if (y0 < 0) { step_free(s); undo_n--; return; }
		{
			/* the part, moved down to the start of the whole (each row
			 * goes no later than it was: a part row is no wider than a
			 * whole one), then the rest given back -- never a second
			 * copy of a big picture */
			uint32_t wb = (uint32_t)(b1 - b0 + 1), n = wb * (uint32_t)(y1 - y0 + 1);
			uint8_t *p;
			for (y = y0; y <= y1; y++)
				memmove(s->bits + (uint32_t)(y - y0) * wb, s->bits + (uint32_t)y * rb + (uint32_t)b0, wb);
			p = realloc(s->bits, n ? n : 1);
			if (p) s->bits = p;
			held -= s->n;
			s->n = n;
			held += n;
			s->part = true;
			s->y0 = y0; s->y1 = y1; s->b0 = b0; s->b1 = b1;
		}
	}
	/* a step that stays: a new edit, so redo has nothing to redo */
	clear(redo_st, &redo_n);
	while (undo_n > PX_UNDO_STEPS) drop_oldest();
	while (undo_n > 1 && held > PX_UNDO_BYTES) drop_oldest();
	if (held > PX_UNDO_BYTES) clear(undo_st, &undo_n);   /* one step bigger than all: none */
}

void px_undo_settle(const px_doc_t *d) {
	settle(d);
}

bool px_undo_save(const px_doc_t *d, int i) {
	if (i < 0 || i >= d->ncells) return false;
	settle(d);
	while (undo_n > PX_UNDO_STEPS) drop_oldest();
	/* whole, for now: settled later (it may be bigger than the limit
	 * while it is whole) */
	if (!snap(&undo_st[undo_n], d, i)) return false;
	undo_n++;
	pending = true;
	return true;
}

int px_undo(px_doc_t *d, bool redo) {
	step_t *from = redo ? redo_st : undo_st, *to = redo ? undo_st : redo_st;
	int *nf = redo ? &redo_n : &undo_n, *nt = redo ? &undo_n : &redo_n;
	step_t s;
	int i;
	bool kept;

	settle(d);
	if (!*nf) return -1;
	s = from[--(*nf)];
	memset(&from[*nf], 0, sizeof(step_t));
	i = s.cell;
	if (i < 0 || i >= d->ncells) { held -= s.n; free(s.bits); return -1; }

	/* what is there now goes to the other stack: the same part, or
	 * the whole cell */
	if (*nt == PX_UNDO_STEPS) {
		if (to == undo_st) drop_oldest();
		else { step_free(&to[0]); memmove(&to[0], &to[1], sizeof(step_t) * (PX_UNDO_STEPS - 1)); (*nt)--; }
	}
	if (s.part && d->cell[i].w == s.w && d->cell[i].h == s.h)
		kept = snap_part(&to[*nt], d, i, s.y0, s.y1, s.b0, s.b1);
	else
		kept = snap(&to[*nt], d, i);
	if (kept) (*nt)++;

	if (s.part) {
		const px_cell_t *c = &d->cell[i];
		uint32_t rb = row_bytes(d, c->w), wb = (uint32_t)(s.b1 - s.b0 + 1);
		int y;
		if (c->w == s.w && c->h == s.h)
			for (y = s.y0; y <= s.y1; y++)
				memcpy(d->bits + c->off + (uint32_t)y * rb + (uint32_t)s.b0,
					s.bits + (uint32_t)(y - s.y0) * wb, wb);
	} else {
		if ((d->cell[i].w != s.w || d->cell[i].h != s.h) && !px_cell_resize(d, i, s.w, s.h)) {
			/* no room to give the cell its old size: nothing changes,
			 * and the step goes back where it was */
			if (kept) { (*nt)--; held -= to[*nt].n; free(to[*nt].bits); memset(&to[*nt], 0, sizeof(step_t)); }
			from[(*nf)++] = s;
			return -1;
		}
		memcpy(d->bits + d->cell[i].off, s.bits, s.n);
	}
	held -= s.n;
	free(s.bits);
	px_touch(d, i);
	return i;
}

uint32_t px_undo_held(void) {
	return held;
}

int px_undo_depth(bool redo) {
	return redo ? redo_n : undo_n;
}
