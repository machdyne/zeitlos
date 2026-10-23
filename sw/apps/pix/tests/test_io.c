/*
 * pix -- the document and the file formats, on the host.
 *
 *   make -C sw/apps/pix/tests
 *
 * Every format read and written back: the release's own dock.zic and
 * the card's .zfn fonts must come back byte for byte, a ZBM and a .zsp
 * must survive the round trip pixel for pixel, the overrides file must
 * carry exactly the changed icons and lay itself over the release's,
 * and cells must keep their pixels through inserts and deletes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../px_doc.h"
#include "../px_io.h"

static int checks, failures;

#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t *slurp(const char *path, uint32_t *len) {
	FILE *f = fopen(path, "rb");
	uint8_t *b;
	long n;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc((size_t)n + 1);
	if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
	fclose(f);
	*len = (uint32_t)n;
	return b;
}

static void same_file(const char *path, int kind) {
	uint32_t len, olen = 0;
	uint8_t *data = slurp(path, &len), *out;
	px_doc_t d;
	char err[80] = "";
	if (!data) { CHECK(0, "cannot read %s", path); return; }
	px_doc_init(&d, 0);
	CHECK(px_detect(data, len, path) == kind, "%s: detected as %d", path, px_detect(data, len, path));
	CHECK(px_read(&d, kind, data, len, err, sizeof(err)), "%s: %s", path, err);
	out = px_write(&d, false, &olen);
	CHECK(out && olen == len && !memcmp(out, data, len),
		"%s: written back differs (%u bytes, was %u)", path, olen, len);
	free(out);
	px_doc_free(&d);
	free(data);
}

static void test_doc(void) {
	px_doc_t d;
	int a, b, c, x;
	px_doc_init(&d, PX_ZSP);
	a = px_cell_insert(&d, -1, 9, 3, "a");
	b = px_cell_insert(&d, -1, 17, 5, "b");
	c = px_cell_insert(&d, 0, 4, 4, "c");          /* before both */
	CHECK(a == 0 && b == 1 && c == 0, "insert order %d %d %d", a, b, c);
	CHECK(!strcmp(d.cell[1].name, "a") && !strcmp(d.cell[2].name, "b"), "names moved");
	for (x = 0; x < 17; x++) px_set(&d, 2, x, x % 5, 1);
	px_set(&d, 1, 8, 2, 1);
	CHECK(px_get(&d, 1, 8, 2) == 1 && px_get(&d, 1, 7, 2) == ZSP_T, "a's pixel");
	px_cell_delete(&d, 0);                          /* c, first in the pool? no: last */
	CHECK(d.ncells == 2 && !strcmp(d.cell[0].name, "a"), "delete");
	CHECK(px_get(&d, 0, 8, 2) == 1, "a kept its pixel");
	for (x = 0; x < 17; x++) CHECK(px_get(&d, 1, x, x % 5) == 1, "b kept (%d)", x);
	px_cell_delete(&d, 0);                          /* a: first in the pool */
	for (x = 0; x < 17; x++) CHECK(px_get(&d, 0, x, x % 5) == 1, "b moved down (%d)", x);
	CHECK(d.bits_len == px_cell_bytes(&d, 17, 5), "pool closed (%u)", d.bits_len);
	b = px_cell_dup(&d, 0, "b2");
	CHECK(b == 1 && px_get(&d, 1, 16, 1) == 1 && (d.cell[1].flags & PX_CELL_CHANGED), "dup");
	px_doc_free(&d);

	/* glyph slots */
	px_doc_init(&d, PX_ZFN);
	{
		static const uint32_t cps[] = { 0x41, 0x20, 0x7a, 0x30 };
		bool ex;
		int i, k;
		for (k = 0; k < 4; k++) {
			i = px_glyph_slot(&d, cps[k], &ex);
			CHECK(!ex, "new glyph");
			i = px_cell_insert(&d, i, 5, 8, "g");
			d.cell[i].code = cps[k];
		}
		for (k = 1; k < 4; k++) CHECK(d.cell[k - 1].code < d.cell[k].code, "glyphs in order");
		i = px_glyph_slot(&d, 0x41, &ex);
		CHECK(ex && d.cell[i].code == 0x41, "found A");
	}
	px_doc_free(&d);
}

