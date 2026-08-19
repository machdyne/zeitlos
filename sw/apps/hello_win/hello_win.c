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
#include "../../common/zkbd.h"

static z_win_t win;
static uint32_t count = 0;

#define LABEL_Y  4

// content-relative y of the counter line -- computed once, since
// z_font_5x7.h isn't a compile-time constant
static int counter_y;

// content-relative y of the keypress debug line, below the counter --
// this is phase 1's actual test harness: confirms Z_WM_KEY (wm.c) and
// the interrupt-driven capture underneath it (sw/os/hid.c) actually
// reach an app. See docs/window_manager.md for the wider input model
// this is part of.
static int key_y;
static char last_key_desc[48] = "(no key yet)";

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
	z_win_draw_text(&win, 4, LABEL_Y, s, 1, &z_font_5x7);
}

// redraws just the counter's own line, not the whole window -- doing
// a full z_win_clear() here (as an earlier version did) meant
// clearing and redrawing "Hello, world!" too on every tick, which is
// both unnecessary and visibly flashes since the clear-then-redraw
// gap is wide enough to see.
static void draw_counter(void) {
	char buf[48];
	z_win_fill_rect(&win, 0, counter_y, win.w, z_font_5x7.h, 0);
	snprintf(buf, sizeof(buf), "count=%lu", (unsigned long)count);
	// same temporary diagnostic as draw_static() above
	printf("hello_win: draw_counter: '%s' (%d chars)\n", buf, (int)strlen(buf));
	z_win_draw_text(&win, 4, counter_y, buf, 1, &z_font_5x7);
}

// renders a Z_WM_KEY payload as a short human-readable line, e.g.
// "Ctrl+Shift+A down" or "Left up" -- caret notation for the raw
// control characters (Enter/Tab/Esc/Ctrl+letter all arrive as
// keysyms < 0x20, per zkbd.h) since those aren't otherwise printable.
static void describe_key(uint32_t packed, char *buf, size_t buflen) {

	uint32_t keysym    = Z_WM_UNPACK_KEY_KEYSYM(packed);
	uint8_t  modifiers = Z_WM_UNPACK_KEY_MODIFIERS(packed);
	bool     pressed   = Z_WM_UNPACK_KEY_PRESSED(packed) != 0;

	char mods[24] = "";
	if (modifiers & Z_KBD_MOD_CTRL)  strncat(mods, "Ctrl+",  sizeof(mods) - strlen(mods) - 1);
	if (modifiers & Z_KBD_MOD_ALT)   strncat(mods, "Alt+",   sizeof(mods) - strlen(mods) - 1);
	if (modifiers & Z_KBD_MOD_SHIFT) strncat(mods, "Shift+", sizeof(mods) - strlen(mods) - 1);
	if (modifiers & Z_KBD_MOD_GUI)   strncat(mods, "Gui+",   sizeof(mods) - strlen(mods) - 1);

	char keyname[16];
	if (keysym < 0x20) {
		snprintf(keyname, sizeof(keyname), "^%c", (char)(keysym + '@'));
	} else if (keysym < 0x7f) {
		snprintf(keyname, sizeof(keyname), "%c", (char)keysym);
	} else if (keysym == 0x7f) {
		snprintf(keyname, sizeof(keyname), "Backspace");
	} else switch (keysym) {
		case Z_KEY_UP:       snprintf(keyname, sizeof(keyname), "Up"); break;
		case Z_KEY_DOWN:     snprintf(keyname, sizeof(keyname), "Down"); break;
		case Z_KEY_LEFT:     snprintf(keyname, sizeof(keyname), "Left"); break;
		case Z_KEY_RIGHT:    snprintf(keyname, sizeof(keyname), "Right"); break;
		case Z_KEY_HOME:     snprintf(keyname, sizeof(keyname), "Home"); break;
		case Z_KEY_END:      snprintf(keyname, sizeof(keyname), "End"); break;
		case Z_KEY_PAGEUP:   snprintf(keyname, sizeof(keyname), "PgUp"); break;
		case Z_KEY_PAGEDOWN: snprintf(keyname, sizeof(keyname), "PgDn"); break;
		case Z_KEY_INSERT:   snprintf(keyname, sizeof(keyname), "Ins"); break;
		case Z_KEY_DELETE:   snprintf(keyname, sizeof(keyname), "Del"); break;
		case Z_KEY_F1: case Z_KEY_F2: case Z_KEY_F3: case Z_KEY_F4:
		case Z_KEY_F5: case Z_KEY_F6: case Z_KEY_F7: case Z_KEY_F8:
		case Z_KEY_F9: case Z_KEY_F10: case Z_KEY_F11: case Z_KEY_F12:
			snprintf(keyname, sizeof(keyname), "F%d", (int)(keysym - Z_KEY_F1 + 1));
			break;
		default:
			snprintf(keyname, sizeof(keyname), "?%lu", (unsigned long)keysym);
			break;
	}

	snprintf(buf, buflen, "%s%s %s", mods, keyname, pressed ? "down" : "up ");

}

static void draw_key(void) {
	z_win_fill_rect(&win, 0, key_y, win.w, z_font_5x7.h, 0);
	z_win_draw_text(&win, 4, key_y, last_key_desc, 1, &z_font_5x7);
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
		} else if (msg.subject == Z_WM_KEY) {
			// redraw immediately, not just on the next tick -- this is
			// the actual point of this line: confirming keypresses show
			// up with no perceptible delay. see docs/window_manager.md
			// for why wm is the one deciding *which* window this was
			// even routed to (focus tracking) -- this app only ever
			// sees keys while it's the focused window.
			describe_key(msg.obj.val.uint32, last_key_desc, sizeof(last_key_desc));
			draw_key();
		}
	}

	if (got_wm_redraw) {
		draw_static();
		draw_counter();
		draw_key();
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

	counter_y = LABEL_Y + z_font_5x7.h;
	key_y = counter_y + z_font_5x7.h;

	if (z_win_create(&win, "hello_win", 160, 100) != Z_OK) {
		printf("hello_win: failed to create window\n");
		return 1;
	}

	// no z_gfx_hw_font_load() call here anymore -- wm now loads
	// z_font_5x7 into hardware glyph memory exactly once, at its own
	// startup, and is the only process on the board that ever does
	// (see wm's Makefile and main()'s own comment there). Every app,
	// this one included, is expected to only ever draw with
	// z_font_5x7 as a result -- this file used z_font_6x12 until this
	// change; switched to z_font_5x7 throughout specifically so this
	// still renders correctly now that it isn't loading its own font
	// data.

	printf("hello_win: window %ld created at (%ld,%ld) %ldx%ld\n",
		(long)win.id, (long)win.x, (long)win.y,
		(long)win.w, (long)win.h);

	draw_static();
	draw_counter();
	draw_key();

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

