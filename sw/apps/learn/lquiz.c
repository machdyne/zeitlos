/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- questions and answers. lquiz.h.
 */

#include <string.h>
#include <stdio.h>
#include <math.h>

#include "lquiz.h"
#include "lmath.h"
#include "../../common/zutf8.h"

static uint32_t rnd(uint32_t *s) {	// xorshift: the same seed, the same shuffle
	*s ^= *s << 13;
	*s ^= *s >> 17;
	*s ^= *s << 5;
	return *s;
}

static const char *str1(const lx_node_t *f) {
	lx_node_t *a = lx_nth(f, 1);
	return a && a->kind == LX_STR ? a->text : NULL;
}

bool lq_load(lq_q_t *q, const lx_node_t *form, uint32_t seed) {
	memset(q, 0, sizeof *q);
	q->form = form;
	const char *k = lx_form_name(form);
	if (!k) return false;
	static const char *const kinds[] = { "choice", "number", "fraction", "text", "order", "match", "point" };
	q->kind = LQ_UNKNOWN;
	for (int i = 0; i < 7; i++) if (!strcmp(k, kinds[i])) q->kind = (uint8_t)i;
	if (q->kind == LQ_UNKNOWN) return false;

	lx_node_t *ask = lx_find(form, "ask");
	for (lx_node_t *e = ask ? ask->head->next : NULL; e && q->nask < LQ_MAX_ASK; e = e->next) {
		if (e->kind == LX_STR) { q->ask[q->nask].text = e->text; q->ask[q->nask++].math = false; }
		else if (lx_is_form(e, "math") && str1(e)) { q->ask[q->nask].text = str1(e); q->ask[q->nask++].math = true; }
	}
	for (int i = 0; i < 3; i++) {
		lx_node_t *h = lx_find_nth(form, "hint", i);
		if (h && str1(h)) q->hints[q->nhints++] = str1(h);
	}
	q->explain = lx_str_of(form, "explain");
	q->id = lx_str_of(form, "id");

	switch (q->kind) {
	case LQ_CHOICE: {
		q->several = lx_flag(form, "several");
		for (lx_node_t *e = form->head->next; e && q->nopt < LQ_MAX_OPT; e = e->next) {
			bool r = lx_is_form(e, "right"), w = lx_is_form(e, "wrong");
			if (!r && !w) continue;
			lq_opt_t *o = &q->opt[q->nopt];
			o->text = str1(e);
			if (!o->text) continue;
			o->right = r;
			o->feedback = NULL;
			for (lx_node_t *f = lx_nth(e, 2); f; f = f->next) if (f->kind == LX_STR) { o->feedback = f->text; break; }
			q->nopt++;
		}
		for (int i = 0; i < q->nopt; i++) q->order[i] = i;
		if (!lx_flag(form, "keep-order")) {
			uint32_t s = seed ? seed : 0x9E3779B9u;
			for (int i = q->nopt - 1; i > 0; i--) {
				int j = (int)(rnd(&s) % (uint32_t)(i + 1));
				int t = q->order[i]; q->order[i] = q->order[j]; q->order[j] = t;
			}
		}
		return q->nopt > 0;
	}
	case LQ_NUMBER:
		lx_num_of(form, "answer", &q->answer);
		lx_num_of(form, "within", &q->within);
		q->unit = lx_str_of(form, "unit");
		break;
	case LQ_FRACTION: {
		lx_node_t *an = lx_find(form, "answer");
		if (an && lx_len(an) == 3) { q->fnum = (long)lx_nth(an, 1)->num; q->fden = (long)lx_nth(an, 2)->num; }
		q->equivalent = lx_flag(form, "equivalent");
		q->simplest = lx_flag(form, "simplest");
		break;
	}
	case LQ_TEXT:
		for (int i = 0; i < LQ_MAX_ACCEPT; i++) {
			lx_node_t *a = lx_find_nth(form, "accept", i);
			if (!a) break;
			if (str1(a)) q->accept[q->naccept++] = str1(a);
		}
		q->case_matters = lx_flag(form, "case");
		{
			const char *acc = lx_str_of(form, "accents");
			q->accents_nearly = acc && !strcmp(acc, "nearly");
		}
		break;
	case LQ_ORDER: {
		lx_node_t *it = lx_find(form, "items");
		for (lx_node_t *e = it ? it->head->next : NULL; e && q->nopt < LQ_MAX_OPT; e = e->next)
			if (e->kind == LX_STR) { q->opt[q->nopt].text = e->text; q->opt[q->nopt].right = true; q->nopt++; }
		for (int i = 0; i < q->nopt; i++) q->order[i] = i;
		uint32_t s = seed ? seed : 0x9E3779B9u;
		// shuffle, but never leave them in the right order
		for (int tries = 0; tries < 8; tries++) {
			for (int i = q->nopt - 1; i > 0; i--) {
				int j = (int)(rnd(&s) % (uint32_t)(i + 1));
				int t = q->order[i]; q->order[i] = q->order[j]; q->order[j] = t;
			}
			bool sorted = true;
			for (int i = 0; i < q->nopt; i++) if (q->order[i] != i) sorted = false;
			if (!sorted) break;
		}
		break;
	}
	default:
		break;
	}
	for (int i = 0; i < LQ_MAX_WRONGIF; i++) {
		lx_node_t *w = lx_find_nth(form, "wrong-if", i);
		if (!w) break;
		lx_node_t *v = lx_nth(w, 1), *fb = lx_nth(w, 2);
		if (!v) continue;
		static char held[LQ_MAX_WRONGIF][32];	// a (wrong-if (4 3)) fraction, written out
		if (v->kind == LX_LIST && lx_len(v) == 2) {
			snprintf(held[i], sizeof held[i], "%s/%s", lx_nth(v, 0)->text, lx_nth(v, 1)->text);
			q->wrong_if[q->nwrong_if] = held[i];
		} else q->wrong_if[q->nwrong_if] = v->text;
		q->wrong_fb[q->nwrong_if] = fb && fb->kind == LX_STR ? fb->text : NULL;
		q->nwrong_if++;
	}
	return true;
}