static void test_zic(void) {
	const char *path = "../../../data/icons/dock.zic";
	uint32_t len, olen;
	uint8_t *data = slurp(path, &len), *ovr, *out;
	px_doc_t d, o;
	char err[80] = "";
	int i, n0;

	same_file(path, PX_ZIC);
	if (!data) return;

	/* change one icon and add one: the overrides carry exactly those */
	px_doc_init(&d, 0);
	px_read(&d, PX_ZIC, data, len, err, sizeof(err));
	n0 = d.ncells;
	i = px_find(&d, "term");
	CHECK(i >= 0, "term in dock.zic");
	px_set(&d, i, 0, 0, !px_get(&d, i, 0, 0));
	px_touch(&d, i);
	i = px_cell_insert(&d, d.ncells, 32, 32, "newapp");
	strcpy(d.cell[i].spoken, "New app");
	px_set(&d, i, 5, 6, 1);
	px_touch(&d, i);
	ovr = px_write(&d, true, &olen);
	CHECK(ovr != NULL, "overrides written");

	px_doc_init(&o, 0);
	CHECK(px_read(&o, PX_ZIC, ovr, olen, err, sizeof(err)), "overrides read: %s", err);
	CHECK(o.ncells == 2 && !strcmp(o.cell[0].name, "newapp") && !strcmp(o.cell[1].name, "term"),
		"overrides hold newapp and term (%d)", o.ncells);
	px_doc_free(&o);

	/* and laid over a fresh copy of the release they give the edit back */
	px_read(&o, PX_ZIC, data, len, err, sizeof(err));
	CHECK(px_zic_overlay(&o, ovr, olen, err, sizeof(err)), "overlay: %s", err);
	CHECK(o.ncells == n0 + 1, "one icon added (%d)", o.ncells);
	i = px_find(&o, "newapp");
	CHECK(i >= 0 && px_get(&o, i, 5, 6) == 1 && !strcmp(o.cell[i].spoken, "New app"), "newapp laid over");
	CHECK(i > 0 && strcmp(o.cell[i - 1].name, "newapp") < 0, "in name order");
	i = px_find(&o, "term");
	CHECK(i >= 0 && (o.cell[i].flags & PX_CELL_CHANGED), "term flagged");
	{
		int j = px_find(&d, "term");
		CHECK(!memcmp(px_cell_rows(&o, i), px_cell_rows(&d, j), 128), "term's pixels");
	}
	out = px_write(&o, true, &olen);
	free(out);
	px_doc_free(&o);
	px_doc_free(&d);
	free(ovr);
	free(data);
}

static void test_zbm(void) {
	px_doc_t d, e;
	uint8_t *out;
	uint32_t len;
	char err[80] = "";
	int x, y, i;
	px_doc_init(&d, PX_ZBM);
	i = px_cell_insert(&d, -1, 45, 7, "picture");
	for (y = 0; y < 7; y++)
		for (x = 0; x < 45; x++)
			px_set(&d, i, x, y, ((x * 7 + y * 3) % 5) == 0);
	out = px_write(&d, false, &len);
	CHECK(out && len == 16 + 7 * 8, "ZBM size %u", len);
	CHECK(out[16] == 0x21, "ZBM bit order: %02x", out[16]);   /* pixels 0 and 5, LSB first */
	px_doc_init(&e, 0);
	CHECK(px_read(&e, PX_ZBM, out, len, err, sizeof(err)), "ZBM read: %s", err);
	for (y = 0; y < 7; y++)
		for (x = 0; x < 45; x++)
			CHECK(px_get(&e, 0, x, y) == px_get(&d, 0, x, y), "ZBM pixel %d,%d", x, y);
	free(out);
	px_doc_free(&d);
	px_doc_free(&e);
}

