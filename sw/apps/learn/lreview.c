/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- review: Leitner boxes from the learner's review.txt.
 * lreview.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lreview.h"
#include "lstore.h"

// days until a question comes back, by box
static const int interval[LR_BOXES + 1] = { 0, 1, 3, 7, 16, 35 };

static struct {
	lr_item_t	*item;
	int			n, cap;
	uint32_t	*hash;			// open addressing: item index + 1, 0 empty
	uint32_t	nhash;
	char		*pool;
	uint32_t	pool_used, pool_cap;
	int32_t		today;
	int32_t		last_day;		// the latest day in the file
	bool		open;
} R;

static uint32_t fnv(const char *s) {
	uint32_t h = 2166136261u;
	while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
	return h;
}

const char *lr_name(uint32_t off) { return R.pool && off < R.pool_used ? R.pool + off : ""; }

static uint32_t intern(const char *s) {
	uint32_t n = (uint32_t)strlen(s) + 1;
	if (R.pool_used + n > R.pool_cap) return 0;
	uint32_t off = R.pool_used;
	memcpy(R.pool + off, s, n);
	R.pool_used += n;
	return off;
}

// the item of an id ("lesson#key"), made if `make`
static lr_item_t *find(const char *id, bool make) {
	if (!R.open || !R.nhash) return NULL;
	uint32_t h = fnv(id) & (R.nhash - 1);
	for (uint32_t k = 0; k < R.nhash; k++) {
		uint32_t *slot = &R.hash[(h + k) & (R.nhash - 1)];
		if (!*slot) {
			if (!make || R.n >= R.cap || R.n * 4 >= (int)R.nhash * 3) return NULL;
			uint32_t off = intern(id);
			if (!off) return NULL;
			lr_item_t *it = &R.item[R.n++];
			memset(it, 0, sizeof *it);
			it->id = off;
			*slot = (uint32_t)R.n;
			return it;
		}
		lr_item_t *it = &R.item[*slot - 1];
		if (!strcmp(R.pool + it->id, id)) return it;
	}
	return NULL;
}

static void place(lr_item_t *it, int32_t day, bool right) {
	if (!right) it->box = 1;
	else if (!it->box) it->box = 2;		// new, and known already: past the first box
	else if (it->box < LR_BOXES) it->box++;
	it->due = day + interval[it->box];
}

static void apply(char *line) {
	char what[4], id[260], file[200];
	long day = 0;
	int step = 0;
	file[0] = 0;
	if (line[0] == 'D') {			// a day without a clock
		if (sscanf(line + 1, "%ld", &day) == 1 && day > R.last_day) R.last_day = (int32_t)day;
		return;
	}
	int n = sscanf(line, "%ld %3s %259s %d %199s", &day, what, id, &step, file);
	if (n < 3 || !strchr(id, '#')) return;
	char w = what[0];
	if (w != 'r' && w != 'w' && w != 'R' && w != 'W') return;
	lr_item_t *it = find(id, true);
	if (!it) return;
	if (day > R.last_day) R.last_day = (int32_t)day;
	it->step = (uint8_t)(step < 0 ? 0 : step > 255 ? 255 : step);
	if (file[0] && strcmp(file, "-") && (!it->file || strcmp(R.pool + it->file, file))) it->file = intern(file);
	place(it, (int32_t)day, w == 'r' || w == 'R');
}

static void path_of(char *out, int cap) {
	const ls_who_t *who = ls_current();
	snprintf(out, (size_t)cap, "%s/review.txt", who ? who->dir : "");
}

bool lr_open(void *mem, uint32_t size) {
	memset(&R, 0, sizeof R);
	if (!ls_current()) return false;
	// a quarter for the items, a sixteenth for the table, the rest for names
	R.cap = (int)(size / 4 / sizeof(lr_item_t));
	R.item = (lr_item_t *)mem;
	uint32_t nh = 64;
	while (nh * 2 * sizeof(uint32_t) <= size / 16) nh *= 2;
	R.nhash = nh;
	R.hash = (uint32_t *)((char *)mem + (size / 4 & ~7u));
	memset(R.hash, 0, nh * sizeof(uint32_t));
	R.pool = (char *)R.hash + nh * sizeof(uint32_t);
	R.pool_cap = size - (uint32_t)(R.pool - (char *)mem);
	R.pool_used = 1;
	R.open = true;

	char path[160];
	path_of(path, sizeof path);
	static char buf[4096 + 1];
	long off = 0, keep = 0;
	for (;;) {
		long n = LP->read_at(path, off, buf + keep, (long)sizeof buf - 1 - keep);
		if (n <= 0) break;
		off += n;
		long len = keep + n;
		buf[len] = 0;
		char *p = buf, *e;
		while ((e = memchr(p, '\n', (size_t)(buf + len - p)))) {
			*e = 0;
			if (e > p && e[-1] == '\r') e[-1] = 0;
			if (*p && *p != '#') apply(p);
			p = e + 1;
		}
		keep = (long)(buf + len - p);
		if (keep >= (long)sizeof buf - 1) keep = 0;
		memmove(buf, p, (size_t)keep);
	}

	// today: the clock's day; without one, one more than the last
	uint32_t now = LP->now();
	if (now) R.today = (int32_t)(now / 86400);
	else {
		R.today = R.last_day + 1;
		char line[32];
		int n = snprintf(line, sizeof line, "D %ld\n", (long)R.today);
		LP->append(path, line, n);
	}
	if (R.today < R.last_day) R.today = R.last_day;		// a clock set back
	return true;
}

