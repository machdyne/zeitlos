#ifndef ZGFX_H
#define ZGFX_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Direct-framebuffer pixel/text drawing, plus (see z_fb_hw_line()
 * below) IRQ-masked access to the GPU line rasterizer
 * (rtl/gpu/gpu_raster.v). The two are different enough to be worth
 * calling out: z_fb_set_pixel()/z_fb_fill_rect()/etc. are plain
 * memory writes to the 1bpp framebuffer, so two apps drawing into two
 * different (non-overlapping) windows can't race on anything -- no
 * shared state involved at all. z_fb_hw_line() is different: the
 * rasterizer's registers are global, shared peripheral state with no
 * per-process isolation whatsoever, so any two processes drawing
 * through it *can* race, in two distinct ways -- see z_fb_hw_line()'s
 * own comment for both, and why IRQ masking (not avoiding the
 * hardware, which is what this file did originally) is what actually
 * closes them.
 *
 * All coordinates are absolute screen coordinates. z_win.h layers
 * window-relative, auto-clipped helpers on top of this for apps that
 * just want to draw inside their own window.
 *
 * Character/text drawing (z_fb_draw_char/z_fb_draw_text) has two
 * implementations, selected at compile time by defining Z_GFX_HW_BLIT
 * (add `-DZ_GFX_HW_BLIT` to CFLAGS, and link rtl-side glyph memory
 * must actually be present in the bitstream -- see rtl/mem/glyph.v
 * and MEM_GLYPH in rtl/sysctl.v):
 *   - undefined (default): the original software renderer, direct
 *     per-pixel writes. Always correct, always available.
 *   - defined: drives the hardware glyph blitter (rtl/gpu/gpu_blit.v)
 *     instead, which is dramatically faster for anything more than a
 *     handful of characters -- but falls back to the software path
 *     for any glyph that would need partial clipping, since the
 *     hardware blit is unclipped by design. See
 *     docs/window_manager.md, "hardware glyph blitting" for the full
 *     writeup, including what hasn't been tested on real hardware
 *     yet.
 */

#include <stdint.h>
#include <stdbool.h>
#include "zfont.h"
#include "zicon.h"

#define Z_SCREEN_W 640
#define Z_SCREEN_H 480

// inclusive bounds
typedef struct {
	int32_t x0, y0, x1, y1;
} z_clip_t;

// clip may be NULL to clip to the screen only
void z_fb_set_pixel(int x, int y, int color, const z_clip_t *clip);
void z_fb_fill_rect(int x, int y, int w, int h, int color, const z_clip_t *clip);
void z_fb_draw_char(int x, int y, char c, int color, const z_font_t *font, const z_clip_t *clip);
void z_fb_draw_text(int x, int y, const char *s, int color, const z_font_t *font, const z_clip_t *clip);

// like z_fb_draw_char()/z_fb_draw_text(), but with fg AND bg
// independently colorable, instead of z_fb_draw_char()'s bg hardcoded
// to 0 (see its own comment). The hardware glyph blitter
// (rtl/gpu/gpu_blit.v) already has two genuinely independent color
// registers (fg_color_reg/bg_color_reg, each latched into every pixel
// of the cell depending on that pixel's glyph bit -- see
// ST_GLYPH_WRITE_LO's read-modify-write) -- z_fb_draw_char() just
// never exposed the second one. Useful for anything drawing solid
// two-color cells, e.g. a terminal emulator's reverse-video attribute
// (sw/apps/term) -- swap fg_color/bg_color for a "reverse" cell
// instead of falling back to per-pixel software rendering, which is
// what motivated adding this: sw/apps/term used to do exactly that
// (see its own git history) and redrawing a full 80x25 grid through
// z_fb_set_pixel() one pixel at a time was slow enough to be visibly
// laggy while typing.
void z_fb_draw_char2(int x, int y, char c, int fg_color, int bg_color, const z_font_t *font, const z_clip_t *clip);
void z_fb_draw_text2(int x, int y, const char *s, int fg_color, int bg_color, const z_font_t *font, const z_clip_t *clip);

