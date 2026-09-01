/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Direct-framebuffer pixel/text drawing. See zgfx.h.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "zeitlos.h"
#include "zgfx.h"
#include "zfont.h"

#define VRAM ((volatile uint32_t *)0x20000000)

/*
 * -- the visible region --
 *
 * A window's CLIP RECT is where its own content lives. Its VISIBLE
 * REGION is the part of that not covered by a window in front of it,
 * and the two are different the moment anything overlaps.
 *
 * Drawing to the clip rect alone is what let an app paint over the
 * windows above it -- a clock running behind a text editor redrew its
 * hands straight through the editor's content, and every app had the
 * same exposure whenever wm asked it to repaint while occluded.
 *
 * The region is a SET of rectangles, not one: a window covered in the
 * middle is visible as a ring, and a bounding box around that would
 * be a superset -- it would permit exactly the drawing this exists to
 * prevent. So the set is stored as-is and every primitive is issued
 * once per rectangle.
 *
 * Empty (count 0) means UNRESTRICTED, not "invisible". That is the
 * state before wm has said anything, and it has to mean "draw
 * normally" or a process would be blank until its first region
 * message arrived. wm sends a genuinely empty region as a
 * fully-occluded window, which is represented as count 1 with an
 * empty rectangle.
 *
 * Process-global rather than per-window: the hardware has one
 * framebuffer and one scissor, and a process draws to one window at a
 * time. z_win_clip_begin()/end() (zwin.c) set it around a window's
 * drawing.
 */

static void z_fb_hw_line_one(int x0, int y0, int x1, int y1, int color,
	const z_clip_t *clip);
static void hw_fill_rect_one(int x, int y, int w, int h, int color,
	bool wait);

// Raster op for the next hardware fill. Z_ROP_COPY for every caller
// but z_fb_hw_fill_rect_rop(), which sets it, fills, and puts it back.
//
// A file-scope variable rather than another parameter threaded through
// hw_fill_rect_core(), hw_fill_rect_one() and the three public entry
// points: the alternative is four signatures changed for one caller.
// Safe because the whole sequence -- set, fill, restore -- happens
// with no yield in between, and the register write it feeds is inside
// gpu_blit_acquire()'s masked section.
static int fill_rop = Z_ROP_COPY;
static void hw_fill_shade_one(int x, int y, int w, int h, int level,
	bool wait);
static void hw_fill_pattern_one(int x, int y, int w, int h,
	const uint8_t *pat);
static void z_fb_hw_box_one(int x0, int y0, int x1, int y1, int color,
	const z_clip_t *clip);

#define Z_GFX_MAX_CLIP  8

static z_clip_t gfx_region[Z_GFX_MAX_CLIP];
static int gfx_region_n;	// 0 = unrestricted

void z_gfx_set_visible(const z_clip_t *rects, int n) {
	if (n > Z_GFX_MAX_CLIP) n = Z_GFX_MAX_CLIP;
	if (n < 0) n = 0;
	for (int i = 0; i < n; i++) gfx_region[i] = rects[i];
	gfx_region_n = n;
}

void z_gfx_clear_visible(void) {
	gfx_region_n = 0;
}

int z_gfx_visible_count(void) {
	return gfx_region_n;
}

// Effective clip for pass `i`: the caller's clip intersected with
// region rectangle i. Returns false if that intersection is empty, so
// the caller can skip the pass entirely.
bool z_gfx_visible_clip(int i, const z_clip_t *clip, z_clip_t *out) {

	z_clip_t r;

	if (gfx_region_n == 0) {
		r.x0 = 0; r.y0 = 0;
		r.x1 = Z_SCREEN_W - 1; r.y1 = Z_SCREEN_H - 1;
	} else {
		if (i < 0 || i >= gfx_region_n) return false;
		r = gfx_region[i];
	}

	if (clip) {
		if (clip->x0 > r.x0) r.x0 = clip->x0;
		if (clip->y0 > r.y0) r.y0 = clip->y0;
		if (clip->x1 < r.x1) r.x1 = clip->x1;
		if (clip->y1 < r.y1) r.y1 = clip->y1;
	}

	if (r.x1 < r.x0 || r.y1 < r.y0) return false;

	*out = r;
	return true;

}

// Programs the BLITTER's scissor (rtl/gpu/gpu_blit.v registers 20..23)
// from region rectangle `i`, intersected with `clip`. Returns false
// when that intersection is empty, so the caller skips the pass.
//
// The blitter's scissor is half-open on the high side -- [x0,x1) --
// while z_clip_t is inclusive, hence the +1.
//
// Every blitter primitive must call this (or clear it) before
// triggering, because the scissor is PERSISTENT hardware state: a
// rectangle left over from a previous, unrelated blit would silently
// clip the next one. Restoring the full screen is what
// z_gfx_blit_scissor_reset() is for.
bool z_gfx_blit_scissor(int i, const z_clip_t *clip) {

	z_clip_t eff;

	if (!z_gfx_visible_clip(i, clip, &eff)) return false;

	gpu_blit_clip_x0 = (uint32_t)eff.x0;
	gpu_blit_clip_y0 = (uint32_t)eff.y0;
	gpu_blit_clip_x1 = (uint32_t)(eff.x1 + 1);
	gpu_blit_clip_y1 = (uint32_t)(eff.y1 + 1);

	return true;

}

void z_gfx_blit_scissor_reset(void) {
	gpu_blit_clip_x0 = 0;
	gpu_blit_clip_y0 = 0;
	gpu_blit_clip_x1 = Z_SCREEN_W;
	gpu_blit_clip_y1 = Z_SCREEN_H;
}

// True if (x,y) is inside the visible region.
//
// This is where every SOFTWARE primitive gets clipped: set_pixel,
// fill_rect, draw_char/text/icon and their _2 variants all funnel
// through clip_allows(), so testing the region here covers all of
// them at once rather than growing a loop in each.
//
// The hardware paths cannot use it -- they hand a rectangle to the
// GPU rather than visiting pixels -- and are handled per primitive.
static inline bool region_allows(int x, int y) {

	if (gfx_region_n == 0) return true;

	for (int i = 0; i < gfx_region_n; i++) {
		const z_clip_t *r = &gfx_region[i];
		if (x >= r->x0 && x <= r->x1 && y >= r->y0 && y <= r->y1)
			return true;
	}

	return false;

}

static inline bool clip_allows(int x, int y, const z_clip_t *clip) {

	if (x < 0 || x >= Z_SCREEN_W || y < 0 || y >= Z_SCREEN_H)
		return false;

	if (clip) {
		if (x < clip->x0 || x > clip->x1 || y < clip->y0 || y > clip->y1)
			return false;
	}

	return region_allows(x, y);

}

void z_fb_set_pixel(int x, int y, int color, const z_clip_t *clip) {

	if (!clip_allows(x, y, clip)) return;

	uint32_t bit_index = (uint32_t)y * Z_SCREEN_W + (uint32_t)x;
	uint32_t word_index = bit_index / 32;
	uint32_t mask = 1U << (bit_index % 32);

	if (color)
		VRAM[word_index] |= mask;
	else
		VRAM[word_index] &= ~mask;

}

void z_fb_fill_rect(int x, int y, int w, int h, int color, const z_clip_t *clip) {

	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++)
			z_fb_set_pixel(x + i, y + j, color, clip);

}

// -- hardware line rasterizer path (rtl/gpu/gpu_raster.v) -- see
// zgfx.h's file header comment and z_fb_hw_line()'s own comment for
// why this needed IRQ masking to be safe for concurrent access from
// multiple processes.

static inline void gpu_wait_fifo(void) {
	// bounded defensively -- see z_fb_hw_line()'s own comment on why
	// the rasterizer's hardware state machine can hang forever with
	// no protection of its own if it's ever handed a bad coordinate.
	// The coordinate clamp there is the real fix; this is a backstop
	// in case some other, not-yet-understood path can still stall
	// it -- an arbitrary-but-generous bound, well past anything a
	// genuinely draining FIFO should ever take, so this essentially
	// never fires under normal operation. If it does fire, that's
	// itself important diagnostic information (the rasterizer really
	// is stuck, not just slow), so it's reported, not silently
	// swallowed -- better for the calling process to stay responsive
	// and report the problem than to freeze forever the way it did
	// before this existed.
	uint32_t waited = 0;
	while (gpu_debug_fifo_count > 15) {
		if (++waited > 10000000) {
			printf("zgfx: gpu fifo wait timed out -- rasterizer may be stuck\n");
			return;
		}
	}
}

// Issued once per visible-region rectangle.
//
// The rasterizer has a hardware scissor (gpu_clip_*), so each pass
// costs five register writes plus a draw whose out-of-clip pixels the
// hardware discards -- no software line clipping. With no region set,
// or one rectangle, this is exactly the single call it always was.
void z_fb_hw_line(int x0, int y0, int x1, int y1, int color,
	const z_clip_t *clip) {

	int n = z_gfx_visible_count();
	z_clip_t eff;

	if (n == 0) {
		z_fb_hw_line_one(x0, y0, x1, y1, color, clip);
		return;
	}

	for (int i = 0; i < n; i++)
		if (z_gfx_visible_clip(i, clip, &eff))
			z_fb_hw_line_one(x0, y0, x1, y1, color, &eff);

}

