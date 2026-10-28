/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * backup's core on the host (docs/backup.md, "Testing").
 *
 *   ./test_bk            every test
 *   ./test_bk -v         and what backup says
 *   ./test_bk --dump D   write a drive made by a few backups to D/usb,
 *                        and what each backup held to D/truth, for
 *                        tools/backup_read.py's test (test_read.py)
 *
 * The card and the drive are memfs (memfs.h): two volumes in memory
 * that lose their power wherever a test says. The power-cut tests run a
 * backup (and a restore, and making room) once for every write it
 * makes, with the power going at that write, then check after the
 * "reboot" that:
 *
 *   - the card is byte for byte as it was (a backup never writes it);
 *   - every backup that was complete before is complete, and every copy
 *     it names is there and right;
 *   - the next backup works, and holds exactly what the card has.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bk.h"
#include "memfs.h"

static int checks, fails;
static bool verbose;

#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *const FROM[] = { "/home", "/data", "/sys/zeitlos.cfg", "/sys/net.cfg" };
static const char *const SKIP[] = { "/data/ask", "*.tmp" };

static bk_opts_t opts(void) {
	bk_opts_t o;
	memset(&o, 0, sizeof o);
	o.dest = "/usb/backup";
	o.name = "zeitlos";
	o.from = FROM; o.nfrom = 4;
	o.skip = SKIP; o.nskip = 2;
	o.keep = 10;
	o.refresh = 10;
	o.verify = true;
	o.reserve_kb = 16;
	return o;
}

static memfs_t *fresh(uint32_t usb_kb) {
	memfs_t *m = memfs_new(64 * 1024, usb_kb);
	memfs_verbose(m, verbose);
	return m;
}

static int run(memfs_t *m, const bk_opts_t *o, bk_result_t *r) {
	bk_io_t io;
	bk_result_t rr;
	memfs_io(m, &io);
	return bk_backup(&io, o, r ? r : &rr);
}

// -- the truth: what each backup should hold --------------------------------

typedef struct {
	char name[BK_NAME_MAX];
	memfs_t *card;      // the card when it was made
	bool watch;
	char only[4][BK_PATH_MAX];
	int nonly;
} truth_t;

#define TMAX 64
static truth_t truth[TMAX];
static int ntruth;

static void truth_clear(void) {
	for (int i = 0; i < ntruth; i++) memfs_free(truth[i].card);
	ntruth = 0;
}

static void truth_add(const char *name, memfs_t *m, const bk_opts_t *o) {
	truth_t *t = &truth[ntruth++];
	memset(t, 0, sizeof *t);
	snprintf(t->name, sizeof t->name, "%s", name);
	t->card = memfs_clone(m);
	t->watch = o->nonly > 0;
	for (int i = 0; i < o->nonly && i < 4; i++) snprintf(t->only[i], BK_PATH_MAX, "%s", o->only[i]);
	t->nonly = o->nonly;
}

static int backup_ok(memfs_t *m, const bk_opts_t *o, bk_result_t *r) {
	bk_result_t rr;
	if (!r) r = &rr;
	int rc = run(m, o, r);
	if (rc <= BK_PARTIAL && r->name[0]) truth_add(r->name, m, o);
	return rc;
}

static bool wanted(const char *p) {
	if (!strncmp(p, "/usb", 4)) return false;
	if (!strncmp(p, "/data/ask", 9)) return false;
	size_t n = strlen(p);
	if (n > 4 && !strcmp(p + n - 4, ".tmp")) return false;
	if (n > 5 && !strcmp(p + n - 5, ".part")) return false;
	return !strncmp(p, "/home/", 6) || !strncmp(p, "/data/", 6) ||
		!strcmp(p, "/sys/zeitlos.cfg") || !strcmp(p, "/sys/net.cfg");
}

// bk_files into a list
typedef struct { char path[BK_PATH_MAX]; char where[BK_NAME_MAX]; uint32_t crc, size; bool present; } fl_t;
typedef struct { fl_t *v; int n; } fls_t;

static int fl_fn(void *arg, const bk_file_t *f) {
	fls_t *l = arg;
	l->v = realloc(l->v, (size_t)(l->n + 1) * sizeof *l->v);
	fl_t *x = &l->v[l->n++];
	snprintf(x->path, sizeof x->path, "%s", f->path);
	snprintf(x->where, sizeof x->where, "%s", f->where);
	x->crc = f->crc; x->size = f->size; x->present = f->present;
	return 0;
}

static fls_t files_of(memfs_t *m, const char *backup) {
	bk_io_t io;
	bk_opts_t o = opts();
	fls_t l = { 0, 0 };
	memfs_io(m, &io);
	bk_files(&io, &o, backup, "/", fl_fn, &l);
	return l;
}

typedef struct { const memfs_t *card; int want, got; bool ok; const fls_t *l; } cmp_t;

static void count_fn(void *arg, const char *p, const uint8_t *d, uint32_t len, bool dir) {
	cmp_t *c = arg;
	(void)d; (void)len;
	if (!dir && wanted(p)) c->want++;
}

// Every complete backup that is in the truth holds what the card had;
// strict: every copy must be present (else thinned copies are allowed,
// as long as the newest backup has everything).
static bool verify_all(memfs_t *m, bool strict, const char *ctx) {
	bool ok = true;
	for (int t = 0; t < ntruth; t++) {
		char bdir[BK_PATH_MAX];
		snprintf(bdir, sizeof bdir, "/usb/backup/zeitlos/%s/INDEX.TXT", truth[t].name);
		if (!memfs_exists(m, bdir)) {
			// thinned away entirely: only allowed when not strict
			if (strict) { printf("  %s: %s is gone\n", ctx, truth[t].name); ok = false; }
			continue;
		}
		fls_t l = files_of(m, truth[t].name);
		bool last = t == ntruth - 1;
		if (!truth[t].watch) {
			cmp_t c = { truth[t].card, 0, 0, true, &l };
			memfs_each(truth[t].card, count_fn, &c);
			if (c.want != l.n) {
				printf("  %s: %s lists %d files, the card had %d\n", ctx, truth[t].name, l.n, c.want);
				ok = false;
			}
		}
		for (int i = 0; i < l.n; i++) {
			uint32_t tl, cl;
			const uint8_t *td = memfs_get(truth[t].card, l.v[i].path, &tl);
			if (truth[t].watch) {
				bool listed = false;
				for (int k = 0; k < truth[t].nonly; k++) if (!strcmp(truth[t].only[k], l.v[i].path)) listed = true;
				if (!listed) continue;   // laid over from before: checked there
			}
			if (!td) { printf("  %s: %s lists %s, not on the card then\n", ctx, truth[t].name, l.v[i].path); ok = false; continue; }
			char cp[BK_PATH_MAX * 2];
			snprintf(cp, sizeof cp, "/usb/backup/zeitlos/%s%s", l.v[i].where, l.v[i].path);
			const uint8_t *cd = memfs_get(m, cp, &cl);
			if (!cd) {
				if (strict || last) { printf("  %s: %s: copy of %s missing\n", ctx, truth[t].name, l.v[i].path); ok = false; }
				continue;
			}
			if (cl != tl || memcmp(cd, td, tl)) {
				printf("  %s: %s: copy of %s differs\n", ctx, truth[t].name, l.v[i].path);
				ok = false;
			}
			if (bk_crc32(0, td, tl) != l.v[i].crc) {
				printf("  %s: %s: CRC of %s wrong in the index\n", ctx, truth[t].name, l.v[i].path);
				ok = false;
			}
		}
		free(l.v);
	}
	return ok;
}

