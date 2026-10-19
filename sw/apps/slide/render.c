/*
 * slide -- drawing a card. See render.h.
 */

#include "render.h"
#include "../../common/zpool.h"

#include <string.h>

#include "../../common/zpaths.h"
#include "../../common/zimg.h"
#include "../../common/zpaper.h"
#include "../../common/zimgjob.h"

extern const z_font_t z_font_5x8, z_font_6x12;
extern const uint8_t z_pattern_table[16][8];		// zwidget.c

static render_load_fn r_load;
static char r_dir[160];

// Working paths: a shape's outline, and the capsules of a stroke.
static zc_path_t p_shape, p_work;
static zc_pt_t *pts_shape, *pts_work;
static uint16_t sub_shape[64], sub_work[256];

void render_init(void *scratch, uint32_t bytes, render_load_fn load) {
	// a third of the scratch for paths, the rest for the edge table
	uint32_t pathb = bytes / 3;
	uint32_t n = pathb / sizeof(zc_pt_t);
	uint8_t *m = (uint8_t *)scratch;
	r_load = load;
	obj_free_hook = render_forget;
	pts_shape = (zc_pt_t *)m;
	pts_work = pts_shape + n / 4;
	zc_path_init(&p_shape, pts_shape, (int)(n / 4), sub_shape, 64);
	zc_path_init(&p_work, pts_work, (int)(n - n / 4), sub_work, 256);
	zc_scratch(m + pathb, bytes - pathb);
}

void render_set_dir(const char *dir) {
	size_t n = dir ? strlen(dir) : 0;
	if (n >= sizeof(r_dir)) n = sizeof(r_dir) - 1;
	memcpy(r_dir, dir ? dir : "", n);
	r_dir[n] = 0;
}

// -- fonts ---------------------------------------------------------------

typedef struct {
	char		name[16];
	zc_font_t	f;
	uint8_t		*data;
	bool		tried, ok;
} font_slot_t;

#define FONT_SLOTS 40
static font_slot_t fonts[FONT_SLOTS];
static zc_font_t f_mono5, f_mono6;
static bool builtins_ready;

static void builtins(void) {
	if (builtins_ready) return;
	zc_font_builtin(&f_mono5, &z_font_5x8, 7, "mono5x8");
	zc_font_builtin(&f_mono6, &z_font_6x12, 10, "mono6x12");
	builtins_ready = true;
}

static void join(char *out, int cap, const char *a, const char *b, const char *c) {
	int i = 0;
	while (a && *a && i < cap - 1) out[i++] = *a++;
	while (b && *b && i < cap - 1) out[i++] = *b++;
	while (c && *c && i < cap - 1) out[i++] = *c++;
	out[i] = 0;
}

static font_slot_t *slot_for(const char *name) {
	int i;
	font_slot_t *free_slot = NULL;
	for (i = 0; i < FONT_SLOTS; i++) {
		if (fonts[i].tried && !strcmp(fonts[i].name, name)) return &fonts[i];
		if (!fonts[i].tried && !free_slot) free_slot = &fonts[i];
	}
	if (!free_slot) return NULL;
	join(free_slot->name, sizeof(free_slot->name), name, NULL, NULL);
	free_slot->tried = true;
	free_slot->ok = false;
	if (r_load) {
		static const char *ext[2] = { ".zhf", ".zfn" };
		int k;
		for (k = 0; k < 2 && !free_slot->ok; k++) {
			char path[96];
			uint32_t len = 0;
			uint8_t *d;
			join(path, sizeof(path), Z_DIR_SYS_FONTS "/", name, ext[k]);
			d = r_load(path, &len);
			if (!d) continue;
			if (zc_font_open(&free_slot->f, d, len, name)) {
				free_slot->data = d;
				free_slot->ok = true;
			} else {
				pool_free(d);
			}
		}
	}
	return free_slot;
}

static const zc_font_t *font_try(const char *name) {
	font_slot_t *s;
	builtins();
	if (!strcmp(name, "mono5x8")) return &f_mono5;
	if (!strcmp(name, "mono6x12")) return &f_mono6;
	s = slot_for(name);
	return (s && s->ok) ? &s->f : NULL;
}

bool render_font_exists(const char *name) {
	return font_try(name) != NULL;
}