// -- text --

// the letter without its accent, for the Latin-9 letters (and a few more)
static uint32_t base_letter(uint32_t c) {
	static const struct { uint16_t lo, hi; char b; } t[] = {
		{ 0xC0, 0xC5, 'A' }, { 0xC7, 0xC7, 'C' }, { 0xC8, 0xCB, 'E' }, { 0xCC, 0xCF, 'I' },
		{ 0xD1, 0xD1, 'N' }, { 0xD2, 0xD6, 'O' }, { 0xD8, 0xD8, 'O' }, { 0xD9, 0xDC, 'U' },
		{ 0xDD, 0xDD, 'Y' }, { 0xE0, 0xE5, 'a' }, { 0xE7, 0xE7, 'c' }, { 0xE8, 0xEB, 'e' },
		{ 0xEC, 0xEF, 'i' }, { 0xF1, 0xF1, 'n' }, { 0xF2, 0xF6, 'o' }, { 0xF8, 0xF8, 'o' },
		{ 0xF9, 0xFC, 'u' }, { 0xFD, 0xFD, 'y' }, { 0xFF, 0xFF, 'y' },
		{ 0x160, 0x160, 'S' }, { 0x161, 0x161, 's' }, { 0x17D, 0x17D, 'Z' }, { 0x17E, 0x17E, 'z' },
	};
	for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (c >= t[i].lo && c <= t[i].hi) return (uint32_t)t[i].b;
	return c;
}

static uint32_t fold(uint32_t c) {
	if (c >= 'A' && c <= 'Z') return c + 32;
	if ((c >= 0xC0 && c <= 0xDE && c != 0xD7)) return c + 32;
	if (c == 0x160 || c == 0x17D) return c + 1;
	return c;
}

// next character for comparing: spaces folded, quotes made plain, trailing punctuation dropped
static uint32_t next_cmp(const char **p, const char *end, bool case_matters, bool strip) {
	while (*p < end) {
		uint32_t c = z_utf8_next(p, end);
		if (c == 0x2019 || c == 0x2018) c = '\'';
		if (c == ' ' || c == '\t') {
			while (*p < end && (**p == ' ' || **p == '\t')) (*p)++;
			if (*p >= end) return 0;		// trailing spaces
			return ' ';
		}
		if ((c == '.' || c == '!' || c == '?') && *p >= end) return 0;	// a full stop at the end
		if (!case_matters) c = fold(c);
		if (strip) c = base_letter(c);
		if (!case_matters && strip) c = fold(c);
		return c;
	}
	return 0;
}

