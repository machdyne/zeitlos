/*
 * clock -- analog and digital wall clock
 *
 * Reads the hardware RTC (rtl/rtc.v, sw/common/zrtc.h) and draws it,
 * either as a dial with three hands or as digits. Two buttons at the
 * bottom switch between the two. See docs/clock_app.md.
 *
 *   > run wm
 *   > run clock
 *
 * -- where the time comes from --
 *
 * Not from here. This app only ever READS the clock; the RTC is set by
 * sw/apps/net's SNTP client, shortly after the network comes up and
 * hourly thereafter (sw/apps/net/ntp.c). That split is deliberate --
 * a clock app that also did the networking would mean the machine only
 * knew the time while a window was open.
 *
 * There is deliberately no Sync button. The system keeps itself in
 * step without being asked, so a button for it would be furniture
 * that does nothing observable in the overwhelmingly common case --
 * and one whose only honest feedback ("asked, no idea yet") is worse
 * than not offering it.
 *
 * -- UTC --
 *
 * Displayed times are UTC, and the window says so. The RTC has a
 * timezone-offset register (rtl/rtc.v) but nothing sets it and nothing
 * here reads it: a wrong offset silently applied is worse than a right
 * time honestly labelled, and there is no zone database on this system
 * to derive one from. See docs/rtc.md.
 *
 * So there are three states worth displaying, not two, and they are
 * genuinely different things:
 *
 *   no RTC in this bitstream   -- `RTC is off in rtl/boards.vh, or
 *                                 this build predates rtl/rtc.v.
 *                                 Either way a gateware change. Says
 *                                 so, and draws nothing that pretends
 *                                 to be a time.
 *   RTC present but never set  -- the counter is running but its
 *                                 epoch is meaningless. Shows
 *                                 --:--:-- and a stopped dial rather
 *                                 than confidently displaying 1970.
 *   RTC set                    -- draw the time.
 *
 * The middle one is the common case for the first few seconds after
 * boot, and on any machine with no network at all it is permanent.
 *
 * -- drawing --
 *
 * The dial and hands go through the GPU line rasterizer
 * (z_win_hw_line(), rtl/gpu/gpu_raster.v); the digits go through the
 * hardware glyph blitter as z_font_6x12 text, which works without this
 * app loading anything into glyph memory because wm already loads that
 * font there at startup (see docs/window_manager.md, "Hardware glyph
 * blitting" -- wm is the only process that ever writes glyph memory,
 * and it loads both 5x8 and 6x12).
 *
 * -- why the hands are erased rather than the window cleared --
 *
 * A full clear-and-redraw once a second is visible as a flash, which
 * on a clock is the whole screen blinking at you forever. So each
 * update erases the three hands by redrawing them in colour 0 at their
 * previous endpoints, then draws them at the new ones.
 *
 * That works only because of a layout constraint that is easy to break
 * later: the hands are shorter than the tick marks' inner radius, so
 * erasing a hand can never rub out part of the dial. If a hand is ever
 * lengthened past HAND_MAX_R, the erase starts eating the ticks and
 * the dial slowly disintegrates -- a bug that looks like a rasterizer
 * fault rather than a layout mistake. All three hands are redrawn
 * together on every update for the same class of reason: erasing the
 * second hand can cross the minute hand, so the fix is to put all
 * three back immediately rather than to reason about overlaps.
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

// Fixed size, and small: a clock is something you leave open in a
// corner while doing something else, so the useful size is the
// smallest one still readable across a room. That floor is set by the
// digital view -- "HH:MM:SS" is 8 characters of z_font_6x12, so 48px
// plus margins -- and by wanting a dial big enough for an hour hand
// and a minute hand to be told apart, which at these proportions is
// about a 100px diameter.
//
// Not resizable: rescaling the dial on every drag is a lot of
// machinery for a window nobody wants a different size of. Content
// area works out at WIN_W-4 by WIN_H-Z_WM_TITLEBAR_H-4 (see
// z_win_content_rect()).
#define WIN_W   132
#define WIN_H   162

static z_win_t win;

#define MARGIN      4
#define BTN_H       14
#define BTN_GAP     4

// -- widgets --
//
// Two radio members, one per view. A radio group rather than two
// independent toggles because the toolkit then OWNS which view is
// current, and this file never keeps a second copy of that fact that
// could disagree with what is drawn.
#define GROUP_VIEW  1

enum { W_ANALOG = 0, W_DIGITAL, W_COUNT };

static z_widget_t widgets[W_COUNT];
static z_widget_set_t wset;

// -- layout, recomputed in layout() from the real content size --
static int clock_x, clock_y, clock_w, clock_h;	// the area above the buttons

// The dial's centre, in BOTH coordinate systems, because this file
// genuinely needs both and converting at each call site is how they
// end up disagreeing. z_win_hw_line() takes absolute screen
// coordinates; z_win_fill_rect()/z_win_draw_text() take
// content-relative ones. Named apart so a mix-up is visible at the
// call site rather than being a plausible-looking variable.
static int cx, cy;				// absolute screen
static int ccx, ccy;			// content-relative
static int radius;

// Hand lengths as a fraction of `radius`, and the tick marks' inner
// radius. HAND_MAX_R is the invariant the erase strategy depends on --
// see this file's header comment. Kept as a named constant precisely
// so the relationship is stated somewhere rather than being an
// accident of three separate divisions.
#define TICK_INNER_NUM   85		// ticks run from 85% of radius to 100%
#define TICK_INNER_DEN  100
#define HAND_MAX_NUM     78		// no hand may exceed 78% of radius
#define HAND_MAX_DEN    100

#define HOUR_HAND_NUM    45
#define MIN_HAND_NUM     68
#define SEC_HAND_NUM     78

// -- sine table --
//
// sin(6 * i degrees) * 1024, i in [0,60) -- one entry per second/minute
// position on the dial. Cosine is the same table 15 entries (90
// degrees) along, so there is only one table.
//
// Integer, because there is no FPU here and no libm in these binaries.
// 1024 as the scale rather than 1000 so the divide back down is a
// shift; radius * 1024 cannot overflow an int32 for any radius this
// window can produce.
static const int32_t sin60[60] = {
	    0,   107,   213,   316,   416,   512,   602,   685,   761,   828,
	  887,   935,   974,  1002,  1018,  1024,  1018,  1002,   974,   935,
	  887,   828,   761,   685,   602,   512,   416,   316,   213,   107,
	    0,  -107,  -213,  -316,  -416,  -512,  -602,  -685,  -761,  -828,
	 -887,  -935,  -974, -1002, -1018, -1024, -1018, -1002,  -974,  -935,
	 -887,  -828,  -761,  -685,  -602,  -512,  -416,  -316,  -213,  -107,
};

// Endpoint of a hand of length `len` at dial position `pos` (0-59,
// 0 = twelve o'clock, increasing clockwise).
//
// Screen y grows downward, so the vertical term is subtracted rather
// than added -- that single minus sign is the difference between a
// clock and its mirror image, and it is not obvious from the maths.
static void hand_end(int pos, int len, int *ex, int *ey) {
	int i = pos % 60;
	*ex = cx + (int)((sin60[i] * (int32_t)len) / 1024);
	*ey = cy - (int)((sin60[(i + 15) % 60] * (int32_t)len) / 1024);
}

// -- state --

// What is currently drawn, so an update knows what to erase. -1 in
// last_pos means nothing is drawn yet and there is nothing to erase --
// which is the state after any full repaint, since those redraw the
// dial and lose the hands with it.
static int last_pos[3] = { -1, -1, -1 };	// hour, minute, second
static uint32_t last_shown_sec;				// the UTC second last drawn
static bool have_shown;

static bool rtc_ok;			// this bitstream has an RTC at all

static bool analog_view = true;

// -- drawing helpers --

static void draw_hand(int pos, int len, int color) {
	int ex, ey;
	hand_end(pos, len, &ex, &ey);
	z_win_hw_line(&win, cx, cy, ex, ey, color);
}

// Centre hub. Small enough not to swallow the hands' origin, big
// enough to hide the fact that three lines meeting at a point look
// ragged at 1bpp.
//
// z_win_fill_rect() rather than z_fb_hw_fill_rect(): the latter takes
// absolute coordinates and clips only to the SCREEN, so it would
// happily draw outside this window.
static void draw_hub(void) {
	z_win_fill_rect(&win, ccx - 1, ccy - 1, 3, 3, 1);
}

// The dial: a 60-segment polygon standing in for a circle, plus a tick
// at each hour.
//
// A polygon rather than a real circle because the rasterizer draws
// lines and nothing else, and at these radii the difference is under a
// pixel -- the segments are ~8 pixels long on an 80-pixel radius, and
// the sagitta of an 8-pixel chord on that circle is about a fifth of a
// pixel. Reusing the same table the hands use also means the dial and
// the hands agree exactly about where twelve o'clock is, which a
// separately-computed circle would only do to within rounding.
static void draw_dial(void) {

	int px = 0, py = 0;

	for (int i = 0; i <= 60; i++) {
		int x, y;
		hand_end(i, radius, &x, &y);
		if (i > 0) z_win_hw_line(&win, px, py, x, y, 1);
		px = x; py = y;
	}

	int inner = (radius * TICK_INNER_NUM) / TICK_INNER_DEN;

	for (int h = 0; h < 12; h++) {
		int x0, y0, x1, y1;
		hand_end(h * 5, inner, &x0, &y0);
		hand_end(h * 5, radius, &x1, &y1);
		z_win_hw_line(&win, x0, y0, x1, y1, 1);
		// The quarters get a second, adjacent tick so the dial reads
		// at a glance without numerals, which will not fit legibly at
		// this size in the only font available.
		if (h % 3 == 0) {
			z_win_hw_line(&win, x0 + 1, y0, x1 + 1, y1, 1);
		}
	}

	draw_hub();

}

// Digital: HH:MM:SS on one line in 6x12, with the date under it and
// the "UTC" label under that.
//
// The label is not decoration. This clock shows UTC and nothing
// converts it, so on a machine anywhere other than Britain in winter
// the displayed time is deliberately not local -- and an unlabelled
// clock showing the wrong hour reads as broken rather than as
// correct-but-elsewhere.
//
// z_font_6x12 rather than the 5x8 most apps use: it is the larger of
// the two fonts wm loads into glyph memory, so it is the biggest text
// available through the hardware blitter, and a clock is a thing you
// read from across a room.
static void draw_digital(const z_tm_t *tm, bool valid) {

	char line[32];

	int lh = z_font_6x12.h + 2;
	int text_w;
	int y = clock_y + (clock_h - lh * 3) / 2;
	if (y < clock_y) y = clock_y;

	// Erase just the three text lines rather than the whole area --
	// same reasoning as the analog view's hand erasing, and the same
	// benefit (no flash).
	z_win_fill_rect(&win, clock_x, y, clock_w, lh * 3, 0);

	if (valid) {
		snprintf(line, sizeof(line), "%02d:%02d:%02d",
			tm->hour, tm->min, tm->sec);
	} else {
		// Not "00:00:00". The clock does not know the time and saying
		// so is the only honest option -- see this file's header.
		snprintf(line, sizeof(line), "--:--:--");
	}

	// 8 characters at 6px. Centred by measuring rather than by a
	// hardcoded offset, so this survives a font change.
	text_w = (int)strlen(line) * z_font_6x12.w;
	z_win_draw_text(&win, clock_x + (clock_w - text_w) / 2, y, line, 1,
		&z_font_6x12);

	if (valid) {
		// Short weekday and month names keep this to 15 characters,
		// 90px, which fits the narrow window with room either side.
		// A numeric date would be shorter still and harder to read at
		// a glance, which is the wrong trade for a wall clock.
		snprintf(line, sizeof(line), "%s %02d %s %04ld",
			z_wday_name(tm->wday), tm->day, z_month_name(tm->month),
			(long)tm->year);
	} else {
		snprintf(line, sizeof(line), "not set");
	}

	text_w = (int)strlen(line) * z_font_6x12.w;
	z_win_draw_text(&win, clock_x + (clock_w - text_w) / 2, y + lh, line, 1,
		&z_font_6x12);

	text_w = 3 * z_font_6x12.w;
	z_win_draw_text(&win, clock_x + (clock_w - text_w) / 2, y + lh * 2, "UTC", 1,
		&z_font_6x12);

}

// Moves the hands to `tm`, erasing wherever they were.
static void draw_hands(const z_tm_t *tm, bool valid) {

	int hour_pos = (tm->hour % 12) * 5 + tm->min / 12;
	int min_pos = tm->min;
	int sec_pos = tm->sec;

	int hr = (radius * HOUR_HAND_NUM) / HAND_MAX_DEN;
	int mr = (radius * MIN_HAND_NUM) / HAND_MAX_DEN;
	int sr = (radius * SEC_HAND_NUM) / HAND_MAX_DEN;

	// Erase first, all three, before drawing any -- see this file's
	// header on why they move as a set.
	if (last_pos[0] >= 0) {
		draw_hand(last_pos[0], hr, 0);
		draw_hand(last_pos[1], mr, 0);
		draw_hand(last_pos[2], sr, 0);
	}

	if (!valid) {
		// Hands parked at twelve and left there. A dial with no hands
		// at all reads as a drawing error; a stopped one reads as a
		// stopped clock, which is exactly what this is.
		hour_pos = min_pos = sec_pos = 0;
	}

	draw_hand(hour_pos, hr, 1);
	draw_hand(min_pos, mr, 1);
	draw_hand(sec_pos, sr, 1);

	// The hub goes back on top -- the erases above cut through it.
	draw_hub();

	last_pos[0] = hour_pos;
	last_pos[1] = min_pos;
	last_pos[2] = sec_pos;

}

// Everything except the buttons. Called on a redraw, on a view switch,
// and any time the hands' erase record has to be thrown away.
static void repaint_clock_area(void) {

	z_win_fill_rect(&win, clock_x, clock_y, clock_w, clock_h, 0);

	last_pos[0] = last_pos[1] = last_pos[2] = -1;
	have_shown = false;

	if (!rtc_ok) {
		// One clear sentence rather than a broken clock face. This is
		// a bitstream problem and the fix is a specific command, so
		// say which.
		// Wrapped by hand to the narrow window: 20 columns of
		// z_font_6x12 is 120px, which is what clock_w allows.
		z_win_draw_text(&win, clock_x, clock_y + 8,
			"No RTC in\nthis bitstream.\n\nEnable `RTC in\n"
			"rtl/boards.vh\nthen: make flash", 1, &z_font_6x12);
		return;
	}

	if (analog_view) draw_dial();

}

// Reads the clock and updates whatever view is showing, if the second
// has changed. Cheap and safe to call as often as the main loop likes.
static void tick(bool force) {

	if (!rtc_ok) return;

	bool valid = z_rtc_valid();
	uint32_t utc = z_rtc_seconds();

	if (!force && have_shown && valid && utc == last_shown_sec) return;
	if (!force && have_shown && !valid) return;	// nothing changes while unset

	z_tm_t tm;
	z_time_to_tm(utc, &tm);

	if (analog_view) draw_hands(&tm, valid);
	else draw_digital(&tm, valid);

	last_shown_sec = utc;
	have_shown = true;

}

static void layout(void) {

	int cw = z_win_content_w(&win);
	int ch = z_win_content_h(&win);

	int btn_y = ch - BTN_H - MARGIN;

	// The two buttons split the row evenly. Widths derived rather
	// than fixed so the row fills the window exactly, with no ragged
	// gap at one end.
	int avail = cw - 2 * MARGIN - BTN_GAP;
	int bw = avail / 2;
	if (bw < 8) bw = 8;

	widgets[W_ANALOG].x = (int16_t)MARGIN;
	widgets[W_DIGITAL].x = (int16_t)(MARGIN + bw + BTN_GAP);

	for (int i = 0; i < W_COUNT; i++) {
		widgets[i].y = (int16_t)btn_y;
		widgets[i].w = (int16_t)bw;
		widgets[i].h = BTN_H;
	}

	clock_x = MARGIN;
	clock_y = MARGIN;
	clock_w = cw - 2 * MARGIN;
	clock_h = btn_y - MARGIN - clock_y;
	if (clock_h < 8) clock_h = 8;

	// Dial centre, in ABSOLUTE screen coordinates -- z_win_hw_line()
	// takes those, unlike z_win_fill_rect()/z_win_draw_text() which
	// are content-relative. Mixing the two up is the easiest mistake
	// to make in this file, hence computing both here, once, and
	// naming them differently.
	ccx = clock_x + clock_w / 2;
	ccy = clock_y + clock_h / 2;

	z_clip_t content;
	z_win_content_rect(&win, &content);
	cx = content.x0 + ccx;
	cy = content.y0 + ccy;

	radius = (clock_w < clock_h ? clock_w : clock_h) / 2 - 2;
	if (radius < 8) radius = 8;

	z_widget_invalidate(&wset);

}

static void repaint(void) {
	// wm clears before most redraws but NOT after a move, so anything
	// not actively rewritten keeps its pre-move contents.
	z_win_clear(&win);
	repaint_clock_area();
	z_widget_draw_all(&wset, true);
	tick(true);
}

static void activate(int idx) {

	if (idx != W_ANALOG && idx != W_DIGITAL) return;

	bool want = (idx == W_ANALOG);
	if (want == analog_view) return;

	analog_view = want;
	repaint_clock_area();
	tick(true);

}

static void handle_mouse(uint32_t packed) {

	int mx, my;
	bool inside = z_win_mouse_content_xy(&win, packed, &mx, &my);
	uint8_t buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);

	// Samples over the titlebar reach us too -- wm's hit test is the
	// whole window rect. Without this, clicking the close icon also
	// lands on whatever widget sits at the clamped coordinates. Same
	// guard as sw/apps/settings and sw/apps/text.
	if (!inside && wset.pressed < 0) return;

	int act = z_widget_mouse(&wset, mx, my, buttons);
	if (act >= 0) activate(act);

}

static void handle_key(uint32_t keysym, uint8_t mods) {

	switch (keysym) {

		case '\t':
			z_widget_focus_next(&wset, (mods & Z_KBD_MOD_SHIFT) != 0);
			z_widget_draw_all(&wset, false);
			return;

		case Z_KEY_LEFT:
			z_widget_focus_next(&wset, true);
			z_widget_draw_all(&wset, false);
			return;

		case Z_KEY_RIGHT:
			z_widget_focus_next(&wset, false);
			z_widget_draw_all(&wset, false);
			return;

		case 0x0d:		// Enter
		case ' ':
			activate(z_widget_key_activate(&wset));
			return;

		default:
			return;

	}

}

static void widgets_init(void) {

	memset(widgets, 0, sizeof(widgets));

	widgets[W_ANALOG].type = Z_WIDGET_TOGGLE;
	widgets[W_ANALOG].group = GROUP_VIEW;
	widgets[W_ANALOG].label = "Analog";
	widgets[W_ANALOG].enabled = true;

	widgets[W_DIGITAL].type = Z_WIDGET_TOGGLE;
	widgets[W_DIGITAL].group = GROUP_VIEW;
	widgets[W_DIGITAL].label = "Digital";
	widgets[W_DIGITAL].enabled = true;

	z_widget_set_init(&wset, widgets, W_COUNT, &win);

	z_widget_select(&wset, analog_view ? W_ANALOG : W_DIGITAL);
	z_widget_focus_set(&wset, analog_view ? W_ANALOG : W_DIGITAL);

}

int main(void) {

	printf("clock: starting\n");

	// z_rtc_available(), NOT z_rtc_present(): on a bitstream built
	// before rtl/rtc.v existed, the RTC's own registers may not be
	// decoded at all, and an undecoded read on this bus never acks --
	// the CPU would hang here rather than returning a wrong answer.
	// z_rtc_available() checks the CSR feature bit first, at an
	// address every bitstream decodes. See sw/common/zrtc.h.
	rtc_ok = z_rtc_available();

	if (!rtc_ok)
		printf("clock: this bitstream has no RTC -- check `RTC in "
			"rtl/boards.vh, then `make flash` (it's gateware, so "
			"`make dev-flash` won't do it)\n");
	else if (!z_rtc_valid())
		printf("clock: RTC present but not set -- waiting for net's "
			"ntp sync (`run net`)\n");

	if (z_win_create_flags(&win, "clock", WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER) != Z_OK) {
		printf("clock: failed to create window -- is wm running?\n");
		return 1;
	}

	// Claimed and discarded: this app takes no argument, but leaving
	// one pending would hand it to whatever the user opens next.
	{
		char ignored[8];
		z_launch_arg_take(ignored, sizeof(ignored));
	}

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

				case Z_WM_REDRAW:

					if (msg.obj.type != Z_UINT32) break;
					if (z_win_redraw_id(msg.obj.val.uint32) != win.id) break;

					z_win_apply_redraw(&win, msg.obj.val.uint32);
					// The window may have moved, so the dial's
					// absolute centre has to be recomputed before
					// anything is drawn -- z_win_hw_line() takes
					// screen coordinates, and stale ones would draw
					// the hands where the window used to be.
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

		tick(false);

		// Wake ~8 times a second rather than once.
		//
		// Not for smoothness -- the second hand only has 60 positions
		// and tick() does nothing when the second hasn't changed. It
		// is so that a hand moves within ~125ms of the second actually
		// turning over, instead of drifting up to a full second behind
		// it. A clock whose second hand is visibly out of step with
		// the digits looks broken even though both are right.
		//
		// The cost is a wakeup that does three register reads and
		// returns, eight times a second.
		z_proc_wait(Z_TICK_HZ / 8);

	}

	return 0;

}
