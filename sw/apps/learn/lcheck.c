/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- checking courses. lcheck.h, docs/learn_format.md ("Checks").
 */

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

#include "lcheck.h"
#include "lmath.h"
#include "../../common/zutf8.h"

// -- reporting --

typedef struct {
	lc_ctx_t		*c;
	const char		*file;
	const lf_course_t *course;
	bool			japanese;
	lx_arena_t		*a;
} cx_t;

static void say(cx_t *x, int level, int line, const char *fmt, ...) {
	char msg[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	if (level == LC_ERROR) x->c->errors++;
	else if (level == LC_WARN) x->c->warnings++;
	if (x->c->report) x->c->report(x->c, level, x->file, line, msg);
}

#define ERR(line, ...)  say(x, LC_ERROR, (line), __VA_ARGS__)
#define WARN(line, ...) say(x, LC_WARN, (line), __VA_ARGS__)

// -- text --

bool lc_drawable(unsigned long cp, bool japanese) {
	if (cp == '\n' || cp == '\t') return true;
	if (cp < 0x20 || cp == 0x7F) return false;
	if (z_cp_to_l9((uint32_t)cp) || z_cp_lookalike((uint32_t)cp)) return true;
	if (japanese) {
		if (cp >= 0x3000 && cp <= 0x30FF) return true;		// punctuation, kana
		if (cp >= 0x4E00 && cp <= 0x9FFF) return true;		// kanji
		if (cp >= 0xFF00 && cp <= 0xFFEF) return true;		// full width
	}
	return false;
}

static void glyphs(cx_t *x, int line, const char *s, int len, const char *where) {
	const char *p = s, *end = s + len;
	while (p < end) {
		uint32_t cp = z_utf8_next(&p, end);
		if (cp == 0xFFFD) { ERR(line, "%s: text that is not UTF-8", where); return; }
		if (!lc_drawable(cp, x->japanese)) {
			if (cp >= 0x370 && cp <= 0x3FF)
				WARN(line, "%s: Greek letter U+%04X in text: the text fonts cannot draw it; write it in a formula, e.g. $pi$",
					where, (unsigned)cp);
			else
				WARN(line, "%s: the fonts cannot draw U+%04X", where, (unsigned)cp);
			return;		// once a block is enough
		}
	}
}

static int syllables(const char *w, int n) {
	int count = 0;
	bool prev = false;
	for (int i = 0; i < n; i++) {
		char c = w[i] | 0x20;
		bool v = c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y';
		if (v && !prev) count++;
		prev = v;
	}
	if (n > 2 && (w[n - 1] | 0x20) == 'e' && (w[n - 2] | 0x20) != 'l' && count > 1) count--;
	return count ? count : 1;
}

double lc_reading_grade(const char *t, int *words_out) {
	int words = 0, sentences = 0, syl = 0;
	const char *p = t;
	while (*p) {
		while (*p && !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) {
			if (*p == '.' || *p == '!' || *p == '?') sentences++;
			p++;
		}
		const char *w = p;
		while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '\'') p++;
		if (p > w) { words++; syl += syllables(w, (int)(p - w)); }
	}
	if (words_out) *words_out = words;
	if (!words) return 0;
	if (!sentences) sentences = 1;
	return 0.39 * words / sentences + 11.8 * syl / words - 15.59;
}

// the grade a course's prose is meant for, or -1 for no expectation
static int target_grade(const char *level) {
	if (!strcmp(level, "K")) return 0;
	if (level[0] >= '1' && level[0] <= '9') return level[1] >= '0' && level[1] <= '9' ? 10 + level[1] - '0' : level[0] - '0';
	if (!strcmp(level, "ms")) return 8;
	if (!strcmp(level, "hs")) return 11;
	if (!strcmp(level, "col")) return 14;
	return -1;
}

// -- the forms of data blocks --

static bool str_arg(const lx_node_t *f, int n) {
	lx_node_t *a = lx_nth(f, n);
	return a && a->kind == LX_STR && a->text[0];
}

