// zv_test -- host tests of ZV: the format, the decoder's views and the
// encoder's agreement with it. `make test` in tools/zv.

#include "zv.h"
#include "zv_masks.h"
#include "zvenc_core.h"
#include "zvcol.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;

#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; if (fails < 30) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static const uint8_t *mask_tab(int m) { return m == ZV_MASK_VAC8 ? zv_mask_vac8 : zv_mask_bayer8; }

// ---- synthetic film -----------------------------------------------------

#define TW 160
#define TH 96

static void make_frame(uint8_t *g, int n) {
	int x, y;
	uint32_t s = 777u + (uint32_t)n * 13u;
	for (y = 0; y < TH; y++)
		for (x = 0; x < TW; x++) {
			int v;
			if (n < 20) v = ((x + n * 3) * 255 / TW + y) & 255;             // a panning ramp
			else v = 40 + (int)(60 * sin((x + n) * 0.1) * cos(y * 0.07));   // another scene
			if (v < 0) v = 0;
			g[y * TW + x] = (uint8_t)v;
		}
	// a bouncing square
	{
		int sx = 10 + (n * 7) % (TW - 40), sy = 10 + (n * 5) % (TH - 30);
		for (y = sy; y < sy + 20; y++) for (x = sx; x < sx + 24; x++) g[y * TW + x] = (uint8_t)(n & 1 ? 250 : 10);
	}
	// a noisy patch
	for (y = 60; y < 90; y++)
		for (x = 100; x < 150; x++) { s = s * 1103515245u + 12345u; g[y * TW + x] = (uint8_t)(s >> 24); }
}

// ---- reference views from the grey pictures ----------------------------------

static void half_ref_of_full(const uint8_t *full, uint8_t *half) {
	// not used for checks (the half views are defined by the entries, not by
	// the full picture); kept for debugging
	(void)full; (void)half;
}

static int check_views(zv_dec_t *grey_full, zv_dec_t *grey_half, zv_dec_t *mono_full, zv_dec_t *mono_half,
                       zv_dec_t *pal_half, const uint8_t *gf, const uint8_t *gh,
                       const uint8_t *mf, const uint8_t *mh, const uint8_t *ph, int mask) {
	const uint8_t *m = mask_tab(mask);
	int x, y, bad = 0;
	(void)grey_full; (void)grey_half; (void)pal_half;
	for (y = 0; y < TH; y++)
		for (x = 0; x < TW; x++) {
			int want = mono_full->lin[gf[y * TW + x]] > (uint32_t)(2 * m[(y & 7) * 8 + (x & 7)] + 1) * 32;
			int got = (mf[y * (TW / 8) + (x >> 3)] >> (x & 7)) & 1;
			if (want != got) bad++;
		}
	CHECK(bad == 0, "mono full != dither(grey full): %d pixels (mask %d)", bad, mask);
	bad = 0;
	for (y = 0; y < TH / 2; y++)
		for (x = 0; x < TW / 2; x++) {
			int want = mono_half->lin[gh[y * (TW / 2) + x]] > (uint32_t)(2 * m[(y & 7) * 8 + (x & 7)] + 1) * 32;
			int got = (mh[y * ((TW / 2 + 7) / 8) + (x >> 3)] >> (x & 7)) & 1;
			if (want != got) bad++;
		}
	CHECK(bad == 0, "mono half != dither(grey half): %d pixels (mask %d)", bad, mask);
	bad = 0;
	for (y = 0; y < TH / 2; y++)
		for (x = 0; x < TW / 2; x++) {
			uint8_t g = gh[y * (TW / 2) + x];
			int want = pal_half->pal_lo[g] + (pal_half->pal_frac[g] > m[(y & 7) * 8 + (x & 7)]);
			if (ph[y * (TW / 2) + x] != want) bad++;
		}
	CHECK(bad == 0, "palette half != dither(grey half): %d pixels (mask %d)", bad, mask);
	return bad;
}

// ---- tests --------------------------------------------------------------------

