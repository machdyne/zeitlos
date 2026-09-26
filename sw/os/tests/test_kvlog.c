/*
 * Host test for sw/os/kvlog.c, the flash key/value store's log.
 *
 *   cc -std=gnu99 -O2 -Wall -o /tmp/t sw/os/tests/test_kvlog.c sw/os/kvlog.c && /tmp/t
 *
 * Add -fsanitize=address,undefined to run it under the sanitizers;
 * it is clean under both.
 *
 * The flash is simulated as NOR: erase sets a sector to 0xFF, and
 * programming can only CLEAR bits (the new byte is old & data), exactly
 * as rtl/spiflash.v's chip behaves. A power cut interrupts the erase or
 * program in progress, leaving a random subset of its bits changed, and
 * nothing after it happens at all.
 *
 * 1. basics        every result code, and the edges of every limit
 * 2. model         20000 random operations against a plain array,
 *                  checking every key after every one, ENOSPC included
 * 3. every cut     for many store states, the next operation is cut at
 *                  EVERY erase and program it performs, several times
 *                  with different torn bits; after the "reboot" each key
 *                  must hold its old value or its new one, never
 *                  anything else, and the store must keep working
 * 4. cuts in a row random operations with random cuts, cumulatively,
 *                  so recovery itself gets cut
 * 5. preempted reads  a writer compacts the store in the MIDDLE of a
 *                  kv_get() -- between two of its flash reads -- which is
 *                  what a preemptive scheduler can do to a lock-free
 *                  reader; the reader must still return the right value
 *
 * Each check was confirmed to fail against a deliberately broken
 * kvlog.c -- see docs/kvstore.md, "Testing".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../kvlog.h"

static uint8_t flash[KV_REGION];
static long cut_at = -1;       // primitive ops until the power fails; -1 never
static int dead;
static unsigned long nreads, nerases[2], nprogs;
static uint32_t rng = 1;

static uint32_t rnd(void) {
	rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
	return rng;
}

// The torn bits come from their own generator, so the operation chosen
// (which draws from rnd()) is the same however the cut lands.
static uint32_t tear = 7;
static uint8_t tear_bits(void) {
	tear ^= tear << 13; tear ^= tear >> 17; tear ^= tear << 5;
	return (uint8_t)tear;
}

// A reader can be interrupted by a whole writer operation: see test 5.
static void (*on_read)(void);
static unsigned long read_trigger;

// Hardware faults, rather than power cuts: a read that returns one bit
// wrong (glitch_in reads from now), and a program that reports success
// while one cell stays at 1 (a worn cell, weak_prog programs from now).
static long glitch_in = -1, glitch_after_erase = -1, weak_prog = -1;

static uint8_t sim_read(void *ctx, uint32_t off) {
	(void)ctx;
	if (off >= KV_REGION) { printf("FATAL: read past the region, %u\n", off); exit(1); }
	nreads++;
	if (glitch_in >= 0 && glitch_in-- == 0) return flash[off] ^ 0x10;
	if (on_read && read_trigger && --read_trigger == 0) {
		void (*f)(void) = on_read;
		on_read = NULL;
		f();
	}
	return flash[off];
}

static int tick(void) {
	if (dead) return 1;
	if (cut_at == 0) { dead = 1; return 2; }
	if (cut_at > 0) cut_at--;
	return 0;
}

static int sim_erase(void *ctx, uint32_t off) {
	int t = tick(), i;
	(void)ctx;
	if (off != 0 && off != KV_SECTOR) { printf("FATAL: erase at %u\n", off); exit(1); }
	if (t == 1) return -1;
	if (t == 2) {           // torn: some bits made it to 1
		for (i = 0; i < (int)KV_SECTOR; i++) flash[off + i] |= tear_bits();
		return -1;
	}
	memset(flash + off, 0xFF, KV_SECTOR);
	nerases[off / KV_SECTOR]++;
	if (glitch_after_erase >= 0) { glitch_in = glitch_after_erase; glitch_after_erase = -1; }
	return 0;
}

static int sim_program(void *ctx, uint32_t off, const uint8_t *p, uint32_t n) {
	int t = tick();
	uint32_t i;
	(void)ctx;
	if (n == 0 || n > KV_PAGE || (off % KV_PAGE) + n > KV_PAGE || off + n > KV_REGION) {
		printf("FATAL: program %u bytes at %u crosses a page\n", n, off);
		exit(1);
	}
	if (t == 1) return -1;
	if (t == 2) {           // torn: some of the bits that should clear did
		for (i = 0; i < n; i++) flash[off + i] &= (uint8_t)(p[i] | tear_bits());
		return -1;
	}
	for (i = 0; i < n; i++) flash[off + i] &= p[i];
	if (weak_prog >= 0 && weak_prog-- == 0) {
		for (i = 0; i < n && p[i] == 0xFF; i++) ;
		if (i < n) flash[off + i] |= (uint8_t)~p[i];     // a cell that would not clear
	}
	nprogs++;
	return 0;
}

static const kv_dev_t dev = { sim_read, sim_erase, sim_program, NULL };
static kv_work_t work, work2;

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; if (fails <= 20) { \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

// -- the model --

#define NKEYS 16
static const char *keys[NKEYS] = {
	"a", "auth.password", "sys.lock.idle", "net.hostkey", "k4", "key.five",
	"x_6", "seven-7", "EIGHT", "nine.nine.nine", "t10",
	"abcdefghijklmnopqrstuvwxyz01234",      // 31, the longest legal key
	"m.12", "m.13", "m.14", "m.15",
};
typedef struct { int have; uint32_t len; uint8_t v[KV_VAL_MAX]; } mval_t;
static mval_t model[NKEYS];

static uint32_t rec_sz(const char *k, uint32_t vlen) {
	return (8u + (uint32_t)strlen(k) + vlen + 3u) & ~3u;
}

static uint32_t model_live(int skip, uint32_t newlen) {
	uint32_t t = KV_HDR_LEN;
	int i;
	for (i = 0; i < NKEYS; i++)
		if (i != skip && model[i].have) t += rec_sz(keys[i], model[i].len);
	if (skip >= 0 && newlen != 0xFFFFFFFFu) t += rec_sz(keys[skip], newlen);
	return t;
}

static int value_is(int k, const mval_t *m) {
	uint8_t buf[KV_VAL_MAX];
	uint32_t len = 12345;
	int rc = kv_get(&dev, keys[k], buf, sizeof(buf), &len);
	if (!m->have) return rc == KV_ENOENT;
	return rc == KV_OK && len == m->len && !memcmp(buf, m->v, len);
}

static void check_all(const char *when) {
	int k;
	for (k = 0; k < NKEYS; k++)
		CHECK(value_is(k, &model[k]), "%s: key %s wrong", when, keys[k]);
}

// Mostly small values, as real settings are. `big` shifts the mix so a
// store of NKEYS keys regularly outgrows a sector, which is the only way
// the model test reaches ENOSPC.
static int big;
static void random_value(mval_t *m) {
	uint32_t r = rnd() % 10, i;
	m->have = 1;
	m->len = r < 5 ? rnd() % 16 : r < 8 ? rnd() % 64 : rnd() % (KV_VAL_MAX + 1);
	if (big) m->len = 240 + rnd() % (KV_VAL_MAX - 239);
	for (i = 0; i < m->len; i++) m->v[i] = (uint8_t)rnd();
}

// One random operation, applied to the store and (if it succeeds) the
// model. Returns the key index touched, or -1 for a compaction.
static int random_op(mval_t *want, int *want_set, int *rc_out) {
	uint32_t r = rnd() % 100, flags = (rnd() % 8 == 0) ? KV_SCRUB : 0;
	int k = (int)(rnd() % NKEYS), rc;
	if (r < 8) {
		*rc_out = kv_compact(&dev, &work);
		*want_set = 0;
		return -1;
	}
	if (r < (big ? 10u : 25u)) {
		rc = kv_del(&dev, &work, keys[k], flags);
		want->have = 0;
		want->len = 0;
		*want_set = 1;
		*rc_out = rc;
		return k;
	}
	random_value(want);
	rc = kv_set(&dev, &work, keys[k], want->v, want->len, flags);
	*want_set = 1;
	*rc_out = rc;
	return k;
}

// -- 1 --

static void test_basics(void) {
	uint8_t buf[300], big[KV_VAL_MAX + 1];
	uint32_t len;
	kv_stat_t st;
	char name[KV_KEY_MAX + 1];

	memset(flash, 0xFF, sizeof(flash));
	memset(big, 0x42, sizeof(big));

	CHECK(kv_get(&dev, "nope", buf, sizeof(buf), &len) == KV_ENOENT, "empty store has a key");
	kv_stat(&dev, &work, &st);
	CHECK(st.active == -1 && st.keys == 0, "empty store is not empty");
	CHECK(kv_del(&dev, &work, "nope", 0) == KV_ENOENT, "deleting from an empty store");
	CHECK(nprogs == 0 && nerases[0] + nerases[1] == 0, "reading an empty store wrote to it");

	CHECK(kv_set(&dev, &work, "", buf, 1, 0) == KV_EINVAL, "empty key accepted");
	CHECK(kv_set(&dev, &work, "has space", buf, 1, 0) == KV_EINVAL, "space in key accepted");
	CHECK(kv_set(&dev, &work, "slash/no", buf, 1, 0) == KV_EINVAL, "slash in key accepted");
	CHECK(kv_set(&dev, &work, "abcdefghijklmnopqrstuvwxyz012345", buf, 1, 0) == KV_EINVAL,
		"32-character key accepted");
	CHECK(kv_set(&dev, &work, "abcdefghijklmnopqrstuvwxyz01234", buf, 0, 0) == KV_OK,
		"31-character key refused");
	CHECK(kv_set(&dev, &work, "big", big, KV_VAL_MAX + 1, 0) == KV_EINVAL, "257-byte value accepted");
	CHECK(kv_set(&dev, &work, "big", big, KV_VAL_MAX, 0) == KV_OK, "256-byte value refused");
	CHECK(kv_set(&dev, &work, "nul", NULL, 3, 0) == KV_EINVAL, "NULL value with a length accepted");
	CHECK(nerases[0] + nerases[1] == 0, "a fresh store erased a blank sector");

	CHECK(kv_set(&dev, &work, "k", (const uint8_t *)"hello", 5, 0) == KV_OK, "set");
	len = 0;
	CHECK(kv_get(&dev, "k", buf, sizeof(buf), &len) == KV_OK && len == 5 &&
		!memcmp(buf, "hello", 5), "get");
	len = 0;
	CHECK(kv_get(&dev, "k", buf, 4, &len) == KV_EBUFFER && len == 5, "short buffer not reported");
	CHECK(kv_get(&dev, "k", buf, 5, &len) == KV_OK, "exact buffer refused");

	unsigned long p0 = nprogs;
	CHECK(kv_set(&dev, &work, "k", (const uint8_t *)"hello", 5, 0) == KV_OK, "same value");
	CHECK(nprogs == p0, "setting the same value wrote");
	CHECK(kv_set(&dev, &work, "k", (const uint8_t *)"hello", 5, KV_SCRUB) == KV_OK, "same value, scrub");
	CHECK(nprogs > p0, "scrub of the same value did not rewrite");

	CHECK(kv_set(&dev, &work, "k", (const uint8_t *)"world!", 6, 0) == KV_OK, "overwrite");
	CHECK(kv_get(&dev, "k", buf, sizeof(buf), &len) == KV_OK && len == 6 &&
		!memcmp(buf, "world!", 6), "overwrite read back");
	CHECK(kv_set(&dev, &work, "k", NULL, 0, 0) == KV_OK, "empty value");
	CHECK(kv_get(&dev, "k", buf, sizeof(buf), &len) == KV_OK && len == 0, "empty value read back");
	CHECK(kv_del(&dev, &work, "k", 0) == KV_OK, "delete");
	CHECK(kv_get(&dev, "k", buf, sizeof(buf), &len) == KV_ENOENT, "deleted key still there");
	CHECK(kv_del(&dev, &work, "k", 0) == KV_ENOENT, "deleting twice");

	kv_stat(&dev, &work, &st);
	CHECK(st.keys == 2, "stat: %u keys, want 2", st.keys);
	CHECK(kv_entry(&dev, &work, 0, name, &len) == KV_OK && !strcmp(name,
		"abcdefghijklmnopqrstuvwxyz01234") && len == 0, "entry 0");
	CHECK(kv_entry(&dev, &work, 1, name, &len) == KV_OK && !strcmp(name, "big") &&
		len == KV_VAL_MAX, "entry 1");
	CHECK(kv_entry(&dev, &work, 2, name, &len) == KV_ENOENT, "entry past the end");

	// scrub: the old value must be gone from the flash, not just superseded
	CHECK(kv_set(&dev, &work, "secret", (const uint8_t *)"OLD-SECRET-VALUE", 16, 0) == KV_OK, "secret");
	CHECK(kv_set(&dev, &work, "secret", (const uint8_t *)"new-secret-value", 16, KV_SCRUB) == KV_OK,
		"secret scrubbed");
	CHECK(memmem(flash, sizeof(flash), "OLD-SECRET-VALUE", 16) == NULL,
		"a scrubbed value is still on the flash");
	CHECK(kv_del(&dev, &work, "secret", KV_SCRUB) == KV_OK, "delete, scrub");
	CHECK(memmem(flash, sizeof(flash), "new-secret-value", 16) == NULL,
		"a scrubbed delete left the value on the flash");

	// full: 15 keys of 256 bytes cannot fit in one sector
	{
		int i, ok = 0, nospc = 0;
		char k[16];
		memset(flash, 0xFF, sizeof(flash));
		for (i = 0; i < 20; i++) {
			sprintf(k, "f%d", i);
			int rc = kv_set(&dev, &work, k, big, KV_VAL_MAX, 0);
			if (rc == KV_OK) ok++;
			else if (rc == KV_ENOSPC) nospc++;
		}
		CHECK(ok == 15 && nospc == 5, "fill: %d fit, %d refused (want 15, 5)", ok, nospc);
		for (i = 0; i < 15; i++) {
			sprintf(k, "f%d", i);
			CHECK(kv_get(&dev, k, buf, sizeof(buf), &len) == KV_OK && len == KV_VAL_MAX &&
				!memcmp(buf, big, KV_VAL_MAX), "fill: %s lost after ENOSPC", k);
		}
		CHECK(kv_del(&dev, &work, "f0", 0) == KV_OK, "delete from a full store");
		CHECK(kv_set(&dev, &work, "f15", big, KV_VAL_MAX, 0) == KV_OK, "set after freeing room");
	}
}

// -- 1b: one test per defence --
//
// The general suites exercise these only by chance, and a mutation run
// showed two of them surviving removal. Each case here is constructed
// so that exactly one defence stands between it and a wrong answer.

static int find_active(void) {
	kv_stat_t st;
	kv_stat(&dev, &work, &st);
	return st.active;
}

static void test_defences(void) {
	static uint8_t snap[KV_REGION];
	uint8_t buf[KV_VAL_MAX];
	uint32_t len, i;
	int s;
	kv_stat_t st;

	// (a) Leftovers of a torn append past the end of the log, with the
	// key-length byte itself still erased: the log still ends there,
	// but the space is not blank. The next write must compact rather
	// than program over it -- programming over it would fail the
	// read-back and the write would be lost as an I/O error.
	memset(flash, 0xFF, sizeof(flash));
	CHECK(kv_set(&dev, &work, "one", (const uint8_t *)"1", 1, 0) == KV_OK, "defence a: setup");
	kv_stat(&dev, &work, &st);
	s = st.active;
	for (i = 1; i < 12; i++) flash[(uint32_t)s * KV_SECTOR + st.used + i] = 0x00;
	CHECK(kv_set(&dev, &work, "two", (const uint8_t *)"22", 2, 0) == KV_OK,
		"defence a: a write over the leftovers of a torn append failed");
	CHECK(kv_get(&dev, "two", buf, sizeof(buf), &len) == KV_OK && len == 2, "defence a: value lost");
	CHECK(kv_get(&dev, "one", buf, sizeof(buf), &len) == KV_OK && len == 1, "defence a: old key lost");

	// (b) A power cut while erasing the OLD sector after a compaction.
	// Erasing sets bits, so its header can keep its magic while its
	// generation grows -- past the new sector's. Only the complement
	// test stops the stale sector, with the stale value, winning.
	memset(flash, 0xFF, sizeof(flash));
	CHECK(kv_set(&dev, &work, "k", (const uint8_t *)"stale", 5, 0) == KV_OK, "defence b: setup");
	s = find_active();
	memcpy(snap, flash, sizeof(flash));
	CHECK(kv_set(&dev, &work, "k", (const uint8_t *)"fresh", 5, KV_SCRUB) == KV_OK, "defence b: scrub");
	CHECK(find_active() == 1 - s, "defence b: the scrub did not move sectors");
	memcpy(flash + (uint32_t)s * KV_SECTOR, snap + (uint32_t)s * KV_SECTOR, KV_SECTOR);
	// generation 1 -> 3: one bit of it erased back to 1, so it now
	// reads as newer than the sector that replaced it (generation 2)
	flash[(uint32_t)s * KV_SECTOR + 4] |= 0x02;
	CHECK(kv_get(&dev, "k", buf, sizeof(buf), &len) == KV_OK && len == 5 &&
		!memcmp(buf, "fresh", 5), "defence b: a half-erased old sector won");
}

// (c) and (d): faults that report no error. After either, every key
// must hold its old value or its new one, and a call that returns
// KV_OK must mean it: a fault may cost a write, never a silent lie.
static void test_faults(void) {
	int i, j, weak = 0, glitch = 0;
	for (i = 0; i < 4000; i++) {
		mval_t want;
		int ws, rc, k;
		memset(flash, 0xFF, sizeof(flash));
		memset(model, 0, sizeof(model));
		for (j = 0; j < 6; j++) {
			k = random_op(&want, &ws, &rc);
			if (k >= 0 && rc == KV_OK) model[k] = want;
		}
		// a scrubbing set always compacts: copy phase included
		k = (int)(rnd() % NKEYS);
		random_value(&want);
		if (i & 1) { weak_prog = (long)(rnd() % 8); weak++; }
		else { glitch_after_erase = KV_SECTOR + (long)(rnd() % 600); glitch++; }
		rc = kv_set(&dev, &work, keys[k], want.v, want.len, KV_SCRUB);
		weak_prog = glitch_after_erase = glitch_in = -1;
		for (j = 0; j < NKEYS; j++) {
			if (j == k) {
				int old = value_is(j, &model[j]), nw = value_is(j, &want);
				CHECK(old || nw, "fault %d: %s neither old nor new", i, keys[j]);
				CHECK(rc != KV_OK || nw, "fault %d: set said OK but %s is not the new value", i, keys[j]);
			} else
				CHECK(value_is(j, &model[j]), "fault %d: %s damaged by a fault", i, keys[j]);
		}
		if (fails > 20) return;
	}
	printf("  faults: %d worn cells, %d misreads during compaction\n", weak, glitch);
}

// -- 2 --

static void test_model(void) {
	int i, k, want_set, rc;
	mval_t want;
	unsigned long nospc = 0, compactions0 = nerases[0] + nerases[1];

	memset(flash, 0xFF, sizeof(flash));
	memset(model, 0, sizeof(model));
	for (i = 0; i < 20000; i++) {
		big = (i / 2000) & 1;       // alternate: ordinary, then crowded
		uint32_t predicted = 0;
		k = random_op(&want, &want_set, &rc);
		if (k >= 0 && want.have) predicted = model_live(k, want.len);
		if (k >= 0 && want.have && predicted > KV_SECTOR) {
			CHECK(rc == KV_ENOSPC, "op %d: set of %s needs %u bytes, got %d not ENOSPC",
				i, keys[k], predicted, rc);
			nospc++;
		} else if (k >= 0 && !want.have) {
			CHECK(rc == (model[k].have ? KV_OK : KV_ENOENT), "op %d: del %s gave %d", i, keys[k], rc);
			if (rc == KV_OK) model[k] = want;
		} else if (k >= 0) {
			CHECK(rc == KV_OK, "op %d: set %s (%u bytes) gave %d", i, keys[k], want.len, rc);
			if (rc == KV_OK) model[k] = want;
		} else
			CHECK(rc == KV_OK, "op %d: compact gave %d", i, rc);
		check_all("model");
		if (fails > 20) return;
	}
	big = 0;
	CHECK(nospc > 100, "the model test reached ENOSPC only %lu times", nospc);
	printf("  model: 20000 operations, %lu ENOSPC, %lu sector erases\n",
		nospc, nerases[0] + nerases[1] - compactions0);
}

// -- 3 --

static void test_every_cut(void) {
	static uint8_t snap[KV_REGION];
	static mval_t msnap[NKEYS];
	int state, cuts = 0, seeds;
	unsigned long worst_ops = 0;

	memset(flash, 0xFF, sizeof(flash));
	memset(model, 0, sizeof(model));

	for (state = 0; state < 400; state++) {
		int want_set, rc, k;
		long n, c;
		mval_t want;
		uint32_t opseed;

		// advance the store by one random, uncut operation
		k = random_op(&want, &want_set, &rc);
		if (rc == KV_OK && k >= 0) model[k] = want;
		if (state < 5) continue;

		memcpy(snap, flash, sizeof(flash));
		memcpy(msnap, model, sizeof(model));

		// the operation to cut: count how many primitive ops it does
		opseed = rnd();
		rng = opseed;
		cut_at = -1;
		unsigned long e0 = nerases[0] + nerases[1], p0 = nprogs;
		k = random_op(&want, &want_set, &rc);
		n = (long)((nerases[0] + nerases[1] - e0) + (nprogs - p0));
		if ((unsigned long)n > worst_ops) worst_ops = (unsigned long)n;
		memcpy(flash, snap, sizeof(flash));

		for (c = 0; c < n; c++) {
			for (seeds = 0; seeds < 3; seeds++) {
				mval_t w2;
				int ws2, rc2, k2, j;
				memcpy(flash, snap, sizeof(flash));
				memcpy(model, msnap, sizeof(model));
				rng = opseed;
				tear = (uint32_t)(c * 2654435761u + (uint32_t)seeds * 40503u + 1u);
				cut_at = c;
				dead = 0;
				k2 = random_op(&w2, &ws2, &rc2);
				(void)rc2;
				CHECK(dead, "state %d cut %ld: the power never failed", state, c);
				// reboot
				dead = 0;
				cut_at = -1;
				for (j = 0; j < NKEYS; j++) {
					if (j == k2 && ws2) {
						int old = value_is(j, &model[j]), nw = value_is(j, &w2);
						CHECK(old || nw, "state %d cut %ld/%ld: %s is neither old nor new",
							state, c, n, keys[j]);
						if (nw && !old) model[j] = w2;
					} else
						CHECK(value_is(j, &model[j]), "state %d cut %ld/%ld: untouched %s changed",
							state, c, n, keys[j]);
				}
				// and it still works: a write, then everything again
				{
					mval_t w3;
					int k3 = (int)(rnd() % NKEYS);
					random_value(&w3);
					w3.len %= 32;
					rc2 = kv_set(&dev, &work, keys[k3], w3.v, w3.len, 0);
					if (model_live(k3, w3.len) <= KV_SECTOR) {
						CHECK(rc2 == KV_OK, "state %d cut %ld: the next write failed, %d", state, c, rc2);
						if (rc2 == KV_OK) model[k3] = w3;
					}
					check_all("after recovery");
				}
				cuts++;
				if (fails > 20) return;
			}
		}
		memcpy(flash, snap, sizeof(flash));
		memcpy(model, msnap, sizeof(model));
		rng = opseed;
		k = random_op(&want, &want_set, &rc);
		if (rc == KV_OK && k >= 0 && want_set) model[k] = want;
	}
	printf("  every cut: %d power failures across 395 store states "
		"(up to %lu flash operations in one call)\n", cuts, worst_ops);
}

// -- 4 --

static void test_cuts_in_a_row(void) {
	int i, j, cuts = 0;
	memset(flash, 0xFF, sizeof(flash));
	memset(model, 0, sizeof(model));
	for (i = 0; i < 30000; i++) {
		mval_t want;
		int ws, rc, k;
		int cut = (rnd() % 3 == 0);
		cut_at = cut ? (long)(rnd() % 6) : -1;
		dead = 0;
		k = random_op(&want, &ws, &rc);
		if (dead) {
			cuts++;
			dead = 0;
			cut_at = -1;
			for (j = 0; j < NKEYS; j++) {
				if (j == k && ws) {
					int old = value_is(j, &model[j]), nw = value_is(j, &want);
					CHECK(old || nw, "op %d: %s neither old nor new after a cut", i, keys[j]);
					if (nw && !old) model[j] = want;
				} else
					CHECK(value_is(j, &model[j]), "op %d: untouched %s changed by a cut", i, keys[j]);
			}
		} else {
			cut_at = -1;
			if (k >= 0 && rc == KV_OK) model[k] = want;
			check_all("in a row");
		}
		if (fails > 20) return;
	}
	printf("  cuts in a row: %d power failures in 30000 operations, some of them "
		"during recovery\n", cuts);
}

// -- 5 --

static int preempt_ok;
static void preempting_writer(void) {
	// what another process might do while the reader is switched out
	preempt_ok = kv_compact(&dev, &work2);
}

static void test_preempted_reads(void) {
	int i, k, trials = 0, bad = 0;
	uint8_t buf[KV_VAL_MAX];
	uint32_t len;

	memset(flash, 0xFF, sizeof(flash));
	memset(model, 0, sizeof(model));
	for (k = 0; k < NKEYS; k++) {
		random_value(&model[k]);
		model[k].len %= 100;
		CHECK(kv_set(&dev, &work, keys[k], model[k].v, model[k].len, 0) == KV_OK, "preempt setup");
	}
	for (i = 0; i < 3000; i++) {
		k = (int)(rnd() % NKEYS);
		unsigned long before = nreads;
		kv_get(&dev, keys[k], buf, sizeof(buf), &len);
		unsigned long reads = nreads - before;
		// the same read again, with the writer landing somewhere inside it
		read_trigger = 1 + rnd() % (reads ? reads : 1);
		on_read = preempting_writer;
		preempt_ok = 99;
		int rc = kv_get(&dev, keys[k], buf, sizeof(buf), &len);
		on_read = NULL;
		read_trigger = 0;
		if (preempt_ok == 99) continue;     // landed after the last read
		trials++;
		CHECK(preempt_ok == KV_OK, "the preempting compaction failed");
		if (!(rc == KV_OK && len == model[k].len && !memcmp(buf, model[k].v, len))) bad++;
	}
	CHECK(bad == 0, "%d of %d preempted reads returned the wrong value", bad, trials);
	printf("  preempted reads: %d reads with a compaction inside them\n", trials);
}

int main(void) {
	printf("kvlog host test\n");
	test_basics();
	test_defences();
	test_faults();
	test_model();
	test_every_cut();
	test_cuts_in_a_row();
	test_preempted_reads();
	printf("sector erases: %lu and %lu (wear is spread over both)\n", nerases[0], nerases[1]);
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
