/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * backup's core. See bk.h, and docs/backup.md for the rules.
 *
 * -- the order of a backup --
 *
 *   1. Take <dest>/<name>/LOCK.TXT (one run at a time; a lock whose
 *      owner is gone is taken over).
 *   2. Remove what a run cut short left: a backup folder with no
 *      INDEX.TXT, or one that does not end with its "end" line.
 *   3. Read the state the newest backups describe: the newest full
 *      backup's index, with the watch backups after it laid over it.
 *   4. Look at the card (sources, skip rules). A file of the same size
 *      and date as before is unchanged; one whose date cannot be
 *      trusted (the clock was not set when it was written) is read and
 *      its CRC compared; anything else is copied.
 *   5. Check the room. Short: make room (oldest copies that have a
 *      newer copy first). Still short: write nothing, say so.
 *   6. Name the backup (bk_name_next), make its folder, copy each file
 *      a chunk at a time, sync, read it back.
 *   7. Write INDEX.TXT, the "end" line last; sync; read it back.
 *   8. Remove LOCK.TXT.
 *
 * Nothing on the drive is renamed or rewritten at any step.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bk.h"

#define INDEX_NAME  "INDEX.TXT"
#define PIN_NAME    "PINNED"
#define LOCK_NAME   "LOCK.TXT"
#define CHECK_DIR   "check"
#define README_NAME "README.TXT"

// FAT's stamp for a file written while the clock was not set
// (get_fattime() in sw/apps/fs/fs.c): 2020-01-01 00:00:00.
#define FAT_NOCLOCK_DATE ((uint16_t)((40u << 9) | (1u << 5) | 1u))
#define FAT_NOCLOCK_TIME ((uint16_t)0)

// -- small things -----------------------------------------------------------

static const bk_io_t *IO;     // the run's io (one run at a time)
static const bk_opts_t *OPT;

static void say(const char *fmt, ...) {
	char b[384];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	IO->out(IO->ctx, b);
}

static void progress(int phase, const char *path, uint32_t fd, uint32_t ft,
		uint64_t bd, uint64_t bt) {
	bk_progress_t p;
	if (!IO->progress) return;
	p.phase = phase; p.path = path;
	p.files_done = fd; p.files_total = ft;
	p.bytes_done = bd; p.bytes_total = bt;
	IO->progress(IO->ctx, &p);
}

static bool stopped(void) {
	return IO->stop && IO->stop(IO->ctx);
}

static void breathe(void) {
	if (IO->yield) IO->yield(IO->ctx);
}

static uint32_t crc_tab[256];
static bool crc_ready;

