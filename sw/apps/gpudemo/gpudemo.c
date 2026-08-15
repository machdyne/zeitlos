/*
 * gpudemo -- minimal hardware line-rasterizer + window clip test
 *
 * Draws simple shapes (a static box+diagonals test pattern, then a
 * small bouncing box) inside a wm window, using the same hardware
 * line rasterizer and window-clip approach gpu3d uses -- deliberately
 * without any 3D projection math, to isolate whether the rasterizer's
 * hardware clip region (rtl/gpu/gpu_raster.v) actually constrains
 * drawing to the window's content area, or whether something more
 * fundamental is going on. See docs/window_manager.md.
 *
 * Prints the exact window/content/clip geometry to the UART on
 * startup and on every redraw, plus the bouncing box's own position
 * on each bounce, so what's actually being programmed into the
 * hardware -- and where things are actually drawn -- can be directly
 * compared against what shows up on screen.
 *
 * Requires the wm to already be running:
 *
 *   > run wm
 *   > run gpudemo
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "../../common/zeitlos.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"

// register access, IRQ-masked atomicity, and clip-region management
// for the hardware line rasterizer are all handled by
// z_win_hw_line()/z_win_hw_box() (zwin.h) now -- this file never
// touches the gpu_*/gpu_clip_* registers directly. See zgfx.h's file
// header comment for why that hardware access needed to move behind
// a shared, careful implementation rather than each app managing it
// (badly, it turned out) on its own.

// small window, similar footprint to hello_win/the resized gpu3d --
// see wm.c's create_window() fixed cascade placement (no check
// against window size) for why staying modest matters.
#define WIN_WIDTH   200
#define WIN_HEIGHT  150

static z_win_t win;
static uint32_t content_x0, content_y0, content_x1, content_y1;

static void draw_line(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint8_t color) {
	z_win_hw_line(&win, x0, y0, x1, y1, color);
}

static void draw_box(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint8_t color) {
	z_win_hw_box(&win, x0, y0, x1, y1, color);
}

// content_x0/y0/x1/y1 here are for this file's own use (the static
// pattern's inset, the bouncing box's boundaries) -- not tied to the
// hardware clip registers at all anymore, z_win_hw_line()/
// z_win_hw_box() compute and apply that themselves, fresh, on every
// call. Queried via z_win_content_rect() (zwin.h) rather than
// duplicating the content-area formula here -- that duplication is
// exactly what caused a real bug once already (two separately-kept
// copies of this rectangle drifting out of sync -- see
// docs/window_manager.md).
static void update_win_geometry(void) {

	z_clip_t clip;
	z_win_content_rect(&win, &clip);
	content_x0 = (uint32_t)clip.x0;
	content_y0 = (uint32_t)clip.y0;
	content_x1 = (uint32_t)clip.x1;
	content_y1 = (uint32_t)clip.y1;

	printf("gpudemo: win=(%ld,%ld) %ldx%ld  content=(%ld,%ld)-(%ld,%ld)\n",
		(long)win.x, (long)win.y, (long)win.w, (long)win.h,
		(long)content_x0, (long)content_y0, (long)content_x1, (long)content_y1);

}

// static test pattern -- a box inset 10px from every content edge,
// plus both diagonals. Deliberately drawn close to the content area's
// own boundary (not tiny/centered) so any bleed past the window's
// border would be immediately, unambiguously visible.
static void draw_static_pattern(void) {
	draw_box(content_x0 + 10, content_y0 + 10, content_x1 - 10, content_y1 - 10, 1);
	draw_line(content_x0 + 10, content_y0 + 10, content_x1 - 10, content_y1 - 10, 1);
	draw_line(content_x1 - 10, content_y0 + 10, content_x0 + 10, content_y1 - 10, 1);
}

// explicit clip test: a line from the content area's own center to a
// corner of the FULL 512x384 screen, far outside the window in every
// direction. If gpu_clip_enable is actually being respected by the
// hardware, this should visibly stop dead at the window's edge --
// nothing beyond it. If the whole line (all the way to the screen
// corner) is visible, the clip registers aren't constraining the
// rasterizer's output at all, regardless of what we've written to
// them or what they read back as.
static void draw_clip_test(void) {
	uint32_t cx = (content_x0 + content_x1) / 2;
	uint32_t cy = (content_y0 + content_y1) / 2;
	printf("gpudemo: clip test line from (%lu,%lu) to screen corner (511,383) -- "
		"should stop at the window edge if clipping works\n",
		(unsigned long)cx, (unsigned long)cy);
	draw_line(cx, cy, 511, 383, 1);
}

int main(void) {

	printf("gpudemo: hardware line rasterizer + window clip test\n");

	if (z_win_create(&win, "gpudemo", WIN_WIDTH, WIN_HEIGHT) != Z_OK) {
		printf("gpudemo: failed to create window\n");
		return 1;
	}

	update_win_geometry();
	z_win_clear(&win);
	draw_static_pattern();
	draw_clip_test();

	printf("gpudemo: static test pattern drawn, starting bouncing box\n");

	// small bouncing box -- same erase-then-redraw pattern gpu3d
	// uses for its cube (erase previous frame, then draw the new
	// one), deliberately simple (fixed-size box, no 3D math, no
	// perspective) to isolate whether that erase/redraw pattern
	// itself is what's causing the border to get overwritten.
	const int box_size = 16;
	int bx = (int)content_x0 + 20, by = (int)content_y0 + 20;
	int dx = 2, dy = 1;
	bool first_frame = true;
	bool redraw_pending = false;
	uint32_t bounce_count = 0;

	while (1) {

		z_msg_t msg;
		while (z_msg_read(&msg) == Z_OK) {
			if (msg.subject == Z_WM_REDRAW) {
				z_win_apply_redraw(&win, msg.obj.val.uint32);
				update_win_geometry();
				first_frame = true;
				redraw_pending = true;
			} else if (msg.subject == Z_WM_WINDOW_MOVED) {
				z_win_parse_rect(&win, &msg.obj);
				update_win_geometry();
			}
		}

		if (first_frame) {
			// wm just did a full-screen clear (per zwm.h's own
			// comment on Z_WM_REDRAW) -- redraw everything fresh,
			// nothing of ours is left on screen to erase
			draw_static_pattern();
		} else {
			draw_box(bx, by, bx + box_size, by + box_size, 0);
		}

		bx += dx;
		by += dy;

		bool bounced = false;

		if (bx < (int)content_x0) { bx = (int)content_x0; dx = -dx; bounced = true; }
		else if (bx + box_size > (int)content_x1) { bx = (int)content_x1 - box_size; dx = -dx; bounced = true; }

		if (by < (int)content_y0) { by = (int)content_y0; dy = -dy; bounced = true; }
		else if (by + box_size > (int)content_y1) { by = (int)content_y1 - box_size; dy = -dy; bounced = true; }

		draw_box(bx, by, bx + box_size, by + box_size, 1);

		if (bounced) {
			bounce_count++;
			printf("gpudemo: bounce #%lu at (%d,%d), content=(%ld,%ld)-(%ld,%ld)\n",
				(unsigned long)bounce_count, bx, by,
				(long)content_x0, (long)content_y0, (long)content_x1, (long)content_y1);
		}

		if (redraw_pending) {
			z_win_redraw_done(&win);
			redraw_pending = false;
		}

		first_frame = false;

		for (volatile int i = 0; i < 30000; i++) ;	// visible speed

	}

}
