/*
 * Zeitlos OS
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The flash key/value store's log. See kvlog.h for the contract and
 * docs/kvstore.md for the design and the power-loss argument.
 *
 * -- on flash --
 *
 * Sector header, 16 bytes at the start of each 4 KB sector:
 *
 *   0  "ZKV1"
 *   4  generation, little-endian
 *   8  ~generation
 *  12  reserved, left erased
 *
 * Valid only if the magic matches AND the two generation words are
 * exact complements. Programming only clears bits, so a header that was
 * cut off mid-program has some position where both words still read 1
 * and fails the complement test: a valid header is a COMPLETE header.
 *
 * Record, from offset 16, each padded to 4 bytes with 0xFF:
 *
 *   0  key length, 1..31        (0xFF: the end of the log)
 *   1  type: 0x5A set, 0xD1 delete
 *   2  value length, 0..256, little-endian (0 for a delete)
 *   4  CRC-32 over bytes 0..3, the key and the value
 *   8  key, then value
 *
 * A record whose CRC fails is skipped (a write that was cut off). A
 * record whose header cannot be parsed ends the scan and marks the log
 * dirty: nothing after it can be found reliably, so the next write
 * compacts instead of appending.
 */

#include <string.h>
#include "kvlog.h"

#define T_SET   0x5Au
#define T_DEL   0xD1u

#define ST_VALID  1u
#define ST_LIVE   2u
#define ST_DEL    4u

static const uint8_t magic[4] = { 'Z', 'K', 'V', '1' };

typedef struct {
	uint32_t off;          // within the region
	uint32_t size;         // padded
	uint32_t crc;
	uint16_t vlen;
	uint8_t klen;
	uint8_t type;
} rec_t;

static uint32_t rd32(const kv_dev_t *d, uint32_t off) {
	return (uint32_t)d->read(d->ctx, off) |
		((uint32_t)d->read(d->ctx, off + 1) << 8) |
		((uint32_t)d->read(d->ctx, off + 2) << 16) |
		((uint32_t)d->read(d->ctx, off + 3) << 24);
}

// Bitwise rather than table-driven: 1 KB of table is a real cost in a
// kernel image with a hard 256 KB ceiling, and a record is at most 295
// bytes.
static uint32_t crc_byte(uint32_t c, uint8_t b) {
	int k;
	c ^= b;
	for (k = 0; k < 8; k++)
		c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
	return c;
}

static uint8_t hash_byte(uint8_t h, uint8_t c) {
	return (uint8_t)(h * 31u + c);
}

static uint32_t rec_size(uint32_t klen, uint32_t vlen) {
	return (KV_REC_HDR + klen + vlen + 3u) & ~3u;
}