static void test_maths(void) {
	zv_header_t h;
	zv_dec_t *d = calloc(1, sizeof(*d));
	int i, worst = 0;
	uint32_t f, prev, tot;
	memset(&h, 0, sizeof(h));
	h.w = 64; h.h = 32; h.fps_num = 24; h.fps_den = 1;
	zv_dec_init(d, &h, ZV_VIEW_MONO_FULL, ZV_MASK_BAYER8, 220);
	CHECK(d->lin[0] == 0 && d->lin[255] == 4096, "lin ends %u %u", d->lin[0], d->lin[255]);
	for (i = 1; i < 256; i++) {
		double want = 4096.0 * pow(i / 255.0, 2.2);
		int e = abs((int)d->lin[i] - (int)(want + 0.5));
		if (e > worst) worst = e;
		CHECK(d->lin[i] >= d->lin[i - 1], "lin not monotonic at %d", i);
	}
	CHECK(worst <= 2, "lin off by %d from pow()", worst);
	for (i = 0; i < 4096; i += 37) {
		double r = pow(((i >> 8) & 15) / 15.0, 2.2), g = pow(((i >> 4) & 15) / 15.0, 2.2), b = pow((i & 15) / 15.0, 2.2);
		double want = 4096 * (0.2126 * r + 0.7152 * g + 0.0722 * b);
		CHECK(fabs(zv_lin12((uint16_t)i, 220) - want) < 4, "lin12 %03x: %u vs %.1f", i, zv_lin12((uint16_t)i, 220), want);
	}
	// cadence
	prev = zv_frame_vsync(&h, 0, 60);
	CHECK(prev == 0, "frame 0 at %u", prev);
	for (f = 1; f < 49; f++) {
		uint32_t v = zv_frame_vsync(&h, f, 60), dlt = v - prev;
		CHECK(dlt == 2 || dlt == 3, "24 fps: frame %u held %u", f, dlt);
		prev = v;
	}
	CHECK(zv_frame_vsync(&h, 48, 60) == 120, "48 frames at 24 fps = 120 refreshes, got %u", zv_frame_vsync(&h, 48, 60));
	h.fps_num = 30000; h.fps_den = 1001;
	CHECK(zv_frame_vsync(&h, 30000, 60) == 60060, "29.97 drift: %u", zv_frame_vsync(&h, 30000, 60));
	h.fps_num = 18; h.fps_den = 1;
	CHECK(zv_frame_vsync(&h, 18, 60) == 60, "18 fps: %u", zv_frame_vsync(&h, 18, 60));
	// audio share
	h.fps_num = 24000; h.fps_den = 1001; h.arate = 22050;
	for (tot = 0, f = 0; f < 1000; f++) tot += zv_audio_before(&h, f + 1) - zv_audio_before(&h, f);
	CHECK(tot == zv_audio_before(&h, 1000), "audio shares %u", tot);
	CHECK(zv_audio_before(&h, 24000) == 22050u * 1001u, "a 1001 s of audio: %u", zv_audio_before(&h, 24000));
	free(d);
}

static void test_header(void) {
	zv_header_t a, b;
	uint8_t hb[ZV_HDR_SIZE];
	memset(&a, 0, sizeof(a));
	a.w = 640; a.h = 360; a.flags = ZV_F_AUDIO; a.fps_num = 24000; a.fps_den = 1001; a.frames = 123456;
	a.arate = 22050; a.achan = 1; a.afmt = ZV_A_PCM8; a.index_off = 0x12345678; a.index_n = 99; a.max_rec = 40000;
	a.video_bytes = 1; a.audio_bytes = 2;
	zv_write_header(hb, &a);
	CHECK(zv_parse_header(hb, sizeof(hb), &b) == 0, "parse");
	CHECK(!memcmp(&a, &b, sizeof(a)), "round trip");
	hb[3] = 'X';
	CHECK(zv_parse_header(hb, sizeof(hb), &b) < 0, "bad magic accepted");
	hb[3] = ZV_MAGIC3; hb[8] = 3;
	CHECK(zv_parse_header(hb, sizeof(hb), &b) < 0, "width not a multiple of 8 accepted");
}

static void test_palette(void) {
	uint16_t pal[ZV_PAL_MAX];
	int n = zv_grey_palette(pal, ZV_PAL_MAX, ZV_GREYS_NEAR), i;
	uint16_t tp[ZV_PAL_MAX];
	int nt = zv_grey_palette(tp, ZV_PAL_MAX, ZV_GREYS_TRUE);
	CHECK(nt == 16 && tp[0] == 0x000 && tp[15] == 0xfff && tp[7] == 0x777, "true greys: %d", nt);
	CHECK(n >= 40 && n <= ZV_PAL_MAX, "%d greys", n);
	CHECK(pal[0] == 0x000 && pal[n - 1] == 0xfff, "ends %03x %03x", pal[0], pal[n - 1]);
	for (i = 1; i < n; i++) {
		int r = (pal[i] >> 8) & 15, g = (pal[i] >> 4) & 15, b = pal[i] & 15;
		int lo = r < g ? (r < b ? r : b) : (g < b ? g : b), hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
		CHECK(zv_lin12(pal[i], 220) > zv_lin12(pal[i - 1], 220), "palette not increasing at %d", i);
		CHECK(hi - lo <= 1, "palette entry %03x not near-grey", pal[i]);
	}
	printf("  grey palette: %d entries\n", n);
}

