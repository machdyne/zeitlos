/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- the screens: learners, home, the catalogue, a course. The
 * lesson itself is lstep.c. docs/learn_ui.md shows them all.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "lapp_int.h"
#include "zpaths.h"
#include "zkbd.h"

la_t A;

#define K_ENTER 0x0d
#define K_ESC 0x1b
#define K_TAB 0x09
#define K_BS 0x7f

#define MENU_W 800		// the menu screens' page in memory (draw_menu())
#define MENU_H 600
#define HDR 24			// the header band
#define FOOT 28			// the footer band

static const char *const help_text =
	"learn teaches, one step at a time.\n"
	"\n"
	"Space, Enter\tnext, or check an answer\n"
	"Backspace\tthe step before\n"
	"1-6\tchoose an answer\n"
	"F5 (or H)\ta hint (up to three)\n"
	"F6 (or S)\tread the step aloud\n"
	"F7 (or M)\treport a mistake in the lesson\n"
	"F8\tskip: on to the next step\n"
	"Tab\tmove between buttons\n"
	"Up, Down\tmove in a list; scroll a step\n"
	"Left, Right\tbetween lists; a figure's control\n"
	"C\ta figure's next control\n"
	"P\tpause a moving figure\n"
	"R\treview (on the home screen)\n"
	"Esc\tgo back up; skip a warm-up\n"
	"F3\tfind a course\n"
	"L\tchange learner (on the home screen)\n"
	"F10\tcolour for figures, on or off\n"
	"\n"
	"Your progress is kept as you go, on this\n"
	"machine or on your own USB stick. Questions\n"
	"come back later, spaced out, to keep them.";

// -- memory --

void *la_alloc(unsigned long n) {
	uint32_t at = (A.used + 7u) & ~7u;
	if (at + n > A.memsize) return NULL;
	A.used = at + (uint32_t)n;
	memset(A.mem + at, 0, n);
	return A.mem + at;
}

static uint8_t scratch_mem[64 << 10];

// a learner's progress and review, read
static bool open_learner(const ls_who_t *w) {
	if (!ls_open(w, A.rec_mem, A.rec_memsize)) return false;
	lr_open(A.rv_mem, A.rv_memsize);
	return true;
}

void la_init(void *mem, uint32_t size) {
	memset(&A, 0, sizeof A);
	A.mem = (uint8_t *)mem;
	A.memsize = size;
	A.course = (lf_course_t *)la_alloc(sizeof(lf_course_t));
	A.les = (lf_lesson_t *)la_alloc(sizeof(lf_lesson_t));
	A.lesson_memsize = 192u << 10;
	A.lesson_mem = (uint8_t *)la_alloc(A.lesson_memsize);
	A.rec_memsize = 160u << 10;
	A.rec_mem = (uint8_t *)la_alloc(A.rec_memsize);
	A.rv_memsize = 192u << 10;
	A.rv_mem = (uint8_t *)la_alloc(A.rv_memsize);
	lx_arena_init(&A.scratch, scratch_mem, sizeof scratch_mem);
	A.focus = -1;
	A.typing = -1;
	A.fig_drag = -1;
	A.screen = SC_LOADING;
	lk_begin(la_alloc);
	// the learner last used, if they are still there
	A.nwho = ls_list(A.who, LS_MAX_LEARNERS);
	char cfg[200];
	long n = LP->fs.read(LP->fs.ctx, Z_DIR_LEARN_DATA "/learn.cfg", cfg, sizeof cfg - 1);
	A.back_to = SC_LEARNERS;
	if (n > 0) {
		cfg[n] = 0;
		char *k = strstr(cfg, "learner = ");
		if (k) {
			k += 10;
			char *e = strchr(k, '\n');
			if (e) *e = 0;
			for (int i = 0; i < A.nwho; i++)
				if (!strcmp(A.who[i].dir, k) && open_learner(&A.who[i])) {
					A.who_sel = i;
					A.back_to = SC_HOME;
				}
		}
	}
	A.dirty = true;
}

void la_resize(int w, int h) {
	A.w = w;
	A.h = h;
	A.dirty = true;
}

bool la_quit(void) { return A.quit; }

void la_close(void) {
	if (A.screen == SC_LESSON && A.step > 0) ls_at(A.lesson_id, A.step);
	A.quit = true;
}
const char *la_help(void) { return help_text; }

const char *la_title(void) {
	if ((A.screen == SC_LESSON || A.screen == SC_COURSE || A.screen == SC_DONE) && A.course_open)
		snprintf(A.title, sizeof A.title, "learn - %s", A.course->m.title);
	else snprintf(A.title, sizeof A.title, "learn");
	return A.title;
}

// -- the screen's targets --

void la_hot(int x, int y, int w, int h, int action, int arg) {
	if (A.nhot >= LA_MAX_HOT) return;
	la_hot_t *t = &A.hot[A.nhot++];
	t->x = (int16_t)x; t->y = (int16_t)y; t->w = (int16_t)w; t->h = (int16_t)h;
	t->action = (uint8_t)action;
	t->arg = (int16_t)arg;
}

void la_go(int screen) {
	A.screen = screen;
	A.focus = -1;
	A.status[0] = 0;
	A.dirty = true;
}

static void remember_learner(void) {
	const ls_who_t *w = ls_current();
	if (!w) return;
	char cfg[200];
	int n = snprintf(cfg, sizeof cfg, "# learn: the learner last used\nlearner = %s\n", w->dir);
	LP->mkdir(Z_DIR_LEARN_DATA);
	LP->write_whole(Z_DIR_LEARN_DATA "/learn.cfg", cfg, n);
}

// -- the frame every screen has --

void la_frame(ld_page_t *p, const char *crumb, int step, int steps) {
	ld_style_t s6 = ld_st(&LDF.f6, 0);
	ld_textz(p, &s6, 12, 15, crumb, LD_INK);
	if (steps > 0) {
		int x = A.w - 12 - steps * 12;
		for (int i = 0; i < steps; i++) {
			double cx = x + i * 12 + 4, cy = 11;
			if (i < step) ld_disc(p, cx, cy, 3.5, LD_INK);
			else if (i == step) ld_disc(p, cx, cy, 4.5, LD_INK);
			else ld_ring(p, cx, cy, 3.5, 1, LD_INK);
		}
	} else if (ls_current()) {
		char who[64];
		snprintf(who, sizeof who, "%s%s", ls_current()->name, ls_current()->where == LS_ON_STICK ? " (stick)" : "");
		ld_text_r(p, &s6, A.w - 12, 15, who, LD_INK);
	}
	ld_hline(p, 0, A.w - 1, HDR, LD_INK);
}

void la_footer(ld_page_t *p, const char *keys, const char *button, int action) {
	int y = A.h - FOOT;
	ld_hline(p, 0, A.w - 1, y, LD_INK);
	if (A.status[0]) {
		ld_style_t s = ld_st(&LDF.f6, 0);
		ld_textz(p, &s, 12, y + 18, A.status, LD_INK);
	} else ld_keys(p, 12, y + 17, keys, LD_INK);
	if (button) {
		int bw = 108, bx = A.w - bw - 10;
		int idx = A.nhot;
		la_hot(bx, y + 4, bw, 20, action, 0);
		ld_button(p, bx, y + 4, bw, 20, button, true, A.focus == idx);
	}
}