static void test_zsp(void) {
	static const char sheet[] =
		"zsp 1\r\n"
		"# a test\n"
		"sprite normal 16 16\n"
		"\t................\n"
		"\t................\n"
		"\t................\n"
		"\t................\n"
		"\t................\n"
		"\t................\n"
		"\t................\n"
		"\t................\n"
		"\t........#.......\n"
		"\t........##......\n"
		"\t........#.#.....\n"
		"\t........#..#....\n"
		"\t........#...#...\n"
		"\t........#.###...\n"
		"\t........##......\n"
		"\t........#.......\n"
		"sprite busy 16 16\n"
		"0000000000000000\n0000000000000000\n0000000000000000\n0000000000000000\n"
		"0000000000000000\n0000000000000000\n0000000000000000\n0000000011000000\n"
		"0000000011000000\n0000000000000000\n0000000000000000\n0000000000000000\n"
		"0000000000000000\n0000000000000000\n0000000000000000\n0000000000000000\n"
		"anim run 80 normal busy\n";
	px_doc_t d, e;
	uint8_t *out;
	uint32_t len;
	char err[80] = "";
	px_doc_init(&d, 0);
	CHECK(px_detect((const uint8_t *)sheet, sizeof(sheet) - 1, NULL) == PX_ZSP, "zsp detected");
	CHECK(px_read(&d, PX_ZSP, (const uint8_t *)sheet, sizeof(sheet) - 1, err, sizeof(err)), "zsp: %s", err);
	CHECK(d.ncells == 2 && d.cursor, "the pointer's two sprites");
	CHECK(px_get(&d, 0, 8, 8) == 1 && px_get(&d, 0, 12, 13) == 1 && px_get(&d, 0, 9, 8) == ZSP_T,
		"normal's pixels: '#' ink, '.' transparent");
	CHECK(px_get(&d, 1, 8, 7) == 1 && px_get(&d, 1, 9, 8) == 1 && px_get(&d, 1, 0, 0) == 0,
		"busy's pixels: '0' is colour 0");
	out = px_write(&d, false, &len);
	px_doc_init(&e, 0);
	CHECK(px_read(&e, PX_ZSP, out, len, err, sizeof(err)), "zsp again: %s", err);
	CHECK(e.ncells == 2 && !memcmp(e.bits, d.bits, d.bits_len), "zsp round trip");
	CHECK(e.extra_len == d.extra_len && strstr(e.extra, "anim run") && strstr(e.extra, "# a test"),
		"other lines kept");
	free(out);
	px_doc_free(&e);

	/* a short row is refused with its line */
	CHECK(!px_read(&e, PX_ZSP, (const uint8_t *)"sprite a 4 2\n....\n...\n", 22, err, sizeof(err)) &&
		strstr(err, "line 3"), "short row: %s", err);
	px_doc_free(&e);

	/* 64 colours: the palette is the document's, and back out as it was */
	{
		static const char c64s[] =
			"zsp 1\ncolours 64\nslots 000 555 aaa fff\n"
			"colour 0 000 0 0 0\ncolour 1 fff 1 3 16\ncolour 40 a62 9! 1 6\n"
			"differ 1 40\nsprite a 3 1\n\t..2803\n";
		uint8_t *o2;
		uint32_t l2;
		CHECK(px_read(&e, PX_ZSP, (const uint8_t *)c64s, sizeof(c64s) - 1, err, sizeof(err)), "64: %s", err);
		CHECK(e.depth == 16 && e.pal.colours == 64 && e.pal.pal[40].rgb == 0xa62 &&
			e.pal.pal[40].to16 == 9 && (e.pal.pal[40].given & ZSP_GIVEN_TO16), "64: the palette");
		CHECK(px_get(&e, 0, 0, 0) == ZSP_T && px_get(&e, 0, 1, 0) == 40 && px_get(&e, 0, 2, 0) == 3, "64: pixels");
		o2 = px_write(&e, false, &l2);
		CHECK(o2 && strstr((char *)o2, "colour 40 a62 9! 1 6\n") && strstr((char *)o2, "differ 1 40\n") &&
			strstr((char *)o2, "\t..2803\n"), "64: written back:\n%s", o2 ? (char *)o2 : "");
		free(o2);
	}
	px_doc_free(&e);
	px_doc_free(&d);
}