static void test_codec(int mask) {
	zv_header_t h;
	zve_opts_t o;
	zve_t *e;
	static zv_dec_t dg, dgh, dm, dmh, dp, dseek, gm, gmh, gp, dpf, gpf;
	static uint8_t pf[TW * TH], pf2[TW * TH];
	static uint8_t mf2[TW * TH / 8], mh2[TW * TH / 32], ph2[TW * TH / 4];
	static uint8_t g[TW * TH], gf[TW * TH], gh[TW * TH / 4], mf[TW * TH / 8], mh[TW * TH / 32],
		ph[TW * TH / 4], sk[TW * TH], pay[200000];
	static uint8_t saved[64][TW * TH];
	int n, seek_at = 10, skipped = 0;
	uint32_t used16 = 0, used4 = 0, used1 = 0;
	memset(&h, 0, sizeof(h));
	h.w = TW; h.h = TH; h.fps_num = 24; h.fps_den = 1;
	memset(&o, 0, sizeof(o));
	o.w = TW; o.h = TH; o.target_bytes = 1500; o.refresh = 4;
	e = zve_new(&o);
	zv_dec_init(&dg, &h, ZV_VIEW_GREY_FULL, mask, 0);
	zv_dec_init(&dgh, &h, ZV_VIEW_GREY_HALF, mask, 0);
	zv_dec_init(&dm, &h, ZV_VIEW_MONO_FULL, mask, 0);
	zv_dec_init(&dmh, &h, ZV_VIEW_MONO_HALF, mask, 0);
	zv_dec_init(&dp, &h, ZV_VIEW_PAL_HALF, mask, 0);
	zv_dec_init(&dseek, &h, ZV_VIEW_GREY_FULL, mask, 0);
	zv_dec_init(&dpf, &h, ZV_VIEW_PAL_FULL, mask, 0);
	zv_dec_init(&gpf, &h, ZV_VIEW_PAL_FULL, mask, 0); gpf.generic = 1;
	memset(pf, 0, sizeof(pf)); memset(pf2, 0, sizeof(pf2));
	zv_dec_init(&gm, &h, ZV_VIEW_MONO_FULL, mask, 0); gm.generic = 1;
	zv_dec_init(&gmh, &h, ZV_VIEW_MONO_HALF, mask, 0); gmh.generic = 1;
	zv_dec_init(&gp, &h, ZV_VIEW_PAL_HALF, mask, 0); gp.generic = 1;
	memset(mf2, 0, sizeof(mf2)); memset(mh2, 0, sizeof(mh2)); memset(ph2, 0, sizeof(ph2));
	memset(mf, 0, sizeof(mf)); memset(mh, 0, sizeof(mh)); memset(ph, 0, sizeof(ph));
	{
		static uint8_t dty[(TW / 8) * (TH / 8)], dty2[(TW / 8) * (TH / 8)];
		dp.dirty = dty; gp.dirty = dty2;
	}
	zv_dec_seek(&dseek, 0);
	memset(gf, 0, sizeof(gf)); memset(sk, 0, sizeof(sk));
	for (n = 0; n < 40; n++) {
		zve_stats_t st;
		int key = n % 10 == 0;
		size_t len;
		make_frame(g, n);
		len = zve_frame(e, g, key, pay, sizeof(pay), &st);
		CHECK(len > 0, "frame %d not encoded", n);
		CHECK(zv_dec_video(&dg, pay, len, key, gf, TW) == 0, "grey full %d", n);
		CHECK(zv_dec_video(&dpf, pay, len, key, pf, TW) == 0, "palette full %d", n);
		CHECK(zv_dec_video(&gpf, pay, len, key, pf2, TW) == 0, "generic palette full %d", n);
		CHECK(!memcmp(pf, pf2, sizeof(pf)), "frame %d: board loop (palette full) != generic", n);
		{
			const uint8_t *mm = mask_tab(mask);
			int x3, y3, bad3 = 0;
			for (y3 = 0; y3 < TH; y3++)
				for (x3 = 0; x3 < TW; x3++) {
					uint8_t g3 = gf[y3 * TW + x3];
					int want = dpf.pal_lo[g3] + (dpf.pal_frac[g3] > mm[(y3 & 7) * 8 + (x3 & 7)]);
					if (pf[y3 * TW + x3] != want) bad3++;
				}
			CHECK(bad3 == 0, "frame %d: palette full != dither(grey full): %d pixels", n, bad3);
		}
		CHECK(zv_dec_video(&dgh, pay, len, key, gh, TW / 2) == 0, "grey half %d", n);
		CHECK(zv_dec_video(&dm, pay, len, key, mf, TW / 8) == 0, "mono full %d", n);
		CHECK(zv_dec_video(&dmh, pay, len, key, mh, (TW / 2 + 7) / 8) == 0, "mono half %d", n);
		{
			static uint8_t prevph[TW * TH / 4];
			int bx2, by2, x2, y2, bad2 = 0;
			memcpy(prevph, ph, sizeof(prevph));
			memset(dp.dirty, 0, (TW / 8) * (TH / 8));
			memset(gp.dirty, 0, (TW / 8) * (TH / 8));
			CHECK(zv_dec_video(&dp, pay, len, key, ph, TW / 2) == 0, "palette %d", n);
			for (by2 = 0; by2 < TH / 8; by2++)
				for (bx2 = 0; bx2 < TW / 8; bx2++)
					if (!dp.dirty[by2 * (TW / 8) + bx2])
						for (y2 = 0; y2 < 4; y2++)
							for (x2 = 0; x2 < 4; x2++) {
								int at = (by2 * 4 + y2) * (TW / 2) + bx2 * 4 + x2;
								if (ph[at] != prevph[at]) bad2++;
							}
			CHECK(bad2 == 0, "frame %d: %d pixels changed outside the changed blocks", n, bad2);
		}
		CHECK(!memcmp(gf, zve_recon(e), sizeof(gf)), "frame %d: decoder != encoder's picture", n);
		CHECK(zv_dec_video(&gm, pay, len, key, mf2, TW / 8) == 0, "generic mono full %d", n);
		CHECK(zv_dec_video(&gmh, pay, len, key, mh2, (TW / 2 + 7) / 8) == 0, "generic mono half %d", n);
		CHECK(zv_dec_video(&gp, pay, len, key, ph2, TW / 2) == 0, "generic palette %d", n);
		CHECK(!memcmp(mf, mf2, sizeof(mf)), "frame %d: board loop (mono full) != generic", n);
		CHECK(!memcmp(mh, mh2, sizeof(mh)), "frame %d: board loop (mono half) != generic", n);
		CHECK(!memcmp(ph, ph2, sizeof(ph)), "frame %d: board loop (palette) != generic", n);
		CHECK(!memcmp(dp.dirty, gp.dirty, (TW / 8) * (TH / 8)), "frame %d: changed blocks differ (board loop, generic)", n);
		check_views(&dg, &dgh, &dm, &dmh, &dp, gf, gh, mf, mh, ph, mask);
		memcpy(saved[n], gf, sizeof(gf));
		used1 += st.v1; used4 += st.v4; used16 += st.v16;
		// a player that starts at frame 15: waits for the seek point at 20
		if (n >= 15) {
			int r;
			if (n == 15) { zv_dec_seek(&dseek, 15); memset(sk, 0x55, sizeof(sk)); }
			r = zv_dec_video(&dseek, pay, len, key, sk, TW);
			if (n < 20) { CHECK(r == 1, "frame %d before the seek point drawn (%d)", n, r); skipped++; }
			else if (n < 20 + 4 - 1) CHECK(r == 0, "refresh frame %d not decoded", n);
			else CHECK(r == 0 && !memcmp(sk, gf, sizeof(sk)), "after a seek and its refresh, frame %d differs", n);
		}
		if (n == 39) printf("  mask %d: frame 39 %u bytes, skip %u v1 %u v4 %u, psnr %.1f\n", mask, st.bytes,
			st.skip, st.v1, st.v4, 10 * log10(255.0 * 255 * TW * TH / (st.sse + 1)));
	}
	CHECK(skipped == 5, "skipped %d", skipped);
	{
		// a player that changes view: from the last seek point again,
		// it must draw what a decoder in that view from the start drew
		static zv_dec_t sw;
		static uint8_t o2[TW * TH / 8];
		zve_t *e2;
		zve_opts_t o3 = o;
		int m;
		zv_dec_init(&sw, &h, ZV_VIEW_MONO_HALF, mask, 0);
		e2 = zve_new(&o3);
		for (m = 0; m < 40; m++) {
			size_t l2;
			make_frame(g, m);
			l2 = zve_frame(e2, g, m % 10 == 0, pay, sizeof(pay), NULL);
			if (m == 25) { zv_dec_set_view(&sw, ZV_VIEW_MONO_FULL, mask); zv_dec_seek(&sw, 25); }
			zv_dec_video(&sw, pay, l2, m % 10 == 0, o2, TW / 8);
		}
		CHECK(!memcmp(o2, mf, sizeof(mf)), "a decoder that changed view differs from one that did not");
		zve_free(e2);
	}
	CHECK(used1 && used4 && used16, "every block kind used: v1 %u v4 %u v16 %u", used1, used4, used16);
	printf("  mask %d: blocks over 40 frames: v1 %u, v4 %u, v16 %u\n", mask, used1, used4, used16);
	(void)seek_at;
	zve_free(e);
}

