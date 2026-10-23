/*
 * view -- its screen face. See vscreen.h.
 */

#include <string.h>

#include "../../common/zimg.h"
#include "../../common/zquant.h"
#include "../../common/zfont.h"
#include "../../common/games/zarcade.h"

#ifndef AR_HOST_TESTS
#include "../../common/zeitlos.h"
#include "../../common/zwm.h"
#endif

#include "vscreen.h"

#if !Z_IMG_HAVE_RGB
#error "view's screen face needs zimg.c's colour rows: build it with -DZ_IMG_HAVE_RGB=1"
#endif

#define VS_W   AR_W
#define VS_H   AR_H
#define VS_WPL 11                       /* 10 words a row + 1 the blitter may read */

/* The fitted picture, RGB444, and where in the screen it is; outside
 * it, black. */
static uint16_t rgb[VS_H][VS_W];
static int fx0, fy0, fw, fh;
static bool have;

/* Its colours, counted once. */
static uint32_t hist[4096];

/* What is shown: the planes of each pixel's index (colour), or the
 * dithered picture (black and white), and the palette. */
static uint32_t planes[8][VS_H][VS_WPL];
static uint32_t mono[VS_H][VS_WPL];
static uint16_t pal[256];
static uint8_t lut[4096];
static uint8_t lutf[4096];              /* every colour: for the diffusion */
static int16_t err[2][VS_W + 3][3];     /* its error, this row and the next */
static int shown_n;                     /* the colours the planes were made for */

/* -- fitting -- */

static void fit(int sw, int sh) {
	if (sw <= VS_W && sh <= VS_H) {
		fw = sw;
		fh = sh;
	} else if ((long)sw * VS_H > (long)sh * VS_W) {
		fw = VS_W;
		fh = (int)((long)sh * VS_W / sw);
	} else {
		fh = VS_H;
		fw = (int)((long)sw * VS_H / sh);
	}
	if (fw < 1) fw = 1;
	if (fh < 1) fh = 1;
	fx0 = (VS_W - fw) / 2;
	fy0 = (VS_H - fh) / 2;
}

/* The source pixel output pixel o (0..n-1) of a fit of `src` into `n`
 * samples: the one under its centre. */
static int sample(int o, int n, int src) {
	return (int)(((long)(2 * o + 1) * src) / (2L * n));
}

typedef struct {
	z_img_t *im;
	bool fitted;
} pass_t;

static void row_cb(void *user, const uint8_t *rgba, int y, int w) {
	pass_t *p = (pass_t *)user;
	int oy, ox, sh;
	if (!p->fitted) {
		fit(p->im->out_w, p->im->out_h);
		p->fitted = true;
	}
	sh = p->im->out_h;
	/* the output rows that sample this source row: around y * fh / sh */
	oy = (int)((long)y * fh / sh) - 1;
	if (oy < 0) oy = 0;
	for (; oy < fh; oy++) {
		int sy = sample(oy, fh, sh);
		if (sy < y) continue;
		if (sy > y) break;
		for (ox = 0; ox < fw; ox++) {
			int sx = sample(ox, fw, w);
			const uint8_t *q = rgba + sx * 4;
			uint16_t c = q[3] < 128 ? 0 :
				(uint16_t)(((q[0] >> 4) << 8) | ((q[1] >> 4) << 4) | (q[2] >> 4));
			rgb[fy0 + oy][fx0 + ox] = c;
		}
	}
}

static void count(void) {
	int x, y;
	memset(hist, 0, sizeof(hist));
	for (y = 0; y < fh; y++)
		for (x = 0; x < fw; x++) hist[rgb[fy0 + y][fx0 + x]]++;
}

/* Black and white: the luminance through a 4x4 ordered dither,
 * anchored to the screen. */
static void make_mono(void) {
	static const uint8_t bayer[4][4] = {
		{ 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 }
	};
	int x, y;
	memset(mono, 0, sizeof(mono));
	for (y = 0; y < VS_H; y++)
		for (x = 0; x < VS_W; x++)
			if (zq_lum(rgb[y][x]) > bayer[y & 3][x & 3])
				mono[y][x >> 5] |= 1u << (x & 31);
}