/* Colour pictures: a ZBM with planes and a palette (zbm.h). */
static void test_zbm_colour(void) {
	static const int counts[4] = { 4, 16, 64, 256 }, planes[4] = { 2, 4, 6, 8 };
	px_doc_t d, e;
	char err[80];
	int t, x, y, c;
	for (t = 0; t < 4; t++) {
		uint8_t *out, *o2;
		uint32_t len, l2, pal;
		px_doc_init(&d, PX_ZBM);
		d.depth = 8;
		zsp_palette_init(&d.pal, counts[t]);
		d.pal.pal[2].rgb = 0x123;
		d.pal.pal[2].ink = 7;
		d.pal.pal[2].given = ZSP_GIVEN_INK;
		d.pal.differ[0][0] = 1; d.pal.differ[0][1] = 2; d.pal.ndiffer = 1;
		c = px_cell_insert(&d, 0, 37, 5, "picture");
		CHECK(c == 0 && px_get(&d, 0, 3, 3) == 0, "a new colour picture is colour 0");
		for (y = 0; y < 5; y++)
			for (x = 0; x < 37; x++) px_set(&d, 0, x, y, (x * 7 + y * 3) % counts[t]);
		out = px_write(&d, false, &len);
		pal = 12 + 8 * (uint32_t)counts[t] + 2;
		pal = (pal + 3) & ~3u;
		CHECK(out && len == 16 + pal + 5 * 8 * (uint32_t)planes[t], "%d colours: size %u", counts[t], len);
		CHECK(out && out[12] == planes[t] && out[16] == (counts[t] & 0xff) && out[17] == (counts[t] >> 8),
			"%d colours: %d planes in the flags, the count", counts[t], planes[t]);
		/* row 0, pixel 1 is (7 % n): bit k in plane k, LSB leftmost */
		for (x = 0; out && x < planes[t]; x++)
			CHECK(((out[16 + pal + (uint32_t)x * 8] >> 1) & 1) == ((7 % counts[t]) >> x & 1),
				"%d colours: pixel 1's bit %d in plane %d", counts[t], x, x);
		px_doc_init(&e, 0);
		CHECK(px_read(&e, PX_ZBM, out, len, err, sizeof(err)), "%d colours: read: %s", counts[t], err);
		CHECK(e.depth == 8 && e.pal.colours == counts[t] && e.pal.pal[2].rgb == 0x123 &&
			e.pal.pal[2].ink == 7 && e.pal.pal[2].given == ZSP_GIVEN_INK && e.pal.ndiffer == 1 &&
			e.pal.differ[0][1] == 2, "%d colours: the palette comes back", counts[t]);
		for (y = 0; y < 5; y++)
			for (x = 0; x < 37; x++)
				if (px_get(&e, 0, x, y) != px_get(&d, 0, x, y)) {
					CHECK(0, "%d colours: pixel %d,%d", counts[t], x, y);
					y = 5;
					break;
				}
		o2 = px_write(&e, false, &l2);
		CHECK(o2 && l2 == len && !memcmp(o2, out, len), "%d colours: written back byte for byte", counts[t]);
		/* cut short, and a palette that is not one */
		CHECK(!px_read(&e, PX_ZBM, out, len - 1, err, sizeof(err)), "%d colours: cut short", counts[t]);
		out[16] = 3;
		CHECK(!px_read(&e, PX_ZBM, out, len, err, sizeof(err)), "a count of 3 is refused");
		free(out);
		free(o2);
		px_doc_free(&e);
		px_doc_free(&d);
	}
	/* 1024 x 1024 in colour is past the limit */
	{
		uint8_t h[16 + 1100];
		zsp_palette_t p;
		memset(h, 0, sizeof(h));
		memcpy(h, "ZBM1", 4);
		h[5] = 4; h[9] = 4;          /* 1024 x 1024 */
		h[12] = 4;
		zsp_palette_init(&p, 16);
		zsp_palette_to_bin(&p, h + 16);
		px_doc_init(&e, 0);
		CHECK(!px_read(&e, PX_ZBM, h, sizeof(h), err, sizeof(err)) && strstr(err, "too big"), "too big: %s", err);
		px_doc_free(&e);
	}
}

int main(void) {
	static const char *fonts[] = { "m6x13", "m7x13", "m10x20", "m8x13" };
	char path[96];
	unsigned i;

	test_doc();
	test_zic();
	test_zbm();
	test_zbm_colour();
	test_zsp();
	for (i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
		snprintf(path, sizeof(path), "../../../data/font/zfn/%s.zfn", fonts[i]);
		same_file(path, PX_ZFN);
	}

	printf("pix io: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
