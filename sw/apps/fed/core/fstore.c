/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zfed's store. docs/fed.md, "Storage"; the files in fstore.h.
 *
 * -- the order of writing, and what a crash leaves --
 *
 * An object is written log first, then its segment's index entry, then
 * the id table, then the state table. fstore_sync() then makes those
 * durable and moves the WATERMARK -- the position up to which the id and
 * state tables are known complete -- in store.hdr, rewritten whole and
 * renamed into place. So after a crash:
 *
 *   - a record at the end of the log that is not whole (it does not
 *     parse, or runs past the end) is not an object: the next one is
 *     written over it;
 *   - records in the log that the index lacks are indexed again;
 *   - everything past the watermark is applied to the tables again --
 *     safely, because applying a record twice changes nothing.
 *
 * -- the tables --
 *
 * Open addressing in a file, probed eight slots per read: opening a file
 * is the expensive part on a FAT card. The id table keeps an id's first
 * 16 bytes: a false "I have it" would need a second preimage of 128 bits.
 * An entry whose position has expired counts as absent. Either table is
 * rebuilt, twice the size, from the segments when it is 70% full.
 *
 * -- state --
 *
 * A state object replaces the one it is newer than (time, then id) for
 * the same topic, origin and key; the replaced record is marked
 * SUPERSEDED in its index entry, and is not sent to anyone again. State
 * objects do not expire: before a segment is dropped, the current state
 * objects in it are copied forward -- appended again, same bytes, same id.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "fstore.h"
#include "../../../common/zplat.h"
#include "../../../common/zsha256.h"

#define P_MAX 200
#define IDX_HDR 16
#define IDX_ENT 12
#define F_STATE 0x40000000u
#define F_SUPER 0x80000000u
#define F_CANCEL 0x20000000u		// cancelled (docs/fed.md, "Moderation"): not sent to anyone again
#define LEN_MASK 0x0000FFFFu		// objects are at most FOBJ_MAX (17,559) bytes
#define SRC_SHIFT 16				// bits 16-23: the peer slot that delivered it (0: here)
#define SRC_MASK 0x00FF0000u
#define TBL_HDR 16
#define PROBE 8
#define IDS_DEFAULT 16384u
#define STATE_SLOTS 1024u

typedef struct {
	const char *name;		// "ids.tbl", "state.tbl"
	uint32_t slot;			// bytes a slot: 24, 64
	uint32_t slots, used;
	const char *magic;
} tbl_t;

static tbl_t T_ids = { "ids.tbl", 24, 0, 0, "ZID1" };
static tbl_t T_st  = { "state.tbl", 64, 0, 0, "ZSS1" };

static char g_dir[160];
static fstore_retain_fn g_retain;
static uint64_t g_epoch;
static uint32_t g_next, g_water;
static uint32_t g_segs[FSTORE_MAX_SEGS];
static int g_nseg;
static uint32_t g_cur_created, g_cur_logend;
static uint8_t g_buf[FOBJ_MAX + 8];

// -- bytes and files --

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32); }

static void path(char *out, const char *rel) { snprintf(out, P_MAX, "%s/%s", g_dir, rel); }
static void seg_path(char *out, uint32_t first, const char *ext) { snprintf(out, P_MAX, "%s/seg/%08x.%s", g_dir, (unsigned)first, ext); }

static bool read_at(const char *p, uint32_t off, void *buf, uint32_t n) {
	int h = plat_open(p, PLAT_READ);
	if (h < 0) return false;
	bool ok = plat_seek(h, off) && plat_read(h, buf, (int)n) == (int)n;
	plat_close(h);
	return ok;
}

// Writes at off, making the file if it is not there.
static bool write_at(const char *p, uint32_t off, const void *buf, uint32_t n) {
	if (plat_size(p) < 0) { int c = plat_open(p, PLAT_CREATE); if (c < 0) return false; plat_close(c); }
	int h = plat_open(p, PLAT_UPDATE);
	if (h < 0) return false;
	bool ok = plat_seek(h, off) && plat_write(h, buf, (int)n) == (int)n;
	plat_close(h);
	return ok;
}

static bool sync_file(const char *p) {
	int h = plat_open(p, PLAT_UPDATE);
	if (h < 0) return false;
	bool ok = plat_sync(h);
	plat_close(h);
	return ok;
}

// A whole file written to <p>.new, made durable, renamed over p.
static bool write_whole(const char *p, const void *buf, uint32_t n) {
	char tmp[P_MAX + 8];
	snprintf(tmp, sizeof(tmp), "%s.new", p);
	int h = plat_open(tmp, PLAT_CREATE);
	if (h < 0) return false;
	bool ok = plat_write(h, buf, (int)n) == (int)n && plat_sync(h);
	plat_close(h);
	return ok && plat_rename(tmp, p);
}