// -- raster operations --
//
// What the `color` argument of z_fb_hw_line()/z_fb_hw_box() (and
// z_win_hw_line()/z_win_hw_box(), zwin.h) actually selects. It has
// always been a colour; it is now a two-bit OPERATION, of which the
// first two values are the old colours unchanged -- every existing
// caller passing 0 or 1 keeps exactly the behaviour it had.
//
// Z_RASTER_XOR is the addition. XOR is its own inverse, so drawing a
// shape a second time in the same place restores whatever was under
// it, exactly, with nothing saved anywhere and no knowledge of what
// was there. On a 1bpp framebuffer with no per-pixel ownership that
// is the only way to draw something temporary over content you do not
// own -- a drag outline, a rubber band, a selection marquee -- and
// take it away again without damaging what it crossed. See
// docs/xor_raster_op.md.
//
// An XOR shape INVERTS what it crosses, so it appears black over
// white areas and white over black. That is the intended look and
// what makes it visible against any background.
//
// Requires gateware built from this tree's rtl/gpu/gpu_raster.v, which
// decodes the two-bit op field. There is deliberately no capability
// bit for it: the rasterizer either has the field or it doesn't, that
// is decided by which gateware you flashed, and gateware and software
// are built and flashed together here. Values 0 and 1 are unchanged
// from the one-bit colour they replaced, so the two sides remain
// compatible in either direction regardless.
#define Z_RASTER_CLEAR   0
#define Z_RASTER_SET     1
#define Z_RASTER_XOR     2

// hardware-accelerated line draw via the GPU line rasterizer
// (rtl/gpu/gpu_raster.v). Handles both hazards of that shared
// hardware state internally -- callers never need to think about
// either one:
//   - clip == NULL disables the rasterizer's own hardware clipping
//     entirely (for callers, like wm.c, that already know their
//     coordinates are valid and never want clipping); clip != NULL
//     sets the rasterizer's clip region to those bounds first. Either
//     way this is asserted fresh on every single call, never assumed
//     to still be correctly set from a previous call, since some
//     other process's own drawing may have changed it in the
//     meantime.
//   - the register-writes-then-trigger sequence is wrapped in
//     maskirq() so it can't be interleaved with another process's own
//     sequence (which would otherwise produce a line drawn from a mix
//     of two callers' parameters) -- but only that sequence, not the
//     FIFO-wait beforehand, which can legitimately take a while if
//     the FIFO's backed up and shouldn't stall the scheduler for
//     other processes while it does.
void z_fb_hw_line(int x0, int y0, int x1, int y1, int color, const z_clip_t *clip);

// four z_fb_hw_line() calls forming a rectangle outline -- see
// z_fb_hw_line() for the clip/coordinate semantics.
void z_fb_hw_box(int x0, int y0, int x1, int y1, int color, const z_clip_t *clip);

// hardware-accelerated rectangle fill via the GPU blitter
// (rtl/gpu/gpu_blit.v, fill mode -- see docs/gpu_blitter.md). Same
// two hazards as z_fb_hw_line()/the line rasterizer, both closed the
// same way -- see that function's own comment for the reasoning in
// full:
//   - x/y/w/h are clamped to the actual screen bounds unconditionally,
//     not just when the blitter's own CTRL_CLIP is set: rtl/gpu/
//     gpu_blit.v's ST_CLIP state only bounds-checks the destination
//     when CTRL_CLIP is requested, and even then, ST_WAIT_READ/
//     ST_WAIT_WRITE wait on the framebuffer bus's ack with no timeout
//     of their own -- an out-of-range destination can hang the
//     blitter's state machine forever, same as the rasterizer.
//   - the register-writes-then-trigger sequence is IRQ-masked, for
//     the same reason and the same way as z_fb_hw_line().
// Unlike z_fb_hw_line() (which only waits for FIFO space before
// submitting, letting the hardware drain asynchronously), this waits
// for the blitter to actually finish before returning: the blitter
// has no FIFO/queue at all (one operation at a time, globally shared
// -- see "Concurrent register access" in docs/gpu_blitter.md, still
// not arbitrated between processes), and whatever a caller does right
// after a fill is very often *not* going through this same hardware
// (e.g. wm.c's own draw_window_box(), which draws through the line
// rasterizer instead) -- so there's no other natural point where that
// next operation would otherwise wait for this fill's pixel writes to
// have actually landed.
void z_fb_hw_fill_rect(int x, int y, int w, int h, int color);

