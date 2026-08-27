#ifndef ZICON_H
#define ZICON_H

#include <stdint.h>

#include "zeitlos.h"

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Window icons -- small, fixed-size (8x8) glyphs blitted into a
 * window's titlebar (the close button, and any future titlebar icon:
 * minimize, open file, save file, etc -- see sw/apps/wm/wm.c's file
 * header comment). NOT to be confused with "dock icons"
 * (sw/apps/wm/dock_icons.h), which are unrelated 32x32 app-launcher
 * bitmaps drawn straight from wm's own .rodata via z_fb_set_pixel().
 * Window icons instead live in hardware glyph memory
 * (rtl/mem/glyph.v) and are drawn through the hardware glyph blitter
 * (rtl/gpu/gpu_blit.v) -- same mechanism as font text -- so every
 * piece of window chrome wm draws goes through the GPU rather than a
 * software pixel loop. See zgfx.h's z_gfx_hw_icon_load()/
 * z_fb_draw_icon() for the drawing side of this.
 *
 * Glyph memory (GLYPH_MEM_SIZE, zeitlos.h -- 4096 bytes) holds font
 * data at the FRONT, starting at offset 0 (z_gfx_hw_font_load(),
 * zgfx.h) and window icons in a small region reserved at the very
 * END, growing downward from GLYPH_MEM_SIZE. This split means adding
 * or resizing window icons never disturbs font glyph addressing
 * (always (codepoint-first)*font_h, from offset 0), and the two
 * regions can only ever collide if the font data itself grows to
 * fill nearly all of glyph memory -- z_font_5x8 currently uses 768
 * bytes (96 glyphs * 8 rows), nowhere near Z_ICON_MEM_OFFSET below
 * even for the largest font declared in zfont.h (z_font_8x16: 96 * 16
 * = 1536 bytes).
 */

#define Z_ICON_W   8
#define Z_ICON_H   8

// how many 8x8 icon slots are reserved at the end of glyph memory --
// deliberately generous (room to add "minimize"/"open file"/"save
// file"/etc, see wm.c's own file header comment, without ever
// touching this constant again) while staying tiny relative to
// GLYPH_MEM_SIZE: 32 slots * 8 bytes/icon = 256 bytes.
#define Z_ICON_SLOTS   32

// byte offset within glyph memory (added to GLYPH_MEM_BASE by
// z_gfx_hw_icon_load()/z_fb_draw_icon() -- zgfx.c -- same convention
// z_gfx_hw_font_load() uses for font data at offset 0) where the icon
// region starts. Icon N's 8 row-bytes live at
// [Z_ICON_MEM_OFFSET + N*Z_ICON_H, +Z_ICON_H).
#define Z_ICON_MEM_OFFSET   (GLYPH_MEM_SIZE - (Z_ICON_SLOTS * Z_ICON_H))

// window icon ids -- index into the icon region above (0..Z_ICON_SLOTS-1).
// Assign new ones by appending, never by renumbering existing ones --
// nothing persists these across a reboot today, but there's no reason
// to invite confusion later for zero benefit now. See
// sw/apps/wm/win_icons.h/.c for the actual bitmap data, and wm.c's
// draw_titlebar_content() for how these get drawn.
// Note these are NOT all titlebar icons. Z_ICON_FOLDER/_FILE/_UPDIR
// are drawn by APPS, into their own content areas -- the file-list
// widget (sw/common/zflist.h) puts one at the start of every row.
// That's fine and doesn't break the single-owner rule zicon.h's
// header comment describes: wm remains the only process that WRITES
// glyph memory, and z_fb_draw_icon() (zgfx.h) only reads it. An app
// drawing an icon wm has already loaded costs nothing and is one
// blitter op instead of a 64-iteration pixel loop.
typedef enum {
	Z_ICON_CLOSE = 0,
	Z_ICON_NEW,
	Z_ICON_SAVE,
	Z_ICON_OPEN,
	Z_ICON_FONT,
	Z_ICON_FOLDER,
	Z_ICON_FILE,
	Z_ICON_UPDIR,
	Z_ICON_ID_COUNT		// keep last
} z_icon_id_t;

#endif