static void test_fuzz(void) {
	zv_header_t h;
	zve_opts_t o;
	zve_t *e;
	static zv_dec_t d;
	static uint8_t g[TW * TH], out[TW * TH], pay[200000], bad[200000];
	size_t len;
	int i, k;
	uint32_t s = 99;
	memset(&h, 0, sizeof(h));
	h.w = TW; h.h = TH; h.fps_num = 24; h.fps_den = 1;
	memset(&o, 0, sizeof(o));
	o.w = TW; o.h = TH; o.lambda = 50;
	e = zve_new(&o);
	make_frame(g, 3);
	len = zve_frame(e, g, 1, pay, sizeof(pay), NULL);
	for (i = 0; i < 2000; i++) {
		size_t l = len;
		memcpy(bad, pay, len);
		s = s * 1664525u + 1013904223u;
		if (i & 1) l = (s >> 8) % len;                       // truncated
		else for (k = 0; k < 4; k++) { s = s * 1664525u + 1013904223u; bad[(s >> 8) % len] = (uint8_t)(s >> 3); }
		zv_dec_init(&d, &h, (int)(i % ZV_VIEW_COUNT), ZV_MASK_VAC8, 0);
		(void)zv_dec_video(&d, bad, l, 1, out, TW);
	}
	CHECK(1, "fuzz survived");
	zve_free(e);
}