uint32_t bk_crc32(uint32_t crc, const void *p, uint32_t n) {
	const uint8_t *b = p;
	if (!crc_ready) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			crc_tab[i] = c;
		}
		crc_ready = true;
	}
	crc = ~crc;
	while (n--) crc = crc_tab[(crc ^ *b++) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int ci_ncmp(const char *a, const char *b, size_t n) {
	for (size_t i = 0; i < n; i++) {
		int x = lower((unsigned char)a[i]), y = lower((unsigned char)b[i]);
		if (x != y) return x - y;
		if (!x) return 0;
	}
	return 0;
}

static bool ci_eq(const char *a, const char *b) {
	return ci_ncmp(a, b, (size_t)-1) == 0;
}

// path is inside dir, or is dir (case-insensitive, as FAT).
static bool inside(const char *path, const char *dir) {
	size_t n = strlen(dir);
	while (n > 1 && dir[n - 1] == '/') n--;
	if (n == 1 && dir[0] == '/') return true;
	if (ci_ncmp(path, dir, n)) return false;
	return path[n] == 0 || path[n] == '/';
}

bool bk_glob(const char *p, const char *s) {
	const char *star = NULL, *ss = NULL;
	while (*s) {
		if (*p == '?' || (*p && *p != '*' && lower((unsigned char)*p) == lower((unsigned char)*s))) {
			p++; s++;
		} else if (*p == '*') {
			star = p++; ss = s;
		} else if (star) {
			p = star + 1; s = ++ss;
		} else return false;
	}
	while (*p == '*') p++;
	return *p == 0;
}

static bool ends_ci(const char *s, const char *suf) {
	size_t a = strlen(s), b = strlen(suf);
	return a >= b && ci_eq(s + a - b, suf);
}

// -- reading fields (no sscanf: it would bring newlib's scanf in) --

static bool get_u(const char **pp, uint64_t *v, int base) {
	const char *p = *pp;
	uint64_t x = 0;
	int n = 0;
	for (;; p++, n++) {
		int d;
		if (*p >= '0' && *p <= '9') d = *p - '0';
		else if (base == 16 && lower((unsigned char)*p) >= 'a' && lower((unsigned char)*p) <= 'f')
			d = lower((unsigned char)*p) - 'a' + 10;
		else break;
		x = x * (uint64_t)base + (uint64_t)d;
	}
	if (!n || (*p && *p != ' ')) return false;
	while (*p == ' ') p++;
	*v = x; *pp = p;
	return true;
}

static bool get_w(const char **pp, char *out, int cap) {
	const char *p = *pp;
	int n = 0;
	while (*p && *p != ' ') { if (n + 1 >= cap) return false; out[n++] = *p++; }
	if (!n) return false;
	out[n] = 0;
	while (*p == ' ') p++;
	*pp = p;
	return true;
}

static bool num2(const char *s, unsigned *v) {
	if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9') return false;
	*v = (unsigned)(s[0] - '0') * 10u + (unsigned)(s[1] - '0');
	return true;
}

static char *xstrdup(const char *s) {
	size_t n = strlen(s) + 1;
	char *d = malloc(n);
	if (d) memcpy(d, s, n);
	return d;
}

// snprintf into a path buffer; false if it does not fit.
static bool pjoin(char *out, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(out, BK_PATH_MAX, fmt, ap);
	va_end(ap);
	return n > 0 && n < BK_PATH_MAX;
}

static const char *base_name(const char *path) {
	const char *s = strrchr(path, '/');
	return s ? s + 1 : path;
}

// FAT date/time as "2026-10-21_11-30-48", or "-" unknown.
static void fat_str(uint16_t d, uint16_t t, char out[20]) {
	if (!d) { strcpy(out, "-"); return; }
	snprintf(out, 20, "%04u-%02u-%02u_%02u-%02u-%02u",
		1980u + (d >> 9), (d >> 5) & 15u, d & 31u,
		t >> 11, (t >> 5) & 63u, (t & 31u) * 2u);
}

static bool fat_parse(const char *s, uint16_t *d, uint16_t *t) {
	unsigned Y, y2, M, D, h, m, sec;
	if (!strcmp(s, "-")) { *d = 0; *t = 0; return true; }
	if (strlen(s) != 19 || !num2(s, &Y) || !num2(s + 2, &y2) || s[4] != '-' || !num2(s + 5, &M) ||
		s[7] != '-' || !num2(s + 8, &D) || s[10] != '_' || !num2(s + 11, &h) || s[13] != '-' ||
		!num2(s + 14, &m) || s[16] != '-' || !num2(s + 17, &sec)) return false;
	Y = Y * 100u + y2;
	if (Y < 1980 || Y > 2107 || M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || m > 59 || sec > 59)
		return false;
	*d = (uint16_t)(((Y - 1980) << 9) | (M << 5) | D);
	*t = (uint16_t)((h << 11) | (m << 5) | (sec / 2));
	return true;
}

static bool date_known(uint16_t d, uint16_t t) {
	return d && !(d == FAT_NOCLOCK_DATE && t == FAT_NOCLOCK_TIME);
}

// -- names ------------------------------------------------------------------
//
// "YYYY-MM-DD_HH-MM-SS", then "+NNNN" when the clock was not set, then
// "w" for a watch backup. Ordered by the base, then by N, which is the
// order they were made in: the base only ever grows (bk_name_next).

static bool digits(const char *s, int n) {
	for (int i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return false;
	return true;
}

bool bk_name_parse(const char *s, char base[20], int *n, bool *watch) {
	if (strlen(s) < 19) return false;
	if (!digits(s, 4) || s[4] != '-' || !digits(s + 5, 2) || s[7] != '-' ||
		!digits(s + 8, 2) || s[10] != '_' || !digits(s + 11, 2) || s[13] != '-' ||
		!digits(s + 14, 2) || s[16] != '-' || !digits(s + 17, 2)) return false;
	const char *p = s + 19;
	int num = 0;
	if (*p == '+') {
		if (!digits(p + 1, 4)) return false;
		num = atoi(p + 1);
		if (num < 1) return false;
		p += 5;
	}
	bool w = false;
	if (*p == 'w' || *p == 'W') { w = true; p++; }
	if (*p) return false;
	if (base) { memcpy(base, s, 19); base[19] = 0; }
	if (n) *n = num;
	if (watch) *watch = w;
	return true;
}

int bk_name_cmp(const char *a, const char *b) {
	char ba[20], bb[20];
	int na, nb;
	bool wa, wb;
	if (!bk_name_parse(a, ba, &na, &wa) || !bk_name_parse(b, bb, &nb, &wb)) return strcmp(a, b);
	int c = strcmp(ba, bb);
	if (c) return c;
	if (na != nb) return na < nb ? -1 : 1;
	return (int)wa - (int)wb;
}

int bk_name_next(const char *newest, const bk_tm_t *t, bool clock_set,
		bool watch, char out[BK_NAME_MAX]) {
	char now[20], nb[20];
	int nn = 0;
	bool have = newest && *newest && bk_name_parse(newest, nb, &nn, NULL);
	const char *w = watch ? "w" : "";
	if (clock_set) {
		snprintf(now, sizeof now, "%04u-%02u-%02u_%02u-%02u-%02u",
			(unsigned)t->year % 10000u, (unsigned)t->month % 100u, (unsigned)t->day % 100u,
			(unsigned)t->hour % 100u, (unsigned)t->min % 100u, (unsigned)t->sec % 100u);
		int c = have ? strcmp(now, nb) : 1;
		if (c > 0) { snprintf(out, BK_NAME_MAX, "%s%s", now, w); return 0; }
		if (c == 0 && nn == 0) return 1;  // the same second: wait for the next
		// the clock is behind (or the same second after a +N): count on
	}
	if (!have) { strcpy(nb, "0000-00-00_00-00-00"); nn = 0; }
	snprintf(out, BK_NAME_MAX, "%s+%04d%s", nb, (nn + 1) % 10000, w);
	return 0;
}

// -- the index --------------------------------------------------------------
//
//   zeitlos-backup 1
//   backup 2026-10-21_11-30-48
//   kind full                       (or watch)
//   time 2026-10-21 11:30:48        (local; "-" when the clock was not set)
//   clock set                       (or: clock not set, 3h05m after power-on)
//   dates utc                       (the file dates below are FAT's, UTC)
//   from /home /data
//   skip /data/ask *.tmp
//   copied 3 812345
//   f <crc32> <size> <date> <where> <path>    a copy, in backup <where>
//   s <crc32> <size> <date> - <path>          installed by update, unchanged
//   end <entries> <crc32 of every byte before this line>
//
// Entries are sorted by path, bytewise. Keys a reader does not know are
// skipped, so the format can grow.

typedef struct {
	char *path;
	uint32_t crc, size;
	uint16_t fdate, ftime;
	int16_t where;      // into the run's name table; -1 = shipped ("-")
} ent_t;

typedef struct {
	ent_t *e;
	int n, cap;
	bool watch;
	bool clock_set;
	char time[24];
	uint32_t copied;
	uint64_t bytes;
} idx_t;

// Every backup's name, oldest first: the run's name table.
static char (*names)[BK_NAME_MAX];
static int nnames, cap_names;

static int name_id(const char *s) {
	for (int i = 0; i < nnames; i++) if (!strcmp(names[i], s)) return i;
	if (nnames == cap_names) {
		int c = cap_names ? cap_names * 2 : 32;
		void *p = realloc(names, (size_t)c * BK_NAME_MAX);
		if (!p) return -2;
		names = p; cap_names = c;
	}
	snprintf(names[nnames], BK_NAME_MAX, "%s", s);
	return nnames++;
}

static void idx_free(idx_t *x) {
	for (int i = 0; i < x->n; i++) free(x->e[i].path);
	free(x->e);
	memset(x, 0, sizeof *x);
}

static bool idx_push(idx_t *x, const ent_t *e) {
	if (x->n == x->cap) {
		int c = x->cap ? x->cap * 2 : 256;
		void *p = realloc(x->e, (size_t)c * sizeof *x->e);
		if (!p) return false;
		x->e = p; x->cap = c;
	}
	x->e[x->n++] = *e;
	return true;
}

static int ent_cmp(const void *a, const void *b) {
	return strcmp(((const ent_t *)a)->path, ((const ent_t *)b)->path);
}

static ent_t *idx_find(const idx_t *x, const char *path) {
	int lo = 0, hi = x->n - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		int c = strcmp(path, x->e[mid].path);
		if (!c) return &x->e[mid];
		if (c < 0) hi = mid - 1; else lo = mid + 1;
	}
	return NULL;
}

// Reads a whole file. NULL if it cannot (or is larger than max).
static char *read_all(const char *path, uint32_t max, uint32_t *len) {
	bk_stat_t st;
	if (IO->stat(IO->ctx, path, &st) != BK_OK || st.dir || st.size > max) return NULL;
	int h = IO->open_read(IO->ctx, path);
	if (h < 0) return NULL;
	char *b = malloc(st.size + 1);
	uint32_t got = 0;
	if (b) {
		while (got < st.size) {
			int n = IO->read(IO->ctx, h, b + got,
				(int)(st.size - got > BK_CHUNK ? BK_CHUNK : st.size - got));
			if (n <= 0 || stopped()) break;
			got += (uint32_t)n;
		}
	}
	IO->close(IO->ctx, h);
	if (!b || got != st.size) { free(b); return NULL; }
	b[got] = 0;
	if (len) *len = got;
	return b;
}

#define INDEX_MAX (16u * 1024 * 1024)

// Index state of a backup folder: 1 complete (end line and CRC right),
// 0 none or cut short (no end line), -1 damaged (an end line, but the
// CRC or the count is wrong).
// Loads it into x (sorted) when x is not NULL and it is complete.
static int idx_load(const char *root, const char *bname, idx_t *x) {
	char path[BK_PATH_MAX];
	uint32_t len;
	if (!pjoin(path, "%s/%s/" INDEX_NAME, root, bname)) return 0;
	char *t = read_all(path, INDEX_MAX, &len);
	if (!t) return 0;
	// the end line
	if (len < 2 || t[len - 1] != '\n') { free(t); return 0; }
	uint32_t ls = len - 1;
	while (ls > 0 && t[ls - 1] != '\n') ls--;
	if (strncmp(t + ls, "end ", 4)) { free(t); return 0; }
	uint64_t cnt, crc;
	const char *q = t + ls + 4;
	t[len - 1] = 0;
	if (!get_u(&q, &cnt, 10) || !get_u(&q, &crc, 16) || *q) { free(t); return -1; }
	uint32_t c32 = 0;
	for (uint32_t o = 0; o < ls; o += 16384) {
		c32 = bk_crc32(c32, t + o, ls - o > 16384 ? 16384 : ls - o);
		breathe();
	}
	if (c32 != (uint32_t)crc) { free(t); return -1; }
	if (!x) {
		// count only
		unsigned n = 0;
		for (uint32_t i = 0; i < ls; i++) {
			if ((i == 0 || t[i - 1] == '\n') && (t[i] == 'f' || t[i] == 's') && t[i + 1] == ' ') n++;
			if (!(i & 0x3FFF)) breathe();
		}
		free(t);
		return n == cnt ? 1 : -1;
	}
	memset(x, 0, sizeof *x);
	x->clock_set = true;
	unsigned n = 0;
	char *line = t;
	t[ls] = 0;
	bool ok = true;
	while (*line && ok) {
		char *nl = strchr(line, '\n');
		if (nl) *nl = 0;
		if ((line[0] == 'f' || line[0] == 's') && line[1] == ' ') {
			ent_t e;
			char date[24], where[BK_NAME_MAX + 2];
			uint64_t c, sz;
			const char *q = line + 2;
			memset(&e, 0, sizeof e);
			if (!get_u(&q, &c, 16) || !get_u(&q, &sz, 10) || !get_w(&q, date, sizeof date) ||
				!get_w(&q, where, sizeof where) || *q != '/' || !fat_parse(date, &e.fdate, &e.ftime) ||
				c > 0xFFFFFFFFu || sz > 0xFFFFFFFFu) { ok = false; break; }
			e.crc = (uint32_t)c; e.size = (uint32_t)sz;
			if (line[0] == 's') e.where = -1;
			else {
				int id = name_id(where);
				if (id < 0) { ok = false; break; }
				e.where = (int16_t)id;
			}
			e.path = xstrdup(q);
			if (!e.path || !idx_push(x, &e)) { free(e.path); ok = false; break; }
			if (!(++n & 255)) breathe();
		} else if (!strncmp(line, "kind ", 5)) {
			x->watch = !strcmp(line + 5, "watch");
		} else if (!strncmp(line, "clock ", 6)) {
			x->clock_set = !strcmp(line + 6, "set");
		} else if (!strncmp(line, "time ", 5)) {
			snprintf(x->time, sizeof x->time, "%s", strcmp(line + 5, "-") ? line + 5 : "");
		} else if (!strncmp(line, "copied ", 7)) {
			uint64_t c = 0, b = 0;
			const char *q = line + 7;
			if (get_u(&q, &c, 10)) get_u(&q, &b, 10);
			x->copied = (uint32_t)c; x->bytes = b;
		}
		if (!nl) break;
		line = nl + 1;
	}
	free(t);
	if (!ok || n != cnt) { idx_free(x); return -1; }
	qsort(x->e, (size_t)x->n, sizeof *x->e, ent_cmp);
	return 1;
}

// The cheap look at an index: its last line only. 1 if it ends with
// a well-formed "end <entries> <crc>" (*count gets the entries), else 0
// -- no index, or one cut short. Whether the CRC is right is only known
// from reading it all (idx_load): `check` does, and so does anything
// that uses an index. Reading every index whole to list the backups
// would be ~1 s a backup at a stick's speed.
static int idx_tail(const char *root, const char *bname, uint32_t *count) {
	char path[BK_PATH_MAX], b[72];
	bk_stat_t st;
	if (count) *count = 0;
	if (!pjoin(path, "%s/%s/" INDEX_NAME, root, bname)) return 0;
	if (IO->stat(IO->ctx, path, &st) != BK_OK || st.dir || st.size < 8) return 0;
	int h = IO->open_read(IO->ctx, path);
	if (h < 0) return 0;
	uint32_t off = st.size > sizeof b - 1 ? st.size - (uint32_t)(sizeof b - 1) : 0;
	int n = 0;
	if (IO->seek(IO->ctx, h, off) == BK_OK) {
		int k;
		while (n < (int)sizeof b - 1 && (k = IO->read(IO->ctx, h, b + n, (int)sizeof b - 1 - n)) > 0) n += k;
	}
	IO->close(IO->ctx, h);
	if (n < 2 || b[n - 1] != '\n') return 0;
	b[n - 1] = 0;
	char *l = strrchr(b, '\n');
	l = l ? l + 1 : (off == 0 ? b : NULL);
	if (!l || strncmp(l, "end ", 4)) return 0;
	const char *q = l + 4;
	uint64_t c, crc;
	if (!get_u(&q, &c, 10) || !get_u(&q, &crc, 16) || *q) return 0;
	if (count) *count = (uint32_t)c;
	return 1;
}

// The head of an index: what bk_list shows (kind, copied, bytes).
static void idx_head(const char *root, const char *bname, uint32_t *copied, uint64_t *bytes) {
	char path[BK_PATH_MAX], b[600];
	*copied = 0; *bytes = 0;
	if (!pjoin(path, "%s/%s/" INDEX_NAME, root, bname)) return;
	int h = IO->open_read(IO->ctx, path);
	if (h < 0) return;
	int n = 0, k;
	while (n < (int)sizeof b - 1 && (k = IO->read(IO->ctx, h, b + n, (int)sizeof b - 1 - n)) > 0) n += k;
	IO->close(IO->ctx, h);
	b[n] = 0;
	const char *c = strstr(b, "\ncopied ");
	if (!c) return;
	const char *q = c + 8;
	uint64_t a = 0, y = 0;
	char *nl = strchr(q, '\n');
	if (nl) *nl = 0;
	if (get_u(&q, &a, 10)) get_u(&q, &y, 10);
	*copied = (uint32_t)a; *bytes = y;
}

// x gets y's entries, replacing any of the same path (a watch backup
// laid over the state before it).
static bool idx_overlay(idx_t *x, idx_t *y) {
	int n0 = x->n;
	for (int i = 0; i < y->n; i++) {
		ent_t *e = NULL;
		int lo = 0, hi = n0 - 1;
		while (lo <= hi) {
			int mid = (lo + hi) / 2, c = strcmp(y->e[i].path, x->e[mid].path);
			if (!c) { e = &x->e[mid]; break; }
			if (c < 0) hi = mid - 1; else lo = mid + 1;
		}
		if (e) {
			free(e->path);
			*e = y->e[i];
		} else if (!idx_push(x, &y->e[i])) return false;
		y->e[i].path = NULL;
	}
	y->n = 0;
	qsort(x->e, (size_t)x->n, sizeof *x->e, ent_cmp);
	return true;
}

// -- the drive --------------------------------------------------------------

static char root[BK_PATH_MAX];      // <dest>/<name>

typedef struct {
	int id;             // into names
	bool watch;
	int state;          // idx_load(..., NULL): 1, 0, -1
} bk_t;

static bk_t *bks;
static int nbks;

static int list_name_fn(void *arg, const char *name, const bk_stat_t *st) {
	(void)arg;
	if (!st->dir || !bk_name_parse(name, NULL, NULL, NULL)) return 0;
	return name_id(name) < 0 ? -1 : 0;
}

static int bk_cmp_id(const void *a, const void *b) {
	return bk_name_cmp(names[((const bk_t *)a)->id], names[((const bk_t *)b)->id]);
}

static void reset_run(void) {
	free(names); names = NULL; nnames = cap_names = 0;
	free(bks); bks = NULL; nbks = 0;
}

// The backups on the drive, oldest first, with their states.
static int scan_backups(bool states) {
	free(bks); bks = NULL; nbks = 0;
	int r = IO->list(IO->ctx, root, list_name_fn, NULL);
	if (r == BK_ENOENT) return 0;
	if (r < 0) return -1;
	int n = nnames;     // idx_load below may add names of its own
	bks = calloc((size_t)n + 1, sizeof *bks);
	if (!bks) return -1;
	for (int i = 0; i < n; i++) {
		char p[BK_PATH_MAX];
		bk_stat_t st;
		if (!pjoin(p, "%s/%s", root, names[i]) || IO->stat(IO->ctx, p, &st) != BK_OK || !st.dir)
			continue;
		bk_t *b = &bks[nbks++];
		b->id = i;
		bk_name_parse(names[i], NULL, NULL, &b->watch);
		b->state = states ? idx_tail(root, names[i], NULL) : 1;
	}
	qsort(bks, (size_t)nbks, sizeof *bks, bk_cmp_id);
	return nbks;
}

static int find_bk(const char *name) {
	for (int i = 0; i < nbks; i++) if (!strcmp(names[bks[i].id], name)) return i;
	return -1;
}

// The state at backup k: the newest complete full backup at or before
// it, with every complete watch backup after that up to k laid over.
// A complete-looking index that turns out damaged when read (its CRC)
// is marked so (-1), said once, and passed over: the full backup before
// it is the base instead, and its own copies are still found through
// the indexes after it.
static int effective(int k, idx_t *x) {
	memset(x, 0, sizeof *x);
	x->clock_set = true;
	int f = -1;
	for (int i = k; i >= 0; i--) {
		if (bks[i].state != 1 || bks[i].watch) continue;
		int r = idx_load(root, names[bks[i].id], x);
		if (r == 1) { f = i; break; }
		if (stopped()) return -2;       // not read: stopping, not damaged
		bks[i].state = -1;
		say("backup: the index of %s is damaged; `backup check` says more\n", names[bks[i].id]);
		memset(x, 0, sizeof *x);
		x->clock_set = true;
	}
	for (int i = f + 1; i <= k; i++) {
		if (bks[i].state != 1 || !bks[i].watch) continue;
		idx_t y;
		if (idx_load(root, names[bks[i].id], &y) != 1) {
			if (stopped()) { idx_free(x); return -2; }
			bks[i].state = -1;
			say("backup: the index of %s is damaged; `backup check` says more\n", names[bks[i].id]);
			continue;
		}
		bool ok = idx_overlay(x, &y);
		idx_free(&y);
		if (!ok) return -1;
	}
	return 0;
}

// Removes a folder and everything in it (unlink only).
static int rm_tree(const char *path);

// Does a complete backup after k name a copy in k?
static bool referenced(int k) {
	const char *name = names[bks[k].id];
	for (int i = k + 1; i < nbks; i++) {
		if (bks[i].state != 1) continue;
		idx_t y;
		if (idx_load(root, names[bks[i].id], &y) != 1) {
			// not read because we are stopping: it may well name k
			if (stopped()) return true;
			continue;
		}
		bool hit = false;
		for (int j = 0; j < y.n && !hit; j++)
			if (y.e[j].where >= 0 && !strcmp(names[y.e[j].where], name)) hit = true;
		idx_free(&y);
		if (hit) return true;
	}
	return false;
}

typedef struct { char **v; int n, cap; } strs_t;

static bool strs_push(strs_t *s, const char *x) {
	if (s->n == s->cap) {
		int c = s->cap ? s->cap * 2 : 32;
		void *p = realloc(s->v, (size_t)c * sizeof *s->v);
		if (!p) return false;
		s->v = p; s->cap = c;
	}
	if (!(s->v[s->n] = xstrdup(x))) return false;
	s->n++;
	return true;
}

static void strs_free(strs_t *s) {
	for (int i = 0; i < s->n; i++) free(s->v[i]);
	free(s->v);
	memset(s, 0, sizeof *s);
}

typedef struct { strs_t files, dirs; } ents_t;

static int ents_fn(void *arg, const char *name, const bk_stat_t *st) {
	ents_t *e = arg;
	if (stopped()) return 1;    // a listing cut short: callers stop too
	return strs_push(st->dir ? &e->dirs : &e->files, name) ? 0 : -1;
}

static int rm_tree(const char *path) {
	ents_t e;
	memset(&e, 0, sizeof e);
	int r = IO->list(IO->ctx, path, ents_fn, &e);
	if (r == BK_OK) {
		for (int i = 0; i < e.files.n; i++) {
			char p[BK_PATH_MAX];
			if (pjoin(p, "%s/%s", path, e.files.v[i])) IO->unlink(IO->ctx, p);
		}
		for (int i = 0; i < e.dirs.n; i++) {
			char p[BK_PATH_MAX];
			if (pjoin(p, "%s/%s", path, e.dirs.v[i])) rm_tree(p);
		}
	}
	strs_free(&e.files); strs_free(&e.dirs);
	return IO->unlink(IO->ctx, path);
}

// mkdir -p, for the folders above a file.
static int mkdirs_for(const char *file) {
	char p[BK_PATH_MAX];
	snprintf(p, sizeof p, "%s", file);
	for (char *s = p + 1; *s; s++) {
		if (*s != '/') continue;
		*s = 0;
		int r = IO->mkdir(IO->ctx, p);
		*s = '/';
		if (r != BK_OK && r != BK_EEXIST) return r;
	}
	return BK_OK;
}

// -- the lock ---------------------------------------------------------------

static bool locked;

static int lock_take(void) {
	char p[BK_PATH_MAX], own[96];
	if (!pjoin(p, "%s/" LOCK_NAME, root)) return BK_ERR;
	uint32_t len;
	char *t = read_all(p, 1024, &len);
	if (t) {
		char *nl = strchr(t, '\n');
		if (nl) *nl = 0;
		bool alive = IO->owner_alive && IO->owner_alive(IO->ctx, t);
		if (alive) {
			say("backup: another backup is running (%s); try again when it is done\n", t);
			free(t);
			return BK_EBUSY;
		}
		free(t);
		IO->unlink(IO->ctx, p);
	}
	own[0] = 0;
	if (IO->owner) IO->owner(IO->ctx, own, sizeof own);
	int h = IO->open_write(IO->ctx, p);
	if (h < 0) return h;
	char line[110];
	int n = snprintf(line, sizeof line, "%s\n", own);
	int w = IO->write(IO->ctx, h, line, n);
	IO->sync(IO->ctx, h);
	IO->close(IO->ctx, h);
	if (w != n) { IO->unlink(IO->ctx, p); return BK_ERR; }
	locked = true;
	return BK_OK;
}

static void lock_drop(void) {
	char p[BK_PATH_MAX];
	if (!locked) return;
	if (pjoin(p, "%s/" LOCK_NAME, root)) IO->unlink(IO->ctx, p);
	locked = false;
}

// -- copying ----------------------------------------------------------------

static uint8_t buf[BK_CHUNK];

// CRC of a whole file; false if it cannot be read (or is not size bytes).
static bool file_crc(const char *path, uint32_t size, uint32_t *crc) {
	int h = IO->open_read(IO->ctx, path);
	if (h < 0) return false;
	uint32_t c = 0, got = 0;
	int n;
	while ((n = IO->read(IO->ctx, h, buf, BK_CHUNK)) > 0) {
		c = bk_crc32(c, buf, (uint32_t)n);
		got += (uint32_t)n;
		if (stopped()) break;
	}
	IO->close(IO->ctx, h);
	*crc = c;
	return n == 0 && got == size;
}

#define CP_OK       0
#define CP_SRC     -1   // the source could not be read
#define CP_DST     -2   // the destination could not be written
#define CP_NOSPC   -3
#define CP_STOP    -4
#define CP_VERIFY  -5   // read back wrong

static uint64_t copy_done;   // bytes copied in this run, for progress
static uint64_t copy_total;
static uint32_t copy_files_done, copy_files_total;

// src -> dst, a chunk at a time; *crc is the source's.
static int copy_file(const char *src, const char *dst, uint32_t *crc, uint32_t *size) {
	int hs = IO->open_read(IO->ctx, src);
	if (hs < 0) return CP_SRC;
	int hd = IO->open_write(IO->ctx, dst);
	if (hd < 0) { IO->close(IO->ctx, hs); return hd == BK_ENOSPC ? CP_NOSPC : CP_DST; }
	uint32_t c = 0, got = 0;
	int n, rc = CP_OK;
	while ((n = IO->read(IO->ctx, hs, buf, BK_CHUNK)) > 0) {
		c = bk_crc32(c, buf, (uint32_t)n);
		int w = IO->write(IO->ctx, hd, buf, n);
		if (w != n) { rc = (w == BK_ENOSPC || (w >= 0 && w < n)) ? CP_NOSPC : CP_DST; break; }
		got += (uint32_t)n;
		copy_done += (uint64_t)n;
		progress(BK_PH_COPY, src, copy_files_done, copy_files_total, copy_done, copy_total);
		if (stopped()) { rc = CP_STOP; break; }
	}
	if (rc == CP_OK && n < 0) rc = CP_SRC;
	if (rc == CP_OK && IO->sync(IO->ctx, hd) != BK_OK) rc = CP_DST;
	if (IO->close(IO->ctx, hd) != BK_OK && rc == CP_OK) rc = CP_DST;
	IO->close(IO->ctx, hs);
	*crc = c; *size = got;
	if (rc != CP_OK) return rc;
	if (OPT->verify) {
		uint32_t v;
		if (!file_crc(dst, got, &v) || v != c) return stopped() ? CP_STOP : CP_VERIFY;
	}
	return CP_OK;
}

// -- the manifest (files update put there) ----------------------------------

typedef struct { char *path; uint32_t crc, size; } man_t;
static man_t *man;
static int nman;

static int man_cmp(const void *a, const void *b) {
	return strcmp(((const man_t *)a)->path, ((const man_t *)b)->path);
}

static void man_free(void) {
	for (int i = 0; i < nman; i++) free(man[i].path);
	free(man); man = NULL; nman = 0;
}

// /sys/manifest's f lines, formats 2-4 (format 1 has no CRC: unused).
static void man_load(const char *path) {
	uint32_t len;
	man_free();
	char *t = read_all(path, 4u * 1024 * 1024, &len);
	if (!t) return;
	int fmt = 0, cap = 0;
	char *line = t;
	while (*line) {
		char *nl = strchr(line, '\n');
		if (nl) *nl = 0;
		if (!strncmp(line, "zeitlos-manifest ", 17)) fmt = atoi(line + 17);
		else if (fmt >= 2 && !strncmp(line, "f ", 2)) {
			char sha[80], pol[8], set[32];
			uint64_t crc = 0, sz = 0;
			const char *q = line + 2;
			bool k = get_w(&q, sha, sizeof sha) && get_u(&q, &crc, 16) && get_u(&q, &sz, 10) &&
				get_w(&q, pol, sizeof pol) && (fmt < 4 || get_w(&q, set, sizeof set));
			if (k && *q) {
				if (nman == cap) {
					cap = cap ? cap * 2 : 256;
					void *p = realloc(man, (size_t)cap * sizeof *man);
					if (!p) break;
					man = p;
				}
				size_t pl = strlen(q);
				char *pp = malloc(pl + 2);
				if (!pp) break;
				pp[0] = '/';
				memcpy(pp + 1, q, pl + 1);
				man[nman].path = pp;
				man[nman].crc = (uint32_t)crc;
				man[nman].size = (uint32_t)sz;
				if (!(++nman & 255)) breathe();
			}
		}
		if (!nl) break;
		line = nl + 1;
	}
	free(t);
	qsort(man, (size_t)nman, sizeof *man, man_cmp);
}

static man_t *man_find(const char *path) {
	int lo = 0, hi = nman - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2, c = strcmp(path, man[mid].path);
		if (!c) return &man[mid];
		if (c < 0) hi = mid - 1; else lo = mid + 1;
	}
	return NULL;
}