// the visible text of an (ask ...) or a label: strings and (math "...")
static void check_math_str(cx_t *x, int line, const char *m, const char *where) {
	char e[96];
	lx_arena_t *a = x->a;
	size_t mark = a->used;
	if (!lm_parse(a, m, (int)strlen(m), e, sizeof e)) ERR(line, "%s (in %s \"%s\")", e, where, m);
	a->used = mark;
}

static void check_text_parts(cx_t *x, const lx_node_t *f, const char *where) {
	for (lx_node_t *e = f->head ? f->head->next : NULL; e; e = e->next) {
		if (e->kind == LX_STR) glyphs(x, e->line, e->text, (int)strlen(e->text), where);
		else if (lx_is_form(e, "math")) {
			if (!str_arg(e, 1)) ERR(e->line, "%s: (math) needs a formula", where);
			else check_math_str(x, e->line, lx_nth(e, 1)->text, where);
		}
	}
}

// (true "3/4 > 2/4") or (false "...") anywhere inside f: evaluate them
static void check_claims(cx_t *x, const lx_node_t *f) {
	for (lx_node_t *e = f->head; e; e = e->next) {
		if (e->kind != LX_LIST) continue;
		bool t = lx_is_form(e, "true"), fl = lx_is_form(e, "false");
		if (t || fl) {
			if (!str_arg(e, 1)) { ERR(e->line, "(%s) needs a formula", t ? "true" : "false"); continue; }
			char err[96];
			size_t mark = x->a->used;
			lm_val_t v = lm_eval_str(x->a, lx_nth(e, 1)->text, err, sizeof err);
			x->a->used = mark;
			if (!v.ok || !v.is_bool)
				ERR(e->line, "the claim \"%s\" cannot be evaluated%s%s", lx_nth(e, 1)->text,
					err[0] ? ": " : "", err);
			else if (v.truth != t)
				ERR(e->line, "the claim \"%s\" is %s, but it is marked (%s)", lx_nth(e, 1)->text,
					v.truth ? "true" : "false", t ? "true" : "false");
		} else check_claims(x, e);
	}
}

static int hints(cx_t *x, const lx_node_t *f) {
	int n = lx_count(f, "hint");
	for (int i = 0; i < n; i++) {
		lx_node_t *h = lx_find_nth(f, "hint", i);
		if (!str_arg(h, 1)) ERR(h->line, "a (hint) with no text");
		else glyphs(x, h->line, lx_nth(h, 1)->text, (int)strlen(lx_nth(h, 1)->text), "hint");
	}
	if (n > 3) WARN(f->line, "%d hints: three is the ladder (a nudge, a bigger one, the worked answer)", n);
	return n;
}

static void need_ask(cx_t *x, const lx_node_t *f, const char *kind) {
	lx_node_t *ask = lx_find(f, "ask");
	if (!ask) { ERR(f->line, "(%s) needs an (ask ...): the question", kind); return; }
	if (lx_len(ask) < 2) ERR(ask->line, "(ask) is empty");
	check_text_parts(x, ask, "question");
}

static void wrong_ifs(cx_t *x, const lx_node_t *f) {
	int n = lx_count(f, "wrong-if");
	for (int i = 0; i < n; i++) {
		lx_node_t *w = lx_find_nth(f, "wrong-if", i);
		lx_node_t *fb = lx_nth(w, 2);
		if (!lx_nth(w, 1)) ERR(w->line, "(wrong-if) needs the wrong answer");
		if (!fb || fb->kind != LX_STR || !fb->text[0])
			ERR(w->line, "(wrong-if) needs its feedback: what to say to someone who answered this");
	}
}

static const char *const check_kinds[] = {
	"choice", "number", "fraction", "text", "order", "match", "point", NULL
};

static bool is_check_kind(const char *n) {
	for (int i = 0; check_kinds[i]; i++) if (!strcmp(n, check_kinds[i])) return true;
	return false;
}