int kv_key_ok(const char *key) {
	uint32_t n = 0;
	if (!key) return 0;
	for (; key[n]; n++) {
		char c = key[n];
		if (n >= KV_KEY_MAX) return 0;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				(c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
			return 0;
	}
	return n > 0;
}

static uint8_t key_hash(const char *key) {
	uint8_t h = 0;
	while (*key) h = hash_byte(h, (uint8_t)*key++);
	return h;
}

// -- sectors --

static int hdr_gen(const kv_dev_t *d, int s, uint32_t *gen) {
	uint32_t base = (uint32_t)s * KV_SECTOR, g, ng;
	int i;
	for (i = 0; i < 4; i++)
		if (d->read(d->ctx, base + (uint32_t)i) != magic[i]) return 0;
	g = rd32(d, base + 4);
	ng = rd32(d, base + 8);
	if ((g ^ ng) != 0xFFFFFFFFu) return 0;
	*gen = g;
	return 1;
}

// The valid sector with the higher generation (serial-number order, so
// a wrap is harmless), or -1.
static int active_sector(const kv_dev_t *d, uint32_t *gen) {
	uint32_t g0 = 0, g1 = 0;
	int v0 = hdr_gen(d, 0, &g0), v1 = hdr_gen(d, 1, &g1);
	if (v0 && v1) {
		if ((int32_t)(g1 - g0) > 0) { *gen = g1; return 1; }
		*gen = g0;
		return 0;
	}
	if (v0) { *gen = g0; return 0; }
	if (v1) { *gen = g1; return 1; }
	return -1;
}

// -- records --

// 1: a record header that parses, 0: the end of the log, -1: garbage
static int rec_hdr(const kv_dev_t *d, uint32_t base, uint32_t off, rec_t *r) {
	uint8_t k;
	if (off >= KV_SECTOR) return 0;
	k = d->read(d->ctx, base + off);
	if (k == 0xFF) return 0;
	if (off + KV_REC_HDR > KV_SECTOR) return -1;
	r->off = base + off;
	r->klen = k;
	r->type = d->read(d->ctx, base + off + 1);
	r->vlen = (uint16_t)(d->read(d->ctx, base + off + 2) |
		(d->read(d->ctx, base + off + 3) << 8));
	if (k == 0 || k > KV_KEY_MAX || r->vlen > KV_VAL_MAX) return -1;
	if (r->type == T_DEL) {
		if (r->vlen) return -1;
	} else if (r->type != T_SET)
		return -1;
	r->size = rec_size(k, r->vlen);
	if (off + r->size > KV_SECTOR) return -1;
	r->crc = rd32(d, base + off + 4);
	return 1;
}

// CRC check against the flash; the key's hash on the side.
static int rec_valid(const kv_dev_t *d, const rec_t *r, uint8_t *hash) {
	uint32_t c = 0xFFFFFFFFu, i;
	uint8_t h = 0;
	for (i = 0; i < 4; i++) c = crc_byte(c, d->read(d->ctx, r->off + i));
	for (i = 0; i < r->klen; i++) {
		uint8_t b = d->read(d->ctx, r->off + KV_REC_HDR + i);
		c = crc_byte(c, b);
		h = hash_byte(h, b);
	}
	for (i = 0; i < r->vlen; i++)
		c = crc_byte(c, d->read(d->ctx, r->off + KV_REC_HDR + r->klen + i));
	if (hash) *hash = h;
	return ~c == r->crc;
}

static int key_eq(const kv_dev_t *d, const rec_t *r, const char *key,
		uint32_t klen) {
	uint32_t i;
	if (r->klen != klen) return 0;
	for (i = 0; i < klen; i++)
		if (d->read(d->ctx, r->off + KV_REC_HDR + i) != (uint8_t)key[i]) return 0;
	return 1;
}

static int keys_eq(const kv_dev_t *d, uint32_t a, uint32_t b) {
	uint8_t n = d->read(d->ctx, a), i;
	if (d->read(d->ctx, b) != n) return 0;
	for (i = 0; i < n; i++)
		if (d->read(d->ctx, a + KV_REC_HDR + i) != d->read(d->ctx, b + KV_REC_HDR + i))
			return 0;
	return 1;
}

// -- the index (writers and enumeration only) --

typedef struct {
	int s;                 // active sector, or -1
	uint32_t gen;
	uint32_t end;          // offset within the sector where the log ends
	int dirty;
} scan_t;

static void scan(const kv_dev_t *d, kv_work_t *w, scan_t *sc) {
	uint32_t base, off = KV_HDR_LEN, i, j;
	rec_t r;
	int rc;

	w->n = 0;
	sc->dirty = 0;
	sc->end = KV_HDR_LEN;
	sc->s = active_sector(d, &sc->gen);
	if (sc->s < 0) return;
	base = (uint32_t)sc->s * KV_SECTOR;

	while ((rc = rec_hdr(d, base, off, &r)) > 0 && w->n < KV_MAX_RECS) {
		uint8_t h = 0;
		uint8_t st = rec_valid(d, &r, &h) ? ST_VALID : 0;
		if (r.type == T_DEL) st |= ST_DEL;
		w->off[w->n] = (uint16_t)off;
		w->hash[w->n] = h;
		w->state[w->n] = st;
		w->n++;
		off += r.size;
	}
	if (rc < 0) sc->dirty = 1;
	sc->end = off;

	// Live: valid, not a delete, and no later valid record for the key.
	for (i = 0; i < w->n; i++) {
		int live;
		if (!(w->state[i] & ST_VALID)) continue;
		live = 1;
		for (j = i + 1; j < w->n && live; j++)
			if ((w->state[j] & ST_VALID) && w->hash[j] == w->hash[i] &&
					keys_eq(d, base + w->off[i], base + w->off[j]))
				live = 0;
		if (live && !(w->state[i] & ST_DEL)) w->state[i] |= ST_LIVE;
	}
}

// The live record for `key`, or -1.
static int find_live(const kv_dev_t *d, const kv_work_t *w, const scan_t *sc,
		const char *key) {
	uint32_t base = (uint32_t)sc->s * KV_SECTOR, klen = (uint32_t)strlen(key);
	uint8_t h = key_hash(key);
	rec_t r;
	int i;
	for (i = 0; i < (int)w->n; i++) {
		if (!(w->state[i] & ST_LIVE) || w->hash[i] != h) continue;
		if (rec_hdr(d, base, w->off[i], &r) > 0 && key_eq(d, &r, key, klen))
			return i;
	}
	return -1;
}

// -- writing --

static int prog(const kv_dev_t *d, uint32_t off, const uint8_t *p, uint32_t n) {
	uint32_t done = 0, i;
	while (done < n) {
		uint32_t o = off + done, chunk = KV_PAGE - (o % KV_PAGE);
		if (chunk > n - done) chunk = n - done;
		if (d->program(d->ctx, o, p + done, chunk)) return KV_EIO;
		done += chunk;
	}
	for (i = 0; i < n; i++)
		if (d->read(d->ctx, off + i) != p[i]) return KV_EIO;
	return KV_OK;
}

static int blank(const kv_dev_t *d, uint32_t off, uint32_t n) {
	uint32_t i;
	for (i = 0; i < n; i++)
		if (d->read(d->ctx, off + i) != 0xFF) return 0;
	return 1;
}

static int erase_sector(const kv_dev_t *d, int s) {
	uint32_t base = (uint32_t)s * KV_SECTOR;
	if (d->erase(d->ctx, base)) return KV_EIO;
	return blank(d, base, KV_SECTOR) ? KV_OK : KV_EIO;
}

static int write_hdr(const kv_dev_t *d, int s, uint32_t gen) {
	uint8_t h[12];
	uint32_t ng = ~gen;
	memcpy(h, magic, 4);
	h[4] = (uint8_t)gen; h[5] = (uint8_t)(gen >> 8);
	h[6] = (uint8_t)(gen >> 16); h[7] = (uint8_t)(gen >> 24);
	h[8] = (uint8_t)ng; h[9] = (uint8_t)(ng >> 8);
	h[10] = (uint8_t)(ng >> 16); h[11] = (uint8_t)(ng >> 24);
	return prog(d, (uint32_t)s * KV_SECTOR, h, sizeof(h));
}

static uint32_t build_rec(uint8_t *b, const char *key, uint8_t type,
		const uint8_t *val, uint32_t vlen) {
	uint32_t klen = (uint32_t)strlen(key), size = rec_size(klen, vlen), c, i;
	b[0] = (uint8_t)klen;
	b[1] = type;
	b[2] = (uint8_t)vlen;
	b[3] = (uint8_t)(vlen >> 8);
	memcpy(b + KV_REC_HDR, key, klen);
	if (vlen) memcpy(b + KV_REC_HDR + klen, val, vlen);
	for (i = KV_REC_HDR + klen + vlen; i < size; i++) b[i] = 0xFF;
	c = 0xFFFFFFFFu;
	for (i = 0; i < 4; i++) c = crc_byte(c, b[i]);
	for (i = 0; i < klen + vlen; i++) c = crc_byte(c, b[KV_REC_HDR + i]);
	c = ~c;
	b[4] = (uint8_t)c; b[5] = (uint8_t)(c >> 8);
	b[6] = (uint8_t)(c >> 16); b[7] = (uint8_t)(c >> 24);
	return size;
}

// Nothing valid anywhere: start a sector at generation 1. Prefers a
// sector that is already blank, which saves an erase on a new board.
static int fresh(const kv_dev_t *d, scan_t *sc) {
	int s = 0, rc;
	if (!blank(d, 0, KV_SECTOR)) {
		if (blank(d, KV_SECTOR, KV_SECTOR)) s = 1;
		else if ((rc = erase_sector(d, 0)) != KV_OK) return rc;
	}
	if ((rc = write_hdr(d, s, 1)) != KV_OK) return rc;
	sc->s = s;
	sc->gen = 1;
	sc->end = KV_HDR_LEN;
	sc->dirty = 0;
	return KV_OK;
}

// Copies the live records -- all but `skip`, which may be NULL -- into
// the other sector, appends the new record if `type` is a set, and only
// then writes that sector's header. The header is the commit: until it
// is written, the old sector is the store.
static int compact(const kv_dev_t *d, kv_work_t *w, const scan_t *sc,
		const char *skip, uint8_t type, const uint8_t *val, uint32_t vlen) {
	int t = 1 - sc->s, rc;
	uint32_t base = (uint32_t)sc->s * KV_SECTOR, tbase = (uint32_t)t * KV_SECTOR;
	uint32_t total = KV_HDR_LEN, off, i, skiplen = skip ? (uint32_t)strlen(skip) : 0;
	rec_t r;

	// 1. will it fit? (decided before anything is erased)
	for (i = 0; i < w->n; i++) {
		if (!(w->state[i] & ST_LIVE)) continue;
		if (rec_hdr(d, base, w->off[i], &r) <= 0) return KV_EIO;
		if (skip && key_eq(d, &r, skip, skiplen)) continue;
		total += r.size;
	}
	if (skip && type == T_SET) total += rec_size(skiplen, vlen);
	if (total > KV_SECTOR) return KV_ENOSPC;

	// 2. the other sector, erased
	if ((rc = erase_sector(d, t)) != KV_OK) return rc;

	// 3. the live records. Each is read ONCE, into RAM, and checked
	// against its own CRC there; every decision after that -- its
	// length, and whether it is the key being replaced -- is made from
	// that verified copy, never from a second read of the flash. A
	// misread would otherwise be able to make another key look like
	// `skip` and silently drop it.
	off = KV_HDR_LEN;
	for (i = 0; i < w->n; i++) {
		uint32_t c = 0xFFFFFFFFu, k, kl, vl, size;
		if (!(w->state[i] & ST_LIVE)) continue;
		if (rec_hdr(d, base, w->off[i], &r) <= 0) return KV_EIO;
		for (k = 0; k < r.size; k++) w->rec[k] = d->read(d->ctx, r.off + k);
		kl = w->rec[0];
		vl = (uint32_t)w->rec[2] | ((uint32_t)w->rec[3] << 8);
		size = rec_size(kl, vl);
		if (kl == 0 || kl > KV_KEY_MAX || vl > KV_VAL_MAX || size != r.size) return KV_EIO;
		for (k = 0; k < 4; k++) c = crc_byte(c, w->rec[k]);
		for (k = 0; k < kl + vl; k++) c = crc_byte(c, w->rec[KV_REC_HDR + k]);
		if (~c != ((uint32_t)w->rec[4] | ((uint32_t)w->rec[5] << 8) |
				((uint32_t)w->rec[6] << 16) | ((uint32_t)w->rec[7] << 24)))
			return KV_EIO;
		if (skip && kl == skiplen && !memcmp(w->rec + KV_REC_HDR, skip, kl)) continue;
		if (off + size > KV_SECTOR) return KV_EIO;
		if ((rc = prog(d, tbase + off, w->rec, size)) != KV_OK) return rc;
		off += size;
	}

	// 4. the new record
	if (skip && type == T_SET) {
		uint32_t size = build_rec(w->rec, skip, T_SET, val, vlen);
		if (off + size > KV_SECTOR) return KV_EIO;
		if ((rc = prog(d, tbase + off, w->rec, size)) != KV_OK) return rc;
	}

	// 5. commit
	if ((rc = write_hdr(d, t, sc->gen + 1)) != KV_OK) return rc;

	// 6. the old sector. Not needed for correctness -- the higher
	// generation already wins -- but it is what physically removes a
	// superseded value (KV_SCRUB), and a failure here is repaired by
	// the next compaction's erase.
	erase_sector(d, sc->s);
	return KV_OK;
}

static int write_rec(const kv_dev_t *d, kv_work_t *w, const char *key,
		uint8_t type, const uint8_t *val, uint32_t vlen, uint32_t flags) {
	scan_t sc;
	uint32_t size, base;
	int rc, i;

	scan(d, w, &sc);

	i = (sc.s >= 0) ? find_live(d, w, &sc, key) : -1;
	if (type == T_DEL && i < 0) return KV_ENOENT;

	// Setting the value it already has: nothing to write, unless the
	// caller wants the history scrubbed.
	if (type == T_SET && i >= 0 && !(flags & KV_SCRUB)) {
		rec_t r;
		uint32_t k, klen = (uint32_t)strlen(key);
		base = (uint32_t)sc.s * KV_SECTOR;
		if (rec_hdr(d, base, w->off[i], &r) > 0 && r.vlen == vlen) {
			for (k = 0; k < vlen; k++)
				if (d->read(d->ctx, r.off + KV_REC_HDR + klen + k) != val[k]) break;
			if (k == vlen) return KV_OK;
		}
	}

	if (sc.s < 0 && (rc = fresh(d, &sc)) != KV_OK) return rc;
	base = (uint32_t)sc.s * KV_SECTOR;

	// Append only onto erased flash. This also covers a dirty log: a
	// log is dirty because unreadable bytes sit exactly at sc.end, so
	// the blank test fails and the write compacts instead.
	size = rec_size((uint32_t)strlen(key), vlen);
	if (!(flags & KV_SCRUB) && sc.end + size <= KV_SECTOR &&
			blank(d, base + sc.end, size)) {
		build_rec(w->rec, key, type, val, vlen);
		return prog(d, base + sc.end, w->rec, size);
	}
	return compact(d, w, &sc, key, type, val, vlen);
}

// -- public --

int kv_get(const kv_dev_t *d, const char *key, uint8_t *buf, uint32_t cap,
		uint32_t *len) {
	uint32_t gen, base, klen, attempt;

	if (!kv_key_ok(key)) return KV_EINVAL;
	klen = (uint32_t)strlen(key);

	// No lock and no index: another process may be compacting while
	// this reads, and the scheduler can switch this reader out between
	// any two flash reads. Every state the flash passes through is
	// consistent, but the sector being read can be ERASED -- and even
	// erased and rewritten -- while this is switched out. That shows up
	// as a log that ends early (a false "no such key") or a value that
	// changes mid-copy.
	//
	// So an answer is only trusted if the sector's header still carries
	// the generation it had when the scan began: a sector is never
	// rewritten under the same generation, because every compaction
	// commits generation + 1. Anything else starts over, from whichever
	// sector is active by then. The copy is also checked against the
	// record's CRC, for an erase that lands between the scan and the
	// generation check.
	//
	// "No valid sector at all" needs the same care. A compaction that
	// runs between the reads of the two headers is seen as the new
	// sector not yet committed AND the old one already erased -- an
	// empty store that never existed. The store is never actually
	// empty once written (the commit comes before the erase), so an
	// empty answer is only believed when it is seen twice in a row.
	int empty = 0;
	for (attempt = 0; attempt < 8; attempt++) {
		uint32_t off = KV_HDR_LEN, c, k, g2;
		rec_t r, found = { 0, 0, 0, 0, 0, 0 };
		int s, have = 0, rc;

		s = active_sector(d, &gen);
		if (s < 0) {
			if (empty++) return KV_ENOENT;
			continue;
		}
		empty = 0;
		base = (uint32_t)s * KV_SECTOR;

		while (rec_hdr(d, base, off, &r) > 0) {
			if (key_eq(d, &r, key, klen) && rec_valid(d, &r, NULL)) {
				found = r;
				have = 1;
			}
			off += r.size;
		}

		if (!have || found.type == T_DEL) {
			rc = KV_ENOENT;
		} else if (found.vlen > cap) {
			rc = KV_EBUFFER;
		} else {
			c = 0xFFFFFFFFu;
			for (k = 0; k < 4; k++) c = crc_byte(c, d->read(d->ctx, found.off + k));
			for (k = 0; k < klen; k++) c = crc_byte(c, (uint8_t)key[k]);
			for (k = 0; k < found.vlen; k++) {
				buf[k] = d->read(d->ctx, found.off + KV_REC_HDR + klen + k);
				c = crc_byte(c, buf[k]);
			}
			if (~c != found.crc) continue;
			rc = KV_OK;
		}

		if (!hdr_gen(d, s, &g2) || g2 != gen) continue;
		if (rc != KV_ENOENT && len) *len = found.vlen;
		return rc;
	}
	return KV_EIO;
}

int kv_set(const kv_dev_t *d, kv_work_t *w, const char *key,
		const uint8_t *val, uint32_t len, uint32_t flags) {
	if (!kv_key_ok(key) || len > KV_VAL_MAX || (len && !val)) return KV_EINVAL;
	return write_rec(d, w, key, T_SET, val, len, flags);
}

int kv_del(const kv_dev_t *d, kv_work_t *w, const char *key, uint32_t flags) {
	if (!kv_key_ok(key)) return KV_EINVAL;
	return write_rec(d, w, key, T_DEL, NULL, 0, flags);
}

int kv_compact(const kv_dev_t *d, kv_work_t *w) {
	scan_t sc;
	scan(d, w, &sc);
	if (sc.s < 0) return KV_OK;
	return compact(d, w, &sc, NULL, 0, NULL, 0);
}

void kv_stat(const kv_dev_t *d, kv_work_t *w, kv_stat_t *st) {
	scan_t sc;
	uint32_t i, base;
	rec_t r;

	scan(d, w, &sc);
	memset(st, 0, sizeof(*st));
	st->active = sc.s;
	if (sc.s < 0) return;
	base = (uint32_t)sc.s * KV_SECTOR;
	st->generation = sc.gen;
	st->used = sc.end;
	st->records = w->n;
	st->dirty = sc.dirty;
	st->live_bytes = KV_HDR_LEN;
	for (i = 0; i < w->n; i++) {
		if (!(w->state[i] & ST_LIVE)) continue;
		if (rec_hdr(d, base, w->off[i], &r) <= 0) continue;
		st->keys++;
		st->live_bytes += r.size;
	}
}

int kv_entry(const kv_dev_t *d, kv_work_t *w, uint32_t index,
		char *key, uint32_t *vlen) {
	scan_t sc;
	uint32_t i, k, base;
	rec_t r;

	scan(d, w, &sc);
	if (sc.s < 0) return KV_ENOENT;
	base = (uint32_t)sc.s * KV_SECTOR;
	for (i = 0; i < w->n; i++) {
		if (!(w->state[i] & ST_LIVE)) continue;
		if (index--) continue;
		if (rec_hdr(d, base, w->off[i], &r) <= 0) return KV_EIO;
		for (k = 0; k < r.klen; k++) key[k] = (char)d->read(d->ctx, r.off + KV_REC_HDR + k);
		key[r.klen] = 0;
		if (vlen) *vlen = r.vlen;
		return KV_OK;
	}
	return KV_ENOENT;
}