static void button(ld_page_t *p, int x, int y, int w, int h, const char *label, bool filled, int action, int arg) {
	int idx = A.nhot;
	la_hot(x, y, w, h, action, arg);
	ld_button(p, x, y, w, h, label, filled, A.focus == idx);
}

// -- learners --

static void draw_learners(ld_page_t *p) {
	la_frame(p, "learn", -1, 0);
	ld_style_t h = ld_st(&LDF.serif, 26), b = ld_st(&LDF.body, 0), s = ld_st(&LDF.small, 0);
	ld_textz(p, &h, 24, 64, "Who is learning?", LD_INK);
	int y = 88;
	if (!A.nwho) {
		ld_wrap(p, &b, 24, y + 14, A.w - 48, 18,
			"Nobody yet. Make a learner: a name, and a place for their progress. "
			"There is no password -- a learner is for keeping progress apart, not secret.", LD_INK);
		y += 64;
	}
	for (int i = 0; i < A.nwho && y < A.h - FOOT - 70; i++) {
		char label[64];
		snprintf(label, sizeof label, "%s", A.who[i].name);
		int idx = A.nhot;
		la_hot(24, y, 300, 26, A_LEARNER, i);
		bool sel = i == A.who_sel;
		if (sel) ld_rrect(p, 24, y, 300, 26, 4, LD_INK);
		else ld_rframe(p, 24, y, 300, 26, 4, 1, LD_INK);
		ld_style_t bb = ld_st(&LDF.body_b, 0);
		ld_textz(p, &bb, 36, y + 18, label, sel ? LD_DARK : LD_INK);
		if (A.who[i].where == LS_ON_STICK) ld_text_r(p, &s, 316, y + 17, "on the stick", sel ? LD_DARK : LD_INK);
		if (A.focus == idx) ld_rframe(p, 21, y - 3, 306, 32, 6, 1, LD_INK);
		y += 34;
	}
	button(p, 24, y + 8, 160, 24, "New learner", false, A_NEW_LEARNER, 0);
	la_footer(p, "Up/Down|choose;Enter|start;N|new learner;F1|help;Esc|leave", NULL, 0);
}

static void pick_learner(int i) {
	if (i < 0 || i >= A.nwho) return;
	A.who_sel = i;
	if (!open_learner(&A.who[i])) {
		snprintf(A.status, sizeof A.status, "Could not read %s's progress.", A.who[i].name);
		return;
	}
	remember_learner();
	la_go(SC_HOME);
}

static void new_learner(void) {
	char name[LS_NAME];
	if (!LP->prompt("New learner", "Their name (first name is enough):", "", name, sizeof name)) return;
	if (!ls_name_ok(name)) {
		snprintf(A.status, sizeof A.status, "A name of letters, digits, spaces, - and _, up to 31.");
		A.dirty = true;
		return;
	}
	int where = LS_ON_MACHINE;
	if (LP->exists(Z_DIR_USB))
		if (LP->confirm("Where to keep it", "Keep this learner's progress on the USB stick?\n(No: on this machine.)"))
			where = LS_ON_STICK;
	ls_who_t w;
	if (!ls_create(name, where, &w)) {
		snprintf(A.status, sizeof A.status, "Could not make %s: is the name taken?", name);
		A.dirty = true;
		return;
	}
	A.nwho = ls_list(A.who, LS_MAX_LEARNERS);
	for (int i = 0; i < A.nwho; i++) if (!strcmp(A.who[i].dir, w.dir)) pick_learner(i);
}

static bool key_learners(uint32_t k) {
	if (k == Z_KEY_UP && A.who_sel > 0) { A.who_sel--; return true; }
	if (k == Z_KEY_DOWN && A.who_sel < A.nwho - 1) { A.who_sel++; return true; }
	if ((k == K_ENTER || k == ' ') && A.nwho) { pick_learner(A.who_sel); return true; }
	if (k == 'n' || k == 'N') { new_learner(); return true; }
	if (k == K_ESC) {
		if (ls_current()) la_go(SC_HOME);
		else A.quit = true;
		return true;
	}
	return false;
}

// -- home --

static void humanize(const char *id, char *out, int cap) {
	// the last part of an id, as words: "same-denominator" -> "Same denominator"
	const char *s = strrchr(id, '/');
	lf_title_of(s ? s + 1 : id, out, cap);
}

// a lesson's name: its title if its course is open, else its id in words
static void lesson_name(const char *id, char *out, int cap) {
	if (A.course_open) {
		char lid[200];
		for (int i = 0; i < A.course->nlessons; i++) {
			lf_lesson_id(A.course, i, lid, sizeof lid);
			if (!strcmp(lid, id)) { snprintf(out, (size_t)cap, "%s", la_lesson_title(i)); return; }
		}
	}
	humanize(id, out, cap);
}

static void course_of(const char *lesson_id, char *out, int cap) {
	const char *sl = strchr(lesson_id, '/');
	int n = sl ? (int)(sl - lesson_id) : (int)strlen(lesson_id);
	if (n >= cap) n = cap - 1;
	memcpy(out, lesson_id, (size_t)n);
	out[n] = 0;
}

