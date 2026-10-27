/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- S-expressions read as data. lsx.h.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "lsx.h"

void lx_arena_init(lx_arena_t *a, void *mem, size_t size) {
	a->mem = (uint8_t *)mem;
	a->size = size;
	a->used = 0;
	a->oom = false;
}

void *lx_alloc(lx_arena_t *a, size_t n) {
	// aligned in memory, not just as an offset: the caller's buffer may not be
	size_t at = (size_t)((((uintptr_t)(a->mem + a->used) + 7) & ~(uintptr_t)7) - (uintptr_t)a->mem);
	if (at + n > a->size) { a->oom = true; return NULL; }
	a->used = at + n;
	memset(a->mem + at, 0, n);
	return a->mem + at;
}

char *lx_strdup(lx_arena_t *a, const char *s, size_t n) {
	char *d = (char *)lx_alloc(a, n + 1);
	if (!d) return NULL;
	memcpy(d, s, n);
	d[n] = 0;
	return d;
}

typedef struct {
	lx_arena_t	*a;
	const char	*s, *end;
	int			line;
	char		*err;
	int			errcap;
	bool		failed;
} rd_t;

static void fail(rd_t *r, const char *why) {
	if (r->failed) return;
	r->failed = true;
	if (r->err && r->errcap > 0) snprintf(r->err, (size_t)r->errcap, "line %d: %s", r->line, why);
}

static void skip_ws(rd_t *r) {
	while (r->s < r->end) {
		char c = *r->s;
		if (c == '\n') { r->line++; r->s++; }
		else if (c == ' ' || c == '\t' || c == '\r') r->s++;
		else if (c == ';') { while (r->s < r->end && *r->s != '\n') r->s++; }
		else break;
	}
}

static bool delim(char c) {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '(' || c == ')' ||
		c == '"' || c == ';';
}

static lx_node_t *node(rd_t *r, int kind) {
	lx_node_t *n = (lx_node_t *)lx_alloc(r->a, sizeof(lx_node_t));
	if (!n) { fail(r, "out of memory"); return NULL; }
	n->kind = (uint8_t)kind;
	n->line = (uint16_t)r->line;
	return n;
}

static bool numeric(const char *t, size_t n, double *v) {
	char buf[40];
	if (n == 0 || n >= sizeof buf) return false;
	memcpy(buf, t, n);
	buf[n] = 0;
	char *e;
	if (!(buf[0] == '-' || buf[0] == '+' || buf[0] == '.' || (buf[0] >= '0' && buf[0] <= '9')))
		return false;
	*v = strtod(buf, &e);
	return e != buf && *e == 0;
}

static lx_node_t *read_form(rd_t *r, int depth);

static lx_node_t *read_list(rd_t *r, int depth) {
	lx_node_t *l = node(r, LX_LIST), *tail = NULL;
	if (!l) return NULL;
	r->s++;		// (
	for (;;) {
		skip_ws(r);
		if (r->s >= r->end) { fail(r, "a ( is not closed"); return NULL; }
		if (*r->s == ')') { r->s++; return l; }
		lx_node_t *e = read_form(r, depth + 1);
		if (!e) return NULL;
		if (tail) tail->next = e; else l->head = e;
		tail = e;
	}
}

static lx_node_t *read_str(rd_t *r) {
	lx_node_t *n = node(r, LX_STR);
	if (!n) return NULL;
	r->s++;		// "
	// at most as long as the source; escapes only shorten it
	const char *p = r->s;
	size_t cap = 0;
	while (p < r->end && *p != '"') { if (*p == '\\' && p + 1 < r->end) p++; p++; cap++; }
	if (p >= r->end) { fail(r, "a string is not closed"); return NULL; }
	char *d = (char *)lx_alloc(r->a, cap + 1);
	if (!d) { fail(r, "out of memory"); return NULL; }
	size_t k = 0;
	while (*r->s != '"') {
		char c = *r->s++;
		if (c == '\n') r->line++;
		if (c == '\\') {
			c = *r->s++;
			if (c == 'n') c = '\n';
			else if (c == 't') c = '\t';
		}
		d[k++] = c;
	}
	d[k] = 0;
	r->s++;		// "
	n->text = d;
	return n;
}