// -- looking at the card ----------------------------------------------------

typedef struct {
	char *path;
	bk_stat_t st;
} card_t;

static card_t *card;
static int ncard, cap_card;

static void card_free(void) {
	for (int i = 0; i < ncard; i++) free(card[i].path);
	free(card); card = NULL; ncard = cap_card = 0;
}

static bool card_push(const char *path, const bk_stat_t *st) {
	if (ncard == cap_card) {
		int c = cap_card ? cap_card * 2 : 256;
		void *p = realloc(card, (size_t)c * sizeof *card);
		if (!p) return false;
		card = p; cap_card = c;
	}
	if (!(card[ncard].path = xstrdup(path))) return false;
	card[ncard].st = *st;
	ncard++;
	return true;
}

static int card_cmp(const void *a, const void *b) {
	return strcmp(((const card_t *)a)->path, ((const card_t *)b)->path);
}

static const char *const never[] = { "/tmp", "/ram", "/usb", "/nfs" };

static bool skipped_path(const char *path, bool dir) {
	const char *name = base_name(path);
	if (!dir && ends_ci(name, ".part")) return true;
	for (unsigned i = 0; i < sizeof never / sizeof *never; i++)
		if (inside(path, never[i])) return true;
	if (OPT->dest && inside(path, OPT->dest)) return true;
	for (int i = 0; i < OPT->nskip; i++) {
		const char *s = OPT->skip[i];
		if (!s || !*s) continue;
		if (s[0] == '/') { if (inside(path, s)) return true; }
		else if (bk_glob(s, name)) return true;
	}
	return false;
}

