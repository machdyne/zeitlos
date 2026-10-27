/*
 * learn -- figures on the build machine: the sandbox, the display list,
 * controls, time, mistakes, and the library drawn out as pictures
 * (tests/out/fig-*.pgm, and in colour fig-*-colour.ppm).
 *
 *   make -C sw/apps/learn test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "../lfig.h"
#include "../ldraw.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const char *outdir = "tests/out", *fontdir = "../../data/font";

static uint32_t now_ms(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static const void *load_font(const char *file, uint32_t *len) {
	char p[512];
	const char *sub = strstr(file, ".zhf") ? "zhf/" : strstr(file, "jp12") ? "" : "zfn/";
	snprintf(p, sizeof p, "%s/%s%s", fontdir, sub, file);
	FILE *f = fopen(p, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *d = malloc((size_t)n);
	if (fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return NULL; }
	fclose(f);
	*len = (uint32_t)n;
	return d;
}

#define PW 336
#define PH 256
static uint32_t bits[((PW + 31) / 32) * PH + 2], planes[5][((PW + 31) / 32) * PH + 2];
static uint8_t scratch[96 << 10], cache[128 << 10];

static int lit(const uint32_t *b, int x, int y) { return (b[y * ((PW + 31) / 32) + (x >> 5)] >> (x & 31)) & 1; }

static void picture(const char *name) {
	ld_page_t p;
	ld_page(&p, bits, PW, PH);
	ld_clear(&p);
	ld_frame(&p, 7, 7, LG_W + 2, LG_H + 2, 1, LD_INK);
	lg_render(&p, 8, 8, LG_W, LG_H);
	char path[512];
	snprintf(path, sizeof path, "%s/fig-%s.pgm", outdir, name);
	FILE *f = fopen(path, "wb");
	if (!f) return;
	fprintf(f, "P5\n%d %d\n255\n", PW, PH);
	for (int y = 0; y < PH; y++) for (int x = 0; x < PW; x++) fputc(lit(bits, x, y) ? 230 : 0, f);
	fclose(f);
	// in colour, plane by plane as the colour window shows it (four planes)
	for (int k = 0; k < 5; k++) {
		ld_page(&p, planes[k], PW, PH);
		ld_clear(&p);
		lg_render_plane(&p, 8, 8, LG_W, LG_H, k, 4);
	}
	snprintf(path, sizeof path, "%s/fig-%s-colour.ppm", outdir, name);
	f = fopen(path, "wb");
	if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", PW, PH);
	for (int y = 0; y < PH; y++)
		for (int x = 0; x < PW; x++) {
			bool in = x >= 8 && x < 8 + LG_W && y >= 8 && y < 8 + LG_H;
			int c = 0;
			for (int k = 0; k < 5; k++) c |= lit(planes[k], x, y) << k;
			uint32_t rgb = in ? lg_colour_rgb(c) : 0x000;
			fputc((int)((rgb >> 8) & 15) * 17, f);
			fputc((int)((rgb >> 4) & 15) * 17, f);
			fputc((int)(rgb & 15) * 17, f);
		}
	fclose(f);
	printf("fig: %s\n", path);
}

static int count_kind(int kind) {
	int n = 0;
	for (int i = 0; i < LG.nops; i++) if (LG.op[i].kind == kind) n++;
	return n;
}

static bool show(const char *name, const char *src) {
	bool ok = lg_load(NULL, src) && lg_frame(0, 2000);
	CHECK(ok, "%s: %s%s", name, LG.err, LG.frame_err);
	CHECK(!LG.outside, "%s draws outside 320x240", name);
	picture(name);
	return ok;
}

int main(int argc, char **argv) {
	if (argc > 1) outdir = argv[1];
	if (argc > 2) fontdir = argv[2];
	ld_fonts_load(load_font);
	ld_init(scratch, sizeof scratch, cache, sizeof cache);
	lg_set_clock(now_ms);

	// -- starting: the library reads without a mistake
	CHECK(lg_start(), "Scheme starts");
	CHECK(!LG.err[0], "the library: %s", LG.err);

	// -- the display list
	CHECK(lg_load(NULL, "(line 0 0 10 10)\n(rect 1 2 3 4 'warm)\n(text 5 6 \"hi\" 'big 'centre)"), "loads: %s", LG.err);
	CHECK(lg_frame(0, 1000), "a frame: %s", LG.frame_err);
	CHECK(LG.nops == 3, "three shapes, not %d", LG.nops);
	CHECK(LG.op[0].kind == LG_OP_LINE && LG.op[0].role == LG_INK, "a line in ink");
	CHECK(LG.op[1].kind == LG_OP_RECT && LG.op[1].role == LG_WARM && LG.op[1].a[3] == 4, "a warm rectangle");
	CHECK(LG.op[2].kind == LG_OP_TEXT && LG.op[2].size == LG_BIG && (LG.op[2].align & 3) == LG_CENTRE &&
		!strcmp(LG.text + LG.op[2].off, "hi"), "big centred words");
	CHECK(!LG.animated, "no time asked: still");

	// a frame again draws the same, not twice as much
	CHECK(lg_frame(0, 1000) && LG.nops == 3, "the list starts again each frame");

	// -- the figure's own definitions are its own
	CHECK(lg_load(NULL, "(define (sq x) (* x x)) (rect 0 0 (sq 3) 1)"), "defines");
	CHECK(lg_frame(0, 1000) && LG.op[0].a[2] == 9, "its own procedure");
	CHECK(lg_load(NULL, "(rect 0 0 (sq 3) 1)"), "the next figure");
	lg_frame(0, 1000);
	CHECK(LG.frame_err[0] != 0, "does not see the last one's sq");

	// a course library comes before the figure, and only for it
	CHECK(lg_load("(define (tri x) (* 3 x))", "(rect 0 0 (tri 2) 1)"), "a course library");
	CHECK(lg_frame(0, 1000) && LG.op[0].a[2] == 6, "uses it");

	// -- mistakes are told, with what went wrong
	CHECK(!lg_load(NULL, "(line 0 0 10 10"), "not closed");
	CHECK(strstr(LG.err, "not closed") != NULL, "says so: %s", LG.err);
	CHECK(lg_load(NULL, "(line 0 0 'warm)"), "reads");
	CHECK(!lg_frame(0, 1000) && strstr(LG.frame_err, "4 numbers"), "line without its numbers: %s", LG.frame_err);
	CHECK(lg_load(NULL, "(rect 0 0 5 5 'purple)"), "reads");
	CHECK(!lg_frame(0, 1000) && strstr(LG.frame_err, "purple"), "an unknown colour: %s", LG.frame_err);
	CHECK(lg_load(NULL, "(frobnicate 1)"), "reads");
	CHECK(!lg_frame(0, 1000) && LG.frame_err[0], "an unknown procedure");
	CHECK(lg_load(NULL, "(rect -10 0 5 5)"), "reads");
	CHECK(lg_frame(0, 1000) && LG.outside, "outside the figure: noticed");

	// words too long for the figure: cut off, and that is a mistake
	CHECK(lg_load(NULL, "(text 160 100 \"Each bar is one whole, cut into 4 equal pieces.\" 'centre 'small)") && lg_frame(0, 1000), "long words");
	CHECK(LG.cut, "noticed as cut off");
	CHECK(lg_load(NULL, "(caption \"Each bar is one whole, cut into 4 equal pieces.\")") && lg_frame(0, 1000), "a long caption");
	CHECK(!LG.cut && LG.op[0].size == LG_TINY, "fits, in the tiny face");
	CHECK(lg_load(NULL, "(caption \"Short.\")") && lg_frame(0, 1000) && LG.op[0].size == LG_SMALL, "a short one: small");

	// -- the sandbox: no way out
	CHECK(lg_load(NULL, "(exit 3)"), "reads");
	CHECK(!lg_frame(0, 1000), "exit is not there");
	CHECK(lg_load(NULL, "(load \"/etc/passwd\")"), "reads");
	CHECK(!lg_frame(0, 1000), "nor load");
	CHECK(lg_load(NULL, "(file->str \"x\")"), "reads");
	CHECK(!lg_frame(0, 1000), "nor files");
	CHECK(lg_load(NULL, "(%fs-list \"/\")"), "reads");
	CHECK(!lg_frame(0, 1000), "nor the Zeitlos API");

	// -- too long: stopped, and told
	CHECK(lg_load(NULL, "(define (forever) (forever)) (forever)"), "reads");
	uint32_t t0 = now_ms();
	CHECK(!lg_frame(0, 100), "stopped");
	CHECK(strstr(LG.frame_err, "too long") != NULL, "too long: %s", LG.frame_err);
	CHECK(now_ms() - t0 < 1000, "soon (%u ms)", now_ms() - t0);
	// and Scheme still works after it
	CHECK(lg_load(NULL, "(dot 5 5)") && lg_frame(0, 1000) && LG.nops == 1, "Scheme after a stop");

	// too much: the list is full, and that is a mistake
	CHECK(lg_load(NULL, "(for-range 0 2000 (lambda (i) (dot 5 5)))"), "reads");
	CHECK(!lg_frame(0, 5000) && LG.full, "too many shapes: %s", LG.frame_err);

	// -- controls keep their values between frames
	CHECK(lg_load(NULL, "(define L (slider 'length 0.1 2.0 0.5 \"Length (m)\" 0.1)) (rect 0 0 (* 100 L) 10)"), "a slider");
	CHECK(lg_frame(0, 1000) && LG.nctl == 1, "one control");
	CHECK(LG.ctl[0].kind == LG_SLIDER && LG.ctl[0].value == 0.5 && !strcmp(LG.ctl[0].label, "Length (m)"), "its start and label");
	CHECK(LG.op[0].a[2] == 50, "drawn by it");
	lg_ctl_nudge(0, 1);
	CHECK(lg_frame(0, 1000) && LG.op[0].a[2] == 60, "a step right: %g", LG.op[0].a[2]);
	lg_ctl_set(0, 9);
	CHECK(LG.ctl[0].value == 2.0, "kept in its range");
	lg_ctl_set(0, 0.537);
	CHECK(LG.ctl[0].value > 0.499 && LG.ctl[0].value < 0.501, "on its steps: %g", LG.ctl[0].value);

	CHECK(lg_load(NULL, "(if (toggle 'f \"Forces\") (dot 1 1))"), "a toggle");
	CHECK(lg_frame(0, 1000) && LG.nops == 0, "off at first");
	lg_ctl_nudge(0, 1);
	CHECK(lg_frame(0, 1000) && LG.nops == 1, "on");

	CHECK(lg_load(NULL, "(text 10 10 (list-ref '(\"a\" \"b\" \"c\") (choice 'k '(\"a\" \"b\" \"c\") \"Which\")))"), "a choice");
	lg_frame(0, 1000);
	CHECK(LG.ctl[0].nopt == 3 && !strcmp(LG.text + LG.op[0].off, "a"), "three, the first");
	lg_ctl_nudge(0, -1);
	lg_frame(0, 1000);
	CHECK(!strcmp(LG.text + LG.op[0].off, "c"), "around to the last");

	CHECK(lg_load(NULL, "(let ((p (drag 'p 100 50))) (circle (car p) (cadr p) 5 'warm))"), "a point to drag");
	lg_frame(0, 1000);
	CHECK(LG.op[0].a[0] == 100 && LG.op[0].a[1] == 50, "where it starts");
	CHECK(lg_drag_at(103, 52) == 0 && lg_drag_at(200, 200) < 0, "found under the mouse");
	lg_ctl_drag_to(0, 150, 900);
	lg_frame(0, 1000);
	CHECK(LG.op[0].a[0] == 150 && LG.op[0].a[1] == 240, "dragged, inside the figure");

	CHECK(lg_load(NULL, "(slider 'a 0 1) (slider 'b 0 1) (slider 'c 0 1) (slider 'd 0 1) (slider 'e 0 1) (slider 'f 0 1) (slider 'g 0 1)"), "seven");
	CHECK(!lg_frame(0, 1000) && strstr(LG.frame_err, "at most 6"), "too many controls: %s", LG.frame_err);

	// -- time
	CHECK(lg_load(NULL, "(rect 0 0 (* 10 (seconds)) 1)"), "time");
	CHECK(lg_frame(2.5, 1000) && LG.animated && LG.op[0].a[2] == 25, "seconds, and it moves");

	// -- still shapes: drawn once, kept while nothing they depend on changes
	CHECK(lg_load(NULL, "(define n (slider 'n 1 5 2)) (still (clock-dial 160 120 100) (rect 0 0 (* 10 n) 5)) (clock-hands 160 120 100 3 (seconds))"), "still");
	CHECK(lg_frame(0, 2000), "first frame: %s", LG.frame_err);
	int nstill = LG.nstill, nops = LG.nops;
	uint32_t serial = LG.still_serial;
	CHECK(nstill > 70 && nops == nstill + 3, "the dial kept apart (%d still, %d in all)", nstill, nops);
	CHECK(lg_frame(1.5, 2000) && LG.nstill == nstill && LG.nops == nops && LG.still_serial == serial, "the next frame: not run again");
	CHECK(LG.op[nops - 1].kind == LG_OP_DISC, "the hands after it");
	lg_ctl_nudge(0, 1);
	CHECK(lg_frame(2.0, 2000) && LG.still_serial != serial && LG.op[LG.nstill - 1].kind == LG_OP_RECT && LG.op[LG.nstill - 1].a[2] > 21.9 && LG.op[LG.nstill - 1].a[2] < 22.1, "a control changed (n 2.2): run again (%d still, kind %d, %g)", LG.nstill, LG.op[LG.nstill - 1].kind, LG.op[LG.nstill - 1].a[2]);
	CHECK(lg_load(NULL, "(dot 1 1) (still (dot 2 2))") && lg_frame(0, 1000) && lg_frame(0, 1000), "still, not first");
	CHECK(LG.nstill == 0 && LG.nops == 2, "drawn every frame (%d %d)", LG.nstill, LG.nops);
	show("clock-moving", "(still (clock-dial 160 120 100)) (clock-hands 160 120 100 3 (* 6 (seconds)))");

	// -- numbers
	CHECK(lg_load(NULL, "(text 0 10 (fmt 3.14159 2)) (text 0 20 (fmt -0.001 1)) (text 0 30 2.5) (text 0 40 (sqrt 16))"), "fmt");
	lg_frame(0, 1000);
	CHECK(!strcmp(LG.text + LG.op[0].off, "3.14"), "fmt 2: %s", LG.text + LG.op[0].off);
	CHECK(!strcmp(LG.text + LG.op[1].off, "0.0"), "no minus zero: %s", LG.text + LG.op[1].off);
	CHECK(!strcmp(LG.text + LG.op[2].off, "2.5"), "a number as words: %s", LG.text + LG.op[2].off);
	CHECK(!strcmp(LG.text + LG.op[3].off, "4"), "a whole one: %s", LG.text + LG.op[3].off);
	CHECK(lg_load(NULL, "(rect 0 0 (* 100 (sin (/ pi 2))) (rad->deg (atan 1 1)))") && lg_frame(0, 1000), "sin, atan");
	CHECK(LG.op[0].a[2] > 99.99 && LG.op[0].a[3] > 44.99 && LG.op[0].a[3] < 45.01, "right values");

	// -- the library, drawn
	show("frac-bars", "(text 80 14 \"one whole\" 'tiny)\n(frac-bar 0 1 0 'warm \"1\")\n"
		"(frac-bar 1 4 3 'warm \"3/4\")\n(frac-bar 2 4 2 'cool \"2/4\")\n"
		"(caption \"Same size pieces: count them.\")");
	CHECK(count_kind(LG_OP_MATH) == 3, "three fractions typeset");
	show("frac-circles", "(frac-circle 80 110 60 3 2 'warm)\n(frac-circle 240 110 60 5 2 'cool)\n"
		"(math 80 210 \"2/3\" 22)\n(math 240 210 \"2/5\" 22)");
	show("number-line", "(number-line 20 300 130 0 10 1 2)\n(nl-jump 2 5 'warm \"+3\")\n(nl-jump 5 9 'cool \"+4\")\n"
		"(nl-mark 2 'ink)\n(nl-mark 9 'good \"9\")\n(caption \"2 + 3 + 4 = 9\")");
	show("graph", "(graph 40 16 264 192 -4 4 -2 10)\n(graph-axes 1 2 \"x\" \"y\")\n"
		"(plot (lambda (x) (* x x)) 'warm)\n(plot (lambda (x) (+ (* 2 x) 1)) 'cool)\n"
		"(plot-point 1 3 'ink \"(1, 3)\")");
	show("pendulum", "(define L (slider 'length 0.1 2.0 0.5 \"Length\" 0.05))\n"
		"(define T (* 2 pi (sqrt (/ L 9.81))))\n"
		"(define a (* 12 (cos (* 2 pi (/ (seconds) T)))))\n"
		"(define len (* 70 L))\n"
		"(define bx (+ 90 (* len (sin (deg->rad a)))))\n(define by (+ 20 (* len (cos (deg->rad a)))))\n"
		"(rect 60 12 60 8 'grey) (line 60 20 120 20 'ink 2)\n"
		"(line 90 20 bx by 'ink 1.2) (circle bx by 9 'warm) (ring bx by 9 'ink 1)\n"
		"(graph 190 30 110 120 0 2 0 3) (graph-axes 0.5 1)\n"
		"(plot (lambda (l) (* 2 pi (sqrt (/ l 9.81)))) 'cool)\n(plot-point L T 'warm)\n"
		"(text 16 200 (string-append \"L = \" (fmt L 2) \" m\") 'body 'bold)\n"
		"(text 170 200 (string-append \"T = \" (fmt T 2) \" s\") 'body 'bold)");
	CHECK(LG.animated && LG.nctl == 1, "the pendulum moves and has its slider");
	show("clock", "(clock-face 90 120 80 3 40)\n(text 200 110 \"3:40\" 32)\n(text 200 140 \"twenty to four\" 'small)");
	show("counting", "(dot-array 20 30 3 4 18 'warm)\n(text 20 110 \"3 rows of 4\" 'small)\n"
		"(base-ten 120 20 1 3 4)\n(text 120 80 \"134\" 'body)\n(tally 20 140 7)\n(ruler 120 140 6 30)\n"
		"(angle-mark 60 220 40 0 60 \"60\")\n(line 60 220 110 220 'ink 1.5) (line 60 220 85 177 'ink 1.5)");
	show("bars", "(bar-chart '((\"Mon\" 3) (\"Tue\" 5) (\"Wed\" 2) (\"Thu\" 6) (\"Fri\" 4)) 'cool 6)\n"
		"(text 160 20 \"Books read\" 'body 'centre)");
	show("roles", "(define roles '(ink paper warm cool good bad accent sky earth plant grey))\n"
		"(let loop ((l roles) (i 0)) (if (pair? l) (begin\n"
		"  (rect (+ 8 (* (modulo i 4) 78)) (+ 8 (* (quotient i 4) 76)) 70 50 (car l))\n"
		"  (frame (+ 8 (* (modulo i 4) 78)) (+ 8 (* (quotient i 4) 76)) 70 50 'ink 1)\n"
		"  (text (+ 43 (* (modulo i 4) 78)) (+ 72 (* (quotient i 4) 76)) (symbol->string (car l)) 'centre 'small)\n"
		"  (loop (cdr l) (+ i 1)))))");

	// a frame of the library's busiest figure is quick
	lg_load(NULL, "(clock-face 90 120 80 3 40) (graph 190 30 110 120 0 2 0 3) (graph-axes 0.5 1) (plot (lambda (l) (* l l)) 'cool)");
	t0 = now_ms();
	for (int i = 0; i < 20; i++) lg_frame(i * 0.1, 2000);
	uint32_t per = (now_ms() - t0) / 20;
	printf("fig: a busy frame on this machine: %u ms\n", per);

	// a figure that runs Scheme out of heap or stack must not break the
	// figures after it (it did: every later one failed the same way)
	CHECK(!lg_load(NULL, "(define (deep n) (if (= n 0) 0 (+ 1 (deep (- n 1))))) (deep 100000)") || !lg_frame(0, 3000),
		"a runaway recursion fails");
	CHECK(lg_load(NULL, "(define l (let loop ((i 0) (acc '())) (if (< i 40000) (loop (+ i 1) (cons i acc)) acc))) (line 0 0 1 1)") ||
		!lg_frame(0, 3000) || 1, "(a figure that fills the heap)");
	CHECK(lg_load(NULL, "(line 0 0 10 10)") && lg_frame(0, 1000) && LG.nops == 1,
		"the next figure draws: %s%s", LG.err, LG.frame_err);
	for (int i = 0; i < 100; i++) lg_load(NULL, "(define a-new-name-every-time 1) (line 0 0 10 10)");
	CHECK(lg_load(NULL, "(rect 1 2 3 4)") && lg_frame(0, 1000), "after a hundred figures: %s%s", LG.err, LG.frame_err);

	printf("test_fig: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
