/*
 * sw/common/zcfont.c on the host, with the real font files from
 * sw/data/font: loading (and refusing damaged files), metrics, wrapping,
 * the glyph cache, bands. `make -f Makefile.zcanvas fonts`; with a
 * directory argument it also draws a sample card at 100% (1bpp) and at
 * print resolution (grey) to look at.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../zcanvas.h"
#include "../zcfont.h"

static int fails, checks;

#define ck(cond, msg) do { checks++; if (!(cond)) { fails++; \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

static uint8_t scratch[96 * 1024];
static uint8_t cache_mem[256 * 1024];

static uint8_t *load(const char *root, const char *rel, uint32_t *len) {
	char p[512];
	FILE *f;
	long n;
	uint8_t *b;
	snprintf(p, sizeof(p), "%s/%s", root, rel);
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

static void write_pgm(const char *dir, const char *name, const zc_target_t *t) {
	char p[512];
	FILE *f;
	int x, y;
	if (!dir) return;
	snprintf(p, sizeof(p), "%s/%s.pgm", dir, name);
	f = fopen(p, "wb");
	if (!f) return;
	fprintf(f, "P5 %d %d 255\n", t->w, t->h);
	for (y = 0; y < t->h; y++)
		for (x = 0; x < t->w; x++) {
			if (t->fmt == ZC_BITS) fputc(zc_get_bit(t, t->ox + x, t->oy + y) ? 255 : 0, f);
			else fputc(((uint8_t *)t->pix)[y * t->stride + x], f);
		}
	fclose(f);
}

static zc_font_t f_sans, f_sansb, f_serif, f_serifi, f_script, f_gothic,
	f_heavy, f_m9x15, f_m6x13b, f_5x8;

// A card, as slide will draw one: a title, a wrapped body, a caption in
// a bitmap font, and a few faces.
static void card(zc_target_t *t, const zc_xf_t *xf) {
	static const char *body =
		"Zeitlos draws this text with public-domain Hershey stroke fonts. "
		"They scale to any size on the screen and on paper: "
		"\xc3\xa4\xc3\xb6\xc3\xbc \xc3\x9f \xc3\xa9\xc3\xa8 \xc3\xa7 \xc5\x93 "
		"\xe2\x82\xac 12 \xc2\xa9 \xe2\x80\x9cquotes\xe2\x80\x9d \xe2\x80\x94 "
		"\xce\xb1\xce\xb2\xce\xb3 \xe2\x86\x92 done.";
	zc_paint_t black = zc_black();
	zc_style_t title = { &f_serif, ZC_I(40), 0, 0, 0, 0, 0 };
	zc_style_t bs = { &f_sans, ZC_I(16), 0, 0, 0, 0, 0 };
	zc_style_t cap = { &f_m9x15, ZC_I(15), 0, 0, 0, 0, 0 };
	zc_style_t mono = { &f_5x8, ZC_I(8), 0, 0, 0, 0, 0 };
	zc_line_t lines[16];
	zc_fix line, asc;
	int n, i;

	zc_text_draw(t, xf, &title, ZC_I(20), ZC_I(50), "Slides on Zeitlos", 17, &black);
	zc_text_metrics(&bs, &asc, NULL, &line);
	n = zc_text_wrap(&bs, body, (uint32_t)strlen(body), ZC_I(470), lines, 16, NULL);
	for (i = 0; i < n; i++)
		zc_text_draw(t, xf, &bs, ZC_I(20), ZC_I(90) + line * i,
			body + lines[i].start, lines[i].len, &black);
	{
		zc_style_t s = { &f_script, ZC_I(28), 0, 0, 0, 0, 0 };
		zc_text_draw(t, xf, &s, ZC_I(20), ZC_I(220), "Script", 6, &black);
		s.font = &f_gothic;
		zc_text_draw(t, xf, &s, ZC_I(130), ZC_I(220), "Gothic", 6, &black);
		s.font = &f_serifi;
		zc_text_draw(t, xf, &s, ZC_I(250), ZC_I(220), "Italic", 6, &black);
		s.font = &f_heavy;
		zc_text_draw(t, xf, &s, ZC_I(350), ZC_I(220), "Heavy", 5, &black);
		s.font = &f_sansb;
		s.size = ZC_I(20);
		s.underline = 1;
		zc_text_draw(t, xf, &s, ZC_I(20), ZC_I(255), "Sans bold, underlined", 21, &black);
		s.font = &f_sans;
		s.underline = 0;
		s.italic = 1;
		zc_text_draw(t, xf, &s, ZC_I(270), ZC_I(255), "slanted sans", 12, &black);
	}
	zc_text_draw(t, xf, &cap, ZC_I(20), ZC_I(290), "misc-fixed 9x15: crisp at 100%", 30, &black);
	{
		zc_style_t b = cap;
		b.font = &f_m6x13b;
		b.size = ZC_I(13);
		zc_text_draw(t, xf, &b, ZC_I(300), ZC_I(290), "6x13 bold", 9, &black);
	}
	zc_text_draw(t, xf, &mono, ZC_I(20), ZC_I(320), "built-in 5x8 (the desktop's)", 28, &black);
}

int main(int argc, char **argv) {
	const char *root = argc > 1 ? argv[1] : "../../data/font";
	const char *dir = argc > 2 ? argv[2] : NULL;
	uint32_t len;
	uint8_t *d;

	zc_scratch(scratch, sizeof(scratch));

	d = load(root, "zhf/sans.zhf", &len);
	ck(zc_font_open(&f_sans, d, len, NULL), "sans.zhf opens");
	ck(f_sans.kind == ZC_FONT_STROKE && f_sans.cap == 21 && f_sans.upm == 32,
		"stroke metrics: cap 21 of 32");
	ck(!strcmp(f_sans.name, "sans"), "family name from the file");

	// damaged copies are refused, never read past their end
	{
		int i, refused = 0, opened = 0;
		uint8_t *c = malloc(len);
		zc_font_t tmp;
		for (i = 0; i < 400; i++) {
			uint32_t cut = (uint32_t)(rand() % (int)len);
			memcpy(c, d, len);
			if (i & 1) {
				c[32 + rand() % (int)(len - 32)] ^= (uint8_t)(1 + rand() % 255);
				cut = len;
			}
			if (zc_font_open(&tmp, c, cut, NULL)) {
				zc_style_t st = { &tmp, ZC_I(20), 0, 0, 0, 0, 0 };
				opened++;
				(void)zc_text_width(&st, "Hamburgefonts", 13);
			} else refused++;
		}
		ck(refused > 150, "truncated fonts refused");
		(void)opened;
		free(c);
		ck(!zc_font_open(&tmp, "ZHF1", 4, NULL), "a header alone is refused");
	}

#define OPEN(var, rel) do { uint32_t l_; uint8_t *d_ = load(root, rel, &l_); \
	ck(zc_font_open(&var, d_, l_, NULL), rel " opens"); } while (0)
	OPEN(f_sansb, "zhf/sansb.zhf");
	OPEN(f_serif, "zhf/serif.zhf");
	OPEN(f_serifi, "zhf/serifi.zhf");
	OPEN(f_script, "zhf/script.zhf");
	OPEN(f_gothic, "zhf/gothic.zhf");
	OPEN(f_heavy, "zhf/heavy.zhf");
	OPEN(f_m9x15, "zfn/m9x15.zfn");
	OPEN(f_m6x13b, "zfn/m6x13b.zfn");
	zc_font_builtin(&f_5x8, &z_font_5x8, 7, "mono5x8");

	ck(f_m9x15.kind == ZC_FONT_BITMAP && f_m9x15.w == 9 && f_m9x15.h == 15 &&
		f_m9x15.asc == 12, "9x15: cell and ascent");

	// coverage of what text needs
	{
		static const uint32_t need[] = { 'A', 'z', 0xE4, 0xDF, 0x20AC, 0x152,
			0x153, 0x178, 0x3B1, 0x3A9, 0x201C, 0x2014, 0x2026, 0x2192 };
		unsigned i;
		bool all = true;
		for (i = 0; i < sizeof(need) / sizeof(need[0]); i++)
			if (!zc_font_has(&f_serif, need[i])) all = false;
		ck(all, "serif has Latin-9, Greek, quotes, dashes, arrows");
		ck(zc_font_has(&f_m9x15, 0x20AC) && zc_font_has(&f_m9x15, 0x2014),
			"9x15 has the euro and an em dash");
		ck(zc_font_has(&f_5x8, 0xE4) && !zc_font_has(&f_5x8, 0x3B1),
			"built-in 5x8 is Latin-9 only");
	}

	// widths
	{
		zc_style_t st = { &f_sans, ZC_I(32), 0, 0, 0, 0, 0 };
		zc_fix h = zc_text_advance(&st, 'H');
		ck(h == ZC_I(22), "H in sans at 32 = its 22-unit advance");
		ck(zc_text_width(&st, "HH", 2) == 2 * h, "widths add");
		st.size = ZC_I(16);
		ck(zc_text_advance(&st, 'H') == ZC_I(11), "and scale with the size");
		st.track = ZC_I(1);
		ck(zc_text_width(&st, "HHH", 3) == 3 * ZC_I(12), "tracking adds per character");
		st.font = &f_m9x15;
		st.track = 0;
		st.size = ZC_I(15);
		ck(zc_text_width(&st, "\xc3\xa4x\xe2\x82\xac", 6) == ZC_I(27),
			"a 9-pixel cell per character, UTF-8 counted by character");
		st.size = ZC_I(30);
		ck(zc_text_advance(&st, 'x') == ZC_I(18), "a bitmap font at twice its size");
	}

	// wrapping
	{
		static const char *t = "The quick brown fox jumps over the lazy dog";
		zc_style_t st = { &f_sans, ZC_I(16), 0, 0, 0, 0, 0 };
		zc_line_t L[16];
		zc_fix maxw = ZC_I(100);
		int n = zc_text_wrap(&st, t, (uint32_t)strlen(t), maxw, L, 16, NULL), i;
		bool fits = true, joins = true;
		char back[128] = "";
		for (i = 0; i < n; i++) {
			if (L[i].width > maxw) fits = false;
			if (L[i].width != zc_text_width(&st, t + L[i].start, L[i].len)) fits = false;
			strncat(back, t + L[i].start, L[i].len);
			if (i + 1 < n) strcat(back, " ");
		}
		if (strcmp(back, t)) joins = false;
		ck(n >= 3 && n <= 5, "wraps into a few lines");
		ck(fits, "every line fits, and its width is measured right");
		ck(joins, "the lines are the words, in order, nothing lost");
		n = zc_text_wrap(&st, "a\n\nb", 4, 0, L, 16, NULL);
		ck(n == 3 && L[1].len == 0 && L[2].start == 3, "newlines make lines, blank ones too");
		n = zc_text_wrap(&st, "Supercalifragilistic", 20, ZC_I(40), L, 16, NULL);
		ck(n > 1 && L[0].len > 0 && L[0].width <= ZC_I(40), "a long word breaks between letters");
		{
			bool more = false;
			n = zc_text_wrap(&st, t, (uint32_t)strlen(t), maxw, L, 2, &more);
			ck(n == 2 && more, "max_lines stops it, and says there was more");
		}
		n = zc_text_wrap(&st, "", 0, maxw, L, 16, NULL);
		ck(n == 1 && L[0].len == 0, "empty text is one empty line");
		n = zc_text_wrap(&st, "end   ", 6, maxw, L, 16, NULL);
		ck(n == 1 && L[0].len == 3, "trailing spaces are not part of the line");
	}

	// the cache draws the same pixels as the rasterizer
	{
		static uint32_t a[342][16], b[342][16];
		zc_target_t ta, tb;
		zc_xf_t xf = zc_xf_scale(ZC_XF_ONE, 0, 0);
		uint32_t hits, misses;
		memset(a, 0xFF, sizeof(a));
		memset(b, 0xFF, sizeof(b));
		zc_target_bits(&ta, &a[0][0], 16, 512, 342);
		zc_target_bits(&tb, &b[0][0], 16, 512, 342);
		zc_text_cache(NULL, 0);
		card(&ta, &xf);
		zc_text_cache(cache_mem, sizeof(cache_mem));
		card(&tb, &xf);
		card(&tb, &xf);
		zc_text_cache_stats(&hits, &misses);
		ck(!memcmp(a, b, sizeof(a)), "cached glyphs are the rasterizer's pixels");
		ck(hits > misses, "the second drawing comes from the cache");
		write_pgm(dir, "zcfont_card", &ta);

		// a tiny cache that empties itself still draws right
		memset(b, 0xFF, sizeof(b));
		zc_text_cache(cache_mem, 6000);
		card(&tb, &xf);
		ck(!memcmp(a, b, sizeof(a)), "and so does a cache that keeps emptying");
		zc_text_cache(cache_mem, sizeof(cache_mem));

		// zoomed: the 200% view is the card drawn at 2, not doubled pixels
		{
			static uint32_t z[684][32];
			zc_target_t tz;
			zc_xf_t x2 = zc_xf_scale(2 * ZC_XF_ONE, 0, 0);
			memset(z, 0xFF, sizeof(z));
			zc_target_bits(&tz, &z[0][0], 32, 1024, 684);
			card(&tz, &x2);
			write_pgm(dir, "zcfont_card_200", &tz);
		}
	}

	// print resolution, in bands, equals the whole page
	{
		enum { PW = 512 * 4, PH = 342 * 4, BAND = 64 };
		static uint8_t page[PH][PW], band[BAND][PW];
		zc_target_t t;
		zc_xf_t xf = zc_xf_scale(4 * ZC_XF_ONE, 0, 0);
		int b, r, same = 1;
		memset(page, 255, sizeof(page));
		zc_target_gray(&t, &page[0][0], PW, PW, PH);
		t.pat_scale = 4;
		card(&t, &xf);
		write_pgm(dir, "zcfont_card_print", &t);
		for (b = 0; b < PH; b += BAND) {
			memset(band, 255, sizeof(band));
			zc_target_gray(&t, &band[0][0], PW, PW, BAND);
			zc_target_origin(&t, 0, b);
			t.pat_scale = 4;
			card(&t, &xf);
			for (r = 0; r < BAND && b + r < PH; r++)
				if (memcmp(band[r], page[b + r], PW)) same = 0;
		}
		ck(same, "a printed card in 64-row bands equals the whole page");
	}

	printf("zcfont: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
