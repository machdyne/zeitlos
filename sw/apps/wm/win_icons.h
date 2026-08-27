#ifndef WIN_ICONS_H
#define WIN_ICONS_H

#include <stdint.h>

#include "../../common/zicon.h"

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Window titlebar icon bitmap data -- 8x8 1bpp glyphs (see
 * sw/common/zicon.h for the format/addressing convention, and wm.c's
 * draw_titlebar_content() for how these actually get drawn). Hand-
 * edited directly with binary literals (one byte per glyph row,
 * MSB-first -- bit 7 is the LEFTMOST pixel, matching
 * sw/common/zfont_data.c's own convention) rather than generated from
 * a PNG the way dock_icons.c is (see dock_icons.h): at 8x8, a shape
 * this simple is easier to eyeball and tweak directly as bits than to
 * round-trip through an image editor and a generator script every
 * time.
 *
 * To add a new window icon: append an id to z_icon_id_t (zicon.h,
 * right before Z_ICON_ID_COUNT), add its 8-byte bitmap below using
 * the same binary-literal style, and add one z_gfx_hw_icon_load()
 * call for it in z_win_icons_load() (win_icons.c) -- same three-step
 * shape dock_apps[]'s own comment describes for dock icons, just
 * hand-edited instead of generated.
 */

// Z_ICON_CLOSE: an 8x8 hollow box -- deliberately NOT an X. The mouse
// cursor itself is drawn as an X (rtl/gpu/gpu_cursor.v), so a close
// icon shaped the same way as the pointer sitting on top of it would
// be genuinely hard to read at a glance, not just an aesthetic
// mismatch.
extern const uint8_t z_icon_close_data[Z_ICON_H];

// The extra titlebar icons -- see Z_WIN_FLAG_NEW_ICON and friends in
// sw/common/zwm.h for what asking for one actually does, and wm.c's
// titlebar_icons() for how they get placed.
extern const uint8_t z_icon_new_data[Z_ICON_H];
extern const uint8_t z_icon_save_data[Z_ICON_H];
extern const uint8_t z_icon_open_data[Z_ICON_H];
extern const uint8_t z_icon_font_data[Z_ICON_H];

// NOT titlebar icons -- these three are for the file-list widget's
// own rows (sw/common/zflist.h). They live here anyway because this
// is the one place that loads glyph memory's icon region, and wm is
// the only process allowed to do that (zicon.h). An app that wants a
// new icon of its own adds it here, not in the app.
extern const uint8_t z_icon_folder_data[Z_ICON_H];
extern const uint8_t z_icon_file_data[Z_ICON_H];
extern const uint8_t z_icon_updir_data[Z_ICON_H];

// pushes every window icon above into hardware glyph memory's
// reserved icon region (zicon.h) via z_gfx_hw_icon_load() (zgfx.h).
// Call once, from wm's own startup, right after z_gfx_hw_font_load()
// -- see wm.c's main() and zicon.h's own header comment for why wm is
// the sole owner of glyph memory, icons included, same single-owner
// discipline the font already follows.
void z_win_icons_load(void);

#endif