// -- the card ---------------------------------------------------------------

static void fill(memfs_t *m, const char *p, uint32_t len, unsigned seed) {
	uint8_t *b = malloc(len ? len : 1);
	for (uint32_t i = 0; i < len; i++) b[i] = (uint8_t)(seed * 31u + i * 7u + (i >> 9));
	memfs_put(m, p, b, len);
	free(b);
}

static void card_basic(memfs_t *m) {
	memfs_puts(m, "/home/book.txt", "Chapter 1\nIt was a dark and stormy night.\n");
	memfs_puts(m, "/home/notes.txt", "milk, eggs\n");
	fill(m, "/home/pics/cat.bmp", 20000, 1);
	memfs_puts(m, "/home/draft.tmp", "skip me");
	memfs_puts(m, "/home/save.txt.part", "skip me too");
	memfs_puts(m, "/data/sheet/budget.zss", "a1 100\na2 200\n");
	fill(m, "/data/ask/big.pack", 30000, 2);
	memfs_puts(m, "/sys/zeitlos.cfg", "system.rtc.timezone: Europe/Berlin\n");
	memfs_puts(m, "/sys/version", "zeitlos 0.0.7\n");
	memfs_puts(m, "/apps/sheet", "a program");
	memfs_puts(m, "/tmp/x", "never");
}

// -- names ------------------------------------------------------------------

static void test_names(void) {
	char b[20], out[BK_NAME_MAX];
	int n;
	bool w;
	bk_tm_t t = { 2026, 10, 21, 11, 30, 48 };
	CHECK(bk_name_parse("2026-10-21_11-30-48", b, &n, &w) && n == 0 && !w && !strcmp(b, "2026-10-21_11-30-48"), "plain");
	CHECK(bk_name_parse("2026-10-21_11-30-48+0003w", b, &n, &w) && n == 3 && w, "+N w");
	CHECK(!bk_name_parse("2026-10-21_11-30", b, &n, &w), "short");
	CHECK(!bk_name_parse("2026-10-21_11-30-48+3", b, &n, &w), "+N needs 4 digits");
	CHECK(!bk_name_parse("2026-10-21_11-30-48x", b, &n, &w), "junk");
	CHECK(!bk_name_parse("check", b, &n, &w), "check dir");
	CHECK(bk_name_cmp("2026-10-21_11-30-48", "2026-10-21_11-30-48+0001") < 0, "base < +1");
	CHECK(bk_name_cmp("2026-10-21_11-30-48+0002", "2026-10-21_11-30-48+0010") < 0, "+2 < +10");
	CHECK(bk_name_cmp("2026-10-21_11-30-48+0010", "2026-10-23_09-05-00") < 0, "+10 < later");
	CHECK(bk_name_cmp("2026-10-21_11-42-07w", "2026-10-21_11-42-08") < 0, "w by time");

	CHECK(bk_name_next("", &t, true, false, out) == 0 && !strcmp(out, "2026-10-21_11-30-48"), "first: %s", out);
	CHECK(bk_name_next("2026-10-20_09-15-40", &t, true, true, out) == 0 && !strcmp(out, "2026-10-21_11-30-48w"), "watch: %s", out);
	CHECK(bk_name_next("2026-10-21_11-30-48", &t, true, false, out) == 1, "same second: wait");
	CHECK(bk_name_next("2026-10-21_11-30-48", &t, false, false, out) == 0 && !strcmp(out, "2026-10-21_11-30-48+0001"), "no clock: %s", out);
	CHECK(bk_name_next("2026-10-21_11-30-48+0001", &t, false, false, out) == 0 && !strcmp(out, "2026-10-21_11-30-48+0002"), "no clock 2: %s", out);
	CHECK(bk_name_next("2026-10-21_11-30-48+0002w", &t, false, true, out) == 0 && !strcmp(out, "2026-10-21_11-30-48+0003w"), "no clock watch: %s", out);
	CHECK(bk_name_next("", &t, false, false, out) == 0 && !strcmp(out, "0000-00-00_00-00-00+0001"), "no clock, empty: %s", out);
	bk_tm_t old = { 1970, 1, 1, 0, 12, 0 };
	CHECK(bk_name_next("2026-10-21_11-30-48", &old, true, false, out) == 0 && !strcmp(out, "2026-10-21_11-30-48+0001"), "clock behind: %s", out);
	CHECK(bk_name_next("2026-10-21_11-30-48+0004", &t, true, false, out) == 0 && !strcmp(out, "2026-10-21_11-30-48+0005"), "same second after +N: %s", out);
	bk_tm_t later = { 2026, 10, 23, 9, 5, 0 };
	CHECK(bk_name_next("2026-10-21_11-30-48+0004", &later, true, false, out) == 0 && !strcmp(out, "2026-10-23_09-05-00"), "clock back: %s", out);

	CHECK(bk_glob("*.tmp", "draft.TMP"), "glob ci");
	CHECK(bk_glob("a?c*", "abcdef") && !bk_glob("a?c", "ac"), "glob ?");
	CHECK(bk_crc32(0, "123456789", 9) == 0xCBF43926u, "crc32 check value");
}

// -- a first backup, nothing changed, a change, a deletion -------------------

static int hist_fn(void *a, const char *b, const bk_stat_t *st) { (void)b; (void)st; (*(int *)a)++; return 0; }

typedef struct { int n; char names[TMAX][BK_NAME_MAX]; bool complete[TMAX], pinned[TMAX]; } blist_t;
static int bl_fn(void *arg, const bk_info_t *b) {
	blist_t *l = arg;
	if (l->n < TMAX) {
		snprintf(l->names[l->n], BK_NAME_MAX, "%s", b->name);
		l->complete[l->n] = b->complete; l->pinned[l->n] = b->pinned;
		l->n++;
	}
	return 0;
}
static blist_t blist(memfs_t *m) {
	blist_t l;
	bk_io_t io;
	bk_opts_t o = opts();
	memset(&l, 0, sizeof l);
	memfs_io(m, &io);
	bk_list(&io, &o, bl_fn, &l);
	return l;
}