static uint32_t scan_skipped;

static int scan_tree(const char *dir) {
	strs_t stack;
	memset(&stack, 0, sizeof stack);
	if (!strs_push(&stack, dir)) return -1;
	while (stack.n) {
		char *d = stack.v[--stack.n];
		ents_t e;
		memset(&e, 0, sizeof e);
		int r = IO->list(IO->ctx, d, ents_fn, &e);
		if (r < 0 && r != BK_ENOENT) {
			say("backup: cannot read %s; left out\n", d);
			scan_skipped++;
		}
		for (int i = 0; i < e.files.n; i++) {
			char p[BK_PATH_MAX];
			bk_stat_t st;
			if (!pjoin(p, "%s%s%s", d, strcmp(d, "/") ? "/" : "", e.files.v[i])) {
				say("backup: path too long, left out: %s/%s\n", d, e.files.v[i]);
				scan_skipped++;
				continue;
			}
			if (skipped_path(p, false)) continue;
			if (stopped()) break;
			if (IO->stat(IO->ctx, p, &st) != BK_OK || st.dir) continue;
			if (!card_push(p, &st)) { strs_free(&e.files); strs_free(&e.dirs); free(d); strs_free(&stack); return -1; }
		}
		for (int i = 0; i < e.dirs.n; i++) {
			char p[BK_PATH_MAX];
			if (!pjoin(p, "%s%s%s", d, strcmp(d, "/") ? "/" : "", e.dirs.v[i])) {
				say("backup: path too long, left out: %s/%s\n", d, e.dirs.v[i]);
				scan_skipped++;
				continue;
			}
			if (skipped_path(p, true)) continue;
			if (!strs_push(&stack, p)) { strs_free(&e.files); strs_free(&e.dirs); free(d); strs_free(&stack); return -1; }
		}
		strs_free(&e.files); strs_free(&e.dirs);
		free(d);
		if (stopped()) break;
	}
	strs_free(&stack);
	return 0;
}

// In path order; the same file named twice (overlapping sources, or a
// watch's file and its folder) once.
static void card_sort(void) {
	if (!ncard) return;
	qsort(card, (size_t)ncard, sizeof *card, card_cmp);
	int w = 0;
	for (int i = 0; i < ncard; i++) {
		if (w && !strcmp(card[w - 1].path, card[i].path)) { free(card[i].path); continue; }
		card[w++] = card[i];
	}
	ncard = w;
}

static int scan_card(void) {
	card_free();
	scan_skipped = 0;
	for (int i = 0; i < OPT->nfrom; i++) {
		const char *f = OPT->from[i];
		bk_stat_t st;
		if (!f || f[0] != '/') continue;
		if (IO->stat(IO->ctx, f, &st) != BK_OK) {
			if (!OPT->quiet_missing) say("backup: %s is not there; nothing of it backed up\n", f);
			continue;
		}
		if (!st.dir) {
			if (!skipped_path(f, false) && !card_push(f, &st)) return -1;
		} else if (!skipped_path(f, true) && scan_tree(f) < 0) return -1;
		if (stopped()) return 0;
	}
	card_sort();
	return 0;
}

// -- making room ------------------------------------------------------------

// The size a file takes on the drive, rounded up: 4 KB more than its
// bytes covers any cluster up to 4 KB and a directory entry; bigger
// clusters are covered by the reserve and the free space read after.
static uint32_t kb_of(uint32_t size) { return size / 1024 + 4; }

static bool bk_protected(int k) {
	// the newest keep full backups, and everything after the oldest of them
	int fulls = 0;
	if (OPT->keep <= 0) return false;
	for (int i = nbks - 1; i >= 0; i--) {
		if (bks[i].state == 1 && !bks[i].watch) fulls++;
		if (fulls >= OPT->keep) return k >= i;
	}
	return true;  // fewer than keep full backups: all are kept
}

static bool bk_pinned(int k) {
	char p[BK_PATH_MAX];
	bk_stat_t st;
	return pjoin(p, "%s/%s/" PIN_NAME, root, names[bks[k].id]) &&
		IO->stat(IO->ctx, p, &st) == BK_OK;
}

// Every copy in folder dir (relative paths "/home/book.txt"), recursively.
static int list_copies(const char *bdir, strs_t *out) {
	strs_t stack;
	memset(&stack, 0, sizeof stack);
	if (!strs_push(&stack, "")) return -1;
	while (stack.n) {
		char *rel = stack.v[--stack.n];
		char d[BK_PATH_MAX];
		ents_t e;
		memset(&e, 0, sizeof e);
		if (pjoin(d, "%s%s", bdir, rel)) IO->list(IO->ctx, d, ents_fn, &e);
		for (int i = 0; i < e.files.n; i++) {
			char r[BK_PATH_MAX];
			if (!*rel && (ci_eq(e.files.v[i], INDEX_NAME) || ci_eq(e.files.v[i], PIN_NAME))) continue;
			if (pjoin(r, "%s/%s", rel, e.files.v[i])) strs_push(out, r);
		}
		for (int i = 0; i < e.dirs.n; i++) {
			char r[BK_PATH_MAX];
			if (pjoin(r, "%s/%s", rel, e.dirs.v[i])) strs_push(&stack, r);
		}
		strs_free(&e.files); strs_free(&e.dirs);
		free(rel);
	}
	strs_free(&stack);
	return 0;
}

// Empty folders under (and including) bdir removed, bottom up; true if
// bdir itself was removed.
static bool rm_empty(const char *bdir) {
	ents_t e;
	memset(&e, 0, sizeof e);
	if (IO->list(IO->ctx, bdir, ents_fn, &e) != BK_OK) return false;
	for (int i = 0; i < e.dirs.n; i++) {
		char p[BK_PATH_MAX];
		if (pjoin(p, "%s/%s", bdir, e.dirs.v[i])) rm_empty(p);
	}
	strs_free(&e.files); strs_free(&e.dirs);
	memset(&e, 0, sizeof e);
	if (IO->list(IO->ctx, bdir, ents_fn, &e) != BK_OK) return false;
	bool empty = e.files.n == 0 && e.dirs.n == 0;
	strs_free(&e.files); strs_free(&e.dirs);
	return empty && IO->unlink(IO->ctx, bdir) == BK_OK;
}

// Does a backup after k have a copy of rel?
static bool later_copy(int k, const char *rel) {
	for (int i = k + 1; i < nbks; i++) {
		char p[BK_PATH_MAX];
		bk_stat_t st;
		if (pjoin(p, "%s/%s%s", root, names[bks[i].id], rel) && IO->stat(IO->ctx, p, &st) == BK_OK)
			return true;
	}
	return false;
}

