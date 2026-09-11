/*
 * settings -- system preferences, and the editor for /zeitlos.cfg
 *
 *   > run wm
 *   > run settings
 *
 * Three settings (docs/config.md):
 *
 *   Display               system.video.mode        applied immediately
 *   Terminal connects to  apps.term.auto_connect   new term windows
 *   Time zone             system.rtc.timezone      clock and cal
 *
 * The time zone is chosen from a list -- UTC, about sixty cities with
 * their daylight-saving rules (z_tz_cities, zrtc.h), and whole-hour
 * offsets -- in a z_listbox_t (zwidget.h). Moving through the list only
 * selects, so browsing does not write the sdcard once per row. The
 * "Set time zone" button saves the selection; it is enabled only while
 * the selection differs from the zone in use, and the status line says
 * so. Enter and double-click save too.
 *
 * The button was added after a person selected a city, saw nothing
 * happen, and reasonably concluded there was no way to save: Enter and
 * double-click were the only ways, and nothing on the panel said so.
 *
 * plus Reload, which re-reads the file into the kernel after it has
 * been edited some other way.
 *
 * -- it edits the FILE, one line at a time --
 *
 * Every change reads /zeitlos.cfg, replaces the one line for the key
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
 * Fully usable with no pointer: Tab and Shift+Tab (or the arrow keys)
 * move between controls, Enter or Space activates one. A settings app
 * you can only reach with a mouse is exactly the wrong thing to have on
 * a machine whose pointer might be the thing you are trying to fix.
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
#include "../../common/zwidget.h"
#include "../../common/zdialog.h"
#include "../../common/zfsapp.h"
#include "../../common/zcfg.h"
#include "../../common/zrtc.h"		// the city table, and z_tz_parse()

// Fixed size, not resizable: a preferences panel has no content to
// reveal.
#define WIN_W   284
#define WIN_H   214

static z_win_t win;
static z_dialog_ctx_t dlg;

#define MARGIN      6
#define LINE_H      (z_font_5x8.h + 2)
#define BTN_H       16
#define EDIT_W      40
#define EDIT_H      14
#define GAP         4	// at least 2px, for the focus ring
#define TZ_ROWS     6	// visible rows in the time zone list

// -- controls --

#define GROUP_VIDEO 1

static const uint32_t video_modes[] = {
	Z_VIDEO_MODE_WHITE, Z_VIDEO_MODE_AMBER, Z_VIDEO_MODE_GREEN, Z_VIDEO_MODE_PAPER,
};
static const char *video_labels[] = { "White", "Amber", "Green", "Paper" };
#define MODE_COUNT 4

// Array order is Tab order, with the time zone list (not a widget)
// between Edit and Set -- see handle_key().
enum {
	W_EDIT_TERM = MODE_COUNT,
	W_SET_TZ,
	W_RELOAD,
	W_COUNT
};

static z_widget_t widgets[W_COUNT];
static z_widget_set_t wset;

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
	{ "apps.term.auto_connect", "Terminal connects to", W_EDIT_TERM, "", false },
};
#define PREF_COUNT (int)(sizeof(prefs) / sizeof(prefs[0]))

static int other_keys;			// settings in the file this app does not show
static char status[64];
static bool status_is_hint;		// status holds the "selected -- Set" hint

static int y_display, y_modes, y_prefs, y_rows, y_tz, y_list, y_reload, y_status;

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
static bool list_focus;			// keyboard focus is on the list

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
	z_listbox_select(&tz_list, tz_row_for(tz_value));
	zone_selection_changed();

	// Counted so the panel can say they exist and are being kept --
	// otherwise a person who put something else in the file has no way
	// to tell from here that this app will not throw it away.
	other_keys = 0;
	for (uint32_t i = 0; z_cfg_entry(i, k, sizeof(k), v, sizeof(v)); i++) {
		bool shown = !strcmp(k, "system.video.mode") ||
			!strcmp(k, "system.rtc.timezone");
		for (int j = 0; j < PREF_COUNT; j++)
			if (!strcmp(k, prefs[j].key)) shown = true;
		if (!shown) other_keys++;
	}

}

static void select_current_mode(void) {

	uint32_t now = z_video_get_mode();

	for (int i = 0; i < MODE_COUNT; i++) {
		if (video_modes[i] == now) {
			z_widget_select(&wset, i);
			break;
		}
	}

}

// -- layout and drawing --

static void layout(void) {

	int cw = z_win_content_w(&win);
	int bw = (cw - 2 * MARGIN - (MODE_COUNT - 1) * GAP) / MODE_COUNT;

	y_display = MARGIN;
	y_modes = y_display + LINE_H + 2;

	for (int i = 0; i < MODE_COUNT; i++) {
		widgets[i].x = (int16_t)(MARGIN + i * (bw + GAP));
		widgets[i].y = (int16_t)y_modes;
		widgets[i].w = (int16_t)bw;
		widgets[i].h = BTN_H;
	}

	y_prefs = y_modes + BTN_H + 10;
	y_rows = y_prefs + LINE_H + 4;

	for (int i = 0; i < PREF_COUNT; i++) {
		z_widget_t *w = &widgets[prefs[i].widget];
		w->x = (int16_t)(cw - MARGIN - EDIT_W);
		w->y = (int16_t)(y_rows + i * (2 * LINE_H + 6) - 3);
		w->w = EDIT_W;
		w->h = EDIT_H;
	}

	y_tz = y_rows + PREF_COUNT * (2 * LINE_H + 6);
	y_list = y_tz + LINE_H + 2;

	// 2px left of the margin on each side for the list's focus ring.
	z_listbox_set_geom(&tz_list, MARGIN, y_list, cw - 2 * MARGIN,
		TZ_ROWS * (z_font_5x8.h + 2) + 2);

	y_reload = y_list + TZ_ROWS * (z_font_5x8.h + 2) + 2 + 6;
	widgets[W_RELOAD].x = (int16_t)MARGIN;
	widgets[W_RELOAD].y = (int16_t)y_reload;
	widgets[W_RELOAD].w = 96;
	widgets[W_RELOAD].h = BTN_H;

	widgets[W_SET_TZ].w = 110;
	widgets[W_SET_TZ].h = BTN_H;
	widgets[W_SET_TZ].x = (int16_t)(cw - MARGIN - 110);
	widgets[W_SET_TZ].y = (int16_t)y_reload;

	y_status = y_reload + BTN_H + 6;

	z_widget_invalidate(&wset);
	z_listbox_invalidate(&tz_list);

}

// The value line under a preference: what is in effect, and where it
// came from.
static void describe(const pref_t *p, char *out, size_t n) {

	int max = (z_win_content_w(&win) - 2 * MARGIN - 8) / z_font_5x8.w;
	const char *v = p->value;
	const char *suffix = p->in_file ? "" : "  (default)";

	if (!strcmp(p->key, "apps.term.auto_connect") &&
		(!v[0] || !strcmp(v, "none")))
		v = "start panel";

	snprintf(out, n, "%.*s%s", max - (int)strlen(suffix), v, suffix);

}

static void draw_text(int x, int y, const char *s) {
	z_win_draw_text(&win, x, y, s, 1, &z_font_5x8);
}

static void draw_status(void) {

	char line[80];
	int cw = z_win_content_w(&win);

	z_win_fill_rect(&win, 0, y_status, cw, LINE_H, 0);

	if (status[0]) {
		draw_text(MARGIN, y_status, status);
	} else if (other_keys) {
		snprintf(line, sizeof(line), "%d other setting%s in the file, kept as is",
			other_keys, other_keys == 1 ? "" : "s");
		draw_text(MARGIN, y_status, line);
	}

}

static void repaint(void) {

	char line[80];
	int cw = z_win_content_w(&win);

	z_win_clear(&win);

	draw_text(MARGIN, y_display,
		can_set_video ? "Display" : "Display (unavailable)");

	draw_text(MARGIN, y_prefs, "Preferences");
	draw_text(cw - MARGIN - (int)strlen(Z_CFG_PATH) * z_font_5x8.w, y_prefs,
		Z_CFG_PATH);

	for (int i = 0; i < PREF_COUNT; i++) {
		int y = y_rows + i * (2 * LINE_H + 6);
		draw_text(MARGIN, y, prefs[i].title);
		describe(&prefs[i], line, sizeof(line));
		draw_text(MARGIN + 8, y + LINE_H, line);
	}

	// Title and what is in effect, on one line: the list below shows
	// the choice, but a value with no row (UTC+5:30 typed into the
	// file) still has to be visible somewhere.
	snprintf(line, sizeof(line), "Time zone: %.32s%s", tz_value,
		tz_in_file ? "" : "  (default)");
	draw_text(MARGIN, y_tz, line);

	draw_status();

	z_widget_draw_all(&wset, true);
	z_listbox_draw(&tz_list, true);

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

	if (wset.count) {
		z_widget_draw_all(&wset, false);
		draw_status();
	}

}

// The file before and after an edit.
//
// STATIC, and that is the fix for a real bug: these used to be
// malloc'd (8KB for the output, plus fs_mallocfile() for the input),
// and on the device save() reported "out of memory" and wrote nothing.
// An app's heap and stack share one 16KB allowance
// (z_proc_stack_size_for(), sw/os/kernel.h), and _sbrk() refuses to
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
			: "could not read /zeitlos.cfg");
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
		n = snprintf(out, Z_CFG_FILE_MAX, "# /zeitlos.cfg -- see docs/config.md\n");
	}

	if (fs_write_file(Z_CFG_PATH, out, n) != n) {
		set_status("could not write /zeitlos.cfg -- no sdcard?");
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

// -- actions --

static void apply_video(int idx) {

	if (idx < 0 || idx >= MODE_COUNT) return;

	if (!can_set_video) {
		// No register in this bitstream (an RTL change: `make flash`,
		// not `make dev-flash`). Put the selection back where the
		// display actually is.
		select_current_mode();
		z_widget_draw_all(&wset, false);
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

static void edit(const pref_t *p) {

	char v[Z_CFG_VAL_MAX];
	const char *msg;

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
		snprintf(s, sizeof(s), "no settings in /zeitlos.cfg -- defaults");
	else
		snprintf(s, sizeof(s), "reloaded: %d setting%s%s", n, n == 1 ? "" : "s",
			ignored ? ", some lines ignored" : "");

	set_status(s);
	read_values();
	select_current_mode();
	repaint();

}

static void choose_zone_and_return(void);

static void activate(int idx) {
	if (idx < 0) return;
	if (idx < MODE_COUNT) { apply_video(idx); return; }
	if (idx == W_EDIT_TERM) { edit(&prefs[0]); return; }
	if (idx == W_SET_TZ) { choose_zone_and_return(); return; }
	if (idx == W_RELOAD) { reload(); return; }
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

// Moves keyboard focus onto or off the list. The list sits between the
// Edit button and Set time zone in the Tab order; see handle_key().
static void focus_list(bool on) {
	list_focus = on;
	z_listbox_set_focus(&tz_list, on);
	if (on) z_widget_focus_set(&wset, -1);
	z_widget_draw_all(&wset, false);
	z_listbox_draw(&tz_list, false);
}

// The Set button, from a click or Enter on it. It disables itself once
// the zone is saved, so focus goes back to the list -- where a keyboard
// user was, and the one control near it still worth pressing.
static void choose_zone_and_return(void) {
	choose_zone();
	focus_list(true);
}

// The control after the list in Tab order: Set while it is enabled,
// otherwise Reload.
static int after_list(void) {
	return widgets[W_SET_TZ].enabled ? W_SET_TZ : W_RELOAD;
}

// -- input --

static void handle_mouse(uint32_t packed) {

	int cx, cy;
	bool inside = z_win_mouse_content_xy(&win, packed, &cx, &cy);
	uint8_t buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);

	// Samples over the titlebar reach us too. Same guard as text/term.
	if (!inside && wset.pressed < 0 && !tz_list.sb.dragging) return;

	// The list first, and for the whole of a scrollbar drag it started.
	if (wset.pressed < 0 && z_listbox_has_pointer(&tz_list, cx, cy)) {
		if ((buttons & Z_MOUSE_BTN_LEFT) && !list_focus) focus_list(true);
		int r = z_listbox_mouse(&tz_list, cx, cy, buttons);
		if (r == Z_LIST_ACTIVATED) choose_zone();
		else if (r == Z_LIST_SELECTED) zone_selection_changed();
		return;
	}
	z_listbox_mouse(&tz_list, cx, cy, buttons);		// keep its edges fed

	int act = z_widget_mouse(&wset, cx, cy, buttons);
	if (act >= 0 && list_focus) focus_list(false);
	activate(act);

}

static void handle_key(uint32_t keysym, uint8_t mods) {

	bool back = (mods & Z_KBD_MOD_SHIFT) != 0;

	// -- the list has focus --
	//
	// Every key it uses goes to it -- arrows move the selection there,
	// not the focus -- and Tab leaves it: forward to Set (or Reload
	// while Set is disabled), back to Edit.
	if (list_focus) {
		if (keysym == '\t') {
			focus_list(false);
			z_widget_focus_set(&wset, back ? W_EDIT_TERM : after_list());
			z_widget_draw_all(&wset, false);
			return;
		}
		int r = z_listbox_key(&tz_list, keysym);
		if (r == Z_LIST_ACTIVATED) choose_zone();
		else if (r == Z_LIST_SELECTED) zone_selection_changed();
		return;
	}

	// A letter or digit belongs to the list wherever focus is: nothing
	// else on this panel uses one, and "open settings, press B, get
	// Bangkok" should not first require finding the list with Tab.
	if ((keysym >= 'a' && keysym <= 'z') || (keysym >= 'A' && keysym <= 'Z') ||
		(keysym >= '0' && keysym <= '9')) {
		focus_list(true);
		if (z_listbox_key(&tz_list, keysym) == Z_LIST_SELECTED)
			zone_selection_changed();
		return;
	}

	switch (keysym) {

		case '\t':
			// Into the list from either side of it.
			if ((!back && wset.focused == W_EDIT_TERM) ||
				(back && wset.focused == after_list())) {
				focus_list(true);
				return;
			}
			z_widget_focus_next(&wset, back);
			z_widget_draw_all(&wset, false);
			return;

		case Z_KEY_UP:
		case Z_KEY_LEFT:
			z_widget_focus_next(&wset, true);
			z_widget_draw_all(&wset, false);
			return;

		case Z_KEY_DOWN:
		case Z_KEY_RIGHT:
			z_widget_focus_next(&wset, false);
			z_widget_draw_all(&wset, false);
			return;

		case 0x0d:
		case ' ':
			activate(z_widget_key_activate(&wset));
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

	widgets[W_EDIT_TERM].type = Z_WIDGET_BUTTON;
	widgets[W_EDIT_TERM].label = "Edit";
	widgets[W_EDIT_TERM].enabled = true;

	widgets[W_SET_TZ].type = Z_WIDGET_BUTTON;
	widgets[W_SET_TZ].label = "Set time zone";
	widgets[W_SET_TZ].enabled = false;		// see zone_selection_changed()

	widgets[W_RELOAD].type = Z_WIDGET_BUTTON;
	widgets[W_RELOAD].label = "Reload file";
	widgets[W_RELOAD].enabled = true;

	z_widget_set_init(&wset, widgets, W_COUNT, &win);

	select_current_mode();
	z_widget_focus_set(&wset, 0);
	for (int i = 0; i < MODE_COUNT; i++)
		if (widgets[i].on) z_widget_focus_set(&wset, i);

}

int main(void) {

	printf("settings: starting\n");

	can_set_video = z_video_mode_present();

	if (!can_set_video)
		printf("settings: this bitstream has no video mode register "
			"-- needs `make flash`, not `make dev-flash`\n");

	if (z_win_create_flags(&win, "settings", WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER) != Z_OK) {
		printf("settings: failed to create window -- is wm running?\n");
		return 1;
	}

	// Claimed and discarded: this app takes no argument, but leaving
	// one pending would hand it to whatever the user opens next.
	{
		char ignored[8];
		z_launch_arg_take(ignored, sizeof(ignored));
	}

	dlg.parent = &win;
	dlg.on_msg = on_dialog_msg;
	dlg.user = NULL;

	z_listbox_init(&tz_list, &win);
	z_listbox_set_items(&tz_list, TZ_ITEMS, tz_label, NULL);

	read_values();
	widgets_init();
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