static void test_basic(void) {
	truth_clear();
	memfs_t *m = fresh(64 * 1024);
	card_basic(m);
	bk_opts_t o = opts();
	bk_result_t r;
	int card0 = memfs_card_ops(m);
	CHECK(backup_ok(m, &o, &r) == BK_DONE, "first backup");
	CHECK(r.copied == 5 && r.files == 5, "copied %u files %u", r.copied, r.files);
	CHECK(memfs_card_ops(m) == card0, "a backup never writes the card");
	CHECK(memfs_exists(m, "/usb/backup/README.TXT"), "README");
	CHECK(!memfs_exists(m, "/usb/backup/zeitlos/LOCK.TXT"), "lock gone");
	CHECK(!memfs_exists(m, "/usb/backup/zeitlos/2026-10-21_11-30-00/data/ask/big.pack"), "skip /data/ask");
	CHECK(!memfs_exists(m, "/usb/backup/zeitlos/2026-10-21_11-30-00/home/draft.tmp"), "skip *.tmp");
	CHECK(!memfs_exists(m, "/usb/backup/zeitlos/2026-10-21_11-30-00/home/save.txt.part"), "skip .part");
	CHECK(memfs_exists(m, "/usb/backup/zeitlos/2026-10-21_11-30-00/home/pics/cat.bmp"), "nested");
	CHECK(verify_all(m, true, "basic 1"), "verify 1");

	memfs_tick(m, 60);
	memfs_out_clear(m);
	CHECK(run(m, &o, &r) == BK_DONE && r.name[0] == 0, "nothing changed: no backup");
	CHECK(strstr(memfs_out(m), "nothing changed"), "says so");

	memfs_tick(m, 3600);
	memfs_puts(m, "/home/book.txt", "Chapter 1\nIt was a dark and stormy night.\nChapter 2\n");
	memfs_puts(m, "/home/new.txt", "new");
	CHECK(backup_ok(m, &o, &r) == BK_DONE && r.copied == 2 && r.files == 6, "second: copied %u", r.copied);
	CHECK(!memfs_exists(m, "/usb/backup/zeitlos/2026-10-21_12-31-00/home/notes.txt"), "unchanged not copied");

	memfs_tick(m, 3600);
	memfs_rm(m, "/home/notes.txt");
	CHECK(backup_ok(m, &o, &r) == BK_DONE && r.copied == 0 && r.files == 5, "deletion alone makes a backup");
	CHECK(verify_all(m, true, "basic 3"), "verify 3");

	// history
	bk_io_t io;
	memfs_io(m, &io);
	int hn = 0;
	bk_history(&io, &o, "/home/book.txt", hist_fn, &hn);
	CHECK(hn == 2, "history: %d versions", hn);

	// restore the first version beside, then replace, then a folder elsewhere
	bk_result_t rr;
	CHECK(bk_restore(&io, &o, "2026-10-21_11-30-00", "/home/book.txt", NULL, false, &rr) == BK_DONE, "restore beside");
	uint32_t l;
	const uint8_t *d = memfs_get(m, "/home/book (2026-10-21 11-30).txt", &l);
	CHECK(d && l == 42 && !memcmp(d, "Chapter 1\nIt was a dark and stormy night.\n", 42), "beside content");
	d = memfs_get(m, "/home/book.txt", &l);
	CHECK(d && l > 42, "original kept");
	CHECK(bk_restore(&io, &o, "2026-10-21_11-30-00", "/home/book.txt", NULL, true, &rr) == BK_DONE, "restore replace");
	d = memfs_get(m, "/home/book.txt", &l);
	CHECK(d && l == 42, "replaced");
	CHECK(!memfs_exists(m, "/home/book.txt.part"), "no .part left");
	CHECK(bk_restore(&io, &o, "2026-10-21_11", "/home", "/r", false, &rr) == BK_DONE, "restore a folder to /r (prefix name)");
	d = memfs_get(m, "/r/notes.txt", &l);
	CHECK(d && l == 11, "folder restore: notes.txt back");
	d = memfs_get(m, "/r/pics/cat.bmp", &l);
	CHECK(d && l == 20000, "folder restore: nested");
	CHECK(bk_restore(&io, &o, NULL, "/home/nothing", NULL, false, &rr) == BK_FAILED, "restore of nothing");
	CHECK(bk_restore(&io, &o, NULL, "/home/new.txt", NULL, false, &rr) == BK_DONE && strstr(memfs_out(m), "already the same"), "same: nothing done");

	blist_t bl = blist(m);
	CHECK(bl.n == 3 && bl.complete[0] && bl.complete[2], "list: %d", bl.n);
	bk_summary_t sm;
	CHECK(bk_summary(&io, &o, &sm) == BK_DONE && sm.backups == 3 && sm.cut == 0 &&
		!strcmp(sm.newest, bl.names[2]) && !strcmp(sm.oldest, bl.names[0]) &&
		sm.newest_files == 5 && sm.free_kb > 0, "summary %d %s %u", sm.backups, sm.newest, sm.newest_files);

	// the window's glance: the same newest, no free-space count, and it stops
	bk_summary_t gl;
	CHECK(bk_glance(&io, &o, &gl) == BK_DONE && !strcmp(gl.newest, sm.newest) &&
		gl.newest_files == sm.newest_files && gl.free_kb == 0 && gl.backups == 3,
		"glance %s %u", gl.newest, gl.newest_files);
	// stopped at each of its checks in turn: BK_FAILED, nothing left held
	int stops_ok = 0, n_ok = 0;
	for (int k = 1; k < 40; k++) {
		memfs_stop_after(m, k);
		int r = bk_glance(&io, &o, &gl);
		if (r == BK_FAILED) stops_ok++;
		else if (r == BK_DONE && !strcmp(gl.newest, sm.newest)) { n_ok++; break; }
	}
	memfs_stop_after(m, 0);
	CHECK(stops_ok >= 2 && n_ok == 1, "glance stops at each check (%d stops), then runs through", stops_ok);

	// pins
	CHECK(bk_pin(&io, &o, bl.names[0], true) == BK_DONE, "pin");
	bl = blist(m);
	CHECK(bl.pinned[0] && !bl.pinned[1], "pinned shown");
	CHECK(bk_pin(&io, &o, bl.names[0], false) == BK_DONE && !blist(m).pinned[0], "unpin");

	// the drive unplugged
	memfs_usb(m, false);
	memfs_out_clear(m);
	CHECK(run(m, &o, &r) == BK_FAILED && strstr(memfs_out(m), "plugged in"), "no drive");
	CHECK(bk_summary(&io, &o, &sm) == BK_FAILED && !strstr(memfs_out(m) + strlen(memfs_out(m)) - 1, "x"), "summary, no drive");
	memfs_usb(m, true);
	memfs_free(m);
}

