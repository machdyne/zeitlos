/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * memfs: a card and a backup drive in memory that can lose power.
 * See memfs.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memfs.h"

#define MAXH 16

typedef struct {
	char path[BK_PATH_MAX];
	bool dir;
	uint8_t *data;
	uint32_t len, cap;
	uint16_t fdate, ftime;
} node_t;

typedef struct {
	int node;           // -1 free
	uint32_t pos;
	bool wr;
	uint32_t synced;    // bytes known to be on the medium
} hand_t;

struct memfs {
	node_t *n;
	int nn, capn;
	hand_t h[MAXH];
	uint32_t card_kb, usb_kb;
	bool usb_there;
	bool clock_set;
	int Y, M, D, hh, mi, ss;
	uint32_t uptime;
	int cut_at, ops, card_ops;
	bool dead;
	uint32_t rnd;
	char owner[64];
	char *out;
	size_t outlen, outcap;
	bool verbose;
	int stop_after, stops;
	uint32_t free_lie;
};

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static bool ci_eq(const char *a, const char *b) {
	while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) { a++; b++; }
	return lower((unsigned char)*a) == lower((unsigned char)*b);
}

static uint32_t rnd(memfs_t *m) {
	m->rnd = m->rnd * 1103515245u + 12345u;
	return m->rnd >> 8;
}

static bool on_usb(const char *p) {
	return (!strncmp(p, "/usb", 4) || !strncmp(p, "/USB", 4)) && (p[4] == 0 || p[4] == '/');
}

static int find(const memfs_t *m, const char *p) {
	for (int i = 0; i < m->nn; i++) if (ci_eq(m->n[i].path, p)) return i;
	return -1;
}

static void parent_of(const char *p, char *out) {
	snprintf(out, BK_PATH_MAX, "%s", p);
	char *s = strrchr(out, '/');
	if (s == out) out[1] = 0;
	else if (s) *s = 0;
}

static bool is_dir(const memfs_t *m, const char *p) {
	if (!strcmp(p, "/")) return true;
	if (ci_eq(p, "/usb")) return m->usb_there;
	int i = find(m, p);
	return i >= 0 && m->n[i].dir;
}

static uint32_t used_kb(const memfs_t *m, bool usb) {
	uint64_t b = 0;
	for (int i = 0; i < m->nn; i++) {
		if (on_usb(m->n[i].path) != usb) continue;
		b += m->n[i].dir ? 4096 : ((uint64_t)m->n[i].len + 4095) / 4096 * 4096;
	}
	return (uint32_t)(b / 1024);
}

static void stamp(const memfs_t *m, uint16_t *d, uint16_t *t) {
	if (!m->clock_set) {
		*d = (uint16_t)((40u << 9) | (1u << 5) | 1u); *t = 0;   // fs.c's 2020-01-01
		return;
	}
	*d = (uint16_t)(((unsigned)(m->Y - 1980) << 9) | ((unsigned)m->M << 5) | (unsigned)m->D);
	*t = (uint16_t)(((unsigned)m->hh << 11) | ((unsigned)m->mi << 5) | ((unsigned)m->ss / 2));
}

static int add(memfs_t *m, const char *p, bool dir) {
	if (m->nn == m->capn) {
		m->capn = m->capn ? m->capn * 2 : 64;
		m->n = realloc(m->n, (size_t)m->capn * sizeof *m->n);
	}
	node_t *n = &m->n[m->nn];
	memset(n, 0, sizeof *n);
	snprintf(n->path, sizeof n->path, "%s", p);
	n->dir = dir;
	stamp(m, &n->fdate, &n->ftime);
	return m->nn++;
}

static void del(memfs_t *m, int i) {
	free(m->n[i].data);
	for (int k = 0; k < MAXH; k++) {
		if (m->h[k].node == i) m->h[k].node = -1;
		else if (m->h[k].node > i) m->h[k].node--;
	}
	memmove(&m->n[i], &m->n[i + 1], (size_t)(m->nn - i - 1) * sizeof *m->n);
	m->nn--;
}

