/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * backup on the real filesystem (docs/backup.md, "Testing"): bk.c and
 * bk_zfs.c -- the board's own file layer -- talking to sw/apps/fs/fs.c
 * and the project's FatFs, through zfsc.c, as on the board. The kernel
 * is sw/apps/fs/tests/test_fs.c's stubs (this file includes it, for its
 * message layer and disks): the card a 4 MB RAM disk formatted by fs,
 * the backup drive a 40 MB FAT32 image made by mkfs.fat, as a stick
 * from a shop comes.
 *
 * Then the power is cut at every sector write a backup makes, in turn:
 * the write it happens at lands in part (some whole sectors, perhaps
 * half of one), nothing after it does, the drive is unplugged and
 * plugged in again ("reboot"), and then:
 *
 *   - the card is byte for byte as it was;
 *   - every backup that was complete holds what the card had then;
 *   - `check -a` finds every copy right;
 *   - the next backup works, and holds what the card has;
 *   - fsck.fat's view of the drive is counted (a cut can leave FAT
 *     inconsistent -- lost clusters, a wrong free count -- which is why
 *     the rules exist; it must never cost a backup).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define USB_SECTORS (40u * 2048u)       // 40 MB: FAT32 with 512-byte clusters
#include <stdbool.h>
#include <stdint.h>
static bool usb_hook(uint32_t lba, uint32_t count, const void *buf);
#define USB_WRITE_HOOK usb_hook
#define main test_fs_main
#include "../../fs/tests/test_fs.c"
#undef main
#undef CHECK

#include "bk.h"
#include "bk_zfs.h"

static int tchecks, tfails;
static bool verbose;
#define T(c, ...) do { tchecks++; if (!(c)) { tfails++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// -- the power ---------------------------------------------------------------

static long usb_writes, cut_at;   // sector-write requests; cut at this one (0 never)
static bool dead;
static uint32_t seed = 1;

static uint32_t rnd(void) { seed = seed * 1103515245u + 12345u; return seed >> 8; }

static bool usb_hook(uint32_t lba, uint32_t count, const void *buf) {
	if (dead) return false;
	usb_writes++;
	if (cut_at && usb_writes == cut_at) {
		// some whole sectors of this request, then perhaps half of one
		uint32_t whole = rnd() % (count + 1);
		memcpy(usbd + lba * 512, buf, whole * 512);
		if (whole < count && rnd() % 2)
			memcpy(usbd + (lba + whole) * 512, (const uint8_t *)buf + whole * 512, 256);
		dead = true;
		return false;
	}
	return true;
}

// The machine comes back: what the dead program held is dropped, the
// drive is mounted afresh (FatFs reads it again; nothing cached).
static void reboot(void) {
	zfs_flush();
	for (int fd = 0; fd < 32; fd++) zfs_close(fd);  // fs lets go of them; writes fail
	dead = false;
	cut_at = 0;
	usb_present = false; usb_ready = false; usb_gen++;
	fs_test_usb_poll();
	usb_present = true; usb_ready = true; usb_gen++;
	fs_test_usb_poll();
}

// -- bk_io_t: bk_zfs.c's files, and a clock and an owner of our own --------

static struct { bool set; int Y, M, D, h, m, s; uint32_t up; } clk = { true, 2026, 10, 21, 11, 30, 0, 100 };

static void tick(int s) {
	clk.up += (uint32_t)s; clk.s += s;
	while (clk.s >= 60) { clk.s -= 60; clk.m++; }
	while (clk.m >= 60) { clk.m -= 60; clk.h++; }
	while (clk.h >= 24) { clk.h -= 24; clk.D++; }
}

static bool io_clock(void *c, bk_tm_t *t, uint32_t *up) {
	(void)c;
	t->year = clk.Y; t->month = clk.M; t->day = clk.D;
	t->hour = clk.h; t->min = clk.m; t->sec = clk.s;
	*up = clk.up;
	return clk.set;
}
static void io_sleep(void *c, int ms) { (void)c; (void)ms; tick(1); }
static void io_owner(void *c, char *b, int n) { (void)c; snprintf(b, (size_t)n, "test"); }
static bool io_alive(void *c, const char *w) { (void)c; (void)w; return false; }
static char said[8192];
static void io_out(void *c, const char *t) {
	(void)c;
	if (verbose) fputs(t, stdout);
	size_t n = strlen(said);
	snprintf(said + n, sizeof said - n, "%s", t);
	if (strlen(said) > sizeof said - 512) memmove(said, said + 4096, strlen(said + 4096) + 1);
}
static bool io_stop(void *c) { (void)c; return dead; }