// the colour layer: encoder, cell runs, palette, apply
static void test_colour(void) {
	enum { CW = 64, CH = 48, NC = (CW / 8) * (CH / 8), NF = 30 };
	static uint8_t y[CW * CH], u[CW * CH], v[CW * CH], out[CW * CH];
	static uint16_t pal[256];
	static uint8_t cls[NC];
	zvc_t *c = zvc_new(CW, CH, 8);
	uint8_t *sec;
	double share;
	size_t len, at;
	zv_col_header_t h;
	uint32_t f, g, i, s = 7;
	int r, bad;
	// a frame: left half red, right half blue, a grey band, moving
	for (f = 0; f < NF; f++) {
		for (i = 0; i < CW * CH; i++) {
			int x = (int)(i % CW), yy = (int)(i / CW), band = ((yy / 8 + (int)f / 10) % 3) == 0;
			y[i] = (uint8_t)(64 + x * 2);
			u[i] = band ? 128 : x < CW / 2 ? 90 : 200;
			v[i] = band ? 128 : x < CW / 2 ? 210 : 100;
		}
		zvc_frame(c, y, u, v, f % 10 == 0);
	}
	len = zvc_finish(c, &sec, &share);
	CHECK(zv_col_parse_header(sec, len, &h) == 0, "colour header");
	CHECK(h.cw == CW / 8 && h.ch == CH / 8 && h.groups == 3, "colour header %ux%u, %u groups", h.cw, h.ch, h.groups);
	CHECK(share > 0.5 && share < 0.8, "coloured share %.2f", share);
	// every frame's runs decode, in the place the index says, and the
	// classes say: grey band neutral, red side and blue side not, and not
	// the same as each other
	at = 0;
	for (g = 0; g < h.groups; g++) {
		uint32_t first = sec[16 + g * 8] | sec[17 + g * 8] << 8, off = sec[20 + g * 8] | sec[21 + g * 8] << 8 | sec[22 + g * 8] << 16;
		const int8_t *tab = (const int8_t *)(sec + off);
		CHECK(first == g * 10, "group %u first frame %u", g, first);
		CHECK(tab[0] == 0 && tab[1] == 0, "class 0 neutral");
		zv_col_palette(tab, pal);
		for (i = 0; i < 16; i++) CHECK(pal[i] == i * 0x111, "palette: class 0 level %u is %03x", i, pal[i]);
		at = off + ZV_COL_TABLE;
		for (f = 0; f < 10; f++) {
			uint32_t fr = g * 10 + f;
			r = zv_col_frame(sec + at, len - at, cls, NC, NULL);
			CHECK(r > 0, "frame %u runs", fr);
			if (r <= 0) break;
			at += (size_t)r;
			bad = 0;
			for (i = 0; i < NC; i++) {
				int cx = (int)(i % (CW / 8)), cy = (int)(i / (CW / 8)), band = ((cy + (int)fr / 10) % 3) == 0;
				int k = cls[i];
				if (band ? k != 0 : k == 0) bad++;
				else if (!band) {
					const int8_t *e = tab + 2 * k;
					// red side: v up, u down; blue side: u up
					if (cx < CW / 16 ? !(e[1] > 40 && e[0] < -20) : !(e[0] > 40)) bad++;
				}
			}
			CHECK(bad == 0, "frame %u: %d cells in the wrong class", fr, bad);
		}
	}
	CHECK(at == len, "the section ends with the last frame (%zu of %zu)", at, len);
	// apply: high nibble from the class, low kept
	for (i = 0; i < CW * CH; i++) out[i] = (uint8_t)(0xf0 | (i & 15));
	zv_col_apply(cls, CW / 8, CH / 8, 8, out, CW);
	bad = 0;
	for (i = 0; i < CW * CH; i++)
		if (out[i] != (uint8_t)(cls[(i / CW / 8) * (CW / 8) + (i % CW) / 8] << 4 | (i & 15))) bad++;
	CHECK(bad == 0, "apply: %d pixels", bad);
	// apply_map: the marked blocks, the same as apply there, the rest alone
	{
		static uint8_t o2[CW * CH], map[NC];
		for (i = 0; i < NC; i++) map[i] = (uint8_t)((i * 7) % 3 == 0);
		for (i = 0; i < CW * CH; i++) o2[i] = (uint8_t)(0x50 | (i & 15));
		zv_col_apply_map(cls, map, CW / 8, CH / 8, 8, 8, o2, CW);
		bad = 0;
		for (i = 0; i < CW * CH; i++) {
			uint32_t c = (i / CW / 8) * (CW / 8) + (i % CW) / 8;
			uint8_t want = map[c] ? (uint8_t)(cls[c] << 4 | (i & 15)) : (uint8_t)(0x50 | (i & 15));
			if (o2[i] != want) bad++;
		}
		CHECK(bad == 0, "apply_map: %d pixels", bad);
	}
	// 4x4 cells: an encoder at 4, its frames decoded, and apply_map at
	// both block sizes equal to the plain apply
	{
		zvc_t *c4 = zvc_new(CW, CH, 4);
		uint8_t *s4;
		double sh4;
		size_t l4;
		zv_col_header_t h4;
		static uint8_t cls4[NC * 4], map4[NC], o3[CW * CH], o4[CW * CH];
		uint32_t a4, bs, ff;
		for (ff = 0; ff < 6; ff++) {
			for (i = 0; i < CW * CH; i++) {
				int x = (int)(i % CW), yy = (int)(i / CW);
				y[i] = 120; u[i] = ((x / 4 + yy / 4 + ff) & 1) ? 70 : 190; v[i] = ((x / 4) & 1) ? 200 : 60;
			}
			zvc_frame(c4, y, u, v, ff == 0);
		}
		l4 = zvc_finish(c4, &s4, &sh4);
		CHECK(zv_col_parse_header(s4, l4, &h4) == 0 && h4.cell == 4 && h4.cw == CW / 4 && h4.ch == CH / 4,
			"4x4 header: cell %u, %ux%u", h4.cell, h4.cw, h4.ch);
		a4 = (uint32_t)(s4[20] | s4[21] << 8 | s4[22] << 16) + ZV_COL_TABLE;
		memset(cls4, 0, sizeof(cls4));
		CHECK(zv_col_frame(s4 + a4, l4 - a4, cls4, NC * 4, NULL) > 0, "4x4 runs");
		bad = 0;     // checkered chroma: neighbouring cells differ
		for (i = 0; i + 1 < (uint32_t)NC * 4; i++) if ((i + 1) % (CW / 4) && cls4[i] == cls4[i + 1]) bad++;
		CHECK(bad == 0, "4x4: %d neighbouring cells alike", bad);
		for (bs = 4; bs <= 8; bs += 4) {
			for (i = 0; i < NC; i++) map4[i] = 1;
			for (i = 0; i < CW * CH; i++) o3[i] = o4[i] = (uint8_t)(i & 15);
			zv_col_apply_map(cls4, map4, CW / 8, CH / 8, (int)bs, 4, o3, CW);
			zv_col_apply(cls4, CW / 4, CH / 4, (int)bs / 2, o4, CW);
			bad = 0;
			for (i = 0; i < (uint32_t)(CH / 8 * bs) * CW; i++) if ((i % CW) < (uint32_t)(CW / 8 * bs) && o3[i] != o4[i]) bad++;
			CHECK(bad == 0, "4x4 apply_map at %u: %d pixels", bs, bad);
		}
		free(s4);
		zvc_free(c4);
	}
	// mark: a frame's runs mark exactly the cells that changed
	{
		static uint8_t before[NC], mk[NC];
		uint32_t a2 = (uint32_t)(sec[20] | sec[21] << 8 | sec[22] << 16) + ZV_COL_TABLE, nm = 0, nd = 0;
		int r2;
		memset(cls, 0, NC);
		r2 = zv_col_frame(sec + a2, len - a2, cls, NC, NULL);      // frame 0: all
		a2 += (uint32_t)r2;
		for (f = 1; f < 10; f++) {
			memcpy(before, cls, NC); memset(mk, 0, NC);
			r2 = zv_col_frame(sec + a2, len - a2, cls, NC, mk);
			if (r2 <= 0) break;
			a2 += (uint32_t)r2;
			for (i = 0; i < NC; i++) { nm += mk[i]; nd += before[i] != cls[i]; if (mk[i] != (before[i] != cls[i])) bad++; }
		}
		CHECK(bad == 0 && nm == nd, "mark: %d cells wrong (%u marked, %u changed)", bad, nm, nd);
		// and the block version agrees, block for block
		{
			static uint8_t c1[NC], c2[NC], m1[NC], bm[NC], bm2[NC];
			static uint16_t lst[NC];
			int nl;
			uint32_t a3 = (uint32_t)(sec[20] | sec[21] << 8 | sec[22] << 16) + ZV_COL_TABLE;
			int r1, r3, nch, bad2 = 0;
			memset(c1, 0, NC); memset(c2, 0, NC);
			for (f = 0; f < 10; f++) {
				memset(m1, 0, NC); memset(bm, 0, NC); memset(bm2, 0, NC);
				r1 = zv_col_frame(sec + a3, len - a3, c1, NC, m1);
				nl = 0;
				r3 = zv_col_frame_blocks(sec + a3, len - a3, c2, CW / 8, CH / 8, 8, bm, lst, &nl);
				(void)nch;
				for (i = 0; i < (uint32_t)nl; i++) bm2[(lst[i] >> 8) * (CW / 8) + (lst[i] & 0xff)]++;
				for (i = 0; i < NC; i++) if (bm2[i] != bm[i]) bad2++;
				if (r1 != r3 || r1 <= 0) { bad2++; break; }
				a3 += (uint32_t)r1;
				for (i = 0; i < NC; i++) if (m1[i] != bm[i] || c1[i] != c2[i]) bad2++;
			}
			CHECK(bad2 == 0, "frame_blocks != frame: %d", bad2);
		}
	}
	// in gamut, and brightness kept: a class's levels never go darker
	zv_col_palette((const int8_t *)(sec + (sec[20] | sec[21] << 8 | sec[22] << 16)), pal);
	for (g = 1; g < 16; g++)
		for (i = 1; i < 16; i++)
			CHECK(zv_lin12(pal[g * 16 + i], 220) + 200 >= zv_lin12(pal[g * 16 + i - 1], 220),
				"palette class %u level %u darker than %u", g, i, i - 1);
	// black and white: nothing coloured
	{
		zvc_t *b = zvc_new(CW, CH, 8);
		uint8_t *s2;
		double sh;
		memset(u, 128, sizeof(u)); memset(v, 128, sizeof(v));
		for (f = 0; f < 12; f++) zvc_frame(b, y, u, v, f == 0);
		free(s2 = NULL);
		zvc_finish(b, &s2, &sh);
		CHECK(sh == 0, "black and white: %.3f coloured", sh);
		free(s2);
		zvc_free(b);
	}
	// fuzz the runs
	for (i = 0; i < 3000; i++) {
		uint8_t junk[64];
		uint32_t k;
		for (k = 0; k < sizeof(junk); k++) { s = s * 1664525u + 1013904223u; junk[k] = (uint8_t)(s >> 24); }
		(void)zv_col_frame(junk, (s >> 8) % sizeof(junk), cls, NC, NULL);
	}
	CHECK(1, "colour fuzz survived");
	free(sec);
	zvc_free(c);
}