static void z_fb_hw_line_one(int x0, int y0, int x1, int y1, int color, const z_clip_t *clip) {

	// unconditional, regardless of clip -- clip only ever narrows
	// where on screen a line can land, it was never a bounds check
	// against the framebuffer's actual physical size. The hardware
	// has no protection of its own here: gpu_y0/gpu_y1 are 10-bit
	// registers (0-1023), but the framebuffer is only Z_SCREEN_H=480
	// pixels tall, so 480-1023 is representable but physically
	// invalid -- and rtl/gpu/gpu_raster.v's WAIT_READ/WAIT_WRITE
	// states wait on the framebuffer bus's ack with no timeout at
	// all (only the pixel *count* along a line is bounded, not how
	// long any single pixel's bus transaction is allowed to take).
	// A coordinate that lands somewhere the bus never acks hangs the
	// rasterizer's state machine forever: draw_busy never clears,
	// the FIFO never drains, and gpu_wait_fifo() (below, and in every
	// future call) spins forever waiting for room that will never
	// free up. Clamping here, at the one shared entry point every
	// caller goes through, closes this regardless of where a bad
	// coordinate would otherwise have come from.
	if (x0 < 0) x0 = 0;
	if (x0 >= Z_SCREEN_W) x0 = Z_SCREEN_W - 1;
	if (x1 < 0) x1 = 0;
	if (x1 >= Z_SCREEN_W) x1 = Z_SCREEN_W - 1;
	if (y0 < 0) y0 = 0;
	if (y0 >= Z_SCREEN_H) y0 = Z_SCREEN_H - 1;
	if (y1 < 0) y1 = 0;
	if (y1 >= Z_SCREEN_H) y1 = Z_SCREEN_H - 1;

	// deliberately outside the masked section below -- this can
	// legitimately take a while if the FIFO's backed up, and masking
	// through that would stall the scheduler for every other process,
	// not just this one.
	gpu_wait_fifo();

	uint32_t old_mask = maskirq(0xFFFFFFFF);

	if (clip) {
		gpu_clip_x0 = (uint32_t)clip->x0;
		gpu_clip_y0 = (uint32_t)clip->y0;
		gpu_clip_x1 = (uint32_t)clip->x1;
		gpu_clip_y1 = (uint32_t)clip->y1;
		gpu_clip_enable = 1;
	} else {
		gpu_clip_enable = 0;
	}

	gpu_x0 = (uint32_t)x0;
	gpu_y0 = (uint32_t)y0;
	gpu_x1 = (uint32_t)x1;
	gpu_y1 = (uint32_t)y1;
	// & 3, not & 1: the colour field is a two-bit RASTER OP now
	// (rtl/gpu/gpu_raster.v) -- 0 clear, 1 set, 2 XOR. Masking to one
	// bit here would silently turn every XOR into a clear, which is
	// exactly the kind of failure that looks like broken gateware.
	gpu_color = color & 3;
	gpu_start = 1;

	maskirq(old_mask);

}

// Issued once per visible-region rectangle.
//
// The rasterizer has a hardware scissor (gpu_clip_*), so each pass
// costs five register writes plus a draw whose out-of-clip pixels the
// hardware discards -- no software line clipping. With no region set,
// or one rectangle, this is exactly the single call it always was.
void z_fb_hw_box(int x0, int y0, int x1, int y1, int color,
	const z_clip_t *clip) {

	int n = z_gfx_visible_count();
	z_clip_t eff;

	if (n == 0) {
		z_fb_hw_box_one(x0, y0, x1, y1, color, clip);
		return;
	}

	for (int i = 0; i < n; i++)
		if (z_gfx_visible_clip(i, clip, &eff))
			z_fb_hw_box_one(x0, y0, x1, y1, color, &eff);

}

static void z_fb_hw_box_one(int x0, int y0, int x1, int y1, int color, const z_clip_t *clip) {

	// The verticals deliberately stop one pixel short at BOTH ends, so
	// that every pixel of the outline is drawn EXACTLY ONCE.
	//
	// This used to draw four lines that each shared an endpoint with
	// the next, so all four corners were painted twice. With only set
	// and clear available that was invisible -- both are idempotent,
	// painting a pixel twice is the same as once. XOR is not: a corner
	// drawn twice XORs back to its original value, so the box would
	// render with all four corners missing.
	//
	// Fixed here rather than only on the XOR path, because "each pixel
	// once" is correct under every op and produces an identical pixel
	// set for set and clear (verified over 1369 box sizes by
	// tools/verify_xor_geometry.py).
	z_fb_hw_line(x0, y0, x1, y0, color, clip);			// top
	z_fb_hw_line(x0, y1, x1, y1, color, clip);			// bottom

	if (y1 - y0 >= 2) {
		z_fb_hw_line(x0, y0 + 1, x0, y1 - 1, color, clip);	// left
		z_fb_hw_line(x1, y0 + 1, x1, y1 - 1, color, clip);	// right
	}

}

// -- GPU blitter fill path (rtl/gpu/gpu_blit.v) -- see zgfx.h's
// z_fb_hw_fill_rect() comment for why this needed the same
// clamp+mask treatment as z_fb_hw_line() above. Declared here,
// unconditionally (not inside the Z_GFX_HW_BLIT block below), since
// fill mode is independent of whether this build also drives the
// hardware glyph path -- the blitter itself is always present in the
// register map regardless of that build flag.

static inline void gpu_blit_wait_idle(void) {
	// bounded defensively, same reasoning and same pattern as
	// gpu_wait_fifo() above -- an out-of-range destination could hang
	// this state machine forever with no protection of its own; the
	// coordinate clamp in z_fb_hw_fill_rect() is the real fix, this
	// is a backstop in case some other path can still stall it.
	uint32_t waited = 0;
	while (gpu_blit_status & 1) {
		if (++waited > 10000000) {
			printf("zgfx: gpu blitter wait timed out -- blitter may be stuck\n");
			return;
		}
	}
}

// See zgfx.h for when this is needed and why it normally isn't.
//
// Bounded the same way gpu_wait_fifo() and gpu_blit_wait_idle() are,
// and for the same reason: neither engine has a timeout of its own, so
// a stuck one must not take the caller down with it. A timeout here
// means the screen may keep a stray mark, which is a great deal better
// than wm hanging.
void z_fb_hw_sync(void) {

	uint32_t waited = 0;

	// Rasterizer first: it is the one with a queue, so it is the one
	// that can still be drawing after its submitter is gone.
	while (gpu_debug_fifo_count) {
		if (++waited > 10000000) {
			printf("zgfx: sync timed out draining the rasterizer FIFO\n");
			return;
		}
	}

	waited = 0;

	while (gpu_blit_status & 1) {
		if (++waited > 10000000) {
			printf("zgfx: sync timed out waiting for the blitter\n");
			return;
		}
	}

}

// -- cross-process TOCTOU race on the blitter's single busy/idle gate --
//
// see docs/gpu_blitter.md, "Concurrent register access", and
// docs/window_manager.md's "Unresolved: horizontal garbage" note --
// this function is the fix for both.
//
// Every caller that's about to START a new blitter operation (fill OR
// glyph -- both modes share this one peripheral, the same single
// draw_busy bit, and no queue of their own) used to poll
// gpu_blit_status with IRQs still enabled (gpu_blit_wait_idle()/
// hw_blit_wait() below), THEN separately call maskirq() before
// writing its own registers and trigger. That gap between "observed
// idle" and "IRQs actually masked" is a real window: a timer IRQ
// landing in it can switch to a different process, which polls the
// SAME still-idle status, wins the race, and starts its own operation
// (masked, so it runs to completion uninterrupted). When the original
// process resumes and finally masks IRQs, the hardware is no longer
// idle -- but nothing re-checked that. Its own CTRL write (with START
// set) lands while draw_busy is 1 and the state machine isn't in
// ST_IDLE (rtl/gpu/gpu_blit.v) -- `start_trigger` is gated on
// `!draw_busy`, evaluated only inside the ST_IDLE case, so the write
// is accepted onto the bus (wb_ack_o still fires) but has NO EFFECT:
// no new operation starts, and nothing reports this back in software.
// The caller returns believing it just triggered a glyph blit or a
// fill; the framebuffer cells it meant to touch are left exactly as
// they were before the call.
//
// This is a strong theoretical fit for term's reported "horizontal
// garbage near freshly-typed text" (docs/window_manager.md): a
// dropped glyph trigger leaves a cell showing whatever was there
// before -- stale content, which is exactly what "duplicating
// recently-typed characters" or "solid blocks" (an old reverse-video
// cursor cell) would look like -- and it's specific to active typing
// because term's render() issues many z_fb_draw_char2() calls in a
// tight sequence while redrawing a dirty row, multiplying how often
// this narrow gap gets exercised; wm's own fill-mode use (repair_
// region()'s fill_rect(), which shares this exact peripheral/busy bit
// with glyph mode) is a second, independent process that can win that
// race against term at any moment, not just during a drag. It also
// explains why the project's own RTL testbenches (docs/gpu_blitter.md)
// never caught it: they exercise gpu_blit_wb in isolation against a
// bus model, not this software-level, cross-PROCESS scheduling race,
// which only exists once two processes are actually competing for the
// same peripheral on real hardware. Not confirmed as THE root cause
// (nothing here was reproduced on real hardware to prove it), but
// it's a genuine, previously-open race regardless of whether it's
// this one -- closing it is correct either way.
//
// Fix: fold the busy-check into the SAME masked section as the
// trigger, so nothing can get between them. The (potentially long)
// spin-wait itself stays OUTSIDE the mask, same reasoning
// gpu_wait_fifo()/gpu_blit_wait_idle()/hw_blit_wait() already
// document (masking through a genuinely long wait would stall the
// scheduler for every other process, not just this one) -- only the
// FINAL check, a single cheap register read, happens with IRQs
// already masked. If that final check finds the hardware busy after
// all (another process won the race in the gap between the spin-wait
// and the mask), this unmasks and goes around again rather than
// blocking with IRQs disabled.
//
// Returns with IRQs masked -- the caller must finish its own register
// writes and trigger, then call maskirq(old_mask) itself (same
// contract the old bare maskirq(0xFFFFFFFF) call had).
static inline uint32_t gpu_blit_acquire(void) {

	for (;;) {

		uint32_t waited = 0;
		while (gpu_blit_status & 1) {
			if (++waited > 10000000) {
				printf("zgfx: gpu blitter wait timed out -- blitter may be stuck\n");
				break;
			}
		}

		uint32_t old_mask = maskirq(0xFFFFFFFF);
		if (!(gpu_blit_status & 1)) return old_mask;

		// lost the race: something else started an operation in the
		// gap between the spin-wait above and this check. unmask and
		// try again.
		maskirq(old_mask);

	}

}

static void hw_fill_rect_core(int x, int y, int w, int h, int color,
	bool wait);
static void hw_fill_shade_core(int x, int y, int w, int h, int level,
	bool wait);

