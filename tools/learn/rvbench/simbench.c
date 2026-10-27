/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * tools/learn/rvbench -- learn on the board's CPU, each kind of key press marked, so
 * sim.py can say what it costs (build.sh). The page is a stand-in for the
 * board's GPU device: each operation a few register writes, as the CPU
 * sees it (learn.c's g_* functions).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lapp_int.h"
#include "ldraw.h"
#include "lfig.h"
#include "zkbd.h"
long sim_call(long n, long a, long b, long c, long d);
extern const lplat_t sim_plat;
#define W 640
#define H 480
static uint32_t bits[(W / 32) * H + 2];
static ld_page_t page;
static uint8_t mem[3u << 20];
static uint8_t scratch[96 << 10], cache[128 << 10];
static char fontbuf[600 << 10];
static long fontused;
// the board's GPU, as far as the CPU sees it: a few register writes an operation
static volatile uint32_t gpu_regs[8];
static uint32_t nops;
static void reg6(void) { for (int i = 0; i < 6; i++) gpu_regs[i] = (uint32_t)i; nops++; }
static void g_fill(ld_page_t *p, int x, int y, int w, int h, const ld_ink_t *k) { (void)p; (void)x; (void)y; (void)w; (void)h; (void)k; reg6(); }
static void g_line(ld_page_t *p, int x0, int y0, int x1, int y1, int c, const ld_clip_t *cl) { (void)p; (void)x0; (void)y0; (void)x1; (void)y1; (void)c; (void)cl; reg6(); }
static void g_blit(ld_page_t *p, const uint32_t *b, int wpl, int sx, int sy, int x, int y, int w, int h, int rop) { (void)p; (void)b; (void)wpl; (void)sx; (void)sy; (void)x; (void)y; (void)w; (void)h; (void)rop; reg6(); reg6(); }
static bool g_scroll(ld_page_t *p, int x, int y, int w, int h, int dy) { (void)p; (void)x; (void)y; (void)w; (void)h; (void)dy; reg6(); return true; }
static bool g_save(ld_page_t *p, int x, int y, int w, int h, uint32_t *bits, int wpl) { (void)p; (void)x; (void)y; for (int j = 0; j < h; j++) for (int i = 0; i < (w + 31) / 32; i++) bits[j * wpl + i] = gpu_regs[0]; return true; }
static const ld_dev_t gdev = { g_fill, g_line, g_blit, g_scroll, g_save };
static int gctx;
static void mark(const char *s) { sim_call(507, (long)s, 0, 0, 0); }
static void endm(void) { sim_call(508, 0, 0, 0, 0); uint32_t d, f; ld_glyph_stats(&d, &f); printf("  gpu ops %u, glyphs drawn %u found %u\n", (unsigned)nops, (unsigned)d, (unsigned)f); nops = 0; }
static const void *load_font(const char *file, uint32_t *len) {
	char p[256];
	const char *sub = strstr(file, ".zhf") ? "zhf/" : strstr(file, "jp12") ? "" : "zfn/";
	snprintf(p, sizeof p, "/font/%s%s", sub, file);
	long n = sim_call(509, (long)p, (long)(fontbuf + fontused), sizeof fontbuf - fontused, 0);
	if (n <= 0) return NULL;
	void *d = fontbuf + fontused;
	fontused += (n + 3) & ~3;
	*len = n;
	return d;
}
static void key(uint32_t k, const char *what) {
	la_ev_t e;
	memset(&e, 0, sizeof e);
	e.type = LA_KEY;
	e.key = k;
	mark(what);
	if (la_event(&e)) {
		int x, y, w, h;
		if (la_partial(&x, &y, &w, &h)) { printf("  part %dx%d\n", w, h); la_draw_part(&page, x, y, w, h); }
		else { printf("  full\n"); la_draw(&page); }
	}
	endm();
}
int main(void) {
	LP = &sim_plat;
	ld_fonts_load(load_font);
	ld_init(scratch, sizeof scratch, cache, sizeof cache);
	(void)bits; ld_page_dev(&page, &gdev, &gctx, 600, 400);
	la_init(mem, sizeof mem);
	la_resize(600, 420 - 20);
	mark("start (catalogue)");
	while (la_work(50)) ;
	endm();
	key('n', "name");
	la_go(SC_CATALOGUE); la_draw(&page);
	key(Z_KEY_DOWN, "catalogue: a key (a menu page)");
	key(Z_KEY_DOWN, "catalogue: another key");
	la_open_course("/opt/ark/edu-zeitlos/zeit-learn");
	mark("page: open clock step");
	la_open_lesson(3); A.step = 2; lv_enter(); la_draw(&page);
	endm();
	for (int i = 0; i < 3; i++) {
		int x, y, w, h;
		mark("clock frame");
		la_draw_figure(&page, &x, &y, &w, &h);
		endm();
		printf("  frame ms %u, still %d of %d ops\n", (unsigned)LG.ms, LG.nstill, LG.nops);
	}
	key('p', "clock P (pause)");
	key('p', "clock P (go on)");
	{ int x, y, w, h; mark("clock frame after P"); la_draw_figure(&page, &x, &y, &w, &h); endm(); }
	{
		static uint32_t pl[10 * 240 + 2];
		la_figure_colour(3);
		mark("clock colour: page"); la_draw(&page); endm();
		for (int i = 0; i < 2; i++) {
			int x, y, w, h;
			mark("clock colour frame + 3 planes");
			la_draw_figure(&page, &x, &y, &w, &h);
			for (int k = 1; k <= 3; k++) { ld_page_t pg; ld_page(&pg, pl, 320, 240); ld_clear(&pg); la_figure_plane(&pg, k); }
			endm();
		}
		key('p', "clock colour P");
		la_figure_colour(0);
	}
	mark("page: open slider step");
	A.step = 1; lv_enter(); la_draw(&page);
	endm();
	for (int i = 0; i < 3; i++) key(Z_KEY_RIGHT, "slider Right");
	mark("page: typed-answer step");
	la_open_lesson(1); A.step = 1; lv_enter(); la_draw(&page);
	endm();
	key('4', "type 4");
	key(0x7f, "type Bksp");
	{
		// dragging a point: the triangle's corner, moved a few pixels at a time
		la_resize(600, 400);
		la_open_lesson(3); A.step = 3; lv_enter(); la_draw(&page);
		la_ev_t e;
		memset(&e, 0, sizeof e);
		int px = A.fig_x + (int)(50.0 * A.fig_w / 320), py = A.fig_y + (int)(200.0 * A.fig_w / 320);
		e.type = LA_DOWN; e.x = px; e.y = py;
		la_event(&e); la_draw(&page);
		for (int i = 1; i <= 3; i++) {
			e.type = LA_MOVE; e.x = px + 6 * i; e.y = py - 4 * i;
			mark("drag a point");
			if (la_event(&e)) {
				int x, y, w, h;
				if (la_partial(&x, &y, &w, &h)) { printf("  part %dx%d\n", w, h); la_draw_part(&page, x, y, w, h); }
				else { printf("  full\n"); la_draw(&page); }
			}
			endm();
		}
	}
	{
		la_open_lesson(3); A.step = 1; lv_enter();
		la_resize(600, 200);		// (short: the step scrolls)
		la_draw(&page);
		la_ev_t e;
		memset(&e, 0, sizeof e);
		e.type = LA_WHEEL;
		e.notches = -1;
		mark("scroll a notch");
		if (la_event(&e)) {
			int x, y, w, h;
			if (la_partial(&x, &y, &w, &h)) { printf("  part %dx%d\n", w, h); la_draw_part(&page, x, y, w, h); }
			else { printf("  full\n"); la_draw(&page); }
		}
		endm();
	}
	return 0;
}
