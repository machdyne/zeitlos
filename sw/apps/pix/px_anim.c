/*
 * pix -- animations, as lines of the sheet. See px_anim.h.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "px_anim.h"

/* An anim line, its words apart. */
typedef struct {
	char name[PX_NAME_MAX];
	int  ms;
	int  n;
	char fr[PX_ANIM_FRAMES][PX_NAME_MAX];
} line_t;

static bool space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* The next word of s .. e into w; false at the end (or too long). */
static bool word(const char **s, const char *e, char *w, int len) {
	int n = 0;
	while (*s < e && space(**s)) (*s)++;
	if (*s >= e) return false;
	while (*s < e && !space(**s)) {
		if (n >= len - 1) return false;
		w[n++] = *(*s)++;
	}
	w[n] = 0;
	return true;
}

static bool number(const char *w, int *v) {
	char *end;
	long l = strtol(w, &end, 10);
	if (end == w || *end) return false;
	*v = (int)l;
	return true;
}

/* Is s .. e an anim line? */
static bool is_anim(const char *s, const char *e) {
	char w[8];
	return word(&s, e, w, sizeof(w)) && !strcmp(w, "anim");
}

/* The words of an anim line, from after "anim" (or the whole of what
 * px_anim_set() is given). False if it is not a good one. */
static bool parse(const char *s, const char *e, line_t *L, const char **err) {
	char w[256];
	memset(L, 0, sizeof(*L));
	if (!word(&s, e, w, sizeof(w))) { *err = "No name"; return false; }
	if (strlen(w) >= PX_NAME_MAX) { *err = "The name is too long"; return false; }
	strcpy(L->name, w);
	if (!word(&s, e, w, sizeof(w))) { L->ms = -1; return true; }   /* just a name */
	if (!number(w, &L->ms) || L->ms < PX_ANIM_MS_MIN || L->ms > PX_ANIM_MS_MAX) {
		*err = "The time is milliseconds, 10 to 10000";
		return false;
	}
	while (word(&s, e, w, sizeof(w))) {
		if (L->n >= PX_ANIM_FRAMES) { *err = "At most 64 frames"; return false; }
		if (strlen(w) >= PX_NAME_MAX) { *err = "A cell's name is too long"; return false; }
		strcpy(L->fr[L->n++], w);
	}
	/* a word too long even to read: not a line of ours, kept as it is */
	while (s < e && space(*s)) s++;
	if (s < e) { *err = "A word is too long"; return false; }
	return true;
}

/* "anim " and the line's words, with a newline. */
static int format(const line_t *L, char *out, int len) {
	int k, n = snprintf(out, (size_t)len, "anim %s %d", L->name, L->ms);
	for (k = 0; k < L->n && n < len; k++) n += snprintf(out + n, (size_t)(len - n), " %s", L->fr[k]);
	if (n < len - 1) { out[n++] = '\n'; out[n] = 0; }
	return n < len ? n : len - 1;
}

/* The anim lines in order: the k-th's start and end. */
static bool nth(const px_doc_t *d, int k, const char **s, const char **e) {
	const char *p = d->extra, *end = d->extra + d->extra_len;
	if (!p) return false;
	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		const char *le = nl ? nl : end;
		if (is_anim(p, le) && k-- == 0) { *s = p; *e = le; return true; }
		p = nl ? nl + 1 : end;
	}
	return false;
}

int px_anim_count(const px_doc_t *d) {
	const char *s, *e;
	int n = 0;
	while (nth(d, n, &s, &e)) n++;
	return n;
}

static bool get_line(const px_doc_t *d, int k, line_t *L) {
	const char *s, *e, *err;
	char w[8];
	if (!nth(d, k, &s, &e)) return false;
	word(&s, e, w, sizeof(w));          /* "anim" */
	return parse(s, e, L, &err) && L->ms > 0;
}

bool px_anim_get(const px_doc_t *d, int k, px_anim_t *a) {
	line_t L;
	int i;
	if (!get_line(d, k, &L)) return false;
	memset(a, 0, sizeof(*a));
	strcpy(a->name, L.name);
	a->ms = L.ms;
	a->n = L.n;
	for (i = 0; i < L.n; i++) a->cell[i] = px_find(d, L.fr[i]);
	return true;
}