// -- the header --

static bool hdr_write(void) {
	char p[P_MAX];
	uint8_t h[32];
	memset(h, 0, sizeof(h));
	memcpy(h, "ZST1", 4);
	put64(h + 4, g_epoch);
	put32(h + 12, g_water);
	put32(h + 16, g_next);
	path(p, "store.hdr");
	return write_whole(p, h, sizeof(h));
}

static bool hdr_read(void) {
	char p[P_MAX], q[P_MAX + 8];
	uint8_t h[32];
	path(p, "store.hdr");
	snprintf(q, sizeof(q), "%s.new", p);
	// a rename that did not finish (Zeitlos unlinks, then renames)
	if (plat_size(p) < 0 && plat_size(q) == 32) plat_rename(q, p);
	if (!read_at(p, 0, h, sizeof(h)) || memcmp(h, "ZST1", 4)) return false;
	g_epoch = get64(h + 4);
	g_water = get32(h + 12);
	g_next = get32(h + 16);
	return true;
}

// -- segments --

static int seg_find(uint32_t pos) {
	int lo = 0, hi = g_nseg - 1, best = -1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (g_segs[mid] <= pos) { best = mid; lo = mid + 1; } else hi = mid - 1;
	}
	return best;
}

static bool entry_read(uint32_t pos, uint32_t *off, uint32_t *lenf, uint32_t *exp) {
	int s = seg_find(pos);
	char p[P_MAX];
	uint8_t e[IDX_ENT];
	if (s < 0 || pos >= g_next) return false;
	seg_path(p, g_segs[s], "idx");
	if (!read_at(p, IDX_HDR + (pos - g_segs[s]) * IDX_ENT, e, IDX_ENT)) return false;
	*off = get32(e); *lenf = get32(e + 4); *exp = get32(e + 8);
	return true;
}

static bool entry_write(uint32_t pos, uint32_t off, uint32_t lenf, uint32_t exp) {
	int s = seg_find(pos);
	char p[P_MAX];
	uint8_t e[IDX_ENT];
	if (s < 0) return false;
	put32(e, off); put32(e + 4, lenf); put32(e + 8, exp);
	seg_path(p, g_segs[s], "idx");
	return write_at(p, IDX_HDR + (pos - g_segs[s]) * IDX_ENT, e, IDX_ENT);
}

static bool seg_new(uint32_t first, uint32_t now) {
	char p[P_MAX];
	uint8_t h[IDX_HDR];
	if (g_nseg >= FSTORE_MAX_SEGS) return false;
	memset(h, 0, sizeof(h));
	memcpy(h, "ZSI1", 4);
	put32(h + 4, first);
	put32(h + 8, now);
	seg_path(p, first, "log");
	{ int c = plat_open(p, PLAT_CREATE); if (c < 0) return false; plat_close(c); }
	seg_path(p, first, "idx");
	{ int c = plat_open(p, PLAT_CREATE); if (c < 0) return false; plat_close(c); }
	if (!write_at(p, 0, h, sizeof(h))) return false;
	g_segs[g_nseg++] = first;
	g_cur_created = now;
	g_cur_logend = 0;
	return true;
}

// The object at pos into g_buf; its length, or 0.
static uint32_t obj_read(uint32_t pos, uint32_t *lenf) {
	uint32_t off, lf, exp;
	char p[P_MAX];
	if (pos < fstore_first() || !entry_read(pos, &off, &lf, &exp)) return 0;
	uint32_t len = lf & LEN_MASK;
	if (len > FOBJ_MAX) return 0;
	seg_path(p, g_segs[seg_find(pos)], "log");
	if (!read_at(p, off + 4, g_buf, len)) return 0;
	if (lenf) *lenf = lf;
	return len;
}

// -- the tables --

static void tbl_path(char *out, const tbl_t *t, bool fresh) {
	char rel[32];
	snprintf(rel, sizeof(rel), "%s%s", t->name, fresh ? ".new" : "");
	path(out, rel);
}

static bool tbl_create(const tbl_t *t, uint32_t slots, bool fresh) {
	char p[P_MAX];
	uint8_t h[TBL_HDR];
	static const uint8_t zero[512] = { 0 };
	tbl_path(p, t, fresh);
	int c = plat_open(p, PLAT_CREATE);
	if (c < 0) return false;
	memset(h, 0, sizeof(h));
	memcpy(h, t->magic, 4);
	put32(h + 4, slots);
	bool ok = plat_write(c, h, TBL_HDR) == TBL_HDR;
	for (uint32_t left = slots * t->slot; ok && left; ) {
		uint32_t k = left < sizeof(zero) ? left : (uint32_t)sizeof(zero);
		ok = plat_write(c, zero, (int)k) == (int)k;
		left -= k;
	}
	plat_close(c);
	return ok;
}

