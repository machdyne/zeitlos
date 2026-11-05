/*
 * A host model of zgl's pages against T2 and the scanout (zgl_chunky.c,
 * docs/zgl_t2.md "Pages"): the real paging code, its registers as calls
 * (mk.py), T2 as frames with their strips over time and gpu_t2.v's VBASE
 * queue, the scanout taking CKBASE at each frame boundary, and a game
 * loop as mausrun3d's. Checks that no picture is written while it is
 * shown, that every frame shown was finished, and that frames go out in
 * order; prints how long frames stayed up.
 *
 *   make -C sw/common/zgl/tests/pages      (W=1: glFinish() each frame)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "stubs.h"
#include "../../zgl_t2.h"

void zglc_poll(void); void zglc_present(void); void zglc_finish(void);
void zglc_picture(int32_t, int32_t, int32_t, int32_t); int zglc_open(uint8_t);

#define P 800000ull                 /* a refresh, 48 MHz / 60 */
#define G 60000ull                  /* T2's geometry after the last word */
#define R 450000ull                 /* raster and output of a frame */
#define NSTRIP 20
static uint64_t now;
static uint32_t rnd_s = 1;
static uint32_t rnd(void) { rnd_s ^= rnd_s << 13; rnd_s ^= rnd_s >> 17; rnd_s ^= rnd_s << 5; return rnd_s; }

volatile uint32_t *z_chunky_mem;
uint32_t z_chunky_mem_phys;
#define MAXW 200000
static struct { uint64_t t; uint32_t v; } ckw[MAXW]; static int nck;
void z_chunky_set_pic(void *pic, bool show) {
	z_chunky_mem = pic;
	z_chunky_mem_phys = (uint32_t)(uintptr_t)pic;
	if (show) { ckw[nck].t = now; ckw[nck].v = z_chunky_mem_phys; nck++; }
}
uint32_t z_uptime_ticks(void) { now += 20; return (uint32_t)(now / 65536); }
volatile uint32_t *z_chunky_seg(uint32_t y, uint32_t s) { (void)y; (void)s; return 0; }
uint32_t sim_frame(void) { now += 30; return (uint32_t)(now / P); }

/* T2 */
#define MAXF 20000
static struct { uint64_t end, first, done; uint32_t page; int popped; } fr[MAXF];
static int nf;                       /* frames sent (END written) */
static struct { uint64_t t; uint32_t v; } vq[4]; static int vq_n;
static uint32_t vb_cur = 0x20000000u, vb_last = 0x20000000u;
static uint64_t fifo_from;           /* RESET: frames before are gone */

static void t2_advance(void) {
	int f;
	for (f = 0; f < nf; f++) {
		if (fr[f].popped || fr[f].first > now) continue;
		/* the first strip goes out: the next queued VBASE written before it */
		if (vq_n > 0 && vq[0].t <= fr[f].first) {
			vb_cur = vq[0].v;
			memmove(&vq[0], &vq[1], sizeof(vq[0]) * 3); vq_n--;
		}
		fr[f].page = vb_cur;
		fr[f].popped = 1;
	}
}
static uint32_t strips_now(void) {
	uint32_t n = 0; int f;
	for (f = 0; f < nf; f++) {
		if (fr[f].first > now) continue;
		if (fr[f].done <= now) n += NSTRIP;
		else n += (uint32_t)((now - fr[f].first) * NSTRIP / R);
	}
	return n;
}
static bool busy(void) { return nf > 0 && fr[nf - 1].done > now; }
uint32_t gtri_rd(uint32_t w) {
	now += 30;
	t2_advance();
	switch (w) {
	case Z_GTRI_W_STATUS: return busy() ? 1u : 0u;
	case Z_GTRI_W_STRIPS: return strips_now();
	case Z_GTRI_W_ID: return T2_ID;
	case Z_GTRI_W_CAPS: return 3u | 13u << 8;
	case Z_GTRI_W_VBASE: return vb_last;
	default: return 0;
	}
}
void gtri_wr(uint32_t w, uint32_t v) {
	now += 10;
	t2_advance();
	if (w == Z_GTRI_W_VBASE) {
		vb_last = v;
		if (vq_n == 4) vq_n--;
		vq[vq_n].t = now; vq[vq_n].v = v; vq_n++;
	} else if (w == Z_GTRI_W_RESET) {
		vq_n = 0; vb_cur = vb_last; fifo_from = now;
	}
}
/* the frame's END written: when T2 takes it up */
static void t2_end(void) {
	uint64_t start = now, prev = nf ? fr[nf - 1].done : 0;
	if (prev > start) start = prev;
	fr[nf].end = now;
	fr[nf].first = start + G;
	fr[nf].done = fr[nf].first + R;
	fr[nf].popped = 0;
	nf++;
}

int main(int argc, char **argv) {
	int w_mode = argc > 1 && atoi(argv[1]);
	int frames = 3000, k, f, bad_write = 0, bad_order = 0, bad_unfinished = 0;
	static uint8_t pic0[Z_CHUNKY_WORDS * 4 + 16];
	int hist[8] = {0};
	z_chunky_set_pic(pic0, true);
	zglc_open(0);
	for (k = 0; k < frames; k++) {
		uint64_t s;
		now += 50000 + rnd() % 100000;                    /* the game */
		zglc_picture(0, 0, 319, 239);
		s = 300000 + rnd() % 900000;                      /* the stream */
		while (s > 0) {
			uint64_t d = s > 30000 ? 30000 : s;
			now += d; s -= d;
			zglc_poll();                              /* a list sent */
			/* the FIFO: no more than its 2,048 words ahead of T2 */
			if (nf && fr[nf - 1].done > now + 100000) now = fr[nf - 1].done - 100000;
		}
		zglc_poll();
		t2_end();
		if (w_mode) zglc_finish();
		zglc_present();
		now = (now / P + 1) * P + 1000;                   /* ar_frame: the boundary */
	}
	/* the scanout: CKBASE at each boundary; what each page held */
	{
		uint64_t end = now, b;
		int last_shown = -1, cur_len = 0;
		for (b = P; b + P < end; b += P) {
			uint32_t pg = 0; int i, shown = -1;
			for (i = 0; i < nck && ckw[i].t <= b; i++) pg = ckw[i].v;
			/* the frame in that page at b: the last whose writing began before b */
			for (f = 0; f < nf; f++) {
				if (!fr[f].popped || fr[f].page != pg) continue;
				if (fr[f].first < b + P && fr[f].done > b) {
					/* written while shown (or not finished when taken) */
					if (fr[f].first <= b) bad_unfinished++; else bad_write++;
				}
				if (fr[f].done <= b) shown = f;
			}
			if (shown < last_shown) bad_order++;
			if (shown == last_shown) cur_len++;
			else { if (last_shown >= 0) hist[cur_len < 7 ? cur_len : 7]++; cur_len = 1; }
			last_shown = shown;
		}
		printf("pages model (%s): %d frames, %d refreshes; written while shown %d, shown unfinished %d, out of order %d\n",
			w_mode ? "W: glFinish each frame" : "not waiting", frames, (int)(end / P),
			bad_write, bad_unfinished, bad_order);
		printf("  frames up for 1..6+ refreshes: %d %d %d %d %d %d\n",
			hist[1], hist[2], hist[3], hist[4], hist[5], hist[6] + hist[7]);
		return (bad_write || bad_order || bad_unfinished) ? 1 : 0;
	}
}
