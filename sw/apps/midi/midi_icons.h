#ifndef MIDI_ICONS_H
#define MIDI_ICONS_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Transport button faces for sw/apps/midi.
 *
 * 16x16, 1bpp, two bytes per row, MSB first -- the shape
 * z_widget_t.icon takes (sw/common/zwidget.h), which is the same
 * convention as zicon.h and z_font_t glyph data.
 *
 * Drawn per-pixel in software by the widget toolkit rather than
 * through the glyph blitter, and that is deliberate rather than an
 * oversight: glyph memory (rtl/mem/glyph.v) holds one font at a time
 * and wm is by convention the only process that writes to it (see
 * docs/window_manager.md). An app loading its own icons there
 * reintroduces exactly the cross-process race that convention exists
 * to prevent. These are 16x16 and repaint only when the transport
 * state actually changes -- three or four times a minute -- so
 * software costs nothing that matters.
 *
 * Deliberately the universal transport glyphs rather than anything
 * clever. A triangle, two bars and a square need no label and no
 * tooltip, and this window has neither to spare.
 */

#include <stdint.h>

extern const uint8_t midi_icon_play[32];
extern const uint8_t midi_icon_pause[32];
extern const uint8_t midi_icon_stop[32];

#endif