static void check_one(cx_t *x, const lx_node_t *f) {
	const char *k = lx_form_name(f);
	x->c->checks++;
	need_ask(x, f, k);
	hints(x, f);
	check_claims(x, f);
	if (!strcmp(k, "choice")) {
		int right = lx_count(f, "right"), wrong = lx_count(f, "wrong");
		if (!right) ERR(f->line, "(choice) has no (right ...) answer");
		if (right > 1 && !lx_flag(f, "several"))
			ERR(f->line, "(choice) has %d right answers: add (several) if more than one is meant", right);
		if (!wrong) ERR(f->line, "(choice) has no (wrong ...) answers to choose from");
		if (right + wrong > 6) WARN(f->line, "%d answers: more than six is hard to read and to choose from", right + wrong);
		const char *seen[12];
		int ns = 0;
		for (int i = 0; i < right + wrong; i++) {
			bool r = i < right;
			lx_node_t *o = lx_find_nth(f, r ? "right" : "wrong", r ? i : i - right);
			if (!str_arg(o, 1)) { ERR(o->line, "an answer with no text"); continue; }
			const char *t = lx_nth(o, 1)->text;
			glyphs(x, o->line, t, (int)strlen(t), "answer");
			for (int j = 0; j < ns; j++) if (!strcmp(seen[j], t)) ERR(o->line, "the answer \"%s\" is there twice", t);
			if (ns < 12) seen[ns++] = t;
			if (!r) {
				lx_node_t *fb = NULL;
				for (lx_node_t *e = lx_nth(o, 2); e; e = e->next) if (e->kind == LX_STR) { fb = e; break; }
				if (!fb) ERR(o->line, "the wrong answer \"%s\" has no feedback: say why it is wrong", t);
				else glyphs(x, fb->line, fb->text, (int)strlen(fb->text), "feedback");
			}
		}
	} else if (!strcmp(k, "number")) {
		double ans, tol = 0;
		lx_node_t *an = lx_find(f, "answer");
		if (!an || !lx_num_of(f, "answer", &ans)) { ERR(f->line, "(number) needs (answer N)"); return; }
		if (lx_find(f, "within") && (!lx_num_of(f, "within", &tol) || tol < 0))
			ERR(f->line, "(within) needs a number >= 0");
		lx_node_t *from = lx_find(f, "from");
		if (from) {
			if (!str_arg(from, 1)) ERR(from->line, "(from) needs a formula");
			else {
				char err[96];
				size_t mark = x->a->used;
				lm_val_t v = lm_eval_str(x->a, lx_nth(from, 1)->text, err, sizeof err);
				x->a->used = mark;
				if (!v.ok || v.is_bool)
					ERR(from->line, "the formula \"%s\" cannot be computed%s%s", lx_nth(from, 1)->text,
						err[0] ? ": " : "", err);
				else if (fabs(v.v - ans) > tol + 1e-9 * (fabs(ans) > 1 ? fabs(ans) : 1))
					ERR(an->line, "the answer %g is not what \"%s\" gives (%.10g)%s", ans,
						lx_nth(from, 1)->text, v.v, tol ? "" : "; add (within ...) if it is rounded");
			}
		} else if (tol > 0 && fabs(ans) > 0 && tol / fabs(ans) > 0.2)
			WARN(f->line, "(within %g) is a fifth of the answer or more", tol);
		wrong_ifs(x, f);
	} else if (!strcmp(k, "fraction")) {
		lx_node_t *an = lx_find(f, "answer");
		if (!an || lx_len(an) != 3 || lx_nth(an, 1)->kind != LX_NUM || lx_nth(an, 2)->kind != LX_NUM)
			ERR(f->line, "(fraction) needs (answer N D)");
		else if (lx_nth(an, 2)->num == 0) ERR(an->line, "a fraction's answer cannot have 0 below");
		wrong_ifs(x, f);
	} else if (!strcmp(k, "text")) {
		int n = lx_count(f, "accept");
		if (!n) ERR(f->line, "(text) needs at least one (accept \"...\")");
		for (int i = 0; i < n; i++) {
			lx_node_t *ac = lx_find_nth(f, "accept", i);
			if (!str_arg(ac, 1)) ERR(ac->line, "(accept) with no text");
		}
		const char *acc = lx_str_of(f, "accents");
		if (acc && strcmp(acc, "strict") && strcmp(acc, "nearly"))
			ERR(f->line, "(accents) is strict or nearly, not %s", acc);
		wrong_ifs(x, f);
	} else if (!strcmp(k, "order")) {
		lx_node_t *it = lx_find(f, "items");
		if (!it || lx_len(it) < 3) ERR(f->line, "(order) needs (items \"first\" \"second\" ...), two or more");
	} else if (!strcmp(k, "match")) {
		int n = lx_count(f, "pair");
		if (n < 2) ERR(f->line, "(match) needs two or more (pair \"left\" \"right\")");
		for (int i = 0; i < n; i++) {
			lx_node_t *pr = lx_find_nth(f, "pair", i);
			if (!str_arg(pr, 1) || !str_arg(pr, 2)) ERR(pr->line, "(pair) needs two texts");
		}
	} else if (!strcmp(k, "point")) {
		lx_node_t *t = lx_find(f, "target");
		if (!t || lx_len(t) < 4) ERR(f->line, "(point) needs (target X Y R) on the figure");
	}
}

