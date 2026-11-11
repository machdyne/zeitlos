#ifndef WM_TOUCH_H
#define WM_TOUCH_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A finger, as wm reads it: tap, drag, long press (docs/touch.md).
 *
 * A finger is not a mouse. It has no hover, so it cannot point and
 * then decide; it lands where it lands, and it wobbles. So when the
 * virtual mouse says TOUCH (reg_vmouse bit 25), wm passes each sample
 * through this filter before anything else sees it:
 *
 *   - the press is held back until the finger has either moved more
 *     than TOUCH_SLOP pixels (a drag -- pressed where the finger first
 *     landed, so a window's titlebar is grabbed where it was touched),
 *     lifted (a tap: press and release where it landed), or stayed put
 *     for TOUCH_LONG_MS (a long press: the right button);
 *   - where it landed may be moved onto one of wm's own controls -- the
 *     close box, the titlebar, the resize grip, a dock icon -- when it
 *     fell just short of one (the `snap` call, which wm supplies); the
 *     whole contact is then offset by as much, so a drag moves with the
 *     finger.
 *
 * Everything downstream -- hit tests, drags, the dock, the apps -- sees
 * an ordinary mouse. A mouse, or a pen (which hovers and is precise),
 * goes straight through.
 *
 * No wm state in here, so tests/test_touch.c runs it on the host.
 */

#include <stdint.h>
#include <stdbool.h>

#define TOUCH_SLOP      6       // pixels a finger may wander and still tap
#define TOUCH_LONG_MS   500     // held this long without moving: right button

typedef enum {
	TOUCH_IDLE,		// no finger
	TOUCH_PENDING,	// a finger, not yet a tap, a drag or a long press
	TOUCH_DRAG,		// left button down, following the finger
	TOUCH_RIGHT,	// right button down, until the finger lifts
} wm_touch_state_t;

// Moves (x, y) onto a control it fell just short of: returns true and
// sets *dx, *dy to the offset, or false to leave it where it is.
typedef bool (*wm_touch_snap_fn)(int x, int y, int *dx, int *dy);

typedef struct {
	wm_touch_state_t	state;
	int		x0, y0;			// where the finger landed (raw)
	int		dx, dy;			// the snap's offset, for this contact
	uint32_t	t0;				// when, in ms
	int		lx, ly;			// where the pointer last was, during a drag
	// One sample held over to the next call: a tap's release, a drag's
	// first move after its press. *wake says to call again at once.
	bool	queued;
	int		qx, qy;
	uint8_t	qbtn;
} wm_touch_t;

// The pointer wm should act on, from one raw sample. `touch` is the
// virtual mouse's TOUCH bit; without it the sample goes straight
// through (and a contact in progress is ended). Returns how many ms
// until the filter needs to be called again even if nothing changes
// (a long press coming due, a queued sample), or -1 for no hurry.
int wm_touch_feed(wm_touch_t *t, uint32_t now_ms, int x, int y, uint8_t btn,
	bool touch, wm_touch_snap_fn snap, int *ox, int *oy, uint8_t *obtn);

#endif
