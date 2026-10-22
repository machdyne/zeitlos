/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * bkjob.c on the host (docs/backup.md, "The window"): the engine in a
 * coroutine, run a slice at a time as the window runs it, over the real
 * fs driver and FatFs (test_fs.c's stubs, as test_fat.c), and measured:
 *
 *   - no slice makes more than one request to fs (docs/responsive.md:
 *     the main loop never waits on more than one);
 *   - cancelled at every slice of a backup in turn, the job lets go
 *     within a few slices, the drive is as the rules say (the next
 *     backup works, check -a is clean), and the card is untouched;
 *   - the coroutine's stack: how much the engine uses.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define USB_SECTORS (40u * 2048u)
#define main test_fs_main
#include "../../fs/tests/test_fs.c"
#undef main
#undef CHECK

#include "bk.h"
#include "bk_zfs.h"
#include "bkjob.h"

static int tchecks, tfails;
#define T(c, ...) do { tchecks++; if (!(c)) { tfails++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static bool io_clock(void *c, bk_tm_t *t, uint32_t *up) {
	(void)c; static int s;
	t->year = 2026; t->month = 10; t->day = 21; t->hour = 12; t->min = s / 60; t->sec = s % 60; s++;
	*up = 100;
	return true;
}
static void io_sleep(void *c, int ms) { (void)c; (void)ms; }
static void io_owner(void *c, char *b, int n) { (void)c; snprintf(b, (size_t)n, "job"); }
static bool io_alive(void *c, const char *w) { (void)c; (void)w; return false; }

static const char *const FROM[] = { "/home" };
static bk_opts_t opts(void) {
	bk_opts_t o;
	memset(&o, 0, sizeof o);
	o.dest = "/usb/backup"; o.name = "zeitlos";
	o.from = FROM; o.nfrom = 1;
	o.keep = 10; o.refresh = 10; o.verify = true; o.reserve_kb = 64;
	return o;
}

static void put(const char *p, uint32_t len, unsigned s) {
	uint8_t *b = malloc(len + 1);
	for (uint32_t i = 0; i < len; i++) b[i] = (uint8_t)(s * 31u + i * 7u + (i >> 9));
	int32_t fd = zfs_open(p, Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC);
	if (fd >= 0) { zfs_write(fd, b, len); zfs_close(fd); }
	free(b);
}

// A job run to the end as the window runs it. The most requests any
// one slice made; the slices.
static int run_job(bkjob_t *j, uint32_t *maxreq, uint32_t *slices, int cancel_at, uint32_t *after) {
	T(bkjob_start(j), "start");
	uint32_t mx = 0, n = 0, aft = 0;
	bool cancelled = false;
	for (;;) {
		uint32_t r0 = reqs;
		bool more = bkjob_step();
		uint32_t d = reqs - r0;
		if (d > mx) mx = d;
		n++;
		if (cancelled) aft++;
		if (!more) break;
		if (cancel_at && (int)n == cancel_at) { bkjob_cancel(); cancelled = true; }
	}
	if (maxreq) *maxreq = mx;
	if (slices) *slices = n;
	if (after) *after = aft;
	return bkjob_current()->rc;
}