static const char *const try_apps[] = {
	"basic", "repl", "term", "calc", "sheet", "text", "read", "draw", "pix", "slide",
	"bench", "logic", "files", "chess", "zcc", "hex", "view", "web", "keyboard", "settings",
	"cal", "clock", "midi", "track", "play", NULL
};

static void check_block(cx_t *x, const lf_block_t *b, int *steps_with_check) {
	const char *n = b->name;
	for (lx_node_t *f = b->forms->head; f; f = f->next) {
		const char *k = lx_form_name(f);
		if (!k) { ERR(f->line, "in the %s block: a form like (name ...) was expected", n); continue; }
		if (!strcmp(n, "check")) {
			if (!is_check_kind(k)) { ERR(f->line, "(%s) is not a kind of check (choice, number, fraction, text, order, match, point)", k); continue; }
			check_one(x, f);
			(*steps_with_check)++;
		} else if (!strcmp(n, "lesson")) {
			static const char *const ok[] = { "objective", "standards", "minutes", "needs", "level", "keywords", NULL };
			bool known = false;
			for (int i = 0; ok[i]; i++) if (!strcmp(k, ok[i])) known = true;
			if (!known) WARN(f->line, "(%s) is not one of the lesson block's forms", k);
		} else if (!strcmp(n, "example")) {
			if (strcmp(k, "example")) { ERR(f->line, "an example block holds (example ...), not (%s)", k); continue; }
			if (!lx_find(f, "problem")) ERR(f->line, "(example) needs a (problem \"...\")");
			int st = lx_count(f, "step");
			if (!st) ERR(f->line, "(example) needs its (step ...)s: the solution, worked");
			for (int i = 0; i < st; i++) {
				lx_node_t *s = lx_find_nth(f, "step", i);
				lx_node_t *in = lx_nth(s, 1);
				if (in && in->kind == LX_LIST && is_check_kind(lx_form_name(in) ? lx_form_name(in) : ""))
					check_one(x, in);
				else if (!str_arg(s, 1)) ERR(s->line, "a (step) needs text, or a check to answer");
			}
			check_claims(x, f);
		} else if (!strcmp(n, "cards")) {
			if (strcmp(k, "card")) { ERR(f->line, "a cards block holds (card \"front\" \"back\"), not (%s)", k); continue; }
			x->c->cards++;
			if (!str_arg(f, 1) || !str_arg(f, 2)) ERR(f->line, "(card) needs a front and a back");
			else {
				glyphs(x, f->line, lx_nth(f, 1)->text, (int)strlen(lx_nth(f, 1)->text), "card");
				glyphs(x, f->line, lx_nth(f, 2)->text, (int)strlen(lx_nth(f, 2)->text), "card");
			}
		} else if (!strcmp(n, "try")) {
			if (strcmp(k, "try")) { ERR(f->line, "a try block holds (try ...), not (%s)", k); continue; }
			const char *app = lx_str_of(f, "app");
			bool known = false;
			for (int i = 0; app && try_apps[i]; i++) if (!strcmp(app, try_apps[i])) known = true;
			if (!app) ERR(f->line, "(try) needs (app \"name\")");
			else if (!known) ERR(f->line, "(try): \"%s\" is not an app a lesson can open", app);
			if (!lx_str_of(f, "task")) ERR(f->line, "(try) needs (task \"what to do\")");
			lx_node_t *ch = lx_find(f, "check");
			if (ch) {
				lx_node_t *in = lx_nth(ch, 1);
				if (in && in->kind == LX_LIST && lx_form_name(in) && is_check_kind(lx_form_name(in))) check_one(x, in);
				else ERR(ch->line, "(check ...) inside a try holds a check, e.g. (check (number ...))");
			}
		} else if (!strcmp(n, "say")) {
			if (strcmp(k, "say") || !str_arg(f, 1)) ERR(f->line, "a say block holds (say \"words\")");
		} else if (!strcmp(n, "note") || !strcmp(n, "warn")) {
			if (strcmp(k, n)) { ERR(f->line, "a %s block holds (%s ...), not (%s)", n, n, k); continue; }
			bool warn = !strcmp(n, "warn");
			if (warn ? !(str_arg(f, 1) && str_arg(f, 2)) : !str_arg(f, 1))
				ERR(f->line, warn ? "(warn \"title\" \"text\")" : "(note \"text\")");
			else check_text_parts(x, f, n);
		} else if (!strcmp(n, "sources")) {
			if (strcmp(k, "source")) { ERR(f->line, "a sources block holds (source \"...\"), not (%s)", k); continue; }
			if (!str_arg(f, 1)) ERR(f->line, "(source) needs the source: title, author or publisher, year, part");
		}
	}
}