// -- power ------------------------------------------------------------------

static void power_cut(memfs_t *m) {
	for (int k = 0; k < MAXH; k++) {
		hand_t *h = &m->h[k];
		if (h->node < 0) continue;
		if (h->wr) {
			node_t *n = &m->n[h->node];
			if (n->len > h->synced) {
				uint32_t keep = h->synced + rnd(m) % (n->len - h->synced + 1);
				n->len = keep;
				// the last sector may hold rubbish
				if (keep > h->synced && rnd(m) % 2) n->data[keep - 1] ^= 0x5A;
			}
		}
		h->node = -1;
	}
	m->dead = true;
}

// One write to a volume. false: the power is gone (do nothing more).
// *half: the cut happens during this operation.
static bool wop(memfs_t *m, const char *p, bool *half) {
	*half = false;
	if (m->dead) return false;
	if (!on_usb(p)) m->card_ops++;
	m->ops++;
	if (m->cut_at && m->ops == m->cut_at) {
		*half = true;
		return true;
	}
	return true;
}

// -- bk_io_t ----------------------------------------------------------------

static int x_open(memfs_t *m, const char *p, bool wr) {
	if (m->dead) return BK_ERR;
	if (on_usb(p) && !m->usb_there) return BK_ENOENT;
	int i = find(m, p);
	bool half = false;
	if (wr) {
		char par[BK_PATH_MAX];
		parent_of(p, par);
		if (!is_dir(m, par)) return BK_ENOENT;
		if (i >= 0 && m->n[i].dir) return BK_ERR;
		if (!wop(m, p, &half)) return BK_ERR;
		if (i < 0) i = add(m, p, false);
		else { m->n[i].len = 0; stamp(m, &m->n[i].fdate, &m->n[i].ftime); }
		if (half) { power_cut(m); return BK_ERR; }
	} else if (i < 0 || m->n[i].dir) return BK_ENOENT;
	for (int k = 0; k < MAXH; k++) {
		if (m->h[k].node >= 0) continue;
		m->h[k].node = i; m->h[k].pos = 0; m->h[k].wr = wr; m->h[k].synced = 0;
		return k;
	}
	return BK_ERR;
}

static int f_open_read(void *c, const char *p) { return x_open(c, p, false); }
static int f_open_write(void *c, const char *p) { return x_open(c, p, true); }

static hand_t *hand(memfs_t *m, int h) {
	if (m->dead || h < 0 || h >= MAXH || m->h[h].node < 0) return NULL;
	return &m->h[h];
}

static int f_read(void *c, int h, void *buf, int n) {
	memfs_t *m = c;
	hand_t *x = hand(m, h);
	if (!x) return BK_ERR;
	node_t *nd = &m->n[x->node];
	if (x->pos >= nd->len) return 0;
	uint32_t k = nd->len - x->pos;
	if (k > (uint32_t)n) k = (uint32_t)n;
	memcpy(buf, nd->data + x->pos, k);
	x->pos += k;
	return (int)k;
}

static int f_write(void *c, int h, const void *buf, int n) {
	memfs_t *m = c;
	hand_t *x = hand(m, h);
	if (!x || !x->wr) return BK_ERR;
	node_t *nd = &m->n[x->node];
	bool half;
	if (!wop(m, nd->path, &half)) return BK_ERR;
	bool usb = on_usb(nd->path);
	uint32_t cap = usb ? m->usb_kb : m->card_kb;
	uint32_t cur = (nd->len + 4095) / 4096 * 4;
	uint32_t after = (x->pos + (uint32_t)n + 4095) / 4096 * 4;
	uint32_t used = used_kb(m, usb);
	if (after > cur && used + (after - cur) > cap) {
		// as much as fits in the clusters it has and the free ones
		uint32_t room = (cap > used ? cap - used : 0) * 1024 + cur * 1024 - x->pos;
		if ((uint32_t)n > room) n = (int)room;
		if (n <= 0) return BK_ENOSPC;
	}
	if (x->pos + (uint32_t)n > nd->cap) {
		nd->cap = (x->pos + (uint32_t)n) * 2 + 64;
		nd->data = realloc(nd->data, nd->cap);
	}
	memcpy(nd->data + x->pos, buf, (size_t)n);
	x->pos += (uint32_t)n;
	if (x->pos > nd->len) nd->len = x->pos;
	if (half) { power_cut(m); return BK_ERR; }
	return n;
}

