/*
 * slide -- a deck as a print job. See printjob.h.
 */

#include "printjob.h"
#include "render.h"

#include <string.h>

bool printjob_pages(const deck_t *d, int first, int count, zp_writer_t *w) {
	int i;
	if (first < 0) first = 0;
	if (count < 0 || first + count > d->ncard) count = d->ncard - first;
	for (i = first; i < first + count; i++) {
		zc_xf_t xf;
		zc_target_t *t = zp_page_begin(w, &d->page, &xf);
		render_card(t, &xf, d, d->card[i], RENDER_PRINT);
		zp_page_end(w);
		if (w->failed) return false;
	}
	return !w->failed;
}

static int num(const char *s, const char **end) {
	int v = 0;
	while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); if (v > 99999) break; }
	*end = s;
	return v;
}

static void add(char *keep, int n, const char *w) {
	int l = (int)strlen(keep), i;
	if (l && l < n - 1) keep[l++] = ' ';
	for (i = 0; w[i] && l < n - 1; i++) keep[l++] = w[i];
	keep[l] = 0;
}

static void say(char *e, int n, const char *a, const char *b) {
	int k = 0, i;
	for (i = 0; a[i] && k < n - 1; i++) e[k++] = a[i];
	for (i = 0; b && b[i] && k < n - 1; i++) e[k++] = b[i];
	e[k] = 0;
}

// One pass over a string of words; `one_off` words allowed (a /print
// line) or not (the deck's own).
static bool words(pj_settings_t *s, const char *str, bool one_off, int cur, int ncard,
	char *keep, int keep_n, char *err, int err_n) {
	char w[24], v[24];
	const char *p = str;
	while (*p) {
		int n = 0;
		while (*p == ' ' || *p == '/') p++;
		if (!*p) break;
		while (*p && *p != ' ' && n < 23) { char c = *p++; w[n++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c); }
		w[n] = 0;
		while (*p && *p != ' ') p++;
		if (!strcmp(w, "print") || !strcmp(w, "to") || !strcmp(w, "printer")) continue;
		if (!strcmp(w, "pdf") && one_off) { s->dest = PJ_PDF; continue; }
		if ((!strcmp(w, "ps") || !strcmp(w, "postscript")) && one_off) { s->dest = PJ_PS; continue; }
		if (!strcmp(w, "this") && one_off) { s->first = cur; s->count = 1; continue; }
		if (!strcmp(w, "setup") && one_off) { s->dest = PJ_NONE; continue; }
		if (!strcmp(w, "all") && one_off) { s->first = 0; s->count = -1; continue; }
		if (w[0] >= '0' && w[0] <= '9') {
			const char *e;
			int a = num(w, &e);
			if (!strcmp(e, "up")) {
				if (a < 1 || a > 16) { say(err, err_n, "1up to 16up", NULL); return false; }
				s->nup = a;
				if (keep) add(keep, keep_n, w);
				continue;
			}
			if (one_off && (*e == 0 || *e == '-')) {
				int b = *e == '-' ? num(e + 1, &e) : a;
				if (a < 1 || b < a || a > ncard) { say(err, err_n, "no such cards: ", w); return false; }
				if (b > ncard) b = ncard;
				s->first = a - 1;
				s->count = b - a + 1;
				continue;
			}
			say(err, err_n, "print: what is ", w);
			return false;
		}
		if (!strcmp(w, "cut")) { s->cut = true; if (keep) add(keep, keep_n, w); continue; }
		if (!strcmp(w, "nocut")) { s->cut = false; continue; }
		if (!strcmp(w, "frame")) { s->frame = true; if (keep) add(keep, keep_n, w); continue; }
		if (!strcmp(w, "noframe")) { s->frame = false; continue; }
		if (!strcmp(w, "actual")) { s->actual = true; if (keep) add(keep, keep_n, w); continue; }
		if (!strcmp(w, "fit")) { s->actual = false; continue; }
		if (!strcmp(w, "draft") || !strcmp(w, "high")) {
			memcpy(s->quality, w, strlen(w) + 1);
			if (keep) add(keep, keep_n, w);
			continue;
		}
		if (!strcmp(w, "normal")) { s->quality[0] = 0; continue; }
		if (!strcmp(w, "paper") || !strcmp(w, "copies")) {
			int k = 0;
			while (*p == ' ') p++;
			while (*p && *p != ' ' && k < 23) { char c = *p++; v[k++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c); }
			v[k] = 0;
			if (!k) { say(err, err_n, w, ": of what?"); return false; }
			if (w[0] == 'p') {
				const zpage_size_t *z = zpage_find(v);
				if (strcmp(v, "page") && strcmp(v, "auto") && (!z || z->kind != ZPAGE_PAPER)) {
					say(err, err_n, "not a paper: ", v);
					return false;
				}
				if (!strcmp(v, "auto")) s->paper[0] = 0;
				else memcpy(s->paper, v, strlen(v) + 1 < sizeof(s->paper) ? strlen(v) + 1 : sizeof(s->paper));
				s->paper[sizeof(s->paper) - 1] = 0;
				if (keep && s->paper[0]) { add(keep, keep_n, "paper"); add(keep, keep_n, s->paper); }
			} else {
				const char *e;
				int c = num(v, &e);
				if (c < 1 || c > 99 || *e) { say(err, err_n, "copies: 1 to 99", NULL); return false; }
				s->copies = c;
				// not kept: copies are for this time
			}
			continue;
		}
		say(err, err_n, "print: what is ", w);
		return false;
	}
	return true;
}

