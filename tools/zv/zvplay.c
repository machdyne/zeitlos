// zvplay -- the Linux player for ZV files (docs/zv.md, "zvplay").
//
//   zvplay [options] FILE.zv               play (SDL2 builds)
//   zvplay --info FILE.zv                  what is in it
//   zvplay --png DIR [--every N] FILE.zv   stills of a view
//   zvplay --compare FRAME OUT.png FILE.zv one frame in every view
//   zvplay --y4m OUT.y4m [--wav OUT.wav] FILE.zv
//                                          the whole film as a view, to
//                                          make a preview with ffmpeg
//
// It draws with zv.c, the board's own decoder, and shows what a
// Zeitlos screen would show: the 640x480 screen, with the window, the
// desktop's full screen, or game mode doubled. Frames are timed as on
// the board: each is shown at the 60 Hz refresh nearest its time on the
// audio clock.

#define ZV_WANT_BN64
#include "zv.h"
#include "zv_masks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef ZV_SDL
#include <SDL.h>
#endif

#define SW 640
#define SH 480

// ---- the file -----------------------------------------------------------

typedef struct {
	FILE *f;
	zv_header_t h;
	uint32_t *idx;              // frame, offset pairs
	uint8_t *buf;
	uint32_t next;              // frame of the next record read
} zvf_t;

static int zvf_open(zvf_t *z, const char *path) {
	uint8_t hb[ZV_HDR_SIZE];
	uint32_t i;
	memset(z, 0, sizeof(*z));
	z->f = fopen(path, "rb");
	if (!z->f) { perror(path); return -1; }
	if (fread(hb, 1, ZV_HDR_SIZE, z->f) != ZV_HDR_SIZE || zv_parse_header(hb, ZV_HDR_SIZE, &z->h)) {
		fprintf(stderr, "zvplay: %s: not a ZV file\n", path);
		return -1;
	}
	z->idx = calloc(z->h.index_n * 2 + 2, sizeof(uint32_t));
	if (z->h.index_n) {
		uint8_t *t = malloc(z->h.index_n * 8);
		fseek(z->f, (long)z->h.index_off, SEEK_SET);
		if (fread(t, 8, z->h.index_n, z->f) != z->h.index_n) { fprintf(stderr, "zvplay: short index\n"); return -1; }
		for (i = 0; i < z->h.index_n * 2; i++)
			z->idx[i] = (uint32_t)t[i * 4] | ((uint32_t)t[i * 4 + 1] << 8) | ((uint32_t)t[i * 4 + 2] << 16) | ((uint32_t)t[i * 4 + 3] << 24);
		free(t);
	}
	z->buf = malloc(z->h.max_rec + 16);
	fseek(z->f, ZV_HDR_SIZE, SEEK_SET);
	return 0;
}

// the record of frame z->next, or NULL at the end
static const uint8_t *zvf_read(zvf_t *z, zv_rec_t *r) {
	uint8_t hb[ZV_REC_HDR];
	uint32_t size;
	if (z->next >= z->h.frames) return NULL;
	if (fread(hb, 1, ZV_REC_HDR, z->f) != ZV_REC_HDR) return NULL;
	size = (uint32_t)hb[0] | ((uint32_t)hb[1] << 8) | ((uint32_t)hb[2] << 16) | ((uint32_t)hb[3] << 24);
	if (size < ZV_REC_HDR || size > z->h.max_rec) return NULL;
	memcpy(z->buf, hb, ZV_REC_HDR);
	if (fread(z->buf + ZV_REC_HDR, 1, size - ZV_REC_HDR, z->f) != size - ZV_REC_HDR) return NULL;
	if (zv_split_record(&z->h, z->buf, size, r)) return NULL;
	z->next++;
	return z->buf;
}

// to the seek point at or before `frame`; returns its frame
static uint32_t zvf_seek(zvf_t *z, uint32_t frame) {
	uint32_t i, best = 0, off = ZV_HDR_SIZE;
	for (i = 0; i < z->h.index_n; i++)
		if (z->idx[i * 2] <= frame) { best = z->idx[i * 2]; off = z->idx[i * 2 + 1]; }
	fseek(z->f, (long)off, SEEK_SET);
	z->next = best;
	return best;
}

// ---- modes: what the screen shows ----------------------------------------

enum {
	M_WINDOW,       // the window: half size, mono, 1:1
	M_DESKFULL,     // the desktop's full screen: full size, mono
	M_GAMEMONO,     // game mode: half size, mono, doubled
	M_GAMEGREY,     // game mode: half size, 256-colour greys, doubled
	M_REF,          // reference: the grey picture, undithered
	M_FS,           // design B preview: Floyd-Steinberg (not on the board)
	M_BN64,         // design B preview: 64x64 blue noise (not on the board)
	M_GAMECOL,      // game mode: half size, the colour layer's 256 colours, doubled
	M_WINCOL,       // the window in 32 colours (the colour window): mono, tinted a cell at a time
	M_COUNT
};

static const char *mode_name[M_COUNT] = {
	"window (half, mono)", "desktop full screen (mono)", "game mode (mono)",
	"game mode (256 greys)", "reference grey", "free dither: Floyd-Steinberg",
	"free dither: 64x64 blue noise", "game mode (256 colours, the colour layer)",
	"the window in 32 colours (the colour window)",
};
static const char *mode_key[M_COUNT] = { "window", "full", "game", "grey", "ref", "fs", "bn64", "colour", "wcolour" };