bool z_fb_hw_dither_available(void) {

	static int cached = -1;

	if (cached < 0) {
		uint32_t old_mask = gpu_blit_acquire();
		gpu_blit_ctrl = GPU_BLIT_CTRL_DITHER;
		cached = (gpu_blit_ctrl & GPU_BLIT_CTRL_DITHER) ? 1 : 0;
		gpu_blit_ctrl = 0;
		maskirq(old_mask);
	}

	return cached == 1;

}

void z_fb_hw_fill_shade(int x, int y, int w, int h, int level) {
	hw_fill_shade_core(x, y, w, h, level, true);
}

void z_fb_hw_fill_shade_async(int x, int y, int w, int h, int level) {
	hw_fill_shade_core(x, y, w, h, level, false);
}

// A grey, on a display that has none.
//
// The level goes into PATTERN, which in dither mode is not a bitmask --
// the hardware generates the 4x4 matrix from it. Falls back to a plain
// black or white fill on a bitstream without dither support, which is
// wrong but legible, rather than filling with the level as a bit
// pattern, which would be noise.
// Same shape, and the same reasoning, as hw_fill_rect_core() above.
static void hw_fill_shade_core(int x, int y, int w, int h, int level,
	bool wait) {

	int n = z_gfx_visible_count();

	if (n == 0) {
		hw_fill_shade_one(x, y, w, h, level, wait);
		return;
	}

	for (int i = 0; i < n; i++) {
		if (!z_gfx_blit_scissor(i, NULL)) continue;
		hw_fill_shade_one(x, y, w, h, level, wait);
	}

	z_gfx_blit_scissor_reset();

}

static void hw_fill_shade_one(int x, int y, int w, int h, int level,
	bool wait) {

	if (level < 0) level = 0;
	if (level > Z_SHADE_MAX) level = Z_SHADE_MAX;

	if (!z_fb_hw_dither_available()) {
		hw_fill_rect_core(x, y, w, h,
			(level > Z_SHADE_MAX / 2) ? 1 : 0, wait);
		return;
	}

	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > Z_SCREEN_W) w = Z_SCREEN_W - x;
	if (y + h > Z_SCREEN_H) h = Z_SCREEN_H - y;
	if (w <= 0 || h <= 0) return;

	{
		uint32_t old_mask = gpu_blit_acquire();

		gpu_blit_dst_x = (uint32_t)x;
		gpu_blit_dst_y = (uint32_t)y;
		gpu_blit_width = (uint32_t)w;
		gpu_blit_height = (uint32_t)h;
		gpu_blit_pattern = (uint32_t)level;

		gpu_blit_ctrl = GPU_BLIT_CTRL_START | GPU_BLIT_CTRL_FILL |
			GPU_BLIT_CTRL_CLIP | GPU_BLIT_CTRL_DITHER;

		maskirq(old_mask);
	}

	if (wait) gpu_blit_wait_idle();

}

// -- shaded SPAN: one scanline, minimum register traffic --
//
// A software triangle rasterizer emits hundreds of these per frame, and
// at that rate the wishbone register writes cost as much as the blit
// itself: six writes per span, each a stalled bus cycle from the CPU.
//
// Height and grey level do not change between the spans of one face,
// so they are hoisted into z_fb_hw_span_begin() and this writes only
// the three registers that vary. Six writes become four, which is the
// difference between the CPU keeping the blitter fed and not.
//
// NO CLIPPING and no validation, deliberately. This is the inner loop
// of a rasterizer that has already clipped; re-checking here would put
// four comparisons back into the path the hoisting just took them out
// of. Callers that have not clipped must use z_fb_hw_fill_shade().
//
// Asynchronous, like every other fill. The next span's acquire waits
// for this one, so a rasterizer that computes the next span's endpoints
// while this one runs overlaps almost completely.
static int span_level_cached = -1;

void z_fb_hw_span_begin(int level) {

	if (level < 0) level = 0;
	if (level > Z_SHADE_MAX) level = Z_SHADE_MAX;

	span_level_cached = level;

	if (!z_fb_hw_dither_available()) return;

	{
		uint32_t old_mask = gpu_blit_acquire();
		gpu_blit_height = 1;
		gpu_blit_pattern = (uint32_t)level;
		maskirq(old_mask);
	}

}

void z_fb_hw_span(int x, int y, int w) {

	if (w <= 0) return;

	// A span is a one-row shaded fill, and z_fb_hw_span_begin()
	// hoists the register setup out so this writes only what varies.
	// That lean path cannot carry a per-rectangle scissor: the
	// scissor would have to be rewritten between passes, which is
	// exactly the setup span_begin() exists to avoid.
	//
	// So while a region is set, route through the shaded fill, which
	// already loops and scissors correctly. Slower per span, and only
	// while something is actually occluding this window.
	if (z_gfx_visible_count() != 0) {
		hw_fill_shade_core(x, y, w, 1, span_level_cached, false);
		return;
	}

	// Falls back to a whole fill where the bitstream has no dither --
	// slower, but this path is only reached on old gateware and
	// correctness matters more than speed there.
	if (!z_fb_hw_dither_available()) {
		hw_fill_rect_core(x, y, w, 1,
			(span_level_cached > Z_SHADE_MAX / 2) ? 1 : 0, false);
		return;
	}

	{
		uint32_t old_mask = gpu_blit_acquire();

		gpu_blit_dst_x = (uint32_t)x;
		gpu_blit_dst_y = (uint32_t)y;
		gpu_blit_width = (uint32_t)w;

		gpu_blit_ctrl = GPU_BLIT_CTRL_START | GPU_BLIT_CTRL_FILL |
			GPU_BLIT_CTRL_CLIP | GPU_BLIT_CTRL_DITHER;

		maskirq(old_mask);
	}

}

// Clear one scanline span. Same lean register path as z_fb_hw_span(),
// but a plain black fill rather than a dither -- a rasterizer erasing
// what it no longer covers needs this as often as it needs the shaded
// version, and going through z_fb_hw_fill_rect() would put back the
// six register writes and the clipping that path exists to avoid.
void z_fb_hw_span_clear(int x, int y, int w) {

	if (w <= 0) return;

	// See z_fb_hw_span() above -- same lean-path reasoning, and this
	// one is a plain fill so it routes to the plain filler.
	if (z_gfx_visible_count() != 0) {
		hw_fill_rect_core(x, y, w, 1, 0, false);
		return;
	}

	{
		uint32_t old_mask = gpu_blit_acquire();

		gpu_blit_dst_x = (uint32_t)x;
		gpu_blit_dst_y = (uint32_t)y;
		gpu_blit_width = (uint32_t)w;
		gpu_blit_height = 1;
		gpu_blit_pattern = 0;

		gpu_blit_ctrl = GPU_BLIT_CTRL_START | GPU_BLIT_CTRL_FILL |
			GPU_BLIT_CTRL_CLIP;

		maskirq(old_mask);
	}

	/* Invalidates the hoisted state, so the next shaded span must
	 * re-establish it. shade_triangle() calls z_fb_hw_span_begin()
	 * per face, and the erase pass runs after all of them, so this
	 * never interleaves in practice -- but leaving the cache stale
	 * would make it a trap for the next caller. */
	span_level_cached = -1;

}

/* Debug counter for the scroll path -- see zgfx.h. Nonzero prints that
 * many more scroll operations and their exact blit arguments, then
 * stops. Bounded rather than a boolean because these run at key-repeat
 * rates and an unbounded print would flood the console and change the
 * timing being investigated. */
int z_fb_scroll_debug = 0;
int z_fb_scroll_dbg_armed = 0;

/* Force the scroll region onto 32-pixel boundaries.
 *
 * A BISECTION TOOL, not a fix. The coordinates handed to this function
 * are provably correct (traced on hardware: source x, destination x and
 * the content rect all agree), and the RTL copies correctly at these
 * alignments in simulation -- so the remaining suspect is the unaligned
 * source path, where blit_copy_setup() computes a sub-word shift.
 *
 * With this set, x is rounded UP and the width DOWN to word
 * boundaries, so the copy is purely word-for-word with no shifter
 * involved. A few pixels at each edge are then not scrolled and will
 * be wrong -- that is expected and is not what to look at.
 *
 * What to look at is the OFFSET. If the content stops appearing
 * shifted sideways, the fault is in the unaligned path. If it still
 * shifts, alignment is innocent and the fault is in the VRAM source
 * addressing itself. */
int z_fb_scroll_align = 0;

/* Issue every scroll blit TWICE, identically.
 *
 * A bisection tool, and the sharpest one left. Everything static has
 * been eliminated: the coordinates, the derived address, the shifter
 * (bypassed with aligned mode and the fault persisted), and the edge
 * mask on the first partial word (rtl/gpu/bench/tb_edge.v seeds source
 * all-ones against destination all-zeros, so a skipped word cannot
 * hide, and it passes).
 *
 * What has never been ruled out is the FIRST transaction of a copy
 * being different from the rest -- a bus-settle or arbitration effect
 * that only appears with another master on the VRAM port, which no
 * simulation here has. A VRAM-to-VRAM copy is the only operation that
 * puts source read, destination read and destination write on that one
 * port back to back.
 *
 * If repeating the blit makes the artifact disappear, that is the
 * answer: the second pass finds the bus already settled. If it changes
 * nothing, the fault is not transient and the copy is being told to do
 * something different from what it is being asked.
 *
 * The second pass is pure waste when it is not needed, so this is a
 * diagnostic and never a fix. */
int z_fb_scroll_twice = 0;

/* Read the source-read probe and compare it against VRAM.
 *
 * The blitter reports the address it last presented and the data it
 * received. This reads that same address with the CPU and prints both.
 * If they differ, the blitter is not seeing what the CPU sees.
 *
 * Also prints the address the copy was ASKED for, so three things can
 * be compared rather than two: what was requested, what the blitter
 * presented, and what came back.
 */