bool printjob_parse(pj_settings_t *s, const char *deck_words, const char *line,
	int cur, int ncard, char *keep, int keep_n, char *err, int err_n) {
	char tmp[96], dw[96];
	// the deck's words may be `keep` itself
	say(dw, sizeof(dw), deck_words ? deck_words : "", NULL);
	deck_words = dw;
	memset(s, 0, sizeof(*s));
	s->nup = 1;
	s->copies = 1;
	s->count = -1;
	err[0] = 0;
	if (keep) keep[0] = 0;
	// the deck's settings first, then the line's on top; what is kept is
	// rebuilt from the result
	if (deck_words && !words(s, deck_words, false, cur, ncard, NULL, 0, err, err_n)) return false;
	if (line && !words(s, line, true, cur, ncard, NULL, 0, err, err_n)) return false;
	if (keep) {
		tmp[0] = 0;
		if (s->nup > 1) { char b[8]; int k = 0, v = s->nup; if (v >= 10) b[k++] = (char)('0' + v / 10); b[k++] = (char)('0' + v % 10); b[k++] = 'u'; b[k++] = 'p'; b[k] = 0; add(tmp, sizeof(tmp), b); }
		if (s->cut) add(tmp, sizeof(tmp), "cut");
		if (s->frame) add(tmp, sizeof(tmp), "frame");
		if (s->actual) add(tmp, sizeof(tmp), "actual");
		if (s->quality[0]) add(tmp, sizeof(tmp), s->quality);
		if (s->paper[0]) { add(tmp, sizeof(tmp), "paper"); add(tmp, sizeof(tmp), s->paper); }
		say(keep, keep_n, tmp, NULL);
	}
	return true;
}

void printjob_options(const pj_settings_t *s, zp_writer_t *w) {
	char b[8];
	if (s->nup > 1) {
		int k = 0;
		if (s->nup >= 10) b[k++] = (char)('0' + s->nup / 10);
		b[k++] = (char)('0' + s->nup % 10);
		b[k] = 0;
		zp_job(w, "nup", b);
	}
	if (s->copies > 1) {
		int k = 0;
		if (s->copies >= 10) b[k++] = (char)('0' + s->copies / 10);
		b[k++] = (char)('0' + s->copies % 10);
		b[k] = 0;
		zp_job(w, "copies", b);
	}
	if (s->cut) zp_job(w, "cut", "1");
	if (s->frame) zp_job(w, "frame", "1");
	if (s->actual) zp_job(w, "size", "actual");
	if (s->quality[0]) zp_job(w, "quality", s->quality);
	if (s->paper[0]) zp_job(w, "paper", s->paper);
	else if (s->dest == PJ_PDF) zp_job(w, "paper", "page");	// a PDF of slides: a page each, their shape
}