// Frees want_kb, oldest copies first. newest: the state now (whose copies
// are never removed in the first pass). Returns the KB freed.
static uint32_t room(uint32_t want_kb, const idx_t *newest, bool dry, bk_result_t *r) {
	uint32_t freed = 0;
	for (int pass = 0; pass < 2 && freed < want_kb; pass++) {
		for (int k = 0; k < nbks && freed < want_kb; k++) {
			if (bks[k].state != 1 || bk_protected(k) || bk_pinned(k)) continue;
			char bdir[BK_PATH_MAX];
			if (!pjoin(bdir, "%s/%s", root, names[bks[k].id])) continue;
			strs_t c;
			memset(&c, 0, sizeof c);
			list_copies(bdir, &c);
			int removed_here = 0;
			for (int i = 0; i < c.n && freed < want_kb; i++) {
				const ent_t *e = newest ? idx_find(newest, c.v[i]) : NULL;
				bool newer;
				if (pass == 0) {
					// a copy goes only when a newer copy of it exists
					if (e) newer = e->where >= 0 && strcmp(names[e->where], names[bks[k].id]) &&
						bk_name_cmp(names[e->where], names[bks[k].id]) > 0;
					else newer = later_copy(k, c.v[i]);
					if (!newer) continue;
				} else {
					// the last copies of files no longer on the card (or
					// that update put back as it ships them)
					if (e && e->where >= 0) continue;
					if (later_copy(k, c.v[i])) continue;
				}
				char p[BK_PATH_MAX];
				bk_stat_t st;
				if (!pjoin(p, "%s%s", bdir, c.v[i]) || IO->stat(IO->ctx, p, &st) != BK_OK) continue;
				if (!dry && IO->unlink(IO->ctx, p) != BK_OK) continue;
				freed += kb_of(st.size);
				r->removed++;
				r->removed_bytes += st.size;
				removed_here++;
				progress(BK_PH_ROOM, p, 0, 0, (uint64_t)freed * 1024, (uint64_t)want_kb * 1024);
			}
			// a folder left with no copies: its index describes nothing
			// that is still there, so it goes too
			if (!dry && removed_here) {
				strs_t left;
				memset(&left, 0, sizeof left);
				list_copies(bdir, &left);
				if (left.n == 0) {
					char ip[BK_PATH_MAX];
					if (pjoin(ip, "%s/" INDEX_NAME, bdir)) IO->unlink(IO->ctx, ip);
					if (rm_empty(bdir)) bks[k].state = 0;
				} else rm_empty(bdir);
				strs_free(&left);
			}
			strs_free(&c);
		}
	}
	return freed;
}

// -- writing the index ------------------------------------------------------

typedef struct {
	int h;
	uint32_t crc;
	char b[BK_CHUNK];
	int n;
	bool err;
} wr_t;

static void wr_flush(wr_t *w) {
	if (w->n && !w->err) {
		int k = IO->write(IO->ctx, w->h, w->b, w->n);
		if (k != w->n) w->err = true;
	}
	w->n = 0;
}

static void wr(wr_t *w, const char *fmt, ...) {
	char line[BK_PATH_MAX + 128];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	if (n < 0) { w->err = true; return; }
	if (n >= (int)sizeof line) n = (int)sizeof line - 1;
	w->crc = bk_crc32(w->crc, line, (uint32_t)n);
	if (w->n + n > (int)sizeof w->b) wr_flush(w);
	memcpy(w->b + w->n, line, (size_t)n);
	w->n += n;
}

static const char README[] =
	"This drive holds backups made by Zeitlos's backup app (docs/backup.md).\r\n"
	"\r\n"
	"Each folder below a computer's name is one backup, named by the time it\r\n"
	"was made: YYYY-MM-DD_HH-MM-SS. \"+0001\" after a name means it was made\r\n"
	"after that time while the computer's clock was not set; \"w\" at the end\r\n"
	"means a watch backup, of the files saved just then.\r\n"
	"\r\n"
	"A backup folder holds the files that had changed since the backup before\r\n"
	"it, under their own names. INDEX.TXT in it lists every file the computer\r\n"
	"had at that time, with the backup folder that holds its copy.\r\n"
	"\r\n"
	"To get one file back: look for it in the folders, newest first; each one\r\n"
	"that has it holds the version of that time, and the newest folder that\r\n"
	"has it holds the newest copy. Copy it back. On Zeitlos, `backup restore\r\n"
	"PATH` does it (the newest copy, put beside the file on the card).\r\n"
	"To get everything back as it was at one time: copy the backup folders up\r\n"
	"to that time over each other, oldest first. (Files deleted later come\r\n"
	"back too.) `backup restore /home` on Zeitlos, or tools/backup_read.py in\r\n"
	"the Zeitlos sources on a PC, does it exactly.\r\n"
	"\r\n"
	"A folder with no INDEX.TXT, or one whose INDEX.TXT does not end with an\r\n"
	"\"end\" line, is a backup that was cut short; it can be deleted.\r\n";

static void readme(void) {
	char p[BK_PATH_MAX];
	bk_stat_t st;
	if (!pjoin(p, "%s/" README_NAME, OPT->dest) || IO->stat(IO->ctx, p, &st) == BK_OK) return;
	int h = IO->open_write(IO->ctx, p);
	if (h < 0) return;
	IO->write(IO->ctx, h, README, (int)sizeof README - 1);
	IO->sync(IO->ctx, h);
	IO->close(IO->ctx, h);
}

// -- one run ----------------------------------------------------------------

static int setup(const bk_io_t *io, const bk_opts_t *o) {
	IO = io; OPT = o;
	reset_run();
	locked = false;
	if (!o->dest || o->dest[0] != '/' || !o->name || !*o->name || strchr(o->name, '/')) {
		say("backup: no backup drive set (apps.backup.to, apps.backup.name)\n");
		return BK_USAGE;
	}
	if (!pjoin(root, "%s/%s", o->dest, o->name)) {
		say("backup: %s/%s: path too long\n", o->dest, o->name);
		return BK_USAGE;
	}
	return BK_OK;
}

// The volume the backups go to is there (mounted). Says nothing.
static bool dest_there_quiet(void) {
	bk_stat_t st;
	char top[BK_PATH_MAX];
	snprintf(top, sizeof top, "%s", OPT->dest);
	char *s = strchr(top + 1, '/');
	if (s) *s = 0;
	return IO->stat(IO->ctx, top, &st) == BK_OK && st.dir;
}

static bool dest_there(void) {
	bk_stat_t st;
	char top[BK_PATH_MAX];
	// the volume: "/usb" of "/usb/backup"
	snprintf(top, sizeof top, "%s", OPT->dest);
	char *s = strchr(top + 1, '/');
	if (s) *s = 0;
	if (IO->stat(IO->ctx, top, &st) != BK_OK || !st.dir) {
		// fs does not mount a drive it cannot read (exFAT, NTFS), and only
		// says so on the console: name both. (Asking usb itself would make
		// this program the drive's client, and take fs's events from it.)
		say("backup: %s is not there%s\n", top, inside(top, "/usb") ?
			": is the backup drive plugged in? (One that is not FAT -- exFAT, NTFS --"
			" is not mounted; the console then says \"usb storage: mount failed\".)" : "");
		return false;
	}
	return true;
}

static void finish(void) {
	lock_drop();
	card_free();
	man_free();
	reset_run();
}

// A 64-bit number as text, without printf's 64-bit support (ZFMT_NO64).
static char *u64str(uint64_t v, char out[24]) {
	char t[24];
	int n = 0;
	do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	for (int i = 0; i < n; i++) out[i] = t[n - 1 - i];
	out[n] = 0;
	return out;
}

static void kb_str(uint64_t bytes, char *out, int n) {
	if (bytes < 10 * 1024) snprintf(out, (size_t)n, "%u bytes", (unsigned)bytes);
	else if (bytes < 10ull * 1024 * 1024) snprintf(out, (size_t)n, "%u KB", (unsigned)(bytes / 1024));
	else snprintf(out, (size_t)n, "%u MB", (unsigned)(bytes / (1024 * 1024)));
}

// Copies check files name as damaged: "<where> <path>" lines.
static strs_t damaged;

static int check_fn(void *arg, const char *name, const bk_stat_t *st) {
	strs_t *s = arg;
	if (st->dir || !ends_ci(name, ".txt")) return 0;
	return strs_push(s, name) ? 0 : -1;
}

static void damaged_load(void) {
	char d[BK_PATH_MAX];
	strs_t files;
	memset(&files, 0, sizeof files);
	strs_free(&damaged);
	if (!pjoin(d, "%s/" CHECK_DIR, root)) return;
	IO->list(IO->ctx, d, check_fn, &files);
	for (int i = 0; i < files.n; i++) {
		char p[BK_PATH_MAX];
		uint32_t len;
		if (!pjoin(p, "%s/%s", d, files.v[i])) continue;
		char *t = read_all(p, 1024 * 1024, &len);
		if (!t) continue;
		for (char *line = t; *line;) {
			char *nl = strchr(line, '\n');
			if (nl) *nl = 0;
			if (!strncmp(line, "bad ", 4)) strs_push(&damaged, line + 4);
			if (!nl) break;
			line = nl + 1;
		}
		free(t);
	}
	strs_free(&files);
}

static bool is_damaged(const char *where, const char *path) {
	size_t wl = strlen(where);
	for (int i = 0; i < damaged.n; i++)
		if (!strncmp(damaged.v[i], where, wl) && damaged.v[i][wl] == ' ' &&
			!strcmp(damaged.v[i] + wl + 1, path)) return true;
	return false;
}

// How many full backups are newer than name.
static int fulls_after(const char *name) {
	int n = 0;
	for (int i = nbks - 1; i >= 0; i--) {
		if (bk_name_cmp(names[bks[i].id], name) <= 0) break;
		if (bks[i].state == 1 && !bks[i].watch) n++;
	}
	return n;
}

typedef struct {
	int ci;             // into card
	uint8_t why;        // 1 new, 2 changed, 3 refresh, 4 damaged copy
} todo_t;