static int greys_kind = ZV_GREYS_TRUE;

static int mode_view(int m) {
	switch (m) {
	case M_WINDOW: case M_GAMEMONO: case M_WINCOL: return ZV_VIEW_MONO_HALF;
	case M_DESKFULL: return ZV_VIEW_MONO_FULL;
	case M_GAMEGREY: case M_GAMECOL: return ZV_VIEW_PAL_HALF;
	default: return ZV_VIEW_GREY_FULL;
	}
}

// phosphors, as the desktop offers them: ink (0) and light (1)
static const uint32_t phos[4][2] = {
	{ 0x000000, 0xffffff },     // white
	{ 0x000000, 0xffb000 },     // amber
	{ 0x000000, 0x33ff66 },     // green
	{ 0x202020, 0xf2ecdc },     // paper
};
static const char *phos_name[4] = { "white", "amber", "green", "paper" };

typedef struct {
	zvf_t z;
	zv_dec_t dec;
	int mode, mask, phosphor, gamma;
	uint8_t *pic;               // the view's picture
	int pw, ph, ps;
	uint16_t pal[ZV_PAL_MAX];
	uint32_t shown;             // frame in pic (+1), 0 none
	// the colour layer (zv.h), all of it in memory
	uint8_t *col;               // the section, or NULL
	size_t col_len;
	zv_col_header_t ch;
	uint8_t cls[ZV_MAX_BW * ZV_MAX_BH * 4];
	int col_group;              // group cls is in (-1 none)
	uint32_t col_next;          // its next frame
	size_t col_at;              // and where that frame's runs are
	uint16_t colpal[256];
	uint8_t *tmp;               // design B previews
	int16_t *err;
} play_t;

static void set_mode(play_t *p, int mode, int mask) {
	int v = mode_view(mode);
	p->mode = mode;
	p->mask = mask;
	if (p->dec.view != v || p->dec.mask != mask || !p->pic) {
		zv_dec_set_view(&p->dec, v, mask);
		zv_view_size(&p->dec, &p->pw, &p->ph, &p->ps);
		free(p->pic);
		p->pic = calloc((size_t)p->ps * p->ph, 1);
		p->shown = 0;
	}
}

static int play_open(play_t *p, const char *path, int mode, int mask, int gamma) {
	memset(p, 0, sizeof(*p));
	p->gamma = gamma;
	if (zvf_open(&p->z, path)) return -1;
	zv_dec_init(&p->dec, &p->z.h, mode_view(mode), mask, p->gamma);
	if (greys_kind != ZV_GREYS_TRUE) zv_dec_set_greys(&p->dec, greys_kind);
	zv_grey_palette(p->pal, ZV_PAL_MAX, greys_kind);
	p->col_group = -1;
	if ((p->z.h.flags & ZV_F_COLOUR) && p->z.h.colour_off) {
		long end;
		fseek(p->z.f, 0, SEEK_END);
		end = ftell(p->z.f);
		if (end > (long)p->z.h.colour_off) {
			p->col_len = (size_t)(end - (long)p->z.h.colour_off);
			p->col = malloc(p->col_len);
			fseek(p->z.f, (long)p->z.h.colour_off, SEEK_SET);
			if (fread(p->col, 1, p->col_len, p->z.f) != p->col_len ||
			    zv_col_parse_header(p->col, p->col_len, &p->ch) ||
			    ZV_COL_HDR + (size_t)p->ch.groups * 8 > p->col_len) {
				fprintf(stderr, "zvplay: bad colour layer, ignored\n");
				free(p->col); p->col = NULL;
			}
		}
		fseek(p->z.f, ZV_HDR_SIZE, SEEK_SET);
	}
	p->tmp = malloc((size_t)p->z.h.w * p->z.h.h);
	p->err = malloc(((size_t)p->z.h.w + 2) * 2 * sizeof(int16_t));
	p->dec.view = -1;
	set_mode(p, mode, mask);
	return 0;
}

static uint32_t col_rd32(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }

// the colour layer's cells for frame f (from its group's first frame if
// need be), into the picture's high nibbles, and the group's palette
static void col_frame(play_t *p, uint32_t f) {
	uint32_t g = 0, i;
	if (!p->col) return;
	for (i = 0; i < p->ch.groups; i++)
		if (col_rd32(p->col + ZV_COL_HDR + i * 8) <= f) g = i;
	if ((int)g != p->col_group || f < p->col_next) {
		uint32_t off = col_rd32(p->col + ZV_COL_HDR + g * 8 + 4);
		if (off + ZV_COL_TABLE > p->col_len) return;
		p->col_group = (int)g;
		p->col_next = col_rd32(p->col + ZV_COL_HDR + g * 8);
		p->col_at = off + ZV_COL_TABLE;
		zv_col_palette((const int8_t *)(p->col + off), p->colpal);
	}
	while (p->col_next <= f) {
		int r = zv_col_frame(p->col + p->col_at, p->col_len - p->col_at, p->cls, p->ch.cw * p->ch.ch, NULL);
		if (r < 0) { fprintf(stderr, "zvplay: frame %u: bad colour record\n", p->col_next); return; }
		p->col_at += (size_t)r;
		p->col_next++;
	}
}