static bk_io_t io;

static const char *const FROM[] = { "/home", "/data" };
static bk_opts_t opts(void) {
	bk_opts_t o;
	memset(&o, 0, sizeof o);
	o.dest = "/usb/backup"; o.name = "zeitlos";
	o.from = FROM; o.nfrom = 2;
	o.keep = 10; o.refresh = 10; o.verify = true;
	o.reserve_kb = 64;
	return o;
}

// -- the card, and what each backup should hold -----------------------------

static void put(const char *p, uint32_t len, unsigned s) {
	uint8_t *b = malloc(len + 1);
	for (uint32_t i = 0; i < len; i++) b[i] = (uint8_t)(s * 131u + i * 7u + (i >> 10));
	int32_t fd = zfs_open(p, Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC);
	T(fd >= 0, "create %s", p);
	if (fd >= 0) { T(zfs_write(fd, b, len) == (int32_t)len, "write %s", p); zfs_close(fd); }
	free(b);
}

typedef struct { char path[96]; uint8_t *data; uint32_t len; } cf_t;
typedef struct { char name[BK_NAME_MAX]; cf_t f[64]; int n; } truth_t;
static truth_t truth[16];
static int ntruth;

static uint8_t *read_file(const char *p, uint32_t *len) {
	z_fs_info_t fi;
	if (zfs_stat(p, &fi) < 0) return NULL;
	uint8_t *b = malloc(fi.size + 1);
	int32_t fd = zfs_open(p, Z_O_RDONLY);
	uint32_t got = 0;
	if (fd >= 0) {
		int32_t r;
		while (got < fi.size && (r = zfs_read(fd, b + got, fi.size - got)) > 0) got += (uint32_t)r;
		zfs_close(fd);
	}
	*len = got;
	return b;
}

static const char *const CARD[] = {
	"/home/book.txt", "/home/notes.txt", "/home/pics/cat.bmp", "/home/pics/dog.bmp",
	"/home/big.bin", "/data/sheet/budget.zss", "/data/slide/talk.zsl",
};
#define NCARD (int)(sizeof CARD / sizeof *CARD)

static void snap(const char *name) {
	truth_t *t = &truth[ntruth++];
	snprintf(t->name, sizeof t->name, "%s", name);
	t->n = 0;
	for (int i = 0; i < NCARD; i++) {
		uint32_t l;
		uint8_t *d = read_file(CARD[i], &l);
		if (!d) continue;
		cf_t *f = &t->f[t->n++];
		snprintf(f->path, sizeof f->path, "%s", CARD[i]);
		f->data = d; f->len = l;
	}
}

typedef struct { char path[96]; char where[BK_NAME_MAX]; bool present; } fl_t;
typedef struct { fl_t v[64]; int n; } fls_t;
static int fl_fn(void *a, const bk_file_t *f) {
	fls_t *l = a;
	if (l->n < 64) {
		snprintf(l->v[l->n].path, 96, "%s", f->path);
		snprintf(l->v[l->n].where, BK_NAME_MAX, "%s", f->where);
		l->v[l->n].present = f->present;
		l->n++;
	}
	return 0;
}

static bool verify(const char *ctx) {
	bool ok = true;
	bk_opts_t o = opts();
	for (int t = 0; t < ntruth; t++) {
		static fls_t l;
		l.n = 0;
		if (bk_files(&io, &o, truth[t].name, "/", fl_fn, &l) != BK_DONE) {
			printf("  %s: %s cannot be read\n", ctx, truth[t].name);
			ok = false;
			continue;
		}
		if (l.n != truth[t].n) { printf("  %s: %s lists %d of %d\n", ctx, truth[t].name, l.n, truth[t].n); ok = false; }
		for (int i = 0; i < l.n; i++) {
			cf_t *f = NULL;
			for (int k = 0; k < truth[t].n; k++) if (!strcmp(truth[t].f[k].path, l.v[i].path)) f = &truth[t].f[k];
			char cp[256];
			snprintf(cp, sizeof cp, "/usb/backup/zeitlos/%s%s", l.v[i].where, l.v[i].path);
			uint32_t len;
			uint8_t *d = read_file(cp, &len);
			if (!f || !d || len != f->len || memcmp(d, f->data, len)) {
				printf("  %s: %s: %s wrong\n", ctx, truth[t].name, l.v[i].path);
				ok = false;
			}
			free(d);
		}
	}
	return ok;
}

