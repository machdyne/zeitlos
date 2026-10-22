/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * backup's window drawn on the host, state by state, as pictures to look
 * at (sw/common/tests/zrender.h): the REAL bkwin.c, with its jobs, the
 * drive and the settings replaced by the state each picture shows.
 * And its keys, buttons and the Aa icon driven: what pressing them does.
 *
 *   sudo sysctl -w vm.mmap_min_addr=0       # sw/common/tests/ztramp.h
 *   make -C sw/apps/backup/tests render     # pictures in tests/render/
 *
 * Exit 0 pass, 1 fail, 77 skipped (no page 0, or not x86-64 Linux).
 */

#include "../../../common/tests/zrender.h"
#include "../../../common/tests/ztramp.h"

#include <stdlib.h>
#include <sys/stat.h>

#include "bk.h"
#include "bkjob.h"
#include "zcfg.h"
#include "zdialog.h"
#include "zfsc.h"

static int checks, fails;
#define T(c, ...) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// -- what bkwin.c reaches, made up ------------------------------------------

static bkjob_t fake;
static bool fake_running, fake_cancel;
static int started[16], nstarted;

void bkjob_init(const bk_io_t *b, void *s, size_t n) { (void)b; (void)s; (void)n; }
bool bkjob_start(const bkjob_t *j) {
	if (fake_running) return false;
	fake = *j;
	if (nstarted < 16) started[nstarted++] = j->kind;
	return true;            // started; nothing runs here
}
bool bkjob_step(void) { return false; }
bool bkjob_running(void) { return fake_running; }
void bkjob_cancel(void) { if (fake_running) fake_cancel = true; }
bool bkjob_cancelling(void) { return fake_running && fake_cancel; }
bkjob_t *bkjob_current(void) { return &fake; }
const char *bkjob_last_line(void) { return fake.nlines ? fake.lines[(fake.nlines - 1) % BKJOB_LINES] : ""; }

static bool usb_there = true;
int32_t zfs_stat(const char *p, z_fs_info_t *i) { (void)p; memset(i, 0, sizeof *i); i->type = Z_FS_TYPE_DIR; return usb_there ? 0 : -Z_ENOENT; }
int32_t zfs_umount(const char *p) { (void)p; usb_there = false; return 0; }
int32_t zfs_open(const char *p, uint32_t f) { (void)p; (void)f; return -Z_ENOENT; }
int32_t zfs_read(int32_t fd, void *b, uint32_t n) { (void)fd; (void)b; (void)n; return -Z_EBADF; }
int32_t zfs_write(int32_t fd, const void *b, uint32_t n) { (void)fd; (void)b; (void)n; return -Z_EBADF; }
int32_t zfs_close(int32_t fd) { (void)fd; return 0; }
int32_t zfs_fsync(int32_t fd) { (void)fd; return 0; }
int32_t zfs_unlink(const char *p) { (void)p; return 0; }
int32_t zfs_rename(const char *a, const char *b) { (void)a; (void)b; return 0; }

static const char *watch_cfg = "";
bool z_cfg_get(const char *k, char *o, size_t n) {
	if (!strcmp(k, "apps.backup.watch") && watch_cfg[0]) { snprintf(o, n, "%s", watch_cfg); return true; }
	if (n) o[0] = 0;
	return false;
}
// fs's watches (zfsproto.h): one free
static int watches_made;
int32_t zfs_watch(const char *p) { (void)p; return watches_made++ < 4 ? watches_made - 1 : -Z_EMFILE; }
int32_t zfs_unwatch(int32_t wd) { (void)wd; watches_made--; return 0; }
int32_t zfs_watch_read(int32_t wd, z_fs_watch_t *h, char *p, uint32_t cap) {
	(void)wd; (void)p; (void)cap; memset(h, 0, sizeof *h); return 0;
}
bool z_cfg_get_bool(const char *k, bool d) { (void)k; return d; }
int32_t z_cfg_get_int(const char *k, int32_t d) { (void)k; return d; }
int z_cfg_text_set(const char *in, size_t inlen, const char *key, const char *val, char *out, size_t cap) {
	(void)in; (void)inlen; (void)key; (void)val; (void)out; (void)cap; return -1;
}
int z_cfg_reload(uint32_t *ig) { (void)ig; return 0; }