static bool cmp_text(const char *a, const char *b, bool case_matters, bool strip) {
	const char *pa = a, *ea = a + strlen(a), *pb = b, *eb = b + strlen(b);
	while (pa < ea && (*pa == ' ' || *pa == '\t')) pa++;
	while (pb < eb && (*pb == ' ' || *pb == '\t')) pb++;
	while (ea > pa && (ea[-1] == ' ' || ea[-1] == '\t')) ea--;
	while (eb > pb && (eb[-1] == ' ' || eb[-1] == '\t')) eb--;
	for (;;) {
		uint32_t ca = next_cmp(&pa, ea, case_matters, strip), cb = next_cmp(&pb, eb, case_matters, strip);
		if (ca != cb) return false;
		if (!ca) return true;
	}
}

bool lq_text_equal(const char *typed, const char *want, bool case_matters, bool *nearly) {
	if (nearly) *nearly = false;
	if (cmp_text(typed, want, case_matters, false)) return true;
	if (nearly && cmp_text(typed, want, case_matters, true)) *nearly = true;
	return false;
}

// -- judging --

static lq_result_t res(int v, const char *fb) {
	lq_result_t r;
	memset(&r, 0, sizeof r);
	r.verdict = v;
	r.feedback = fb;
	return r;
}

lq_result_t lq_judge_choice(const lq_q_t *q, uint32_t chosen) {
	bool all_right = true;
	const char *fb = NULL;
	int nchosen = 0;
	for (int i = 0; i < q->nopt; i++) {
		const lq_opt_t *o = &q->opt[q->order[i]];
		bool c = (chosen >> i) & 1;
		if (c) nchosen++;
		if (c != o->right) {
			all_right = false;
			if (c && !fb) fb = o->feedback;		// a wrong answer chosen: its feedback
		}
	}
	if (!nchosen) return res(LQ_UNREADABLE, NULL);
	if (all_right) return res(LQ_RIGHT, q->explain);
	if (!fb && q->several) fb = "Not all of the right answers are chosen.";
	return res(LQ_WRONG, fb);
}

static const char *wrong_if(const lq_q_t *q, const char *typed, lx_arena_t *a) {
	for (int i = 0; i < q->nwrong_if; i++) {
		if (q->kind == LQ_TEXT) {
			if (lq_text_equal(typed, q->wrong_if[i], q->case_matters, NULL)) return q->wrong_fb[i];
			continue;
		}
		// numbers and fractions compare by value
		size_t mark = a->used;
		lm_val_t t = lm_eval_str(a, typed, NULL, 0), w = lm_eval_str(a, q->wrong_if[i], NULL, 0);
		a->used = mark;
		if (t.ok && w.ok && !t.is_bool && !w.is_bool && fabs(t.v - w.v) <= (q->within > 0 ? q->within : 1e-9))
			return q->wrong_fb[i];
	}
	return NULL;
}

// a typed number as learn reads it: "1.42", "1.42 s", "-3", "1,000" (a thousands comma), "1/2"
static bool read_number(const char *s, const char *unit, char *clean, int cap) {
	int o = 0;
	while (*s == ' ') s++;
	for (const char *p = s; *p && o < cap - 1; p++) {
		if (*p == ',' && p[1] >= '0' && p[1] <= '9' && p[2] >= '0' && p[2] <= '9' && p[3] >= '0' && p[3] <= '9'
			&& !(p[4] >= '0' && p[4] <= '9')) continue;	// 1,000
		clean[o++] = *p;
	}
	clean[o] = 0;
	while (o > 0 && clean[o - 1] == ' ') clean[--o] = 0;
	if (unit && unit[0]) {
		int ul = (int)strlen(unit);
		if (o >= ul && !strcmp(clean + o - ul, unit)) {
			o -= ul;
			clean[o] = 0;
			while (o > 0 && clean[o - 1] == ' ') clean[--o] = 0;
		}
	}
	return o > 0;
}