void z_fb_hw_scroll_probe(uint32_t expect_adr) {

	uint32_t adr = gpu_blit_dbg_src_adr;
	uint32_t dat = gpu_blit_dbg_src_dat;
	uint32_t cnt = gpu_blit_dbg_src_cnt;
	uint32_t cpu;

	/* The probe address is a byte address into VRAM; read the same
	 * word the CPU way. */
	cpu = *(volatile uint32_t *)(uintptr_t)adr;

	/* Is the probe even reachable?
	 *
	 * dst_x (register 2) was written by the blit that just ran, so its
	 * value is known: it must read back as the destination x. If it
	 * does, this block is decoding fine and the probe registers are
	 * genuinely absent -- which means the GATEWARE has not been
	 * reflashed, since the probe is RTL.
	 *
	 * Worth checking automatically. "All three registers read zero" is
	 * ambiguous between a missing feature, a decode that stops short
	 * of register 16, and a bus that is not answering at all -- and
	 * those need completely different responses. */
	{
		uint32_t known = gpu_blit_dst_x;
		printf("blit probe: decode check -- dst_x reads %lu\n",
			(unsigned long)known);
		if (cnt == 0 && adr == 0 && dat == 0) {
			printf("  probe registers read 0.\n");
			printf("  If dst_x above is the destination x of the last\n");
			printf("  blit, the block is reachable and the probe simply\n");
			printf("  is not in this bitstream -- it is RTL, so it needs\n");
			printf("  `make flash`, not just a rebuilt app.\n");
			return;
		}
	}

	printf("blit probe: reads=%lu\n", (unsigned long)cnt);
	printf("  asked for  : %08lx\n", (unsigned long)expect_adr);
	printf("  presented  : %08lx  (%+ld words from asked)\n",
		(unsigned long)adr, (long)(((int32_t)adr - (int32_t)expect_adr) / 4));
	printf("  blitter got: %08lx\n", (unsigned long)dat);
	printf("  cpu reads  : %08lx  %s\n", (unsigned long)cpu,
		(cpu == dat) ? "MATCH -- bus path is fine, look at the logic"
		             : "DIFFER -- the blitter is not seeing what the CPU sees");

}

// -- copies and the region --
//
// A copy CANNOT simply be issued once per region rectangle the way a
// fill can. The blitter's copy source is aligned to the destination
// WORD, not to dst_x, so a scissor that pushes the first written word
// later than the one this address was computed for feeds the engine
// the wrong source data -- see "Copy and the scissor" in
// docs/gpu_blitter.md, and rtl/gpu/bench/tb_clip.v, which asserts the
// current behaviour.
//
// Rounding each region rectangle's left edge DOWN to a 32-pixel
// boundary would keep the source aligned, but it would also let the
// copy write up to 31 pixels outside the region -- over the window in
// front. That is the exact bug this whole exercise exists to remove,
// so it is not an acceptable trade.
//
// So while a region is set, a copy is refused rather than issued
// wrong or issued over its neighbour. Callers that need to scroll
// while partially occluded must repaint instead, which is what the
// terminal already does when wm asks it to.
//
// Returns true when the caller should go ahead unclipped.
static bool copy_region_allows(void) {

	if (z_gfx_visible_count() == 0) return true;

	// A single rectangle that is word-aligned on its left edge and
	// covers the full width it needs is the one case that IS safe --
	// but proving that here needs the destination rectangle, which
	// these entry points have. Left for when a caller actually needs
	// it; today nothing scrolls while occluded except by repainting.
	return false;

}

void z_fb_hw_scroll(int x, int y, int w, int h, int dy) {

	// See copy_region_allows(): a partially occluded window must
	// repaint rather than scroll, because the blitter cannot clip a
	// copy without either corrupting it or overrunning the region.
	if (!copy_region_allows()) return;

	if (dy == 0 || w <= 0 || h <= 0) return;

	if (z_fb_scroll_align) {
		int nx = (x + 31) & ~31;
		w -= (nx - x);
		x = nx;
		w &= ~31;
		if (w <= 0) return;
	}

	if (z_fb_scroll_debug > 0) {
		z_fb_scroll_debug--;
		printf("zgfx scroll: x=%d y=%d w=%d h=%d dy=%d\n", x, y, w, h, dy);
	}

	if (dy <= -h || dy >= h) return;   /* nothing survives the scroll */

	if (dy < 0) {

		/* Content moves UP: destination above source, so the whole
		 * region is one safe blit -- each row is read before anything
		 * overwrites it. */
		int n = -dy;

		/* Captured BEFORE the decrement, so the probe after the blit
		 * runs on the same pass that printed the arguments. Gating it
		 * on the counter directly makes the two prints depend on the
		 * order they happen to be decremented in, which is how the
		 * probe silently never fired the first time. */
		int trace = z_fb_scroll_dbg_armed;

		if (z_fb_scroll_twice)
			z_fb_hw_blit_vram(x, y + n, x, y, w, h - n);

		if (trace) {
			z_fb_scroll_dbg_armed--;
			printf("  blit_vram sx=%d sy=%d dx=%d dy=%d w=%d h=%d\n",
				x, y + n, x, y, w, h - n);
		}

		z_fb_hw_blit_vram(x, y + n, x, y, w, h - n);

		if (trace) {
			/* The LAST source word the walk should have reached:
			 * final source row, last word of the span. The probe
			 * holds whatever it actually reached. */
			uint32_t last_row = (uint32_t)(y + n + (h - n) - 1);
			uint32_t last_word = (uint32_t)((x + w - 1) >> 5);
			z_fb_hw_scroll_probe(0x20000000u + last_row * 80u
				+ last_word * 4u);
		}

	} else {

		/* Content moves DOWN: destination below source. A single blit
		 * would overwrite rows it has not read yet, so this goes in
		 * strips exactly `dy` deep, BOTTOM TO TOP.
		 *
		 * Within a strip source and destination cannot overlap, and
		 * working upward means each strip is read before the strip
		 * below it is written. See zgfx.h. */
		int sy = y + h - dy;          /* top of the last destination strip */

		while (sy > y) {
			int src = sy - dy;
			int dst = sy;
			int hh = dy;

			/* The topmost strip is partly off the region: its source
			 * would start above y. Skip that part -- and advance the
			 * DESTINATION by the same amount, not just the source.
			 *
			 * Clamping only the source was wrong and lost exactly one
			 * row per scroll, at the top of the region. It looked
			 * correct for dy == 1 (where the skip is zero) and failed
			 * for every larger step, which is the kind of thing that
			 * survives a quick test of the common case. */
			if (src < y) {
				int skip = y - src;
				src += skip;
				dst += skip;
				hh -= skip;
			}

			if (z_fb_scroll_dbg_armed) {
				z_fb_scroll_dbg_armed--;
				printf("  blit_vram sx=%d sy=%d dx=%d dy=%d w=%d h=%d\n",
					x, src, x, dst, w, hh);
			}
			if (hh > 0) z_fb_hw_blit_vram(x, src, x, dst, w, hh);
			sy -= dy;
		}

	}

}

void z_fb_hw_fill_rect(int x, int y, int w, int h, int color) {
	hw_fill_rect_core(x, y, w, h, color, true);
}

void z_fb_hw_fill_rect_async(int x, int y, int w, int h, int color) {
	hw_fill_rect_core(x, y, w, h, color, false);
}

// Issued once per visible-region rectangle, via the blitter's
// hardware scissor (rtl/gpu/gpu_blit.v registers 20..23).
//
// Fills are the easy half of clipping a blit: there is no source to
// keep in step, so the scissor can cut anywhere, including through
// the middle of a word. Copies are not -- see z_fb_hw_scroll() and
// z_fb_hw_blit_vram() below, and "Copy and the scissor" in
// docs/gpu_blitter.md.
//
// The scissor is PERSISTENT hardware state, so this resets it to the
// full screen on the way out. Leaving it set would silently clip
// whatever blit came next, in this process or the next one scheduled.
static void hw_fill_rect_core(int x, int y, int w, int h, int color,
	bool wait) {

	int n = z_gfx_visible_count();

	if (n == 0) {
		hw_fill_rect_one(x, y, w, h, color, wait);
		return;
	}

	for (int i = 0; i < n; i++) {
		if (!z_gfx_blit_scissor(i, NULL)) continue;
		hw_fill_rect_one(x, y, w, h, color, wait);
	}

	z_gfx_blit_scissor_reset();

}

static void hw_fill_rect_one(int x, int y, int w, int h, int color,
	bool wait) {

	// clamp to actual screen bounds -- unconditional, regardless of
	// CTRL_CLIP, for the same reason z_fb_hw_line() clamps
	// unconditionally. See this function's own declaration in zgfx.h
	// for the full reasoning.
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > Z_SCREEN_W) w = Z_SCREEN_W - x;
	if (y + h > Z_SCREEN_H) h = Z_SCREEN_H - y;
	if (w <= 0 || h <= 0) return;	// nothing left to draw

	// waits for any prior blitter operation to finish AND masks IRQs
	// atomically with that check -- see gpu_blit_acquire()'s own
	// comment for why the old "wait, then separately mask" sequence
	// that used to be here was a genuine cross-process race.
	uint32_t old_mask = gpu_blit_acquire();

	gpu_blit_dst_x = (uint32_t)x;
	gpu_blit_dst_y = (uint32_t)y;
	gpu_blit_width = (uint32_t)w;
	gpu_blit_height = (uint32_t)h;
	gpu_blit_pattern = color ? 0xFFFFFFFFu : 0x00000000u;
	gpu_blit_ctrl = GPU_BLIT_CTRL_START | GPU_BLIT_CTRL_FILL |
		GPU_BLIT_CTRL_CLIP |
		((uint32_t)(fill_rop & 3) << GPU_BLIT_CTRL_ROP_LSB);

	maskirq(old_mask);

	// wait for this fill to actually finish before returning -- see
	// zgfx.h's comment on why this can't rely on the next operation
	// to serialize the way consecutive glyph blits do.
	if (wait) gpu_blit_wait_idle();

}

// -- patterned fill -- see zgfx.h for the format and the anchoring
// rule this relies on.