// Finds `key` (klen bytes at the start of a slot): its slot index, and
// the slot's bytes in out. Else -1, with *free_i the first free slot
// (never used, or holding an expired position) along the probe.
static long tbl_find(const tbl_t *t, bool fresh, uint32_t slots, const uint8_t *key, uint32_t klen,
	uint8_t *out, long *free_i) {
	char p[P_MAX];
	uint8_t chunk[PROBE * 64];
	uint32_t i = get32(key) & (slots - 1);
	tbl_path(p, t, fresh);
	*free_i = -1;
	for (uint32_t seen = 0; seen < slots; ) {
		uint32_t k = PROBE;
		if (i + k > slots) k = slots - i;
		if (!read_at(p, TBL_HDR + i * t->slot, chunk, k * t->slot)) return -2;
		for (uint32_t j = 0; j < k; j++, seen++) {
			const uint8_t *s = chunk + j * t->slot;
			uint32_t pos = get32(s + 16);
			if (pos == 0) { if (*free_i < 0) *free_i = (long)(i + j); return -1; }
			if (!memcmp(s, key, klen)) { memcpy(out, s, t->slot); return (long)(i + j); }
			if (pos < fstore_first() && *free_i < 0) *free_i = (long)(i + j);
		}
		i = (i + k) & (slots - 1);
	}
	return -1;
}

static bool tbl_put_at(const tbl_t *t, bool fresh, long idx, const uint8_t *slot) {
	char p[P_MAX];
	tbl_path(p, t, fresh);
	return write_at(p, TBL_HDR + (uint32_t)idx * t->slot, slot, t->slot);
}

// Inserts or replaces the slot whose key is its first klen bytes.
// *grew: a never-used slot was taken.
static bool tbl_set(const tbl_t *t, bool fresh, uint32_t slots, const uint8_t *slot, uint32_t klen, bool *grew) {
	uint8_t old[64];
	long f, i = tbl_find(t, fresh, slots, slot, klen, old, &f);
	*grew = false;
	if (i == -2) return false;
	if (i < 0) {
		if (f < 0) return false;			// full: cannot happen below 70%
		// was the free slot never used (position 0)? -- for the load count
		uint8_t chk[64];
		char p[P_MAX];
		tbl_path(p, t, fresh);
		if (!read_at(p, TBL_HDR + (uint32_t)f * t->slot, chk, t->slot)) return false;
		*grew = get32(chk + 16) == 0;
		i = f;
	}
	return tbl_put_at(t, fresh, i, slot);
}

// Slots in use: one open, read straight through (at every start, so not
// one open per few slots -- thousands of opens is seconds on a card).
static uint32_t tbl_count(const tbl_t *t) {
	char p[P_MAX];
	uint8_t chunk[PROBE * 64];
	uint32_t used = 0;
	tbl_path(p, t, false);
	int h = plat_open(p, PLAT_READ);
	if (h < 0 || !plat_seek(h, TBL_HDR)) { if (h >= 0) plat_close(h); return 0; }
	for (uint32_t i = 0; i < t->slots; i += PROBE) {
		uint32_t k = t->slots - i < PROBE ? t->slots - i : PROBE;
		if (plat_read(h, chunk, (int)(k * t->slot)) != (int)(k * t->slot)) break;
		for (uint32_t j = 0; j < k; j++) if (get32(chunk + j * t->slot + 16)) used++;
	}
	plat_close(h);
	return used;
}

static bool tbl_load(tbl_t *t) {
	char p[P_MAX];
	uint8_t h[TBL_HDR];
	tbl_path(p, t, false);
	if (!read_at(p, 0, h, TBL_HDR) || memcmp(h, t->magic, 4)) return false;
	t->slots = get32(h + 4);
	if (!t->slots || (t->slots & (t->slots - 1))) return false;
	if (plat_size(p) != (int32_t)(TBL_HDR + t->slots * t->slot)) return false;
	t->used = tbl_count(t);
	return true;
}

// A state object's key: SHA-256 of topic, NUL, origin, key -- 16 bytes.
static void state_key_of(const char *topic, const uint8_t origin[32], const char *key, uint8_t out[16]) {
	z_sha256_ctx c;
	uint8_t h[32], z = 0;
	z_sha256_init(&c);
	z_sha256_update(&c, topic, (uint32_t)strlen(topic));
	z_sha256_update(&c, &z, 1);
	z_sha256_update(&c, origin, 32);
	z_sha256_update(&c, key, (uint32_t)strlen(key));
	z_sha256_final(&c, h);
	memcpy(out, h, 16);
}
static void state_key(const fobj_t *o, uint8_t out[16]) {
	state_key_of(o->topic, o->origin, o->key, out);
}

