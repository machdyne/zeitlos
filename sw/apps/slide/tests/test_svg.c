/*
 * zpsvg (sw/common/zpsvg.c) on the host: decks drawn as print jobs, the
 * way slide's /print and /export svg do, each page written as SVG and
 * checked. `make -C sw/apps/slide test`; the files are left in
 * /tmp/slide/svg to look at.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../common/zpool.h"
#include "../../../common/zpsvg.h"
#include "../deck.h"
#include "../deckio.h"
#include "../render.h"
#include "../printjob.h"

static int fails, checks;

#define ck(cond, msg) do { checks++; if (!(cond)) { fails++; \
	printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

static uint8_t mem[8 << 20];
static uint8_t scratch[96 * 1024];
static const char *root = "../../..";

static uint8_t *load(const char *path, uint32_t *len) {
	char p[600];
	FILE *f;
	long n;
	uint8_t *b;
	if (!strncmp(path, "/sys/fonts/", 11)) {
		const char *e = strrchr(path, '.');
		snprintf(p, sizeof(p), "%s/sw/data/font/%s/%s", root, e ? e + 1 : "", path + 11);
	} else if (path[0] == '/') snprintf(p, sizeof(p), "%s/sw/data%s", root, path + (strncmp(path, "/data/", 6) ? 0 : 5));
	else snprintf(p, sizeof(p), "%s", path);
	f = fopen(p, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = pool_alloc((size_t)n + 1);
	if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { pool_free(b); b = NULL; }
	fclose(f);
	if (b) b[n] = 0;
	*len = (uint32_t)n;
	return b;
}

typedef struct { uint8_t *b; uint32_t n, cap; } buf_t;
static bool to_buf(void *ctx, const void *d, uint32_t len) {
	buf_t *j = ctx;
	if (j->n + len > j->cap) return false;
	memcpy(j->b + j->n, d, len);
	j->n += len;
	return true;
}
static bool svg_put(void *ctx, const char *s, uint32_t len) { return to_buf(ctx, s, len); }
static uint8_t *svg_load(void *ctx, const char *path, uint32_t *len) { (void)ctx; return load(path, len); }
static void svg_release(void *ctx, uint8_t *p) { (void)ctx; pool_free(p); }
static bool svg_font(void *ctx, const char *name, const zc_font_t **out) {
	(void)ctx;
	*out = render_font_named(name);
	return *out != NULL;
}

static uint8_t jobmem[4 << 20];
static uint8_t svgmem[4 << 20];

// Tags open and close in order: <x ...>, </x>, <x .../>, <?...?>.
static bool balanced(const char *s, uint32_t n, char *why, int whyn) {
	char stack[32][24];
	int sp = 0;
	uint32_t i = 0;
	while (i < n) {
		uint32_t j, k;
		if (s[i] != '<') { i++; continue; }
		if (s[i + 1] == '?') { while (i < n && s[i] != '>') i++; continue; }
		for (j = i + 1; j < n && s[j] != '>'; j++)
			if (s[j] == '<') { snprintf(why, whyn, "'<' inside a tag at %u", j); return false; }
		if (j >= n) { snprintf(why, whyn, "unclosed tag"); return false; }
		if (s[i + 1] == '/') {
			char name[24];
			for (k = 0; k < 23 && s[i + 2 + k] != '>'; k++) name[k] = s[i + 2 + k];
			name[k] = 0;
			if (!sp || strcmp(stack[sp - 1], name)) { snprintf(why, whyn, "</%s> at %u", name, i); return false; }
			sp--;
		} else if (s[j - 1] != '/') {
			if (sp == 32) { snprintf(why, whyn, "too deep"); return false; }
			for (k = 0; k < 23 && s[i + 1 + k] != ' ' && s[i + 1 + k] != '>'; k++) stack[sp][k] = s[i + 1 + k];
			stack[sp++][k] = 0;
		}
		i = j + 1;
	}
	if (sp) { snprintf(why, whyn, "<%s> not closed", stack[sp - 1]); return false; }
	return true;
}

static int count(const char *s, uint32_t n, const char *what) {
	int c = 0;
	size_t l = strlen(what);
	uint32_t i;
	for (i = 0; i + l <= n; i++) if (!memcmp(s + i, what, l)) c++;
	return c;
}

static deck_t *open_deck(const char *path) {
	uint32_t len;
	char err[96];
	uint8_t *text = load(path, &len);
	deck_t *d;
	if (!text) return NULL;
	d = deckio_read((const char *)text, len, err, sizeof(err));
	pool_free(text);
	if (!d) printf("%s: %s\n", path, err);
	return d;
}

// A deck's cards as SVG pages; totals of a few tags over all of them.
typedef struct { int pages, paths, texts, images, patterns, clips; uint32_t bytes; } tally_t;

static bool deck_svg(deck_t *d, const char *name, tally_t *t) {
	buf_t job = { jobmem, 0, sizeof(jobmem) };
	zp_writer_t w;
	int i;
	memset(t, 0, sizeof(*t));
	zp_writer_init(&w, to_buf, &job);
	zp_job(&w, "title", name);
	if (!printjob_pages(d, 0, -1, &w)) return false;
	for (i = 0; i < d->ncard; i++) {
		buf_t out = { svgmem, 0, sizeof(svgmem) };
		zp_svg_t s = { svg_put, svg_load, svg_release, svg_font, &out, 0 };
		char why[64], fn[200];
		FILE *f;
		bool ok = zp_svg_page(&s, job.b, job.n, i);
		ck(ok, "page converted");
		if (!ok) return false;
		ck(!memcmp(out.b, "<?xml", 5), "starts as XML");
		ck(out.n > 7 && !memcmp(out.b + out.n - 7, "</svg>\n", 7), "ends with </svg>");
		if (!balanced((const char *)out.b, out.n, why, sizeof(why))) {
			printf("FAIL %s card %d: %s\n", name, i + 1, why);
			fails++;
		}
		checks++;
		t->pages++;
		t->bytes += out.n;
		t->paths += count((const char *)out.b, out.n, "<path");
		t->texts += count((const char *)out.b, out.n, "<text");
		t->images += count((const char *)out.b, out.n, "<image");
		t->patterns += count((const char *)out.b, out.n, "<pattern");
		t->clips += count((const char *)out.b, out.n, "<clipPath");
		snprintf(fn, sizeof(fn), "/tmp/slide/svg/%s-%02d.svg", name, i + 1);
		f = fopen(fn, "wb");
		if (f) { fwrite(out.b, 1, out.n, f); fclose(f); }
	}
	ck(!zp_svg_page(&(zp_svg_t){ svg_put, NULL, NULL, NULL, &(buf_t){ svgmem, 0, sizeof(svgmem) }, 0 }, job.b, job.n, d->ncard),
		"no page past the last");
	return true;
}

// A card of flowchart shapes, connectors, a pattern and text.
static deck_t *diagram(void) {
	deck_t *d = deck_new();
	card_t *c = card_new("flow");
	obj_t *o;
	static const struct { int kind; const char *name, *text; int x, y; } sh[] = {
		{ OB_PILL, "start", "Start", 40, 40 },
		{ OB_SLANT, "in", "a<b & \"c\"", 40, 120 },
		{ OB_DIAMOND, "ok", "OK?", 220, 110 },
		{ OB_DOC, "doc", "Report", 400, 40 },
		{ OB_DRUM, "db", "Store", 400, 180 },
	};
	unsigned i;
	zpage_set(&d->page, "screen", false);
	for (i = 0; i < sizeof(sh) / sizeof(sh[0]); i++) {
		o = obj_new(sh[i].kind);
		strcpy(o->name, sh[i].name);
		o->x = sh[i].x; o->y = sh[i].y; o->w = 90; o->h = 50;
		obj_set_text(o, sh[i].text, (uint32_t)strlen(sh[i].text));
		if (i == 2) { o->fill = FILL_PATTERN; o->fill_v = 3; }
		card_add(c, -1, o);
	}
	{
		static const char *ends[4][2] = { { "start", "in" }, { "in", "ok" }, { "ok", "doc" }, { "ok", "db" } };
		for (i = 0; i < 4; i++) {
			o = obj_new(OB_LINE);
			obj_make_connector(o, ends[i][0], ends[i][1], i == 1 || i == 2);
			card_add(c, -1, o);
		}
	}
	deck_insert_card(d, -1, c);
	while (d->ncard > 1) card_free(deck_remove_card(d, 0));	// the new deck's own card
	{
		int chg[32][4];
		card_route(c, NULL, chg, 16);
	}
	return d;
}

int main(int argc, char **argv) {
	tally_t t;
	deck_t *d;
	if (argc > 1) root = argv[1];
	pool_init(mem, sizeof(mem));
	render_init(scratch, sizeof(scratch), load);

	d = diagram();
	ck(d != NULL, "diagram deck");
	if (d && deck_svg(d, "diagram", &t)) {
		ck(t.pages == 1, "one page");
		ck(t.paths >= 9, "shapes and connectors are paths");
		ck(t.texts == 5, "five shapes' words as text");
		ck(t.patterns == 1, "the pattern as an SVG pattern");
		printf("diagram: %d paths, %d texts, %u bytes\n", t.paths, t.texts, t.bytes);
	}
	{
		// the escaped text came through as text
		FILE *f = fopen("/tmp/slide/svg/diagram-01.svg", "rb");
		static char b[1 << 20];
		size_t n = f ? fread(b, 1, sizeof(b) - 1, f) : 0;
		if (f) fclose(f);
		b[n] = 0;
		ck(strstr(b, "a&lt;b &amp; &quot;c&quot;") != NULL, "text escaped");
		ck(strstr(b, "viewBox=\"0 0 512 ") != NULL, "a card is 512 wide");
		ck(strstr(b, "textLength=") != NULL, "runs keep their widths");
	}
	deck_free(d);

	d = open_deck("../../data/slide/tour.zsl");
	ck(d != NULL, "tour");
	if (d && deck_svg(d, "tour", &t)) {
		ck(t.pages == d->ncard, "every card");
		ck(t.texts > 20 && t.paths > 20, "text and drawing");
		printf("tour: %d pages, %d paths, %d texts, %d images, %d patterns, %d clips, %u bytes\n",
			t.pages, t.paths, t.texts, t.images, t.patterns, t.clips, t.bytes);
	}
	if (d) deck_free(d);

	printf("svg: %d/%d checks passed\n", checks - fails, checks);
	return fails ? 1 : 0;
}