// like z_fb_hw_fill_rect(), but fills with an 8x8 1bpp PATTERN
// instead of a solid color -- the blitter's BLIT_PATTERN register has
// always been a full 32-bit word (docs/gpu_blitter.md), and
// z_fb_hw_fill_rect() simply hardcodes it to all-0s or all-1s. This
// exposes the rest of it.
//
// `pat` is 8 bytes, one per pattern row, MSB-first -- the same
// row-major/bit-order convention z_font_t glyphs and zicon.h window
// icons already use, so a pattern can be written as eight binary
// literals and read as a picture of itself. See Z_PATTERN_* in
// zwidget.h for a ready-made MacPaint-style set.
//
// The pattern is anchored to the SCREEN's own 8-pixel grid, not to
// (x,y): two adjacent rects filled with the same pattern tile
// seamlessly into each other rather than each restarting the pattern
// at its own corner. That falls out of how the hardware works (the
// blitter writes the pattern word into destination words, and both 32
// and the framebuffer's word grid are multiples of 8) and is also the
// behavior you want -- it's what makes a pattern read as a texture
// the shapes are cut out of, which is exactly the early-MacPaint
// model.
//
// Cost is one blitter operation per RUN OF ROWS SHARING A PATTERN
// BYTE, not per row: a solid or 8-row-uniform pattern costs a single
// op, the pathological alternating case costs h. Worth knowing before
// using this for anything in a tight loop -- for per-pixel work on an
// offscreen 1bpp buffer, do it in software instead (the blitter only
// ever addresses the visible framebuffer).
void z_fb_hw_fill_pattern(int x, int y, int w, int h, const uint8_t *pat);

// hardware copy of a 1bpp bitmap from MAIN MEMORY into the framebuffer
// (rtl/gpu/gpu_blit.v, CTRL_SRCMEM). This is what makes an offscreen
// document buffer practical: the alternative is a software loop moving
// words into VRAM one at a time, which is what sw/apps/draw used to do.
//
// `src` points at the bitmap; `src_stride` is its row pitch in BYTES.
// The bitmap's bit order must match the framebuffer's -- pixel x at bit
// (x & 31) of word (x >> 5), least significant bit leftmost, the same
// convention z_fb_set_pixel() uses. src_x/src_y select a rectangle
// within it; dst_x/dst_y place it on screen. Source and destination
// need no particular alignment to each other: the hardware slides a
// 64-bit window along the source row, so an odd offset costs the same
// as an aligned one.
//
// Clipped to the screen only, exactly like z_fb_hw_fill_rect() -- a
// caller that needs to clip to something smaller (a window's content
// area, a scrolling viewport) must narrow the rectangle itself before
// calling, adjusting src_x/src_y in step.
//
// Returns false, having drawn nothing, on a bitstream whose blitter
// predates this mode. Callers that must work on both should check
// z_fb_hw_blit_mem_available() once at startup and keep a software
// path -- see sw/apps/draw's canvas_blit().
//
// Two constraints worth knowing:
//   - `src` is translated to a physical address internally (the
//     blitter bypasses the MTU), so it must be ordinary app or kernel
//     memory that stays put. It cannot be a pointer into VRAM.
//   - the hardware may read up to one word beyond the last source word
//     it actually needs, whenever source and destination are not
//     word-aligned to each other. Those bits are masked out of the
//     result, but the read does happen, so `src` should not end exactly
//     at the last valid byte of a mapping.
bool z_fb_hw_blit_mem(const void *src, int src_stride,
	int src_x, int src_y, int dst_x, int dst_y, int w, int h);