const zc_font_t *render_font_named(const char *name) {
	const zc_font_t *f = font_try(name);
	if (!f) f = font_try("sans");
	if (!f) f = &f_mono6;
	return f;
}

// The face for a family and style: "serif" bold italic is "serifbi" if
// there is one, then "serifb", and the rest is synthetic.
static const zc_font_t *face_for(const char *family, int style, int *synth) {
	static const char *suf[4][3] = {
		{ "", NULL, NULL },
		{ "b", NULL, NULL },
		{ "i", "o", NULL },
		{ "bi", "bo", NULL },
	};
	int want = ((style & ST_BOLD) ? 1 : 0) | ((style & ST_ITALIC) ? 2 : 0);
	int k;
	*synth = want;
	if (want) {
		for (k = 0; k < 2 && suf[want][k]; k++) {
			char nm[24];
			const zc_font_t *f;
			join(nm, sizeof(nm), family, suf[want][k], NULL);
			f = font_try(nm);
			if (f) { *synth = 0; return f; }
		}
		if (want == 3) {
			// bold italic: a bold face slanted, or an italic one made heavier
			char nm[24];
			const zc_font_t *f;
			join(nm, sizeof(nm), family, "b", NULL);
			if ((f = font_try(nm))) { *synth = 2; return f; }
			join(nm, sizeof(nm), family, "i", NULL);
			if ((f = font_try(nm))) { *synth = 1; return f; }
		}
	}
	return render_font_named(family);
}

void render_style(const deck_t *d, const obj_t *o, zc_style_t *st) {
	const char *family = (o->font < d->nfont) ? d->font[o->font] : "sans";
	int synth;
	memset(st, 0, sizeof(*st));
	st->font = face_for(family, o->style, &synth);
	st->size = ZC_I(o->size ? o->size : 16);
	st->bold = (synth & 1) ? 1 : 0;
	st->italic = (synth & 2) ? 1 : 0;
	st->underline = (o->style & ST_UNDERLINE) ? 1 : 0;
	// a bitmap face keeps its own size unless enlarged on purpose: a
	// size under its cell is its cell
	if (st->font->kind != ZC_FONT_STROKE && st->size < ZC_I(st->font->h))
		st->size = ZC_I(st->font->h);
}

// -- paint ---------------------------------------------------------------

static zc_paint_t fill_paint(const obj_t *o) {
	if (o->fill == FILL_PATTERN)
		return zc_pattern(z_pattern_table[o->fill_v & 15], 16, true);
	return zc_shade(o->fill_v);
}

static zc_paint_t tone_paint(int tone) {
	return zc_shade(tone);
}

// -- text ----------------------------------------------------------------

void render_text_layout(const deck_t *d, const obj_t *o, text_layout_t *L) {
	int x0, y0, x1, y1;
	zc_fix inner_h, total;
	const char *t = o->text ? o->text : "";
	memset(L, 0, sizeof(*L));
	render_style(d, o, &L->st);
	obj_bounds(o, &x0, &y0, &x1, &y1);
	L->x0 = ZC_I(x0 + o->pad);
	L->w = ZC_I(x1 - x0 - 2 * o->pad);
	if (L->w < ZC_I(1)) L->w = ZC_I(1);
	zc_text_metrics(&L->st, &L->asc, NULL, &L->line_h);
	L->n = zc_text_wrap(&L->st, t, (uint32_t)strlen(t), L->w, L->line, RENDER_MAX_LINES, NULL);
	inner_h = ZC_I(y1 - y0 - 2 * o->pad);
	total = L->line_h * L->n;
	L->top = ZC_I(y0 + o->pad);
	if (o->valign == VA_MIDDLE) L->top += (inner_h - total) / 2;
	else if (o->valign == VA_BOTTOM) L->top += inner_h - total;
}

zc_fix render_line_x(const obj_t *o, const text_layout_t *L, int i) {
	zc_fix w = (i >= 0 && i < L->n) ? L->line[i].width : 0;
	if (o->align == AL_CENTER) return L->x0 + (L->w - w) / 2;
	if (o->align == AL_RIGHT) return L->x0 + L->w - w;
	return L->x0;
}

