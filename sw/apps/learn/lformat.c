/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- courses and lessons as files. lformat.h, docs/learn_format.md.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "lformat.h"

static const char *const data_blocks[] = {
	"lesson", "check", "example", "cards", "try", "say", "note", "warn", "sources", NULL
};

bool lf_is_data_block(const char *name) {
	for (int i = 0; data_blocks[i]; i++) if (!strcmp(name, data_blocks[i])) return true;
	return false;
}

// -- lines --

typedef struct {
	const char	*s, *end;
	int			line;
} lines_t;

// the next line, without its newline (and a \r before it); false at the end
static bool next_line(lines_t *l, const char **ls, int *ll) {
	if (l->s >= l->end) return false;
	const char *e = memchr(l->s, '\n', (size_t)(l->end - l->s));
	if (!e) e = l->end;
	*ls = l->s;
	*ll = (int)(e - l->s);
	if (*ll > 0 && (*ls)[*ll - 1] == '\r') (*ll)--;
	l->s = e < l->end ? e + 1 : e;
	l->line++;
	return true;
}

static bool blank(const char *s, int n) {
	for (int i = 0; i < n; i++) if (s[i] != ' ' && s[i] != '\t') return false;
	return true;
}

static int indent(const char *s, int n) {
	int i = 0;
	while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
	return i;
}

static void trim(const char **s, int *n) {
	while (*n > 0 && (**s == ' ' || **s == '\t')) { (*s)++; (*n)--; }
	while (*n > 0 && ((*s)[*n - 1] == ' ' || (*s)[*n - 1] == '\t')) (*n)--;
}

static bool is_rule(const char *s, int n) {	// --- alone: the step separator
	trim(&s, &n);
	if (n < 3) return false;
	for (int i = 0; i < n; i++) if (s[i] != '-') return false;
	return true;
}

static bool is_fence(const char *s, int n) {
	int i = indent(s, n);
	return n - i >= 3 && !memcmp(s + i, "```", 3);
}

static int list_marker(const char *s, int n, int *num) {	// length of "- " / "12. ", or 0
	int i = indent(s, n);
	*num = 0;
	if (i < n - 1 && (s[i] == '-' || s[i] == '*' || s[i] == '+') && s[i + 1] == ' ') return i + 2;
	int j = i, v = 0;
	while (j < n && s[j] >= '0' && s[j] <= '9' && j - i < 4) v = v * 10 + (s[j++] - '0');
	if (j > i && j < n - 1 && (s[j] == '.' || s[j] == ')') && s[j + 1] == ' ') {
		*num = v ? v : 1;
		return j + 2;
	}
	return 0;
}

typedef struct {
	lx_arena_t		*a;
	lf_lesson_t		*l;
	char			*err;
	int				errcap;
	bool			failed;
	// a paragraph being gathered
	char			*para;
	int				para_len, para_cap, para_line, para_kind, para_level, para_num;
} pz_t;

static void pfail(pz_t *p, int line, const char *why) {
	if (p->failed) return;
	p->failed = true;
	if (p->err && p->errcap) snprintf(p->err, (size_t)p->errcap, "line %d: %s", line, why);
}

static lf_block_t *add_block(pz_t *p, int kind, int line) {
	lf_lesson_t *l = p->l;
	if (l->nblocks >= LF_MAX_BLOCKS) { pfail(p, line, "too many blocks in one lesson"); return NULL; }
	lf_block_t *b = &l->blocks[l->nblocks++];
	memset(b, 0, sizeof *b);
	b->kind = (uint8_t)kind;
	b->line = (uint16_t)line;
	if (l->nsteps == 0) { l->nsteps = 1; l->steps[0].first = 0; }
	l->steps[l->nsteps - 1].count++;
	return b;
}