// -- a figure: Scheme, checked here for its brackets and strings --

static void check_figure(cx_t *x, const lf_block_t *b) {
	int depth = 0, line = b->line + 1;
	bool str = false;
	for (const char *p = b->text; *p; p++) {
		if (*p == '\n') line++;
		if (str) {
			if (*p == '\\' && p[1]) p++;
			else if (*p == '"') str = false;
			continue;
		}
		if (*p == ';') { while (*p && *p != '\n') p++; if (!*p) break; line++; continue; }
		if (*p == '"') str = true;
		else if (*p == '(') depth++;
		else if (*p == ')' && --depth < 0) { ERR(line, "figure: a ) without a ("); return; }
	}
	if (str) ERR(b->line, "figure: a string is not closed");
	else if (depth) ERR(b->line, "figure: %d ( not closed", depth);
	x->c->figures++;
}

// -- links and pictures --

static bool lesson_exists(cx_t *x, const char *target) {
	const lf_course_t *c = x->course;
	char id[200], want[200];
	snprintf(want, sizeof want, "%s/%s", c->m.id, target);
	for (int i = 0; i < c->nlessons; i++) {
		lf_lesson_id(c, i, id, sizeof id);
		if (!strcmp(id, want) || !strcmp(id, target)) return true;
	}
	return false;
}

static bool course_known(cx_t *x, const char *id, int len) {
	if ((int)strlen(x->course->m.id) == len && !strncmp(x->course->m.id, id, (size_t)len)) return true;
	for (int i = 0; i < x->c->nknown; i++)
		if ((int)strlen(x->c->known_courses[i]) == len && !strncmp(x->c->known_courses[i], id, (size_t)len)) return true;
	return false;
}

static void check_link(cx_t *x, int line, const char *t) {
	if (!strncmp(t, "lesson:", 7)) {
		const char *l = t + 7;
		if (lesson_exists(x, l)) return;
		const char *sl = strchr(l, '/');
		if (sl && course_known(x, l, (int)(sl - l)) && strncmp(l, x->course->m.id, (size_t)(sl - l))) {
			if (!x->c->nknown) WARN(line, "the link %s goes to another course: check with all courses", t);
			return;	// another course's lesson: checked when that course is
		}
		ERR(line, "the link %s goes to no lesson in this course", t);
	} else if (!strncmp(t, "course:", 7)) {
		if (!course_known(x, t + 7, (int)strlen(t + 7)))
			say(x, x->c->nknown ? LC_ERROR : LC_WARN, line, "the link %s goes to a course %s", t,
				x->c->nknown ? "that does not exist" : "not checked here");
	} else if (!strncmp(t, "docs:", 5)) {
		// a page of the Zeitlos docs: /docs/<name>.md on the card
	} else if (!strncmp(t, "http://", 7) || !strncmp(t, "https://", 8) || !strncmp(t, "gemini://", 9)) {
		WARN(line, "the link %s needs the network: a lesson should work without it", t);
	} else ERR(line, "the link %s: a link starts lesson:, course:, docs: or http", t);
}

