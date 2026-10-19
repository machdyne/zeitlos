/*
 * Benchmark: zcanvas + zcfont on the CPU, run under sim/zsim-headless,
 * which counts retired instructions (docs/zcanvas.md, "Speed").
 *
 *   make -C sw/common/tests/bench_zcanvas
 *   sim/zsim-headless -n 0 sw/common/tests/bench_zcanvas/bench_zcanvas.bin
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "../../zeitlos.h"
#include "../../zcanvas.h"
#include "../../zcfont.h"

extern const uint8_t f_sans[], f_serif[], f_m9x15[];
extern const uint32_t f_sans_len, f_serif_len, f_m9x15_len;

static uint8_t scratch[48 * 1024];
static uint8_t cache_mem[96 * 1024];
static uint32_t card_bits[342][16];
static uint8_t band[64][2048];
static zc_font_t sans, serif, m9;
static zc_pt_t pts[512], wpts[2048];
static uint16_t subs[64], wsubs[256];
static zc_path_t path, work;

static inline uint32_t instret(void) {
	uint32_t v;
	__asm__ volatile ("rdinstret %0" : "=r"(v));
	return v;
}

static void card(zc_target_t *t, const zc_xf_t *xf) {
	static const char *body =
		"Zeitlos draws this text with public-domain Hershey stroke fonts. "
		"They scale to any size on the screen and on paper. A card holds "
		"a title, a few lines of body text, and some shapes and images.";
	zc_paint_t black = zc_black(), mid = zc_shade(5);
	zc_style_t title = { &serif, ZC_I(40), 0, 0, 0, 0, 0 };
	zc_style_t bs = { &sans, ZC_I(16), 0, 0, 0, 0, 0 };
	zc_style_t cap = { &m9, ZC_I(15), 0, 0, 0, 0, 0 };
	zc_line_t lines[12];
	zc_fix line;
	int n, i;
	uint32_t t0 = instret(), t1, t2, t3, t4, t5;
	zc_path_set_xf(&path, xf);
	zc_path_reset(&path);
	zc_path_rrect(&path, ZC_I(300), ZC_I(200), ZC_I(180), ZC_I(110), ZC_I(10));
	zc_fill(t, &path, ZC_NONZERO, &mid);
	zc_frame_rrect(t, &path, ZC_I(300), ZC_I(200), ZC_I(180), ZC_I(110), ZC_I(10), ZC_I(2), &black);
	zc_frame_ellipse(t, &path, ZC_I(90), ZC_I(220), ZC_I(120), ZC_I(80), ZC_I(3), &black);
	zc_path_reset(&path);
	zc_path_move(&path, ZC_I(20), ZC_I(320));
	zc_path_line(&path, ZC_I(250), ZC_I(300));
	zc_stroke(t, &path, ZC_I(2), false, &work, &black);
	t1 = instret();
	zc_text_draw(t, xf, &title, ZC_I(20), ZC_I(50), "Slides on Zeitlos", 17, &black);
	t2 = instret();
	zc_text_metrics(&bs, NULL, NULL, &line);
	n = zc_text_wrap(&bs, body, (uint32_t)strlen(body), ZC_I(470), lines, 12, NULL);
	t3 = instret();
	for (i = 0; i < n; i++)
		zc_text_draw(t, xf, &bs, ZC_I(20), ZC_I(90) + line * i, body + lines[i].start, lines[i].len, &black);
	t4 = instret();
	zc_text_draw(t, xf, &cap, ZC_I(20), ZC_I(330), "misc-fixed 9x15 caption line", 28, &black);
	t5 = instret();
	if (xf->a == ZC_XF_ONE)
		printf("  shapes %lu title %lu wrap %lu body %lu caption %lu\n",
			(unsigned long)(t1 - t0), (unsigned long)(t2 - t1), (unsigned long)(t3 - t2),
			(unsigned long)(t4 - t3), (unsigned long)(t5 - t4));
}

int main(void) {
	zc_target_t t;
	zc_xf_t one = zc_xf_scale(ZC_XF_ONE, 0, 0);
	uint32_t a, b, c, d, e;
	int bnd;
	zc_scratch(scratch, sizeof(scratch));
	zc_path_init(&path, pts, 512, subs, 64);
	zc_path_init(&work, wpts, 2048, wsubs, 256);
	zc_font_open(&sans, f_sans, f_sans_len, 0);
	zc_font_open(&serif, f_serif, f_serif_len, 0);
	zc_font_open(&m9, f_m9x15, f_m9x15_len, 0);

	zc_target_bits(&t, &card_bits[0][0], 16, 512, 342);
#ifdef ONLY_CACHED
	zc_text_cache(cache_mem, sizeof(cache_mem));
	card(&t, &one);
	for (bnd = 0; bnd < 10; bnd++) card(&t, &one);
	return 0;
#endif
	a = instret();
	memset(card_bits, 0xFF, sizeof(card_bits));
	zc_text_cache(0, 0);
	card(&t, &one);
	b = instret();
	zc_text_cache(cache_mem, sizeof(cache_mem));
	memset(card_bits, 0xFF, sizeof(card_bits));
	card(&t, &one);
	c = instret();
	memset(card_bits, 0xFF, sizeof(card_bits));
	card(&t, &one);
	d = instret();
	printf("card 512x342, no cache:      %lu insns\n", (unsigned long)(b - a));
	printf("card, filling the cache:     %lu insns\n", (unsigned long)(c - b));
	printf("card, from the cache:        %lu insns\n", (unsigned long)(d - c));

	// one 64-row band of the card printed at 4x (2048 px wide, ~300 dpi on a 7 in card)
	{
		zc_xf_t x4 = zc_xf_scale(4 * ZC_XF_ONE, 0, 0);
		uint32_t tot = 0;
		for (bnd = 0; bnd < 342 * 4; bnd += 64) {
			zc_target_gray(&t, &band[0][0], 2048, 2048, 64);
			zc_target_origin(&t, 0, bnd);
			t.pat_scale = 4;
			memset(band, 255, sizeof(band));
			e = instret();
			card(&t, &x4);
			tot += instret() - e;
		}
		printf("card at 4x in 64-row bands:  %lu insns (%d bands)\n", (unsigned long)tot, (342 * 4 + 63) / 64);
	}
	return 0;
}
