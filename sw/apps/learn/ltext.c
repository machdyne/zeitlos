/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- text laid out. ltext.h.
 */

#include <string.h>

#include "ltext.h"
#include "lmathdraw.h"
#include "../../common/zutf8.h"

static bool is_cjk(uint32_t cp) {
	return (cp >= 0x3000 && cp <= 0x30FF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
		(cp >= 0xFF00 && cp <= 0xFFEF);
}

static ld_style_t run_style(const lt_layout_t *lt, const lt_run_t *r) {
	ld_style_t st;
	if (r->kind == LT_CODE) st = ld_st(&LDF.mono, 0);
	else if (r->cjk) st = ld_st(&LDF.jp, 0);
	else {
		st = ld_st(r->bold ? lt->bold_font : lt->font, 0);
		if (lt->font->kind == ZC_FONT_STROKE) st.size = lt->size;
		st.italic = r->italic;
	}
	st.underline = r->link;
	return st;
}

static void metrics_of(const lt_layout_t *lt, const lt_run_t *r, lx_arena_t *a, int *w, int *asc, int *desc) {
	if (r->kind == LT_MATH) {
		lm_box_t b = lm_draw_str(NULL, a, r->s, r->len, lt->size, 0, 0, 0, true);
		*w = b.w;
		*asc = b.asc;
		*desc = b.desc;
		return;
	}
	ld_style_t st = run_style(lt, r);
	ld_metrics(&st, asc, desc, NULL);
	*w = ld_width(&st, r->s, r->len);
}

// a token: a word, the spaces after it, or one CJK character
typedef struct {
	lt_run_t	r;
	int			w, asc, desc;
	bool		space;		// a run of spaces (a place to break; dropped at line ends)
} tok_t;

#define MAX_TOK 640

static int tokens(lt_layout_t *lt, lx_arena_t *a, const lf_span_t *sp, int nsp, tok_t *t, int max) {
	int n = 0;
	for (int i = 0; i < nsp && n < max; i++) {
		const lf_span_t *s = &sp[i];
		int link = -1;
		if (s->kind == LS_LINK) {
			if (lt->nlinks < LT_MAX_LINKS && s->target) {
				strncpy(lt->links[lt->nlinks], s->target, sizeof lt->links[0] - 1);
				lt->links[lt->nlinks][sizeof lt->links[0] - 1] = 0;
				link = lt->nlinks++;
			}
		}
		if (s->kind == LS_MATH || s->kind == LS_CODE) {
			tok_t *k = &t[n++];
			memset(k, 0, sizeof *k);
			k->r.kind = s->kind == LS_MATH ? LT_MATH : LT_CODE;
			k->r.s = s->s;
			k->r.len = s->len;
			k->r.link_id = -1;
			metrics_of(lt, &k->r, a, &k->w, &k->asc, &k->desc);
			continue;
		}
		const char *p = s->s, *end = s->s + s->len;
		while (p < end && n < max) {
			tok_t *k = &t[n];
			memset(k, 0, sizeof *k);
			k->r.kind = LT_TEXT;
			k->r.bold = s->bold;
			k->r.italic = s->italic;
			k->r.link = s->kind == LS_LINK;
			k->r.link_id = (int16_t)link;
			k->r.s = p;
			const char *q = p;
			uint32_t cp = z_utf8_next(&q, end);
			if (cp == ' ' || cp == '\t' || cp == '\n') {
				while (q < end && (*q == ' ' || *q == '\t' || *q == '\n')) q++;
				k->space = true;
			} else if (is_cjk(cp)) {
				k->r.cjk = 1;
			} else {
				const char *e = q;
				while (e < end) {
					const char *nx = e;
					uint32_t c2 = z_utf8_next(&nx, end);
					if (c2 == ' ' || c2 == '\t' || c2 == '\n' || is_cjk(c2)) break;
					e = nx;
				}
				q = e;
			}
			k->r.len = (int)(q - p);
			if (k->space) {
				// a space's width in the text's face
				lt_run_t sr = k->r;
				sr.s = " ";
				sr.len = 1;
				metrics_of(lt, &sr, a, &k->w, &k->asc, &k->desc);
			} else metrics_of(lt, &k->r, a, &k->w, &k->asc, &k->desc);
			n++;
			p = q;
		}
	}
	return n;
}

static void add_line(lt_layout_t *lt, int first, int count, int w, int asc, int desc) {
	if (lt->nlines >= LT_MAX_LINES) return;
	lt_line_t *l = &lt->lines[lt->nlines++];
	l->first = first;
	l->count = count;
	l->asc = (int16_t)asc;
	l->desc = (int16_t)desc;
	l->w = (int16_t)w;
	lt->height += asc + desc + lt->lead;
	if (w > lt->width) lt->width = w;
}

void lt_layout(lt_layout_t *lt, lx_arena_t *a, const char *text, int w) {
	lt->nruns = lt->nlines = lt->nlinks = 0;
	lt->height = lt->width = 0;
	static lf_span_t sp[160];
	static tok_t tk[MAX_TOK];
	size_t mark = a->used;
	int nsp = lf_inline(a, text, (int)strlen(text), sp, 160);
	int nt = tokens(lt, a, sp, nsp, tk, MAX_TOK);
	// the text's own line height, so an empty or short line is not squashed
	ld_style_t base = ld_st(lt->font, lt->font->kind == ZC_FONT_STROKE ? lt->size : 0);
	int basc, bdesc;
	ld_metrics(&base, &basc, &bdesc, NULL);

	int i = 0;
	while (i < nt) {
		while (i < nt && tk[i].space) i++;		// no line starts with spaces
		if (i >= nt) break;
		int first = lt->nruns, x = 0, asc = basc, desc = bdesc, last_end = 0;
		int j = i;
		while (j < nt) {
			tok_t *k = &tk[j];
			if (!k->space && x + k->w > w && lt->nruns > first) break;	// it does not fit: next line
			if (k->space) {
				// a space only between things: look ahead to the next word
				int nx = j + 1;
				if (nx >= nt || x + k->w + tk[nx].w > w) { j++; break; }
				x += k->w;
				j++;
				continue;
			}
			if (lt->nruns >= LT_MAX_RUNS) break;
			lt_run_t *r = &lt->runs[lt->nruns++];
			*r = k->r;
			r->x = (int16_t)x;
			r->w = (int16_t)k->w;
			x += k->w;
			last_end = x;
			if (k->asc > asc) asc = k->asc;
			if (k->desc > desc) desc = k->desc;
			j++;
			if (k->w > w) break;		// a word wider than the line has a line of its own
		}
		add_line(lt, first, lt->nruns - first, last_end, asc, desc);
		if (j == i) j++;
		i = j;
	}
	if (lt->nlines) lt->height -= lt->lead;	// no leading after the last line
	a->used = mark;
}

void lt_draw(const lt_layout_t *lt, ld_page_t *p, lx_arena_t *a, int x, int y, int c) {
	int top = y;
	for (int i = 0; i < lt->nlines; i++) {
		const lt_line_t *l = &lt->lines[i];
		int base = top + l->asc;
		for (int k = l->first; k < l->first + l->count; k++) {
			const lt_run_t *r = &lt->runs[k];
			if (r->kind == LT_MATH) {
				lm_draw_str(p, a, r->s, r->len, lt->size, x + r->x, base, c, false);
				continue;
			}
			ld_style_t st = run_style(lt, r);
			if (r->kind == LT_CODE) {
				// code: knocked out of a lit box, as read draws it
				int asc, desc;
				ld_metrics(&st, &asc, &desc, NULL);
				ld_fill(p, x + r->x - 1, base - asc, r->w + 2, asc + desc, c);
				ld_text(p, &st, x + r->x, base, r->s, r->len, !c);
			} else ld_text(p, &st, x + r->x, base, r->s, r->len, c);
		}
		top += l->asc + l->desc + lt->lead;
	}
}

int lt_link_at(const lt_layout_t *lt, lx_arena_t *a, int px, int py) {
	(void)a;
	int top = 0;
	for (int i = 0; i < lt->nlines; i++) {
		const lt_line_t *l = &lt->lines[i];
		int h = l->asc + l->desc;
		if (py >= top && py < top + h + lt->lead)
			for (int k = l->first; k < l->first + l->count; k++) {
				const lt_run_t *r = &lt->runs[k];
				if (r->link_id >= 0 && px >= r->x && px < r->x + r->w) return r->link_id;
			}
		top += h + lt->lead;
	}
	return -1;
}

int lt_plain(lx_arena_t *a, const char *text, char *out, int cap) {
	lf_span_t sp[160];
	size_t mark = a->used;
	int n = lf_inline(a, text, (int)strlen(text), sp, 160), o = 0;
	for (int i = 0; i < n && o < cap - 1; i++) {
		int l = sp[i].len;
		if (o + l >= cap) l = cap - 1 - o;
		memcpy(out + o, sp[i].s, (size_t)l);
		o += l;
	}
	out[o] = 0;
	a->used = mark;
	return o;
}
