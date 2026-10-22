/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * backup's window. See bkwin.h and docs/backup.md, "The window".
 *
 * One small window that answers one question -- am I backed up? -- in a
 * headline, a line or two under it, and one button:
 *
 *   Backed up today 14:40
 *   412 files - /usb, 12.3 GB free
 *   Watching /home/book: book.txt backed up at 14:38
 *   [Back up now] [Show backups] [Settings] [Remove drive]
 *
 * The titlebar's Aa icon (or M) turns it into a strip for a corner of
 * the screen and back; shaded, its title says the same. Getting files
 * back is copying them from the drive (Show backups opens `files` there;
 * README.TXT on the drive says how) or `backup restore` in a shell.
 *
 * -- views --
 *
 *   main      the above. While a backup runs: what it is doing, a bar,
 *             Stop (Esc).
 *   strip     one line, the watch's line, and Back up / Stop.
 *   settings  What to back up (apps.backup.from) and Back up each save
 *             (apps.backup.watch); everything else is in zeitlos.cfg.
 *
 * -- watch (bkwatch.h; docs/backup.md, "Watch") --
 *
 * While the window is open, what apps.backup.watch names is watched
 * through fs; 10 s after a save there, the file is backed up on its own
 * (a watch backup), in the main and strip views -- in Settings it waits
 * until you are back.
 *
 * -- responsiveness (docs/responsive.md) --
 *
 * All work on the drive runs in bkjob.c's coroutine, one request to fs
 * a slice, and the main loop answers its messages between slices. The
 * main loop itself makes at most one quick call to fs a second while
 * idle (is the drive there?), and a watch read when fs says something
 * changed. Two things can still hold it: the first free-space count on
 * a stick whose FSINFO is not valid (docs/backup.md, "Running alongside
 * everything"), and a dialog (a prompt, the help box), during which a
 * job waits.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zwin.h"
#include "../../common/zwm.h"
#include "../../common/zwidget.h"
#include "../../common/zdialog.h"
#include "../../common/zkbd.h"
#include "../../common/zfont.h"
#include "../../common/zfsc.h"
#include "../../common/zfs.h"
#include "../../common/zcfg.h"
#include "../../common/zrtc.h"
#include "../../common/zpaths.h"

#include "bk.h"
#include "bkjob.h"
#include "bkwin.h"
#include "bkwatch.h"

#define F           (&z_font_5x8)
#define BIG         (&z_font_8x16)  // the headline
#define LH          12          // a line
#define M           8           // the margin
#define BTN_H       18
#define GAP         4

#define MAIN_W      316
#define MAIN_H      100
#define STRIP_W     264
#define STRIP_H     30

static z_win_t win;
static z_dialog_ctx_t dlg;
static int cw, ch;              // content size now

enum { V_MAIN, V_STRIP, V_SETTINGS };
static int view = V_MAIN;
static bool quit;

static bk_opts_t opts;
static bk_io_t base;

// -- what the window knows ---------------------------------------------------

static bool drive_there;        // the volume of apps.backup.to is mounted
static bool have_sum;           // sum is from the drive as it is
static bool sum_failed;         // ... it could not be read
static bk_summary_t sum;
static char last_line[BKJOB_LINE_MAX];  // what the engine said last
static int last_rc = -1;        // the last backup this window ran (not the watch's): its rc
static char note[BKJOB_LINE_MAX];       // a line of the window's own, until the next action
static bool pending_sum;        // read the drive again once this job ends
static bool want_full;          // Back up now, pressed while a short job ran

// the watch
static bkw_t watch;
static bool watch_job;                  // the backup running is the watch's
static const char *watch_only[BKW_PENDING];
static char watch_file[64];             // what the last watch backup was for
static bool watch_failed;
static bool watch_at_set;               // the clock was set: watch_at is its time
static char watch_at[8];                // "11:32"
static uint32_t watch_tick;             // when it ended (uptime)
static char watch_shown[200];           // the watch's line as drawn

static bool busy_backup(void);

// -- dates and sizes -----------------------------------------------------------

static const char *const MON[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
	"Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char *const WD[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

static bool now_local(z_tm_t *t) {
	bk_tm_t b;
	uint32_t up;
	if (!base.clock || !base.clock(base.ctx, &b, &up)) return false;
	memset(t, 0, sizeof *t);
	t->year = b.year; t->month = (uint8_t)b.month; t->day = (uint8_t)b.day;
	t->hour = (uint8_t)b.hour; t->min = (uint8_t)b.min; t->sec = (uint8_t)b.sec;
	return true;
}

// A backup's name as people say it: "today 11:30", "yesterday 18:00",
// "Mon 19 Oct 18:00"; with the clock not set, "no. 12".
static void when(const char *name, char *out, size_t n) {
	char base_s[20];
	int k;
	bool w;
	if (!bk_name_parse(name, base_s, &k, &w)) { snprintf(out, n, "%s", name); return; }
	if (k && !strncmp(base_s, "0000", 4)) { snprintf(out, n, "no. %d", k); return; }
	z_tm_t t;
	memset(&t, 0, sizeof t);
	t.year = atoi(base_s); t.month = (uint8_t)atoi(base_s + 5); t.day = (uint8_t)atoi(base_s + 8);
	t.hour = (uint8_t)atoi(base_s + 11); t.min = (uint8_t)atoi(base_s + 14);
	uint32_t secs = z_tm_to_time(&t);
	z_time_to_tm(secs, &t);         // the weekday
	char day[20];
	z_tm_t now;
	if (!k && now_local(&now)) {
		uint32_t d0 = z_tm_to_time(&now) / 86400, d1 = secs / 86400;
		if (d0 == d1) snprintf(day, sizeof day, "today");
		else if (d0 == d1 + 1) snprintf(day, sizeof day, "yesterday");
		else snprintf(day, sizeof day, "%s %u %s", WD[t.wday % 7], t.day, MON[(t.month + 11) % 12]);
	} else snprintf(day, sizeof day, "%s %u %s", WD[t.wday % 7], t.day, MON[(t.month + 11) % 12]);
	// +N on a set date: the clock was not set then; the date is a floor
	snprintf(out, n, "%s%s %02u:%02u", k ? "after " : "", day, t.hour, t.min);
}

static void kb_text(uint32_t kb, char *o, size_t n) {
	if (kb < 10 * 1024) snprintf(o, n, "%u KB", (unsigned)kb);
	else if (kb < 1024 * 1024) snprintf(o, n, "%u MB", (unsigned)(kb / 1024));
	else snprintf(o, n, "%u.%u GB", (unsigned)(kb / (1024 * 1024)), (unsigned)(kb % (1024 * 1024) * 10 / (1024 * 1024)));
}

static void volume_of(const char *dest, char *out, size_t n) {
	snprintf(out, n, "%s", dest);
	char *s = strchr(out + 1, '/');
	if (s) *s = 0;
}

static const char *base_name(const char *p) {
	const char *s = strrchr(p, '/');
	return s && s[1] ? s + 1 : p;
}

// The engine's lines start "backup: "; on screen they do not.
static const char *plain(const char *l) { return strncmp(l, "backup: ", 8) ? l : l + 8; }

// -- drawing helpers -------------------------------------------------------

static void text(int x, int y, const char *s) { z_win_draw_text(&win, x, y, s, 1, F); }

// s cut to fit w pixels, "..." at the end when cut.
static void text_fit_f(int x, int y, int w, const char *s, const z_font_t *f) {
	int max = w / f->w;
	char b[160];
	if (max <= 0) return;
	if ((int)strlen(s) <= max) { z_win_draw_text(&win, x, y, s, 1, f); return; }
	if (max > (int)sizeof b - 1) max = (int)sizeof b - 1;
	snprintf(b, sizeof b, "%.*s...", max > 3 ? max - 3 : 0, s);
	z_win_draw_text(&win, x, y, b, 1, f);
}
static void text_fit(int x, int y, int w, const char *s) { text_fit_f(x, y, w, s, F); }

static void clear(int x, int y, int w, int h) { z_win_fill_rect(&win, x, y, w, h, 0); }

// A frame and a bar, both with the blitter (docs/widgets.md: rasterizer
// lines and blitter glyphs do not order against each other).
static void bar(int x, int y, int w, int h, int permille) {
	clear(x, y, w, h);
	z_win_fill_rect(&win, x, y, w, 1, 1);
	z_win_fill_rect(&win, x, y + h - 1, w, 1, 1);
	z_win_fill_rect(&win, x, y, 1, h, 1);
	z_win_fill_rect(&win, x + w - 1, y, 1, h, 1);
	int fw = (w - 4) * (permille < 0 ? 0 : permille > 1000 ? 1000 : permille) / 1000;
	if (fw > 0) z_win_fill_rect(&win, x + 2, y + 2, fw, h - 4, 1);
}

// -- widgets ---------------------------------------------------------------

// main: only the buttons that can be pressed now, left to right in this
// order (buttons_for_state()). bw[] holds the ones shown, bid[] which
// each is.
enum { B_BACKUP, B_SHOW, B_SETTINGS, B_REMOVE, B_COUNT };
static z_widget_t bw[B_COUNT];
static int bid[B_COUNT];
static z_widget_set_t bset;
static const int B_W[B_COUNT] = { 72, 72, 56, 72 };
static const char *const B_LABEL[B_COUNT] = { "Back up now", "Show backups", "Settings", "Remove drive" };

enum { S_FROM, S_WATCH, S_BACK, S_COUNT };
static z_widget_t sw[S_COUNT];
static z_widget_set_t sset;

enum { T_BACKUP, T_COUNT };
static z_widget_t tw[T_COUNT];
static z_widget_set_t tset;

static z_widget_set_t *cur_set(void) {
	return view == V_SETTINGS ? &sset : view == V_STRIP ? &tset : &bset;
}

static void button(z_widget_t *w, const char *label, int bwid) {
	w->type = Z_WIDGET_BUTTON;
	w->label = label;
	w->w = (int16_t)bwid; w->h = BTN_H;
	w->enabled = true;
}

#define Y_HEAD      6           // the headline (16 px)
#define Y_L1        28          // the line under it
#define Y_L2        (Y_L1 + LH) // the watch's line, or the bar
#define Y_BTN       (MAIN_H - BTN_H - 8)

static void layout(void) {
	cw = z_win_content_w(&win);
	ch = z_win_content_h(&win);
	int x = M;
	for (int i = 0; i < bset.count; i++) {
		bw[i].x = (int16_t)x; bw[i].y = (int16_t)Y_BTN;
		x += bw[i].w + GAP;
	}
	for (int i = 0; i < S_BACK; i++) {
		sw[i].x = (int16_t)(cw - M - 56); sw[i].y = (int16_t)(6 + i * 32);
	}
	sw[S_BACK].x = M; sw[S_BACK].y = (int16_t)Y_BTN;
	tw[T_BACKUP].x = (int16_t)(cw - 4 - 56);
	tw[T_BACKUP].y = (int16_t)((ch - BTN_H) / 2);
	z_widget_invalidate(&bset); z_widget_invalidate(&sset); z_widget_invalidate(&tset);
}

static void widgets_init(void) {
	memset(bw, 0, sizeof bw); memset(sw, 0, sizeof sw); memset(tw, 0, sizeof tw);
	for (int i = 0; i < B_COUNT; i++) { button(&bw[i], B_LABEL[i], B_W[i]); bid[i] = i; }
	button(&sw[S_FROM], "Change", 56);
	button(&sw[S_WATCH], "Change", 56);
	button(&sw[S_BACK], "Back", 56);
	button(&tw[T_BACKUP], "Back up", 56);
	z_widget_set_init(&bset, bw, B_COUNT, &win);
	z_widget_set_init(&sset, sw, S_COUNT, &win);
	z_widget_set_init(&tset, tw, T_COUNT, &win);
	z_widget_focus_set(&bset, B_BACKUP);
	z_widget_focus_set(&sset, S_FROM);
	z_widget_focus_set(&tset, T_BACKUP);
}

// -- the watch ----------------------------------------------------------------

static void watch_restart(void) {
	char spec[Z_CFG_VAL_MAX];
	bkw_stop(&watch);
	if (!z_cfg_get("apps.backup.watch", spec, sizeof spec)) spec[0] = 0;
	bkw_start(&watch, &opts, spec);
	watch_shown[0] = 0;
}

static bool usb_dest(void) {
	char vol[16];
	volume_of(opts.dest, vol, sizeof vol);
	return !strcmp(vol, Z_DIR_USB);
}

// The watch's line ("" when watch is off); shorter for the strip.
static void watch_text(char *b, size_t n, bool small) {
	char what[80], at[24];
	uint32_t now = z_uptime_ticks();
	snprintf(what, sizeof what, "%s", watch.n ? watch.what[0] : "");
	if (watch.n > 1) {
		size_t k = strlen(what);
		snprintf(what + k, sizeof what - k, " and %d more", watch.n - 1);
	}
	b[0] = 0;
	switch (bkw_state(&watch, drive_there)) {
	case BKW_OFF:
		break;
	case BKW_NONE:
		snprintf(b, n, "Not watching: %s", bkw_why(&watch));
		break;
	case BKW_NO_DRIVE:
		if (small) snprintf(b, n, "Saves are not backed up: no drive");
		else snprintf(b, n, "Saves in %s are not backed up: no drive", what);
		break;
	case BKW_WAITING: {
		const char *f = base_name(watch.last_saved[0] ? watch.last_saved : what);
		int left = BKW_QUIET - (int)((now - watch.last) / Z_TICK_HZ);
		if (watch_job) snprintf(b, n, "Backing up %s now", watch_file[0] ? watch_file : f);
		else if (view == V_SETTINGS) snprintf(b, n, "%s saved: backed up when you are back", f);
		else if (watch.writers && left <= 0) snprintf(b, n, "%s saved: waiting while it is written", f);
		else snprintf(b, n, "%s saved: backed up in %d s", f, left > 0 ? left : 1);
		break;
	}
	default:        // watching
		if (!watch_file[0]) {
			snprintf(b, n, small ? "Watching %s" : "Watching %s: each save is backed up", what);
			break;
		}
		if (watch_at_set) snprintf(at, sizeof at, "at %s", watch_at);
		else {
			uint32_t m = (now - watch_tick) / Z_TICK_HZ / 60;
			if (m < 1) snprintf(at, sizeof at, "just now");
			else if (m < 120) snprintf(at, sizeof at, "%u min ago", (unsigned)m);
			else snprintf(at, sizeof at, "%u h ago", (unsigned)(m / 60));
		}
		if (watch_failed) snprintf(b, n, "%s was NOT backed up (%s)", watch_file, at);
		else if (small) snprintf(b, n, "%s backed up %s", watch_file, at);
		else snprintf(b, n, "Watching %s: %s backed up %s", what, watch_file, at);
		break;
	}
}

// -- what the window says -------------------------------------------------------

// The headline, and the line under it.
static void headline(char *h, size_t hn, char *l1, size_t ln) {
	bkjob_t *j = bkjob_current();
	char w[48], f[24], vol[16];
	volume_of(opts.dest, vol, sizeof vol);
	l1[0] = 0;
	if (busy_backup()) {
		if (bkjob_cancelling()) { snprintf(h, hn, "Stopping..."); return; }
		snprintf(h, hn, "Backing up...");
		if (j->prog.phase == BK_PH_COPY)
			snprintf(l1, ln, "%u of %u: %s", (unsigned)j->prog.files_done + 1,
				(unsigned)j->prog.files_total, j->path_now);
		else if (j->prog.phase == BK_PH_ROOM) snprintf(l1, ln, "Making room on the drive");
		else if (j->prog.phase == BK_PH_INDEX || j->prog.phase == BK_PH_CHECK) snprintf(l1, ln, "Checking the copies");
		else snprintf(l1, ln, "Looking at what has changed");
		return;
	}
	if (note[0]) {      // what the person just did, until the next thing
		snprintf(h, hn, "%s", note);
		if (!strncmp(note, "You can pull", 12)) snprintf(l1, ln, "Plug it in again to back up");
		return;
	}
	if (!drive_there) {
		snprintf(h, hn, "Plug in the backup drive");
		snprintf(l1, ln, "A USB stick, or an SD card in a reader, formatted FAT32");
		return;
	}
	if (last_rc > BK_PARTIAL) {
		snprintf(h, hn, "Not backed up");
		snprintf(l1, ln, "%s", plain(last_line));
		return;
	}
	if (!have_sum) {
		snprintf(h, hn, "%s", sum_failed ? "Cannot read the drive" : "Looking at the drive...");
		if (sum_failed) snprintf(l1, ln, "%s", plain(last_line));
		return;
	}
	f[0] = 0;
	if (sum.free_kb) kb_text(sum.free_kb, f, sizeof f);
	if (!sum.newest[0]) {
		snprintf(h, hn, "No backups yet");
		if (f[0]) snprintf(l1, ln, "%s, %s free: Back up now makes the first", vol, f);
		else snprintf(l1, ln, "%s: Back up now makes the first", vol);
		return;
	}
	when(sum.newest, w, sizeof w);
	if (!strncmp(w, "no. ", 4)) snprintf(h, hn, "Last backup: %s", w);    // no clock: no date
	else snprintf(h, hn, "Backed up %s", w);
	if (last_rc == BK_PARTIAL) snprintf(l1, ln, "Some files left out: %s", plain(last_line));
	else if (f[0]) snprintf(l1, ln, "%u files - %s, %s free", (unsigned)sum.newest_files, vol, f);
	else snprintf(l1, ln, "%u files - %s", (unsigned)sum.newest_files, vol);
}

// The title, so a shaded window says it too.
static char title_now[80];
static void title_update(void) {
	char t[80], h[80], l1[160];
	bkjob_t *j = bkjob_current();
	int ws = bkw_state(&watch, drive_there);
	if (busy_backup() && !bkjob_cancelling()) {
		unsigned pm = j->prog.bytes_total ? (unsigned)(j->prog.bytes_done * 100 / j->prog.bytes_total) : 0;
		if (j->prog.phase == BK_PH_COPY) snprintf(t, sizeof t, "Backup %u%%", pm);
		else snprintf(t, sizeof t, "Backup: looking");
	} else if (!bkjob_running() && ws == BKW_WAITING) {
		snprintf(t, sizeof t, "Backup: %.40s saved", base_name(watch.last_saved));
	} else if (!bkjob_running() && ws == BKW_WATCHING && watch_failed) {
		snprintf(t, sizeof t, "Backup: %.40s NOT backed up", watch_file);
	} else {
		headline(h, sizeof h, l1, sizeof l1);
		// "Backed up today 14:40" -> "Backup: today 14:40"
		if (!strncmp(h, "Backed up ", 10)) snprintf(t, sizeof t, "Backup: %s%s", h + 10,
			ws == BKW_WATCHING ? ", watching" : "");
		else if (!strncmp(h, "Last backup: ", 13)) snprintf(t, sizeof t, "Backup: %s%s", h + 13,
			ws == BKW_WATCHING ? ", watching" : "");
		else snprintf(t, sizeof t, "Backup: %s", h);
	}
	if (strcmp(t, title_now)) {
		snprintf(title_now, sizeof title_now, "%s", t);
		z_win_set_title(&win, t);
	}
}

// -- the views ---------------------------------------------------------------

// A backup of the window's own is running (a watch backup is brief and
// changes no buttons; a summary neither).
static bool busy_backup(void) {
	return bkjob_running() && bkjob_current()->kind == BKJOB_BACKUP && !watch_job;
}

// Only what can be pressed is there: a backup running, Stop; no drive,
// Settings; a drive, Back up now, Show backups (once there are
// backups), Settings, Remove drive (a USB drive). The focus stays on the
// same button when it is still there; when it is not, there is none (so
// Enter cannot open something the person did not choose: with no
// focus, Enter is Back up now); and Back up now takes it when it comes
// back (once the drive is in) -- unless the person has moved it
// meanwhile.
static void buttons_for_state(void) {
	bool busy = busy_backup();
	bool want[B_COUNT];
	want[B_BACKUP] = busy || drive_there;
	want[B_SHOW] = !busy && drive_there && !(have_sum && !sum.newest[0] && !sum.newest_watch[0]);
	want[B_SETTINGS] = !busy;
	want[B_REMOVE] = !busy && drive_there && usb_dest();
	static int wanted = -1;         // the button that had the focus, gone for now
	int had = bset.focused >= 0 && bset.focused < bset.count ? bid[bset.focused] : -1;
	int ids[B_COUNT], n = 0;
	for (int i = 0; i < B_COUNT; i++) if (want[i]) ids[n++] = i;
	bool same = n == bset.count;
	for (int i = 0; same && i < n; i++) same = bid[i] == ids[i];
	const char *l = busy ? "Stop" : "Back up now";
	if (!same) {
		if (view == V_MAIN) clear(0, Y_BTN - 1, cw, BTN_H + 2);
		for (int i = 0; i < n; i++) {
			memset(&bw[i], 0, sizeof bw[i]);
			button(&bw[i], ids[i] == B_BACKUP ? l : B_LABEL[ids[i]], B_W[ids[i]]);
			bid[i] = ids[i];
		}
		bset.count = n;
		bset.focused = -1;
		bset.pressed = -1;
		int f = had >= 0 ? had : wanted;
		for (int i = 0; i < n; i++) if (bid[i] == f) z_widget_focus_set(&bset, i);
		if (had >= 0 && bset.focused < 0) wanted = B_BACKUP;   // the one that matters, when it is back
		else if (bset.focused >= 0) wanted = -1;
		layout();
	} else if (n && bid[0] == B_BACKUP && bw[0].label != l) { bw[0].label = l; bw[0].dirty = true; }
	if (bset.focused >= 0) wanted = -1;
	// the strip: its one button, while it can be pressed
	tset.count = want[B_BACKUP] ? 1 : 0;
	const char *tl = busy ? "Stop" : "Back up";
	if (tw[T_BACKUP].label != tl) { tw[T_BACKUP].label = tl; tw[T_BACKUP].dirty = true; }
	if (!tset.count && view == V_STRIP) clear(tw[T_BACKUP].x, tw[T_BACKUP].y, tw[T_BACKUP].w + 1, BTN_H + 1);
	if (tset.count && tset.focused < 0) z_widget_focus_set(&tset, T_BACKUP);
}

static void enable_for_job(void) { buttons_for_state(); }

static bool copying_bar(void) { return busy_backup(); }

static int permille(void) {
	bkjob_t *j = bkjob_current();
	return j->prog.phase == BK_PH_COPY && j->prog.bytes_total ?
		(int)(j->prog.bytes_done * 1000 / j->prog.bytes_total) : 0;
}

static void draw_main(void) {
	char h[80], l1[200], l2[200];
	headline(h, sizeof h, l1, sizeof l1);
	clear(0, 0, cw, Y_BTN - 2);
	text_fit_f(M, Y_HEAD, cw - 2 * M, h, BIG);
	text_fit(M, Y_L1, cw - 2 * M, l1);
	if (copying_bar()) {
		bar(M, Y_L2 + 1, cw - 2 * M - 96, 8, permille());
		text(cw - M - 88, Y_L2, "Keep the power on");
		snprintf(watch_shown, sizeof watch_shown, "-");
		return;
	}
	watch_text(l2, sizeof l2, false);
	snprintf(watch_shown, sizeof watch_shown, "%s", l2);
	text_fit(M, Y_L2, cw - 2 * M, l2);
}

static void draw_strip(void) {
	char h[80], l1[200], l2[200];
	int w = tw[T_BACKUP].x - 4 - 4;
	clear(0, 0, tw[T_BACKUP].x - 2, ch);
	headline(h, sizeof h, l1, sizeof l1);
	text_fit(4, 4, w, h);
	if (copying_bar()) {
		bar(4, 4 + LH, w, 8, permille());
		snprintf(watch_shown, sizeof watch_shown, "-");
		return;
	}
	watch_text(l2, sizeof l2, true);
	snprintf(watch_shown, sizeof watch_shown, "%s", l2);
	text_fit(4, 4 + LH, w, l2[0] ? l2 : l1);
}

static void setting_value(int i, char *b, size_t n) {
	char v[Z_CFG_VAL_MAX];
	if (i == S_WATCH) {
		if (!z_cfg_get("apps.backup.watch", v, sizeof v) || !v[0] || !strcmp(v, "none")) snprintf(b, n, "off");
		else snprintf(b, n, "%s", v);
		return;
	}
	int k = 0;
	b[0] = 0;
	for (int j = 0; j < opts.nfrom && k < (int)n - 2; j++)
		k += snprintf(b + k, n - (size_t)k, "%s%s", j ? " " : "", opts.from[j]);
	if (!opts.nfrom) snprintf(b, n, "nothing");
}

static void draw_settings(void) {
	char b[200];
	static const char *const LABEL[2] = { "What to back up", "Back up each save" };
	static const char *const HELP[2] = { "folders and files", "in these folders, 10 s after" };
	z_win_clear(&win);
	int vx = M + 19 * F->w;
	for (int i = 0; i < 2; i++) {
		int y = 6 + i * 32;
		text(M, y, LABEL[i]);
		setting_value(i, b, sizeof b);
		text_fit(vx, y, cw - M - 56 - 8 - vx, b);
		text_fit(vx, y + LH, cw - M - 56 - 8 - vx, HELP[i]);
	}
	text_fit(M + 64, sw[S_BACK].y + 5, cw - M - 64 - M, note[0] ? note : "The rest: apps.backup.* in zeitlos.cfg");
}

static void repaint(void) {
	enable_for_job();
	if (view == V_MAIN) { draw_main(); z_widget_draw_all(&bset, true); }
	else if (view == V_SETTINGS) { draw_settings(); z_widget_draw_all(&sset, true); }
	else { draw_strip(); z_widget_draw_all(&tset, true); }
	title_update();
}

// Only what a running job changes, a few times a second.
static void repaint_progress(void) {
	enable_for_job();
	if (view == V_MAIN) { draw_main(); z_widget_draw_all(&bset, false); }
	else if (view == V_STRIP) { draw_strip(); z_widget_draw_all(&tset, false); }
	title_update();
}

// -- jobs -------------------------------------------------------------------

static bool start(int kind) {
	bkjob_t j;
	memset(&j, 0, sizeof j);
	backup_settings(&opts);
	j.kind = kind;
	j.o = opts;
	if (kind == BKJOB_BACKUP) { note[0] = 0; last_rc = -1; }
	if (!bkjob_start(&j)) return false;
	repaint();
	return true;
}

static void watch_backup(void) {
	bkjob_t j;
	memset(&j, 0, sizeof j);
	backup_settings(&opts);
	int n = bkw_take(&watch, watch_only, BKW_PENDING);
	if (!n) return;
	snprintf(watch_file, sizeof watch_file, "%.40s", n == 1 ? base_name(watch_only[0]) :
		watch.last_saved[0] ? base_name(watch.last_saved) : "the saves");
	if (n > 1) {
		size_t k = strlen(watch_file);
		snprintf(watch_file + k, sizeof watch_file - k, " and %d more", n - 1);
	}
	j.kind = BKJOB_BACKUP;
	j.o = opts;
	j.o.only = watch_only;
	j.o.nonly = n;
	if (!bkjob_start(&j)) { bkw_done(&watch); return; }
	watch_job = true;
	repaint();
}

static void job_done(void) {
	bkjob_t *j = bkjob_current();
	snprintf(last_line, sizeof last_line, "%s", bkjob_last_line());
	switch (j->kind) {
	case BKJOB_SUMMARY:
		have_sum = j->rc == BK_DONE;
		sum_failed = !have_sum && drive_there;
		if (have_sum) sum = j->sum;
		break;
	case BKJOB_BACKUP:
		pending_sum = true;
		if (watch_job) {
			// never ejected after: that would end the watch
			z_tm_t t;
			watch_job = false;
			bkw_done(&watch);
			watch_failed = j->rc > BK_PARTIAL;
			watch_tick = z_uptime_ticks();
			watch_at_set = now_local(&t);
			if (watch_at_set) snprintf(watch_at, sizeof watch_at, "%02u:%02u", t.hour, t.min);
			break;
		}
		last_rc = j->rc;
		if (j->rc <= BK_PARTIAL && usb_dest() && z_cfg_get_bool("apps.backup.eject", false) &&
				zfs_umount(Z_DIR_USB) >= 0) {
			snprintf(note, sizeof note, "You can pull out the drive now");
			drive_there = false;
			pending_sum = false;
		}
		break;
	}
	repaint();
}

// -- the drive ------------------------------------------------------------------

static uint32_t last_poll;

static void poll_drive(bool force) {
	uint32_t now = z_uptime_ticks();
	if (!force && now - last_poll < Z_TICK_HZ) return;
	last_poll = now;
	char vol[16];
	volume_of(opts.dest, vol, sizeof vol);
	z_fs_info_t f;
	bool there = zfs_stat(vol, &f) >= 0;
	if (there != drive_there || force) {
		if (there && !drive_there) { note[0] = 0; last_rc = -1; }  // plugged in: a fresh start
		drive_there = there;
		have_sum = false;
		sum_failed = false;
		if (there) { if (!bkjob_running()) start(BKJOB_SUMMARY); else pending_sum = true; }
		repaint();
	}
}

// -- settings: one line of zeitlos.cfg, everything else kept -------------------

static char cfg_in[Z_CFG_FILE_MAX + 1], cfg_out[Z_CFG_FILE_MAX];

// Reads, changes one key (z_cfg_text_set: every other line kept byte for
// byte, as the settings app does), writes NAME.part, renames it over
// the file (nothing half written), reloads.
static bool cfg_save(const char *key, const char *val) {
	int32_t fd = zfs_open(Z_CFG_PATH, Z_O_RDONLY);
	int n = 0;
	if (fd >= 0) {
		int32_t r;
		while (n < Z_CFG_FILE_MAX && (r = zfs_read(fd, cfg_in + n, (uint32_t)(Z_CFG_FILE_MAX - n))) > 0) n += r;
		zfs_close(fd);
	}
	if (n >= Z_CFG_FILE_MAX) { snprintf(note, sizeof note, "zeitlos.cfg is too large to change here"); return false; }
	int k = z_cfg_text_set(cfg_in, (size_t)n, key, val, cfg_out, Z_CFG_FILE_MAX);
	if (k < 0) { snprintf(note, sizeof note, "That cannot be saved"); return false; }
	if (k == 0) k = snprintf(cfg_out, sizeof cfg_out, "# " Z_CFG_PATH " -- see docs/config.md\n");
	char part[64];
	snprintf(part, sizeof part, "%s.part", Z_CFG_PATH);
	fd = zfs_open(part, Z_O_WRONLY | Z_O_CREAT | Z_O_TRUNC);
	bool ok = fd >= 0 && zfs_write(fd, cfg_out, (uint32_t)k) == k && zfs_fsync(fd) >= 0;
	if (fd >= 0) zfs_close(fd);
	if (ok) {
		zfs_unlink(Z_CFG_PATH);
		ok = zfs_rename(part, Z_CFG_PATH) >= 0;
	} else zfs_unlink(part);
	if (!ok) { snprintf(note, sizeof note, "Could not write " Z_CFG_PATH " (no card?)"); return false; }
	z_cfg_reload(NULL);
	backup_settings(&opts);
	snprintf(note, sizeof note, "Saved");
	return true;
}

static void change_setting(int i) {
	char cur[200], out[Z_CFG_VAL_MAX];
	setting_value(i, cur, sizeof cur);
	bool got;
	if (i == S_WATCH) {
		if (!strcmp(cur, "off")) cur[0] = 0;
		got = z_dialog_prompt_empty_ok(&dlg, "Back up each save",
			"Folders whose saves are backed up 10 s after,\n"
			"while the backup window is open.\n"
			"Empty: off.", cur, out, sizeof out);
		if (got && !out[0]) snprintf(out, sizeof out, "none");
	} else {
		got = z_dialog_prompt(&dlg, "What to back up",
			"Folders and files, separated by spaces\n"
			"(quotes around one with a space in it):", cur, out, sizeof out);
	}
	if (got) {
		cfg_save(i == S_WATCH ? "apps.backup.watch" : "apps.backup.from", out);
		watch_restart();        // From decides what may be watched, too
		if (watch.set && bkw_why(&watch)[0]) snprintf(note, sizeof note, "Not watched: %s", bkw_why(&watch));
		poll_drive(true);
	}
	layout();
	repaint();
}

// -- actions ----------------------------------------------------------------------

static void go_view(int v) {
	if (v == view) return;
	int was = view;
	view = v;
	if (v == V_STRIP) z_win_resize(&win, STRIP_W + (win.w - cw), STRIP_H + (win.h - ch));
	else if (was == V_STRIP) z_win_resize(&win, MAIN_W + (win.w - cw), MAIN_H + (win.h - ch));
	if (was == V_SETTINGS || v == V_SETTINGS) note[0] = 0;
	layout();
	z_win_clear(&win);
	repaint();
}

static void toggle_size(void) {
	if (view != V_SETTINGS) go_view(view == V_STRIP ? V_MAIN : V_STRIP);
}

static void back_up_or_stop(void) {
	if (busy_backup()) { bkjob_cancel(); repaint_progress(); return; }
	if (!drive_there) return;
	if (bkjob_running()) { want_full = true; return; }     // a summary or a watch backup: then
	start(BKJOB_BACKUP);
}

static void show_backups(void) {
	char p[BK_PATH_MAX];
	z_fs_info_t fi;
	// the backup folder itself, so README.TXT (how to get files back)
	// is the first thing there, beside this computer's folder
	snprintf(p, sizeof p, "%s/%s", opts.dest, opts.name);
	if (zfs_stat(p, &fi) < 0) { snprintf(note, sizeof note, "No backups on the drive yet"); repaint(); return; }
	snprintf(p, sizeof p, "%s", opts.dest);
	z_launch_arg_set(p);
	if (!z_proc_run("files")) { snprintf(note, sizeof note, "files did not start"); repaint(); }
}

static void remove_drive(void) {
	if (bkjob_running()) { snprintf(note, sizeof note, "Not yet: the drive is being written"); repaint(); return; }
	if (zfs_umount(Z_DIR_USB) >= 0) {
		snprintf(note, sizeof note, "You can pull out the drive now");
		drive_there = false;
		have_sum = false;
	} else snprintf(note, sizeof note, "Not yet: a file on the drive is open");
	repaint();
}

static void activate(int idx) {
	if (idx < 0) return;
	if (view == V_MAIN) {
		if (idx >= bset.count) return;
		switch (bid[idx]) {
		case B_BACKUP: back_up_or_stop(); break;
		case B_SHOW: show_backups(); break;
		case B_SETTINGS: go_view(V_SETTINGS); break;
		case B_REMOVE: remove_drive(); break;
		}
	} else if (view == V_SETTINGS) {
		if (idx == S_BACK) go_view(V_MAIN);
		else change_setting(idx);
	} else back_up_or_stop();
}

// -- input --------------------------------------------------------------------------

static const char HELP[] =
	"Dated backups of your files on a second drive -- a USB stick, or "
	"an SD card in a reader. A backup only reads the card.\n\n"
	"Enter\tBack up now (or the focused button)\n"
	"Esc\tStop a backup; back from Settings\n"
	"B\tShow backups (in files)\n"
	"S\tSettings\n"
	"M\tSmall and back (the Aa icon)\n"
	"F5\tLook at the drive again\n"
	"Tab\tNext button\n\n"
	"Getting a file back: Show backups opens the drive in files. Each "
	"folder is one backup, named by its date, and holds only the files "
	"that had changed: the newest copy of a file is in the newest folder "
	"that has it. Copy it back. A whole folder: `backup restore /home` "
	"in a shell.\n\n"
	"Back up each save (Settings): while this window is open, each file "
	"saved in those folders is backed up on its own 10 s later.\n\n"
	"Keep the power on while a backup writes; stopping it at any time "
	"is safe.";

static void handle_key(uint32_t k, uint8_t mods) {
	bool shift = (mods & Z_KBD_MOD_SHIFT) != 0;
	enable_for_job();       // what may be pressed now decides what a key does
	if (k == Z_KEY_HELP) { z_dialog_help(&dlg, "Backup help", HELP, "backup"); repaint(); return; }
	if (k == 0x1b) {
		if (bkjob_running()) { bkjob_cancel(); want_full = false; repaint_progress(); return; }
		if (view == V_SETTINGS) go_view(V_MAIN);
		return;
	}
	if (k == Z_KEY_F5) { poll_drive(true); return; }
	char c = (k < 128) ? (char)(k | 0x20) : 0;
	if (c == 'm') { toggle_size(); return; }
	if (view == V_MAIN && !busy_backup()) {
		if (c == 'b' && drive_there) { show_backups(); return; }
		if (c == 's') { go_view(V_SETTINGS); return; }
	}
	z_widget_set_t *set = cur_set();
	switch (k) {
	case '\t': z_widget_focus_next(set, shift); z_widget_draw_all(set, false); return;
	case Z_KEY_LEFT: case Z_KEY_UP: z_widget_focus_next(set, true); z_widget_draw_all(set, false); return;
	case Z_KEY_RIGHT: case Z_KEY_DOWN: z_widget_focus_next(set, false); z_widget_draw_all(set, false); return;
	case 0x0d: case ' ': {
		int a = z_widget_key_activate(set);
		if (a < 0 && view != V_SETTINGS && k == 0x0d) back_up_or_stop();
		else activate(a);
		return;
	}
	}
}

static void handle_mouse(uint32_t packed) {
	int cx, cy;
	bool inside = z_win_mouse_content_xy(&win, packed, &cx, &cy);
	uint8_t buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);
	z_widget_set_t *set = cur_set();
	if (!inside && set->pressed < 0) return;
	activate(z_widget_mouse(set, cx, cy, buttons));
}

// Messages for the window while a dialog is up, and from the main loop.
static void on_msg(z_msg_t *m, void *user) {
	(void)user;
	switch (m->subject) {
	case Z_WM_SET_CLIP:
		z_win_apply_clip(&win, &m->obj);
		break;
	case Z_WM_REDRAW:
		if (m->obj.type != Z_UINT32 || z_win_redraw_id(m->obj.val.uint32) != win.id) break;
		z_win_apply_redraw(&win, m->obj.val.uint32);
		z_widget_invalidate(cur_set());
		z_win_clear(&win);
		repaint();
		z_win_redraw_done(&win);
		break;
	case Z_WM_WINDOW_MOVED:
		z_win_parse_rect(&win, &m->obj);
		break;
	case Z_WM_WINDOW_RESIZED:
		z_win_parse_rect(&win, &m->obj);
		layout();
		break;
	case Z_WM_CLOSE:
		quit = true;
		bkjob_cancel();
		break;
	case Z_FS_CHANGED:      // a save under the watch (read in watch_turn())
		bkw_msg(&watch, m);
		break;
	default:
		break;
	}
}

static void handle(z_msg_t *m) {
	switch (m->subject) {
	case Z_WM_KEY:
		if (m->obj.type == Z_UINT32 && Z_WM_UNPACK_KEY_PRESSED(m->obj.val.uint32))
			handle_key(Z_WM_UNPACK_KEY_KEYSYM(m->obj.val.uint32), (uint8_t)Z_WM_UNPACK_KEY_MODIFIERS(m->obj.val.uint32));
		break;
	case Z_WM_MOUSE:
		if (m->obj.type == Z_UINT32) handle_mouse(m->obj.val.uint32);
		break;
	case Z_WM_TITLEBAR_ICON:    // Aa: small and back
		if (m->obj.type == Z_UINT32 && (int)Z_WM_UNPACK_TBICON_ID(m->obj.val.uint32) == win.id &&
				Z_WM_UNPACK_TBICON_KIND(m->obj.val.uint32) == Z_WM_TBICON_FONT)
			toggle_size();
		break;
	default:
		on_msg(m, NULL);
		break;
	}
}

// The loop's turn: read what changed, start the watch backup when it is
// due, keep the watch's line (and the title) up to date.
static void watch_turn(void) {
	uint32_t now = z_uptime_ticks();
	bkw_poll(&watch, now);
	if (!bkjob_running() && !pending_sum && bkw_due(&watch, now)) {
		if (!drive_there) bkw_drop(&watch);     // not kept: no drive, no watch
		else if (view != V_SETTINGS) watch_backup();
	}
	char b[200];
	watch_text(b, sizeof b, view == V_STRIP);
	if (strcmp(b, watch_shown) && !bkjob_running()) {
		if (view == V_MAIN) draw_main();
		else if (view == V_STRIP) draw_strip();
		title_update();
	}
}

int bkwin_run(void) {
	// The engine's stack, in this frame: above the heap (bkjob.h). On the
	// host the engine took ~16.5 KB of it (tests/test_job.c); twice that.
	uint8_t costack[32 * 1024];

	if (z_win_create_flags(&win, "Backup", MAIN_W + 4, MAIN_H + Z_WM_TITLEBAR_H + 4, -1, -1,
			Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_FONT_ICON) != Z_OK)
		return BKWIN_NO_WM;
	{
		char ignored[8];
		z_launch_arg_take(ignored, sizeof ignored);
	}
	dlg.parent = &win;
	dlg.on_msg = on_msg;
	dlg.user = NULL;

	backup_settings(&opts);
	backup_base_io(&base);
	bkjob_init(&base, costack, sizeof costack);

	widgets_init();
	layout();
	z_win_clear(&win);
	poll_drive(true);
	watch_restart();
	repaint();

	uint32_t last_ui = 0;
	for (;;) {
		z_msg_t m;
		while (z_msg_read(&m) == Z_OK) handle(&m);
		if (quit && !bkjob_running()) break;
		if (bkjob_running()) {
			bool more = bkjob_step();
			if (!more) { job_done(); continue; }
			uint32_t now = z_uptime_ticks();
			if (now - last_ui > Z_TICK_HZ / 8) { last_ui = now; repaint_progress(); bkw_poll(&watch, now); }
			continue;
		}
		if (want_full) {
			want_full = false;
			if (drive_there) start(BKJOB_BACKUP);
			continue;
		}
		if (pending_sum) {
			pending_sum = false;
			if (drive_there) start(BKJOB_SUMMARY);
			continue;
		}
		poll_drive(false);
		watch_turn();
		z_proc_wait(Z_TICK_HZ / 4);
	}
	bkw_stop(&watch);
	z_win_destroy(&win);
	return 0;
}