static void draw_text(zc_target_t *t, const zc_xf_t *xf, const deck_t *d,
	const obj_t *o) {
	text_layout_t *L;
	static text_layout_t lay;		// big; one drawing at a time
	zc_paint_t ink = tone_paint(o->text_tone);
	int i;
	if (!o->text || !o->text[0]) return;
	L = &lay;
	render_text_layout(d, o, L);
	for (i = 0; i < L->n; i++) {
		zc_fix y = L->top + L->asc + L->line_h * i;
		zc_text_draw(t, xf, &L->st, render_line_x(o, L, i), y,
			o->text + L->line[i].start, L->line[i].len, &ink);
	}
}

// -- images ----------------------------------------------------------------

enum { IMG_NONE = 0, IMG_WANTED, IMG_LOADING, IMG_READY, IMG_FAILED, IMG_STOPPED };

struct img_cache {
	uint8_t		*gray;			// the decoded image, grey
	int			gw, gh;
	uint32_t	*bits;			// dithered at the size last shown
	int			bw, bh, bwpl;
	int8_t		contrast, bright;
	uint8_t		state;			// IMG_*
};

static bool path_is_abs(const char *p) { return p && p[0] == '/'; }

// -- loading, a slice at a time (zimgjob.h; docs/responsive.md) --------------
//
// Nothing here waits for a picture. Drawing one that is not decoded yet
// asks for it (it joins the queue) and draws its frame with a progress
// bar; the app's main loop runs render_load_step() when it has no
// message waiting, one picture at a time, and redraws a picture when it
// is done.

#define WANT_MAX 64

static z_imgjob_t job;
static obj_t *job_obj;				// the picture being loaded
static obj_t *wanted[WANT_MAX];		// those waiting, oldest first
static int nwanted;

static void unqueue(obj_t *o) {
	int i, k = 0;
	for (i = 0; i < nwanted; i++) if (wanted[i] != o) wanted[k++] = wanted[i];
	nwanted = k;
}

// Pictures recently let go of, by path: an object undone and redone,
// pasted, duplicated or on several cards gets its picture back at once
// instead of decoding it again.
#define RECENT_MAX 4
#define RECENT_BYTES (1200u * 1024u)
static struct {
	char		path[200];
	uint8_t		*gray;
	int			w, h;
	uint32_t	age;
} recent[RECENT_MAX];
static uint32_t recent_tick;

static void path_of(const obj_t *o, char *out, int n) {
	if (path_is_abs(o->src) || !r_dir[0]) join(out, n, o->src, NULL, NULL);
	else join(out, n, r_dir, "/", o->src);
}

static void recent_put(const char *path, uint8_t *gray, int w, int h) {
	int i, slot = 0;
	uint32_t total = 0;
	for (i = 0; i < RECENT_MAX; i++) {
		if (recent[i].gray && !strcmp(recent[i].path, path)) {
			// already kept (the same picture on two objects)
			pool_free(gray);
			recent[i].age = ++recent_tick;
			return;
		}
	}
	for (i = 0; i < RECENT_MAX; i++) {
		if (!recent[i].gray) { slot = i; break; }
		if (recent[i].age < recent[slot].age) slot = i;
	}
	pool_free(recent[slot].gray);
	recent[slot].gray = gray;
	recent[slot].w = w;
	recent[slot].h = h;
	recent[slot].age = ++recent_tick;
	join(recent[slot].path, sizeof(recent[slot].path), path, NULL, NULL);
	// within the budget: the oldest go first
	for (;;) {
		int old = -1;
		total = 0;
		for (i = 0; i < RECENT_MAX; i++) {
			if (!recent[i].gray) continue;
			total += (uint32_t)recent[i].w * (uint32_t)recent[i].h;
			if (i != slot && (old < 0 || recent[i].age < recent[old].age)) old = i;
		}
		if (total <= RECENT_BYTES || old < 0) break;
		pool_free(recent[old].gray);
		recent[old].gray = NULL;
	}
}

static uint8_t *recent_take(const char *path, int *w, int *h) {
	int i;
	for (i = 0; i < RECENT_MAX; i++) {
		if (recent[i].gray && !strcmp(recent[i].path, path)) {
			uint8_t *g = recent[i].gray;
			*w = recent[i].w;
			*h = recent[i].h;
			recent[i].gray = NULL;
			return g;
		}
	}
	return NULL;
}

void render_drop_recent(void) {
	int i;
	for (i = 0; i < RECENT_MAX; i++) { pool_free(recent[i].gray); recent[i].gray = NULL; }
}