// -- no clock, dates that cannot be trusted ---------------------------------

static void test_noclock(void) {
	truth_clear();
	memfs_t *m = fresh(64 * 1024);
	card_basic(m);
	bk_opts_t o = opts();
	bk_result_t r;
	CHECK(backup_ok(m, &o, &r) == BK_DONE, "first");
	memfs_reboot(m);
	memfs_owner(m, "boot 2");
	// written while the clock is not set: FAT's 2020-01-01 stamp, same size
	memfs_puts(m, "/home/notes.txt", "milk, EGGS\n");
	CHECK(backup_ok(m, &o, &r) == BK_DONE && r.copied == 1, "change seen by CRC: copied %u", r.copied);
	CHECK(!strcmp(r.name, "2026-10-21_11-30-00+0001"), "name %s", r.name);
	memfs_puts(m, "/home/notes.txt", "milk, EGGs\n");
	memfs_tick(m, 10);
	CHECK(backup_ok(m, &o, &r) == BK_DONE && r.copied == 1 && !strcmp(r.name, "2026-10-21_11-30-00+0002"), "again: %s", r.name);
	// nothing changed: the date is the placeholder, so the CRC decides
	CHECK(run(m, &o, &r) == BK_DONE && r.name[0] == 0, "unchanged with untrusted dates");
	CHECK(verify_all(m, true, "noclock"), "verify");
	// the clock set again
	memfs_clock(m, true, 2026, 10, 23, 9, 5, 0);
	memfs_puts(m, "/home/book.txt", "x");
	CHECK(backup_ok(m, &o, &r) == BK_DONE && !strcmp(r.name, "2026-10-23_09-05-00"), "clock back: %s", r.name);
	// the clock wrong, behind the newest
	memfs_clock(m, true, 2026, 1, 1, 0, 0, 0);
	memfs_puts(m, "/home/book.txt", "xy");
	CHECK(backup_ok(m, &o, &r) == BK_DONE && !strcmp(r.name, "2026-10-23_09-05-00+0001"), "clock behind: %s", r.name);
	blist_t bl = blist(m);
	CHECK(bl.n == 5, "five");
	for (int i = 1; i < bl.n; i++) CHECK(bk_name_cmp(bl.names[i - 1], bl.names[i]) < 0, "in order");
	memfs_free(m);
}

// -- the same second --------------------------------------------------------

static void test_same_second(void) {
	truth_clear();
	memfs_t *m = fresh(64 * 1024);
	card_basic(m);
	bk_opts_t o = opts();
	bk_result_t r;
	backup_ok(m, &o, &r);
	memfs_puts(m, "/home/notes.txt", "changed");
	CHECK(backup_ok(m, &o, &r) == BK_DONE && !strcmp(r.name, "2026-10-21_11-30-01"), "waited a second: %s", r.name);
	memfs_free(m);
}

// -- files update put there --------------------------------------------------

static void test_manifest(void) {
	truth_clear();
	memfs_t *m = fresh(64 * 1024);
	card_basic(m);
	fill(m, "/data/fonts/big.zfn", 9000, 5);
	uint32_t l;
	const uint8_t *d = memfs_get(m, "/data/fonts/big.zfn", &l);
	char man[512];
	snprintf(man, sizeof man,
		"zeitlos-manifest 4\nname v0.0.7\n"
		"f %064d %08x %u r base data/fonts/big.zfn\n"
		"f %064d %08x %u k base sys/zeitlos.cfg\n",
		0, (unsigned)bk_crc32(0, d, l), (unsigned)l, 0, 0x12345678u, 3u);
	memfs_puts(m, "/sys/manifest", man);
	bk_opts_t o = opts();
	o.manifest = "/sys/manifest";
	bk_result_t r;
	CHECK(backup_ok(m, &o, &r) == BK_DONE && r.shipped == 1, "shipped %u", r.shipped);
	CHECK(!memfs_exists(m, "/usb/backup/zeitlos/2026-10-21_11-30-00/data/fonts/big.zfn"), "not copied");
	CHECK(memfs_exists(m, "/usb/backup/zeitlos/2026-10-21_11-30-00/sys/zeitlos.cfg"), "edited config copied");
	// changed by the person: backed up after all
	memfs_tick(m, 60);
	fill(m, "/data/fonts/big.zfn", 9000, 6);
	CHECK(backup_ok(m, &o, &r) == BK_DONE && r.copied == 1 && r.shipped == 0, "edited shipped file copied");
	memfs_free(m);
}

// -- watch backups ----------------------------------------------------------

static void test_watch(void) {
	truth_clear();
	memfs_t *m = fresh(64 * 1024);
	card_basic(m);
	bk_opts_t o = opts();
	bk_result_t r;
	backup_ok(m, &o, &r);
	memfs_tick(m, 100);
	memfs_puts(m, "/home/book.txt", "Chapter 1, revised\n");
	const char *only[] = { "/home/book.txt" };
	bk_opts_t w = opts();
	w.only = only; w.nonly = 1;
	CHECK(backup_ok(m, &w, &r) == BK_DONE && r.copied == 1 && r.files == 1, "watch: %u", r.copied);
	CHECK(strchr(r.name, 'w'), "w name %s", r.name);
	char wname[BK_NAME_MAX];
	snprintf(wname, sizeof wname, "%s", r.name);
	memfs_out_clear(m);
	CHECK(run(m, &w, &r) == BK_DONE && r.name[0] == 0, "watch again: nothing");
	// a full backup after it does not copy the book again
	memfs_tick(m, 100);
	memfs_puts(m, "/home/notes.txt", "eggs\n");
	CHECK(backup_ok(m, &o, &r) == BK_DONE && r.copied == 1, "full after watch copied %u", r.copied);
	fls_t l = files_of(m, NULL);
	bool found = false;
	for (int i = 0; i < l.n; i++) if (!strcmp(l.v[i].path, "/home/book.txt")) found = !strcmp(l.v[i].where, wname);
	free(l.v);
	CHECK(found, "the full backup names the watch copy");
	// the state at the watch backup is the full one with the book laid over
	l = files_of(m, wname);
	CHECK(l.n == 5, "watch state %d", l.n);
	free(l.v);
	// watch with the drive gone: nothing
	memfs_usb(m, false);
	CHECK(run(m, &w, &r) == BK_FAILED, "watch, no drive");
	memfs_usb(m, true);
	CHECK(verify_all(m, true, "watch"), "verify");
	memfs_free(m);
}