int bk_backup(const bk_io_t *io, const bk_opts_t *o, bk_result_t *r) {
	memset(r, 0, sizeof *r);
	int rc = setup(io, o);
	if (rc) return rc;
	bool watch = o->nonly > 0;
	if (!dest_there()) { finish(); return BK_FAILED; }

	if (!o->dry_run) {
		char lp[BK_PATH_MAX];
		int m = pjoin(lp, "%s/" LOCK_NAME, root) ? mkdirs_for(lp) : BK_ERR;
		if (m != BK_OK && m != BK_EEXIST) {
			say("backup: cannot make %s\n", root);
			finish();
			return BK_FAILED;
		}
		readme();
		rc = lock_take();
		if (rc) { finish(); return BK_FAILED; }
	}

	if (scan_backups(true) < 0) { say("backup: out of memory\n"); finish(); return BK_FAILED; }
	if (stopped()) { say("backup: stopped; nothing was written\n"); finish(); return BK_FAILED; }

	// 2. what a run cut short left. A folder with no complete index that
	// a later index still names is not that: its index was damaged, and
	// its copies are needed. It is left, and reported.
	if (!o->dry_run) {
		for (int i = 0; i < nbks; i++) {
			if (bks[i].state != 0) continue;
			if (referenced(i)) {
				if (stopped()) break;
				say("backup: the index of %s is damaged (later backups use its copies); kept\n", names[bks[i].id]);
				bks[i].state = -1;
				continue;
			}
			if (stopped()) break;
			char p[BK_PATH_MAX];
			if (!pjoin(p, "%s/%s", root, names[bks[i].id])) continue;
			say("backup: removing %s, a backup that was cut short\n", names[bks[i].id]);
			rm_tree(p);
		}
		scan_backups(true);
	}

	// 3. the state now
	idx_t prev;
	int er = nbks ? effective(nbks - 1, &prev) : (memset(&prev, 0, sizeof prev), 0);
	if (er) {
		say(er == -2 ? "backup: stopped; nothing was written\n" : "backup: out of memory reading the backups\n");
		finish();
		return BK_FAILED;
	}
	damaged_load();
	if (o->manifest && !stopped()) man_load(o->manifest);
	if (stopped()) { say("backup: stopped; nothing was written\n"); idx_free(&prev); finish(); return BK_FAILED; }

	// 4. the card
	if (watch) {
		// what was saved: files, and a folder when fs could not say
		// which of its files (more changed than it keeps, zfsproto.h)
		card_free();
		bool oom = false;
		for (int i = 0; i < o->nonly && !oom; i++) {
			bk_stat_t st;
			const char *p = o->only[i];
			if (!p || p[0] != '/') continue;
			if (IO->stat(IO->ctx, p, &st) != BK_OK) continue;  // deleted again
			if (st.dir) { if (!skipped_path(p, true) && scan_tree(p) < 0) oom = true; }
			else if (!skipped_path(p, false) && !card_push(p, &st)) oom = true;
			if (stopped()) break;
		}
		if (oom) { say("backup: out of memory looking at the card\n"); idx_free(&prev); finish(); return BK_FAILED; }
		card_sort();
		scan_skipped = 0;
	} else {
		progress(BK_PH_SCAN, NULL, 0, 0, 0, 0);
		if (scan_card() < 0) { say("backup: out of memory looking at the card\n"); idx_free(&prev); finish(); return BK_FAILED; }
	}
	r->skipped = scan_skipped;
	if (stopped()) { say("backup: stopped; nothing was written\n"); idx_free(&prev); finish(); return BK_FAILED; }

	// compare
	todo_t *todo = calloc((size_t)ncard + 1, sizeof *todo);
	uint32_t *crc_known = calloc((size_t)ncard + 1, sizeof *crc_known);
	uint8_t *state = calloc((size_t)ncard + 1, 1);   // 0 copy, 1 unchanged, 2 shipped
	int16_t *where = calloc((size_t)ncard + 1, sizeof *where);
	if (!todo || !crc_known || !state || !where) {
		say("backup: out of memory\n");
		free(todo); free(crc_known); free(state); free(where);
		idx_free(&prev); finish(); return BK_FAILED;
	}
	int ntodo = 0;
	uint64_t total = 0;
	for (int i = 0; i < ncard; i++) {
		const card_t *c = &card[i];
		const ent_t *e = idx_find(&prev, c->path);
		if (!(i & 63)) breathe();
		if (stopped()) break;
		const man_t *m = man_find(c->path);
		bool same_date = e && e->size == c->st.size && e->fdate == c->st.fdate &&
			e->ftime == c->st.ftime && date_known(c->st.fdate, c->st.ftime);
		uint32_t crc = 0;
		bool have_crc = false;
		if (same_date) { crc = e->crc; have_crc = true; }
		else if ((e && e->size == c->st.size) || (m && m->size == c->st.size)) {
			// same size, date not to be trusted or different: read it
			if (file_crc(c->path, c->st.size, &crc)) have_crc = true;
			if (stopped()) break;
		}
		if (have_crc && m && m->size == c->st.size && m->crc == crc) {
			state[i] = 2; crc_known[i] = crc; where[i] = -1;
			r->shipped++;
			continue;
		}
		if (have_crc && e && e->size == c->st.size && e->crc == crc && e->where >= 0) {
			uint8_t why = 0;
			if (o->refresh > 0 && fulls_after(names[e->where]) >= o->refresh) why = 3;
			if (is_damaged(names[e->where], c->path)) why = 4;
			if (!why) { state[i] = 1; crc_known[i] = crc; where[i] = e->where; continue; }
			todo[ntodo].ci = i; todo[ntodo].why = why; ntodo++;
			total += c->st.size;
			continue;
		}
		todo[ntodo].ci = i; todo[ntodo].why = e ? 2 : 1; ntodo++;
		total += c->st.size;
	}
	if (stopped()) {
		say("backup: stopped; nothing was written\n");
		free(todo); free(crc_known); free(state); free(where);
		idx_free(&prev); finish(); return BK_FAILED;
	}

	// files gone from the card since the last backup
	int gone = 0;
	if (!watch)
		for (int i = 0; i < prev.n; i++) {
			int lo = 0, hi = ncard - 1;
			bool found = false;
			while (lo <= hi) {
				int mid = (lo + hi) / 2, c = strcmp(prev.e[i].path, card[mid].path);
				if (!c) { found = true; break; }
				if (c < 0) hi = mid - 1; else lo = mid + 1;
			}
			if (!found) gone++;
		}

	if (ntodo == 0 && gone == 0 && nbks > 0) {
		say("backup: nothing changed since %s; nothing written\n", names[bks[nbks - 1].id]);
		free(todo); free(crc_known); free(state); free(where);
		idx_free(&prev); finish(); return BK_DONE;
	}
	if (watch && ntodo == 0) {
		say("backup: already backed up\n");
		free(todo); free(crc_known); free(state); free(where);
		idx_free(&prev); finish(); return BK_DONE;
	}

	char tb[24];
	kb_str(total, tb, sizeof tb);
	if (watch) say("backup: %d file%s to copy (%s)\n", ntodo, ntodo == 1 ? "" : "s", tb);
	else say("backup: %d file%s on the card: %d to copy (%s), %d unchanged, %d gone since the last backup%s\n",
		ncard, ncard == 1 ? "" : "s", ntodo, tb, ncard - ntodo - (int)r->shipped, gone,
		r->shipped ? "" : "");
	if (r->shipped) say("backup: %u installed by update and unchanged; not copied\n", (unsigned)r->shipped);

	// 5. room
	uint32_t need = (uint32_t)(ncard / 12 + 8) + o->reserve_kb;   // the index: ~80 bytes a file
	for (int i = 0; i < ntodo; i++) need += kb_of(card[todo[i].ci].st.size);
	uint32_t free_kb = 0;
	if (IO->free_kb(IO->ctx, o->dest, &free_kb) != BK_OK) free_kb = 0;
	if (free_kb < need) {
		uint32_t want = need - free_kb;
		say("backup: %u KB more room needed; making room\n", (unsigned)want);
		uint32_t got = room(want, &prev, o->dry_run, r);
		// the estimate per copy is not the drive's own count: read it
		// again and go on while copies are still being removed
		for (int more = 0; !o->dry_run && got && more < 64; more++) {
			uint32_t f2 = 0;
			if (IO->free_kb(IO->ctx, o->dest, &f2) != BK_OK || f2 >= need) break;
			scan_backups(true);
			got = room(need - f2, &prev, false, r);
		}
		if (r->removed) {
			char rb[24];
			kb_str(r->removed_bytes, rb, sizeof rb);
			say("backup: %s %u older cop%s (%s)\n", o->dry_run ? "would remove" : "removed",
				(unsigned)r->removed, r->removed == 1 ? "y" : "ies", rb);
		}
		if (!o->dry_run) IO->free_kb(IO->ctx, o->dest, &free_kb);
		else free_kb += got;
		if (free_kb < need) {
			say("backup: not enough room on the drive: %u KB free, %u KB needed; nothing %s\n",
				(unsigned)free_kb, (unsigned)need, o->dry_run ? "would be written" : "written");
			free(todo); free(crc_known); free(state); free(where);
			idx_free(&prev); finish(); return BK_FAILED;
		}
		if (!o->dry_run) scan_backups(true);
	}

	if (o->dry_run) {
		for (int i = 0; i < ntodo; i++) {
			static const char *const whys[] = { "", "new", "changed", "a fresh copy", "its copy was damaged" };
			say("  %s (%s)\n", card[todo[i].ci].path, whys[todo[i].why]);
		}
		say("backup: -n: nothing written\n");
		free(todo); free(crc_known); free(state); free(where);
		idx_free(&prev); finish(); return BK_DONE;
	}

	// 6. the name and the copies
	char bname[BK_NAME_MAX];
	for (int tries = 0;; tries++) {
		bk_tm_t t;
		uint32_t up = 0;
		bool set = IO->clock(IO->ctx, &t, &up);
		const char *newest = nbks ? names[bks[nbks - 1].id] : "";
		if (bk_name_next(newest, &t, set, watch, bname) == 0) break;
		if (tries > 30 || !IO->sleep_ms) { bk_name_next(newest, &t, false, watch, bname); break; }
		IO->sleep_ms(IO->ctx, 100);
	}
	int bid = name_id(bname);
	char bdir[BK_PATH_MAX];
	if (bid < 0 || !pjoin(bdir, "%s/%s", root, bname) || IO->mkdir(IO->ctx, bdir) != BK_OK) {
		say("backup: cannot make %s\n", bdir);
		free(todo); free(crc_known); free(state); free(where);
		idx_free(&prev); finish(); return BK_FAILED;
	}
	snprintf(r->name, sizeof r->name, "%s", bname);
	say("backup: to %s\n", bdir);

	copy_done = 0; copy_total = total;
	copy_files_done = 0; copy_files_total = (uint32_t)ntodo;
	int fail = BK_OK;
	for (int t = 0; t < ntodo; t++) {
		int i = todo[t].ci;
		card_t *c = &card[i];
		char dst[BK_PATH_MAX];
		if (!pjoin(dst, "%s%s", bdir, c->path)) {
			say("  %s: path too long on the drive; left out\n", c->path);
			r->skipped++;
			state[i] = 3;
			continue;
		}
		int k = mkdirs_for(dst);
		if (k != BK_OK && k != BK_EEXIST) { fail = CP_DST; break; }
		uint32_t crc = 0, size = 0;
		int cr = CP_OK;
		for (int attempt = 0; attempt < 2; attempt++) {
			cr = copy_file(c->path, dst, &crc, &size);
			if (cr != CP_OK) break;
			bk_stat_t after;
			if (IO->stat(IO->ctx, c->path, &after) == BK_OK && after.size == size &&
				after.fdate == c->st.fdate && after.ftime == c->st.ftime) break;
			// it changed while it was copied: once more
			if (IO->stat(IO->ctx, c->path, &after) == BK_OK) c->st = after;
			if (attempt == 1) {
				say("  %s changed while it was copied; the copy may be of neither version\n", c->path);
				r->changed++;
			}
		}
		if (cr == CP_SRC) {
			say("  %s: cannot be read; left out\n", c->path);
			IO->unlink(IO->ctx, dst);
			r->skipped++;
			state[i] = 3;
			continue;
		}
		if (cr == CP_VERIFY) {
			say("  %s: the copy read back wrong; the drive may be failing\n", c->path);
			fail = cr;
			break;
		}
		if (cr != CP_OK) { fail = cr; break; }
		c->st.size = size;
		state[i] = 0;
		crc_known[i] = crc;
		where[i] = (int16_t)bid;
		r->copied++;
		r->bytes += size;
		copy_files_done++;
		if (ntodo <= 50 || todo[t].why != 1) say("  %s\n", c->path);
	}
	if (fail != BK_OK) {
		if (fail == CP_STOP) say("backup: stopped; %s is incomplete and will be removed by the next backup\n", bname);
		else if (fail == CP_NOSPC) say("backup: the drive is full; %s is incomplete and will be removed by the next backup\n", bname);
		else if (fail != CP_VERIFY) say("backup: cannot write to the drive; %s is incomplete\n", bname);
		free(todo); free(crc_known); free(state); free(where);
		idx_free(&prev); finish();
		return BK_FAILED;
	}

	// 7. the index
	progress(BK_PH_INDEX, NULL, copy_files_done, copy_files_total, copy_done, copy_total);
	char ip[BK_PATH_MAX];
	pjoin(ip, "%s/" INDEX_NAME, bdir);
	wr_t *w = calloc(1, sizeof *w);
	int nent = 0;
	if (w) {
		w->h = IO->open_write(IO->ctx, ip);
		if (w->h < 0) w->err = true;
	}
	if (w && !w->err) {
		bk_tm_t t;
		uint32_t up = 0;
		bool set = IO->clock(IO->ctx, &t, &up);
		wr(w, "zeitlos-backup 1\n");
		wr(w, "backup %s\n", bname);
		wr(w, "kind %s\n", watch ? "watch" : "full");
		if (set) {
			wr(w, "time %04d-%02d-%02d %02d:%02d:%02d\n", t.year, t.month, t.day, t.hour, t.min, t.sec);
			wr(w, "clock set\n");
		} else {
			wr(w, "time -\n");
			wr(w, "clock not set, %uh%02um after power-on\n", (unsigned)(up / 3600), (unsigned)(up / 60 % 60));
		}
		wr(w, "dates utc\n");
		wr(w, "from");
		for (int i = 0; i < o->nfrom; i++) if (o->from[i]) wr(w, " %s", o->from[i]);
		wr(w, "\nskip");
		for (int i = 0; i < o->nskip; i++) if (o->skip[i]) wr(w, " %s", o->skip[i]);
		char u64[24];
		wr(w, "\ncopied %u %s\n", (unsigned)r->copied, u64str(r->bytes, u64));
		for (int i = 0; i < ncard && !w->err; i++) {
			if (!(i & 127)) breathe();
			if (state[i] == 3) continue;
			char d[20];
			fat_str(card[i].st.fdate, card[i].st.ftime, d);
			if (!date_known(card[i].st.fdate, card[i].st.ftime)) strcpy(d, "-");
			if (watch && state[i] != 0) continue;   // a watch backup: only what it copied
			if (state[i] == 2)
				wr(w, "s %08x %u %s - %s\n", (unsigned)crc_known[i], (unsigned)card[i].st.size, d, card[i].path);
			else
				wr(w, "f %08x %u %s %s %s\n", (unsigned)crc_known[i], (unsigned)card[i].st.size, d,
					names[where[i]], card[i].path);
			nent++;
		}
		wr_flush(w);
		char end[48];
		int n = snprintf(end, sizeof end, "end %d %08x\n", nent, (unsigned)w->crc);
		if (!w->err && IO->write(IO->ctx, w->h, end, n) != n) w->err = true;
		if (!w->err && IO->sync(IO->ctx, w->h) != BK_OK) w->err = true;
		if (IO->close(IO->ctx, w->h) != BK_OK) w->err = true;
	}
	bool ok = w && !w->err && idx_load(root, bname, NULL) == 1;
	free(w);
	free(todo); free(crc_known); free(state); free(where);
	idx_free(&prev);
	if (!ok) {
		say("backup: the index could not be written; %s is incomplete\n", bname);
		finish();
		return BK_FAILED;
	}
	r->files = (uint32_t)nent;
	kb_str(r->bytes, tb, sizeof tb);
	say("backup: done: %s, %u file%s, %u copied (%s)\n", bname, (unsigned)r->files,
		r->files == 1 ? "" : "s", (unsigned)r->copied, tb);
	finish();
	return (r->skipped || r->changed) ? BK_PARTIAL : BK_DONE;
}

