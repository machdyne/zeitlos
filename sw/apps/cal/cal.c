/*
 * cal -- month calendar
 *
 * A single month as a 7x6 grid, with today marked if the machine
 * knows what today is. Left/right arrows move a month at a time.
 * See docs/cal_app.md.
 *
 *   > run wm
 *   > run net      # optional -- without it there is no "today"
 *   > run cal
 *
 * Or click its icon in the dock.
 *
 * -- what this app deliberately is not --
 *
 * There are no notes, no events, no reminders and no settings. That
 * is not a staging decision -- a calendar that stores things needs a
 * file format, a place on the card to put it, and an answer for what
 * happens to it when the clock is wrong, and none of that belongs in
 * the thing that draws a month. This app answers "what day is the
 * 14th" and nothing else.
 *
 * -- where the date comes from --
 *
 * Not from here. Same split as sw/apps/clock: this only ever READS
 * the RTC, which is set by sw/apps/net's SNTP client (sw/apps/net/
 * ntp.c). An app that also did the networking would mean the machine
 * only knew the date while a window was open.
 *
 * -- three states, and the epoch --
 *
 *   no RTC in this bitstream  -- `RTC off in rtl/boards.vh, or a
 *                                build predating rtl/rtc.v. A
 *                                gateware change; the status line
 *                                says which command.
 *   RTC present, never set    -- the counter runs but its epoch is
 *                                meaningless. Opens on January 1970
 *                                and marks no day.
 *   RTC set                   -- opens on the current month with
 *                                today marked.
 *
 * The middle case opens on the epoch on purpose. It is not a guess
 * dressed up as a date: 1970 is visibly not now, so the app cannot be
 * mistaken for one that knows something it doesn't. The clock app
 * shows `--:--:--` for the same reason; a grid has no equivalent of
 * blanking, so it shows the one month that is honestly implied by a
 * counter at zero.
 *
 * That would be useless on its own -- 1970 is 672 presses of the
 * right arrow from anywhere anybody cares about -- so typing four
 * digits jumps to a year. On a machine with no network that is not a
 * shortcut, it is the primary navigation.
 *
 * -- unlike clock, this does not need a live RTC to be useful --
 *
 * Worth stating because it drives the design. A clock with no time is
 * furniture. A calendar with no time is still a perpetual calendar:
 * every month from 1970 to 2105 is exactly as correct without a
 * network as with one. Only the highlight depends on the RTC, so
 * everything else stays fully live when there isn't one.
 *
 -- time zone --
 *
 * "Today" is today in the zone system.rtc.timezone names in
 * /zeitlos.cfg (docs/config.md), UTC by default -- the same conversion
 * the clock app uses (z_tz_*(), zrtc.h). The status line ends with the
 * zone's abbreviation, so with nothing configured it still says UTC and
 * a highlight that moves hours before local midnight reads as
 * correct-but-elsewhere rather than broken.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zrtc.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zgfx.h"
#include "../../common/zfont.h"
#include "../../common/zkbd.h"
#include "../../common/zwidget.h"

#include "../../common/zcfg.h"		// system.rtc.timezone
#include "cal_core.h"

// -- geometry --
//
// Fixed size. A month is a fixed amount of information, so there is
// nothing a bigger window would reveal -- the same reason clock and
// settings are fixed, and it also means the six-row allocation
// (cal_core.h) never has to fight a resize.
//
// The width is built up from the grid rather than chosen: 7 columns
// of CELL_W, plus a margin either side, plus the 4px z_win_content_
// rect() insets. Everything in layout() below re-derives from the
// real content size anyway, so changing a constant here moves
// everything that depends on it.

#define CELL_W      20		// fits "30" in 6x12 (12px) with 4px either side
#define CELL_H      14		// 12px glyph + 2px leading

#define MARGIN      6
#define GAP         4

#define BTN_W       16
#define BTN_H       14

#define HDR_H       (z_font_6x12.h)		// the Su/Mo/Tu row
#define STATUS_H    (z_font_5x8.h)

#define GRID_W      (CAL_COLS * CELL_W)
#define GRID_H      (CAL_ROWS * CELL_H)

#define WIN_W       (GRID_W + 2 * MARGIN + 4)
#define WIN_H       (MARGIN + BTN_H + GAP + HDR_H + GRID_H + GAP + \
					STATUS_H + MARGIN + Z_WM_TITLEBAR_H + 4)

static z_win_t win;

// -- widgets --
//
// Two push buttons, one at each end of the top row. Buttons rather
// than a radio group because neither is a state: they are steps, and
// pressing one twice should move two months.
//
// They sit at opposite ends of a 140px row, so the >= 2px separation
// the focus ring needs (docs/widgets.md) is not something this
// layout has to think about.

enum { W_PREV = 0, W_NEXT, W_COUNT };

static z_widget_t widgets[W_COUNT];
static z_widget_set_t wset;

// -- layout, recomputed from the real content size --

static int hdr_y;			// weekday name row
static int grid_x, grid_y;	// top-left of cell (0,0)
static int title_x, title_y, title_w;
static int status_y;

// -- state --

// The month on screen.
static int32_t cur_year = CAL_EPOCH_YEAR;
static uint8_t cur_month = CAL_EPOCH_MONTH;
static cal_month_t shown;

static bool rtc_ok;			// this bitstream has an RTC at all

// Today, as last read. day == 0 means "not known" -- either no RTC
// or one that has never been set -- which is the only thing that
// suppresses the highlight.
static int32_t today_year;
static uint8_t today_month, today_day, today_wday;

// The UTC day index (seconds / 86400) the display was last built
// against, so the periodic check can tell "the day rolled over" from
// "nothing happened" with one comparison. -1 before the first read.
static int32_t shown_day_index = -1;

// Whether the user has moved off the month the clock implies.
//
// This exists for one specific case: cal is launched before net's
// first NTP sync lands, so it opens on the epoch, and the RTC becomes
// valid a few seconds later. Following that to the real month is
// obviously right -- unless the user has already navigated somewhere,
// in which case yanking the view out from under them is not a
// feature. So the app follows the clock only while it has not been
// steered.
static bool user_navigated;

static cal_entry_t entry;

// -- small helpers --
//
// Day numbers are drawn 42 times per grid repaint, so they do not go
// through snprintf -- see docs/widgets.md on keeping a draw path off
// newlib's formatter. Two digits never need one.
static void day_str(int d, char *out) {

	if (d >= 10) {
		out[0] = (char)('0' + d / 10);
		out[1] = (char)('0' + d % 10);
		out[2] = '\0';
	} else {
		out[0] = (char)('0' + d);
		out[1] = '\0';
	}

}

static int text_w6(const char *s) {
	return (int)strlen(s) * z_font_6x12.w;
}

static int text_w5(const char *s) {
	return (int)strlen(s) * z_font_5x8.w;
}

// -- reading the clock --

static z_tz_t tz;
static uint32_t tz_generation = 0xFFFFFFFFu;
static bool tz_dst;

// Re-reads the zone when the config generation has moved. Cheap enough
// to call on every clock check.
static void tz_refresh(void) {

	uint32_t gen = z_cfg_generation();
	char v[Z_CFG_VAL_MAX];

	if (gen == tz_generation) return;
	tz_generation = gen;

	z_cfg_get("system.rtc.timezone", v, sizeof(v));
	if (!z_tz_parse(v, &tz))
		printf("cal: system.rtc.timezone '%s' not understood -- using UTC "
			"(see docs/config.md)\n", v);

}

// Refreshes today_* from the RTC. Returns the current LOCAL day index,
// or -1 if the date is not known. A zone change moves the index too,
// which is what makes check_clock() redraw after `cfg reload`.
static int32_t read_today(void) {

	uint32_t utc;
	int32_t y;
	uint8_t m, d;

	today_day = 0;

	if (!rtc_ok) return -1;
	if (!z_rtc_valid()) return -1;

	tz_refresh();
	utc = z_tz_local(&tz, z_rtc_seconds(), &tz_dst);

	// A timestamp outside the displayable range cannot come from a
	// sane sync, but it can come from a garbage one, and a today
	// that is not in any month this app can draw would otherwise
	// silently never highlight anything.
	if (!cal_from_time(utc, &y, &m, &d, &today_wday)) return -1;

	today_year = y;
	today_month = m;
	today_day = d;

	return (int32_t)(utc / Z_SECS_PER_DAY);

}

// True if the grid currently on screen contains today.
static bool showing_today_month(void) {
	return today_day != 0 &&
		today_year == cur_year && today_month == cur_month;
}

// -- layout --

static void layout(void) {

	int cw = z_win_content_w(&win);
	int ch = z_win_content_h(&win);

	int gw = CAL_COLS * CELL_W;

	// The grid is centred in whatever width there actually is,
	// rather than pinned at MARGIN. The window is fixed so these
	// agree today; centring means they still agree if a constant
	// changes, instead of leaving the grid off to one side.
	grid_x = (cw - gw) / 2;
	if (grid_x < 0) grid_x = 0;

	widgets[W_PREV].x = (int16_t)MARGIN;
	widgets[W_NEXT].x = (int16_t)(cw - MARGIN - BTN_W);

	for (int i = 0; i < W_COUNT; i++) {
		widgets[i].y = (int16_t)MARGIN;
		widgets[i].w = BTN_W;
		widgets[i].h = BTN_H;
	}

	// The month label lives between the two buttons, with a gap
	// either side so the focus ring on a button never touches it.
	title_x = MARGIN + BTN_W + GAP;
	title_w = cw - 2 * (MARGIN + BTN_W + GAP);
	if (title_w < 8) title_w = 8;
	title_y = MARGIN + (BTN_H - z_font_6x12.h) / 2;

	hdr_y = MARGIN + BTN_H + GAP;
	grid_y = hdr_y + HDR_H;

	status_y = ch - MARGIN - STATUS_H;
	if (status_y < grid_y + CAL_ROWS * CELL_H) 
		status_y = grid_y + CAL_ROWS * CELL_H;

	z_widget_invalidate(&wset);

}

// -- drawing --

// The weekday initials row. Two characters each, centred in their
// column, and taken from cal_col_wday() rather than written out in
// one fixed order -- so this row rotates with CAL_WEEK_START instead
// of quietly disagreeing with the grid under it.
static void draw_weekday_header(void) {

	z_win_fill_rect(&win, grid_x, hdr_y, CAL_COLS * CELL_W, HDR_H, 0);

	for (int col = 0; col < CAL_COLS; col++) {

		const char *full = z_wday_name(cal_col_wday(col));
		char lbl[3];
		int tw;

		// "Sun" -> "Su". The third letter is what distinguishes
		// Sat from Sun and Thu from Tue, so dropping it would be
		// ambiguous in a list -- but the column POSITION already
		// disambiguates here, and two characters is what fits a
		// 20px cell with margins at 6px per glyph.
		lbl[0] = full[0];
		lbl[1] = full[1];
		lbl[2] = '\0';

		tw = text_w6(lbl);

		z_win_draw_text(&win, grid_x + col * CELL_W + (CELL_W - tw) / 2,
			hdr_y, lbl, 1, &z_font_6x12);

	}

}

// One cell. `today` inverts it.
//
// The inversion uses z_win_draw_text2() rather than z_win_draw_text():
// the latter hardcodes its glyph background to 0, so ink 0 on a lit
// cell would erase the cell instead of writing on it -- the exact
// failure documented in zwin.h and in docs/widgets.md.
static void draw_cell(int row, int col, int day, bool today) {

	int x = grid_x + col * CELL_W;
	int y = grid_y + row * CELL_H;
	char s[3];
	int tw;

	z_win_fill_rect(&win, x, y, CELL_W, CELL_H, today ? 1 : 0);

	if (day == 0) return;

	day_str(day, s);
	tw = text_w6(s);

	// Centred horizontally, and vertically within the cell's
	// leading, so the numbers sit on a common baseline whatever
	// CELL_H becomes.
	if (today)
		z_win_draw_text2(&win, x + (CELL_W - tw) / 2,
			y + (CELL_H - z_font_6x12.h) / 2, s, 0, 1, &z_font_6x12);
	else
		z_win_draw_text(&win, x + (CELL_W - tw) / 2,
			y + (CELL_H - z_font_6x12.h) / 2, s, 1, &z_font_6x12);

}

static void draw_grid(void) {

	bool mark = showing_today_month();
	int trow = -1, tcol = -1;

	if (mark)
		cal_cell_of_day(&shown, (int)today_day, &trow, &tcol);

	for (int row = 0; row < CAL_ROWS; row++)
		for (int col = 0; col < CAL_COLS; col++)
			draw_cell(row, col, cal_cell_day(&shown, row, col),
				mark && row == trow && col == tcol);

}

// The month heading, or the year being typed.
//
// One line serving both is deliberate. Year entry replaces the thing
// it is about to change, so the buffer appears exactly where its
// result will, and there is no separate field to notice, dismiss or
// lay out.
static void draw_title(void) {

	char line[24];
	int tw;

	z_win_fill_rect(&win, title_x, title_y, title_w, z_font_6x12.h, 0);

	if (entry.active) {
		// Underscores for the digits not yet typed, so the line
		// shows how many are still wanted. A four-digit year is
		// required (cal_core.h) and this is where that is
		// communicated.
		snprintf(line, sizeof(line), "Year %s%s", entry.buf,
			"____" + entry.len);
	} else {
		snprintf(line, sizeof(line), "%s %04ld",
			z_month_name_long(cur_month), (long)cur_year);
	}

	tw = text_w6(line);

	// Clamp rather than centre if it somehow overruns, so a long
	// string loses its right edge instead of both ends.
	z_win_draw_text(&win, title_x + (tw < title_w ? (title_w - tw) / 2 : 0),
		title_y, line, 1, &z_font_6x12);

}

// The status line: what the app knows about the clock, or what it is
// waiting for. 5x8 rather than 6x12 -- this is secondary text and
// should not compete with the grid.
// The fixed status strings, named rather than written inline.
//
// tests/test_layout.c measures these symbols to prove they fit the
// window. Inline literals would mean the test measuring its own copy,
// which passes forever while the app overruns -- and the first
// version of that test did exactly that, right up until the copies
// diverged. These are the strings that ship, so they are the strings
// that get measured.
//
// The no-RTC line names the command because it is a gateware problem
// and `make dev-flash` will not fix it, which is the specific
// confusion worth heading off (see docs/rtc.md).
static const char *const STATUS_ENTRY   = "Enter to go, Esc to cancel";
static const char *const STATUS_NO_RTC  = "No RTC: `RTC off, make flash";
static const char *const STATUS_NOT_SET = "Clock not set -- type a year";

static void draw_status(void) {

	char buf[40];
	const char *s;
	int cw = z_win_content_w(&win);
	int tw;

	z_win_fill_rect(&win, 0, status_y, cw, STATUS_H, 0);

	if (entry.active) {
		s = STATUS_ENTRY;
	} else if (!rtc_ok) {
		s = STATUS_NO_RTC;
	} else if (today_day == 0) {
		// The common case for the first seconds after boot, and
		// permanent on a machine with no network.
		s = STATUS_NOT_SET;
	} else {
		// The date in full, not a bare "UTC" label.
		//
		// The highlight already says which square is today, but
		// only while today's month is the one on screen -- page
		// away and the app would otherwise stop telling you the
		// date at all, which is a thing people open a calendar to
		// find out. Spelling it here means the answer survives
		// navigation.
		//
		// The word UTC is doing the same job as the clock app's:
		// west of Greenwich this rolls over hours before local
		// midnight, and an unlabelled date that disagrees with the
		// wall reads as broken rather than as correct-but-elsewhere.
		snprintf(buf, sizeof(buf), "Today %s %d %s %04ld %s",
			z_wday_name(today_wday), (int)today_day,
			z_month_name(today_month), (long)today_year,
			z_tz_name(&tz, tz_dst));
		s = buf;
	}

	tw = text_w5(s);

	z_win_draw_text(&win, (cw - tw) / 2, status_y, s, 1, &z_font_5x8);

}

// Arrows go dead at the ends of the representable range rather than
// silently refusing -- a live-looking button that does nothing is a
// bug report. cal_step_month() on a copy is the range check: it
// refuses without moving anything, so there is no separate notion of
// "where the ends are" to keep in step with cal_core.
static void update_arrows(void) {

	int32_t y;
	uint8_t m;
	bool prev_ok, next_ok;

	y = cur_year; m = cur_month;
	prev_ok = cal_step_month(&y, &m, -1);

	y = cur_year; m = cur_month;
	next_ok = cal_step_month(&y, &m, 1);

	if (widgets[W_PREV].enabled != prev_ok) {
		widgets[W_PREV].enabled = prev_ok;
		widgets[W_PREV].dirty = true;
	}

	if (widgets[W_NEXT].enabled != next_ok) {
		widgets[W_NEXT].enabled = next_ok;
		widgets[W_NEXT].dirty = true;
	}

	// Focus must not be left sitting on a button that just went
	// dead -- z_widget_key_activate() would return -1 and Enter
	// would appear to do nothing at all.
	if (wset.focused >= 0 && !widgets[wset.focused].enabled)
		z_widget_focus_next(&wset, false);

}

// Rebuilds `shown` for the current position. Falls back to the epoch
// if the position somehow does not resolve, rather than drawing a
// stale month under a new heading.
static void reload_month(void) {

	if (!cal_month_info(cur_year, cur_month, &shown)) {
		cur_year = CAL_EPOCH_YEAR;
		cur_month = CAL_EPOCH_MONTH;
		cal_month_info(cur_year, cur_month, &shown);
	}

	update_arrows();

}

// Everything below the button row. Used whenever the month changes:
// repainting only this leaves the buttons alone, so paging months
// does not flash the whole window.
static void repaint_body(void) {

	draw_title();
	draw_weekday_header();
	draw_grid();
	draw_status();

}

static void repaint(void) {

	// wm clears before most redraws but NOT after a move, so
	// anything not actively rewritten keeps its pre-move contents.
	z_win_clear(&win);

	repaint_body();
	z_widget_draw_all(&wset, true);

}

// -- navigation --

static void goto_month(int32_t year, uint8_t month) {

	if (!cal_pos_valid(year, month)) return;
	if (year == cur_year && month == cur_month) return;

	cur_year = year;
	cur_month = month;

	reload_month();
	repaint_body();
	z_widget_draw_all(&wset, false);

}

static void step_month(int delta) {

	int32_t y = cur_year;
	uint8_t m = cur_month;

	if (!cal_step_month(&y, &m, delta)) return;

	user_navigated = true;
	goto_month(y, m);

}

static void step_year(int delta) {

	int32_t y = cur_year;
	uint8_t m = cur_month;

	if (!cal_step_year(&y, &m, delta)) return;

	user_navigated = true;
	goto_month(y, m);

}

// Back to the month containing today, and back under the clock's
// control. Does nothing when there is no today to go to -- see
// handle_key() on why that is silent.
static void goto_today(void) {

	if (today_day == 0) return;

	user_navigated = false;
	goto_month(today_year, today_month);

}

// -- the clock check --
//
// Called from the main loop. Redraws only when something actually
// changed: the day rolled over, or the clock became valid (or
// stopped being).
static void check_clock(void) {

	int32_t idx = read_today();
	bool had_today = shown_day_index >= 0;

	if (idx == shown_day_index) return;

	// The clock just became valid and the user has not steered --
	// follow it to the real month. This is the launched-before-NTP
	// case; see user_navigated's own comment.
	if (idx >= 0 && !had_today && !user_navigated) {
		shown_day_index = idx;
		goto_month(today_year, today_month);
		// goto_month() is a no-op if we were somehow already there,
		// so redraw the parts that depend on today regardless.
		draw_grid();
		draw_status();
		return;
	}

	shown_day_index = idx;

	// Otherwise only the highlight and the status line can have
	// changed, and the highlight only if today is on screen.
	if (showing_today_month() || idx < 0) draw_grid();

	draw_status();

}

// -- input --

static void handle_mouse(uint32_t packed) {

	int cx, cy;
	bool inside = z_win_mouse_content_xy(&win, packed, &cx, &cy);

	uint8_t buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);

	// Samples over the titlebar reach us too -- wm's hit test is
	// the whole window rect. Without this, clicking the close icon
	// also lands on whatever widget sits at the clamped
	// coordinates. Same guard as sw/apps/settings and sw/apps/clock.
	if (!inside && wset.pressed < 0) return;

	int act = z_widget_mouse(&wset, cx, cy, buttons);

	if (act == W_PREV) step_month(-1);
	else if (act == W_NEXT) step_month(1);

}

static void handle_key(uint32_t keysym, uint8_t mods) {

	// -- year entry first --
	//
	// While a year is being typed the digit keys belong to the
	// buffer, and so do Enter, Escape and Backspace. Everything
	// else still works, so an arrow press during entry moves the
	// month and abandons the half-typed year -- which is the
	// reading a user who has started doing something else expects,
	// rather than a modal state they have to escape from first.

	if (keysym >= '0' && keysym <= '9') {
		if (cal_entry_digit(&entry, (char)keysym)) {
			draw_title();
			draw_status();
		}
		return;
	}

	if (entry.active) {

		switch (keysym) {

			case 0x0d:		// Enter
			{
				int32_t y;
				if (cal_entry_commit(&entry, &y)) {
					user_navigated = true;
					// The month is kept across a year jump, so
					// typing a year lands on the same month of it.
					goto_month(y, cur_month);
					// goto_month() repaints the body only if the
					// position actually changed; the title has to
					// come back either way, since it is currently
					// showing the entry buffer.
					draw_title();
					draw_status();
				} else {
					// An incomplete or out-of-range year: keep the
					// buffer so it can be corrected rather than
					// retyped, and let the status line say what is
					// wanted.
					draw_status();
				}
				return;
			}

			case 0x1b:		// Escape
				cal_entry_reset(&entry);
				draw_title();
				draw_status();
				return;

			case '\b':
			case 0x7f:
				if (cal_entry_back(&entry)) {
					draw_title();
					draw_status();
				}
				return;

			default:
				break;

		}

	}

	switch (keysym) {

		case '\t':
			z_widget_focus_next(&wset, (mods & Z_KBD_MOD_SHIFT) != 0);
			z_widget_draw_all(&wset, false);
			return;

		// Arrows are unambiguous here: there is no list and no text
		// field on this window for them to belong to. Left/right a
		// month, up/down a year -- which is the pairing every
		// calendar with two axes of navigation uses.
		case Z_KEY_LEFT:
			step_month(-1);
			return;

		case Z_KEY_RIGHT:
			step_month(1);
			return;

		case Z_KEY_UP:
			step_year(-1);
			return;

		case Z_KEY_DOWN:
			step_year(1);
			return;

		case Z_KEY_HOME:
		case 't':
		case 'T':
			// Silent when the clock is unset. There is nowhere to
			// go, the status line already says why, and an error
			// for a key the user may have pressed speculatively is
			// noise.
			goto_today();
			return;

		case 0x0d:		// Enter
		case ' ':
		{
			int act = z_widget_key_activate(&wset);
			if (act == W_PREV) step_month(-1);
			else if (act == W_NEXT) step_month(1);
			return;
		}

		default:
			return;

	}

}

static void widgets_init(void) {

	memset(widgets, 0, sizeof(widgets));

	widgets[W_PREV].type = Z_WIDGET_BUTTON;
	widgets[W_PREV].label = "<";
	widgets[W_PREV].enabled = true;

	widgets[W_NEXT].type = Z_WIDGET_BUTTON;
	widgets[W_NEXT].label = ">";
	widgets[W_NEXT].enabled = true;

	z_widget_set_init(&wset, widgets, W_COUNT, &win);

	// Focus starts on the forward arrow, so Enter out of the box
	// does the thing a calendar is usually opened to do.
	z_widget_focus_set(&wset, W_NEXT);

}

int main(void) {

	printf("cal: starting\n");

	// z_rtc_available(), NOT z_rtc_present(): on a bitstream built
	// before rtl/rtc.v existed the RTC registers may not be decoded
	// at all, and an undecoded read on this bus never acks -- the
	// CPU would hang here rather than returning a wrong answer. See
	// sw/common/zrtc.h.
	rtc_ok = z_rtc_available();

	if (!rtc_ok)
		printf("cal: this bitstream has no RTC -- the calendar still "
			"works, but nothing will be marked as today\n");
	else if (!z_rtc_valid())
		printf("cal: RTC present but not set -- opening on the epoch, "
			"waiting for net's ntp sync (`run net`)\n");

	cal_entry_reset(&entry);

	// Open on today if the clock knows it, on the epoch otherwise.
	shown_day_index = read_today();

	if (today_day != 0) {
		cur_year = today_year;
		cur_month = today_month;
	}

	if (z_win_create_flags(&win, "cal", WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER) != Z_OK) {
		printf("cal: failed to create window -- is wm running?\n");
		return 1;
	}

	// Claimed and discarded: this app takes no argument, but leaving
	// one pending would hand it to whatever the user opens next.
	{
		char ignored[8];
		z_launch_arg_take(ignored, sizeof(ignored));
	}

	widgets_init();
	reload_month();
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

				// The part of this window not covered by the windows
				// in front of it. Confines every subsequent draw to
				// it -- see z_win_apply_clip() in zwin.c. The ack it
				// sends is not optional: wm waits for it when a
				// region narrows.
				case Z_WM_SET_CLIP:
					if (!z_win_apply_clip(&win, &msg.obj))
						printf("cal: bad clip region message\n");
					break;

				case Z_WM_REDRAW:

					if (msg.obj.type != Z_UINT32) break;
					if (z_win_redraw_id(msg.obj.val.uint32) != win.id) break;

					z_win_apply_redraw(&win, msg.obj.val.uint32);
					// The window may have moved, and z_widget_draw()
					// resolves its own absolute coordinates through
					// z_win_content_rect() -- but layout() also
					// depends on the content SIZE, so re-running it
					// here keeps one path for both.
					layout();
					repaint();
					z_win_redraw_done(&win);

					break;

				case Z_WM_WINDOW_MOVED:
					z_win_parse_rect(&win, &msg.obj);
					layout();
					break;

				default:
					break;

			}

		}

		check_clock();

		// Once a second.
		//
		// A calendar changes at most once a day, so this looks
		// generous -- but the thing being waited for is not
		// midnight, it is net's first NTP sync, which lands at an
		// unpredictable moment a few seconds after boot and is what
		// turns the epoch into today. A minute of staring at 1970
		// after the network came up would read as the app being
		// broken.
		//
		// The cost is one register read and a comparison per
		// second; check_clock() returns immediately when the day
		// index is unchanged, which is every time but one per day.
		z_proc_wait(Z_TICK_HZ);

	}

	return 0;

}
