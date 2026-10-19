/*
 * sw/common/zpaper.c on the host: a page recorded through zcanvas and
 * played back onto a grey raster is the page drawn directly; records
 * read back exactly; a damaged job is refused or cut short, never read
 * past its end. `make -f Makefile.zcanvas paper`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../zpaper.h"
#include "../zfont.h"

static int fails, checks;

#define ck(cond, msg) do { checks++; if (!(cond)) { fails++; \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

// -- a growable job in memory --
typedef struct { uint8_t *p; uint32_t n, cap; } mem_t;
static bool mem_write(void *ctx, const void *d, uint32_t n) {
	mem_t *m = ctx;
	if (m->n + n > m->cap) {
		m->cap = (m->n + n) * 2 + 1024;
		m->p = realloc(m->p, m->cap);
	}
	memcpy(m->p + m->n, d, n);
	m->n += n;
	return true;
}

static zc_font_t f_sans, f_mono;
static uint8_t *sans_data;
static zc_pt_t pts[4096], wpts[8192];
static uint16_t subs[512], wsubs[1024];
static zc_path_t path, work;

static bool font_by_name(void *ctx, const char *name, const zc_font_t **out) {
	(void)ctx;
	if (!strcmp(name, "sans")) { *out = &f_sans; return true; }
	if (!strcmp(name, "mono6x12")) { *out = &f_mono; return true; }
	return false;
}

static uint8_t photo[64 * 48];
static bool image_by_path(void *ctx, const char *p, int contrast, int bright,
	int maxw, int maxh, const uint8_t **pix, int *w, int *h) {
	(void)ctx; (void)contrast; (void)bright; (void)maxw; (void)maxh;
	if (strcmp(p, "/media/images/test.pgm")) return false;
	*pix = photo;
	*w = 64;
	*h = 48;
	return true;
}

// A card's worth of drawing, in page units through xf: every kind of
// thing the recorder hears about.
static void scene(zc_target_t *t, const zc_xf_t *xf, bool with_photo) {
	static const uint8_t bricks[8] = { 0xFF, 0x80, 0x80, 0x80, 0xFF, 0x08, 0x08, 0x08 };
	zc_paint_t black = zc_black(), shade = zc_shade(6), pat = zc_pattern(bricks, 16, true);
	zc_style_t st = { &f_sans, ZC_I(24), 0, 0, 0, 0, 0 };
	zc_style_t sm = { &f_mono, ZC_I(12), 0, 0, 0, 0, 1 };
	zc_fix x0, y0;
	static uint32_t glyph[8];
	(void)x0; (void)y0;
	int i;
	for (i = 0; i < 8; i++) glyph[i] = ~(0x81u << i);	// a cross, 0 = ink

	zc_path_set_xf(&path, xf);
	zc_path_reset(&path);
	zc_path_rect(&path, ZC_I(20), ZC_I(20), ZC_I(200), ZC_I(80));
	zc_fill(t, &path, ZC_NONZERO, &shade);
	zc_frame_ellipse(t, &path, ZC_I(240), ZC_I(20), ZC_I(160), ZC_I(100), ZC_I(3), &black);
	zc_path_reset(&path);
	zc_path_rrect(&path, ZC_I(30), ZC_I(130), ZC_I(180), ZC_I(90), ZC_I(14));
	zc_fill(t, &path, ZC_NONZERO, &pat);
	zc_path_reset(&path);
	zc_path_move(&path, ZC_I(250), ZC_I(150));
	zc_path_line(&path, ZC_I(480), ZC_I(300));
	zc_path_line(&path, ZC_I(300), ZC_I(320));
	zc_stroke(t, &path, ZC_I(4), false, &work, &black);
	zc_text_draw(t, xf, &st, ZC_I(30), ZC_I(270), "Paper, Zeitlos!", 15, &black);
	zc_text_draw(t, xf, &sm, ZC_I(30), ZC_I(300), "mono 6x12", 9, &black);
	// pictures through the transform, as anything turned must be
	{
		zc_xf_t g = { (ZC_I(32) << 8) / 8, 0, 0, (ZC_I(32) << 8) / 8, ZC_I(440), ZC_I(40) };
		zc_xf_t m = zc_xf_mul(xf, &g);
		zc_blit_xf(t, ZC_IMG_MASK, glyph, 1, 8, 8, &m, &black);
	}
	if (with_photo) {
		zc_fix x1, y1;
		zc_xf_apply(xf, ZC_I(400), ZC_I(230), &x0, &y0);
		zc_xf_apply(xf, ZC_I(496), ZC_I(302), &x1, &y1);
		if (t->fmt == ZC_REC) {
			zp_image_file(t, "/media/images/test.pgm", 0, 0, x0, y0, x1 - x0, y1 - y0);
		} else {
			zc_xf_t g = { (ZC_I(96) << 8) / 64, 0, 0, (ZC_I(72) << 8) / 48, ZC_I(400), ZC_I(230) };
			zc_xf_t m = zc_xf_mul(xf, &g);
			zc_blit_xf(t, ZC_IMG_GRAY, photo, 64, 64, 48, &m, NULL);
		}
	}
}

static void write_pgm(const char *dir, const char *name, const uint8_t *g, int w, int h) {
	char p[256];
	FILE *f;
	if (!dir) return;
	snprintf(p, sizeof(p), "%s/%s.pgm", dir, name);
	f = fopen(p, "wb");
	if (!f) return;
	fprintf(f, "P5\n%d %d\n255\n", w, h);
	fwrite(g, 1, (size_t)w * h, f);
	fclose(f);
}

static uint8_t *load(const char *path, uint32_t *n) {
	FILE *f = fopen(path, "rb");
	uint8_t *b;
	long k;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	k = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc((size_t)k);
	if (fread(b, 1, (size_t)k, f) != (size_t)k) { fclose(f); free(b); return NULL; }
	fclose(f);
	*n = (uint32_t)k;
	return b;
}

int main(int argc, char **argv) {
	const char *fonts = argc > 1 ? argv[1] : "../../data/font";
	const char *look = argc > 2 ? argv[2] : NULL;
	static uint8_t scratch[256 * 1024];
	char fp[512];
	uint32_t n;
	mem_t job = { 0 };
	zp_writer_t w;
	zpage_t pg;
	zc_xf_t xf;
	zc_target_t *rt;
	int i;

	zc_scratch(scratch, sizeof(scratch));
	zc_path_init(&path, pts, 4096, subs, 512);
	zc_path_init(&work, wpts, 8192, wsubs, 1024);
	snprintf(fp, sizeof(fp), "%s/zhf/sans.zhf", fonts);
	sans_data = load(fp, &n);
	ck(sans_data && zc_font_open(&f_sans, sans_data, n, "sans"), "sans.zhf");
	zc_font_builtin(&f_mono, &z_font_6x12, 10, "mono6x12");
	for (i = 0; i < 64 * 48; i++) photo[i] = (uint8_t)((i % 64) * 4);

	// -- record two pages and a job option --
	zp_writer_init(&w, mem_write, &job);
	zp_job(&w, "title", "A test");
	zp_job(&w, "copies", "2");
	zpage_set(&pg, "classic", true);
	rt = zp_page_begin(&w, &pg, &xf);
	ck(rt && rt->fmt == ZC_REC, "a recording target");
	scene(rt, &xf, true);
	zp_page_end(&w);
	zpage_set(&pg, "letter", false);
	rt = zp_page_begin(&w, &pg, &xf);
	zc_clip(rt, 0, 0, 100 * ZP_SUB, 100 * ZP_SUB);
	scene(rt, &xf, false);
	zp_page_end(&w);
	ck(!w.failed && w.pages == 2, "two pages written");

	// -- read back --
	{
		char v[32];
		uint32_t st[4], by[4];
		zp_reader_t r;
		zp_rec_t rec;
		zp_page_t pi;
		int ops[16] = { 0 };
		ck(zp_job_get(job.p, job.n, "title", v, sizeof(v)) && !strcmp(v, "A test"), "job title");
		ck(zp_job_get(job.p, job.n, "copies", v, sizeof(v)) && !strcmp(v, "2"), "job copies");
		ck(!zp_job_get(job.p, job.n, "nup", v, sizeof(v)) && v[0] == 0, "unset option");
		ck(zp_pages(job.p, job.n, st, by, 4) == 2, "two pages found");
		ck(zp_open(&r, job.p, job.n), "opens");
		while (zp_next(&r, &rec)) {
			zp_op_t op;
			if (rec.op < 16) ops[rec.op]++;
			if (rec.op == ZP_PAGE && ops[ZP_PAGE] == 1) {
				ck(zp_page_info(&rec, &pi) && pi.gw == 512 && pi.gh == 342 && !strcmp(pi.name, "classic"),
					"page 1 is classic");
			}
			if (rec.op == ZP_PAGE && ops[ZP_PAGE] == 2) {
				ck(zp_page_info(&rec, &pi) && pi.w_mm10 == 2159 && pi.h_mm10 == 2794, "page 2 is letter paper");
			}
			if (rec.op >= ZP_CLIP && rec.op <= ZP_TEXT) ck(zp_decode(&rec, &op), "decodes");
			if (rec.op == ZP_TEXT && ops[ZP_TEXT] == 1) {
				ck(zp_decode(&rec, &op) && op.tlen == 15 && !memcmp(op.text, "Paper, Zeitlos!", 15) &&
					!strcmp(op.font, "sans") && op.size == ZC_I(24), "text run kept");
			}
			if (rec.op == ZP_IMAGE_FILE) {
				ck(zp_decode(&rec, &op) && !strcmp(op.path, "/media/images/test.pgm") &&
					op.dw == ZC_I(96 * ZP_SUB), "picture by path");
			}
		}
		ck(!r.bad, "read to the end cleanly");
		ck(ops[ZP_FILL] >= 3 && ops[ZP_STROKE] >= 1 && ops[ZP_TEXT] == 4 && ops[ZP_IMAGE] == 2 &&
			ops[ZP_IMAGE_FILE] == 1 && ops[ZP_CLIP] >= 2, "every kind recorded");
		ck(zp_max_points(job.p, job.n) > 16, "longest path");
	}

	// -- played back at 300 dpi is drawn directly at 300 dpi --
	{
		zp_reader_t r;
		uint32_t st[4], by[4];
		zpage_t cpg;
		zpage_sheet_t sheet;
		zpage_cell_t cell;
		int sw, sh, band, diff = 0, ink = 0;
		uint8_t *a, *b;
		zp_play_t pl;
		zp_rec_t rec;
		zp_page_t pi;

		zp_pages(job.p, job.n, st, by, 4);
		zp_open(&r, job.p, job.n);
		r.pos = st[0];
		zp_next(&r, &rec);
		zp_page_info(&rec, &pi);
		zp_page_zpage(&pi, &cpg);
		zpage_sheet(&sheet, zpage_find("letter"), 300, 50);
		// a landscape sheet, so the card lies upright: the direct drawing
		// below knows nothing of turning patterns
		{ int k = sheet.w; sheet.w = sheet.h; sheet.h = k; }
		ck(zpage_place(&cpg, &sheet, 1, 0, ZPAGE_FIT, 0, &cell) && !cell.turned, "placed upright");
		sw = sheet.w; sh = sheet.h;
		a = malloc((size_t)sw * sh);
		b = malloc((size_t)sw * sh);

		// directly, the page whole
		{
			zc_target_t t;
			zc_paint_t white = zc_white();
			zc_target_gray(&t, a, sw, sw, sh);
			zc_clear(&t, &white);
			t.pat_scale = (uint8_t)((zc_xf_scale_of(&cell.xf) + 32768) >> 16);
			{ zc_fix ox, oy; zc_xf_apply(&cell.xf, 0, 0, &ox, &oy); t.px = (ox + ZC_HALF) >> ZC_FRAC; t.py = (oy + ZC_HALF) >> ZC_FRAC; }
			zc_clip(&t, cell.x, cell.y, cell.x + cell.w, cell.y + cell.h);
			scene(&t, &cell.xf, true);
		}
		// played, in bands of 100 rows
		memset(&pl, 0, sizeof(pl));
		pl.font = font_by_name;
		pl.image = image_by_path;
		pl.path = &path;
		pl.work = &work;
		memset(b, 0xFF, (size_t)sw * sh);
		for (band = 0; band < sh; band += 100) {
			zc_target_t t;
			int bh = sh - band < 100 ? sh - band : 100;
			zc_target_gray(&t, b + (size_t)band * sw, sw, sw, bh);
			zc_target_origin(&t, 0, band);
			zc_clip(&t, cell.x, cell.y, cell.x + cell.w, cell.y + cell.h);
			zp_play_page(&pl, job.p + st[0], by[0], &t, &cell.xf);
		}
		ck(pl.skipped == 0, "nothing skipped");
		for (i = 0; i < sw * sh; i++) {
			if (a[i] != 0xFF) ink++;
			if (a[i] != b[i]) diff++;
		}
		printf("zpaper: %d of %d inked pixels differ (%.3f%%)\n", diff, ink, 100.0 * diff / (ink ? ink : 1));
		ck(ink > 100000, "the page has ink");
		// curves flattened at 16 units a page unit rather than at device
		// resolution, and glyph origins rounded once rather than twice:
		// a few edge pixels in ten thousand, nothing more
		ck(diff * 1000 < ink, "played == drawn, within a tenth of a percent");
		write_pgm(look, "zpaper_direct", a, sw, sh);
		write_pgm(look, "zpaper_played", b, sw, sh);

		// turned a quarter: the same page, every pixel where the turn puts
		// it -- patterns turned with it, pictures, bitmap text and all.
		// Unturned into A (the page at scale s from the corner), turned
		// into B (B's width is A's height): A[y][x] is B[x][Wb-1-y].
		{
			int32_t s = 4 << 16;
			int aw = 512 * 4, ah = 342 * 4, bw2 = ah, bh2 = aw, x, y, off = 0, ink2 = 0;
			uint8_t *A = malloc((size_t)aw * ah), *B = malloc((size_t)bw2 * bh2);
			zc_target_t t;
			zc_xf_t up = zc_xf_scale(s, 0, 0), turned = zc_xf_rot90(s, ZC_I(bw2), 0);
			memset(A, 0xFF, (size_t)aw * ah);
			memset(B, 0xFF, (size_t)bw2 * bh2);
			zc_target_gray(&t, A, aw, aw, ah);
			pl.skipped = 0;
			zp_play_page(&pl, job.p + st[0], by[0], &t, &up);
			zc_target_gray(&t, B, bw2, bw2, bh2);
			zp_play_page(&pl, job.p + st[0], by[0], &t, &turned);
			ck(pl.skipped == 0, "turned: nothing skipped");
			for (y = 0; y < ah; y++)
				for (x = 0; x < aw; x++) {
					uint8_t pa = A[(size_t)y * aw + x], pb = B[(size_t)x * bw2 + (bw2 - 1 - y)];
					if (pa < 128) ink2++;
					if ((pa < 128) != (pb < 128)) off++;
				}
			printf("zpaper: turned, %d of %d inked pixels differ (%.3f%%)\n", off, ink2, 100.0 * off / (ink2 ? ink2 : 1));
			// glyphs: an unturned run is drawn from the glyph cache on whole
			// pixels, a turned one stroke by stroke -- a few edge pixels
			ck(off * 100 < ink2, "turned == unturned, turned back (patterns too)");
			write_pgm(look, "zpaper_turned", B, bw2, bh2);
			write_pgm(look, "zpaper_upright", A, aw, ah);
			free(A);
			free(B);
		}
		free(a);
		free(b);
	}

	// -- damaged jobs --
	{
		uint32_t cut;
		zp_reader_t r;
		zp_rec_t rec;
		uint8_t *copy = malloc(job.n);
		ck(!zp_open(&r, "ZPP2\1\0\0\0", 8), "a newer version is refused");
		for (cut = 0; cut < job.n; cut += 7) {
			uint32_t st[4], by[4];
			memcpy(copy, job.p, cut);
			if (zp_open(&r, copy, cut)) {
				while (zp_next(&r, &rec)) { zp_op_t op; zp_decode(&rec, &op); }
			}
			zp_pages(copy, cut, st, by, 4);
			zp_max_points(copy, cut);
		}
		// lengths and counts scribbled on
		srand(1);
		for (i = 0; i < 3000; i++) {
			memcpy(copy, job.p, job.n);
			copy[8 + rand() % (job.n - 8)] = (uint8_t)rand();
			copy[8 + rand() % (job.n - 8)] = (uint8_t)rand();
			if (zp_open(&r, copy, job.n)) {
				while (zp_next(&r, &rec)) {
					zp_op_t op;
					if (zp_decode(&rec, &op) && (op.op == ZP_FILL || op.op == ZP_STROKE)) {
						int k;
						zc_fix x, y;
						for (k = 0; k < op.npts; k++) zp_point(&op, k, &x, &y);
						for (k = 0; k < op.nsub; k++) zp_sub_at(&op, k);
					}
				}
			}
		}
		free(copy);
		ck(1, "damaged jobs read without a fault (ASan)");
	}

	free(job.p);
	free(sans_data);
	printf("zpaper: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
