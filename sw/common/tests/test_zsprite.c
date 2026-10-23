/*
 * Host test for sprite sheets, sw/common/zsprite.c.
 *
 *   make -C sw/common/tests -f Makefile.zsprite
 *
 * Every palette size read and written back exactly, the defaults, the
 * reduction tables at every tier, "must differ", the 2-colour sheets
 * the pointer's file and hand-drawn art are, and the errors a broken
 * sheet gets.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../zsprite.h"

static int checks, fails;

#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static bool same_sheet(const zsp_sheet_t *a, const zsp_sheet_t *b) {
	int i;
	if (a->p.colours != b->p.colours || a->n != b->n) return false;
	if (memcmp(a->p.slot_rgb, b->p.slot_rgb, sizeof(a->p.slot_rgb))) return false;
	if (a->p.ndiffer != b->p.ndiffer ||
		memcmp(a->p.differ, b->p.differ, sizeof(a->p.differ[0]) * (size_t)a->p.ndiffer)) return false;
	for (i = 0; i < a->p.colours; i++) {
		const zsp_colour_t *x = &a->p.pal[i], *y = &b->p.pal[i];
		if (x->rgb != y->rgb || x->to64 != y->to64 || x->to16 != y->to16 ||
			x->slot != y->slot || x->ink != y->ink || x->given != y->given) {
			printf("  entry %d: %03x %d %d %d %d %x / %03x %d %d %d %d %x\n", i,
				x->rgb, x->to64, x->to16, x->slot, x->ink, x->given,
				y->rgb, y->to64, y->to16, y->slot, y->ink, y->given);
			return false;
		}
	}
	for (i = 0; i < a->n; i++) {
		const zsp_sprite_t *x = &a->spr[i], *y = &b->spr[i];
		if (strcmp(x->name, y->name) || x->w != y->w || x->h != y->h ||
			memcmp(x->px, y->px, sizeof(uint16_t) * x->w * x->h)) return false;
	}
	return true;
}

static void round_trip(int colours) {
	zsp_sheet_t s, t;
	zsp_sprite_t *e;
	char *text, err[80];
	uint32_t len;
	int x, y;

	zsp_init(&s, colours);
	e = zsp_add(&s, "walk0", 13, 7);
	CHECK(e != NULL, "add");
	for (y = 0; y < 7; y++)
		for (x = 0; x < 13; x++)
			e->px[y * 13 + x] = (uint16_t)(((x * 7 + y * 5) % 5) == 0 ? ZSP_T : (x * 31 + y * 17) % colours);
	e = zsp_add(&s, "w2", 2, 1);
	e->px[0] = (uint16_t)(colours - 1);
	CHECK(zsp_add(&s, "w2", 3, 3) == NULL, "a second w2 refused");
	if (colours > 2) {
		s.p.pal[1].rgb = 0x7a3;
		s.p.pal[1].slot = 2;
		s.p.pal[1].given = ZSP_GIVEN_SLOT;
		zsp_defaults(&s.p, 1);
		s.p.differ[0][0] = 1; s.p.differ[0][1] = (uint8_t)(colours - 1);
		s.p.ndiffer = 1;
		s.p.slot_rgb[2] = 0x48c;
	}
	if (colours == 256) { s.p.pal[200].to64 = 3; s.p.pal[200].given |= ZSP_GIVEN_TO64; }
	s.extra = strdup("anim walk 80 walk0 w2\n# a note\n");
	s.extra_len = (uint32_t)strlen(s.extra);

	text = zsp_write(&s, &len);
	CHECK(text != NULL, "%d colours written", colours);
	CHECK(zsp_parse(&t, text, len, err, sizeof(err)), "%d colours read: %s", colours, err);
	CHECK(same_sheet(&s, &t), "%d colours: the same sheet back", colours);
	CHECK(t.extra && strstr(t.extra, "anim walk 80") && strstr(t.extra, "# a note"), "other lines kept");
	{
		uint32_t len2;
		char *again = zsp_write(&t, &len2);
		CHECK(again && len2 == len && !memcmp(again, text, len), "%d colours: written again the same", colours);
		free(again);
	}
	free(text);
	zsp_free(&s);
	zsp_free(&t);
}

static void test_palette(void) {
	zsp_palette_t p;
	int i, a, b, tier;

	zsp_palette_init(&p, 256);
	CHECK(p.pal[0].rgb == 0x000 && p.pal[1].rgb == 0xfff, "C64 first: 0 black, 1 white");
	CHECK(p.pal[0].ink == 0 && p.pal[1].ink == 16, "inks of black and white");
	CHECK(p.pal[0].slot == 0 && p.pal[1].slot == 3, "slots of black and white");
	for (i = 0; i < 16; i++) CHECK(p.pal[i].to16 == i && p.pal[i].to64 == i, "entry %d stands for itself", i);
	for (i = 16; i < 64; i++) CHECK(p.pal[i].to64 == i && p.pal[i].to16 < 16, "entry %d", i);
	for (i = 64; i < 256; i++) CHECK(p.pal[i].to64 < 64 && p.pal[i].to16 < 16, "entry %d", i);
	/* a shade is near its colour */
	CHECK(p.pal[16 + 2].to16 == 2 || p.pal[16 + 2].to16 == 9 || p.pal[16 + 2].to16 == 11,
		"a darker red stands for a red or a dark colour (%d)", p.pal[18].to16);

	/* reduction, tier by tier */
	p.pal[200].to64 = 40; p.pal[200].to16 = 9; p.pal[200].slot = 1; p.pal[200].ink = 5;
	CHECK(zsp_reduce(&p, 200, 256) == 200, "256: itself");
	CHECK(zsp_reduce(&p, 200, 64) == 40, "64: to64");
	CHECK(zsp_reduce(&p, 200, 16) == 9, "16: to16");
	CHECK(zsp_reduce(&p, 200, 4) == 1, "4: the slot");
	CHECK(zsp_reduce(&p, 200, 2) == 0, "2: ink 5 is dark");
	CHECK(zsp_reduce(&p, ZSP_T, 16) == ZSP_T, "transparent stays");
	CHECK(zsp_shown_rgb(&p, 200, 4) == p.slot_rgb[1], "4: the slot's colour");
	CHECK(zsp_shown_rgb(&p, 1, 2) == 0xfff && zsp_shown_rgb(&p, 0, 2) == 0x000, "2: greys");
	p.colours = 16;
	CHECK(zsp_reduce(&p, 9, 64) == 9 && zsp_reduce(&p, 9, 16) == 9, "raising changes nothing");

	/* must differ */
	p.colours = 256;
	p.ndiffer = 1;
	p.differ[0][0] = 200; p.differ[0][1] = 9;
	CHECK(zsp_collision(&p, &a, &b, &tier) && tier == 16 && a == 200 && b == 9,
		"200 and 9 meet at 16 colours (%d)", tier);
	p.pal[200].to16 = 3;
	p.pal[9].ink = 5; p.pal[9].slot = 2;
	CHECK(zsp_collision(&p, &a, &b, &tier) && tier == 2, "then at 2 (%d)", tier);
	p.pal[9].ink = 12;
	CHECK(!zsp_collision(&p, NULL, NULL, NULL), "then nowhere");
}

