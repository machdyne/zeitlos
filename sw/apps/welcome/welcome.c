/*
 * welcome -- what a new machine shows first (docs/welcome_app.md)
 *
 *   > run welcome               or, in term's REPL: (run "welcome")
 *
 * A core app, in flash. wm starts it once by itself (zwelcome.h):
 *
 *   - for a new card: the release tool writes /sys/firstrun on every
 *     card image, and never in a manifest, so `update` never does;
 *   - for a machine with no card that has never shown it: no
 *     Z_WELCOME_KV_SEEN in the flash key/value store.
 *
 * It removes the file and writes the key the moment it starts, so Skip,
 * Close, the close box and a power cut all count as having been shown.
 * It introduces the machine once; it does not nag.
 *
 * Its first job is the welcome: what this computer is, where to start,
 * the keys, and the guide (docs/welcome.md, in `read`; both are in
 * flash, so a machine with no card has them). With a card it then offers the
 * first questions, each optional:
 *
 *   Keyboard     the layout the machine starts in  (system.keyboard.layouts)
 *   Time zone    a city or an offset                (system.rtc.timezone)
 *   Display      white, amber, green or paper       (system.video.mode),
 *                only on a bitstream that can change it
 *   Ready        what is in use; the course, a password
 *
 * With no card there is nowhere to keep an answer, so it asks nothing:
 * the welcome is the whole of it.
 *
 * Nothing here is a setting of its own. Every value is one settings
 * already edits, written the way settings writes it: the one line for
 * that key replaced in /sys/zeitlos.cfg with z_cfg_text_set(), every
 * other line copied back byte for byte, then a reload. Desktop colour
 * themes and the rest are left to settings.
 *
 * -- small, because it is in flash --
 *
 * No dialogs (F1 shows the help in this window), no desktop colour
 * themes, the layouts' names from a table generated at build time
 * (kbd_names.h) rather than zkbd's key tables, and the integer-only
 * printf (ZFMT).
 *
 * -- keyboard-only, pointer-only --
 *
 * Enter is Next wherever the keyboard is, except on one of the page's
 * own buttons, which it presses. Esc is Back. Tab and the arrows move
 * between the controls. On a list the arrows move the selection, a
 * letter jumps to the first row starting with it, and Enter takes the
 * row and goes on. Every page can be done with the pointer alone.
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
#include "../../common/zkbd.h"		// the key names only; zkbd.o is not linked
#include "../../common/zwidget.h"
#include "../../common/zfsapp.h"
#include "../../common/zcfg.h"
#include "../../common/zrtc.h"
#include "../../common/zpaths.h"
#include "../../common/zkv.h"
#include "../../common/zwelcome.h"
#include "../../common/zspeak.h"

// The size of settings' window: the same lists, the same rows.
#define WIN_W   420
#define WIN_H   300

static z_win_t win;

#define MARGIN      8
#define LINE_H      (z_font_5x8.h + 2)
#define ROW_PX      (z_font_5x8.h + 2)	// a list row
#define BTN_H       16
#define NAV_W       64
#define GAP         4		// at least 2px, for the focus ring
#define DONE_BTN_W  116

// -- the pages --

enum { S_WELCOME, S_KEYBOARD, S_TIME, S_DISPLAY, S_DONE, S_COUNT };

typedef struct {
	const char	*title;
	const char	*note;		// under the title
} step_t;

static const step_t steps[S_COUNT] = {
	{ "Welcome to Zeitlos", "A few optional questions follow -- Skip leaves everything as it is" },
	{ "Keyboard", "The layout this machine starts in" },
	{ "Time zone", "Used by clock, cal and date" },
	{ "Display", "Applied at once, and saved" },
	{ "Ready", "Everything here can be changed in settings" },
};

static int step = S_WELCOME;
static bool has_card;			// a card is mounted: answers can be kept
static bool first_run;			// the card had /sys/firstrun
static bool finished;
static bool help_on;			// F1: the help, in place of the page

// What this board can do, probed once (zsoc.h).
static bool can_set_video;

// -- controls --
//
// One array, in page order, a widget set over each page's slice of it
// (as settings does per page), and one more set over the two buttons
// at the bottom, which every page shares.

#define GROUP_VIDEO 1
#define MODE_COUNT  4

static const uint32_t video_modes[MODE_COUNT] = {
	Z_VIDEO_MODE_WHITE, Z_VIDEO_MODE_AMBER, Z_VIDEO_MODE_GREEN, Z_VIDEO_MODE_PAPER,
};
static const char *video_labels[MODE_COUNT] = { "White", "Amber", "Green", "Paper" };

enum {
	W_GUIDE,			// Welcome
	W_SKIP,
	W_MODE0,			// Display: the four modes are W_MODE0..+3
	W_COURSE = W_MODE0 + MODE_COUNT,	// Ready
	W_PASSWORD,
	W_BACK,				// the bottom of every page
	W_NEXT,
	W_COUNT
};

// Each page's first control; the next entry ends it.
static const int step_first[S_COUNT + 1] = {
	W_GUIDE, W_MODE0, W_MODE0, W_MODE0, W_COURSE, W_BACK,
};

static z_widget_t widgets[W_COUNT];
static z_widget_set_t psets[S_COUNT];
static z_widget_set_t nav;
#define ps (&psets[step])

// -- the two lists --

static z_listbox_t kb_list, tz_list;

// Keyboard rows: every ordinary layout in zkbd's table, by name and
// description, generated from sw/common/zkbd_layouts.c at build time
// (gen_kbd_names.awk), so a layout added there appears here. The
// Japanese input methods are left out: they are a second layout to
// switch to, and settings turns them on with the font.
typedef struct { const char *name, *desc; } kbd_name_t;
static const kbd_name_t kbd_names[] = {
#include "kbd_names.h"
};
#define KB_COUNT ((int)(sizeof(kbd_names) / sizeof(kbd_names[0])))

static char kb_value[Z_CFG_VAL_MAX];	// system.keyboard.layouts, "" if absent
static char kb_first[24];		// its first name; "us" if absent

static char tz_value[Z_CFG_VAL_MAX];	// system.rtc.timezone; "UTC" if absent

static char status[72];
static int y_body, y_nav;

static z_listbox_t *cur_list(void) {
	if (help_on) return NULL;
	return step == S_KEYBOARD ? &kb_list : step == S_TIME ? &tz_list : NULL;
}

// -- time zones: the rows settings shows --
//
// Row 0 is UTC, then every city in z_tz_cities, then the whole-hour
// offsets UTC-12..UTC+14 for anywhere not listed.

#define TZ_OFFSET_MIN   -12
#define TZ_OFFSET_MAX    14
#define TZ_OFFSETS      (TZ_OFFSET_MAX - TZ_OFFSET_MIN)	// excluding 0
#define TZ_ITEMS        (1 + z_tz_city_count + TZ_OFFSETS)

static int tz_row_hours(int i) {
	int h = TZ_OFFSET_MIN + (i - 1 - z_tz_city_count);
	return h >= 0 ? h + 1 : h;		// skip 0, which is row 0 ("UTC")
}

// What choosing row `i` writes to the file.
static void tz_row_value(int i, char *out, int n) {
	if (i <= 0) snprintf(out, (size_t)n, "UTC");
	else if (i <= z_tz_city_count) snprintf(out, (size_t)n, "%s", z_tz_cities[i - 1].city);
	else z_tz_format_offset(tz_row_hours(i) * 60, out, n);
}

static const char *tz_label(void *user, int i) {

	static char buf[64];
	char off[Z_TZ_NAME_MAX];

	(void)user;

	if (i == 0) return "UTC";

	if (i <= z_tz_city_count) {
		const z_tz_city_t *c = &z_tz_cities[i - 1];
		z_tz_format_offset(c->std_min, off, sizeof(off));
		snprintf(buf, sizeof(buf), "%-14s %-9s%s", c->city, off,
			c->dst != Z_DST_NONE ? "  summer +1" : "");
		return buf;
	}

	z_tz_format_offset(tz_row_hours(i) * 60, buf, sizeof(buf));
	return buf;

}

// The row for a configured value, or -1 (UTC+5:30 has none).
static int tz_row_for(const char *value) {

	z_tz_t tz;

	if (!z_tz_parse(value, &tz)) return -1;

	if (tz.city) return 1 + (int)(tz.city - z_tz_cities);
	if (tz.std_off == 0) return 0;
	if (tz.std_off % 3600) return -1;

	int h = tz.std_off / 3600;
	if (h < TZ_OFFSET_MIN || h > TZ_OFFSET_MAX) return -1;
	return 1 + z_tz_city_count + (h < 0 ? h - TZ_OFFSET_MIN : h - TZ_OFFSET_MIN - 1);

}

// -- keyboard layouts --

static const char *kb_label(void *user, int i) {
	static char buf[64];
	(void)user;
	snprintf(buf, sizeof(buf), "%-36.36s %s", kbd_names[i].desc, kbd_names[i].name);
	return buf;
}

static char lower_ascii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static bool same_name(const char *a, const char *b) {
	while (*a && *b && lower_ascii(*a) == lower_ascii(*b)) { a++; b++; }
	return !*a && !*b;
}

// The row showing layout `name`, or -1 (an input method, or a name
// this build does not have).
static int kb_row_for(const char *name) {
	for (int i = 0; i < KB_COUNT; i++)
		if (same_name(kbd_names[i].name, name)) return i;
	return -1;
}

static const char *kb_desc(const char *name) {
	int r = kb_row_for(name);
	return r >= 0 ? kbd_names[r].desc : name;
}

// The first name in a comma- or space-separated list.
static void first_name(const char *v, char *out, size_t n) {
	size_t k = 0;
	while (*v == ',' || *v == ' ') v++;
	while (*v && *v != ',' && *v != ' ' && k + 1 < n) out[k++] = *v++;
	out[k] = 0;
}

// -- reading --

static void select_current_mode(void) {
	uint32_t now = z_video_get_mode();
	for (int i = 0; i < MODE_COUNT; i++)
		if (video_modes[i] == now) {
			z_widget_select(&psets[S_DISPLAY], i);
			break;
		}
}

static void read_values(void) {

	if (!z_cfg_get("system.keyboard.layouts", kb_value, sizeof(kb_value))) kb_value[0] = 0;
	first_name(kb_value, kb_first, sizeof(kb_first));
	if (!kb_first[0]) snprintf(kb_first, sizeof(kb_first), "us");

	if (!z_cfg_get("system.rtc.timezone", tz_value, sizeof(tz_value)))
		snprintf(tz_value, sizeof(tz_value), "UTC");

}

// -- which pages there are --

static bool step_shown(int s) {
	if (s == S_WELCOME) return true;
	if (!has_card) return false;		// nowhere to keep an answer
	if (s == S_DISPLAY) return can_set_video;
	return true;
}

static int step_after(int s, int dir) {
	for (s += dir; s >= 0 && s < S_COUNT; s += dir)
		if (step_shown(s)) return s;
	return -1;
}

// "2 of 5": where `s` is among the pages this machine has.
static void step_position(int s, int *pos, int *total) {
	*pos = *total = 0;
	for (int i = 0; i < S_COUNT; i++) {
		if (!step_shown(i)) continue;
		(*total)++;
		if (i <= s) (*pos)++;
	}
}

// -- layout --

static int body_w(void) { return z_win_content_w(&win) - 2 * MARGIN; }

// Where the Welcome page's guide button is: a line below its words.
static const char *const *welcome_text(int *n);
static int guide_y(void) { int n; welcome_text(&n); return y_body + (n + 1) * LINE_H; }

static void place(int idx, int x, int y, int w, int h) {
	widgets[idx].x = (int16_t)x;
	widgets[idx].y = (int16_t)y;
	widgets[idx].w = (int16_t)w;
	widgets[idx].h = (int16_t)h;
}

// Where the Ready page's three buttons are: below the three lines that
// say what is in use.
static int done_row_y(int i) { return y_body + 4 * LINE_H + 4 + i * (BTN_H + 8); }

static void layout(void) {

	int cw = z_win_content_w(&win), ch = z_win_content_h(&win);
	int bw = body_w();

	y_body = MARGIN + z_font_8x16.h + LINE_H + 10;	// below the title and its note
	y_nav = ch - BTN_H - MARGIN;

	// the bottom of every page: Back and Next, at the right
	place(W_NEXT, cw - MARGIN - NAV_W, y_nav, NAV_W, BTN_H);
	place(W_BACK, cw - MARGIN - 2 * NAV_W - GAP, y_nav, NAV_W, BTN_H);

	// Welcome: Skip, at the left of the same line, and the guide
	// below the words (welcome_text())
	place(W_SKIP, MARGIN, y_nav, NAV_W, BTN_H);
	place(W_GUIDE, MARGIN, guide_y(), DONE_BTN_W, BTN_H);

	// the lists: from under the line that says what is in use, to the
	// line above the buttons
	int yl = y_body + LINE_H + 4;
	int rows = (y_nav - 8 - yl - 2) / ROW_PX;
	if (rows < 3) rows = 3;
	z_listbox_set_geom(&kb_list, MARGIN, yl, bw, rows * ROW_PX + 2);
	z_listbox_set_geom(&tz_list, MARGIN, yl, bw, rows * ROW_PX + 2);

	// Display: the four modes across
	int mw = (bw - (MODE_COUNT - 1) * GAP) / MODE_COUNT;
	for (int i = 0; i < MODE_COUNT; i++)
		place(W_MODE0 + i, MARGIN + i * (mw + GAP), y_body + LINE_H + 2, mw, BTN_H);

	// Ready: two buttons, each with a line beside it
	place(W_COURSE, MARGIN, done_row_y(0), DONE_BTN_W, BTN_H);
	place(W_PASSWORD, MARGIN, done_row_y(1), DONE_BTN_W, BTN_H);

	for (int s = 0; s < S_COUNT; s++) z_widget_invalidate(&psets[s]);
	z_widget_invalidate(&nav);
	z_listbox_invalidate(&kb_list);
	z_listbox_invalidate(&tz_list);

}

// -- drawing --

static void draw_text(int x, int y, const char *s) {
	z_win_draw_text(&win, x, y, s, 1, &z_font_5x8);
}

static void draw_lines(int y, const char *const *text, int n) {
	for (int i = 0; i < n; i++) draw_text(MARGIN, y + i * LINE_H, text[i]);
}

#define LINES(a) (a), (int)(sizeof(a) / sizeof((a)[0]))

// The time zone in use and the time it makes.
static void describe_now(char *out, size_t n) {
	z_tz_t tz;
	bool dst = false;
	if (z_rtc_available() && z_rtc_valid() && z_tz_parse(tz_value, &tz)) {
		z_tm_t tm;
		z_time_to_tm(z_tz_local(&tz, z_rtc_seconds(), &dst), &tm);
		snprintf(out, n, "%.20s: now %s %u %s %02u:%02u %s", tz_value, z_wday_name(tm.wday),
			(unsigned)tm.day, z_month_name(tm.month), (unsigned)tm.hour, (unsigned)tm.min,
			z_tz_name(&tz, dst));
	} else
		snprintf(out, n, "%.20s -- the clock is not set yet (the network sets it)", tz_value);
}

static void draw_status(void) {
	int w = widgets[W_BACK].x - GAP - MARGIN;
	if (step == S_WELCOME && !help_on) return;	// Skip is there
	z_win_fill_rect(&win, MARGIN, y_nav, w, BTN_H, 0);
	if (status[0]) draw_text(MARGIN, y_nav + (BTN_H - z_font_5x8.h) / 2, status);
}

static void draw_header(const char *title, const char *note) {
	char pos[32];
	int p, t;
	z_win_draw_text(&win, MARGIN, MARGIN, title, 1, &z_font_8x16);
	draw_text(MARGIN, MARGIN + z_font_8x16.h + 3, note);
	step_position(step, &p, &t);
	if (t > 1 && !help_on) {
		snprintf(pos, sizeof(pos), "%d of %d", p, t);
		draw_text(z_win_content_w(&win) - MARGIN - (int)strlen(pos) * z_font_5x8.w, MARGIN + 4, pos);
	}
	z_win_fill_rect(&win, MARGIN, y_nav - 7, body_w(), 1, 1);	// above the buttons
}

// The keys, the same on every page and in every app.
static const char *const keys_text[] = {
	"  Enter     next                Esc       back",
	"  Tab       the next control    F1        help",
	"  Super+S   speech on or off    Super+K   on-screen keyboard",
};

static const char *const help_text[] = {
	"With a memory card, each answer is a line in",
	Z_PATH_SYS_CONFIG ", the file settings edits and changes later.",
	"",
	"  Enter       next; on a button, press it",
	"  Esc         back",
	"  Tab, arrows the next control",
	"  Space       press the focused button",
	"  A-Z, 0-9    in a list: jump to a row",
	"  Up, Down    in a list: the row before or after",
	"",
	"A display colour applies at once. A keyboard layout or a",
	"time zone is saved when you go on from its page.",
	"",
	"It opens by itself once on a new machine. After that:",
	"(run \"welcome\") in term's REPL, or `run welcome` at the",
	"console.",
	"",
	"Any key or click goes back to the page.",
};

// The Welcome page's words: with a card, the questions that follow;
// without one, where to start. Either way nothing about what is not
// there -- a person on their first minute does not need it.
static const char *const welcome_card[] = {
	"Every part of this computer is open and documented, from",
	"the processor to this window, and it is made to go on",
	"working: no account, no subscription, nothing to sign in to.",
	"",
	"If you like, the next pages ask for your keyboard, your",
	"time zone and the colour of the display. Each question is",
	"optional, and any answer can be changed later in settings.",
};

static const char *const welcome_flash[] = {
	"Every part of this computer is open and documented, from",
	"the processor to this window, and it is made to go on",
	"working: no account, no subscription, nothing to sign in to.",
	"",
	"Some places to start, in the dock: term, for Scheme, telnet,",
	"ssh and serial; text, for writing; files, for USB sticks. A",
	"memory card adds documents, courses, games and the manuals.",
};

static const char *const *welcome_text(int *n) {
	if (has_card) { *n = (int)(sizeof(welcome_card) / sizeof(welcome_card[0])); return welcome_card; }
	*n = (int)(sizeof(welcome_flash) / sizeof(welcome_flash[0]));
	return welcome_flash;
}

static void draw_step(void) {

	char line[96];
	int y = y_body;

	switch (step) {

	case S_WELCOME: {
		int n;
		const char *const *text = welcome_text(&n);
		draw_lines(y, text, n);
		y = guide_y();
		draw_text(MARGIN + DONE_BTN_W + 8, y + (BTN_H - z_font_5x8.h) / 2,
			"windows, the dock, files and the keys");
		y += BTN_H + 10;
		draw_text(MARGIN, y, "Keys that work here, and in every app:");
		draw_lines(y + 2 * LINE_H, LINES(keys_text));
		break;
	}

	case S_KEYBOARD:
		snprintf(line, sizeof(line), "In use: %.40s (%s)", kb_desc(kb_first), kb_first);
		draw_text(MARGIN, y, line);
		break;

	case S_TIME:
		describe_now(line, sizeof(line));
		draw_text(MARGIN, y, line);
		break;

	case S_DISPLAY:
		draw_text(MARGIN, y, "Display colour");
		y += LINE_H + 2 + BTN_H + 2 * LINE_H;
		draw_text(MARGIN, y, "Desktop colour themes are in settings, on boards that");
		draw_text(MARGIN, y + LINE_H, "have them (Super+F10 turns desktop colour on or off).");
		break;

	case S_DONE: {
		int tx = MARGIN + DONE_BTN_W + 8, ty = (BTN_H - z_font_5x8.h) / 2;
		snprintf(line, sizeof(line), "Keyboard    %.40s (%s)", kb_desc(kb_first), kb_first);
		draw_text(MARGIN, y, line);
		snprintf(line, sizeof(line), "Time zone   %.40s", tz_value);
		draw_text(MARGIN, y + LINE_H, line);
		snprintf(line, sizeof(line), "Display     %s", z_video_mode_name(z_video_get_mode()));
		draw_text(MARGIN, y + 2 * LINE_H, line);
		draw_text(tx, done_row_y(0) + ty, "Getting started, a course in learn");
		draw_text(tx, done_row_y(1) + ty, "for the screen lock and network logins");
		y = done_row_y(2) + 2;
		draw_text(MARGIN, y, "Finish closes this window. The manuals are in " Z_DIR_DOCS " (F1 in any");
		draw_text(MARGIN, y + LINE_H, "app), and settings changes anything chosen here.");
		break;
	}
	}

	if (ps->count) z_widget_draw_all(ps, true);
	if (cur_list()) z_listbox_draw(cur_list(), true);

}

// A disabled button is drawn as an empty box. Where it leads nowhere
// (Back on Welcome; Skip with no card, where Close does the same) that
// is a box about nothing: leave it out.
static void hide_if_disabled(int idx) {
	if (!widgets[idx].enabled)
		z_win_fill_rect(&win, widgets[idx].x - 2, widgets[idx].y - 2,
			widgets[idx].w + GAP, widgets[idx].h + 4, 0);
}

static void repaint(void) {
	z_win_clear(&win);
	if (help_on) {
		draw_header("Help", "welcome: what a new machine shows first");
		draw_lines(y_body, LINES(help_text));
	} else {
		draw_header(steps[step].title, step == S_WELCOME && !has_card
			? "What this computer is, and where to start" : steps[step].note);
		draw_step();
	}
	z_widget_draw_all(&nav, true);
	hide_if_disabled(W_BACK);
	if (step == S_WELCOME && !help_on) hide_if_disabled(W_SKIP);
	draw_status();
}

// -- keyboard focus --
//
// One tab order over three things that are not one widget set: the
// page's list, its own controls, and Back and Next. A stop is a small
// code naming one of them.

#define STOP_LIST   0
#define STOP_PAGE   1		// + the control's index in the page's set
#define STOP_NAV    100		// + 0 Back, 1 Next
#define STOPS_MAX   (W_COUNT + 1)

static int focus = -1;

static int stops(int *out) {
	int n = 0;
	if (cur_list()) out[n++] = STOP_LIST;
	for (int i = 0; i < ps->count; i++)
		if (ps->items[i].enabled) out[n++] = STOP_PAGE + i;
	for (int i = 0; i < nav.count; i++)
		if (nav.items[i].enabled) out[n++] = STOP_NAV + i;
	return n;
}

static void set_focus(int code) {
	z_listbox_t *lb = cur_list();
	focus = code;
	if (lb) z_listbox_set_focus(lb, code == STOP_LIST);
	if (ps->count)
		z_widget_focus_set(ps, code >= STOP_PAGE && code < STOP_NAV ? code - STOP_PAGE : -1);
	z_widget_focus_set(&nav, code >= STOP_NAV ? code - STOP_NAV : -1);
	if (ps->count) z_widget_draw_all(ps, false);
	z_widget_draw_all(&nav, false);
	if (lb) z_listbox_draw(lb, false);
}

static void step_focus(bool backward) {
	int st[STOPS_MAX], n = stops(st), at = -1;
	if (!n) return;
	for (int i = 0; i < n; i++) if (st[i] == focus) at = i;
	if (at < 0) at = backward ? 0 : n - 1;
	set_focus(st[(at + (backward ? n - 1 : 1)) % n]);
}

// -- writing: as settings writes --

static void set_status(const char *s) { snprintf(status, sizeof(status), "%s", s); }

// The file before and after an edit. Static, not allocated: an app's
// heap and stack share one small allowance (docs/executables.md), and
// settings once failed on the board with exactly this as a malloc.
static char cfg_in[Z_CFG_FILE_MAX + 1];
static char cfg_out[Z_CFG_FILE_MAX];

// Sets `key` to `val` in the file (NULL removes it) and reloads. Every
// other line is left exactly as it was. `said` is the status line when
// it worked.
static bool save(const char *key, const char *val, const char *said) {

	int n = fs_read_file((char *)Z_CFG_PATH, cfg_in, Z_CFG_FILE_MAX);
	bool ok = false;

	// A missing file reads as empty, and the first setting makes it.
	if (n < 0) {
		set_status(fs_size((char *)Z_CFG_PATH) > Z_CFG_FILE_MAX
			? "zeitlos.cfg is too large to edit here"
			: "could not read " Z_CFG_PATH);
		goto done;
	}

	n = z_cfg_text_set(cfg_in, (size_t)n, key, val, cfg_out, Z_CFG_FILE_MAX);
	if (n < 0) {
		set_status("that value cannot be saved");
		goto done;
	}

	// fs_write_file() reports bytes written, so an empty file could not
	// be told from a failed write: one that has lost its last setting
	// keeps a line saying what it is.
	if (n == 0)
		n = snprintf(cfg_out, Z_CFG_FILE_MAX, "# " Z_CFG_PATH " -- see docs/config.md\n");

	if (fs_write_file((char *)Z_CFG_PATH, cfg_out, n) != n) {
		set_status("not saved: the card could not be written");
		goto done;
	}

	if (z_cfg_reload(NULL) < 0) set_status("saved, but this kernel cannot reload it");
	else set_status(said);
	ok = true;

done:
	read_values();
	return ok;

}

// -- what each page does --

// Keyboard: the chosen layout becomes the first, and any others that
// were listed stay behind it as they were, so Super+Space still reaches
// them.
static void commit_keyboard(void) {

	char v[Z_CFG_VAL_MAX], said[72];
	int row = z_listbox_selected(&kb_list);

	if (row < 0 || row >= KB_COUNT) return;
	const kbd_name_t *l = &kbd_names[row];
	if (same_name(l->name, kb_first)) return;	// already the one in use

	size_t o = (size_t)snprintf(v, sizeof(v), "%s", l->name);
	const char *p = kb_value;
	while (*p) {
		char name[24];
		size_t k = 0;
		while (*p == ',' || *p == ' ') p++;
		while (*p && *p != ',' && *p != ' ') {
			if (k + 1 < sizeof(name)) name[k++] = *p;
			p++;
		}
		name[k] = 0;
		if (k && !same_name(name, l->name) && o + k + 2 < sizeof(v))
			o += (size_t)snprintf(v + o, sizeof(v) - o, ",%s", name);
	}

	snprintf(said, sizeof(said), "saved: %.40s keyboard", l->desc);
	save("system.keyboard.layouts", v, said);

}

static void commit_time(void) {

	char v[Z_CFG_VAL_MAX], said[72];
	int row = z_listbox_selected(&tz_list);

	if (row < 0) return;			// a value with no row is kept
	tz_row_value(row, v, sizeof(v));
	if (!strcmp(v, tz_value)) return;

	snprintf(said, sizeof(said), "saved: time zone %.40s", v);
	save("system.rtc.timezone", v, said);

}

// A display colour: applied there and then, and saved if the display
// took it. Read back rather than trusted -- the group must show what
// the display is doing.
static void apply_video(int idx) {

	if (idx < 0 || idx >= MODE_COUNT) return;

	z_video_set_mode(video_modes[idx]);
	if (z_video_get_mode() == video_modes[idx])
		save("system.video.mode", z_video_mode_name(video_modes[idx]), "saved");

	select_current_mode();

}

// Starts another app from the Ready page. This window stays, so Finish
// is still there when the other one is closed.
static bool launch(const char *app, const char *arg, const char *said) {

	char drop[64];

	if (arg) z_launch_arg_set(arg);
	if (z_proc_run(app)) {
		set_status(said);
		return true;
	}

	// Not on this card. An argument left pending would be handed to
	// whatever opens next, so it is taken back.
	if (arg) z_launch_arg_take(drop, sizeof(drop));
	snprintf(status, sizeof(status), "%s is not on this card", app);
	return false;

}

// The guide (docs/welcome.md), in `read`: both are in flash, so a
// machine with no card has them. `text` shows the same file as it is
// written, should read ever be missing (a small build that left it out).
static void open_guide(void) {
	if (!launch("read", Z_PATH_DOCS_WELCOME, "the guide is open"))
		launch("text", Z_PATH_DOCS_WELCOME, "the guide is open");
}

static void enter_step(int s) {

	z_listbox_t *lb;
	int p, t;

	if (ps->count) z_widget_focus_set(ps, -1);
	if (cur_list()) z_listbox_set_focus(cur_list(), false);

	step = s;
	widgets[W_BACK].enabled = step_after(step, -1) >= 0;
	widgets[W_NEXT].label = step_after(step, 1) >= 0 ? "Next" : has_card ? "Finish" : "Close";

	if (step == S_KEYBOARD) z_listbox_select(&kb_list, kb_row_for(kb_first));
	if (step == S_TIME) z_listbox_select(&tz_list, tz_row_for(tz_value));
	if (step == S_DISPLAY) select_current_mode();

	// The list if the page has one, else Next: Enter goes on either way.
	lb = cur_list();
	focus = lb ? STOP_LIST : STOP_NAV + 1;
	if (lb) z_listbox_set_focus(lb, true);
	z_widget_focus_set(&nav, lb ? -1 : 1);

	repaint();

	// Said aloud when speech is on: where this is, and what it is for.
	if (z_speak_available()) {
		char say[Z_SPEAK_SLOT_MAX];
		step_position(step, &p, &t);
		snprintf(say, sizeof(say), "Step %d of %d. %s. %s.", p, t, steps[step].title, steps[step].note);
		z_speak(say, 0);
	}

}

static void go_next(void) {

	int n;

	if (step == S_KEYBOARD) commit_keyboard();
	else if (step == S_TIME) commit_time();
	else if (step != S_DISPLAY) status[0] = 0;	// nothing was saved here

	n = step_after(step, 1);
	if (n < 0) { finished = true; return; }
	enter_step(n);

}

static void go_back(void) {
	int p = step_after(step, -1);
	if (p < 0) return;
	status[0] = 0;
	enter_step(p);
}

static void show_help(bool on) {
	help_on = on;
	if (on) {
		if (ps->count) z_widget_focus_set(ps, -1);
		z_listbox_set_focus(&kb_list, false);
		z_listbox_set_focus(&tz_list, false);
		z_listbox_invalidate(&kb_list);
		z_listbox_invalidate(&tz_list);
		repaint();
	} else
		enter_step(step);
}

static void activate(int idx) {

	if (idx < 0) return;

	if (idx == W_NEXT) { go_next(); return; }
	if (idx == W_BACK) { go_back(); return; }
	if (idx == W_SKIP) { finished = true; return; }
	if (idx == W_GUIDE) { open_guide(); return; }

	if (idx >= W_MODE0 && idx < W_MODE0 + MODE_COUNT) apply_video(idx - W_MODE0);
	else if (idx == W_COURSE) launch("learn", NULL, "learn is open: Getting started");
	else if (idx == W_PASSWORD) launch("settings", "security", "settings is open at Security");
	else return;

	repaint();

}

// -- input --

static void handle_mouse(uint32_t packed) {

	int cx, cy;
	bool inside = z_win_mouse_content_xy(&win, packed, &cx, &cy);
	uint8_t buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);
	z_listbox_t *lb = cur_list();
	bool held = (ps->count && ps->pressed >= 0) || nav.pressed >= 0;

	// The help: a press anywhere in the window goes back to the page.
	if (help_on) {
		static uint8_t last;
		bool down = (buttons & Z_MOUSE_BTN_LEFT) && !(last & Z_MOUSE_BTN_LEFT);
		last = buttons;
		if (down && inside) show_help(false);
		return;
	}

	// Samples over the titlebar reach us too. Same guard as settings.
	if (!inside && !held && !(lb && lb->sb.dragging)) return;

	// The list first, and for the whole of a scrollbar drag it started.
	if (lb && !held) {
		if (z_listbox_has_pointer(lb, cx, cy)) {
			if ((buttons & Z_MOUSE_BTN_LEFT) && focus != STOP_LIST) set_focus(STOP_LIST);
			if (z_listbox_mouse(lb, cx, cy, buttons) == Z_LIST_ACTIVATED) go_next();
			return;
		}
		z_listbox_mouse(lb, cx, cy, buttons);		// keep its edges fed
	}

	if (ps->count && nav.pressed < 0) {
		int at = step;				// activate() may change the page
		int act = z_widget_mouse(ps, cx, cy, buttons);
		if (act >= 0) {
			set_focus(STOP_PAGE + act);
			activate(step_first[at] + act);
			return;
		}
		if (ps->pressed >= 0) return;
	}

	int act = z_widget_mouse(&nav, cx, cy, buttons);
	if (act >= 0) activate(W_BACK + act);

}

static void handle_wheel(int notches) {
	z_listbox_t *lb = cur_list();
	if (!lb) return;
	z_listbox_wheel(lb, notches);
	z_listbox_draw(lb, false);
}

static bool is_alnum(uint32_t k) {
	return (k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9');
}

static void handle_key(uint32_t keysym, uint8_t mods) {

	z_listbox_t *lb = cur_list();
	bool back = (mods & Z_KBD_MOD_SHIFT) != 0;

	// F1: the help, in this window (docs/app_help.md). Any key goes
	// back to the page, and does nothing else.
	if (help_on) { show_help(false); return; }
	if (keysym == Z_KEY_HELP) { show_help(true); return; }

	if (keysym == 0x1b) { go_back(); return; }

	if (keysym == '\t') { step_focus(back); return; }

	// -- the list has the keyboard: every key it uses goes to it --
	if (lb && focus == STOP_LIST) {
		if (keysym == 0x0d) { go_next(); return; }
		if (z_listbox_key(lb, keysym) == Z_LIST_ACTIVATED) go_next();
		return;
	}

	// A letter belongs to the list wherever the keyboard is: "press B,
	// get Berlin" should not first need Tab.
	if (lb && is_alnum(keysym)) {
		set_focus(STOP_LIST);
		z_listbox_key(lb, keysym);
		return;
	}

	switch (keysym) {

		case Z_KEY_UP:
		case Z_KEY_LEFT:
			step_focus(true);
			return;

		case Z_KEY_DOWN:
		case Z_KEY_RIGHT:
			step_focus(false);
			return;

		case 0x0d:
		case ' ':
			// On one of the page's own controls: that control. Anywhere
			// else Enter is Next, and Space presses what has the focus.
			if (focus >= STOP_PAGE && focus < STOP_NAV) {
				int at = step;
				int act = z_widget_key_activate(ps);
				if (act >= 0) activate(step_first[at] + act);
			} else if (focus >= STOP_NAV) {
				int act = z_widget_key_activate(&nav);
				if (act >= 0) activate(W_BACK + act);
			} else if (keysym == 0x0d)
				go_next();
			return;

		default:
			return;

	}

}

// Every message the loop below does not handle itself. The redraw ack
// is not optional -- see zwin.h.
static void on_wm_msg(z_msg_t *msg) {

	switch (msg->subject) {
		case Z_WM_SET_CLIP:
			z_win_apply_clip(&win, &msg->obj);
			break;
		case Z_WM_REDRAW:
			if (msg->obj.type != Z_UINT32) break;
			if (z_win_redraw_id(msg->obj.val.uint32) != win.id) break;
			z_win_apply_redraw(&win, msg->obj.val.uint32);
			z_listbox_invalidate(&kb_list);
			z_listbox_invalidate(&tz_list);
			repaint();
			z_win_redraw_done(&win);
			break;
		case Z_WM_WINDOW_MOVED:
			z_win_parse_rect(&win, &msg->obj);
			break;
		default:
			break;
	}

}

static void widgets_init(void) {

	static const struct { int idx; const char *label; } buttons[] = {
		{ W_GUIDE, "Read the guide" }, { W_SKIP, "Skip" }, { W_COURSE, "Start the course" },
		{ W_PASSWORD, "Set a password" }, { W_BACK, "Back" }, { W_NEXT, "Next" },
	};

	memset(widgets, 0, sizeof(widgets));

	for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
		widgets[buttons[i].idx].type = Z_WIDGET_BUTTON;
		widgets[buttons[i].idx].label = buttons[i].label;
		widgets[buttons[i].idx].enabled = true;
	}

	for (int i = 0; i < MODE_COUNT; i++) {
		widgets[W_MODE0 + i].type = Z_WIDGET_TOGGLE;
		widgets[W_MODE0 + i].group = GROUP_VIDEO;
		widgets[W_MODE0 + i].label = video_labels[i];
		widgets[W_MODE0 + i].enabled = true;
	}
	widgets[W_BACK].enabled = false;		// nothing before Welcome
	widgets[W_SKIP].enabled = has_card;		// with no card, Close is the same

	for (int s = 0; s < S_COUNT; s++)
		z_widget_set_init(&psets[s], &widgets[step_first[s]], step_first[s + 1] - step_first[s], &win);
	z_widget_set_init(&nav, &widgets[W_BACK], 2, &win);

	select_current_mode();

}

// The values, then the widgets, then the lists over them. One function,
// so tests/render.c starts exactly as the app does.
static void startup(void) {
	read_values();
	widgets_init();
	z_listbox_init(&kb_list, &win);
	z_listbox_set_items(&kb_list, KB_COUNT, kb_label, NULL);
	z_listbox_init(&tz_list, &win);
	z_listbox_set_items(&tz_list, TZ_ITEMS, tz_label, NULL);
	layout();
}

// Is there a card to keep answers on? A mounted volume, as the fs
// driver reports it -- not a card-detect line, which most boards do not
// wire and which says nothing about whether the card can be read.
static bool card_mounted(void) {
	uint32_t total = 0;
	return fs_df(&total, NULL) && total > 0;
}

// Whatever made wm start this is cleared now, before anything is
// asked: closing the window, Skip and a power cut must all count as
// having been asked (zwelcome.h).
static void take_first_run(void) {

	uint32_t len;
	char b[4];

	if (fs_size((char *)Z_PATH_SYS_FIRSTRUN) > 0) {
		first_run = true;
		if (!fs_unlink((char *)Z_PATH_SYS_FIRSTRUN))
			printf("welcome: could not remove " Z_PATH_SYS_FIRSTRUN " -- it will start again\n");
	}

	if (z_kv_get(Z_WELCOME_KV_SEEN, b, sizeof(b), &len) == Z_KV_E_NOENT)
		z_kv_set(Z_WELCOME_KV_SEEN, "1", 1);

}

int main(void) {

	printf("welcome: starting\n");

	can_set_video = z_video_mode_present();

	if (z_win_create_flags(&win, "welcome", WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER) != Z_OK) {
		printf("welcome: failed to create window -- is wm running?\n");
		return 1;
	}

	// No argument is used, but one left pending would be handed to
	// whatever opens next.
	{
		char arg[24];
		z_launch_arg_take(arg, sizeof(arg));
	}

	has_card = card_mounted();
	take_first_run();
	printf("welcome: %s%s\n", has_card ? "a card" : "no card: the welcome only",
		first_run ? " -- " Z_PATH_SYS_FIRSTRUN " removed" : "");

	startup();
	enter_step(S_WELCOME);

	while (!finished) {

		z_msg_t msg;

		while (!finished && z_msg_read(&msg) == Z_OK) {

			switch (msg.subject) {

				case Z_WM_KEY:
					if (msg.obj.type != Z_UINT32) break;
					if (!Z_WM_UNPACK_KEY_PRESSED(msg.obj.val.uint32)) break;
					handle_key(Z_WM_UNPACK_KEY_KEYSYM(msg.obj.val.uint32),
						(uint8_t)Z_WM_UNPACK_KEY_MODIFIERS(msg.obj.val.uint32));
					break;

				case Z_WM_MOUSE:
					if (msg.obj.type == Z_UINT32)
						handle_mouse(msg.obj.val.uint32);
					break;

				// The wheel scrolls the page's list: the view, not
				// the selection (docs/user_input.md).
				case Z_WM_WHEEL:
					if (msg.obj.type == Z_UINT32)
						handle_wheel(Z_WM_WHEEL_NOTCHES(msg.obj.val.uint32));
					break;

				default:
					on_wm_msg(&msg);
					break;

			}

		}

		// Nothing here polls anything, so block until something
		// arrives rather than spinning.
		if (!finished) z_proc_wait(0);

	}

	z_win_destroy(&win);
	return 0;

}