static void check_image(cx_t *x, int line, const char *src, const char *media) {
	char p[LF_PATH + 128];
	char dummy[4];
	if (strstr(src, "..") || src[0] == '/') { ERR(line, "a picture's path is inside the course: %s", src); return; }
	snprintf(p, sizeof p, "%s/%s", x->course->m.path, src);
	if (x->c->fs->read(x->c->fs->ctx, p, dummy, sizeof dummy) < 0) ERR(line, "the picture %s is not in the course", src);
	// its source and licence, from media.cfg
	const char *m = media ? strstr(media, src) : NULL;
	while (m && !((m == media || m[-1] == '\n') && (m[strlen(src)] == ' ' || m[strlen(src)] == '=')))
		m = strstr(m + 1, src);
	if (!m) ERR(line, "the picture %s has no line in media.cfg (its source and licence)", src);
}

// -- a lesson --

static void check_inline(cx_t *x, const lf_block_t *b) {
	lf_span_t sp[64];
	size_t mark = x->a->used;
	int n = lf_inline(x->a, b->text, (int)strlen(b->text), sp, 64);
	for (int i = 0; i < n; i++) {
		if (sp[i].kind == LS_MATH) {
			char m[256];
			int l = sp[i].len < 255 ? sp[i].len : 255;
			memcpy(m, sp[i].s, (size_t)l);
			m[l] = 0;
			check_math_str(x, b->line, m, "formula");
		} else if (sp[i].kind == LS_LINK && sp[i].target) check_link(x, b->line, sp[i].target);
	}
	x->a->used = mark;
	glyphs(x, b->line, b->text, (int)strlen(b->text), "text");
}