static void flush_para(pz_t *p) {
	if (!p->para_len) return;
	lf_block_t *b = add_block(p, p->para_kind, p->para_line);
	if (b) {
		b->text = lx_strdup(p->a, p->para, (size_t)p->para_len);
		b->level = (uint8_t)p->para_level;
		b->num = (uint16_t)p->para_num;
		if (!b->text) pfail(p, p->para_line, "out of memory");
	}
	p->para_len = 0;
}

static void para_add(pz_t *p, int kind, int level, int num, int line, const char *s, int n,
	char sep) {
	if (p->para_len && (kind != p->para_kind || kind == LB_ITEM)) flush_para(p);
	if (!p->para_len) {
		p->para_kind = kind;
		p->para_level = level;
		p->para_num = num;
		p->para_line = line;
	}
	if (p->para_len + n + 2 > p->para_cap) { pfail(p, line, "a paragraph is too long"); return; }
	if (p->para_len) p->para[p->para_len++] = sep;
	memcpy(p->para + p->para_len, s, (size_t)n);
	p->para_len += n;
}

static void new_step(pz_t *p, int line) {
	lf_lesson_t *l = p->l;
	if (l->nsteps == 0) { l->nsteps = 1; l->steps[0].first = 0; }
	if (l->steps[l->nsteps - 1].count == 0) return;		// an empty step is not a step
	if (l->nsteps >= LF_MAX_STEPS) { pfail(p, line, "too many steps in one lesson"); return; }
	l->steps[l->nsteps].first = l->nblocks;
	l->steps[l->nsteps].count = 0;
	l->nsteps++;
}

