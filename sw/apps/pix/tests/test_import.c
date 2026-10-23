/*
 * pix -- pictures from elsewhere (px_import.c, through zimg.c's colour
 * rows), on the host. The pictures are made by gen_import.py.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../px_doc.h"
#include "../px_io.h"
#include "../px_edit.h"
#include "../px_import.h"

/* zimg.c's file source is not used here: the files are in memory */
int fs_open_read(const char *p) { (void)p; return -1; }
int fs_read_chunk(int h, void *b, int n) { (void)h; (void)b; (void)n; return -1; }
int fs_seek(int h, uint32_t o) { (void)h; (void)o; return 0; }
int fs_close_handle(int h) { (void)h; return 1; }

static int checks, failures;

#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *dir = "out/import";

static uint8_t *slurp(const char *name, uint32_t *len) {
	char p[256];
	FILE *f;
	uint8_t *b;
	long n;
	snprintf(p, sizeof(p), "%s/%s", dir, name);
	f = fopen(p, "rb");
	if (!f) { printf("cannot open %s\n", p); exit(2); }
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc((size_t)n);
	if (fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
	fclose(f);
	*len = (uint32_t)n;
	return b;
}

/* The index of RGB444 `rgb` in d's palette, or -1. */
static int entry(const px_doc_t *d, uint16_t rgb) {
	int i;
	for (i = 0; i < d->pal.colours; i++) if (d->pal.pal[i].rgb == rgb) return i;
	return -1;
}

static void picture(const char *name, int w, int h, int seen, int tier) {
	px_doc_t d;
	px_src_t s;
	px_mem_t m;
	uint32_t len;
	uint8_t *b = slurp(name, &len);
	char err[80] = "";
	int n = -1;
	px_doc_init(&d, 0);
	px_src_mem(&s, &m, b, len);
	CHECK(px_import_picture(&d, &s, &n, err, sizeof(err)), "%s: %s", name, err);
	CHECK(d.kind == PX_ZBM && d.depth == 8 && d.ncells == 1, "%s: a colour picture", name);
	CHECK(d.ncells == 1 && d.cell[0].w == w && d.cell[0].h == h, "%s: %d x %d (%d x %d)", name, w, h,
		d.ncells ? d.cell[0].w : 0, d.ncells ? d.cell[0].h : 0);
	if (seen >= 0) CHECK(n == seen, "%s: %d colours seen (%d)", name, seen, n);
	CHECK(d.pal.colours == tier, "%s: a palette of %d (%d)", name, tier, d.pal.colours);
	px_doc_free(&d);
	free(b);
}

int main(int argc, char **argv) {
	px_doc_t d;
	px_src_t s;
	px_mem_t m;
	px_piece_t p = { 0, 0, NULL };
	uint32_t len;
	uint8_t *b;
	char err[80];
	int n, x;

	if (argc > 1) dir = argv[1];

	/* -- a new picture -- */
	/* pal.png: 5 colours, one see-through (black, as it has no colour of
	 * its own to be: it counts nothing) */
	b = slurp("pal.png", &len);
	px_doc_init(&d, 0);
	px_src_mem(&s, &m, b, len);
	CHECK(px_import_picture(&d, &s, &n, err, sizeof(err)), "pal.png: %s", err);
	CHECK(n == 4, "pal.png: 4 opaque colours (%d)", n);
	CHECK(d.pal.colours == 4, "pal.png: 4 colours, so a palette of 4 (%d)", d.pal.colours);
	CHECK(entry(&d, 0x000) >= 0 && entry(&d, 0xfff) >= 0 && entry(&d, 0xc42) >= 0 && entry(&d, 0x28e) >= 0,
		"pal.png: every colour kept, as RGB444");
	/* columns of 4: colour (x / 4) % 5 */
	CHECK(px_get(&d, 0, 0, 0) == entry(&d, 0x000) && px_get(&d, 0, 4, 0) == entry(&d, 0xfff) &&
		px_get(&d, 0, 8, 5) == entry(&d, 0xc42) && px_get(&d, 0, 13, 9) == entry(&d, 0x28e),
		"pal.png: the pixels");
	/* written as a colour ZBM and read back */
	{
		uint8_t *o;
		uint32_t ol;
		px_doc_t e;
		o = px_write(&d, false, &ol);
		px_doc_init(&e, 0);
		CHECK(o && px_read(&e, PX_ZBM, o, ol, err, sizeof(err)) && e.depth == 8 && e.pal.colours == 4 &&
			px_get(&e, 0, 8, 5) == px_get(&d, 0, 8, 5), "pal.png: saved as a ZBM: %s", err);
		/* and that ZBM imports too (zimg reads colour ZBMs) */
		{
			px_doc_t f2;
			px_src_t s2;
			px_mem_t m2;
			px_doc_init(&f2, 0);
			px_src_mem(&s2, &m2, o, ol);
			CHECK(px_import_picture(&f2, &s2, &n, err, sizeof(err)) && n == 4 &&
				f2.pal.pal[px_get(&f2, 0, 8, 5)].rgb == 0xc42,
				"a colour ZBM imports, its colours kept: %s", err);
			px_doc_free(&f2);
		}
		free(o);
		px_doc_free(&e);
	}
	px_doc_free(&d);
	free(b);

	picture("pal.gif", 20, 10, 5, 16);           /* the see-through index came out black */
	picture("pal.bmp", 20, 10, 5, 16);
	picture("rgba.png", 16, 8, 1, 4);
	picture("many.png", 64, 64, -1, 256);
	picture("big.png", 400, 300, 1, 4);           /* 800 x 600, halved to fit */

	/* -- a piece, into a palette of the document's -- */
	px_doc_init(&d, PX_ZSP);
	zsp_palette_init(&d.pal, 16);                 /* the C64's: 2 is red, 6 blue */
	b = slurp("rgba.png", &len);
	px_src_mem(&s, &m, b, len);
	CHECK(px_import_piece(&d, &s, &p, err, sizeof(err)), "rgba.png as a piece: %s", err);
	CHECK(p.w == 16 && p.h == 8, "piece: 16 x 8");
	CHECK(p.px && p.px[0] != ZSP_T && d.pal.pal[p.px[0]].rgb >> 8 > 4, "piece: the red half is the reddest entry (%d)",
		p.px ? p.px[0] : -1);
	for (x = 8, n = 0; p.px && x < 16; x++) n += p.px[3 * 16 + x] == ZSP_T;
	CHECK(n == 8, "piece: the see-through half is see-through");
	free(b);

	b = slurp("pal.png", &len);
	px_src_mem(&s, &m, b, len);
	CHECK(px_import_piece(&d, &s, &p, err, sizeof(err)), "pal.png as a piece: %s", err);
	CHECK(p.px[0] == 0 && p.px[4] == 1, "piece: black is 0, white 1 (exact matches)");
	CHECK(p.px[16] == ZSP_T, "piece: the tRNS index is see-through");
	free(b);
	px_doc_free(&d);

	/* two colours: white is ink */
	px_doc_init(&d, PX_ZIC);
	b = slurp("pal.png", &len);
	px_src_mem(&s, &m, b, len);
	CHECK(px_import_piece(&d, &s, &p, err, sizeof(err)), "into an icon set: %s", err);
	CHECK(p.px[0] == 0 && p.px[4] == 1, "two colours: black clear, white ink");
	free(b);
	px_piece_free(&p);
	px_doc_free(&d);

	/* not a picture */
	{
		static const uint8_t junk[40] = "this is not a picture of anything at all";
		px_doc_init(&d, 0);
		px_src_mem(&s, &m, junk, sizeof(junk));
		CHECK(!px_import_picture(&d, &s, &n, err, sizeof(err)), "junk is refused");
		CHECK(!px_import_sniff(junk, sizeof(junk)), "and not sniffed as one");
		px_doc_free(&d);
	}

	printf("pix import: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
