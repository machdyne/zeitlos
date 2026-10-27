/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- learners and their records. lstore.h.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "lstore.h"
#include "zpaths.h"
#include "zrtc.h"

const lplat_t *LP;

// -- names --

bool ls_name_ok(const char *n) {
	int len = (int)strlen(n);
	if (len < 1 || len >= LS_NAME || n[0] == ' ' || n[len - 1] == ' ' || n[0] == '.') return false;
	for (const char *p = n; *p; p++) {
		unsigned char c = (unsigned char)*p;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
			c == ' ' || c == '-' || c == '_' || c >= 0x80)) return false;
	}
	return true;
}

typedef struct {
	ls_who_t	*out;
	int			max, n;
	uint8_t		where;
	const char	*root;
} lister_t;

static void on_entry(void *arg, const char *name, bool is_dir) {
	lister_t *l = (lister_t *)arg;
	if (!is_dir || l->n >= l->max || !ls_name_ok(name)) return;
	ls_who_t *w = &l->out[l->n];
	snprintf(w->dir, sizeof w->dir, "%s/%s", l->root, name);
	// the name as the learner wrote it is in learner.cfg; the folder's name otherwise
	char path[128], cfg[256];
	snprintf(path, sizeof path, "%s/learner.cfg", w->dir);
	long n = LP->fs.read(LP->fs.ctx, path, cfg, sizeof cfg - 1);
	strncpy(w->name, name, LS_NAME - 1);
	w->name[LS_NAME - 1] = 0;
	if (n > 0) {
		cfg[n] = 0;
		char *k = strstr(cfg, "name =");
		if (k && (k == cfg || k[-1] == '\n')) {
			k += 6;
			while (*k == ' ') k++;
			int i = 0;
			while (k[i] && k[i] != '\n' && k[i] != '\r' && i < LS_NAME - 1) { w->name[i] = k[i]; i++; }
			w->name[i] = 0;
		}
	}
	w->where = l->where;
	l->n++;
}

int ls_list(ls_who_t *out, int max) {
	lister_t l = { out, max, 0, LS_ON_MACHINE, Z_DIR_LEARN_LEARNERS };
	LP->fs.list(LP->fs.ctx, Z_DIR_LEARN_LEARNERS, on_entry, &l);
	l.where = LS_ON_STICK;
	l.root = Z_DIR_USB_LEARN;
	LP->fs.list(LP->fs.ctx, Z_DIR_USB_LEARN, on_entry, &l);
	return l.n;
}

bool ls_create(const char *name, int where, ls_who_t *out) {
	if (!ls_name_ok(name)) return false;
	const char *root = where == LS_ON_STICK ? Z_DIR_USB_LEARN : Z_DIR_LEARN_LEARNERS;
	if (where == LS_ON_MACHINE) LP->mkdir(Z_DIR_LEARN_DATA);
	if (!LP->mkdir(root)) return false;
	memset(out, 0, sizeof *out);
	snprintf(out->dir, sizeof out->dir, "%s/%s", root, name);
	if (LP->exists(out->dir)) return false;
	if (!LP->mkdir(out->dir)) return false;
	strncpy(out->name, name, LS_NAME - 1);
	out->where = (uint8_t)where;
	char path[128], cfg[160], stamp[24];
	ls_stamp(LP->now(), stamp, sizeof stamp);
	snprintf(path, sizeof path, "%s/learner.cfg", out->dir);
	int n = snprintf(cfg, sizeof cfg, "# a learner of learn (docs/learn.md)\nname = %s\ncreated = %s\n", name, stamp);
	return LP->write_whole(path, cfg, n);
}

// -- the records --

typedef struct {
	uint32_t	hash;
	uint32_t	key;		// offset of the id in the string pool, 0 = empty
	ls_rec_t	r;
} slot_t;

static struct {
	ls_who_t	who;
	bool		open;
	slot_t		*slots;
	uint32_t	nslots, used;
	char		*pool;
	uint32_t	pool_cap, pool_used;
	char		last[128];			// the most recent lesson
	// the last lesson per course: a short list
	char		last_course[32][LF_NAME];
	char		last_lesson[32][128];
	int			nlast;
} S;

