#ifndef VSCREEN_H
#define VSCREEN_H

/*
 * view -- its screen face: the picture on a 320x240 screen, in colour,
 * on the arcade runtime (docs/view_app.md, "In colour"; view is a dual
 * app, docs/app_kinds.md).
 *
 * The picture is decoded again, in colour (zimg's RGBA rows), and
 * fitted into 320x240: kept whole, scaled down to the larger of the
 * two fits by nearest sampling, centred on black. Its colours are
 * reduced to what the screen shows -- 32 (8) in the colour window, 16,
 * 64 or 256 full screen -- by median cut, and again whenever that
 * changes. Without colour it is dithered to black and white. A source
 * the colour decoders cannot read (an SVG) is taken from view's own
 * one-bit document instead.
 *
 * Pure C over a byte source: tests/render.c runs it on the build host.
 */

#include <stdint.h>
#include <stdbool.h>

/* Where the bytes come from, and how to start again. */
typedef struct {
	int  (*read)(void *ctx, uint8_t *buf, int len);
	bool (*rewind)(void *ctx);
	void *ctx;
} vs_src_t;

/* The picture from `src`, fitted. False if it cannot be decoded in
 * colour (then vs_from_bits() below). */
bool vs_load(vs_src_t *src);

/* The picture from a one-bit document (zbm packing: bit x of word x/32,
 * LSB leftmost, `wpl` words a row), w x h of it, lit pixels white. */
void vs_from_bits(const uint32_t *bits, int wpl, int w, int h);

/* Nothing to show. */
void vs_clear(void);

/* The screen face, in `win` (view's z_win_t), until F10, the colour
 * icon, a resize or Esc in the window asks for the desktop face again:
 * then VS_DESKTOP. `full` starts full screen (F11 from the desktop
 * face). VS_QUIT: the window was closed; VS_OPEN: the open icon -- the
 * caller opens its file box. */
enum { VS_DESKTOP = 0, VS_QUIT, VS_OPEN };
int vs_run(void *win, bool full, const char *name);

/* The flags view's window needs for the screen face (ar_window_flags()):
 * game mode (F11 is view's), the colour window and its icon where the
 * board has them. ar_window_colour(true) first. */
uint32_t vs_window_flags(void);

#endif