static bool ids_set(const uint8_t id[32], uint32_t pos, bool fresh, uint32_t slots, uint32_t *used) {
	uint8_t s[24];
	bool grew;
	memset(s, 0, sizeof(s));
	memcpy(s, id, 16);
	put32(s + 16, pos);
	if (!tbl_set(&T_ids, fresh, slots, s, 16, &grew)) return false;
	if (grew) (*used)++;
	return true;
}

static bool st_set(const uint8_t key[16], uint32_t pos, uint64_t time, const uint8_t id[32], bool fresh, uint32_t slots, uint32_t *used) {
	uint8_t s[64];
	bool grew;
	memset(s, 0, sizeof(s));
	memcpy(s, key, 16);
	put32(s + 16, pos);
	put64(s + 24, time);
	memcpy(s + 32, id, 32);
	if (!tbl_set(&T_st, fresh, slots, s, 16, &grew)) return false;
	if (grew) (*used)++;
	return true;
}

// Rebuilds a table, `slots` big, from the segments; renamed into place.
static bool tbl_rebuild(tbl_t *t, uint32_t slots) {
	char a[P_MAX], b[P_MAX];
	uint32_t used = 0;
	if (!tbl_create(t, slots, true)) return false;
	for (uint32_t pos = fstore_first(); pos < g_next; pos++) {
		uint32_t lf, len = obj_read(pos, &lf);
		fobj_t o;
		if (!len || fobj_parse(g_buf + 0, len, 0, &o, NULL)) continue;
		if (t == &T_ids) { if (!ids_set(o.id, pos, true, slots, &used)) return false; }
		else if ((lf & F_STATE) && !(lf & (F_SUPER | F_CANCEL))) {
			uint8_t k[16];
			state_key(&o, k);
			if (!st_set(k, pos, o.time, o.id, true, slots, &used)) return false;
		}
	}
	tbl_path(a, t, true);
	tbl_path(b, t, false);
	if (!sync_file(a) || !plat_rename(a, b)) return false;
	t->slots = slots;
	t->used = used;
	return true;
}

static bool maybe_grow(tbl_t *t) {
	if (t->used * 10u <= t->slots * 7u) return true;
	return tbl_rebuild(t, t->slots * 2);
}

// -- applying a record to the tables (idempotent) --

static bool apply(uint32_t pos, const fobj_t *o, uint32_t lenf) {
	if (!ids_set(o->id, pos, false, T_ids.slots, &T_ids.used)) return false;
	if ((lenf & F_STATE) && !(lenf & F_SUPER)) {
		uint8_t k[16], cur[64];
		long f, i;
		state_key(o, k);
		i = tbl_find(&T_st, false, T_st.slots, k, 16, cur, &f);
		if (i == -2) return false;
		if (i >= 0) {
			uint32_t cpos = get32(cur + 16);
			uint64_t ct = get64(cur + 24);
			int c = (ct > o->time) ? 1 : (ct < o->time) ? -1 : memcmp(cur + 32, o->id, 32);
			if (cpos == pos) return true;
			if (c > 0 || (c == 0 && cpos > pos)) {
				// the table's is newer: this record is superseded
				uint32_t off, lf, exp;
				if (entry_read(pos, &off, &lf, &exp)) entry_write(pos, off, lf | F_SUPER, exp);
				return true;
			}
			if (c < 0 && cpos >= fstore_first()) {
				uint32_t off, lf, exp;
				if (entry_read(cpos, &off, &lf, &exp)) entry_write(cpos, off, lf | F_SUPER, exp);
			}
		}
		if (!st_set(k, pos, o->time, o->id, false, T_st.slots, &T_st.used)) return false;
	}
	return maybe_grow(&T_ids) && maybe_grow(&T_st);
}

// -- appending --