// the decoder with the colour layer's classes: each palette loop (and
// the generic code) gives the plain picture with the classes applied --
// when the blocks whose class changed but were not drawn get
// zv_col_apply_map(), as cinema does
static void test_colour_decode(void) {
	static uint8_t g[TW * TH], pay[1 << 16];
	static uint8_t plain[TW * TH], ref[TW * TH], got[4][TW * TH];
	static uint8_t cls[(TW / 4) * (TH / 4)], cmap[(TW / 8) * (TH / 8)];
	static uint32_t colw[(TW / 8) * (TH / 8) * 4];
	zv_header_t h;
	zve_opts_t o;
	zve_t *e;
	zv_dec_t dpl, dc[4];
	int view, cell, n, i, k, bad;
	uint32_t s = 99;
	memset(&h, 0, sizeof(h));
	h.w = TW; h.h = TH; h.fps_num = 24; h.fps_den = 1; h.frames = 40; h.refresh = 1;
	for (view = ZV_VIEW_PAL_HALF; view <= ZV_VIEW_PAL_FULL; view++)
		for (cell = 4; cell <= 8; cell += 4) {
			int cw = TW / cell, ch = TH / cell, st = view == ZV_VIEW_PAL_FULL ? TW : TW / 2;
			int bs = view == ZV_VIEW_PAL_FULL ? 8 : 4, rows = TH / 8 * bs;
			memset(&o, 0, sizeof(o));
			o.w = TW; o.h = TH; o.lambda = 30;
			e = zve_new(&o);
			zv_dec_init(&dpl, &h, view, ZV_MASK_VAC8, 0);
			for (k = 0; k < 2; k++) {
				zv_dec_init(&dc[k], &h, view, ZV_MASK_VAC8, 0);
				dc[k].generic = k;
				dc[k].colw = colw;
			}
			memset(plain, 0, sizeof(plain)); memset(got, 0, sizeof(got)); memset(cls, 0, sizeof(cls));
			zv_col_words(cls, cw, ch, cell, NULL, colw);
			bad = 0;
			for (n = 0; n < 30; n++) {
				int key = n % 10 == 0;
				size_t len;
				make_frame(g, n);
				len = zve_frame(e, g, key, pay, sizeof(pay), NULL);
				// some classes change; their blocks are noted
				memset(cmap, 0, sizeof(cmap));
				for (i = 0; i < 12; i++) {
					uint32_t c;
					s = s * 1664525u + 1013904223u;
					c = (s >> 8) % (uint32_t)(cw * ch);
					cls[c] = (uint8_t)((s >> 4) & 15);
					cmap[(c / cw) * bs / bs * 0 + ((int)(c / cw) * cell / 8) * (TW / 8) + ((int)(c % cw) * cell / 8)] = 1;
				}
				zv_col_words(cls, cw, ch, cell, cmap, colw);
				zv_dec_video(&dpl, pay, len, key, plain, st);
				for (k = 0; k < 2; k++) {
					zv_dec_video(&dc[k], pay, len, key, got[k], st);
					if (k == 0) zv_col_apply_map(cls, cmap, TW / 8, TH / 8, bs, cell, got[k], st);
					else {      // the list way, as cinema does it
						static uint16_t lst[(TW / 8) * (TH / 8)];
						int nl = 0, b2;
						for (b2 = 0; b2 < (TW / 8) * (TH / 8); b2++)
							if (cmap[b2]) lst[nl++] = (uint16_t)((b2 / (TW / 8)) << 8 | (b2 % (TW / 8)));
						zv_col_apply_list(colw, TW / 8, lst, nl, bs, got[k], st);
					}
				}
				memcpy(ref, plain, sizeof(ref));
				zv_col_apply(cls, cw, ch, cell * bs / 8, ref, st);
				for (k = 0; k < 2; k++)
					for (i = 0; i < rows * st; i++) if (got[k][i] != ref[i]) bad++;
			}
			CHECK(bad == 0, "colour decode, view %d, %dx%d cells: %d pixels differ", view, cell, cell, bad);
			zve_free(e);
		}
}