static int f_seek(void *c, int h, uint32_t off) {
	hand_t *x = hand(c, h);
	if (!x) return BK_ERR;
	x->pos = off;
	return BK_OK;
}

static int f_sync(void *c, int h) {
	memfs_t *m = c;
	hand_t *x = hand(m, h);
	if (!x) return BK_ERR;
	if (!x->wr) return BK_OK;
	bool half;
	if (!wop(m, m->n[x->node].path, &half)) return BK_ERR;
	if (half) { power_cut(m); return BK_ERR; }
	x->synced = m->n[x->node].len;
	return BK_OK;
}

static int f_close(void *c, int h) {
	memfs_t *m = c;
	hand_t *x = hand(m, h);
	if (!x) return BK_ERR;
	if (x->wr) {
		bool half;
		if (!wop(m, m->n[x->node].path, &half)) return BK_ERR;
		if (half) { power_cut(m); return BK_ERR; }
		stamp(m, &m->n[x->node].fdate, &m->n[x->node].ftime);
	}
	x->node = -1;
	return BK_OK;
}

static int f_stat(void *c, const char *p, bk_stat_t *st) {
	memfs_t *m = c;
	if (m->dead) return BK_ERR;
	memset(st, 0, sizeof *st);
	if (!strcmp(p, "/") || ci_eq(p, "/usb")) {
		if (ci_eq(p, "/usb") && !m->usb_there) return BK_ENOENT;
		st->dir = true;
		return BK_OK;
	}
	if (on_usb(p) && !m->usb_there) return BK_ENOENT;
	int i = find(m, p);
	if (i < 0) return BK_ENOENT;
	st->dir = m->n[i].dir;
	st->size = m->n[i].dir ? 0 : m->n[i].len;
	st->fdate = m->n[i].fdate;
	st->ftime = m->n[i].ftime;
	return BK_OK;
}

static int f_list(void *c, const char *p,
		int (*fn)(void *arg, const char *name, const bk_stat_t *st), void *arg) {
	memfs_t *m = c;
	if (m->dead) return BK_ERR;
	if (!is_dir(m, p)) return BK_ENOENT;
	// a copy of the names first: fn may change the tree
	int cnt = 0;
	char (*names)[BK_PATH_MAX] = malloc((size_t)(m->nn + 2) * BK_PATH_MAX);
	bk_stat_t *sts = malloc((size_t)(m->nn + 2) * sizeof *sts);
	if (!strcmp(p, "/") && m->usb_there) {
		strcpy(names[cnt], "usb");
		memset(&sts[cnt], 0, sizeof *sts); sts[cnt].dir = true;
		cnt++;
	}
	for (int i = 0; i < m->nn; i++) {
		char par[BK_PATH_MAX];
		parent_of(m->n[i].path, par);
		if (!ci_eq(par, p)) continue;
		strcpy(names[cnt], strrchr(m->n[i].path, '/') + 1);
		memset(&sts[cnt], 0, sizeof *sts);
		sts[cnt].dir = m->n[i].dir;
		sts[cnt].size = m->n[i].dir ? 0 : m->n[i].len;
		sts[cnt].fdate = m->n[i].fdate; sts[cnt].ftime = m->n[i].ftime;
		cnt++;
	}
	int r = 0;
	for (int i = 0; i < cnt && !r; i++) r = fn(arg, names[i], &sts[i]);
	free(names); free(sts);
	return r ? r : BK_OK;
}