static int append(const uint8_t *obj, uint32_t n, uint32_t flags, uint32_t exp, uint32_t now, uint32_t *pos) {
	char p[P_MAX];
	uint8_t len4[4];
	int s = g_nseg - 1;
	uint32_t count = g_next - g_segs[s];
	if (count && (g_cur_logend + 4 + n > FSTORE_SEG_BYTES || (now > g_cur_created && now - g_cur_created > FSTORE_SEG_SECONDS))) {
		if (!seg_new(g_next, now)) return -FSTORE_E_FULL;
		s = g_nseg - 1;
	}
	put32(len4, n);
	seg_path(p, g_segs[s], "log");
	{
		if (plat_size(p) < 0) return -FSTORE_E_IO;
		int h = plat_open(p, PLAT_UPDATE);
		if (h < 0) return -FSTORE_E_IO;
		bool ok = plat_seek(h, g_cur_logend) && plat_write(h, len4, 4) == 4 && plat_write(h, obj, (int)n) == (int)n;
		plat_close(h);
		if (!ok) return -FSTORE_E_IO;
	}
	*pos = g_next;
	g_next++;
	if (!entry_write(*pos, g_cur_logend, n | flags, exp)) { g_next--; return -FSTORE_E_IO; }
	g_cur_logend += 4 + n;
	return 0;
}

// -- the interface --

uint64_t fstore_epoch(void) { return g_epoch; }
uint32_t fstore_last(void) { return g_next ? g_next - 1 : 0; }
uint32_t fstore_first(void) { return g_nseg ? g_segs[0] : 1; }

bool fstore_have(const uint8_t id[32]) {
	uint8_t s[64];
	long f, i = tbl_find(&T_ids, false, T_ids.slots, id, 16, s, &f);
	return i >= 0 && get32(s + 16) >= fstore_first() && get32(s + 16) < g_next;
}

int fstore_put(const uint8_t *obj, uint32_t n, uint32_t now, uint32_t *pos) {
	return fstore_put_from(obj, n, now, 0, pos);
}

int fstore_put_from(const uint8_t *obj, uint32_t n, uint32_t now, uint8_t slot, uint32_t *pos) {
	fobj_t o;
	uint32_t exp, flags = 0, retain;
	if (n > FOBJ_MAX || fobj_parse(obj, n, 0, &o, NULL) || o.size != n) return -FSTORE_E_OBJ;
	if (o.kind == FOBJ_STATE) { exp = 0xFFFFFFFFu; flags = F_STATE; }
	else {
		retain = g_retain ? g_retain(o.topic) : FSTORE_RETAIN;
		exp = o.time + retain > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)(o.time + retain);
		if (exp <= now) return FSTORE_EXPIRED;
	}
	if (fstore_have(o.id)) return FSTORE_HAVE;
	if (o.kind == FOBJ_STATE) {
		uint8_t k[16], cur[64];
		long f, i;
		state_key(&o, k);
		i = tbl_find(&T_st, false, T_st.slots, k, 16, cur, &f);
		if (i == -2) return -FSTORE_E_IO;
		if (i >= 0 && get32(cur + 16) >= fstore_first()) {
			uint64_t ct = get64(cur + 24);
			if (ct > o.time || (ct == o.time && memcmp(cur + 32, o.id, 32) >= 0)) return FSTORE_STALE;
		}
	}
	flags |= (uint32_t)slot << SRC_SHIFT;
	int r = append(obj, n, flags, exp, now, pos);
	if (r < 0) return r;
	if (!apply(*pos, &o, n | flags)) return -FSTORE_E_IO;
	return FSTORE_NEW;
}

int fstore_state_get(const char *topic, const uint8_t origin[32], const char *key, uint8_t *buf, uint32_t cap) {
	uint8_t k[16], cur[64];
	long f, i;
	state_key_of(topic, origin, key, k);
	i = tbl_find(&T_st, false, T_st.slots, k, 16, cur, &f);
	if (i == -2) return -FSTORE_E_IO;
	if (i < 0) return 0;
	uint32_t off, lf, exp;
	if (entry_read(get32(cur + 16), &off, &lf, &exp) && (lf & F_CANCEL)) return 0;	// cancelled: as if absent
	return fstore_get(get32(cur + 16), buf, cap);
}

// An id's position, if it is held; 0 if not.
static uint32_t id_pos(const uint8_t id[32]) {
	uint8_t s[64];
	long f, i = tbl_find(&T_ids, false, T_ids.slots, id, 16, s, &f);
	if (i < 0) return 0;
	uint32_t p = get32(s + 16);
	return p >= fstore_first() && p < g_next ? p : 0;
}

int fstore_get_id(const uint8_t id[32], uint8_t *buf, uint32_t cap) {
	uint32_t p = id_pos(id), off, lf, exp;
	if (!p || !entry_read(p, &off, &lf, &exp) || (lf & F_CANCEL)) return 0;
	return fstore_get(p, buf, cap);
}

int fstore_cancel(const uint8_t id[32]) {
	uint32_t p = id_pos(id), off, lf, exp;
	if (!p || !entry_read(p, &off, &lf, &exp)) return 0;
	if (lf & F_CANCEL) return 0;
	return entry_write(p, off, lf | F_CANCEL, exp) ? 1 : -FSTORE_E_IO;
}

