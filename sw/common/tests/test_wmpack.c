/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * wm's packed coordinates (zwm.h, docs/screen.md): 12 bits each, and
 * below 1024 the same words as when they were 10 bits.
 */

#include <stdio.h>
#include <stdint.h>
#include "zwm.h"

static int run, failed;
#define CHECK(c, what) do { run++; if (!(c)) { failed++; printf("FAIL: %s\n", what); } } while (0)

// the 10-bit packs as they were, for the comparison
#define OLD_MOUSE(x, y, b, in) ((((in) ? 1u : 0u) << 24) | (((uint32_t)(b) & 0xF) << 20) | \
	(((uint32_t)(y) & 0x3FF) << 10) | ((uint32_t)(x) & 0x3FF))
#define OLD_XY(id, x, y) ((((uint32_t)(id) & 0xFF) << 20) | (((uint32_t)(x) & 0x3FF) << 10) | \
	((uint32_t)(y) & 0x3FF))
#define OLD_PLACE(x, y) ((((uint32_t)(y) & 0x3FF) << 10) | ((uint32_t)(x) & 0x3FF))

int main(void) {

	int same = 1;
	for (int x = 0; x < 1024; x += 7)
		for (int y = 0; y < 1024; y += 11) {
			if (Z_WM_PACK_MOUSE(x, y, 5, 1) != OLD_MOUSE(x, y, 5, 1)) same = 0;
			if (Z_WM_PACK_XY(9, x, y) != OLD_XY(9, x, y)) same = 0;
			if (Z_WM_PACK_PLACE(x, y) != OLD_PLACE(x, y)) same = 0;
		}
	CHECK(same, "below 1024, every pack is the word it was");

	int round = 1;
	for (int x = 0; x <= 4095; x += 13)
		for (int y = 0; y <= 4095; y += 17) {
			uint32_t m = Z_WM_PACK_MOUSE(x, y, 0xF, 1);
			if ((int)Z_WM_UNPACK_MOUSE_X(m) != x || (int)Z_WM_UNPACK_MOUSE_Y(m) != y ||
				Z_WM_UNPACK_MOUSE_BUTTONS(m) != 0xF || Z_WM_UNPACK_MOUSE_INSIDE(m) != 1) round = 0;
			uint32_t p = Z_WM_PACK_PLACE(x, y);
			if (Z_WM_UNPACK_PLACE_X(p) != x || Z_WM_UNPACK_PLACE_Y(p) != y) round = 0;
			if (y <= 2047) {
				uint32_t v = Z_WM_PACK_XY(255, x, y) | Z_WM_REDRAW_DAMAGE;
				if ((int)Z_WM_UNPACK_X(v) != x || (int)Z_WM_UNPACK_Y(v) != y ||
					Z_WM_UNPACK_ID(v) != 255) round = 0;
			}
		}
	CHECK(round, "x and y round-trip to 4095 (the window position's y to 2047)");

	uint32_t v = Z_WM_PACK_XY(0, 4095, 2047);
	CHECK(!(v & Z_WM_REDRAW_DAMAGE), "a position never sets the damage bit");
	CHECK((Z_WM_PACK_MOUSE(4095, 4095, 0xF, 1) >> 29) == 0, "the mouse pack leaves 31:29 reserved");
	CHECK((Z_WM_PACK_PLACE(4095, 4095) >> 24) == 0, "the place pack leaves 31:24 reserved");
	CHECK(Z_WM_UNPACK_MOUSE_X(Z_WM_PACK_MOUSE(1920, 1200, 1, 0)) == 1920 &&
		Z_WM_UNPACK_MOUSE_Y(Z_WM_PACK_MOUSE(1920, 1200, 1, 0)) == 1200, "1920x1200's corner");

	printf("test_wmpack: %d checks, %d failed\n", run, failed);
	return failed != 0;

}