bool vs_load(vs_src_t *src) {
	z_img_t im;
	uint8_t hdr[16];
	z_img_fmt_t fmt;
	pass_t p;
	int n, rv;

	vs_clear();
	if (!src->rewind(src->ctx)) return false;
	n = src->read(src->ctx, hdr, sizeof(hdr));
	fmt = z_img_sniff(hdr, n);
	if (fmt == Z_IMG_FMT_NONE) return false;
	if (!src->rewind(src->ctx)) return false;
	memset(&im, 0, sizeof(im));
	im.read = src->read;
	im.ctx = src->ctx;
	im.doc_w = Z_IMG_MAX_W;
	im.doc_h = Z_IMG_MAX_H;
	p.im = &im;
	p.fitted = false;
	rv = z_img_decode_rgba(&im, fmt, row_cb, &p);
	if (!p.fitted) return false;
	/* a picture cut short keeps what was decoded, as the desktop face
	 * does */
	(void)rv;
	count();
	make_mono();
	have = true;
	shown_n = 0;
	return true;
}

void vs_from_bits(const uint32_t *bits, int wpl, int w, int h) {
	int ox, oy;
	vs_clear();
	if (w <= 0 || h <= 0) return;
	fit(w, h);
	for (oy = 0; oy < fh; oy++) {
		int sy = sample(oy, fh, h);
		for (ox = 0; ox < fw; ox++) {
			int sx = sample(ox, fw, w);
			bool on = (bits[sy * wpl + (sx >> 5)] >> (sx & 31)) & 1u;
			rgb[fy0 + oy][fx0 + ox] = on ? 0xfff : 0x000;
		}
	}
	count();
	make_mono();
	have = true;
	shown_n = 0;
}

void vs_clear(void) {
	memset(rgb, 0, sizeof(rgb));
	memset(mono, 0, sizeof(mono));
	fw = fh = 0;
	have = false;
	shown_n = 0;
}

/* -- colours for the screen as it is now -- */

static int planes_for(int n) {
	int k = 0;
	while ((1 << k) < n) k++;
	return k;
}

/* The picture in n colours: its own (or median cut's), the border the
 * darkest of them, every pixel's index a bit in each plane. */
static void quantize(int n) {
	int i, x, y, k, nk, dark = 0, np;
	bool diffuse;
	np = zq_median_cut(hist, pal, n);
	if (np < 1) { pal[0] = 0x000; np = 1; }
	for (i = np; i < n; i++) pal[i] = pal[0];
	/* every colour of the picture is shown as it is: no diffusion */
	for (i = 0, diffuse = false; i < 4096 && !diffuse; i++)
		if (hist[i]) {
			int j;
			for (j = 0; j < np && pal[j] != i; j++) ;
			if (j == np) diffuse = true;
		}
	if (diffuse) zq_nearest_lut(lutf, pal, np, NULL);
	else zq_nearest_lut(lut, pal, np, hist);
	for (i = 1; i < np; i++)
		if (zq_lum(pal[i]) < zq_lum(pal[dark])) dark = i;
	nk = planes_for(n);
	memset(planes, 0, sizeof(planes));
	/* Floyd-Steinberg in RGB444 sixteenths, when the picture has more
	 * colours than are shown: the error a pixel's nearest colour makes
	 * goes to its neighbours to come, so 8 or 32 colours read as more.
	 * Inside the picture only; its border is the darkest colour. */
	memset(err, 0, sizeof(err));
	for (y = 0; y < VS_H; y++) {
		int16_t (*cur)[3] = err[y & 1], (*nxt)[3] = err[(y + 1) & 1];
		memset(nxt, 0, sizeof(err[0]));
		for (x = 0; x < VS_W; x++) {
			bool in = x >= fx0 && x < fx0 + fw && y >= fy0 && y < fy0 + fh;
			int c;
			if (!in) {
				c = dark;
			} else if (!diffuse) {
				c = lut[rgb[y][x]];
			} else {
				int v[3], q[3], ch;
				uint16_t p = rgb[y][x], key;
				for (ch = 0; ch < 3; ch++) {
					v[ch] = (int)((p >> (8 - 4 * ch)) & 15) * 17 + cur[x + 1][ch] / 16;
					if (v[ch] < 0) v[ch] = 0;
					if (v[ch] > 255) v[ch] = 255;
				}
				key = (uint16_t)((((v[0] + 8) / 17) << 8) | (((v[1] + 8) / 17) << 4) | ((v[2] + 8) / 17));
				c = lutf[key];
				for (ch = 0; ch < 3; ch++) {
					q[ch] = v[ch] - (int)((pal[c] >> (8 - 4 * ch)) & 15) * 17;
					/* 7/16 right, 3/16 below left, 5/16 below, 1/16 below right;
					 * kept in sixteenths of a level */
					cur[x + 2][ch] = (int16_t)(cur[x + 2][ch] + q[ch] * 7);
					nxt[x][ch] = (int16_t)(nxt[x][ch] + q[ch] * 3);
					nxt[x + 1][ch] = (int16_t)(nxt[x + 1][ch] + q[ch] * 5);
					nxt[x + 2][ch] = (int16_t)(nxt[x + 2][ch] + q[ch]);
				}
			}
			for (k = 0; k < nk; k++)
				if ((c >> k) & 1) planes[k][y][x >> 5] |= 1u << (x & 31);
		}
	}
	for (i = 0; i < n; i++) ar_pal(i, pal[i]);
	shown_n = n;
}