bool fstore_cancelled(const uint8_t id[32]) {
	uint32_t p = id_pos(id), off, lf, exp;
	return p && entry_read(p, &off, &lf, &exp) && (lf & F_CANCEL);
}

int fstore_get(uint32_t pos, uint8_t *buf, uint32_t cap) {
	uint32_t len = obj_read(pos, NULL);
	if (!len) return 0;
	if (len > cap) return -FSTORE_E_FULL;
	memcpy(buf, g_buf, len);
	return (int)len;
}

uint32_t fstore_next_for(uint32_t after, uint8_t slot) {
	for (uint32_t p = after + 1 < fstore_first() ? fstore_first() : after + 1; p < g_next; p++) {
		uint32_t off, lf, exp;
		if (!entry_read(p, &off, &lf, &exp)) return 0;
		if (lf & (F_SUPER | F_CANCEL)) continue;
		if (slot && ((lf & SRC_MASK) >> SRC_SHIFT) == slot) continue;
		return p;
	}
	return 0;
}

uint32_t fstore_next(uint32_t after) {
	return fstore_next_for(after, 0);
}

int fstore_sync(void) {
	char p[P_MAX];
	int s = g_nseg - 1;
	seg_path(p, g_segs[s], "log"); if (!sync_file(p)) return -FSTORE_E_IO;
	seg_path(p, g_segs[s], "idx"); if (!sync_file(p)) return -FSTORE_E_IO;
	path(p, T_ids.name); if (!sync_file(p)) return -FSTORE_E_IO;
	path(p, T_st.name); if (!sync_file(p)) return -FSTORE_E_IO;
	g_water = g_next - 1;
	return hdr_write() ? 0 : -FSTORE_E_IO;
}

// Brings the newest segment's index up to its log: entries that point
// past the log dropped, whole records the index lacks added. The next
// position follows.
static bool recover_current(uint32_t now) {
	char lp[P_MAX], ip[P_MAX];
	uint8_t h[IDX_HDR];
	int s = g_nseg - 1;
	uint32_t first = g_segs[s];
	seg_path(lp, first, "log");
	seg_path(ip, first, "idx");
	int32_t lsz = plat_size(lp), isz = plat_size(ip);
	if (lsz < 0) { int c = plat_open(lp, PLAT_CREATE); if (c < 0) return false; plat_close(c); lsz = 0; }
	if (isz < IDX_HDR || !read_at(ip, 0, h, IDX_HDR) || memcmp(h, "ZSI1", 4)) {
		memset(h, 0, sizeof(h)); memcpy(h, "ZSI1", 4); put32(h + 4, first); put32(h + 8, now);
		if (!write_at(ip, 0, h, IDX_HDR)) return false;
		isz = IDX_HDR;
	}
	g_cur_created = get32(h + 8);
	uint32_t n = (uint32_t)(isz - IDX_HDR) / IDX_ENT;
	g_next = first + n;
	g_cur_logend = 0;
	while (n > 0) {
		uint32_t off, lf, exp;
		if (!entry_read(first + n - 1, &off, &lf, &exp)) return false;
		if ((uint64_t)off + 4 + (lf & LEN_MASK) <= (uint64_t)lsz) { g_cur_logend = off + 4 + (lf & LEN_MASK); break; }
		n--;
		g_next = first + n;
	}
	g_next = first + n;
	// whole records the index lacks
	while (g_cur_logend + 4 <= (uint32_t)lsz) {
		uint8_t len4[4];
		fobj_t o;
		if (!read_at(lp, g_cur_logend, len4, 4)) break;
		uint32_t len = get32(len4);
		if (len > FOBJ_MAX || g_cur_logend + 4 + len > (uint32_t)lsz) break;
		if (!read_at(lp, g_cur_logend + 4, g_buf, len)) break;
		if (fobj_parse(g_buf, len, 0, &o, NULL) || o.size != len) break;
		uint32_t flags = o.kind == FOBJ_STATE ? F_STATE : 0, exp;
		if (o.kind == FOBJ_STATE) exp = 0xFFFFFFFFu;
		else {
			uint32_t retain = g_retain ? g_retain(o.topic) : FSTORE_RETAIN;
			exp = o.time + retain > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)(o.time + retain);
		}
		g_next++;
		if (!entry_write(g_next - 1, g_cur_logend, len | flags, exp)) return false;
		g_cur_logend += 4 + len;
	}
	return true;
}

static int cmp_u32(const void *a, const void *b) {
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return x < y ? -1 : x > y;
}

