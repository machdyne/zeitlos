/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zgl_chunky.c -- ZGL_FRAMES into chunky VRAM (zgl_desc_t.flags
 * ZGL_CHUNKY; docs/zgl.md, "Chunky"): by T2 where the bitstream has it
 * (rtl/gpu/gpu_t2.v, docs/zgl_t2.md), else by the software strips, a
 * byte a pixel straight into VRAM -- no conversion to planes. The
 * program has chunky game mode on itself.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "zgl.h"
#include "zgl_int.h"
#include "../zsoc.h"
#include "../zeitlos.h"
#include "zgl_t2.h"

static bool use_engine;
static uint32_t vbase_now;
static uint32_t ctrl_now;
static void pg_reset(void);

/* 2: T2 is here (its ID), and not ZGL_NO_ENGINE; 0: the software */
int zglc_open(uint8_t flags) {
	int kind = 0;
	if (!(flags & ZGL_NO_ENGINE) && *z_gtri_reg(Z_GTRI_W_ID) == T2_ID) kind = 2;
	use_engine = kind != 0;
	if (use_engine) {
		*z_gtri_reg(Z_GTRI_W_RESET) = 1;
		vbase_now = z_chunky_phys();
		*z_gtri_reg(Z_GTRI_W_VBASE) = vbase_now;
		/* each frame starts going out in vertical blanking */
		ctrl_now = Z_GTRI_CTRL_VSYNC;
		*z_gtri_reg(Z_GTRI_W_CTRL) = ctrl_now;
	}
	pg_reset();
	return kind;
}

/* -- pages: T2 never draws the picture being shown (`VRAM_SDRAM) ----------
 *
 * With one picture, T2 writes the one the scanout is reading, so its
 * output waits for vertical blanking (CTRL.VSYNC) to stay ahead of the
 * beam -- and the raster waits with it, its strip buffer full: half a
 * video frame a frame on average, the largest part of a T2 frame's time.
 *
 * In main memory a picture is 76,800 bytes of the program's heap, so
 * there are three: the one shown, one maybe waiting to be shown at the
 * next frame (CKBASE is taken as a frame starts: rtl/gpu/gpu_ckfetch.v),
 * and the one T2 draws, with no waiting at all. At glFinish() the
 * finished one becomes z_chunky_mem, so what the program draws after
 * it (a HUD, text) goes on that picture too; zglc_present() -- which
 * ar_frame() calls before it waits for the frame boundary -- shows it.
 * A program that never calls ar_frame() has it shown as its next frame
 * starts. Either way the picture is shown whole, HUD and all.
 *
 * Only when the frame is the whole picture. T2 then clears and draws
 * every pixel every frame, so nothing is meant to last from one frame
 * to the next and three pictures are the same as one. A smaller view
 * leaves the rest to last, and that needs the one picture: as before.
 *
 * Which pictures are busy: `disp` is being shown; `pend` was handed to
 * CKBASE in video frame `pend_frame` and is certainly shown once the
 * frame counter has moved on (the scanout took it at that boundary, or
 * one before). A second flip inside one video frame waits for that, so
 * never more than two are busy and the third is always free to draw. */
static uint8_t *pg_pic[3];
static int pg_disp = -1, pg_pend = -1, pg_draw = -1;
static uint16_t pg_pend_frame;
static bool pg_drawn;                 /* a paged frame went to T2, not yet flipped */

/* Handed over, not waited for. zglc_present() does not wait for T2: the
 * frame it is given becomes `pg_q`, still being drawn, and is shown
 * once T2's STRIPS (strips written out since RESET) passes `pg_q_end`
 * -- while the program steps its game and streams the next frame into
 * the third picture. Every paged frame is the whole picture, so the
 * same number of strips, `st_per`, learned from the first frames,
 * which go the old way: wait until T2 is idle, then show. `st_sync`:
 * `st_end` is STRIPS once every frame handed over is out (lost when a
 * frame that is not paged goes to T2, found again at the next wait).
 * STRIPS counts a strip when its last word has gone to the write queue;
 * a flip takes effect at the next frame boundary, and the scanout comes
 * to the last strip's rows a frame later, long after the queue drains. */
