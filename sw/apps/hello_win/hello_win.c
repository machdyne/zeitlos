/*
 * hello_win -- text-in-a-window demo app
 *
 * Creates a window through the wm (zwin.h) and draws a static line of
 * text plus a counter into it. Redraws whenever the wm asks
 * (Z_WM_REDRAW, sent after every wm-triggered screen redraw -- see
 * docs/window_manager.md) and also periodically on its own, to show
 * that content updates aren't only driven by the wm.
 *
 * Requires the wm to already be running as pid 1 (see Z_PID_WM in
 * zwm.h):
 *
 *   > run wm
 *   > run hello_win
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zfont.h"
#include "../../common/zgfx.h"

static z_win_t win;
static uint32_t count = 0;

#define LABEL_Y  4

// content-relative y of the counter line -- computed once, since
// z_font_6x12.h isn't a compile-time constant
static int counter_y;

// the label never changes -- only redraw it in response to a wm
// notification (window moved, or the wm just cleared the screen),
// never on the periodic tick below.
static void draw_static(void) {
	static const char *s = "Hello, world!";
	// temporary diagnostic -- logs exactly what's about to be drawn
	// and its length, so we can compare against what actually shows
	// up on screen (see docs/window_manager.md, "hardware glyph
	// blitting" investigation notes).
	printf("hello_win: draw_static: '%s' (%d chars)\n", s, (int)strlen(s));
	z_win_clear(&win);
	z_win_draw_text(&win, 4, LABEL_Y, s, 1, &z_font_6x12);
}

// redraws just the counter's own line, not the whole window -- doing
// a full z_win_clear() here (as an earlier version did) meant
// clearing and redrawing "Hello, world!" too on every tick, which is
// both unnecessary and visibly flashes since the clear-then-redraw
// gap is wide enough to see.
static void draw_counter(void) {
	char buf[48];
	z_win_fill_rect(&win, 0, counter_y, win.w, z_font_6x12.h, 0);
	snprintf(buf, sizeof(buf), "count=%lu", (unsigned long)count);
	// same temporary diagnostic as draw_static() above
	printf("hello_win: draw_counter: '%s' (%d chars)\n", buf, (int)strlen(buf));
	z_win_draw_text(&win, 4, counter_y, buf, 1, &z_font_6x12);
}

// drains every pending message, applying any position update and
// redrawing (once) if a Z_WM_REDRAW came in. Z_WM_WINDOW_MOVED is
// deliberately NOT treated as its own redraw trigger here, even
// though it also carries a position update -- the wm's repair_region()
// always sends Z_WM_REDRAW for the same window in the same call, and
// treating both as independent triggers caused a real, visible
// double-redraw (flash): the two messages can arrive at measurably
// different times (WINDOW_MOVED is sent before the wm's chrome-
// drawing work, REDRAW after), so if this app got scheduled in
// between, it would redraw once for each. See docs/window_manager.md,
// "content z-order" for the wider protocol this is part of.
static bool drain_messages(void) {

	bool got_wm_redraw = false;
	z_msg_t msg;

	while (z_msg_read(&msg) == Z_OK) {
		if (msg.subject == Z_WM_REDRAW) {
			z_win_apply_redraw(&win, msg.obj.val.uint32);
			got_wm_redraw = true;
		} else if (msg.subject == Z_WM_WINDOW_MOVED) {
			z_win_parse_rect(&win, &msg.obj);	// keep win.x/y in sync; no redraw here
		}
	}

	if (got_wm_redraw) {
		draw_static();
		draw_counter();
		z_win_redraw_done(&win);
	}

	return got_wm_redraw;

}

// total delay between counter ticks, split into chunks so messages
// get checked throughout the wait rather than only at the very start
// and end of it -- checking only at the ends meant a wm redraw
// notification arriving mid-wait could sit unprocessed for the whole
// ~10-second delay before hello_win noticed it (see
// docs/window_manager.md, "known limitations" -- this was the actual
// cause of the multi-second lag after a move, not the race that
// comment used to be about).
#define TICK_ITERATIONS   3000000
#define POLL_CHUNK          20000

int main(void) {

	counter_y = LABEL_Y + z_font_6x12.h;

	if (z_win_create(&win, "hello_win", 160, 100) != Z_OK) {
		printf("hello_win: failed to create window\n");
		return 1;
	}

	// required for Z_GFX_HW_BLIT builds -- pushes z_font_6x12's glyph
	// data into hardware glyph memory so the blitter has something to
	// read; a documented no-op in software-only builds. must happen
	// before the first draw_static()/draw_counter() call below.
	z_gfx_hw_font_load(&z_font_6x12);

	printf("hello_win: window %ld created at (%ld,%ld) %ldx%ld\n",
		(long)win.id, (long)win.x, (long)win.y,
		(long)win.w, (long)win.h);

	draw_static();
	draw_counter();

	while (1) {

		int waited = 0;
		while (waited < TICK_ITERATIONS) {
			drain_messages();
			for (volatile int i = 0; i < POLL_CHUNK; i++);
			waited += POLL_CHUNK;
		}

		count++;
		draw_counter();

	}

}

