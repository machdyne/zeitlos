/*
 * learn -- the app on the build machine, driven by a script of keys and
 * clicks, each screen written out as a picture (tests/out, PGM and PNG
 * beside them). The app is the shipped code; only the platform
 * (tests/hostplat.c) and the window are stand-ins.
 *
 *   make -C sw/apps/learn render
 *
 * It also checks what it can without a person looking: that every
 * screen draws, that answers are judged and recorded, that progress
 * reaches the learner's files.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../lapp_int.h"
#include "../ldraw.h"
#include "hostplat.h"
#include "zkbd.h"
#include "zco.h"

#define W 638
#define H 467

static uint32_t bits[((W + 31) / 32) * H + 2];
static ld_page_t page;
static uint8_t mem[3u << 20];
static uint8_t scratch[96 << 10], cache[128 << 10];
static const char *outdir = "tests/out";
static const char *fontdir = "../../data/font";
static int shots, fails;

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

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
	if (fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); return NULL; }
	fclose(f);
	*len = (uint32_t)n;
	return d;
}

static void draw(void) { la_draw(&page); }

// words the figure drew (each of its texts)
static bool fig_says(const char *needle) {
	for (int i = 0; i < LG.nops; i++)
		if ((LG.op[i].kind == LG_OP_TEXT || LG.op[i].kind == LG_OP_MATH) && strstr(LG.text + LG.op[i].off, needle)) return true;
	return false;
}

static void shot(const char *name) {
	draw();
	char p[512];
	snprintf(p, sizeof p, "%s/%s.pgm", outdir, name);
	FILE *f = fopen(p, "wb");
	fprintf(f, "P5\n%d %d\n255\n", W, H);
	int wpl = (W + 31) / 32;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			fputc((bits[y * wpl + (x >> 5)] >> (x & 31)) & 1 ? 230 : 0, f);
	fclose(f);
	shots++;
	printf("render: %s\n", p);
}

static void key(uint32_t k) {
	la_ev_t e;
	memset(&e, 0, sizeof e);
	e.type = LA_KEY;
	e.key = k;
	la_event(&e);
	draw();		// as the window would: the targets are where the last drawing put them
}

static void type(const char *s) { for (; *s; s++) key((uint8_t)*s); }

// A key that changes only part of the page: drawn as the board draws it
// (that part alone over the last page), it must be the page a whole
// drawing makes. Returns whether it was partial.
static uint32_t part_bits[((W + 31) / 32) * H + 2];
static bool ev_part(int type, uint32_t k, int notches) {
	la_draw(&page);
	la_ev_t e;
	memset(&e, 0, sizeof e);
	e.type = (uint8_t)type;
	e.key = k;
	e.notches = notches;
	la_event(&e);
	int x, y, w, h;
	if (!la_partial(&x, &y, &w, &h)) { la_draw(&page); return false; }
	la_draw_part(&page, x, y, w, h);
	memcpy(part_bits, bits, sizeof bits);
	la_draw(&page);
	if (memcmp(part_bits, bits, sizeof bits)) {
		int wpl = (W + 31) / 32, n = 0;
		int ymin = 9999, ymax = -1, xmin = 9999, xmax = -1;
		for (int yy = 0; yy < H; yy++) for (int xx = 0; xx < W; xx++) {
			int a = (part_bits[yy * wpl + (xx >> 5)] >> (xx & 31)) & 1, b = (bits[yy * wpl + (xx >> 5)] >> (xx & 31)) & 1;
			if (a != b) { n++; if (yy < ymin) ymin = yy; if (yy > ymax) ymax = yy; if (xx < xmin) xmin = xx; if (xx > xmax) xmax = xx; }
		}
		printf("  %d pixels differ, rows %d-%d, columns %d-%d\n", n, ymin, ymax, xmin, xmax);
	}
	CHECK(!memcmp(part_bits, bits, sizeof bits), "the part drawn alone is what the whole page draws (key %u, wheel %d, %dx%d at %d,%d)", k, notches, w, h, x, y);
	return true;
}
static bool key_part(uint32_t k) { return ev_part(LA_KEY, k, 0); }

static void click_action(int action) {
	draw();
	for (int i = 0; i < A.nhot; i++)
		if (A.hot[i].action == action) {
			la_ev_t e;
			memset(&e, 0, sizeof e);
			e.type = LA_DOWN;
			e.x = A.hot[i].x + 2;
			e.y = A.hot[i].y + 2;
			la_event(&e);
			draw();
			return;
		}
	printf("FAIL: no target with action %d on screen %d\n", action, A.screen);
	fails++;
}

static void settle(void) { while (la_work(50)) hp_ms_now += 50; draw(); }

static void setup_card(void) {
	snprintf(hp_root, sizeof hp_root, "/tmp/learn_render_card");
	hp_reset_card();
	char cmd[2400];
	// two packs, as a card has them: edu-zeitlos from this tree (its
	// first course: the screens below count on what the catalogue
	// holds), and a test course standing in for the Ark's edu
	snprintf(cmd, sizeof cmd, "mkdir -p %s/opt/ark/edu %s/opt/ark/edu-zeitlos %s/opt/ark/zdocs && "
		"cp -r ../../data/learn/edu-zeitlos/zeit-learn %s/opt/ark/edu-zeitlos/ && cp -r tests/courses/t-good tests/courses/lib %s/opt/ark/edu/",
		hp_root, hp_root, hp_root, hp_root, hp_root);
	if (system(cmd)) { printf("render: cannot make the card\n"); exit(1); }
}

static long file_has(const char *path, const char *needle) {
	char p[700], buf[8192];
	snprintf(p, sizeof p, "%s%s", hp_root, path);
	FILE *f = fopen(p, "rb");
	if (!f) return 0;
	size_t n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = 0;
	return strstr(buf, needle) != NULL;
}

// -- the slow work as the board runs it: in a coroutine that hands back
// after every request to the card (learn.c, "slow work") --
static zco_t wco;
static bool won, win_co;
static int yields;
static uint8_t *wstack;
static lplat_t yplat;
static bool y_list(void *ctx, const char *dir, void (*fn)(void *, const char *, bool), void *arg) {
	bool r = host_plat.fs.list(ctx, dir, fn, arg);
	if (win_co) { yields++; zco_yield(); }
	return r;
}
static long y_read(void *ctx, const char *path, char *buf, long cap) {
	long r = host_plat.fs.read(ctx, path, buf, cap);
	if (win_co) { yields++; zco_yield(); }
	return r;
}
static long y_read_at(const char *path, long off, char *buf, long cap) {
	long r = host_plat.read_at(path, off, buf, cap);
	if (win_co) { yields++; zco_yield(); }
	return r;
}
static void wfn(void *arg) { (void)arg; la_work(15); }
// one turn of the main loop's work; false when there is none
static bool wstep(void) {
	if (!la_busy() && !won) return false;
	if (!won) { zco_init(&wco, wfn, NULL, wstack, 64 << 10); won = true; }
	win_co = true;
	bool more = zco_resume(&wco);
	win_co = false;
	if (!more) won = false;
	hp_ms_now += 5;
	return true;
}

int main(int argc, char **argv) {
	if (argc > 1) outdir = argv[1];
	if (argc > 2) fontdir = argv[2];
	LP = &host_plat;
	setup_card();
	ld_fonts_load(load_font);
	ld_init(scratch, sizeof scratch, cache, sizeof cache);
	ld_page(&page, bits, W, H);

	la_init(mem, sizeof mem);
	la_resize(W, H);
	shot("00-loading");
	settle();
	CHECK(A.screen == SC_LEARNERS, "first: who is learning (screen %d)", A.screen);
	CHECK(LK.n == 2, "courses found: %d", LK.n);
	shot("01-learners-none");

	// a learner
	snprintf(hp_prompt_answer, sizeof hp_prompt_answer, "Mia");
	key('n');
	CHECK(A.screen == SC_HOME, "a new learner goes home (screen %d)", A.screen);
	CHECK(file_has("/data/learn/learners/Mia/learner.cfg", "name = Mia"), "learner.cfg written");
	CHECK(file_has("/data/learn/learn.cfg", "learner = /data/learn/learners/Mia"), "the learner is remembered");
	shot("02-home-new");

	// the catalogue, and a course
	key('c');
	CHECK(A.screen == SC_CATALOGUE, "catalogue");
	shot("03-catalogue");
	key(Z_KEY_DOWN);
	key(Z_KEY_DOWN);
	key(Z_KEY_DOWN);
	key(Z_KEY_DOWN);
	key(Z_KEY_DOWN);		// Using Zeitlos
	shot("04-catalogue-zeitlos");
	{	// a key in a menu copies only what changed: the footer is not touched
		int wpl = (W + 31) / 32, at = (A.h - 10) * wpl;
		uint32_t was = page.bits[at];
		page.bits[at] ^= 0x55555555u;		// a mark a redraw of the footer would wipe
		key(Z_KEY_UP);
		CHECK(page.bits[at] == (was ^ 0x55555555u), "the footer was not drawn again for a key in the catalogue");
		page.bits[at] = was;
		key(Z_KEY_DOWN);
	}
	key(0x0d);			// to the courses column
	key(0x0d);			// start
	settle();
	CHECK(A.screen == SC_COURSE && A.course_open && !strcmp(A.course->m.id, "zeit-learn"), "course open (%d)", A.screen);
	shot("05-course");

	// the first lesson
	key(Z_KEY_RIGHT);
	key(0x0d);
	CHECK(A.screen == SC_LESSON && A.step == 0, "lesson open");
	shot("06-lesson-step1");
	key(' ');
	CHECK(A.step == 1, "step 2");
	shot("07-lesson-step2");
	key(' ');
	CHECK(A.step == 2 && A.nchecks == 1, "the check");
	shot("08-check");
	key(' ');
	CHECK(A.step == 2, "cannot go on without answering");
	// the right answer is "Space (or Enter)": find its number
	int right = -1;
	for (int i = 0; i < A.checks[0].q.nopt; i++) if (A.checks[0].q.opt[A.checks[0].q.order[i]].right) right = i;
	int wrong = right == 0 ? 1 : 0;
	key('1' + (uint32_t)wrong);
	CHECK(A.checks[0].judged && !A.checks[0].answered, "a wrong answer, then another try");
	shot("09-check-wrong");
	key('h');
	key('1' + (uint32_t)right);
	CHECK(A.checks[0].answered && !A.checks[0].right_first, "right the second time: answered, not counted");
	shot("10-check-right");
	key(' ');
	CHECK(A.screen == SC_DONE, "lesson done");
	CHECK(file_has("/data/learn/learners/Mia/progress.txt", "done zeit-learn/start/a-lesson 0/1"), "progress written");
	shot("11-done");

	// lesson 2: a choice right first time, a fraction typed, a number typed with hints
	key(0x0d);
	CHECK(A.screen == SC_LESSON && A.lesson == 1, "next lesson");
	int r2 = -1;
	for (int i = 0; i < A.checks[0].q.nopt; i++) if (A.checks[0].q.opt[A.checks[0].q.order[i]].right) r2 = i;
	key('1' + (uint32_t)r2);
	shot("12-choice-right");
	key(' ');
	CHECK(A.step == 1 && A.typing == 0, "a typed question has the keyboard");
	CHECK(key_part('2') && key_part('/') && key_part('1'), "typing draws only the field");
	CHECK(key_part(0x0d), "Check draws from the question down, not the whole page");
	shot("13-fraction-wrong-if");
	CHECK(A.checks[0].judged && !A.checks[0].answered && A.checks[0].last.feedback &&
		strstr(A.checks[0].last.feedback, "two over one"), "wrong-if feedback");
	for (int i = 0; i < 3; i++) key(0x7f);
	type("1/2");
	key(0x0d);
	CHECK(A.checks[0].answered, "fraction right");
	shot("14-fraction-right");
	key(' ');
	CHECK(A.step == 2, "step 3");
	CHECK(A.typing == 0, "the number field has the keyboard");
	key('h');
	CHECK(A.checks[0].hints_shown == 0 && !strcmp(A.checks[0].typed, "h"), "h in the box is a letter, not a hint");
	key(0x7f);
	key(Z_KEY_F5);
	CHECK(A.checks[0].hints_shown == 1 && !A.checks[0].typed[0], "F5 gives a hint while the box has the keyboard");
	click_action(A_HINT);
	shot("15-hints");
	type("3");
	// F8 while typing: the question passed over, straight on (here: the lesson's end)
	key(Z_KEY_F8);
	CHECK(A.screen == SC_DONE && A.right == 1 && A.total == 3, "F8 skips: lesson 2: %d of %d", A.right, A.total);
	shot("16-done-2");

	// lesson 3: a table, an example shown step by step, a report
	key(0x0d);
	shot("17-table");
	key(' ');
	shot("18-example");
	key(' ');
	key(' ');
	key(' ');
	shot("19-example-shown");
	snprintf(hp_prompt_answer, sizeof hp_prompt_answer, "a test report");
	key(Z_KEY_F7);
	CHECK(file_has("/data/learn/learners/Mia/reports.txt", "zeit-learn v1 zeit-learn/start/progress step 2: a test report"), "report kept");
	// leave in the middle: the place is kept
	key(0x1b);
	CHECK(A.screen == SC_COURSE, "back to the course");
	CHECK(file_has("/data/learn/learners/Mia/progress.txt", "at zeit-learn/start/progress 1"), "where they stopped");
	shot("20-course-progress");

	// home again (through the catalogue it came from), then a fresh
	// start: the learner and their place come back
	key(0x1b);
	CHECK(A.screen == SC_CATALOGUE, "back to the catalogue");
	key(0x1b);
	CHECK(A.screen == SC_HOME, "home");
	shot("21-home-progress");
	la_init(mem, sizeof mem);
	la_resize(W, H);
	settle();
	CHECK(A.screen == SC_HOME && ls_current() && !strcmp(ls_current()->name, "Mia"), "Mia again, at home");
	key(0x0d);
	settle();
	CHECK(A.screen == SC_LESSON && A.lesson == 2 && A.step == 1, "continue: back at lesson 3 step 2 (%d %d %d)", A.screen, A.lesson, A.step);
	shot("22-continued");

	// a smaller window: the step reflows; the figure pane goes above
	la_resize(420, 400);
	draw();
	CHECK(true, "small window draws");
	{
		// write it out at its size
		char p[512];
		snprintf(p, sizeof p, "%s/23-small-window.pgm", outdir);
		FILE *f = fopen(p, "wb");
		fprintf(f, "P5\n%d %d\n255\n", 420, 400);
		int wpl = (W + 31) / 32;
		for (int y = 0; y < 400; y++) for (int x = 0; x < 420; x++) fputc((bits[y * wpl + (x >> 5)] >> (x & 31)) & 1 ? 230 : 0, f);
		fclose(f);
		shots++;
	}
	la_resize(W, H);

	// the test course with a figure: the pane
	key(0x1b);
	key(0x1b);
	key('c');
	key(Z_KEY_DOWN);		// School
	key(0x0d);
	key(0x0d);
	settle();
	key(Z_KEY_RIGHT);
	key(0x0d);
	CHECK(A.screen == SC_LESSON, "the test course's lesson");
	shot("24-figure-pane");
	CHECK(A.fig && LG.loaded && LG.nops > 0 && !LG.err[0] && !LG.frame_err[0], "the figure draws, with the pack's lib/ (%s%s)", LG.err, LG.frame_err);

	// figures in "Using learn": still, a slider, moving, points to drag
	CHECK(la_open_course("/opt/ark/edu-zeitlos/zeit-learn"), "zeit-learn again");
	la_open_lesson(3);
	CHECK(A.screen == SC_LESSON && A.fig && !LG.err[0], "lesson 4 has a figure (%s)", LG.err);
	shot("25-figure");
	// a link to another lesson opens it
	click_action(A_LINK);
	CHECK(A.screen == SC_LESSON && A.lesson == 1 && !A.fig, "the link: Answering questions (%d)", A.lesson);
	la_open_lesson(3);
	int r4 = -1;
	for (int i = 0; i < A.checks[0].q.nopt; i++) if (A.checks[0].q.opt[A.checks[0].q.order[i]].right) r4 = i;
	key('1' + (uint32_t)r4);
	key(' ');
	CHECK(A.step == 1 && LG.nctl == 1 && LG.ctl[0].value == 3, "the slider step: 3 shaded");
	CHECK(key_part(Z_KEY_RIGHT), "a slider's step draws only the figure");
	CHECK(LG.ctl[0].value == 4 && !A.checks[0].typed[0], "Right, the answer empty: the slider (%g)", LG.ctl[0].value);
	key(Z_KEY_LEFT);
	CHECK(LG.ctl[0].value == 3, "Left: back");
	// the number question has the keyboard: the mouse moves the slider
	{
		la_ev_t e;
		memset(&e, 0, sizeof e);
		e.type = LA_DOWN;
		e.x = A.fig_track_x + A.fig_track_w / 2;		// the middle: 4
		e.y = A.fig_ctl_y[0] + 10;
		la_event(&e);
		draw();
		CHECK(LG.ctl[0].value == 4, "a click on the track: %g", LG.ctl[0].value);
		e.type = LA_MOVE;
		e.x = A.fig_track_x + A.fig_track_w;
		la_event(&e);
		e.type = LA_UP;
		la_event(&e);
		draw();
		CHECK(LG.ctl[0].value == 8, "dragged to the end: %g", LG.ctl[0].value);
		e.type = LA_DOWN;
		e.x = A.fig_track_x + A.fig_track_w / 2;
		la_event(&e);
		e.type = LA_UP;
		la_event(&e);
		draw();
	}
	CHECK(LG.nops > 0 && fig_says("4/8"), "the figure shows 4/8");
	shot("26-slider");
	// a slider let go where it is changes nothing: nothing is drawn again
	{
		la_ev_t e;
		memset(&e, 0, sizeof e);
		e.type = LA_DOWN;
		e.x = A.fig_track_x + A.fig_track_w / 2;
		e.y = A.fig_ctl_y[0] + 12;
		la_event(&e);
		draw();
		e.type = LA_MOVE;
		CHECK(!la_event(&e), "dragged nowhere: not drawn again");
		e.type = LA_UP;
		CHECK(!la_event(&e), "let go where it is: not drawn again");
		CHECK(key_part(Z_KEY_RIGHT) && key_part(Z_KEY_LEFT), "a slider's step: the figure and its controls alone");
		for (int i = 0; i < 12; i++) key(Z_KEY_RIGHT);
		la_ev_t k;
		memset(&k, 0, sizeof k);
		k.type = LA_KEY;
		k.key = Z_KEY_RIGHT;
		CHECK(!la_event(&k), "at the end of the slider: Right draws nothing");
		for (int i = 0; i < 12; i++) key(Z_KEY_LEFT);
		for (int i = 0; i < 4; i++) key(Z_KEY_RIGHT);
	}
	// the same in colour, as the colour window shows it: the page's bit
	// and four planes over the figure, black and white elsewhere
	{
		int fx, fy, fw, fh;
		CHECK(la_figure_rect(&fx, &fy, &fw, &fh) && fw == 320 && fh == 240, "the figure can take the colour window");
		la_figure_colour(4);
		draw();
		static uint32_t pl[4][10 * 240 + 2];
		for (int k = 0; k < 4; k++) {
			ld_page_t pg;
			ld_page(&pg, pl[k], 320, 240);
			ld_clear(&pg);
			la_figure_plane(&pg, k + 1);
		}
		char path[512];
		snprintf(path, sizeof path, "%s/26-slider-colour.ppm", outdir);
		FILE *f = fopen(path, "wb");
		fprintf(f, "P6\n%d %d\n255\n", W, H);
		int wpl = (W + 31) / 32;
		for (int y = 0; y < H; y++)
			for (int x = 0; x < W; x++) {
				int bit = (bits[y * wpl + (x >> 5)] >> (x & 31)) & 1;
				uint32_t rgb = bit ? 0xEEE : 0x000;
				if (x >= fx && x < fx + fw && y >= fy && y < fy + fh) {
					int c = bit, px = x - fx, py = y - fy;
					for (int k = 0; k < 4; k++) c |= (int)((pl[k][py * 10 + (px >> 5)] >> (px & 31)) & 1) << (k + 1);
					rgb = lg_colour_rgb(c);
				}
				fputc((int)((rgb >> 8) & 15) * 17, f);
				fputc((int)((rgb >> 4) & 15) * 17, f);
				fputc((int)(rgb & 15) * 17, f);
			}
		fclose(f);
		shots++;
		la_figure_colour(0);
		draw();
		// the colour window taken and given back: the figure alone drawn again
		la_figure_recolour(&page, 4);
		memcpy(part_bits, bits, sizeof bits);
		la_figure_colour(4);
		draw();
		CHECK(!memcmp(part_bits, bits, sizeof bits), "recoloured: the figure alone, as a whole drawing makes it");
		la_figure_recolour(&page, 0);
		memcpy(part_bits, bits, sizeof bits);
		la_figure_colour(0);
		draw();
		CHECK(!memcmp(part_bits, bits, sizeof bits), "and back to black and white");
	}
	type("4");
	key(0x0d);
	CHECK(A.checks[0].answered && A.checks[0].right_first, "4 eighths");
	key(' ');
	CHECK(A.step == 2 && LG.animated, "a moving figure");
	CHECK(la_animating(), "animating");
	hp_ms_now += 2500;
	{
		int x, y, w, h;
		CHECK(la_draw_figure(&page, &x, &y, &w, &h) && w == 320 && h == 240, "a frame of it alone (%d x %d)", w, h);
	}
	shot("27-moving");
	// frame after frame, only what moved is drawn (the still face copied
	// back under it): each must be what a whole drawing makes
	{
		int same = 0;
		for (int f = 0; f < 6; f++) {
			hp_ms_now += 170;
			int x, y, w, h;
			la_draw_figure(&page, &x, &y, &w, &h);
			if (f == 2) {
				int r[4];
				CHECK(lg_dirty_px(A.fig_x, A.fig_y, A.fig_w, A.fig_h, r) && (r[2] - r[0]) * (r[3] - r[1]) < A.fig_w * A.fig_h,
					"a frame draws only where the hands were and are");
			}
			memcpy(part_bits, bits, sizeof bits);
			A.fig_need = false;		// (the same frame, drawn whole)
			la_draw(&page);
			if (!memcmp(part_bits, bits, sizeof bits)) same++;
			else {
				int wpl = (W + 31) / 32, n = 0;
				for (int i = 0; i < wpl * H; i++) n += __builtin_popcount(part_bits[i] ^ bits[i]);
				printf("  frame %d: %d pixels differ\n", f, n);
			}
		}
		CHECK(same == 6, "six frames drawn by their changes are the frames drawn whole (%d)", same);
		hp_ms_now -= 6 * 170;
	}
	// a long step, scrolled with the wheel: the pixels move, only the band is drawn
	la_resize(W, 330);			// (a shorter window: this step scrolls in it)
	la_draw(&page);

	CHECK(ev_part(LA_WHEEL, 0, -1), "scrolling down draws only what it uncovers (scroll %d)", A.scroll);
	CHECK(A.scroll == 32, "a notch: 32 rows (%d)", A.scroll);
	CHECK(ev_part(LA_WHEEL, 0, -2) && A.scroll == 96, "two more (%d)", A.scroll);
	CHECK(ev_part(LA_WHEEL, 0, 3) && A.scroll == 0, "and back up (%d)", A.scroll);
	la_resize(W, H);
	la_draw(&page);
	key('p');
	CHECK(A.fig_paused && !la_animating(), "P stops it");
	uint32_t before = hp_ms_now;
	hp_ms_now += 5000;
	key('p');
	(void)before;
	CHECK(!A.fig_paused && hp_ms_now - A.fig_t0 == 2500, "P starts it again where it stopped (%u)", hp_ms_now - A.fig_t0);
	int r5 = -1;
	for (int i = 0; i < A.checks[0].q.nopt; i++) if (A.checks[0].q.opt[A.checks[0].q.order[i]].right) r5 = i;
	key('1' + (uint32_t)r5);
	for (int i = 0; i < 3 && A.step == 2; i++) key(' ');		// a long step shows its end first
	CHECK(A.step == 3 && LG.nctl == 3, "the triangle: three points (step %d, %d: %s%s)", A.step, LG.nctl, LG.err, LG.frame_err);
	{
		// drag corner c from (140, 50) to (200, 90), in figure units
		double s = (double)A.fig_w / LG_W;
		la_ev_t e;
		memset(&e, 0, sizeof e);
		e.type = LA_DOWN;
		e.x = A.fig_x + (int)(140 * s);
		e.y = A.fig_y + (int)(50 * s);
		la_event(&e);
		CHECK(A.fig_drag == 2, "the corner is taken (%d)", A.fig_drag);
		e.type = LA_MOVE;
		e.x = A.fig_x + (int)(200 * s);
		e.y = A.fig_y + (int)(90 * s);
		la_event(&e);
		e.type = LA_UP;
		la_event(&e);
		draw();
		CHECK(LG.ctl[2].x > 199 && LG.ctl[2].x < 201 && LG.ctl[2].y > 89 && LG.ctl[2].y < 91, "moved (%g, %g)", LG.ctl[2].x, LG.ctl[2].y);
		CHECK(fig_says("Together: 180"), "still 180 together");
	}
	shot("28-drag");
	// a drag draws the figure alone, never the page -- also under a line
	// of news (Space before the answer: "Answer the question first")
	snprintf(A.status, sizeof A.status, "Answer the question first (H for a hint).");
	draw();
	{
		double s = (double)A.fig_w / LG_W;
		la_ev_t e;
		memset(&e, 0, sizeof e);
		e.type = LA_DOWN;
		e.x = A.fig_x + (int)(200 * s);
		e.y = A.fig_y + (int)(90 * s);
		la_event(&e);
		CHECK(!A.dirty, "taking a point draws not the page (part %d fig %d)", A.part, A.fig_part);
		draw();
		e.type = LA_MOVE;
		e.x += 10;
		la_event(&e);
		e.x += 6;			// (moves come several at a time)
		la_event(&e);
		CHECK(!A.dirty && A.part && A.fig_part, "dragging draws the figure alone (dirty %d part %d fig %d)", A.dirty, A.part, A.fig_part);
		{
			int x, y, w, h;
			la_partial(&x, &y, &w, &h);
			la_draw_part(&page, x, y, w, h);
			int r[4];
			CHECK(lg_dirty_px(A.fig_x, A.fig_y, A.fig_w, A.fig_h, r) && r[1] > A.fig_y + 20,
				"a corner dragged: only around the triangle (not the line above it: %d,%d-%d,%d)", r[0], r[1], r[2], r[3]);
			memcpy(part_bits, bits, sizeof bits);
			draw();
			CHECK(!memcmp(part_bits, bits, sizeof bits), "and it is what a whole drawing makes");
		}
		e.type = LA_UP;
		la_event(&e);
		draw();
	}

	// -- review: the next day, questions from before come back --
	CHECK(file_has("/data/learn/learners/Mia/review.txt", "zeit-learn/start/a-lesson#"), "answers kept for review");
	hp_now += 86400;
	la_init(mem, sizeof mem);
	la_resize(W, H);
	settle();
	CHECK(A.screen == SC_HOME, "home the next day");
	int due = lr_count_due(NULL);
	CHECK(due >= 2, "questions due the next day: %d", due);		// (a skipped one is not among them)
	shot("29-home-review");
	key('r');
	CHECK(A.screen == SC_LESSON && A.review && A.nchecks == 1 && !A.rv_done, "a review question");
	shot("30-review");
	// answer each: the right one if it is a choice, else type what is right
	for (int guard = 0; guard < 30 && A.review && !A.rv_done; guard++) {
		la_check_t *k = &A.checks[0];
		if (!k->answered) {
			if (k->q.kind == LQ_CHOICE) {
				for (int i = 0; i < k->q.nopt; i++) if (k->q.opt[k->q.order[i]].right) key('1' + (uint32_t)i);
			} else {
				char ans[64];
				lq_answer_text(&k->q, ans, sizeof ans);
				type(ans);
				key(0x0d);
			}
			CHECK(k->answered && k->right_first, "reviewed right: %s", A.rv_key);
			if (guard == 0) shot("31-review-answered");
		}
		for (int i = 0; i < 3 && A.review && !A.rv_done && A.checks[0].answered; i++) key(' ');
	}
	CHECK(A.review && A.rv_done && A.rv_right == A.rv_total && A.rv_total >= 2, "review done: %d of %d", A.rv_right, A.rv_total);
	shot("32-review-done");
	key(0x0d);
	CHECK(A.screen == SC_HOME && lr_count_due(NULL) < due, "home, fewer due (%d)", lr_count_due(NULL));
	CHECK(file_has("/data/learn/learners/Mia/review.txt", " R zeit-learn/"), "review answers kept");

	// three days on: the questions known since come back as a lesson's warm-up
	hp_now += 3 * 86400;
	la_init(mem, sizeof mem);
	la_resize(W, H);
	settle();
	CHECK(lr_count_due("zeit-learn") > 0, "due again in three days");
	CHECK(la_open_course("/opt/ark/edu-zeitlos/zeit-learn"), "the course");
	la_open_lesson(2);
	CHECK(A.review && A.rv_then == 2 && A.rv_n <= 3, "a warm-up before lesson 3 (%d)", A.rv_n);
	shot("33-warm-up");
	key(0x1b);			// skip it: the lesson
	CHECK(!A.review && A.screen == SC_LESSON && A.lesson == 2, "Esc: on to the lesson");

	// the board: a step's words drawn first, its figure a moment after
	la_defer_figures = true;
	la_open_lesson(3);
	if (A.review) key(0x1b);		// (a warm-up first: skipped)
	A.step = 1;
	lv_enter();
	draw();
	CHECK(la_figure_due() && !LG.loaded && A.fig_wait, "the figure waits until the words are drawn (review %d fig %d wait %d shown %d screen %d)", A.review, A.fig, A.fig_wait, A.fig_shown, A.screen);
	shot("34-figure-waits");
	la_figure_now();
	{
		int x, y, w, h;
		bool part = la_partial(&x, &y, &w, &h);
		if (part) la_draw_part(&page, x, y, w, h);
		else la_draw(&page);
		memcpy(part_bits, bits, sizeof bits);
		la_draw(&page);
		CHECK(!memcmp(part_bits, bits, sizeof bits), "then the figure: what a whole page draws (%s)", part ? "the pane alone" : "all of it");
	}
	CHECK(!la_figure_due() && LG.loaded && LG.nops > 0, "drawn");
	draw();
	type("4");
	CHECK(A.typing == 0 && !strcmp(A.checks[0].typed, "4"), "then the answer can be typed (typing %d, \"%s\")", A.typing, A.checks[0].typed);
	// scrolled, the field is drawn where it now is as it is typed in
	la_resize(W, 300);
	draw();
	CHECK(ev_part(LA_WHEEL, 0, -1) && A.scroll > 0, "scrolled (%d)", A.scroll);
	CHECK(key_part('2'), "a letter typed after a scroll: the field, where it is now");
	CHECK(!strcmp(A.checks[0].typed, "42"), "typed (\"%s\")", A.checks[0].typed);
	la_resize(W, H);
	// as on the board: the lesson from its first step, the figures late
	la_open_lesson(3);
	if (A.review) key(0x1b);
	A.step = 0;
	lv_enter();
	draw();
	if (la_figure_due()) { la_figure_now(); draw(); }
	for (int i = 0; i < A.checks[0].q.nopt && !A.checks[0].answered; i++)
		if (A.checks[0].q.opt[A.checks[0].q.order[i]].right) key((uint32_t)('1' + i));
	key(' ');
	CHECK(A.step == 1, "on to Controls (%d, answered %d)", A.step, A.checks[0].answered);
	draw();
	if (la_figure_due()) { la_figure_now(); draw(); }
	CHECK(A.typing == 0, "the field has the keyboard (%d)", A.typing);
	CHECK(key_part('7'), "a letter: the field alone");
	CHECK(!strcmp(A.checks[0].typed, "7"), "typed (\"%s\")", A.checks[0].typed);
	la_defer_figures = false;

	// opening a course as the board does: a request to the card a turn,
	// the window answering between; Esc stops it; titles come after
	{
		yplat = host_plat;
		yplat.fs.list = y_list;
		yplat.fs.read = y_read;
		yplat.read_at = y_read_at;
		LP = &yplat;
		wstack = malloc(64 << 10);
		la_go(SC_CATALOGUE);
		draw();
		snprintf(A.want_course, sizeof A.want_course, "%s", A.course_open ? A.course->m.path : "");
		CHECK(A.want_course[0], "a course to open again");
		char again[sizeof A.want_course];
		snprintf(again, sizeof again, "%s", A.want_course);
		A.back_to = SC_CATALOGUE;
		la_go(SC_LOADING);
		yields = 0;
		for (int i = 0; i < 2; i++) wstep();
		CHECK(la_opening() && won && yields == 2, "opening, a request at a time (%d)", yields);
		if (won) zco_abandon(&wco);
		won = false;
		la_stop_opening();		// Esc
		CHECK(A.screen == SC_CATALOGUE && !A.course_open, "Esc stops it: back to the catalogue (%d)", A.screen);
		snprintf(A.want_course, sizeof A.want_course, "%s", again);
		la_go(SC_LOADING);
		yields = 0;
		int turns = 0;
		while (wstep() && turns < 1000) turns++;
		CHECK(A.screen == SC_COURSE && A.course_open, "opened in %d turns, %d requests", turns, yields);
		CHECK(turns > 3, "more than a turn or two (%d)", turns);
		const lf_unit_t *un = &A.course->units[A.unit_sel];
		bool all = true;
		for (int k = 0; k < un->nlessons; k++) if (A.course->lessons[un->first_lesson + k].title[0] != 1) all = false;
		CHECK(all && !la_busy(), "the unit's titles read, a turn each");
		LP = &host_plat;
		free(wstack);
	}

	printf("%s: %d pictures, %d failed\n", fails ? "render FAILED" : "render ok", shots, fails);
	return fails != 0;
}
