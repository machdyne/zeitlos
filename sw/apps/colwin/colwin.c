/*
 * colwin -- a test of the colour window (docs/color.md, "The colour
 * window")
 *
 * Opens a 320x240 window of colour swatches: 8 on a GAME_PLANES 6
 * bitstream, 32 on 8. Each swatch's colour c is drawn as the window
 * mechanism wants it: c's bit 0 in the desktop (the window's ordinary
 * pixels, through zwin), its higher bits in the second VRAM sheet at
 * the folded positions z_color_window_plane_xy() gives. The palette's
 * window entries are set so bit 0 is dark (0) or light (1), so the
 * window still reads in black and white when the colour is off.
 *
 * The colour is on while the window is wholly uncovered, off
 * otherwise; every swatch is drawn again after a move. This program
 * sets the window itself -- it is a test of the hardware, not of who
 * owns it (wm, by focus, later: claude/screen-apps-proposal.md).
 * Esc or the close box ends it, with the colour off.
 *
 *   > run colwin
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zfont.h"
#include "../../common/zgfx.h"

#define CW 320
#define CH 240

static z_win_t win;
static uint32_t planes;     // the window's planes: 2 or 4
static uint32_t ncol;       // 8 or 32

// 16 hues, the dark half; the light half is each mixed halfway to
// white. Hue 0 is black / the desktop's white (0x888 is the desktop's
// ink on DDMI at the default brightness, docs/color.md).
static const uint16_t hue[16] = {
	0x000, 0xa00, 0x0a0, 0x00c, 0xa60, 0x0aa, 0xa0a, 0x666,
	0x630, 0x060, 0x036, 0x606, 0xc40, 0x4a0, 0x04c, 0x333,
};

static uint32_t light(uint32_t rgb) {
	uint32_t r = (rgb >> 8) & 15u, g = (rgb >> 4) & 15u, b = rgb & 15u;
	r = (r + 15u + 1u) / 2u;
	g = (g + 15u + 1u) / 2u;
	b = (b + 15u + 1u) / 2u;
	return (r << 8) | (g << 4) | b;
}

static void palette(void) {
	uint32_t c;
	for (c = 0; c < ncol; c++) {
		uint32_t h = c >> 1, rgb;
		if (h == 0) rgb = (c & 1u) ? 0x888u : 0x000u;
		else rgb = (c & 1u) ? light(hue[h]) : hue[h];
		z_color_set_palette(z_color_window_entry(c), rgb);
	}
}

// one bit of window plane k over a desktop rectangle, split where the
// second sheet folds
static void fill_fold(uint32_t k, int x, int y, int w, int h, int bit) {
	int rows1 = (int)z_color_rows() - 480;
	int px, py, r, w1, h1;
	z_color_window_plane_xy(k, x, y, &px, &py);
	r = py - 480;
	w1 = (w < 640 - px) ? w : 640 - px;
	h1 = (h < rows1 - r) ? h : rows1 - r;
	z_fb_hw_fill_rect(px, py, w1, h1, bit);
	if (w > w1) z_fb_hw_fill_rect(0, py, w - w1, h1, bit);
	if (h > h1) {
		z_fb_hw_fill_rect(px, 480, w1, h - h1, bit);
		if (w > w1) z_fb_hw_fill_rect(0, 480, w - w1, h - h1, bit);
	}
}

static void swatch_geom(uint32_t c, int *x, int *y, int *w, int *h) {
	int cols = (ncol == 8) ? 4 : 8;
	int rows = (int)ncol / cols;
	*w = CW / cols;
	*h = CH / rows;
	*x = (int)(c % (uint32_t)cols) * *w;
	*y = (int)(c / (uint32_t)cols) * *h;
}

static void paint(void) {
	z_clip_t cr;
	uint32_t c, k;
	char s[4];

	// the desktop's bit, through the window as any app draws
	for (c = 0; c < ncol; c++) {
		int x, y, w, h;
		swatch_geom(c, &x, &y, &w, &h);
		z_win_fill_rect(&win, x, y, w, h, (int)(c & 1u));
		s[0] = (char)('0' + c / 10u);
		s[1] = (char)('0' + c % 10u);
		s[2] = 0;
		/* draw_text2: plain draw_text paints its cell's background
		 * 0, which on a lit (odd) swatch is black on black */
		z_win_draw_text2(&win, x + 3, y + 3, s, (int)(~c & 1u),
			(int)(c & 1u), &z_font_5x8);
	}

	// the window's planes, in the second sheet: no visible region
	// (it is not the screen), and rows past 479 allowed
	z_win_content_rect(&win, &cr);
	z_gfx_clear_visible();
	z_fb_set_rows((int)z_color_rows());
	for (c = 0; c < ncol; c++) {
		int x, y, w, h;
		swatch_geom(c, &x, &y, &w, &h);
		for (k = 0; k < planes; k++)
			fill_fold(k, cr.x0 + x, cr.y0 + y, w, h, (int)((c >> (k + 1u)) & 1u));
	}
	z_fb_set_rows(480);
}

