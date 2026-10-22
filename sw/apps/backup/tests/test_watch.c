/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Watch on the host (docs/backup.md, "Watch"): bkwatch.c over the real
 * fs driver's watches (sw/apps/fs/fs.c, zfsproto.h "Watching") and
 * FatFs, through zfsc.c, with test_fs.c's kernel stubs -- and the watch
 * backups bk.c makes of what it hands over. Another process saving is
 * another client pid; the clock is the stubs' tick count.
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
#include "bkwatch.h"

static int tchecks, tfails;
#define T(c, ...) do { tchecks++; if (!(c)) { tfails++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define ME      12      // the backup window
#define WRITER  5       // text, say
#define SEC     Z_TICK_HZ

static char said[16384];
static void out(void *c, const char *t) {
	(void)c;
	size_t k = strlen(said);
	snprintf(said + k, sizeof said - k, "%s", t);
}
static bool no_clock(void *c, bk_tm_t *t, uint32_t *up) {
	(void)c; memset(t, 0, sizeof *t); *up = ticks / SEC; return false;
}

static void put_as(uint32_t who, const char *path, const char *text) {
	uint32_t was = client;
	client = who;
	int32_t fd = zfs_open(path, Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC);
	if (fd >= 0) { zfs_write(fd, text, (uint32_t)strlen(text)); zfs_close(fd); }
	client = was;
}

// What fs sent since the last call, as the window's mailbox has it.
static uint32_t notices_seen;
static void deliver(bkw_t *w) {
	while (notices_seen < changed_n) {
		notices_seen++;
		z_msg_t m;
		memset(&m, 0, sizeof m);
		m.subject = Z_FS_CHANGED;
		m.tag = changed_tag;
		m.from = FS_PID;
		T(changed_to == ME, "a notice for the watcher only (to %u)", (unsigned)changed_to);
		T(bkw_msg(w, &m), "the notice is the watch's");
	}
	client = ME;
	bkw_poll(w, ticks);
}

static bk_io_t io;
static bk_opts_t o;
static const char *from[] = { "/home", "/sys/zeitlos.cfg" };

// The watch backup the window would start now: its rc.
static int watch_backup(bkw_t *w, int *copied) {
	const char *only[BKW_PENDING];
	int n = bkw_take(w, only, BKW_PENDING);
	bk_opts_t q = o;
	q.only = only; q.nonly = n;
	bk_result_t r;
	said[0] = 0;
	client = ME;
	int rc = bk_backup(&io, &q, &r);
	bkw_done(w);
	if (copied) *copied = (int)r.copied;
	return rc;
}

typedef struct { int n, watch; char newest[BK_NAME_MAX]; } ls_t;
static int ls_fn(void *a, const bk_info_t *b) {
	ls_t *l = a;
	if (!b->complete) return 0;
	l->n++;
	l->watch += b->watch;
	snprintf(l->newest, sizeof l->newest, "%s", b->name);
	return 0;
}
static ls_t ls(void) {
	ls_t l;
	memset(&l, 0, sizeof l);
	client = ME;
	bk_list(&io, &o, ls_fn, &l);
	return l;
}