lq_result_t lq_judge_typed(const lq_q_t *q, const char *answer) {
	static uint8_t mem[8192];
	lx_arena_t a;
	lx_arena_init(&a, mem, sizeof mem);
	lq_result_t r;
	switch (q->kind) {
	case LQ_NUMBER: {
		char clean[64];
		if (!read_number(answer, q->unit, clean, sizeof clean)) return res(LQ_UNREADABLE, "Type a number.");
		lm_val_t v = lm_eval_str(&a, clean, NULL, 0);
		if (!v.ok || v.is_bool) {
			r = res(LQ_UNREADABLE, "That is not a number learn can read. Type it like 1.5 or 3/4.");
			return r;
		}
		double tol = q->within > 0 ? q->within : 1e-9 * (fabs(q->answer) > 1 ? fabs(q->answer) : 1);
		if (fabs(v.v - q->answer) <= tol + 1e-12) return res(LQ_RIGHT, q->explain);
		return res(LQ_WRONG, wrong_if(q, clean, &a));
	}
	case LQ_FRACTION: {
		char clean[64];
		if (!read_number(answer, NULL, clean, sizeof clean)) return res(LQ_UNREADABLE, "Type a fraction, like 3/4.");
		size_t mark = a.used;
		char err[8];
		lm_node_t *n = lm_parse(&a, clean, (int)strlen(clean), err, sizeof err);
		lm_val_t v = n ? lm_eval(n) : (lm_val_t){ 0 };
		if (!n || !v.ok || !v.exact || v.is_bool) {
			a.used = mark;
			return res(LQ_UNREADABLE, "Type a fraction, like 3/4.");
		}
		// same value?
		bool same = v.num * (int64_t)q->fden == (int64_t)q->fnum * v.den;
		// written as exactly the answer? (a/b with these numbers, or a whole number)
		bool written = n->kind == LM_DIV && n->a->kind == LM_NUM && n->b->kind == LM_NUM;
		long tn = 0, td = 1;
		if (written) { sscanf(n->a->text, "%ld", &tn); sscanf(n->b->text, "%ld", &td); }
		a.used = mark;
		if (same) {
			bool exact_form = written ? (tn == q->fnum && td == q->fden) : (q->fden == 1);
			if (exact_form || q->equivalent) {
				if (q->simplest && written) {
					long x = tn < 0 ? -tn : tn, y = td;
					while (y) { long t = x % y; x = y; y = t; }
					if (x != 1) {
						r = res(LQ_NEARLY, "Right value, but it can be simplified: divide the top and bottom by the same number.");
						return r;
					}
				}
				return res(LQ_RIGHT, q->explain);
			}
			return res(LQ_NEARLY, "That is the same amount, written another way. Write it as the question asks.");
		}
		return res(LQ_WRONG, wrong_if(q, clean, &a));
	}
	case LQ_TEXT: {
		bool any_nearly = false;
		if (!answer[0]) return res(LQ_UNREADABLE, "Type your answer.");
		for (int i = 0; i < q->naccept; i++) {
			bool nearly;
			if (lq_text_equal(answer, q->accept[i], q->case_matters, &nearly)) return res(LQ_RIGHT, q->explain);
			if (nearly) any_nearly = true;
		}
		if (any_nearly)
			return res(q->accents_nearly ? LQ_NEARLY : LQ_WRONG,
				"Nearly: check the accents (an umlaut or an accent is part of the spelling).");
		return res(LQ_WRONG, wrong_if(q, answer, &a));
	}
	default:
		return res(LQ_UNREADABLE, NULL);
	}
}

lq_result_t lq_judge_order(const lq_q_t *q, const int *perm, int n) {
	if (n != q->nopt) return res(LQ_UNREADABLE, NULL);
	for (int i = 0; i < n; i++) if (perm[i] != i) return res(LQ_WRONG, NULL);
	return res(LQ_RIGHT, q->explain);
}

void lq_answer_text(const lq_q_t *q, char *out, int cap) {
	out[0] = 0;
	switch (q->kind) {
	case LQ_CHOICE: {
		int o = 0;
		for (int i = 0; i < q->nopt; i++) {
			const lq_opt_t *op = &q->opt[q->order[i]];
			if (!op->right) continue;
			o += snprintf(out + o, (size_t)(cap - o), "%s%s", o ? "; " : "", op->text);
			if (o >= cap) break;
		}
		break;
	}
	case LQ_NUMBER: {
		// as written in the lesson: a number learncheck has checked
		const lx_node_t *an = lx_find(q->form, "answer");
		const char *t = an && lx_nth(an, 1) ? lx_nth(an, 1)->text : "?";
		snprintf(out, (size_t)cap, "%s%s%s", t, q->unit ? " " : "", q->unit ? q->unit : "");
		break;
	}
	case LQ_FRACTION: snprintf(out, (size_t)cap, "%ld/%ld", q->fnum, q->fden); break;
	case LQ_TEXT: snprintf(out, (size_t)cap, "%s", q->naccept ? q->accept[0] : ""); break;
	case LQ_ORDER: {
		int o = 0;
		for (int i = 0; i < q->nopt; i++) {
			o += snprintf(out + o, (size_t)(cap - o), "%s%s", i ? ", " : "", q->opt[i].text);
			if (o >= cap) break;
		}
		break;
	}
	default: break;
	}
}