// -- raster operations (rtl/gpu/gpu_blit.v, CTRL bits 6:5) --
//
// How the source combines with what is already on screen. Every blit
// and fill function above takes Z_ROP_COPY implicitly, which is what
// they have always done; the _rop variants below take one explicitly.
//
// Z_ROP_ANDN is (dst & ~src), not plain AND, because the operation
// sprites actually need is "punch a hole shaped like this mask" -- and
// with plain AND every mask would have to be stored inverted.
#define Z_ROP_COPY 0   // dst = src
#define Z_ROP_OR   1   // dst = dst | src
#define Z_ROP_XOR  2   // dst = dst ^ src
#define Z_ROP_ANDN 3   // dst = dst & ~src

bool z_fb_hw_blit_mem_rop(const void *src, int src_stride,
	int src_x, int src_y, int dst_x, int dst_y, int w, int h, int rop);

void z_fb_hw_fill_rect_rop(int x, int y, int w, int h, int color, int rop);

// -- masked sprite, in hardware --
//
// The thing raster ops exist for. Two passes over the same rectangle:
// ANDN the mask to clear exactly the pixels the sprite will occupy,
// then OR the data to lay them in. Background outside the mask is
// untouched.
//
//   mask: 1 wherever the sprite is opaque
//   data: the sprite's own pixels, 0 wherever the mask is 0
//
// Both planes share src_stride and the src_x/src_y rectangle, so a
// sprite sheet can keep them as two bitmaps of identical geometry --
// which is what sw/apps/gamedemo's gen_sprites.py emits.
//
// TWO passes and not one: a single-pass masked blit would need the
// hardware to fetch two source streams at once, which is a second
// source port and a second shifter. Two passes reuse everything and
// cost one extra read-modify-write per word, on an engine that is
// already far faster than the software loop this replaces.
//
// Not atomic, therefore. Between the two passes the sprite's footprint
// is momentarily blank, so a sprite drawn directly into the VISIBLE
// page can tear into a one-frame hole. Draw into the back page and
// flip (sw/common/zgame.h), which a game is doing anyway.
//
// Returns false, having drawn nothing, if this bitstream's blitter has
// no raster ops -- check z_fb_hw_rop_available() once at startup.
bool z_fb_hw_blit_sprite(const void *data, const void *mask, int src_stride,
	int src_x, int src_y, int dst_x, int dst_y, int w, int h);

// true if this bitstream's blitter implements raster ops. Probed the
// same way, and for the same reason, as z_fb_hw_blit_mem_available():
// the same binary may be run against an older bitstream, where the ROP
// bits are simply ignored and every op silently behaves as COPY --
// which for a sprite means an opaque box rather than nothing at all,
// and is exactly the kind of thing that gets misdiagnosed as bad art.
bool z_fb_hw_rop_available(void);

// True if the blitter can do a masked sprite in ONE pass (CTRL bit 7).
//
// z_fb_hw_blit_sprite() uses it automatically where present and falls
// back to ANDN-then-OR where not, so most callers never need to ask.
// Ask when it changes what you can safely do: a one-pass sprite is
// ATOMIC and can go straight onto the visible page, where the two-pass
// fallback leaves the footprint momentarily blank and must draw into a
// back buffer.
bool z_fb_hw_cookie_available(void);

// -- shaded fills --
//
// A grey, on a display that has none: the blitter generates a 4x4
// ordered dither for `level`, 0 (black) to Z_SHADE_MAX (solid). 17
// evenly spaced levels.
//
// SCREEN-ALIGNED, which is the property that makes this useful rather
// than decorative. The pattern is indexed by absolute framebuffer row
// and column, never by the rectangle -- so two adjacent shaded fills
// join with no seam, and a region redrawn at a different offset comes
// out identical. A rectangle-relative pattern gets both wrong: visible
// seams where fills meet, and a shimmer whenever anything scrolls.
//
// What it is for, beyond 3D: disabled controls, scrollbar troughs,
// chart shading, selection washes, and textured backgrounds drawn as
// one fill rather than a blit per tile.
//
// A shaded SPAN is one fill, which is what lets a software triangle
// rasterizer emit flat-shaded polygons without any triangle hardware.
//
// Falls back to a plain black or white fill where the bitstream has no
// dither support -- wrong but legible, rather than filling with the
// level as a bit pattern, which would be noise.
#define Z_SHADE_MAX 16

