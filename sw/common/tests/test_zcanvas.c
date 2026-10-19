/*
 * sw/common/zcanvas.c on the host: coverage, clipping, bands, strokes,
 * blits, patterns, dithering. `make -f Makefile.zcanvas` (ASan/UBSan).
 * With a directory argument it also writes zcanvas_*.pgm there, to be
 * LOOKED AT (docs/zcanvas.md, "Looking at it").
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../zcanvas.h"

static int fails, checks;

#define ck(cond, msg) do { checks++; if (!(cond)) { fails++; \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

static uint8_t scratch[64 * 1024];
static zc_pt_t pts[8192], wpts[8192];
static uint16_t subs[1024], wsubs[1024];

static zc_path_t path, work;

#define W 200
#define H 150
#define WPL ((W + 31) / 32)

static uint32_t bits[H][WPL];
static uint8_t gray[H][W];

static void bits_white(void) { memset(bits, 0xFF, sizeof(bits)); }

static int black_bits(const zc_target_t *t) {
	int x, y, n = 0;
	for (y = 0; y < t->h; y++)
		for (x = 0; x < t->w; x++)
			if (zc_get_bit(t, t->ox + x, t->oy + y) == 0) n++;
	return n;
}

static void write_pgm_bits(const char *dir, const char *name,
	const uint32_t *b, int wpl, int w, int h) {
	char p[512];
	FILE *f;
	int x, y;
	if (!dir) return;
	snprintf(p, sizeof(p), "%s/%s.pgm", dir, name);
	f = fopen(p, "wb");
	if (!f) return;
	fprintf(f, "P5 %d %d 255\n", w, h);
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			fputc(((b[y * wpl + (x >> 5)] >> (x & 31)) & 1) ? 255 : 0, f);
	fclose(f);
}

static void write_pgm_gray(const char *dir, const char *name, const uint8_t *g,
	int stride, int w, int h) {
	char p[512];
	FILE *f;
	int y;
	if (!dir) return;
	snprintf(p, sizeof(p), "%s/%s.pgm", dir, name);
	f = fopen(p, "wb");
	if (!f) return;
	fprintf(f, "P5 %d %d 255\n", w, h);
	for (y = 0; y < h; y++) fwrite(g + y * stride, 1, (size_t)w, f);
	fclose(f);
}

// a scene with every kind of primitive, drawn through `xf`
static void scene(zc_target_t *t, const zc_xf_t *xf) {
	static const uint8_t brick[8] = { 0xFF, 0x80, 0x80, 0x80, 0xFF, 0x08, 0x08, 0x08 };
	zc_paint_t black = zc_black(), mid = zc_shade(6),
		pat = zc_pattern(brick, 16, true);
	zc_path_set_xf(&path, xf);

	zc_path_reset(&path);
	zc_path_rect(&path, ZC_I(5), ZC_I(5), ZC_I(60), ZC_I(40));
	zc_fill(t, &path, ZC_NONZERO, &pat);

	zc_path_reset(&path);
	zc_path_ellipse(&path, ZC_I(110), ZC_I(40), ZC_I(35), ZC_I(25));
	zc_fill(t, &path, ZC_NONZERO, &mid);
	zc_stroke(t, &path, ZC_I(3), true, &work, &black);

	zc_path_reset(&path);
	zc_path_rrect(&path, ZC_I(20), ZC_I(70), ZC_I(70), ZC_I(50), ZC_I(12));
	zc_path_rrect(&path, ZC_I(30), ZC_I(80), ZC_I(50), ZC_I(30), ZC_I(4));
	zc_fill(t, &path, ZC_EVENODD, &black);

	zc_path_reset(&path);
	zc_path_move(&path, ZC_I(110), ZC_I(80));
	zc_path_line(&path, ZC_I(180), ZC_I(90));
	zc_path_line(&path, ZC_I(130), ZC_I(140));
	zc_path_cubic(&path, ZC_I(150), ZC_I(100), ZC_I(190), ZC_I(140), ZC_I(195), ZC_I(75));
	zc_stroke(t, &path, ZC_I(5), false, &work, &black);

	zc_path_reset(&path);
	zc_path_move(&path, ZC_I(5), ZC_I(140));
	zc_path_line(&path, ZC_I(100), ZC_I(146));
	zc_stroke(t, &path, ZC_I(1), false, &work, &black);
}

int main(int argc, char **argv) {
	const char *dir = argc > 1 ? argv[1] : NULL;
	zc_target_t t;
	zc_paint_t black = zc_black(), x = zc_xor();
	int n;

	ck(zc_scratch(scratch, sizeof(scratch)) > 3000, "scratch holds edges");
	zc_path_init(&path, pts, 8192, subs, 1024);
	zc_path_init(&work, wpts, 8192, wsubs, 1024);

	// -- rectangles: exact coverage by pixel centre --
	bits_white();
	zc_target_bits(&t, &bits[0][0], WPL, W, H);
	zc_path_rect(&path, ZC_I(10), ZC_I(10), ZC_I(20), ZC_I(30));
	zc_fill(&t, &path, ZC_NONZERO, &black);
	ck(black_bits(&t) == 600, "20x30 rect is 600 pixels");
	ck(zc_get_bit(&t, 10, 10) == 0 && zc_get_bit(&t, 29, 39) == 0, "corners in");
	ck(zc_get_bit(&t, 30, 10) == 1 && zc_get_bit(&t, 9, 10) == 1, "edges out");

	// shared edges: two rects meeting at a fractional x, XORed, cover
	// every pixel once (a pixel painted twice would flip back)
	bits_white();
	zc_path_reset(&path);
	zc_path_rect(&path, ZC_I(0), ZC_I(0), ZC_I(10) + 77, ZC_I(10));
	zc_fill(&t, &path, ZC_NONZERO, &x);
	zc_path_reset(&path);
	zc_path_rect(&path, ZC_I(10) + 77, ZC_I(0), ZC_I(20) - (ZC_I(10) + 77), ZC_I(10));
	zc_fill(&t, &path, ZC_NONZERO, &x);
	ck(black_bits(&t) == 200, "abutting rects tile without gap or overlap");

	// -- ellipse area within 2% --
	bits_white();
	zc_path_reset(&path);
	zc_path_ellipse(&path, ZC_I(100), ZC_I(75), ZC_I(60), ZC_I(40));
	zc_fill(&t, &path, ZC_NONZERO, &black);
	n = black_bits(&t);
	ck(n > 7390 && n < 7690, "ellipse area ~ pi*60*40 (7540)");

	// even-odd ring = outer - inner
	bits_white();
	zc_path_reset(&path);
	zc_path_rect(&path, ZC_I(10), ZC_I(10), ZC_I(100), ZC_I(100));
	zc_path_rect(&path, ZC_I(30), ZC_I(30), ZC_I(60), ZC_I(60));
	zc_fill(&t, &path, ZC_EVENODD, &black);
	ck(black_bits(&t) == 10000 - 3600, "even-odd frame");
	// nonzero with both clockwise fills the hole
	bits_white();
	zc_fill(&t, &path, ZC_NONZERO, &black);
	ck(black_bits(&t) == 10000, "nonzero, same direction: no hole");

	// -- strokes --
	bits_white();
	zc_path_reset(&path);
	zc_path_move(&path, ZC_I(20), ZC_I(50));
	zc_path_line(&path, ZC_I(120), ZC_I(50));
	zc_path_line(&path, ZC_I(120), ZC_I(120));
	zc_stroke(&t, &path, ZC_I(6), false, &work, &black);
	{
		int y, col = 0;
		for (y = 40; y < 60; y++) if (zc_get_bit(&t, 70, y) == 0) col++;
		ck(col == 6, "a 6-unit stroke is 6 pixels thick");
		ck(zc_get_bit(&t, 120, 50) == 0 && zc_get_bit(&t, 122, 48) == 0,
			"the join is filled");
		ck(zc_get_bit(&t, 16, 50) == 1 && zc_get_bit(&t, 18, 50) == 0,
			"round cap reaches half the width past the end");
	}
	// hairline: one pixel per column
	bits_white();
	zc_path_reset(&path);
	zc_path_move(&path, ZC_I(10) + 128, ZC_I(10) + 128);
	zc_path_line(&path, ZC_I(110) + 128, ZC_I(60) + 128);
	zc_stroke(&t, &path, ZC_I(1), false, &work, &black);
	ck(black_bits(&t) == 101, "a hairline has one pixel per column");

	// -- frames sit inside their box --
	bits_white();
	zc_frame_rect(&t, &work, ZC_I(10), ZC_I(10), ZC_I(30), ZC_I(20), ZC_I(1), &black);
	ck(zc_get_bit(&t, 10, 15) == 0 && zc_get_bit(&t, 39, 15) == 0 &&
		zc_get_bit(&t, 25, 10) == 0 && zc_get_bit(&t, 25, 29) == 0,
		"a 1-unit frame is on the box's own edge pixels");
	ck(zc_get_bit(&t, 9, 15) == 1 && zc_get_bit(&t, 40, 15) == 1 &&
		zc_get_bit(&t, 11, 15) == 1, "and nowhere else");
	ck(black_bits(&t) == 2 * 30 + 2 * 18, "30x20 frame is 96 pixels");
	bits_white();
	zc_frame_ellipse(&t, &work, ZC_I(20), ZC_I(20), ZC_I(100), ZC_I(60), ZC_I(4), &black);
	{
		int y, ink = 0;
		for (y = 15; y < 85; y++) if (zc_get_bit(&t, 70, y) == 0) ink++;
		ck(ink == 8, "a 4-unit ellipse frame: 4 pixels at top and bottom");
		ck(zc_get_bit(&t, 70, 20) == 0 && zc_get_bit(&t, 70, 19) == 1, "inside the box");
	}

	// -- clipping: nothing outside the clip changes --
	bits_white();
	zc_clip(&t, 50, 40, 150, 110);
	scene(&t, &(zc_xf_t){ ZC_XF_ONE, 0, 0, ZC_XF_ONE, 0, 0 });
	{
		int xx, yy, bad = 0;
		for (yy = 0; yy < H; yy++)
			for (xx = 0; xx < W; xx++)
				if ((xx < 50 || xx >= 150 || yy < 40 || yy >= 110) &&
					zc_get_bit(&t, xx, yy) == 0) bad++;
		ck(bad == 0, "no pixel drawn outside the clip");
	}
	zc_clip_reset(&t);

	// -- bands: a page drawn in bands is the page drawn whole --
	{
		static uint8_t whole[H][W], band[7][W];
		zc_target_t g;
		zc_xf_t xf = zc_xf_scale(ZC_XF_ONE, 0, 0);
		int b, row, same = 1;
		zc_target_gray(&g, &whole[0][0], W, W, H);
		zc_clear(&g, &(zc_paint_t){ ZC_P_SHADE, 0, 0, {0} });
		scene(&g, &xf);
		write_pgm_gray(dir, "zcanvas_scene_gray", &whole[0][0], W, W, H);
		for (b = 0; b < H; b += 7) {
			zc_target_gray(&g, &band[0][0], W, W, 7);
			zc_target_origin(&g, 0, b);
			zc_clip(&g, 0, b, W, H);
			memset(band, 255, sizeof(band));
			scene(&g, &xf);
			for (row = 0; row < 7 && b + row < H; row++)
				if (memcmp(band[row], whole[b + row], W)) same = 0;
		}
		ck(same, "grey page in 7-row bands == whole page");
		memcpy(gray, whole, sizeof(gray));
	}
	{
		static uint32_t whole[H][WPL], band[5][WPL];
		zc_target_t g;
		zc_xf_t xf = zc_xf_scale(ZC_XF_ONE, 0, 0);
		int b, row, same = 1;
		memset(whole, 0xFF, sizeof(whole));
		zc_target_bits(&g, &whole[0][0], WPL, W, H);
		scene(&g, &xf);
		write_pgm_bits(dir, "zcanvas_scene_bits", &whole[0][0], WPL, W, H);
		for (b = 0; b < H; b += 5) {
			memset(band, 0xFF, sizeof(band));
			zc_target_bits(&g, &band[0][0], WPL, W, 5);
			zc_target_origin(&g, 0, b);
			scene(&g, &xf);
			for (row = 0; row < 5 && b + row < H; row++)
				if (memcmp(band[row], whole[b + row], sizeof(band[0]))) same = 0;
		}
		ck(same, "1bpp page in 5-row bands == whole page");
	}

	// -- the same scene at 3x, the way a page prints --
	{
		static uint8_t big[H * 3][W * 3];
		zc_target_t g;
		zc_xf_t xf = zc_xf_scale(3 * ZC_XF_ONE, 0, 0);
		zc_target_gray(&g, &big[0][0], W * 3, W * 3, H * 3);
		g.pat_scale = 3;
		memset(big, 255, sizeof(big));
		scene(&g, &xf);
		write_pgm_gray(dir, "zcanvas_scene_3x", &big[0][0], W * 3, W * 3, H * 3);
		ck(big[3 * 50][3 * 110] < 255 && big[3 * 50][3 * 110] > 0,
			"shade prints as grey");
	}

	// -- transforms --
	{
		zc_xf_t r = zc_xf_rot90(ZC_XF_ONE, ZC_I(100), 0);
		zc_fix ox, oy;
		zc_xf_apply(&r, ZC_I(10), ZC_I(0), &ox, &oy);
		ck(ox == ZC_I(100) && oy == ZC_I(10), "quarter turn: +x goes to +y");
		zc_xf_apply(&r, 0, ZC_I(10), &ox, &oy);
		ck(ox == ZC_I(90) && oy == 0, "quarter turn: +y goes to -x");
		ck(zc_xf_scale_of(&r) == ZC_XF_ONE, "rotation keeps scale");
	}

	// -- patterns are anchored to the page origin --
	{
		static const uint8_t dotpat[8] = { 0x80, 0, 0, 0, 0, 0, 0, 0 };
		zc_paint_t d = zc_pattern(dotpat, 16, true);
		bits_white();
		zc_target_bits(&t, &bits[0][0], WPL, W, H);
		t.px = 3;
		t.py = 5;
		zc_fill_rect_dev(&t, 0, 0, 40, 40, &d);
		ck(zc_get_bit(&t, 3, 5) == 0 && zc_get_bit(&t, 11, 13) == 0,
			"pattern bit (0,0) at the anchor, repeating every 8");
		ck(zc_get_bit(&t, 4, 5) == 1, "and only there");
		ck(black_bits(&t) == 25, "one dot per 8x8 cell in 40x40");
	}

	// -- the word-at-a-time pattern and shade masks match the rule per
	//    pixel, at any anchor and cell size --
	{
		static const uint8_t pat[8] = { 0x81, 0x42, 0x24, 0x18, 0xF0, 0x0F, 0xAA, 0x55 };
		static const uint8_t b4[4][4] = { {0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5} };
		int ps, tone, op, bad = 0;
		for (ps = 1; ps <= 3; ps++)
			for (tone = 0; tone <= 16; tone += 5)
				for (op = 0; op < 2; op++) {
					zc_paint_t pp = zc_pattern(pat, tone, op != 0);
					int xx, yy;
					memset(bits, 0x55, sizeof(bits));
					zc_target_bits(&t, &bits[0][0], WPL, W, H);
					t.pat_scale = (uint8_t)ps;
					t.px = 13;
					t.py = -7;
					zc_fill_rect_dev(&t, 3, 2, 150, 40, &pp);
					for (yy = 2; yy < 42; yy++)
						for (xx = 3; xx < 153; xx++) {
							int cx = xx - 13, cy = yy + 7;
							int fx = cx >= 0 ? cx / ps : -((-cx + ps - 1) / ps);
							int fy = cy >= 0 ? cy / ps : -((-cy + ps - 1) / ps);
							int ink = (pat[fy & 7] >> (7 - (fx & 7))) & 1;
							int dark = b4[fy & 3][fx & 3] < tone;
							int was = (0x55555555u >> (xx & 31)) & 1;
							int want = ink ? !dark : (op ? 1 : was);
							if (zc_get_bit(&t, xx, yy) != want) bad++;
						}
				}
		ck(bad == 0, "pattern/shade words agree with the per-pixel rule");
		t.pat_scale = 1;
		t.px = t.py = 0;
	}

	// -- shades: density follows tone --
	{
		int tone;
		bool mono = true;
		int prev = -1;
		for (tone = 0; tone <= 16; tone++) {
			zc_paint_t s = zc_shade(tone);
			bits_white();
			zc_target_bits(&t, &bits[0][0], WPL, W, H);
			zc_fill_rect_dev(&t, 0, 0, 64, 64, &s);
			n = black_bits(&t);
			if (n != tone * 256) mono = false;
			if (n <= prev && tone) mono = false;
			prev = n;
		}
		ck(mono, "tone t darkens exactly t/16 of the pixels");
	}

	// -- blits: the aligned fast path agrees with nearest sampling --
	{
		static uint32_t src[40][2];
		int xx, yy, ok = 1;
		srand(7);
		for (yy = 0; yy < 40; yy++) { src[yy][0] = (uint32_t)rand() * 2654435761u; src[yy][1] = (uint32_t)rand(); }
		bits_white();
		zc_target_bits(&t, &bits[0][0], WPL, W, H);
		zc_blit_bits(&t, &src[0][0], 2, 50, 40, ZC_I(13), ZC_I(7), ZC_I(50), ZC_I(40));
		for (yy = 0; yy < 40; yy++)
			for (xx = 0; xx < 50; xx++) {
				int sb = (src[yy][xx >> 5] >> (xx & 31)) & 1;
				if (zc_get_bit(&t, 13 + xx, 7 + yy) != sb) ok = 0;
			}
		ck(ok, "1:1 blit copies every bit at an odd offset");
		ck(zc_get_bit(&t, 12, 7) == 1 && zc_get_bit(&t, 63, 7) == 1,
			"and nothing beside it");

		// mask: black source bits paint, white ones leave the target
		bits_white();
		zc_fill_rect_dev(&t, 0, 0, W, H, &(zc_paint_t){ ZC_P_SHADE, 8, 0, {0} });
		{
			static uint32_t before[H][WPL];
			memcpy(before, bits, sizeof(bits));
			zc_blit_mask(&t, &src[0][0], 2, 50, 40, ZC_I(5), ZC_I(5), ZC_I(50), ZC_I(40), &black);
			ok = 1;
			for (yy = 0; yy < 40; yy++)
				for (xx = 0; xx < 50; xx++) {
					int sb = (src[yy][xx >> 5] >> (xx & 31)) & 1;
					int was = (before[5 + yy][(5 + xx) >> 5] >> ((5 + xx) & 31)) & 1;
					int now = zc_get_bit(&t, 5 + xx, 5 + yy);
					if (sb == 0 && now != 0) ok = 0;
					if (sb == 1 && now != was) ok = 0;
				}
			ck(ok, "mask paints only where the source is black");
		}

		// partly off the target's left and top edges
		bits_white();
		zc_blit_bits(&t, &src[0][0], 2, 50, 40, ZC_I(-21), ZC_I(-3), ZC_I(50), ZC_I(40));
		ok = 1;
		for (yy = 0; yy < 37; yy++)
			for (xx = 0; xx < 29; xx++) {
				int sb = (src[yy + 3][(xx + 21) >> 5] >> ((xx + 21) & 31)) & 1;
				if (zc_get_bit(&t, xx, yy) != sb) ok = 0;
			}
		ck(ok && zc_get_bit(&t, 29, 0) == 1, "a blit hanging off the edge keeps its alignment");

		// scaled 2x: each source bit becomes a 2x2 block
		bits_white();
		zc_blit_bits(&t, &src[0][0], 2, 50, 40, 0, 0, ZC_I(100), ZC_I(80));
		ok = 1;
		for (yy = 0; yy < 80; yy++)
			for (xx = 0; xx < 100; xx++) {
				int sb = (src[yy / 2][(xx / 2) >> 5] >> ((xx / 2) & 31)) & 1;
				if (zc_get_bit(&t, xx, yy) != sb) ok = 0;
			}
		ck(ok, "2x blit doubles every bit");
	}

	// -- grey resampling and dithering --
	{
		static uint8_t ramp[32][64], out[16][32];
		static uint32_t d[60][4];
		static int16_t row[3 * (128 + 2)];
		int xx, yy, ink = 0;
		uint32_t r4[64];
		for (yy = 0; yy < 32; yy++) for (xx = 0; xx < 64; xx++) ramp[yy][xx] = 128;
		zc_resample(&ramp[0][0], 64, 64, 32, &out[0][0], 32, 32, 16, r4);
		ck(out[7][9] == 128, "box reduction of flat grey is the same grey");
		zc_dither(&ramp[0][0], 64, 64, 32, &d[0][0], 4, 100, 60, row, 0, 0);
		for (yy = 0; yy < 60; yy++)
			for (xx = 0; xx < 100; xx++)
				if (!((d[yy][xx >> 5] >> (xx & 31)) & 1)) ink++;
		ck(ink > 2700 && ink < 3300, "50% grey dithers to about half ink");
		for (yy = 0; yy < 32; yy++) for (xx = 0; xx < 64; xx++) ramp[yy][xx] = (uint8_t)(xx * 4);
		zc_dither(&ramp[0][0], 64, 64, 32, &d[0][0], 4, 128, 60, row, 0, 0);
		write_pgm_bits(dir, "zcanvas_dither", &d[0][0], 4, 128, 60);
	}

	printf("zcanvas: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