static img_cache_t *image_of(obj_t *o) {
	img_cache_t *c = o->img;
	if (!c) {
		c = pool_zalloc(sizeof(img_cache_t));
		if (!c) return NULL;
		o->img = c;
	}
	if (c->state == IMG_NONE) {
		char path[256];
		if (!o->src || !o->src[0]) { c->state = IMG_FAILED; return c; }
		path_of(o, path, sizeof(path));
		c->gray = recent_take(path, &c->gw, &c->gh);
		if (c->gray) { c->state = IMG_READY; return c; }
		if (nwanted < WANT_MAX) { wanted[nwanted++] = o; c->state = IMG_WANTED; }
	}
	return c;
}

static void *job_alloc(size_t n) { return pool_alloc(n); }
static void job_free(void *p) { pool_free(p); }

bool render_loading(void) { return job_obj != NULL || nwanted > 0; }

obj_t *render_load_step(uint32_t ticks, bool *finished) {
	*finished = false;
	if (!job_obj) {
		char path[256];
		obj_t *o;
		if (!nwanted) return NULL;
		o = wanted[0];
		unqueue(o);
		path_of(o, path, sizeof(path));
		job_obj = o;
		o->img->state = IMG_LOADING;
		if (!z_imgjob_start(&job, path, job_alloc, job_free)) {
			o->img->state = IMG_FAILED;
			job_obj = NULL;
			*finished = true;
			return o;
		}
	}
	{
		obj_t *o = job_obj;
		img_cache_t *c = o->img;
		int st = z_imgjob_step(&job, ticks);
		if (st == Z_IMGJOB_RUNNING) return o;
		if (st == Z_IMGJOB_DONE) {
			c->gw = job.w;
			c->gh = job.h;
			c->gray = z_imgjob_take(&job);
			c->state = c->gray ? IMG_READY : IMG_FAILED;
		} else {
			c->state = st == Z_IMGJOB_STOPPED ? IMG_STOPPED : IMG_FAILED;
		}
		z_imgjob_end(&job);
		job_obj = NULL;
		*finished = true;
		return o;
	}
}

uint16_t render_load_progress(const char **name) {
	if (!job_obj) { if (name) *name = NULL; return 0; }
	if (name) {
		const char *p = job.path, *slash = strrchr(p, '/');
		*name = slash ? slash + 1 : p;
	}
	return z_imgjob_permille(&job);
}

int render_load_stop(void) {
	int n = 0, i;
	if (job_obj) {
		z_imgjob_stop(&job);
		z_imgjob_end(&job);
		job_obj->img->state = IMG_STOPPED;
		job_obj = NULL;
		n++;
	}
	for (i = 0; i < nwanted; i++) { wanted[i]->img->state = IMG_STOPPED; n++; }
	nwanted = 0;
	return n;
}

bool render_load_retry(obj_t *o) {
	img_cache_t *c = o->img;
	if (o->kind != OB_IMAGE || !c || (c->state != IMG_STOPPED && c->state != IMG_FAILED)) return false;
	c->state = IMG_NONE;
	image_of(o);
	return true;
}

static void want_one(obj_t *o, int mode, bool first) {
	img_cache_t *c;
	int i;
	if (o->kind != OB_IMAGE || ((o->flags & OBF_HIDDEN) && mode != RENDER_EDIT)) return;
	c = image_of(o);
	if (!first || !c || c->state != IMG_WANTED) return;
	// to the front of the queue
	unqueue(o);
	for (i = nwanted; i > 0; i--) wanted[i] = wanted[i - 1];
	wanted[0] = o;
	nwanted++;
}

void render_card_want(const deck_t *d, const card_t *c, int mode, bool first) {
	int i;
	// the card's own last, so that with `first` they end up at the front
	if (c->bg >= 0 && c->bg < d->nbg) {
		const card_t *b = d->bg[c->bg];
		for (i = b->nobj - 1; i >= 0; i--) want_one(b->obj[i], mode, first);
	}
	for (i = c->nobj - 1; i >= 0; i--) want_one(c->obj[i], mode, first);
}