// -- refresh and repair -----------------------------------------------------

static void test_refresh_repair(void) {
	truth_clear();
	memfs_t *m = fresh(64 * 1024);
	card_basic(m);
	bk_opts_t o = opts();
	o.refresh = 2;
	bk_result_t r;
	backup_ok(m, &o, &r);
	for (int i = 0; i < 2; i++) {
		memfs_tick(m, 100);
		char t[16]; snprintf(t, sizeof t, "v%d", i);
		memfs_puts(m, "/home/book.txt", t);
		backup_ok(m, &o, &r);
	}
	memfs_tick(m, 100);
	memfs_puts(m, "/home/book.txt", "v9");
	CHECK(backup_ok(m, &o, &r) == BK_DONE && r.copied == 5, "refresh: copied %u (the book + 4 old ones)", r.copied);

	// damage a copy; check finds it; the next backup copies it again
	truth_clear();
	memfs_t *n = fresh(64 * 1024);
	card_basic(n);
	o = opts();
	backup_ok(n, &o, &r);
	CHECK(memfs_poke(n, "/usb/backup/zeitlos/2026-10-21_11-30-00/home/notes.txt", 0, 'X'), "poke");
	bk_io_t io;
	memfs_io(n, &io);
	bk_result_t cr;
	CHECK(bk_check(&io, &o, false, &cr) == BK_DONE, "quick check: indexes fine");
	CHECK(bk_check(&io, &o, true, &cr) == BK_PARTIAL && cr.bad == 1, "check -a: bad %u", cr.bad);
	memfs_tick(n, 100);
	CHECK(backup_ok(n, &o, &r) == BK_DONE && r.copied == 1, "repair copied %u", r.copied);
	fls_t l = files_of(n, NULL);
	for (int i = 0; i < l.n; i++)
		if (!strcmp(l.v[i].path, "/home/notes.txt")) CHECK(!strcmp(l.v[i].where, r.name), "repaired copy is used");
	free(l.v);
	// a damaged restore source is refused, not written
	bk_result_t rr;
	CHECK(bk_restore(&io, &o, "2026-10-21_11-30-00", "/home/notes.txt", "/r/n.txt", false, &rr) != BK_DONE &&
		!memfs_exists(n, "/r/n.txt") && !memfs_exists(n, "/r/n.txt.part"), "damaged copy not restored");
	memfs_free(m);
	memfs_free(n);
}

// -- the lock ---------------------------------------------------------------

static void test_lock(void) {
	truth_clear();
	memfs_t *m = fresh(64 * 1024);
	card_basic(m);
	bk_opts_t o = opts();
	bk_result_t r;
	memfs_mkdir_p(m, "/usb/backup/zeitlos");
	memfs_puts(m, "/usb/backup/zeitlos/LOCK.TXT", "test boot 1\n");
	memfs_out_clear(m);
	CHECK(run(m, &o, &r) == BK_FAILED && strstr(memfs_out(m), "another backup"), "live lock refuses");
	memfs_puts(m, "/usb/backup/zeitlos/LOCK.TXT", "pid 9 of a boot long gone\n");
	CHECK(run(m, &o, &r) == BK_DONE && r.name[0], "stale lock taken over");
	memfs_free(m);
}

// -- making room ------------------------------------------------------------

struct under { const char *p; int n; };
static void under_fn(void *a, const char *p, const uint8_t *d, uint32_t l, bool dir) {
	struct under *x = a; (void)d; (void)l;
	if (!dir && !strncmp(p, x->p, strlen(x->p))) x->n++;
}
static int nfiles_under(memfs_t *m, const char *prefix) {
	struct under c = { prefix, 0 };
	memfs_each(m, under_fn, &c);
	return c.n;
}