static lx_node_t *read_form(rd_t *r, int depth) {
	if (depth > 64) { fail(r, "nested too deeply"); return NULL; }
	skip_ws(r);
	if (r->s >= r->end) { fail(r, "unexpected end"); return NULL; }
	char c = *r->s;
	if (c == '(') return read_list(r, depth);
	if (c == ')') { fail(r, "a ) without a ("); return NULL; }
	if (c == '"') return read_str(r);
	if (c == '\'') {	// 'sym reads as the symbol itself: data, so a quote means nothing
		r->s++;
		return read_form(r, depth + 1);
	}
	const char *t = r->s;
	while (r->s < r->end && !delim(*r->s)) r->s++;
	size_t len = (size_t)(r->s - t);
	double v;
	lx_node_t *n;
	if (numeric(t, len, &v)) { n = node(r, LX_NUM); if (n) n->num = v; }
	else n = node(r, LX_SYM);
	if (!n) return NULL;
	n->text = lx_strdup(r->a, t, len);
	if (!n->text) { fail(r, "out of memory"); return NULL; }
	return n;
}

lx_node_t *lx_read(lx_arena_t *a, const char *s, size_t len, int line0,
	char *err, int errcap) {
	rd_t r = { a, s, s + len, line0, err, errcap, false };
	if (err && errcap) err[0] = 0;
	lx_node_t *top = node(&r, LX_LIST), *tail = NULL;
	if (!top) return NULL;
	for (;;) {
		skip_ws(&r);
		if (r.s >= r.end) break;
		lx_node_t *f = read_form(&r, 0);
		if (!f) return NULL;
		if (tail) tail->next = f; else top->head = f;
		tail = f;
	}
	return r.failed ? NULL : top;
}

int lx_len(const lx_node_t *l) {
	int n = 0;
	if (!l || l->kind != LX_LIST) return 0;
	for (lx_node_t *e = l->head; e; e = e->next) n++;
	return n;
}

lx_node_t *lx_nth(const lx_node_t *l, int n) {
	if (!l || l->kind != LX_LIST) return NULL;
	lx_node_t *e = l->head;
	while (e && n-- > 0) e = e->next;
	return e;
}

bool lx_is_sym(const lx_node_t *n, const char *name) {
	return n && n->kind == LX_SYM && !strcmp(n->text, name);
}

bool lx_is_str(const lx_node_t *n) { return n && n->kind == LX_STR; }

bool lx_is_form(const lx_node_t *n, const char *name) {
	return n && n->kind == LX_LIST && lx_is_sym(n->head, name);
}

const char *lx_form_name(const lx_node_t *n) {
	if (!n || n->kind != LX_LIST || !n->head || n->head->kind != LX_SYM) return NULL;
	return n->head->text;
}

lx_node_t *lx_find_nth(const lx_node_t *form, const char *name, int nth) {
	if (!form || form->kind != LX_LIST || !form->head) return NULL;
	// a form (name ...) is searched after its name; a list of forms
	// (a whole block, as lx_read returns it) from its first element
	lx_node_t *e = form->head->kind == LX_SYM ? form->head->next : form->head;
	for (; e; e = e->next)
		if (lx_is_form(e, name) && nth-- == 0) return e;
	return NULL;
}

lx_node_t *lx_find(const lx_node_t *form, const char *name) {
	return lx_find_nth(form, name, 0);
}

int lx_count(const lx_node_t *form, const char *name) {
	int n = 0;
	while (lx_find_nth(form, name, n)) n++;
	return n;
}

const char *lx_str_of(const lx_node_t *form, const char *name) {
	lx_node_t *f = lx_find(form, name);
	if (!f || !f->head->next) return NULL;
	lx_node_t *v = f->head->next;
	return v->kind == LX_LIST ? NULL : v->text;
}

bool lx_num_of(const lx_node_t *form, const char *name, double *out) {
	lx_node_t *f = lx_find(form, name);
	if (!f || !f->head->next || f->head->next->kind != LX_NUM) return false;
	*out = f->head->next->num;
	return true;
}

bool lx_flag(const lx_node_t *form, const char *name) {
	lx_node_t *f = lx_find(form, name);
	if (!f) return false;
	lx_node_t *v = f->head->next;
	return !v || !lx_is_sym(v, "#f");
}