int px_anim_for(const px_doc_t *d, int cell, px_anim_t *a, int *at) {
	int k, i, n = px_anim_count(d);
	*at = 0;
	for (k = 0; k < n; k++) {
		if (!px_anim_get(d, k, a)) continue;
		for (i = 0; i < a->n; i++)
			if (a->cell[i] == cell) { *at = i; return k; }
	}
	/* none: the cells of its size, around it if there are too many */
	memset(a, 0, sizeof(*a));
	a->ms = PX_ANIM_DEFAULT_MS;
	if (cell < 0 || cell >= d->ncells) return -1;
	{
		int w = d->cell[cell].w, h = d->cell[cell].h, from = 0, same = 0, before = 0;
		for (i = 0; i < d->ncells; i++)
			if (d->cell[i].w == w && d->cell[i].h == h) {
				if (i < cell) before++;
				same++;
			}
		if (same > PX_ANIM_FRAMES) from = before - PX_ANIM_FRAMES / 2 < 0 ? 0 : before - PX_ANIM_FRAMES / 2;
		for (i = 0, same = 0; i < d->ncells && a->n < PX_ANIM_FRAMES; i++)
			if (d->cell[i].w == w && d->cell[i].h == h && same++ >= from) {
				if (i == cell) *at = a->n;
				a->cell[a->n++] = i;
			}
	}
	return -1;
}

void px_anim_neighbours(const px_doc_t *d, int cell, int *prev, int *next) {
	px_anim_t a;
	int at;
	*prev = *next = -1;
	px_anim_for(d, cell, &a, &at);
	if (a.n < 2) return;
	*prev = a.cell[(at + a.n - 1) % a.n];
	*next = a.cell[(at + 1) % a.n];
	if (*prev == cell) *prev = -1;
	if (*next == cell) *next = -1;
}

/* The extra text again, every anim line passed through edit(), which
 * may change it or say to drop it (false); then `add`, if given, at the
 * end. False if out of memory, and nothing is changed. */
typedef bool (*edit_fn)(line_t *L, void *ctx);

static bool rebuild(px_doc_t *d, edit_fn edit, void *ctx, const line_t *add) {
	const char *p = d->extra, *end = d->extra ? d->extra + d->extra_len : NULL;
	uint32_t cap = d->extra_len + 4096, n = 0;
	char *out = malloc(cap), one[PX_ANIM_FRAMES * (PX_NAME_MAX + 1) + 64];
	if (!out) return false;
	while (p && p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		const char *le = nl ? nl : end;
		const char *s = p, *err;
		char w[8];
		line_t L;
		int len;
		if (edit && is_anim(p, le) && (word(&s, le, w, sizeof(w)), parse(s, le, &L, &err)) && L.ms > 0) {
			if (!edit(&L, ctx)) { p = nl ? nl + 1 : end; continue; }
			len = format(&L, one, sizeof(one));
		} else {
			len = (int)(le - p);
			if (len >= (int)sizeof(one) - 1) {
				/* a long line that is not ours: as it is */
				if (n + (uint32_t)len + 2 > cap) {
					char *o = realloc(out, cap = n + (uint32_t)len + 4096);
					if (!o) { free(out); return false; }
					out = o;
				}
				memcpy(out + n, p, (size_t)len);
				n += (uint32_t)len;
				out[n++] = '\n';
				p = nl ? nl + 1 : end;
				continue;
			}
			memcpy(one, p, (size_t)len);
			one[len++] = '\n';
		}
		if (n + (uint32_t)len + 1 > cap) {
			char *o = realloc(out, cap = n + (uint32_t)len + 4096);
			if (!o) { free(out); return false; }
			out = o;
		}
		memcpy(out + n, one, (size_t)len);
		n += (uint32_t)len;
		p = nl ? nl + 1 : end;
	}
	if (add) {
		int len = format(add, one, sizeof(one));
		if (n + (uint32_t)len + 1 > cap) {
			char *o = realloc(out, cap = n + (uint32_t)len + 1);
			if (!o) { free(out); return false; }
			out = o;
		}
		memcpy(out + n, one, (size_t)len);
		n += (uint32_t)len;
	}
	out[n] = 0;
	free(d->extra);
	if (n == 0) { free(out); out = NULL; }
	d->extra = out;
	d->extra_len = n;
	d->dirty = true;
	return true;
}

/* -- setting -- */

typedef struct {
	const line_t *with;         /* the new line (ms -1: delete) */
	bool found;
} set_ctx;

static bool set_edit(line_t *L, void *v) {
	set_ctx *c = v;
	if (strcmp(L->name, c->with->name)) return true;
	if (c->found || c->with->ms < 0) { c->found = true; return false; }   /* a second of the name, or deleting */
	*L = *c->with;
	c->found = true;
	return true;
}