bool lf_lesson_parse(lx_arena_t *a, const char *text, long len, lf_lesson_t *out,
	char *err, int errcap) {
	memset(out, 0, sizeof *out);
	if (err && errcap) err[0] = 0;
	if (len > LF_MAX_LESSON) {
		if (err) snprintf(err, (size_t)errcap, "line 1: the lesson is longer than %d bytes", LF_MAX_LESSON);
		return false;
	}
	pz_t p;
	memset(&p, 0, sizeof p);
	p.a = a;
	p.l = out;
	p.err = err;
	p.errcap = errcap;
	p.para_cap = 4096;
	p.para = (char *)lx_alloc(a, (size_t)p.para_cap);
	if (!p.para) { if (err) snprintf(err, (size_t)errcap, "line 1: out of memory"); return false; }

	lines_t L = { text, text + len, 0 };
	const char *s;
	int n;
	while (!p.failed && next_line(&L, &s, &n)) {
		int line = L.line;
		if (blank(s, n)) { flush_para(&p); continue; }

		if (is_fence(s, n)) {
			flush_para(&p);
			const char *info = s + indent(s, n) + 3;
			int il = (int)(s + n - info);
			trim(&info, &il);
			char name[LF_NAME];
			int k = 0;
			while (k < il && k < LF_NAME - 1 && info[k] != ' ') { name[k] = info[k]; k++; }
			name[k] = 0;
			const char *body = L.s;
			const char *bend = NULL;
			int body_line = L.line + 1;
			while (next_line(&L, &s, &n)) {
				if (is_fence(s, n)) { bend = s; break; }
			}
			if (!bend) { pfail(&p, line, "a ``` block is not closed"); break; }
			int blen = (int)(bend - body);
			int kind = !strcmp(name, "figure") ? LB_FIGURE :
				lf_is_data_block(name) ? LB_DATA : LB_CODE;
			lf_block_t *b = add_block(&p, kind, line);
			if (!b) break;
			b->name = lx_strdup(a, name, strlen(name));
			b->text = lx_strdup(a, body, (size_t)(blen > 0 ? blen : 0));
			if (!b->name || !b->text) { pfail(&p, line, "out of memory"); break; }
			if (kind == LB_DATA) {
				char e2[96];
				b->forms = lx_read(a, body, (size_t)(blen > 0 ? blen : 0), body_line, e2, sizeof e2);
				if (!b->forms) {
					p.failed = true;
					if (err && errcap) snprintf(err, (size_t)errcap, "%s (in the %s block)", e2, name);
					break;
				}
				if (!strcmp(name, "lesson")) out->meta = b->forms;
			}
			continue;
		}

		if (is_rule(s, n)) { flush_para(&p); new_step(&p, line); continue; }

		int i = indent(s, n);
		const char *t = s + i;
		int tl = n - i;

		if (t[0] == '#') {
			flush_para(&p);
			int lev = 0;
			while (lev < tl && t[lev] == '#') lev++;
			const char *h = t + lev;
			int hl = tl - lev;
			trim(&h, &hl);
			if (lev == 1) {
				if (out->title) { pfail(&p, line, "a lesson has one # title"); break; }
				out->title = lx_strdup(a, h, (size_t)hl);
				out->title_line = (uint16_t)line;
				continue;
			}
			lf_block_t *b = add_block(&p, LB_HEAD, line);
			if (!b) break;
			b->level = (uint8_t)(lev > 3 ? 3 : lev);
			b->text = lx_strdup(a, h, (size_t)hl);
			continue;
		}

		if (tl >= 4 && !memcmp(t, "$$", 2)) {
			const char *m = t + 2;
			int ml = tl - 2;
			trim(&m, &ml);
			if (ml >= 2 && !memcmp(m + ml - 2, "$$", 2)) {
				flush_para(&p);
				ml -= 2;
				trim(&m, &ml);
				lf_block_t *b = add_block(&p, LB_MATH, line);
				if (!b) break;
				b->text = lx_strdup(a, m, (size_t)ml);
				continue;
			}
		}

		if (tl > 4 && t[0] == '!' && t[1] == '[') {
			const char *close = memchr(t, ']', (size_t)tl);
			if (close && close + 1 < t + tl && close[1] == '(') {
				const char *pe = memchr(close, ')', (size_t)(t + tl - close));
				if (pe) {
					flush_para(&p);
					lf_block_t *b = add_block(&p, LB_IMAGE, line);
					if (!b) break;
					b->text = lx_strdup(a, t + 2, (size_t)(close - t - 2));
					b->src = lx_strdup(a, close + 2, (size_t)(pe - close - 2));
					continue;
				}
			}
		}

		if (t[0] == '|') {
			para_add(&p, LB_TABLE, 0, 0, line, t, tl, '\n');
			continue;
		}

		if (t[0] == '>') {
			const char *q = t + 1;
			int ql = tl - 1;
			trim(&q, &ql);
			para_add(&p, LB_QUOTE, 0, 0, line, q, ql, ' ');
			continue;
		}

		int num, ml = list_marker(s, n, &num);
		if (ml) {
			para_add(&p, LB_ITEM, 1 + i / 2, num, line, s + ml, n - ml, ' ');
			continue;
		}

		// a line continuing an item or a quote, or starting a paragraph
		int kind = p.para_len && (p.para_kind == LB_ITEM || p.para_kind == LB_QUOTE) ?
			p.para_kind : LB_PARA;
		if (p.para_len && p.para_kind == LB_TABLE) flush_para(&p);
		const char *c = t;
		int cl = tl;
		trim(&c, &cl);
		if (p.para_len && kind == p.para_kind) {
			if (p.para_len + cl + 2 > p.para_cap) { pfail(&p, line, "a paragraph is too long"); break; }
			p.para[p.para_len++] = ' ';
			memcpy(p.para + p.para_len, c, (size_t)cl);
			p.para_len += cl;
		} else para_add(&p, LB_PARA, 0, 0, line, c, cl, ' ');
	}
	flush_para(&p);
	// a trailing --- leaves an empty step: drop it
	if (out->nsteps > 1 && out->steps[out->nsteps - 1].count == 0) out->nsteps--;
	if (a->oom && !p.failed) pfail(&p, L.line, "out of memory");
	return !p.failed;
}

// -- inline --