// decode the next record into pic; returns its frame + 1, 0 at the end
static uint32_t play_step(play_t *p, zv_rec_t *rec) {
	zv_rec_t r;
	uint32_t f = p->z.next;
	if (!zvf_read(&p->z, &r)) return 0;
	if (zv_dec_video(&p->dec, r.video, r.video_len, r.flags & ZV_RF_KEY, p->pic, p->ps) < 0) {
		fprintf(stderr, "zvplay: frame %u: bad record\n", f);
		return 0;
	}
	if (rec) *rec = r;
	p->shown = f + 1;
	return f + 1;
}

// show frame `f` in the current mode (from its seek point)
static int play_goto(play_t *p, uint32_t f) {
	if (f >= p->z.h.frames) f = p->z.h.frames - 1;
	if (!p->shown || f + 1 < p->shown || f + 1 > p->shown + 48) {
		uint32_t k = zvf_seek(&p->z, zv_start_for(&p->z.h, f));
		zv_dec_seek(&p->dec, k);
		memset(p->pic, 0, (size_t)p->ps * p->ph);
		p->shown = 0;
	}
	while (!p->shown || p->shown < f + 1)
		if (!play_step(p, NULL)) return -1;
	return 0;
}

// ---- drawing the screen ------------------------------------------------------

// the grey picture dithered freely (design B previews), into tmp (0/1)
static void free_dither(play_t *p) {
	int w = p->z.h.w, h = p->z.h.h, x, y;
	const uint16_t *L = p->dec.lin;
	if (p->mode == M_BN64) {
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++) {
				uint32_t r = zv_mask_bn64[(y & 63) * 64 + (x & 63)];
				p->tmp[y * w + x] = (uint32_t)L[p->pic[y * w + x]] * 4096u > (2 * r + 1) * 2048u;
			}
		return;
	}
	// Floyd-Steinberg in linear light, serpentine
	{
		int32_t *e0 = calloc((size_t)w + 2, sizeof(int32_t)), *e1 = calloc((size_t)w + 2, sizeof(int32_t));
		for (y = 0; y < h; y++) {
			int dir = (y & 1) ? -1 : 1, xs = dir > 0 ? 0 : w - 1, k;
			for (k = 0; k < w; k++) {
				int xx = xs + k * dir;
				int32_t v = (int32_t)L[p->pic[y * w + xx]] + e0[xx + 1] / 16;
				int on = v > 2048;
				int32_t er = v - (on ? 4096 : 0);
				p->tmp[y * w + xx] = (uint8_t)on;
				if (xx + dir >= 0 && xx + dir < w) e0[xx + dir + 1] += er * 7;
				e1[xx - dir + 1] += er * 3;
				e1[xx + 1] += er * 5;
				e1[xx + dir + 1] += er * 1;
			}
			memcpy(e0, e1, ((size_t)w + 2) * sizeof(int32_t));
			memset(e1, 0, ((size_t)w + 2) * sizeof(int32_t));
		}
		free(e0); free(e1);
	}
}