bool render_card_has(const deck_t *d, const card_t *c, const obj_t *o) {
	int i;
	if (!o) return false;
	for (i = 0; i < c->nobj; i++) if (c->obj[i] == o) return true;
	if (c->bg >= 0 && c->bg < d->nbg) {
		const card_t *b = d->bg[c->bg];
		for (i = 0; i < b->nobj; i++) if (b->obj[i] == o) return true;
	}
	return false;
}

int render_image_state(const obj_t *o) {
	const img_cache_t *c = o->img;
	if (o->kind != OB_IMAGE) return RENDER_IMG_NONE;
	if (!c) return RENDER_IMG_WAITING;
	switch (c->state) {
	case IMG_READY: return RENDER_IMG_READY;
	case IMG_FAILED: return RENDER_IMG_FAILED;
	case IMG_STOPPED: return RENDER_IMG_STOPPED;
	default: return RENDER_IMG_WAITING;
	}
}

void render_forget(obj_t *o) {
	img_cache_t *c = o->img;
	if (o == job_obj) {
		z_imgjob_stop(&job);
		z_imgjob_end(&job);
		job_obj = NULL;
	}
	unqueue(o);
	if (!c) return;
	if (c->gray && o->src && o->src[0]) {
		char path[256];
		path_of(o, path, sizeof(path));
		recent_put(path, c->gray, c->gw, c->gh);
		c->gray = NULL;
	}
	pool_free(c->gray);
	pool_free(c->bits);
	pool_free(c);
	o->img = NULL;
}

bool render_image_size(obj_t *o, int *w, int *h) {
	img_cache_t *c = o->img;
	if (!c || c->state != IMG_READY) return false;
	*w = c->gw;
	*h = c->gh;
	return true;
}

// A picture not (yet) drawn: its frame, and -- loading or waiting -- a
// bar across the middle filled as far as it has got; failed: a cross.
static void image_placeholder(zc_target_t *t, const zc_xf_t *xf, const obj_t *o) {
	zc_paint_t ink = zc_shade(10), light = zc_shade(4), bar = zc_black();
	int x0, y0, x1, y1;
	const img_cache_t *c = o->img;
	obj_bounds(o, &x0, &y0, &x1, &y1);
	zc_path_set_xf(&p_shape, xf);
	zc_frame_rect(t, &p_shape, ZC_I(x0), ZC_I(y0), ZC_I(x1 - x0), ZC_I(y1 - y0), ZC_I(1), &ink);
	if (c && c->state != IMG_FAILED && c->state != IMG_STOPPED && t->fmt != ZC_REC) {
		int w = x1 - x0, h = y1 - y0;
		int bw = w * 2 / 3, bh = h / 12 < 3 ? 3 : h / 12 > 8 ? 8 : h / 12;
		int bx = x0 + (w - bw) / 2, by = y0 + (h - bh) / 2;
		int fill = o == job_obj ? (int)((int64_t)(bw - 2) * z_imgjob_permille(&job) / 1000) : 0;
		if (bw < 8 || h < 8) return;
		zc_frame_rect(t, &p_shape, ZC_I(bx), ZC_I(by), ZC_I(bw), ZC_I(bh), ZC_I(1), &bar);
		if (fill > 0) {
			zc_path_reset(&p_shape);
			zc_path_rect(&p_shape, ZC_I(bx + 1), ZC_I(by + 1), ZC_I(fill), ZC_I(bh - 2));
			zc_fill(t, &p_shape, ZC_NONZERO, &bar);
		}
		return;
	}
	zc_path_reset(&p_shape);
	zc_path_move(&p_shape, ZC_I(x0), ZC_I(y0));
	zc_path_line(&p_shape, ZC_I(x1), ZC_I(y1));
	zc_path_move(&p_shape, ZC_I(x1), ZC_I(y0));
	zc_path_line(&p_shape, ZC_I(x0), ZC_I(y1));
	// failed: a cross; stopped: a light one (Enter loads it again)
	zc_stroke(t, &p_shape, ZC_I(1), false, &p_work, c && c->state == IMG_STOPPED ? &light : &ink);
}