static int find(const char *s, int from, int len, const char *m) {
	int ml = (int)strlen(m);
	for (int i = from; i + ml <= len; i++) {
		if (s[i] == '\\') { i++; continue; }
		if (!memcmp(s + i, m, (size_t)ml)) return i;
	}
	return -1;
}

int lf_inline(lx_arena_t *a, const char *s, int len, lf_span_t *out, int max) {
	int n = 0, i = 0, run = 0;
	bool bold = false, ital = false;
#define EMIT(K, P, L) do { if (n < max && (L) > 0) { lf_span_t *sp = &out[n++]; \
	memset(sp, 0, sizeof *sp); sp->kind = (K); sp->s = (P); sp->len = (L); \
	sp->bold = bold; sp->italic = ital; } } while (0)
	while (i < len) {
		char c = s[i];
		if (c == '\\' && i + 1 < len) {	// \* \$ \` : the character itself
			EMIT(LS_TEXT, s + run, i - run);
			run = i + 1;
			i += 2;
			continue;
		}
		if (c == '`') {
			int e = find(s, i + 1, len, "`");
			if (e > i) {
				EMIT(LS_TEXT, s + run, i - run);
				EMIT(LS_CODE, s + i + 1, e - i - 1);
				i = run = e + 1;
				continue;
			}
		}
		if (c == '$' && !(i + 1 < len && s[i + 1] == ' ')) {
			int e = find(s, i + 1, len, "$");
			if (e > i + 1) {
				EMIT(LS_TEXT, s + run, i - run);
				EMIT(LS_MATH, s + i + 1, e - i - 1);
				i = run = e + 1;
				continue;
			}
		}
		if (c == '[') {
			int e = find(s, i + 1, len, "](");
			int pe = e >= 0 ? find(s, e + 2, len, ")") : -1;
			if (e > i && pe > e) {
				EMIT(LS_TEXT, s + run, i - run);
				if (n < max) {
					lf_span_t *sp = &out[n++];
					memset(sp, 0, sizeof *sp);
					sp->kind = LS_LINK;
					sp->s = s + i + 1;
					sp->len = e - i - 1;
					sp->target = lx_strdup(a, s + e + 2, (size_t)(pe - e - 2));
				}
				i = run = pe + 1;
				continue;
			}
		}
		if (c == '*' && i + 1 < len && s[i + 1] == '*') {
			if (bold || find(s, i + 2, len, "**") > i + 2) {
				EMIT(LS_TEXT, s + run, i - run);
				bold = !bold;
				i = run = i + 2;
				continue;
			}
		}
		if ((c == '*' || c == '_') && !(i + 1 < len && s[i + 1] == c)) {
			// _ only at a word's edge, so snake_case and x_1 stay text
			bool edge = c == '*' || (ital ? (i + 1 >= len || s[i + 1] == ' ' || s[i + 1] == '.' ||
				s[i + 1] == ',') : (i == 0 || s[i - 1] == ' ' || s[i - 1] == '('));
			char m[2] = { c, 0 };
			if (edge && (ital || find(s, i + 1, len, m) > i + 1)) {
				EMIT(LS_TEXT, s + run, i - run);
				ital = !ital;
				i = run = i + 1;
				continue;
			}
		}
		i++;
	}
	EMIT(LS_TEXT, s + run, len - run);
#undef EMIT
	return n;
}

// -- course.cfg --

static void cwarn(lf_info_t *c, const char *fmt, const char *arg) {
	if (c->nwarn >= 4) return;
	snprintf(c->warn[c->nwarn++], sizeof c->warn[0], fmt, arg);
}

static void copy(char *dst, int cap, const char *s, int n) {
	if (n >= cap) n = cap - 1;
	memcpy(dst, s, (size_t)n);
	dst[n] = 0;
}

