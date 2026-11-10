/*
 * checkup -- tests this machine, and saves what it found (docs/checkup.md)
 *
 *   > run checkup
 *
 * One window, one line per check, run in order as soon as it opens:
 *
 *   System        the release, the ABI, the bitstream's memory and uptime
 *   Memory        the kernel's pool, and a pattern test of a block of RAM
 *                 borrowed as this app's arena (nobody else's memory)
 *   Flash store   the key/value store answers, and how full it is
 *   Random        the TRNG: present, healthy, and its words not stuck
 *   Clock         the RTC: present, set, and counting
 *   Memory card   mounted, its size, and /sys/manifest read to the end
 *   RAM disk      a file written, read back and removed in /tmp
 *   Network       the network driver running
 *   Crashes       the last crash report still in the console log
 *
 * Nothing it does writes the card: the card is only read, and the write
 * test is on the RAM disk. **Save report...** writes everything it found,
 * the last crash and the console log to a file the person chooses --
 * a USB stick, the RAM disk, or the card, as they like -- and to nothing
 * else. Nothing is sent anywhere.
 *
 * -- responsive --
 *
 * The checks are steps, each well under 20ms (docs/responsive.md): the
 * RAM test does 16KB a step, the clock and the TRNG wait across steps
 * rather than in one. Keys and clicks are answered between steps. Run
 * again (F5) starts over.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zgfx.h"
#include "../../common/zfont.h"
#include "../../common/zkbd.h"
#include "../../common/zwidget.h"
#include "../../common/zdialog.h"
#include "../../common/zfsapp.h"
#include "../../common/zfs.h"
#include "../../common/zrtc.h"
#include "../../common/zrng.h"
#include "../../common/zkv.h"
#include "../../common/zconsole.h"
#include "../../common/zcrash.h"
#include "../../common/zpaths.h"
#include "../../common/zversion.h"

#define WIN_W   440
#define WIN_H   290

static z_win_t win;
static z_dialog_ctx_t dlg;

#define MARGIN  8
#define LINE_H  (z_font_5x8.h + 4)
#define BTN_H   16
#define GAP     4
#define NAME_X  MARGIN
#define STAT_X  (MARGIN + 13 * 6)
#define INFO_X  (STAT_X + 6 * 6)

// -- the checks --

enum { C_SYSTEM, C_MEMORY, C_FLASH, C_RANDOM, C_CLOCK, C_CARD, C_RAMDISK, C_NET, C_CRASH, C_COUNT };

typedef enum { ST_WAIT, ST_RUN, ST_PASS, ST_FAIL, ST_SKIP, ST_NOTE } status_t;

static const char *const check_names[C_COUNT] = {
	"System", "Memory", "Flash store", "Random", "Clock", "Memory card",
	"RAM disk", "Network", "Crashes",
};
static const char *const status_words[] = { "", "...", "pass", "FAIL", "skip", "note" };

typedef struct {
	status_t	st;
	char		info[64];
} result_t;

static result_t res[C_COUNT];
static int cur;				// the check running; C_COUNT when done
static int phase;			// where it is in it
static bool dirty_rows;

// state carried between steps
static uint32_t t0, c0, n0, words, ones, last_word, repeats, bytes;
static uint32_t arena_size;
static int fh = -1;

// the console log, read once per run, for Crashes and the report
static char klog[Z_KLOG_SIZE];
static uint32_t klog_n, crash_s, crash_l;
static bool crash_found;

static void set(int c, status_t st, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#include <stdarg.h>
static void set(int c, status_t st, const char *fmt, ...) {
	va_list ap;
	res[c].st = st;
	va_start(ap, fmt);
	vsnprintf(res[c].info, sizeof(res[c].info), fmt, ap);
	va_end(ap);
	dirty_rows = true;
}

static void next_check(void) { cur++; phase = 0; if (cur < C_COUNT) res[cur].st = ST_RUN; dirty_rows = true; }

// -- each check, one step at a time; true when it is finished --

static bool check_system(void) {
	char abi[24];
	z_abi(abi, sizeof(abi));
	set(C_SYSTEM, ST_PASS, "Zeitlos %s, abi %s, %luMB, up %lus", Z_OS_VERSION, abi[0] ? abi : "?",
		(unsigned long)z_soc_mem_mb(), (unsigned long)(z_uptime_ticks() / Z_TICK_HZ));
	return true;
}

// An arena -- memory beside this app's own, freed after -- written with
// a pattern that differs at every word and its complement, then read
// back. 16KB a step.
#define MEM_TEST_MAX  (256u * 1024)
#define MEM_STEP      (16u * 1024)

static uint32_t pattern(uint32_t i, int pass) {
	uint32_t v = i * 2654435761u ^ 0xA5A5A5A5u;
	return pass ? ~v : v;
}

static bool check_memory(void) {

	z_mem_stats_args_t ms;
	void *base;

	if (phase == 0) {
		memset(&ms, 0, sizeof(ms));
		if (!z_mem_stats(&ms)) { set(C_MEMORY, ST_SKIP, "this kernel does not report memory"); return true; }
		uint32_t want = ms.largest_free / 4;
		if (want > MEM_TEST_MAX) want = MEM_TEST_MAX;
		want &= ~(MEM_STEP - 1);
		if (want < MEM_STEP || z_arena_set(want, &base, &arena_size) != Z_ARENA_OK || !base) {
			set(C_MEMORY, ST_SKIP, "%luKB free; none to borrow for a test", (unsigned long)(ms.free / 1024));
			return true;
		}
		n0 = ms.free / 1024;
		bytes = 0;
		phase = 1;
		set(C_MEMORY, ST_RUN, "%luKB free; testing %luKB", (unsigned long)n0, (unsigned long)(arena_size / 1024));
		return false;
	}

	// phases 1-4: write 0, verify 0, write 1, verify 1 -- each over the arena
	z_arena_set(Z_ARENA_QUERY, &base, NULL);
	if (!base) { set(C_MEMORY, ST_FAIL, "the arena went away"); return true; }
	volatile uint32_t *w = (volatile uint32_t *)base;
	uint32_t first = bytes / 4, n = MEM_STEP / 4;
	int pass = phase >= 3;
	bool verify = !(phase & 1);

	for (uint32_t i = first; i < first + n; i++) {
		if (!verify) w[i] = pattern(i, pass);
		else if (w[i] != pattern(i, pass)) {
			set(C_MEMORY, ST_FAIL, "at %08lx: %08lx, not %08lx", (unsigned long)((uintptr_t)&w[i]),
				(unsigned long)w[i], (unsigned long)pattern(i, pass));
			z_arena_set(0, NULL, NULL);
			return true;
		}
	}
	bytes += MEM_STEP;
	if (bytes < arena_size) return false;
	bytes = 0;
	if (++phase <= 4) return false;

	z_arena_set(0, NULL, NULL);
	set(C_MEMORY, ST_PASS, "%luKB free; %luKB written and read back twice", (unsigned long)n0,
		(unsigned long)(arena_size / 1024));
	return true;

}

static bool check_flash(void) {
	z_kv_info_t ki;
	memset(&ki, 0, sizeof(ki));
	int rc = z_kv_info(&ki);
	if (rc == Z_KV_E_NOSYS) { set(C_FLASH, ST_SKIP, "this kernel has no key/value store"); return true; }
	if (rc != Z_KV_OK) { set(C_FLASH, ST_FAIL, "the store does not answer (%d)", rc); return true; }
	set(C_FLASH, ST_PASS, "%lu keys, %lu of %lu bytes used%s", (unsigned long)ki.keys,
		(unsigned long)ki.live, (unsigned long)ki.size,
		(ki.flags & Z_KV_INFO_WRITABLE) ? "" : ", read-only on this bitstream");
	return true;
}

// The TRNG: its health bit, then 256 raw words in at most two seconds,
// counted for ones (about half) and for a word repeated straight after
// itself (a stuck source). Not a randomness test -- a working one.
#define RNG_WORDS 256

static bool check_random(void) {

	uint32_t w;

	if (phase == 0) {
		if (!z_rng_present()) { set(C_RANDOM, ST_SKIP, "no TRNG in this bitstream"); return true; }
		if (!z_rng_hw_healthy()) z_rng_hw_clear();
		t0 = z_uptime_ticks();
		words = ones = repeats = 0;
		phase = 1;
	}

	for (int k = 0; k < 64 && words < RNG_WORDS && z_rng_hw_word(&w); k++) {
		if (words && w == last_word) repeats++;
		last_word = w;
		for (uint32_t b = w; b; b &= b - 1) ones++;
		words++;
	}

	if (words < RNG_WORDS) {
		if (z_uptime_ticks() - t0 < 2 * Z_TICK_HZ) return false;
		set(C_RANDOM, ST_FAIL, "%lu of %u words in 2s%s", (unsigned long)words, RNG_WORDS,
			z_rng_hw_healthy() ? "" : "; its health test failed");
		return true;
	}

	uint32_t pct = ones * 100 / (RNG_WORDS * 32);
	bool ok = repeats == 0 && pct >= 45 && pct <= 55 && z_rng_hw_healthy();
	set(C_RANDOM, ok ? ST_PASS : ST_FAIL, "healthy%s; %lu%% ones, %lu repeats", z_rng_hw_healthy() ? "" : ": NO",
		(unsigned long)pct, (unsigned long)repeats);
	return true;

}

// The RTC: there, set, and one second later, counting.
static bool check_clock(void) {
	if (phase == 0) {
		if (!z_rtc_available()) { set(C_CLOCK, ST_SKIP, "no clock in this bitstream"); return true; }
		t0 = z_uptime_ticks();
		c0 = z_rtc_seconds();
		phase = 1;
		return false;
	}
	if (z_uptime_ticks() - t0 < Z_TICK_HZ + Z_TICK_HZ / 4) return false;
	uint32_t d = z_rtc_seconds() - c0;
	if (d < 1 || d > 2) { set(C_CLOCK, ST_FAIL, "counted %lu seconds in 1.25", (unsigned long)d); return true; }
	if (!z_rtc_valid()) set(C_CLOCK, ST_NOTE, "counting, but not set yet (the network sets it)");
	else set(C_CLOCK, ST_PASS, "set, and counting");
	return true;
}

// The card: mounted, and a file read to the end, 1KB a step.
static bool check_card(void) {

	static char buf[1024];

	if (phase == 0) {
		uint32_t total = 0, freek = 0;
		if (!fs_df(&total, &freek) || !total) { set(C_CARD, ST_SKIP, "no card"); return true; }
		n0 = total;
		c0 = freek;
		words = (uint32_t)fs_size((char *)Z_PATH_SYS_MANIFEST);
		if (!words) { set(C_CARD, ST_NOTE, "%luMB, %luMB free; no %s to read", (unsigned long)(n0 / 1024),
			(unsigned long)(c0 / 1024), Z_PATH_SYS_MANIFEST); return true; }
		fh = fs_open_read(Z_PATH_SYS_MANIFEST);
		if (fh < 0) { set(C_CARD, ST_FAIL, "%s does not open", Z_PATH_SYS_MANIFEST); return true; }
		bytes = 0;
		phase = 1;
		return false;
	}

	int r = fs_read_chunk(fh, buf, sizeof(buf));
	if (r > 0) { bytes += (uint32_t)r; return false; }
	fs_close_handle(fh);
	fh = -1;
	if (r < 0 || bytes != words)
		set(C_CARD, ST_FAIL, "%s: read %lu of %lu bytes", Z_PATH_SYS_MANIFEST, (unsigned long)bytes,
			(unsigned long)words);
	else
		set(C_CARD, ST_PASS, "%luMB, %luMB free; %lu%s read", (unsigned long)(n0 / 1024),
			(unsigned long)(c0 / 1024), (unsigned long)(bytes >= 1024 ? bytes / 1024 : bytes),
			bytes >= 1024 ? "KB" : " bytes");
	return true;

}

// The RAM disk: 8KB written, read back, removed.
#define RAM_TEST_PATH  Z_DIR_TMP "/checkup.tmp"
#define RAM_TEST_LEN   8192

static bool check_ramdisk(void) {
	static uint8_t a[RAM_TEST_LEN], b[RAM_TEST_LEN];
	for (uint32_t i = 0; i < RAM_TEST_LEN; i++) a[i] = (uint8_t)(i * 7 + (i >> 8));
	if (fs_write_file((char *)RAM_TEST_PATH, (char *)a, RAM_TEST_LEN) != RAM_TEST_LEN) {
		set(C_RAMDISK, ST_SKIP, "no RAM disk on this machine");
		return true;
	}
	memset(b, 0, sizeof(b));
	int r = fs_read_file((char *)RAM_TEST_PATH, (char *)b, RAM_TEST_LEN);
	fs_unlink((char *)RAM_TEST_PATH);
	if (r != RAM_TEST_LEN || memcmp(a, b, RAM_TEST_LEN))
		set(C_RAMDISK, ST_FAIL, "8KB written, %d read back%s", r, r == RAM_TEST_LEN ? ", different" : "");
	else
		set(C_RAMDISK, ST_PASS, "8KB written, read back and removed");
	return true;
}

static bool check_net(void) {
	uint32_t pid;
	if (z_pid_lookup("net0", &pid)) set(C_NET, ST_PASS, "the network driver is running (pid %lu)", (unsigned long)pid);
	else set(C_NET, ST_NOTE, "the network driver is not running");
	return true;
}

// The console log, as much as the kernel still holds (Z_KLOG_SIZE).
static void read_log(void) {
	uint32_t pos = 0, n;
	klog_n = 0;
	while (klog_n < sizeof(klog) && (n = z_klog_read(&pos, (uint8_t *)klog + klog_n,
			(uint32_t)sizeof(klog) - klog_n, NULL)) > 0)
		klog_n += n;
}

static bool check_crash(void) {
	read_log();
	crash_found = z_crash_find(klog, klog_n, &crash_s, &crash_l);
	if (!crash_found) { set(C_CRASH, ST_PASS, "none in the console log"); return true; }
	// the first line, without "*** "
	uint32_t e = crash_s + 4;
	while (e < crash_s + crash_l && klog[e] != '\n') e++;
	set(C_CRASH, ST_NOTE, "%.*s", (int)(e - crash_s - 4), klog + crash_s + 4);
	return true;
}

static bool (*const check_fns[C_COUNT])(void) = {
	check_system, check_memory, check_flash, check_random, check_clock, check_card,
	check_ramdisk, check_net, check_crash,
};

static void start_run(void) {
	for (int c = 0; c < C_COUNT; c++) { res[c].st = ST_WAIT; res[c].info[0] = 0; }
	if (fh >= 0) { fs_close_handle(fh); fh = -1; }
	z_arena_set(0, NULL, NULL);
	cur = -1;
	next_check();
}

// One step of the check running. Returns true while there is more.
static bool step(void) {
	if (cur >= C_COUNT) return false;
	if (check_fns[cur]()) next_check();
	return cur < C_COUNT;
}

// -- the report --

static char report[Z_KLOG_SIZE + 2048];

static int build_report(void) {

	int n = 0, fails = 0;
	size_t cap = sizeof(report);

	for (int c = 0; c < C_COUNT; c++) if (res[c].st == ST_FAIL) fails++;

	n += snprintf(report + n, cap - (size_t)n, "Zeitlos checkup, up %lus: %s\n\n",
		(unsigned long)(z_uptime_ticks() / Z_TICK_HZ),
		cur < C_COUNT ? "not finished" : fails ? "something failed" : "nothing failed");
	for (int c = 0; c < C_COUNT && n < (int)cap; c++)
		n += snprintf(report + n, cap - (size_t)n, "%-12s %-5s %s\n", check_names[c],
			status_words[res[c].st], res[c].info);
	if (crash_found && n < (int)cap)
		n += snprintf(report + n, cap - (size_t)n, "\nThe last crash:\n%.*s", (int)crash_l, klog + crash_s);
	if (n < (int)cap)
		n += snprintf(report + n, cap - (size_t)n, "\nThe console log:\n%.*s", (int)klog_n, klog);
	if (n > (int)cap - 1) n = (int)cap - 1;
	return n;

}

// -- the window --

enum { B_AGAIN, B_SAVE, B_CLOSE, B_COUNT };
static z_widget_t buttons[B_COUNT];
static z_widget_set_t bset;
static char status[72];
static bool finished;

static int rows_y(void) { return MARGIN + z_font_8x16.h + LINE_H + 6; }
static int btn_y(void) { return z_win_content_h(&win) - BTN_H - MARGIN; }

static void layout(void) {
	int cw = z_win_content_w(&win), y = btn_y();
	static const int w[B_COUNT] = { 70, 100, 60 };
	int x = cw - MARGIN;
	for (int i = B_COUNT - 1; i >= 0; i--) {
		x -= w[i];
		buttons[i].x = (int16_t)x; buttons[i].y = (int16_t)y;
		buttons[i].w = (int16_t)w[i]; buttons[i].h = BTN_H;
		x -= GAP;
	}
	z_widget_invalidate(&bset);
}

static void draw_text(int x, int y, const char *s) { z_win_draw_text(&win, x, y, s, 1, &z_font_5x8); }

static void draw_rows(void) {
	int y = rows_y();
	int w = z_win_content_w(&win) - 2 * MARGIN;
	char info[80];
	for (int c = 0; c < C_COUNT; c++, y += LINE_H) {
		z_win_fill_rect(&win, MARGIN, y - 1, w, LINE_H, 0);
		draw_text(NAME_X, y, check_names[c]);
		draw_text(STAT_X, y, status_words[res[c].st]);
		snprintf(info, sizeof(info), "%.52s", res[c].info);
		draw_text(INFO_X, y, info);
	}
	dirty_rows = false;
}

static void draw_status(void) {
	int y = btn_y() + (BTN_H - z_font_5x8.h) / 2;
	z_win_fill_rect(&win, MARGIN, y, buttons[0].x - GAP - MARGIN, z_font_5x8.h, 0);
	if (status[0]) draw_text(MARGIN, y, status);
}

static void repaint(void) {
	z_win_clear(&win);
	z_win_draw_text(&win, MARGIN, MARGIN, "Checkup", 1, &z_font_8x16);
	draw_text(MARGIN, MARGIN + z_font_8x16.h + 3, "Tests this machine. The card is only read.");
	draw_rows();
	z_win_fill_rect(&win, MARGIN, btn_y() - 7, z_win_content_w(&win) - 2 * MARGIN, 1, 1);
	z_widget_draw_all(&bset, true);
	draw_status();
}

static void set_status(const char *s) { snprintf(status, sizeof(status), "%s", s); draw_status(); }

// Save report...: where the person chooses. A USB stick if one is in,
// else the RAM disk -- not the card first, since a checkup is often run
// because something is wrong.
static void save_report(void) {

	char path[Z_FS_PATH_MAX];
	z_fs_info_t fi;
	const char *dir = fs_stat(Z_DIR_USB, &fi) ? Z_DIR_USB : Z_DIR_RAM;

	if (!z_dialog_save(&dlg, dir, "checkup.txt", path, sizeof(path))) { repaint(); return; }
	if (fs_size(path) > 0 && z_dialog_confirm(&dlg, "Save report", "Replace the file that is there?",
			Z_DIALOG_YES_NO) != Z_DIALOG_YES) { repaint(); return; }

	int n = build_report();
	if (fs_write_file(path, report, n) == n) snprintf(status, sizeof(status), "saved: %.56s", path);
	else snprintf(status, sizeof(status), "could not write %.48s", path);
	repaint();

}

static void activate(int b) {
	if (b == B_AGAIN) { start_run(); set_status(""); draw_rows(); }
	else if (b == B_SAVE) save_report();
	else if (b == B_CLOSE) finished = true;
}

static void handle_key(uint32_t keysym, uint8_t mods) {
	if (keysym == Z_KEY_HELP) {
		static const char help[] =
			"Tests this machine, one line a check, and saves what it "
			"found if you ask.\n\n"
			"F5\tRun again\n"
			"Tab\tThe next button\n"
			"Enter, Space\tPress it\n\n"
			"Nothing is written to the card: it is only read, and the "
			"write test is on the RAM disk. Save report writes the "
			"results, the last crash and the console log to a file you "
			"choose. Nothing is sent anywhere.";
		z_dialog_help(&dlg, "Checkup help", help, "checkup");
		repaint();
		return;
	}
	if (keysym == Z_KEY_F5) { activate(B_AGAIN); return; }
	if (keysym == '\t') {
		z_widget_focus_next(&bset, (mods & Z_KBD_MOD_SHIFT) != 0);
		z_widget_draw_all(&bset, false);
		return;
	}
	if (keysym == 0x0d || keysym == ' ') activate(z_widget_key_activate(&bset));
}

static void handle_mouse(uint32_t packed) {
	int cx, cy;
	bool inside = z_win_mouse_content_xy(&win, packed, &cx, &cy);
	if (!inside && bset.pressed < 0) return;
	activate(z_widget_mouse(&bset, cx, cy, (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed)));
}

static void on_wm_msg(z_msg_t *msg, void *user) {
	(void)user;
	switch (msg->subject) {
		case Z_WM_SET_CLIP: z_win_apply_clip(&win, &msg->obj); break;
		case Z_WM_REDRAW:
			if (msg->obj.type != Z_UINT32) break;
			if (z_win_redraw_id(msg->obj.val.uint32) != win.id) break;
			z_win_apply_redraw(&win, msg->obj.val.uint32);
			repaint();
			z_win_redraw_done(&win);
			break;
		case Z_WM_WINDOW_MOVED: z_win_parse_rect(&win, &msg->obj); break;
		default: break;
	}
}

static void widgets_init(void) {
	static const char *const labels[B_COUNT] = { "Run again", "Save report...", "Close" };
	memset(buttons, 0, sizeof(buttons));
	for (int i = 0; i < B_COUNT; i++) {
		buttons[i].type = Z_WIDGET_BUTTON;
		buttons[i].label = labels[i];
		buttons[i].enabled = true;
	}
	z_widget_set_init(&bset, buttons, B_COUNT, &win);
	z_widget_focus_set(&bset, B_SAVE);
}

int main(void) {

	printf("checkup: starting\n");

	if (z_win_create_flags(&win, "checkup", WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER) != Z_OK) {
		printf("checkup: failed to create window -- is wm running?\n");
		return 1;
	}
	{
		char arg[24];
		z_launch_arg_take(arg, sizeof(arg));
	}

	dlg.parent = &win;
	dlg.on_msg = on_wm_msg;
	dlg.user = NULL;

	widgets_init();
	layout();
	start_run();
	repaint();

	while (!finished) {

		z_msg_t msg;

		while (!finished && z_msg_read(&msg) == Z_OK) {
			switch (msg.subject) {
				case Z_WM_KEY:
					if (msg.obj.type != Z_UINT32 || !Z_WM_UNPACK_KEY_PRESSED(msg.obj.val.uint32)) break;
					handle_key(Z_WM_UNPACK_KEY_KEYSYM(msg.obj.val.uint32),
						(uint8_t)Z_WM_UNPACK_KEY_MODIFIERS(msg.obj.val.uint32));
					break;
				case Z_WM_MOUSE:
					if (msg.obj.type == Z_UINT32) handle_mouse(msg.obj.val.uint32);
					break;
				default:
					on_wm_msg(&msg, NULL);
					break;
			}
		}
		if (finished) break;

		// A step of the check running, then the messages again; with
		// nothing to run, wait for one.
		if (step()) {
			if (dirty_rows) draw_rows();
			z_proc_wait(1);
		} else {
			if (dirty_rows) {
				draw_rows();
				int fails = 0;
				for (int c = 0; c < C_COUNT; c++) if (res[c].st == ST_FAIL) fails++;
				snprintf(status, sizeof(status), fails ? "%d check%s failed" : "done: nothing failed",
					fails, fails == 1 ? "" : "s");
				draw_status();
			}
			z_proc_wait(0);
		}

	}

	if (fh >= 0) fs_close_handle(fh);
	z_arena_set(0, NULL, NULL);
	z_win_destroy(&win);
	return 0;

}