static void draw_image(zc_target_t *t, const zc_xf_t *xf, obj_t *o) {
	img_cache_t *c;
	int x0, y0, x1, y1;
	zc_fix dx0, dy0, dx1, dy1;
	obj_bounds(o, &x0, &y0, &x1, &y1);
	if (t->fmt == ZC_REC && o->src && o->src[0]) {
		// printing: the picture goes by its path, and the print service
		// decodes it at the size it prints (zpaper.h)
		char path[256];
		if (path_is_abs(o->src) || !r_dir[0]) join(path, sizeof(path), o->src, NULL, NULL);
		else join(path, sizeof(path), r_dir, "/", o->src);
		zc_xf_apply(xf, ZC_I(x0), ZC_I(y0), &dx0, &dy0);
		zc_xf_apply(xf, ZC_I(x1), ZC_I(y1), &dx1, &dy1);
		zp_image_file(t, path, o->contrast, o->bright, dx0, dy0, dx1 - dx0, dy1 - dy0);
		return;
	}
	c = image_of(o);
	if (!c || c->state != IMG_READY || !c->gray) { image_placeholder(t, xf, o); return; }
	zc_xf_apply(xf, ZC_I(x0), ZC_I(y0), &dx0, &dy0);
	zc_xf_apply(xf, ZC_I(x1), ZC_I(y1), &dx1, &dy1);
	if (dx1 < dx0) { zc_fix k = dx0; dx0 = dx1; dx1 = k; }
	if (dy1 < dy0) { zc_fix k = dy0; dy0 = dy1; dy1 = k; }
	if (t->fmt == ZC_GRAY || !zc_xf_is_axis(xf)) {
		zc_blit_gray(t, c->gray, c->gw, c->gw, c->gh, dx0, dy0, dx1 - dx0, dy1 - dy0);
		return;
	}
	{
		// on screen: dithered once at the size it is shown, on whole pixels
		int px = (dx0 + ZC_HALF) >> ZC_FRAC, py = (dy0 + ZC_HALF) >> ZC_FRAC;
		int pw = ((dx1 + ZC_HALF) >> ZC_FRAC) - px;
		int ph = ((dy1 + ZC_HALF) >> ZC_FRAC) - py;
		if (pw <= 0 || ph <= 0) return;
		if (pw > 4096 || ph > 4096) return;
		if (!c->bits || c->bw != pw || c->bh != ph || c->contrast != o->contrast ||
			c->bright != o->bright) {
			int wpl = (pw + 31) / 32;
			int16_t *row = pool_alloc((size_t)3 * (size_t)(pw + 2) * sizeof(int16_t));
			pool_free(c->bits);
			// one spare word: the blitter may read a word past the end
			c->bits = pool_alloc((size_t)wpl * (size_t)ph * 4 + 4);
			if (!c->bits || !row) {
				pool_free(row);
				pool_free(c->bits);
				c->bits = NULL;
				image_placeholder(t, xf, o);
				return;
			}
			zc_dither(c->gray, c->gw, c->gw, c->gh, c->bits, wpl, pw, ph, row,
				o->contrast ? 65536 + o->contrast * 655 : 0, o->bright * 2);
			pool_free(row);
			c->bw = pw;
			c->bh = ph;
			c->bwpl = wpl;
			c->contrast = o->contrast;
			c->bright = o->bright;
		}
		zc_blit_bits(t, c->bits, c->bwpl, c->bw, c->bh, ZC_I(px), ZC_I(py),
			ZC_I(c->bw), ZC_I(c->bh));
	}
}

// -- shapes ----------------------------------------------------------------

int render_arrow_len(int w) { return 7 + 3 * (w > 1 ? w : 1); }

// A filled arrowhead at (tx, ty) pointing away from (fx, fy).
static void arrowhead(zc_target_t *t, const zc_xf_t *xf, int fx, int fy,
	int tx, int ty, int w, const zc_paint_t *p) {
	int64_t dx = tx - fx, dy = ty - fy;
	int64_t len = (int64_t)zc_isqrt64((uint64_t)(dx * dx + dy * dy));
	zc_fix L = ZC_I(render_arrow_len(w)), H = (L * 4) / 10;
	zc_fix ux, uy;
	if (len == 0) return;
	ux = (zc_fix)((dx * 65536) / len);	// 16.16 unit vector
	uy = (zc_fix)((dy * 65536) / len);
	{
		zc_fix bx = ZC_I(tx) - (zc_fix)(((int64_t)ux * L) >> 16);
		zc_fix by = ZC_I(ty) - (zc_fix)(((int64_t)uy * L) >> 16);
		zc_fix nx = (zc_fix)(((int64_t)-uy * H) >> 16), ny = (zc_fix)(((int64_t)ux * H) >> 16);
		zc_path_set_xf(&p_shape, xf);
		zc_path_reset(&p_shape);
		zc_path_move(&p_shape, ZC_I(tx), ZC_I(ty));
		zc_path_line(&p_shape, bx + nx, by + ny);
		zc_path_line(&p_shape, bx - nx, by - ny);
		zc_fill(t, &p_shape, ZC_NONZERO, p);
	}
}