static void cfg_line(lf_info_t *c, const char *k, int kl, const char *v, int vl) {
	char key[32];
	copy(key, sizeof key, k, kl);
#define S(name, field) if (!strcmp(key, name)) { copy(c->field, sizeof c->field, v, vl); return; }
	S("id", id) S("title", title) S("level", level) S("area", area) S("subject", subject)
	S("ages", ages) S("lang", lang) S("license", license) S("summary", summary)
	S("standards", standards)
#undef S
	char num[16];
	copy(num, sizeof num, v, vl);
	if (!strcmp(key, "version")) { c->version = atoi(num); return; }
	if (!strcmp(key, "format")) { c->format = atoi(num); return; }
	if (!strcmp(key, "class")) {
		int k2 = 0;
		for (int i = 0; i < vl && k2 < (int)sizeof c->classes - 1; i++)
			if (v[i] >= 'A' && v[i] <= 'D') c->classes[k2++] = v[i];
		c->classes[k2] = 0;
		return;
	}
	if (!strcmp(key, "parent-opt-in")) { c->parent_opt_in = num[0] == 'y' || num[0] == '1'; return; }
	if (!strcmp(key, "author")) {
		if (c->nauthors < LF_MAX_LIST) copy(c->authors[c->nauthors++], sizeof c->authors[0], v, vl);
		return;
	}
	if (!strcmp(key, "reviewer")) {
		if (c->nreviewers < LF_MAX_LIST) copy(c->reviewers[c->nreviewers++], sizeof c->reviewers[0], v, vl);
		return;
	}
	if (!strcmp(key, "needs")) {
		if (c->nneeds < LF_MAX_LIST) copy(c->needs[c->nneeds++], sizeof c->needs[0], v, vl);
		return;
	}
	cwarn(c, "course.cfg: unknown key \"%s\"", key);
}

static void parse_cfg(lf_info_t *c, const char *s, long len) {
	lines_t L = { s, s + len, 0 };
	const char *ls;
	int ll;
	while (next_line(&L, &ls, &ll)) {
		trim(&ls, &ll);
		if (!ll || ls[0] == '#') continue;
		const char *eq = memchr(ls, '=', (size_t)ll);
		if (!eq) { cwarn(c, "course.cfg: a line without = (%s)", "key = value"); continue; }
		const char *k = ls, *v = eq + 1;
		int kl = (int)(eq - ls), vl = (int)(ls + ll - v);
		trim(&k, &kl);
		trim(&v, &vl);
		cfg_line(c, k, kl, v, vl);
	}
}

// -- names --

void lf_strip_num(const char *name, char *out, int cap) {
	const char *s = name;
	const char *d = s;
	while (*d >= '0' && *d <= '9') d++;
	if (d > s && *d == '-') s = d + 1;
	int n = (int)strlen(s);
	if (n > 3 && !strcmp(s + n - 3, ".md")) n -= 3;
	copy(out, cap, s, n);
}

void lf_title_of(const char *name, char *out, int cap) {
	lf_strip_num(name, out, cap);
	for (char *p = out; *p; p++) if (*p == '-' || *p == '_') *p = ' ';
	if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 32);
}

// -- the tree --

typedef struct {
	char	names[256][LF_NAME];
	bool	dirs[256];
	int		n;
} names_t;

static void collect(void *arg, const char *name, bool is_dir) {
	names_t *ns = (names_t *)arg;
	if (ns->n >= 256 || name[0] == '.' || strlen(name) >= LF_NAME) return;
	strcpy(ns->names[ns->n], name);
	ns->dirs[ns->n] = is_dir;
	ns->n++;
}