// the picture as RGB: w x h at scale 1 (the view's own size)
static void pic_rgb(play_t *p, uint32_t *rgb, int *ow, int *oh) {
	int x, y;
	const uint32_t *ph = phos[p->phosphor];
	*ow = p->pw; *oh = p->ph;
	switch (p->mode) {
	case M_WINDOW: case M_GAMEMONO: case M_DESKFULL:
		for (y = 0; y < p->ph; y++)
			for (x = 0; x < p->pw; x++)
				rgb[y * p->pw + x] = ph[(p->pic[y * p->ps + (x >> 3)] >> (x & 7)) & 1];
		break;
	case M_WINCOL: {
		// as cinema shows it: a pixel is black or its cell's class at
		// the ink's level (0x888: the desktop's white at the default
		// brightness, the palette's colours as they are -- dim here, as
		// in zsys, beside a desktop drawn at full white)
		int lv = 8, cs = p->col ? p->ch.cell / 2 : 1;
		if (p->col && p->shown) col_frame(p, p->shown - 1);
		for (y = 0; y < p->ph; y++)
			for (x = 0; x < p->pw; x++) {
				int on = (p->pic[y * p->ps + (x >> 3)] >> (x & 7)) & 1, k = 0;
				uint16_t c;
				uint32_t r, g, b;
				if (!on) { rgb[y * p->pw + x] = 0; continue; }
				if (p->col) k = p->cls[(y / cs) * p->ch.cw + x / cs];
				c = k && p->col ? p->colpal[k * 16 + lv] : 0x888;
				r = ((c >> 8) & 15) * 17; g = ((c >> 4) & 15) * 17; b = (c & 15) * 17;
				rgb[y * p->pw + x] = (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
			}
		break;
	}
	case M_GAMEGREY: case M_GAMECOL:
		// the classes into the high nibbles: as the board would, after
		// the base layer's decode (which leaves them alone)
		if (p->mode == M_GAMECOL && p->col && p->shown) {
			col_frame(p, p->shown - 1);
			zv_col_apply(p->cls, p->ch.cw, p->ch.ch, p->ch.cell / 2, p->pic, p->ps);   /* the half view */
		}
		for (y = 0; y < p->ph; y++)
			for (x = 0; x < p->pw; x++) {
				uint8_t v = p->pic[y * p->ps + x];
				uint16_t c = p->mode == M_GAMECOL && p->col ? p->colpal[v] : p->pal[v & 15];
				rgb[y * p->pw + x] = (uint32_t)(((c >> 8) & 15) * 17) << 16 | (uint32_t)(((c >> 4) & 15) * 17) << 8 | (uint32_t)((c & 15) * 17);
			}
		break;
	case M_REF:
		for (y = 0; y < p->ph; y++)
			for (x = 0; x < p->pw; x++) {
				uint32_t g = p->pic[y * p->ps + x];
				rgb[y * p->pw + x] = g << 16 | g << 8 | g;
			}
		break;
	default:
		free_dither(p);
		for (y = 0; y < p->ph; y++)
			for (x = 0; x < p->pw; x++)
				rgb[y * p->pw + x] = ph[p->tmp[y * p->pw + x]];
		break;
	}
}

// the whole 640x480 screen as a Zeitlos would show it
static void screen_rgb(play_t *p, uint32_t *scr, uint32_t *pic) {
	int w, h, x, y, s = 1, ox, oy;
	uint32_t bg = 0;
	pic_rgb(p, pic, &w, &h);
	if (p->mode == M_GAMEMONO || p->mode == M_GAMEGREY || p->mode == M_GAMECOL) s = 2;
	if (p->mode == M_WINDOW) bg = 0x808080;
	for (y = 0; y < SH; y++)
		for (x = 0; x < SW; x++)
			scr[y * SW + x] = p->mode == M_WINDOW ? (((x ^ y) & 1) ? phos[p->phosphor][1] : phos[p->phosphor][0]) : bg;
	ox = (SW - w * s) / 2;
	oy = (SH - h * s) / 2;
	if (p->mode == M_WINDOW) {
		// a window frame and titlebar, roughly as wm draws one
		int fx0 = ox - 1, fy0 = oy - 12, fx1 = ox + w, fy1 = oy + h;
		for (y = fy0; y <= fy1; y++)
			for (x = fx0; x <= fx1; x++)
				if (x >= 0 && y >= 0 && x < SW && y < SH)
					scr[y * SW + x] = (y < oy) && !(y == fy0 || x == fx0 || x == fx1) ? phos[p->phosphor][1] : phos[p->phosphor][0];
	}
	for (y = 0; y < h * s; y++)
		for (x = 0; x < w * s; x++)
			if (ox + x >= 0 && oy + y >= 0 && ox + x < SW && oy + y < SH)
				scr[(oy + y) * SW + ox + x] = pic[(y / s) * w + x / s];
}

// ---- PNG (uncompressed deflate: no zlib needed) -------------------------------

static uint32_t crc_tab[256];

static uint32_t crc32_upd(uint32_t c, const uint8_t *b, size_t n) {
	size_t i;
	if (!crc_tab[1]) {
		uint32_t k, j;
		for (k = 0; k < 256; k++) { uint32_t v = k; for (j = 0; j < 8; j++) v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1; crc_tab[k] = v; }
	}
	c = ~c;
	for (i = 0; i < n; i++) c = crc_tab[(c ^ b[i]) & 0xff] ^ (c >> 8);
	return ~c;
}

static void be32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v; }

static void chunk(FILE *f, const char *t, const uint8_t *d, uint32_t n) {
	uint8_t b[8];
	uint32_t c;
	be32(b, n);
	memcpy(b + 4, t, 4);
	fwrite(b, 1, 8, f);
	if (n) fwrite(d, 1, n, f);
	c = crc32_upd(0, b + 4, 4);
	c = crc32_upd(c, d, n);
	be32(b, c);
	fwrite(b, 1, 4, f);
}

static int write_png(const char *path, const uint32_t *rgb, int w, int h) {
	FILE *f = fopen(path, "wb");
	size_t raw = (size_t)(w * 3 + 1) * h, nblk = (raw + 65534) / 65535, zl = 2 + raw + nblk * 5 + 4, i, o = 0;
	uint8_t *r = malloc(raw), *z = malloc(zl), ih[13];
	uint32_t a = 1, bsum = 0;
	int x, y;
	if (!f) { perror(path); return -1; }
	for (y = 0; y < h; y++) {
		uint8_t *row = r + (size_t)y * (w * 3 + 1);
		row[0] = 0;
		for (x = 0; x < w; x++) {
			uint32_t c = rgb[y * w + x];
			row[1 + x * 3] = (uint8_t)(c >> 16); row[2 + x * 3] = (uint8_t)(c >> 8); row[3 + x * 3] = (uint8_t)c;
		}
	}
	z[o++] = 0x78; z[o++] = 0x01;
	for (i = 0; i < raw; i += 65535) {
		size_t n = raw - i < 65535 ? raw - i : 65535;
		z[o++] = (uint8_t)(i + n >= raw);
		z[o++] = (uint8_t)n; z[o++] = (uint8_t)(n >> 8);
		z[o++] = (uint8_t)~n; z[o++] = (uint8_t)(~n >> 8);
		memcpy(z + o, r + i, n);
		o += n;
	}
	for (i = 0; i < raw; i++) { a = (a + r[i]) % 65521; bsum = (bsum + a) % 65521; }
	be32(z + o, (bsum << 16) | a);
	o += 4;
	fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
	be32(ih, (uint32_t)w); be32(ih + 4, (uint32_t)h);
	ih[8] = 8; ih[9] = 2; ih[10] = 0; ih[11] = 0; ih[12] = 0;
	chunk(f, "IHDR", ih, 13);
	chunk(f, "IDAT", z, (uint32_t)o);
	chunk(f, "IEND", NULL, 0);
	fclose(f);
	free(r); free(z);
	return 0;
}