// -- looking ----------------------------------------------------------------

static int open_drive(const bk_io_t *io, const bk_opts_t *o) {
	int rc = setup(io, o);
	if (rc) return rc;
	if (!dest_there()) { finish(); return BK_FAILED; }
	if (scan_backups(true) < 0) { say("backup: out of memory\n"); finish(); return BK_FAILED; }
	return BK_OK;
}

static bool bk_time(int k, char out[24], bool *clock_set) {
	char base[20];
	int n;
	bk_name_parse(names[bks[k].id], base, &n, NULL);
	*clock_set = n == 0;
	if (n) { out[0] = 0; return false; }
	snprintf(out, 24, "%.10s %.2s:%.2s:%.2s", base, base + 11, base + 14, base + 17);
	return true;
}

int bk_list(const bk_io_t *io, const bk_opts_t *o,
		int (*fn)(void *arg, const bk_info_t *b), void *arg) {
	int rc = open_drive(io, o);
	if (rc) return rc;
	for (int k = 0; k < nbks; k++) {
		bk_info_t b;
		memset(&b, 0, sizeof b);
		snprintf(b.name, sizeof b.name, "%s", names[bks[k].id]);
		b.watch = bks[k].watch;
		b.complete = bks[k].state == 1;
		b.pinned = bk_pinned(k);
		bk_time(k, b.time, &b.clock_set);
		if (b.complete) {
			idx_tail(root, b.name, &b.files);
			idx_head(root, b.name, &b.copied, &b.bytes);
		}
		if (fn(arg, &b)) break;
	}
	finish();
	return BK_DONE;
}

int bk_summary(const bk_io_t *io, const bk_opts_t *o, bk_summary_t *s) {
	memset(s, 0, sizeof *s);
	IO = io;
	int rc = setup(io, o);
	if (rc) return rc;
	if (!dest_there_quiet()) { finish(); return BK_FAILED; }
	if (scan_backups(true) < 0) { finish(); return BK_FAILED; }
	for (int k = 0; k < nbks; k++) {
		if (bks[k].state == 1) {
			if (!s->backups) snprintf(s->oldest, sizeof s->oldest, "%s", names[bks[k].id]);
			if (bks[k].watch) snprintf(s->newest_watch, sizeof s->newest_watch, "%s", names[bks[k].id]);
			else snprintf(s->newest, sizeof s->newest, "%s", names[bks[k].id]);
			s->backups++;
		} else if (bks[k].state == 0) s->cut++;
	}
	if (s->newest[0]) idx_tail(root, s->newest, &s->newest_files);
	uint32_t fr = 0;
	if (IO->free_kb(IO->ctx, o->dest, &fr) == BK_OK) s->free_kb = fr;
	finish();
	return BK_DONE;
}

static int pick(const char *backup) {
	if (!backup || !*backup) {
		for (int k = nbks - 1; k >= 0; k--) if (bks[k].state == 1) return k;
		say("backup: there is no backup on the drive yet\n");
		return -1;
	}
	int k = find_bk(backup);
	if (k < 0) {
		// a prefix ("2026-10-21") names the newest that starts so
		for (int i = nbks - 1; i >= 0; i--)
			if (bks[i].state == 1 && !strncmp(names[bks[i].id], backup, strlen(backup))) return i;
		say("backup: no backup %s\n", backup);
		return -1;
	}
	if (bks[k].state != 1) { say("backup: %s is not complete\n", backup); return -1; }
	return k;
}

int bk_files(const bk_io_t *io, const bk_opts_t *o, const char *backup,
		const char *dir, int (*fn)(void *arg, const bk_file_t *f), void *arg) {
	int rc = open_drive(io, o);
	if (rc) return rc;
	int k = pick(backup), er;
	if (k < 0) { finish(); return BK_FAILED; }
	idx_t x;
	if ((er = effective(k, &x)) != 0) { say(er == -2 ? "backup: stopped\n" : "backup: out of memory\n"); finish(); return BK_FAILED; }
	for (int i = 0; i < x.n; i++) {
		ent_t *e = &x.e[i];
		if (dir && !inside(e->path, dir)) continue;
		bk_file_t f;
		f.path = e->path; f.crc = e->crc; f.size = e->size;
		f.fdate = e->fdate; f.ftime = e->ftime;
		f.where = e->where >= 0 ? names[e->where] : "-";
		f.present = false;
		if (e->where >= 0) {
			char p[BK_PATH_MAX];
			bk_stat_t st;
			f.present = pjoin(p, "%s/%s%s", root, names[e->where], e->path) &&
				IO->stat(IO->ctx, p, &st) == BK_OK;
		}
		if (fn(arg, &f)) break;
	}
	idx_free(&x);
	finish();
	return BK_DONE;
}

int bk_history(const bk_io_t *io, const bk_opts_t *o, const char *path,
		int (*fn)(void *arg, const char *backup, const bk_stat_t *st), void *arg) {
	int rc = open_drive(io, o);
	if (rc) return rc;
	int found = 0;
	for (int k = 0; k < nbks; k++) {
		if (bks[k].state != 1) continue;
		char p[BK_PATH_MAX];
		bk_stat_t st;
		if (!pjoin(p, "%s/%s%s", root, names[bks[k].id], path)) continue;
		if (IO->stat(IO->ctx, p, &st) != BK_OK || st.dir) continue;
		found++;
		if (fn(arg, names[bks[k].id], &st)) break;
	}
	if (!found) say("backup: no copy of %s on the drive\n", path);
	finish();
	return found ? BK_DONE : BK_FAILED;
}

// -- restoring --------------------------------------------------------------

// "book.txt" restored beside itself from 2026-10-21_11-30-48:
// "book (2026-10-21 11-30).txt".
static bool beside(const char *path, const char *bname, char out[BK_PATH_MAX]) {
	char base[20], tag[32];
	int n;
	bk_name_parse(bname, base, &n, NULL);
	if (n) snprintf(tag, sizeof tag, "%.10s %.5s+%d", base, base + 11, n);
	else snprintf(tag, sizeof tag, "%.10s %.5s", base, base + 11);
	const char *slash = strrchr(path, '/');
	const char *dot = strrchr(path, '.');
	if (!dot || (slash && dot < slash) || dot == slash + 1) dot = path + strlen(path);
	for (int k = 1; k < 100; k++) {
		char extra[8] = "";
		if (k > 1) snprintf(extra, sizeof extra, " %d", k);
		if (!pjoin(out, "%.*s (%s%s)%s", (int)(dot - path), path, tag, extra, dot)) return false;
		bk_stat_t st;
		if (IO->stat(IO->ctx, out, &st) == BK_ENOENT) return true;
	}
	return false;
}

