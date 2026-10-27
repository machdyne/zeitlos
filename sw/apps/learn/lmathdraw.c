/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- formulas typeset. lmathdraw.h.
 */

#include <string.h>

#include "lmathdraw.h"

typedef struct {
	ld_page_t	*p;		// NULL: measure only
	int			c;
} md_t;

static int smaller(int size) { int s = size * 3 / 4; return s < 10 ? 10 : s; }

static ld_style_t upright(int size) { return ld_st(&LDF.serif, size); }
static ld_style_t slanted(int size) { return ld_st(&LDF.serif_i, size); }

static lm_box_t text_box(md_t *m, const ld_style_t *st, const char *s, int x, int base) {
	lm_box_t b;
	int asc, desc;
	ld_metrics(st, &asc, &desc, NULL);
	b.w = ld_widthz(st, s);
	b.asc = asc;
	b.desc = desc;
	if (m->p) ld_textz(m->p, st, x, base, s, m->c);
	return b;
}

static lm_box_t join(lm_box_t a, lm_box_t b) {
	lm_box_t r = { a.w + b.w, a.asc > b.asc ? a.asc : b.asc, a.desc > b.desc ? a.desc : b.desc };
	return r;
}

static int axis(int size) { return size * 3 / 10; }	// where a fraction's bar sits above the baseline

static lm_box_t set(md_t *m, const lm_node_t *n, int size, int x, int base);

static lm_box_t op(md_t *m, const char *s, int size, int x, int base) {
	ld_style_t st = upright(size);
	int gap = size / 5;
	lm_box_t b = text_box(m, &st, s, x + gap, base);
	b.w += 2 * gap;
	return b;
}

// two pieces side by side: the first measured, then both drawn
static lm_box_t pair(md_t *m, const lm_node_t *a, lm_box_t (*mid)(md_t *, const char *, int, int, int),
	const char *midtext, const lm_node_t *b, int size, int x, int base) {
	lm_box_t ba = set(m, a, size, x, base);
	lm_box_t bm = mid ? mid(m, midtext, size, x + ba.w, base) : (lm_box_t){ 0, 0, 0 };
	lm_box_t bb = set(m, b, size, x + ba.w + bm.w, base);
	return join(join(ba, bm), bb);
}

static lm_box_t cmp_sym(md_t *m, int o, int size, int x, int base) {
	static const char *const sym[] = { "", "<", ">", "<", ">", "=", "=" };
	lm_box_t b = op(m, sym[o], size, x, base);
	if (m->p && (o == LM_LE || o == LM_GE)) {	// a bar under: <= and >=
		int gap = size / 5;
		ld_fill(m->p, x + gap, base + size / 8, b.w - 2 * gap, 1, m->c);
	}
	if (m->p && o == LM_NE) {
		int cx = x + b.w / 2;
		ld_line(m->p, cx + size / 6, base - size * 6 / 10, cx - size / 6, base + size / 8, 1, m->c);
	}
	return b;
}

static lm_box_t frac(md_t *m, const lm_node_t *num, const lm_node_t *den, int size, int x, int base) {
	int s2 = smaller(size);
	md_t meas = { NULL, 0 };
	lm_box_t bn = set(&meas, num, s2, 0, 0), bd = set(&meas, den, s2, 0, 0);
	int w = (bn.w > bd.w ? bn.w : bd.w) + size / 4;
	int ax = axis(size), gap = size / 8 + 1;
	if (m->p) {
		set(m, num, s2, x + (w - bn.w) / 2, base - ax - gap - bn.desc);
		ld_fill(m->p, x, base - ax, w, size >= 28 ? 2 : 1, m->c);
		set(m, den, s2, x + (w - bd.w) / 2, base - ax + gap + bd.asc + 1);
	}
	lm_box_t b = { w + 2, ax + gap + bn.desc + bn.asc, -ax + gap + bd.asc + bd.desc + 1 };
	if (b.desc < 0) b.desc = 0;
	return b;
}

static lm_box_t paren(md_t *m, const lm_node_t *in, int size, int x, int base) {
	md_t meas = { NULL, 0 };
	lm_box_t bi = set(&meas, in, size, 0, 0);
	int h = bi.asc + bi.desc;
	int ps = h > size ? h * 9 / 10 : size;	// a taller bracket for a taller inside
	ld_style_t st = upright(ps);
	int shift = (bi.desc - bi.asc + (ps * 7 / 10)) / 2;	// centre the bracket on the contents
	lm_box_t l = text_box(m, &st, "(", x, base + (ps > size ? shift : 0));
	set(m, in, size, x + l.w, base);
	lm_box_t r = text_box(m, &st, ")", x + l.w + bi.w, base + (ps > size ? shift : 0));
	lm_box_t b = { l.w + bi.w + r.w, bi.asc > l.asc ? bi.asc : l.asc, bi.desc > l.desc ? bi.desc : l.desc };
	return b;
}

static lm_box_t root(md_t *m, const lm_node_t *in, int size, int x, int base) {
	md_t meas = { NULL, 0 };
	lm_box_t bi = set(&meas, in, size, 0, 0);
	int top = base - bi.asc - 2, bot = base + bi.desc;
	int lw = size / 2 + 2;
	if (m->p) {
		ld_line(m->p, x, base - size / 4, x + lw / 3, base - size / 4 - 1, 1, m->c);
		ld_line(m->p, x + lw / 3, base - size / 4, x + lw * 2 / 3, bot, 1.4, m->c);
		ld_line(m->p, x + lw * 2 / 3, bot, x + lw, top, 1, m->c);
		ld_fill(m->p, x + lw, top, bi.w + 3, 1, m->c);
		set(m, in, size, x + lw + 2, base);
	}
	lm_box_t b = { lw + bi.w + 4, bi.asc + 3, bi.desc + 1 };
	return b;
}