static bool card_matches_newest(const char *ctx) {
	bk_opts_t o = opts();
	static fls_t l;
	l.n = 0;
	bk_files(&io, &o, NULL, "/", fl_fn, &l);
	bool ok = l.n == NCARD;
	for (int i = 0; i < l.n && ok; i++) {
		uint32_t a, b;
		char cp[256];
		snprintf(cp, sizeof cp, "/usb/backup/zeitlos/%s%s", l.v[i].where, l.v[i].path);
		uint8_t *x = read_file(l.v[i].path, &a), *y = read_file(cp, &b);
		ok = x && y && a == b && !memcmp(x, y, a);
		free(x); free(y);
	}
	if (!ok) printf("  %s: the newest backup is not the card\n", ctx);
	return ok;
}

static char kinds[16][64];
static int kind_n[16], nkinds;

// fsck.fat on the drive as it is: 0 clean, else what fsck said.
static char scratch[512];
static int fsck_drive(void) {
	char img[600], cmd[1400];
	snprintf(img, sizeof img, "%s/usb.img", scratch);
	FILE *f = fopen(img, "wb");
	if (!f) return -1;
	fwrite(usbd, 512, USB_SECTORS, f);
	fclose(f);
	snprintf(cmd, sizeof cmd, "fsck.fat -n '%s' >'%s/fsck.txt' 2>&1", img, scratch);
	int r = system(cmd);
	if (r) {
		// what kind of damage: each message once, numbers left out
		char p[600], line[256];
		snprintf(p, sizeof p, "%s/fsck.txt", scratch);
		FILE *t = fopen(p, "r");
		while (t && fgets(line, sizeof line, t)) {
			if (strstr(line, " files, ")) continue;
			if (strncmp(line, "  ", 2) && !strstr(line, "cluster") && !strstr(line, "ount") &&
				!strstr(line, "Orphan") && !strstr(line, "wrong")) continue;
			char k[64];
			int n = 0;
			for (char *c = line; *c && *c != '\n' && n < 60; c++)
				if (!(*c >= '0' && *c <= '9')) k[n++] = *c;
			k[n] = 0;
			int i;
			for (i = 0; i < nkinds && strcmp(kinds[i], k); i++) {}
			if (i == nkinds && nkinds < 16) snprintf(kinds[nkinds++], 64, "%s", k);
			if (i < 16) kind_n[i]++;
		}
		if (t) fclose(t);
	}
	return r == 0 ? 0 : 1;
}

static void make_drive(void) {
	char img[600], cmd[800];
	snprintf(img, sizeof img, "%s/fresh.img", scratch);
	unlink(img);
	snprintf(cmd, sizeof cmd, "mkfs.fat -F 32 -s 1 -n BACKUP -C '%s' %u >/dev/null", img, USB_SECTORS / 2);
	if (system(cmd)) { printf("test_fat: mkfs.fat failed (dosfstools?)\n"); exit(2); }
	FILE *f = fopen(img, "rb");
	usbd = calloc(USB_SECTORS, 512);
	if (!f || fread(usbd, 512, USB_SECTORS, f) != USB_SECTORS) { printf("test_fat: cannot read the image\n"); exit(2); }
	fclose(f);
	usb_present = true; usb_ready = true; usb_gen++;
	fs_test_usb_poll();
}

// -- the tests ---------------------------------------------------------------