// expands one MSB-first pattern row byte into the 32-bit word the
// blitter actually wants.
//
// The bit reversal is the part worth explaining. Framebuffer pixel x
// lives at bit (x % 32) of its word -- z_fb_set_pixel()'s own
// `1U << (bit_index % 32)`, i.e. the LEFTMOST pixel of a word is its
// LEAST significant bit. Pattern rows, on the other hand, are written
// MSB-first so they read as a picture (bit 7 is the leftmost pixel),
// matching every other bitmap in this tree. Those two conventions are
// opposite, so one of them has to be reversed here rather than
// silently producing mirrored patterns -- which is a genuinely
// annoying bug to spot, since the symmetric patterns (solid, 50%
// checker, horizontal lines) all look perfectly correct and only the
// asymmetric ones (diagonals) come out wrong, and then only as a
// mirror image that still looks like a plausible diagonal.
static uint32_t pattern_row_word(uint8_t row) {

	uint32_t b = 0;
	for (int i = 0; i < 8; i++)
		if (row & (0x80u >> i)) b |= (1u << i);

	// replicate across all four bytes: 32 is a multiple of 8, so this
	// is what anchors the pattern to the screen's 8-pixel grid.
	return b | (b << 8) | (b << 16) | (b << 24);

}

// Same shape as the fills above.
void z_fb_hw_fill_pattern(int x, int y, int w, int h, const uint8_t *pat) {

	int n = z_gfx_visible_count();

	if (n == 0) {
		hw_fill_pattern_one(x, y, w, h, pat);
		return;
	}

	for (int i = 0; i < n; i++) {
		if (!z_gfx_blit_scissor(i, NULL)) continue;
		hw_fill_pattern_one(x, y, w, h, pat);
	}

	z_gfx_blit_scissor_reset();

}

static void hw_fill_pattern_one(int x, int y, int w, int h,
	const uint8_t *pat) {

	if (!pat) return;

	// same unconditional clamp z_fb_hw_fill_rect() applies, for the
	// same reason -- see its own comment. Done here, once, before the
	// row loop, so the per-run fills below are all known in-bounds.
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > Z_SCREEN_W) w = Z_SCREEN_W - x;
	if (y + h > Z_SCREEN_H) h = Z_SCREEN_H - y;
	if (w <= 0 || h <= 0) return;

	int row = 0;
	while (row < h) {

		uint8_t b = pat[(y + row) & 7];

		// merge the following rows that share this same pattern byte
		// into one blitter operation -- see zgfx.h's note on cost.
		// A solid fill collapses to a single op this way, which
		// matters because "solid black" and "solid white" are the two
		// most-used patterns by a wide margin.
		int run = 1;
		while (row + run < h && pat[(y + row + run) & 7] == b) run++;

		uint32_t old_mask = gpu_blit_acquire();

		gpu_blit_dst_x = (uint32_t)x;
		gpu_blit_dst_y = (uint32_t)(y + row);
		gpu_blit_width = (uint32_t)w;
		gpu_blit_height = (uint32_t)run;
		gpu_blit_pattern = pattern_row_word(b);
		gpu_blit_ctrl = GPU_BLIT_CTRL_START | GPU_BLIT_CTRL_FILL | GPU_BLIT_CTRL_CLIP;

		maskirq(old_mask);

		gpu_blit_wait_idle();

		row += run;

	}

}

// -- copy from main memory / from VRAM --
//
// See zgfx.h for the API contract. This function's whole job beyond the
// usual register poke is computing what the hardware actually wants,
// which is not what the caller naturally has.
//
// The hardware assembles each destination word from a sliding two-word
// window over the source row, and needs to be told two things about the
// FIRST destination word: which source word the window starts on, and
// at what bit offset within it. Both are derived from the same quantity:
//
//   sbit0 = src_x - (dst_x & 31)
//
// which is the source-row bit index that lands on bit 0 of the first
// destination word (that word starts at screen x = dst_x & ~31, i.e.
// (dst_x & 31) pixels to the left of dst_x).
//
// sbit0 can be negative, and exactly one word's worth negative at most,
// since (dst_x & 31) <= 31 and src_x >= 0. That case means the window
// would start in the word BEFORE the source buffer -- so instead of
// reading out of bounds, the prime-with-zero bit tells the hardware to
// start the window with zeros. Every pixel that would have come from
// that phantom word sits left of dst_x and is masked out of the
// destination anyway, so nothing is lost.
static void blit_copy_setup(uint32_t src_base, int src_stride,
	int src_x, int src_y, int dst_x) {

	int sbit0 = src_x - (dst_x & 31);
	int sword, sshift;
	uint32_t prime;

	if (sbit0 >= 0) {
		sword = sbit0 >> 5;
		sshift = sbit0 & 31;
		prime = 0;
	} else {
		sword = 0;
		sshift = sbit0 + 32;
		prime = GPU_BLIT_SRC_PRIME_ZERO;
	}

	gpu_blit_src_addr = src_base + (uint32_t)(src_y * src_stride) +
		(uint32_t)(sword * 4);
	gpu_blit_src_stride = (uint32_t)src_stride;
	gpu_blit_src_shift = prime | (uint32_t)sshift;

	/* The addressing this actually derives, for the scroll
	 * investigation. This is where the remaining hypothesis lives:
	 * src_x is an offset WITHIN THE SOURCE BITMAP, and for a VRAM
	 * source the bitmap is the whole framebuffer -- so src_x is an
	 * absolute screen x, and the two only coincide when the row
	 * origin is the bitmap origin.
	 *
	 * If src_addr does not land on (src_y, src_x rounded down to a
	 * word) the addressing is the fault; if it does, it is not. */
	if (z_fb_scroll_dbg_armed) {
		uint32_t off = gpu_blit_src_addr - src_base;
		printf("  setup: sx=%d dx=%d sbit0=%d sword=%d sshift=%d prime=%d\n",
			src_x, dst_x, sbit0, sword, sshift, prime ? 1 : 0);
		printf("  setup: src_addr=+%lu -> row %lu word %lu (want row %d word %d)\n",
			(unsigned long)off,
			(unsigned long)(off / (uint32_t)src_stride),
			(unsigned long)((off % (uint32_t)src_stride) / 4),
			src_y, src_x >> 5);
	}

}

// Translates an app pointer to the physical address the blitter needs.
//
// The blitter is its own bus master and does not go through the MTU
// (rtl/mtu.v), which only translates addresses the CPU issues. An app
// runs at virtual 0x8000_0000 and its buffers are there, so handing one
// of those pointers straight to the blitter would point it at whatever
// happens to live at physical 0x8000_0000 -- not this app's data, and
// quite possibly not memory at all.
//
// A translation base of 0 means no translation is active (the kernel's
// own context), in which case the address is already physical.
static uint32_t blit_phys_addr(const void *p) {

	// via uintptr_t: a direct pointer-to-uint32_t cast warns on any
	// 64-bit host, which matters because sw/common is also built by the
	// host-side tests and by sim/ (see sim/README.md).
	uint32_t v = (uint32_t)(uintptr_t)p;
	uint32_t base = reg_mtu_base;

	if (base == 0) return v;
	if ((v & 0xF0000000u) != 0x80000000u) return v;

	return base + (v & 0x0FFFFFFFu);

}

bool z_fb_hw_blit_mem_available(void) {

	// Probe rather than assume: the same app binary may be run against
	// a bitstream whose blitter predates this mode, and the failure
	// there is silent (the mode bit is simply ignored and the copy
	// writes the destination back unchanged), which is exactly the kind
	// of thing that gets misdiagnosed as an app bug.
	//
	// Safe to do at any time: CTRL_START is not set, so this configures
	// nothing and starts nothing. Cached because it cannot change while
	// this process runs.
	static int cached = -1;

	if (cached < 0) {
		uint32_t old_mask = gpu_blit_acquire();
		gpu_blit_ctrl = GPU_BLIT_CTRL_SRCMEM;
		cached = (gpu_blit_ctrl & GPU_BLIT_CTRL_SRCMEM) ? 1 : 0;
		gpu_blit_ctrl = 0;
		maskirq(old_mask);
		if (!cached)
			printf("zgfx: blitter has no memory-copy mode in this bitstream\n");
	}

	return cached == 1;

}

// Z_ROP_* are numbered to match the hardware exactly, so this is a
// shift and not a translation table -- one fewer thing to keep in step
// with rtl/gpu/gpu_blit.v. The bit positions themselves live in
// zeitlos.h alongside every other GPU_BLIT_CTRL_*.


static inline uint32_t rop_bits(int rop) {
	return ((uint32_t)(rop & 3)) << GPU_BLIT_CTRL_ROP_LSB;
}

// Probed, not assumed, exactly like z_fb_hw_blit_mem_available().
//
// The failure this guards against is nastier than that one, though: on
// a bitstream without raster ops the bits are simply ignored, so every
// op behaves as COPY. A masked sprite then draws its mask as a solid
// block and its data over the top -- an opaque box, which looks like
// bad art rather than a missing hardware feature.
//
// Detected by writing ROP_XOR without a start bit and reading it back.
// gpu_blit's CTRL register readback returns the mode bits it actually
// stored, so an older bitstream returns zero here.
bool z_fb_hw_rop_available(void) {

	static int cached = -1;

	if (cached < 0) {
		uint32_t old_mask = gpu_blit_acquire();
		gpu_blit_ctrl = rop_bits(Z_ROP_XOR);
		cached = ((gpu_blit_ctrl >> GPU_BLIT_CTRL_ROP_LSB) & 3) ==
			(uint32_t)Z_ROP_XOR ? 1 : 0;
		gpu_blit_ctrl = 0;
		maskirq(old_mask);
	}

	return cached == 1;

}

bool z_fb_hw_blit_mem(const void *src, int src_stride,
	int src_x, int src_y, int dst_x, int dst_y, int w, int h) {
	return z_fb_hw_blit_mem_rop(src, src_stride, src_x, src_y,
		dst_x, dst_y, w, h, Z_ROP_COPY);
}

static bool hw_blit_mem_core(const void *src, int src_stride,
	int src_x, int src_y, int dst_x, int dst_y, int w, int h, int rop,
	bool wait);

bool z_fb_hw_blit_mem_rop(const void *src, int src_stride,
	int src_x, int src_y, int dst_x, int dst_y, int w, int h, int rop) {
	return hw_blit_mem_core(src, src_stride, src_x, src_y,
		dst_x, dst_y, w, h, rop, true);
}

