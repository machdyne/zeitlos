#ifndef DRAW_ICONS_H
#define DRAW_ICONS_H

/*
 * draw -- 16x16 tool icons
 *
 * Hand-authored, not generated from PNGs the way the dock's 32x32
 * icons are (sw/data/icons/gen_dock_icon_data.py) -- at this size the
 * binary literals below ARE the source form, and are easier to nudge a
 * pixel at a time than a PNG round trip would be. Same reasoning
 * sw/apps/wm/win_icons.h gives for the 8x8 titlebar icons.
 *
 * Format: 16 rows, 2 bytes per row, MSB-first, left byte then right --
 * the convention z_widget_t.icon (zwidget.h) expects, which is itself
 * the convention zicon.h and z_font_t glyph data already use.
 */

#include <stdint.h>

extern const uint8_t draw_icon_pencil[32];
extern const uint8_t draw_icon_brush[32];
extern const uint8_t draw_icon_eraser[32];
extern const uint8_t draw_icon_line[32];
extern const uint8_t draw_icon_rect[32];
extern const uint8_t draw_icon_rectfill[32];
extern const uint8_t draw_icon_oval[32];
extern const uint8_t draw_icon_bucket[32];

#endif