static void draw_home(ld_page_t *p) {
	la_frame(p, "learn", -1, 0);
	ld_style_t h = ld_st(&LDF.serif, 26), b = ld_st(&LDF.body, 0), bb = ld_st(&LDF.body_b, 0),
		s = ld_st(&LDF.small, 0), f5 = ld_st(&LDF.f5, 0), big = ld_st(&LDF.sans_b, 22);
	char line[160];
	snprintf(line, sizeof line, "Hello, %s", ls_current() ? ls_current()->name : "");
	ld_textz(p, &h, 24, 60, line, LD_INK);

	// continue: the one thing to do next; review beside it when some is due
	int due = lr_count_due(NULL);
	int cw = A.w - 48 - (due ? 196 : 0), cy = 74, ch = 104;
	if (due) {
		int rx = 24 + cw + 12, rw = 184, ridx = A.nhot;
		la_hot(rx, cy, rw, ch, A_REVIEW, 0);
		ld_rframe(p, rx, cy, rw, ch, 6, 1, LD_INK);
		if (A.focus == ridx) ld_rframe(p, rx - 3, cy - 3, rw + 6, ch + 6, 8, 1, LD_INK);
		ld_textz(p, &f5, rx + 14, cy + 16, "REVIEW", LD_INK);
		snprintf(line, sizeof line, "%d due", due);
		ld_textz(p, &big, rx + 14, cy + 44, line, LD_INK);
		ld_wrap(p, &s, rx + 14, cy + 66, rw - 24, 14, "Questions from before, to keep them.", LD_INK);
		ld_rframe(p, rx + rw - 40, cy + ch - 30, 26, 20, 4, 1, LD_INK);
		ld_style_t s6r = ld_st(&LDF.f6, 0);
		ld_text_c(p, &s6r, rx + rw - 27, cy + ch - 16, "R", LD_INK);
	}
	const char *last = ls_last();
	int idx = A.nhot;
	la_hot(24, cy, cw, ch, A_CONTINUE, 0);
	ld_rrect(p, 24, cy, cw, ch, 6, LD_INK);
	if (A.focus == idx) ld_rframe(p, 21, cy - 3, cw + 6, ch + 6, 8, 1, LD_INK);
	if (last[0]) {
		char cid[LF_NAME], t[96];
		course_of(last, cid, sizeof cid);
		const lf_info_t *m = lk_find(cid);
		ld_textz(p, &f5, 38, cy + 16, "CONTINUE", LD_DARK);
		ld_fitz(p, &big, 38, cy + 44, cw - 28, m ? m->title : cid, LD_DARK);
		lesson_name(last, t, sizeof t);
		const ls_rec_t *r = ls_get(last);
		snprintf(line, sizeof line, "%s %s", r && r->done ? "After" : "At", t);
		ld_fitz(p, &b, 38, cy + 68, cw - 28, line, LD_DARK);
		int done = ls_count_done(cid);
		snprintf(line, sizeof line, "%d lesson%s done in this course", done, done == 1 ? "" : "s");
		ld_textz(p, &s, 38, cy + 88, line, LD_DARK);
	} else {
		ld_textz(p, &f5, 38, cy + 16, "START", LD_DARK);
		ld_textz(p, &big, 38, cy + 44, "Choose a course", LD_DARK);
		ld_textz(p, &b, 38, cy + 68, "School, college, life skills, languages, Zeitlos", LD_DARK);
	}
	ld_rframe(p, 24 + cw - 64, cy + ch - 30, 50, 20, 4, 1, LD_DARK);
	ld_style_t s6 = ld_st(&LDF.f6, 0);
	ld_text_c(p, &s6, 24 + cw - 39, cy + ch - 16, "Enter", LD_DARK);

	int y = cy + ch + 30;
	ld_textz(p, &bb, 24, y, "Your courses", LD_INK);
	ld_hline(p, 24, A.w - 24, y + 6, LD_INK);
	y += 14;
	int nc = ls_ncourses();
	if (!nc) {
		ld_textz(p, &s, 24, y + 16, "None yet: the courses you start will be here.", LD_INK);
		y += 30;
	}
	for (int i = 0; i < nc && y < A.h - FOOT - 70; i++) {
		const char *cid = ls_course(i);
		const lf_info_t *m = lk_find(cid);
		char lvl[32] = "", t[96];
		if (m) lk_level_name(m, lvl, sizeof lvl);
		int done = ls_count_done(cid);
		idx = A.nhot;
		la_hot(20, y, A.w - 40, 32, A_COURSE_ROW, i);
		if (A.focus == idx) ld_rframe(p, 18, y - 2, A.w - 36, 34, 4, 1, LD_INK);
		ld_mastery(p, 26, y + 4, done ? (done >= 10 ? 3 : done >= 3 ? 2 : 1) : 0);
		ld_fitz(p, &bb, 44, y + 13, 200, m ? m->title : cid, LD_INK);
		ld_textz(p, &s6, 250, y + 12, lvl, LD_INK);
		snprintf(line, sizeof line, "%d done", done);
		ld_text_r(p, &s6, A.w - 30, y + 12, line, LD_INK);
		lesson_name(ls_last_in(cid), t, sizeof t);
		snprintf(line, sizeof line, "last: %s", t);
		ld_textz(p, &s6, 44, y + 26, line, LD_INK);
		y += 36;
	}
	// explore
	y = A.h - FOOT - 36;
	ld_textz(p, &bb, 24, y + 15, "Explore", LD_INK);
	int x = 100;
	for (int a = LK_SCHOOL; a < LK_AREAS && x < A.w - 60; a++) {
		const char *nm = lk_area_name(a);
		int w = ld_widthz(&s, nm) + 20;
		button(p, x, y, w, 22, nm, false, A_EXPLORE, a);
		x += w + 8;
	}
	la_footer(p, due ? "Enter|continue;R|review;C|courses;L|learner;F1|help;Esc|leave" :
		"Enter|continue;C|courses;L|learner;F1|help;Esc|leave", NULL, 0);
}

static void continue_last(void) {
	const char *last = ls_last();
	if (!last[0]) { A.area = LK_ALL; la_go(SC_CATALOGUE); return; }
	char cid[LF_NAME];
	course_of(last, cid, sizeof cid);
	const lf_info_t *m = lk_find(cid);
	if (!m) {
		snprintf(A.status, sizeof A.status, "The course %s is not on this machine.", cid);
		A.dirty = true;
		return;
	}
	snprintf(A.want_lesson, sizeof A.want_lesson, "%s", last);
	snprintf(A.want_course, sizeof A.want_course, "%s", m->path);
	A.back_to = SC_HOME;
	la_go(SC_LOADING);
}

static void open_course_row(int i) {
	const lf_info_t *m = lk_find(ls_course(i));
	if (!m) return;
	A.want_lesson[0] = 0;
	snprintf(A.want_course, sizeof A.want_course, "%s", m->path);
	A.back_to = SC_HOME;
	la_go(SC_LOADING);
}

static bool key_home(uint32_t k) {
	if (k == K_ENTER || k == ' ') { continue_last(); return true; }
	if (k == 'c' || k == 'C') { A.area = LK_ALL; la_go(SC_CATALOGUE); return true; }
	if (k == 'r' || k == 'R') { if (!lv_review_start(NULL, 20, -1)) snprintf(A.status, sizeof A.status, "Nothing to review today."); return true; }
	if (k == 'l' || k == 'L') {		// another learner
		A.nwho = ls_list(A.who, LS_MAX_LEARNERS);
		la_go(SC_LEARNERS);
		return true;
	}
	if (k == K_ESC) { A.quit = true; return true; }
	return false;
}

// -- the catalogue --

static void filter(void) {
	A.cat_n = 0;
	for (int i = 0; i < LK.n; i++) {
		const lf_info_t *m = LK.c[i];
		if (A.area != LK_ALL && lk_area_of(m) != A.area) continue;
		if (A.search[0]) {
			// any word of the search in the title, summary or subject
			char hay[400];
			snprintf(hay, sizeof hay, "%s %s %s %s", m->title, m->summary, m->subject, m->id);
			for (char *q = hay; *q; q++) if (*q >= 'A' && *q <= 'Z') *q = (char)(*q + 32);
			char need[48];
			snprintf(need, sizeof need, "%s", A.search);
			for (char *q = need; *q; q++) if (*q >= 'A' && *q <= 'Z') *q = (char)(*q + 32);
			if (!strstr(hay, need)) continue;
		}
		A.cat_idx[A.cat_n++] = i;
	}
	if (A.cat_sel >= A.cat_n) A.cat_sel = A.cat_n ? A.cat_n - 1 : 0;
}