// ---- headless tools ------------------------------------------------------------

static void info(play_t *p, const char *path) {
	zv_header_t *h = &p->z.h;
	double secs = (double)h->frames * h->fps_den / h->fps_num;
	struct stat sb;
	uint32_t f, keys = 0, maxv = 0;
	zv_rec_t r;
	stat(path, &sb);
	printf("%s\n", path);
	printf("  picture   %ux%u (half: %ux%u)\n", h->w, h->h, h->w / 2, h->h / 2);
	printf("  rate      %u/%u = %.3f fps, %u frames, %.1f s\n", h->fps_num, h->fps_den,
		(double)h->fps_num / h->fps_den, h->frames, secs);
	if (h->flags & ZV_F_AUDIO) printf("  audio     PCM8, %u Hz, %u channel(s)\n", h->arate, h->achan);
	else printf("  audio     none\n");
	printf("  seek      %u points, the picture whole %u frames after each\n", h->index_n, h->refresh);
	printf("  size      %.1f MB: video %.1f KB/s, audio %.1f KB/s, all %.1f KB/s\n",
		sb.st_size / 1e6, h->video_bytes / 1024.0 / secs, h->audio_bytes / 1024.0 / secs, sb.st_size / 1024.0 / secs);
	printf("            90 minutes like this: %.0f MB\n", sb.st_size / secs * 5400 / 1e6);
	zvf_seek(&p->z, 0);
	for (f = 0; zvf_read(&p->z, &r); f++) {
		if (r.flags & ZV_RF_KEY) keys++;
		if (r.video_len > maxv) maxv = (uint32_t)r.video_len;
	}
	printf("  records   %u read, %u keyframes, largest video part %u bytes\n", f, keys, maxv);
}

static int export_png(play_t *p, const char *dir, uint32_t from, uint32_t count, uint32_t every, int screen) {
	uint32_t *scr = malloc(SW * SH * 4), *pic = malloc((size_t)ZV_MAX_W * ZV_MAX_H * 4);
	uint32_t f, done = 0;
	char path[1024];
	int w, h;
	mkdir(dir, 0755);
	for (f = from; f < p->z.h.frames && done < count; f += every, done++) {
		if (play_goto(p, f)) break;
		snprintf(path, sizeof(path), "%s/%s_%06u.png", dir, mode_key[p->mode], f);
		if (screen) { screen_rgb(p, scr, pic); write_png(path, scr, SW, SH); }
		else { pic_rgb(p, pic, &w, &h); write_png(path, pic, w, h); }
	}
	free(scr); free(pic);
	return 0;
}

// one frame in every mode, side by side, at the screen's scale
static int compare(play_t *p, uint32_t f, const char *out) {
	static const int modes[] = { M_REF, M_DESKFULL, M_DESKFULL, M_FS, M_BN64, M_WINDOW, M_GAMEGREY, M_GAMECOL, M_WINDOW, M_WINCOL };
	static const int masks[] = { 0, ZV_MASK_BAYER8, ZV_MASK_VAC8, 0, 0, ZV_MASK_VAC8, ZV_MASK_VAC8, ZV_MASK_VAC8, ZV_MASK_VAC8, ZV_MASK_VAC8 };
	int n = (int)(sizeof(modes) / sizeof(modes[0])), i, fw = p->z.h.w, fh = p->z.h.h, cols = 2, rows, x, y, w, h;
	int cw = fw + 8, ch = fh + 8;
	uint32_t *pic = malloc((size_t)ZV_MAX_W * ZV_MAX_H * 4), *sheet;
	rows = (n + cols - 1) / cols;
	sheet = malloc((size_t)cw * cols * ch * rows * 4);
	for (i = 0; i < cw * cols * ch * rows; i++) sheet[i] = 0x404040;
	for (i = 0; i < n; i++) {
		int s, cx = (i % cols) * cw + 4, cy = (i / cols) * ch + 4;
		set_mode(p, modes[i], masks[i]);
		if (play_goto(p, f)) { fprintf(stderr, "zvplay: no frame %u\n", f); return -1; }
		pic_rgb(p, pic, &w, &h);
		s = w < fw ? 2 : 1;     // half views doubled, as game mode shows them
		for (y = 0; y < h * s; y++)
			for (x = 0; x < w * s; x++)
				sheet[(cy + y) * cw * cols + cx + x] = pic[(y / s) * w + x / s];
		fprintf(stderr, "  %d: %s%s\n", i + 1, mode_name[modes[i]],
			modes[i] == M_DESKFULL || modes[i] == M_WINDOW || modes[i] == M_GAMEGREY || modes[i] == M_GAMECOL || modes[i] == M_WINCOL
				? (masks[i] == ZV_MASK_VAC8 ? ", void-and-cluster 8x8" : ", Bayer 8x8") : "");
	}
	write_png(out, sheet, cw * cols, ch * rows);
	free(pic); free(sheet);
	return 0;
}

static void wr_le32(FILE *f, uint32_t v) { uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; fwrite(b, 1, 4, f); }
static void wr_le16(FILE *f, uint32_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; fwrite(b, 1, 2, f); }