bool px_anim_set(px_doc_t *d, const char *words, const char **err) {
	line_t L;
	set_ctx c;
	int i;
	if (d->kind != PX_ZSP) { *err = "Only a sprite sheet keeps animations"; return false; }
	if (!parse(words, words + strlen(words), &L, err)) return false;
	if (L.ms > 0 && L.n == 0) { *err = "No frames"; return false; }
	for (i = 0; i < L.n; i++)
		if (px_find(d, L.fr[i]) < 0) { *err = "No cell of that name"; return false; }
	c.with = &L;
	c.found = false;
	if (L.ms < 0) {
		/* deleting: there must be one */
		px_anim_t a;
		int k, n = px_anim_count(d);
		for (k = 0; k < n; k++) if (px_anim_get(d, k, &a) && !strcmp(a.name, L.name)) break;
		if (k == n) { *err = "No animation of that name"; return false; }
		if (!rebuild(d, set_edit, &c, NULL)) { *err = "Out of memory"; return false; }
		return true;
	}
	{
		/* replace in place if there is one, else add at the end */
		px_anim_t a;
		int k, n = px_anim_count(d);
		bool have = false;
		for (k = 0; k < n; k++) if (px_anim_get(d, k, &a) && !strcmp(a.name, L.name)) have = true;
		if (!rebuild(d, set_edit, &c, have ? NULL : &L)) { *err = "Out of memory"; return false; }
	}
	return true;
}

bool px_anim_words(const px_doc_t *d, int k, char *out, int len) {
	line_t L;
	char one[PX_ANIM_FRAMES * (PX_NAME_MAX + 1) + 64];
	int n;
	if (!get_line(d, k, &L)) return false;
	n = format(&L, one, sizeof(one));
	if (n && one[n - 1] == '\n') one[--n] = 0;
	snprintf(out, (size_t)len, "%s", one + 5);    /* past "anim " */
	return true;
}

/* -- following the cells -- */

typedef struct { const char *was, *now; } rename_ctx;

static bool rename_edit(line_t *L, void *v) {
	rename_ctx *c = v;
	int i;
	for (i = 0; i < L->n; i++)
		if (!strcmp(L->fr[i], c->was)) snprintf(L->fr[i], PX_NAME_MAX, "%s", c->now);
	return true;
}

void px_anim_rename(px_doc_t *d, const char *was, const char *now) {
	rename_ctx c;
	if (!px_anim_count(d) || !strcmp(was, now)) return;
	c.was = was;
	c.now = now;
	rebuild(d, rename_edit, &c, NULL);
}

static bool forget_edit(line_t *L, void *v) {
	const char *name = v;
	int i, j = 0;
	for (i = 0; i < L->n; i++)
		if (strcmp(L->fr[i], name)) memmove(L->fr[j++], L->fr[i], PX_NAME_MAX);
	L->n = j;
	return j > 0;
}

void px_anim_forget(px_doc_t *d, const char *name) {
	if (!px_anim_count(d)) return;
	rebuild(d, forget_edit, (void *)name, NULL);
}

typedef struct { const char *after, *added; } insert_ctx;

static bool insert_edit(line_t *L, void *v) {
	insert_ctx *c = v;
	int i;
	for (i = 0; i < L->n && L->n < PX_ANIM_FRAMES; i++)
		if (!strcmp(L->fr[i], c->after)) {
			memmove(L->fr[i + 2], L->fr[i + 1], (size_t)(L->n - i - 1) * PX_NAME_MAX);
			snprintf(L->fr[i + 1], PX_NAME_MAX, "%s", c->added);
			L->n++;
			i++;
		}
	return true;
}

void px_anim_insert_after(px_doc_t *d, const char *after, const char *added) {
	insert_ctx c;
	if (!px_anim_count(d)) return;
	c.after = after;
	c.added = added;
	rebuild(d, insert_edit, &c, NULL);
}

void px_anim_next_name(const px_doc_t *d, const char *name, char *out) {
	char stem[PX_NAME_MAX];
	int len = (int)strlen(name), k = len, num = 1, width;
	while (k > 0 && name[k - 1] >= '0' && name[k - 1] <= '9') k--;
	if (len - k > 6) k = len;               /* a number too long to count on: not one */
	if (k < len) num = atoi(name + k) + 1;
	else num = 2;
	width = len - k;
	snprintf(stem, sizeof(stem), "%.*s", k > PX_NAME_MAX - 8 ? PX_NAME_MAX - 8 : k, name);
	if (width > 6) width = 6;
	for (;; num++) {
		char dg[12];
		int nd = 0, v = num, sl = (int)strlen(stem);
		/* the number, at least `width` digits */
		do { dg[nd++] = (char)('0' + v % 10); v /= 10; } while (v && nd < 10);
		while (nd < width) dg[nd++] = '0';
		if (sl + nd > PX_NAME_MAX - 1) sl = PX_NAME_MAX - 1 - nd;
		memcpy(out, stem, (size_t)sl);
		while (nd) out[sl++] = dg[--nd];
		out[sl] = 0;
		if (px_find(d, out) < 0) return;
	}
}