/* After a mode change, or the first time: colours for what is shown. */
static void colours_now(void) {
	if (ar_color() && ar_colours() != shown_n) quantize(ar_colours());
	ar_dirty_all();
}

/* -- the screen -- */

static void paint(void *user, const ar_rect_t *r) {
	int k, nk;
	(void)user;
	if (!ar_color()) {
		ar_bitmap_opaque(mono, VS_WPL * 4, r->x, r->y, r->x, r->y, r->w, r->h);
		return;
	}
	nk = planes_for(ar_colours());
	for (k = 0; k < nk; k++)
		ar_plane(k, planes[k], VS_WPL * 4, r->x, r->y, r->x, r->y, r->w, r->h);
}

static const char help_text[] =
	"The picture on a 320x240 screen, in colour where the board "
	"has it.\n\n"
	"F10\tColour off: back to the window\n"
	"Esc\tBack to the window\n"
	"F11\tFull screen, or back\n"
	"C\tFull screen: colour on or off\n\n"
	"Full screen shows up to 256 colours; the window, 32 (8 on some "
	"boards). JPEG pictures are in grey.";

static ar_desc_t desc = {
	.name = "view",
	.help_title = "view help",
	.help_text = help_text,
	.help_doc = "view_app",
	.paint = paint,
	.pointer = true,
};

static int result;

uint32_t vs_window_flags(void) { return ar_window_flags(&desc); }

#ifndef AR_HOST_TESTS
/* The titlebar's other icons, while the screen is up: open goes back to
 * the desktop face and its file box. */
static void on_msg(void *mp) {
	z_msg_t *m = (z_msg_t *)mp;
	if (m->subject == Z_WM_TITLEBAR_ICON &&
	    Z_WM_UNPACK_TBICON_KIND(m->obj.val.uint32) == Z_WM_TBICON_OPEN)
		result = VS_OPEN;
}
#endif

int vs_run(void *win, bool full, const char *name) {
	(void)name;
	result = -1;
	ar_attach(win, &desc);
#ifndef AR_HOST_TESTS
	ar_msg_hook(on_msg);
#endif
	ar_set_colours(256);
	shown_n = 0;
	if (full) ar_set_fullscreen(true);
	colours_now();
	while (result < 0) {
		ar_event_t e;
		while (ar_poll(&e)) {
			if (e.type == AR_EV_QUIT) result = VS_QUIT;
			else if (e.type == AR_EV_DESKTOP) result = VS_DESKTOP;
			else if (e.type == AR_EV_MODE) colours_now();
			else if (e.type == AR_EV_KEY && e.pressed && e.key == 0x1b &&
			         !ar_fullscreen())
				result = VS_DESKTOP;
			if (result >= 0) break;
		}
		if (result >= 0) break;
		ar_frame();
	}
	ar_msg_hook(NULL);
	ar_detach();
	return result;
}