void z_fb_hw_fill_shade(int x, int y, int w, int h, int level);
void z_fb_hw_fill_shade_async(int x, int y, int w, int h, int level);

bool z_fb_hw_dither_available(void);

// -- shaded spans, for a software rasterizer --
//
// z_fb_hw_span_begin(level) once per face, then z_fb_hw_span(x, y, w)
// per scanline. Height and grey level are hoisted out of the loop, so
// each span costs three register writes instead of six.
//
// That matters at rasterizer rates: hundreds of spans a frame, each
// register write a stalled bus cycle from the CPU. Halving the traffic
// is the difference between keeping the blitter fed and not.
//
// z_fb_hw_span() does NOT clip and does not validate. It is the inner
// loop of something that has already clipped; re-checking would put the
// comparisons back that the hoisting removed. Anything that has not
// clipped must call z_fb_hw_fill_shade() instead.
void z_fb_hw_span_begin(int level);
void z_fb_hw_span(int x, int y, int w);

// Clear one scanline span, same lean path. For a rasterizer erasing
// what its silhouette no longer covers -- which lets it draw first and
// erase after, so the object is never blanked and nothing flashes.
void z_fb_hw_span_clear(int x, int y, int w);

// -- asynchronous blits --
//
// Every function above returns only once the blitter has finished. The
// _async variants return as soon as the operation has been STARTED, so
// the caller gets the blit duration back to do something else with.
//
// -- why this is safe, and why it needs almost no new machinery --
//
// gpu_blit_acquire() (zgfx.c) already waits for the blitter to go idle
// BEFORE it starts anything, and re-checks with interrupts masked. So
// the trailing wait these functions do is not what makes back-to-back
// blits correct -- the leading wait in the next one is. Dropping the
// trailing wait therefore changes when the caller regains control, and
// nothing else. A sequence of async blits is exactly as correct as a
// sequence of synchronous ones, cross-process races included.
//
// -- the one thing a caller MUST do --
//
// An async blit is still running when the call returns. Anything that
// reads or writes the affected pixels with the CPU -- z_fb_get_pixel(),
// a software sprite path, a direct VRAM poke -- must call
// z_fb_hw_sync() first, or it will race the hardware and see a
// half-drawn rectangle. Another BLIT is fine; only CPU access needs
// the barrier.
//
// The synchronous functions are unchanged and remain the default. This
// is opt-in on purpose: the callers that benefit are the ones with
// real work to overlap, and the ones that don't benefit would only
// gain a new way to get it wrong.
//
// -- how much this actually buys --
//
// Nothing at all if the caller immediately blocks. The gain is exactly
// the CPU work done between starting a blit and needing it finished,
// capped at the blit's own duration. A loop that blits and does
// nothing else runs at the same speed either way; a loop that computes
// the next item's position, looks up its bitmap, or steps its physics
// gets that work for free.
//
// So the useful shape is: start the blit for item N, prepare item N+1,
// start its blit (which waits for N as a side effect), and so on.
bool z_fb_hw_blit_mem_async(const void *src, int src_stride,
	int src_x, int src_y, int dst_x, int dst_y, int w, int h, int rop);

void z_fb_hw_fill_rect_async(int x, int y, int w, int h, int color);

// Masked sprite, started but not waited for.
//
// NOTE this is two blits internally (see z_fb_hw_blit_sprite above), so
// it necessarily waits for the first pass before starting the second --
// only the SECOND is left running when this returns. Half the win of
// the synchronous version, until the blitter grows a single-pass
// cookie-cut mode.
bool z_fb_hw_blit_sprite_async(const void *data, const void *mask,
	int src_stride, int src_x, int src_y, int dst_x, int dst_y,
	int w, int h);

