/*
 * Zeitlos -- update's core on the host (docs/update.md, "Testing").
 *
 *   make -C sw/apps/update/tests
 *
 * The card is a directory, the server is the store mkfixture.py
 * published with release/lib's own code, and upd.c is the file the board
 * runs. What it checks:
 *
 *   - a blank card, then the same release again (nothing fetched);
 *   - v1 -> v2: changed, added, removed, a new folder two deep, a name
 *     with a space; an edited KEEP file left, an unedited one replaced;
 *     the person's own files untouched;
 *   - a file removed upstream but edited here is left (and unlisted);
 *   - a card from before manifests: nothing removed, KEEP files kept;
 *   - -v hashes everything and repairs an edited REPLACE file;
 *   - a file in use: left, the run says so, the next run finishes it
 *     (from the .upd, without fetching again);
 *   - STOPPED AT EVERY STEP: the run is cut off after its first, second,
 *     ... mutating call, then run again to the end, and each time the
 *     card must be exactly v2, with no .upd or .old left behind;
 *   - a damaged object, a missing object, a damaged and a newer-format
 *     manifest, not enough room, -n, "latest", a bad release name.
 */

#define _XOPEN_SOURCE 700
#include <errno.h>
#include <ftw.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "upd.h"

// -- a counting allocator for upd.c (alloc_hook.h) ----------------------------
//
// A header before each block holds its size. `heap_now` and `heap_peak`
// count what upd.c holds; past `heap_limit` (0: none) a request fails,
// as malloc() does on the board when the app's heap is used up.

static size_t heap_now, heap_peak, heap_limit;

typedef struct { size_t n; size_t pad; } hblk_t;

void *t_malloc(size_t n) {
	if (heap_limit && heap_now + n > heap_limit) return NULL;
	hblk_t *b = malloc(sizeof *b + n);
	if (!b) return NULL;
	b->n = n;
	heap_now += n;
	if (heap_now > heap_peak) heap_peak = heap_now;
	return b + 1;
}

void t_free(void *p) {
	if (!p) return;
	hblk_t *b = (hblk_t *)p - 1;
	heap_now -= b->n;
	free(b);
}

void *t_calloc(size_t n, size_t m) {
	void *p = t_malloc(n * m);
	if (p) memset(p, 0, n * m);
	return p;
}