static void test_two(void) {
	/* what the pointer's file looks like, hand-drawn */
	static const char sheet[] =
		"zsp 1\r\n"
		"sprite normal 4 2\n"
		"#..X\n"
		"\t0.1.\n"
		"anim x 10 normal\n";
	zsp_sheet_t s;
	char err[80];
	char *out;
	uint32_t len;
	CHECK(zsp_parse(&s, sheet, sizeof(sheet) - 1, err, sizeof(err)), "2 colours: %s", err);
	CHECK(s.p.colours == 2 && s.n == 1, "one sprite, 2 colours");
	CHECK(s.spr[0].px[0] == 1 && s.spr[0].px[1] == ZSP_T && s.spr[0].px[3] == 1, "marks are ink");
	CHECK(s.spr[0].px[4] == 0 && s.spr[0].px[5] == ZSP_T && s.spr[0].px[6] == 1, "0, ., 1");
	out = zsp_write(&s, &len);
	CHECK(out && !strncmp(out, "zsp 1\ncolours 2\nanim x 10 normal\nsprite normal 4 2\n\t1..1\n\t0.1.\n", len),
		"written as the pointer's file always was:\n%s", out ? out : "");
	free(out);
	zsp_free(&s);
}

static void test_sixteen(void) {
	static const char sheet[] =
		"zsp 1\ncolours 16\n"
		"colour 9 740 1 5\n"
		"sprite a 3 1\n"
		"9.f\n";
	zsp_sheet_t s;
	uint8_t px[3], op[3];
	char err[80];
	CHECK(zsp_parse(&s, sheet, sizeof(sheet) - 1, err, sizeof(err)), "16 colours: %s", err);
	CHECK(s.p.pal[9].rgb == 0x740 && s.p.pal[9].slot == 1 && s.p.pal[9].ink == 5, "colour 9 as written");
	CHECK(s.p.pal[9].given == 0, "nothing marked given");
	CHECK(s.spr[0].px[0] == 9 && s.spr[0].px[1] == ZSP_T && s.spr[0].px[2] == 15, "pixels");
	zsp_pixels(&s, 0, 4, px, NULL);
	CHECK(px[0] == 1 && px[1] == 0xff, "reduced to 4: slot 1, transparent 0xff");
	zsp_pixels(&s, 0, 2, px, op);
	CHECK(px[0] == 0 && op[0] && !op[1] && op[2], "reduced to 2 with an opacity map");
	zsp_free(&s);
}

