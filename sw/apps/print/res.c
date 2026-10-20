/*
 * print -- fonts and pictures. See res.h.
 */

#include "res.h"

#include <string.h>

#include "../../common/zpool.h"
#include "../../common/zimg.h"
#include "../../common/zfont.h"
#include "../../common/zpaper.h"
#include "../../common/zpaths.h"

#define RES_FONTS	24
#define RES_IMAGES	16

static res_load_fn r_load;
static uint32_t r_budget, r_used;

static struct {
	char		name[ZP_FONT_MAX];
	zc_font_t	font;
	uint8_t		*data;
	bool		ok;
} fonts[RES_FONTS];
static int nfonts;
static zc_font_t f_mono6, f_mono5;

static struct {
	char		path[ZP_PATH_MAX];
	int16_t		contrast, bright;
	uint8_t		*gray;
	int			w, h;
	bool		failed;
	uint32_t	used;		// when last asked for (for dropping the oldest)
} imgs[RES_IMAGES];
static uint32_t tick;

static int nmissing;
static char first_missing[ZP_PATH_MAX];

static void missing(const char *what) {
	if (!nmissing) {
		int i;
		for (i = 0; i < ZP_PATH_MAX - 1 && what[i]; i++) first_missing[i] = what[i];
		first_missing[i] = 0;
	}
	nmissing++;
}

int res_missing(char *first, int n) {
	int i;
	if (n > 0) {
		for (i = 0; i < n - 1 && first_missing[i]; i++) first[i] = first_missing[i];
		first[i] = 0;
	}
	return nmissing;
}

void res_init(res_load_fn load, uint32_t image_budget) {
	int i;
	for (i = 0; i < nfonts; i++) pool_free(fonts[i].data);
	for (i = 0; i < RES_IMAGES; i++) pool_free(imgs[i].gray);
	memset(fonts, 0, sizeof(fonts));
	memset(imgs, 0, sizeof(imgs));
	nfonts = 0;
	r_load = load;
	r_budget = image_budget;
	r_used = 0;
	nmissing = 0;
	first_missing[0] = 0;
	zc_font_builtin(&f_mono6, &z_font_6x12, 10, "mono6x12");
	zc_font_builtin(&f_mono5, &z_font_5x8, 7, "mono5x8");
}

const zc_font_t *res_fallback(void) {
	const zc_font_t *f;
	if (res_font(NULL, "sans", &f)) return f;
	return &f_mono6;
}

bool res_font(void *ctx, const char *name, const zc_font_t **out) {
	int i;
	(void)ctx;
	if (!strcmp(name, "mono6x12")) { *out = &f_mono6; return true; }
	if (!strcmp(name, "mono5x8")) { *out = &f_mono5; return true; }
	for (i = 0; i < nfonts; i++) {
		if (!strcmp(fonts[i].name, name)) {
			*out = &fonts[i].font;
			return fonts[i].ok;
		}
	}
	if (nfonts >= RES_FONTS || !r_load) return false;
	{
		static const char *const ext[2] = { ".zhf", ".zfn" };
		int k;
		char path[64];
		int n = 0;
		const char *dir = Z_DIR_SYS_FONTS "/";
		memset(&fonts[nfonts], 0, sizeof(fonts[nfonts]));
		for (i = 0; i < ZP_FONT_MAX - 1 && name[i]; i++) fonts[nfonts].name[i] = name[i];
		for (k = 0; k < 2 && !fonts[nfonts].ok; k++) {
			uint32_t len = 0;
			uint8_t *d;
			n = 0;
			for (i = 0; dir[i]; i++) path[n++] = dir[i];
			for (i = 0; name[i] && n < 50; i++) {
				char c = name[i];
				if (c == '/' || c == '.') { n = 0; break; }	// a name, not a path
				path[n++] = c;
			}
			if (!n) break;
			for (i = 0; ext[k][i]; i++) path[n++] = ext[k][i];
			path[n] = 0;
			d = r_load(path, &len);
			if (!d) continue;
			if (zc_font_open(&fonts[nfonts].font, d, len, name)) {
				fonts[nfonts].data = d;
				fonts[nfonts].ok = true;
			} else {
				pool_free(d);
			}
		}
		if (!fonts[nfonts].ok) missing(name);
		*out = &fonts[nfonts].font;
		return fonts[nfonts++].ok;
	}
}

// -- pictures ------------------------------------------------------------------

typedef struct { const uint8_t *p; uint32_t n, pos; } msrc_t;

static int msrc_read(void *ctx, uint8_t *buf, int len) {
	msrc_t *m = ctx;
	uint32_t k = m->n - m->pos;
	if ((uint32_t)len < k) k = (uint32_t)len;
	memcpy(buf, m->p + m->pos, k);
	m->pos += k;
	return (int)k;
}

