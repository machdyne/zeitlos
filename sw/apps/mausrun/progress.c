/*
 * mausrun -- what is remembered between games. See progress.h.
 */

#include <string.h>

#include "progress.h"

static const char *const words[MR_SKILLS] = { "easy", "normal", "hard" };
static const char *const names[MR_SKILLS] = { "EASY", "NORMAL", "HARD" };
static const char *const spoken[MR_SKILLS] = { "Easy", "Normal", "Hard" };

const char *mr_skill_name(int s) { return (s >= 0 && s < MR_SKILLS) ? names[s] : "?"; }
const char *mr_skill_spoken(int s) { return (s >= 0 && s < MR_SKILLS) ? spoken[s] : ""; }

static bool is_end(char c) {
	return c == 0 || c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* `s` starts with the word `w` (ignoring case), then a word's end */
static bool word_is(const char *s, const char *w) {
	while (*w) {
		char c = *s++;
		if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
		if (c != *w++) return false;
	}
	return is_end(*s);
}

int mr_skill_word(const char *w) {
	int i;
	if (!w) return -1;
	while (*w == ' ' || *w == '\t') w++;
	for (i = 0; i < MR_SKILLS; i++) if (word_is(w, words[i])) return i;
	return -1;
}

static const char *skip_word(const char *s) {
	while (*s == ' ' || *s == '\t') s++;
	while (!is_end(*s)) s++;
	while (*s == ' ' || *s == '\t') s++;
	return s;
}

static int number(const char **sp) {
	const char *s = *sp;
	int v = 0, any = 0;
	while (*s == ' ' || *s == '\t') s++;
	while (*s >= '0' && *s <= '9' && v < 100000) { v = v * 10 + (*s++ - '0'); any = 1; }
	*sp = s;
	return any ? v : -1;
}

/* one of ours: written back by mr_progress_format() itself */
static bool ours(const char *s) {
	while (*s == ' ' || *s == '\t') s++;
	return word_is(s, "skill") || word_is(s, "unlocked") || word_is(s, "best") ||
		word_is(s, "stars");
}

void mr_progress_parse(const char *text, mr_progress_t *p) {
	const char *s = text;
	memset(p, 0, sizeof(*p));
	p->skill = -1;
	p->unlocked = 1;
	while (s && *s) {
		const char *l = s, *v;
		while (*l == ' ' || *l == '\t') l++;
		v = skip_word(l);
		if (word_is(l, "skill")) {
			int k = mr_skill_word(v);
			if (k >= 0) p->skill = k;
		} else if (word_is(l, "unlocked")) {
			int n = number(&v);
			if (n >= 1 && n <= MR_LEVELS_MAX) p->unlocked = n;
		} else if (word_is(l, "best")) {
			int lv = number(&v), n = number(&v);
			if (lv >= 1 && lv <= MR_LEVELS_MAX && n >= 0 && n < 65536)
				p->best[lv] = (uint16_t)n;
		} else if (word_is(l, "stars")) {
			int lv = number(&v), m = number(&v);
			if (lv >= 1 && lv <= MR_LEVELS_MAX && m >= 0 && m <= 7)
				p->stars[lv] = (uint8_t)m;
		}
		while (*s && *s != '\n') s++;
		if (*s) s++;
	}
}

static int put(char *out, int cap, int n, const char *s) {
	while (*s) {
		if (n >= cap - 1) return -1;
		out[n++] = *s++;
	}
	return n;
}

static int put_num(char *out, int cap, int n, int v) {
	char b[12];
	int k = 0;
	if (v < 0) v = 0;
	do { b[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 11);
	while (k > 0 && n >= 0) {
		if (n >= cap - 1) return -1;
		out[n++] = b[--k];
	}
	return n;
}

int mr_progress_format(char *out, int cap, const char *old,
	const mr_progress_t *p) {
	int n = 0, lv;
	const char *s = old;
	if (cap <= 0) return -1;
	while (s && *s) {
		const char *e = s;
		while (*e && *e != '\n') e++;
		if (e > s && !ours(s)) {
			int len = (int)(e - s);
			if (n + len + 1 >= cap) return -1;
			memcpy(out + n, s, (size_t)len);
			n += len;
			out[n++] = '\n';
		}
		s = *e ? e + 1 : e;
	}
	if (p->skill >= 0 && p->skill < MR_SKILLS) {
		n = put(out, cap, n, "skill ");
		if (n >= 0) n = put(out, cap, n, words[p->skill]);
		if (n >= 0) n = put(out, cap, n, "\n");
	}
	if (n >= 0) n = put(out, cap, n, "unlocked ");
	if (n >= 0) n = put_num(out, cap, n, p->unlocked);
	if (n >= 0) n = put(out, cap, n, "\n");
	for (lv = 1; lv <= MR_LEVELS_MAX && n >= 0; lv++) {
		if (!p->best[lv]) continue;
		n = put(out, cap, n, "best ");
		if (n >= 0) n = put_num(out, cap, n, lv);
		if (n >= 0) n = put(out, cap, n, " ");
		if (n >= 0) n = put_num(out, cap, n, p->best[lv]);
		if (n >= 0) n = put(out, cap, n, "\n");
	}
	for (lv = 1; lv <= MR_LEVELS_MAX && n >= 0; lv++) {
		if (!p->stars[lv]) continue;
		n = put(out, cap, n, "stars ");
		if (n >= 0) n = put_num(out, cap, n, lv);
		if (n >= 0) n = put(out, cap, n, " ");
		if (n >= 0) n = put_num(out, cap, n, p->stars[lv] & 7);
		if (n >= 0) n = put(out, cap, n, "\n");
	}
	if (n < 0) return -1;
	out[n] = 0;
	return n;
}

int mr_star_count(unsigned mask) {
	return (int)((mask & 1) + ((mask >> 1) & 1) + ((mask >> 2) & 1));
}

int mr_progress_stars(const mr_progress_t *p) {
	int lv, n = 0;
	for (lv = 1; lv <= MR_LEVELS_MAX; lv++) n += mr_star_count(p->stars[lv]);
	return n;
}