static int dialogs;
void z_dialog_help(const z_dialog_ctx_t *c, const char *t, const char *x, const char *d) { (void)c; (void)t; (void)x; (void)d; dialogs++; }
int z_dialog_confirm(const z_dialog_ctx_t *c, const char *t, const char *m, int b) { (void)c; (void)t; (void)m; (void)b; dialogs++; return Z_DIALOG_YES; }
bool z_dialog_prompt(const z_dialog_ctx_t *c, const char *t, const char *m, const char *i, char *o, int n) { (void)c; (void)t; (void)m; (void)i; (void)o; (void)n; dialogs++; return false; }
bool z_dialog_prompt_empty_ok(const z_dialog_ctx_t *c, const char *t, const char *m, const char *i, char *o, int n) { (void)c; (void)t; (void)m; (void)i; (void)o; (void)n; dialogs++; return false; }
const char *z_ftype_app_for(const char *p) { (void)p; return "text"; }

static const char *const FROM[] = { "/home", "/data", "/sys/zeitlos.cfg", "/sys/net.cfg" };
static const char *const SKIP[] = { "/data/ask", "*.tmp" };
void backup_settings(bk_opts_t *o) {
	memset(o, 0, sizeof *o);
	o->dest = "/usb/backup"; o->name = "zeitlos";
	o->from = FROM; o->nfrom = 4; o->skip = SKIP; o->nskip = 2;
	o->keep = 10; o->verify = true;
}
static bool fclock(void *c, bk_tm_t *t, uint32_t *up) {
	(void)c; *up = 1000;
	t->year = 2026; t->month = 10; t->day = 21; t->hour = 14; t->min = 40; t->sec = 0;
	return true;
}
void backup_base_io(bk_io_t *io) { memset(io, 0, sizeof *io); io->clock = fclock; }

#include "../bkwin.c"

// -- pictures ---------------------------------------------------------------

static char dir[512];

static void shot(const char *name) {
	char p[700];
	snprintf(p, sizeof p, "%s/%s.pbm", dir, name);
	z_render_clear();
	z_widget_invalidate(cur_set());
	z_win_clear(&win);
	repaint();
	z_render_write(p, &win, 2);
}

static void set_size(int content_w, int content_h) {
	win.w = content_w + 4;
	win.h = content_h + Z_WM_TITLEBAR_H + 4;
	win.clip[0].x1 = win.x + win.w - 1;
	win.clip[0].y1 = win.y + win.h - 1;
	layout();
}

static void key(uint32_t k) { handle_key(k, 0); }

static void aa(void) {             // the titlebar's Aa icon
	z_msg_t m;
	memset(&m, 0, sizeof m);
	m.subject = Z_WM_TITLEBAR_ICON;
	m.obj.type = Z_UINT32;
	m.obj.val.uint32 = (uint32_t)win.id | (Z_WM_TBICON_FONT << 8);
	handle(&m);
}

static bool started_kind(int k) {
	for (int i = 0; i < nstarted; i++) if (started[i] == k) return true;
	return false;
}

static void running(int phase, uint32_t done, uint32_t total) {
	fake_running = true; fake_cancel = false;
	fake.kind = BKJOB_BACKUP; fake.prog.phase = phase;
	fake.prog.bytes_done = done; fake.prog.bytes_total = total;
	fake.prog.files_done = 11; fake.prog.files_total = 40;
	snprintf(fake.path_now, sizeof fake.path_now, "/home/My Book/chapter 12.txt");
}

static void said(const char *line) {
	fake.nlines = 1;
	snprintf(fake.lines[0], BKJOB_LINE_MAX, "%s", line);
}

