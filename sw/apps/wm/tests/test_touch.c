/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * touch.c on the host: a finger's samples in, the mouse wm acts on out
 * (docs/touch.md). `make test` in sw/apps/wm.
 */

#include <stdio.h>
#include <string.h>
#include "../touch.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static wm_touch_t t;
static int ox, oy;
static uint8_t ob;
static int wake;

// one raw sample; then, while the filter asks to be called again at
// once, the samples it held over (with the same raw input, as wm's loop
// would read it again)
static int nout;
static int outx[8], outy[8];
static uint8_t outb[8];

static void feed(uint32_t ms, int x, int y, uint8_t b, bool touch, wm_touch_snap_fn snap) {
	nout = 0;
	wake = wm_touch_feed(&t, ms, x, y, b, touch, snap, &ox, &oy, &ob);
	outx[nout] = ox; outy[nout] = oy; outb[nout] = ob; nout++;
	while (wake == 0 && nout < 8 && t.queued) {
		wake = wm_touch_feed(&t, ms, x, y, b, touch, snap, &ox, &oy, &ob);
		outx[nout] = ox; outy[nout] = oy; outb[nout] = ob; nout++;
	}
}

static bool snap_up(int x, int y, int *dx, int *dy) {
	(void)x;
	if (y >= 50 && y < 58) { *dx = 0; *dy = 45 - y; return true; }	// a titlebar at y 40..49
	return false;
}

int main(void) {

	// -- a mouse goes straight through --
	memset(&t, 0, sizeof(t));
	feed(0, 100, 100, 1, false, NULL);
	CHECK(nout == 1 && outx[0] == 100 && outy[0] == 100 && outb[0] == 1, "a mouse press is a press");
	feed(10, 120, 100, 1, false, NULL);
	CHECK(outx[0] == 120 && outb[0] == 1, "a mouse drag is a drag at once");
	feed(20, 120, 100, 0, false, NULL);
	CHECK(outb[0] == 0, "a mouse release");

	// -- a tap: nothing pressed until it lifts, then press and release where it landed --
	memset(&t, 0, sizeof(t));
	feed(1000, 200, 150, 1, true, NULL);
	CHECK(nout == 1 && outb[0] == 0 && outx[0] == 200 && outy[0] == 150,
		"finger down: the pointer moves there, no button yet");
	CHECK(wake == TOUCH_LONG_MS, "and wm is told when a long press would be due (%d)", wake);
	feed(1080, 202, 151, 1, true, NULL);
	CHECK(outb[0] == 0 && outx[0] == 200, "a wobble under the slop: still nothing, still where it landed");
	feed(1120, 203, 152, 0, true, NULL);
	CHECK(nout == 2 && outb[0] == 1 && outb[1] == 0 && outx[0] == 200 && outy[0] == 150 &&
		outx[1] == 200, "lifted: a press and a release where it landed");
	CHECK(t.state == TOUCH_IDLE, "and the contact is over");

	// -- a drag: pressed where it landed, then following --
	memset(&t, 0, sizeof(t));
	feed(0, 300, 200, 1, true, NULL);
	feed(30, 310, 205, 1, true, NULL);
	CHECK(nout == 2 && outb[0] == 1 && outx[0] == 300 && outy[0] == 200 &&
		outb[1] == 1 && outx[1] == 310 && outy[1] == 205,
		"past the slop: the press where it landed, then the finger");
	feed(60, 400, 250, 1, true, NULL);
	CHECK(outb[0] == 1 && outx[0] == 400, "the drag follows");
	feed(90, 400, 250, 0, true, NULL);
	CHECK(outb[0] == 0 && t.state == TOUCH_IDLE, "lifted: released");

	// -- a long press: the right button --
	memset(&t, 0, sizeof(t));
	feed(0, 50, 60, 1, true, NULL);
	feed(300, 51, 60, 1, true, NULL);
	CHECK(outb[0] == 0 && wake == TOUCH_LONG_MS - 300, "not yet; due in %d ms", wake);
	feed(TOUCH_LONG_MS, 51, 61, 1, true, NULL);
	CHECK(outb[0] == 2 && outx[0] == 50 && outy[0] == 60, "held still: the right button, where it landed");
	feed(TOUCH_LONG_MS + 100, 90, 90, 1, true, NULL);
	CHECK(outb[0] == 2 && outx[0] == 50, "and it stays there while held, however the finger wanders");
	feed(TOUCH_LONG_MS + 200, 90, 90, 0, true, NULL);
	CHECK(outb[0] == 0 && t.state == TOUCH_IDLE, "lifted: released");

	// -- a snap: landed just below a titlebar, grabbed it, and drags with an offset --
	memset(&t, 0, sizeof(t));
	feed(0, 100, 53, 1, true, snap_up);
	CHECK(outx[0] == 100 && outy[0] == 45 && outb[0] == 0, "snapped onto the titlebar");
	feed(20, 140, 60, 1, true, snap_up);
	CHECK(outb[0] == 1 && outy[0] == 45 && outb[1] == 1 && outx[1] == 140 && outy[1] == 52,
		"the drag starts on the titlebar and keeps the offset");
	feed(40, 140, 60, 0, true, snap_up);
	CHECK(outb[0] == 0 && outy[0] == 52, "released with the offset");
	feed(60, 20, 20, 1, true, snap_up);
	CHECK(outx[0] == 20 && outy[0] == 20, "the next contact, nowhere near: no offset");
	feed(70, 20, 20, 0, true, snap_up);

	// -- a mouse takes over mid-drag: the finger is let go first --
	memset(&t, 0, sizeof(t));
	feed(0, 10, 10, 1, true, NULL);
	feed(10, 40, 10, 1, true, NULL);
	feed(20, 300, 300, 0, false, NULL);
	CHECK(outb[0] == 0 && outx[0] == 40 && t.state == TOUCH_IDLE, "released where the finger was");
	feed(30, 300, 300, 0, false, NULL);
	CHECK(outx[0] == 300, "then the mouse");

	// -- a pen's barrel button: through, as a mouse's --
	memset(&t, 0, sizeof(t));
	feed(0, 70, 70, 3, true, NULL);
	CHECK(outb[0] == 3 && outx[0] == 70, "a contact with another button: as it is");

	printf("touch: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;

}