// Moves (tx, ty) back towards (fx, fy) by `by` units: a line stops
// inside its arrowhead instead of poking through the tip.
static void pull_back(int fx, int fy, int *tx, int *ty, int by) {
	int64_t dx = *tx - fx, dy = *ty - fy;
	int64_t len = (int64_t)zc_isqrt64((uint64_t)(dx * dx + dy * dy));
	if (len <= by) return;
	*tx = fx + (int)((dx * (len - by)) / len);
	*ty = fy + (int)((dy * (len - by)) / len);
}

static void draw_shape(zc_target_t *t, const zc_xf_t *xf, const obj_t *o) {
	int x0, y0, x1, y1;
	zc_paint_t fp = fill_paint(o), lp = tone_paint(o->line_tone);
	zc_fix X, Y, W, H, lw = ZC_I(o->line_w);
	obj_bounds(o, &x0, &y0, &x1, &y1);
	X = ZC_I(x0); Y = ZC_I(y0); W = ZC_I(x1 - x0); H = ZC_I(y1 - y0);
	zc_path_set_xf(&p_shape, xf);
	switch (o->kind) {
	case OB_TEXT:
	case OB_RECT:
		if (o->fill) {
			zc_path_reset(&p_shape);
			zc_path_rect(&p_shape, X, Y, W, H);
			zc_fill(t, &p_shape, ZC_NONZERO, &fp);
		}
		if (o->line_w) zc_frame_rect(t, &p_shape, X, Y, W, H, lw, &lp);
		break;
	case OB_RRECT:
		if (o->fill) {
			zc_path_reset(&p_shape);
			zc_path_rrect(&p_shape, X, Y, W, H, ZC_I(o->radius));
			zc_fill(t, &p_shape, ZC_NONZERO, &fp);
		}
		if (o->line_w) zc_frame_rrect(t, &p_shape, X, Y, W, H, ZC_I(o->radius), lw, &lp);
		break;
	case OB_OVAL:
		if (o->fill) {
			zc_path_reset(&p_shape);
			zc_path_ellipse(&p_shape, X + W / 2, Y + H / 2, W / 2, H / 2);
			zc_fill(t, &p_shape, ZC_NONZERO, &fp);
		}
		if (o->line_w) zc_frame_ellipse(t, &p_shape, X, Y, W, H, lw, &lp);
		break;
	case OB_LINE: {
		int ax = o->x, ay = o->y, bx = o->x + o->w, by = o->y + o->h;
		int al = render_arrow_len(o->line_w);
		if (!o->line_w) break;
		if (o->arrows & AR_END) pull_back(ax, ay, &bx, &by, (al * 7) / 10);
		if (o->arrows & AR_START) pull_back(bx, by, &ax, &ay, (al * 7) / 10);
		zc_path_reset(&p_shape);
		zc_path_move(&p_shape, ZC_I(ax), ZC_I(ay));
		zc_path_line(&p_shape, ZC_I(bx), ZC_I(by));
		zc_stroke(t, &p_shape, lw, false, &p_work, &lp);
		if (o->arrows & AR_END) arrowhead(t, xf, o->x, o->y, o->x + o->w, o->y + o->h, o->line_w, &lp);
		if (o->arrows & AR_START) arrowhead(t, xf, o->x + o->w, o->y + o->h, o->x, o->y, o->line_w, &lp);
		break;
	}
	case OB_POLY: {
		int i, n = o->npts;
		if (n < 2) break;
		if (o->closed && o->fill && n >= 3) {
			zc_path_reset(&p_shape);
			for (i = 0; i < n; i++) {
				if (!i) zc_path_move(&p_shape, ZC_I(o->pts[0]), ZC_I(o->pts[1]));
				else zc_path_line(&p_shape, ZC_I(o->pts[2 * i]), ZC_I(o->pts[2 * i + 1]));
			}
			zc_fill(t, &p_shape, ZC_NONZERO, &fp);
		}
		if (o->line_w) {
			int sx = o->pts[0], sy = o->pts[1];
			int ex = o->pts[2 * n - 2], ey = o->pts[2 * n - 1];
			bool arrows = !o->closed && o->arrows;
			int al = render_arrow_len(o->line_w);
			if (arrows && (o->arrows & AR_START)) pull_back(o->pts[2], o->pts[3], &sx, &sy, (al * 7) / 10);
			if (arrows && (o->arrows & AR_END)) pull_back(o->pts[2 * n - 4], o->pts[2 * n - 3], &ex, &ey, (al * 7) / 10);
			zc_path_reset(&p_shape);
			zc_path_move(&p_shape, ZC_I(sx), ZC_I(sy));
			for (i = 1; i < n - 1; i++)
				zc_path_line(&p_shape, ZC_I(o->pts[2 * i]), ZC_I(o->pts[2 * i + 1]));
			zc_path_line(&p_shape, ZC_I(ex), ZC_I(ey));
			zc_stroke(t, &p_shape, lw, o->closed != 0, &p_work, &lp);
			if (arrows && (o->arrows & AR_END))
				arrowhead(t, xf, o->pts[2 * n - 4], o->pts[2 * n - 3], o->pts[2 * n - 2], o->pts[2 * n - 1], o->line_w, &lp);
			if (arrows && (o->arrows & AR_START))
				arrowhead(t, xf, o->pts[2], o->pts[3], o->pts[0], o->pts[1], o->line_w, &lp);
		}
		break;
	}
	case OB_IMAGE:
		draw_image(t, xf, (obj_t *)o);
		if (o->line_w) zc_frame_rect(t, &p_shape, X, Y, W, H, lw, &lp);
		break;
	default:
		break;
	}
}