typedef struct { z_img_t *im; int slot; bool oom; } sink_t;

static void gray_row(void *user, const uint8_t *g, int y, int w) {
	sink_t *s = user;
	int i = s->slot;
	if (!imgs[i].gray && !s->oom) {
		imgs[i].w = s->im->out_w;
		imgs[i].h = s->im->out_h;
		imgs[i].gray = pool_alloc((size_t)imgs[i].w * (size_t)imgs[i].h);
		if (!imgs[i].gray) s->oom = true;
		else memset(imgs[i].gray, 255, (size_t)imgs[i].w * (size_t)imgs[i].h);
	}
	if (imgs[i].gray && y >= 0 && y < imgs[i].h && w <= imgs[i].w)
		memcpy(imgs[i].gray + (size_t)y * imgs[i].w, g, (size_t)w);
}

static void drop(int i) {
	if (imgs[i].gray) r_used -= (uint32_t)imgs[i].w * (uint32_t)imgs[i].h;
	pool_free(imgs[i].gray);
	memset(&imgs[i], 0, sizeof(imgs[i]));
}

bool res_image(void *ctx, const char *path, int contrast, int bright,
	int maxw, int maxh, const uint8_t **pix, int *w, int *h) {
	int i, slot = -1;
	(void)ctx; (void)maxw; (void)maxh;
	tick++;
	for (i = 0; i < RES_IMAGES; i++) {
		if (imgs[i].path[0] && !strcmp(imgs[i].path, path) &&
			imgs[i].contrast == contrast && imgs[i].bright == bright) {
			imgs[i].used = tick;
			if (imgs[i].failed) return false;
			*pix = imgs[i].gray;
			*w = imgs[i].w;
			*h = imgs[i].h;
			return true;
		}
	}
	// a slot: an empty one, else the one asked for longest ago
	for (i = 0; i < RES_IMAGES; i++) if (!imgs[i].path[0]) { slot = i; break; }
	if (slot < 0) {
		slot = 0;
		for (i = 1; i < RES_IMAGES; i++) if (imgs[i].used < imgs[slot].used) slot = i;
		drop(slot);
	}
	for (i = 0; i < ZP_PATH_MAX - 1 && path[i]; i++) imgs[slot].path[i] = path[i];
	imgs[slot].contrast = (int16_t)contrast;
	imgs[slot].bright = (int16_t)bright;
	imgs[slot].used = tick;
	imgs[slot].failed = true;
	if (!r_load) return false;

	// room: drop the oldest until a full-size decode fits the budget
	while (r_used + (uint32_t)Z_IMG_MAX_W * Z_IMG_MAX_H > r_budget) {
		int old = -1;
		for (i = 0; i < RES_IMAGES; i++)
			if (i != slot && imgs[i].gray && (old < 0 || imgs[i].used < imgs[old].used)) old = i;
		if (old < 0) break;
		drop(old);
	}
	{
		uint32_t len = 0;
		uint8_t *data = r_load(path, &len);
		z_img_t im;
		msrc_t m;
		sink_t s;
		z_img_fmt_t fmt;
		if (!data) { missing(path); return false; }
		m.p = data; m.n = len; m.pos = 0;
		s.im = &im; s.slot = slot; s.oom = false;
		fmt = z_img_sniff(data, (int)(len < 64 ? len : 64));
		memset(&im, 0, sizeof(im));
		im.read = msrc_read;
		im.ctx = &m;
		im.doc_w = Z_IMG_MAX_W;
		im.doc_h = Z_IMG_MAX_H;
		if (z_img_decode_gray(&im, fmt, gray_row, &s) != Z_IMG_OK && !imgs[slot].gray) s.oom = true;
		pool_free(data);
		if (s.oom || !imgs[slot].gray) {
			pool_free(imgs[slot].gray);
			imgs[slot].gray = NULL;
			missing(path);
			return false;
		}
	}
	r_used += (uint32_t)imgs[slot].w * (uint32_t)imgs[slot].h;
	imgs[slot].failed = false;

	// slide's contrast and brightness, as zc_dither() applies them
	if (contrast || bright) {
		int32_t gain = contrast ? 65536 + contrast * 655 : 0;
		uint32_t n = (uint32_t)imgs[slot].w * (uint32_t)imgs[slot].h, k;
		uint8_t *g = imgs[slot].gray;
		for (k = 0; k < n; k++) {
			int v = g[k];
			if (gain) v = 128 + (int)((((int64_t)(v - 128)) * gain) >> 16);
			v += bright * 2;
			g[k] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
		}
	}
	*pix = imgs[slot].gray;
	*w = imgs[slot].w;
	*h = imgs[slot].h;
	return true;
}
