/*
 * Host test: a scrollbar never paints over, or takes clicks meant for,
 * the resize grip (sw/common/zwidget.c, sb_len()), and z_win_clear()
 * does not blank it (sw/common/zwin.c).
 *
 * wm draws the grip as part of the window's frame, in the bottom-right
 * corner of the content area, and an app's redraw comes after it. A
 * vertical bar as long as the content -- term's is as long as its rows,
 * which reaches the corner at three window heights in four -- painted
 * over it, leaving a few of its dots. On a resizable window the bar now
 * stops Z_WIN_GRIP_INSET short of the corner, whatever length the app
 * gives it; a window that cannot be resized has no grip, and its bars
 * are as the app says.
 *
 *   make -C sw/common/tests scrollbar
 *
 * Needs -no-pie and vm.mmap_min_addr=0 (zrender.h); exits 77 without.
 */
#include "zrender.h"

#include <stdio.h>

#include "zwin.h"
#include "zwidget.h"
#include "zwm.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static z_win_t win;

// The grip's square of the content area, in screen pixels, filled with
// ink -- what wm's ticks are, there -- before the app draws.
static void plant_grip(void) {
	z_clip_t c;
	z_win_content_rect(&win, &c);
	volatile uint32_t *vram = Z_RENDER_VRAM;
	for (int y = c.y1 - Z_WIN_GRIP_INSET + 1; y <= c.y1; y++)
		for (int x = c.x1 - Z_WIN_GRIP_INSET + 1; x <= c.x1; x++) {
			uint32_t bit = (uint32_t)y * Z_SCREEN_W + (uint32_t)x;
			vram[bit / 32] |= 1u << (bit % 32);
		}
}

// Of those, how many the bar's drawing left.
static int grip_left(void) {
	z_clip_t c;
	z_win_content_rect(&win, &c);
	int n = 0;
	for (int y = c.y1 - Z_WIN_GRIP_INSET + 1; y <= c.y1; y++)
		for (int x = c.x1 - Z_WIN_GRIP_INSET + 1; x <= c.x1; x++)
			n += z_render_get(x, y);
	return n;
}

#define GRIP_PX (Z_WIN_GRIP_INSET * Z_WIN_GRIP_INSET)

static void bar(z_sb_orient_t o, bool resizable, int len_short_of_end, const char *what) {
	z_render_clear();
	plant_grip();
	win.resizable = resizable;
	int cw = z_win_content_w(&win), ch = z_win_content_h(&win);
	z_scrollbar_t sb;
	z_scrollbar_init(&sb, &win, o);
	if (o == Z_SB_VERT) z_scrollbar_set_geom(&sb, cw - Z_SB_THICK, 0, ch - len_short_of_end);
	else z_scrollbar_set_geom(&sb, 0, ch - Z_SB_THICK, cw - len_short_of_end);
	z_scrollbar_set_range(&sb, 1000, 900);		// thumb at the start, track at the end
	z_scrollbar_draw(&sb, true);

	z_clip_t c;
	z_win_content_rect(&win, &c);
	int left = grip_left();
	// (content-relative, as the app's mouse handler passes them)
	bool hit = z_scrollbar_has_pointer(&sb, cw - 2, ch - 2);
	// just outside the grip's square, along the bar
	bool hit_edge = o == Z_SB_VERT
		? z_scrollbar_has_pointer(&sb, cw - 2, ch - Z_WIN_GRIP_INSET - 1)
		: z_scrollbar_has_pointer(&sb, cw - Z_WIN_GRIP_INSET - 1, ch - 2);
	if (resizable) {
		CHECK(left == GRIP_PX, "%s: the bar erased %d of the grip's pixels", what, GRIP_PX - left);
		CHECK(!hit, "%s: a click on the grip goes to the bar", what);
		if (len_short_of_end <= Z_WIN_GRIP_INSET)
			CHECK(hit_edge, "%s: the bar still runs to the grip's edge", what);
	} else if (len_short_of_end == 0) {
		CHECK(left < GRIP_PX, "%s: a fixed window's bar runs to the corner, as set", what);
		CHECK(hit, "%s: and takes clicks there", what);
	}
}

int main(void) {

	if (!z_render_open(&win, 320, 240)) {
		printf("test_scrollbar: skipped (needs -no-pie and vm.mmap_min_addr=0)\n");
		return 77;
	}

	bar(Z_SB_VERT, true, 0, "vertical, full height, resizable");
	bar(Z_SB_VERT, true, 3, "vertical, 3 short (term at most heights), resizable");
	bar(Z_SB_VERT, true, Z_WIN_GRIP_INSET, "vertical, stopping short already, resizable");
	bar(Z_SB_VERT, false, 0, "vertical, full height, fixed size");
	bar(Z_SB_HORZ, true, 0, "horizontal, full width, resizable");
	bar(Z_SB_HORZ, false, 0, "horizontal, full width, fixed size");

	// z_win_clear(): what read (and 25 other apps) blank the window
	// with before drawing it -- the grip's corner left alone when the
	// window is resizable, everything else cleared.
	{
		int cw = z_win_content_w(&win), ch = z_win_content_h(&win);
		z_clip_t c;
		z_win_content_rect(&win, &c);
		volatile uint32_t *vram = Z_RENDER_VRAM;
		for (int y = c.y0; y <= c.y1; y++)
			for (int x = c.x0; x <= c.x1; x++) {
				uint32_t bit = (uint32_t)y * Z_SCREEN_W + (uint32_t)x;
				vram[bit / 32] |= 1u << (bit % 32);
			}
		win.resizable = 1;
		z_win_clear(&win);
		int left = grip_left(), total = 0;
		for (int y = c.y0; y <= c.y1; y++)
			for (int x = c.x0; x <= c.x1; x++) total += z_render_get(x, y);
		CHECK(left == GRIP_PX, "z_win_clear erased %d of the grip's pixels", GRIP_PX - left);
		CHECK(total == GRIP_PX, "z_win_clear left %d pixels outside the grip", total - GRIP_PX);
		win.resizable = 0;
		z_win_clear(&win);
		total = 0;
		for (int y = c.y0; y <= c.y1; y++)
			for (int x = c.x0; x <= c.x1; x++) total += z_render_get(x, y);
		CHECK(total == 0, "z_win_clear on a fixed window clears it all (%d left)", total);
		(void)cw; (void)ch;
	}

	printf("test_scrollbar: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