// As on the board: the new block is had before the old one is let go.
void *t_realloc(void *p, size_t n) {
	if (!p) return t_malloc(n);
	hblk_t *b = (hblk_t *)p - 1;
	void *q = t_malloc(n);
	if (!q) return NULL;
	memcpy(q, p, b->n < n ? b->n : n);
	t_free(p);
	return q;
}

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
	printf("  FAIL  "); printf(__VA_ARGS__); printf("  (%s:%d)\n", __FILE__, __LINE__); } \
	else if (verbose) { printf("  ok    "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static bool verbose;
static char fx[512];        // the fixture directory
static char card[600];      // the card, a directory
static char store[600];     // the server's root: store/ is "release/"

// -- the host's upd_io_t --------------------------------------------------

#define MAXH 8
static FILE *hs[MAXH];
static bool hw[MAXH];
static char outbuf[1 << 20];
static int outlen;
static uint32_t clock_ms;
static uint64_t free_kb = 1 << 30;
static const char *busy_path;          // rename of this (from) is EBUSY
static int fetches;                    // objects (not manifests) fetched
static FILE *src;
static long kill_at = -1, ops;         // stop after this many mutations
static jmp_buf killed;
static bool stop_flag;

static void full(char *out, const char *path) {
	snprintf(out, 1200, "%s%s", card, path);
}

static void mutate(void) {
	if (kill_at >= 0 && ++ops > kill_at) longjmp(killed, 1);
}

static int h_open_read(void *c, const char *p) {
	(void)c;
	char f[1200];
	full(f, p);
	struct stat st;
	if (stat(f, &st) || !S_ISREG(st.st_mode)) return UPD_ENOENT;
	for (int i = 0; i < MAXH; i++)
		if (!hs[i]) {
			hs[i] = fopen(f, "rb");
			hw[i] = false;
			return hs[i] ? i : UPD_ERR;
		}
	return UPD_ERR;
}

static int h_open_write(void *c, const char *p) {
	(void)c;
	mutate();
	char f[1200];
	full(f, p);
	for (int i = 0; i < MAXH; i++)
		if (!hs[i]) {
			hs[i] = fopen(f, "wb");
			hw[i] = true;
			return hs[i] ? i : UPD_ERR;
		}
	return UPD_ERR;
}

static long bytes_read;		// read from the card, by anything

static int h_read(void *c, int h, void *b, int n) {
	(void)c;
	int r = (int)fread(b, 1, n, hs[h]);
	if (r > 0) bytes_read += r;
	return r;
}

static int h_write(void *c, int h, const void *b, int n) {
	(void)c;
	mutate();
	return (int)fwrite(b, 1, n, hs[h]);
}

static int h_close(void *c, int h) {
	(void)c;
	if (hw[h]) mutate();
	int r = fclose(hs[h]);
	hs[h] = NULL;
	return r ? UPD_ERR : UPD_OK;
}

static int h_size(void *c, const char *p, uint32_t *out) {
	(void)c;
	char f[1200];
	full(f, p);
	struct stat st;
	if (stat(f, &st) || !S_ISREG(st.st_mode)) return UPD_ENOENT;
	*out = (uint32_t)st.st_size;
	return UPD_OK;
}

static int h_unlink(void *c, const char *p) {
	(void)c;
	char f[1200];
	full(f, p);
	struct stat st;
	if (stat(f, &st)) return UPD_ENOENT;
	mutate();
	return unlink(f) ? UPD_ERR : UPD_OK;
}

static int h_rename(void *c, const char *a, const char *b) {
	(void)c;
	char fa[1200], fb[1200];
	full(fa, a);
	full(fb, b);
	struct stat st;
	if (busy_path && !strcmp(a, busy_path)) return UPD_EBUSY;
	if (stat(fa, &st)) return UPD_ENOENT;
	if (!stat(fb, &st)) return UPD_EEXIST;	// FAT: rename never replaces
	mutate();
	return rename(fa, fb) ? UPD_ERR : UPD_OK;
}

static int h_mkdir(void *c, const char *p) {
	(void)c;
	char f[1200];
	full(f, p);
	struct stat st;
	if (!stat(f, &st)) return S_ISDIR(st.st_mode) ? UPD_OK : UPD_ERR;
	mutate();
	return mkdir(f, 0755) ? UPD_ERR : UPD_OK;
}

static int h_free_kb(void *c, uint32_t *kb) {
	(void)c;
	*kb = (uint32_t)free_kb;
	return UPD_OK;
}

static int h_fetch_open(void *c, const char *name, char *err, int errlen) {
	(void)c;
	char f[1200];
	// The server's root holds the store as release/ (or whatever dir).
	snprintf(f, sizeof f, "%s/%s", store, name);
	if (strncmp(name, "release/", 8)) {
		snprintf(err, errlen, "file not found");
		return UPD_ERR;
	}
	snprintf(f, sizeof f, "%s/%s", store, name + 8);
	src = fopen(f, "rb");
	if (!src) {
		snprintf(err, errlen, "file not found");
		return UPD_ERR;
	}
	if (strstr(name, "/o/")) fetches++;
	return UPD_OK;
}

static int h_fetch_read(void *c, uint8_t *b, int cap, char *err, int errlen) {
	(void)c; (void)err; (void)errlen;
	int n = cap < 1468 ? cap : 1468;		// a TFTP block
	return (int)fread(b, 1, n, src);
}

static void h_fetch_close(void *c) {
	(void)c;
	if (src) fclose(src);
	src = NULL;
}

static void h_out(void *c, const char *t) {
	(void)c;
	int n = (int)strlen(t);
	if (outlen + n < (int)sizeof outbuf - 1) {
		memcpy(outbuf + outlen, t, n);
		outlen += n;
		outbuf[outlen] = 0;
	}
	if (verbose) fputs(t, stdout);
}

static uint32_t h_ms(void *c) {
	(void)c;
	return clock_ms += 7;
}

static bool h_stop(void *c) {
	(void)c;
	return stop_flag;
}

static const upd_io_t IO = {
	NULL, h_open_read, h_open_write, h_read, h_write, h_close, h_size,
	h_unlink, h_rename, h_mkdir, h_free_kb, h_fetch_open, h_fetch_read,
	h_fetch_close, h_out, h_ms, h_stop
};

// -- helpers ---------------------------------------------------------------

static void sh(const char *fmt, ...) {
	char b[4096];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	if (system(b) != 0) {
		printf("command failed: %s\n", b);
		exit(2);
	}
}

static void fresh_card(void) {
	sh("rm -rf '%s' && mkdir -p '%s'", card, card);
}

static int run(const char *release, bool dry, bool verify) {
	upd_opts_t o = { release, "release", "test", dry, verify };
	outlen = 0;
	outbuf[0] = 0;
	fetches = 0;
	bytes_read = 0;
	return upd_run(&IO, &o);
}

// A run cut off after `n` mutations. true if it was cut off.
static bool run_killed(const char *release, long n) {
	kill_at = n;
	ops = 0;
	if (setjmp(killed)) {
		kill_at = -1;
		for (int i = 0; i < MAXH; i++)
			if (hs[i]) { fclose(hs[i]); hs[i] = NULL; }
		h_fetch_close(NULL);
		return true;
	}
	run(release, false, false);
	kill_at = -1;
	return false;
}

static char *slurp(const char *path, long *len) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *b = malloc(n + 1);
	if (fread(b, 1, n, f) != (size_t)n) n = 0;
	b[n] = 0;
	fclose(f);
	if (len) *len = n;
	return b;
}

static bool same_file(const char *a, const char *b) {
	long la, lb;
	char *x = slurp(a, &la), *y = slurp(b, &lb);
	bool eq = x && y && la == lb && !memcmp(x, y, la);
	free(x);
	free(y);
	return eq;
}

static void write_card(const char *path, const char *text) {
	char f[1200];
	full(f, path);
	FILE *o = fopen(f, "wb");
	fputs(text, o);
	fclose(o);
}

static bool card_has(const char *path) {
	char f[1200];
	full(f, path);
	struct stat st;
	return !stat(f, &st);
}

static bool card_is(const char *path, const char *text) {
	char f[1200];
	full(f, path);
	char *b = slurp(f, NULL);
	bool eq = b && !strcmp(b, text);
	free(b);
	return eq;
}

static bool card_matches_src(const char *path, const char *rel) {
	char f[1200], s[1200];
	full(f, path);
	snprintf(s, sizeof s, "%s/src/%s%s", fx, rel, path);
	return same_file(f, s);
}

static char id_v1[16], id_v2[16];

static bool manifest_is(const char *id) {
	char f[1200];
	full(f, "/sys/manifest");
	char *b = slurp(f, NULL);
	char want[64];
	snprintf(want, sizeof want, "\nsnapshot %s\n", id);
	bool ok = b && strstr(b, want);
	free(b);
	return ok;
}

// The card holds exactly release `rel`'s files (+ /sys, + `extra`), and
// nothing left over from an interrupted run.
static const char *tree_rel, *tree_extra;
static int tree_bad;
static char tree_first[600];

static int walk(const char *p, const struct stat *st, int type, struct FTW *ftw) {
	(void)st; (void)ftw;
	if (type != FTW_F) return 0;
	const char *rel = p + strlen(card);
	size_t n = strlen(rel);
	bool ok;
	char s[1200];
	if ((n > 4 && !strcmp(rel + n - 4, ".upd")) || (n > 4 && !strcmp(rel + n - 4, ".old")))
		ok = false;
	else if (!strcmp(rel, "/sys/manifest") || !strcmp(rel, "/sys/version"))
		ok = true;
	else if (tree_extra && strstr(tree_extra, rel))
		ok = true;
	else {
		snprintf(s, sizeof s, "%s/src/%s%s", fx, tree_rel, rel);
		ok = same_file(p, s);
	}
	if (!ok && !tree_bad++) snprintf(tree_first, sizeof tree_first, "%s", rel);
	return 0;
}

// Every file of the release is on the card, the same (except `extra`).
static int walk_src(const char *p, const struct stat *st, int type, struct FTW *ftw) {
	(void)st; (void)ftw;
	if (type != FTW_F) return 0;
	char s[1200], c[1200];
	snprintf(s, sizeof s, "%s/src/%s", fx, tree_rel);
	const char *rel = p + strlen(s);
	if (tree_extra && strstr(tree_extra, rel)) return 0;
	snprintf(c, sizeof c, "%s%s", card, rel);
	if (!same_file(p, c) && !tree_bad++)
		snprintf(tree_first, sizeof tree_first, "%s differs or is missing", rel);
	return 0;
}

static bool card_is_release(const char *rel, const char *extra) {
	char s[1200];
	tree_rel = rel;
	tree_extra = extra;
	tree_bad = 0;
	tree_first[0] = 0;
	nftw(card, walk, 16, FTW_PHYS);
	snprintf(s, sizeof s, "%s/src/%s", fx, rel);
	nftw(s, walk_src, 16, FTW_PHYS);
	return tree_bad == 0;
}

static bool said(const char *s) {
	return strstr(outbuf, s) != NULL;
}

// -- the cases ---------------------------------------------------------------

static void blank_card(void) {
	printf("a blank card\n");
	fresh_card();
	int r = run("v1", false, false);
	CHECK(r == UPD_DONE, "v1 onto a blank card: done (%d)", r);
	CHECK(card_is_release("v1", NULL), "the card is v1 exactly%s%s",
		tree_bad ? ": " : "", tree_first);
	CHECK(manifest_is(id_v1), "/sys/manifest says snapshot %s", id_v1);
	CHECK(card_is("/sys/version", "zeitlos 0.0.7\nsnapshot v1 ") ||
		said("done"), "/sys/version written");
	{
		char f[1200];
		full(f, "/sys/version");
		char *b = slurp(f, NULL);
		CHECK(b && !strncmp(b, "zeitlos 0.0.7\nsnapshot v1 ", 26) && strstr(b, id_v1),
			"/sys/version: zeitlos 0.0.7, then the snapshot");
		free(b);
	}
	CHECK(card_has("/home"), "the empty folder /home is made");
	CHECK(fetches == 7, "7 objects fetched (%d)", fetches);
	CHECK(said("100%") || said("KB/s"), "progress shown");

	r = run("v1", false, false);
	CHECK(r == UPD_DONE && said("up to date") && fetches == 0,
		"again: up to date, nothing fetched");
}

static void v1_to_v2(void) {
	printf("v1 -> v2\n");
	fresh_card();
	run("v1", false, false);
	write_card("/data/x/cfg", "# my own settings\nkey: mine\n");
	write_card("/home/mine.txt", "mine\n");
	int r = run("v2", false, false);
	CHECK(r == UPD_DONE, "done (%d)", r);
	CHECK(card_is("/data/x/cfg", "# my own settings\nkey: mine\n"),
		"an edited KEEP file is left as it is");
	CHECK(card_is("/home/mine.txt", "mine\n"), "a file in no manifest is untouched");
	CHECK(!card_has("/data/x/old.txt"), "a file dropped from the release is removed");
	CHECK(card_matches_src("/apps/a", "v2"), "a changed app is replaced");
	CHECK(card_matches_src("/apps/new", "v2"), "a new app is added");
	CHECK(card_matches_src("/data/y/deep/f", "v2"), "into a new folder two deep");
	CHECK(card_matches_src("/data/x/ex.c", "v2"), "a new KEEP file is added");
	CHECK(card_matches_src("/docs/my notes.md", "v2"), "a name with a space");
	CHECK(fetches == 4, "4 objects fetched, not 9 (%d)", fetches);
	CHECK(manifest_is(id_v2), "the card's snapshot is v2's: the kept file counts as done");
	CHECK(card_is_release("v2", "/data/x/cfg /home/mine.txt"),
		"otherwise exactly v2%s%s", tree_bad ? ": " : "", tree_first);
	r = run("v2", false, false);
	CHECK(r == UPD_DONE && said("up to date"), "again: up to date");

	printf("an unedited KEEP file\n");
	fresh_card();
	run("v1", false, false);
	run("v2", false, false);
	CHECK(card_matches_src("/data/x/cfg", "v2"), "is replaced by the new one");

	printf("a removed file that was edited\n");
	fresh_card();
	run("v1", false, false);
	write_card("/data/x/old.txt", "I changed this\n");
	r = run("v2", false, false);
	CHECK(r == UPD_DONE && card_is("/data/x/old.txt", "I changed this\n"),
		"is left");
	CHECK(said("no longer shipped, but changed"), "and the run says so");
	{
		char f[1200];
		full(f, "/sys/manifest");
		char *b = slurp(f, NULL);
		CHECK(b && !strstr(b, "old.txt"), "and it is no longer in the manifest");
		free(b);
	}
}

static void no_manifest(void) {
	printf("a card from before manifests\n");
	fresh_card();
	sh("cp -r '%s/src/v1/.' '%s/'", fx, card);
	write_card("/data/x/cfg", "# edited on an old card\n");
	int r = run("v2", false, false);
	CHECK(r == UPD_DONE, "done (%d)", r);
	CHECK(said("no /sys/manifest"), "says it is checking every file");
	CHECK(card_has("/data/x/old.txt"), "nothing is removed: nothing says it was shipped");
	CHECK(card_is("/data/x/cfg", "# edited on an old card\n"), "KEEP files are kept");
	CHECK(card_matches_src("/apps/a", "v2"), "REPLACE files that differ are replaced");
	CHECK(fetches == 4, "only what differs is fetched (%d)", fetches);
	CHECK(manifest_is(id_v2), "and now it has a manifest");
}

static void format1(void) {
	printf("a card whose manifest is format 1 (no CRC)\n");
	fresh_card();
	run("v1", false, false);
	sh("cp '%s/v1.manifest1' '%s/sys/manifest'", fx, card);
	int r = run("v2", false, false);
	// 4 changed or new, and the unedited KEEP file: recognised as v1's by
	// SHA-256, since a format-1 entry has no CRC.
	CHECK(r == UPD_DONE && fetches == 5, "v1 -> v2: done, 5 fetched (%d, %d)", r, fetches);
	CHECK(card_matches_src("/data/x/cfg", "v2"), "the unedited KEEP file is replaced");
	CHECK(bytes_read < 20000, "the unchanged files are not read: the manifests "
		"answer (%ld bytes read)", bytes_read);
	CHECK(manifest_is(id_v2) && card_is_release("v2", NULL),
		"the card is v2, its manifest now format 2%s%s", tree_bad ? ": " : "", tree_first);
	{
		char f[1200];
		full(f, "/sys/manifest");
		char *b = slurp(f, NULL);
		CHECK(b && !strncmp(b, "zeitlos-manifest 2\n", 19), "written as format 2");
		free(b);
	}

	printf("checking a card without a manifest reads each file once\n");
	fresh_card();
	sh("cp -r '%s/src/v1/.' '%s/'", fx, card);
	r = run("v1", false, false);
	CHECK(r == UPD_DONE && fetches == 0, "nothing to fetch (%d, %d)", r, fetches);
	CHECK(bytes_read > 300000 && bytes_read < 340000,
		"every file read once, by CRC (%ld bytes)", bytes_read);
}

static void memory(void) {
	printf("memory\n");
	// Out of memory at every point: the heap limit rises until the run
	// succeeds. Each failed run must stop cleanly, and the next run,
	// with all the memory it wants, must leave exactly the release.
	int failed = 0, bad = 0;
	size_t ok_at = 0;
	for (size_t lim = 2048; lim < 600000; lim += lim / 8) {
		fresh_card();
		run("v1", false, false);
		write_card("/data/x/cfg", "# edited\n");
		heap_limit = lim;
		int r = run("v2", false, false);
		heap_limit = 0;
		if (r == UPD_DONE) { ok_at = lim; break; }
		failed++;
		r = run("v2", false, false);
		if (r != UPD_DONE || !card_is_release("v2", "/data/x/cfg") ||
				!manifest_is(id_v2) || !card_is("/data/x/cfg", "# edited\n")) {
			if (bad++ < 3) printf("    limit %lu: r=%d %s\n%s\n", (unsigned long)lim,
				r, tree_first, outbuf);
		}
	}
	CHECK(failed > 10 && ok_at, "ran out of memory at %d limits, then succeeded "
		"with %lu bytes", failed, (unsigned long)ok_at);
	CHECK(bad == 0, "after each, the next run left exactly v2 (%d bad)", bad);

	heap_peak = 0;
	fresh_card();
	run("v1", false, false);
	run("v2", false, false);
	// Mostly the 64 KB a release manifest is fetched into.
	CHECK(heap_peak < 128 * 1024, "the fixture's peak is %lu bytes",
		(unsigned long)heap_peak);
	CHECK(heap_now == 0, "and all of it is freed (%lu left)", (unsigned long)heap_now);
}

static void verify(void) {
	printf("-v\n");
	fresh_card();
	run("v2", false, false);
	write_card("/docs/d.md", "broken by hand\n");
	int r = run("v2", false, false);
	CHECK(r == UPD_DONE && said("up to date") && card_is("/docs/d.md", "broken by hand\n"),
		"without -v the manifest is trusted");
	r = run("v2", false, true);
	CHECK(r == UPD_DONE && card_matches_src("/docs/d.md", "v2") && fetches == 1,
		"-v hashes every file and repairs it (%d fetched)", fetches);
}

static void in_use(void) {
	printf("a file in use\n");
	fresh_card();
	run("v1", false, false);
	busy_path = "/apps/a";
	int r = run("v2", false, false);
	busy_path = NULL;
	CHECK(r == UPD_PARTIAL, "partly done (%d)", r);
	CHECK(said("in use by a running program"), "says which");
	CHECK(card_matches_src("/apps/a", "v1"), "the file is left as it was");
	CHECK(card_has("/apps/a.upd"), "its new version waits as a .upd");
	CHECK(card_matches_src("/apps/new", "v2"), "the rest is done");
	CHECK(!manifest_is(id_v2), "the card's snapshot is not v2's yet");
	r = run("v2", false, false);
	CHECK(r == UPD_DONE && said("already fetched") && fetches == 0,
		"the next run finishes it from the .upd, fetching nothing");
	CHECK(card_is_release("v2", NULL) && manifest_is(id_v2),
		"and the card is v2%s%s", tree_bad ? ": " : "", tree_first);
}

static void stopped_everywhere(void) {
	printf("stopped after every step, then run again\n");
	int bad = 0, cut = 0;
	long n;
	for (n = 0; ; n++) {
		fresh_card();
		run("v1", false, false);
		write_card("/data/x/cfg", "# edited\n");
		write_card("/home/mine.txt", "mine\n");
		if (!run_killed("v2", n)) break;	// ran to the end
		cut++;
		int r = run("v2", false, false);
		if (r != UPD_DONE || !card_is_release("v2", "/data/x/cfg /home/mine.txt") ||
				!manifest_is(id_v2) || !card_is("/data/x/cfg", "# edited\n") ||
				!card_is("/home/mine.txt", "mine\n")) {
			if (bad++ < 3)
				printf("    after %ld steps: r=%d %s\n%s\n", n, r, tree_first, outbuf);
		}
		// and a third run has nothing to do
		if (run("v2", false, false) != UPD_DONE || !said("up to date")) {
			if (bad++ < 3) printf("    after %ld steps: not up to date after\n", n);
		}
	}
	CHECK(cut > 25, "the run was cut at %d different points", cut);
	CHECK(bad == 0, "every one finished as v2, edits kept, nothing left over (%d bad)", bad);

	printf("stopped on request\n");
	fresh_card();
	run("v1", false, false);
	stop_flag = true;
	int r = run("v2", false, false);
	stop_flag = false;
	CHECK(r == UPD_PARTIAL && said("stopped"), "stops, and says so (%d)", r);
	r = run("v2", false, false);
	CHECK(r == UPD_DONE && card_is_release("v2", NULL), "and the next run finishes");
}

static void bad_store(void) {
	char saved[700];
	snprintf(saved, sizeof saved, "%s.saved", store);
	sh("rm -rf '%s' && cp -r '%s' '%s'", saved, store, saved);

	printf("a damaged object\n");
	int r;
	// Same length, one byte changed: only the hash can tell.
	static const char *damage[2] = {
		"printf X | dd of=\"$f\" bs=1 seek=0 conv=notrunc 2>/dev/null",
		"printf X >> \"$f\"" };
	static const char *why[2] = { "does not match the manifest",
		"longer than the manifest says" };
	for (int d = 0; d < 2; d++) {
		fresh_card();
		run("v1", false, false);
		sh("cd '%s/o' && for f in */*; do %s; done", store, damage[d]);
		r = run("v2", false, false);
		CHECK(r == UPD_PARTIAL && said(why[d]), "%s: refused (%d)",
			d ? "longer" : "same length", r);
		CHECK(said("retrying"), "after one retry");
		CHECK(card_is_release("v1", NULL) && manifest_is(id_v1),
			"and the card is still v1, consistent%s%s", tree_bad ? ": " : "", tree_first);
		sh("rm -rf '%s' && cp -r '%s' '%s'", store, saved, store);
		r = run("v2", false, false);
		CHECK(r == UPD_DONE && card_is_release("v2", NULL),
			"fixed on the server: the next run finishes");
	}

	printf("a missing object\n");
	fresh_card();
	run("v1", false, false);
	sh("rm -rf '%s/o'", store);
	r = run("v2", false, false);
	CHECK(r == UPD_PARTIAL && said("file not found"), "reported (%d)", r);
	CHECK(card_is_release("v1", NULL), "the card is unchanged");
	sh("rm -rf '%s' && cp -r '%s' '%s'", store, saved, store);

	printf("a damaged manifest\n");
	sh("sed -i 's/^f \\(.\\)/f 0/' '%s/v2/manifest'", store);
	r = run("v2", false, false);
	CHECK(r == UPD_FAILED && said("does not match its files"), "is refused (%d)", r);
	sh("rm -rf '%s' && cp -r '%s' '%s'", store, saved, store);

	printf("a newer manifest format\n");
	sh("sed -i 's/^zeitlos-manifest 2/zeitlos-manifest 3/' '%s/v2/manifest'", store);
	r = run("v2", false, false);
	CHECK(r == UPD_FAILED && said("newer than this update"), "is refused (%d)", r);
	CHECK(said("release/v2/update /apps/update"), "and says how to get a newer update");
	sh("rm -rf '%s' && cp -r '%s' '%s'", store, saved, store);

	printf("no such release\n");
	r = run("v9", false, false);
	CHECK(r == UPD_FAILED && said("file not found"), "is reported (%d)", r);
	r = run("../x", false, false);
	CHECK(r == UPD_USAGE, "a bad name is refused before asking (%d)", r);
	sh("rm -rf '%s'", saved);
}

static void misc(void) {
	printf("not enough room\n");
	fresh_card();
	free_kb = 100;
	int r = run("v1", false, false);
	free_kb = 1 << 30;
	CHECK(r == UPD_FAILED && said("not enough room"), "refused before writing (%d)", r);
	CHECK(!card_has("/apps") && !card_has("/sys/manifest"), "nothing was written");

	printf("-n\n");
	fresh_card();
	run("v1", false, false);
	r = run("v2", true, false);
	CHECK(r == UPD_DONE && said("replace  apps/a") && said("add      apps/new") &&
		said("remove   data/x/old.txt") && fetches == 0, "lists the changes, fetches nothing");
	CHECK(card_is_release("v1", NULL) && manifest_is(id_v1), "and changes nothing");

	printf("latest\n");
	fresh_card();
	r = run("latest", false, false);
	CHECK(r == UPD_DONE && manifest_is(id_v2), "is whatever LATEST names: v2");

	printf("what the card has\n");
	outlen = 0;
	upd_show(&IO);
	CHECK(said("zeitlos 0.0.7") && said("/sys/manifest: 9 files"),
		"update with no release shows the version and the manifest");
	fresh_card();
	outlen = 0;
	upd_show(&IO);
	CHECK(said("written before updates"), "and says so for a card without one");
}

static void read_ids(void) {
	char p[700];
	snprintf(p, sizeof p, "%s/ids", fx);
	FILE *f = fopen(p, "r");
	if (!f || fscanf(f, "v1 %15s\nv2 %15s", id_v1, id_v2) != 2) {
		printf("no %s: run mkfixture.py first\n", p);
		exit(2);
	}
	fclose(f);
}

int main(int argc, char **argv) {
	if (argc < 2) {
		printf("usage: test_update FIXTURE [-v]\n"
		       "       test_update --update CARD STORE RELEASE [-n] [-v]\n");
		return 2;
	}
	// The core on any directory and store: a card laid out on the host
	// (or one in a card reader) brought up to date from a store that
	// `zrelease tftp` wrote. For trying things; the board runs update.
	if (!strcmp(argv[1], "--update") && argc >= 5) {
		bool dry = false, ver = false;
		for (int i = 5; i < argc; i++) {
			if (!strcmp(argv[i], "-n")) dry = true;
			if (!strcmp(argv[i], "-v")) ver = true;
		}
		verbose = true;
		snprintf(card, sizeof card, "%s", argv[2]);
		snprintf(store, sizeof store, "%s", argv[3]);
		upd_opts_t o = { argv[4], "release", "a directory", dry, ver };
		heap_peak = 0;
		int r = upd_run(&IO, &o);
		printf("(heap: %lu bytes at the peak)\n", (unsigned long)heap_peak);
		return r;
	}
	verbose = argc > 2 && !strcmp(argv[2], "-v");
	snprintf(fx, sizeof fx, "%s", argv[1]);
	snprintf(card, sizeof card, "%s/card", fx);
	snprintf(store, sizeof store, "%s/store", fx);
	read_ids();

	blank_card();
	v1_to_v2();
	no_manifest();
	format1();
	memory();
	verify();
	in_use();
	stopped_everywhere();
	bad_store();
	misc();

	printf("\n%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