static int f_unlink(void *c, const char *p) {
	memfs_t *m = c;
	int i = find(m, p);
	if (m->dead) return BK_ERR;
	if (i < 0) return BK_ENOENT;
	if (m->n[i].dir)
		for (int k = 0; k < m->nn; k++) {
			char par[BK_PATH_MAX];
			parent_of(m->n[k].path, par);
			if (ci_eq(par, p)) return BK_ERR;
		}
	bool half;
	if (!wop(m, p, &half)) return BK_ERR;
	if (half) {
		if (rnd(m) % 2) del(m, find(m, p));
		power_cut(m);
		return BK_ERR;
	}
	del(m, i);
	return BK_OK;
}

static int f_mkdir(void *c, const char *p) {
	memfs_t *m = c;
	if (m->dead) return BK_ERR;
	if (is_dir(m, p)) return BK_EEXIST;
	if (find(m, p) >= 0) return BK_ERR;
	char par[BK_PATH_MAX];
	parent_of(p, par);
	if (!is_dir(m, par)) return BK_ENOENT;
	bool half;
	if (!wop(m, p, &half)) return BK_ERR;
	if (half) {
		if (rnd(m) % 2) add(m, p, true);
		power_cut(m);
		return BK_ERR;
	}
	add(m, p, true);
	return BK_OK;
}

static int f_rename(void *c, const char *from, const char *to) {
	memfs_t *m = c;
	if (m->dead) return BK_ERR;
	int i = find(m, from);
	if (i < 0) return BK_ENOENT;
	if (find(m, to) >= 0) return BK_EEXIST;
	for (int k = 0; k < MAXH; k++) if (m->h[k].node == i && m->h[k].wr) return BK_EBUSY;
	bool half;
	if (!wop(m, from, &half)) return BK_ERR;
	if (half) {
		if (rnd(m) % 2) snprintf(m->n[i].path, BK_PATH_MAX, "%s", to);
		power_cut(m);
		return BK_ERR;
	}
	snprintf(m->n[i].path, BK_PATH_MAX, "%s", to);
	return BK_OK;
}

static int f_free_kb(void *c, const char *p, uint32_t *kb) {
	memfs_t *m = c;
	if (m->dead) return BK_ERR;
	bool usb = on_usb(p);
	uint32_t cap = usb ? m->usb_kb : m->card_kb, used = used_kb(m, usb);
	*kb = (cap > used ? cap - used : 0) + m->free_lie;
	return BK_OK;
}

static bool f_clock(void *c, bk_tm_t *t, uint32_t *up) {
	memfs_t *m = c;
	t->year = m->Y; t->month = m->M; t->day = m->D;
	t->hour = m->hh; t->min = m->mi; t->sec = m->ss;
	*up = m->uptime;
	return m->clock_set;
}

static void f_sleep(void *c, int ms) {
	memfs_t *m = c;
	(void)ms;
	memfs_tick(m, 1);
}

static void f_owner(void *c, char *buf, int len) {
	snprintf(buf, (size_t)len, "%s", ((memfs_t *)c)->owner);
}

static bool f_alive(void *c, const char *who) {
	return !strcmp(((memfs_t *)c)->owner, who);
}

static void f_out(void *c, const char *t) {
	memfs_t *m = c;
	size_t n = strlen(t);
	if (m->outlen + n + 1 > m->outcap) {
		m->outcap = (m->outlen + n + 1) * 2;
		m->out = realloc(m->out, m->outcap);
	}
	memcpy(m->out + m->outlen, t, n + 1);
	m->outlen += n;
	if (m->verbose) fputs(t, stdout);
}

