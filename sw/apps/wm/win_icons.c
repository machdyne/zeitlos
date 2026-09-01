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

// box with x
const uint8_t z_icon_new_data[Z_ICON_H] = {
	0b00100100,
	0b00011000,
	0b00100100,
	0b00000000,
	0b01111110,
	0b01000010,
	0b01111110,
	0b00000000,
};

// box with down arrow
const uint8_t z_icon_save_data[Z_ICON_H] = {
	0b01111110,
	0b00111100,
	0b00011000,
	0b00000000,
	0b01111110,
	0b01000010,
	0b01111110,
	0b00000000,
};

// box with up arrow
const uint8_t z_icon_open_data[Z_ICON_H] = {
	0b00011000,
	0b00111100,
	0b01111110,
	0b00000000,
	0b01111110,
	0b01000010,
	0b01111110,
	0b00000000,
};

// two letter A's at different sizes
const uint8_t z_icon_font_data[Z_ICON_H] = {
	0b00000000,
	0b00100000,
	0b01010000,
	0b01110010,
	0b01010101,
	0b00000111,
	0b00000101,
	0b00000000,
};

// A closed folder, for a directory row in a file list. The notch at
// the top left is the tab.
const uint8_t z_icon_folder_data[Z_ICON_H] = {
	0b00000000,
	0b01110000,
	0b11111110,
	0b10000010,
	0b10000010,
	0b10000010,
	0b11111110,
	0b00000000,
};

// A plain page with a folded corner, for a file row. The same outline
// as Z_ICON_NEW but with two "text" rules inside it, so a file list
// row doesn't read as a column of new-document buttons.
const uint8_t z_icon_file_data[Z_ICON_H] = {
	0b00111100,
	0b00100110,
	0b00100010,
	0b00111111,
	0b00101101,
	0b00100001,
	0b00101101,
	0b00111111,
};

// An up arrow, for the ".." row that leaves a directory.
const uint8_t z_icon_updir_data[Z_ICON_H] = {
	0b00000000,
	0b00011000,
	0b00111100,
	0b01111110,
	0b00011000,
	0b00011000,
	0b00011000,
	0b00000000,
};

void z_win_icons_load(void) {
	z_gfx_hw_icon_load(Z_ICON_CLOSE,  z_icon_close_data);
	z_gfx_hw_icon_load(Z_ICON_NEW,    z_icon_new_data);
	z_gfx_hw_icon_load(Z_ICON_SAVE,   z_icon_save_data);
	z_gfx_hw_icon_load(Z_ICON_OPEN,   z_icon_open_data);
	z_gfx_hw_icon_load(Z_ICON_FONT,   z_icon_font_data);
	z_gfx_hw_icon_load(Z_ICON_FOLDER, z_icon_folder_data);
	z_gfx_hw_icon_load(Z_ICON_FILE,   z_icon_file_data);
	z_gfx_hw_icon_load(Z_ICON_UPDIR,  z_icon_updir_data);
}