static int export_y4m(play_t *p, const char *y4m, const char *wav, int screen, int scale, int cadence) {
	FILE *fy = y4m ? fopen(y4m, "wb") : NULL, *fw = wav ? fopen(wav, "wb") : NULL;
	uint32_t *scr = malloc(SW * SH * 4), *pic = malloc((size_t)ZV_MAX_W * ZV_MAX_H * 4), f, lastvs = 0;
	uint8_t *yuv = NULL;
	int ow = 0, oh = 0, w, h, x, y;
	uint32_t asamples = 0;
	zv_rec_t r;
	if ((y4m && !fy) || (wav && !fw)) { perror("zvplay"); return -1; }
	if (fw) { fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, fw); wr_le32(fw, 16); wr_le16(fw, 1); wr_le16(fw, p->z.h.achan);
		wr_le32(fw, p->z.h.arate); wr_le32(fw, p->z.h.arate * p->z.h.achan); wr_le16(fw, p->z.h.achan); wr_le16(fw, 8);
		fwrite("data\0\0\0\0", 1, 8, fw); }
	zvf_seek(&p->z, 0);
	zv_dec_seek(&p->dec, 0);
	memset(p->pic, 0, (size_t)p->ps * p->ph);
	for (f = 0; play_step(p, &r); f++) {
		uint32_t reps = 1, i;
		if (fw && (p->z.h.flags & ZV_F_AUDIO)) { fwrite(r.audio, p->z.h.achan, r.n_audio, fw); asamples += r.n_audio; }
		if (!fy) continue;
		if (screen) { screen_rgb(p, scr, pic); w = SW; h = SH; }
		else pic_rgb(p, pic, &w, &h);
		if (!yuv) {
			ow = w * scale; oh = h * scale;
			yuv = malloc((size_t)ow * oh * 3);
			if (cadence) fprintf(fy, "YUV4MPEG2 W%d H%d F60:1 Ip A1:1 C444\n", ow, oh);
			else fprintf(fy, "YUV4MPEG2 W%d H%d F%u:%u Ip A1:1 C444\n", ow, oh, p->z.h.fps_num, p->z.h.fps_den);
		}
		for (y = 0; y < oh; y++)
			for (x = 0; x < ow; x++) {
				uint32_t c = (screen ? scr : pic)[(y / scale) * w + x / scale];
				int R = (c >> 16) & 255, G = (c >> 8) & 255, B = c & 255;
				int Y = (66 * R + 129 * G + 25 * B + 128) / 256 + 16;
				int U = (-38 * R - 74 * G + 112 * B + 128) / 256 + 128;
				int V = (112 * R - 94 * G - 18 * B + 128) / 256 + 128;
				yuv[(size_t)y * ow + x] = (uint8_t)Y;
				yuv[(size_t)ow * oh + (size_t)y * ow + x] = (uint8_t)U;
				yuv[(size_t)ow * oh * 2 + (size_t)y * ow + x] = (uint8_t)V;
			}
		if (cadence) {
			// as a 60 Hz screen shows it: each frame until the next one's refresh
			uint32_t vs = zv_frame_vsync(&p->z.h, f + 1, 60);
			reps = vs - lastvs;
			lastvs = vs;
		}
		for (i = 0; i < reps; i++) { fwrite("FRAME\n", 1, 6, fy); fwrite(yuv, 1, (size_t)ow * oh * 3, fy); }
	}
	if (fw) {
		uint32_t db = asamples * p->z.h.achan;
		fseek(fw, 4, SEEK_SET); wr_le32(fw, 36 + db);
		fseek(fw, 40, SEEK_SET); wr_le32(fw, db);
		fclose(fw);
	}
	if (fy) fclose(fy);
	free(scr); free(pic); free(yuv);
	return 0;
}