// on while the content area is wholly ours to show
static void update_window(void) {
	z_clip_t cr;
	bool whole;
	z_win_content_rect(&win, &cr);
	whole = !win.frozen && win.clip_n == 1 &&
		win.clip[0].x0 <= cr.x0 && win.clip[0].y0 <= cr.y0 &&
		win.clip[0].x1 >= cr.x1 && win.clip[0].y1 >= cr.y1 &&
		cr.x1 - cr.x0 + 1 == CW && cr.y1 - cr.y0 + 1 == CH;
	if (whole)
		z_color_window_set(cr.x0, cr.y0, cr.x1 + 1, cr.y1 + 1);
	else
		z_color_window_off();
}

int main(void) {
	z_msg_t msg;
	bool quit = false;

	if (!z_color_window_available()) {
		printf("colwin: this bitstream has no colour window\n");
		return 1;
	}
	planes = z_color_window_planes();
	ncol = 2u << planes;
	printf("colwin: %lu colours (%lu planes and the desktop's)\n",
		(unsigned long)ncol, (unsigned long)planes);

	if (z_win_create_flags(&win, "colwin", CW + Z_WIN_CHROME_W,
		CH + Z_WIN_CHROME_H, -1, -1, Z_WIN_FLAG_CLOSE_ICON) != Z_OK) {
		printf("colwin: failed to create window -- is wm running?\n");
		return 1;
	}

	z_game_wait_frame();
	palette();

	while (!quit) {
		bool redraw = false;
		while (z_msg_read(&msg) == Z_OK) {
			switch (msg.subject) {
			case Z_WM_SET_CLIP:
				// off before the ack, so nothing wm draws over us
				// next is shown in our colours; on again below if
				// we are still whole
				z_color_window_off();
				z_win_apply_clip(&win, &msg.obj);
				update_window();
				break;
			case Z_WM_WINDOW_MOVED:
				z_color_window_off();
				z_win_parse_rect(&win, &msg.obj);
				break;
			case Z_WM_REDRAW:
				z_win_apply_redraw(&win, msg.obj.val.uint32);
				redraw = true;
				break;
			case Z_WM_KEY:
				if (Z_WM_UNPACK_KEY_PRESSED(msg.obj.val.uint32) &&
				    Z_WM_UNPACK_KEY_KEYSYM(msg.obj.val.uint32) == 0x1b)
					quit = true;
				break;
			case Z_WM_CLOSE:
				quit = true;
				break;
			default:
				break;
			}
		}
		if (redraw) {
			z_win_damage_ignore(&win);
			paint();
			z_win_redraw_done(&win);
			update_window();
		}
		if (!quit) z_proc_wait(Z_TICK_HZ / 30);
	}

	z_color_window_off();
	z_win_destroy(&win);
	return 0;
}
