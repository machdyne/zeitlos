/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A finger, as wm reads it -- see touch.h.
 */

#include "touch.h"

static void out(int *ox, int *oy, uint8_t *obtn, int x, int y, uint8_t b) {
	*ox = x; *oy = y; *obtn = b;
}

int wm_touch_feed(wm_touch_t *t, uint32_t now_ms, int x, int y, uint8_t btn,
	bool touch, wm_touch_snap_fn snap, int *ox, int *oy, uint8_t *obtn) {

	bool down = (btn & 1) != 0;

	// A sample held over from the last call goes first; the new one
	// waits for the next (wm calls again at once when told 0).
	if (t->queued) {
		t->queued = false;
		out(ox, oy, obtn, t->qx, t->qy, t->qbtn);
		return 0;
	}

	if (!touch || (btn & ~1u)) {
		// A mouse or a pen, or a contact that also reports another
		// button (a pen's barrel): as it is. A finger that was down
		// when the mouse took over is let go of first.
		if (t->state == TOUCH_DRAG || t->state == TOUCH_RIGHT) {
			t->state = TOUCH_IDLE;
			out(ox, oy, obtn, t->lx, t->ly, 0);
			return 0;
		}
		t->state = TOUCH_IDLE;
		out(ox, oy, obtn, x, y, btn);
		return -1;
	}

	switch (t->state) {

	case TOUCH_IDLE:
		if (!down) {
			out(ox, oy, obtn, x, y, 0);
			return -1;
		}
		t->state = TOUCH_PENDING;
		t->x0 = x; t->y0 = y;
		t->dx = t->dy = 0;
		t->t0 = now_ms;
		if (snap && !snap(x, y, &t->dx, &t->dy)) t->dx = t->dy = 0;
		// The pointer goes where the finger is, with no button yet.
		out(ox, oy, obtn, x + t->dx, y + t->dy, 0);
		return TOUCH_LONG_MS;

	case TOUCH_PENDING: {
		int mx = x - t->x0, my = y - t->y0;
		int sx = t->x0 + t->dx, sy = t->y0 + t->dy;
		if (!down) {
			// A tap: pressed and let go where it landed.
			t->state = TOUCH_IDLE;
			out(ox, oy, obtn, sx, sy, 1);
			t->queued = true;
			t->qx = sx; t->qy = sy; t->qbtn = 0;
			return 0;
		}
		if (mx * mx + my * my > TOUCH_SLOP * TOUCH_SLOP) {
			// A drag: pressed where it landed, then following.
			t->state = TOUCH_DRAG;
			out(ox, oy, obtn, sx, sy, 1);
			t->queued = true;
			t->qx = t->lx = x + t->dx; t->qy = t->ly = y + t->dy; t->qbtn = 1;
			return 0;
		}
		if ((int32_t)(now_ms - t->t0) >= TOUCH_LONG_MS) {
			// Held still: the right button, until it lifts.
			t->state = TOUCH_RIGHT;
			t->lx = sx; t->ly = sy;
			out(ox, oy, obtn, sx, sy, 2);
			return -1;
		}
		out(ox, oy, obtn, sx, sy, 0);
		return TOUCH_LONG_MS - (int)(now_ms - t->t0);
	}

	case TOUCH_DRAG:
		if (!down) t->state = TOUCH_IDLE;
		t->lx = x + t->dx; t->ly = y + t->dy;
		out(ox, oy, obtn, t->lx, t->ly, down ? 1 : 0);
		return -1;

	case TOUCH_RIGHT:
		// The right button stays where it went down: a finger that
		// wanders after a long press is not dragging anything.
		if (!down) t->state = TOUCH_IDLE;
		out(ox, oy, obtn, t->x0 + t->dx, t->y0 + t->dy, down ? 2 : 0);
		return -1;

	}

	out(ox, oy, obtn, x, y, btn);
	return -1;

}