// how long decoding takes, per view, and what the blocks were
static void bench(play_t *p) {
	static const int modes[] = { M_WINDOW, M_DESKFULL, M_GAMEGREY, M_REF };
	int i;
	for (i = 0; i < 4; i++) {
		clock_t t0;
		uint32_t f = 0;
		set_mode(p, modes[i], ZV_MASK_VAC8);
		zvf_seek(&p->z, 0);
		zv_dec_seek(&p->dec, 0);
		t0 = clock();
		while (play_step(p, NULL)) f++;
		printf("  %-28s %u frames, %.3f ms a frame on this machine\n", mode_name[modes[i]], f,
			(double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / (f ? f : 1));
	}
}

// ---- playing (SDL2) ---------------------------------------------------------------

#ifdef ZV_SDL
static int play_sdl(play_t *p, const char *path, int scale, int muted) {
	SDL_Window *win;
	SDL_Renderer *ren;
	SDL_Texture *tex;
	SDL_AudioDeviceID dev = 0;
	SDL_AudioSpec want, have;
	uint32_t *scr = malloc(SW * SH * 4), *pic = malloc((size_t)ZV_MAX_W * ZV_MAX_H * 4);
	zvf_t az;                   // a second reader, for the sound, ahead of the picture
	int audio = (p->z.h.flags & ZV_F_AUDIO) && !muted, run = 1, paused = 0, dirty = 1;
	uint64_t abase = 0, aqueued = 0;    // samples: the seek's first, and queued since
	uint32_t start_frame = 0, f_show = 0, ticks0, pause_t = 0;
	char title[256];
	zv_rec_t r;

	if (SDL_Init(SDL_INIT_VIDEO | (audio ? SDL_INIT_AUDIO : 0))) { fprintf(stderr, "zvplay: %s\n", SDL_GetError()); return -1; }
	win = SDL_CreateWindow("zvplay", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SW * scale, SH * scale, SDL_WINDOW_RESIZABLE);
	ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
	if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
	SDL_RenderSetLogicalSize(ren, SW, SH);
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
	tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, SW, SH);
	if (audio) {
		SDL_zero(want);
		want.freq = (int)p->z.h.arate;
		want.format = AUDIO_U8;
		want.channels = p->z.h.achan;
		want.samples = 1024;
		dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
		if (!dev) { fprintf(stderr, "zvplay: no audio: %s\n", SDL_GetError()); audio = 0; }
	}
	if (audio) {
		// the sound's own reader of the same file
		az = p->z;
		az.f = fopen(path, "rb");
		az.buf = malloc(p->z.h.max_rec + 16);
		if (!az.f) audio = 0;
		else zvf_seek(&az, 0);
	}

	// to frame fr: decoded quietly from the seek point before it (with
	// its refresh), then the sound from fr
	#define SEEK_TO(fr) do { \
		uint32_t f_ = (fr) < p->z.h.frames ? (fr) : p->z.h.frames - 1; \
		p->shown = 0; play_goto(p, f_); start_frame = f_; f_show = f_ + 1; \
		if (audio) { zv_rec_t r_; zvf_seek(&az, f_); while (az.next < f_ && zvf_read(&az, &r_)) { } \
			SDL_ClearQueuedAudio(dev); abase = zv_audio_before(&p->z.h, f_); aqueued = 0; } \
		ticks0 = SDL_GetTicks(); dirty = 1; } while (0)

	ticks0 = SDL_GetTicks();
	if (audio) SDL_PauseAudioDevice(dev, 0);
	while (run) {
		SDL_Event ev;
		uint64_t played_ms;
		uint32_t vs;
		while (SDL_PollEvent(&ev)) {
			if (ev.type == SDL_QUIT) run = 0;
			if (ev.type == SDL_KEYDOWN) {
				SDL_Keycode k = ev.key.keysym.sym;
				uint32_t cur = p->shown ? p->shown - 1 : 0;
				uint32_t step = 10 * p->z.h.fps_num / p->z.h.fps_den;
				if (k == SDLK_q || k == SDLK_ESCAPE) run = 0;
				else if (k >= SDLK_1 && k < SDLK_1 + M_COUNT) { set_mode(p, (int)(k - SDLK_1), p->mask); SEEK_TO(cur); }
				else if (k == SDLK_d) { set_mode(p, p->mode, p->mask == ZV_MASK_BAYER8 ? ZV_MASK_VAC8 : ZV_MASK_BAYER8); SEEK_TO(cur); }
				else if (k == SDLK_p) { p->phosphor = (p->phosphor + 1) & 3; dirty = 1; }
				else if (k == SDLK_SPACE) {
					paused = !paused;
					if (audio) SDL_PauseAudioDevice(dev, paused);
					if (paused) pause_t = SDL_GetTicks(); else ticks0 += SDL_GetTicks() - pause_t;
				}
				else if (k == SDLK_RIGHT) SEEK_TO(cur + step);
				else if (k == SDLK_LEFT) SEEK_TO(cur > step ? cur - step : 0);
				else if (k == SDLK_HOME) SEEK_TO(0);
				else if (k == SDLK_PERIOD && paused) { if (play_step(p, NULL)) dirty = 1; }
				else if (k == SDLK_s) {
					char fn[64];
					screen_rgb(p, scr, pic);
					snprintf(fn, sizeof(fn), "zvplay_%06u.png", cur);
					write_png(fn, scr, SW, SH);
					fprintf(stderr, "zvplay: wrote %s\n", fn);
				}
			}
		}
		// keep a second of sound queued
		while (audio && !paused && SDL_GetQueuedAudioSize(dev) < p->z.h.arate * p->z.h.achan) {
			if (!zvf_read(&az, &r)) break;
			SDL_QueueAudio(dev, r.audio, r.n_audio * p->z.h.achan);
			aqueued += r.n_audio;
		}
		// the clock: the sound played, or the wall clock without sound
		if (audio) {
			uint64_t q = SDL_GetQueuedAudioSize(dev) / p->z.h.achan;
			uint64_t played = abase + (aqueued > q ? aqueued - q : 0);
			played_ms = played * 1000 / p->z.h.arate;
		} else {
			uint32_t now = paused ? pause_t : SDL_GetTicks();
			played_ms = (uint64_t)start_frame * 1000 * p->z.h.fps_den / p->z.h.fps_num + (now - ticks0);
		}
		// the 60 Hz refresh that is now, and the frames due by it
		vs = (uint32_t)(played_ms * 60 / 1000);
		if (!paused) {
			while (f_show < p->z.h.frames && zv_frame_vsync(&p->z.h, f_show, 60) <= vs) {
				if (!play_step(p, NULL)) break;
				f_show++;
				dirty = 1;
			}
			if (f_show >= p->z.h.frames && (!audio || SDL_GetQueuedAudioSize(dev) == 0)) {
				if (!audio) run = 0;
				else if (SDL_GetQueuedAudioSize(dev) == 0) run = 0;
			}
		}
		if (dirty) {
			void *px;
			int pitch, y;
			screen_rgb(p, scr, pic);
			SDL_LockTexture(tex, NULL, &px, &pitch);
			for (y = 0; y < SH; y++) memcpy((uint8_t *)px + y * pitch, scr + y * SW, SW * 4);
			SDL_UnlockTexture(tex);
			snprintf(title, sizeof(title), "zvplay  %s, %s%s  frame %u  %u:%02u",
				mode_name[p->mode],
				p->mode <= M_GAMEGREY ? (p->mask == ZV_MASK_VAC8 ? "void-and-cluster, " : "Bayer, ") : "",
				phos_name[p->phosphor], p->shown ? p->shown - 1 : 0,
				(unsigned)(played_ms / 60000), (unsigned)(played_ms / 1000 % 60));
			SDL_SetWindowTitle(win, title);
			dirty = 0;
		}
		SDL_RenderClear(ren);
		SDL_RenderCopy(ren, tex, NULL, NULL);
		SDL_RenderPresent(ren);
		if (paused) SDL_Delay(10);
	}
	if (dev) SDL_CloseAudioDevice(dev);
	SDL_Quit();
	free(scr); free(pic);
	return 0;
}
#endif