int main(int argc, char **argv) {
	bool verbose = argc > 1 && !strcmp(argv[1], "-v");
	char scratch[512], c[1200];
	const char *tmp = getenv("TMPDIR");
	snprintf(scratch, sizeof scratch, "%s/test_watch.XXXXXX", tmp ? tmp : "/tmp");
	if (!mkdtemp(scratch)) return 2;
	card = calloc(CARD_SECTORS, 512);
	ram = calloc(RAM_SECTORS, 512);
	for (int i = 0; i < 64; i++) alive[i] = true;
	fs_test_start(true);
	zfs_mkfs("/");
	snprintf(c, sizeof c, "mkfs.fat -F 32 -s 1 -C '%s/u.img' %u >/dev/null", scratch, USB_SECTORS / 2);
	if (system(c)) { printf("test_watch: mkfs.fat failed\n"); return 2; }
	snprintf(c, sizeof c, "%s/u.img", scratch);
	FILE *f = fopen(c, "rb");
	usbd = calloc(USB_SECTORS, 512);
	if (!f || fread(usbd, 512, USB_SECTORS, f) != USB_SECTORS) return 2;
	fclose(f);
	usb_present = usb_ready = true; usb_gen++;
	fs_test_usb_poll();

	bk_zfs_io(&io);
	io.clock = no_clock;
	io.out = out;
	memset(&o, 0, sizeof o);
	o.dest = "/usb/backup"; o.name = "zeitlos";
	o.from = from; o.nfrom = 2;
	o.keep = 10; o.refresh = 10; o.verify = true; o.reserve_kb = 64;

	client = WRITER;
	zfs_mkdir("/home");
	zfs_mkdir("/home/book");
	put_as(WRITER, "/home/book/ch1.txt", "It was a dark night.\n");
	put_as(WRITER, "/home/notes.txt", "milk\n");
	client = ME;
	said[0] = 0;
	T(bk_backup(&io, &o, &(bk_result_t){0}) == BK_DONE, "the first full backup:\n%s", said);

	// -- what can be watched ------------------------------------------------
	bkw_t w;
	T(!bkw_start(&w, &o, "") && bkw_state(&w, true) == BKW_OFF, "empty: off");
	T(!bkw_start(&w, &o, "none") && bkw_state(&w, true) == BKW_OFF, "none: off");
	T(!bkw_start(&w, &o, "/etc /usb/backup/x book") && bkw_state(&w, true) == BKW_NONE, "nothing watchable");
	T(strstr(bkw_why(&w), "/etc is not in From") && strstr(bkw_why(&w), "/usb/backup/x is the backup drive's") &&
		strstr(bkw_why(&w), "book is not a path"), "why: %s", bkw_why(&w));
	client = ME;
	T(bkw_start(&w, &o, "/home/book/ \"/home/notes.txt\" /nope"), "watching");
	T(w.n == 2 && !strcmp(w.what[0], "/home/book") && !strcmp(w.what[1], "/home/notes.txt"), "the two");
	T(strstr(bkw_why(&w), "/nope is not in From") != NULL, "why: %s", bkw_why(&w));
	T(bkw_state(&w, true) == BKW_WATCHING && bkw_state(&w, false) == BKW_NO_DRIVE, "state");

	// -- a save: backed up 10 s after, alone ---------------------------------
	notices_seen = changed_n;
	ticks += SEC;
	put_as(WRITER, "/home/book/ch1.txt", "It was a dark and stormy night.\n");
	deliver(&w);
	T(w.npend == 1 && !strcmp(w.pend[0], "/home/book/ch1.txt"), "pending: %d", w.npend);
	T(!strcmp(w.last_saved, "/home/book/ch1.txt"), "last saved");
	T(bkw_state(&w, true) == BKW_WAITING, "waiting");
	ticks += 9 * SEC; deliver(&w);
	T(!bkw_due(&w, ticks), "not after 9 s");
	ticks += 1 * SEC; deliver(&w);
	T(bkw_due(&w, ticks), "after 10 s");
	int copied = 0;
	T(watch_backup(&w, &copied) == BK_DONE && copied == 1, "the watch backup:\n%s", said);
	ls_t l = ls();
	T(l.n == 2 && l.watch == 1 && l.newest[strlen(l.newest) - 1] == 'w', "a w backup: %s", l.newest);
	T(bkw_state(&w, true) == BKW_WATCHING, "watching again");
	T(!bkw_due(&w, ticks + 60 * SEC), "nothing more due");

	// its own writes (a restore into the folder) are not heard
	uint32_t before = changed_n;
	put_as(ME, "/home/book/restored.txt", "x");
	T(changed_n == before, "own write not noticed");

	// a save that keeps typing: each change restarts the 10 s
	put_as(WRITER, "/home/book/ch1.txt", "v3"); deliver(&w);
	ticks += 6 * SEC;
	put_as(WRITER, "/home/book/ch2.txt", "v1"); deliver(&w);
	ticks += 6 * SEC; deliver(&w);
	T(!bkw_due(&w, ticks) && w.npend == 2, "6 s after the second save");
	ticks += 4 * SEC; deliver(&w);
	T(bkw_due(&w, ticks), "10 s after it");
	T(watch_backup(&w, &copied) == BK_DONE && copied == 2, "both:\n%s", said);

	// a file still open for writing: wait for its close (at most 60 s)
	client = WRITER;
	int32_t fd = zfs_open("/home/book/long.txt", Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC);
	zfs_write(fd, "part", 4);
	deliver(&w);
	T(w.writers == 1, "a writer: %u", (unsigned)w.writers);
	ticks += 20 * SEC; deliver(&w);
	T(!bkw_due(&w, ticks), "not while it is open");
	client = WRITER;
	zfs_write(fd, " two", 4);
	zfs_close(fd);
	deliver(&w);
	T(w.writers == 0, "closed");
	T(!bkw_due(&w, ticks), "the close restarts the quiet");
	ticks += 10 * SEC; deliver(&w);
	T(bkw_due(&w, ticks), "then due");
	T(watch_backup(&w, &copied) == BK_DONE && copied == 1, "the whole file:\n%s", said);
	// ... and one left open for good is taken after 60 s
	client = WRITER;
	fd = zfs_open("/home/book/log.txt", Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC);
	zfs_write(fd, "line\n", 5);
	zfs_fsync(fd);
	deliver(&w);
	ticks += 59 * SEC; deliver(&w);
	T(!bkw_due(&w, ticks), "59 s");
	ticks += 1 * SEC; deliver(&w);
	T(bkw_due(&w, ticks), "60 s");
	T(watch_backup(&w, &copied) == BK_DONE, "a log:\n%s", said);
	client = WRITER;
	zfs_close(fd);
	deliver(&w);
	ticks += 10 * SEC; deliver(&w);
	bkw_drop(&w);

	// a rename-save (new copy, renamed over): the new name is what counts
	put_as(WRITER, "/home/book/ch1.new", "v4");
	client = WRITER;
	zfs_unlink("/home/book/ch1.txt");
	zfs_rename("/home/book/ch1.new", "/home/book/ch1.txt");
	deliver(&w);
	ticks += 10 * SEC; deliver(&w);
	T(bkw_due(&w, ticks), "due");
	T(watch_backup(&w, &copied) == BK_DONE && copied == 1, "ch1 only (ch1.new is gone):\n%s", said);

	// the single file watched
	put_as(WRITER, "/home/notes.txt", "milk, eggs\n");
	deliver(&w);
	ticks += 10 * SEC; deliver(&w);
	T(watch_backup(&w, &copied) == BK_DONE && copied == 1, "the file:\n%s", said);
	put_as(WRITER, "/home/other.txt", "not watched\n");
	deliver(&w);
	T(w.npend == 0, "beside it: not watched");

	// many at once: fs drops paths past 4, the folder is taken whole
	for (int i = 0; i < 9; i++) {
		char p[40];
		snprintf(p, sizeof p, "/home/book/pic%d.txt", i);
		put_as(WRITER, p, "pic");
	}
	deliver(&w);
	T(w.npend == 1 && !strcmp(w.pend[0], "/home/book"), "the folder: %d %s", w.npend, w.npend ? w.pend[0] : "");
	ticks += 10 * SEC; deliver(&w);
	// the nine, and the file restored there before (its own write: not
	// heard, but new in the folder); nothing that was already backed up
	T(watch_backup(&w, &copied) == BK_DONE && copied == 10, "the new ones only:\n%s", said);

	// more than BKW_PENDING heard one by one: the folders
	for (int i = 0; i < BKW_PENDING + 3; i++) {
		char p[40];
		snprintf(p, sizeof p, "/home/book/many%02d.txt", i);
		put_as(WRITER, p, "m");
		deliver(&w);
	}
	T(w.npend == 2, "the two watched, whole: %d", w.npend);
	ticks += 10 * SEC; deliver(&w);
	T(watch_backup(&w, &copied) == BK_DONE && copied == BKW_PENDING + 3, "all of them:\n%s", said);

	// a lost notice: asked anyway within 30 s
	put_as(WRITER, "/home/book/ch2.txt", "v2");
	notices_seen = changed_n;           // never delivered
	client = ME;
	bkw_poll(&w, ticks + 5 * SEC);
	T(w.npend == 0, "not asked yet");
	bkw_poll(&w, ticks + 31 * SEC);
	T(w.npend == 1, "asked: %d", w.npend);
	// ... and the notice after it comes, now that it is read again
	put_as(WRITER, "/home/book/ch3.txt", "v1");
	T(changed_n == notices_seen + 1, "re-armed");
	deliver(&w);
	T(w.npend == 2, "%d", w.npend);

	// the drive goes: saves are let go, not kept
	bkw_drop(&w);
	T(w.npend == 0 && w.lost == 1, "dropped");

	// nothing pending is a backup "already backed up"
	put_as(WRITER, "/home/book/gone.txt", "g");
	client = WRITER; zfs_unlink("/home/book/gone.txt");
	deliver(&w);
	ticks += 10 * SEC; deliver(&w);
	T(watch_backup(&w, &copied) == BK_DONE && copied == 0, "a file saved and deleted:\n%s", said);

	// restore what a watch backup had
	client = ME;
	said[0] = 0;
	T(bk_restore(&io, &o, NULL, "/home/book/ch1.txt", "/home/back.txt", false, &(bk_result_t){0}) == BK_DONE,
		"restore:\n%s", said);
	z_fs_info_t fi;
	T(zfs_stat("/home/back.txt", &fi) == 0 && fi.size == 2, "the newest ch1 (v4)");

	// stop lets fs's watches go: four starts in a row
	for (int i = 0; i < 6; i++) {
		bkw_stop(&w);
		client = ME;
		T(bkw_start(&w, &o, "/home/book /home/notes.txt"), "again %d: %s", i, bkw_why(&w));
	}
	// another program holding fs's watches: said
	bkw_t w2, w3;
	client = 13;
	T(bkw_start(&w2, &o, "/home /home/book"), "two more");
	T(!bkw_start(&w3, &o, "/home"), "a fifth");
	T(strstr(bkw_why(&w3), "watches are in use") != NULL, "why: %s", bkw_why(&w3));
	client = 13; bkw_stop(&w2);
	client = ME; bkw_stop(&w);

	if (verbose) printf("%s", said);
	snprintf(c, sizeof c, "rm -rf '%s'", scratch);
	if (system(c)) {}
	printf("test_watch: %d checks, %d failed\n", tchecks, tfails);
	return tfails != 0;
}