static void draw_catalogue(ld_page_t *p) {
	filter();
	char crumb[96];
	snprintf(crumb, sizeof crumb, "Courses  >  %s%s%s", lk_area_name(A.area), A.search[0] ? "  >  " : "", A.search);
	la_frame(p, crumb, -1, 0);
	ld_style_t b = ld_st(&LDF.small, 0), bb = ld_st(&LDF.small_b, 0), s6 = ld_st(&LDF.f6, 0),
		rb = ld_st(&LDF.body_b, 0), big = ld_st(&LDF.sans_b, 20);
	int c1 = 150, c2 = A.w > 560 ? c1 + 226 : A.w;		// a narrow window: no details column
	// areas
	for (int a = 0; a < LK_AREAS; a++) {
		int y = HDR + 10 + a * 22;
		int idx = A.nhot;
		la_hot(6, y, c1 - 12, 20, A_AREA, a);
		if (a == A.area) {
			ld_fill(p, 6, y, c1 - 12, 20, LD_INK);
			ld_textz(p, &bb, 14, y + 14, lk_area_name(a), LD_DARK);
		} else ld_textz(p, &b, 14, y + 14, lk_area_name(a), LD_INK);
		if (A.focus == idx || (A.cat_col == 0 && a == A.area && A.focus < 0))
			ld_frame(p, 4, y - 2, c1 - 8, 24, 1, LD_INK);
	}
	int ay = HDR + 10 + LK_AREAS * 22 + 8;
	ld_hline(p, 6, c1 - 6, ay, LD_INK);
	ld_textz(p, &s6, 12, ay + 18, "F3  find a course", LD_INK);
	char cnt[40];
	snprintf(cnt, sizeof cnt, "%d course%s here", A.cat_n, A.cat_n == 1 ? "" : "s");
	ld_textz(p, &s6, 12, ay + 34, cnt, LD_INK);
	ld_vline(p, c1, HDR + 1, A.h - FOOT - 1, LD_INK);

	// courses
	int rh = 42, rows = (A.h - FOOT - HDR - 8) / rh;
	if (rows < 1) rows = 1;
	if (A.cat_sel < A.cat_top) A.cat_top = A.cat_sel;
	if (A.cat_sel >= A.cat_top + rows) A.cat_top = A.cat_sel - rows + 1;
	if (!A.cat_n) {
		ld_wrap(p, &b, c1 + 12, HDR + 30, c2 - c1 - 24, 15,
			LK.n ? "No course here matches." :
			"No courses are installed. Courses come in packs, in " Z_DIR_ARK "/edu..., "
			"or in a folder called edu on a USB stick.", LD_INK);
	}
	for (int r = 0; r < rows && A.cat_top + r < A.cat_n; r++) {
		int i = A.cat_top + r;
		const lf_info_t *m = LK.c[A.cat_idx[i]];
		int y = HDR + 4 + r * rh;
		int idx = A.nhot;
		la_hot(c1 + 4, y, c2 - c1 - 8, rh - 2, A_CAT_ROW, i);
		bool sel = i == A.cat_sel;
		if (sel) ld_frame(p, c1 + 4, y, c2 - c1 - 8, rh - 2, A.cat_col == 1 ? 2 : 1, LD_INK);
		if (A.focus == idx) ld_rframe(p, c1 + 2, y - 2, c2 - c1 - 4, rh + 2, 4, 1, LD_INK);
		ld_fitz(p, &rb, c1 + 12, y + 17, c2 - c1 - 24, m->title, LD_INK);
		char lvl[32], meta[96];
		lk_level_name(m, lvl, sizeof lvl);
		snprintf(meta, sizeof meta, "%s%s%s%s", lvl, m->ages[0] ? ", " : "", m->ages, m->nreviewers ? "" : ", not reviewed yet");
		ld_textz(p, &s6, c1 + 12, y + 33, meta, LD_INK);
	}
	if (A.cat_top + rows < A.cat_n) ld_text_r(p, &s6, c2 - 10, A.h - FOOT - 6, "more below", LD_INK);
	if (c2 >= A.w) { la_footer(p, "Up/Down|choose;Enter|start;F3|find;Esc|home", "Start", A_START); return; }
	ld_vline(p, c2, HDR + 1, A.h - FOOT - 1, LD_INK);

	// the chosen course
	if (A.cat_n) {
		const lf_info_t *m = LK.c[A.cat_idx[A.cat_sel]];
		int x = c2 + 12, w = A.w - x - 12, y = HDR + 30;
		int lines = ld_wrap_lines(&big, w, m->title);
		y = ld_wrap(p, &big, x, y, w, 22, m->title, LD_INK) + 4;
		(void)lines;
		if (m->summary[0]) y = ld_wrap(p, &b, x, y, w, 15, m->summary, LD_INK) + 8;
		char lvl[32], t[200];
		lk_level_name(m, lvl, sizeof lvl);
		snprintf(t, sizeof t, "%s%s%s", lvl, m->ages[0] ? ", ages " : "", m->ages);
		ld_textz(p, &s6, x, y, t, LD_INK);
		y += 14;
		if (m->nneeds) {
			snprintf(t, sizeof t, "First: %s%s", m->needs[0], m->nneeds > 1 ? " ..." : "");
			ld_textz(p, &s6, x, y, t, LD_INK);
			y += 14;
		}
		int o = snprintf(t, sizeof t, "By ");
		for (int i = 0; i < m->nauthors && o < (int)sizeof t - 2; i++)
			o += snprintf(t + o, sizeof t - (size_t)o, "%s%s", i ? "; " : "", m->authors[i]);
		y = ld_wrap(p, &s6, x, y, w, 13, t, LD_INK) + 2;
		if (m->nreviewers) {
			const char *last = m->reviewers[m->nreviewers - 1];
			const char *d = strrchr(last, ';');
			while (d && (*d == ';' || *d == ' ')) d++;
			snprintf(t, sizeof t, "Reviewed%s%s (%d reviewer%s)", d ? " " : "", d ? d : "", m->nreviewers,
				m->nreviewers == 1 ? "" : "s");
		} else snprintf(t, sizeof t, "Not reviewed yet");
		ld_textz(p, &s6, x, y, t, LD_INK);
		y += 14;
		snprintf(t, sizeof t, "Version %d", m->version);
		ld_textz(p, &s6, x, y, t, LD_INK);
		y += 10;
		if (strchr(m->classes, 'B') && y < A.h - FOOT - 110) {
			int bh = 14 + 13 * ld_wrap_lines(&s6, w - 16, "Some of what this course teaches can hurt if it is done wrong. Practise it with an experienced adult.");
			ld_rframe(p, x, y + 8, w, bh + 14, 4, 2, LD_INK);
			ld_textz(p, &bb, x + 8, y + 24, "! Safety", LD_INK);
			ld_wrap(p, &s6, x + 8, y + 38, w - 16, 13,
				"Some of what this course teaches can hurt if it is done wrong. Practise it with an experienced adult.", LD_INK);
		}
	}
	la_footer(p, "Up/Down|choose;Left/Right|areas;Enter|start;F3|find;Esc|home", A.cat_n ? "Start" : NULL, A_START);
}

static void start_selected(void) {
	if (!A.cat_n) return;
	A.back_to = SC_CATALOGUE;
	const lf_info_t *m = LK.c[A.cat_idx[A.cat_sel]];
	A.want_lesson[0] = 0;
	snprintf(A.want_course, sizeof A.want_course, "%s", m->path);
	la_go(SC_LOADING);
}