static void test_room(void) {
	truth_clear();
	// a drive just big enough for a few versions of a 40 KB file
	memfs_t *m = fresh(400);
	memfs_puts(m, "/home/keep.txt", "never changes");
	fill(m, "/home/big.bin", 40000, 1);
	bk_opts_t o = opts();
	o.keep = 2;
	o.reserve_kb = 8;
	bk_result_t r;
	CHECK(backup_ok(m, &o, &r) == BK_DONE, "first");
	int made = 1;
	for (int i = 2; i < 14; i++) {
		memfs_tick(m, 100);
		fill(m, "/home/big.bin", 40000, (unsigned)i);
		int rc = backup_ok(m, &o, &r);
		CHECK(rc == BK_DONE, "backup %d: rc %d", i, rc);
		if (rc == BK_DONE) made++;
	}
	CHECK(made == 13, "all made");
	{
		// the newest keep (2) backups are whole
		blist_t bl = blist(m);
		for (int b = bl.n - 2; b < bl.n; b++) {
			fls_t l = files_of(m, bl.names[b]);
			int present = 0;
			for (int i = 0; i < l.n; i++) present += l.v[i].present;
			CHECK(l.n == 2 && present == 2, "kept backup %s: %d of %d copies", bl.names[b], present, l.n);
			free(l.v);
		}
	}
	CHECK(verify_all(m, false, "room"), "every remaining backup right; the newest complete");
	// the file that never changed is still there, once
	CHECK(nfiles_under(m, "/usb/backup/zeitlos/2026-10-21_11-30-00") >= 1, "the first folder keeps keep.txt");
	CHECK(memfs_exists(m, "/usb/backup/zeitlos/2026-10-21_11-30-00/home/keep.txt"), "keep.txt's only copy kept");
	CHECK(!memfs_exists(m, "/usb/backup/zeitlos/2026-10-21_11-30-00/home/big.bin"), "the oldest big.bin went");
	// a folder emptied completely is gone (index too)
	CHECK(!memfs_exists(m, "/usb/backup/zeitlos/2026-10-21_11-31-40"), "emptied folder removed");

	// pinned: never touched
	truth_clear();
	memfs_t *p = fresh(400);
	memfs_puts(p, "/home/keep.txt", "never changes");
	fill(p, "/home/big.bin", 40000, 1);
	backup_ok(p, &o, &r);
	bk_io_t io;
	memfs_io(p, &io);
	bk_pin(&io, &o, r.name, true);
	char pinned[BK_NAME_MAX];
	snprintf(pinned, sizeof pinned, "%s", r.name);
	int ok = 0;
	for (int i = 2; i < 12; i++) {
		memfs_tick(p, 100);
		fill(p, "/home/big.bin", 40000, (unsigned)i);
		if (backup_ok(p, &o, &r) == BK_DONE) ok++;
	}
	char pp[BK_PATH_MAX];
	snprintf(pp, sizeof pp, "/usb/backup/zeitlos/%s/home/big.bin", pinned);
	CHECK(memfs_exists(p, pp), "pinned copy kept");
	CHECK(ok >= 3, "backups still made beside a pin: %d", ok);

	// a deleted file's last copy goes only when nothing else frees enough
	truth_clear();
	memfs_t *q = fresh(200);
	fill(q, "/home/old.bin", 60000, 9);
	fill(q, "/home/big.bin", 40000, 1);
	o.keep = 1;
	backup_ok(q, &o, &r);
	memfs_rm(q, "/home/old.bin");
	for (int i = 2; i < 6; i++) {
		memfs_tick(q, 100);
		fill(q, "/home/big.bin", 40000, (unsigned)i);
		CHECK(backup_ok(q, &o, &r) == BK_DONE, "deleted-file room backup %d", i);
	}
	CHECK(!memfs_exists(q, "/usb/backup/zeitlos/2026-10-21_11-30-00/home/old.bin"), "a deleted file's last copy went when needed");
	CHECK(verify_all(q, false, "room deleted"), "verify");

	// no room even after making room: nothing written
	truth_clear();
	memfs_t *s = fresh(40);
	fill(s, "/home/huge.bin", 100000, 3);
	o = opts();
	memfs_out_clear(s);
	CHECK(run(s, &o, &r) == BK_FAILED && strstr(memfs_out(s), "not enough room"), "too big");
	CHECK(nfiles_under(s, "/usb/backup/zeitlos/2") == 0, "nothing written");
	// the newest keep backups are never thinned: rather no backup
	truth_clear();
	memfs_t *k = fresh(180);
	fill(k, "/home/big.bin", 40000, 1);
	o = opts();
	o.keep = 3; o.reserve_kb = 8;
	int rc = BK_DONE;
	made = 0;
	for (int i = 2; i < 8 && rc == BK_DONE; i++) {
		rc = backup_ok(k, &o, &r);
		if (rc == BK_DONE) made++;
		memfs_tick(k, 100);
		fill(k, "/home/big.bin", 40000, (unsigned)i);
	}
	CHECK(rc == BK_FAILED && made == 3, "with 3 kept and room for 3, the 4th is refused (made %d, rc %d)", made, rc);
	CHECK(verify_all(k, true, "keep"), "the 3 are whole");

	// the drive full in the middle of a copy (its free count was wrong)
	truth_clear();
	memfs_t *f = fresh(120);
	fill(f, "/home/a.bin", 30000, 1);
	o = opts();
	o.reserve_kb = 4;
	backup_ok(f, &o, &r);
	memfs_tick(f, 60);
	fill(f, "/home/a.bin", 90000, 2);
	memfs_free_lie(f, 200);
	memfs_out_clear(f);
	CHECK(run(f, &o, &r) == BK_FAILED && strstr(memfs_out(f), "full"), "full mid-copy: said so");
	memfs_free_lie(f, 0);
	CHECK(verify_all(f, true, "full"), "the earlier backup is whole");
	blist_t bl = blist(f);
	CHECK(bl.n == 2 && bl.complete[0] && !bl.complete[1], "the cut one is incomplete");
	memfs_free(m); memfs_free(p); memfs_free(q); memfs_free(s); memfs_free(k); memfs_free(f);
}

// -- stopping ---------------------------------------------------------------

static void test_stop(void) {
	truth_clear();
	memfs_t *m = fresh(64 * 1024);
	card_basic(m);
	fill(m, "/home/big.bin", 200000, 1);
	bk_opts_t o = opts();
	bk_result_t r;
	backup_ok(m, &o, &r);
	memfs_tick(m, 60);
	fill(m, "/home/big.bin", 200000, 2);
	memfs_stop_after(m, 120);
	CHECK(run(m, &o, &r) == BK_FAILED, "stopped");
	memfs_stop_after(m, 0);
	blist_t bl = blist(m);
	CHECK(bl.n == 2 && !bl.complete[1], "the stopped one is incomplete");
	memfs_tick(m, 60);
	CHECK(backup_ok(m, &o, &r) == BK_DONE, "next run");
	bl = blist(m);
	CHECK(bl.n == 2 && bl.complete[1], "the cut one removed, the new one complete (%d)", bl.n);
	CHECK(verify_all(m, true, "stop"), "verify");
	memfs_free(m);
}

// -- a damaged index is not mistaken for a cut-short run --------------------

static void test_damaged_index(void) {
	truth_clear();
	memfs_t *m = fresh(64 * 1024);
	card_basic(m);
	bk_opts_t o = opts();
	bk_result_t r;
	backup_ok(m, &o, &r);
	char first[BK_NAME_MAX];
	snprintf(first, sizeof first, "%s", r.name);
	memfs_tick(m, 60);
	memfs_puts(m, "/home/book.txt", "two");
	backup_ok(m, &o, &r);
	// the first index loses its end
	char ip[BK_PATH_MAX];
	snprintf(ip, sizeof ip, "/usb/backup/zeitlos/%s/INDEX.TXT", first);
	uint32_t l;
	memfs_get(m, ip, &l);
	memfs_truncate(m, ip, l - 10);
	memfs_tick(m, 60);
	memfs_puts(m, "/home/book.txt", "three");
	memfs_out_clear(m);
	CHECK(backup_ok(m, &o, &r) == BK_DONE, "backup");
	CHECK(strstr(memfs_out(m), "damaged") != NULL, "reported");
	char cp[BK_PATH_MAX * 2];
	snprintf(cp, sizeof cp, "/usb/backup/zeitlos/%s/home/notes.txt", first);
	CHECK(memfs_exists(m, cp), "its copies kept (later backups use them)");
	fls_t f = files_of(m, NULL);
	int present = 0;
	for (int i = 0; i < f.n; i++) present += f.v[i].present;
	CHECK(present == f.n && f.n == 5, "newest restorable: %d of %d", present, f.n);
	free(f.v);
	memfs_free(m);

	// an index damaged in the middle (its end line intact): the CRC
	// catches it, check says so, and it is not used as the state
	truth_clear();
	m = fresh(64 * 1024);
	card_basic(m);
	backup_ok(m, &o, &r);
	snprintf(ip, sizeof ip, "/usb/backup/zeitlos/%s/INDEX.TXT", r.name);
	const uint8_t *d = memfs_get(m, ip, &l);
	const char *hit = strstr((const char *)d, "/home/book.txt");
	CHECK(hit && memfs_poke(m, ip, (uint32_t)(hit - (const char *)d) + 6, 'X'), "poke index");
	bk_io_t io;
	bk_result_t cr;
	memfs_io(m, &io);
	CHECK(bk_check(&io, &o, false, &cr) == BK_PARTIAL && cr.bad == 1, "check: damaged index (%u)", cr.bad);
	blist_t bl = blist(m);
	// the list looks only at last lines (cheap); check reads it all
	CHECK(bl.n == 1 && bl.complete[0], "a damaged index still looks complete to list");
	memfs_tick(m, 60);
	CHECK(backup_ok(m, &o, &r) == BK_DONE && r.copied == 5, "next backup starts afresh: %u", r.copied);
	memfs_free(m);

	// two backups, the newer index damaged in the middle: the next
	// backup builds on the older one, and every backup's files are right
	truth_clear();
	m = fresh(64 * 1024);
	card_basic(m);
	backup_ok(m, &o, &r);
	memfs_tick(m, 60);
	memfs_puts(m, "/home/book.txt", "second");
	backup_ok(m, &o, &r);
	snprintf(ip, sizeof ip, "/usb/backup/zeitlos/%s/INDEX.TXT", r.name);
	d = memfs_get(m, ip, &l);
	hit = strstr((const char *)d, "/home/notes.txt");
	CHECK(hit && memfs_poke(m, ip, (uint32_t)(hit - (const char *)d) + 7, 'Q'), "poke the newer index");
	memfs_free(truth[--ntruth].card);   // the damaged backup's own index cannot be trusted
	memfs_tick(m, 60);
	memfs_puts(m, "/home/book.txt", "third");
	memfs_out_clear(m);
	CHECK(backup_ok(m, &o, &r) == BK_DONE && strstr(memfs_out(m), "damaged"), "backup past a damaged index");
	CHECK(r.copied == 1, "built on the older backup: copied %u (the book)", r.copied);
	CHECK(verify_all(m, true, "damaged newer"), "verify");
	memfs_free(m);
}