// The plane views against the palette views: a pixel's four plane bits
// are the palette index, for every frame of a clip with skips, keys and
// all three block kinds.
static void test_planes(void) {
	static uint8_t g[TW * TH], pay[1 << 16];
	static uint8_t pal[TW * TH], pln[4 * (TW / 8) * TH];
	zv_header_t h;
	zve_opts_t o;
	zve_t *e;
	zv_dec_t dp, dq;
	int half, n, x, y, k, bad;
	memset(&h, 0, sizeof(h));
	h.w = TW; h.h = TH; h.fps_num = 24; h.fps_den = 1; h.frames = 40; h.refresh = 1;
	for (half = 0; half < 2; half++) {
		int w = half ? TW / 2 : TW, hh = half ? TH / 2 : TH, st = (w + 7) / 8, po = st * hh;
		memset(&o, 0, sizeof(o));
		o.w = TW; o.h = TH; o.lambda = 30;
		e = zve_new(&o);
		zv_dec_init(&dp, &h, half ? ZV_VIEW_PAL_HALF : ZV_VIEW_PAL_FULL, ZV_MASK_VAC8, 0);
		zv_dec_init(&dq, &h, half ? ZV_VIEW_PL_HALF : ZV_VIEW_PL_FULL, ZV_MASK_VAC8, 0);
		dq.plane_off = po;
		memset(pal, 0, sizeof(pal)); memset(pln, 0, sizeof(pln));
		bad = 0;
		for (n = 0; n < 30; n++) {
			int key = n % 10 == 0;
			size_t len;
			make_frame(g, n);
			len = zve_frame(e, g, key, pay, sizeof(pay), NULL);
			zv_dec_video(&dp, pay, len, key, pal, w);
			zv_dec_video(&dq, pay, len, key, pln, st);
			for (y = 0; y < hh; y++)
				for (x = 0; x < w; x++) {
					int v = 0;
					for (k = 0; k < 4; k++) v |= ((pln[k * po + y * st + (x >> 3)] >> (x & 7)) & 1) << k;
					if (v != pal[y * w + x]) bad++;
				}
		}
		CHECK(bad == 0, "plane view, %s: %d pixels differ from the palette view", half ? "half" : "full", bad);
		zve_free(e);
	}
}