static bool key_catalogue(uint32_t k) {
	if (k == Z_KEY_LEFT) { A.cat_col = 0; return true; }
	if (k == Z_KEY_RIGHT) { A.cat_col = 1; return true; }
	if (k == Z_KEY_UP) {
		if (A.cat_col == 0) { if (A.area > 0) { A.area--; A.cat_sel = A.cat_top = 0; } }
		else if (A.cat_sel > 0) A.cat_sel--;
		return true;
	}
	if (k == Z_KEY_DOWN) {
		if (A.cat_col == 0) { if (A.area < LK_AREAS - 1) { A.area++; A.cat_sel = A.cat_top = 0; } }
		else if (A.cat_sel < A.cat_n - 1) A.cat_sel++;
		return true;
	}
	if (k == Z_KEY_PAGEDOWN) { A.cat_sel += 8; if (A.cat_sel >= A.cat_n) A.cat_sel = A.cat_n - 1; if (A.cat_sel < 0) A.cat_sel = 0; return true; }
	if (k == Z_KEY_PAGEUP) { A.cat_sel -= 8; if (A.cat_sel < 0) A.cat_sel = 0; return true; }
	if (k == K_ENTER || k == ' ') {
		if (A.cat_col == 0) { A.cat_col = 1; return true; }
		start_selected();
		return true;
	}
	if (k == Z_KEY_FIND) {
		char q[48];
		if (LP->prompt("Find a course", "Words to look for (empty: every course):", A.search, q, sizeof q)) {
			snprintf(A.search, sizeof A.search, "%s", q);
			A.area = LK_ALL;
			A.cat_sel = A.cat_top = 0;
			A.cat_col = 1;
		}
		return true;
	}
	if (k == K_ESC) {
		if (A.search[0]) { A.search[0] = 0; return true; }
		la_go(SC_HOME);
		return true;
	}
	return false;
}

// -- a course --

bool la_open_course(const char *path) {
	char err[160];
	A.course_open = false;
	if (!lf_course_load(&LP->fs, path, A.course, err, sizeof err)) {
		snprintf(A.status, sizeof A.status, "%s", err);
		return false;
	}
	A.course_open = true;
	lv_course_lib();
	A.unit_sel = 0;
	A.lesson_sel = 0;
	A.course_col = 0;
	// start where the learner left off: the unit of the last lesson, or the first not done
	const char *last = ls_last_in(A.course->m.id);
	char id[200];
	int first_open = -1;
	for (int i = 0; i < A.course->nlessons; i++) {
		lf_lesson_id(A.course, i, id, sizeof id);
		const ls_rec_t *r = ls_get(id);
		if (last[0] && !strcmp(id, last)) {
			A.unit_sel = A.course->lessons[i].unit;
			A.lesson_sel = i - A.course->units[A.unit_sel].first_lesson;
		}
		if (first_open < 0 && !(r && r->done)) first_open = i;
	}
	if (!last[0] && first_open >= 0) {
		A.unit_sel = A.course->lessons[first_open].unit;
		A.lesson_sel = first_open - A.course->units[A.unit_sel].first_lesson;
	}
	return true;
}

static int unit_mastery(int u) {
	const lf_unit_t *un = &A.course->units[u];
	int done = 0;
	char id[200];
	for (int i = 0; i < un->nlessons; i++) {
		lf_lesson_id(A.course, un->first_lesson + i, id, sizeof id);
		const ls_rec_t *r = ls_get(id);
		if (r && r->done) done++;
	}
	if (!un->nlessons) return 0;
	if (done == un->nlessons) return 4;
	return done * 4 / un->nlessons;
}

// a lesson's title, its # line, read from the card (once): la_work()
// does it, a lesson a slice, never the drawing
static const char *read_title(int i) {
	lf_lref_t *r = &A.course->lessons[i];
	if (r->title[0] == 1) return r->title + 1;	// read before
	char path[LF_PATH + 200], buf[256];
	lf_lesson_path(A.course, i, path, sizeof path);
	uint32_t t0 = LP->ms();
	long n = LP->read_at(path, 0, buf, sizeof buf - 1);
	uint32_t took = LP->ms() - t0;
	if (took > 200) printf("learn: a lesson's title: %lu ms to read %s (%ld)\n", (unsigned long)took, path, n);
	if (n > 0) {
		buf[n] = 0;
		char *h = strstr(buf, "# ");
		if (h && (h == buf || h[-1] == '\n')) {
			h += 2;
			char *e = strchr(h, '\n');
			int len = e ? (int)(e - h) : (int)strlen(h);
			if (len > (int)sizeof r->title - 2) len = (int)sizeof r->title - 2;
			while (len && (h[len - 1] == '\r' || h[len - 1] == ' ')) len--;
			r->title[0] = 1;
			memcpy(r->title + 1, h, (size_t)len);
			r->title[len + 1] = 0;
			return r->title + 1;
		}
	}
	// unreadable, or no # line: the title made from its name stands, and
	// is not read again (each read is a trip to the card)
	size_t len = strlen(r->title);
	if (len > sizeof r->title - 2) len = sizeof r->title - 2;
	memmove(r->title + 1, r->title, len);
	r->title[0] = 1;
	r->title[len + 1] = 0;
	return r->title + 1;
}

// the title of a lesson: its # line if read yet, else the one made from
// its name -- drawing never waits for the card (docs/responsive.md)
const char *la_lesson_title(int i) {
	lf_lref_t *r = &A.course->lessons[i];
	return r->title[0] == 1 ? r->title + 1 : r->title;
}

// the first lesson of the chosen unit whose title is not read yet; -1
static int title_wanted(void) {
	if (A.screen != SC_COURSE || !A.course_open || A.unit_sel >= A.course->nunits) return -1;
	const lf_unit_t *un = &A.course->units[A.unit_sel];
	for (int k = 0; k < un->nlessons; k++)
		if (A.course->lessons[un->first_lesson + k].title[0] != 1) return un->first_lesson + k;
	return -1;
}