static void test_real(void) {
	z_fs_info_t fi;
	T(zfs_stat("/usb", &fi) == 0, "the drive is mounted");
	z_fs_vfs_t v;
	T(zfs_statvfs("/usb", &v) == 0 && v.total_kb > 30000, "FAT32, %u KB", (unsigned)v.total_kb);

	put("/home/book.txt", 1800, 1);
	put("/home/notes.txt", 11, 2);
	T(zfs_mkdir("/home/pics") == 0, "mkdir");
	put("/home/pics/cat.bmp", 20000, 3);
	put("/home/pics/dog.bmp", 9000, 4);
	put("/home/big.bin", 300000, 5);
	T(zfs_mkdir("/data/sheet") == 0 && zfs_mkdir("/data/slide") == 0, "mkdir data");
	put("/data/sheet/budget.zss", 700, 6);
	put("/data/slide/talk.zsl", 5000, 7);

	bk_opts_t o = opts();
	bk_result_t r;
	T(bk_backup(&io, &o, &r) == BK_DONE && r.copied == NCARD, "first backup: %u", r.copied);
	snap(r.name);
	T(zfs_stat("/usb/backup/README.TXT", &fi) == 0, "README");
	T(fsck_drive() == 0, "fsck clean after a backup");

	tick(3600);
	put("/home/book.txt", 2100, 8);
	put("/home/big.bin", 300000, 9);
	T(bk_backup(&io, &o, &r) == BK_DONE && r.copied == 2, "second: %u", r.copied);
	snap(r.name);
	T(verify("real"), "verify");
	T(card_matches_newest("real"), "newest");

	// restore through the real fs: beside, then replace
	put("/home/book.txt", 50, 10);
	T(bk_restore(&io, &o, truth[0].name, "/home/book.txt", NULL, false, &r) == BK_DONE, "restore beside");
	char beside[96];
	snprintf(beside, sizeof beside, "/home/book (%.10s %.5s).txt", truth[0].name, truth[0].name + 11);
	T(zfs_stat(beside, &fi) == 0 && fi.size == 1800, "beside: %s", beside);
	zfs_unlink(beside);
	T(bk_restore(&io, &o, NULL, "/home/book.txt", NULL, true, &r) == BK_DONE, "restore replace");
	T(zfs_stat("/home/book.txt", &fi) == 0 && fi.size == 2100, "replaced");
	T(zfs_stat("/home/book.txt.part", &fi) < 0, "no .part");
	T(fsck_drive() == 0, "fsck clean");
}

// A backup cut at each of its sector writes.
static void test_cuts(void) {
	uint8_t *card0 = malloc(CARD_SECTORS * 512), *usb0 = malloc((size_t)USB_SECTORS * 512);
	tick(3600);
	put("/home/book.txt", 2400, 11);
	put("/home/pics/dog.bmp", 9500, 12);
	put("/home/new.txt", 333, 13);
	zfs_unlink("/home/new.txt");   // (put and gone: nothing to back up of it)
	put("/data/slide/talk.zsl", 5200, 14);
	memcpy(card0, card, CARD_SECTORS * 512);
	memcpy(usb0, usbd, (size_t)USB_SECTORS * 512);

	// how many writes an uncut backup makes
	bk_opts_t o = opts();
	bk_result_t r;
	usb_writes = 0;
	T(bk_backup(&io, &o, &r) == BK_DONE, "uncut");
	long n = usb_writes;
	printf("  a backup on FAT32 makes %ld write requests to the drive\n", n);
	int unclean = 0, runs = 0;
	for (long k = 1; k <= n; k++) {
		// back to before it
		usb_present = false; usb_ready = false; usb_gen++;
		fs_test_usb_poll();
		memcpy(usbd, usb0, (size_t)USB_SECTORS * 512);
		usb_present = true; usb_ready = true; usb_gen++;
		fs_test_usb_poll();
		seed = (uint32_t)k * 2654435761u;
		usb_writes = 0;
		cut_at = k;
		bk_backup(&io, &o, &r);
		bool cut = dead;
		reboot();
		if (!cut) continue;
		runs++;
		char ctx[48];
		snprintf(ctx, sizeof ctx, "cut %ld", k);
		T(!memcmp(card, card0, CARD_SECTORS * 512), "%s: the card changed", ctx);
		if (fsck_drive()) unclean++;
		T(verify(ctx), "%s: earlier backups", ctx);
		bk_result_t cr;
		bk_check(&io, &o, true, &cr);
		T(cr.bad == 0, "%s: check -a: %u", ctx, cr.bad);
		tick(2);
		int rc = bk_backup(&io, &o, &r);
		T(rc == BK_DONE, "%s: next backup rc %d", ctx, rc);
		T(card_matches_newest(ctx), "%s: next backup", ctx);
		T(verify(ctx), "%s: earlier backups after it", ctx);
	}
	printf("  cut at each of %d writes; fsck.fat found the drive inconsistent after %d of them\n", runs, unclean);
	for (int i = 0; i < nkinds; i++) printf("    %3d x %s\n", kind_n[i], kinds[i]);
	free(card0); free(usb0);
}

