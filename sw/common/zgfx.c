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

static inline bool clip_allows(int x, int y, const z_clip_t *clip) {

	if (x < 0 || x >= Z_SCREEN_W || y < 0 || y >= Z_SCREEN_H)
		return false;

	if (clip) {
		if (x < clip->x0 || x > clip->x1 || y < clip->y0 || y > clip->y1)
			return false;
	}

	return true;

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

void z_fb_hw_line(int x0, int y0, int x1, int y1, int color, const z_clip_t *clip) {

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

void z_fb_hw_box(int x0, int y0, int x1, int y1, int color, const z_clip_t *clip) {

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

void z_fb_hw_fill_rect(int x, int y, int w, int h, int color) {

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
	gpu_blit_ctrl = GPU_BLIT_CTRL_START | GPU_BLIT_CTRL_FILL | GPU_BLIT_CTRL_CLIP;

	maskirq(old_mask);

	// wait for this fill to actually finish before returning -- see
	// zgfx.h's comment on why this can't rely on the next operation
	// to serialize the way consecutive glyph blits do.
	gpu_blit_wait_idle();

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

void z_fb_hw_fill_pattern(int x, int y, int w, int h, const uint8_t *pat) {

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

bool z_fb_hw_blit_mem(const void *src, int src_stride,
	int src_x, int src_y, int dst_x, int dst_y, int w, int h) {

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
		GPU_BLIT_CTRL_SRCMEM;

	maskirq(old_mask);

	gpu_blit_wait_idle();

	return true;

}

void z_fb_hw_blit_vram(int src_x, int src_y,
	int dst_x, int dst_y, int w, int h) {

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
	uint32_t old_mask = gpu_blit_acquire();

	gpu_blit_dst_x = x;
	gpu_blit_dst_y = y;
	gpu_blit_glyph_addr = glyph_base + (uint32_t)(uc - font->first) * font->h;
	gpu_blit_glyph_w = font->w;
	gpu_blit_glyph_h = font->h;
	gpu_blit_fg_color = color ? 1 : 0;
	gpu_blit_bg_color = 0;	// solid cell fill -- see zgfx.h/docs for how
							// this differs from the software renderer's
							// transparent-overlay behavior
	gpu_blit_ctrl = GPU_BLIT_CTRL_START | GPU_BLIT_CTRL_GLYPH;

	maskirq(old_mask);

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