int32_t lr_today(void) { return R.today; }

void lr_key(const lq_q_t *q, char *out, int cap) {
	if (q->id && q->id[0]) { snprintf(out, (size_t)cap, "%s", q->id); return; }
	uint32_t h = 2166136261u;
	h ^= q->kind;
	h *= 16777619u;
	for (int i = 0; i < q->nask; i++)
		for (const char *s = q->ask[i].text ? q->ask[i].text : ""; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
	snprintf(out, (size_t)cap, "q%08lx", (unsigned long)h);
}

bool lr_answered(const char *lesson_id, const char *key, int step, const char *file,
	bool right_first, bool in_review) {
	if (!R.open) return false;
	char id[260];
	snprintf(id, sizeof id, "%s#%s", lesson_id, key);
	char w = in_review ? (right_first ? 'R' : 'W') : (right_first ? 'r' : 'w');
	lr_item_t *it = find(id, true);
	if (it) {
		it->step = (uint8_t)(step < 0 ? 0 : step > 255 ? 255 : step);
		if (file && file[0] && (!it->file || strcmp(R.pool + it->file, file))) it->file = intern(file);
		place(it, R.today, right_first);
	}
	char line[520], path[160];
	int n = snprintf(line, sizeof line, "%ld %c %s %d %s\n", (long)R.today, w, id, step,
		file && file[0] ? file : "-");
	if (n >= (int)sizeof line) return false;
	path_of(path, sizeof path);
	return LP->append(path, line, n);
}

int lr_count(void) { return R.n; }
const lr_item_t *lr_get(int i) { return i >= 0 && i < R.n ? &R.item[i] : NULL; }

int lr_box_of(const char *lesson_id, const char *key) {
	char id[260];
	snprintf(id, sizeof id, "%s#%s", lesson_id, key);
	lr_item_t *it = find(id, false);
	return it ? it->box : 0;
}

void lr_course_of(const char *id, char *out, int cap) {
	int n = 0;
	while (id[n] && id[n] != '/' && id[n] != '#' && n < cap - 1) { out[n] = id[n]; n++; }
	out[n] = 0;
}

static bool in_course(const lr_item_t *it, const char *course) {
	if (!course) return true;
	size_t l = strlen(course);
	const char *id = R.pool + it->id;
	return !strncmp(id, course, l) && (id[l] == '/' || id[l] == '#');
}

int lr_count_due(const char *course) {
	int n = 0;
	for (int i = 0; i < R.n; i++) if (R.item[i].due <= R.today && in_course(&R.item[i], course)) n++;
	return n;
}

int lr_due(int *out, int max, const char *course) {
	// the due ones, the longest-waiting first (then the lowest box)
	static int idx[4096];
	int n = 0;
	for (int i = 0; i < R.n && n < 4096; i++)
		if (R.item[i].due <= R.today && in_course(&R.item[i], course)) idx[n++] = i;
	for (int i = 1; i < n; i++)
		for (int j = i; j > 0; j--) {
			const lr_item_t *a = &R.item[idx[j - 1]], *b = &R.item[idx[j]];
			if (a->due < b->due || (a->due == b->due && a->box <= b->box)) break;
			int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t;
		}
	// then the courses in turn: one from each, again and again
	int got = 0;
	static uint8_t taken[4096];
	memset(taken, 0, (size_t)n);
	while (got < max && got < n) {
		char seen[16][48];
		int ns = 0;
		bool any = false;
		for (int i = 0; i < n && got < max; i++) {
			if (taken[i]) continue;
			char c[48];
			lr_course_of(R.pool + R.item[idx[i]].id, c, sizeof c);
			bool dup = false;
			for (int k = 0; k < ns; k++) if (!strcmp(seen[k], c)) dup = true;
			if (dup) continue;
			if (ns < 16) strcpy(seen[ns++], c);
			taken[i] = 1;
			out[got++] = idx[i];
			any = true;
		}
		if (!any) break;
	}
	return got;
}