bool z_fb_hw_blit_mem_async(const void *src, int src_stride,
	int src_x, int src_y, int dst_x, int dst_y, int w, int h, int rop) {
	return hw_blit_mem_core(src, src_stride, src_x, src_y,
		dst_x, dst_y, w, h, rop, false);
}

static bool hw_blit_mem_core(const void *src, int src_stride,
	int src_x, int src_y, int dst_x, int dst_y, int w, int h, int rop,
	bool wait) {

	if (!z_fb_hw_blit_mem_available()) return false;

	// Clamp to the screen, moving the SOURCE origin in step -- clamping
	// only the destination would slide the wrong part of the bitmap into
	// view. Same unconditional clamp z_fb_hw_fill_rect() applies, and
	// for the same reason: the coordinate registers are unsigned, so a
	// negative value wraps to something enormous rather than clipping.
	if (dst_x < 0) { w += dst_x; src_x -= dst_x; dst_x = 0; }
	if (dst_y < 0) { h += dst_y; src_y -= dst_y; dst_y = 0; }
	if (dst_x + w > Z_SCREEN_W) w = Z_SCREEN_W - dst_x;
	if (dst_y + h > Z_SCREEN_H) h = Z_SCREEN_H - dst_y;
	if (w <= 0 || h <= 0) return true;	// nothing to draw is not a failure

	uint32_t old_mask = gpu_blit_acquire();

	gpu_blit_dst_x = (uint32_t)dst_x;
	gpu_blit_dst_y = (uint32_t)dst_y;
	gpu_blit_width = (uint32_t)w;
	gpu_blit_height = (uint32_t)h;

	blit_copy_setup(blit_phys_addr(src), src_stride, src_x, src_y, dst_x);

	gpu_blit_ctrl = GPU_BLIT_CTRL_START | GPU_BLIT_CTRL_CLIP |
		GPU_BLIT_CTRL_SRCMEM | rop_bits(rop);

	maskirq(old_mask);

	if (wait) gpu_blit_wait_idle();

	return true;

}

// True if the blitter can do a masked sprite in ONE pass.
//
// Probed like every other capability here, and it matters more than
// the others: on a bitstream without it the two-pass fallback is
// correct but neither atomic nor fully async, so a caller drawing onto
// the visible page needs to know which it is getting.
bool z_fb_hw_cookie_available(void) {

	static int cached = -1;

	if (cached < 0) {
		uint32_t old_mask = gpu_blit_acquire();
		gpu_blit_ctrl = GPU_BLIT_CTRL_COOKIE;
		cached = (gpu_blit_ctrl & GPU_BLIT_CTRL_COOKIE) ? 1 : 0;
		gpu_blit_ctrl = 0;
		maskirq(old_mask);
	}

	return cached == 1;

}

static bool hw_blit_sprite_core(const void *data, const void *mask,
	int src_stride, int src_x, int src_y, int dst_x, int dst_y,
	int w, int h, bool wait) {

	if (!z_fb_hw_rop_available()) return false;

	// -- one pass, where the hardware has it --
	//
	// Identical result to the ANDN-then-OR pair below, but atomic (the
	// sprite's footprint is never momentarily blank) and genuinely
	// asynchronous (there is no first pass to wait for). Both matter
	// more than the cycle count: two passes can only safely draw into
	// a back buffer.
	if (z_fb_hw_cookie_available()) {

		if (dst_x < 0) { w += dst_x; src_x -= dst_x; dst_x = 0; }
		if (dst_y < 0) { h += dst_y; src_y -= dst_y; dst_y = 0; }
		if (dst_x + w > Z_SCREEN_W) w = Z_SCREEN_W - dst_x;
		if (dst_y + h > Z_SCREEN_H) h = Z_SCREEN_H - dst_y;
		if (w <= 0 || h <= 0) return true;

		{
			uint32_t old_mask = gpu_blit_acquire();

			gpu_blit_dst_x = (uint32_t)dst_x;
			gpu_blit_dst_y = (uint32_t)dst_y;
			gpu_blit_width = (uint32_t)w;
			gpu_blit_height = (uint32_t)h;

			// A is the mask, B is the data. blit_copy_setup() handles
			// A; B needs only its base, because the two share stride,
			// shift and geometry -- which is the same fact that lets
			// them share the shifter in hardware.
			blit_copy_setup(blit_phys_addr(mask), src_stride,
				src_x, src_y, dst_x);

			// B's base is A's base with the mask pointer swapped for
			// the data pointer. blit_copy_setup() has already folded
			// the row offset and the source-word offset into
			// gpu_blit_src_addr, and both are identical for the two
			// planes, so the difference between the bases is the only
			// thing that differs -- recomputing it would just be the
			// same arithmetic with more chances to disagree.
			gpu_blit_src_b_addr = gpu_blit_src_addr
				- blit_phys_addr(mask) + blit_phys_addr(data);

			gpu_blit_ctrl = GPU_BLIT_CTRL_START | GPU_BLIT_CTRL_CLIP |
				GPU_BLIT_CTRL_SRCMEM | GPU_BLIT_CTRL_COOKIE;

			maskirq(old_mask);
		}

		if (wait) gpu_blit_wait_idle();
		return true;

	}

	// ANDN the mask first, then OR the data. The order matters: OR
	// first would light pixels the mask pass then clears again,
	// leaving a sprite-shaped hole instead of a sprite.
	//
	// The first pass is ALWAYS started async: waiting for it here
	// would be pointless, because starting the second pass calls
	// gpu_blit_acquire(), which waits for exactly the same thing. The
	// `wait` flag therefore only governs the second pass.
	if (!hw_blit_mem_core(mask, src_stride, src_x, src_y,
		dst_x, dst_y, w, h, Z_ROP_ANDN, false)) return false;

	return hw_blit_mem_core(data, src_stride, src_x, src_y,
		dst_x, dst_y, w, h, Z_ROP_OR, wait);

}

bool z_fb_hw_blit_sprite(const void *data, const void *mask, int src_stride,
	int src_x, int src_y, int dst_x, int dst_y, int w, int h) {
	return hw_blit_sprite_core(data, mask, src_stride, src_x, src_y,
		dst_x, dst_y, w, h, true);
}

bool z_fb_hw_blit_sprite_async(const void *data, const void *mask,
	int src_stride, int src_x, int src_y, int dst_x, int dst_y,
	int w, int h) {
	return hw_blit_sprite_core(data, mask, src_stride, src_x, src_y,
		dst_x, dst_y, w, h, false);
}

void z_fb_hw_blit_vram(int src_x, int src_y,
	int dst_x, int dst_y, int w, int h) {

	// See copy_region_allows() above.
	if (!copy_region_allows()) return;

	// Only the destination is clamped here, and only against the visible
	// screen. The source is deliberately NOT clamped to Z_SCREEN_H: on a
	// bitstream with more VRAM than the visible framebuffer, a legitimate
	// source row lives past 479. Clamping it would make offscreen sprite
	// storage unreachable, which is most of the point of this call.
	if (dst_x < 0) { w += dst_x; src_x -= dst_x; dst_x = 0; }
	if (dst_y < 0) { h += dst_y; src_y -= dst_y; dst_y = 0; }
	if (dst_x + w > Z_SCREEN_W) w = Z_SCREEN_W - dst_x;
	if (dst_y + h > Z_SCREEN_H) h = Z_SCREEN_H - dst_y;
	if (w <= 0 || h <= 0) return;

	uint32_t old_mask = gpu_blit_acquire();

	gpu_blit_dst_x = (uint32_t)dst_x;
	gpu_blit_dst_y = (uint32_t)dst_y;
	gpu_blit_width = (uint32_t)w;
	gpu_blit_height = (uint32_t)h;

	// VRAM is addressed by the blitter directly, with the framebuffer's
	// own stride -- no MTU translation, because this address never
	// leaves the VRAM bus.
	blit_copy_setup(0x20000000u, Z_SCREEN_W / 8, src_x, src_y, dst_x);

	// CTRL_FILL and CTRL_SRCMEM both clear: copy, source in VRAM.
	gpu_blit_ctrl = GPU_BLIT_CTRL_START | GPU_BLIT_CTRL_CLIP;

	maskirq(old_mask);

	gpu_blit_wait_idle();

}

#ifdef Z_GFX_HW_BLIT

// -- hardware glyph blit path -- see zgfx.h and
// docs/window_manager.md, "hardware glyph blitting" --

// -- glyph memory layout --
//
// More than one font can be resident at once, at FIXED offsets
// declared here rather than assigned as fonts are loaded.
//
// That distinction is the whole design. Glyph memory is a single piece
// of global hardware, but zgfx.c is linked into every process
// separately, so any allocation state kept here would be per-process:
// wm would load a font, record where it put it, and no other process
// would have any idea. Every app would then fall back to software
// rendering for text, which is exactly what the hardware path exists
// to avoid.
//
// A fixed table compiled from one source file gives every process the
// same answer without anyone having to communicate it. wm writes the
// glyphs (it remains the only process that ever does -- see zicon.h);
// everyone else just reads the offset out of this table and blits.
//
// Fonts are identified by ADDRESS, which is safe within a process --
// &z_font_5x8 differs between processes, but each process compares
// against its own copy and every process agrees on the OFFSET, which
// is all the hardware cares about.
//
// Current occupancy, against Z_ICON_MEM_OFFSET (3840):
//
//   z_font_5x8    96 glyphs x  8 rows =  768 bytes at    0
//   z_font_6x12   96 glyphs x 12 rows = 1152 bytes at  768
//                                              free from 1920
//
// To add a font: append an entry with the next free offset, check the
// total still clears Z_ICON_MEM_OFFSET, and add a
// z_gfx_hw_font_load() call for it in wm's main(). A font that isn't
// in this table still WORKS -- it just renders in software, since
// glyph_offset_of() reports it as absent.
typedef struct {
	const z_font_t	*font;
	uint32_t	offset;
} z_glyph_slot_t;

static const z_glyph_slot_t glyph_layout[] = {
	{ &z_font_5x8,  0   },
	{ &z_font_6x12, 768 },
};

#define GLYPH_LAYOUT_COUNT \
	(int)(sizeof(glyph_layout) / sizeof(glyph_layout[0]))