static lm_box_t set(md_t *m, const lm_node_t *n, int size, int x, int base) {
	lm_box_t z = { 0, 0, 0 };
	if (!n) return z;
	switch (n->kind) {
	case LM_NUM: { ld_style_t st = upright(size); return text_box(m, &st, n->text, x, base); }
	case LM_VAR: {
		const char *g = lm_greek(n->text);
		bool constant = !strcmp(n->text, "pi") || !strcmp(n->text, "e");
		ld_style_t st = constant || g ? upright(size) : slanted(size);
		lm_box_t b = text_box(m, &st, g ? g : n->text, x, base);
		b.w += size / 12;
		return b;
	}
	case LM_ADD: return pair(m, n->a, op, "+", n->b, size, x, base);
	case LM_SUB: return pair(m, n->a, op, "-", n->b, size, x, base);
	case LM_MUL: return pair(m, n->a, op, "\xC3\x97", n->b, size, x, base);		// ×
	case LM_IDIV: return pair(m, n->a, op, "\xC3\xB7", n->b, size, x, base);	// ÷
	case LM_IMUL: {
		// two numbers side by side need a sign between them; anything else is juxtaposed
		bool nn = n->a->kind == LM_NUM && n->b->kind == LM_NUM;
		if (nn) return pair(m, n->a, op, "\xC2\xB7", n->b, size, x, base);		// ·
		lm_box_t ba = set(m, n->a, size, x, base);
		int gap = size / 10;
		lm_box_t bb = set(m, n->b, size, x + ba.w + gap, base);
		lm_box_t r = join(ba, bb);
		r.w += gap;
		return r;
	}
	case LM_DIV: return frac(m, n->a, n->b, size, x, base);
	case LM_NEG: {
		ld_style_t st = upright(size);
		lm_box_t s = text_box(m, &st, "-", x, base);
		return join(s, set(m, n->a, size, x + s.w, base));
	}
	case LM_POW: {
		lm_box_t b = set(m, n->a, size, x, base);
		int s2 = smaller(size) * 9 / 10;
		md_t meas = { NULL, 0 };
		lm_box_t e = set(&meas, n->b, s2, 0, 0);
		int raise = b.asc - e.asc / 2;
		set(m, n->b, s2, x + b.w + 1, base - raise);
		lm_box_t r = { b.w + e.w + 1, raise + e.asc > b.asc ? raise + e.asc : b.asc, b.desc };
		return r;
	}
	case LM_SUB_SCRIPT: {
		lm_box_t b = set(m, n->a, size, x, base);
		int s2 = smaller(size) * 9 / 10;
		md_t meas = { NULL, 0 };
		lm_box_t e = set(&meas, n->b, s2, 0, 0);
		int drop = size / 4;
		set(m, n->b, s2, x + b.w, base + drop);
		lm_box_t r = { b.w + e.w, b.asc, drop + e.desc > b.desc ? drop + e.desc : b.desc };
		return r;
	}
	case LM_PCT: {
		lm_box_t b = set(m, n->a, size, x, base);
		ld_style_t st = upright(size);
		return join(b, text_box(m, &st, "%", x + b.w, base));
	}
	case LM_PAREN: return paren(m, n->a, size, x, base);
	case LM_FUNC:
		if (!strcmp(n->text, "sqrt")) return root(m, n->a, size, x, base);
		{
			ld_style_t st = upright(size);
			lm_box_t f = text_box(m, &st, n->text, x, base);
			lm_node_t pn;
			memset(&pn, 0, sizeof pn);
			pn.kind = LM_PAREN;
			pn.a = n->a;
			return join(f, paren(m, &pn, size, x + f.w + 1, base));
		}
	case LM_MIXED: {
		lm_box_t w = set(m, n->a, size, x, base);
		return join(w, frac(m, n->b, n->c, size, x + w.w + size / 8, base));
	}
	case LM_CMP: {
		lm_box_t r = set(m, n->kids[0], size, x, base);
		for (int i = 1; i < n->nkids; i++) {
			lm_box_t o = cmp_sym(m, n->ops[i - 1], size, x + r.w, base);
			r = join(r, o);
			r = join(r, set(m, n->kids[i], size, x + r.w, base));
		}
		return r;
	}
	}
	return z;
}

lm_box_t lm_measure(const lm_node_t *n, int size) {
	md_t m = { NULL, 0 };
	return set(&m, n, size, 0, 0);
}

lm_box_t lm_draw(ld_page_t *p, const lm_node_t *n, int size, int x, int base, int c) {
	md_t m = { p, c };
	return set(&m, n, size, x, base);
}

lm_box_t lm_draw_str(ld_page_t *p, lx_arena_t *a, const char *s, int len, int size,
	int x, int base, int c, bool measure_only) {
	size_t mark = a->used;
	char err[8];
	lm_node_t *n = lm_parse(a, s, len, err, sizeof err);
	lm_box_t b;
	if (n) b = measure_only ? lm_measure(n, size) : lm_draw(p, n, size, x, base, c);
	else {
		ld_style_t st = upright(size);
		int asc, desc;
		ld_metrics(&st, &asc, &desc, NULL);
		b.w = ld_width(&st, s, len);
		b.asc = asc;
		b.desc = desc;
		if (!measure_only) ld_text(p, &st, x, base, s, len, c);
	}
	a->used = mark;
	return b;
}