static bool f_stop(void *c) {
	memfs_t *m = c;
	if (m->dead) return true;
	if (m->stop_after && ++m->stops >= m->stop_after) return true;
	return false;
}

void memfs_io(memfs_t *m, bk_io_t *io) {
	memset(io, 0, sizeof *io);
	io->ctx = m;
	io->open_read = f_open_read; io->open_write = f_open_write;
	io->read = f_read; io->write = f_write; io->seek = f_seek;
	io->sync = f_sync; io->close = f_close; io->stat = f_stat;
	io->list = f_list; io->unlink = f_unlink; io->mkdir = f_mkdir;
	io->rename = f_rename; io->free_kb = f_free_kb;
	io->clock = f_clock; io->sleep_ms = f_sleep;
	io->owner = f_owner; io->owner_alive = f_alive;
	io->out = f_out; io->stop = f_stop;
}

// -- the tests' side --------------------------------------------------------

memfs_t *memfs_new(uint32_t card_kb, uint32_t usb_kb) {
	memfs_t *m = calloc(1, sizeof *m);
	m->card_kb = card_kb; m->usb_kb = usb_kb;
	m->usb_there = true;
	m->rnd = 1;
	for (int k = 0; k < MAXH; k++) m->h[k].node = -1;
	snprintf(m->owner, sizeof m->owner, "test boot 1");
	memfs_clock(m, true, 2026, 10, 21, 11, 30, 0);
	return m;
}

void memfs_free(memfs_t *m) {
	if (!m) return;
	for (int i = 0; i < m->nn; i++) free(m->n[i].data);
	free(m->n);
	free(m->out);
	free(m);
}

memfs_t *memfs_clone(const memfs_t *s) {
	memfs_t *m = malloc(sizeof *m);
	*m = *s;
	m->n = malloc((size_t)(s->capn ? s->capn : 1) * sizeof *m->n);
	for (int i = 0; i < s->nn; i++) {
		m->n[i] = s->n[i];
		m->n[i].cap = s->n[i].len;
		m->n[i].data = s->n[i].len ? malloc(s->n[i].len) : NULL;
		if (s->n[i].len) memcpy(m->n[i].data, s->n[i].data, s->n[i].len);
	}
	m->out = NULL; m->outlen = m->outcap = 0;
	return m;
}

void memfs_clock(memfs_t *m, bool set, int Y, int M, int D, int h, int mi, int s) {
	m->clock_set = set;
	m->Y = Y; m->M = M; m->D = D; m->hh = h; m->mi = mi; m->ss = s;
}

void memfs_tick(memfs_t *m, int secs) {
	m->uptime += (uint32_t)secs;
	m->ss += secs;
	while (m->ss >= 60) { m->ss -= 60; m->mi++; }
	while (m->mi >= 60) { m->mi -= 60; m->hh++; }
	while (m->hh >= 24) { m->hh -= 24; m->D++; }
	while (m->D > 28) { m->D -= 28; m->M++; }   // a test calendar
	while (m->M > 12) { m->M -= 12; m->Y++; }
}

void memfs_mkdir_p(memfs_t *m, const char *p) {
	char b[BK_PATH_MAX];
	snprintf(b, sizeof b, "%s", p);
	for (char *s = b + 1;; s++) {
		if (*s == '/' || !*s) {
			char ch = *s;
			*s = 0;
			if (!is_dir(m, b) && find(m, b) < 0) add(m, b, true);
			*s = ch;
			if (!ch) break;
		}
	}
}

void memfs_put(memfs_t *m, const char *p, const void *data, uint32_t len) {
	char par[BK_PATH_MAX];
	parent_of(p, par);
	if (strcmp(par, "/")) memfs_mkdir_p(m, par);
	int i = find(m, p);
	if (i < 0) i = add(m, p, false);
	node_t *n = &m->n[i];
	free(n->data);
	n->data = malloc(len ? len : 1);
	memcpy(n->data, data, len);
	n->len = n->cap = len;
	stamp(m, &n->fdate, &n->ftime);
}