static int cmp_names(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static void sort_names(names_t *ns) {
	// sort names and their flags together: dirs first by marking them
	for (int i = 0; i < ns->n; i++) {
		int best = i;
		for (int j = i + 1; j < ns->n; j++) if (cmp_names(ns->names[j], ns->names[best]) < 0) best = j;
		if (best != i) {
			char t[LF_NAME];
			bool td = ns->dirs[i];
			strcpy(t, ns->names[i]);
			strcpy(ns->names[i], ns->names[best]);
			strcpy(ns->names[best], t);
			ns->dirs[i] = ns->dirs[best];
			ns->dirs[best] = td;
		}
	}
}

static bool numbered(const char *name) {
	return name[0] >= '0' && name[0] <= '9';
}

// the # title of dir/index.md, if there is one
static void folder_title(const lf_fs_t *fs, const char *dir, char *out, int cap,
	const char *fallback) {
	char path[LF_PATH + LF_NAME];
	char buf[512];
	snprintf(path, sizeof path, "%s/index.md", dir);
	long n = fs->read(fs->ctx, path, buf, sizeof buf - 1);
	if (n > 0) {
		buf[n] = 0;
		char *h = strstr(buf, "# ");
		if (h && (h == buf || h[-1] == '\n')) {
			h += 2;
			char *e = strchr(h, '\n');
			int len = e ? (int)(e - h) : (int)strlen(h);
			while (len && (h[len - 1] == '\r' || h[len - 1] == ' ')) len--;
			copy(out, cap, h, len);
			return;
		}
	}
	lf_title_of(fallback, out, cap);
}

static bool load_unit(const lf_fs_t *fs, lf_course_t *c, const char *dir, const char *name,
	int part) {
	if (c->nunits >= LF_MAX_UNITS) { cwarn(&c->m, "too many units (at %s)", name); return false; }
	lf_unit_t *u = &c->units[c->nunits];
	memset(u, 0, sizeof *u);
	copy(u->dir, sizeof u->dir, name, (int)strlen(name));
	lf_strip_num(name, u->id, sizeof u->id);
	u->part = part;
	folder_title(fs, dir, u->title, sizeof u->title, name);
	u->first_lesson = c->nlessons;
	static names_t ns;
	ns.n = 0;
	if (!fs->list(fs->ctx, dir, collect, &ns)) { cwarn(&c->m, "cannot read %s", dir); return false; }
	sort_names(&ns);
	for (int i = 0; i < ns.n; i++) {
		const char *f = ns.names[i];
		int fl = (int)strlen(f);
		if (ns.dirs[i] || fl < 4 || strcmp(f + fl - 3, ".md") || !numbered(f)) continue;
		if (c->nlessons >= LF_MAX_LESSONS) { cwarn(&c->m, "too many lessons (at %s)", f); break; }
		lf_lref_t *r = &c->lessons[c->nlessons++];
		memset(r, 0, sizeof *r);
		copy(r->file, sizeof r->file, f, fl);
		lf_strip_num(f, r->id, sizeof r->id);
		lf_title_of(f, r->title, sizeof r->title);
		r->unit = c->nunits;
		u->nlessons++;
	}
	if (!u->nlessons) { cwarn(&c->m, "a unit with no lessons: %s", name); return true; }
	c->nunits++;
	return true;
}

// does dir hold numbered lesson files (a unit) rather than numbered folders (a part)?
static bool holds_lessons(const lf_fs_t *fs, const char *dir) {
	static names_t ns;
	ns.n = 0;
	if (!fs->list(fs->ctx, dir, collect, &ns)) return false;
	for (int i = 0; i < ns.n; i++) {
		int fl = (int)strlen(ns.names[i]);
		if (!ns.dirs[i] && fl > 3 && !strcmp(ns.names[i] + fl - 3, ".md") && numbered(ns.names[i]))
			return true;
	}
	return false;
}

bool lf_info_load(const lf_fs_t *fs, const char *path, lf_info_t *m, char *err, int errcap) {
	memset(m, 0, sizeof *m);
	copy(m->path, sizeof m->path, path, (int)strlen(path));
	char p[LF_PATH + 16];
	static char cfg[4096];
	snprintf(p, sizeof p, "%s/course.cfg", path);
	long n = fs->read(fs->ctx, p, cfg, sizeof cfg - 1);
	if (n < 0) {
		if (err) snprintf(err, (size_t)errcap, "%s: no course.cfg", path);
		return false;
	}
	cfg[n] = 0;
	parse_cfg(m, cfg, n);
	if (!m->id[0]) {
		if (err) snprintf(err, (size_t)errcap, "%s/course.cfg: no id", path);
		return false;
	}
	return true;
}

bool lf_course_load(const lf_fs_t *fs, const char *path, lf_course_t *c,
	char *err, int errcap) {
	memset(c, 0, sizeof *c);
	if (!lf_info_load(fs, path, &c->m, err, errcap)) return false;
	char p[LF_PATH + LF_NAME * 3];

	static names_t top;
	top.n = 0;
	if (!fs->list(fs->ctx, path, collect, &top)) {
		if (err) snprintf(err, (size_t)errcap, "%s: cannot be read", path);
		return false;
	}
	sort_names(&top);
	// a course's numbered folders are all parts or all units
	static names_t mine;
	mine = top;
	for (int i = 0; i < mine.n; i++) {
		if (!mine.dirs[i] || !numbered(mine.names[i])) continue;
		snprintf(p, sizeof p, "%s/%s", path, mine.names[i]);
		if (holds_lessons(fs, p)) {
			load_unit(fs, c, p, mine.names[i], -1);
			continue;
		}
		if (c->nparts >= LF_MAX_PARTS) { cwarn(&c->m, "too many parts (at %s)", mine.names[i]); break; }
		lf_part_t *pt = &c->parts[c->nparts];
		memset(pt, 0, sizeof *pt);
		copy(pt->dir, sizeof pt->dir, mine.names[i], (int)strlen(mine.names[i]));
		lf_strip_num(mine.names[i], pt->id, sizeof pt->id);
		folder_title(fs, p, pt->title, sizeof pt->title, mine.names[i]);
		pt->first_unit = c->nunits;
		static names_t sub;
		sub.n = 0;
		fs->list(fs->ctx, p, collect, &sub);
		sort_names(&sub);
		for (int j = 0; j < sub.n; j++) {
			if (!sub.dirs[j] || !numbered(sub.names[j])) continue;
			char up[LF_PATH + LF_NAME * 4];
			snprintf(up, sizeof up, "%s/%s", p, sub.names[j]);
			load_unit(fs, c, up, sub.names[j], c->nparts);
		}
		pt->nunits = c->nunits - pt->first_unit;
		if (pt->nunits) c->nparts++;
		else cwarn(&c->m, "a part with no units: %s", mine.names[i]);
	}
	if (c->nparts && c->nunits) {
		for (int i = 0; i < c->nunits; i++)
			if (c->units[i].part < 0) {
				char d[LF_NAME];
				strcpy(d, c->units[i].dir);
				cwarn(&c->m, "units and parts mixed at the top: %s", d);
				break;
			}
	}
	return true;
}

void lf_lesson_id(const lf_course_t *c, int lesson, char *out, int cap) {
	const lf_lref_t *r = &c->lessons[lesson];
	const lf_unit_t *u = &c->units[r->unit];
	if (u->part >= 0)
		snprintf(out, (size_t)cap, "%s/%s/%s/%s", c->m.id, c->parts[u->part].id, u->id, r->id);
	else
		snprintf(out, (size_t)cap, "%s/%s/%s", c->m.id, u->id, r->id);
}

void lf_lesson_path(const lf_course_t *c, int lesson, char *out, int cap) {
	const lf_lref_t *r = &c->lessons[lesson];
	const lf_unit_t *u = &c->units[r->unit];
	if (u->part >= 0)
		snprintf(out, (size_t)cap, "%s/%s/%s/%s", c->m.path, c->parts[u->part].dir, u->dir, r->file);
	else
		snprintf(out, (size_t)cap, "%s/%s/%s", c->m.path, u->dir, r->file);
}