static void check_lesson(cx_t *x, const char *path, const char *media, const char *level) {
	lc_ctx_t *c = x->c;
	static char buf[LF_MAX_LESSON + 2];
	long n = c->fs->read(c->fs->ctx, path, buf, LF_MAX_LESSON + 1);
	x->file = path;
	if (n < 0) { ERR(0, "cannot be read"); return; }
	if (n > LF_MAX_LESSON) { ERR(0, "longer than %d bytes: split the lesson", LF_MAX_LESSON); return; }
	buf[n] = 0;
	if (!z_utf8_valid(buf, (size_t)n)) { ERR(0, "not UTF-8"); return; }

	lx_arena_t a;
	lx_arena_init(&a, c->mem, (size_t)c->memsize);
	x->a = &a;
	static lf_lesson_t les;
	char err[160];
	if (!lf_lesson_parse(&a, buf, n, &les, err, sizeof err)) {
		int line = 0;
		sscanf(err, "line %d", &line);
		const char *why = strchr(err, ':');
		ERR(line, "%s", why ? why + 2 : err);
		return;
	}
	c->lessons++;
	c->steps += les.nsteps;
	if (!les.title) ERR(1, "a lesson starts with its # title");
	else glyphs(x, les.title_line, les.title, (int)strlen(les.title), "title");
	if (!les.meta) WARN(1, "no lesson block: (objective \"...\") says what the lesson teaches");
	else {
		if (!lx_str_of(les.meta, "objective")) ERR(1, "the lesson block needs (objective \"...\")");
		double m;
		if (lx_num_of(les.meta, "minutes", &m)) {
			if (m < 5 || m > 45) WARN(1, "(minutes %g): a lesson is 5 to 45 minutes", m);
			c->minutes += (int)m;
		}
	}

	int checks = 0, since_check = 0, prose_total = 0;
	static char prose[LF_MAX_LESSON];
	int pl = 0;
	for (int s = 0; s < les.nsteps; s++) {
		int words = 0, step_checks = 0;
		bool teaches = false;
		const lf_step_t *st = &les.steps[s];
		static char fig[16384];
		int fl = 0, fig_line = 0;
		for (int i = st->first; i < st->first + st->count; i++) {
			const lf_block_t *b = &les.blocks[i];
			switch (b->kind) {
			case LB_PARA: case LB_ITEM: case LB_QUOTE: case LB_HEAD: {
				int w = 0;
				lc_reading_grade(b->text, &w);
				words += w;
				teaches = true;
				check_inline(x, b);
				int l = (int)strlen(b->text);
				if (pl + l + 2 < (int)sizeof prose) {
					memcpy(prose + pl, b->text, (size_t)l);
					pl += l;
					prose[pl++] = b->kind == LB_HEAD || b->kind == LB_ITEM ? '.' : ' ';
					prose[pl] = 0;
				}
				break;
			}
			case LB_TABLE: check_inline(x, b); teaches = true; break;
			case LB_MATH: check_math_str(x, b->line, b->text, "formula"); break;
			case LB_IMAGE: check_image(x, b->line, b->src, media); break;
			case LB_FIGURE: {
				int e0 = c->errors;
				check_figure(x, b);
				teaches = true;
				int l = (int)strlen(b->text);
				if (c->errors == e0 && fl + l + 2 < (int)sizeof fig) {
					if (!fig_line) fig_line = b->line;
					memcpy(fig + fl, b->text, (size_t)l);
					fl += l;
					fig[fl++] = '\n';
				}
				break;
			}
			case LB_CODE: glyphs(x, b->line, b->text, (int)strlen(b->text), "code"); break;
			case LB_DATA: {
				int before = c->checks;
				check_block(x, b, &step_checks);
				(void)before;
				if (!strcmp(b->name, "example") || !strcmp(b->name, "warn") || !strcmp(b->name, "note"))
					teaches = true;
				break;
			}
			}
		}
		if (fl && c->run_figure) {
			fig[fl] = 0;
			c->errors += c->run_figure(c, x->course->m.path, fig, x->file, fig_line);
		}
		prose_total += words;
		if (words > 250) ERR(les.blocks[st->first].line, "step %d has %d words: one idea a step, 150 at most", s + 1, words);
		else if (words > 150) WARN(les.blocks[st->first].line, "step %d has %d words: one idea a step, 150 at most", s + 1, words);
		checks += step_checks;
		if (step_checks) since_check = 0;
		else if (teaches && ++since_check >= 3)
			WARN(les.blocks[st->first].line, "three teaching steps without a check: ask something after each step or two");
	}
	c->words += prose_total;
	if (!checks && les.nsteps > 0)
		WARN(1, "no check: a lesson asks the learner something, at least at its end");

	int tg = target_grade(level);
	int words = 0;
	double g = lc_reading_grade(prose, &words);
	if (tg >= 0 && words >= 80 && g > tg + 3)
		WARN(0, "reads at about grade %.0f for a grade %d course: shorter sentences, simpler words", g, tg);
}

// -- a course --

static bool valid_id(const char *s) {
	if (!s[0]) return false;
	for (const char *p = s; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) return false;
	return true;
}

static const char *const levels[] = {
	"K", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "ms", "hs", "col", "life", "zeit", "lang", NULL
};
static const char *const areas[] = { "school", "college", "life", "lang", "zeit", NULL };

static bool in(const char *v, const char *const *set) {
	for (int i = 0; set[i]; i++) if (!strcmp(v, set[i])) return true;
	return false;
}