int main(void) {
	char scratch[512], c[1200];
	const char *tmp = getenv("TMPDIR");
	snprintf(scratch, sizeof scratch, "%s/test_job.XXXXXX", tmp ? tmp : "/tmp");
	if (!mkdtemp(scratch)) return 2;
	card = calloc(CARD_SECTORS, 512);
	ram = calloc(RAM_SECTORS, 512);
	for (int i = 0; i < 64; i++) alive[i] = true;
	fs_test_start(true);
	zfs_mkfs("/");
	snprintf(c, sizeof c, "mkfs.fat -F 32 -s 1 -C '%s/u.img' %u >/dev/null", scratch, USB_SECTORS / 2);
	if (system(c)) { printf("test_job: mkfs.fat failed\n"); return 2; }
	snprintf(c, sizeof c, "%s/u.img", scratch);
	FILE *f = fopen(c, "rb");
	usbd = calloc(USB_SECTORS, 512);
	if (!f || fread(usbd, 512, USB_SECTORS, f) != USB_SECTORS) return 2;
	fclose(f);
	usb_present = usb_ready = true; usb_gen++;
	fs_test_usb_poll();

	zfs_mkdir("/home/pics");
	for (int i = 0; i < 40; i++) {
		char p[64];
		snprintf(p, sizeof p, "/home/%sfile%02d.txt", i & 1 ? "pics/" : "", i);
		put(p, 100 + (uint32_t)i * 900, (unsigned)i);
	}
	put("/home/big.bin", 200000, 99);
	// a folder whose listing is several pages (fs sends 2 KB at a time)
	zfs_mkdir("/home/many");
	for (int i = 0; i < 120; i++) {
		char p[96];
		snprintf(p, sizeof p, "/home/many/a rather long file name to fill pages %03d.txt", i);
		put(p, 20, (unsigned)i);
	}

	// the stack is the frame's, as the window keeps it (bkjob.h)
	static bool once;
	uint8_t stack[48 * 1024];
	bk_io_t base;
	memset(&base, 0, sizeof base);
	bk_zfs_io(&base);
	base.clock = io_clock; base.sleep_ms = io_sleep; base.owner = io_owner; base.owner_alive = io_alive;
	bkjob_init(&base, stack, sizeof stack);
	(void)once;

	bkjob_t j;
	memset(&j, 0, sizeof j);
	j.kind = BKJOB_BACKUP;
	j.o = opts();
	uint32_t mx, sl, aft;
	T(run_job(&j, &mx, &sl, 0, NULL) == BK_DONE, "a first backup as a job");
	T(mx <= 1, "at most one fs request a slice (%u)", mx);
	printf("  a first backup of 161 files: %u slices, at most %u request a slice, stack %u bytes\n",
		sl, mx, bkjob_current()->stack_used);
	T(bkjob_current()->stack_used < sizeof stack / 2, "stack used %u of %u", bkjob_current()->stack_used, (unsigned)sizeof stack);
	T(bkjob_current()->nlines > 0 && strstr(bkjob_last_line(), "done"), "its last line: %s", bkjob_last_line());

	// the summary, the list, a restore, a check: each a job
	memset(&j, 0, sizeof j); j.kind = BKJOB_SUMMARY; j.o = opts();
	T(run_job(&j, &mx, &sl, 0, NULL) == BK_DONE && bkjob_current()->sum.backups == 1 && mx <= 1, "summary as a job");
	memset(&j, 0, sizeof j); j.kind = BKJOB_CHECK; j.o = opts(); j.all = true;
	T(run_job(&j, &mx, &sl, 0, NULL) == BK_DONE && mx <= 1, "check -a as a job (max %u)", mx);
	memset(&j, 0, sizeof j); j.kind = BKJOB_RESTORE; j.o = opts();
	snprintf(j.path, sizeof j.path, "/home/big.bin");
	snprintf(j.out, sizeof j.out, "/home/big2.bin");
	T(run_job(&j, &mx, &sl, 0, NULL) == BK_DONE && mx <= 1, "restore as a job (max %u)", mx);
	z_fs_info_t fi;
	T(zfs_stat("/home/big2.bin", &fi) == 0 && fi.size == 200000, "restored");
	zfs_unlink("/home/big2.bin");

	// a second backup, cancelled at each slice in turn
	for (int i = 0; i < 40; i += 3) {
		char p[64];
		snprintf(p, sizeof p, "/home/%sfile%02d.txt", i & 1 ? "pics/" : "", i);
		put(p, 100 + (uint32_t)i * 950, (unsigned)i + 100);
	}
	put("/home/big.bin", 200000, 98);
	uint8_t *card0 = malloc(CARD_SECTORS * 512);
	uint8_t *usb0 = malloc((size_t)USB_SECTORS * 512);
	memcpy(card0, card, CARD_SECTORS * 512);
	memcpy(usb0, usbd, (size_t)USB_SECTORS * 512);
	memset(&j, 0, sizeof j); j.kind = BKJOB_BACKUP; j.o = opts();
	// how many slices it takes, uncut (then the drive as it was)
	run_job(&j, &mx, &sl, 0, NULL);
	uint32_t total = sl, worst = 0;
	int runs = 0;
	for (uint32_t k = 1; k < total; k += (total > 400 ? total / 400 : 1)) {
		usb_present = usb_ready = false; usb_gen++; fs_test_usb_poll();
		memcpy(usbd, usb0, (size_t)USB_SECTORS * 512);
		usb_present = usb_ready = true; usb_gen++; fs_test_usb_poll();
		memset(&j, 0, sizeof j); j.kind = BKJOB_BACKUP; j.o = opts();
		run_job(&j, &mx, &sl, (int)k, &aft);
		if (aft > worst) worst = aft;
		T(mx <= 1, "cancel %u: a slice made %u requests", k, mx);
		T(!memcmp(card, card0, CARD_SECTORS * 512), "cancel %u: the card changed", k);
		T(zfs_stat("/usb/backup/zeitlos/LOCK.TXT", &fi) < 0, "cancel %u: the lock let go", k);
		// the next backup works, and check finds nothing wrong
		memset(&j, 0, sizeof j); j.kind = BKJOB_BACKUP; j.o = opts();
		int rc = run_job(&j, NULL, NULL, 0, NULL);
		T(rc == BK_DONE, "cancel %u: next backup %d", k, rc);
		memset(&j, 0, sizeof j); j.kind = BKJOB_CHECK; j.o = opts(); j.all = true;
		T(run_job(&j, NULL, NULL, 0, NULL) == BK_DONE && bkjob_current()->r.bad == 0, "cancel %u: check -a", k);
		runs++;
	}
	printf("  a backup of %u slices cancelled at %d of them: it let go within %u slices at the most\n",
		total, runs, worst);
	// The most is at the very start (the lock, the folder), before the
	// first look at the stop flag: ~20 requests, a few tenths of a second.
	T(worst <= 24, "let go within %u slices", worst);

	snprintf(c, sizeof c, "rm -rf '%s'", scratch);
	if (system(c)) {}
	free(card0); free(usb0);
	printf("test_job: %d checks, %d failed\n", tchecks, tfails);
	return tfails != 0;
}