// -- the destination inside a source ----------------------------------------

static void test_dest_inside(void) {
	truth_clear();
	memfs_t *m = fresh(64 * 1024);
	card_basic(m);
	bk_opts_t o = opts();
	o.dest = "/home/backups";
	bk_result_t r;
	CHECK(run(m, &o, &r) == BK_DONE, "first");
	memfs_tick(m, 60);
	memfs_puts(m, "/home/book.txt", "changed");
	CHECK(run(m, &o, &r) == BK_DONE && r.copied == 1, "second copies only the book: %u", r.copied);
	char p[BK_PATH_MAX];
	snprintf(p, sizeof p, "/home/backups/zeitlos/%s/home/backups", r.name);
	CHECK(!memfs_exists(m, p), "the backups are not backed up");
	memfs_free(m);
}

// -- power cuts -------------------------------------------------------------

static bool newest_matches_card(memfs_t *m, const char *ctx) {
	fls_t l = files_of(m, NULL);
	bool ok = true;
	cmp_t c = { m, 0, 0, true, &l };
	memfs_each(m, count_fn, &c);
	if (c.want != l.n) { printf("  %s: newest lists %d, card has %d\n", ctx, l.n, c.want); ok = false; }
	for (int i = 0; i < l.n; i++) {
		uint32_t a, b;
		const uint8_t *cd = memfs_get(m, l.v[i].path, &a);
		char cp[BK_PATH_MAX * 2];
		snprintf(cp, sizeof cp, "/usb/backup/zeitlos/%s%s", l.v[i].where, l.v[i].path);
		const uint8_t *bd = memfs_get(m, cp, &b);
		if (!cd || !bd || a != b || memcmp(cd, bd, a)) { printf("  %s: newest: %s differs\n", ctx, l.v[i].path); ok = false; }
	}
	free(l.v);
	return ok;
}

// The newest state has this file as the card has it now.
static bool newest_has(memfs_t *m, const char *path) {
	fls_t l = files_of(m, NULL);
	bool ok = false;
	for (int i = 0; i < l.n; i++) {
		if (strcmp(l.v[i].path, path)) continue;
		uint32_t a, b;
		const uint8_t *cd = memfs_get(m, path, &a);
		char cp[BK_PATH_MAX * 2];
		snprintf(cp, sizeof cp, "/usb/backup/zeitlos/%s%s", l.v[i].where, path);
		const uint8_t *bd = memfs_get(m, cp, &b);
		ok = cd && bd && a == b && !memcmp(cd, bd, a);
	}
	free(l.v);
	return ok;
}

// A backup, cut at each of its writes in turn.
static void cut_backups(const char *what, memfs_t *base, const bk_opts_t *o, bool strict) {
	int ntr = ntruth;
	int k, completed = 0;
	for (k = 1; k < 5000; k++) {
		memfs_t *m = memfs_clone(base);
		memfs_verbose(m, false);
		memfs_t *card_before = memfs_clone(base);
		memfs_cut_after(m, k);
		bk_result_t r;
		int rc = run(m, o, &r);
		bool cut = memfs_dead(m);
		if (!cut) {
			// the run finished before the k-th write: every point was covered
			CHECK(rc == BK_DONE, "%s: uncut run rc %d", what, rc);
			memfs_free(m); memfs_free(card_before);
			break;
		}
		memfs_reboot(m);
		CHECK(memfs_card_same(m, card_before), "%s cut %d: the card changed", what, k);
		char ctx[64];
		snprintf(ctx, sizeof ctx, "%s cut %d", what, k);
		ntruth = ntr;
		CHECK(verify_all(m, strict, ctx), "%s cut %d: earlier backups", what, k);
		// the next run works and holds the card
		bk_result_t r2;
		int rc2 = run(m, o, &r2);
		CHECK(rc2 == BK_DONE, "%s cut %d: next run rc %d", what, k, rc2);
		if (!o->nonly) CHECK(newest_matches_card(m, ctx), "%s cut %d: next run's backup", what, k);
		else CHECK(newest_has(m, o->only[0]), "%s cut %d: next watch run's backup", what, k);
		CHECK(verify_all(m, strict, ctx), "%s cut %d: earlier backups after the next run", what, k);
		bk_io_t io;
		bk_result_t cr;
		memfs_io(m, &io);
		bk_check(&io, o, true, &cr);
		CHECK(cr.bad == 0, "%s cut %d: check -a found %u problems", what, k, cr.bad);
		completed++;
		memfs_free(m); memfs_free(card_before);
	}
	ntruth = ntr;
	printf("  %s: cut at each of %d writes\n", what, k - 1);
}