// Where `font` lives in glyph memory. Returns false if it isn't one
// of the resident fonts, in which case the caller must render in
// software -- blitting from an offset that holds a different font's
// data would draw confident nonsense, which is worse than being slow.
static bool glyph_offset_of(const z_font_t *font, uint32_t *out) {

	for (int i = 0; i < GLYPH_LAYOUT_COUNT; i++) {
		if (glyph_layout[i].font == font) {
			*out = glyph_layout[i].offset;
			return true;
		}
	}

	return false;

}

void z_gfx_hw_font_load(const z_font_t *font) {

	uint32_t base;
	if (!glyph_offset_of(font, &base)) {
		// Not a resident font. Loading it anywhere would either
		// overwrite one that IS resident or land somewhere nothing
		// will ever look, so do neither -- text in this font simply
		// renders in software.
		printf("zgfx: font not in glyph_layout[], not loaded\n");
		return;
	}

	volatile uint8_t *glyph_mem = (volatile uint8_t *)GLYPH_MEM_BASE;
	uint32_t n = (uint32_t)(font->last - font->first + 1) * font->h;

	// Never past the icon region at the top of glyph memory
	// (zicon.h). Truncating produces a font with missing glyphs at
	// the end, which is ugly; overrunning corrupts the icons, which
	// looks like an unrelated bug in wm's titlebar.
	if (base + n > Z_ICON_MEM_OFFSET) {
		printf("zgfx: font at offset %u would overrun the icon region\n",
			(unsigned)base);
		n = (base < Z_ICON_MEM_OFFSET) ? (Z_ICON_MEM_OFFSET - base) : 0;
	}

	for (uint32_t i = 0; i < n; i++)
		glyph_mem[base + i] = font->glyphs[i];

}

static inline void hw_blit_wait(void) {
	while (gpu_blit_status & 1) /* wait for busy to clear */;
}

// software fallback for a glyph that isn't fully on-screen -- the
// hardware blit is unclipped by design, see zgfx.h. identical to the
// software-only implementation below, just not the default in this
// build.
static void draw_char_sw(int x, int y, char c, int color,
	const z_font_t *font, const z_clip_t *clip) {

	unsigned char uc = (unsigned char)c;
	const uint8_t *glyph = font->glyphs + (uc - font->first) * font->h;

	for (int row = 0; row < font->h; row++) {
		uint8_t bits = glyph[row];
		for (int col = 0; col < font->w; col++) {
			if (bits & (0x80 >> col))
				z_fb_set_pixel(x + col, y + row, color, clip);
		}
	}

}

void z_fb_draw_char(int x, int y, char c, int color, const z_font_t *font, const z_clip_t *clip) {

	unsigned char uc = (unsigned char)c;
	if (uc < font->first || uc > font->last) return;

	bool fits =
		x >= 0 && y >= 0 &&
		x + font->w <= Z_SCREEN_W && y + font->h <= Z_SCREEN_H &&
		(!clip ||
			(x >= clip->x0 && y >= clip->y0 &&
			 x + font->w - 1 <= clip->x1 && y + font->h - 1 <= clip->y1));

	uint32_t glyph_base;
	bool resident = glyph_offset_of(font, &glyph_base);

	// A font that isn't resident in glyph memory renders in software,
	// exactly like a clipped glyph does -- see glyph_layout[].
	if (!fits || !resident) {
		hw_blit_wait();	// a prior hardware blit could still be in
						// flight; wait for it before writing directly
						// to VRAM here, or the two could race
		draw_char_sw(x, y, c, color, font, clip);
		return;
	}

	// z_fb_hw_line()/z_fb_hw_fill_rect() both mask IRQs around their
	// own multi-register hardware setup, specifically because the
	// blitter/rasterizer registers are SHARED, board-wide hardware --
	// term/hello_win/wm (and multiple `term` instances at once) can
	// all reach this same register set, preemptively scheduled. This
	// function used to be the one exception: 7 separate MMIO writes
	// (dst_x, dst_y, glyph_addr, glyph_w, glyph_h, fg_color, bg_color)
	// before the ctrl=START trigger, all unprotected -- a timer IRQ
	// landing between any two of them could let a DIFFERENT process's
	// own glyph-blit setup interleave before ctrl=START ever fires,
	// mixing one process's coordinates with another's glyph/color,
	// which is exactly the kind of thing that shows up as garbled
	// pixels near text on real hardware, timing-dependent (worse with
	// more than one process actually drawing glyphs around the same
	// time, e.g. two `term` windows, or `term` alongside `hello_win`).
	//
	// gpu_blit_acquire() (see its own comment, above z_fb_hw_fill_
	// rect()) folds the "wait for idle" step into the SAME masked
	// section as the trigger below -- this used to be a separate
	// hw_blit_wait() (unmasked) followed by a separate maskirq() call,
	// which was itself a cross-process race: see that function's
	// comment for the full writeup, including why this is suspected
	// to be the actual cause of the "horizontal garbage near
	// freshly-typed text" report (docs/window_manager.md).
	// Once per visible-region rectangle.
	//
	// The blitter's glyph mode honours the scissor now
	// (rtl/gpu/gpu_blit.v, phase 2), including partial cells -- so a
	// character the region cuts through renders in hardware rather
	// than falling back to the software renderer. Both the glyph bits
	// AND the cell are clipped, which matters: glyph mode paints a
	// solid cell, so clipping only the bits would let the background
	// erase pixels outside the region.
	//
	// The scissor is written INSIDE the masked section, alongside the
	// rest of the setup, for the reason the comment above gives: every
	// register in this sequence is unprotected, and a scissor written
	// outside it could be overwritten by another process's glyph blit
	// before this one's trigger fires.
	int rn = z_gfx_visible_count();

	for (int ri = 0; ri < (rn ? rn : 1); ri++) {

		uint32_t old_mask = gpu_blit_acquire();

		if (rn) {
			if (!z_gfx_blit_scissor(ri, clip)) {
				maskirq(old_mask);
				continue;
			}
		}

		gpu_blit_dst_x = x;
		gpu_blit_dst_y = y;
		gpu_blit_glyph_addr = glyph_base + (uint32_t)(uc - font->first) * font->h;
		gpu_blit_glyph_w = font->w;
		gpu_blit_glyph_h = font->h;
		gpu_blit_fg_color = color ? 1 : 0;
		gpu_blit_bg_color = 0;	// solid cell fill -- see zgfx.h/docs for
								// how this differs from the software
								// renderer's transparent-overlay behavior
		gpu_blit_ctrl = GPU_BLIT_CTRL_START | GPU_BLIT_CTRL_GLYPH |
			(rn ? GPU_BLIT_CTRL_CLIP : 0);

		maskirq(old_mask);

	}

	if (rn) z_gfx_blit_scissor_reset();

}

void z_fb_draw_text(int x, int y, const char *s, int color, const z_font_t *font, const z_clip_t *clip) {

	int cx = x, cy = y;

	for (; *s; s++) {
		if (*s == '\n') {
			cx = x;
			cy += font->h;
			continue;
		}
		// Skip a glyph that starts entirely past the clip's right
		// edge instead of handing it to z_fb_draw_char(), which
		// would route it to the software renderer and then reject
		// all font->w * font->h of its pixel writes one at a time.
		// Truncated text is common -- a long window title in a
		// narrow titlebar, a filename wider than the list -- and
		// this is the difference between a handful of wasted pixel
		// operations and several hundred.
		//
		// `continue`, not `break`: the string may contain a newline
		// that resets cx and puts later glyphs back inside the clip.
		if (clip && cx > clip->x1) { cx += font->w; continue; }

		z_fb_draw_char(cx, cy, *s, color, font, clip);
		cx += font->w;
	}

	// make sure the last glyph has actually finished before returning --
	// callers (e.g. z_win_redraw_done()) rely on the draw being complete
	// once this returns, and the wm's content z-order protocol depends
	// on that being true (see docs/window_manager.md, "content z-order")
	hw_blit_wait();

}

// software fallback for z_fb_draw_char2() below, used the same way
// draw_char_sw() is used by z_fb_draw_char() -- a glyph that needs
// clipping, since the hardware blit is unclipped by design. Unlike
// draw_char_sw() (transparent overlay, only touches ink pixels), this
// draws a SOLID cell -- fills the whole glyph-sized rect with bg_color
// first, then ink pixels with fg_color on top -- matching what the
// hardware path below actually does, so clipped and unclipped glyphs
// look identical.
static void draw_char_sw2(int x, int y, char c, int fg_color, int bg_color,
	const z_font_t *font, const z_clip_t *clip) {

	unsigned char uc = (unsigned char)c;
	const uint8_t *glyph = font->glyphs + (uc - font->first) * font->h;

	z_fb_fill_rect(x, y, font->w, font->h, bg_color, clip);

	for (int row = 0; row < font->h; row++) {
		uint8_t bits = glyph[row];
		for (int col = 0; col < font->w; col++) {
			if (bits & (0x80 >> col))
				z_fb_set_pixel(x + col, y + row, fg_color, clip);
		}
	}

}