int lc_check_course(lc_ctx_t *c, const char *path) {
	static lf_course_t course;
	cx_t xx = { c, path, &course, false, NULL }, *x = &xx;
	char err[200], cfgpath[LF_PATH + 16];
	int before = c->errors;
	snprintf(cfgpath, sizeof cfgpath, "%s/course.cfg", path);
	x->file = cfgpath;
	if (!lf_course_load(c->fs, path, &course, err, sizeof err)) { ERR(0, "%s", err); return c->errors - before; }
	x->japanese = !strcmp(course.m.lang, "ja") || !strncmp(course.m.id, "lang-ja", 7);

	for (int i = 0; i < course.m.nwarn; i++) WARN(0, "%s", course.m.warn[i]);
	if (!valid_id(course.m.id)) ERR(0, "id \"%s\": lower case letters, digits and -", course.m.id);
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	if (strcmp(base, course.m.id)) ERR(0, "id \"%s\" is not the folder's name \"%s\"", course.m.id, base);
	if (!course.m.title[0]) ERR(0, "no title");
	if (!course.m.level[0]) ERR(0, "no level (K, 1-12, ms, hs, col, life, zeit, lang)");
	else if (!in(course.m.level, levels)) ERR(0, "level \"%s\" is not one of K, 1-12, ms, hs, col, life, zeit, lang", course.m.level);
	if (!course.m.area[0]) ERR(0, "no area (school, college, life, lang, zeit)");
	else if (!in(course.m.area, areas)) ERR(0, "area \"%s\" is not one of school, college, life, lang, zeit", course.m.area);
	if (!course.m.classes[0]) ERR(0, "no class (A, B, C, D: docs/learn_format.md, \"Review classes\")");
	if (course.m.version < 1) ERR(0, "no version (1, then one more for each release)");
	if (!course.m.format) WARN(0, "no format = %d", LF_FORMAT_VERSION);
	else if (course.m.format > LF_FORMAT_VERSION) ERR(0, "format %d is newer than this checker (%d)", course.m.format, LF_FORMAT_VERSION);
	if (!course.m.nauthors) ERR(0, "no author: every course names its authors, people and models");
	if (strcmp(course.m.license, "CC0-1.0")) WARN(0, "license \"%s\": the edu pack is CC0-1.0", course.m.license);
	if (!course.m.lang[0]) WARN(0, "no lang (en)");
	if (!course.m.summary[0]) WARN(0, "no summary: one line for the catalogue");
	for (int i = 0; i < course.m.nneeds; i++) {
		if (!valid_id(course.m.needs[i])) ERR(0, "needs \"%s\" is not a course id", course.m.needs[i]);
		else if (c->nknown && !course_known(x, course.m.needs[i], (int)strlen(course.m.needs[i])))
			ERR(0, "needs %s: there is no such course", course.m.needs[i]);
	}
	// reviews: "name; qualification; yyyy-mm-dd"
	int qualified = 0;
	for (int i = 0; i < course.m.nreviewers; i++) {
		const char *r = course.m.reviewers[i];
		const char *s1 = strchr(r, ';'), *s2 = s1 ? strchr(s1 + 1, ';') : NULL;
		if (!s1 || !s2) ERR(0, "reviewer \"%s\": write name; qualification; date", r);
		else {
			const char *q = s1 + 1;
			while (*q == ' ') q++;
			if (q < s2 && *q != ';') qualified++;
		}
	}
	bool B = strchr(course.m.classes, 'B') != NULL, D = strchr(course.m.classes, 'D') != NULL;
	if (c->release) {
		if (!course.m.nreviewers) ERR(0, "release: not reviewed (no reviewer line)");
		if (B && !qualified) ERR(0, "release: class B needs a qualified reviewer (reviewer = name; qualification; date)");
		if (D && course.m.nreviewers < 2) ERR(0, "release: class D needs more than one reviewer");
	} else if (B && !course.m.nreviewers)
		say(x, LC_NOTE, 0, "class B: not to be released before a qualified review");
	if (!course.nlessons) ERR(0, "no lessons (numbered folders of numbered .md files)");

	// pictures' sources and licences
	static char media[16384];
	char mp[LF_PATH + 16];
	snprintf(mp, sizeof mp, "%s/media.cfg", path);
	long mn = c->fs->read(c->fs->ctx, mp, media, sizeof media - 1);
	const char *mptr = NULL;
	if (mn >= 0) { media[mn] = 0; mptr = media; }

	for (int i = 0; i < course.nlessons; i++) {
		char lp[LF_PATH + 200];
		lf_lesson_path(&course, i, lp, sizeof lp);
		check_lesson(x, lp, mptr, course.m.level);
	}
	return c->errors - before;
}
