/*
 * logic -- a logic analyser, pin driver, pattern generator and I2C
 * decoder for the GPIO ports (rtl/gpio.v, docs/gpio.md).
 *
 *   > run wm
 *   > run logic
 *
 * -- What this is for --
 *
 * Bit-banging a bus is much easier when you can SEE the bus. Before
 * this existed, debugging sw/common/zi2c.c on real hardware meant a
 * scope, or printf and guesswork. This puts the instrument on the same
 * machine as the pins.
 *
 * Four things, in the order you reach for them:
 *
 *   CHANNELS   drive a pin high or low, or watch it. A breakout box
 *              with switches and lamps -- the first thing anyone wants
 *              is to wiggle a pin and confirm it moved.
 *   CAPTURE    sample all eight pins into a buffer and draw them.
 *   MEASURE    frequency and duty from the captured trace.
 *   DECODE     read a captured I2C transaction back as bytes.
 *
 * -- It is honestly a slow instrument, and it says so --
 *
 * Sampling is a CPU loop reading one wishbone register. That puts the
 * ceiling somewhere around 1-2 MSa/s and the floor of the loop
 * overhead is not something a faster timebase setting can get under.
 * So the rate you ask for is a REQUEST, and the readout always shows
 * what was actually achieved, measured with rdcycle across the
 * capture. A bench instrument that lies about its timebase is worse
 * than one with a slow timebase.
 *
 * If this proves too slow to be useful, the fix is a hardware sampler
 * in rtl/gpio.v writing to a BRAM FIFO -- not a tighter loop here. See
 * docs/logic_app.md.
 *
 * -- Interrupts are masked during a capture, and that is bounded --
 *
 * A sample loop that gets descheduled leaves a gap in the trace with
 * no marker where it happened, which is worse than a shorter trace:
 * you cannot tell a gap from a slow signal. So captures run with
 * maskirq() (sw/common/zeitlos.h).
 *
 * WHICH MEANS THE WHOLE MACHINE STOPS for the duration -- no timer, no
 * keyboard, no display updates. That is fine for a few milliseconds
 * and unacceptable for a few hundred, so the capture length is capped
 * at CAPTURE_MAX_MS and the app REDUCES THE DEPTH to fit rather than
 * silently taking the machine away. The readout says when it did.
 *
 * That cap is the real constraint on this instrument, and it is the
 * same trade a real analyser makes between memory depth and timebase.
 * At 20ms you get 2048 samples at 100kSa/s or 200 at 10kSa/s.
 *
 * -- Keyboard-only operation --
 *
 * Tab moves between controls, Enter and Space activate. Same as
 * sw/apps/settings, and for the same reason: this app is most useful
 * exactly when something is wrong, and requiring a pointer to reach a
 * button is a dead end for anyone whose pointer is the thing that is
 * wrong.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zcycles.h"
#include "../../common/zgpio.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zgfx.h"
#include "../../common/zfont.h"
#include "../../common/zkbd.h"
#include "../../common/zwidget.h"

// Fixed size, and large: this is an instrument face, and every part of
// it needs to be visible at once.
//
// LAID OUT IN CONTENT COORDINATES, and the window is sized from them
// rather than the other way round. z_win_content_rect() insets by 2px
// on every content-bearing edge AND the titlebar takes
// Z_WM_TITLEBAR_H off the top, so a window is 4px wider and 15px
// taller than the area an app may draw in (zwin.c).
//
// Getting that backwards is what the first version of this panel did:
// it treated the window size as the content size, so the bottom 15
// rows -- most of the readout well -- were simply off the window.
// sw/apps/logic/tests/test_layout.c now asserts the relationship so it
// cannot come back.
//
// Not resizable. A rack instrument's panel does not resize, and the
// waveform area is the only thing that would benefit -- at the cost of
// re-laying-out four panels' worth of controls on every drag.
#define CONTENT_W 576
#define CONTENT_H 356

#define WIN_W (CONTENT_W + 4)
#define WIN_H (CONTENT_H + Z_WM_TITLEBAR_H + 4)

// How long the machine may be frozen for one capture. See the header.
//
// 20ms is about fifteen KTIMER slices. Long enough to be a visible
// hitch if you are watching a clock, short enough that nothing times
// out and no keystroke is lost (the PS/2 controller buffers).
#define CAPTURE_MAX_MS 20

#define MAX_DEPTH 2048


// One byte per sample: all eight pins of one port, which is exactly
// what one read of the IN register gives. Sampling two ports would
// mean two reads per sample and half the rate for a case nothing has
// needed yet.
static uint8_t samples[MAX_DEPTH];
static uint32_t sample_count;
static uint32_t sample_cycles;		// measured, for the real rate

static z_win_t win;

// -- panel geometry ---------------------------------------------

#define MARGIN    5
#define FH        (z_font_5x8.h)		// 8
#define LINE      (FH + 2)

// left: the channel strip
#define CH_X      MARGIN
#define CH_W      196
#define CH_ROW_H  24
#define CH_TOP    36

// right: the waveform screen
#define WV_X      (CH_X + CH_W + 6)
#define WV_TOP    CH_TOP
#define WV_W      (CONTENT_W - WV_X - MARGIN - 2)
#define WV_ROW_H  24
#define WV_H      (8 * WV_ROW_H)

// bottom: the control deck
#define DECK_TOP  (CH_TOP + 8 * CH_ROW_H + 8)

// WV_ROW_H == CH_ROW_H is deliberate, not a coincidence: channel 7's
// row in the strip lines up with trace 7 on the screen beside it, so
// the eye can go straight across from a lamp to its waveform. Change
// one and change the other.

// -- state ------------------------------------------------------

typedef enum { M_IN = 0, M_OUT, M_OD } chmode_t;

static uint32_t nports;
static uint32_t port;

static chmode_t mode[8];
static bool drive_hi[8];

// Live input, as last polled. Kept so the lamps and the redraw path
// agree -- repainting from a fresh read would make the lamps flicker
// against a trace captured a moment earlier.
static uint8_t in_live;

// timebase, in requested samples per second. 0 means "as fast as the
// loop goes", which is a real setting on an instrument this slow --
// it is the only one that shows what the hardware can actually do.
static const uint32_t rates[] = {
	0, 1000000, 500000, 200000, 100000, 50000, 20000, 10000, 5000, 1000
};
#define RATE_COUNT (int)(sizeof(rates) / sizeof(rates[0]))
static int rate_idx = 4;			// 100 kSa/s

static const uint32_t depths[] = { 128, 256, 512, 1024, 2048 };
#define DEPTH_COUNT (int)(sizeof(depths) / sizeof(depths[0]))
static int depth_idx = 3;			// 1024

typedef enum { T_NONE = 0, T_RISE, T_FALL } trig_t;
static trig_t trig_edge = T_NONE;
static int trig_ch = 0;

static int gen_ch = 0;
static const uint32_t gen_hz[] = { 1000, 5000, 10000, 50000, 100000 };
#define GEN_COUNT (int)(sizeof(gen_hz) / sizeof(gen_hz[0]))
static int gen_idx = 2;

static int scl_ch = 0, sda_ch = 1;

// What the readout says. Two lines: a status line and a decode line.
static char status[120];
static char decode[96];

// Depth actually used by the last capture, which may be less than the
// selected depth -- see CAPTURE_MAX_MS.
static bool depth_was_clamped;

// -- widgets ----------------------------------------------------
//
// Order matters only in that Tab walks it, so the array is laid out in
// the order a hand would move across the panel: channels top to
// bottom, then the deck left to right.

enum {
	W_PORT_DN, W_PORT_UP,
	W_MODE0, W_MODE7 = W_MODE0 + 7,
	W_LVL0,  W_LVL7  = W_LVL0 + 7,
	W_RATE_DN, W_RATE_UP,
	W_DEPTH_DN, W_DEPTH_UP,
	W_TRIG_CH, W_TRIG_EDGE,
	W_RUN,
	W_GEN_CH, W_GEN_HZ, W_GEN_GO,
	W_SCL, W_SDA, W_DECODE,
	W_COUNT
};

static z_widget_t widgets[W_COUNT];
static z_widget_set_t wset;

// Labels that change are held here rather than pointed at literals,
// because z_widget_t.label is a borrowed pointer the toolkit reads at
// draw time -- so it has to stay valid and it has to be ours to
// rewrite.
static char lbl_mode[8][4];
static char lbl_lvl[8][3];
static char lbl_port[12], lbl_rate[16], lbl_depth[12];
static char lbl_trig_ch[4], lbl_trig_edge[6];
static char lbl_gen_ch[4], lbl_gen_hz[16];
static char lbl_scl[8], lbl_sda[8];

static const char *mode_name(chmode_t m) {
	return m == M_IN ? "IN" : (m == M_OUT ? "OUT" : "OD");
}

// -- small drawing helpers --------------------------------------
//
// The panel look: recessed wells for displays, raised frames for
// groups. One pixel of black on white is all a 1bpp display has, so
// "recessed" is a box with a doubled top-left edge and "raised" is a
// box with a doubled bottom-right one. Cheap, and it reads correctly
// at this size.

// z_win_hw_line() and z_win_hw_box() take ABSOLUTE SCREEN
// COORDINATES. Everything else here -- z_win_fill_rect(),
// z_win_draw_text(), zwidget.c -- is content-relative. zwin.h says so;
// it is easy to miss because the two families sit next to each other
// and take identical-looking arguments.
//
// This file had it wrong: every frame, rule and lamp outline was
// drawn at the window's SCREEN position instead of its content
// origin, so the boxes were off by (2, Z_WM_TITLEBAR_H+2) with the
// window at the top-left of the screen and off by the whole window
// position anywhere else. That is the real reason this panel's boxes
// did not contain their controls -- it was diagnosed twice as
// spacing, and twice the spacing was fine.
//
// Found by rendering the panel on the build machine rather than by
// asserting about it; see sw/apps/mmod/tests/render.c.
static void abs_box(int x0, int y0, int x1, int y1, int color) {
	z_clip_t clip;
	z_win_content_rect(&win, &clip);
	z_win_hw_box(&win, clip.x0 + x0, clip.y0 + y0,
		clip.x0 + x1, clip.y0 + y1, color);
}

static void abs_line(int x0, int y0, int x1, int y1, int color) {
	z_clip_t clip;
	z_win_content_rect(&win, &clip);
	z_win_hw_line(&win, clip.x0 + x0, clip.y0 + y0,
		clip.x0 + x1, clip.y0 + y1, color);
}

static void frame_raised(int x, int y, int w, int h) {
	abs_box(x, y, x + w - 1, y + h - 1, 1);
	abs_line(x + 1, y + h - 2, x + w - 2, y + h - 2, 1);
	abs_line(x + w - 2, y + 1, x + w - 2, y + h - 2, 1);
}

static void frame_well(int x, int y, int w, int h) {
	abs_box(x, y, x + w - 1, y + h - 1, 1);
	abs_line(x + 1, y + 1, x + w - 2, y + 1, 1);
	abs_line(x + 1, y + 1, x + 1, y + h - 2, 1);
}

// A section heading in the style of a silkscreened panel label: text
// with a rule running out to the right of it.
static void panel_label(int x, int y, int w, const char *s) {
	int tw = (int)strlen(s) * z_font_5x8.w;
	z_win_draw_text(&win, x, y, s, 1, &z_font_5x8);
	if (w > tw + 6)
		abs_line(x + tw + 4, y + FH / 2, x + w - 1, y + FH / 2, 1);
}

// An indicator lamp. Filled when lit, hollow when not -- a ring rather
// than a dot when dark, so "off" is visibly a lamp rather than blank
// panel.
static void lamp(int x, int y, bool lit) {
	if (lit) {
		z_win_fill_rect(&win, x, y, 7, 7, 1);
		z_win_fill_rect(&win, x + 2, y + 2, 3, 3, 0);
		z_win_fill_rect(&win, x + 3, y + 3, 1, 1, 1);
	} else {
		abs_box(x, y, x + 6, y + 6, 1);
	}
}

// -- layout -----------------------------------------------------
//
// The deck is laid out with a CURSOR rather than with absolute
// coordinates, and every label, readout well and button in a group
// takes its position from the same walk. Hand-placed constants for
// this many controls drift the moment a label changes length, and the
// failure is a button sitting on top of a readout -- visible, but only
// once it is on a screen. This way a wrong number moves a whole group
// and cannot overlap its neighbour.

typedef struct { int16_t lbl_x, well_x, well_w; } grp_t;

// The three drawn panel frames, in content coordinates.
//
// Computed in layout() and read by the draw functions, rather than
// each draw call recomputing the same expression. That is not tidiness:
// nothing could check that a widget stayed INSIDE its own box while
// the boxes existed only as arguments at the call site, and the first
// version of this panel put the PORT buttons across the top edge of
// the channel frame for exactly that reason.
// sw/apps/logic/tests/test_layout.c now asserts containment.
typedef struct { int16_t x, y, w, h; } rect_t;
static rect_t strip_f, wave_f, deck_f;

static grp_t g_rate, g_depth, g_trig, g_gen, g_i2c, g_port;
static int deck_y1, deck_y2, read_y, read_h;

#define BTN_SM_W  14
#define BTN_SM_H  13
#define TXT_W(n)  ((n) * z_font_5x8.w)

static int put_lbl(int cx, int16_t *out, int chars) {
	*out = (int16_t)cx;
	return cx + TXT_W(chars) + 4;
}

static int put_btn(int cx, int y, int idx, int w) {
	widgets[idx].x = (int16_t)cx;
	widgets[idx].y = (int16_t)y;
	widgets[idx].w = (int16_t)w;
	widgets[idx].h = BTN_SM_H;
	return cx + w + 2;
}

static int put_well(int cx, grp_t *g, int w) {
	g->well_x = (int16_t)cx;
	g->well_w = (int16_t)w;
	return cx + w + 2;
}

static void layout(void) {

	int i, y, cx;

	// -- channel strip --

	cx = put_lbl(CH_X, &g_port.lbl_x, 4);				// "PORT"
	cx = put_btn(cx, MARGIN + 10, W_PORT_DN, BTN_SM_W);
	cx = put_well(cx, &g_port, 18);
	cx = put_btn(cx, MARGIN + 10, W_PORT_UP, BTN_SM_W);

	for (i = 0; i < 8; i++) {
		y = CH_TOP + i * CH_ROW_H;
		widgets[W_MODE0 + i].x = (int16_t)(CH_X + 30);
		widgets[W_MODE0 + i].y = (int16_t)(y + 1);
		widgets[W_MODE0 + i].w = 32;
		widgets[W_MODE0 + i].h = 15;
		widgets[W_LVL0 + i].x = (int16_t)(CH_X + 66);
		widgets[W_LVL0 + i].y = (int16_t)(y + 1);
		widgets[W_LVL0 + i].w = 26;
		widgets[W_LVL0 + i].h = 15;
	}

	// -- deck, row 1: timebase --

	deck_y1 = DECK_TOP + 10;
	cx = MARGIN + 4;

	cx = put_lbl(cx, &g_rate.lbl_x, 4);					// "RATE"
	cx = put_btn(cx, deck_y1, W_RATE_DN, BTN_SM_W);
	cx = put_well(cx, &g_rate, 40);
	cx = put_btn(cx, deck_y1, W_RATE_UP, BTN_SM_W);
	cx += 10;

	cx = put_lbl(cx, &g_depth.lbl_x, 5);				// "DEPTH"
	cx = put_btn(cx, deck_y1, W_DEPTH_DN, BTN_SM_W);
	cx = put_well(cx, &g_depth, 30);
	cx = put_btn(cx, deck_y1, W_DEPTH_UP, BTN_SM_W);
	cx += 10;

	cx = put_lbl(cx, &g_trig.lbl_x, 4);					// "TRIG"
	cx = put_btn(cx, deck_y1, W_TRIG_CH, 18);
	cx = put_btn(cx, deck_y1, W_TRIG_EDGE, 38);

	// RUN, right-aligned and taller than everything else. The one
	// button you press repeatedly gets its own space and its own size.
	widgets[W_RUN].x = (int16_t)(CONTENT_W - MARGIN - 6 - 62);
	widgets[W_RUN].y = (int16_t)(DECK_TOP + 6);
	widgets[W_RUN].w = 62;
	widgets[W_RUN].h = 21;

	// -- deck, row 2: generator and decoder --

	deck_y2 = DECK_TOP + 30;
	cx = MARGIN + 4;

	cx = put_lbl(cx, &g_gen.lbl_x, 3);					// "GEN"
	cx = put_btn(cx, deck_y2, W_GEN_CH, 18);
	cx = put_btn(cx, deck_y2, W_GEN_HZ, 44);
	cx = put_btn(cx, deck_y2, W_GEN_GO, 40);
	cx += 14;

	cx = put_lbl(cx, &g_i2c.lbl_x, 3);					// "I2C"
	cx = put_btn(cx, deck_y2, W_SCL, 34);
	cx = put_btn(cx, deck_y2, W_SDA, 34);
	cx = put_btn(cx, deck_y2, W_DECODE, 46);

	// -- the readout well, filling what is left --

	read_y = DECK_TOP + 48;
	read_h = CONTENT_H - MARGIN - read_y - 2;

	// -- the frames the above sits inside --

	strip_f.x = CH_X - 2;
	strip_f.y = CH_TOP - 4;
	strip_f.w = CH_W + 4;
	strip_f.h = (int16_t)(8 * CH_ROW_H + 6);

	wave_f.x = (int16_t)(WV_X - 2);
	wave_f.y = (int16_t)(WV_TOP - 4);
	wave_f.w = (int16_t)(WV_W + 4);
	wave_f.h = (int16_t)(WV_H + 6);

	deck_f.x = MARGIN - 2;
	deck_f.y = (int16_t)(DECK_TOP - 2);
	deck_f.w = (int16_t)(CONTENT_W - 2 * MARGIN + 4);
	deck_f.h = (int16_t)(CONTENT_H - DECK_TOP - MARGIN + 2);

	z_widget_invalidate(&wset);

}

// -- label upkeep -----------------------------------------------

static void relabel(void) {

	int i;

	snprintf(lbl_port, sizeof(lbl_port), "%lu", (unsigned long)port);

	for (i = 0; i < 8; i++) {
		int pin = 7 - i;
		snprintf(lbl_mode[i], sizeof(lbl_mode[i]), "%s", mode_name(mode[pin]));
		snprintf(lbl_lvl[i], sizeof(lbl_lvl[i]), "%s",
			mode[pin] == M_IN ? "-" : (drive_hi[pin] ? "HI" : "LO"));
		widgets[W_MODE0 + i].dirty = true;
		widgets[W_LVL0 + i].dirty = true;
		widgets[W_LVL0 + i].enabled = (mode[pin] != M_IN);
		widgets[W_LVL0 + i].on = (mode[pin] != M_IN) && drive_hi[pin];
	}

	if (rates[rate_idx] == 0)
		snprintf(lbl_rate, sizeof(lbl_rate), "MAX");
	else if (rates[rate_idx] >= 1000)
		snprintf(lbl_rate, sizeof(lbl_rate), "%luk",
			(unsigned long)(rates[rate_idx] / 1000));
	else
		snprintf(lbl_rate, sizeof(lbl_rate), "%lu",
			(unsigned long)rates[rate_idx]);

	snprintf(lbl_depth, sizeof(lbl_depth), "%lu",
		(unsigned long)depths[depth_idx]);

	snprintf(lbl_trig_ch, sizeof(lbl_trig_ch), "%d", trig_ch);
	snprintf(lbl_trig_edge, sizeof(lbl_trig_edge), "%s",
		trig_edge == T_NONE ? "FREE" : (trig_edge == T_RISE ? "RISE" : "FALL"));

	snprintf(lbl_gen_ch, sizeof(lbl_gen_ch), "%d", gen_ch);
	if (gen_hz[gen_idx] >= 1000)
		snprintf(lbl_gen_hz, sizeof(lbl_gen_hz), "%lukHz",
			(unsigned long)(gen_hz[gen_idx] / 1000));
	else
		snprintf(lbl_gen_hz, sizeof(lbl_gen_hz), "%luHz",
			(unsigned long)gen_hz[gen_idx]);

	snprintf(lbl_scl, sizeof(lbl_scl), "SCL%d", scl_ch);
	snprintf(lbl_sda, sizeof(lbl_sda), "SDA%d", sda_ch);

	widgets[W_PORT_DN].dirty = true;
	widgets[W_RATE_DN].dirty = true;
	widgets[W_DEPTH_DN].dirty = true;
	widgets[W_TRIG_CH].dirty = true;
	widgets[W_TRIG_EDGE].dirty = true;
	widgets[W_GEN_CH].dirty = true;
	widgets[W_GEN_HZ].dirty = true;
	widgets[W_SCL].dirty = true;
	widgets[W_SDA].dirty = true;

}

// -- pin control ------------------------------------------------

// Push this app's idea of the channel state onto the hardware.
//
// Only the pins this app has been told to drive: a pin left in IN is
// released, which is the reset state and the safe one. Nothing here
// ever drives a pin the user did not ask for.
static void apply_pins(void) {

	int p;

	for (p = 0; p < 8; p++) {
		switch (mode[p]) {
		case M_IN:
			z_gpio_mode(port, (uint32_t)p, Z_GPIO_IN);
			break;
		case M_OUT:
			z_gpio_mode(port, (uint32_t)p, Z_GPIO_OUT);
			z_gpio_write(port, (uint32_t)p, drive_hi[p]);
			break;
		case M_OD:
			// Open drain: OUT parked at 0 by the mode call, and the
			// level moves DIR instead. "HI" here means released, not
			// driven high -- which is what open drain means, and why
			// the label reads HI/LO rather than DRIVE/FLOAT.
			z_gpio_mode(port, (uint32_t)p, Z_GPIO_OD);
			z_gpio_od_write(port, (uint32_t)p, drive_hi[p]);
			break;
		}
	}

}

// -- the capture engine -----------------------------------------

// How many samples fit inside CAPTURE_MAX_MS at this rate.
static uint32_t depth_budget(uint32_t rate) {
	uint32_t max_cycles = (Z_SYSCLK_HZ / 1000u) * CAPTURE_MAX_MS;
	if (!rate) return MAX_DEPTH;		// MAX rate is bounded by the loop
	{
		uint32_t per = Z_SYSCLK_HZ / rate;
		if (!per) return MAX_DEPTH;
		return max_cycles / per;
	}
}

// Sample `n` bytes of the port's IN register into samples[].
//
// INTERRUPTS ARE OFF for the whole of this, including the trigger
// wait. Both are bounded -- see the header on why, and depth_budget()
// on how the sample count is clamped to fit.
//
// The trigger wait has its own, separate budget: a trigger that never
// arrives must not hold the machine for longer than a capture would,
// and "no trigger" is a normal outcome on a bus that is idle.
static bool do_capture(void) {

	volatile uint32_t *in_reg = z_gpio_reg(port, Z_GPIO_REG_IN);
	uint32_t rate = rates[rate_idx];
	uint32_t want = depths[depth_idx];
	uint32_t budget = depth_budget(rate);
	uint32_t per = rate ? (Z_SYSCLK_HZ / rate) : 0;
	uint32_t trig_budget = (Z_SYSCLK_HZ / 1000u) * CAPTURE_MAX_MS;
	uint32_t i, t0, next, saved;
	uint8_t trig_mask = (uint8_t)(1u << trig_ch);
	bool triggered = true;

	depth_was_clamped = false;

	if (want > budget) {
		want = budget;
		depth_was_clamped = true;
	}
	if (want > MAX_DEPTH) want = MAX_DEPTH;
	if (want < 2) want = 2;

	saved = maskirq(~0u);

	// -- trigger --
	if (trig_edge != T_NONE) {

		uint8_t prev = (uint8_t)(*in_reg & 0xffu);
		uint32_t tstart = z_cycles();

		triggered = false;

		for (;;) {
			uint8_t now = (uint8_t)(*in_reg & 0xffu);
			bool was = (prev & trig_mask) != 0;
			bool is = (now & trig_mask) != 0;
			if (trig_edge == T_RISE && !was && is) { triggered = true; break; }
			if (trig_edge == T_FALL && was && !is) { triggered = true; break; }
			prev = now;
			if ((z_cycles() - tstart) > trig_budget) break;
		}

	}

	// -- sample --
	//
	// Two loops rather than one with a conditional inside: at MAX
	// rate the whole point is that nothing sits between the reads,
	// and a branch per sample is a measurable fraction of the loop
	// at these speeds.
	t0 = z_cycles();

	if (!per) {
		for (i = 0; i < want; i++)
			samples[i] = (uint8_t)(*in_reg & 0xffu);
	} else {
		next = t0;
		for (i = 0; i < want; i++) {
			samples[i] = (uint8_t)(*in_reg & 0xffu);
			next += per;
			while ((int32_t)(z_cycles() - next) < 0)
				;
		}
	}

	sample_cycles = z_cycles() - t0;
	sample_count = i;

	maskirq(saved);

	return triggered;

}

// The rate the capture actually achieved, in samples per second.
//
// Measured, never assumed. See the header: the requested rate is a
// request, and on an instrument whose ceiling is a CPU loop the
// difference is often large. Reporting the request would make the
// timebase a lie and every measurement derived from it wrong by the
// same factor.
static uint32_t actual_rate(void) {
	if (!sample_count || !sample_cycles) return 0;
	return (uint32_t)(((uint64_t)sample_count * Z_SYSCLK_HZ) / sample_cycles);
}

// -- the burst generator ----------------------------------------
//
// N cycles of a square wave, then stop. A BURST rather than a
// continuous output, and that is a design choice worth stating: a
// continuous generator would need this process to hold the CPU
// forever, or to run from an interrupt this app does not have. A
// burst is bounded, honest, and enough to trigger something else or
// to check a receiver.

static void do_generate(void) {

	uint32_t hz = gen_hz[gen_idx];
	uint32_t half = (Z_SYSCLK_HZ / hz) / 2u;
	uint32_t cycles = (hz / 1000u) * CAPTURE_MAX_MS;	// ~CAPTURE_MAX_MS worth
	uint32_t i, saved, t;

	if (!half) half = 1;
	if (cycles < 4) cycles = 4;

	// The pin has to be an output for this to do anything. Rather
	// than silently switching it, the app refuses and says so --
	// quietly reconfiguring a pin the user set to IN is how you
	// drive into something that was driving back.
	if (mode[gen_ch] != M_OUT) {
		snprintf(status, sizeof(status),
			"GEN: set channel %d to OUT first", gen_ch);
		return;
	}

	saved = maskirq(~0u);

	t = z_cycles();
	for (i = 0; i < cycles; i++) {
		z_gpio_write(port, (uint32_t)gen_ch, true);
		t += half;
		while ((int32_t)(z_cycles() - t) < 0) ;
		z_gpio_write(port, (uint32_t)gen_ch, false);
		t += half;
		while ((int32_t)(z_cycles() - t) < 0) ;
	}

	maskirq(saved);

	// Left where the pin started, not where the burst ended.
	z_gpio_write(port, (uint32_t)gen_ch, drive_hi[gen_ch]);

	snprintf(status, sizeof(status),
		"GEN: %lu cycles at %lu Hz on ch%d",
		(unsigned long)cycles, (unsigned long)hz, gen_ch);

}

// -- measurement ------------------------------------------------

// Edges on a channel across the whole capture, and the fraction of
// samples that were high.
static void measure(int ch, uint32_t *edges, uint32_t *high) {

	uint8_t m = (uint8_t)(1u << ch);
	uint32_t i, e = 0, h = 0;

	for (i = 0; i < sample_count; i++) {
		if (samples[i] & m) h++;
		if (i && ((samples[i] ^ samples[i - 1]) & m)) e++;
	}

	*edges = e;
	*high = h;

}

// -- I2C decode -------------------------------------------------
//
// Walks a captured trace the way a slave would: START and STOP from
// SDA transitions while SCL is high, data bits sampled on SCL rising.
//
// This decodes what was on the WIRE, which is the point of having it
// here rather than printf in sw/common/zi2c.c -- it will happily show
// you a NACK where the library reported success, or an address one bit
// off from the one you passed in.
//
// Deliberately not a full decoder: no repeated-START distinction from
// a plain one (both show as S), no 10-bit addressing. It has to fit on
// one line of a panel, and the first question is always "did the right
// address go out and did anything answer".
static void do_decode(void) {

	uint32_t i;
	uint8_t sclm = (uint8_t)(1u << scl_ch);
	uint8_t sdam = (uint8_t)(1u << sda_ch);
	int state = 0;			// 0 idle, 1 collecting bits
	int bit = 0, nbyte = 0;
	uint8_t shift = 0;
	size_t k = 0;
	bool prev_scl, prev_sda;

	decode[0] = 0;

	if (!sample_count) {
		snprintf(decode, sizeof(decode), "DECODE: capture something first");
		return;
	}

	if (scl_ch == sda_ch) {
		snprintf(decode, sizeof(decode), "DECODE: SCL and SDA must differ");
		return;
	}

	k += (size_t)snprintf(decode + k, sizeof(decode) - k, "I2C:");

	prev_scl = (samples[0] & sclm) != 0;
	prev_sda = (samples[0] & sdam) != 0;

	for (i = 1; i < sample_count && k < sizeof(decode) - 10; i++) {

		bool scl = (samples[i] & sclm) != 0;
		bool sda = (samples[i] & sdam) != 0;

		if (scl && prev_scl && prev_sda && !sda) {
			k += (size_t)snprintf(decode + k, sizeof(decode) - k, " S");
			state = 1; bit = 0; shift = 0; nbyte = 0;
		}
		else if (scl && prev_scl && !prev_sda && sda) {
			k += (size_t)snprintf(decode + k, sizeof(decode) - k, " P");
			state = 0;
		}
		else if (state && !prev_scl && scl) {

			if (bit < 8) {
				shift = (uint8_t)((shift << 1) | (sda ? 1 : 0));
				bit++;
			} else {
				// the ninth bit is the acknowledgement, and a LOW is
				// the ack -- an active pull-down, so "nobody there"
				// and "device says no" look the same on the wire
				if (nbyte == 0)
					k += (size_t)snprintf(decode + k, sizeof(decode) - k,
						" %02x%c%c", shift >> 1,
						(shift & 1) ? 'r' : 'w', sda ? '-' : '+');
				else
					k += (size_t)snprintf(decode + k, sizeof(decode) - k,
						" %02x%c", shift, sda ? '-' : '+');
				nbyte++;
				bit = 0; shift = 0;
			}

		}

		prev_scl = scl;
		prev_sda = sda;

	}

	if (k <= 4)
		snprintf(decode, sizeof(decode),
			"I2C: no start condition on ch%d/ch%d", scl_ch, sda_ch);

}

// -- painting ---------------------------------------------------

static void draw_channel_strip(void) {

	int i;

	panel_label(CH_X, MARGIN, CH_W, "CHANNELS");

	z_win_draw_text(&win, g_port.lbl_x, MARGIN + 13, "PORT", 1, &z_font_5x8);
	frame_well(g_port.well_x, MARGIN + 10, g_port.well_w, BTN_SM_H);
	z_win_draw_text(&win, g_port.well_x + 6, MARGIN + 13, lbl_port, 1,
		&z_font_5x8);

	frame_raised(strip_f.x, strip_f.y, strip_f.w, strip_f.h);

	for (i = 0; i < 8; i++) {

		int pin = 7 - i;
		int y = CH_TOP + i * CH_ROW_H;
		char n[3];

		snprintf(n, sizeof(n), "%d", pin);
		z_win_draw_text(&win, CH_X + 6, y + 5, n, 1, &z_font_5x8);

		// live lamp: what the PIN is doing, not what we asked for
		lamp(CH_X + 16, y + 5, (in_live & (1u << pin)) != 0);

		// pin-to-connector reminder, since bit order is not pin order
		// on a PMOD (bits 4-7 are the bottom row) -- see docs/gpio.md
		{
			static const char *pmod[8] = {
				"p1","p2","p3","p4","p7","p8","p9","p10"
			};
			z_win_draw_text(&win, CH_X + 98, y + 5, pmod[pin], 1, &z_font_5x8);
		}

	}

}

static void draw_wave_screen(void) {

	int i, x;
	uint32_t rate = actual_rate();

	panel_label(WV_X, MARGIN, 120, "TIMEBASE");

	if (sample_count) {
		char t[48];
		// microseconds across the whole screen. The number that tells
		// you whether you are looking at the thing you meant to.
		uint32_t us = rate ? (uint32_t)(((uint64_t)sample_count * 1000000u) / rate) : 0;
		snprintf(t, sizeof(t), "%lu us / %lu Sa", (unsigned long)us,
			(unsigned long)sample_count);
		z_win_draw_text(&win, WV_X + 130, MARGIN, t, 1, &z_font_5x8);
	}

	frame_well(wave_f.x, wave_f.y, wave_f.w, wave_f.h);

	// graticule: a dotted vertical division every eighth of the
	// screen, the way an instrument does, so you can count time
	// without measuring pixels
	for (i = 1; i < 8; i++) {
		int gx = WV_X + (WV_W * i) / 8;
		int gy;
		for (gy = WV_TOP; gy < WV_TOP + WV_H; gy += 4)
			z_win_fill_rect(&win, gx, gy, 1, 1, 1);
	}

	for (i = 0; i < 8; i++) {

		int ch = 7 - i;
		int base = WV_TOP + i * WV_ROW_H;
		int lo = base + WV_ROW_H - 5;
		int hi = base + 3;
		char n[3];

		snprintf(n, sizeof(n), "%d", ch);
		z_win_draw_text(&win, WV_X + 2, base + (WV_ROW_H - FH) / 2 - 1,
			n, 1, &z_font_5x8);

		if (!sample_count) continue;

		// One column per pixel, and each column is the OR of every
		// sample that falls in it -- so a pulse narrower than a pixel
		// still shows as a transition rather than disappearing
		// depending on where it landed. An instrument that hides
		// narrow pulses at a wide timebase is exactly the wrong tool
		// for finding a glitch.
		{
			int prev_y = -1;
			for (x = 0; x < WV_W - 10; x++) {

				uint32_t s0 = ((uint32_t)x * sample_count) / (uint32_t)(WV_W - 10);
				uint32_t s1 = ((uint32_t)(x + 1) * sample_count) / (uint32_t)(WV_W - 10);
				uint32_t j;
				bool any_hi = false, any_lo = false;
				int px = WV_X + 10 + x;

				if (s1 <= s0) s1 = s0 + 1;
				if (s1 > sample_count) s1 = sample_count;

				for (j = s0; j < s1; j++) {
					if (samples[j] & (1u << ch)) any_hi = true;
					else any_lo = true;
				}

				if (any_hi && any_lo) {
					// both levels in this column: draw the edge
					abs_line(px, hi, px, lo, 1);
					prev_y = -1;
				} else {
					int y = any_hi ? hi : lo;
					z_win_fill_rect(&win, px, y, 1, 1, 1);
					// join to the previous column so a transition
					// between adjacent columns is a visible riser
					// rather than two disconnected levels
					if (prev_y >= 0 && prev_y != y)
						abs_line(px, prev_y, px, y, 1);
					prev_y = y;
				}

			}
		}

	}

}

// The readout well's three lines. Factored out because repaint() and
// repaint_readout() both draw them and had drifted apart once
// already.
static void draw_readout_lines(void) {
	z_win_draw_text(&win, MARGIN + 7, read_y + 4, status, 1, &z_font_5x8);
	z_win_draw_text(&win, MARGIN + 7, read_y + 4 + LINE, decode, 1,
		&z_font_5x8);
	// Static, and worth the row: every control here has a keyboard
	// route and none of them look like they do.
	z_win_draw_text(&win, MARGIN + 7, read_y + 4 + 2 * LINE,
		"R run   D decode   Tab move   Enter/Space press", 1, &z_font_5x8);
}

static void draw_deck(void) {

	frame_raised(deck_f.x, deck_f.y, deck_f.w, deck_f.h);

	// Row 1. Every coordinate comes from layout()'s walk, so a label
	// and its readout cannot end up in different places.
	z_win_draw_text(&win, g_rate.lbl_x, deck_y1 + 3, "RATE", 1, &z_font_5x8);
	frame_well(g_rate.well_x, deck_y1, g_rate.well_w, BTN_SM_H);
	z_win_draw_text(&win, g_rate.well_x + 4, deck_y1 + 3, lbl_rate, 1,
		&z_font_5x8);

	z_win_draw_text(&win, g_depth.lbl_x, deck_y1 + 3, "DEPTH", 1, &z_font_5x8);
	frame_well(g_depth.well_x, deck_y1, g_depth.well_w, BTN_SM_H);
	z_win_draw_text(&win, g_depth.well_x + 4, deck_y1 + 3, lbl_depth, 1,
		&z_font_5x8);

	z_win_draw_text(&win, g_trig.lbl_x, deck_y1 + 3, "TRIG", 1, &z_font_5x8);

	// Row 2.
	z_win_draw_text(&win, g_gen.lbl_x, deck_y2 + 3, "GEN", 1, &z_font_5x8);
	z_win_draw_text(&win, g_i2c.lbl_x, deck_y2 + 3, "I2C", 1, &z_font_5x8);

	// The readout: two lines in a recessed well, which is where an
	// instrument puts the numbers it wants you to read rather than
	// the controls it wants you to press.
	frame_well(MARGIN + 2, read_y, CONTENT_W - 2 * MARGIN - 4, read_h);
	draw_readout_lines();

}

static void repaint(void) {
	z_win_clear(&win);
	draw_channel_strip();
	draw_wave_screen();
	draw_deck();
	z_widget_draw_all(&wset, true);
}

// Cheap partial repaints, so pressing a button does not flash the
// whole panel. The waveform and the readout are the only things that
// change without a widget changing with them.
static void repaint_wave(void) {
	z_win_fill_rect(&win, wave_f.x - 1, wave_f.y - 1,
		wave_f.w + 2, wave_f.h + 2, 0);
	draw_wave_screen();
}

static void repaint_readout(void) {
	z_win_fill_rect(&win, MARGIN + 3, read_y + 1,
		CONTENT_W - 2 * MARGIN - 6, read_h - 2, 0);
	frame_well(MARGIN + 2, read_y, CONTENT_W - 2 * MARGIN - 4, read_h);
	draw_readout_lines();
}

static void repaint_lamps(void) {
	int i;
	for (i = 0; i < 8; i++) {
		int pin = 7 - i;
		int y = CH_TOP + i * CH_ROW_H;
		z_win_fill_rect(&win, CH_X + 16, y + 5, 7, 7, 0);
		lamp(CH_X + 16, y + 5, (in_live & (1u << pin)) != 0);
	}
}

// -- actions ----------------------------------------------------

static void after_capture(bool triggered) {

	uint32_t rate = actual_rate();
	uint32_t e, h;

	if (!triggered) {
		snprintf(status, sizeof(status),
			"NO TRIG: no %s edge on ch%d within %d ms",
			trig_edge == T_RISE ? "rising" : "falling",
			trig_ch, CAPTURE_MAX_MS);
		return;
	}

	measure(trig_ch, &e, &h);

	// Frequency from edge count rather than from one period: a single
	// period measurement on a noisy or aperiodic signal is a number
	// that looks precise and is not. Edges over the whole window is
	// an average, which is what it says it is.
	{
		uint32_t hz = 0;
		uint32_t duty = sample_count ? (h * 100u) / sample_count : 0;

		if (e >= 2 && rate)
			hz = (uint32_t)(((uint64_t)e * rate) / (2u * sample_count));

		snprintf(status, sizeof(status),
			"%lu Sa @ %lu kSa/s%s  ch%d: %lu edges, %lu Hz, %lu%% hi",
			(unsigned long)sample_count,
			(unsigned long)(rate / 1000u),
			depth_was_clamped ? " (depth capped)" : "",
			trig_ch,
			(unsigned long)e, (unsigned long)hz, (unsigned long)duty);
	}

}

static void act(int idx) {

	int i;

	if (idx < 0) return;

	if (idx == W_PORT_DN || idx == W_PORT_UP) {
		if (idx == W_PORT_UP && port + 1 < nports) port++;
		else if (idx == W_PORT_DN && port > 0) port--;
		// Modes are per-port state this app does not keep per port:
		// switching resets the strip to all-inputs, which is the safe
		// answer rather than silently driving the new port's pins the
		// way the old one was set.
		for (i = 0; i < 8; i++) { mode[i] = M_IN; drive_hi[i] = false; }
		apply_pins();
		relabel();
		repaint();
		return;
	}

	if (idx >= W_MODE0 && idx <= W_MODE7) {
		int pin = 7 - (idx - W_MODE0);
		mode[pin] = (chmode_t)((mode[pin] + 1) % 3);
		apply_pins();
		relabel();
		z_widget_draw_all(&wset, false);
		return;
	}

	if (idx >= W_LVL0 && idx <= W_LVL7) {
		int pin = 7 - (idx - W_LVL0);
		if (mode[pin] == M_IN) return;
		drive_hi[pin] = !drive_hi[pin];
		apply_pins();
		relabel();
		z_widget_draw_all(&wset, false);
		return;
	}

	if (idx == W_RATE_DN || idx == W_RATE_UP) {
		if (idx == W_RATE_UP && rate_idx + 1 < RATE_COUNT) rate_idx++;
		else if (idx == W_RATE_DN && rate_idx > 0) rate_idx--;
		relabel(); repaint(); return;
	}

	if (idx == W_DEPTH_DN || idx == W_DEPTH_UP) {
		if (idx == W_DEPTH_UP && depth_idx + 1 < DEPTH_COUNT) depth_idx++;
		else if (idx == W_DEPTH_DN && depth_idx > 0) depth_idx--;
		relabel(); repaint(); return;
	}

	if (idx == W_TRIG_CH) { trig_ch = (trig_ch + 1) & 7; relabel(); repaint(); return; }
	if (idx == W_TRIG_EDGE) {
		trig_edge = (trig_t)((trig_edge + 1) % 3);
		relabel(); repaint(); return;
	}

	if (idx == W_RUN) {
		bool t = do_capture();
		after_capture(t);
		decode[0] = 0;
		repaint_wave();
		repaint_readout();
		return;
	}

	if (idx == W_GEN_CH) { gen_ch = (gen_ch + 1) & 7; relabel(); repaint(); return; }
	if (idx == W_GEN_HZ) {
		gen_idx = (gen_idx + 1) % GEN_COUNT;
		relabel(); repaint(); return;
	}
	if (idx == W_GEN_GO) { do_generate(); repaint_readout(); return; }

	if (idx == W_SCL) { scl_ch = (scl_ch + 1) & 7; relabel(); repaint(); return; }
	if (idx == W_SDA) { sda_ch = (sda_ch + 1) & 7; relabel(); repaint(); return; }
	if (idx == W_DECODE) { do_decode(); repaint_readout(); return; }

}

// -- input ------------------------------------------------------

static void handle_mouse(uint32_t packed) {

	int cx, cy;
	bool inside = z_win_mouse_content_xy(&win, packed, &cx, &cy);
	uint8_t buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);

	// Titlebar samples reach us too -- wm's hit test is the whole
	// window rect. Same guard as sw/apps/settings and sw/apps/term.
	if (!inside && wset.pressed < 0) return;

	act(z_widget_mouse(&wset, cx, cy, buttons));

}

static void handle_key(uint32_t keysym, uint8_t mods) {

	switch (keysym) {

	case '\t':
		z_widget_focus_next(&wset, (mods & Z_KBD_MOD_SHIFT) != 0);
		z_widget_draw_all(&wset, false);
		return;

	case 0x0d:
	case ' ':
		act(z_widget_key_activate(&wset));
		return;

	// 'r' for run, because on an instrument the big button has a
	// shortcut and this is the one you press over and over.
	case 'r':
	case 'R':
		act(W_RUN);
		return;

	case 'd':
	case 'D':
		act(W_DECODE);
		return;

	default:
		return;

	}

}

// -- setup ------------------------------------------------------

static void widgets_init(void) {

	int i;

	memset(widgets, 0, sizeof(widgets));

	for (i = 0; i < W_COUNT; i++) {
		widgets[i].type = Z_WIDGET_BUTTON;
		widgets[i].enabled = true;
	}

	widgets[W_PORT_DN].label = "-";
	widgets[W_PORT_UP].label = "+";

	for (i = 0; i < 8; i++) {
		widgets[W_MODE0 + i].label = lbl_mode[i];
		// The level control is a TOGGLE rather than a button: it has a
		// state the panel should show, and a button that says HI while
		// the pin is low would be a switch that lies about its
		// position.
		widgets[W_LVL0 + i].type = Z_WIDGET_TOGGLE;
		widgets[W_LVL0 + i].label = lbl_lvl[i];
	}

	widgets[W_RATE_DN].label = "-";
	widgets[W_RATE_UP].label = "+";
	widgets[W_DEPTH_DN].label = "-";
	widgets[W_DEPTH_UP].label = "+";
	widgets[W_TRIG_CH].label = lbl_trig_ch;
	widgets[W_TRIG_EDGE].label = lbl_trig_edge;
	widgets[W_RUN].label = "RUN";
	widgets[W_GEN_CH].label = lbl_gen_ch;
	widgets[W_GEN_HZ].label = lbl_gen_hz;
	widgets[W_GEN_GO].label = "BURST";
	widgets[W_SCL].label = lbl_scl;
	widgets[W_SDA].label = lbl_sda;
	widgets[W_DECODE].label = "DECODE";

	z_widget_set_init(&wset, widgets, W_COUNT, &win);
	z_widget_focus_set(&wset, W_RUN);

}

int main(void) {

	printf("logic: starting\n");

	nports = z_gpio_port_count();

	if (!z_gpio_present() || !nports) {
		// Nothing to instrument. Said on the console rather than in a
		// window, because a window whose only content is "there is no
		// hardware" is a window in the way.
		printf("logic: this bitstream has no GPIO ports with pins.\n");
		printf("logic: try `zrelease build obst_uart_gpio` "
			"-- see docs/gpio.md\n");
		return 1;
	}

	if (z_win_create_flags(&win, "logic", WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER) != Z_OK) {
		printf("logic: failed to create window -- is wm running?\n");
		return 1;
	}

	{
		char ignored[8];
		z_launch_arg_take(ignored, sizeof(ignored));
	}

	// Every pin an input at startup, whatever the last app left them
	// as. An instrument that starts by driving pins is the wrong kind
	// of instrument.
	{
		int i;
		for (i = 0; i < 8; i++) { mode[i] = M_IN; drive_hi[i] = false; }
	}
	apply_pins();

	snprintf(status, sizeof(status),
		"%lu port%s. RUN captures %d ms max (irqs masked).",
		(unsigned long)nports, nports == 1 ? "" : "s", CAPTURE_MAX_MS);
	decode[0] = 0;

	widgets_init();
	relabel();
	layout();
	in_live = z_gpio_in_get(port);
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

			case Z_WM_SET_CLIP:
				z_win_apply_clip(&win, &msg.obj);
				break;

			case Z_WM_REDRAW:
				if (msg.obj.type != Z_UINT32) break;
				if (z_win_redraw_id(msg.obj.val.uint32) != win.id) break;
				z_win_apply_redraw(&win, msg.obj.val.uint32);
				repaint();
				z_win_redraw_done(&win);
				break;

			case Z_WM_WINDOW_MOVED:
				z_win_parse_rect(&win, &msg.obj);
				break;

			default:
				break;

			}

		}

		// Poll the pins for the lamps.
		//
		// This is the one thing here that costs CPU while idle, and
		// it is what makes the panel feel like hardware -- a lamp
		// that only updates when you click something is a label.
		// 15Hz is fast enough to look live and slow enough to be a
		// rounding error in the scheduler; the lamps are for "is that
		// pin high", not for catching a pulse. Catching pulses is
		// what RUN is for.
		{
			uint8_t now = z_gpio_in_get(port);
			if (now != in_live) {
				in_live = now;
				repaint_lamps();
			}
		}

		z_proc_wait(Z_TICK_HZ / 15);

	}

	return 0;

}