static uint32_t hash(const char *s) {
	uint32_t h = 2166136261u;
	while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
	return h ? h : 1;
}

static slot_t *find(const char *id, bool make) {
	if (!S.nslots) return NULL;
	uint32_t h = hash(id), i = h & (S.nslots - 1);
	for (uint32_t probe = 0; probe < S.nslots; probe++, i = (i + 1) & (S.nslots - 1)) {
		slot_t *s = &S.slots[i];
		if (!s->key) {
			if (!make || S.used * 4 >= S.nslots * 3) return NULL;	// keep it a quarter empty
			uint32_t len = (uint32_t)strlen(id) + 1;
			if (S.pool_used + len > S.pool_cap) return NULL;
			memcpy(S.pool + S.pool_used, id, len);
			s->key = S.pool_used;
			S.pool_used += len;
			s->hash = h;
			memset(&s->r, 0, sizeof s->r);
			S.used++;
			return s;
		}
		if (s->hash == h && !strcmp(S.pool + s->key, id)) return s;
	}
	return NULL;
}

static void note_last(const char *lesson_id) {
	strncpy(S.last, lesson_id, sizeof S.last - 1);
	const char *sl = strchr(lesson_id, '/');
	if (!sl || sl - lesson_id >= LF_NAME) return;
	char course[LF_NAME];
	memcpy(course, lesson_id, (size_t)(sl - lesson_id));
	course[sl - lesson_id] = 0;
	// the most recent first: this course moves to the front
	int i;
	for (i = 0; i < S.nlast; i++) if (!strcmp(S.last_course[i], course)) break;
	if (i == S.nlast) { if (S.nlast < 32) S.nlast++; i = S.nlast - 1; }
	for (; i > 0; i--) {
		strcpy(S.last_course[i], S.last_course[i - 1]);
		strcpy(S.last_lesson[i], S.last_lesson[i - 1]);
	}
	strcpy(S.last_course[0], course);
	strncpy(S.last_lesson[0], lesson_id, sizeof S.last_lesson[0] - 1);
	S.last_lesson[0][sizeof S.last_lesson[0] - 1] = 0;
}

int ls_ncourses(void) { return S.nlast; }
const char *ls_course(int i) { return i >= 0 && i < S.nlast ? S.last_course[i] : ""; }

int ls_count_done(const char *prefix) {
	int n = 0;
	size_t pl = strlen(prefix);
	for (uint32_t i = 0; i < S.nslots; i++) {
		const slot_t *s = &S.slots[i];
		if (!s->key || !s->r.done) continue;
		const char *id = S.pool + s->key;
		if (!strncmp(id, prefix, pl) && (id[pl] == '/' || id[pl] == 0)) n++;
	}
	return n;
}

static uint32_t parse_stamp(const char *s) {
	// 2026-10-26T09:12
	z_tm_t tm;
	memset(&tm, 0, sizeof tm);
	int y, mo, d, h, mi;
	if (sscanf(s, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) != 5) return 0;
	tm.year = (uint16_t)y; tm.month = (uint8_t)mo; tm.day = (uint8_t)d; tm.hour = (uint8_t)h; tm.min = (uint8_t)mi;
	return z_tm_to_time(&tm);
}

static void apply(char *line) {
	char stamp[24], what[8], id[128], a[24], b[24];
	a[0] = b[0] = 0;
	int n = sscanf(line, "%23s %7s %127s %23s %23s", stamp, what, id, a, b);
	if (n < 3) return;
	slot_t *s = find(id, true);
	if (!s) return;
	uint32_t t = parse_stamp(stamp);
	if (t) s->r.when = t;
	if (!strcmp(what, "done")) {
		int r = 0, tot = 0;
		sscanf(a, "%d/%d", &r, &tot);
		if (!s->r.done || r * (s->r.total ? s->r.total : 1) > s->r.right * (tot ? tot : 1) ||
			(tot && !s->r.total)) {
			s->r.right = (uint8_t)(r > 255 ? 255 : r);
			s->r.total = (uint8_t)(tot > 255 ? 255 : tot);
		}
		s->r.done = 1;
		s->r.step = 0;
	} else if (!strcmp(what, "at")) {
		s->r.step = (uint8_t)atoi(a);
	} else return;
	note_last(id);
}