// A frame decoded a few blocks a call (step_blocks) is the frame decoded
// in one, in every view the board uses.
static void test_steps(void) {
	static uint8_t g[TW * TH], pay[1 << 16];
	static uint8_t whole[4 * TW * TH], parts[4 * TW * TH];
	static const int views[6] = { ZV_VIEW_MONO_FULL, ZV_VIEW_MONO_HALF, ZV_VIEW_PAL_HALF, ZV_VIEW_PAL_FULL,
	                              ZV_VIEW_PL_FULL, ZV_VIEW_PL_HALF };
	zv_header_t h;
	zve_opts_t o;
	zve_t *e;
	zv_dec_t da, db;
	int v, n, calls;
	memset(&h, 0, sizeof(h));
	h.w = TW; h.h = TH; h.fps_num = 24; h.fps_den = 1; h.frames = 40; h.refresh = 1;
	for (v = 0; v < 6; v++) {
		int w, hh, st, bad = 0;
		memset(&o, 0, sizeof(o));
		o.w = TW; o.h = TH; o.lambda = 30;
		e = zve_new(&o);
		zv_dec_init(&da, &h, views[v], ZV_MASK_VAC8, 0);
		zv_dec_init(&db, &h, views[v], ZV_MASK_VAC8, 0);
		zv_view_size(&da, &w, &hh, &st);
		da.plane_off = db.plane_off = st * hh;
		db.step_blocks = 37;
		memset(whole, 0, sizeof(whole)); memset(parts, 0, sizeof(parts));
		calls = 0;
		for (n = 0; n < 30; n++) {
			int key = n % 10 == 0, r;
			size_t len;
			make_frame(g, n);
			len = zve_frame(e, g, key, pay, sizeof(pay), NULL);
			zv_dec_video(&da, pay, len, key, whole, st);
			do { r = zv_dec_video(&db, pay, len, key, parts, st); calls++; } while (r == 2);
			if (r != 0 || memcmp(whole, parts, sizeof(whole))) bad++;
		}
		CHECK(bad == 0 && calls > 60, "view %d in steps: %d frames differ (%d calls)", views[v], bad, calls);
		zve_free(e);
	}
}

int main(void) {
	printf("zv_test\n");
	test_maths();
	test_header();
	test_palette();
	test_codec(ZV_MASK_BAYER8);
	test_codec(ZV_MASK_VAC8);
	test_fuzz();
	test_colour();
	test_colour_decode();
	test_planes();
	test_steps();
	(void)half_ref_of_full;
	printf("%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