int bk_restore(const bk_io_t *io, const bk_opts_t *o, const char *backup,
		const char *path, const char *out, bool replace, bk_result_t *r) {
	memset(r, 0, sizeof *r);
	if (!path || path[0] != '/') {
		IO = io;
		say("backup: restore what? a path like /home/book.txt\n");
		return BK_USAGE;
	}
	int rc = open_drive(io, o);
	if (rc) return rc;
	int k = pick(backup), er;
	if (k < 0) { finish(); return BK_FAILED; }
	idx_t x;
	if ((er = effective(k, &x)) != 0) { say(er == -2 ? "backup: stopped\n" : "backup: out of memory\n"); finish(); return BK_FAILED; }
	const char *bname = names[bks[k].id];
	snprintf(r->name, sizeof r->name, "%s", bname);
	int n = 0;
	for (int i = 0; i < x.n; i++) if (inside(x.e[i].path, path)) n++;
	if (!n) {
		say("backup: %s has no %s\n", bname, path);
		idx_free(&x); finish(); return BK_FAILED;
	}
	bool single = false;
	for (int i = 0; i < x.n; i++) if (!strcmp(x.e[i].path, path)) single = true;
	if (!o->dry_run && lock_take() != BK_OK) { idx_free(&x); finish(); return BK_FAILED; }
	int done = 0;
	for (int i = 0; i < x.n; i++) {
		ent_t *e = &x.e[i];
		if (!inside(e->path, path)) continue;
		if (stopped()) { say("backup: stopped\n"); break; }
		if (e->where < 0) {
			say("  %s: installed by update, not in the backup; `update` brings it back\n", e->path);
			r->skipped++;
			continue;
		}
		char src[BK_PATH_MAX], dst[BK_PATH_MAX], part[BK_PATH_MAX];
		bk_stat_t st;
		if (!pjoin(src, "%s/%s%s", root, names[e->where], e->path) || IO->stat(IO->ctx, src, &st) != BK_OK) {
			say("  %s: its copy from %s was removed to make room\n", e->path, names[e->where]);
			r->skipped++;
			continue;
		}
		if (out) {
			if (single) snprintf(dst, sizeof dst, "%s", out);
			else if (!pjoin(dst, "%s%s", out, e->path + strlen(path))) { r->skipped++; continue; }
		} else snprintf(dst, sizeof dst, "%s", e->path);
		bk_stat_t cur;
		bool exists = IO->stat(IO->ctx, dst, &cur) == BK_OK && !cur.dir;
		if (exists) {
			uint32_t c;
			if (cur.size == e->size && file_crc(dst, cur.size, &c) && c == e->crc) {
				say("  %s: already the same\n", dst);
				done++;
				continue;
			}
			if (!replace) {
				char b[BK_PATH_MAX];
				if (!beside(dst, bname, b)) { r->skipped++; continue; }
				snprintf(dst, sizeof dst, "%s", b);
				exists = false;
			}
		}
		if (!pjoin(part, "%s.part", dst)) { r->skipped++; continue; }
		if (mkdirs_for(dst) < 0) { say("  %s: cannot make its folder\n", dst); r->skipped++; continue; }
		uint32_t crc, size;
		bool v = OPT->verify;
		((bk_opts_t *)OPT)->verify = true;     // a restore is always read back
		int cr = copy_file(src, part, &crc, &size);
		((bk_opts_t *)OPT)->verify = v;
		if (cr == CP_OK && crc != e->crc) cr = CP_VERIFY;
		if (cr != CP_OK) {
			IO->unlink(IO->ctx, part);
			if (cr == CP_STOP) { say("backup: stopped\n"); break; }
			say("  %s: %s\n", dst, cr == CP_VERIFY ? "the copy on the drive is damaged" :
				cr == CP_SRC ? "cannot read the copy" : "cannot write it");
			r->bad += cr == CP_VERIFY;
			r->skipped++;
			continue;
		}
		if (exists && IO->unlink(IO->ctx, dst) != BK_OK) {
			say("  %s: in use; the restored copy is %s\n", dst, part);
			r->skipped++;
			continue;
		}
		if (IO->rename(IO->ctx, part, dst) != BK_OK) {
			say("  %s: could not be put in place; it is %s\n", dst, part);
			r->skipped++;
			continue;
		}
		say("  %s\n", dst);
		r->copied++;
		r->bytes += size;
		done++;
	}
	idx_free(&x);
	say("backup: %d of %d restored from %s\n", done, n, bname);
	finish();
	return r->skipped ? (done ? BK_PARTIAL : BK_FAILED) : BK_DONE;
}

// -- checking ---------------------------------------------------------------

int bk_check(const bk_io_t *io, const bk_opts_t *o, bool all, bk_result_t *r) {
	memset(r, 0, sizeof *r);
	int rc = open_drive(io, o);
	if (rc) return rc;
	strs_t bad;
	memset(&bad, 0, sizeof bad);
	int complete = 0, cut = 0;
	for (int k = 0; k < nbks; k++)
		if (bks[k].state == 1) bks[k].state = idx_load(root, names[bks[k].id], NULL);
	for (int k = 0; k < nbks; k++) {
		const char *bname = names[bks[k].id];
		if (bks[k].state == 0 && referenced(k)) bks[k].state = -1;
		if (bks[k].state == 0) { say("  %s: cut short (no complete index); the next backup removes it\n", bname); cut++; continue; }
		if (bks[k].state < 0) { say("  %s: its index is damaged\n", bname); r->bad++; continue; }
		complete++;
		if (!all) continue;
		idx_t x;
		if (idx_load(root, bname, &x) != 1) { r->bad++; continue; }
		int here = 0, badhere = 0;
		for (int i = 0; i < x.n; i++) {
			ent_t *e = &x.e[i];
			if (e->where < 0 || strcmp(names[e->where], bname)) continue;
			char p[BK_PATH_MAX];
			bk_stat_t st;
			here++;
			if (!pjoin(p, "%s/%s%s", root, bname, e->path)) continue;
			progress(BK_PH_CHECK, p, (uint32_t)k, (uint32_t)nbks, 0, 0);
			if (IO->stat(IO->ctx, p, &st) != BK_OK) {
				if (later_copy(k, e->path)) r->thinned++;
				else { say("  %s%s: missing\n", bname, e->path); r->bad++; badhere++;
					char l[BK_PATH_MAX + 32]; snprintf(l, sizeof l, "%s %s", bname, e->path); strs_push(&bad, l); }
				continue;
			}
			uint32_t c;
			if (!file_crc(p, e->size, &c) || c != e->crc) {
				if (stopped()) break;
				say("  %s%s: damaged\n", bname, e->path);
				r->bad++; badhere++;
				char l[BK_PATH_MAX + 32];
				snprintf(l, sizeof l, "%s %s", bname, e->path);
				strs_push(&bad, l);
			}
			if (stopped()) break;
		}
		idx_free(&x);
		if (stopped()) { say("backup: stopped\n"); break; }
		say("  %s: %d cop%s%s\n", bname, here, here == 1 ? "y" : "ies", badhere ? ", some damaged" : ", all good");
	}
	if (bad.n && !o->dry_run && lock_take() == BK_OK) {
		char d[BK_PATH_MAX], p[BK_PATH_MAX];
		if (pjoin(d, "%s/" CHECK_DIR, root)) {
			IO->mkdir(IO->ctx, d);
			const char *newest = nbks ? names[bks[nbks - 1].id] : "none";
			for (int k = 1; k < 1000; k++) {
				bk_stat_t st;
				if (!pjoin(p, "%s/%s-%d.txt", d, newest, k)) break;
				if (IO->stat(IO->ctx, p, &st) == BK_ENOENT) break;
			}
			int h = IO->open_write(IO->ctx, p);
			if (h >= 0) {
				for (int i = 0; i < bad.n; i++) {
					char l[BK_PATH_MAX + 40];
					int n = snprintf(l, sizeof l, "bad %s\n", bad.v[i]);
					IO->write(IO->ctx, h, l, n);
				}
				IO->sync(IO->ctx, h);
				IO->close(IO->ctx, h);
				say("backup: the next backup copies these again from the card where they are unchanged (%s)\n", p);
			}
		}
	}
	strs_free(&bad);
	say("backup: %d backup%s complete, %d cut short, %u problem%s%s\n", complete, complete == 1 ? "" : "s",
		cut, (unsigned)r->bad, r->bad == 1 ? "" : "s",
		all ? "" : " (`backup check -a` reads every copy)");
	finish();
	return r->bad ? BK_PARTIAL : BK_DONE;
}

int bk_pin(const bk_io_t *io, const bk_opts_t *o, const char *backup, bool pin) {
	int rc = open_drive(io, o);
	if (rc) return rc;
	int k = pick(backup);
	if (k < 0) { finish(); return BK_FAILED; }
	char p[BK_PATH_MAX];
	if (!pjoin(p, "%s/%s/" PIN_NAME, root, names[bks[k].id])) { finish(); return BK_FAILED; }
	int res = BK_OK;
	if (pin) {
		int h = IO->open_write(IO->ctx, p);
		if (h < 0) res = h;
		else { IO->sync(IO->ctx, h); IO->close(IO->ctx, h); }
	} else {
		bk_stat_t st;
		if (IO->stat(IO->ctx, p, &st) == BK_OK) res = IO->unlink(IO->ctx, p);
	}
	say("backup: %s %s\n", names[bks[k].id], res ? "could not be changed" : pin ? "pinned: never removed" : "unpinned");
	finish();
	return res ? BK_FAILED : BK_DONE;
}

int bk_make_room(const bk_io_t *io, const bk_opts_t *o, uint32_t kb, bk_result_t *r) {
	memset(r, 0, sizeof *r);
	int rc = open_drive(io, o);
	if (rc) return rc;
	if (!o->dry_run && lock_take() != BK_OK) { finish(); return BK_FAILED; }
	idx_t now;
	if (nbks ? effective(nbks - 1, &now) : (memset(&now, 0, sizeof now), 0)) { finish(); return BK_FAILED; }
	uint32_t got = room(kb, &now, o->dry_run, r);
	idx_free(&now);
	char rb[24];
	kb_str(r->removed_bytes, rb, sizeof rb);
	say("backup: %s %u cop%s (%s)%s\n", o->dry_run ? "would remove" : "removed", (unsigned)r->removed,
		r->removed == 1 ? "y" : "ies", rb, got < kb ? "; nothing more may be removed" : "");
	finish();
	return got >= kb ? BK_DONE : BK_PARTIAL;
}