static void test_power(void) {
	truth_clear();
	memfs_t *m = fresh(64 * 1024);
	card_basic(m);
	fill(m, "/home/big.bin", 9000, 1);
	bk_opts_t o = opts();
	bk_result_t r;
	backup_ok(m, &o, &r);
	memfs_tick(m, 60);
	memfs_puts(m, "/home/book.txt", "Chapter 1 again\n");
	memfs_rm(m, "/home/notes.txt");
	backup_ok(m, &o, &r);
	memfs_tick(m, 60);
	memfs_puts(m, "/home/book.txt", "Chapter 1 once more\n");
	fill(m, "/home/big.bin", 9000, 2);
	memfs_puts(m, "/home/fresh.txt", "new");
	cut_backups("a backup", m, &o, true);

	// a watch backup
	const char *only[] = { "/home/book.txt" };
	bk_opts_t w = opts();
	w.only = only; w.nonly = 1;
	cut_backups("a watch backup", m, &w, true);

	// a first backup on an empty drive
	truth_clear();
	memfs_t *e = fresh(64 * 1024);
	card_basic(e);
	cut_backups("a first backup", e, &o, true);
	memfs_free(e);

	// one that has to make room first
	truth_clear();
	memfs_t *t = fresh(300);
	memfs_puts(t, "/home/keep.txt", "never changes");
	fill(t, "/home/big.bin", 40000, 1);
	bk_opts_t ro = opts();
	ro.keep = 2; ro.reserve_kb = 8;
	for (int i = 2; i < 7; i++) {
		backup_ok(t, &ro, &r);
		memfs_tick(t, 100);
		fill(t, "/home/big.bin", 40000, (unsigned)i);
	}
	cut_backups("making room", t, &ro, false);
	memfs_free(t);

	// a restore with replace: the card file is old or new, never half
	truth_clear();
	memfs_t *s = fresh(64 * 1024);
	card_basic(s);
	backup_ok(s, &o, &r);
	memfs_puts(s, "/home/book.txt", "something else entirely, and longer than before\n");
	uint32_t ol;
	const uint8_t *od = memfs_get(s, "/home/book.txt", &ol);
	char *old = malloc(ol); memcpy(old, od, ol);
	int k;
	for (k = 1; k < 1000; k++) {
		memfs_t *c = memfs_clone(s);
		memfs_cut_after(c, k);
		bk_io_t io;
		bk_result_t rr;
		memfs_io(c, &io);
		bk_restore(&io, &o, NULL, "/home/book.txt", NULL, true, &rr);
		bool cut = memfs_dead(c);
		memfs_reboot(c);
		uint32_t l;
		const uint8_t *d = memfs_get(c, "/home/book.txt", &l);
		bool isold = d && l == ol && !memcmp(d, old, ol);
		bool isnew = d && l == 42 && !memcmp(d, "Chapter 1\nIt was a dark and stormy night.\n", 42);
		bool partok = memfs_exists(c, "/home/book.txt.part");
		CHECK(isold || isnew || (!d && partok), "restore cut %d: book.txt is neither old nor restored", k);
		memfs_free(c);
		if (!cut) break;
	}
	printf("  a restore: cut at each of %d writes\n", k - 1);
	free(old);
	memfs_free(s);
	memfs_free(m);
}

// -- a drive for tools/backup_read.py ---------------------------------------

static const char *dump_dir;

static void dump_fn(void *arg, const char *p, const uint8_t *d, uint32_t l, bool dir) {
	const char *top = arg;
	if (strncmp(p, "/usb/", 5)) return;
	char out[1024];
	snprintf(out, sizeof out, "%s%s", top, p);
	char cmd[1200];
	if (dir) { snprintf(cmd, sizeof cmd, "mkdir -p '%s'", out); if (system(cmd)) {} return; }
	char *s = strrchr(out, '/');
	*s = 0; snprintf(cmd, sizeof cmd, "mkdir -p '%s'", out); if (system(cmd)) {} *s = '/';
	FILE *f = fopen(out, "wb");
	if (f) { fwrite(d, 1, l, f); fclose(f); }
}

static void dump_truth_fn(void *arg, const char *p, const uint8_t *d, uint32_t l, bool dir) {
	FILE *f = arg;
	(void)d;
	if (dir || !wanted(p)) return;
	fprintf(f, "%08x %u %s\n", (unsigned)bk_crc32(0, d, l), (unsigned)l, p);
}

static int dump(const char *dir) {
	truth_clear();
	memfs_t *m = fresh(64 * 1024);
	card_basic(m);
	bk_opts_t o = opts();
	bk_result_t r;
	backup_ok(m, &o, &r);
	memfs_tick(m, 3600);
	memfs_puts(m, "/home/book.txt", "Chapter 1\nChapter 2\n");
	memfs_rm(m, "/home/notes.txt");
	memfs_puts(m, "/home/name with spaces.txt", "spaces");
	backup_ok(m, &o, &r);
	memfs_tick(m, 600);
	memfs_puts(m, "/home/book.txt", "Chapter 1\nChapter 2\nChapter 3\n");
	const char *only[] = { "/home/book.txt" };
	bk_opts_t w = opts(); w.only = only; w.nonly = 1;
	backup_ok(m, &w, &r);
	memfs_reboot(m);
	memfs_puts(m, "/home/notes.txt", "back again\n");
	backup_ok(m, &o, &r);
	char path[1024];
	snprintf(path, sizeof path, "mkdir -p '%s/truth'", dir);
	if (system(path)) {}
	memfs_each(m, dump_fn, (void *)dir);
	for (int i = 0; i < ntruth; i++) {
		snprintf(path, sizeof path, "%s/truth/%s.txt", dir, truth[i].name);
		FILE *f = fopen(path, "w");
		if (!f) return 1;
		fprintf(f, "%s\n", truth[i].watch ? "watch" : "full");
		if (truth[i].watch) {
			for (int k = 0; k < truth[i].nonly; k++) {
				uint32_t l;
				const uint8_t *d = memfs_get(truth[i].card, truth[i].only[k], &l);
				fprintf(f, "%08x %u %s\n", (unsigned)bk_crc32(0, d, l), (unsigned)l, truth[i].only[k]);
			}
		} else memfs_each(truth[i].card, dump_truth_fn, f);
		fclose(f);
	}
	truth_clear();
	memfs_free(m);
	printf("dumped to %s\n", dir);
	return 0;
}

int main(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) verbose = true;
		else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump_dir = argv[++i];
	}
	if (dump_dir) return dump(dump_dir);
	test_names();
	test_basic();
	test_noclock();
	test_same_second();
	test_manifest();
	test_watch();
	test_refresh_repair();
	test_lock();
	test_room();
	test_stop();
	test_damaged_index();
	test_dest_inside();
	test_power();
	truth_clear();
	printf("test_bk: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