// true if this bitstream's blitter implements the mode above. Probes
// the hardware (writes the mode bit without a start bit and reads it
// back) rather than assuming, so the same binary runs on an older
// bitstream instead of silently drawing nothing.
bool z_fb_hw_blit_mem_available(void);

// hardware copy of a rectangle from one part of VRAM to another
// (rtl/gpu/gpu_blit.v with CTRL_FILL and CTRL_SRCMEM both clear).
//
// Same engine, same arbitrary bit alignment and same clipping as
// z_fb_hw_blit_mem() above -- the only difference is that the source is
// read through the blitter's framebuffer port instead of its
// main-memory port, so this needs no main-bus traffic at all.
//
// src_x/src_y are in VRAM pixel coordinates, using the framebuffer's
// own 640-pixel stride. That deliberately allows a source ABOVE or
// BELOW the visible area: on a bitstream whose VRAM is larger than
// 640x480 (rtl/mem/vram.v), rows past 479 are offscreen storage, and
// this is how a sprite kept there gets drawn. On a bitstream with
// exactly 640x480 of VRAM there is no such region and this is only
// useful for moving visible pixels around.
//
// Overlapping source and destination rectangles are NOT handled: the
// copy runs top-to-bottom, left-to-right with no direction selection,
// so an overlap where the destination trails the source in that order
// will read pixels this same operation has already written. Copy via a
// third rectangle if you need that.
//
// Unlike z_fb_hw_blit_mem() this has no availability check, because a
// blitter that predates the copy path doesn't fail cleanly -- the old
// stub wrote the destination back unchanged, so a caller would see a
// no-op rather than an error. Gate on z_fb_hw_blit_mem_available()
// instead: the two arrived together, so it answers for both.
// -- vertical scroll, in hardware --
//
// Moves the contents of a rectangle up or down by `dy` pixels.
// Negative dy moves content UP (scrolling forward through a document);
// positive moves it DOWN (scrolling back). The strip `dy` pixels deep
// that scrolls in is NOT touched -- the caller redraws it.
//
// Roughly 4x faster than re-rendering the text: for an 80x25 terminal,
// 1.07ms against 4.67ms. Not a throughput win -- glyph drawing costs
// about 1% of the CPU either way -- but it removes a perceptible hitch
// every time output scrolls.
//
// -- why the direction matters, and why both still work --
//
// The blitter walks rows top-down and copies through VRAM, so an
// overlapping copy can overwrite a source row before it has been read.
//
//   Content moving UP: the destination is ABOVE the source, so each
//   row is read before anything overwrites it. One blit, safe.
//
//   Content moving DOWN: the destination is BELOW the source, and a
//   single blit would destroy rows it has not read yet.
//
// The second case is NOT unfixable, which is worth stating because the
// obvious conclusion is that scrolling back cannot be accelerated. It
// is done as a series of blits `dy` deep, issued BOTTOM TO TOP. Within
// one strip source and destination cannot overlap (the strip is
// exactly the scroll distance), and going upward means each strip is
// read before the strip below it is written.
//
// The cost is one blit per strip instead of one overall -- 24 rather
// than 1 for a 25-line screen scrolling by one line. Same pixels, so
// the same blitter time; only the per-blit setup is multiplied, and it
// still comes out several times faster than re-rendering.
void z_fb_hw_scroll(int x, int y, int w, int h, int dy);

/* Debug counters for the scroll path. Set to N to have the next N
 * scroll operations print their arguments, and the next N blits print
 * theirs, then stop.
 *
 * Bounded rather than boolean on purpose: scrolls happen at key-repeat
 * rates, and an unbounded print would flood the console and change the
 * timing of the thing being investigated. */
extern int z_fb_scroll_debug;
extern int z_fb_scroll_dbg_armed;