int main(int argc, char **argv) {
	snprintf(dir, sizeof dir, "%s", argc > 1 ? argv[1] : "render");
	mkdir(dir, 0755);
	if (!z_render_open(&win, MAIN_W + 4, MAIN_H + Z_WM_TITLEBAR_H + 4)) {
		printf("render: cannot map VRAM -- skipped\n");
		return 77;
	}
	if (!z_tramp_install()) return 77;
	backup_settings(&opts);
	backup_base_io(&base);
	widgets_init();
	layout();
	T(cw == MAIN_W && ch == MAIN_H, "content %dx%d", cw, ch);

	// 1. backed up
	drive_there = true; have_sum = true;
	memset(&sum, 0, sizeof sum);
	sum.backups = 14; sum.free_kb = 12900000;
	snprintf(sum.newest, sizeof sum.newest, "2026-10-21_11-30-48");
	snprintf(sum.oldest, sizeof sum.oldest, "2026-10-12_18-02-11");
	sum.newest_files = 412;
	shot("1-backed-up");
	T(!strcmp(title_now, "Backup: today 11:30"), "title: %s", title_now);
	T(bset.count == 4 && bid[0] == B_BACKUP && bid[1] == B_SHOW && bid[2] == B_SETTINGS && bid[3] == B_REMOVE,
		"four buttons: Back up now, Show backups, Settings, Remove drive");
	T(bset.focused == 0, "Back up now focused");

	// 2. Enter: a backup; while it runs, only Stop
	nstarted = 0;
	key(0x0d);
	T(started_kind(BKJOB_BACKUP) && fake.o.nonly == 0, "Enter: a full backup");
	running(BK_PH_COPY, 450000, 1000000);
	shot("2-running");
	T(!strcmp(title_now, "Backup 45%"), "title: %s", title_now);
	T(bset.count == 1 && bid[0] == B_BACKUP && !strcmp(bw[0].label, "Stop"), "while running: only Stop");
	key(0x1b);
	T(fake_cancel, "Esc stops");
	shot("3-stopping");
	T(!strcmp(title_now, "Backup: Stopping..."), "title: %s", title_now);
	// it ends: the drive is read again
	fake_running = false; fake.rc = BK_DONE;
	said("backup: done: 2026-10-21_14-40-00, 412 files, 3 copied (812 KB)");
	job_done();
	T(pending_sum && last_rc == BK_DONE, "done: the drive read again");
	pending_sum = false;

	// 4. no drive
	usb_there = false;
	poll_drive(true);
	shot("4-no-drive");
	T(bset.count == 1 && bid[0] == B_SETTINGS, "no drive: only Settings");
	T(tset.count == 0, "... and the strip has no button");
	T(!strcmp(title_now, "Backup: Plug in the backup drive"), "title: %s", title_now);
	nstarted = 0;
	key(0x0d);
	T(!nstarted, "Enter does nothing without a drive");

	// 5. a new stick: no backups yet
	usb_there = true;
	nstarted = 0;
	poll_drive(true);
	T(started_kind(BKJOB_SUMMARY), "plugged in: the drive is read");
	have_sum = true; sum.newest[0] = 0; sum.backups = 0; sum.free_kb = 7600000;
	shot("5-no-backups-yet");
	T(bset.count == 3 && bid[1] == B_SETTINGS, "no backups yet: no Show backups");
	T(!strcmp(title_now, "Backup: No backups yet"), "title: %s", title_now);

	// 6. a backup that failed
	nstarted = 0;
	key(0x0d);
	T(started_kind(BKJOB_BACKUP), "Enter: a backup (view %d, focus %d, started %d)", view, bset.focused, nstarted);
	fake_running = false; fake.rc = BK_FAILED;
	said("backup: the drive is full: 2.1 MB needed, 0.4 MB free (nothing may be removed)");
	job_done();
	pending_sum = false;
	shot("6-failed");
	T(!strcmp(title_now, "Backup: Not backed up"), "title: %s", title_now);
	last_rc = -1;
	snprintf(sum.newest, sizeof sum.newest, "2026-10-21_11-30-48");

	// 7. no clock: "no. 12"
	snprintf(sum.newest, sizeof sum.newest, "0000-00-00_00-00-00+0012");
	shot("7-no-clock");
	T(!strcmp(title_now, "Backup: no. 12"), "title: %s", title_now);
	snprintf(sum.newest, sizeof sum.newest, "2026-10-21_11-30-48");

	// Remove drive
	key(Z_KEY_RIGHT); key(Z_KEY_RIGHT); key(Z_KEY_RIGHT);
	T(bset.focused == 3 && bid[3] == B_REMOVE, "focus on Remove drive: %d", bset.focused);
	key(0x0d);
	shot("8-removed");
	T(!drive_there && !strcmp(title_now, "Backup: You can pull out the drive now"), "title: %s", title_now);
	T(bset.count == 1 && bset.focused == -1, "Remove drive gone, no focus (Enter cannot open Settings): %d", bset.focused);
	usb_there = true; poll_drive(true); have_sum = true;
	T(!note[0], "plugged in again: the note is gone");
	T(bset.count == 4 && bset.focused == 0, "and the focus is on Back up now: %d", bset.focused);
	// Back up now while a watch backup (or the drive's summary) runs: right after
	fake_running = true; fake.kind = BKJOB_SUMMARY;
	nstarted = 0;
	key(0x0d);
	T(want_full && !nstarted && bset.count == 4, "pressed during a summary: wanted, buttons unchanged");
	fake_running = false; want_full = false;

	// Show backups: files, at the backup folder
	key('b');
	T(!note[0], "B: files started");
	// Settings
	key('s');
	T(view == V_SETTINGS, "S: settings");
	shot("9-settings");
	key(0x1b);
	T(view == V_MAIN, "Esc: back");

	// 10.-11. Aa: the strip and back; M the same
	aa();
	T(view == V_STRIP, "Aa: small");
	set_size(STRIP_W, STRIP_H);
	shot("10-small");
	running(BK_PH_COPY, 700000, 1000000);
	shot("11-small-running");
	fake_running = false;
	key('m');
	T(view == V_MAIN, "M: back");
	set_size(MAIN_W, MAIN_H);
	key('s'); aa();
	T(view == V_SETTINGS, "Aa does nothing in Settings");
	key(0x1b);

	// 12.-15. watch
	watch_cfg = "/home/book /etc";
	watch_restart();
	T(watch.n == 1 && strstr(bkw_why(&watch), "/etc is not in From"), "watching /home/book: %s", bkw_why(&watch));
	shot("12-watching");
	T(!strcmp(title_now, "Backup: today 11:30, watching"), "title: %s", title_now);
	watch.pend[0] = strdup("/home/book/ch3.txt");
	watch.npend = 1;
	snprintf(watch.last_saved, sizeof watch.last_saved, "/home/book/ch3.txt");
	watch.last = z_uptime_ticks() - 3 * Z_TICK_HZ;
	shot("13-watch-saved");
	T(strstr(watch_shown, "ch3.txt saved: backed up in ") != NULL, "the watch's line: %s", watch_shown);
	T(!strcmp(title_now, "Backup: ch3.txt saved"), "title: %s", title_now);
	watch.last = z_uptime_ticks() - 11 * Z_TICK_HZ;
	nstarted = 0;
	watch_turn();
	T(started_kind(BKJOB_BACKUP) && fake.o.nonly == 1 && !strcmp(fake.o.only[0], "/home/book/ch3.txt"),
		"due: a watch backup of ch3.txt");
	fake_running = true; fake.kind = BKJOB_BACKUP; fake.prog.phase = BK_PH_COPY;
	shot("14-watch-backing-up");
	fake_running = false; fake.rc = BK_DONE;
	job_done();
	pending_sum = false;
	T(!watch_job && watch_at_set && !strcmp(watch_at, "14:40") && last_rc == -1, "done at 14:40; not the window's backup");
	shot("15-watched");
	T(strstr(watch_shown, "Watching /home/book: ch3.txt backed up at 14:40") != NULL, "watch: %s", watch_shown);
	aa(); set_size(STRIP_W, STRIP_H);
	shot("16-small-watching");
	usb_there = false; poll_drive(true);
	shot("17-small-no-drive");
	T(strstr(watch_shown, "not backed up: no drive") != NULL, "watch: %s", watch_shown);
	aa(); set_size(MAIN_W, MAIN_H);
	watch.pend[0] = strdup("/home/book/ch4.txt"); watch.npend = 1;
	watch.last = z_uptime_ticks() - 11 * Z_TICK_HZ;
	nstarted = 0;
	watch_turn();
	T(!nstarted && !watch.npend && watch.lost == 1, "no drive: the save let go, nothing started");
	shot("18-watch-no-drive");
	usb_there = true; poll_drive(true);
	bkw_stop(&watch);
	T(watches_made == 0, "fs's watches let go");

	// F1
	dialogs = 0;
	key(Z_KEY_HELP);
	T(dialogs == 1, "F1: help");

	printf("render: %d checks, %d failed (pictures in %s)\n", checks, fails, dir);
	return fails != 0;
}