static void usage(void) {
	fprintf(stderr,
		"usage: zvplay [options] FILE.zv\n"
		"  --mode M         window (default), full, game, grey, ref, fs, bn64, colour, wcolour\n"
		"  --mask M         vac8 (default) or bayer8\n"
		"  --phosphor P     white, amber, green, paper\n"
		"  --gamma G        grey to light for the mono dither (default 1.6)\n"
		"  --scale N        window scale when playing (default 2); y4m scale\n"
		"  --greys G        game mode's greys: true (16, the default) or near (64, faint tints)\n"
		"  --mute\n"
		"headless\n"
		"  --info\n"
		"  --bench          decode every frame in each view, timed\n"
		"  --png DIR        stills [--from F] [--count N] [--every N] [--screen]\n"
		"  --compare F OUT.png   frame F in every view, side by side\n"
		"  --y4m OUT [--wav OUT] [--screen] [--cadence]   the film as one view\n"
		"keys when playing\n"
		"  1-7 view, d mask, p phosphor, space pause, . step, left/right 10 s,\n"
		"  home start, s screenshot, q quit\n");
	exit(2);
}

int main(int argc, char **argv) {
	const char *path = NULL, *png = NULL, *cmp_out = NULL, *y4m = NULL, *wav = NULL;
	int gamma = 0, mode = M_WINDOW, mask = ZV_MASK_VAC8, phosphor = 0, scale = 0, mute = 0, do_info = 0, do_bench = 0, screen = 0, cadence = 0;
	uint32_t from = 0, count = 0xffffffffu, every = 1, cmp_f = 0;
	int i, k;
	play_t p;
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
#define NX() (i + 1 < argc ? argv[++i] : (usage(), ""))
		if (!strcmp(a, "--mode")) { const char *m = NX(); for (k = 0; k < M_COUNT; k++) if (!strcmp(m, mode_key[k])) mode = k; }
		else if (!strcmp(a, "--mask")) mask = !strcmp(NX(), "bayer8") ? ZV_MASK_BAYER8 : ZV_MASK_VAC8;
		else if (!strcmp(a, "--phosphor")) { const char *m = NX(); for (k = 0; k < 4; k++) if (!strcmp(m, phos_name[k])) phosphor = k; }
		else if (!strcmp(a, "--scale")) scale = atoi(NX());
		else if (!strcmp(a, "--gamma")) gamma = (int)(atof(NX()) * 100 + 0.5);
		else if (!strcmp(a, "--mute")) mute = 1;
		else if (!strcmp(a, "--greys")) greys_kind = !strcmp(NX(), "true") ? ZV_GREYS_TRUE : ZV_GREYS_NEAR;
		else if (!strcmp(a, "--info")) do_info = 1;
		else if (!strcmp(a, "--bench")) do_bench = 1;
		else if (!strcmp(a, "--png")) png = NX();
		else if (!strcmp(a, "--from")) from = (uint32_t)atoi(NX());
		else if (!strcmp(a, "--count")) count = (uint32_t)atoi(NX());
		else if (!strcmp(a, "--every")) every = (uint32_t)atoi(NX());
		else if (!strcmp(a, "--screen")) screen = 1;
		else if (!strcmp(a, "--cadence")) cadence = 1;
		else if (!strcmp(a, "--compare")) { cmp_f = (uint32_t)atoi(NX()); cmp_out = NX(); }
		else if (!strcmp(a, "--y4m")) y4m = NX();
		else if (!strcmp(a, "--wav")) wav = NX();
		else if (a[0] == '-') usage();
		else path = a;
	}
	if (!path) usage();
	if (every < 1) every = 1;
	if (play_open(&p, path, mode, mask, gamma)) return 1;
	p.phosphor = phosphor;
	if (do_info) { info(&p, path); return 0; }
	if (do_bench) { bench(&p); return 0; }
	if (png) return export_png(&p, png, from, count, every, screen) ? 1 : 0;
	if (cmp_out) return compare(&p, cmp_f, cmp_out) ? 1 : 0;
	if (y4m || wav) return export_y4m(&p, y4m, wav, screen, scale > 0 ? scale : 1, cadence) ? 1 : 0;
#ifdef ZV_SDL
	return play_sdl(&p, path, scale > 0 ? scale : 2, mute) ? 1 : 0;
#else
	fprintf(stderr, "zvplay: built without SDL2: use --png, --compare or --y4m\n");
	return 2;
#endif
}