void render_obj(zc_target_t *t, const zc_xf_t *xf, const deck_t *d,
	const obj_t *o, int mode) {
	if ((o->flags & OBF_HIDDEN) && mode != RENDER_EDIT) return;
	draw_shape(t, xf, o);
	if (o->kind == OB_TEXT) draw_text(t, xf, d, o);
	if ((o->flags & OBF_HIDDEN) && mode == RENDER_EDIT) {
		// hidden, in the editor: there, but greyed over with a light screen
		static const uint8_t screen[8] = { 0x88, 0, 0x22, 0, 0x88, 0, 0x22, 0 };
		zc_paint_t p = zc_pattern(screen, 0, false);
		int x0, y0, x1, y1;
		obj_bounds(o, &x0, &y0, &x1, &y1);
		zc_path_set_xf(&p_shape, xf);
		zc_path_reset(&p_shape);
		zc_path_rect(&p_shape, ZC_I(x0), ZC_I(y0), ZC_I(x1 - x0 + 1), ZC_I(y1 - y0 + 1));
		zc_fill(t, &p_shape, ZC_NONZERO, &p);
	}
}

void render_card(zc_target_t *t, const zc_xf_t *xf, const deck_t *d,
	const card_t *c, int mode) {
	zc_paint_t white = zc_white();
	int i;
	zc_fix x0, y0, x1, y1;
	// the page itself, white, inside the clip
	zc_xf_apply(xf, 0, 0, &x0, &y0);
	zc_xf_apply(xf, ZC_I(d->page.gw), ZC_I(d->page.gh), &x1, &y1);
	{
		int ax = (int)((x0 < x1 ? x0 : x1) + ZC_HALF) >> ZC_FRAC;
		int ay = (int)((y0 < y1 ? y0 : y1) + ZC_HALF) >> ZC_FRAC;
		int bx = (int)((x0 < x1 ? x1 : x0) + ZC_HALF) >> ZC_FRAC;
		int by = (int)((y0 < y1 ? y1 : y0) + ZC_HALF) >> ZC_FRAC;
		zc_fill_rect_dev(t, ax, ay, bx - ax, by - ay, &white);
	}
	if (c->bg >= 0 && c->bg < d->nbg) {
		const card_t *b = d->bg[c->bg];
		for (i = 0; i < b->nobj; i++) render_obj(t, xf, d, b->obj[i], mode);
	}
	for (i = 0; i < c->nobj; i++) render_obj(t, xf, d, c->obj[i], mode);
}