static int pg_q = -1;
static uint32_t pg_q_end;
static bool pg_draw_out;              /* pg_draw's frame known written out (a wait since) */
static uint32_t st_per, st_end, st_last;
static bool st_sync, st_last_ok;

static void pg_reset(void) {
	pg_q = -1;
	pg_drawn = false;
	pg_draw_out = false;
	st_sync = false;
	st_last_ok = false;
}

static uint32_t pg_strips(void) { return *z_gtri_reg(Z_GTRI_W_STRIPS); }

static uint32_t pg_phys(const uint8_t *p) {
	/* the program's memory is one run through the MTU */
	return z_chunky_mem_phys + (uint32_t)((uintptr_t)p - (uintptr_t)z_chunky_mem);
}

static int pg_index(const volatile void *p) {
	int i;
	for (i = 0; i < 3; i++) if (pg_pic[i] && (const volatile void *)pg_pic[i] == p) return i;
	return -1;
}

static uint16_t pg_frame(void) { return (uint16_t)(reg_socctl_frame & 0xffffu); }

static void pg_settle(void) {
	if (pg_pend >= 0 && pg_frame() != pg_pend_frame) { pg_disp = pg_pend; pg_pend = -1; }
}

/* picture i, finished, shown from the next video frame */
static void pg_show(int i) {
	pg_settle();
	if (pg_pend >= 0) {
		/* two frames inside one video frame: the first must be shown
		 * (or skipped) before a third picture is free */
		uint32_t t0 = z_uptime_ticks();
		/* a counter that does not move (no scanout): no frame to
		 * wait for -- as ar_frame()'s wait gives up after a tenth */
		while (pg_frame() == pg_pend_frame && z_uptime_ticks() - t0 <= Z_TICK_HZ / 10u) ;
		pg_disp = pg_pend;
		pg_pend = -1;
	}
	z_chunky_set_pic(pg_pic[i], true);
	pg_pend = i;
	pg_pend_frame = pg_frame();
}

/* the frame handed over, if T2 has written it out (or, wait: until it has) */
static void pg_retire(bool wait) {
	if (pg_q < 0) return;
	if (wait) while ((int32_t)(pg_strips() - pg_q_end) < 0) ;
	else if ((int32_t)(pg_strips() - pg_q_end) < 0) return;
	pg_show(pg_q);
	pg_q = -1;
}

/* T2 idle: everything handed over is out; STRIPS is a frame's end */
static void pg_idle(void) {
	uint32_t s;
	while (*z_gtri_reg(Z_GTRI_W_STATUS) & 1u) ;
	pg_retire(true);
	if (pg_drawn) pg_draw_out = true;
	s = pg_strips();
	st_end = s;
	st_sync = true;
}

/* the frame T2 drew, shown from the next video frame (zgl_int.h) */
void zglc_present(void) {
	pg_retire(false);
	if (!pg_drawn) return;
	pg_drawn = false;
	if (pg_draw_out && st_per) {
		/* waited for already (glFinish()): out, so shown now */
		pg_draw_out = false;
		pg_show(pg_draw);
		return;
	}
	pg_draw_out = false;
	if (!st_per || !st_sync) {
		/* the old way: wait, then show -- and learn a frame's strips,
		 * from two waits in a row a paged frame apart */
		bool was = st_last_ok;
		uint32_t before = st_last;
		pg_idle();
		if (was && st_end - before > 0 && st_end - before <= 64) st_per = st_end - before;
		st_last = st_end;
		st_last_ok = true;
		pg_show(pg_draw);
		return;
	}
	/* one frame at a time in hand: an older one still out waits now */
	pg_retire(true);
	st_end += st_per;
	pg_q = pg_draw;
	pg_q_end = st_end;
}

static bool pg_ready(void) {
	int i, k;
	if (!z_vram_sdram() || !z_chunky_mem) return false;
	k = pg_index(z_chunky_mem);
	if (k < 0) {
		/* the first time, or the program moved the picture itself:
		 * that one is picture 0, the one shown */
		pg_pic[0] = (uint8_t *)(uintptr_t)z_chunky_mem;
		pg_disp = 0; pg_pend = -1;
	}
	for (i = 1; i < 3; i++) {
		if (!pg_pic[i]) {
			uint8_t *raw = malloc(Z_CHUNKY_WORDS * 4u + 15u);
			if (!raw) return false;
			pg_pic[i] = (uint8_t *)(((uintptr_t)raw + 15u) & ~(uintptr_t)15u);
		}
	}
	return true;
}

