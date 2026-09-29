/*
 * Host test for sw/apps/fed/core/fstore.c. docs/fed.md, "Storage".
 *
 *   make -C sw/apps/fed test
 *
 * Every crash the design says it survives is made to happen -- by
 * editing the files as a crash would leave them -- and the store must
 * come back with nothing lost and nothing invented. Then a fuzz: random
 * objects stored, reopened, expired, against a model of what should be
 * there.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../core/fstore.h"
#include "../../../ext/monocypher/monocypher-ed25519.h"

extern int plat_quiet;

static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char dir[128];
static uint8_t sk[4][64];
static uint32_t seqs[4];
static uint8_t obj[FOBJ_MAX], got[FOBJ_MAX];

// An object: kind, key (state), time, payload size.
static int make(int who, const char *topic, int state, const char *key, uint64_t time, uint32_t plen, fobj_t *o) {
	static uint8_t payload[FOBJ_PAYLOAD_MAX];
	memset(o, 0, sizeof(*o));
	snprintf(o->topic, sizeof(o->topic), "%s", topic);
	snprintf(o->type, sizeof(o->type), "test");
	o->format = FOBJ_BYTES;
	o->kind = state ? FOBJ_STATE : FOBJ_LOG;
	if (state) snprintf(o->key, sizeof(o->key), "%s", key);
	o->time = time;
	o->seq = ++seqs[who];
	o->len = plen;
	for (uint32_t i = 0; i < plen; i++) payload[i] = (uint8_t)(i * 31 + o->seq);
	return fobj_make(o, payload, sk[who], obj, sizeof(obj));
}

static int put(int who, const char *topic, int state, const char *key, uint64_t time, uint32_t plen, uint32_t now, fobj_t *o, uint32_t *pos) {
	int n = make(who, topic, state, key, time, plen, o);
	return fstore_put(obj, (uint32_t)n, now, pos);
}

static void sh(const char *cmd) { int r = system(cmd); (void)r; }

// Objects fstore would serve to a peer whose source is `src`.
static int count_for(uint64_t src) {
	int k = 0;
	uint32_t p = 0;
	while ((p = fstore_next_for(p, src))) k++;
	return k;
}
static char *file(const char *rel) { static char p[256]; snprintf(p, sizeof(p), "%s/%s", dir, rel); return p; }

int main(void) {
	uint32_t now = 1790505386, pos;
	fobj_t o;
	plat_quiet = getenv("FSTORE_TEST_LOG") ? 0 : 1;
	for (int i = 0; i < 4; i++) {
		uint8_t seed[32], pk[32];
		memset(seed, 40 + i, 32);
		crypto_ed25519_key_pair(sk[i], pk, seed);
	}
	snprintf(dir, sizeof(dir), "/tmp/fstore-test-%d", (int)getpid());

	// -- 1. a fresh store; logs in, back out --
	CK(fstore_open(dir, now, NULL, 64) == 0, "a fresh store opens");
	uint64_t epoch = fstore_epoch();
	CK(epoch != 0 && fstore_last() == 0, "an epoch, no objects");
	uint8_t ids[8][32];
	for (int i = 0; i < 8; i++) {
		int r = put(0, "net/forum/a", 0, "", now - 10, (uint32_t)(i * 100), now, &o, &pos);
		memcpy(ids[i], o.id, 32);
		CK(r == FSTORE_NEW && pos == (uint32_t)i + 1, "object %d stored at %u", i, pos);
	}
	CK(fstore_last() == 8 && fstore_have(ids[3]), "eight held");
	{
		int n = make(0, "net/forum/a", 0, "", now - 10, 50, &o);
		(void)n;
		int r = fstore_get(3, got, sizeof(got));
		fobj_t p;
		CK(r > 0 && fobj_parse(got, (uint32_t)r, 0, &p, NULL) == 0 && !memcmp(p.id, ids[2], 32), "get: the exact bytes back");
	}
	{
		// the same object again: nothing to do
		int n = make(1, "net/forum/a", 0, "", now - 5, 10, &o);
		uint32_t p1, p2;
		CK(fstore_put(obj, (uint32_t)n, now, &p1) == FSTORE_NEW, "new");
		CK(fstore_put(obj, (uint32_t)n, now, &p2) == FSTORE_HAVE && fstore_last() == 9, "the same again: HAVE, not stored twice");
		CK(fstore_put(obj, (uint32_t)n - 1, now, &p2) == -FSTORE_E_OBJ, "not a whole object: refused");
	}
	{
		uint32_t n = 0, p = 0;
		while ((p = fstore_next(p))) n++;
		CK(n == 9, "next: all nine, in order");
	}

	// -- 2. state: newest wins, per origin --
	{
		uint32_t pv1, pv2, pv3;
		fobj_t v1, v2, v0, v3;
		CK(put(2, "net/nodes", 1, "list", now - 300, 20, now, &v1, &pv1) == FSTORE_NEW, "state v1");
		CK(put(2, "net/nodes", 1, "list", now - 200, 20, now, &v2, &pv2) == FSTORE_NEW, "state v2, newer");
		CK(put(2, "net/nodes", 1, "list", now - 400, 20, now, &v0, &pos) == FSTORE_STALE, "an older one: STALE, not stored");
		CK(put(3, "net/nodes", 1, "list", now - 500, 20, now, &v3, &pv3) == FSTORE_NEW, "another origin, same key: its own state");
		uint32_t p = 0, seen1 = 0, seen2 = 0, seen3 = 0;
		while ((p = fstore_next(p))) { seen1 += p == pv1; seen2 += p == pv2; seen3 += p == pv3; }
		CK(!seen1 && seen2 && seen3, "next skips the superseded v1, and sends v2 and the other origin's");
		// the same time: the higher id wins
		fobj_t a, b;
		int na = make(2, "net/nodes", 1, "tie", now - 100, 5, &a);
		static uint8_t oa[FOBJ_MAX];
		memcpy(oa, obj, (size_t)na);
		int nb = make(2, "net/nodes", 1, "tie", now - 100, 6, &b);
		int hi_first = memcmp(a.id, b.id, 32) > 0;
		uint32_t q1, q2;
		if (hi_first) {
			CK(fstore_put(oa, (uint32_t)na, now, &q1) == FSTORE_NEW && fstore_put(obj, (uint32_t)nb, now, &q2) == FSTORE_STALE,
				"same time: the lower id is STALE after the higher");
		} else {
			CK(fstore_put(obj, (uint32_t)nb, now, &q1) == FSTORE_NEW && fstore_put(oa, (uint32_t)na, now, &q2) == FSTORE_STALE,
				"same time: the lower id is STALE after the higher");
		}
	}

	// -- 3. expired on arrival --
	CK(put(0, "net/forum/a", 0, "", now - FSTORE_RETAIN - 1, 10, now, &o, &pos) == FSTORE_EXPIRED,
		"a log object already past retention is not stored");
	CK(put(0, "net/nodes", 1, "old", 1, 10, now, &o, &pos) == FSTORE_NEW, "a state object never expires");

	// -- 4. reopen --
	uint32_t last = fstore_last();
	fstore_close();
	CK(fstore_open(dir, now, NULL, 64) == 0 && fstore_epoch() == epoch && fstore_last() == last && fstore_have(ids[7]),
		"reopened: the same epoch, positions and ids");
	CK(put(2, "net/nodes", 1, "list", now - 400, 20, now, &o, &pos) == FSTORE_STALE, "and the state table remembers");

	// -- 5. crashes --
	char segname[64];
	snprintf(segname, sizeof(segname), "seg/%08x", 1);
	{
		// a torn record at the end of the log. (Octal escapes: /bin/sh's
		// printf may be dash's, which knows no \x -- the first version of
		// this test appended literal backslashes, and so never tested what
		// it said it did.)
		char cmd[400];
		snprintf(cmd, sizeof(cmd), "printf '\\100\\000\\000\\000ZFED1\\ntopic: torn' >> %s.log", file(segname));
		fstore_close();
		sh(cmd);
		CK(fstore_open(dir, now, NULL, 64) == 0 && fstore_last() == last, "a torn record: not an object");
		CK(put(0, "net/forum/b", 0, "", now - 1, 30, now, &o, &pos) == FSTORE_NEW && pos == last + 1, "the next goes where it was");
		fobj_t p;
		int r = fstore_get(pos, got, sizeof(got));
		CK(r > 0 && fobj_parse(got, (uint32_t)r, 0, &p, NULL) == 0 && !memcmp(p.id, o.id, 32), "and reads back whole");
		last = pos;
		// torn, but its length fits: 20 bytes that are not an object
		// (after the store above wrote over the first torn record: the
		// scan stops at the first record that is not whole)
		fstore_close();
		snprintf(cmd, sizeof(cmd), "printf '\\024\\000\\000\\000ZFED1\\ntopic: xxxxxxxx' >> %s.log", file(segname));
		sh(cmd);
		CK(fstore_open(dir, now, NULL, 64) == 0 && fstore_last() == last, "a torn record whose length fits: still not an object");
	}
	{
		// the index one entry behind the log (a crash between the two writes)
		fstore_close();
		struct stat st;
		char p[256]; snprintf(p, sizeof(p), "%s.idx", file(segname));
		stat(p, &st);
		CK(truncate(p, st.st_size - 12) == 0, "(an index entry lost)");
		CK(fstore_open(dir, now, NULL, 64) == 0 && fstore_last() == last && fstore_have(o.id), "indexed again from the log");
	}
	{
		// the id table gone
		fstore_close();
		unlink(file("ids.tbl"));
		CK(fstore_open(dir, now, NULL, 64) == 0 && fstore_have(ids[0]) && fstore_have(o.id), "the id table rebuilt from the segments");
		fstore_close();
		unlink(file("state.tbl"));
		CK(fstore_open(dir, now, NULL, 64) == 0, "(the state table gone)");
		CK(put(2, "net/nodes", 1, "list", now - 400, 20, now, &o, &pos) == FSTORE_STALE, "the state table rebuilt: still knows v2");
	}
	{
		// tables that miss objects the log has: the watermark says so
		char cmd[600];
		fstore_sync();
		fstore_close();
		snprintf(cmd, sizeof(cmd), "cp %s %s.bak && cp %s %s.bak && cp %s %s.bak", file("ids.tbl"), file("ids.tbl"),
			file("state.tbl"), file("state.tbl"), file("store.hdr"), file("store.hdr"));
		sh(cmd);
		fstore_open(dir, now, NULL, 64);
		fobj_t n1, n2;
		uint32_t p1, p2;
		put(1, "net/forum/c", 0, "", now - 1, 40, now, &n1, &p1);
		put(1, "net/nodes", 1, "info", now - 1, 40, now, &n2, &p2);
		// the log and index are written, the tables and header are not
		snprintf(cmd, sizeof(cmd), "cp %s.bak %s && cp %s.bak %s && cp %s.bak %s", file("ids.tbl"), file("ids.tbl"),
			file("state.tbl"), file("state.tbl"), file("store.hdr"), file("store.hdr"));
		sh(cmd);
		CK(fstore_open(dir, now, NULL, 64) == 0 && fstore_have(n1.id) && fstore_have(n2.id), "past the watermark: applied again");
		fobj_t older;
		CK(put(1, "net/nodes", 1, "info", now - 50, 40, now, &older, &pos) == FSTORE_STALE, "including to the state table");
	}

	// -- 6. the id table grows (64 slots to start) --
	{
		uint8_t many[120][32];
		int bad = 0;
		for (int i = 0; i < 120; i++) { put(i % 4, "net/forum/grow", 0, "", now - 1, 8, now, &o, &pos); memcpy(many[i], o.id, 32); }
		for (int i = 0; i < 120; i++) bad += !fstore_have(many[i]);
		struct stat st;
		stat(file("ids.tbl"), &st);
		CK(bad == 0 && st.st_size > 16 + 64 * 24, "120 more: the table grew (%ld bytes), every id found", (long)st.st_size);
	}

	// -- 7. segments: by size, and by age --
	{
		int before = 0; struct stat st; char p[256];
		for (uint32_t f = 1; f < 100000; f++) { snprintf(p, sizeof(p), "%s/seg/%08x.log", dir, f); if (!stat(p, &st)) before++; }
		for (int i = 0; i < 70; i++) put(0, "net/forum/big", 0, "", now - 1, FOBJ_PAYLOAD_MAX, now, &o, &pos);
		int after = 0;
		for (uint32_t f = 1; f < 100000; f++) { snprintf(p, sizeof(p), "%s/seg/%08x.log", dir, f); if (!stat(p, &st)) after++; }
		CK(after >= before + 1, "70 objects of 16 KB: a new segment past 1 MB (%d -> %d)", before, after);
		put(0, "net/forum/late", 0, "", now + 31 * 86400 - 5, 8, now + 31 * 86400, &o, &pos);
		int later = 0;
		for (uint32_t f = 1; f < 100000; f++) { snprintf(p, sizeof(p), "%s/seg/%08x.log", dir, f); if (!stat(p, &st)) later++; }
		CK(later == after + 1, "and one after 30 days (%d)", later);
	}

	// -- 8. retention: segments dropped, current state carried forward --
	{
		uint32_t t = now + 200 * 86400;
		fobj_t keep;
		// the current v2 of net/nodes/list is in the first segment
		int dropped = fstore_expire(t);
		CK(dropped >= 1 && fstore_first() > 1, "%d segments dropped; the oldest position now %u", dropped, fstore_first());
		CK(!fstore_have(ids[0]), "a dropped log object is gone");
		CK(put(2, "net/nodes", 1, "list", now - 400, 20, t, &keep, &pos) == FSTORE_STALE, "the current state survived, copied forward");
		uint32_t p = 0, states = 0;
		while ((p = fstore_next(p))) {
			int r = fstore_get(p, got, sizeof(got));
			fobj_t q;
			if (r > 0 && !fobj_parse(got, (uint32_t)r, 0, &q, NULL) && q.kind == FOBJ_STATE) states++;
		}
		CK(states >= 4, "and is sent, with the other current state (%u)", states);
		fstore_close();
		CK(fstore_open(dir, t, NULL, 64) == 0 && fstore_first() > 1, "reopened after expiry");
	}

	// -- 9. cursors and consumers --
	{
		uint8_t peer[32]; uint64_t e; uint32_t cp;
		memset(peer, 7, 32);
		CK(!fstore_cursor(peer, &e, &cp), "no cursor yet");
		fstore_set_cursor(peer, 0x1122334455667788ULL, 42);
		memset(peer, 8, 32);
		fstore_set_cursor(peer, 5, 9);
		memset(peer, 7, 32);
		fstore_set_cursor(peer, 0x1122334455667788ULL, 43);
		fstore_set_consumer("bbs", 17);
		fstore_close();
		fstore_open(dir, now, NULL, 64);
		CK(fstore_cursor(peer, &e, &cp) && e == 0x1122334455667788ULL && cp == 43, "a cursor, updated, kept across a reopen");
		memset(peer, 8, 32);
		CK(fstore_cursor(peer, &e, &cp) && e == 5 && cp == 9, "and another peer's beside it");
		CK(fstore_consumer("bbs") == 17 && fstore_consumer("other") == 0, "a consumer's position");
	}
	fstore_close();

	// -- 10. a fuzz against a model --
	{
		char cmd[300];
		snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
		sh(cmd);
		srand(20260927);
		fstore_open(dir, now, NULL, 128);
		#define NM 600
		static uint8_t mid[NM][32];
		static int mstate[NM], mkey[NM], mwho[NM], mcur[NM];
		static uint64_t mtime[NM];
		int n = 0, wrong = 0;
		for (int step = 0; step < NM; step++) {
			int who = rand() % 4, state = rand() % 3 == 0, key = rand() % 5;
			char k[8]; snprintf(k, sizeof(k), "k%d", key);
			uint64_t t = now - (uint64_t)(rand() % 1000);
			fobj_t f;
			int r = put(who, state ? "fz/state" : "fz/log", state, k, t, (uint32_t)(rand() % 64), now, &f, &pos);
			// the model's verdict
			int want = FSTORE_NEW;
			if (state) {
				for (int i = 0; i < n; i++)
					if (mstate[i] && mcur[i] && mwho[i] == who && mkey[i] == key &&
							(mtime[i] > t || (mtime[i] == t && memcmp(mid[i], f.id, 32) >= 0))) want = FSTORE_STALE;
			}
			if (r != want) wrong++;
			if (r == FSTORE_NEW) {
				if (state) for (int i = 0; i < n; i++) if (mstate[i] && mwho[i] == who && mkey[i] == key) mcur[i] = 0;
				memcpy(mid[n], f.id, 32); mstate[n] = state; mkey[n] = key; mwho[n] = who; mtime[n] = t; mcur[n] = 1; n++;
			}
			if (rand() % 50 == 0) { fstore_close(); fstore_open(dir, now, NULL, 128); }
		}
		int missing = 0, stale_sent = 0;
		for (int i = 0; i < n; i++) missing += !fstore_have(mid[i]);
		uint32_t p = 0;
		while ((p = fstore_next(p))) {
			int r = fstore_get(p, got, sizeof(got));
			fobj_t q;
			if (r <= 0 || fobj_parse(got, (uint32_t)r, 0, &q, NULL)) continue;
			for (int i = 0; i < n; i++) if (!memcmp(mid[i], q.id, 32) && mstate[i] && !mcur[i]) stale_sent++;
		}
		CK(wrong == 0 && missing == 0 && stale_sent == 0,
			"fuzz: %d stored of 600 random, with reopens: %d verdicts wrong, %d missing, %d superseded still sent",
			n, wrong, missing, stale_sent);
		fstore_close();
	}

	// -- 10. cancels (docs/fed.md, "Moderation") --
	{
		char d2[160];
		snprintf(d2, sizeof(d2), "%s/cx", dir);
		CK(fstore_open(d2, now, NULL, 64) == 0, "(a store for cancels)");
		fobj_t lg, st, st2, other;
		uint32_t pl, ps, po;
		put(0, "net/forum", 0, NULL, now, 10, now, &lg, &pl);
		put(1, "net/prof", 1, "me", now, 10, now, &st, &ps);
		put(2, "net/prof", 1, "me", now, 10, now, &st2, &po);			// another origin's: kept
		put(0, "net/forum", 0, NULL, now, 12, now, &other, &po);
		CK(fstore_get_id(lg.id, got, sizeof(got)) > 0 && !fstore_cancelled(lg.id), "an object, by its id");
		CK(fstore_cancel(lg.id) == 1 && fstore_cancel(lg.id) == 0, "cancelled; cancelling again changes nothing");
		CK(fstore_cancelled(lg.id) && fstore_get_id(lg.id, got, sizeof(got)) == 0 && fstore_have(lg.id),
			"cancelled: not given out, still known (so not taken again)");
		uint32_t p = 0;
		int sent = 0, others = 0;
		while ((p = fstore_next(p))) {
			int r = fstore_get(p, got, sizeof(got));
			fobj_t q;
			if (r > 0 && !fobj_parse(got, (uint32_t)r, 0, &q, NULL)) { if (!memcmp(q.id, lg.id, 32)) sent++; else others++; }
		}
		CK(sent == 0 && others == 3, "not delivered or relayed; the others are (%d, %d)", sent, others);
		CK(fstore_cancel(st.id) == 1 && fstore_state_get("net/prof", (sk[1] + 32), "me", got, sizeof(got)) == 0,
			"a cancelled state object: no longer the current state");
		CK(fstore_state_get("net/prof", (sk[2] + 32), "me", got, sizeof(got)) > 0, "another origin's, still");
		uint8_t unknown[32];
		memset(unknown, 0x5a, 32);
		CK(fstore_cancel(unknown) == 0 && !fstore_cancelled(unknown), "an id not held: nothing to cancel");
		// expiry: a cancelled state object is not copied forward
		fobj_t late;
		put(0, "net/forum", 0, NULL, now + 31 * 86400, 10, now + 31 * 86400, &late, &po);		// a new segment
		fstore_expire(now + 400 * 86400);
		CK(fstore_state_get("net/prof", (sk[1] + 32), "me", got, sizeof(got)) == 0 &&
			fstore_state_get("net/prof", (sk[2] + 32), "me", got, sizeof(got)) > 0,
			"its segment expired: the cancelled state gone for good, the other carried forward");
		fstore_close();
	}

	// -- sources (fstore_source()): what came from a peer is not sent back to it --
	{
		char cmd[400];
		uint8_t pa[32], pb[32];
		memset(pa, 0xa1, 32);
		memset(pb, 0xb2, 32);
		uint64_t A = fstore_source(pa, 7), A2 = fstore_source(pa, 8), B = fstore_source(pb, 7);
		CK(A && A2 && B && A == fstore_source(pa, 7) && A != A2 && A != B,
			"a source: the same for a peer's epoch; another for its next epoch, or another peer; never 0");
		snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", dir, dir);
		sh(cmd);
		CK(fstore_open(dir, now, NULL, 64) == 0, "(a fresh store)");
		fobj_t a1, s1, h1;
		uint32_t p1, p2, p3;
		int n = make(0, "net/forum", 0, NULL, now, 50, &a1);
		fstore_put_from(obj, (uint32_t)n, now, A, &p1);
		n = make(1, "net/prof", 1, "me", now, 40, &s1);
		fstore_put_from(obj, (uint32_t)n, now, A, &p2);
		n = make(2, "net/forum", 0, NULL, now, 30, &h1);
		fstore_put(obj, (uint32_t)n, now, &p3);
		CK(count_for(A) == 1 && count_for(B) == 3 && count_for(A2) == 3,
			"served to A: not what came from A; to B, or to A with a new epoch: all");
		// a crash with the index two entries behind the log: rebuilt from the log
		fstore_close();
		snprintf(cmd, sizeof(cmd), "truncate -s %d %s/seg/00000001.idx", 16 + 20, dir);
		sh(cmd);
		CK(fstore_open(dir, now, NULL, 64) == 0 && fstore_last() == 3, "(reopened: the index rebuilt from the log)");
		CK(count_for(A) == 1 && count_for(B) == 3, "the sources rebuilt with it -- kept in the log too");
		// expiry: a state object copied forward keeps its source
		fobj_t late;
		uint32_t pl;
		put(3, "net/forum", 0, NULL, now + 31 * 86400, 10, now + 31 * 86400, &late, &pl);	// a new segment
		fstore_expire(now + 400 * 86400);
		// by day 400 all else has expired: only the state object is left, copied forward
		CK(fstore_state_get("net/prof", (sk[1] + 32), "me", got, sizeof(got)) > 0 && fstore_first() == fstore_last() &&
			count_for(A) == 0 && count_for(B) == 1,
			"a state object copied forward keeps its source: still not sent back to A, sent to B");
		fstore_close();
		// a store from before sources: refused, and left as it was
		snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", dir, dir);
		sh(cmd);
		uint8_t old[32] = { 'Z', 'S', 'T', '1', 9, 9, 9 }, back[32];
		FILE *f = fopen(file("store.hdr"), "wb");
		fwrite(old, 1, 32, f);
		fclose(f);
		CK(fstore_open(dir, now, NULL, 64) == -FSTORE_E_FORMAT, "a store from before sources: refused -- not read as another");
		f = fopen(file("store.hdr"), "rb");
		CK(f && fread(back, 1, 32, f) == 32 && !memcmp(back, old, 32), "and not overwritten as if new");
		if (f) fclose(f);
	}

	printf("fstore: %d checks, %d failed\n", checks, fails);
	if (!fails) { char cmd[300]; snprintf(cmd, sizeof(cmd), "rm -rf %s", dir); sh(cmd); }
	return fails != 0;
}