void z_fb_draw_char2(int x, int y, char c, int fg_color, int bg_color,
	const z_font_t *font, const z_clip_t *clip) {

	unsigned char uc = (unsigned char)c;
	if (uc < font->first || uc > font->last) return;

	bool fits =
		x >= 0 && y >= 0 &&
		x + font->w <= Z_SCREEN_W && y + font->h <= Z_SCREEN_H &&
		(!clip ||
			(x >= clip->x0 && y >= clip->y0 &&
			 x + font->w - 1 <= clip->x1 && y + font->h - 1 <= clip->y1));

	uint32_t glyph_base;
	bool resident = glyph_offset_of(font, &glyph_base);

	// Same two fallback conditions as z_fb_draw_char(): a glyph that
	// needs clipping, or a font that isn't resident in glyph memory
	// (see glyph_layout[]).
	//
	// The software call here was COMMENTED OUT, so a clipped glyph
	// drew nothing at all -- a partially-visible character at the
	// edge of a clip region simply vanished. That was survivable
	// while the only caller was term (whose cells are always fully
	// inside its grid), and stopped being so as soon as the file-list
	// widget started drawing clipped rows through it. It is also
	// load-bearing now for a different reason: without it, a
	// non-resident font would draw nothing rather than falling back,
	// which is a far more confusing failure than being slow.
	if (!fits || !resident) {
		hw_blit_wait();	// see z_fb_draw_char()'s own comment on why
		draw_char_sw2(x, y, c, fg_color, bg_color, font, clip);
		return;
	}

	// see z_fb_draw_char()'s own comment above (and gpu_blit_
	// acquire()'s own, longer comment above z_fb_hw_fill_rect()) for
	// why this needs the SAME atomic wait+mask gpu_blit_acquire()
	// provides, not a separate wait-then-mask -- this function has
	// the identical 7-write-then-trigger shape, and term.c's own
	// per-cell rendering (draw_cell(), via this exact function,
	// called many times per keystroke while typing) is precisely the
	// kind of frequent, multi-process-concurrent caller that gap
	// mattered for, and the leading suspect for the "horizontal
	// garbage near freshly-typed text" report.
	uint32_t old_mask = gpu_blit_acquire();

	gpu_blit_dst_x = x;
	gpu_blit_dst_y = y;
	gpu_blit_glyph_addr = glyph_base + (uint32_t)(uc - font->first) * font->h;
	gpu_blit_glyph_w = font->w;
	gpu_blit_glyph_h = font->h;
	gpu_blit_fg_color = fg_color ? 1 : 0;
	gpu_blit_bg_color = bg_color ? 1 : 0;
	gpu_blit_ctrl = GPU_BLIT_CTRL_START | GPU_BLIT_CTRL_GLYPH;

	maskirq(old_mask);

}

void z_fb_draw_text2(int x, int y, const char *s, int fg_color, int bg_color,
	const z_font_t *font, const z_clip_t *clip) {

	int cx = x, cy = y;

	for (; *s; s++) {
		if (*s == '\n') {
			cx = x;
			cy += font->h;
			continue;
		}
		// Skip a glyph that starts entirely past the clip's right
		// edge instead of handing it to z_fb_draw_char(), which
		// would route it to the software renderer and then reject
		// all font->w * font->h of its pixel writes one at a time.
		// Truncated text is common -- a long window title in a
		// narrow titlebar, a filename wider than the list -- and
		// this is the difference between a handful of wasted pixel
		// operations and several hundred.
		//
		// `continue`, not `break`: the string may contain a newline
		// that resets cx and puts later glyphs back inside the clip.
		if (clip && cx > clip->x1) { cx += font->w; continue; }

		z_fb_draw_char2(cx, cy, *s, fg_color, bg_color, font, clip);
		cx += font->w;
	}

	hw_blit_wait();	// see z_fb_draw_text()'s own comment on why

}

// -- window icons -- see zicon.h and zgfx.h's own comments on
// z_gfx_hw_icon_load()/z_fb_draw_icon(). Same glyph-blit hardware
// path as font text above (rtl/gpu/gpu_blit.v's CTRL_GLYPH mode),
// just addressed into the reserved icon region at the end of glyph
// memory (Z_ICON_MEM_OFFSET) instead of the font region at the start.

void z_gfx_hw_icon_load(int icon_id, const uint8_t *bitmap) {

	if (icon_id < 0 || icon_id >= Z_ICON_SLOTS) return;

	volatile uint8_t *glyph_mem = (volatile uint8_t *)GLYPH_MEM_BASE;
	uint32_t base = (uint32_t)Z_ICON_MEM_OFFSET + (uint32_t)icon_id * Z_ICON_H;

	for (uint32_t i = 0; i < Z_ICON_H; i++)
		glyph_mem[base + i] = bitmap[i];

}

void z_fb_draw_icon(int x, int y, int icon_id, int fg_color, int bg_color, const z_clip_t *clip) {

	if (icon_id < 0 || icon_id >= Z_ICON_SLOTS) return;

	bool fits =
		x >= 0 && y >= 0 &&
		x + Z_ICON_W <= Z_SCREEN_W && y + Z_ICON_H <= Z_SCREEN_H &&
		(!clip ||
			(x >= clip->x0 && y >= clip->y0 &&
			 x + Z_ICON_W - 1 <= clip->x1 && y + Z_ICON_H - 1 <= clip->y1));

	// unlike z_fb_draw_char()/z_fb_draw_char2(), there's no software
	// fallback for a partially off-screen/clipped icon -- see this
	// function's own declaration in zgfx.h for why that's fine here
	// (window icons are only ever drawn by wm itself, entirely inside
	// a titlebar rect it already knows is on-screen).
	if (!fits) return;

	uint32_t old_mask = gpu_blit_acquire();	// see its own comment,
												// above z_fb_hw_fill_rect()

	gpu_blit_dst_x = x;
	gpu_blit_dst_y = y;
	gpu_blit_glyph_addr = (uint32_t)Z_ICON_MEM_OFFSET + (uint32_t)icon_id * Z_ICON_H;
	gpu_blit_glyph_w = Z_ICON_W;
	gpu_blit_glyph_h = Z_ICON_H;
	gpu_blit_fg_color = fg_color ? 1 : 0;
	gpu_blit_bg_color = bg_color ? 1 : 0;
	gpu_blit_ctrl = GPU_BLIT_CTRL_START | GPU_BLIT_CTRL_GLYPH;

	maskirq(old_mask);

}

#else

// -- software renderer (default) --

void z_gfx_hw_font_load(const z_font_t *font) {
	// no-op: nothing to load when there's no hardware glyph blitter in
	// this build. always callable regardless of Z_GFX_HW_BLIT so
	// callers don't need their own #ifdef -- see zgfx.h.
	(void)font;
}

void z_fb_draw_char(int x, int y, char c, int color, const z_font_t *font, const z_clip_t *clip) {

	unsigned char uc = (unsigned char)c;
	if (uc < font->first || uc > font->last) return;

	const uint8_t *glyph = font->glyphs + (uc - font->first) * font->h;

	for (int row = 0; row < font->h; row++) {
		uint8_t bits = glyph[row];
		for (int col = 0; col < font->w; col++) {
			if (bits & (0x80 >> col))
				z_fb_set_pixel(x + col, y + row, color, clip);
		}
	}

}

void z_fb_draw_text(int x, int y, const char *s, int color, const z_font_t *font, const z_clip_t *clip) {

	int cx = x, cy = y;

	for (; *s; s++) {
		if (*s == '\n') {
			cx = x;
			cy += font->h;
			continue;
		}
		// Skip a glyph that starts entirely past the clip's right
		// edge instead of handing it to z_fb_draw_char(), which
		// would route it to the software renderer and then reject
		// all font->w * font->h of its pixel writes one at a time.
		// Truncated text is common -- a long window title in a
		// narrow titlebar, a filename wider than the list -- and
		// this is the difference between a handful of wasted pixel
		// operations and several hundred.
		//
		// `continue`, not `break`: the string may contain a newline
		// that resets cx and puts later glyphs back inside the clip.
		if (clip && cx > clip->x1) { cx += font->w; continue; }

		z_fb_draw_char(cx, cy, *s, color, font, clip);
		cx += font->w;
	}

}

void z_fb_draw_char2(int x, int y, char c, int fg_color, int bg_color,
	const z_font_t *font, const z_clip_t *clip) {

	unsigned char uc = (unsigned char)c;
	if (uc < font->first || uc > font->last) return;

	const uint8_t *glyph = font->glyphs + (uc - font->first) * font->h;

	z_fb_fill_rect(x, y, font->w, font->h, bg_color, clip);

	for (int row = 0; row < font->h; row++) {
		uint8_t bits = glyph[row];
		for (int col = 0; col < font->w; col++) {
			if (bits & (0x80 >> col))
				z_fb_set_pixel(x + col, y + row, fg_color, clip);
		}
	}

}

void z_fb_draw_text2(int x, int y, const char *s, int fg_color, int bg_color,
	const z_font_t *font, const z_clip_t *clip) {

	int cx = x, cy = y;

	for (; *s; s++) {
		if (*s == '\n') {
			cx = x;
			cy += font->h;
			continue;
		}
		// Skip a glyph that starts entirely past the clip's right
		// edge instead of handing it to z_fb_draw_char(), which
		// would route it to the software renderer and then reject
		// all font->w * font->h of its pixel writes one at a time.
		// Truncated text is common -- a long window title in a
		// narrow titlebar, a filename wider than the list -- and
		// this is the difference between a handful of wasted pixel
		// operations and several hundred.
		//
		// `continue`, not `break`: the string may contain a newline
		// that resets cx and puts later glyphs back inside the clip.
		if (clip && cx > clip->x1) { cx += font->w; continue; }

		z_fb_draw_char2(cx, cy, *s, fg_color, bg_color, font, clip);
		cx += font->w;
	}

}

// window icons are a hardware-glyph-blit-only feature (see zicon.h's
// own header comment and zgfx.h's declarations) -- documented no-ops
// without Z_GFX_HW_BLIT, same contract z_gfx_hw_font_load() above
// already has, so callers don't need their own #ifdef.
void z_gfx_hw_icon_load(int icon_id, const uint8_t *bitmap) {
	(void)icon_id;
	(void)bitmap;
}

void z_fb_draw_icon(int x, int y, int icon_id, int fg_color, int bg_color, const z_clip_t *clip) {
	(void)x;
	(void)y;
	(void)icon_id;
	(void)fg_color;
	(void)bg_color;
	(void)clip;
}

#endif


// Declared in zgfx.h since the raster ops were added, and never
// defined -- nothing had needed a non-COPY fill until wm started
// inverting the focused window's titlebar. The link error it produced
// was the first thing to notice.
//
// XOR over a strip is what makes the focus indicator work at all:
// the title is hardware-blitted and the icons are bitmaps, so
// inverting the finished pixels is the only way to reverse-video both
// without a second drawing path for each.
void z_fb_hw_fill_rect_rop(int x, int y, int w, int h, int color, int rop) {

	int prev = fill_rop;
	fill_rop = rop;
	hw_fill_rect_core(x, y, w, h, color, true);
	fill_rop = prev;

}