static void set_vbase(uint32_t v, uint32_t ctrl) {
	if (v != vbase_now) {
		vbase_now = v;
		*z_gtri_reg(Z_GTRI_W_VBASE) = v;
	}
	if (ctrl != ctrl_now) {
		ctrl_now = ctrl;
		*z_gtri_reg(Z_GTRI_W_CTRL) = ctrl;
	}
}

/* T2's picture for the frame starting, the frame's rect (zgl_int.h) */
void zglc_picture(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
	int i;
	if (!use_engine) return;
	zglc_present();                     /* the last frame, if nobody showed it */
	if (x0 <= 0 && y0 <= 0 && x1 >= Z_CHUNKY_W - 1 && y1 >= Z_CHUNKY_H - 1 && pg_ready()) {
		for (;;) {
			pg_settle();
			for (i = 0; i < 3; i++) if (i != pg_disp && i != pg_pend && i != pg_q) break;
			if (i < 3) break;
			/* all three busy: shown, waiting to be shown, being drawn */
			pg_retire(true);
		}
		pg_draw = i;
		pg_drawn = true;
		pg_draw_out = false;
		set_vbase(pg_phys(pg_pic[i]), 0);
		return;
	}
	/* one picture, VSYNC: whatever was handed over is shown first, and
	 * the strips' count is not a whole number of paged frames after */
	if (pg_q >= 0) pg_idle();
	st_sync = false;
	st_last_ok = false;
	set_vbase(z_chunky_phys(), Z_GTRI_CTRL_VSYNC);
}

/* T2's texture memory (zgl_int.h): CAPS bit 0, its size in [12:8] */
uint32_t zglc_tex_room(void) {
	uint32_t c;
	if (!use_engine) return 0;
	if (*z_gtri_reg(Z_GTRI_W_ID) != T2_ID) return 0;
	c = *z_gtri_reg(Z_GTRI_W_CAPS);
	if (!(c & 1u)) return 0;
	return 1u << ((c >> 8) & 31u);
}

void zglc_tex_put(uint32_t word, uint32_t v) {
	if (!use_engine) return;
	/* the raster may be drawing with the texture memory */
	while (*z_gtri_reg(Z_GTRI_W_STATUS) & 1u) ;
	*z_gtri_reg(Z_GTRI_W_TEXADDR) = word;
	*z_gtri_reg(Z_GTRI_W_TEXDATA) = v;
}

/* a command word: the store waits while the engine's FIFO is full */
void zglc_put(uint32_t w) {
	*z_gtri_reg(Z_GTRI_W_CMD) = w;
}

volatile uint32_t *zglc_cmd(void) {
	return z_gtri_reg(Z_GTRI_W_CMD);
}

void zglc_finish(void) {
	if (!use_engine) return;
	pg_idle();
	/* the program draws on the finished page now; shown at zglc_present() */
	if (pg_drawn && z_chunky_mem != (volatile uint32_t *)pg_pic[pg_draw])
		z_chunky_set_pic(pg_pic[pg_draw], false);
}

/* A software strip's rows, columns cx0..cx1, to chunky VRAM: whole
 * words where four pixels are inside, bytes at the ends. */
void zglc_strip(int32_t y0, int rows, int32_t cx0, int32_t cx1, const uint8_t *col) {
	int r;
	for (r = 0; r < rows; r++) {
		const uint8_t *src = &col[r * 320];
		uint32_t y = (uint32_t)(y0 + r);
		int32_t x = cx0;
		while (x <= cx1) {
			uint32_t seg = (uint32_t)x / 80u, off = (uint32_t)x % 80u;
			volatile uint32_t *d = z_chunky_seg(y, seg);
			if ((x & 3) == 0 && x + 3 <= cx1) {
				/* to the end of the segment, or of the span */
				int32_t end = (int32_t)(seg * 80u + 80u) - 1;
				if (end > cx1) end = cx1;
				while (x + 3 <= end) {
					d[off >> 2] = *(const uint32_t *)&src[x];
					x += 4;
					off += 4;
				}
			} else {
				((volatile uint8_t *)d)[off] = src[x];
				x++;
			}
		}
	}
}