bool ls_open(const ls_who_t *who, void *mem, uint32_t memsize) {
	memset(&S, 0, sizeof S);
	S.who = *who;
	// a third of the memory for the table, the rest for the ids
	uint32_t n = 64;
	while ((n * 2) * sizeof(slot_t) <= memsize / 3) n *= 2;
	S.slots = (slot_t *)mem;
	S.nslots = n;
	memset(S.slots, 0, n * sizeof(slot_t));
	S.pool = (char *)mem + n * sizeof(slot_t);
	S.pool_cap = memsize - n * (uint32_t)sizeof(slot_t);
	S.pool_used = 1;		// 0 means empty
	S.open = true;

	char path[128];
	snprintf(path, sizeof path, "%s/progress.txt", who->dir);
	// in pieces: a long history needs no long buffer. Only whole lines
	// count -- a line cut off by a power cut is not a record.
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
		if (keep >= (long)sizeof buf - 1) keep = 0;		// a line longer than the buffer: not a record
		memmove(buf, p, (size_t)keep);
	}
	return true;
}

const ls_who_t *ls_current(void) { return S.open ? &S.who : NULL; }

const ls_rec_t *ls_get(const char *lesson_id) {
	slot_t *s = find(lesson_id, false);
	return s ? &s->r : NULL;
}

const char *ls_last_in(const char *course_id) {
	for (int i = 0; i < S.nlast; i++) if (!strcmp(S.last_course[i], course_id)) return S.last_lesson[i];
	return "";
}

const char *ls_last(void) { return S.last; }

void ls_stamp(uint32_t t, char *out, int cap) {
	if (!t) { snprintf(out, (size_t)cap, "-"); return; }
	z_tm_t tm;
	z_time_to_tm(t, &tm);
	snprintf(out, (size_t)cap, "%04d-%02d-%02dT%02d:%02d", (int)tm.year, (int)tm.month, (int)tm.day, (int)tm.hour, (int)tm.min);
}

static bool add_line(const char *file, const char *line) {
	if (!S.open) return false;
	char path[128];
	snprintf(path, sizeof path, "%s/%s", S.who.dir, file);
	return LP->append(path, line, (long)strlen(line));
}

bool ls_done(const char *lesson_id, int right, int total, uint32_t seconds) {
	char line[200], stamp[24];
	uint32_t now = LP->now();
	ls_stamp(now, stamp, sizeof stamp);
	snprintf(line, sizeof line, "%s done %s %d/%d %lu\n", stamp, lesson_id, right, total, (unsigned long)seconds);
	char copy[200];
	strcpy(copy, line);
	copy[strlen(copy) - 1] = 0;
	apply(copy);
	if (now) { slot_t *s = find(lesson_id, false); if (s) s->r.when = now; }
	return add_line("progress.txt", line);
}

bool ls_at(const char *lesson_id, int step) {
	char line[200], stamp[24];
	ls_stamp(LP->now(), stamp, sizeof stamp);
	snprintf(line, sizeof line, "%s at %s %d\n", stamp, lesson_id, step);
	char copy[200];
	strcpy(copy, line);
	copy[strlen(copy) - 1] = 0;
	apply(copy);
	return add_line("progress.txt", line);
}

bool ls_report(const char *course_id, int version, const char *lesson_id, int step, const char *words) {
	char line[400], stamp[24], clean[240];
	int k = 0;
	for (const char *p = words; *p && k < (int)sizeof clean - 1; p++) clean[k++] = (*p == '\n' || *p == '\r') ? ' ' : *p;
	clean[k] = 0;
	ls_stamp(LP->now(), stamp, sizeof stamp);
	snprintf(line, sizeof line, "%s %s v%d %s step %d: %s\n", stamp, course_id, version, lesson_id, step + 1, clean);
	return add_line("reports.txt", line);
}
