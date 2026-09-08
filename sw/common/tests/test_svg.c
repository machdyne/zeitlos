/*
 * Zeitlos -- host test for sw/common/zsvg.c.
 *
 *   cc -std=gnu99 -I sw/common -o /tmp/t sw/common/tests/test_svg.c \
 *      sw/common/zsvg.c -lm && /tmp/t
 *
 * The renderer emits spans and lines rather than pixels, so a test can
 * collect them into a small bitmap and assert on what was actually
 * covered -- which is the only thing that matters and the only thing
 * that catches a rasterizer being subtly wrong.
 *
 * What is checked here is geometry, not appearance: that a shape lands
 * where the coordinates say, that a transform moves it, that the fill
 * rules differ where they should, and that malformed input is refused
 * rather than half-drawn.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <math.h>

#include "zsvg.h"

// zsvg.c's own maths, checked against libm below. Declared here
// rather than in zsvg.h: they are an implementation detail of that
// file and nothing else should be calling them.
float zs_fabs(float), zs_floor(float), zs_ceil(float), zs_sqrt(float);
float zs_sin(float), zs_cos(float), zs_tan(float);
float zs_atan2(float, float);

static int fails, checks;
static void ck(int c, const char *w) {
	checks++; if (!c) { fails++; printf("FAIL: %s\n", w); } }

#define W 64
#define H 64
static uint8_t fb[H][W];

static void sp(void *u, int y, int x0, int x1, int level) {
	(void)u; (void)level;
	for (int x = x0; x <= x1; x++)
		if (y >= 0 && y < H && x >= 0 && x < W) fb[y][x] = 1;
}

static void ln(void *u, int x0, int y0, int x1, int y1) {
	// Bresenham, so a stroked path marks the pixels it passes over.
	(void)u;
	int dx = abs(x1 - x0), dy = abs(y1 - y0);
	int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx - dy;
	for (;;) {
		if (y0 >= 0 && y0 < H && x0 >= 0 && x0 < W) fb[y0][x0] = 1;
		if (x0 == x1 && y0 == y1) break;
		int e2 = 2 * e;
		if (e2 > -dy) { e -= dy; x0 += sx; }
		if (e2 <  dx) { e += dx; y0 += sy; }
	}
}

static z_svg_edge_t edges[2048];

static int render(const char *doc, int max_edges) {
	z_svg_t s;
	memset(fb, 0, sizeof(fb));
	memset(&s, 0, sizeof(s));
	s.doc = doc; s.len = (uint32_t)strlen(doc);
	s.x = 0; s.y = 0; s.w = W; s.h = H;
	s.edges = edges;
	s.max_edges = max_edges ? max_edges : (int)(sizeof(edges)/sizeof(edges[0]));
	s.sink.span = sp; s.sink.line = ln;
	return z_svg_render(&s);
}

static int ink(void) {
	int n = 0;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) n += fb[y][x];
	return n;
}

int main(void) {

	// -- the float maths --
	//
	// zsvg.c implements sqrt, sin, cos, tan and atan2 itself rather
	// than linking libm, because it is shared code and a library that
	// only links when every caller remembers -lm breaks the next time
	// someone uses it.
	//
	// This is also how that omission HID: the host compiler folds
	// these into builtins, so the file linked cleanly here and failed
	// only on the target. Checking them against libm keeps the
	// substitutes honest.
	{
		double ws = 0, wc = 0, wq = 0, wa = 0;
		int i, j;

		for (i = -3600; i <= 3600; i++) {
			float a = (float)i * 0.1f * 3.14159265f / 180.0f;
			double d;
			d = fabs((double)zs_sin(a) - sin(a)); if (d > ws) ws = d;
			d = fabs((double)zs_cos(a) - cos(a)); if (d > wc) wc = d;
		}

		for (i = 1; i < 20000; i++) {
			float x = (float)i * 0.1f;
			double d = fabs((double)zs_sqrt(x) - sqrt(x)) / sqrt(x);
			if (d > wq) wq = d;
		}

		for (i = -60; i <= 60; i++) for (j = -60; j <= 60; j++) {
			double d;
			if (!i && !j) continue;
			d = fabs((double)zs_atan2((float)i, (float)j) -
				atan2((double)i, (double)j));
			if (d > 3.0) d = fabs(d - 2.0 * 3.14159265358979);
			if (d > wa) wa = d;
		}

		// A tenth of a pixel at a 300-pixel radius is 3e-4 radians,
		// so 1e-4 is the bar and these clear it by orders.
		ck(ws < 1e-4, "sin matches libm");
		ck(wc < 1e-4, "cos matches libm");
		ck(wq < 1e-5, "sqrt matches libm");
		ck(wa < 1e-3, "atan2 matches libm");

		ck(zs_floor(-2.5f) == -3.0f && zs_ceil(-2.5f) == -2.0f,
			"floor and ceil round the right way for negatives");
		ck(zs_floor(2.5f) == 2.0f && zs_ceil(2.5f) == 3.0f,
			"and for positives");
	}

	// -- sniffing --
	ck(z_svg_sniff("<svg viewBox='0 0 1 1'/>", 24), "a bare tag is SVG");
	ck(z_svg_sniff("<?xml version='1.0'?>\n<!-- c -->\n<svg>", 37),
		"a declaration and a comment first is still SVG");
	ck(!z_svg_sniff("<html><body>", 12), "HTML is not SVG");
	ck(!z_svg_sniff("\x89PNG\r\n", 6), "a PNG is not SVG");

	// -- a rectangle lands where its coordinates say --
	//
	// viewBox 0 0 64 64 into a 64x64 box is 1:1, so the numbers in
	// the document are pixel coordinates and the test can be exact.
	{
		ck(render("<svg viewBox='0 0 64 64'>"
			"<rect x='16' y='16' width='32' height='32' fill='black'/>"
			"</svg>", 0) == Z_SVG_OK, "a rect renders");

		ck(fb[32][32], "its centre is filled");
		ck(fb[16][16], "its top-left corner is filled");
		ck(fb[47][47], "its bottom-right corner is filled");
		ck(!fb[15][32], "one row above is not");
		ck(!fb[48][32], "one row below is not");
		ck(!fb[32][15], "one column left is not");
		ck(!fb[32][48], "one column right is not");
	}

	// -- fill='none' paints nothing --
	{
		render("<svg viewBox='0 0 64 64'>"
			"<rect x='8' y='8' width='48' height='48' fill='none'/>"
			"</svg>", 0);
		ck(ink() == 0, "fill='none' paints nothing");
	}

	// -- a transform moves it --
	{
		render("<svg viewBox='0 0 64 64'>"
			"<g transform='translate(16,16)'>"
			"<rect x='0' y='0' width='16' height='16' fill='black'/>"
			"</g></svg>", 0);
		ck(fb[20][20], "a translated rect is at its new position");
		ck(!fb[4][4], "and not at its old one");
	}

	// -- nested transforms compose --
	{
		render("<svg viewBox='0 0 64 64'>"
			"<g transform='translate(16,0)'><g transform='translate(0,16)'>"
			"<rect x='0' y='0' width='8' height='8' fill='black'/>"
			"</g></g></svg>", 0);
		ck(fb[20][20], "nested translates compose");
	}

	// -- a group's transform ends with the group --
	{
		render("<svg viewBox='0 0 64 64'>"
			"<g transform='translate(32,32)'>"
			"<rect x='0' y='0' width='8' height='8' fill='black'/>"
			"</g>"
			"<rect x='0' y='0' width='8' height='8' fill='black'/>"
			"</svg>", 0);
		ck(fb[34][34], "the shape inside the group is moved");
		ck(fb[4][4], "the shape after it is NOT");
	}

	// -- fill rules differ --
	//
	// Two concentric squares wound the same way. Nonzero fills the
	// middle; even-odd leaves a hole. If these agree, the winding
	// direction is being ignored.
	{
		const char *tmpl =
			"<svg viewBox='0 0 64 64'><path fill='black' %s"
			" d='M8 8 H56 V56 H8 Z M20 20 H44 V44 H20 Z'/></svg>";
		char doc[512];
		int solid, holed;

		snprintf(doc, sizeof(doc), tmpl, "");
		render(doc, 0);
		solid = fb[32][32];

		snprintf(doc, sizeof(doc), tmpl, "fill-rule='evenodd'");
		render(doc, 0);
		holed = fb[32][32];

		ck(solid, "nonzero fills the middle of two same-wound squares");
		ck(!holed, "even-odd leaves it hollow");
		ck(fb[12][32], "and the ring itself is still filled");
	}

	// -- curves are flattened, not dropped --
	{
		render("<svg viewBox='0 0 64 64'>"
			"<path d='M8 32 C8 8 56 8 56 32 Z' fill='black'/></svg>", 0);
		ck(ink() > 200, "a cubic contributes area");
		ck(fb[24][32], "under the arc is filled");
		ck(!fb[2][32], "well above it is not");
	}

	// -- circles are arcs, and arcs are the fiddly part --
	{
		render("<svg viewBox='0 0 64 64'>"
			"<circle cx='32' cy='32' r='16' fill='black'/></svg>", 0);
		ck(fb[32][32], "a circle's centre is filled");
		ck(fb[32][20] && fb[32][44], "and its horizontal extremes");
		ck(fb[20][32] && fb[44][32], "and its vertical extremes");
		ck(!fb[32][12] && !fb[12][32], "but not outside its radius");
		// Round, not square: the corners of its bounding box are out.
		ck(!fb[20][20], "the corners of its bounding box are empty");
	}

	// -- polygon --
	{
		render("<svg viewBox='0 0 64 64'>"
			"<polygon points='32,8 56,56 8,56' fill='black'/></svg>", 0);
		ck(fb[48][32], "a polygon fills");
		ck(!fb[12][8], "outside its edges it does not");
	}

	// -- refusals --
	ck(render("<html><body>hello</body></html>", 0) == Z_SVG_E_FORMAT,
		"HTML is refused");
	ck(render("<svg viewBox='0 0 64 64'>"
		"<circle cx='32' cy='32' r='16' fill='black'/></svg>", 16)
		== Z_SVG_E_TOOCOMPLEX,
		"a drawing past the caller's edge budget is refused, not truncated");

	// A shape this renderer does not know is skipped, not fatal: a
	// <text> label missing is better than the diagram it labels being
	// thrown away.
	{
		int rv = render("<svg viewBox='0 0 64 64'>"
			"<text x='0' y='0'>hi</text>"
			"<rect x='16' y='16' width='16' height='16' fill='black'/>"
			"</svg>", 0);
		ck(rv == Z_SVG_OK, "an unknown element does not fail the document");
		ck(fb[20][20], "and the shapes around it still draw");
	}

	printf("%s: %d checks, %d failures\n", fails ? "FAIL" : "ok",
		checks, fails);
	return fails ? 1 : 0;

}