/* Bisection tool -- see zgfx.c. Set nonzero to force the scroll region
 * onto 32-pixel boundaries, so the copy needs no bit shifting at all.
 * A few edge pixels are then not scrolled; the point is whether the
 * SIDEWAYS OFFSET disappears, which would localise the fault to the
 * unaligned source path. */
extern int z_fb_scroll_align;

/* Issue each scroll blit twice -- see zgfx.c. Distinguishes a
 * transient (bus settle, arbitration) from a systematic fault. Never a
 * fix: the second pass is pure waste when it is not needed. */
extern int z_fb_scroll_twice;

/* Read the blitter's source-read probe and compare it against what the
 * CPU sees at the same address. Prints three things: what the copy
 * asked for, what the blitter presented, and what came back -- which
 * separates "asked for the wrong word" from "asked correctly and got
 * the wrong data". See zeitlos.h's gpu_blit_dbg_* registers. */
void z_fb_hw_scroll_probe(uint32_t expect_adr);

void z_fb_hw_blit_vram(int src_x, int src_y,
	int dst_x, int dst_y, int w, int h);

// Waits until BOTH graphics engines have finished everything already
// submitted -- the line rasterizer's FIFO is empty and the blitter is
// idle.
//
// Normally nobody needs this. z_fb_hw_line() only waits for FIFO
// SPACE and returns with the line still queued, which is exactly what
// makes it fast, and the pixels landing a moment later is invisible.
//
// It matters when something is about to change what those pixels mean.
// The case this was added for: an app draws through the rasterizer,
// the app is killed, and wm repairs the screen region it occupied --
// but the dead app's queued lines are still in the FIFO and drain
// AFTERWARDS, painting over the repair. The result is a window that
// closes and leaves a scribble behind, which looks like wm failing to
// clean up rather than a queue that outlived its owner.
//
// So: call this before any repair that follows a process going away.
// It is a wait, so don't put it in a draw loop.
void z_fb_hw_sync(void);

// loads a font's glyph data into hardware glyph memory (rtl/mem/glyph.v)
// for use by the hardware-accelerated draw path. Call once, before the
// first draw using that font. Always declared/callable regardless of
// Z_GFX_HW_BLIT -- it's a no-op when built without it, so callers don't
// need their own #ifdef. See docs/window_manager.md, "hardware glyph
// blitting".
void z_gfx_hw_font_load(const z_font_t *font);

// loads one 8x8 window icon's glyph data (Z_ICON_H bytes, one row per
// byte, MSB-first -- same row-major/bit-order convention as a
// z_font_t's own glyphs, see zfont.h) into hardware glyph memory's
// reserved icon region (zicon.h's Z_ICON_MEM_OFFSET). `icon_id` is a
// zicon.h z_icon_id_t. Same single-owner discipline as
// z_gfx_hw_font_load(): call this once per icon, from wm's own
// startup, right after loading the font -- see sw/apps/wm/win_icons.h
// and wm.c's main(). Always declared/callable regardless of
// Z_GFX_HW_BLIT -- a documented no-op without it, so callers don't
// need their own #ifdef, same contract z_gfx_hw_font_load() has.
void z_gfx_hw_icon_load(int icon_id, const uint8_t *bitmap);

// hardware-blits window icon `icon_id` (zicon.h) at (x,y), 8x8,
// solid two-color-cell semantics (fg where an icon bit is set, bg
// where it isn't) -- same as z_fb_draw_char2(). Unlike font glyphs
// there's no software fallback for a partially off-screen/clipped
// icon: window icons are only ever drawn by wm itself, entirely
// within a titlebar rect it already computed and knows is on-screen
// (see sw/apps/wm/wm.c's draw_titlebar_content()), so that case is
// never actually exercised, and a call that doesn't fully fit simply
// draws nothing rather than carrying a second, untested rendering
// path. Always declared/callable regardless of Z_GFX_HW_BLIT -- a
// documented no-op without it (window icons are a hardware-only
// feature, see zicon.h's own header comment), so callers don't need
// their own #ifdef.
void z_fb_draw_icon(int x, int y, int icon_id, int fg_color, int bg_color, const z_clip_t *clip);

#endif