int fstore_open(const char *dir, uint32_t now, fstore_retain_fn retain, uint32_t ids_slots) {
	char p[P_MAX];
	static char names[FSTORE_MAX_SEGS * 16];
	snprintf(g_dir, sizeof(g_dir), "%s", dir);
	g_retain = retain;
	g_nseg = 0;
	if (!plat_mkdir(g_dir)) return -FSTORE_E_IO;
	path(p, "seg");
	if (!plat_mkdir(p)) return -FSTORE_E_IO;

	bool fresh = !hdr_read();
	if (fresh) {
		plat_random(&g_epoch, sizeof(g_epoch));
		g_water = 0;
		g_next = 1;
	}
	// the segments, from their names
	int k = plat_list(p, names, sizeof(names));
	for (char *q = names; k-- > 0; q += strlen(q) + 1) {
		size_t l = strlen(q);
		if (l == 12 && !strcmp(q + 8, ".idx") && g_nseg < FSTORE_MAX_SEGS)
			g_segs[g_nseg++] = (uint32_t)strtoul(q, NULL, 16);
	}
	qsort(g_segs, (size_t)g_nseg, sizeof(g_segs[0]), cmp_u32);
	if (!g_nseg && !seg_new(g_next, now)) return -FSTORE_E_IO;
	if (!recover_current(now)) return -FSTORE_E_IO;

	// the tables: made if missing, rebuilt if unreadable
	if (!tbl_load(&T_ids)) {
		uint32_t slots = ids_slots ? ids_slots : IDS_DEFAULT;
		if (!tbl_create(&T_ids, slots, false) || !tbl_load(&T_ids)) return -FSTORE_E_IO;
		if (!fresh) g_water = 0;
	}
	if (!tbl_load(&T_st)) {
		if (!tbl_create(&T_st, STATE_SLOTS, false) || !tbl_load(&T_st)) return -FSTORE_E_IO;
		if (!fresh) g_water = 0;
	}
	// everything past the watermark, applied again
	for (uint32_t pos = (g_water + 1 > fstore_first() ? g_water + 1 : fstore_first()); pos < g_next; pos++) {
		uint32_t lf, len = obj_read(pos, &lf);
		fobj_t o;
		if (!len || fobj_parse(g_buf, len, 0, &o, NULL)) continue;
		if (!apply(pos, &o, lf)) return -FSTORE_E_IO;
	}
	return fstore_sync();
}

void fstore_close(void) {
	fstore_sync();
	g_nseg = 0;
}

int fstore_expire(uint32_t now) {
	int dropped = 0;
	while (g_nseg >= 2) {
		uint32_t first = g_segs[0], end = g_segs[1];
		bool all = true;
		for (uint32_t pos = first; pos < end && all; pos++) {
			uint32_t off, lf, exp;
			if (!entry_read(pos, &off, &lf, &exp)) return dropped;
			if (!(lf & F_STATE) && exp > now) all = false;
		}
		if (!all) break;
		// current state objects: copied forward first
		for (uint32_t pos = first; pos < end; pos++) {
			uint32_t lf, len = obj_read(pos, &lf), np;
			fobj_t o;
			if (!len || !(lf & F_STATE) || (lf & (F_SUPER | F_CANCEL))) continue;	// cancelled: not kept
			static uint8_t copy[FOBJ_MAX];
			memcpy(copy, g_buf, len);
			if (fobj_parse(copy, len, 0, &o, NULL)) continue;
			if (append(copy, len, F_STATE, 0xFFFFFFFFu, now, &np) < 0) return dropped;
			uint8_t k[16];
			state_key(&o, k);
			if (!ids_set(o.id, np, false, T_ids.slots, &T_ids.used)) return dropped;
			if (!st_set(k, np, o.time, o.id, false, T_st.slots, &T_st.used)) return dropped;
		}
		if (fstore_sync() < 0) return dropped;
		char p[P_MAX];
		seg_path(p, first, "log"); plat_unlink(p);
		seg_path(p, first, "idx"); plat_unlink(p);
		memmove(g_segs, g_segs + 1, (size_t)(g_nseg - 1) * sizeof(g_segs[0]));
		g_nseg--;
		dropped++;
	}
	return dropped;
}

// -- cursors and consumers: small text files, rewritten whole --

static int kv_load(const char *file, char *buf, uint32_t cap) {
	char p[P_MAX];
	path(p, file);
	int h = plat_open(p, PLAT_READ);
	if (h < 0) { buf[0] = 0; return 0; }
	int n = plat_read(h, buf, (int)cap - 1);
	plat_close(h);
	if (n < 0) n = 0;
	buf[n] = 0;
	return n;
}