static void draw_course(ld_page_t *p) {
	lf_course_t *c = A.course;
	char crumb[160], lvl[32];
	lf_info_t *m = &c->m;
	lk_level_name(m, lvl, sizeof lvl);
	snprintf(crumb, sizeof crumb, "%s  >  %s", lvl, m->title);
	la_frame(p, crumb, -1, 0);
	ld_style_t big = ld_st(&LDF.sans_b, 22), b = ld_st(&LDF.body, 0), bb = ld_st(&LDF.body_b, 0),
		s = ld_st(&LDF.small, 0), s6 = ld_st(&LDF.f6, 0), f5 = ld_st(&LDF.f5, 0);
	ld_fitz(p, &big, 24, HDR + 32, A.w - 48, m->title, LD_INK);
	int c1 = A.w > 560 ? A.w / 2 : A.w;
	int bottom = A.h - FOOT - 46;

	// units (with part headings), scrolled to keep the chosen one in view
	int rows_y = HDR + 50, rh = 30;
	int first = 0, total_rows = 0;
	for (int u = 0; u < c->nunits; u++) total_rows += (u == 0 || c->units[u].part != c->units[u - 1].part) && c->units[u].part >= 0 ? 2 : 1;
	(void)total_rows;
	{	// find the first unit to show so the chosen one fits
		int h = 0;
		for (int u = A.unit_sel; u >= 0; u--) {
			h += rh + ((u == 0 || c->units[u].part != c->units[u - 1].part) && c->units[u].part >= 0 ? 18 : 0);
			if (rows_y + h > bottom) { first = u + 1; break; }
		}
	}
	int y = rows_y;
	for (int u = first; u < c->nunits && y + rh <= bottom; u++) {
		const lf_unit_t *un = &c->units[u];
		if (un->part >= 0 && (u == first || un->part != c->units[u - 1].part)) {
			ld_fitz(p, &f5, 24, y + 10, c1 - 40, c->parts[un->part].title, LD_INK);
			y += 16;
		}
		int idx = A.nhot;
		la_hot(16, y, c1 - 28, rh - 2, A_UNIT, u);
		if (u == A.unit_sel) ld_frame(p, 16, y, c1 - 28, rh - 2, A.course_col == 0 ? 2 : 1, LD_INK);
		if (A.focus == idx) ld_rframe(p, 14, y - 2, c1 - 24, rh + 2, 4, 1, LD_INK);
		ld_mastery(p, 24, y + 8, unit_mastery(u));
		char t[100];
		snprintf(t, sizeof t, "%d  %s", u - (un->part >= 0 ? c->parts[un->part].first_unit : 0) + 1, un->title);
		ld_fitz(p, &b, 42, y + 19, c1 - 28 - 32, t, LD_INK);
		y += rh;
	}
	if (c1 >= A.w) goto foot;
	ld_vline(p, c1, HDR + 44, bottom, LD_INK);

	// the chosen unit's lessons
	{
		const lf_unit_t *un = &c->units[A.unit_sel];
		ld_fitz(p, &bb, c1 + 16, HDR + 62, A.w - c1 - 32, un->title, LD_INK);
		int ly = HDR + 76;
		char id[200];
		for (int i = 0; i < un->nlessons && ly + 28 <= bottom; i++) {
			int li = un->first_lesson + i;
			lf_lesson_id(c, li, id, sizeof id);
			const ls_rec_t *r = ls_get(id);
			int idx = A.nhot;
			la_hot(c1 + 10, ly, A.w - c1 - 20, 26, A_LESSON, li);
			if (i == A.lesson_sel) ld_frame(p, c1 + 10, ly, A.w - c1 - 20, 26, A.course_col == 1 ? 2 : 1, LD_INK);
			if (A.focus == idx) ld_rframe(p, c1 + 8, ly - 2, A.w - c1 - 16, 30, 4, 1, LD_INK);
			if (r && r->done) ld_tick(p, c1 + 18, ly + 8, LD_INK);
			else if (r && r->step) {
				double tri[] = { c1 + 18, ly + 8, c1 + 28, ly + 13, c1 + 18, ly + 18 };
				ld_poly(p, tri, 3, LD_INK);
			} else ld_ring(p, c1 + 23, ly + 13, 4.5, 1, LD_INK);
			ld_fitz(p, &s, c1 + 36, ly + 17, A.w - c1 - 36 - (r && r->done && r->total ? 64 : 24), la_lesson_title(li), LD_INK);
			if (r && r->done && r->total) {
				char sc[16];
				snprintf(sc, sizeof sc, "%d/%d", r->right, r->total);
				ld_text_r(p, &s6, A.w - 18, ly + 16, sc, LD_INK);
			}
			ly += 30;
		}
	}
foot:
	{
		ld_hline(p, 16, A.w - 16, A.h - FOOT - 40, LD_INK);
		char t[300];
		int o = snprintf(t, sizeof t, "By ");
		for (int i = 0; i < m->nauthors && o < (int)sizeof t - 2; i++)
			o += snprintf(t + o, sizeof t - (size_t)o, "%s%s", i ? "; " : "", m->authors[i]);
		snprintf(t + o, sizeof t - (size_t)o, "   %s   Version %d", m->nreviewers ? "Reviewed" : "Not reviewed yet", m->version);
		ld_textz(p, &s6, 24, A.h - FOOT - 24, t, LD_INK);
		if (m->standards[0]) {
			snprintf(t, sizeof t, "Aligned with: %s", m->standards);
			ld_textz(p, &s6, 24, A.h - FOOT - 10, t, LD_INK);
		}
	}
	la_footer(p, "Up/Down|choose;Left/Right|units, lessons;Enter|lesson;Esc|back", "Open lesson", A_OPEN);
}

static bool key_course(uint32_t k) {
	lf_course_t *c = A.course;
	const lf_unit_t *un = &c->units[A.unit_sel];
	if (k == Z_KEY_LEFT) { A.course_col = 0; return true; }
	if (k == Z_KEY_RIGHT) { A.course_col = 1; return true; }
	if (k == Z_KEY_UP) {
		if (A.course_col == 0) { if (A.unit_sel > 0) { A.unit_sel--; A.lesson_sel = 0; } }
		else if (A.lesson_sel > 0) A.lesson_sel--;
		return true;
	}
	if (k == Z_KEY_DOWN) {
		if (A.course_col == 0) { if (A.unit_sel < c->nunits - 1) { A.unit_sel++; A.lesson_sel = 0; } }
		else if (A.lesson_sel < un->nlessons - 1) A.lesson_sel++;
		return true;
	}
	if (k == K_ENTER || k == ' ') {
		if (A.course_col == 0 && A.w > 560) { A.course_col = 1; return true; }
		la_open_lesson(un->first_lesson + A.lesson_sel);
		return true;
	}
	if (k == K_ESC) { la_go(A.back_to == SC_CATALOGUE ? SC_CATALOGUE : SC_HOME); return true; }
	return false;
}

// -- loading --

static void draw_loading(ld_page_t *p) {
	la_frame(p, "learn", -1, 0);
	ld_style_t b = ld_st(&LDF.body, 0);
	char t[96];
	if (LK.scanning) snprintf(t, sizeof t, "Finding courses ... %d", LK.n);
	else snprintf(t, sizeof t, "Opening the course ...   (Esc stops)");
	ld_text_c(p, &b, A.w / 2, A.h / 2, t, LD_INK);
}

// Esc while a course opens: back where the learner came from
void la_stop_opening(void) {
	if (A.screen != SC_LOADING || LK.scanning) return;
	A.want_course[0] = 0;
	A.want_lesson[0] = 0;
	A.course_open = false;
	la_go(A.back_to == SC_CATALOGUE ? SC_CATALOGUE : SC_HOME);
}

bool la_opening(void) { return A.screen == SC_LOADING && !LK.scanning; }

bool la_busy(void) { return A.screen == SC_LOADING || title_wanted() >= 0; }

bool la_work(uint32_t budget_ms) {
	if (A.screen == SC_COURSE) {
		int li = title_wanted();
		if (li < 0) return false;
		read_title(li);
		// the unit's titles shown together, once all are read: a page
		// a title was a redraw of the whole screen for each
		if (title_wanted() < 0) A.dirty = true;
		return title_wanted() >= 0;
	}
	if (A.screen != SC_LOADING) return false;
	uint32_t t0 = LP->ms();
	if (LK.scanning) {
		while (lk_step() && LP->ms() - t0 < budget_ms) { }
		A.dirty = true;
		if (LK.scanning) return true;
		// Scheme and the figure library, while the loading screen is up
		// anyway: the first figure then shows at once
		lg_set_clock(LP->ms);
		lg_start();
		if (!A.want_course[0]) { la_go(A.back_to); return false; }
	}
	if (A.want_course[0]) {
		char path[LF_PATH];
		snprintf(path, sizeof path, "%s", A.want_course);
		A.want_course[0] = 0;
		int from = A.back_to;
		if (!la_open_course(path)) { la_go(SC_HOME); return false; }
		A.back_to = from == SC_CATALOGUE ? SC_CATALOGUE : SC_HOME;
		if (A.want_lesson[0]) {
			char id[200];
			for (int i = 0; i < A.course->nlessons; i++) {
				lf_lesson_id(A.course, i, id, sizeof id);
				if (!strcmp(id, A.want_lesson)) {
					const ls_rec_t *r = ls_get(id);
					// done: go on to the next lesson; else back into this one
					int li = r && r->done && i + 1 < A.course->nlessons ? i + 1 : i;
					A.want_lesson[0] = 0;
					la_open_lesson(li);
					return false;
				}
			}
			A.want_lesson[0] = 0;
		}
		la_go(SC_COURSE);
	}
	return false;
}