// The drive nearly full: refused before anything is written. Then its
// FSINFO free count claims more than there is (as after a power cut,
// test_cuts: "Free cluster summary wrong"), so the drive fills in the
// middle of a copy: that backup fails, said plainly, and every other
// one is untouched.
static void test_full(void) {
	bk_opts_t o = opts();
	bk_result_t r;
	z_fs_vfs_t v;
	// a backup that has something to copy
	tick(60);
	put("/home/big.bin", 300000, 40);
	T(zfs_statvfs("/usb", &v) == 0, "statvfs");
	// fill the drive to within ~150 KB (in files of at most 4 MB)
	uint32_t want = v.free_kb > 150 ? v.free_kb - 150 : 0;
	static uint8_t blk[64 * 1024];
	int nfill = 0;
	while (want > 0) {
		char p[64];
		snprintf(p, sizeof p, "/usb/fill%02d.bin", nfill++);
		int32_t fd = zfs_open(p, Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC);
		uint32_t in = 0;
		while (want > 0 && in < 4096) {
			uint32_t k = want > 64 ? 64 : want;
			zfs_write(fd, blk, k * 1024);
			want -= k; in += k;
		}
		zfs_close(fd);
	}
	T(zfs_statvfs("/usb", &v) == 0 && v.free_kb < 400, "nearly full: %u KB free", (unsigned)v.free_kb);
	said[0] = 0;
	T(bk_backup(&io, &o, &r) == BK_FAILED && strstr(said, "not enough room"), "refused: not enough room\n%s", said);
	z_fs_info_t fi;
	T(zfs_stat("/usb/backup/zeitlos/LOCK.TXT", &fi) < 0 && !r.name[0], "nothing written");
	T(verify("full"), "untouched");

	// the free count lies
	usb_present = false; usb_ready = false; usb_gen++;
	fs_test_usb_poll();
	// FAT32: the FSINFO sector is the boot sector's word at 48 (usually 1);
	// its free count at 488
	uint32_t fsinfo = (uint32_t)usbd[48] | (uint32_t)usbd[49] << 8;
	uint8_t *fc = usbd + fsinfo * 512 + 488;
	// (FatFs believes a count up to the number of clusters: 3/4 of them)
	uint32_t lie = v.total_kb * 2 / 4 * 3;
	fc[0] = (uint8_t)lie; fc[1] = (uint8_t)(lie >> 8); fc[2] = (uint8_t)(lie >> 16); fc[3] = (uint8_t)(lie >> 24);
	usb_present = true; usb_ready = true; usb_gen++;
	fs_test_usb_poll();
	T(zfs_statvfs("/usb", &v) == 0 && v.free_kb > 10000, "the drive claims %u KB free", (unsigned)v.free_kb);
	said[0] = 0;
	int rc = bk_backup(&io, &o, &r);
	T(rc == BK_FAILED && strstr(said, "the drive is full"), "full mid-copy: rc %d\n%s", rc, said);
	T(verify("full mid-copy"), "untouched after it");
	// room again: the next backup removes what that one left, and works
	for (int i = 0; i < nfill; i++) { char p[64]; snprintf(p, sizeof p, "/usb/fill%02d.bin", i); zfs_unlink(p); }
	tick(60);
	T(bk_backup(&io, &o, &r) == BK_DONE, "after making space");
	T(card_matches_newest("full"), "newest");
	T(verify("full after"), "all");
}

int main(int argc, char **argv) {
	for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-v")) verbose = true;
	const char *tmp = getenv("TMPDIR");
	snprintf(scratch, sizeof scratch, "%s/test_fat.XXXXXX", tmp ? tmp : "/tmp");
	if (!mkdtemp(scratch)) { perror("mkdtemp"); return 2; }

	card = calloc(CARD_SECTORS, 512);
	ram = calloc(RAM_SECTORS, 512);
	for (int i = 0; i < 64; i++) alive[i] = true;
	fs_test_start(true);
	zfs_mkfs("/");
	make_drive();

	memset(&io, 0, sizeof io);
	bk_zfs_io(&io);
	io.clock = io_clock; io.sleep_ms = io_sleep;
	io.owner = io_owner; io.owner_alive = io_alive;
	io.out = io_out; io.stop = io_stop;

	test_real();
	test_cuts();
	test_full();

	char cmd[600];
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", scratch);
	if (system(cmd)) {}
	printf("test_fat: %d checks, %d failed\n", tchecks, tfails);
	return tfails != 0;
}