void memfs_puts(memfs_t *m, const char *p, const char *t) { memfs_put(m, p, t, (uint32_t)strlen(t)); }

const uint8_t *memfs_get(const memfs_t *m, const char *p, uint32_t *len) {
	int i = find(m, p);
	if (i < 0 || m->n[i].dir) return NULL;
	if (len) *len = m->n[i].len;
	return m->n[i].data ? m->n[i].data : (const uint8_t *)"";
}

bool memfs_rm(memfs_t *m, const char *p) {
	int i = find(m, p);
	if (i < 0) return false;
	del(m, i);
	return true;
}

bool memfs_exists(const memfs_t *m, const char *p) { return find(m, p) >= 0; }

bool memfs_poke(memfs_t *m, const char *p, uint32_t off, uint8_t v) {
	int i = find(m, p);
	if (i < 0 || off >= m->n[i].len) return false;
	m->n[i].data[off] = v;
	return true;
}

bool memfs_truncate(memfs_t *m, const char *p, uint32_t len) {
	int i = find(m, p);
	if (i < 0 || len > m->n[i].len) return false;
	m->n[i].len = len;
	return true;
}

void memfs_usb(memfs_t *m, bool there) { m->usb_there = there; }
void memfs_cut_after(memfs_t *m, int n) { m->cut_at = n ? m->ops + n : 0; m->rnd = (uint32_t)n * 2654435761u + 7; }
bool memfs_dead(const memfs_t *m) { return m->dead; }

void memfs_reboot(memfs_t *m) {
	m->dead = false;
	m->cut_at = 0;
	for (int k = 0; k < MAXH; k++) m->h[k].node = -1;
	m->clock_set = false;
	m->Y = 1970; m->M = 1; m->D = 1; m->hh = m->mi = m->ss = 0;
	m->uptime = 0;
	snprintf(m->owner, sizeof m->owner, "test boot after a cut");
	m->stop_after = m->stops = 0;
}

int memfs_drive_ops(const memfs_t *m) { return m->ops; }
int memfs_card_ops(const memfs_t *m) { return m->card_ops; }

bool memfs_card_same(const memfs_t *a, const memfs_t *b) {
	int na = 0, nb = 0;
	for (int i = 0; i < a->nn; i++) {
		if (on_usb(a->n[i].path)) continue;
		na++;
		int k = find(b, a->n[i].path);
		if (k < 0 || a->n[i].dir != b->n[k].dir) return false;
		if (!a->n[i].dir && (a->n[i].len != b->n[k].len ||
			(a->n[i].len && memcmp(a->n[i].data, b->n[k].data, a->n[i].len)) ||
			a->n[i].fdate != b->n[k].fdate || a->n[i].ftime != b->n[k].ftime)) return false;
	}
	for (int i = 0; i < b->nn; i++) if (!on_usb(b->n[i].path)) nb++;
	return na == nb;
}

void memfs_each(const memfs_t *m, void (*fn)(void *arg, const char *path,
		const uint8_t *data, uint32_t len, bool dir), void *arg) {
	for (int i = 0; i < m->nn; i++) fn(arg, m->n[i].path, m->n[i].data, m->n[i].len, m->n[i].dir);
}

void memfs_owner(memfs_t *m, const char *who) { snprintf(m->owner, sizeof m->owner, "%s", who); }
const char *memfs_out(const memfs_t *m) { return m->out ? m->out : ""; }
void memfs_out_clear(memfs_t *m) { m->outlen = 0; if (m->out) m->out[0] = 0; }
void memfs_verbose(memfs_t *m, bool v) { m->verbose = v; }
void memfs_stop_after(memfs_t *m, int n) { m->stop_after = n; m->stops = 0; }

void memfs_free_lie(memfs_t *m, uint32_t kb) { m->free_lie = kb; }
