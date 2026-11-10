/*
 * settings -- system preferences, and the editor for /sys/zeitlos.cfg
 *
 *   > run wm
 *   > run settings            or: run settings time   (a page by name)
 *
 * Categories down the left, each with an icon; the chosen one's settings
 * on the right (docs/settings_app.md):
 *
 *   Display      display colour (system.video.mode), desktop colours
 *                (system.video.desktop_theme)                applied at once
 *   Mouse        pointer speed (system.mouse.speed)           applied at once
 *   Keyboard     layouts (system.keyboard.layouts), Japanese text
 *                (system.font.japanese)                       the next key / at once
 *   Date & time  time zone (system.rtc.timezone)              clock, cal, date
 *   Security     password and screen lock                     the flash store
 *   Terminal     what new term windows connect to (apps.term.auto_connect)
 *   System       what the machine opens with (system.start); the file
 *                itself: what else is in it, Reload
 *
 * -- it edits the FILE, one line at a time --
 *
 * Every change reads /sys/zeitlos.cfg, replaces the one line for the key
 * being changed with z_cfg_text_set() (sw/common/zcfg.h), writes it
 * back and asks the kernel to reload. Every other line -- comments,
 * blank lines, keys this app has never heard of, even lines that are
 * not valid settings -- is copied back byte for byte. That is the whole
 * reason it does not keep its own model of the file and write that out:
 * a model only holds what the app understands, and writing it back is
 * how an editor clobbers everything else.
 *
 * -- keyboard-only --
 *
 * Fully usable with no pointer. The category list has focus at first:
 * Up and Down choose a page, Tab (or Right, or Enter) goes into it. In
 * a page, Tab and Shift+Tab move through its controls and out the far
 * end back to the list; Esc goes back at once; Enter or Space presses
 * a button. Ctrl+PgUp and Ctrl+PgDn change page from anywhere. A
 * settings app you can only reach with a mouse is exactly the wrong
 * thing to have on a machine whose pointer might be the thing you are
 * trying to fix.
 *
 * -- applying immediately --
 *
 * Choosing a display colour applies it there and then and saves it;
 * there is no OK button. For a setting whose entire effect is visible
 * the instant it changes, a confirmation step asks the user to commit
 * to something they can already see.
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
#include "../../common/zjfont.h"		// Z_JFONT_NAME
#include "../../common/zwidget.h"
#include "../../common/zdialog.h"
#include "../../common/zfsapp.h"
#include "../../common/zcfg.h"
#include "../../common/zrtc.h"		// the city table, z_tz_parse(), the clock
#include "../../common/zauth.h"		// the password and screen lock: docs/security.md
#include "../../common/zpaths.h"
#include "../../common/zaccent.h"	// the desktop colour themes
#include "../../common/zpointer.h"	// pointer speed

// Fixed size, not resizable: a preferences panel has no content to
// reveal.
#define WIN_W   420
#define WIN_H   300

static z_win_t win;
static z_dialog_ctx_t dlg;

#define MARGIN      8
#define LINE_H      (z_font_5x8.h + 2)
#define BTN_H       16
#define EDIT_W      44
#define EDIT_H      14
#define GAP         4	// at least 2px, for the focus ring
#define STEP_W      18	// the < and > buttons
#define PW_BTN_W    48

// -- the categories --

enum { P_DISPLAY, P_MOUSE, P_KEYBOARD, P_TIME, P_SECURITY, P_TERMINAL, P_SYSTEM, P_COUNT };

#define SB_W        104		// the category list
#define SB_ROW_H    24
#define PX          (SB_W + MARGIN)	// where a page begins

typedef struct {
	const char	*label;
	const char	*name;			// for `run settings NAME`
	const char	*note;			// under the page's title: when and where it applies
	const char	*art[16];		// the icon, '#' for ink
} page_t;

static const page_t pages[P_COUNT] = {
	{ "Display", "display", "Applied at once, and saved", {
		"................", ".##############.", ".#............#.", ".#.##########.#.",
		".#.#........#.#.", ".#.#........#.#.", ".#.#........#.#.", ".#.#........#.#.",
		".#.##########.#.", ".#............#.", ".##############.", ".......##.......",
		".......##.......", "....########....", "................", "................" } },
	{ "Mouse", "mouse", "Applied at once, and saved", {
		".......#........", ".......#........", ".....######.....", "....#...#..#....",
		"...#....#...#...", "...#....#...#...", "...#....#...#...", "...##########...",
		"...#........#...", "...#........#...", "...#........#...", "...#........#...",
		"...#........#...", "....#......#....", ".....######.....", "................" } },
	{ "Keyboard", "keyboard", "Saved; used from the next key", {
		"................", "................", "................", "################",
		"#..............#", "#.##.##.##.##..#", "#..............#", "#.##.##.##.##..#",
		"#..............#", "#.##.######.##.#", "#..............#", "################",
		"................", "................", "................", "................" } },
	{ "Date & time", "time", "Used by clock, cal and date", {
		".....######.....", "...##......##...", "..#....#.....#..", ".#.....#......#.",
		".#.....#......#.", "#......#.......#", "#......#.......#", "#......#####...#",
		"#..............#", "#..............#", ".#............#.", ".#............#.",
		"..#..........#..", "...##......##...", ".....######.....", "................" } },
	{ "Security", "security", "Kept in the flash, not in the file", {
		"................", ".....######.....", "....##....##....", "....#......#....",
		"....#......#....", "....#......#....", "..############..", "..############..",
		"..#####..#####..", "..####....####..", "..#####..#####..", "..######.#####..",
		"..######.#####..", "..############..", "..############..", "................" } },
	{ "Terminal", "terminal", "Used by new term windows", {
		"################", "#..............#", "################", "#..............#",
		"#.##...........#", "#..##..........#", "#...##.........#", "#..##..........#",
		"#.##...######..#", "#..............#", "#..............#", "################",
		"................", "................", "................", "................" } },
	{ "System", "system", "What it opens with, and the settings file", {
		"................", "...##########...", "...#.......#.#..", "...#.......#..#.",
		"...#.......####.", "...#..........#.", "...#.######...#.", "...#..........#.",
		"...#.######...#.", "...#..........#.", "...#.######...#.", "...#..........#.",
		"...#.####.....#.", "...#..........#.", "...############.", "................" } },
};

static uint8_t icons[P_COUNT][32];	// pages[].art, as 16x16 bitmaps

static int page = P_DISPLAY;
static bool sidebar_focus = true;	// keyboard focus: the category list, or the page

// -- controls --
//
// One array, in page order, and a widget set over each page's slice of
// it: a page's set draws and hit-tests only its own controls. A
// control's index here is its name everywhere else (activate()).

#define GROUP_VIDEO 1

static const uint32_t video_modes[] = {
	Z_VIDEO_MODE_WHITE, Z_VIDEO_MODE_AMBER, Z_VIDEO_MODE_GREEN, Z_VIDEO_MODE_PAPER,
};
static const char *video_labels[] = { "White", "Amber", "Green", "Paper" };
#define MODE_COUNT 4

enum {
	// Display: the four modes are 0..3
	W_THEME_PREV = MODE_COUNT,
	W_THEME_NEXT,
	// Mouse
	W_SPEED_PREV,
	W_SPEED_NEXT,
	// Keyboard
	W_EDIT_KBD,
	W_TOGGLE_JA,
	// Date & time (and the list, which is not a widget)
	W_SET_TZ,
	// Security
	W_PW_SET,
	W_PW_REMOVE,
	W_LOCK_EDIT,
	// Terminal
	W_EDIT_TERM,
	// System
	W_EDIT_START,
	W_RELOAD,
	W_COUNT
};

// Each page's first control; the next page's first ends it.
static const int page_first[P_COUNT + 1] = {
	0, W_SPEED_PREV, W_EDIT_KBD, W_SET_TZ, W_PW_SET, W_EDIT_TERM, W_EDIT_START, W_COUNT,
};

static z_widget_t widgets[W_COUNT];
static z_widget_set_t psets[P_COUNT];
static z_widget_set_t *ws = &psets[P_DISPLAY];	// the shown page's

// -- desktop colour: off, or one of zaccent.h's themes --
//
// One row, "< >" stepping through Off and the themes and saving each at
// once, as the display colours do: a colour scheme is judged by looking
// at it. Choosing one writes system.video.desktop_theme (the default,
// phosphor, as no line at all), turns desktop colour on, and removes any
// custom desktop_accents/desktop_base -- otherwise they would go on
// overriding what was chosen and the panel would seem to do nothing.
static bool can_theme;			// an eight-plane board with the colour window
static bool colour_on;			// system.video.desktop_colour
static int theme_idx;			// z_accent_themes, 0 for an unknown name
static bool theme_in_file, theme_custom;

// True if this bitstream can change the display mode. Probed once --
// see z_video_mode_present() in zsoc.h.
static bool can_set_video;

// -- what the preference rows show --

typedef struct {
	const char	*key;
	const char	*title;
	int			widget;
	char		value[Z_CFG_VAL_MAX];
	bool		in_file;
} pref_t;

static pref_t prefs[] = {
	{ "apps.term.auto_connect", "New terminal windows connect to", W_EDIT_TERM, "", false },
	{ "system.keyboard.layouts", "Keyboard layouts", W_EDIT_KBD, "", false },
	{ "system.font.japanese", "Japanese text", W_TOGGLE_JA, "", false },
	{ "system.start", "Opens at start", W_EDIT_START, "", false },
};

// The Japanese font switch -- defined with its button, further down.
static bool japanese_on(void);
static void set_japanese_label(void);
#define PREF_COUNT (int)(sizeof(prefs) / sizeof(prefs[0]))

static int other_keys;			// settings in the file this app does not show
static char status[64];
static bool status_is_hint;		// status holds the "selected -- Set" hint

// -- pointer speed (zpointer.h): "< >" through Z_POINTER_LEVELS, each
// about a fifth faster than the last, applied and saved at once, as the
// colours are: a speed is judged by moving the mouse. The kernel applies
// the saved value at boot (sw/os/cfg.c).
static bool can_speed;			// a HID port with the speed register (zpointer.h)
static int speed_idx = Z_POINTER_LEVEL_NORMAL;
static bool speed_in_file;

// where things are on the page shown (layout())
static int y_page, y_status, tz_rows;

// -- security: the password and the screen lock (docs/security.md) --
//
// Not in /sys/zeitlos.cfg: the kernel keeps them in the flash key/value
// store, so they hold with no card, and it changes them only when
// given the current password (Z_SYS_AUTH, zauth.h).
static z_auth_status_t auth;
static bool auth_ok;			// the kernel answered: it has Z_SYS_AUTH
static void read_auth(void);		// with read_values()

// -- the time zone list --
//
// Row 0 is UTC, then every city in z_tz_cities, then the whole-hour
// offsets UTC-12..UTC+14 for anywhere not listed. A fixed offset with
// minutes (UTC+5:30 set by hand in the file) has no row; the list then
// shows no selection and the line above it shows the value.

#define TZ_OFFSET_MIN   -12
#define TZ_OFFSET_MAX    14
#define TZ_OFFSETS      (TZ_OFFSET_MAX - TZ_OFFSET_MIN)	// excluding 0
#define TZ_ITEMS        (1 + z_tz_city_count + TZ_OFFSETS)

static z_listbox_t tz_list;
static bool list_focus;			// keyboard focus is on the list (Date & time)

static char tz_value[Z_CFG_VAL_MAX];
static bool tz_in_file;

// The whole-hour offset shown at row `i`, for i past the cities.
static int tz_row_hours(int i) {
	int h = TZ_OFFSET_MIN + (i - 1 - z_tz_city_count);
	return h >= 0 ? h + 1 : h;		// skip 0, which is row 0 ("UTC")
}

// What saving row `i` writes to the file.
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

// The row for a configured value, or -1.
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

static void zone_selection_changed(void);

// -- reading --

static void read_values(void) {

	char k[Z_CFG_KEY_MAX], v[Z_CFG_VAL_MAX];

	for (int i = 0; i < PREF_COUNT; i++)
		prefs[i].in_file = z_cfg_get(prefs[i].key, prefs[i].value,
			sizeof(prefs[i].value));

	tz_in_file = z_cfg_get("system.rtc.timezone", tz_value, sizeof(tz_value));
	if (!tz_in_file) snprintf(tz_value, sizeof(tz_value), "UTC");

	colour_on = z_cfg_get_bool("system.video.desktop_colour", true);
	theme_in_file = z_cfg_get("system.video.desktop_theme", v, sizeof(v));
	theme_idx = z_accent_theme_find(v);
	if (theme_idx < 0) theme_idx = 0;
	theme_custom = z_cfg_get("system.video.desktop_accents", v, sizeof(v)) ||
		z_cfg_get("system.video.desktop_base", v, sizeof(v));
	widgets[W_THEME_PREV].enabled = widgets[W_THEME_NEXT].enabled = can_theme;
	speed_in_file = z_cfg_get("system.mouse.speed", v, sizeof(v));
	speed_idx = z_pointer_level_of(z_cfg_get_int("system.mouse.speed", 16));
	widgets[W_SPEED_PREV].enabled = can_speed && speed_idx > 0;
	widgets[W_SPEED_NEXT].enabled = can_speed && speed_idx < Z_POINTER_LEVELS - 1;
	set_japanese_label();
	read_auth();
	z_listbox_select(&tz_list, tz_row_for(tz_value));
	zone_selection_changed();

	// Counted so the panel can say they exist and are being kept --
	// otherwise a person who put something else in the file has no way
	// to tell from here that this app will not throw it away.
	other_keys = 0;
	for (uint32_t i = 0; z_cfg_entry(i, k, sizeof(k), v, sizeof(v)); i++) {
		bool shown = !strcmp(k, "system.video.mode") ||
			!strcmp(k, "system.mouse.speed") ||
			!strcmp(k, "system.rtc.timezone") ||
			!strncmp(k, "system.video.desktop_", 21);
		for (int j = 0; j < PREF_COUNT; j++)
			if (!strcmp(k, prefs[j].key)) shown = true;
		if (!shown) other_keys++;
	}

}

static void read_auth(void) {
	auth_ok = z_auth_status(&auth) == Z_AUTH_OK;
	if (!auth_ok) memset(&auth, 0, sizeof(auth));
	bool w = auth_ok && (auth.flags & Z_AUTH_WRITABLE);
	bool has = (auth.flags & Z_AUTH_HAS_PASSWORD) != 0;
	widgets[W_PW_SET].label = has ? "Change" : "Set";
	widgets[W_PW_SET].enabled = w;
	widgets[W_PW_REMOVE].enabled = w && has;
	widgets[W_LOCK_EDIT].enabled = w;
}

static void select_current_mode(void) {

	uint32_t now = z_video_get_mode();

	for (int i = 0; i < MODE_COUNT; i++) {
		if (video_modes[i] == now) {
			z_widget_select(&psets[P_DISPLAY], i);
			break;
		}
	}

}

// -- layout --
//
// Every page's controls are placed at once; a page draws and hit-tests
// only its own (psets[]), so they may overlap from page to page.

static int page_w(void) { return z_win_content_w(&win) - PX - MARGIN; }
static int right_x(int w) { return PX + page_w() - w; }

// A setting's row: its title at y, its value a line below, its
// buttons at the right of the title line.
#define ROW_H       (2 * LINE_H + 10)

static void place(int idx, int x, int y, int w, int h) {
	widgets[idx].x = (int16_t)x;
	widgets[idx].y = (int16_t)y;
	widgets[idx].w = (int16_t)w;
	widgets[idx].h = (int16_t)h;
}

static void layout(void) {

	int cw = z_win_content_w(&win), ch = z_win_content_h(&win);
	int pw = cw - PX - MARGIN;

	(void)cw;
	y_page = MARGIN + z_font_6x12.h + LINE_H + 10;		// below the title and its note
	y_status = ch - LINE_H - 4;

	// Display: the four modes across, then the desktop colours row
	int bw = (pw - (MODE_COUNT - 1) * GAP) / MODE_COUNT;
	for (int i = 0; i < MODE_COUNT; i++)
		place(i, PX + i * (bw + GAP), y_page + LINE_H + 2, bw, BTN_H);
	int yt = y_page + LINE_H + 2 + BTN_H + 2 * LINE_H + 6;
	place(W_THEME_PREV, right_x(2 * STEP_W + GAP), yt - 3, STEP_W, EDIT_H);
	place(W_THEME_NEXT, right_x(STEP_W), yt - 3, STEP_W, EDIT_H);

	// Mouse
	place(W_SPEED_PREV, right_x(2 * STEP_W + GAP), y_page - 3, STEP_W, EDIT_H);
	place(W_SPEED_NEXT, right_x(STEP_W), y_page - 3, STEP_W, EDIT_H);

	// Keyboard: layouts, then Japanese text
	place(W_EDIT_KBD, right_x(EDIT_W), y_page - 3, EDIT_W, EDIT_H);
	place(W_TOGGLE_JA, right_x(EDIT_W + 12), y_page + ROW_H + LINE_H + 4 - 3, EDIT_W + 12, EDIT_H);

	// Date & time: the zone and the clock, the list, then Set
	int yl = y_page + 2 * LINE_H + 6;
	int ys = y_status - BTN_H - 6;
	tz_rows = (ys - 6 - yl - 2) / (z_font_5x8.h + 2);
	if (tz_rows < 3) tz_rows = 3;
	z_listbox_set_geom(&tz_list, PX, yl, pw, tz_rows * (z_font_5x8.h + 2) + 2);
	place(W_SET_TZ, right_x(110), ys, 110, BTN_H);

	// Security: the password, then the screen lock
	place(W_PW_SET, right_x(2 * PW_BTN_W + GAP), y_page - 3, PW_BTN_W, EDIT_H);
	place(W_PW_REMOVE, right_x(PW_BTN_W), y_page - 3, PW_BTN_W, EDIT_H);
	place(W_LOCK_EDIT, right_x(EDIT_W), y_page + ROW_H - 3, EDIT_W, EDIT_H);

	// Terminal
	place(W_EDIT_TERM, right_x(EDIT_W), y_page - 3, EDIT_W, EDIT_H);

	// System: what it opens with, then the file
	place(W_EDIT_START, right_x(EDIT_W), y_page - 3, EDIT_W, EDIT_H);
	place(W_RELOAD, PX, y_page + ROW_H + 3 * LINE_H + 8, 96, BTN_H);

	for (int p = 0; p < P_COUNT; p++) z_widget_invalidate(&psets[p]);
	z_listbox_invalidate(&tz_list);

}

// -- drawing --

// The value line under a preference: what is in effect, and where it
// came from.
static void describe(const pref_t *p, char *out, size_t n) {

	int max = (page_w() - 8) / z_font_5x8.w;
	const char *v = p->value;
	const char *suffix = p->in_file ? "" : "  (default)";

	if (!strcmp(p->key, "apps.term.auto_connect") &&
		(!v[0] || !strcmp(v, "none")))
		v = "the start panel";
	if (!strcmp(p->key, "system.keyboard.layouts") && !v[0])
		v = "us";
	if (!strcmp(p->key, "system.font.japanese"))
		v = japanese_on() ? "on -- jfont holds the font, about 190KB" : "off";
	if (!strcmp(p->key, "system.start") && (!v[0] || !strcmp(v, "none") || !strcmp(v, "desktop")))
		v = "the desktop";

	snprintf(out, n, "%.*s%s", max - (int)strlen(suffix), v, suffix);

}

static void draw_text(int x, int y, const char *s) {
	z_win_draw_text(&win, x, y, s, 1, &z_font_5x8);
}

// A title, and its value a line below, indented.
static void draw_row(int y, const char *title, const char *value) {
	draw_text(PX, y, title);
	if (value) draw_text(PX + 8, y + LINE_H, value);
}

static void describe_theme(char *out, size_t n) {
	if (!can_theme)
		snprintf(out, n, "needs eight colour planes (the 45F boards)");
	else if (!colour_on)
		snprintf(out, n, "off");
	else if (theme_custom)
		snprintf(out, n, "custom colours, from the file");
	else
		snprintf(out, n, "%s%s", z_accent_theme_names[theme_idx],
			theme_in_file ? "" : "  (default)");
}

static void describe_password(char *out, size_t n) {
	if (!auth_ok)
		snprintf(out, n, "unavailable: this kernel has no password support");
	else if (!(auth.flags & Z_AUTH_WRITABLE))
		snprintf(out, n, "read-only: this bitstream cannot write the flash");
	else if (!(auth.flags & Z_AUTH_HAS_PASSWORD))
		snprintf(out, n, "not set");
	else
		snprintf(out, n, "set%s", (auth.flags & Z_AUTH_NET_OK) ? "" :
			" -- too short for network use");
}

static void describe_lock(char *out, size_t n) {
	int k = 0;
	out[0] = 0;
	if (auth.lock_boot)
		k += snprintf(out + k, n - (size_t)k, "at start");
	if (auth.lock_idle_min && (size_t)k < n)
		k += snprintf(out + k, n - (size_t)k, "%safter %lu min idle", k ? ", " : "",
			(unsigned long)auth.lock_idle_min);
	if (auth.lock_console && (size_t)k < n)
		k += snprintf(out + k, n - (size_t)k, "%sconsole too", k ? ", " : "");
	if (!k) snprintf(out, n, "only on Super+L");
	else if (!(auth.flags & Z_AUTH_HAS_PASSWORD) && (size_t)k < n)
		snprintf(out + k, n - (size_t)k, " (needs a password)");
}

static void draw_status(void) {

	z_win_fill_rect(&win, PX, y_status, page_w(), LINE_H, 0);
	if (status[0]) draw_text(PX, y_status, status);

}

// One category: its icon and label, inverted when it is the page shown
// -- and tinted too, on a board with desktop colour, while the list has
// the keyboard; with the keyboard in the page, framed instead, so it is
// plain where Tab has gone.
static void draw_category(int p) {

	z_clip_t c;
	int y = MARGIN + p * SB_ROW_H, x0 = 4, w = SB_W - 8;
	bool sel = p == page, solid = sel && sidebar_focus;

	z_win_content_rect(&win, &c);
	z_win_fill_rect(&win, x0, y, w, SB_ROW_H - 2, solid ? 1 : 0);
	if (sel && !solid)
		z_win_hw_box(&win, c.x0 + x0, c.y0 + y, c.x0 + x0 + w - 1, c.y0 + y + SB_ROW_H - 3, 1);

	int ix = c.x0 + x0 + 4, iy = c.y0 + y + (SB_ROW_H - 2 - 16) / 2;
	for (int r = 0; r < 16; r++)
		for (int b = 0; b < 16; b++)
			if (icons[p][r * 2 + (b >> 3)] & (0x80u >> (b & 7)))
				z_fb_set_pixel(ix + b, iy + r, solid ? 0 : 1, &c);

	z_win_draw_text2(&win, x0 + 24, y + (SB_ROW_H - 2 - z_font_5x8.h) / 2, pages[p].label,
		solid ? 0 : 1, solid ? 1 : 0, &z_font_5x8);

	if (z_win_accent_ok())
		z_win_accent_rect(&win, c.x0 + x0, c.y0 + y, c.x0 + x0 + w - 1, c.y0 + y + SB_ROW_H - 3, solid);

}

static void draw_sidebar(void) {
	int ch = z_win_content_h(&win);
	for (int p = 0; p < P_COUNT; p++) draw_category(p);
	z_win_fill_rect(&win, SB_W, 0, 1, ch, 1);		// the line between
}

// The pointer speed as a scale: a box per level, filled up to the one
// in use, the normal one marked below.
static void draw_speed_scale(int y) {
	int bx = PX + 8, step = 9;
	z_clip_t c;
	z_win_content_rect(&win, &c);
	draw_text(bx, y, "slower");
	int sx = bx + 7 * z_font_5x8.w;
	for (int i = 0; i < Z_POINTER_LEVELS; i++) {
		int x = sx + i * step;
		z_win_fill_rect(&win, x, y, 7, 7, 0);
		z_win_hw_box(&win, c.x0 + x, c.y0 + y, c.x0 + x + 6, c.y0 + y + 6, 1);
		if (can_speed && i <= speed_idx) z_win_fill_rect(&win, x + 1, y + 1, 5, 5, 1);
		if (i == Z_POINTER_LEVEL_NORMAL) z_win_fill_rect(&win, x + 2, y + 9, 3, 3, 1);	// normal
	}
	draw_text(sx + Z_POINTER_LEVELS * step + 2, y, "faster");
}

static void draw_page(void) {

	char line[96];
	int y = y_page;

	z_win_draw_text(&win, PX, MARGIN, pages[page].label, 1, &z_font_6x12);
	draw_text(PX, MARGIN + z_font_6x12.h + 3, pages[page].note);

	switch (page) {

	case P_DISPLAY:
		draw_row(y, can_set_video ? "Display colour" : "Display colour (not on this bitstream)", NULL);
		y += LINE_H + 2 + BTN_H + 2 * LINE_H + 6;
		describe_theme(line, sizeof(line));
		draw_row(y, "Desktop colours (Super+F10)", line);
		break;

	case P_MOUSE:
		if (!can_speed)
			snprintf(line, sizeof(line), "fixed on this bitstream");
		else {
			// "Normal", or "+3 faster: x1.68": the level and its factor
			int d = speed_idx - Z_POINTER_LEVEL_NORMAL;
			int m = z_pointer_levels[speed_idx];
			int n = d ? snprintf(line, sizeof(line), "%+d %s: x%d.%02d", d, d > 0 ? "faster" : "slower",
					m / 16, (m % 16) * 100 / 16) : snprintf(line, sizeof(line), "Normal");
			if (!speed_in_file && n < (int)sizeof(line))
				snprintf(line + n, sizeof(line) - (size_t)n, "  (default)");
		}
		draw_row(y, "Pointer speed", line);
		draw_speed_scale(y + 2 * LINE_H + 6);
		break;

	case P_KEYBOARD:
		describe(&prefs[1], line, sizeof(line));
		draw_row(y, prefs[1].title, line);
		draw_text(PX + 8, y + 2 * LINE_H, "Super+Space changes between them");
		y += ROW_H + LINE_H + 4;
		describe(&prefs[2], line, sizeof(line));
		draw_row(y, prefs[2].title, line);
		draw_text(PX + 8, y + 2 * LINE_H, "drawn at 6x12; typed with romaji (Super+Space)");
		break;

	case P_TIME: {
		// What is in effect and the time it makes, on two lines: the
		// list below shows the choice, but a value with no row
		// (UTC+5:30 typed into the file) still has to be visible.
		snprintf(line, sizeof(line), "Time zone: %.32s%s", tz_value, tz_in_file ? "" : "  (default)");
		draw_text(PX, y, line);
		z_tz_t tz;
		bool dst = false;
		if (z_rtc_available() && z_rtc_valid()) {
			z_tm_t tm;
			z_tz_parse(tz_value, &tz);
			z_time_to_tm(z_tz_local(&tz, z_rtc_seconds(), &dst), &tm);
			snprintf(line, sizeof(line), "now %s %u %s %02u:%02u %s", z_wday_name(tm.wday),
				(unsigned)tm.day, z_month_name(tm.month), (unsigned)tm.hour, (unsigned)tm.min,
				z_tz_name(&tz, dst));
		} else snprintf(line, sizeof(line), "the clock is not set (the network sets it)");
		draw_text(PX + 8, y + LINE_H, line);
		break;
	}

	case P_SECURITY:
		describe_password(line, sizeof(line));
		draw_row(y, "Password", line);
		describe_lock(line, sizeof(line));
		draw_row(y + ROW_H, "Screen lock", line);
		y += 2 * ROW_H + 4;
		draw_text(PX, y, "The password opens the screen lock (Super+L),");
		draw_text(PX, y + LINE_H, "the console, and SSH and telnet logins.");
		break;

	case P_TERMINAL:
		describe(&prefs[0], line, sizeof(line));
		draw_row(y, prefs[0].title, line);
		y += ROW_H + 4;
		draw_text(PX, y, "port repl0, port posix0, telnet HOST,");
		draw_text(PX, y + LINE_H, "ssh USER@HOST, serial [BAUD], or none --");
		draw_text(PX, y + 2 * LINE_H, "the start panel lets you choose each time.");
		break;

	case P_SYSTEM:
		describe(&prefs[3], line, sizeof(line));
		draw_row(y, prefs[3].title, line);
		y += ROW_H;
		draw_row(y, "Settings file", Z_CFG_PATH);
		if (other_keys)
			snprintf(line, sizeof(line), "%d other setting%s in it, kept as %s", other_keys,
				other_keys == 1 ? "" : "s", other_keys == 1 ? "it is" : "they are");
		else snprintf(line, sizeof(line), "nothing else in it");
		draw_text(PX + 8, y + 2 * LINE_H, line);
		y = widgets[W_RELOAD].y + BTN_H + 8;
		draw_text(PX, y, "Reload reads the file again after it was");
		draw_text(PX, y + LINE_H, "edited elsewhere -- in text or vi.");
		break;
	}

	z_widget_draw_all(ws, true);
	if (page == P_TIME) z_listbox_draw(&tz_list, true);

}

static void repaint(void) {
	z_win_clear(&win);
	draw_sidebar();
	draw_page();
	draw_status();
}

// -- writing --

static void set_status(const char *s) {
	snprintf(status, sizeof(status), "%s", s);
	status_is_hint = false;
}

// Enables "Set time zone" exactly when the list's selection is a zone
// other than the one in use, and says so on the status line -- so the
// way to save is visible the moment there is something to save.
static void zone_selection_changed(void) {

	char v[Z_CFG_VAL_MAX];
	int row = z_listbox_selected(&tz_list);
	bool pending = false;

	if (row >= 0) {
		tz_row_value(row, v, sizeof(v));
		pending = strcmp(v, tz_value) != 0;
	}

	if (widgets[W_SET_TZ].enabled != pending) {
		widgets[W_SET_TZ].enabled = pending;
		widgets[W_SET_TZ].dirty = true;
	}

	if (pending) {
		snprintf(status, sizeof(status), "%.14s selected -- Set time zone to use it", v);
		status_is_hint = true;
	} else if (status_is_hint) {
		status[0] = 0;
		status_is_hint = false;
	}

	if (ws->count && page == P_TIME) {
		z_widget_draw_all(ws, false);
		draw_status();
	}

}

// The file before and after an edit.
//
// STATIC, and that is the fix for a real bug: these used to be
// malloc'd (8KB for the output, plus fs_mallocfile() for the input),
// and on the device save() reported "out of memory" and wrote nothing.
// An app's heap and stack share one 16KB allowance
// (the default tier, docs/executables.md), and _sbrk() refuses to
// grow the heap past the stack pointer -- so an allocation that is
// trivial on a build machine does not fit here. The host test could
// not see it; it now fails if save() allocates at all.
//
// 8KB of .bss is paid whether or not anything is ever saved, which is
// the right trade for an app whose job is saving.
static char cfg_in[Z_CFG_FILE_MAX + 1];
static char cfg_out[Z_CFG_FILE_MAX];

// Sets `key` to `val` in the file (NULL removes it), then reloads.
// Everything else in the file is left exactly as it was.
static bool save(const char *key, const char *val) {

	int insize = fs_read_file(Z_CFG_PATH, cfg_in, Z_CFG_FILE_MAX);
	char *out = cfg_out;
	bool ok = false;

	if (insize < 0) {
		set_status(fs_size(Z_CFG_PATH) > Z_CFG_FILE_MAX
			? "zeitlos.cfg is too large to edit here"
			: "could not read " Z_CFG_PATH);
		goto done;
	}

	int n = z_cfg_text_set(cfg_in, (size_t)insize, key, val, out,
		Z_CFG_FILE_MAX);

	if (n < 0) {
		set_status("that value cannot be saved");
		goto done;
	}

	// fs_write_file() reports bytes written, so an empty result could
	// not be told apart from a failed write. A file that has lost its
	// last setting keeps a line saying what it is instead.
	if (n == 0) {
		n = snprintf(out, Z_CFG_FILE_MAX, "# " Z_CFG_PATH " -- see docs/config.md\n");
	}

	if (fs_write_file(Z_CFG_PATH, out, n) != n) {
		set_status("could not write " Z_CFG_PATH " -- no sdcard?");
		goto done;
	}

	int loaded = z_cfg_reload(NULL);
	if (loaded < 0) set_status("saved, but this kernel cannot reload it");
	else set_status("saved and reloaded");
	ok = true;

done:
	read_values();
	return ok;

}

// Several keys in one write and one reload (NULL removes a key).
static bool save_many(const char *const *keys, const char *const *vals, int nkeys) {

	int insize = fs_read_file(Z_CFG_PATH, cfg_in, Z_CFG_FILE_MAX);
	bool ok = false;

	if (insize < 0) {
		set_status(fs_size(Z_CFG_PATH) > Z_CFG_FILE_MAX
			? "zeitlos.cfg is too large to edit here"
			: "could not read " Z_CFG_PATH);
		goto done;
	}

	// z_cfg_text_set() one key at a time, ping-ponging between the
	// two buffers; each step leaves every other line as it was
	int n = insize;
	for (int i = 0; i < nkeys; i++) {
		n = z_cfg_text_set(cfg_in, (size_t)n, keys[i], vals[i], cfg_out,
			Z_CFG_FILE_MAX);
		if (n < 0) {
			set_status("that value cannot be saved");
			goto done;
		}
		memcpy(cfg_in, cfg_out, (size_t)n);
	}
	if (n == 0)
		n = snprintf(cfg_in, Z_CFG_FILE_MAX, "# " Z_CFG_PATH " -- see docs/config.md\n");

	if (fs_write_file(Z_CFG_PATH, cfg_in, n) != n) {
		set_status("could not write " Z_CFG_PATH " -- no sdcard?");
		goto done;
	}

	if (z_cfg_reload(NULL) < 0) set_status("saved, but this kernel cannot reload it");
	else set_status("saved and reloaded");
	ok = true;

done:
	read_values();
	return ok;

}

// -- actions --

// One step through Off, then the themes, wrapping. wm picks the change
// up within a second (it rereads the file's generation).
static void step_theme(int dir) {

	int count = z_accent_theme_count + 1;		// Off first
	int pos = colour_on ? 1 + theme_idx : 0;
	static const char *const keys[4] = {
		"system.video.desktop_colour", "system.video.desktop_theme",
		"system.video.desktop_accents", "system.video.desktop_base",
	};
	const char *vals[4] = { NULL, NULL, NULL, NULL };

	if (!can_theme) return;

	pos = (pos + dir + count) % count;
	if (pos == 0) {
		// off keeps the theme for when it comes back on
		vals[0] = "no";
		save_many(keys, vals, 1);
	} else {
		// on is the default, phosphor the default theme: no lines
		vals[1] = pos == 1 ? NULL : z_accent_themes[pos - 1].id;
		save_many(keys, vals, 4);
	}
	repaint();

}

// One level slower or faster, applied at once and saved; no wrap (the
// ends are the ends). Normal (16) is saved as no line at all.
static void step_speed(int dir) {
	int i = speed_idx + dir;
	char v[4];
	if (!can_speed || i < 0 || i >= Z_POINTER_LEVELS) return;
	z_pointer_speed_set(z_pointer_levels[i]);
	snprintf(v, sizeof(v), "%d", z_pointer_levels[i]);
	save("system.mouse.speed", i == Z_POINTER_LEVEL_NORMAL ? NULL : v);
	repaint();
}

static void apply_video(int idx) {

	if (idx < 0 || idx >= MODE_COUNT) return;

	if (!can_set_video) {
		// No register in this bitstream (an RTL change: `make flash`,
		// not `make dev-flash`). Put the selection back where the
		// display actually is.
		select_current_mode();
		z_widget_draw_all(ws, false);
		return;
	}

	z_video_set_mode(video_modes[idx]);

	// Read back rather than trusting the write -- the group must show
	// what the display is doing, not what was asked for.
	select_current_mode();

	if (z_video_get_mode() == video_modes[idx])
		save("system.video.mode", z_video_mode_name(video_modes[idx]));

	repaint();

}

static bool valid_auto_connect(const char *v) {
	static const char *words[] = { "port ", "serial", "telnet ", "ssh ", "none" };
	for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++)
		if (!strncmp(v, words[i], strlen(words[i]))) return true;
	return false;
}

// True if every name in a comma- or space-separated list is a layout
// zkbd knows, and there is at least one. `bad` gets the first one that
// is not.
static bool valid_layouts(const char *v, char *bad, size_t badlen) {

	char name[24];
	int n = 0;

	while (*v) {
		while (*v == ',' || *v == ' ') v++;
		size_t k = 0;
		while (*v && *v != ',' && *v != ' ') {
			if (k + 1 < sizeof(name)) name[k++] = *v;
			v++;
		}
		name[k] = 0;
		if (!k) break;
		if (z_kbd_layout_find(name) < 0) {
			snprintf(bad, badlen, "%s", name);
			return false;
		}
		n++;
	}

	if (!n) snprintf(bad, badlen, "(none)");
	return n > 0;

}

// -- the Japanese font -- docs/text_encoding.md, "Japanese" --
//
// Not a prompt but a switch, and it acts at once: turning it on saves
// the setting AND starts jfont, so Japanese draws in the next repaint;
// turning it off saves it and stops jfont, giving its ~190KB back. wm
// reads the setting only at boot, so without starting and stopping it
// here the change would wait for a reboot.

static bool japanese_on(void) {
	const char *v = prefs[2].value;
	return !strcmp(v, "yes") || !strcmp(v, "on") || !strcmp(v, "true") || !strcmp(v, "1");
}

static void set_japanese_label(void) {
	widgets[W_TOGGLE_JA].label = japanese_on() ? "Turn off" : "Turn on";
}

static void toggle_japanese(void) {

	bool on = !japanese_on();
	if (!save("system.font.japanese", on ? "yes" : "no")) { repaint(); return; }

	if (on) {
		if (!z_proc_run("jfont"))
			set_status("saved; could not start jfont -- is " Z_DIR_APPS "/jfont on the card?");
	} else {
		uint32_t pid;
		if (z_pid_lookup(Z_JFONT_NAME, &pid)) z_proc_kill(pid);
	}

	set_japanese_label();
	repaint();

}

static void edit_layouts(const pref_t *p) {

	// The names, wrapped for the dialog, from zkbd's own table -- so a
	// layout added there shows up here with no change to this app.
	char msg[200];
	size_t o = 0, col = 0;
	o += (size_t)snprintf(msg + o, sizeof(msg) - o, "First is used at start:\n");
	for (int i = 0; i < z_kbd_layout_count && o < sizeof(msg) - 20; i++) {
		size_t len = strlen(z_kbd_layouts[i].name);
		if (col + len + 1 > 38) { msg[o++] = '\n'; col = 0; }
		o += (size_t)snprintf(msg + o, sizeof(msg) - o, "%s%s",
			z_kbd_layouts[i].name, i + 1 < z_kbd_layout_count ? " " : "");
		col += len + 1;
	}

	char v[Z_CFG_VAL_MAX], bad[24];
	if (!z_dialog_prompt(&dlg, "Keyboard layouts", msg, p->value, v, sizeof(v)))
		return;

	if (!strcmp(v, "default")) {
		save(p->key, NULL);
	} else if (!valid_layouts(v, bad, sizeof(bad))) {
		char m[64];
		snprintf(m, sizeof(m), "Unknown layout: %.20s\nnothing saved.", bad);
		z_dialog_confirm(&dlg, "Keyboard layouts", m, Z_DIALOG_OK_CANCEL);
		return;
	} else {
		save(p->key, v);
	}

	repaint();

}

// system.start (docs/config.md): an app's name, then optionally its
// argument -- "text /home/book.txt". wm starts it when boot has
// finished. 'default' (or 'desktop') removes the line. Only the name is
// checked, for being one: whether it is on this card is a question for
// the next boot, and the console says so then.
static bool valid_start(const char *v) {
	int n = 0;
	for (; *v && *v != ' '; v++, n++)
		if (!((*v >= 'a' && *v <= 'z') || (*v >= 'A' && *v <= 'Z') ||
			(*v >= '0' && *v <= '9') || *v == '_' || *v == '-')) return false;
	return n > 0;
}

static void edit_start(const pref_t *p) {

	char v[Z_CFG_VAL_MAX];

	if (!z_dialog_prompt(&dlg, p->title,
		"An app, and what to open in it:\n"
		"text, text " Z_DIR_HOME "/book.txt, learn, term\n"
		"'default' removes the line (the desktop)", p->value, v, sizeof(v)))
		return;

	if (!strcmp(v, "default") || !strcmp(v, "desktop")) save(p->key, NULL);
	else if (!valid_start(v)) {
		z_dialog_confirm(&dlg, p->title, "An app's name first, as in\n"
			"text " Z_DIR_HOME "/book.txt -- nothing saved.", Z_DIALOG_OK_CANCEL);
		return;
	} else save(p->key, v);

	repaint();

}

static void edit(const pref_t *p) {

	char v[Z_CFG_VAL_MAX];
	const char *msg;

	if (!strcmp(p->key, "system.keyboard.layouts")) { edit_layouts(p); return; }
	if (!strcmp(p->key, "system.start")) { edit_start(p); return; }

	msg = "port repl0, port posix0, telnet <host>,\n"
	      "ssh <user@host>, serial [baud], or none\n"
	      "'default' removes the line (start panel)";

	// z_dialog_prompt() returns false for an EMPTY field as well as for
	// Cancel, so "clear this setting" needs a word of its own -- hence
	// 'default'.
	if (!z_dialog_prompt(&dlg, p->title, msg, p->value, v, sizeof(v)))
		return;

	if (!strcmp(v, "default")) {
		save(p->key, NULL);
	} else {
		if (!valid_auto_connect(v)) {
			z_dialog_confirm(&dlg, p->title,
				"Start with port, serial, telnet, ssh\n"
				"or none -- nothing saved.", Z_DIALOG_OK_CANCEL);
			return;
		}
		save(p->key, v);
	}

	repaint();

}

static void reload(void) {

	uint32_t ignored = 0;
	int n = z_cfg_reload(&ignored);
	char s[64];

	if (n < 0)
		snprintf(s, sizeof(s), "this kernel has no config support");
	else if (n == 0)
		snprintf(s, sizeof(s), "no settings in " Z_CFG_PATH " -- defaults");
	else
		snprintf(s, sizeof(s), "reloaded: %d setting%s%s", n, n == 1 ? "" : "s",
			ignored ? ", some lines ignored" : "");

	set_status(s);
	read_values();
	select_current_mode();
	repaint();

}

static void choose_zone_and_return(void);

// -- security actions --

static void wipe(char *p, size_t n) {
	volatile char *v = p;
	while (n--) *v++ = 0;
}

static void auth_result(int rc, uint32_t w, const char *ok) {
	char m[64];
	switch (rc) {
	case Z_AUTH_OK:        set_status(ok); return;
	case Z_AUTH_E_BAD:     set_status("wrong password -- nothing changed"); return;
	case Z_AUTH_E_WAIT:
		snprintf(m, sizeof(m), "too many attempts -- wait %lu s", (unsigned long)((w + 999) / 1000));
		set_status(m);
		return;
	case Z_AUTH_E_STORE:   set_status("the flash store refused (kv at the console)"); return;
	case Z_AUTH_E_NOSYS:   set_status("this kernel has no password support"); return;
	default:               set_status("not accepted -- nothing changed"); return;
	}
}

// The current password, if there is one: false if the user cancelled.
static bool ask_current(const char *title, char *cur, size_t n) {
	cur[0] = 0;
	if (!(auth.flags & Z_AUTH_HAS_PASSWORD)) return true;
	return z_dialog_prompt_secret(&dlg, title, "Current password:", cur, (int)n);
}

static void set_password(void) {
	char cur[Z_AUTH_PW_MAX + 1], n1[Z_AUTH_PW_MAX + 1], n2[Z_AUTH_PW_MAX + 1];
	const char *title = (auth.flags & Z_AUTH_HAS_PASSWORD) ? "Change password" : "Set password";
	uint32_t w = 0;
	int rc;

	n1[0] = n2[0] = 0;
	if (!ask_current(title, cur, sizeof(cur))) goto out;
	if (!z_dialog_prompt_secret(&dlg, title,
			"New password -- for the screen lock and\nthe console; 10 characters or more\n"
			"for network logins.", n1, sizeof(n1)))
		goto out;
	if (!z_dialog_prompt_secret(&dlg, title, "The new password again:", n2, sizeof(n2)))
		goto out;
	if (strcmp(n1, n2)) {
		set_status("the two differ -- nothing changed");
		goto out;
	}
	set_status("working...");
	draw_status();
	rc = z_auth_set(cur, (uint32_t)strlen(cur), n1, (uint32_t)strlen(n1), &w);
	auth_result(rc, w, strlen(n1) < Z_AUTH_NET_MIN ?
		"password set -- too short for network logins" : "password set");
	if (rc == Z_AUTH_OK) z_wm_lock_request(Z_WM_LOCK_RELOAD);
out:
	wipe(cur, sizeof(cur));
	wipe(n1, sizeof(n1));
	wipe(n2, sizeof(n2));
	read_auth();
	repaint();
}

static void remove_password(void) {
	char cur[Z_AUTH_PW_MAX + 1];
	uint32_t w = 0;
	int rc;

	if (z_dialog_confirm(&dlg, "Remove password",
			"Remove the password? The screen lock\nwill then open with Enter.",
			Z_DIALOG_YES_NO) != Z_DIALOG_YES)
		return;
	if (!ask_current("Remove password", cur, sizeof(cur))) goto out;
	set_status("working...");
	draw_status();
	rc = z_auth_set(cur, (uint32_t)strlen(cur), NULL, 0, &w);
	auth_result(rc, w, "password removed");
	if (rc == Z_AUTH_OK) z_wm_lock_request(Z_WM_LOCK_RELOAD);
out:
	wipe(cur, sizeof(cur));
	read_auth();
	repaint();
}

static void edit_lock(void) {
	z_auth_status_t p = auth;
	char mins[8], init[12], cur[Z_AUTH_PW_MAX + 1];
	uint32_t w = 0, v = 0;
	int r, rc, i;

	cur[0] = 0;
	r = z_dialog_confirm(&dlg, "Screen lock", "Lock the screen when the machine starts?",
		Z_DIALOG_YES_NO_CANCEL);
	if (r == Z_DIALOG_CANCEL) return;
	p.lock_boot = (r == Z_DIALOG_YES);

	snprintf(init, sizeof(init), "%lu", (unsigned long)p.lock_idle_min);
	if (!z_dialog_prompt(&dlg, "Screen lock",
			"Lock after how many minutes without\ninput? 0 for never, 1440 at most.",
			init, mins, sizeof(mins)))
		return;
	for (i = 0; mins[i]; i++) {
		if (mins[i] < '0' || mins[i] > '9' || v > 1440) { v = 9999; break; }
		v = v * 10 + (uint32_t)(mins[i] - '0');
	}
	if (v > 1440) {
		set_status("minutes: a number from 0 to 1440");
		repaint();
		return;
	}
	p.lock_idle_min = v;

	r = z_dialog_confirm(&dlg, "Screen lock",
		"Ask for the password on the serial\nconsole too?", Z_DIALOG_YES_NO_CANCEL);
	if (r == Z_DIALOG_CANCEL) return;
	p.lock_console = (r == Z_DIALOG_YES);

	if (!ask_current("Screen lock", cur, sizeof(cur))) goto out;
	rc = z_auth_policy(cur, (uint32_t)strlen(cur), &p, &w);
	auth_result(rc, w, "screen lock saved");
	if (rc == Z_AUTH_OK) z_wm_lock_request(Z_WM_LOCK_RELOAD);
out:
	wipe(cur, sizeof(cur));
	read_auth();
	repaint();
}

static void activate(int idx) {
	if (idx < 0) return;
	if (idx < MODE_COUNT) { apply_video(idx); return; }
	if (idx == W_THEME_PREV) { step_theme(-1); return; }
	if (idx == W_THEME_NEXT) { step_theme(1); return; }
	if (idx == W_SPEED_PREV) { step_speed(-1); return; }
	if (idx == W_SPEED_NEXT) { step_speed(1); return; }
	if (idx == W_EDIT_TERM) { edit(&prefs[0]); return; }
	if (idx == W_EDIT_KBD) { edit(&prefs[1]); return; }
	if (idx == W_TOGGLE_JA) { toggle_japanese(); return; }
	if (idx == W_SET_TZ) { choose_zone_and_return(); return; }
	if (idx == W_RELOAD) { reload(); return; }
	if (idx == W_EDIT_START) { edit(&prefs[3]); return; }
	if (idx == W_PW_SET) { set_password(); return; }
	if (idx == W_PW_REMOVE) { remove_password(); return; }
	if (idx == W_LOCK_EDIT) { edit_lock(); return; }
}

// A page's control as activate() names it, from its set's index.
static int global_of(int local) {
	return local < 0 ? -1 : page_first[page] + local;
}

// Saves the time zone at the list's selection.
static void choose_zone(void) {

	char v[Z_CFG_VAL_MAX];
	int row = z_listbox_selected(&tz_list);

	if (row < 0) return;

	tz_row_value(row, v, sizeof(v));
	save("system.rtc.timezone", v);
	repaint();

}

// Moves keyboard focus onto or off the list.
static void focus_list(bool on) {
	list_focus = on;
	z_listbox_set_focus(&tz_list, on);
	if (on) z_widget_focus_set(ws, -1);
	if (page == P_TIME) {
		z_widget_draw_all(ws, false);
		z_listbox_draw(&tz_list, false);
	}
}

// The Set button, from a click or Enter on it. It disables itself once
// the zone is saved, so focus goes back to the list -- where a keyboard
// user was, and the one control near it still worth pressing.
static void choose_zone_and_return(void) {
	choose_zone();
	focus_list(true);
}

// -- pages and focus --

// Shows page p. The keyboard stays where it was: on the list of
// categories, or -- from Ctrl+PgUp/PgDn in a page -- at the new page's
// first control.
static void show_page(int p) {
	if (p < 0 || p >= P_COUNT || p == page) return;
	focus_list(false);
	z_widget_focus_set(ws, -1);
	page = p;
	ws = &psets[page];
	if (status_is_hint && page != P_TIME) { status[0] = 0; status_is_hint = false; }
	else if (!status_is_hint) status[0] = 0;
	repaint();
}

// The places Tab stops in the page shown, in order: on Date & time the
// list first, then every enabled control. STOP_LIST is the list.
#define STOP_LIST   (-2)
#define STOP_NONE   (-1)

static int page_stops(int *out) {
	int n = 0;
	if (page == P_TIME) out[n++] = STOP_LIST;
	for (int g = page_first[page]; g < page_first[page + 1]; g++)
		if (widgets[g].enabled) out[n++] = g;
	return n;
}

static int current_stop(void) {
	if (list_focus) return STOP_LIST;
	return global_of(ws->focused);
}

static void focus_stop(int s) {
	if (s == STOP_LIST) { focus_list(true); return; }
	focus_list(false);
	z_widget_focus_set(ws, s - page_first[page]);
	z_widget_draw_all(ws, false);
}

static void focus_sidebar(bool on) {
	if (sidebar_focus == on) return;
	sidebar_focus = on;
	if (on) {
		focus_list(false);
		z_widget_focus_set(ws, -1);
		z_widget_draw_all(ws, false);
	}
	draw_category(page);
}

// Into the page: its first stop (or its last, coming back with
// Shift+Tab). A page with nothing to press keeps the keyboard on the
// list.
static void enter_page(bool from_end) {
	int st[W_COUNT + 1], n = page_stops(st);
	if (!n) return;
	focus_sidebar(false);
	focus_stop(st[from_end ? n - 1 : 0]);
}

// Tab, Shift+Tab: the next stop in the page, and out of either end to
// the list of categories.
static void step_focus(bool back) {
	int st[W_COUNT + 1], n = page_stops(st), cur = current_stop(), i;
	for (i = 0; i < n && st[i] != cur; i++) ;
	int next = i == n ? (back ? n - 1 : 0) : i + (back ? -1 : 1);
	if (next < 0 || next >= n) { focus_sidebar(true); return; }
	focus_stop(st[next]);
}

// -- input --

static void handle_mouse(uint32_t packed) {

	int cx, cy;
	bool inside = z_win_mouse_content_xy(&win, packed, &cx, &cy);
	uint8_t buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);
	static uint8_t last;
	bool down = (buttons & Z_MOUSE_BTN_LEFT) && !(last & Z_MOUSE_BTN_LEFT);
	last = buttons;

	// Samples over the titlebar reach us too. Same guard as text/term.
	if (!inside && ws->pressed < 0 && !tz_list.sb.dragging) return;

	// A category: that page, the keyboard on the list.
	if (down && cx < SB_W && ws->pressed < 0 && !tz_list.sb.dragging) {
		int p = (cy - MARGIN) / SB_ROW_H;
		if (cy >= MARGIN && p >= 0 && p < P_COUNT) {
			focus_sidebar(true);
			show_page(p);
		}
		return;
	}

	// The list first, and for the whole of a scrollbar drag it started.
	if (page == P_TIME) {
		if (ws->pressed < 0 && z_listbox_has_pointer(&tz_list, cx, cy)) {
			if ((buttons & Z_MOUSE_BTN_LEFT) && !list_focus) {
				focus_sidebar(false);
				focus_list(true);
			}
			int r = z_listbox_mouse(&tz_list, cx, cy, buttons);
			if (r == Z_LIST_ACTIVATED) choose_zone();
			else if (r == Z_LIST_SELECTED) zone_selection_changed();
			return;
		}
		z_listbox_mouse(&tz_list, cx, cy, buttons);		// keep its edges fed
	}

	int act = z_widget_mouse(ws, cx, cy, buttons);
	if (act >= 0) {
		if (list_focus) focus_list(false);
		if (sidebar_focus) focus_sidebar(false);
	}
	activate(global_of(act));

}

static void handle_key(uint32_t keysym, uint8_t mods) {

	if (keysym == Z_KEY_HELP) {	// docs/app_help.md
		static const char help[] =
			"System preferences, saved to " Z_PATH_SYS_CONFIG ".\n\n"
			"Up/Down\tChoose a category\n"
			"Tab, Enter\tInto its page (Shift+Tab back)\n"
			"Esc\tBack to the categories\n"
			"Ctrl+PgUp/Dn\tThe category before or after\n"
			"Enter/Space\tPress the focused button\n"
			"A-Z, 0-9\tDate & time: jump to a zone\n\n"
			"Display colours, desktop colour themes and the pointer "
			"speed (< >) apply at once. Pick a time zone, then "
			"Set time zone. System reloads the file after it was "
			"edited elsewhere.";
		z_dialog_help(&dlg, "Settings help", help, "settings_app");
		return;
	}

	bool back = (mods & Z_KBD_MOD_SHIFT) != 0;

	// Ctrl+PgUp/PgDn: the page before or after, from anywhere.
	if ((mods & Z_KBD_MOD_CTRL) && (keysym == Z_KEY_PAGEUP || keysym == Z_KEY_PAGEDOWN)) {
		int p = page + (keysym == Z_KEY_PAGEUP ? -1 : 1);
		bool was = sidebar_focus;
		show_page(p);
		if (!was) enter_page(false);
		return;
	}

	// -- the list of categories --
	if (sidebar_focus) {
		switch (keysym) {
		case Z_KEY_UP:   show_page(page - 1); return;
		case Z_KEY_DOWN: show_page(page + 1); return;
		case Z_KEY_HOME: show_page(0); return;
		case Z_KEY_END:  show_page(P_COUNT - 1); return;
		case '\t': enter_page(back); return;
		case Z_KEY_RIGHT: case 0x0d: case ' ': enter_page(false); return;
		}
		// a letter on Date & time finds a zone, as it does in its page
		if (page == P_TIME && ((keysym >= 'a' && keysym <= 'z') ||
				(keysym >= 'A' && keysym <= 'Z') || (keysym >= '0' && keysym <= '9'))) {
			focus_sidebar(false);
			focus_list(true);
			if (z_listbox_key(&tz_list, keysym) == Z_LIST_SELECTED) zone_selection_changed();
		}
		return;
	}

	if (keysym == 0x1b) { focus_sidebar(true); return; }

	// -- the list of zones has focus --
	//
	// Every key it uses goes to it -- arrows move the selection there,
	// not the focus -- and Tab leaves it.
	if (list_focus) {
		if (keysym == '\t') { step_focus(back); return; }
		int r = z_listbox_key(&tz_list, keysym);
		if (r == Z_LIST_ACTIVATED) choose_zone();
		else if (r == Z_LIST_SELECTED) zone_selection_changed();
		return;
	}

	// A letter or digit on Date & time belongs to the list wherever
	// focus is: "press B, get Bangkok" should not first require finding
	// the list with Tab.
	if (page == P_TIME && ((keysym >= 'a' && keysym <= 'z') || (keysym >= 'A' && keysym <= 'Z') ||
		(keysym >= '0' && keysym <= '9'))) {
		focus_list(true);
		if (z_listbox_key(&tz_list, keysym) == Z_LIST_SELECTED)
			zone_selection_changed();
		return;
	}

	switch (keysym) {

		case '\t':
			step_focus(back);
			return;

		case Z_KEY_UP:
		case Z_KEY_LEFT: {
			// Left from the first control is back to the categories
			int st[W_COUNT + 1], n = page_stops(st);
			if (keysym == Z_KEY_LEFT && n && current_stop() == st[0]) { focus_sidebar(true); return; }
			step_focus(true);
			if (sidebar_focus && keysym == Z_KEY_UP) enter_page(false);	// Up stops at the top
			return;
		}

		case Z_KEY_DOWN:
		case Z_KEY_RIGHT: {
			int st[W_COUNT + 1], n = page_stops(st);
			if (n && current_stop() == st[n - 1]) return;		// stops at the end
			step_focus(false);
			return;
		}

		case 0x0d:
		case ' ':
			activate(global_of(z_widget_key_activate(ws)));
			return;

		default:
			return;

	}

}

// Messages that arrive while a dialog is up and are not the dialog's
// own. The redraw ack is not optional -- see zdialog.h.
static void on_dialog_msg(z_msg_t *msg, void *user) {

	(void)user;

	switch (msg->subject) {
		case Z_WM_SET_CLIP:
			z_win_apply_clip(&win, &msg->obj);
			break;
		case Z_WM_REDRAW:
			if (msg->obj.type != Z_UINT32) break;
			if (z_win_redraw_id(msg->obj.val.uint32) != win.id) break;
			z_win_apply_redraw(&win, msg->obj.val.uint32);
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

	memset(widgets, 0, sizeof(widgets));

	for (int i = 0; i < MODE_COUNT; i++) {
		widgets[i].type = Z_WIDGET_TOGGLE;
		widgets[i].group = GROUP_VIDEO;
		widgets[i].label = video_labels[i];
		widgets[i].enabled = true;
	}

	widgets[W_THEME_PREV].type = Z_WIDGET_BUTTON;
	widgets[W_THEME_PREV].label = "<";
	widgets[W_THEME_PREV].enabled = can_theme;
	widgets[W_THEME_NEXT].type = Z_WIDGET_BUTTON;
	widgets[W_THEME_NEXT].label = ">";
	widgets[W_THEME_NEXT].enabled = can_theme;

	widgets[W_SPEED_PREV].type = Z_WIDGET_BUTTON;
	widgets[W_SPEED_PREV].label = "<";
	widgets[W_SPEED_NEXT].type = Z_WIDGET_BUTTON;
	widgets[W_SPEED_NEXT].label = ">";
	widgets[W_SPEED_PREV].enabled = can_speed && speed_idx > 0;
	widgets[W_SPEED_NEXT].enabled = can_speed && speed_idx < Z_POINTER_LEVELS - 1;

	widgets[W_EDIT_TERM].type = Z_WIDGET_BUTTON;
	widgets[W_EDIT_TERM].label = "Edit";
	widgets[W_EDIT_TERM].enabled = true;

	widgets[W_EDIT_KBD].type = Z_WIDGET_BUTTON;
	widgets[W_EDIT_KBD].label = "Edit";
	widgets[W_EDIT_KBD].enabled = true;

	widgets[W_TOGGLE_JA].type = Z_WIDGET_BUTTON;
	widgets[W_TOGGLE_JA].label = "Turn on";	// set_japanese_label() after reading
	widgets[W_TOGGLE_JA].enabled = true;

	widgets[W_SET_TZ].type = Z_WIDGET_BUTTON;
	widgets[W_SET_TZ].label = "Set time zone";
	widgets[W_SET_TZ].enabled = false;		// see zone_selection_changed()

	widgets[W_EDIT_START].type = Z_WIDGET_BUTTON;
	widgets[W_EDIT_START].label = "Edit";
	widgets[W_EDIT_START].enabled = true;

	widgets[W_RELOAD].type = Z_WIDGET_BUTTON;
	widgets[W_RELOAD].label = "Reload file";
	widgets[W_RELOAD].enabled = true;

	widgets[W_PW_SET].type = Z_WIDGET_BUTTON;
	widgets[W_PW_SET].label = "Set";		// read_auth() after reading
	widgets[W_PW_REMOVE].type = Z_WIDGET_BUTTON;
	widgets[W_PW_REMOVE].label = "Remove";
	widgets[W_LOCK_EDIT].type = Z_WIDGET_BUTTON;
	widgets[W_LOCK_EDIT].label = "Edit";

	for (int p = 0; p < P_COUNT; p++)
		z_widget_set_init(&psets[p], &widgets[page_first[p]], page_first[p + 1] - page_first[p], &win);
	ws = &psets[page];

	select_current_mode();

}

// The icons, from their pictures in pages[].
static void icons_init(void) {
	memset(icons, 0, sizeof(icons));
	for (int p = 0; p < P_COUNT; p++)
		for (int r = 0; r < 16; r++)
			for (int b = 0; b < 16; b++)
				if (pages[p].art[r][b] == '#') icons[p][r * 2 + (b >> 3)] |= (uint8_t)(0x80u >> (b & 7));
}

// The values, then the widgets, then the values the widgets carry.
// One function, so tests/render.c starts up exactly as the app does:
// the Security buttons once came up disabled -- drawn as empty boxes --
// because widgets_init() zeroed what read_values() had set, and a test
// that re-read the values later could not see it.
static void startup(void) {
	icons_init();
	read_values();
	widgets_init();
	read_auth();
}

// `run settings time`: the page whose name the argument begins.
static int page_named(const char *a) {
	size_t n = strlen(a);
	if (!n) return -1;
	for (int p = 0; p < P_COUNT; p++)
		if (!strncmp(pages[p].name, a, n)) return p;
	return -1;
}

int main(void) {

	printf("settings: starting\n");

	can_set_video = z_video_mode_present();
	can_theme = z_color_desktop_available();
	can_speed = z_pointer_speed_available();

	if (!can_set_video)
		printf("settings: this bitstream has no video mode register "
			"-- needs `make flash`, not `make dev-flash`\n");

	if (z_win_create_flags(&win, "settings", WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER) != Z_OK) {
		printf("settings: failed to create window -- is wm running?\n");
		return 1;
	}

	// The page to open on, if one was named. Taken even when empty:
	// leaving one pending would hand it to whatever opens next.
	{
		char arg[24];
		if (z_launch_arg_take(arg, sizeof(arg)) && page_named(arg) >= 0)
			page = page_named(arg);
	}

	dlg.parent = &win;
	dlg.on_msg = on_dialog_msg;
	dlg.user = NULL;

	z_listbox_init(&tz_list, &win);
	z_listbox_set_items(&tz_list, TZ_ITEMS, tz_label, NULL);

	startup();
	layout();
	repaint();

	for (;;) {

		z_msg_t msg;

		while (z_msg_read(&msg) == Z_OK) {

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

				default:
					on_dialog_msg(&msg, NULL);
					break;

			}

		}

		// Nothing here polls anything, so block until something
		// arrives rather than spinning.
		z_proc_wait(0);

	}

	return 0;

}
