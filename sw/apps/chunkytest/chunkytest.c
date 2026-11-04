/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * chunkytest -- a hardware test of chunky game mode (docs/color.md,
 * "Chunky").
 *
 * Not one of sw/apps/Makefile's APPS and not on any card: build it by
 * itself and copy it over.
 *
 *   make -C sw/apps/chunkytest
 *   > run chunkytest
 *
 * Ten seconds full screen, then back to the desktop, then a line of
 * numbers on stdout. What to see (docs/color.md, "Chunky", bring-up):
 *
 *   - top: all 256 palette entries, 16 x 16 swatches of 20 x 8, in
 *     index order from the top left; a white frame round each row
 *     of four swatches
 *   - the segment seams (x 79|80, 159|160, 239|240) marked by a
 *     one-pixel red line on the left of each seam and green on the
 *     right: no gap, no doubled column, no swapped bytes
 *   - bottom: diagonal stripes moving one pixel a frame, smooth, with
 *     no tearing line (each frame is copied in from the frame
 *     boundary, ahead of the beam)
 *
 * What it prints: the cycles one whole 320x240 copy took
 * (z_chunky_put_rows()), the average and the worst, per line too, and
 * how many frames came late. The copy has to stay under ~6,100 cycles
 * a line to stay ahead of the beam.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zcycles.h"
#include "../../common/zwm.h"

#define W Z_CHUNKY_W
#define H Z_CHUNKY_H
#define FRAMES 600

static uint32_t pic[W * H / 4];

static void px(int x, int y, uint32_t c) {
	((uint8_t *)pic)[y * W + x] = (uint8_t)c;
}

// RGB 3-3-2 -> RGB444: every entry a different colour
static uint32_t rgb332(uint32_t i) {
	uint32_t r = (i >> 5) & 7u, g = (i >> 2) & 7u, b = i & 3u;
	return ((r * 15u / 7u) << 8) | ((g * 15u / 7u) << 4) | (b * 5u);
}

static void card(void) {
	int x, y;
	for (y = 0; y < 128; y++)
		for (x = 0; x < W; x++)
			px(x, y, (uint32_t)((y / 8) * 16 + x / 20));
	for (y = 0; y < 128; y += 8)
		for (x = 0; x < W; x++) px(x, y, 0xff);
	for (x = 0; x < W; x += 80)
		for (y = 0; y < 128; y++) px(x, y, 0xff);
}

static void stripes(uint32_t f) {
	int x, y;
	for (y = 128; y < H; y++)
		for (x = 0; x < W; x++)
			px(x, y, (((uint32_t)(x + y) + f) & 16u) ? 0x1f : 0x03);
	for (x = 80; x < W; x += 80)
		for (y = 0; y < H; y++) {
			px(x - 1, y, 0xe0);	// red, left of the seam
			px(x, y, 0x1c);		// green, right of it
		}
}

int main(void) {
	uint32_t i, f, t, sum = 0, worst = 0, late = 0, fr;
	uint32_t wm_pid;

	if (!z_color_chunky_available()) {
		printf("chunkytest: this bitstream has no chunky game mode "
			"(GAME_CHUNKY, eight planes)\n");
		return 1;
	}

	for (i = 0; i < 256; i++) z_color_set_palette(i, rgb332(i));
	card();
	stripes(0);

	z_game_set_enabled(true, false);
	z_game_set_view(0, 0);
	/* first: on a VRAM_SDRAM console it allocates the picture, which
	 * z_chunky_put_rows() then writes (docs/color.md, "In main memory") */
	if (!z_color_set_chunky(true)) {
		z_game_set_enabled(false, false);
		printf("chunkytest: no memory for the chunky picture\n");
		return 1;
	}
	z_chunky_put_rows(pic, W, 0, H);
	z_game_wait_frame();

	fr = z_game_frame();
	for (f = 1; f <= FRAMES; f++) {
		stripes(f);
		z_game_wait_frame();
		if ((uint32_t)(z_game_frame() - fr) > 1) late++;
		fr = z_game_frame();
		t = z_cycles();
		z_chunky_put_rows(pic, W, 0, H);
		t = z_cycles() - t;
		sum += t;
		if (t > worst) worst = t;
	}

	z_color_set_chunky(false);
	z_game_set_enabled(false, false);
	if (z_pid_lookup("wm0", &wm_pid))
		z_msg_new_send(wm_pid, Z_WM_REPAINT, 0, z_obj_uint32(0));

	printf("chunkytest: copy of 320x240: avg %lu cycles (%lu a line), "
		"worst %lu (%lu a line); %lu of %u frames late\n",
		(unsigned long)(sum / FRAMES), (unsigned long)(sum / FRAMES / H),
		(unsigned long)worst, (unsigned long)(worst / H),
		(unsigned long)late, FRAMES);
	return 0;
}
