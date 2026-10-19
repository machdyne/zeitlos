/*
 * slide's renderer on the host, with the real fonts and an image: every
 * kind of object, the editor's 1bpp view against a printed grey page,
 * bands against the whole, text layout. With a directory argument it
 * writes the card to look at. `make -C sw/apps/slide test`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../common/zpool.h"
#include "../deck.h"
#include "../deckio.h"
#include "../render.h"

static int fails, checks;
int hostfs_open_count(void);

#define ck(cond, msg) do { checks++; if (!(cond)) { fails++; \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

static uint8_t mem[8 << 20];
static uint8_t scratch[96 * 1024];
static const char *root = "../../..";

// /sys/fonts/NAME.zhf -> sw/data/font/zhf/NAME.zhf, the rest from the tree
static uint8_t *load(const char *path, uint32_t *len) {
	char p[512];
	FILE *f;
	long n;
	uint8_t *b;
	if (!strncmp(path, "/sys/fonts/", 11)) {
		const char *e = strrchr(path, '.');
		snprintf(p, sizeof(p), "%s/sw/data/font/%s/%s", root, e ? e + 1 : "", path + 11);
	} else {
		snprintf(p, sizeof(p), "%s", path);
	}
	f = fopen(p, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = pool_alloc((size_t)n + 1);
	if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { pool_free(b); b = NULL; }
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
		for (x = 0; x < t->w; x++)
			fputc(t->fmt == ZC_BITS ? (zc_get_bit(t, t->ox + x, t->oy + y) ? 255 : 0)
				: ((uint8_t *)t->pix)[y * t->stride + x], f);
	fclose(f);
}

static const char *deck_text =
	"(deck (version 1) (page \"classic\" landscape) (title \"Render test\")\n"
	" (background \"plain\"\n"
	"  (rect \"band\" (box 0 0 512 34) (fill shade 5) (line 0 16))\n"
	"  (text \"foot\" (box 8 318 300 20) (font \"m6x13\" 13) (text \"misc-fixed footer, 1:1\")))\n"
	" (card \"one\" (background \"plain\")\n"
	"  (text \"title\" (box 0 36 512 56) (font \"serif\" 36 bold) (align center middle)\n"
	"    (text \"Slides on Zeitlos\"))\n"
	"  (text \"body\" (box 20 96 250 150) (font \"sans\" 15) (line 1 16) (pad 6)\n"
	"    (text \"Every object kind, drawn by zcanvas at any scale. This box wraps its words.\\nA new paragraph.\"))\n"
	"  (rrect \"panel\" (box 290 100 200 70) (fill pattern 13) (line 2 16) (radius 12))\n"
	"  (oval \"dot\" (box 300 185 80 60) (fill shade 9) (line 3 16))\n"
	"  (line \"arrow\" (box 390 250 90 -60) (line 2 16) (arrows end))\n"
	"  (poly \"star\" (box 400 185 90 60) (fill pattern 5) (line 1 16) (closed)\n"
	"    (points 445 185 455 205 490 210 465 225 470 245 445 235 420 245 425 225 400 210 435 205))\n"
	"  (image \"pic\" (box 20 250 120 64) (src \"sw/data/images/squirrel.jpg\") (line 1 16))\n"
	"  (image \"gone\" (box 150 255 60 50) (src \"no/such.png\"))\n"
	"  (text \"script\" (box 220 255 160 40) (font \"script\" 26) (text \"Hershey\"))\n"
	"  (text \"hid\" (box 380 300 120 16) (font \"sans\" 12) (hidden) (text \"hidden\"))))\n";

int main(int argc, char **argv) {
	const char *dir = argc > 1 ? argv[1] : NULL;
	deck_t *d;
	char err[96];
	size_t free0;

	if (argc > 2) root = argv[2];
	pool_init(mem, sizeof(mem));
	render_init(scratch, sizeof(scratch), load);
	render_set_dir(root);
	free0 = pool_free_bytes();
	{
		static uint8_t cache[128 * 1024];
		zc_text_cache(cache, sizeof(cache));
	}

	d = deckio_read(deck_text, (uint32_t)strlen(deck_text), err, sizeof(err));
	ck(d != NULL, "the test deck reads");
	if (!d) { printf("  %s\n", err); return 1; }

	ck(render_font_exists("serif") && render_font_exists("m9x15") &&
		render_font_exists("mono5x8") && !render_font_exists("nosuch"), "fonts by name");

	// a style picks the bold face when the family has one
	{
		zc_style_t st;
		render_style(d, d->card[0]->obj[0], &st);
		ck(st.font->kind == ZC_FONT_STROKE && (st.font->flags & ZC_FONT_BOLD) && !st.bold,
			"serif bold is the serifb face, not a synthetic one");
	}

	// pictures load in the background (docs/responsive.md): drawing a
	// card asks for them and draws a placeholder; the main loop's
	// render_load_step() decodes them a slice at a time
	{
		static uint8_t g0[342][512], g1[342][512];
		zc_target_t t;
		zc_xf_t xf = zc_xf_scale(ZC_XF_ONE, 0, 0);
		obj_t *pic = d->card[0]->obj[6], *gone = d->card[0]->obj[7];
		int w = 0, h = 0, steps = 0, finished = 0, y, x, diff = 0;
		bool fin;
		uint16_t pm, last = 0;
		bool rising = true;
		const char *name = NULL;
		char seen[64] = "";

		ck(!render_loading(), "nothing loading before a card is drawn");
		ck(!render_image_size(pic, &w, &h), "no size before it is loaded");
		memset(g0, 255, sizeof(g0));
		zc_target_gray(&t, &g0[0][0], 512, 512, 342);
		render_card(&t, &xf, d, d->card[0], RENDER_EDIT);
		ck(render_loading(), "drawing the card asks for its pictures");
		ck(render_image_state(pic) == RENDER_IMG_WAITING &&
			render_image_state(gone) == RENDER_IMG_WAITING, "both waiting");
		ck(render_image_state(d->card[0]->obj[0]) == RENDER_IMG_NONE, "a text box has no picture");

		// one picture at a time, in many short slices, progress rising
		while (render_loading()) {
			obj_t *o = render_load_step(0, &fin);
			steps++;
			if (fin) finished++;
			if (o == pic && !fin) {
				pm = render_load_progress(&name);
				if (pm < last) rising = false;
				if (name) snprintf(seen, sizeof(seen), "%s", name);
				last = pm;
			}
			if (steps > 100000) break;
		}
		ck(steps > 20, "loaded in slices");
		ck(finished == 2, "each picture finishes once");
		ck(rising && !strcmp(seen, "squirrel.jpg"), "progress rises, named by file");
		ck(render_image_state(pic) == RENDER_IMG_READY, "the picture is ready");
		ck(render_image_size(pic, &w, &h) && w > 0 && h > 0, "image size");
		ck(render_image_state(gone) == RENDER_IMG_FAILED, "a missing image fails");
		ck(!render_image_size(gone, &w, &h), "a missing image has no size");
		ck(hostfs_open_count() == 0, "no file left open");

		// the picture now drawn where the placeholder was
		memset(g1, 255, sizeof(g1));
		zc_target_gray(&t, &g1[0][0], 512, 512, 342);
		render_card(&t, &xf, d, d->card[0], RENDER_EDIT);
		ck(!render_loading(), "a loaded picture is not asked for again");
		for (y = 252; y < 312; y++) for (x = 22; x < 138; x++) if (g0[y][x] != g1[y][x]) diff++;
		ck(diff > 1000, "the picture replaces its placeholder");

		// forgotten (an edit, an undo): back from the recent ones at once
		render_forget(pic);
		render_card(&t, &xf, d, d->card[0], RENDER_EDIT);
		ck(!render_loading() && render_image_state(pic) == RENDER_IMG_READY,
			"a forgotten picture comes back without decoding");
		ck(render_image_size(pic, &x, &y) && x == w && y == h, "the same size");

		// stopped part way, then loaded again
		render_forget(pic);
		render_drop_recent();
		render_card(&t, &xf, d, d->card[0], RENDER_EDIT);
		ck(render_loading(), "dropped from the recent ones: loaded again");
		for (x = 0; x < 5; x++) render_load_step(0, &fin);
		ck(render_image_state(pic) == RENDER_IMG_WAITING && render_load_progress(NULL) > 0,
			"part way through");
		ck(render_load_stop() == 1 && !render_loading(), "Esc stops it");
		ck(render_image_state(pic) == RENDER_IMG_STOPPED, "and it says so");
		ck(hostfs_open_count() == 0 && pool_check(), "stopping frees its file and memory");
		render_card(&t, &xf, d, d->card[0], RENDER_EDIT);
		ck(!render_loading(), "a stopped picture is not asked for again by drawing");
		ck(render_load_retry(pic) && render_loading(), "Enter on it loads it again");
		ck(!render_load_retry(d->card[0]->obj[0]), "a text box has nothing to retry");
		while (render_loading()) render_load_step(50, &fin);
		ck(render_image_state(pic) == RENDER_IMG_READY, "loaded after the retry");

		// asked for without drawing (the show's next card), at the front
		render_forget(pic);
		render_drop_recent();
		render_card_want(d, d->card[0], RENDER_SHOW, true);
		ck(render_loading() && render_image_state(pic) == RENDER_IMG_WAITING,
			"a card's pictures asked for without drawing it");
		ck(render_image_state(gone) == RENDER_IMG_FAILED, "a failed one is not asked for again");
		ck(render_card_has(d, d->card[0], pic) && !render_card_has(d, d->card[0], NULL),
			"which card a picture is on");
		while (render_loading()) render_load_step(50, &fin);
		ck(render_image_state(pic) == RENDER_IMG_READY, "and loaded");
	}

	// the editor's view, 1bpp at 100%
	{
		static uint32_t bits[342][16];
		zc_target_t t;
		zc_xf_t xf = zc_xf_scale(ZC_XF_ONE, 0, 0);
		memset(bits, 0, sizeof(bits));
		zc_target_bits(&t, &bits[0][0], 16, 512, 342);
		render_card(&t, &xf, d, d->card[0], RENDER_EDIT);
		write_pgm(dir, "slide_card_edit", &t);
		ck(zc_get_bit(&t, 256, 200) == 1, "the page is white");
		ck(zc_get_bit(&t, 290, 120) == 0 && zc_get_bit(&t, 291, 120) == 0,
			"a 2-unit frame: two pixels inside the box");
		{
			static uint32_t again[342][16];
			zc_target_t u;
			memset(again, 0, sizeof(again));
			zc_target_bits(&u, &again[0][0], 16, 512, 342);
			render_card(&u, &xf, d, d->card[0], RENDER_EDIT);
			ck(!memcmp(bits, again, sizeof(bits)), "drawing twice (images and glyphs cached) is the same");
		}
		// only a damaged part: the rest stays as it was
		{
			static uint32_t part[342][16];
			zc_target_t u;
			int x, y, same = 1;
			memset(part, 0, sizeof(part));
			zc_target_bits(&u, &part[0][0], 16, 512, 342);
			zc_clip(&u, 280, 90, 500, 260);
			render_card(&u, &xf, d, d->card[0], RENDER_EDIT);
			for (y = 0; y < 342; y++)
				for (x = 0; x < 512; x++) {
					int in = x >= 280 && x < 500 && y >= 90 && y < 260;
					int b = zc_get_bit(&u, x, y);
					if (in && b != zc_get_bit(&t, x, y)) same = 0;
					if (!in && b != 0) same = 0;
				}
			ck(same, "a damaged rectangle redraws exactly that rectangle");
		}
	}

	// a show: hidden objects are not drawn
	{
		static uint32_t a[342][16], b[342][16];
		zc_target_t ta, tb;
		zc_xf_t xf = zc_xf_scale(ZC_XF_ONE, 0, 0);
		zc_target_bits(&ta, &a[0][0], 16, 512, 342);
		zc_target_bits(&tb, &b[0][0], 16, 512, 342);
		render_card(&ta, &xf, d, d->card[0], RENDER_EDIT);
		render_card(&tb, &xf, d, d->card[0], RENDER_SHOW);
		ck(memcmp(a, b, sizeof(a)) != 0, "hidden objects show in the editor only");
	}

	// printed: grey at 4x, in bands, equals the whole page
	{
		enum { PW = 2048, PH = 1368, BAND = 96 };
		static uint8_t page[PH][PW], band[BAND][PW];
		zc_target_t t;
		zc_xf_t xf = zc_xf_scale(4 * ZC_XF_ONE, 0, 0);
		int b, r, same = 1;
		zc_target_gray(&t, &page[0][0], PW, PW, PH);
		t.pat_scale = 4;
		render_card(&t, &xf, d, d->card[0], RENDER_PRINT);
		write_pgm(dir, "slide_card_print", &t);
		for (b = 0; b < PH; b += BAND) {
			zc_target_gray(&t, &band[0][0], PW, PW, BAND);
			zc_target_origin(&t, 0, b);
			t.pat_scale = 4;
			memset(band, 0x5A, sizeof(band));
			render_card(&t, &xf, d, d->card[0], RENDER_PRINT);
			for (r = 0; r < BAND && b + r < PH; r++)
				if (memcmp(band[r], page[b + r], PW)) same = 0;
		}
		ck(same, "a printed card in bands equals the whole page");
		ck(page[4 * 210][4 * 340] > 0 && page[4 * 210][4 * 340] < 255, "a shade prints grey");
	}

	// text layout for the caret
	{
		text_layout_t L;
		obj_t *body = d->card[0]->obj[1];
		render_text_layout(d, body, &L);
		ck(L.n >= 4, "the body wraps into lines");
		ck(L.line[L.n - 1].start + L.line[L.n - 1].len == strlen(body->text),
			"the last line ends the text");
		ck(render_line_x(body, &L, 0) == ZC_I(26), "left aligned at the padding");
		{
			obj_t *t = d->card[0]->obj[0];
			text_layout_t T;
			render_text_layout(d, t, &T);
			ck(T.n == 1 && render_line_x(t, &T, 0) > ZC_I(40), "a centred title");
		}
	}

	// the deck's memory comes back when it goes, image caches included
	deck_free(d);
	render_drop_recent();
	ck(pool_check(), "pool intact");
	{
		// fonts stay loaded (they are kept for the next deck)
		size_t now = pool_free_bytes();
		ck(free0 - now < 200 * 1024, "only the fonts remain");
	}

	printf("render: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}

