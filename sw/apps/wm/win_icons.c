/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Window titlebar icon bitmap data -- hand-edited, not generated. See
 * win_icons.h for the format/convention and how to add a new one.
 */

#include "../../common/zgfx.h"
#include "../../common/zicon.h"
#include "win_icons.h"

// 8x8, MSB-first, bit set = ink. A hollow box: a 6x6 outline centered
// in the 8x8 cell (one pixel of margin on every side).
const uint8_t z_icon_close_data[Z_ICON_H] = {
	0b00000000,
	0b01111110,
	0b01000010,
	0b01000010,
	0b01000010,
	0b01000010,
	0b01111110,
	0b00000000,
};

void z_win_icons_load(void) {
	z_gfx_hw_icon_load(Z_ICON_CLOSE, z_icon_close_data);
}