void la_open_arg(const char *arg) {
	if (!arg || !arg[0]) return;
	snprintf(A.want_course, sizeof A.want_course, "%s", arg);
}

// -- events --

static bool activate(int action, int arg) {
	switch (action) {
	case A_LEARNER: if (A.who_sel == arg) pick_learner(arg); else A.who_sel = arg; return true;
	case A_NEW_LEARNER: new_learner(); return true;
	case A_CONTINUE: continue_last(); return true;
	case A_REVIEW: lv_review_start(NULL, 20, -1); return true;
	case A_EXPLORE: A.area = arg; A.cat_sel = A.cat_top = 0; A.cat_col = 1; A.back_to = SC_HOME; la_go(SC_CATALOGUE); return true;
	case A_COURSE_ROW: open_course_row(arg); return true;
	case A_AREA: A.area = arg; A.cat_sel = A.cat_top = 0; A.cat_col = 0; return true;
	case A_CAT_ROW:
		if (A.cat_sel == arg && A.cat_col == 1) start_selected();
		else { A.cat_sel = arg; A.cat_col = 1; }
		return true;
	case A_START: start_selected(); return true;
	case A_UNIT: A.unit_sel = arg; A.lesson_sel = 0; A.course_col = 0; return true;
	case A_OPEN: la_open_lesson(A.course->units[A.unit_sel].first_lesson + A.lesson_sel); return true;
	case A_LESSON:
		if (A.course->lessons[arg].unit == A.unit_sel &&
			arg - A.course->units[A.unit_sel].first_lesson == A.lesson_sel && A.course_col == 1)
			la_open_lesson(arg);
		else {
			A.lesson_sel = arg - A.course->units[A.unit_sel].first_lesson;
			A.course_col = 1;
		}
		return true;
	default:
		if (A.screen == SC_LESSON || A.screen == SC_DONE) return lv_hot(action, arg);
		return false;
	}
}

static bool on_key(uint32_t k, uint8_t mods) {
	// Tab moves between the screen's buttons; Enter presses the one with focus
	if (k == K_TAB && A.typing < 0) {
		if (!A.nhot) return false;
		if (mods & Z_KBD_MOD_SHIFT) A.focus = A.focus <= 0 ? A.nhot - 1 : A.focus - 1;
		else A.focus = A.focus + 1 >= A.nhot ? 0 : A.focus + 1;
		A.dirty = true;
		return true;
	}
	if ((k == K_ENTER || k == ' ') && A.focus >= 0 && A.focus < A.nhot && A.typing < 0) {
		la_hot_t h = A.hot[A.focus];
		A.focus = -1;
		return activate(h.action, h.arg);
	}
	if (A.focus >= 0 && k != K_ENTER) A.focus = -1;
	A.status[0] = 0;
	switch (A.screen) {
	case SC_LEARNERS: return key_learners(k);
	case SC_HOME: return key_home(k);
	case SC_CATALOGUE: return key_catalogue(k);
	case SC_COURSE: return key_course(k);
	case SC_LESSON: return lv_key(k, mods);
	case SC_DONE: return lv_done_key(k);
	}
	return false;
}

void la_damage(int x, int y, int w, int h) {
	if (w <= 0 || h <= 0) return;
	A.fig_part = false;		// (fig_changed() says when it is the figure's alone)
	// a scroll waiting draws only the band it uncovers: anything else, the whole page
	if (A.scroll_shift) { A.dirty = true; A.part_now = true; return; }
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (!A.part) {
		A.part_x0 = x; A.part_y0 = y; A.part_x1 = x + w; A.part_y1 = y + h;
	} else {
		if (x < A.part_x0) A.part_x0 = x;
		if (y < A.part_y0) A.part_y0 = y;
		if (x + w > A.part_x1) A.part_x1 = x + w;
		if (y + h > A.part_y1) A.part_y1 = y + h;
	}
	A.part = A.part_now = true;
}

bool la_partial(int *x, int *y, int *w, int *h) {
	if (A.dirty || !A.part) return false;
	int x1 = A.part_x1 > A.w ? A.w : A.part_x1, y1 = A.part_y1 > A.h ? A.h : A.part_y1;
	*x = A.part_x0;
	*y = A.part_y0;
	*w = x1 - A.part_x0;
	*h = y1 - A.part_y0;
	return *w > 0 && *h > 0;
}

bool la_pressed(const la_ev_t *e, int *x, int *y, int *w, int *h) {
	const la_hot_t *h0 = NULL;
	if (e->type == LA_DOWN) {
		for (int i = A.nhot - 1; i >= 0 && !h0; i--) {
			const la_hot_t *t = &A.hot[i];
			if (e->x >= t->x && e->x < t->x + t->w && e->y >= t->y && e->y < t->y + t->h) h0 = t;
		}
	} else if (e->type == LA_KEY && (e->key == K_ENTER || e->key == ' ') && A.typing < 0) {
		if (A.focus >= 0 && A.focus < A.nhot) h0 = &A.hot[A.focus];
		else for (int i = 0; i < A.nhot && !h0; i++) if (A.hot[i].action == A_NEXT) h0 = &A.hot[i];
	}
	if (!h0 || h0->action == A_FIELD) return false;
	*x = h0->x; *y = h0->y; *w = h0->w; *h = h0->h;
	return true;
}

// One line of what a key found, for the board's console (learn.c).
const char *la_trace(void) {
	static char t[160];
	if (A.screen == SC_LESSON && A.typing >= 0 && A.typing < A.nchecks)
		snprintf(t, sizeof t, "lesson %d step %d, typing in check %d: \"%.40s\"%s", A.lesson, A.step, A.typing,
			A.checks[A.typing].typed, A.checks[A.typing].answered ? " (answered)" : "");
	else snprintf(t, sizeof t, "screen %d, lesson %d step %d, not typing (%d of %d checks)", A.screen, A.lesson, A.step, A.typing, A.nchecks);
	return t;
}

bool la_event(const la_ev_t *e) {
	bool d = false;
	A.part_now = false;
	A.quiet = false;
	// a line of news that comes, changes or goes, or a focus ring that
	// goes, changes more than a part (a line already showing does not:
	// dragging a figure's point under it draws the figure alone)
	char had_status[sizeof A.status];
	memcpy(had_status, A.status, sizeof had_status);
	bool had_focus = A.focus >= 0;
	switch (e->type) {
	case LA_KEY: d = on_key(e->key, e->mods); break;
	case LA_DOWN:
		if (lv_mouse(LA_DOWN, e->x, e->y)) { A.focus = -1; d = true; break; }
		for (int i = A.nhot - 1; i >= 0; i--) {
			const la_hot_t *h = &A.hot[i];
			if (e->x >= h->x && e->x < h->x + h->w && e->y >= h->y && e->y < h->y + h->h) {
				A.focus = -1;
				A.status[0] = 0;
				d = activate(h->action, h->arg);
				break;
			}
		}
		break;
	case LA_WHEEL:
		if (A.screen == SC_LESSON) d = lv_wheel(e->notches);
		else if (A.screen == SC_CATALOGUE) {
			A.cat_sel -= e->notches;
			if (A.cat_sel < 0) A.cat_sel = 0;
			if (A.cat_sel >= A.cat_n) A.cat_sel = A.cat_n ? A.cat_n - 1 : 0;
			d = true;
		}
		break;
	case LA_MOVE:
	case LA_UP:
		d = lv_mouse(e->type, e->x, e->y);
		break;
	default: break;
	}
	// taken, and nothing changed (a slider at its end): nothing drawn
	if (d && A.quiet && !A.part_now) d = false;
	// a handler that said which part changed asked for that part alone
	if (d && (!A.part_now || strcmp(had_status, A.status) || (had_focus && A.focus < 0))) A.dirty = true;
	return A.dirty || A.part;
}

