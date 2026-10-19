/*
 * sw/common/zpage.c and zimg.c's grey output on the host: card grids,
 * placing cards on sheets (fit, actual size, turned, n-up, cut marks),
 * and an image decoded to grey, scaled and dithered once.
 * `make -f Makefile.zcanvas page`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../zpage.h"
#include "../zimg.h"
#include "../zcanvas.h"

static int fails, checks;

#define ck(cond, msg) do { checks++; if (!(cond)) { fails++; \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

static int grid(const char *name, bool land, int *w, int *h) {
	zpage_t pg;
	if (!zpage_set(&pg, name, land)) return 0;
	*w = pg.gw;
	*h = pg.gh;
	return 1;
}

// -- an in-memory byte source for zimg --
typedef struct { const uint8_t *p; int n, pos; } mem_src_t;
static int mem_read(void *ctx, uint8_t *buf, int len) {
	mem_src_t *m = ctx;
	int k = m->n - m->pos < len ? m->n - m->pos : len;
	if (k < 0) k = 0;
	memcpy(buf, m->p + m->pos, (size_t)k);
	m->pos += k;
	return k;
}

static uint8_t *load(const char *path, int *n) {
	FILE *f = fopen(path, "rb");
	uint8_t *b;
	long l;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	l = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc((size_t)l);
	if (fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return NULL; }
	fclose(f);
	*n = (int)l;
	return b;
}

static uint8_t gimg[480][640];
static int g_w, g_h, g_rows;
static void take_row(void *u, const uint8_t *g, int y, int w) {
	(void)u;
	memcpy(gimg[y], g, (size_t)w);
	if (w > g_w) g_w = w;
	if (y + 1 > g_h) g_h = y + 1;
	g_rows++;
}

static void write_pbm(const char *dir, const char *name, const uint32_t *b,
	int wpl, int w, int h) {
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

int main(int argc, char **argv) {
	const char *root = argc > 1 ? argv[1] : "../../..";
	const char *dir = argc > 2 ? argv[2] : NULL;
	int w, h;

	// -- grids --
	ck(grid("classic", true, &w, &h) && w == 512 && h == 342, "classic is 512x342");
	ck(grid("4x6", true, &w, &h) && w == 512 && h == 341, "4x6 landscape 512x341");
	ck(grid("letter", true, &w, &h) && w == 512 && h == 396, "letter landscape 512x396");
	ck(grid("a4", true, &w, &h) && w == 512 && h == 362, "A4 landscape 512x362");
	ck(grid("16:9", true, &w, &h) && w == 512 && h == 288, "16:9 512x288");
	ck(grid("4:3", true, &w, &h) && w == 512 && h == 384, "4:3 512x384");
	ck(grid("letter", false, &w, &h) && w == 340 && h == 440, "letter portrait 340x440");
	ck(grid("a4", false, &w, &h) && w == 311 && h == 440, "A4 portrait 311x440");
	ck(grid("3x5", true, &w, &h) && w == 512 && h == 307, "index card 512x307");
	ck(grid("LETTER", true, &w, &h), "names ignore case");
	ck(!grid("b5", true, &w, &h), "an unknown size is refused");

	// -- sheets and placement --
	{
		zpage_t pg;
		zpage_sheet_t sh;
		zpage_cell_t c;
		zc_fix x, y;

		// a landscape letter card on letter paper turns a quarter and
		// fills the printable area
		zpage_set(&pg, "letter", true);
		zpage_sheet(&sh, zpage_find("letter"), 300, 50);
		ck(sh.w == 2550 && sh.h == 3300 && sh.left == 59, "letter at 300 dpi, 5 mm margins");
		ck(zpage_place(&pg, &sh, 1, 0, ZPAGE_FIT, 0, &c), "fits");
		ck(c.turned, "landscape on portrait paper is turned");
		ck(c.w <= sh.w - 2 * 59 && c.h <= sh.h - 2 * 59, "inside the printable area");
		ck(c.w >= sh.w - 2 * 59 - 2 || c.h >= sh.h - 2 * 59 - 2, "and as large as fits");
		// the card's top-left grid corner lands at the cell's top-right
		zc_xf_apply(&c.xf, 0, 0, &x, &y);
		ck((x >> 8) == c.x + c.w && (y >> 8) == c.y, "grid origin at the turned cell's corner");
		zc_xf_apply(&c.xf, ZC_I(pg.gw), ZC_I(pg.gh), &x, &y);
		ck(abs((x >> 8) - c.x) <= 1 && abs((y >> 8) - (c.y + c.h)) <= 1,
			"far corner at the opposite corner");

		// a 4x6 card at actual size on letter: 4 x 6 inches, centred
		zpage_set(&pg, "4x6", false);
		ck(zpage_place(&pg, &sh, 1, 0, ZPAGE_ACTUAL, 0, &c) && !c.turned, "4x6 actual");
		ck(abs(c.w - 1200) <= 1 && abs(c.h - 1800) <= 2, "is 1200x1800 pixels at 300 dpi");
		if (abs(c.w - 1200) > 1 || abs(c.h - 1800) > 2) printf("  got %dx%d\n", c.w, c.h);
		ck(abs(c.x - (2550 - c.w) / 2) <= 1, "centred");

		// a 3x5 card on its own sheet covers it edge to edge
		zpage_set(&pg, "3x5", true);
		zpage_sheet(&sh, zpage_find("3x5"), 300, 30);
		ck(zpage_place(&pg, &sh, 1, 0, ZPAGE_ACTUAL, 0, &c), "3x5 on 3x5");
		ck(c.turned && c.x == 0 && c.y == 0 && abs(c.w - sh.w) <= 1, "turned, edge to edge");

		// four 4x6 cards on a letter sheet at actual size
		zpage_set(&pg, "4x6", true);
		zpage_sheet(&sh, zpage_find("letter"), 300, 50);
		{
			zpage_cell_t cs[4];
			int i, ok = 1, segs;
			static int seg[64][4];
			for (i = 0; i < 4; i++) ok &= zpage_place(&pg, &sh, 4, i, ZPAGE_ACTUAL, 60, &cs[i]);
			ck(!ok, "four 4x6 do not fit a letter sheet at actual size");
			for (i = 0; i < 2; i++) ok = zpage_place(&pg, &sh, 2, i, ZPAGE_ACTUAL, 60, &cs[i]);
			ck(ok && abs(cs[0].w - 1800) <= 1 && cs[1].y > cs[0].y + cs[0].h,
				"two do, one above the other");
			for (i = 0; i < 4; i++) zpage_place(&pg, &sh, 4, i, ZPAGE_FIT, 60, &cs[i]);
			ck(cs[1].x > cs[0].x + cs[0].w && cs[2].y > cs[0].y + cs[0].h, "four fitted in a 2x2 grid");
			segs = zpage_cut_marks(&sh, cs, 4, 40, seg, 64);
			ck(segs > 0 && segs <= 32, "cut marks");
			for (i = 0, ok = 1; i < segs; i++) {
				int k;
				for (k = 0; k < 4; k++) {
					int sx = seg[i][0] < seg[i][2] ? seg[i][0] : seg[i][2];
					int ex = seg[i][0] < seg[i][2] ? seg[i][2] : seg[i][0];
					int sy = seg[i][1] < seg[i][3] ? seg[i][1] : seg[i][3];
					int ey = seg[i][1] < seg[i][3] ? seg[i][3] : seg[i][1];
					if (ex >= cs[k].x && sx < cs[k].x + cs[k].w &&
						ey >= cs[k].y && sy < cs[k].y + cs[k].h) ok = 0;
				}
			}
			ck(ok, "no cut mark crosses a card");
		}
		// a 16:9 slide on A4 fits and centres (no paper of its own)
		zpage_set(&pg, "16:9", true);
		zpage_sheet(&sh, zpage_find("a4"), 300, 50);
		ck(zpage_place(&pg, &sh, 1, 0, ZPAGE_ACTUAL, 0, &c), "a ratio page asked for actual size still fits");
		ck(c.turned, "turned on portrait A4");
		// handouts: 6 to a page
		{
			zpage_cell_t cs[6];
			int i;
			for (i = 0; i < 6; i++) zpage_place(&pg, &sh, 6, i, ZPAGE_FIT, 80, &cs[i]);
			ck(!cs[0].turned && cs[1].x > cs[0].x, "six 16:9 slides: two columns, upright");
			ck(cs[5].y > cs[3].y && cs[3].y > cs[1].y, "three rows");
		}
	}

	// -- grey decoding, then scaled and dithered once --
	{
		char p[512];
		int n;
		uint8_t *b;
		z_img_t im;
		mem_src_t src;
		static uint32_t doc[480][20];

		snprintf(p, sizeof(p), "%s/sw/data/demo/squirrel.pgm", root);
		b = load(p, &n);
		ck(b != NULL, "squirrel.pgm");
		if (b) {
			// the PGM's own samples come through unchanged
			int hdr, y, x, same = 1;
			memset(&im, 0, sizeof(im));
			src = (mem_src_t){ b, n, 0 };
			im.read = mem_read;
			im.ctx = &src;
			im.doc = NULL;
			im.doc_w = 640;
			im.doc_h = 480;
			g_w = g_h = g_rows = 0;
			ck(z_img_decode_gray(&im, z_img_sniff(b, n), take_row, NULL) == Z_IMG_OK,
				"PGM decodes to grey with no document");
			ck(g_w == im.out_w && g_h == im.out_h && g_rows == im.out_h, "every row once");
			hdr = n - im.src_w * im.src_h;	// P5: the samples end the file
			for (y = 0; y < im.out_h && same; y++)
				for (x = 0; x < im.out_w; x++)
					if (gimg[y][x] != b[hdr + y * im.src_w + x]) { same = 0; break; }
			ck(im.shift == 0 && same, "grey rows are the file's samples");

			// the ordinary decode still dithers into a document
			memset(&im, 0, sizeof(im));
			src.pos = 0;
			im.read = mem_read;
			im.ctx = &src;
			im.doc = &doc[0][0];
			im.doc_wpl = 20;
			im.doc_w = 640;
			im.doc_h = 480;
			ck(z_img_decode(&im, z_img_sniff(b, n)) == Z_IMG_OK, "and still decodes dithered");

			// scaled once, dithered once, at two sizes
			{
				static uint32_t small[100][5], large[300][13];
				static int16_t row[3 * (420 + 2)];
				zc_dither(&gimg[0][0], 640, g_w, g_h, &small[0][0], 5, 150, 100, row, 0, 0);
				zc_dither(&gimg[0][0], 640, g_w, g_h, &large[0][0], 13, 400, 300, row, 0, 0);
				write_pbm(dir, "zpage_squirrel_150", &small[0][0], 5, 150, 100);
				write_pbm(dir, "zpage_squirrel_400", &large[0][0], 13, 400, 300);
			}
			free(b);
		}

		snprintf(p, sizeof(p), "%s/sw/data/images/squirrel.jpg", root);
		b = load(p, &n);
		if (b) {
			memset(&im, 0, sizeof(im));
			src = (mem_src_t){ b, n, 0 };
			im.read = mem_read;
			im.ctx = &src;
			im.doc_w = 320;
			im.doc_h = 240;
			g_w = g_h = g_rows = 0;
			ck(z_img_decode_gray(&im, z_img_sniff(b, n), take_row, NULL) == Z_IMG_OK,
				"JPEG decodes to grey");
			ck(im.out_w <= 320 && im.out_h <= 240 && g_rows == im.out_h,
				"reduced by a power of two into the box");
			free(b);
		}
		ck(z_img_decode_gray(&im, Z_IMG_FMT_PNM, NULL, NULL) != Z_IMG_OK, "no row callback is refused");
	}

	printf("zpage: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}

/* zimg.c's convenience file source pulls in the app FS API. */
int fs_open_read(const char *p) { (void)p; return -1; }
int fs_read_chunk(int a, char *b, int c) { (void)a; (void)b; (void)c; return 0; }
int fs_seek(int a, unsigned b) { (void)a; (void)b; return -1; }
void fs_close_handle(int a) { (void)a; }