// Replaces the line starting with `key ` (or adds it) with `line`.
static int kv_set(const char *file, const char *key, const char *line) {
	static char in[8192], out[8192];
	char p[P_MAX];
	uint32_t o = 0, kl = (uint32_t)strlen(key);
	kv_load(file, in, sizeof(in));
	for (char *s = in; *s; ) {
		char *e = strchr(s, '\n');
		uint32_t l = e ? (uint32_t)(e - s) : (uint32_t)strlen(s);
		if (!(l > kl && !strncmp(s, key, kl) && s[kl] == ' ') && l) {
			if (o + l + 2 >= sizeof(out)) return -FSTORE_E_FULL;
			memcpy(out + o, s, l); out[o + l] = '\n'; o += l + 1;
		}
		s += l + (e ? 1 : 0);
	}
	uint32_t ll = (uint32_t)strlen(line);
	if (o + ll + 2 >= sizeof(out)) return -FSTORE_E_FULL;
	memcpy(out + o, line, ll); out[o + ll] = '\n'; o += ll + 1;
	path(p, file);
	return write_whole(p, out, o) ? 0 : -FSTORE_E_IO;
}

static const char *kv_get(const char *file, const char *key) {
	static char in[8192];
	uint32_t kl = (uint32_t)strlen(key);
	kv_load(file, in, sizeof(in));
	for (char *s = in; *s; ) {
		char *e = strchr(s, '\n');
		if (!strncmp(s, key, kl) && s[kl] == ' ') { if (e) *e = 0; return s + kl + 1; }
		if (!e) break;
		s = e + 1;
	}
	return NULL;
}

bool fstore_cursor(const uint8_t peer[32], uint64_t *epoch, uint32_t *pos) {
	char key[65];
	uint64_t e;
	unsigned long v;
	fobj_hex(peer, 32, key);
	// "<16 hex> <decimal>": by hand, never 64 bits through scanf (fobj.h)
	const char *s = kv_get("cursors.txt", key);
	char *end;
	if (!s || !fobj_hex_u64(s, &e) || s[16] != ' ') return false;
	v = strtoul(s + 17, &end, 10);
	if (end == s + 17) return false;
	*epoch = e;
	*pos = (uint32_t)v;
	return true;
}

int fstore_set_cursor(const uint8_t peer[32], uint64_t epoch, uint32_t pos) {
	char key[65], line[120], eh[17];
	fobj_hex(peer, 32, key);
	fobj_u64_hex(eh, epoch);
	snprintf(line, sizeof(line), "%s %s %u", key, eh, (unsigned)pos);
	return kv_set("cursors.txt", key, line);
}

int fstore_clear_cursors(void) {
	char p[P_MAX];
	path(p, "cursors.txt");
	plat_unlink(p);				// none: every peer is asked from the start
	return 0;
}

uint32_t fstore_consumer(const char *name) {
	const char *s = kv_get("consumers.txt", name);
	return s ? (uint32_t)strtoul(s, NULL, 10) : 0;
}

int fstore_set_consumer(const char *name, uint32_t pos) {
	char line[120];
	snprintf(line, sizeof(line), "%s %lu", name, (unsigned long)pos);
	return kv_set("consumers.txt", name, line);
}

// -- peer slots: peers.txt, "<key hex> <slot> <epoch hex>" lines, and
// "next <n>" -- the next slot never yet given. A slot is never given
// twice: objects marked with it would otherwise be skipped for a peer
// that never sent them.

uint8_t fstore_peer_slot(const uint8_t peer[32], uint64_t epoch) {
	char key[65], line[120];
	unsigned slot = 0;
	uint64_t e;
	char *end, eh[17];
	fobj_hex(peer, 32, key);
	// "<slot> <16 hex>": by hand, never 64 bits through scanf (fobj.h)
	const char *s = kv_get("peers.txt", key);
	if (s) {
		slot = (unsigned)strtoul(s, &end, 10);
		if (end != s && *end == ' ' && fobj_hex_u64(end + 1, &e) && e == epoch && slot >= 1 && slot <= 255) return (uint8_t)slot;
	}
	const char *nx = kv_get("peers.txt", "next");
	unsigned next = nx ? (unsigned)strtoul(nx, NULL, 10) : 1;
	if (next > 255) return 0;					// none left: no skipping, never a wrong one
	snprintf(line, sizeof(line), "next %u", next + 1);
	if (kv_set("peers.txt", "next", line) < 0) return 0;
	fobj_u64_hex(eh, epoch);
	snprintf(line, sizeof(line), "%s %u %s", key, next, eh);
	if (kv_set("peers.txt", key, line) < 0) return 0;
	return (uint8_t)next;
}
