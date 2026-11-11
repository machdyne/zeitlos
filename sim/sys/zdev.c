/*
 * zdev -- the board's devices beyond the CPU, memory and UART0
 * (zsys.h, docs/zsys.md "The board").
 *
 *   0x7000_02xx  socctl (rtl/socctl.v): cursor, phosphor, frame counter, RECONFIG
 *   0x7000_03xx  the RTC (rtl/rtc.v)
 *   0xa000_0000  the line rasterizer (rtl/gpu/gpu_raster.v)
 *   0xb000_0000  the SD card's SPI controller (rtl/spim.v) and a card behind it
 *   0xd000_0000  the blitter (rtl/gpu/gpu_blit.v)
 *
 * The GPU blocks are their registers in front of C that does the same
 * work, word for word in the RTL's order -- so a VRAM-to-VRAM copy that
 * overlaps itself comes out as it does on the board -- but all of it
 * between two instructions. BUSY therefore always reads 0. Every word
 * written marks its framebuffer row dirty.
 *
 * Widths follow the RTL: the blitter's coordinates are 16 bits and
 * saturate (cw_sat), its VRAM addresses 17 bits; the rasterizer's
 * coordinates 10 bits and wrap; the scissors 11 and 10 bits.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stddef.h>
#include "zsys.h"

#define SYSCLK_HZ 48000000u
#define IRQ_HID0 (1u << 5)			// rtl/sysctl.v: cpu_irq[5], [6]; latched
#define IRQ_HID1 (1u << 6)

// -- VRAM, as the GPU's port sees it --

static inline uint32_t vram_rd(zsys_t *s, uint32_t off) {
	uint32_t v;
	memcpy(&v, s->vram + (off & (s->vram_size - 1) & ~3u), 4);
	return v;
}

static inline void vram_wr(zsys_t *s, uint32_t off, uint32_t v) {
	off &= (s->vram_size - 1) & ~3u;
	memcpy(s->vram + off, &v, 4);
	zsys_vram_dirty(s, off);
}

// The blitter's s port: main memory (or anything on the main bus that
// is memory), physical -- the blitter does not go through the MTU.
static uint32_t main_rd(zsys_t *s, uint32_t pa) {
	pa &= ~3u;
	uint32_t v = 0;
	switch (pa >> 28) {
	case 0x0: memcpy(&v, s->bram + (pa & (s->bram_size - 1)), 4); break;
	case 0x2: v = vram_rd(s, pa); break;
	case 0x4: if (s->ram_size) memcpy(&v, s->ram + (pa & (s->ram_size - 1)), 4); break;
	case 0x1: {
		uint32_t o = pa & 0x0fffffffu;
		v = 0xffffffffu;
		if (s->flash && o + 4 <= s->flash_size) memcpy(&v, s->flash + o, 4);
		break;
	}
	}
	return v;
}

// =====================================================================
// the blitter (rtl/gpu/gpu_blit.v)
// =====================================================================

#define CTRL_START  (1u << 0)
#define CTRL_FILL   (1u << 1)
#define CTRL_CLIP   (1u << 2)
#define CTRL_GLYPH  (1u << 3)
#define CTRL_SRCMEM (1u << 4)
#define CTRL_ROP(c) (((c) >> 5) & 3)
#define CTRL_COOKIE (1u << 7)
#define CTRL_DITHER (1u << 8)
#define CTRL_KEPT   0x1feu			// what CTRL reads back

#define AW_MASK 0x1ffffu			// VRAM byte addresses, 17 bits

static inline uint16_t cw_sat(uint32_t v) { return v > 0xffffu ? 0xffffu : (uint16_t)v; }
static inline uint16_t glyph_w_sat(uint32_t v) { return v > 0xffffu ? (uint16_t)(0xffc0u | (v & 63)) : (uint16_t)v; }

static inline uint32_t rop(uint32_t op, uint32_t dst, uint32_t src) {
	switch (op) {
	case 1: return dst | src;
	case 2: return dst ^ src;
	case 3: return dst & ~src;
	}
	return src;
}

// The 4x4 ordered dither: bit c of row r lit when bayer < level.
static uint32_t dither_word(uint32_t y, uint32_t level) {
	static const uint8_t bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
	uint32_t w = 0;
	for (int c = 0; c < 32; c++)
		if (bayer[((y & 3) << 2) | (c & 3)] < level) w |= 1u << c;
	return w;
}

// A source word for copy mode: from main memory (SRCMEM) or VRAM.
static uint32_t src_rd(zsys_t *s, bool srcmem, uint32_t a) {
	zsys_blit_t *b = &s->blit;
	uint32_t v = srcmem ? main_rd(s, a) : vram_rd(s, a - 0x20000000u);
	b->dbg_adr = a; b->dbg_dat = v; b->dbg_cnt++;
	return v;
}

static void blit_glyph(zsys_t *s, const zsys_blit_t *w, bool clip) {
	uint32_t off = w->dst_x & 31;
	uint32_t line = ((uint32_t)w->dst_y * ZSYS_STRIDE + (uint32_t)(w->dst_x >> 5) * 4) & AW_MASK;
	uint32_t cur_y = w->dst_y & 0x3ff;
	uint32_t gw = w->glyph_w;
	uint32_t cell = gw >= 32 ? 0xffffffffu : (1u << gw) - 1;
	bool straddle = ((off + (gw & 63)) & 63) > 32;
	uint32_t fg = (w->fg & 1) ? 0xffffffffu : 0, bg = (w->bg & 1) ? 0xffffffffu : 0;

	// the scissor's horizontal mask for the two words a cell can touch
	uint32_t cmask_lo = 0xffffffffu, cmask_hi = 0xffffffffu;
	if (clip) {
		uint32_t wx_lo = w->dst_x & 0xffe0u, wx_hi = (wx_lo + 32) & 0xffffu;
		uint32_t cx0 = w->clip_x0, cx1 = w->clip_x1;
		#define LIM(cx, wx) ((cx) <= (wx) ? 0 : ((cx) - (wx)) >= 32 ? 32 : (cx) - (wx))
		uint32_t dl_lo = LIM(cx0, wx_lo), kl_lo = LIM(cx1, wx_lo);
		uint32_t dl_hi = LIM(cx0, wx_hi), kl_hi = LIM(cx1, wx_hi);
		#undef LIM
		#define SH(n) ((uint32_t)(~0ull << (n)))
		cmask_lo = SH(dl_lo) & ~SH(kl_lo);
		cmask_hi = SH(dl_hi) & ~SH(kl_hi);
		#undef SH
	}

	uint32_t row = 0;
	do {
		uint8_t g = s->glyph[(w->glyph_addr + row) & (ZSYS_GLYPH_SIZE - 1)];
		uint8_t rev = 0;
		for (int i = 0; i < 8; i++) if (g & (0x80 >> i)) rev |= 1u << i;	// MSB-first: leftmost
		bool visible = !clip || (cur_y >= (w->clip_y0 & 0x3ffu) && cur_y < (w->clip_y1 & 0x3ffu));
		if (visible) {
			uint64_t bits = (uint64_t)(rev & cell) << off, cel = (uint64_t)cell << off;
			uint32_t b_lo = (uint32_t)bits & cmask_lo, c_lo = (uint32_t)cel & cmask_lo;
			uint32_t d = vram_rd(s, line);
			vram_wr(s, line, (d & ~c_lo) | (fg & b_lo) | (bg & (c_lo & ~b_lo)));
			if (straddle) {
				uint32_t b_hi = (uint32_t)(bits >> 32) & cmask_hi, c_hi = (uint32_t)(cel >> 32) & cmask_hi;
				d = vram_rd(s, line + 4);
				vram_wr(s, line + 4, (d & ~c_hi) | (fg & b_hi) | (bg & (c_hi & ~b_hi)));
			}
		}
		line = (line + ZSYS_STRIDE) & AW_MASK;
		cur_y = (cur_y + 1) & 0x3ff;
		row++;
	} while (row < w->glyph_h);
}

static void blit_start(zsys_t *s, uint32_t ctrl) {

	zsys_blit_t w = s->blit;			// the work_* registers: latched at the start
	bool clip = ctrl & CTRL_CLIP, fill = ctrl & CTRL_FILL, srcmem = ctrl & CTRL_SRCMEM;
	bool cookie = ctrl & CTRL_COOKIE, dither = ctrl & CTRL_DITHER;
	uint32_t op = CTRL_ROP(ctrl);
	s->blit.blits++;

	if (ctrl & CTRL_GLYPH) { blit_glyph(s, &w, clip); return; }

	uint32_t words, lines, y, line, lmask = 0xffffffffu, rmask = 0xffffffffu;
	if (clip) {
		uint32_t x_end = (uint32_t)(w.dst_x + w.width) & 0xffffu, y_end = (uint32_t)(w.dst_y + w.height) & 0xffffu;
		uint32_t fx = w.dst_x < w.clip_x0 ? w.clip_x0 : w.dst_x;
		uint32_t fy = w.dst_y < w.clip_y0 ? w.clip_y0 : w.dst_y;
		uint32_t fxe = x_end > w.clip_x1 ? w.clip_x1 : x_end;
		uint32_t fye = y_end > w.clip_y1 ? w.clip_y1 : y_end;
		if (fx >= fxe || fy >= fye) return;
		uint32_t left = fx & ~31u, right = ((fxe + 31) >> 5) << 5;
		words = ((right - left) & 0xffffu) >> 5;
		lines = fye - fy;
		y = fy;
		line = (fy * ZSYS_STRIDE + (left >> 3)) & AW_MASK;
		lmask = 0xffffffffu << (fx - left);
		rmask = (fxe & 31) == 0 ? 0xffffffffu : 0xffffffffu >> (32 - (fxe & 31));
	} else {
		words = ((uint32_t)(w.width + 31) & 0xffffu) >> 5;
		lines = w.height;
		y = w.dst_y;
		line = ((uint32_t)w.dst_y * ZSYS_STRIDE + (uint32_t)(w.dst_x >> 5) * 4) & AW_MASK;
	}

	uint32_t row_a = w.src_addr, row_b = w.src_b_addr;
	uint32_t shift = w.src_shift & 31;
	bool prime = (w.src_shift >> 8) & 1;
	uint32_t cur_line = 0;

	// the state machine's loops, as do-whiles: ST_NEXT tests after the
	// word, so a zero width or height still writes one word, as on the board
	do {
		uint32_t a = row_a, bb = row_b, prev_a = 0, prev_b = 0, addr = line, wi = 0;
		if (!fill && !prime) {			// ST_MEM_PRIME
			prev_a = src_rd(s, srcmem, a); a += 4;
			if (cookie) { prev_b = src_rd(s, srcmem, bb); bb += 4; }
		}
		do {
			uint32_t old = vram_rd(s, addr), out;
			uint32_t m = 0xffffffffu;
			if (clip) m = words > 1 ? (wi == 0 ? lmask : wi == words - 1 ? rmask : 0xffffffffu) : (lmask & rmask);
			if (fill) {
				uint32_t src = dither ? dither_word(y, w.pattern & 31) : w.pattern;
				out = (old & ~m) | (rop(op, old, src) & m);
			} else {
				uint32_t cur = src_rd(s, srcmem, a), wa, wb = 0;	// ST_MEM_READ, ST_MEM_BLEND
				wa = shift ? (prev_a >> shift) | (cur << (32 - shift)) : prev_a;
				prev_a = cur; a += 4;
				if (cookie) {
					uint32_t cb = src_rd(s, srcmem, bb);
					wb = shift ? (prev_b >> shift) | (cb << (32 - shift)) : prev_b;
					prev_b = cb; bb += 4;
				}
				// the destination is read after the source, as ST_READ follows the blend
				old = vram_rd(s, addr);
				uint32_t src = cookie ? (wa & wb) | (~wa & old) : rop(op, old, wa);
				out = (old & ~m) | (src & m);
			}
			vram_wr(s, addr, out);
			addr = (addr + 4) & AW_MASK;
			wi++;
		} while (wi < words);
		cur_line++;
		y = (y + 1) & 0x3ff;
		line = (line + ZSYS_STRIDE) & AW_MASK;
		row_a += w.src_stride;
		row_b += w.src_stride;
	} while (cur_line < lines);
}

static uint32_t blit_read(zsys_t *s, uint32_t r) {
	zsys_blit_t *b = &s->blit;
	switch (r) {
	case 0: return b->ctrl & CTRL_KEPT;
	case 1: return 0;				// STATUS: done before the next instruction
	case 2: return b->dst_x;
	case 3: return b->dst_y;
	case 4: return b->width;
	case 5: return b->height;
	case 6: return b->pattern;
	case 7: return b->glyph_addr;
	case 8: return b->glyph_w;
	case 9: return b->glyph_h;
	case 10: return b->fg;
	case 11: return b->bg;
	case 12: return b->src_addr;
	case 13: return b->src_stride;
	case 14: return b->src_shift;
	case 15: return b->src_b_addr;
	case 16: return b->dbg_adr;
	case 17: return b->dbg_dat;
	case 18: return b->dbg_cnt;
	case 20: return b->clip_x0;
	case 21: return b->clip_y0;
	case 22: return b->clip_x1;
	case 23: return b->clip_y1;
	}
	return 0;
}

static void blit_write(zsys_t *s, uint32_t r, uint32_t v) {
	zsys_blit_t *b = &s->blit;
	switch (r) {
	case 0: b->ctrl = v & CTRL_KEPT; if (v & CTRL_START) blit_start(s, v); return;
	case 2: b->dst_x = cw_sat(v); return;
	case 3: b->dst_y = cw_sat(v); return;
	case 4: b->width = cw_sat(v); return;
	case 5: b->height = cw_sat(v); return;
	case 6: b->pattern = v; return;
	case 7: b->glyph_addr = v; return;
	case 8: b->glyph_w = glyph_w_sat(v); return;
	case 9: b->glyph_h = cw_sat(v); return;
	case 10: b->fg = v; return;
	case 11: b->bg = v; return;
	case 12: b->src_addr = v; return;
	case 13: b->src_stride = v; return;
	case 14: b->src_shift = v; return;
	case 15: b->src_b_addr = v; return;
	case 20: b->clip_x0 = v & 0x7ff; return;
	case 21: b->clip_y0 = v & 0x7ff; return;
	case 22: b->clip_x1 = v & 0x7ff; return;
	case 23: b->clip_y1 = v & 0x7ff; return;
	}
}

// =====================================================================
// the line rasterizer (rtl/gpu/gpu_raster.v): Bresenham, one pixel a
// read-modify-write, inclusive scissor, at most 1002 pixels a line
// =====================================================================

static void raster_line(zsys_t *s) {
	zsys_raster_t *r = &s->raster;
	int x0 = r->x0, y0 = r->y0, x1 = r->x1, y1 = r->y1;
	int deltax = ((x1 - x0) << 21) >> 21, deltay = ((y1 - y0) << 21) >> 21;	// 11 bits, signed
	bool right = deltax >= 0, down = deltay >= 0;
	int dx = right ? deltax : -deltax, dy = down ? -deltay : deltay;
	int err = dx + dy;
	uint32_t cx = (uint32_t)x0, cy = (uint32_t)y0, count = 0;
	r->lines++;
	for (;;) {
		bool in = !r->clip || (cx >= r->cx0 && cx <= r->cx1 && cy >= r->cy0 && cy <= r->cy1);
		if (in) {
			uint32_t a = (cy * 20 + (cx >> 5)) * 4, m = 1u << (cx & 31), d = vram_rd(s, a);
			vram_wr(s, a, r->color == 0 ? d & ~m : r->color == 2 ? d ^ m : d | m);
		}
		bool at_end = cx == (uint32_t)x1 && cy == (uint32_t)y1;
		bool stop = at_end || count > 1000;
		count++;
		if (stop) break;
		int e2 = err * 2;
		bool gx = e2 > dy, ly = e2 < dx;
		if (gx) { cx = (right ? cx + 1 : cx - 1) & 0x3ff; err += dy; }
		if (ly) { cy = (down ? cy + 1 : cy - 1) & 0x3ff; err += dx; }
	}
	r->count = (uint16_t)count;
	r->cur_x = (uint16_t)cx;
	r->cur_y = (uint16_t)cy;
}

static uint32_t raster_read(zsys_t *s, uint32_t i) {
	zsys_raster_t *r = &s->raster;
	switch (i) {
	case 0: return r->x0;
	case 1: return r->y0;
	case 2: return r->x1;
	case 3: return r->y1;
	case 4: return r->color;
	case 6: return 0;				// busy: never
	case 7: return r->count;
	case 8: return r->cur_x;
	case 9: return r->cur_y;
	case 10: return 0;				// the FIFO: always drained
	case 11: return r->cx0;
	case 12: return r->cy0;
	case 13: return r->cx1;
	case 14: return r->cy1;
	case 15: return r->clip;
	}
	return 0;
}

static void raster_write(zsys_t *s, uint32_t i, uint32_t v) {
	zsys_raster_t *r = &s->raster;
	switch (i) {
	case 0: r->x0 = v & 0x3ff; return;
	case 1: r->y0 = v & 0x3ff; return;
	case 2: r->x1 = v & 0x3ff; return;
	case 3: r->y1 = v & 0x3ff; return;
	case 4: r->color = v & 3; return;
	case 5: raster_line(s); return;
	case 11: r->cx0 = v & 0x3ff; return;
	case 12: r->cy0 = v & 0x3ff; return;
	case 13: r->cx1 = v & 0x3ff; return;
	case 14: r->cy1 = v & 0x3ff; return;
	case 15: r->clip = v & 1; return;
	}
}

// =====================================================================
// socctl (rtl/socctl.v)
// =====================================================================

#define SOCCTL_MAGIC  0x5a435452u		// "ZCTR"
#define RECONFIG_KEY  0x5a524254u		// "ZRBT"

// 800x525 pixel clocks of 25.175MHz a frame: 59.94 frames a second;
// lines 480..524 are the vertical blank.
static void frame(const zsys_t *s, uint32_t *n, bool *vblank) {
	double f = (double)s->cycles * 25175000.0 / (800.0 * 525.0) / SYSCLK_HZ;
	uint64_t whole = (uint64_t)f;
	*n = (uint32_t)(whole & 0xffff);
	*vblank = (f - (double)whole) * 525.0 >= 480.0;
}

static uint32_t socctl_read(zsys_t *s, uint32_t r) {
	switch (r) {
	case 0: return 0x80000000u | (s->soc_ctrl & 0x7fffffffu);
	case 1: return SOCCTL_MAGIC;
	case 2: return 0x56430000u | 0x8000u | ((uint32_t)s->video_bright << 8) | s->video_mode;
	case 3: return 0x5a470004u | (s->game & 3);	// GAME: available, wrap, enable
	case 4: return s->view;
	case 5: { uint32_t n; bool vb; frame(s, &n, &vb); return (vb ? 0x10000u : 0) | n; }
	case 6: return 0x5a520001u;			// RECONFIG: available
	case 7: return 0;				// DIRTY: no VRAM_DIRTY
	case 8: return 0x5a4b0000u | ((uint32_t)(s->cslots >> 2) << 4) | (s->cslots & 3);
	// COLOR: available, chunky, offsets, chunky available, XP 2 (eight planes), NP, EN
	case 9: return 0x5a508000u | (s->col_chunky ? 1u << 14 : 0) | (uint32_t)s->col_off << 8 | 1u << 7 |
		2u << 4 | (uint32_t)s->col_np << 1 | (s->col_en ? 1 : 0);
	case 10: return 0x5a440000u;			// DDMI
	case 14: return 0x5a560000u;			// CVBS
	case 16: return 0x5a57c000u | (s->win_desk ? 2 : 0) | (s->win_en ? 1 : 0);	// COLWIN: avail, davail
	case 17: return (uint32_t)s->win_y1 << 16 | s->win_x1;
	}
	return 0;
}

static void socctl_write(zsys_t *s, uint32_t r, uint32_t v, uint32_t sel) {
	uint32_t lanes = (sel & 1 ? 0xffu : 0) | (sel & 2 ? 0xff00u : 0) | (sel & 4 ? 0xff0000u : 0) |
		(sel & 8 ? 0xff000000u : 0);
	switch (r) {
	case 0: s->soc_ctrl = (s->soc_ctrl & ~lanes) | (v & lanes); return;
	case 2:
		if (sel & 1) s->video_mode = v & 3;
		if ((sel & 2) && (v & 0x8000u)) s->video_bright = (v >> 8) & 15;
		return;
	case 3:					// GAME
		if (sel & 1) {
			s->game = v & 3;
			if (!(v & 1)) { s->col_en = false; s->col_chunky = false; }	// leaving game mode leaves colour
			else s->win_en = false;		// entering it leaves the colour window
			s->video_gen++;
		}
		return;
	case 4: {				// VIEW, range-limited on the way in
		uint32_t vx = v & 0x3ff, vy = (v >> 16) & 0x3ff;
		if (vx > 639) vx = 639;
		if (vy > 479) vy = 479;
		if (sel & 3) s->view = (s->view & 0xffff0000u) | vx;
		if (sel & 12) s->view = (s->view & 0xffffu) | vy << 16;
		s->video_gen++;
		return;
	}
	case 9:					// COLOR
		if (sel & 1) { s->col_en = v & 1; s->col_np = (v >> 1) & 7; }
		if (sel & 2) { s->col_off = (v >> 8) & 63; s->col_chunky = (v >> 14) & 1; }
		s->video_gen++;
		return;
	case 13:				// PALETTE: whole words
		if (sel == 15) { s->palette[v >> 24] = v & 0xfff; s->video_gen++; }
		return;
	case 16:				// COLWIN
		if (sel & 3) s->win_x0 = v & 0x3ff;
		if (sel & 12) { s->win_y0 = (v >> 16) & 0x3ff; s->win_en = (v >> 31) & 1; s->win_desk = (v >> 30) & 1; }
		s->video_gen++;
		return;
	case 17:				// COLWINEND: staged, adopted with the next COLWIN
		if (sel & 3) s->win_x1 = v & 0x3ff;
		if (sel & 12) s->win_y1 = (v >> 16) & 0x3ff;
		return;
	case 6:
		if (sel == 15 && v == RECONFIG_KEY) s->reboot = true;
		return;
	case 8:
		if (sel & 1) s->cslots = (uint8_t)((((v >> 4) & 3) << 2) | (v & 3));
		if (sel & 4) s->caddr = (v >> 16) & 63;
		return;
	case 15:
		if (sel == 15) { s->cursor_rows[s->caddr] = (uint16_t)v; s->caddr = (s->caddr + 1) & 63; }
		return;
	}
}

// =====================================================================
// the RTC (rtl/rtc.v): seconds since the epoch and 1/1024ths, settable
// =====================================================================

#define RTC_MAGIC 0x5a525443u			// "ZRTC"
#define SUBSEC_HZ 1024u

static uint64_t rtc_ticks(const zsys_t *s) {	// 1/1024ths of a second since the epoch
	uint64_t el = (s->cycles - s->rtc_cycles) * SUBSEC_HZ / SYSCLK_HZ;
	return (uint64_t)s->rtc_sec * SUBSEC_HZ + s->rtc_sub_pre + el;
}

static void rtc_set(zsys_t *s, uint32_t sec, uint32_t sub) {
	s->rtc_sec = sec;
	s->rtc_sub_pre = sub;
	s->rtc_cycles = s->cycles;
}

void zsys_rtc_from_host(zsys_t *s) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	rtc_set(s, (uint32_t)ts.tv_sec, (uint32_t)(ts.tv_nsec / (1000000000 / SUBSEC_HZ)) % SUBSEC_HZ);
	s->rtc_valid = true;
	s->rtc_host = true;
}

static uint32_t rtc_read(zsys_t *s, uint32_t r) {
	switch (r) {
	case 0: return RTC_MAGIC;
	case 1: return (uint32_t)(rtc_ticks(s) / SUBSEC_HZ);
	case 2: return (uint32_t)(rtc_ticks(s) % SUBSEC_HZ);
	case 3: return s->rtc_valid;
	case 4: return SUBSEC_HZ;
	case 5: return (uint32_t)(int32_t)s->rtc_tz;
	}
	return 0;
}

static void rtc_write(zsys_t *s, uint32_t r, uint32_t v, uint32_t sel) {
	switch (r) {
	case 1: {
		uint32_t sec = (uint32_t)(rtc_ticks(s) / SUBSEC_HZ);
		for (int l = 0; l < 4; l++)
			if (sel & (1u << l)) sec = (sec & ~(0xffu << 8 * l)) | (v & (0xffu << 8 * l));
		rtc_set(s, sec, s->rtc_sub_staged);
		s->rtc_sub_staged = 0;
		s->rtc_valid = true;
		return;
	}
	case 2:
		if (sel & 1) s->rtc_sub_staged = (s->rtc_sub_staged & ~0xffu) | (v & 0xff);
		if (sel & 2) s->rtc_sub_staged = (s->rtc_sub_staged & 0xffu) | (v & 0x300);
		return;
	case 3: if (sel & 1) s->rtc_valid = v & 1; return;
	case 5:
		if (sel & 1) s->rtc_tz = (int16_t)(((uint16_t)s->rtc_tz & 0xff00u) | (v & 0xff));
		if (sel & 2) s->rtc_tz = (int16_t)(((uint16_t)s->rtc_tz & 0x00ffu) | (v & 0xff00u));
		return;
	}
}

// =====================================================================
// the SD card, in SPI mode, behind rtl/spim.v
//
// A version 2 high-capacity card (CCS set: block addresses), as the
// driver (sw/apps/fs/fatfs/sdmm.c) expects of anything recent: CMD0, 8,
// 55 + ACMD41, 58, 9 (the CSD gives the size), 12, 13, 16, 17, 18, 23,
// 24, 25, 32, 33, 38 and 59. No CRCs are checked, and none are sent
// that mean anything (0xFFFF), as with CRC off.
// =====================================================================

enum { SD_NONE, SD_READ_MULTI, SD_WAIT_TOKEN, SD_WAIT_TOKEN_MULTI, SD_RECV, SD_RECV_MULTI };

static void sd_q(zsys_sd_t *d, uint8_t b) { if (d->out_n < (int)sizeof(d->out)) d->out[d->out_n++] = b; }
static void sd_q_clear(zsys_sd_t *d) { d->out_n = d->out_pos = 0; }

static void sd_q_block(zsys_t *s, uint32_t block) {
	zsys_sd_t *d = &s->sd;
	sd_q(d, 0xff);
	sd_q(d, 0xfe);
	uint64_t o = (uint64_t)block * 512;
	for (int i = 0; i < 512; i++) sd_q(d, o + 512 <= d->size ? d->img[o + i] : 0);
	sd_q(d, 0xff);
	sd_q(d, 0xff);
	d->reads++;
}

static void sd_write_block(zsys_t *s) {
	zsys_sd_t *d = &s->sd;
	uint64_t o = (uint64_t)d->block * 512;
	if (o + 512 <= d->size) {
		memcpy(d->img + o, d->wbuf, 512);
		if (d->written) d->written[d->block >> 3] |= (uint8_t)(1u << (d->block & 7));
		if (d->fd >= 0 && pwrite(d->fd, d->wbuf, 512, (off_t)o) != 512)
			fprintf(stderr, "zsys: the card image: a write at block %lu failed\n", (unsigned long)d->block);
	}
	d->block++;
	d->writes++;
}

static bool sd_in_range(const zsys_sd_t *d, uint32_t block) { return (uint64_t)block * 512 + 512 <= d->size; }

static void sd_command(zsys_t *s) {
	zsys_sd_t *d = &s->sd;
	uint8_t cmd = d->cmd[0] & 0x3f;
	uint32_t arg = (uint32_t)d->cmd[1] << 24 | (uint32_t)d->cmd[2] << 16 | (uint32_t)d->cmd[3] << 8 | d->cmd[4];
	bool app = d->app;
	d->app = false;

	if (cmd == 12) {				// STOP_TRANSMISSION: a stuff byte, then R1
		d->mode = SD_NONE;
		sd_q_clear(d);
		sd_q(d, 0xff); sd_q(d, 0xff); sd_q(d, 0x00); sd_q(d, 0x00); sd_q(d, 0xff);
		return;
	}

	sd_q_clear(d);
	d->mode = SD_NONE;
	uint8_t r1 = d->idle ? 0x01 : 0x00;
	sd_q(d, 0xff);					// Ncr: one byte before the response

	switch (cmd) {
	case 0: d->idle = true; sd_q(d, 0x01); return;
	case 1: d->idle = false; sd_q(d, 0x00); return;
	case 8: sd_q(d, r1); sd_q(d, 0); sd_q(d, 0); sd_q(d, (arg >> 8) & 0x0f); sd_q(d, arg & 0xff); return;
	case 55: d->app = true; sd_q(d, r1); return;
	case 41:
		if (app) { d->idle = false; sd_q(d, 0x00); return; }
		break;
	case 58: sd_q(d, r1); sd_q(d, 0xc0); sd_q(d, 0xff); sd_q(d, 0x80); sd_q(d, 0x00); return;	// powered up, CCS
	case 9: {					// the CSD, version 2: C_SIZE from the image's size
		uint32_t c = (uint32_t)(d->size / (512u * 1024u)) - 1;
		uint8_t csd[16] = { 0x40, 0x0e, 0x00, 0x32, 0x5b, 0x59, 0x00, (uint8_t)((c >> 16) & 0x3f),
			(uint8_t)(c >> 8), (uint8_t)c, 0x7f, 0x80, 0x0a, 0x40, 0x00, 0x01 };
		sd_q(d, r1); sd_q(d, 0xff); sd_q(d, 0xfe);
		for (int i = 0; i < 16; i++) sd_q(d, csd[i]);
		sd_q(d, 0xff); sd_q(d, 0xff);
		return;
	}
	case 13: sd_q(d, r1); sd_q(d, 0x00); return;
	case 16: case 23: case 32: case 33: case 59: sd_q(d, r1); return;
	case 38: sd_q(d, r1); sd_q(d, 0x00); return;
	case 17:
		if (!sd_in_range(d, arg)) { sd_q(d, r1 | 0x20); return; }
		sd_q(d, r1);
		sd_q_block(s, arg);
		return;
	case 18:
		if (!sd_in_range(d, arg)) { sd_q(d, r1 | 0x20); return; }
		sd_q(d, r1);
		d->block = arg;
		d->mode = SD_READ_MULTI;
		return;
	case 24: case 25:
		if (!sd_in_range(d, arg)) { sd_q(d, r1 | 0x20); return; }
		sd_q(d, r1);
		d->block = arg;
		d->mode = cmd == 24 ? SD_WAIT_TOKEN : SD_WAIT_TOKEN_MULTI;
		return;
	}
	sd_q(d, r1 | 0x04);				// an illegal command
}

// One byte each way.
static uint8_t sd_xchg(zsys_t *s, uint8_t in) {
	zsys_sd_t *d = &s->sd;
	if (!d->img || !d->cs) return 0xff;

	// what the card sends: the queue, or the next block of a multiple read
	if (d->out_pos >= d->out_n) {
		sd_q_clear(d);
		if (d->mode == SD_READ_MULTI) {
			if (sd_in_range(d, d->block)) sd_q_block(s, d->block++);
			else { d->mode = SD_NONE; sd_q(d, 0xff); }
		}
	}
	uint8_t out = d->out_pos < d->out_n ? d->out[d->out_pos++] : 0xff;

	// what it takes in
	switch (d->mode) {
	case SD_RECV: case SD_RECV_MULTI:
		d->wbuf[d->wn++] = in;
		if (d->wn == 514) {			// the block and its CRC: accepted, then busy
			sd_write_block(s);
			sd_q_clear(d);
			sd_q(d, 0x05); sd_q(d, 0x00); sd_q(d, 0x00);
			d->mode = d->mode == SD_RECV_MULTI ? SD_WAIT_TOKEN_MULTI : SD_NONE;
		}
		return out;
	case SD_WAIT_TOKEN:
		if (in == 0xfe) { d->mode = SD_RECV; d->wn = 0; }
		return out;
	case SD_WAIT_TOKEN_MULTI:
		if (in == 0xfc) { d->mode = SD_RECV_MULTI; d->wn = 0; }
		else if (in == 0xfd) { d->mode = SD_NONE; sd_q_clear(d); sd_q(d, 0xff); sd_q(d, 0x00); }
		return out;
	}
	if (d->cmd_n == 0 && (in & 0xc0) != 0x40) return out;	// not a command's first byte
	d->cmd[d->cmd_n++] = in;
	if (d->cmd_n == 6) { d->cmd_n = 0; sd_command(s); }
	return out;
}

bool zsys_sd_insert(zsys_t *s, const char *path, bool snapshot) {
	zsys_sd_t *d = &s->sd;
	int fd = open(path, snapshot ? O_RDONLY : O_RDWR);
	if (fd < 0) { perror(path); return false; }
	struct stat st;
	if (fstat(fd, &st) < 0 || st.st_size < 512 * 1024) {
		fprintf(stderr, "zsys: %s: a card image is at least 512KB\n", path);
		close(fd);
		return false;
	}
	uint8_t *img = malloc((size_t)st.st_size);
	if (!img || pread(fd, img, (size_t)st.st_size, 0) != st.st_size) {
		fprintf(stderr, "zsys: %s: could not read it\n", path);
		free(img);
		close(fd);
		return false;
	}
	free(d->img);
	if (d->fd >= 0) close(d->fd);
	d->img = img;
	d->size = (uint64_t)st.st_size;
	if (snapshot) { close(fd); fd = -1; }
	d->fd = fd;
	return true;
}

bool zsys_sd_insert_mem(zsys_t *s, uint8_t *img, uint64_t size) {
	zsys_sd_t *d = &s->sd;
	if (size < 512 * 1024) return false;
	uint8_t *w = calloc(1, (size_t)(size / 512 / 8 + 1));
	if (!w) return false;
	free(d->img);
	free(d->written);
	if (d->fd >= 0) close(d->fd);
	d->img = img;
	d->size = size;
	d->fd = -1;
	d->written = w;
	return true;
}

static uint32_t spisd_read(zsys_t *s, uint32_t r) {
	switch (r) {
	case 0: return s->spisd_data;			// DATA: what the last transfer received
	case 1: return (s->spisd_ctrl & 1) << 1 | (1u << 2);	// STATUS: not busy, CS, INT high
	case 2: return s->spisd_ctrl;
	case 3: return 0x53504931u;			// MAGIC "SPI1"
	}
	return 0;
}

static void spisd_write(zsys_t *s, uint32_t r, uint32_t v) {
	switch (r) {
	case 0:
		if (s->spisd_ctrl & 2) {		// 32 bits: byte 0 first, received likewise
			uint32_t rx = 0;
			for (int i = 0; i < 4; i++) rx |= (uint32_t)sd_xchg(s, (uint8_t)(v >> 8 * i)) << 8 * i;
			s->spisd_data = rx;
		} else s->spisd_data = sd_xchg(s, (uint8_t)v);
		return;
	case 2:
		s->spisd_ctrl = v & 0xff03u;
		s->sd.cs = v & 1;
		if (!s->sd.cs) s->sd.cmd_n = 0;
		return;
	}
}

// =====================================================================
// the USB HID ports (rtl/usb_hid.v): what the devices last reported,
// a report raising the port's interrupt
// =====================================================================

void zsys_key(zsys_t *s, uint8_t usage, bool down) {
	zsys_hid_t *h = &s->hid[0];
	if (usage >= 0xe0 && usage <= 0xe7) {
		uint8_t bit = (uint8_t)(1u << (usage - 0xe0)), m = down ? h->mods | bit : h->mods & ~bit;
		if (m == h->mods) return;			// no change: no report
		h->mods = m;
	} else if (usage >= 4) {
		int at = -1;
		for (int i = 0; i < 4; i++) if (h->keys[i] == usage) at = i;
		if (down && at < 0) {
			for (int i = 0; i < 4; i++) if (!h->keys[i]) { h->keys[i] = usage; at = i; break; }
			if (at < 0) return;		// four held already: the boot report has no room
		} else if (!down && at >= 0) {
			for (int i = at; i < 3; i++) h->keys[i] = h->keys[i + 1];
			h->keys[3] = 0;
		} else return;
	} else return;
	h->reports++;
	s->irq_pending |= IRQ_HID0;
}

void zsys_mouse(zsys_t *s, int x, int y, uint8_t buttons) {
	zsys_hid_t *h = &s->hid[1];
	x = x < 0 ? 0 : x > 639 ? 639 : x;
	y = y < 0 ? 0 : y > 479 ? 479 : y;
	int dx = x - h->cx, dy = y - h->cy;
	if (!dx && !dy && buttons == h->btn) return;
	h->dx = (int8_t)(dx < -127 ? -127 : dx > 127 ? 127 : dx);
	h->dy = (int8_t)(dy < -127 ? -127 : dy > 127 ? 127 : dy);
	// the cursor's old and new rows change on the screen, not in VRAM
	for (int r = -8; r < 8; r++) {
		int a = h->cy + r, b = y + r;
		if (a >= 0 && a < ZSYS_ROWS) s->dirty[a >> 5] |= 1u << (a & 31);
		if (b >= 0 && b < ZSYS_ROWS) s->dirty[b >> 5] |= 1u << (b & 31);
	}
	h->cx = (uint16_t)x;
	h->cy = (uint16_t)y;
	h->btn = buttons;
	h->reports++;
	s->irq_pending |= IRQ_HID1;
}

void zsys_wheel(zsys_t *s, int notches) {
	zsys_hid_t *h = &s->hid[1];
	if (!notches) return;
	h->wheel = (uint8_t)(h->wheel + notches);
	h->dx = h->dy = 0;
	h->reports++;
	s->irq_pending |= IRQ_HID1;
}

// -- the virtual mouse (rtl/sysctl.v `VMOUSE) --

static void vmouse_set(zsys_t *s, uint32_t v) {
	int ox, oy, nx, ny;
	zsys_pointer(s, &ox, &oy);
	s->vmouse = v;
	zsys_pointer(s, &nx, &ny);
	// the cursor's old and new rows change on the screen
	for (int r = -8; r < 8; r++) {
		int a = oy + r, b = ny + r;
		if (a >= 0 && a < ZSYS_ROWS) s->dirty[a >> 5] |= 1u << (a & 31);
		if (b >= 0 && b < ZSYS_ROWS) s->dirty[b >> 5] |= 1u << (b & 31);
	}
}

uint32_t zsys_vmouse_read(zsys_t *s) { return s->vmouse; }
void zsys_vmouse_write(zsys_t *s, uint32_t v) { vmouse_set(s, v); }

void zsys_touch(zsys_t *s, int x, int y, bool down) {
	x = x < 0 ? 0 : x > 639 ? 639 : x;
	y = y < 0 ? 0 : y > 479 ? 479 : y;
	vmouse_set(s, (uint32_t)x | (uint32_t)y << 10 | (down ? 1u : 0u) << 20 | 1u << 24 | 1u << 25 |
		(uint32_t)(s->vwheel & 0x3f) << 26);
	s->irq_pending |= IRQ_HID1;		// wakes wm, as net's z_wm_wake() would
}

void zsys_touch_wheel(zsys_t *s, int notches) {
	if (!notches || !(s->vmouse & (1u << 24))) return;
	s->vwheel = (uint8_t)(s->vwheel + notches);
	vmouse_set(s, (s->vmouse & 0x03ffffffu) | (uint32_t)(s->vwheel & 0x3f) << 26);
	s->irq_pending |= IRQ_HID1;
}

void zsys_touch_off(zsys_t *s) {
	if (!s->vmouse) return;
	vmouse_set(s, 0);
	s->irq_pending |= IRQ_HID1;
}

void zsys_keyboard_plugged(zsys_t *s, bool plugged) {
	zsys_hid_t *h = &s->hid[0];
	uint8_t typ = plugged ? 1 : 0;
	s->kbd_unplugged = !plugged;		// and so after a reboot too
	if (h->typ == typ) return;
	h->typ = typ;
	h->mods = 0;
	memset(h->keys, 0, sizeof(h->keys));
	s->irq_pending |= IRQ_HID0;		// rtl/usb_hid.v raises it on a type change
}

static uint32_t hid_read(zsys_t *s, uint32_t port, uint32_t r) {
	const zsys_hid_t *h = &s->hid[port];
	switch (r) {
	case 0: return (uint32_t)h->typ << 24 | (h->typ == 1 ? h->mods : 0);
	case 1: return h->typ == 1 ? (uint32_t)h->keys[0] << 24 | (uint32_t)h->keys[1] << 16 |
		(uint32_t)h->keys[2] << 8 | h->keys[3] : 0;
	// the top byte: the wheel's count -- the USB host's mouse slot has
	// it (Lakritz's real gateware); rtl/usb_hid.v's HID-only core reads 0
	case 2: return (uint32_t)h->wheel << 24 | (uint32_t)h->btn << 16 | (uint32_t)(uint8_t)h->dy << 8 | (uint8_t)h->dx;
	case 3: return (uint32_t)h->btn << 20 | (uint32_t)h->cy << 10 | h->cx;
	case 4: return (uint32_t)h->typ << 10;	// no gamepad state
	case 5: return 0x53504500u | h->speed;	// "SPE", the speed
	}
	return 0;
}

static void hid_write(zsys_t *s, uint32_t port, uint32_t r, uint32_t v) {
	if (r == 5) s->hid[port].speed = v & 63;
}

// =====================================================================
// writing the flash (rtl/spiflash.v, docs/spiflash.md): erase a 4KB
// sector, program up to a page, never below LOCK_END; armed for one
// operation at a time. Done at once, so BUSY never shows.
// =====================================================================

#define FL_MAGIC   0x5a464c53u			// "ZFLS"
#define FL_KEY     0x5a465752u			// "ZFWR"
#define FL_LOCK    0x040000u
#define FL_DONE    (1u << 1)
#define FL_LOCKED  (1u << 2)
#define FL_UNARMED (1u << 3)
#define FL_BADLEN  (1u << 4)

static void flash_put(zsys_t *s, uint32_t off, uint32_t n) {
	if (s->flash_fd >= 0 && pwrite(s->flash_fd, s->flash + off, n, (off_t)off) != (ssize_t)n)
		fprintf(stderr, "zsys: the flash image: a write at %06lx failed\n", (unsigned long)off);
}

static void flash_cmd(zsys_t *s, uint32_t cmd) {
	zsys_flashw_t *f = &s->flashw;
	uint32_t a = f->addr & 0xffffffu;
	f->status = 0;
	if (cmd == 3) { f->status = FL_DONE; return; }		// the ID: read already
	if (cmd != 1 && cmd != 2) return;
	bool armed = f->armed;
	f->armed = false;
	if (!armed) { f->status = FL_UNARMED; return; }
	if (cmd == 1) {
		a &= ~0xfffu;
		if (a < FL_LOCK) { f->status = FL_LOCKED; return; }
		if (a + 4096 <= s->flash_size) { memset(s->flash + a, 0xff, 4096); flash_put(s, a, 4096); }
		f->erases++;
	} else {
		if (f->len < 1 || f->len > 256 || (a & 0xff) + f->len > 256) { f->status = FL_BADLEN; return; }
		if (a < FL_LOCK) { f->status = FL_LOCKED; return; }
		if (a + f->len <= s->flash_size) {
			for (uint32_t i = 0; i < f->len; i++) s->flash[a + i] &= f->page[i];	// NOR: only 1 -> 0
			flash_put(s, a, f->len);
		}
		f->programs++;
	}
	f->status = FL_DONE;
}

static uint32_t flash_id(const zsys_t *s) {		// a Winbond W25Q of the image's size
	uint32_t cap = 0;
	while (cap < 31 && (1u << (cap + 1)) <= s->flash_size) cap++;
	return 0xef4000u | (s->flash_size ? cap : 0x15);
}

static uint32_t flashw_read(zsys_t *s, uint32_t off) {
	zsys_flashw_t *f = &s->flashw;
	if (off >= 0x100) return 0;			// the page buffer: write-only
	switch (off >> 2) {
	case 0: return FL_MAGIC;
	case 1: return f->status;
	case 2: return flash_id(s);
	case 3: return FL_LOCK;
	case 4: return f->addr;
	case 7: return f->len;
	}
	return 0;
}

static void flashw_write(zsys_t *s, uint32_t off, uint32_t v, uint32_t sel) {
	zsys_flashw_t *f = &s->flashw;
	if (off >= 0x100) {
		for (int l = 0; l < 4; l++) if (sel & (1u << l)) f->page[(off & 0xfc) + l] = (uint8_t)(v >> 8 * l);
		return;
	}
	switch (off >> 2) {
	case 4: f->addr = v; return;
	case 5: f->armed = sel == 15 && v == FL_KEY; return;
	case 6: flash_cmd(s, v); return;
	case 7: f->len = v & 0x1ff; return;
	}
}

// =====================================================================
// audio (rtl/audio.v, rtl/audio_mixer.v; docs/audio.md): the FIFO for
// software mixing, the eight-channel mixer reading samples from main
// memory, the sample clock (FSDIV), the system volume. Each frame is
// what the DAC would be given; it goes into a ring the host takes from
// (zsys_audio_take()).
// =====================================================================

#define AU_MAGIC   0x5a415544u			// "ZAUD"
#define AU_EN      0x01
#define AU_IRQEN   0x02
#define AU_MIXEN   0x40
#define AU_DEPTH_LOG2 10
#define AU_FRAC    14

static void au_refill(zsys_audio_t *a) {		// head from the FIFO, as the block does each cycle
	if ((a->ctrl & AU_EN) && !(a->ctrl & AU_MIXEN) && !a->head_valid && a->f_n) {
		a->head = a->fifo[a->f_rd];
		a->f_rd = (a->f_rd + 1) % ZSYS_AUDIO_FIFO;
		a->f_n--;
		a->head_valid = true;
	}
}

static uint32_t au_level(const zsys_audio_t *a) { return a->f_n + (a->head_valid ? 1u : 0u); }

bool zdev_audio_irq(const zsys_t *s) {
	const zsys_audio_t *a = &s->audio;
	return (a->ctrl & AU_IRQEN) && au_level(a) < a->wmark;
}

uint64_t zdev_audio_next_frame(const zsys_t *s) {
	const zsys_audio_t *a = &s->audio;
	uint64_t period = (uint64_t)a->mod * 128u, left = period > a->acc ? period - a->acc : 0;
	return a->step ? (left + a->step - 1) / a->step + 1 : SYSCLK_HZ;
}

double zsys_audio_hz(const zsys_t *s) {
	const zsys_audio_t *a = &s->audio;
	return a->mod ? (double)SYSCLK_HZ * a->step / (128.0 * a->mod) : 0;
}

// One frame of the mixer, as audio_mixer.v's sequencer computes it.
static void au_mix(zsys_t *s, int16_t *l, int16_t *r) {
	zsys_audio_t *a = &s->audio;
	int32_t acc_l = 0, acc_r = 0;
	for (int c = 0; c < 8; c++) {
		uint32_t ctrl = a->cctrl[c], pos = a->pos[c];
		bool active = (a->active >> c) & 1;
		bool past_end = (pos >> AU_FRAC) >= (a->len[c] & 0xfffff);
		uint32_t next = pos + a->cstep[c];
		uint32_t llen = a->llen[c] & 0xfffff, loopend = (a->lstart[c] & 0xfffff) + llen;
		bool wrap = llen && (next >> AU_FRAC) >= loopend;
		if (!active || !(ctrl & (1u << 16)) || past_end) {
			if (past_end && active) a->active &= (uint8_t)~(1u << c);
			continue;				// skipped: the position stays
		}
		uint32_t addr = a->base[c] + (pos >> AU_FRAC), w = main_rd(s, addr & ~3u);
		a->dbg_adr = addr & ~3u;
		a->dbg_dat = w;
		int32_t smp = (ctrl & (1u << 18)) ? (int16_t)(addr & 2 ? w >> 16 : w)	// 16-bit
			: (int16_t)(uint16_t)(((w >> 8 * (addr & 3)) & 0xff) << 8);		// 8-bit, to the top
		acc_l += smp * (int32_t)(ctrl & 0xff);
		acc_r += smp * (int32_t)((ctrl >> 8) & 0xff);
		a->pos[c] = wrap ? next - (llen << AU_FRAC) : next;
	}
	int64_t sl = ((int64_t)acc_l * a->mixvol) >> 18, sr = ((int64_t)acc_r * a->mixvol) >> 18;
	*l = (int16_t)(sl > 32767 ? 32767 : sl < -32768 ? -32768 : sl);
	*r = (int16_t)(sr > 32767 ? 32767 : sr < -32768 ? -32768 : sr);
}

void zdev_audio_frames(zsys_t *s) {
	zsys_audio_t *a = &s->audio;
	uint64_t period = (uint64_t)a->mod * 128u;
	if (!period) { a->acc = 0; return; }
	while (a->acc >= period) {
		a->acc -= period;
		a->frames++;
		int16_t l = a->out_l, r = a->out_r;
		if (a->ctrl & AU_EN) {
			if (a->ctrl & AU_MIXEN) au_mix(s, &l, &r);
			else if (a->head_valid) {		// the head, then the next behind it
				l = (int16_t)(a->head >> 16);
				r = (int16_t)a->head;
				a->head_valid = false;
				au_refill(a);
			} else a->underrun = true;		// and the last frame held
		}
		// a muted block (EN clear) still runs its clocks and plays nothing
		a->out_l = l;
		a->out_r = r;
		uint32_t vol = a->volume & 0x100 ? 256 : a->volume;
		int16_t vl = (a->ctrl & AU_EN) ? (int16_t)(((int32_t)l * (int32_t)vol) >> 8) : 0;
		int16_t vr = (a->ctrl & AU_EN) ? (int16_t)(((int32_t)r * (int32_t)vol) >> 8) : 0;
		if (a->ctrl & 0x04) { int16_t t = vl; vl = vr; vr = t; }	// SWAPLR
		uint32_t at = (a->r_head + a->r_n) % ZSYS_AUDIO_OUT;
		a->ring[at][0] = vl;
		a->ring[at][1] = vr;
		if (a->r_n < ZSYS_AUDIO_OUT) a->r_n++;
		else a->r_head = (a->r_head + 1) % ZSYS_AUDIO_OUT;	// the oldest is gone
	}
}

uint32_t zsys_audio_take(zsys_t *s, float *out, uint32_t max, uint32_t out_hz) {
	zsys_audio_t *a = &s->audio;
	double in_hz = zsys_audio_hz(s), step = out_hz ? in_hz / out_hz : 1;
	uint32_t n = 0;
	// linear, between the ring's first two frames; a frame is used up
	// once the phase passes it
	while (n < max && a->r_n >= 2) {
		const int16_t *f0 = a->ring[a->r_head], *f1 = a->ring[(a->r_head + 1) % ZSYS_AUDIO_OUT];
		float t = (float)a->r_phase;
		out[2 * n] = (f0[0] + (f1[0] - f0[0]) * t) / 32768.0f;
		out[2 * n + 1] = (f0[1] + (f1[1] - f0[1]) * t) / 32768.0f;
		n++;
		a->r_phase += step;
		while (a->r_phase >= 1.0 && a->r_n >= 2) {
			a->r_phase -= 1.0;
			a->r_head = (a->r_head + 1) % ZSYS_AUDIO_OUT;
			a->r_n--;
		}
	}
	return n;
}

static uint32_t audio_read(zsys_t *s, uint32_t r) {
	zsys_audio_t *a = &s->audio;
	uint32_t level = au_level(a);
	switch (r) {
	case 0: return AU_MAGIC;
	case 1: return a->ctrl;
	case 2: return (a->underrun ? 1u << 19 : 0) | (level < a->wmark ? 1u << 18 : 0) |
		(a->f_n >= ZSYS_AUDIO_FIFO ? 1u << 17 : 0) | (level == 0 ? 1u << 16 : 0) | (level & 0xffff);
	case 4: return a->rate;
	case 5: return a->wmark;
	case 6: return 0x5a410000u | 1u << 15 | 1u << 14 | 1u << 13 | 1u << 12 | 1u << 8 | AU_DEPTH_LOG2;
	case 7: return SYSCLK_HZ;
	case 8: return (uint32_t)a->spdif_fs << 8 | a->mixvol;
	case 9: return a->active;
	case 10: return a->dbg_adr;
	case 11: return a->dbg_dat;
	case 12: return a->mixpos_sel;
	case 13: return a->pos[a->mixpos_sel & 7];
	case 14: return a->volume;
	case 15: return (uint32_t)a->mod << 16 | a->step;
	}
	return 0;
}

static void audio_write(zsys_t *s, uint32_t r, uint32_t v, uint32_t sel) {
	zsys_audio_t *a = &s->audio;
	if (r >= 16 && r < 64) {				// a mixer channel's six words
		uint32_t off = r - 16, c = off / 6, w = off % 6;
		if (c > 7) return;
		switch (w) {
		case 0: a->base[c] = v; break;
		case 1: a->len[c] = v & 0xfffff; break;
		case 2: a->lstart[c] = v & 0xfffff; break;
		case 3: a->llen[c] = v & 0xfffff; break;
		case 4: a->cstep[c] = v; break;
		case 5:
			a->cctrl[c] = v;
			if (v & (1u << 17)) {			// TRIG: from offset x 256, EN as written
				a->pos[c] = (v >> 24) << (8 + AU_FRAC);
				a->active = (uint8_t)((a->active & ~(1u << c)) | ((v >> 16 & 1) << c));
			} else if (!(v & (1u << 16))) a->active &= (uint8_t)~(1u << c);
			break;
		}
		return;
	}
	switch (r) {
	case 1:
		if (!(sel & 1)) return;
		a->ctrl = (uint8_t)(v & 0xe7);
		if (v & 0x08) { a->f_rd = a->f_n = 0; a->head_valid = false; }	// FLUSH
		if (v & 0x10) a->underrun = false;			// CLRUR
		au_refill(a);
		return;
	case 3:
		if (a->f_n < ZSYS_AUDIO_FIFO) {			// a full FIFO drops it, silently
			a->fifo[(a->f_rd + a->f_n) % ZSYS_AUDIO_FIFO] = v;
			a->f_n++;
			au_refill(a);
		}
		return;
	case 4:
		if (sel & 1) {
			uint32_t eff = (v & 0xff) < 2 ? 2 : (v & 0xff);
			a->rate = (uint8_t)v;
			a->step = 128;
			a->mod = (uint16_t)(64 * eff);
		}
		return;
	case 5:
		if (sel & 1) a->wmark = (uint16_t)((a->wmark & 0xff00) | (v & 0xff));
		if (sel & 2) a->wmark = (uint16_t)((a->wmark & 0x00ff) | (v & 0xff00));
		return;
	case 8:
		if (sel & 1) a->mixvol = (uint8_t)v;
		if (sel & 2) a->spdif_fs = (v >> 8) & 15;
		return;
	case 12: if (sel & 1) a->mixpos_sel = v & 7; return;
	case 14:
		if (sel & 1) a->volume = (uint16_t)((a->volume & 0x100) | (v & 0xff));
		if (sel & 2) a->volume = (uint16_t)((a->volume & 0xff) | (v & 0x100));
		return;
	case 15:
		if (sel & 1) a->step = (uint16_t)((a->step & 0xff00) | (v & 0xff));
		if (sel & 2) a->step = (uint16_t)((a->step & 0x00ff) | (v & 0xff00));
		if (sel & 4) a->mod = (uint16_t)((a->mod & 0xff00) | ((v >> 16) & 0xff));
		if (sel & 8) a->mod = (uint16_t)((a->mod & 0x00ff) | ((v >> 16) & 0xff00));
		return;
	}
}

static void audio_reset(zsys_t *s) {
	zsys_audio_t *a = &s->audio;
	memset(a, 0, offsetof(zsys_audio_t, ring));	// the host's ring is kept
	a->rate = 17;
	a->step = 147;					// 44100 Hz: CLK_HZ 147 / (128 1250)
	a->mod = 1250;
	a->mixvol = 128;
	a->volume = 128;				// 50%, the system's default
	a->wmark = ZSYS_AUDIO_FIFO / 2;
}

// =====================================================================
// the bus
// =====================================================================

uint32_t zdev_read(zsys_t *s, uint32_t pa, bool *ok) {
	*ok = true;
	uint32_t r = (pa & 0x7f) >> 2;
	switch (pa >> 28) {
	case 0x1: return flashw_read(s, pa & 0x1fc);
	case 0xc: return hid_read(s, (pa >> 5) & 1, (pa >> 2) & 7);
	case 0x7:
		if ((pa & 0x0f00u) == 0x200) return socctl_read(s, (pa & 0xff) >> 2);
		if ((pa & 0x0f00u) == 0x300) return rtc_read(s, (pa & 0x1f) >> 2);
		if ((pa & 0x0f00u) == 0x500 && s->audio_on) return audio_read(s, (pa & 0xff) >> 2);
		*ok = false;
		return 0;
	case 0xa: return raster_read(s, r & 31);
	case 0xb: return spisd_read(s, r & 3);
	case 0xd: return blit_read(s, r & 31);
	}
	*ok = false;
	return 0;
}

bool zdev_write(zsys_t *s, uint32_t pa, uint32_t v, uint32_t sel) {
	uint32_t r = (pa & 0x7f) >> 2;
	switch (pa >> 28) {
	case 0x1: flashw_write(s, pa & 0x1fc, v, sel); return true;
	case 0xc: hid_write(s, (pa >> 5) & 1, (pa >> 2) & 7, v); return true;
	case 0x7:
		if ((pa & 0x0f00u) == 0x200) { socctl_write(s, (pa & 0xff) >> 2, v, sel); return true; }
		if ((pa & 0x0f00u) == 0x300) { rtc_write(s, (pa & 0x1f) >> 2, v, sel); return true; }
		if ((pa & 0x0f00u) == 0x500 && s->audio_on) { audio_write(s, (pa & 0xff) >> 2, v, sel); return true; }
		return true;				// csrs.v: acked, ignored
	case 0xa: raster_write(s, r & 31, v); return true;
	case 0xb: spisd_write(s, r & 3, v); return true;
	case 0xd: blit_write(s, r & 31, v); return true;
	}
	return false;
}

// =====================================================================
// power-on
// =====================================================================

// gpu_video.v's cursor shapes from configuration on (docs/cursor.md)
static const struct { uint8_t row; uint16_t bits; } cursor_init[] = {
	{ 6, 0x0440 }, { 7, 0x0280 }, { 8, 0x0100 }, { 9, 0x0280 }, { 10, 0x0440 },			// slot 0: X
	{ 22, 0x07c0 }, { 23, 0x0200 }, { 24, 0x0100 }, { 25, 0x0080 }, { 26, 0x07c0 },		// slot 1: Z
	{ 36, 0x1010 }, { 37, 0x0820 }, { 38, 0x0440 }, { 39, 0x0280 }, { 40, 0x0100 },		// slot 2: 9x9 X
	{ 41, 0x0280 }, { 42, 0x0440 }, { 43, 0x0820 }, { 44, 0x1010 },
	{ 52, 0x1ff0 }, { 53, 0x0800 }, { 54, 0x0400 }, { 55, 0x0200 }, { 56, 0x0100 },		// slot 3: 9x9 Z
	{ 57, 0x0080 }, { 58, 0x0040 }, { 59, 0x0020 }, { 60, 0x1ff0 },
};

void zdev_reset(zsys_t *s) {
	uint64_t blits = s->blit.blits, lines = s->raster.lines;	// counts, for --stats: kept
	memset(&s->blit, 0, sizeof(s->blit));
	s->blit.ctrl = CTRL_CLIP;			// clipping on by default
	s->blit.clip_x1 = 640;
	s->blit.clip_y1 = 480;
	s->blit.fg = 1;
	memset(&s->raster, 0, sizeof(s->raster));
	s->raster.cx1 = 639;
	s->raster.cy1 = 479;
	s->blit.blits = blits;
	s->raster.lines = lines;
	s->soc_ctrl = 0;
	s->video_mode = 0;				// white on black, as Lakritz comes up
	s->video_bright = 7;
	s->game = 0;
	s->view = 0;
	s->col_en = s->col_chunky = false;
	s->col_np = 0;
	s->col_off = 0;
	s->win_en = s->win_desk = false;
	s->win_x0 = s->win_y0 = s->win_x1 = s->win_y1 = 0;
	s->video_gen++;
	// the palette is block RAM: its power-on contents (EGA's sixteen,
	// repeated), and no reset after that
	if (!s->palette_set) {
		static const uint16_t ega[16] = { 0x000, 0x00a, 0x0a0, 0x0aa, 0xa00, 0xa0a, 0xa50, 0xaaa,
			0x555, 0x55f, 0x5f5, 0x5ff, 0xf55, 0xf5f, 0xff5, 0xfff };
		for (int i = 0; i < 256; i++) s->palette[i] = ega[i % 16];
		s->palette_set = true;
	}
	s->cslots = 1u << 2;				// normal slot 0, busy slot 1
	s->caddr = 0;
	memset(s->cursor_rows, 0, sizeof(s->cursor_rows));
	for (size_t i = 0; i < sizeof(cursor_init) / sizeof(cursor_init[0]); i++)
		s->cursor_rows[cursor_init[i].row] = cursor_init[i].bits;
	// the USB ports: a keyboard on 0 and a mouse on 1, plugged in, the
	// pointer at (0, 0) as after the board's reset; their arrival is a
	// report on each (rtl/usb_hid.v raises it on a type change)
	memset(s->hid, 0, sizeof(s->hid));
	s->hid[0].typ = s->kbd_unplugged ? 0 : 1;
	s->vmouse = 0;
	s->vwheel = 0;
	s->hid[1].typ = 2;
	s->hid[0].speed = s->hid[1].speed = 16;
	s->irq_pending |= IRQ_HID0 | IRQ_HID1;
	// the flash's write path: idle, disarmed
	memset(&s->flashw, 0, sizeof(s->flashw));
	audio_reset(s);
	zt2_reset();
	// the RTC counts from 0 and knows it does not know the time --
	// unless the front end gave it the host's clock
	s->rtc_cycles = s->cycles;
	s->rtc_sec = s->rtc_sub_pre = 0;
	s->rtc_valid = false;
	s->rtc_tz = 0;
	s->rtc_sub_staged = 0;
	if (s->rtc_host) zsys_rtc_from_host(s);
	// the card: as just powered, its image kept
	zsys_sd_t *d = &s->sd;
	d->cs = d->app = false;
	d->idle = true;
	d->cmd_n = d->wn = 0;
	d->mode = SD_NONE;
	sd_q_clear(d);
	memset(s->dirty, 0xff, sizeof(s->dirty));
}

// =====================================================================
// the picture
// =====================================================================

void zsys_dirty_take(zsys_t *s, uint32_t rows[ZSYS_ROWS / 32]) {
	memcpy(rows, s->dirty, sizeof(s->dirty));
	memset(s->dirty, 0, sizeof(s->dirty));
}

void zsys_frame_dirty(zsys_t *s, uint32_t rows[ZSYS_ROWS / 32]) {
	zsys_dirty_take(s, rows);
	uint32_t key = s->video_gen * 2654435761u ^ (uint32_t)s->video_mode << 24 ^ (s->soc_ctrl & 7) << 20 ^
		(uint32_t)s->cslots << 12;
	if (key != s->shown_key || (s->game & 1) || s->win_en) memset(rows, 0xff, ZSYS_ROWS / 8);
	s->shown_key = key;
}

// The VRAM bit at framebuffer (x, y), rows past 479 being the second sheet.
static inline int fb_bit(const zsys_t *s, uint32_t x, uint32_t y) {
	uint32_t o = (y * ZSYS_STRIDE + (x >> 3)) & (s->vram_size - 1);
	return (s->vram[o] >> (x & 7)) & 1;
}

static inline uint32_t rgb444(uint16_t c) {	// each channel's four bits doubled to eight
	uint32_t r = (c >> 8) & 15, g = (c >> 4) & 15, b = c & 15;
	return 0xff000000u | (r * 17) << 16 | (g * 17) << 8 | (b * 17);
}

// The cursor's bit at framebuffer (fx, fy): the shape row of the slot
// CTRL picks, the cell's centre at the pointer (gpu_video.v, as it is
// compared against the framebuffer coordinates it scans).
static inline int cursor_at(const zsys_t *s, int fx, int fy, bool game) {
	if (s->soc_ctrl & 2) return 0;
	int px, py;
	zsys_pointer(s, &px, &py);
	if (game && (s->soc_ctrl & 4)) { px >>= 1; py >>= 1; }	// CURSOR_SCREEN: half, in game mode
	int dx = fx - px + 8, dy = fy - py + 8;
	if (dx < 0 || dx >= 16 || dy < 0 || dy >= 16) return 0;
	uint32_t slot = (s->soc_ctrl & 1) ? (s->cslots >> 2) & 3 : s->cslots & 3;
	return (s->cursor_rows[slot * 16 + dy] >> dx) & 1;
}

// One row of the picture, 0xAARRGGBB: the desktop with its phosphor (and
// the colour window or desktop colour over it), or game mode's 320x240
// viewport doubled, monochrome, in planes through the palette, or
// chunky -- what gpu_video.v sends, the frame's payload taken as it
// stands now.
void zsys_render_row(const zsys_t *s, int y, uint32_t *argb, bool cursor) {
	// the phosphor's ink at full scale (the board's DAC levels are
	// gpu_video.v's; a screen is for looking at): white, amber
	// (0xD2:0x9D), green; paper is white's, inverted
	static const uint32_t ink[4] = { 0xffffffffu, 0xffffbf00u, 0xff00ff00u, 0xffffffffu };
	uint32_t on = ink[s->video_mode & 3], off = 0xff000000u;
	if ((s->video_mode & 3) == 3) { off = on; on = 0xff000000u; }
	bool game = s->game & 1, wrap = (s->game >> 1) & 1;

	if (game) {
		uint32_t vx = s->view & 0x3ff, vy = (s->view >> 16) & 0x3ff;
		if (!wrap) { if (vx > 320) vx = 320; if (vy > 240) vy = 240; }	// clamped at the frame
		uint32_t fy = (vy + (uint32_t)y / 2) % 480;
		if (s->col_en && s->col_chunky) {	// a byte a pixel, four 80-pixel segments a line
			uint32_t r = (uint32_t)y / 2 + vy;
			for (int x = 0; x < ZSYS_SCREEN_W; x++) {
				uint32_t p = (uint32_t)x / 2, seg = p / 80;
				uint32_t row = (seg & 1 ? 240 : 0) + (seg & 2 ? 480 : 0) + r % 240;
				uint32_t idx = s->vram[(row * ZSYS_STRIDE + p % 80) & (s->vram_size - 1)];
				if (cursor && cursor_at(s, (int)p, (int)(r % 240), true)) idx ^= 0xff;
				argb[x] = rgb444(s->palette[idx]);
			}
			return;
		}
		uint32_t np = s->col_en ? s->col_np + 1u : 1u;
		for (int x = 0; x < ZSYS_SCREEN_W; x++) {
			uint32_t fx = (vx + (uint32_t)x / 2) % 640, idx = 0;
			for (uint32_t k = 0; k < np; k++) {
				uint32_t j = k & 3, px = fx, py = fy;
				if (j) {
					uint32_t o = (s->col_off >> (2 * (j - 1))) & 3;
					if (o & 1) px = (px + 320) % 640;
					if (o & 2) py = (py + 240) % 480;
				}
				if (k >= 4) py += 480;		// planes 4..7: the second sheet
				idx |= (uint32_t)fb_bit(s, px, py) << k;
			}
			int c = cursor && cursor_at(s, (int)fx, (int)fy, true);
			if (s->col_en) argb[x] = rgb444(s->palette[c ? idx ^ ((1u << np) - 1) : idx]);
			else argb[x] = (idx ^ (uint32_t)c) ? on : off;
		}
		return;
	}

	const uint8_t *row = s->vram + y * ZSYS_STRIDE;
	bool win = s->win_en && (uint32_t)y >= s->win_y0 && (uint32_t)y < s->win_y1;
	for (int x = 0; x < ZSYS_SCREEN_W; x++) {
		int b = (row[x >> 3] >> (x & 7)) & 1;
		int c = cursor && cursor_at(s, x, y, false);
		if (win && (uint32_t)x >= s->win_x0 && (uint32_t)x < s->win_x1) {
			// the window's planes: sheet 1, at the pixel and folded by 320 and 240
			uint32_t idx = (uint32_t)b;
			for (uint32_t k = 0; k < 4; k++) {
				uint32_t px = (uint32_t)x + (k & 1 ? 320 : 0), py = (uint32_t)y + (k & 2 ? 240 : 0);
				idx |= (uint32_t)fb_bit(s, px % 640, 480 + py % 480) << (4 + k);
			}
			if (s->win_desk) {			// the exact path: only an accent pixel goes through the palette
				if ((idx & 0x10) && !c) { argb[x] = rgb444(s->palette[0x10 | (idx & 1)]); continue; }
			} else {
				if (c) idx ^= 0xf1;		// the pointer inverts every plane in use
				argb[x] = rgb444(s->palette[idx]);
				continue;
			}
		}
		argb[x] = (b ^ c) ? on : off;
	}
}

void zsys_screen_rgb(const zsys_t *s, uint8_t *rgb, bool cursor) {
	uint32_t line[ZSYS_SCREEN_W];
	for (int y = 0; y < ZSYS_SCREEN_H; y++) {
		zsys_render_row(s, y, line, cursor);
		for (int x = 0; x < ZSYS_SCREEN_W; x++) {
			uint8_t *o = rgb + (y * ZSYS_SCREEN_W + x) * 3;
			o[0] = (uint8_t)(line[x] >> 16); o[1] = (uint8_t)(line[x] >> 8); o[2] = (uint8_t)line[x];
		}
	}
}

// -- a PNG, uncompressed: stored deflate blocks, so no zlib --

static uint32_t crc_table[256];
static uint32_t crc32_of(uint32_t c, const uint8_t *p, size_t n) {
	if (!crc_table[1])
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t v = i;
			for (int k = 0; k < 8; k++) v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
			crc_table[i] = v;
		}
	c = ~c;
	while (n--) c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
	return ~c;
}

static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static bool chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
	uint8_t h[8];
	be32(h, n);
	memcpy(h + 4, type, 4);
	uint32_t c = crc32_of(0, h + 4, 4);
	c = crc32_of(c, data, n);
	uint8_t t[4];
	be32(t, c);
	return fwrite(h, 1, 8, f) == 8 && (n == 0 || fwrite(data, 1, n, f) == n) && fwrite(t, 1, 4, f) == 4;
}

// 1 bit a pixel, indexed: the framebuffer's bit (with the cursor's
// inversion) is the index, the palette the phosphor's two colours -- so
// 38KB stored, and a reader can tell a lit pixel whatever the phosphor.
static bool screenshot_rgb(const zsys_t *s, const char *path, bool cursor);

bool zsys_screenshot(const zsys_t *s, const char *path, bool cursor) {
	if ((s->game & 1) || s->win_en) return screenshot_rgb(s, path, cursor);	// colours: 8-bit RGB
	const uint32_t W = ZSYS_SCREEN_W, H = ZSYS_SCREEN_H, line = W / 8 + 1, raw_n = line * H;
	uint8_t *rgb = malloc(W * H * 3), *raw = malloc(raw_n);
	uint32_t blocks = (raw_n + 65534) / 65535;
	uint8_t *z = malloc(raw_n + blocks * 5 + 6);
	if (!rgb || !raw || !z) { free(rgb); free(raw); free(z); return false; }
	zsys_screen_rgb(s, rgb, cursor);
	bool paper = (s->video_mode & 3) == 3;
	uint8_t plte[6] = { 0, 0, 0, 0, 0, 0 };
	if (paper) { plte[0] = plte[1] = plte[2] = 255; }
	for (uint32_t i = 0; i < W * H; i++) {
		const uint8_t *px = rgb + i * 3;
		bool lit = (px[0] | px[1] | px[2]) != 0;
		if (lit && !paper) memcpy(plte + 3, px, 3);
		if (paper) lit = !lit;
		uint32_t y = i / W, x = i % W;
		uint8_t *b = raw + y * line + 1 + x / 8;
		if (x % 8 == 0) *b = 0;
		if (lit) *b |= 0x80 >> (x % 8);
	}
	if (!paper && !(plte[3] | plte[4] | plte[5])) plte[3] = plte[4] = plte[5] = 255;
	for (uint32_t y = 0; y < H; y++) raw[y * line] = 0;	// filter: none
	uint32_t zn = 0, a = 1, b = 0;
	z[zn++] = 0x78; z[zn++] = 0x01;
	for (uint32_t o = 0; o < raw_n; ) {
		uint32_t n = raw_n - o > 65535 ? 65535 : raw_n - o;
		z[zn++] = o + n == raw_n;
		z[zn++] = n & 0xff; z[zn++] = n >> 8; z[zn++] = ~n & 0xff; z[zn++] = (~n >> 8) & 0xff;
		memcpy(z + zn, raw + o, n);
		zn += n;
		o += n;
	}
	for (uint32_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
	be32(z + zn, b << 16 | a);
	zn += 4;
	uint8_t ihdr[13];
	be32(ihdr, W); be32(ihdr + 4, H);
	ihdr[8] = 1; ihdr[9] = 3; ihdr[10] = ihdr[11] = ihdr[12] = 0;	// 1-bit, indexed
	FILE *f = fopen(path, "wb");
	bool ok = f && fwrite("\x89PNG\r\n\x1a\n", 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) &&
		chunk(f, "PLTE", plte, 6) && chunk(f, "IDAT", z, zn) && chunk(f, "IEND", NULL, 0);
	if (f && fclose(f)) ok = false;
	free(rgb); free(raw); free(z);
	return ok;
}

// 8-bit RGB, stored: game mode, colour, the colour window.
static bool screenshot_rgb(const zsys_t *s, const char *path, bool cursor) {
	const uint32_t W = ZSYS_SCREEN_W, H = ZSYS_SCREEN_H, line = W * 3 + 1, raw_n = line * H;
	uint8_t *rgb = malloc(W * H * 3), *raw = malloc(raw_n);
	uint32_t blocks = (raw_n + 65534) / 65535;
	uint8_t *z = malloc(raw_n + blocks * 5 + 6);
	if (!rgb || !raw || !z) { free(rgb); free(raw); free(z); return false; }
	zsys_screen_rgb(s, rgb, cursor);
	for (uint32_t y = 0; y < H; y++) {
		raw[y * line] = 0;
		memcpy(raw + y * line + 1, rgb + y * W * 3, W * 3);
	}
	uint32_t zn = 0, a = 1, b = 0;
	z[zn++] = 0x78; z[zn++] = 0x01;
	for (uint32_t o = 0; o < raw_n; ) {
		uint32_t n = raw_n - o > 65535 ? 65535 : raw_n - o;
		z[zn++] = o + n == raw_n;
		z[zn++] = n & 0xff; z[zn++] = n >> 8; z[zn++] = ~n & 0xff; z[zn++] = (~n >> 8) & 0xff;
		memcpy(z + zn, raw + o, n);
		zn += n;
		o += n;
	}
	for (uint32_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
	be32(z + zn, b << 16 | a);
	zn += 4;
	uint8_t ihdr[13];
	be32(ihdr, W); be32(ihdr + 4, H);
	ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
	FILE *f = fopen(path, "wb");
	bool ok = f && fwrite("\x89PNG\r\n\x1a\n", 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) &&
		chunk(f, "IDAT", z, zn) && chunk(f, "IEND", NULL, 0);
	if (f && fclose(f)) ok = false;
	free(rgb); free(raw); free(z);
	return ok;
}
