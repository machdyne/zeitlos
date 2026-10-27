/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- formulas: parsing and evaluation. lmath.h.
 */

#include <string.h>
#include <stdio.h>
#include <math.h>

#include "lmath.h"

typedef struct {
	lx_arena_t	*a;
	const char	*s, *end;
	char		*err;
	int			errcap;
	bool		failed;
} ps_t;

static void fail(ps_t *p, const char *why) {
	if (p->failed) return;
	p->failed = true;
	if (p->err && p->errcap) snprintf(p->err, (size_t)p->errcap, "formula: %s", why);
}

static lm_node_t *mk(ps_t *p, int kind) {
	lm_node_t *n = (lm_node_t *)lx_alloc(p->a, sizeof(lm_node_t));
	if (!n) { fail(p, "out of memory"); return NULL; }
	n->kind = (uint8_t)kind;
	return n;
}

static void ws(ps_t *p) { while (p->s < p->end && (*p->s == ' ' || *p->s == '\t')) p->s++; }

static bool at(ps_t *p, const char *t) {
	size_t n = strlen(t);
	return (size_t)(p->end - p->s) >= n && !memcmp(p->s, t, n);
}

static bool eat(ps_t *p, const char *t) {
	ws(p);
	if (!at(p, t)) return false;
	p->s += strlen(t);
	return true;
}