static void test_errors(void) {
	static const struct { const char *text, *want; } bad[] = {
		{ "zsp 1\nsprite a 4 2\n....\n...\n", "line 4: short row" },
		{ "zsp 1\ncolours 16\nsprite a 2 1\n.g\n", "line 4: not a colour" },
		{ "zsp 1\ncolours 4\nsprite a 2 1\n.5\n", "line 4: colour past the palette" },
		{ "zsp 1\ncolours 64\nsprite a 2 1\n..4\n", "line 4: short row" },
		{ "zsp 1\ncolours 64\nsprite a 2 1\n..4x\n", "line 4: not two hex digits" },
		{ "zsp 1\ncolours 64\nsprite a 2 1\n..44\n", "line 4: colour past the palette" },
		{ "zsp 1\ncolours 5\n", "line 2: colours is" },
		{ "zsp 1\nsprite a 2 2\n..\n", "line 4: the sheet ends inside" },
		{ "zsp 1\nsprite a 1 1\n.\nsprite a 1 1\n.\n", "line 4: a second sprite" },
		{ "zsp 1\ncolours 16\ncolour 3 12\n", "line 3: colour needs an RGB" },
		{ "zsp 1\ncolours 16\ncolour 3 123 9\n", "line 3: slot past 3" },
	};
	unsigned i;
	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		zsp_sheet_t s;
		char err[80] = "";
		CHECK(!zsp_parse(&s, bad[i].text, (uint32_t)strlen(bad[i].text), err, sizeof(err)) &&
			!strncmp(err, bad[i].want, strlen(bad[i].want)), "%u: \"%s\", wanted \"%s\"", i, err, bad[i].want);
		zsp_free(&s);
	}
}

/* The palette as a colour picture's bytes (zbm.h) and back. */
static void test_bin(void) {
	static const int counts[5] = { 2, 4, 16, 64, 256 };
	int t, i;
	for (t = 0; t < 5; t++) {
		zsp_palette_t p, q;
		uint8_t b[12 + 8 * 256 + 2 * ZSP_DIFFER_MAX + 4];
		uint32_t used = 0, n;
		zsp_palette_init(&p, counts[t]);
		p.pal[1].rgb = 0x5a3;
		p.pal[1].ink = 11;
		p.pal[1].given = ZSP_GIVEN_INK;
		p.slot_rgb[2] = 0x0f0;
		p.ndiffer = 3;
		for (i = 0; i < 3; i++) { p.differ[i][0] = (uint8_t)(i & 1); p.differ[i][1] = (uint8_t)(1 + i / 2); }
		n = zsp_palette_bin_size(&p);
		CHECK(n == ((12u + 8u * (uint32_t)counts[t] + 6u + 3u) & ~3u) && n <= sizeof(b), "%d: size %u", counts[t], n);
		memset(b, 0xee, sizeof(b));
		zsp_palette_to_bin(&p, b);
		CHECK(zsp_palette_from_bin(&q, b, n, &used) && used == n, "%d: read back", counts[t]);
		CHECK(q.colours == counts[t] && q.ndiffer == (counts[t] > 2 ? 3 : 2) && q.slot_rgb[2] == 0x0f0,
			"%d: count, pairs (one past a 2-colour palette dropped), slots", counts[t]);
		for (i = 0; i < counts[t]; i++)
			if (memcmp(&p.pal[i], &q.pal[i], sizeof(zsp_colour_t))) { CHECK(0, "%d: entry %d", counts[t], i); break; }
		CHECK(!zsp_palette_from_bin(&q, b, n - 1, &used), "%d: cut short", counts[t]);
	}
	{
		uint8_t b[16] = { 3, 0 };
		zsp_palette_t q;
		uint32_t used;
		CHECK(!zsp_palette_from_bin(&q, b, sizeof(b), &used), "a count of 3 is not one");
	}
}

int main(void) {
	test_palette();
	round_trip(2);
	round_trip(4);
	round_trip(16);
	round_trip(64);
	round_trip(256);
	test_two();
	test_sixteen();
	test_errors();
	test_bin();
	printf("zsprite: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