static void draw_screen(ld_page_t *p);

// The screens around the lessons (learners, home, catalogue, course,
// done) are lists and words that change a row at a time: cleared and
// drawn again in the window, the whole screen flashed -- the footer and
// the column line with it -- for every key. So they are drawn in memory
// and only the box of what changed is copied to the window: what did not
// change is never touched. (A lesson keeps its own ways: its figure and
// its fields are drawn alone, and a new step is a new page.)
#define MENU_WORDS (((MENU_W + 31) / 32) * MENU_H)
static uint32_t menu_a[MENU_WORDS], menu_b[MENU_WORDS];
static uint32_t *menu_now = menu_a, *menu_was = menu_b;	// swapped, not copied
static int menu_w, menu_h;		// what menu_was holds (0: nothing)

static bool menu_screen(void) {
	return A.screen != SC_LESSON && A.screen != SC_LOADING;
}

static void draw_menu(ld_page_t *p) {
	ld_page_t m;
	int wpl = (A.w + 31) / 32;
	ld_page(&m, menu_now, A.w, A.h);
	{	// cleared a word at a time (the C library's memset goes a byte at a time)
		uint32_t *q = menu_now, *e = menu_now + wpl * A.h;
		while (q < e) *q++ = 0;
	}
	draw_screen(&m);
	ld_clip_reset(p);
	if (menu_w != A.w || menu_h != A.h || A.menu_fresh) {
		// the window's pixels are not what was drawn last: all of it
		ld_blit(p, menu_now, wpl, 0, 0, 0, 0, A.w, A.h, LD_ROP_COPY);
	} else {
		// each run of changed rows, with the columns that changed in it:
		// rows that did not change part one run from the next, so a
		// footer and a list are copied apart, the rows between left alone
		int y0 = -1, w0 = wpl, w1 = -1;
		for (int y = 0; y <= A.h; y++) {
			int a0 = wpl, a1 = -1;
			if (y < A.h) {
				const uint32_t *a = menu_now + y * wpl, *b = menu_was + y * wpl;
				for (int i = 0; i < wpl; i++) if (a[i] != b[i]) { if (i < a0) a0 = i; a1 = i; }
			}
			if (a1 >= 0) {
				if (y0 < 0) { y0 = y; w0 = a0; w1 = a1; }
				else { if (a0 < w0) w0 = a0; if (a1 > w1) w1 = a1; }
			} else if (y0 >= 0) {
				int x0 = w0 * 32, x1 = (w1 + 1) * 32;
				if (x1 > A.w) x1 = A.w;
				ld_blit(p, menu_now, wpl, x0, y0, x0, y0, x1 - x0, y - y0, LD_ROP_COPY);
				y0 = -1;
			}
		}
	}
	uint32_t *t = menu_was;
	menu_was = menu_now;
	menu_now = t;
	menu_w = A.w;
	menu_h = A.h;
	A.menu_fresh = false;
}

bool la_wants_draw(void) { return A.dirty || A.part; }

void la_draw(ld_page_t *p) {
	A.keep_fig = false;
	A.scroll_shift = 0;			// (all of it is drawn anyway)
	A.fig_part = false;
	if (menu_screen() && A.w <= MENU_W && A.h <= MENU_H) { draw_menu(p); return; }
	menu_w = 0;					// (the window no longer shows what menu_was holds)
	ld_clip_reset(p);
	ld_clear(p);
	draw_screen(p);
}

// the window was drawn by someone else (wm asked for all of it, or it was
// covered): the next menu is copied whole
void la_forget_page(void) { A.menu_fresh = true; }

// The same drawing, kept to the part: what is outside it is the last
// page's, unchanged. (The targets are made again whole: the layout runs
// as always, only the pixels are confined.)
static bool menu_screen(void);
static void draw_menu(ld_page_t *p);

void la_draw_part(ld_page_t *p, int x, int y, int w, int h) {
	if (menu_screen() && A.w <= MENU_W && A.h <= MENU_H) { draw_menu(p); return; }	// (it copies only what changed)
	if (A.scroll_shift && A.screen == SC_LESSON) { lv_scrolled(p); return; }
	if (A.fig_part && A.screen == SC_LESSON) {
		// a figure's control: the figure and its controls, drawn over what is there
		A.fig_part = false;
		ld_clip_reset(p);
		ld_clip_push(p, x, y, x + w, y + h);
		lv_fig_part(p);
		ld_clip_reset(p);
		A.part = false;
		return;
	}
	ld_clip_reset(p);
	ld_clip_push(p, x, y, x + w, y + h);
	if (A.screen == SC_LESSON && A.fig && A.fig_w > 0 && A.keep_fig) {
		// the figure is drawn whole over itself (a copy from memory), so
		// it is not cleared first: cleared, it flashed for a key that only
		// changed the words beside it (Check: the verdict, the footer)
		int fx0 = A.fig_x, fy0 = A.fig_y, fx1 = A.fig_x + A.fig_w, fy1 = A.fig_y + A.fig_h;
		int x1 = x + w, y1 = y + h;
		if (fx0 >= x1 || fx1 <= x || fy0 >= y1 || fy1 <= y) ld_fill(p, x, y, w, h, LD_DARK);
		else {
			if (fy0 > y) ld_fill(p, x, y, w, fy0 - y, LD_DARK);
			if (fy1 < y1) ld_fill(p, x, fy1, w, y1 - fy1, LD_DARK);
			int ty = fy0 > y ? fy0 : y, by = fy1 < y1 ? fy1 : y1;
			if (fx0 > x) ld_fill(p, x, ty, fx0 - x, by - ty, LD_DARK);
			if (fx1 < x1) ld_fill(p, fx1, ty, x1 - fx1, by - ty, LD_DARK);
		}
	} else ld_fill(p, x, y, w, h, LD_DARK);
	A.keep_fig = false;
	draw_screen(p);
	ld_clip_reset(p);
}

static void draw_screen(ld_page_t *p) {
	A.nhot = 0;
	lx_arena_reset(&A.scratch);
	switch (A.screen) {
	case SC_LEARNERS: draw_learners(p); break;
	case SC_HOME: draw_home(p); break;
	case SC_CATALOGUE: draw_catalogue(p); break;
	case SC_COURSE: draw_course(p); break;
	case SC_LESSON: lv_draw(p); break;
	case SC_DONE: lv_done_draw(p); break;
	default: draw_loading(p); break;
	}
	if (A.focus >= A.nhot) A.focus = -1;
	A.dirty = false;
	A.part = false;
}