static bool isdig(char c) { return c >= '0' && c <= '9'; }
static bool isalpha_(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

static lm_node_t *expr(ps_t *p);
static lm_node_t *unary(ps_t *p);

static const char *const greek[][2] = {
	{ "alpha", "\xCE\xB1" }, { "beta", "\xCE\xB2" }, { "gamma", "\xCE\xB3" }, { "delta", "\xCE\xB4" },
	{ "epsilon", "\xCE\xB5" }, { "zeta", "\xCE\xB6" }, { "eta", "\xCE\xB7" }, { "theta", "\xCE\xB8" },
	{ "iota", "\xCE\xB9" }, { "kappa", "\xCE\xBA" }, { "lambda", "\xCE\xBB" }, { "mu", "\xCE\xBC" },
	{ "nu", "\xCE\xBD" }, { "xi", "\xCE\xBE" }, { "omicron", "\xCE\xBF" }, { "pi", "\xCF\x80" },
	{ "rho", "\xCF\x81" }, { "sigma", "\xCF\x83" }, { "tau", "\xCF\x84" }, { "upsilon", "\xCF\x85" },
	{ "phi", "\xCF\x86" }, { "chi", "\xCF\x87" }, { "psi", "\xCF\x88" }, { "omega", "\xCF\x89" },
	{ "Gamma", "\xCE\x93" }, { "Delta", "\xCE\x94" }, { "Theta", "\xCE\x98" }, { "Lambda", "\xCE\x9B" },
	{ "Xi", "\xCE\x9E" }, { "Pi", "\xCE\xA0" }, { "Sigma", "\xCE\xA3" }, { "Phi", "\xCE\xA6" },
	{ "Psi", "\xCE\xA8" }, { "Omega", "\xCE\xA9" },
};

const char *lm_greek(const char *name) {
	for (size_t i = 0; i < sizeof greek / sizeof greek[0]; i++)
		if (!strcmp(name, greek[i][0])) return greek[i][1];
	return NULL;
}

static const char *const funcs[] = { "sqrt", "abs", "sin", "cos", "tan", "ln", "log", "exp", NULL };

static bool is_func(const char *n) {
	for (int i = 0; funcs[i]; i++) if (!strcmp(n, funcs[i])) return true;
	return false;
}

static lm_node_t *number(ps_t *p) {
	const char *t = p->s;
	while (p->s < p->end && (isdig(*p->s) || *p->s == '.')) p->s++;
	// 1,000 is not a number here: a comma separates nothing in a formula
	lm_node_t *n = mk(p, LM_NUM);
	if (!n) return NULL;
	n->text = lx_strdup(p->a, t, (size_t)(p->s - t));
	if (!n->text) { fail(p, "out of memory"); return NULL; }
	return n;
}

static bool is_int_text(const lm_node_t *n) {
	return n && n->kind == LM_NUM && !strchr(n->text, '.');
}

static lm_node_t *atom(ps_t *p) {
	ws(p);
	if (p->s >= p->end) { fail(p, "something is missing at the end"); return NULL; }
	char c = *p->s;
	if (c == '(') {
		p->s++;
		lm_node_t *in = expr(p);
		if (!in) return NULL;
		if (!eat(p, ")")) { fail(p, "a ( is not closed"); return NULL; }
		lm_node_t *n = mk(p, LM_PAREN);
		if (n) n->a = in;
		return n;
	}
	if (isdig(c) || (c == '.' && p->s + 1 < p->end && isdig(p->s[1]))) return number(p);
	if (isalpha_(c)) {
		const char *t = p->s;
		while (p->s < p->end && isalpha_(*p->s)) p->s++;
		char name[24];
		size_t len = (size_t)(p->s - t);
		if (len >= sizeof name) { fail(p, "a name is too long"); return NULL; }
		memcpy(name, t, len);
		name[len] = 0;
		if (is_func(name)) {
			ws(p);
			if (!eat(p, "(")) { fail(p, "a function needs ( )"); return NULL; }
			lm_node_t *arg = expr(p);
			if (!arg) return NULL;
			if (!eat(p, ")")) { fail(p, "a ( is not closed"); return NULL; }
			lm_node_t *n = mk(p, LM_FUNC);
			if (!n) return NULL;
			n->text = lx_strdup(p->a, name, len);
			n->a = arg;
			return n;
		}
		// a known word (pi, theta, ...) is one name; otherwise each letter
		// is its own variable, so `xy` is x times y and `2ab` is 2 a b
		if (!lm_greek(name) && strcmp(name, "e") && len > 1) {
			p->s = t + 1;
			len = 1;
		}
		lm_node_t *n = mk(p, LM_VAR);
		if (!n) return NULL;
		n->text = lx_strdup(p->a, t, len);
		return n;
	}
	fail(p, "a character this notation does not know");
	return NULL;
}

static lm_node_t *postfix(ps_t *p) {
	lm_node_t *n = atom(p);
	while (n && !p->failed) {
		if (eat(p, "_")) {
			lm_node_t *s = atom(p);
			lm_node_t *m = mk(p, LM_SUB_SCRIPT);
			if (!m || !s) return NULL;
			m->a = n; m->b = s; n = m;
		} else if (eat(p, "%")) {
			lm_node_t *m = mk(p, LM_PCT);
			if (!m) return NULL;
			m->a = n; n = m;
		} else break;
	}
	return n;
}

static lm_node_t *power(ps_t *p) {
	lm_node_t *b = postfix(p);
	if (b && eat(p, "^")) {
		lm_node_t *e = unary(p);		// right-associative: 2^3^2 = 2^(3^2)
		lm_node_t *n = mk(p, LM_POW);
		if (!n || !e) return NULL;
		n->a = b; n->b = e;
		return n;
	}
	return b;
}

static lm_node_t *unary(ps_t *p) {
	if (eat(p, "-") || eat(p, "\xE2\x88\x92")) {		// - or U+2212
		lm_node_t *a = unary(p);
		lm_node_t *n = mk(p, LM_NEG);
		if (!n || !a) return NULL;
		n->a = a;
		return n;
	}
	if (eat(p, "+")) return unary(p);
	return power(p);
}

static bool starts_atom(ps_t *p) {
	ws(p);
	if (p->s >= p->end) return false;
	char c = *p->s;
	return c == '(' || isdig(c) || isalpha_(c) || c == '.';
}

static lm_node_t *term(ps_t *p) {
	lm_node_t *l = unary(p);
	while (l && !p->failed) {
		int k;
		if (eat(p, "*") || eat(p, "\xC3\x97") || eat(p, "\xC2\xB7")) k = LM_MUL;	// * × ·
		else if (eat(p, "\xC3\xB7")) k = LM_IDIV;		// ÷
		else if (eat(p, "/")) k = LM_DIV;
		else if (starts_atom(p)) {
			// 2 1/2: an integer, a space, then integer/integer -- a mixed number
			const char *save = p->s;
			if (is_int_text(l) && isdig(*p->s)) {
				lm_node_t *n1 = number(p);
				if (n1 && is_int_text(n1) && eat(p, "/")) {
					ws(p);
					if (p->s < p->end && isdig(*p->s)) {
						lm_node_t *d1 = number(p);
						if (d1 && is_int_text(d1)) {
							lm_node_t *m = mk(p, LM_MIXED);
							if (!m) return NULL;
							m->a = l; m->b = n1; m->c = d1;
							l = m;
							continue;
						}
					}
				}
				p->s = save;
			}
			k = LM_IMUL;
		} else break;
		lm_node_t *r = unary(p);
		lm_node_t *n = mk(p, k);
		if (!n || !r) return NULL;
		n->a = l; n->b = r; l = n;
	}
	return l;
}

static lm_node_t *sum(ps_t *p) {
	lm_node_t *l = term(p);
	while (l && !p->failed) {
		int k;
		if (eat(p, "+")) k = LM_ADD;
		else if (eat(p, "-") || eat(p, "\xE2\x88\x92")) k = LM_SUB;
		else break;
		lm_node_t *r = term(p);
		lm_node_t *n = mk(p, k);
		if (!n || !r) return NULL;
		n->a = l; n->b = r; l = n;
	}
	return l;
}

static int cmp_op(ps_t *p) {
	if (eat(p, "<=") || eat(p, "\xE2\x89\xA4")) return LM_LE;	// <= ≤
	if (eat(p, ">=") || eat(p, "\xE2\x89\xA5")) return LM_GE;	// >= ≥
	if (eat(p, "!=") || eat(p, "\xE2\x89\xA0")) return LM_NE;	// != ≠
	if (eat(p, "<")) return LM_LT;
	if (eat(p, ">")) return LM_GT;
	if (eat(p, "=")) return LM_EQ;
	return 0;
}

static lm_node_t *expr(ps_t *p) {
	lm_node_t *first = sum(p);
	if (!first) return NULL;
	int op = cmp_op(p);
	if (!op) return first;
	lm_node_t *c = mk(p, LM_CMP);
	if (!c) return NULL;
	c->kids[c->nkids++] = first;
	while (op) {
		if (c->nkids >= 8) { fail(p, "too many comparisons in a row"); return NULL; }
		lm_node_t *r = sum(p);
		if (!r) return NULL;
		c->ops[c->nkids - 1] = (uint8_t)op;
		c->kids[c->nkids++] = r;
		op = cmp_op(p);
	}
	return c;
}

lm_node_t *lm_parse(lx_arena_t *a, const char *s, int len, char *err, int errcap) {
	ps_t p = { a, s, s + len, err, errcap, false };
	if (err && errcap) err[0] = 0;
	lm_node_t *n = expr(&p);
	ws(&p);
	if (!p.failed && p.s < p.end) fail(&p, "something after the end of the formula");
	if (!p.failed && !n) fail(&p, "empty");
	return p.failed ? NULL : n;
}

// -- evaluation: exact rationals while they fit, doubles after --

static int64_t gcd64(int64_t a, int64_t b) {
	if (a < 0) a = -a;
	if (b < 0) b = -b;
	while (b) { int64_t t = a % b; a = b; b = t; }
	return a ? a : 1;
}

static lm_val_t bad(void) { lm_val_t v; memset(&v, 0, sizeof v); return v; }

static lm_val_t real(double d) {
	lm_val_t v = bad();
	if (isnan(d) || isinf(d)) return v;
	v.ok = true;
	v.v = d;
	return v;
}

static lm_val_t rat(int64_t n, int64_t d) {
	if (d == 0) return bad();
	if (d < 0) { n = -n; d = -d; }
	int64_t g = gcd64(n, d);
	lm_val_t v = bad();
	v.ok = v.exact = true;
	v.num = n / g;
	v.den = d / g;
	v.v = (double)v.num / (double)v.den;
	return v;
}

#define BIG 3037000499LL	// sqrt(2^63): products of two of these fit

static bool small(int64_t x) { return x > -BIG && x < BIG; }

static lm_val_t num_text(const char *t) {
	int64_t n = 0, d = 1;
	double dv = 0, scale = 1;
	bool frac = false, fits = true;
	int digits = 0, dots = 0;
	for (const char *c = t; *c; c++) {
		if (*c == '.') { frac = true; if (++dots > 1) return bad(); continue; }
		int k = *c - '0';
		dv = dv * 10 + k;
		if (frac) scale *= 10;
		if (++digits > 17) fits = false;
		if (fits) { n = n * 10 + k; if (frac) d *= 10; }
	}
	if (digits == 0) return bad();
	return fits ? rat(n, d) : real(dv / scale);
}

static lm_val_t add(lm_val_t a, lm_val_t b, int sign) {
	if (!a.ok || !b.ok || a.is_bool || b.is_bool) return bad();
	if (a.exact && b.exact && small(a.num) && small(a.den) && small(b.num) && small(b.den))
		return rat(a.num * b.den + sign * b.num * a.den, a.den * b.den);
	return real(a.v + sign * b.v);
}

static lm_val_t mul(lm_val_t a, lm_val_t b) {
	if (!a.ok || !b.ok || a.is_bool || b.is_bool) return bad();
	if (a.exact && b.exact && small(a.num) && small(a.den) && small(b.num) && small(b.den))
		return rat(a.num * b.num, a.den * b.den);
	return real(a.v * b.v);
}

static lm_val_t dvd(lm_val_t a, lm_val_t b) {
	if (!a.ok || !b.ok || a.is_bool || b.is_bool) return bad();
	if (b.exact ? b.num == 0 : b.v == 0) return bad();
	if (a.exact && b.exact && small(a.num) && small(a.den) && small(b.num) && small(b.den))
		return rat(a.num * b.den, a.den * b.num);
	return real(a.v / b.v);
}

static lm_val_t pw(lm_val_t a, lm_val_t b) {
	if (!a.ok || !b.ok || a.is_bool || b.is_bool) return bad();
	if (a.exact && b.exact && b.den == 1 && b.num >= -62 && b.num <= 62) {
		lm_val_t r = rat(1, 1);
		int64_t e = b.num < 0 ? -b.num : b.num;
		for (int64_t i = 0; i < e; i++) {
			r = mul(r, a);
			if (!r.ok) return r;
		}
		return b.num < 0 ? dvd(rat(1, 1), r) : r;
	}
	if (a.v < 0 && floor(b.v) != b.v) return bad();
	return real(pow(a.v, b.v));
}

static lm_val_t func(const char *f, lm_val_t x) {
	if (!x.ok || x.is_bool) return bad();
	double v = x.v;
	if (!strcmp(f, "abs")) return x.exact ? rat(x.num < 0 ? -x.num : x.num, x.den) : real(fabs(v));
	if (!strcmp(f, "sqrt")) {
		if (v < 0) return bad();
		if (x.exact) {	// exact when both parts are squares
			int64_t rn = (int64_t)llround(sqrt((double)x.num)), rd = (int64_t)llround(sqrt((double)x.den));
			if (rn * rn == x.num && rd * rd == x.den) return rat(rn, rd);
		}
		return real(sqrt(v));
	}
	if (!strcmp(f, "sin")) return real(sin(v));
	if (!strcmp(f, "cos")) return real(cos(v));
	if (!strcmp(f, "tan")) return real(tan(v));
	if (!strcmp(f, "exp")) return real(exp(v));
	if (!strcmp(f, "ln")) return v > 0 ? real(log(v)) : bad();
	if (!strcmp(f, "log")) return v > 0 ? real(log10(v)) : bad();
	return bad();
}

static int cmpv(lm_val_t a, lm_val_t b) {
	if (a.exact && b.exact && small(a.num) && small(a.den) && small(b.num) && small(b.den)) {
		int64_t l = a.num * b.den, r = b.num * a.den;
		return l < r ? -1 : l > r;
	}
	// doubles: equal to 1 part in 10^12, so 0.1 + 0.2 = 0.3 holds
	double d = a.v - b.v, m = fabs(a.v) > fabs(b.v) ? fabs(a.v) : fabs(b.v);
	if (fabs(d) <= 1e-12 * (m > 1 ? m : 1)) return 0;
	return d < 0 ? -1 : 1;
}

lm_val_t lm_eval(const lm_node_t *n) {
	if (!n) return bad();
	switch (n->kind) {
	case LM_NUM: return num_text(n->text);
	case LM_VAR:
		if (!strcmp(n->text, "pi")) return real(M_PI);
		if (!strcmp(n->text, "e")) return real(M_E);
		return bad();
	case LM_ADD: return add(lm_eval(n->a), lm_eval(n->b), 1);
	case LM_SUB: return add(lm_eval(n->a), lm_eval(n->b), -1);
	case LM_MUL: case LM_IMUL: return mul(lm_eval(n->a), lm_eval(n->b));
	case LM_DIV: case LM_IDIV: return dvd(lm_eval(n->a), lm_eval(n->b));
	case LM_NEG: return mul(rat(-1, 1), lm_eval(n->a));
	case LM_POW: return pw(lm_eval(n->a), lm_eval(n->b));
	case LM_PCT: return dvd(lm_eval(n->a), rat(100, 1));
	case LM_PAREN: return lm_eval(n->a);
	case LM_FUNC: return func(n->text, lm_eval(n->a));
	case LM_SUB_SCRIPT: return bad();	// x_1 is a name
	case LM_MIXED: {
		lm_val_t w = lm_eval(n->a), f = dvd(lm_eval(n->b), lm_eval(n->c));
		return add(w, f, 1);
	}
	case LM_CMP: {
		lm_val_t prev = lm_eval(n->kids[0]);
		bool all = true;
		for (int i = 1; i < n->nkids; i++) {
			lm_val_t cur = lm_eval(n->kids[i]);
			if (!prev.ok || !cur.ok || prev.is_bool || cur.is_bool) return bad();
			int c = cmpv(prev, cur);
			bool t;
			switch (n->ops[i - 1]) {
			case LM_LT: t = c < 0; break;
			case LM_GT: t = c > 0; break;
			case LM_LE: t = c <= 0; break;
			case LM_GE: t = c >= 0; break;
			case LM_EQ: t = c == 0; break;
			default: t = c != 0; break;
			}
			all = all && t;
			prev = cur;
		}
		lm_val_t v = bad();
		v.ok = v.is_bool = true;
		v.truth = all;
		return v;
	}
	}
	return bad();
}

lm_val_t lm_eval_str(lx_arena_t *a, const char *s, char *err, int errcap) {
	lm_node_t *n = lm_parse(a, s, (int)strlen(s), err, errcap);
	if (!n) return bad();
	return lm_eval(n);
}

bool lm_has_vars(const lm_node_t *n) {
	if (!n) return false;
	if (n->kind == LM_VAR) return strcmp(n->text, "pi") && strcmp(n->text, "e");
	if (n->kind == LM_SUB_SCRIPT) return true;
	if (n->kind == LM_CMP) {
		for (int i = 0; i < n->nkids; i++) if (lm_has_vars(n->kids[i])) return true;
		return false;
	}
	return lm_has_vars(n->a) || lm_has_vars(n->b) || lm_has_vars(n->c);
}
