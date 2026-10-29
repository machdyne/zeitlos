/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- a lesson, one step at a time: its words, its figure pane,
 * its questions and their feedback, and the end of the lesson. The
 * shape of every step follows docs/learn.md ("How a lesson teaches").
 */

#include <string.h>
#include <stdio.h>

#include "lapp_int.h"
#include "lmathdraw.h"
#include "zkbd.h"
#include "zpaths.h"

#define K_ENTER 0x0d
#define K_ESC 0x1b
#define K_BS 0x7f

#define HDR 24
#define FOOT 28
#define PANE_W 320
#define PANE_H 240
#define CTL_H 24		// a control's row under the figure
#define GAP 10			// between blocks

static lt_layout_t LT;		// one block's words at a time
// the links of the step being drawn: A_LINK's argument is the index
static char step_links[24][96];
static int nstep_links;

// review (at the end of this file)
static void rv_next(void);
static void scrollbar(ld_page_t *p);
static void fig_render(ld_page_t *p, int x, int y, int w, int h);
static void rv_leave(void);
static void rv_done_draw(ld_page_t *p);
static bool rv_done_key(uint32_t key);
static void rv_record(la_check_t *k);
static bool warmed;			// the warm-up before this lesson is done (or skipped)
static const char *fig_course;		// the course whose lesson is shown (its lib/)

// -- opening a lesson --

static uint32_t hash(const char *s) {
	uint32_t h = 2166136261u;
	while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
	return h;
}

void la_open_lesson(int lesson) {
	lf_course_t *c = A.course;
	if (lesson < 0 || lesson >= c->nlessons) return;
	// questions of this course that are due come first: a warm-up of three
	if (!warmed && !A.review && lr_count_due(c->m.id) > 0 && lv_review_start(c->m.id, 3, lesson)) return;
	warmed = false;
	A.review = false;
	fig_course = c->m.path;
	char path[LF_PATH + 200], err[160];
	lf_lesson_path(c, lesson, path, sizeof path);
	// the file at the start of the lesson's memory, the parse after it
	char *text = (char *)A.lesson_mem;
	long n = LP->read_at(path, 0, text, LF_MAX_LESSON);
	if (n < 0) { snprintf(A.status, sizeof A.status, "Cannot read the lesson %.100s", A.course->lessons[lesson].file); A.dirty = true; return; }
	text[n] = 0;
	lx_arena_init(&A.les_arena, A.lesson_mem + LF_MAX_LESSON + 8, A.lesson_memsize - LF_MAX_LESSON - 8);
	if (!lf_lesson_parse(&A.les_arena, text, n, A.les, err, sizeof err)) {
		snprintf(A.status, sizeof A.status, "This lesson has a mistake in it (%.90s): learncheck shows it.", err);
		A.dirty = true;
		return;
	}
	A.lesson = lesson;
	lf_lesson_id(c, lesson, A.lesson_id, sizeof A.lesson_id);
	const ls_rec_t *r = ls_get(A.lesson_id);
	A.step = r && !r->done && r->step < A.les->nsteps ? r->step : 0;
	A.right = A.total = 0;
	A.started_ms = LP->ms();
	const lf_unit_t *u = &c->units[c->lessons[lesson].unit];
	snprintf(A.crumb, sizeof A.crumb, "%s  >  %s  >  lesson %d of %d", c->m.title, u->title,
		lesson - u->first_lesson + 1, u->nlessons);
	la_go(SC_LESSON);
	lv_enter();
}

static const lf_step_t *cur(void) { return &A.les->steps[A.step]; }

static void add_check(const lx_node_t *form) {
	if (A.nchecks >= LA_MAX_CHECKS) return;
	la_check_t *k = &A.checks[A.nchecks];
	memset(k, 0, sizeof *k);
	if (!lq_load(&k->q, form, hash(A.lesson_id) + (uint32_t)A.step * 977u + (uint32_t)A.nchecks)) return;
	A.nchecks++;
}

// -- the figure --

static char course_lib[24576];		// the course's lib/*.scm, joined
static int course_lib_len;
static char fig_src[16384];			// the step's figure blocks, joined

typedef struct { char names[16][LF_NAME]; int n; } libs_t;

static void lib_file(void *arg, const char *name, bool is_dir) {
	libs_t *l = arg;
	int n = (int)strlen(name);
	if (is_dir || n < 5 || strcmp(name + n - 4, ".scm") || l->n >= 16) return;
	snprintf(l->names[l->n++], LF_NAME, "%s", name);
}

static char lib_for[LF_PATH];

// one folder's *.scm, in name order (so a course can say which comes
// first), added to course_lib; false if it is too big
static bool lib_dir(const char *dir) {
	if (!LP->exists(dir)) return true;
	static libs_t l;
	l.n = 0;
	LP->fs.list(LP->fs.ctx, dir, lib_file, &l);
	for (int i = 1; i < l.n; i++)
		for (int j = i; j > 0 && strcmp(l.names[j - 1], l.names[j]) > 0; j--) {
			char t[LF_NAME];
			memcpy(t, l.names[j], LF_NAME);
			memcpy(l.names[j], l.names[j - 1], LF_NAME);
			memcpy(l.names[j - 1], t, LF_NAME);
		}
	for (int i = 0; i < l.n; i++) {
		char path[LF_PATH + 80];
		snprintf(path, sizeof path, "%s/%s", dir, l.names[i]);
		long room = (long)sizeof course_lib - course_lib_len - 2;
		long n = LP->read_at(path, 0, course_lib + course_lib_len, room);
		if (n < 0) continue;
		if (n >= room) return false;
		course_lib_len += (int)n;
		course_lib[course_lib_len++] = '\n';
	}
	return true;
}

// the figure libraries of a course: its pack's lib/ (shared by the
// pack's courses), then its own lib/ (docs/learn_figures.md)
static void lib_load(const char *course_path) {
	if (!strcmp(lib_for, course_path)) return;
	snprintf(lib_for, sizeof lib_for, "%s", course_path);
	course_lib_len = 0;
	char dir[LF_PATH + 8];
	snprintf(dir, sizeof dir, "%s", course_path);
	char *slash = strrchr(dir, '/');
	bool ok = true;
	if (slash) {
		snprintf(slash, sizeof dir - (size_t)(slash - dir), "/lib");
		ok = lib_dir(dir);
	}
	snprintf(dir, sizeof dir, "%s/lib", course_path);
	if (ok) ok = lib_dir(dir);
	if (!ok) {
		snprintf(A.status, sizeof A.status, "The course's figure libraries are too big (24 KB at most).");
		course_lib_len = 0;
	}
	course_lib[course_lib_len] = 0;
}

void lv_course_lib(void) { lib_for[0] = 0; lib_load(A.course->m.path); }

static void fig_start(void);
static void fig_run(void);
static int pane_extra(void);

static void fig_enter(void) {
	lg_unload();
	if (fig_course) lib_load(fig_course);
	A.fig = false;
	A.fig_sel = 0;
	A.fig_drag = -1;
	A.fig_paused = false;
	A.fig_shown = false;
	int n = 0;
	const lf_step_t *s = cur();
	for (int i = s->first; i < s->first + s->count; i++) {
		const lf_block_t *b = &A.les->blocks[i];
		if (b->kind != LB_FIGURE || !b->text) continue;
		int len = (int)strlen(b->text);
		if (n + len + 2 >= (int)sizeof fig_src) break;
		memcpy(fig_src + n, b->text, (size_t)len);
		n += len;
		fig_src[n++] = '\n';
	}
	fig_src[n] = 0;
	if (!n) return;
	A.fig = true;
	lg_set_clock(LP->ms);
	A.fig_wait = true;
	if (!la_defer_figures) fig_start();
}

// The figure read and its first frame run: on the board after the page
// has been drawn without it (la_defer_figures), so the words show at
// once and the figure a moment later.
static void fig_start(void) {
	A.fig_wait = false;
	lg_load(course_lib_len ? course_lib : NULL, fig_src);
	A.fig_t0 = LP->ms();
	A.fig_need = true;
}

bool la_figure_due(void) { return A.screen == SC_LESSON && A.fig && A.fig_wait && A.fig_shown; }

static uint32_t lc_key(void);

void la_figure_now(void) {
	if (!A.fig || !A.fig_wait) return;
	uint32_t k0 = lc_key();
	fig_start();
	fig_run();
	// the same layout (no controls came, no mistake): the pane alone
	if (lc_key() == k0 && A.fig_shown) {
		bool alone = (!A.part || A.fig_part) && !A.dirty;
		la_damage(A.fig_x - 1, A.fig_y - 1, A.fig_w + 2, A.fig_h + 2 + pane_extra());
		A.fig_part = alone;
	} else A.dirty = true;
}

bool la_defer_figures;

static void fig_run(void) {
	if (!A.fig || !A.fig_need || A.fig_wait) return;
	A.fig_need = false;
	if (!LG.loaded) return;
	double t = A.fig_paused ? A.fig_pause_t : (LP->ms() - A.fig_t0) / 1000.0;
	// A frame that draws the still shapes too (the first, or after a
	// control changed) may take a while; one of a moving figure must be
	// quick, or the window stops answering between frames.
	bool moving = LG.animated && LG.nstill && LG.still_gen == LG.ctl_gen;
	bool ok = lg_frame(t, moving ? 800 : 5000);
	A.fig_last = LP->ms();
	if (!ok && LG.animated && strstr(LG.frame_err, "too long")) {
		// a moving figure this computer cannot keep up with: stopped, and said so
		A.fig_paused = true;
		A.fig_pause_t = t;
		snprintf(LG.frame_err, sizeof LG.frame_err, "this figure moves too slowly on this computer (P to try again)");
	}
	if (A.fig_sel >= LG.nctl || (LG.nctl && (!LG.ctl[A.fig_sel].used || LG.ctl[A.fig_sel].kind == LG_DRAG))) {
		A.fig_sel = 0;
		for (int i = 0; i < LG.nctl; i++) if (LG.ctl[i].used && LG.ctl[i].kind != LG_DRAG) { A.fig_sel = i; break; }
	}
}

// controls with a row under the figure (points to drag are in it)
static int fig_rows(void) {
	int n = 0;
	for (int i = 0; i < LG.nctl; i++) if (LG.ctl[i].used && LG.ctl[i].kind != LG_DRAG) n++;
	return n;
}

static bool fig_has_controls(void) { return A.fig && fig_rows() > 0; }

static void value_text(const lg_ctl_t *c, char *t, int cap) {
	double r = c->max - c->min, v = c->value;
	int d = c->step > 0 ? (c->step >= 1 ? 0 : c->step >= 0.1 ? 1 : c->step >= 0.01 ? 2 : 3) :
		(r >= 50 ? 0 : r >= 5 ? 1 : 2);
	snprintf(t, (size_t)cap, "%.*f", d, v);
	if (t[0] == '-' && strspn(t + 1, "0.") == strlen(t + 1)) memmove(t, t + 1, strlen(t));
}

static void draw_controls(ld_page_t *p, int x, int y, int w) {
	ld_style_t sm = ld_st(&LDF.small, 0), smb = ld_st(&LDF.small_b, 0);
	for (int i = 0; i < LG_MAX_CTL; i++) A.fig_ctl_y[i] = -1;
	int lw = 0;		// the labels' column
	for (int i = 0; i < LG.nctl; i++)
		if (LG.ctl[i].used && LG.ctl[i].kind != LG_DRAG) {
			int t = ld_widthz(&sm, LG.ctl[i].label);
			if (t > lw) lw = t;
		}
	if (lw > w / 3) lw = w / 3;
	int tx = x + lw + 12, vw = 52, tw = w - lw - 12 - vw - 8;
	A.fig_track_x = tx;
	A.fig_track_w = tw;
	for (int i = 0; i < LG.nctl; i++) {
		const lg_ctl_t *c = &LG.ctl[i];
		if (!c->used || c->kind == LG_DRAG) continue;
		A.fig_ctl_y[i] = y;
		ld_fill(p, x - 4, y, w + 8, CTL_H, LD_DARK);		// (drawn again alone: fig_update())
		bool sel = i == A.fig_sel;
		int base = y + 16;
		ld_fitz(p, sel ? &smb : &sm, x, base, lw + 4, c->label, LD_INK);
		char t[48];
		if (c->kind == LG_SLIDER) {
			ld_fill(p, tx, y + 11, tw, 2, LD_INK);
			double f = c->max > c->min ? (c->value - c->min) / (c->max - c->min) : 0;
			int kx = tx + (int)(f * tw + 0.5);
			ld_rrect(p, kx - 5, y + 4, 10, 16, 2, LD_INK);
			if (sel) ld_rframe(p, kx - 7, y + 2, 14, 20, 3, 1, LD_INK);
			value_text(c, t, sizeof t);
			ld_textz(p, &smb, tx + tw + 10, base, t, LD_INK);
		} else if (c->kind == LG_TOGGLE) {
			ld_frame(p, tx, y + 5, 14, 14, 1, LD_INK);
			if (c->value) ld_tick(p, tx + 2, y + 7, LD_INK);
			if (sel) ld_frame(p, tx - 2, y + 3, 18, 18, 1, LD_INK);
			ld_textz(p, &sm, tx + 22, base, c->value ? "on" : "off", LD_INK);
		} else if (c->kind == LG_CHOICE) {
			int n = c->nopt ? c->nopt : 1, cw = tw / n, cx = tx;
			for (int k = 0; k < c->nopt; k++) {
				bool on = (int)c->value == k;
				if (on) ld_rrect(p, cx, y + 3, cw - 4, 18, 3, LD_INK);
				else ld_rframe(p, cx, y + 3, cw - 4, 18, 3, 1, LD_INK);
				ld_style_t st = on ? smb : sm;
				ld_text_c(p, &st, cx + (cw - 4) / 2, base, c->opt[k], on ? LD_DARK : LD_INK);
				cx += cw;
			}
			if (sel) ld_frame(p, tx - 3, y + 1, n * cw + 2, 22, 1, LD_INK);
		}
		y += CTL_H;
	}
}

// The controls are drawn in memory and put on the page in one copy: a
// slider moved is never seen half drawn.
#define CB_W (LG_W + 16)
static uint32_t cb_bits[((CB_W + 31) / 32) * LG_MAX_CTL * CTL_H + 2];

static void controls(ld_page_t *p, int x, int y, int w) {
	int rows = 0;
	for (int i = 0; i < LG.nctl; i++) if (LG.ctl[i].used && LG.ctl[i].kind != LG_DRAG) rows++;
	if (!rows) { for (int i = 0; i < LG_MAX_CTL; i++) A.fig_ctl_y[i] = -1; return; }
	if (w + 8 > CB_W || !ld_clip_hits(p, x - 4, y, w + 8, rows * CTL_H)) {
		draw_controls(p, x, y, w);
		return;
	}
	ld_page_t b;
	ld_page(&b, cb_bits, w + 8, rows * CTL_H);
	ld_clear(&b);
	draw_controls(&b, 4, 0, w);
	for (int i = 0; i < LG_MAX_CTL; i++) if (A.fig_ctl_y[i] >= 0) A.fig_ctl_y[i] += y;
	A.fig_track_x += x - 4;
	ld_blit(p, cb_bits, b.wpl, 0, 0, x - 4, y, w + 8, rows * CTL_H, LD_ROP_COPY);
}

static uint32_t fig_drawn;		// the frame of the figure on the page

// over the figure: a frame's mistake, "paused"
static void pane_marks(ld_page_t *p, int x, int y, int w, int h) {
	ld_style_t sm = ld_st(&LDF.small, 0);
	if (LG.frame_err[0]) {
		char t[200];
		snprintf(t, sizeof t, "Figure: %s", LG.frame_err);
		int lines = ld_wrap_lines(&sm, w - 12, t);
		ld_fill(p, x, y + h - lines * 15 - 8, w, lines * 15 + 8, LD_INK);
		ld_wrap(p, &sm, x + 6, y + h - lines * 15 + 4, w - 12, 15, t, LD_DARK);
	}
	if (LG.animated && A.fig_paused) {
		ld_fill(p, x + w - 58, y + 4, 54, 16, LD_INK);
		ld_style_t f5 = ld_st(&LDF.f6, 0);
		ld_text_c(p, &f5, x + w - 31, y + 16, "paused", LD_DARK);
	}
}

// the figure, its frame and its controls; (x, y): the top left of the figure
static void draw_pane(ld_page_t *p, int x, int y, int w, int h) {
	ld_frame(p, x - 1, y - 1, w + 2, h + 2, 1, LD_INK);
	A.fig_x = x;
	A.fig_y = y;
	A.fig_w = w;
	A.fig_h = h;
	A.fig_shown = true;
	ld_style_t sm = ld_st(&LDF.small, 0);
	if (LG.err[0]) {
		// learncheck says the same, with the line: an author's mistake
		ld_fill(p, x, y, w, h, LD_DARK);
		char t[240];
		snprintf(t, sizeof t, "This figure has a mistake in it: %.150s. Press M to tell the course's authors.", LG.err);
		ld_wrap(p, &sm, x + 10, y + 24, w - 20, 15, t, LD_INK);
		return;
	}
	if (A.fig_wait) {
		ld_fill(p, x, y, w, h, LD_DARK);
		ld_style_t f6 = ld_st(&LDF.f6, 0);
		ld_text_c(p, &f6, x + w / 2, y + h / 2 + 4, "drawing the figure ...", LD_INK);
		return;
	}
	fig_render(p, x, y, w, h);
	if (ld_clip_hits(p, x, y, w, h)) fig_drawn = LG.frames;
	pane_marks(p, x, y, w, h);
	controls(p, x, y + h + 8, w);
}

static int fig_planes;		// the colour window's planes while it shows the figure, 0 off

// a figure that asks for colour, shown (black and white is the rule)
bool la_figure_present(void) {
	return A.screen == SC_LESSON && A.fig && LG.loaded && LG.colour && !LG.err[0] && !(A.review && A.rv_done);
}

bool la_figure_rect(int *x, int *y, int *w, int *h) {
	if (A.screen != SC_LESSON || !A.fig || !A.fig_shown || !LG.loaded || LG.err[0]) return false;
	if (A.fig_y < HDR + 1 || A.fig_y + A.fig_h > A.h - FOOT) return false;
	*x = A.fig_x;
	*y = A.fig_y;
	*w = A.fig_w;
	*h = A.fig_h;
	return true;
}

// The figure alone drawn again in the new way (the colour window taken
// or given back): the rest of the page is as it was.
void la_figure_recolour(ld_page_t *p, int planes) {
	if (planes == fig_planes) return;
	fig_planes = planes;
	if (A.screen != SC_LESSON || !A.fig || !A.fig_shown || A.fig_wait || LG.err[0] || (A.review && A.rv_done)) return;
	ld_clip_reset(p);
	ld_clip_t was = ld_clip_push(p, 0, HDR + 1, A.w, A.h - FOOT);
	ld_clip_push(p, A.fig_x, A.fig_y, A.fig_x + A.fig_w, A.fig_y + A.fig_h);
	fig_render(p, A.fig_x, A.fig_y, A.fig_w, A.fig_h);
	pane_marks(p, A.fig_x, A.fig_y, A.fig_w, A.fig_h);
	ld_clip_pop(p, was);
	ld_clip_reset(p);
	fig_drawn = LG.frames;
}

void la_figure_colour(int planes) {
	if (planes == fig_planes) return;
	fig_planes = planes;
	A.dirty = true;
}

void la_figure_plane(ld_page_t *p, int k) {
	lg_render_plane(p, 0, 0, A.fig_w, A.fig_h, k, fig_planes);
}

// The figure is drawn straight onto the page, its shapes by the
// hardware (on the board); a moving figure's still shapes are a copy.
// The figure is drawn in memory -- its own bitmap, kept from one
// drawing to the next -- and put on the page in one copy: what the
// page's clip lets through of it, which after a control or a frame is
// what changed. Nothing is seen half drawn, and its shapes cost spans
// in memory rather than an operation of the hardware each.
static uint32_t fig_bits[(LG_W / 32) * LG_H + 2];

static void fig_render(ld_page_t *p, int x, int y, int w, int h) {
	int x0 = x > p->cx0 ? x : p->cx0, y0 = y > p->cy0 ? y : p->cy0;
	int x1 = x + w < p->cx1 ? x + w : p->cx1, y1 = y + h < p->cy1 ? y + h : p->cy1;
	if (x1 <= x0 || y1 <= y0) return;
	if (w > LG_W || h > LG_H) {		// (never: the pane is at most 320 x 240)
		if (fig_planes) lg_render_plane(p, x, y, w, h, 0, fig_planes);
		else lg_render(p, x, y, w, h);
		return;
	}
	ld_page_t b;
	ld_page(&b, fig_bits, LG_W, LG_H);
	ld_clip_push(&b, x0 - x, y0 - y, x1 - x, y1 - y);
	if (fig_planes) lg_render_plane(&b, 0, 0, w, h, 0, fig_planes);
	else lg_render(&b, 0, 0, w, h);
	ld_blit(p, fig_bits, LG_W / 32, x0 - x, y0 - y, x0, y0, x1 - x0, y1 - y0, LD_ROP_COPY);
}

static int pane_extra(void) { int r = fig_rows(); return r ? r * CTL_H + 8 : 0; }

bool la_animating(void) {
	return A.screen == SC_LESSON && A.fig && LG.loaded && LG.animated && !A.fig_paused && A.fig_shown;
}

// The figure again and nothing else, over what is there (nothing is
// cleared first, so nothing flickers): the shapes that moved since the
// frame on the page -- the still ones under them copied back -- or all
// of it; and its controls when they changed.
static void fig_update(ld_page_t *p, bool with_controls) {
	// the control first: it moves at once, the figure follows it
	if (with_controls) controls(p, A.fig_x, A.fig_y + A.fig_h + 8, A.fig_w);
	fig_run();
	if (LG.err[0] || A.fig_wait) return;
	if (LG.unchanged && LG.frames == fig_drawn + 1) { fig_drawn = LG.frames; return; }	// (between two ticks)
	int r[4];
	if (!(LG.frames == fig_drawn + 1 && lg_dirty_px(A.fig_x, A.fig_y, A.fig_w, A.fig_h, r))) {
		r[0] = A.fig_x; r[1] = A.fig_y; r[2] = A.fig_x + A.fig_w; r[3] = A.fig_y + A.fig_h;
	}
	ld_clip_t was = ld_clip_push(p, r[0], r[1], r[2], r[3]);
	fig_render(p, A.fig_x, A.fig_y, A.fig_w, A.fig_h);
	pane_marks(p, A.fig_x, A.fig_y, A.fig_w, A.fig_h);
	ld_clip_pop(p, was);
	fig_drawn = LG.frames;
}

// the part of the page a figure's change draws (la_draw_part())
void lv_fig_part(ld_page_t *p) {
	ld_clip_t was = ld_clip_push(p, 0, HDR + 1, A.w, A.h - FOOT);
	fig_update(p, true);
	ld_clip_pop(p, was);
}

bool la_draw_figure(ld_page_t *p, int *x, int *y, int *w, int *h) {
	if (!la_animating()) return false;
	A.fig_need = true;
	// only what shows between the header and the footer
	int y0 = A.fig_y, y1 = A.fig_y + A.fig_h;
	if (y0 < HDR + 1) y0 = HDR + 1;
	if (y1 > A.h - FOOT) y1 = A.h - FOOT;
	if (y1 <= y0) { fig_run(); return false; }
	ld_clip_reset(p);
	ld_clip_t was = ld_clip_push(p, A.fig_x, y0, A.fig_x + A.fig_w, y1);
	fig_update(p, false);
	ld_clip_pop(p, was);
	*x = A.fig_x;
	*y = y0;
	*w = A.fig_w;
	*h = y1 - y0;
	return true;
}

// a control moved: the figure and its controls change, nothing else
static void fig_changed(void) {
	A.fig_need = true;
	if (A.fig_shown) {
		// the figure's part alone, if nothing else is waiting to be drawn
		// (the figure's own part, from the event before -- several moves
		// of the mouse come in one go -- is not something else)
		bool alone = (!A.part || A.fig_part) && !A.dirty;
		la_damage(A.fig_x - 1, A.fig_y - 1, A.fig_w + 2, A.fig_h + 2 + pane_extra());
		A.fig_part = alone;
	} else A.dirty = true;
}

// after the mouse or a key: drawn again only if a control changed, or
// another is the one Left and Right move
static bool fig_touched(uint32_t gen0, int sel0) {
	if (LG.ctl_gen == gen0 && A.fig_sel == sel0) return false;
	fig_changed();
	return true;
}

// a slider's value from the mouse's x
static void slide_to(int i, int mx) {
	const lg_ctl_t *c = &LG.ctl[i];
	double f = A.fig_track_w > 0 ? (double)(mx - A.fig_track_x) / A.fig_track_w : 0;
	if (f < 0) f = 0;
	if (f > 1) f = 1;
	lg_ctl_set(i, c->min + f * (c->max - c->min));
}

bool lv_mouse(int type, int x, int y) {
	if (A.screen != SC_LESSON || !A.fig || !A.fig_shown) return false;
	uint32_t gen0 = LG.ctl_gen;
	int sel0 = A.fig_sel;
	double ux = (x - A.fig_x) * (double)LG_W / A.fig_w, uy = (y - A.fig_y) * (double)LG_W / A.fig_w;
	if (type == LA_DOWN) {
		A.fig_drag = -1;
		if (x >= A.fig_x && x < A.fig_x + A.fig_w && y >= A.fig_y && y < A.fig_y + A.fig_h) {
			int d = lg_drag_at(ux, uy);
			if (d < 0) return false;
			A.fig_drag = d;
			lg_ctl_drag_to(d, ux, uy);
			return fig_touched(gen0, sel0);
		}
		for (int i = 0; i < LG.nctl; i++) {
			int ry = A.fig_ctl_y[i];
			if (ry < 0 || y < ry || y >= ry + CTL_H || x < A.fig_x || x >= A.fig_x + A.fig_w) continue;
			lg_ctl_t *c = &LG.ctl[i];
			A.fig_sel = i;
			if (c->kind == LG_SLIDER) {
				if (x >= A.fig_track_x - 8 && x <= A.fig_track_x + A.fig_track_w + 8) {
					slide_to(i, x);
					A.fig_drag = i;
				}
			} else if (c->kind == LG_TOGGLE) lg_ctl_nudge(i, 1);
			else if (c->kind == LG_CHOICE && c->nopt) {
				int cw = A.fig_track_w / c->nopt, k = (x - A.fig_track_x) / (cw ? cw : 1);
				if (k >= 0 && k < c->nopt) lg_ctl_set(i, k);
			}
			return fig_touched(gen0, sel0);
		}
		return false;
	}
	if (A.fig_drag < 0) return false;
	lg_ctl_t *c = &LG.ctl[A.fig_drag];
	if (c->kind == LG_DRAG) lg_ctl_drag_to(A.fig_drag, ux, uy);
	else if (c->kind == LG_SLIDER) slide_to(A.fig_drag, x);
	if (type == LA_UP) A.fig_drag = -1;
	return fig_touched(gen0, sel0);
}

// Left and Right, C, P: true if the figure took the key
static bool fig_key(uint32_t key) {
	if (!A.fig) return false;
	if ((key == 'p' || key == 'P') && LG.animated) {
		if (A.fig_paused) { A.fig_t0 = LP->ms() - (uint32_t)(A.fig_pause_t * 1000); A.fig_paused = false; }
		else { A.fig_pause_t = (LP->ms() - A.fig_t0) / 1000.0; A.fig_paused = true; }
		fig_drawn = 0;		// ("paused" comes or goes: all of it)
		fig_changed();
		return true;
	}
	if (!fig_has_controls()) return false;
	if (key == Z_KEY_LEFT || key == Z_KEY_RIGHT) {
		uint32_t gen0 = LG.ctl_gen;
		lg_ctl_nudge(A.fig_sel, key == Z_KEY_RIGHT ? 1 : -1);
		if (!fig_touched(gen0, A.fig_sel)) A.quiet = true;	// (at the end of its track: taken, nothing drawn)
		return true;
	}
	if (key == 'c' || key == 'C') {
		for (int k = 1; k <= LG.nctl; k++) {
			int i = (A.fig_sel + k) % LG.nctl;
			if (LG.ctl[i].used && LG.ctl[i].kind != LG_DRAG) { A.fig_sel = i; break; }
		}
		A.dirty = true;
		return true;
	}
	return false;
}

static void lc_forget(void);

// The questions of a step are answered in order: the keyboard goes to
// the first one not answered yet -- its box, if it is typed; the choice
// keys (1-6, Up/Down, Enter), if it is a choice. A typed box later in the
// step used to take the keyboard first, and the choice above it could
// then not be answered by keys at all.
static void give_keyboard(void) {
	A.typing = -1;
	for (int i = 0; i < A.nchecks; i++)
		if (!A.checks[i].answered) {
			if (A.checks[i].q.kind == LQ_NUMBER || A.checks[i].q.kind == LQ_FRACTION || A.checks[i].q.kind == LQ_TEXT)
				A.typing = i;
			return;
		}
}

void lv_enter(void) {
	lc_forget();
	A.scroll = 0;
	A.nchecks = 0;
	A.example_shown = 0;
	A.typing = -1;
	A.said[0] = 0;
	const lf_step_t *s = cur();
	for (int i = s->first; i < s->first + s->count; i++) {
		const lf_block_t *b = &A.les->blocks[i];
		if (b->kind != LB_DATA) continue;
		for (lx_node_t *f = b->forms->head; f; f = f->next) {
			if (!strcmp(b->name, "check")) add_check(f);
			else if (!strcmp(b->name, "try")) {
				lx_node_t *ch = lx_find(f, "check");
				if (ch && lx_nth(ch, 1)) add_check(lx_nth(ch, 1));
			} else if (!strcmp(b->name, "say") && lx_is_str(lx_nth(f, 1))) {
				LP->speak(lx_nth(f, 1)->text);
			}
		}
	}
	give_keyboard();
	fig_enter();
	A.dirty = true;
}

// -- laying out and drawing: each block returns the y after it --

typedef struct {
	ld_page_t	*p;			// NULL: measure only
	int			x, w;		// the column
	int			check;		// the next check's index
	int			view0, view1;	// the visible band, page y
	int			ex_before;	// steps of the step's earlier worked examples
} ctx_t;

static bool visible(const ctx_t *c, int y, int h) { return y + h > c->view0 && y < c->view1; }

// -- the step's layout, kept --
//
// Where each block of the step starts and ends, and the targets it
// makes, from the last time it was laid out whole; good until something
// that moves them changes (lc_key()). With it a part of the page -- a
// typed letter, a slider, the band a scroll uncovers -- lays out only
// the blocks that cross it, and the whole page only those in view.

#define LC_BLOCKS 160
#define LC_HOTS 96
typedef struct { int16_t x, y, w, h; uint8_t action; int16_t arg; } lc_hot_t;
static struct {
	bool		ok;
	uint32_t	key;
	int			height;				// layout_step's
	int			head;				// the title (or the review's heading) ends
	int16_t		y0[LC_BLOCKS], y1[LC_BLOCKS], chk[LC_BLOCKS], exb[LC_BLOCKS];	// from the top; the check and example steps after
	uint8_t		h0[LC_BLOCKS], hn[LC_BLOCKS];	// its targets in hot[]
	lc_hot_t	hot[LC_HOTS];
	int			nhot;
} LC;
static lc_hot_t lc_new[LC_HOTS];	// the targets of this drawing, in order
static int lc_nnew, lc_top;
static bool lc_rec;					// hot() records what it is given

static void lc_forget(void) { LC.ok = false; }

static uint32_t mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

static uint32_t lc_key(void) {
	uint32_t h = 2166136261u;
	h = mix(h, (uint32_t)(uintptr_t)A.les);
	h = mix(h, (uint32_t)A.step);
	h = mix(h, (uint32_t)A.w);
	h = mix(h, (uint32_t)A.h);
	h = mix(h, (uint32_t)A.example_shown);
	h = mix(h, A.review ? 1u + (uint32_t)A.rv_pos * 7u + (uint32_t)(A.rv_then + 1) * 131u : 0u);
	h = mix(h, (uint32_t)A.nchecks);
	for (int i = 0; i < A.nchecks; i++) {
		const la_check_t *k = &A.checks[i];
		h = mix(h, (uint32_t)k->answered | (uint32_t)k->judged << 1 | (uint32_t)k->hints_shown << 2 |
			(uint32_t)k->last.verdict << 8);
		h = mix(h, (uint32_t)(uintptr_t)k->last.feedback);
	}
	h = mix(h, A.fig ? (uint32_t)LG.nctl * 3u + (LG.err[0] ? 1u : 0u) : 0u);
	for (int i = 0; A.fig && i < LG.nctl; i++) h = mix(h, LG.ctl[i].used ? (uint32_t)LG.ctl[i].kind : 0u);
	return h;
}

static void hot(const ctx_t *c, int x, int y, int w, int h, int action, int arg) {
	if (lc_rec && c->p && lc_nnew < LC_HOTS)
		lc_new[lc_nnew++] = (lc_hot_t){ (int16_t)x, (int16_t)(y - lc_top), (int16_t)w, (int16_t)h, (uint8_t)action, (int16_t)arg };
	if (!c->p || !visible(c, y, h)) return;
	// only the part of it that shows
	int y0 = y < c->view0 ? c->view0 : y, y1 = y + h > c->view1 ? c->view1 : y + h;
	la_hot(x, y0, w, y1 - y0, action, arg);
}

static int words(ctx_t *c, int y, const char *text, const zc_font_t *f, const zc_font_t *fb,
	int size, int lead, int indent) {
	LT.font = f;
	LT.bold_font = fb;
	LT.size = size;
	LT.lead = lead;
	lt_layout(&LT, &A.scratch, text, c->w - indent);
	// drawn only where the page is being drawn (a part of it: la_draw_part)
	bool in_clip = c->p && ld_clip_hits(c->p, c->x + indent, y, LT.width + 4, LT.height + 4);
	if (c->p && visible(c, y, LT.height) && in_clip) {
		lt_draw(&LT, c->p, &A.scratch, c->x + indent, y, LD_INK);
		// links: their lines are hot
		if (LT.nlinks) {
			int top = y;
			for (int i = 0; i < LT.nlines; i++) {
				const lt_line_t *l = &LT.lines[i];
				for (int k = l->first; k < l->first + l->count; k++) {
					const lt_run_t *r = &LT.runs[k];
					if (r->link_id < 0) continue;
					int li = 0;
					while (li < nstep_links && strcmp(step_links[li], LT.links[r->link_id])) li++;
					if (li == nstep_links && nstep_links < 24) snprintf(step_links[nstep_links++], 96, "%s", LT.links[r->link_id]);
					if (li < 24) hot(c, c->x + indent + r->x, top, r->w, l->asc + l->desc, A_LINK, (int16_t)li);
				}
				top += l->asc + l->desc + LT.lead;
			}
		}
	}
	return y + LT.height;
}

static int body(ctx_t *c, int y, const char *text, int indent) {
	return words(c, y, text, &LDF.body, &LDF.body_b, 16, 3, indent);
}

static int small(ctx_t *c, int y, const char *text, int indent) {
	return words(c, y, text, &LDF.small, &LDF.small_b, 14, 2, indent);
}

static bool saying;		// the step's words are being gathered (measuring it)

static void say_add(const char *t) {
	if (!saying) return;
	int n = (int)strlen(A.said);
	if (n < (int)sizeof A.said - 2) {
		if (n) A.said[n++] = ' ';
		lt_plain(&A.scratch, t, A.said + n, (int)sizeof A.said - n);
	}
}

static int table(ctx_t *c, int y, const char *text) {
	// cells by |, a row a line; a row of only - : | marks the header above it
	char rows[12][8][64];
	int nr = 0, nc = 0, header = -1;
	const char *p = text;
	while (*p && nr < 12) {
		const char *e = strchr(p, '\n');
		int len = e ? (int)(e - p) : (int)strlen(p);
		bool sep = true;
		for (int i = 0; i < len; i++) if (!strchr("|-: ", p[i])) sep = false;
		if (sep) { header = nr - 1; }
		else {
			int col = 0;
			const char *q = p + (p[0] == '|');
			const char *end = p + len;
			while (q < end && col < 8) {
				const char *bar = memchr(q, '|', (size_t)(end - q));
				const char *ce = bar ? bar : end;
				while (q < ce && *q == ' ') q++;
				int cl = (int)(ce - q);
				while (cl && q[cl - 1] == ' ') cl--;
				if (cl > 63) cl = 63;
				memcpy(rows[nr][col], q, (size_t)cl);
				rows[nr][col][cl] = 0;
				col++;
				if (!bar) break;
				q = bar + 1;
				if (q >= end) break;
			}
			for (int k = col; k < 8; k++) rows[nr][k][0] = 0;
			if (col > nc) nc = col;
			nr++;
		}
		p = e ? e + 1 : p + len;
	}
	if (!nr || !nc) return y;
	ld_style_t s = ld_st(&LDF.small, 0), sb = ld_st(&LDF.small_b, 0);
	int cw[8] = { 0 }, total = 0;
	for (int r = 0; r < nr; r++)
		for (int k = 0; k < nc; k++) {
			int w = ld_widthz(r <= header ? &sb : &s, rows[r][k]) + 16;
			if (w > cw[k]) cw[k] = w;
		}
	for (int k = 0; k < nc; k++) total += cw[k];
	if (total > c->w) for (int k = 0; k < nc; k++) cw[k] = cw[k] * c->w / total;
	int rh = 18;
	if (c->p && visible(c, y, rh * nr)) {
		for (int r = 0; r < nr; r++) {
			int x = c->x;
			for (int k = 0; k < nc; k++) {
				ld_textz(c->p, r <= header ? &sb : &s, x + 4, y + r * rh + 13, rows[r][k], LD_INK);
				x += cw[k];
			}
			if (r == header) ld_hline(c->p, c->x, c->x + (total < c->w ? total : c->w), y + r * rh + rh - 1, LD_INK);
		}
	}
	return y + nr * rh;
}

static int boxed(ctx_t *c, int y, const char *title, const char *text, int lw) {
	// a framed aside: note (thin), warn (heavy, with its title)
	ld_style_t tb = ld_st(&LDF.small_b, 0);
	int pad = 8, th = title ? 18 : 0;
	ctx_t m = *c;
	m.p = NULL;
	m.x = c->x + pad;
	m.w = c->w - 2 * pad;
	int h = small(&m, 0, text, 0) + 2 * pad + th;
	if (c->p && visible(c, y, h)) {
		ld_rframe(c->p, c->x, y, c->w, h, 4, lw, LD_INK);
		if (title) {
			char t[100];
			snprintf(t, sizeof t, "! %s", title);
			ld_textz(c->p, &tb, c->x + pad, y + pad + 11, t, LD_INK);
		}
		ctx_t d = *c;
		d.x = c->x + pad;
		d.w = c->w - 2 * pad;
		small(&d, y + pad + th, text, 0);
	}
	return y + h;
}

// -- a question --

static int question(ctx_t *c, int y, int ci) {
	if (ci >= A.nchecks) return y;
	la_check_t *k = &A.checks[ci];
	const lq_q_t *q = &k->q;
	ld_style_t f5 = ld_st(&LDF.f5, 0), s6 = ld_st(&LDF.f6, 0), bb = ld_st(&LDF.body_b, 0);
	if (c->p && visible(c, y, 10)) ld_textz(c->p, &f5, c->x, y + 8, "CHECK", LD_INK);
	y += 14;
	// the question: its strings and formulas as one line of text
	char ask[600];
	int o = 0;
	for (int i = 0; i < q->nask && o < (int)sizeof ask - 4; i++) {
		const char *t = q->ask[i].text;
		bool punct = !q->ask[i].math && t[0] && strchr(".,;:?!)", t[0]);
		o += snprintf(ask + o, sizeof ask - (size_t)o, q->ask[i].math ? "%s$%s$" : "%s%s", o && !punct ? " " : "", t);
	}
	ask[o] = 0;
	y = words(c, y, ask, &LDF.body_b, &LDF.body_b, 17, 3, 0) + 8;
	say_add(ask);

	if (q->kind == LQ_CHOICE) {
		for (int i = 0; i < q->nopt; i++) {
			const lq_opt_t *op = &q->opt[q->order[i]];
			bool chosen = (k->chosen >> i) & 1;
			bool show_right = k->answered && op->right;
			bool wrong = chosen && k->judged && !op->right;
			char key[12];
			snprintf(key, sizeof key, "%d", i + 1);
			ctx_t m = *c;
			m.p = NULL;
			int th = words(&m, 0, op->text, &LDF.body, &LDF.body_b, 16, 2, 28) ;
			int h = th + 12 < 26 ? 26 : th + 12;
			if (c->p && visible(c, y - 3, h + 6)) {		// (with the focus ring round it)
				int w = c->w;
				if (show_right || (chosen && !k->judged)) ld_rrect(c->p, c->x, y, w, h, 4, LD_INK);
				else ld_rframe(c->p, c->x, y, w, h, 4, wrong ? 2 : 1, LD_INK);
				int fg = show_right || (chosen && !k->judged) ? LD_DARK : LD_INK;
				ld_keycap(c->p, c->x + 8, y + h / 2 + 4, key, fg);
				LT.font = &LDF.body; LT.bold_font = &LDF.body_b; LT.size = 16; LT.lead = 2;
				lt_layout(&LT, &A.scratch, op->text, w - 28 - 30);
				lt_draw(&LT, c->p, &A.scratch, c->x + 28, y + (h - LT.height) / 2, fg);
				if (wrong) ld_cross(c->p, c->x + w - 20, y + h / 2 - 5, LD_INK);
				if (show_right) ld_tick(c->p, c->x + w - 20, y + h / 2 - 5, LD_DARK);
				if (k->focus == i && A.typing < 0 && !k->answered) ld_rframe(c->p, c->x - 3, y - 3, w + 6, h + 6, 6, 1, LD_INK);
				if (!k->answered) hot(c, c->x, y, w, h, A_CHOICE, (int16_t)(ci * 16 + i));
			}
			y += h + 6;
		}
		if (q->several && !k->answered) {
			if (c->p && visible(c, y, 24)) {
				int idx = A.nhot;
				hot(c, c->x, y, 120, 22, A_CHECK, ci);
				ld_button(c->p, c->x, y, 120, 22, "Check", true, A.focus == idx);
				ld_textz(c->p, &s6, c->x + 132, y + 15, "(choose every right answer)", LD_INK);
			}
			y += 30;
		}
	} else if (q->kind == LQ_NUMBER || q->kind == LQ_FRACTION || q->kind == LQ_TEXT) {
		ld_style_t fs = ld_st(&LDF.big, 0);
		int fw = c->w < 260 ? c->w : 260, fh = 30;
		// where the field is, from the step's top: it may be drawn again
		// after a scroll without its block being laid out (the kept layout)
		k->fx = (int16_t)c->x; k->fy = (int16_t)(y - lc_top); k->fw = (int16_t)fw; k->fh = (int16_t)fh;
		if (c->p && visible(c, y, fh)) {
			bool active = A.typing == ci && !k->answered;
			ld_frame(c->p, c->x, y, fw, fh, active ? 2 : 1, LD_INK);
			int tx = ld_textz(c->p, &fs, c->x + 8, y + 21, k->typed, LD_INK);
			if (active) {
				char pre[64];
				memcpy(pre, k->typed, (size_t)k->cursor);
				pre[k->cursor] = 0;
				int cx = c->x + 8 + ld_widthz(&fs, pre);
				ld_fill(c->p, cx, y + 5, 2, fh - 10, LD_INK);
			}
			(void)tx;
			if (q->unit) ld_textz(c->p, &bb, c->x + fw + 8, y + 20, q->unit, LD_INK);
			if (k->answered && k->last.verdict == LQ_RIGHT) ld_tick(c->p, c->x + fw + (q->unit ? 40 : 10), y + 10, LD_INK);
			if (!k->answered) hot(c, c->x, y, fw, fh, A_FIELD, ci);
		}
		y += fh + 6;
		if (!k->answered && c->p && visible(c, y, 22)) {
			int idx = A.nhot;
			hot(c, c->x, y, 100, 22, A_CHECK, ci);
			ld_button(c->p, c->x, y, 100, 22, "Check", true, A.focus == idx);
			ld_textz(c->p, &s6, c->x + 112, y + 15,
				q->kind == LQ_FRACTION ? "type a fraction, like 3/4" : q->kind == LQ_NUMBER ? "type a number" : "type the answer", LD_INK);
		}
		if (!k->answered) y += 28;
	} else {
		y = small(c, y, "This kind of question needs a newer learn: it is not counted.", 0) + 6;
	}

	// hints so far
	for (int i = 0; i < k->hints_shown && i < q->nhints; i++) {
		char h[300];
		snprintf(h, sizeof h, "**Hint %d.** %s", i + 1, q->hints[i]);
		y = small(c, y, h, 0) + 4;
	}
	// the verdict
	if (k->judged) {
		char fb[700];
		const char *head = k->last.verdict == LQ_RIGHT ? "**Right.**" :
			k->last.verdict == LQ_NEARLY ? "**Nearly.**" : "**Not quite.**";
		if (k->answered && k->last.verdict != LQ_RIGHT) {
			char ans[200];
			lq_answer_text(q, ans, sizeof ans);
			snprintf(fb, sizeof fb, "%s %s The answer: %s", head, k->last.feedback ? k->last.feedback : "", ans);
		} else if (k->last.verdict != LQ_RIGHT && !k->answered) {
			snprintf(fb, sizeof fb, "%s %s %s", head, k->last.feedback ? k->last.feedback : "",
				q->kind == LQ_CHOICE ? "Try again." : "Change your answer and check again.");
		} else snprintf(fb, sizeof fb, "%s %s", head, k->last.feedback ? k->last.feedback : "");
		y = boxed(c, y + 2, NULL, fb, 1) + 4;
	}
	if (!k->answered && q->nhints > k->hints_shown) {
		// (its room is taken whether or not it shows: the layout must not
		// depend on what is in view, or the measured step is not the drawn one)
		if (c->p && visible(c, y, 14)) {
			ld_textz(c->p, &s6, c->x, y + 11, "F5  a hint", LD_INK);
			hot(c, c->x, y, 84, 14, A_HINT, ci);
		}
		y += 16;
	}
	return y;
}

// a worked example: the problem, the steps shown so far, then the answer
static int example(ctx_t *c, int y, const lx_node_t *f) {
	const char *prob = lx_str_of(f, "problem");
	ld_style_t f5 = ld_st(&LDF.f5, 0);
	if (c->p && visible(c, y, 10)) ld_textz(c->p, &f5, c->x, y + 8, "WORKED EXAMPLE", LD_INK);
	y += 14;
	if (prob) { char t[400]; snprintf(t, sizeof t, "**%s**", prob); y = body(c, y, t, 0) + 6; say_add(prob); }
	int nsteps = lx_count(f, "step");
	// A step's worked examples are shown one after another: Space (or the
	// button) reveals the first one's steps, then the next one's. One
	// count for the step; this example has what is past the ones before.
	int before = c->ex_before;
	c->ex_before += nsteps;
	int shown = A.example_shown - before;
	if (shown < 0) shown = 0;
	if (shown > nsteps) shown = nsteps;
	for (int i = 0; i < nsteps && i < shown; i++) {
		lx_node_t *s = lx_find_nth(f, "step", i);
		lx_node_t *in = lx_nth(s, 1);
		char num[16];
		snprintf(num, sizeof num, "%d.", i + 1);
		if (c->p && visible(c, y, 14)) { ld_style_t sb = ld_st(&LDF.body_b, 0); ld_textz(c->p, &sb, c->x, y + 12, num, LD_INK); }
		if (in && in->kind == LX_STR) { y = body(c, y, in->text, 24) + 4; say_add(in->text); }
	}
	if (shown < nsteps) {
		if (A.example_shown < before) return y;		// a later one: its turn comes
		if (c->p && visible(c, y, 22)) {
			int idx = A.nhot;
			hot(c, c->x, y, 150, 22, A_EXAMPLE, 0);
			ld_button(c->p, c->x, y, 150, 22, "Show the next step", false, A.focus == idx);
		}
		y += 28;
	} else {
		const char *ans = lx_str_of(f, "answer");
		if (ans) { char t[300]; snprintf(t, sizeof t, "**So:** %s", ans); y = body(c, y, t, 0) + 4; }
	}
	return y;
}

static int data_block(ctx_t *c, int y, const lf_block_t *b) {
	ld_style_t s6 = ld_st(&LDF.f6, 0), bb = ld_st(&LDF.body_b, 0);
	for (lx_node_t *f = b->forms->head; f; f = f->next) {
		const char *n = b->name;
		if (!strcmp(n, "lesson")) return y - GAP;		// nothing to show
		if (!strcmp(n, "check")) { y = question(c, y, c->check++) + GAP; continue; }
		if (!strcmp(n, "note") && lx_is_str(lx_nth(f, 1))) { y = boxed(c, y, NULL, lx_nth(f, 1)->text, 1) + GAP; say_add(lx_nth(f, 1)->text); continue; }
		if (!strcmp(n, "warn") && lx_is_str(lx_nth(f, 1)) && lx_is_str(lx_nth(f, 2))) {
			y = boxed(c, y, lx_nth(f, 1)->text, lx_nth(f, 2)->text, 2) + GAP;
			say_add(lx_nth(f, 1)->text);
			say_add(lx_nth(f, 2)->text);
			continue;
		}
		if (!strcmp(n, "say") && lx_is_str(lx_nth(f, 1))) {
			if (c->p && visible(c, y, 14)) { ld_speaker(c->p, c->x, y + 1, LD_INK); hot(c, c->x, y, 16, 16, A_SAY, 0); }
			y = body(c, y, lx_nth(f, 1)->text, 22) + GAP;
			say_add(lx_nth(f, 1)->text);
			continue;
		}
		if (!strcmp(n, "sources") && lx_is_str(lx_nth(f, 1))) {
			char t[300];
			snprintf(t, sizeof t, "Source: %s", lx_nth(f, 1)->text);
			ld_style_t st = s6;
			(void)st;
			y = words(c, y, t, &LDF.f6, &LDF.f6, 12, 1, 0) + 2;
			continue;
		}
		if (!strcmp(n, "example") && lx_is_form(f, "example")) { y = example(c, y, f) + GAP; continue; }
		if (!strcmp(n, "cards") && lx_is_form(f, "card")) {
			if (f == b->forms->head) {
				if (c->p && visible(c, y, 12)) { ld_style_t f5 = ld_st(&LDF.f5, 0); ld_textz(c->p, &f5, c->x, y + 8, "TO REMEMBER", LD_INK); }
				y += 14;
			}
			char t[300];
			snprintf(t, sizeof t, "**%s** -- %s", lx_nth(f, 1) && lx_nth(f, 1)->text ? lx_nth(f, 1)->text : "",
				lx_nth(f, 2) && lx_nth(f, 2)->text ? lx_nth(f, 2)->text : "");
			y = body(c, y, t, 0) + 4;
			if (!f->next) y += GAP;
			continue;
		}
		if (!strcmp(n, "try") && lx_is_form(f, "try")) {
			const char *app = lx_str_of(f, "app"), *task = lx_str_of(f, "task");
			if (c->p && visible(c, y, 14)) ld_textz(c->p, &bb, c->x, y + 12, "Try it", LD_INK);
			y += 18;
			if (task) { y = body(c, y, task, 0) + 6; say_add(task); }
			if (app) {
				char lbl[40];
				snprintf(lbl, sizeof lbl, "Open %s", app);
				if (c->p && visible(c, y, 22)) {
					int idx = A.nhot;
					hot(c, c->x, y, 130, 22, A_TRY, 0);
					ld_button(c->p, c->x, y, 130, 22, lbl, false, A.focus == idx);
				}
				y += 30;
			}
			if (lx_find(f, "check")) y = question(c, y, c->check++) + GAP;
			continue;
		}
	}
	return y;
}

static int block(ctx_t *c, int y, const lf_block_t *b) {
	switch (b->kind) {
	case LB_PARA: say_add(b->text); return body(c, y, b->text, 0) + GAP;
	case LB_HEAD:
		say_add(b->text);
		if (b->level == 2) return words(c, y, b->text, &LDF.serif_b, &LDF.serif_b, 21, 2, 0) + GAP + 4;
		return words(c, y, b->text, &LDF.body_b, &LDF.body_b, 16, 2, 0) + GAP / 2;
	case LB_ITEM: {
		int ind = 6 + 16 * b->level;
		if (c->p && visible(c, y, 14)) {
			if (b->num) {
				char n[8];
				snprintf(n, sizeof n, "%d.", b->num);
				ld_style_t st = ld_st(&LDF.body, 0);
				ld_text_r(c->p, &st, c->x + ind - 5, y + 12, n, LD_INK);
			} else ld_disc(c->p, c->x + ind - 9, y + 8, 2.5, LD_INK);
		}
		say_add(b->text);
		int ny = body(c, y, b->text, ind) + 4;
		// the last item of a list: a gap after it
		const lf_block_t *next = b + 1;
		bool last = next >= A.les->blocks + A.les->nblocks || next->kind != LB_ITEM;
		return ny + (last ? GAP - 4 : 0);
	}
	case LB_QUOTE: {
		ctx_t m = *c;
		m.p = NULL;
		int h = words(&m, 0, b->text, &LDF.body, &LDF.body_b, 16, 3, 14);
		if (c->p && visible(c, y, h)) ld_fill(c->p, c->x + 3, y, 3, h, LD_INK);
		say_add(b->text);
		return words(c, y, b->text, &LDF.body, &LDF.body_b, 16, 3, 14) + GAP;
	}
	case LB_TABLE: return table(c, y, b->text) + GAP;
	case LB_MATH: {
		lm_box_t bx = lm_draw_str(NULL, &A.scratch, b->text, (int)strlen(b->text), 22, 0, 0, LD_INK, true);
		int h = bx.asc + bx.desc;
		if (c->p && visible(c, y, h))
			lm_draw_str(c->p, &A.scratch, b->text, (int)strlen(b->text), 22, c->x + (c->w - bx.w) / 2, y + bx.asc, LD_INK, false);
		say_add(b->text);
		return y + h + GAP;
	}
	case LB_CODE: {
		ld_style_t st = ld_st(&LDF.mono, 0);
		int lines = 1;
		for (const char *q = b->text; *q; q++) if (*q == '\n') lines++;
		if (b->text[0] && b->text[strlen(b->text) - 1] == '\n') lines--;
		int h = lines * 14 + 10;
		if (c->p && visible(c, y, h)) {
			ld_frame(c->p, c->x, y, c->w, h, 1, LD_INK);
			const char *q = b->text;
			for (int i = 0; i < lines; i++) {
				const char *e = strchr(q, '\n');
				int len = e ? (int)(e - q) : (int)strlen(q);
				ld_text(c->p, &st, c->x + 6, y + 15 + i * 14, q, len, LD_INK);
				q = e ? e + 1 : q + len;
			}
		}
		return y + h + GAP;
	}
	case LB_IMAGE: {
		int h = 80;
		if (c->p && visible(c, y, h)) {
			ld_frame(c->p, c->x, y, c->w, h, 1, LD_INK);
			ld_style_t st = ld_st(&LDF.small, 0);
			char t[120];
			snprintf(t, sizeof t, "[picture: %s]", b->text);
			ld_text_c(c->p, &st, c->x + c->w / 2, y + h / 2 + 4, t, LD_INK);
		}
		return y + h + GAP;
	}
	case LB_DATA: return data_block(c, y, b);
	case LB_FIGURE: return y;		// in the pane
	}
	return y;
}

static bool has_figure(void) {
	const lf_step_t *s = cur();
	for (int i = s->first; i < s->first + s->count; i++)
		if (A.les->blocks[i].kind == LB_FIGURE) return true;
	return false;
}

// the whole step, from page y `top`; returns its height
static int layout_step(ld_page_t *p, int top, bool *pane_beside) {
	ctx_t c;
	memset(&c, 0, sizeof c);
	c.p = p;
	c.view0 = HDR + 1;
	c.view1 = A.h - FOOT;
	bool fig = has_figure();
	int pw = PANE_W, ph = PANE_H;
	bool beside = fig && A.w >= 2 * 24 + 260 + pw;
	*pane_beside = beside;
	int y = top;
	if (fig && !beside) {
		if (A.w - 48 < pw) { pw = A.w - 48; ph = pw * 3 / 4; }
		if (p) draw_pane(p, (A.w - pw) / 2, y, pw, ph);
		y += ph + pane_extra() + 14;
	}
	c.x = 24;
	c.w = beside ? A.w - 24 - pw - 24 - 16 : A.w - 48;
	if (!beside && c.w > 640) c.w = 640;
	const lf_step_t *s = cur();
	// with the layout kept, a block outside what is being drawn is passed over
	bool kept = p && LC.ok;
	int cy0 = p ? p->cy0 - 8 : 0, cy1 = p ? p->cy1 + 8 : 0;		// (focus rings stand out of their blocks)
	lc_top = top;
	lc_nnew = 0;
	if (kept && (top + LC.head <= cy0 || y >= cy1)) y = top + LC.head;
	else if (A.review) {
		// a question again: what it asks (and the step's figure), not the teaching
		y = words(&c, y, A.rv_then >= 0 ? "Warm-up" : "Review", &LDF.serif_b, &LDF.serif_b, 24, 2, 0) + 4;
		char t[180];
		snprintf(t, sizeof t, "From the lesson \"%s\"", A.rv_from);
		y = words(&c, y, t, &LDF.small, &LDF.small_b, 13, 1, 0) + 12;
	} else if (A.step == 0 && A.les->title) {
		// the first step opens with the lesson's title
		y = words(&c, y, A.les->title, &LDF.serif_b, &LDF.serif_b, 24, 2, 0) + 14;
		say_add(A.les->title);
	}
	if (!p) LC.head = y - top;
	int nb = A.review ? 1 : s->count;
	if (nb > LC_BLOCKS) nb = LC_BLOCKS;
	for (int bi = 0; bi < nb; bi++) {
		if (kept && (top + LC.y1[bi] <= cy0 || top + LC.y0[bi] >= cy1)) {
			// not drawn: where it was, and its targets as they were
			for (int k = LC.h0[bi]; k < LC.h0[bi] + LC.hn[bi]; k++) {
				const lc_hot_t *t = &LC.hot[k];
				lc_rec = false;
				hot(&c, t->x, top + t->y, t->w, t->h, t->action, t->arg);
				if (lc_nnew < LC_HOTS) lc_new[lc_nnew++] = *t;
			}
			y = top + LC.y1[bi];
			c.check = LC.chk[bi];
			c.ex_before = LC.exb[bi];
			continue;
		}
		int n0 = lc_nnew;
		lc_rec = p != NULL;
		int y0 = y;
		y = A.review ? question(&c, y, 0) + GAP : block(&c, y, &A.les->blocks[s->first + bi]);
		lc_rec = false;
		if (!p) {
			LC.y0[bi] = (int16_t)(y0 - top);
			LC.y1[bi] = (int16_t)(y - top);
			LC.chk[bi] = (int16_t)c.check;
			LC.exb[bi] = (int16_t)c.ex_before;
		}
		if (p) { LC.h0[bi] = (uint8_t)n0; LC.hn[bi] = (uint8_t)(lc_nnew - n0); }
	}
	if (p) {
		// the targets as now drawn, for the next time
		memcpy(LC.hot, lc_new, (size_t)lc_nnew * sizeof lc_new[0]);
		LC.nhot = lc_nnew;
	}
	if (beside) {
		int py = top + 2;
		if (p) draw_pane(p, A.w - 24 - pw, py, pw, ph);
		if (py + ph + pane_extra() > y) y = py + ph + pane_extra();
	}
	return y - top;
}

void lv_draw(ld_page_t *p) {
	if (A.review && A.rv_done) { rv_done_draw(p); return; }
	fig_run();
	A.fig_shown = false;
	nstep_links = 0;
	if (A.review) la_frame(p, A.crumb, A.rv_pos, A.rv_n);
	else la_frame(p, A.crumb, A.step, A.les->nsteps);
	bool beside;
	int view_h = A.h - HDR - FOOT - 1;
	// measure (unless the layout is kept), clamp the scroll, then draw clipped to the view
	uint32_t key = lc_key();
	if (!LC.ok || LC.key != key) {
		LC.ok = false;
		A.said[0] = 0;
		saying = true;
		LC.height = layout_step(NULL, 0, &beside);
		saying = false;
		LC.key = key;
	}
	A.step_height = LC.height + 16;
	int max = A.step_height - view_h;
	if (A.scroll > max) A.scroll = max;
	if (A.scroll < 0) A.scroll = 0;
	ld_clip_t was = ld_clip_push(p, 0, HDR + 1, A.w, A.h - FOOT);
	layout_step(p, HDR + 12 - A.scroll, &beside);
	LC.ok = true;		// (its targets are known now too)
	ld_clip_pop(p, was);
	scrollbar(p);		// where the view is in the step
	bool last = A.review ? A.rv_pos == A.rv_n - 1 : A.step == A.les->nsteps - 1;
	const char *btn = !lv_ready() ? NULL : (A.scroll < max ? "More  v" : last ? "Finish" : "Next  >");
	const char *keys = A.review ? (A.rv_then >= 0 ? "1-6|choose;Enter|check;F5|hint;F8|skip;Esc|skip to the lesson" :
		"1-6|choose;Enter|check;F5|hint;F8|skip;F7|mistake;Esc|stop") :
		A.nchecks && A.typing >= 0 && !A.checks[A.typing].answered ? "Enter|check;F5|hint;F8|skip;F6|say;F7|mistake;Esc|leave" :
		A.nchecks ? "1-6|choose;Enter|check;F5|hint;F8|skip;F6|say;F7|mistake;Esc|leave" :
		"Space|next;Bksp|back;F6|say;F7|mistake;Esc|leave";
	if (fig_has_controls() && A.nchecks && A.typing >= 0)
		keys = "Left/Right|change;Enter|check;F5|hint;F8|skip;F6|say;Esc|leave";
	else if (fig_has_controls() && !A.nchecks)
		keys = fig_rows() > 1 ? "Left/Right|change;C|next control;Space|next;F6|say;Esc|leave" :
			LG.animated ? "Left/Right|change;P|pause;Space|next;F6|say;Esc|leave" :
			"Left/Right|change;Space|next;Bksp|back;F6|say;Esc|leave";
	else if (A.fig && LG.animated && !A.nchecks) keys = "P|pause;Space|next;Bksp|back;F6|say;F7|mistake;Esc|leave";
	la_footer(p, keys, btn, A_NEXT);
}

// -- going on --

bool lv_ready(void) {
	for (int i = 0; i < A.nchecks; i++)
		if (!A.checks[i].answered && A.checks[i].q.kind <= LQ_TEXT) return false;
	return true;
}

static void finish_lesson(void) {
	uint32_t secs = (LP->ms() - A.started_ms) / 1000;
	if (!ls_done(A.lesson_id, A.right, A.total, secs))
		snprintf(A.status, sizeof A.status, "Your progress could not be saved: is the card or stick full?");
	la_go(SC_DONE);
}

static void next_step(void) {
	int view_h = A.h - HDR - FOOT - 1;
	if (A.scroll + view_h < A.step_height) {	// more of this step below: show it first
		A.scroll += view_h - 40;
		A.dirty = true;
		return;
	}
	if (!lv_ready()) {
		snprintf(A.status, sizeof A.status, "Answer the question first (F5 for a hint, F8 to skip it).");
		A.dirty = true;
		return;
	}
	if (A.review) { rv_next(); return; }
	if (A.step + 1 >= A.les->nsteps) { finish_lesson(); return; }
	A.step++;
	lv_enter();
}

// F8: on to the next step, the questions of this one passed over. They
// count in the lesson's total (not right) and do not go into review: the
// learner said they do not care about them.
static void skip(void) {
	for (int i = 0; i < A.nchecks; i++) {
		la_check_t *k = &A.checks[i];
		if (k->answered) continue;
		k->answered = true;
		k->skipped = true;
		if (!A.review) A.total++;
	}
	A.typing = -1;
	A.dirty = true;
	if (A.review) { rv_next(); return; }
	if (A.step + 1 >= A.les->nsteps) { finish_lesson(); return; }
	A.step++;
	lv_enter();
}

static void judged(la_check_t *k, lq_result_t r) {
	if (r.verdict == LQ_UNREADABLE) {
		snprintf(A.status, sizeof A.status, "%s", r.feedback ? r.feedback : "Choose an answer.");
		return;
	}
	k->last = r;
	k->judged = true;
	k->attempts++;
	if (r.verdict == LQ_RIGHT) {
		if (k->attempts == 1 && !k->hints_shown) { k->right_first = true; A.right++; }
		A.total++;
		k->answered = true;
	} else if (k->attempts >= 2 || (k->q.kind == LQ_CHOICE && k->q.nopt <= 2)) {
		A.total++;
		k->answered = true;		// shown the answer
	}
	if (k->answered) rv_record(k);
	if (k->answered) give_keyboard();
	LP->speak(r.verdict == LQ_RIGHT ? "Right." : r.verdict == LQ_NEARLY ? "Nearly." : "Not quite.");
}

static void choose(int ci, int opt) {
	if (ci >= A.nchecks) return;
	la_check_t *k = &A.checks[ci];
	if (k->answered || opt >= k->q.nopt) return;
	if (k->q.several) { k->chosen ^= 1u << opt; k->judged = false; return; }
	// a wrong answer tried already stays crossed out
	if (k->judged && ((k->chosen >> opt) & 1)) return;
	k->chosen = 1u << opt;
	judged(k, lq_judge_choice(&k->q, k->chosen));
}

// After a check, what changes is the question's verdict and what follows
// it in the step (it may move down), and the footer: from the question
// to the bottom of the window. The words above it and the figure stay.
static void check_damage(const la_check_t *k) {
	int y = k->fw ? HDR + 12 - A.scroll + k->fy - 4 : HDR + 1;
	if (y < HDR + 1) y = HDR + 1;
	la_damage(0, y, A.w, A.h - y);
	A.keep_fig = true;
}

static void check_typed(int ci) {
	la_check_t *k = &A.checks[ci];
	if (k->answered) return;
	check_damage(k);
	if (k->q.kind == LQ_CHOICE) { judged(k, lq_judge_choice(&k->q, k->chosen)); return; }
	judged(k, lq_judge_typed(&k->q, k->typed));
}

static void speak_step(void) {
	if (A.said[0]) LP->speak(A.said);
}

static void report(void) {
	char words[200];
	if (!LP->prompt("Report a mistake", "What is wrong in this step? It is kept with your progress, to send.", "", words, sizeof words))
		return;
	if (!words[0]) return;
	if (ls_report(A.course->m.id, A.course->m.version, A.lesson_id, A.step, words))
		snprintf(A.status, sizeof A.status, "Kept in reports.txt with your progress. Thank you.");
	else snprintf(A.status, sizeof A.status, "The report could not be saved.");
}

static void leave(void) {
	if (A.review) { rv_leave(); return; }
	if (A.step > 0) ls_at(A.lesson_id, A.step);
	la_go(SC_COURSE);
}

// the first question of the step that is not answered yet
static int open_check(void) {
	for (int i = 0; i < A.nchecks; i++) if (!A.checks[i].answered) return i;
	return -1;
}

static bool type_key(la_check_t *k, uint32_t key) {
	int len = (int)strlen(k->typed);
	if (key == K_BS) {
		if (k->cursor > 0) {
			int from = k->cursor - 1;
			while (from > 0 && ((unsigned char)k->typed[from] & 0xC0) == 0x80) from--;
			memmove(k->typed + from, k->typed + k->cursor, (size_t)(len - k->cursor + 1));
			k->cursor = from;
		}
		k->judged = false;
		return true;
	}
	if (key == Z_KEY_LEFT) { while (k->cursor > 0) { k->cursor--; if (((unsigned char)k->typed[k->cursor] & 0xC0) != 0x80) break; } return true; }
	if (key == Z_KEY_RIGHT) { while (k->cursor < len) { k->cursor++; if (((unsigned char)k->typed[k->cursor] & 0xC0) != 0x80) break; } return true; }
	if (key == Z_KEY_HOME) { k->cursor = 0; return true; }
	if (key == Z_KEY_END) { k->cursor = len; return true; }
	if (key >= 0x20 && key < 0x110000 && key != 0x7f) {
		char u[5];
		int n = 0;
		if (key < 0x80) u[n++] = (char)key;
		else if (key < 0x800) { u[n++] = (char)(0xC0 | (key >> 6)); u[n++] = (char)(0x80 | (key & 0x3F)); }
		else if (key < 0x10000) { u[n++] = (char)(0xE0 | (key >> 12)); u[n++] = (char)(0x80 | ((key >> 6) & 0x3F)); u[n++] = (char)(0x80 | (key & 0x3F)); }
		else { u[n++] = (char)(0xF0 | (key >> 18)); u[n++] = (char)(0x80 | ((key >> 12) & 0x3F)); u[n++] = (char)(0x80 | ((key >> 6) & 0x3F)); u[n++] = (char)(0x80 | (key & 0x3F)); }
		if (len + n >= (int)sizeof k->typed) return true;
		memmove(k->typed + k->cursor + n, k->typed + k->cursor, (size_t)(len - k->cursor + 1));
		memcpy(k->typed + k->cursor, u, (size_t)n);
		k->cursor += n;
		k->judged = false;
		return true;
	}
	return false;
}

bool lv_key(uint32_t key, uint8_t mods) {
	(void)mods;
	if (A.review && A.rv_done) return rv_done_key(key);
	// F5-F7 work everywhere in a lesson, a typed answer's box too, where
	// H, S and M are letters of the answer
	if (key == Z_KEY_F5) {
		int hc = A.typing >= 0 && A.typing < A.nchecks && !A.checks[A.typing].answered ? A.typing : open_check();
		if (hc >= 0 && A.checks[hc].hints_shown < A.checks[hc].q.nhints) A.checks[hc].hints_shown++;
		return true;
	}
	if (key == Z_KEY_F8) { skip(); return true; }
	if (key == Z_KEY_F6) { speak_step(); return true; }
	if (key == Z_KEY_F7) { report(); return true; }
	// a typed answer has the keyboard: letters are the answer, not commands
	if (A.typing >= 0 && A.typing < A.nchecks && !A.checks[A.typing].answered) {
		la_check_t *k = &A.checks[A.typing];
		if (key == K_ENTER) { check_typed(A.typing); return true; }
		if (key == K_ESC) { leave(); return true; }
		// an empty answer: Left and Right are the figure's (its slider)
		if ((key == Z_KEY_LEFT || key == Z_KEY_RIGHT) && !k->typed[0] && fig_key(key)) return true;
		if (key == Z_KEY_UP || key == Z_KEY_DOWN || key == Z_KEY_PAGEUP || key == Z_KEY_PAGEDOWN) {
			return lv_wheel(key == Z_KEY_UP ? 1 : key == Z_KEY_DOWN ? -1 : key == Z_KEY_PAGEUP ? 6 : -6);
		}
		bool shown = k->judged;		// a verdict showing goes when the answer changes: the step moves
		if (type_key(k, key)) {
			// only the field changed: draw it alone, at once
			if (!shown && k->fw) la_damage(k->fx, HDR + 12 - A.scroll + k->fy, k->fw + 1, k->fh + 1);
			return true;
		}
		return false;
	}
	if (fig_key(key)) return true;
	int oc = open_check();
	if (key >= '1' && key <= '9' && oc >= 0 && A.checks[oc].q.kind == LQ_CHOICE) {
		choose(oc, (int)(key - '1'));
		return true;
	}
	if ((key == Z_KEY_UP || key == Z_KEY_DOWN) && oc >= 0 && A.checks[oc].q.kind == LQ_CHOICE) {
		la_check_t *k = &A.checks[oc];
		k->focus += key == Z_KEY_DOWN ? 1 : -1;
		if (k->focus < 0) k->focus = 0;
		if (k->focus >= k->q.nopt) k->focus = k->q.nopt - 1;
		return true;
	}
	if (key == Z_KEY_UP || key == Z_KEY_DOWN || key == Z_KEY_PAGEUP || key == Z_KEY_PAGEDOWN) {
		return lv_wheel(key == Z_KEY_UP ? 1 : key == Z_KEY_DOWN ? -1 : key == Z_KEY_PAGEUP ? 6 : -6);
	}
	if (key == K_ENTER && oc >= 0 && A.checks[oc].q.kind == LQ_CHOICE) {
		la_check_t *k = &A.checks[oc];
		if (k->q.several) check_typed(oc);
		else choose(oc, k->focus);
		return true;
	}
	if (key == ' ' || key == K_ENTER) {
		// the step's examples' steps come before going on, one example after another
		const lf_step_t *s = cur();
		int total = 0;
		for (int i = s->first; i < s->first + s->count; i++) {
			const lf_block_t *b = &A.les->blocks[i];
			if (b->kind != LB_DATA || strcmp(b->name, "example")) continue;
			for (lx_node_t *f = b->forms->head; f; f = f->next)
				if (lx_is_form(f, "example")) total += lx_count(f, "step");
		}
		if (A.example_shown < total) {
			A.example_shown++;
			return true;
		}
		next_step();
		return true;
	}
	if (key == K_BS) {
		if (A.step > 0 && !A.review) { A.step--; lv_enter(); }
		return true;
	}
	if (key == 'h' || key == 'H') {
		if (oc >= 0 && A.checks[oc].hints_shown < A.checks[oc].q.nhints) A.checks[oc].hints_shown++;
		return true;
	}
	if (key == 's' || key == 'S') { speak_step(); return true; }
	if (key == 'm' || key == 'M') { report(); return true; }
	if (key == K_ESC) { leave(); return true; }
	return false;
}

// A scroll moves the page's pixels and draws only the band it uncovers
// (lv_scrolled()), when it can: by whole pattern cells (8 rows, so the
// one-bit patterns keep their phase), less than a view, the footer's
// button unchanged. Returns false when the view did not move.
#define SCROLL_STEP 32

bool lv_wheel(int notches) {
	int view_h = A.h - HDR - FOOT - 1;
	int max = A.step_height - view_h;		// (as lv_draw measured it)
	if (max < 0) max = 0;
	int old = A.scroll, now = old - notches * SCROLL_STEP;
	if (now > max) now = max;
	if (now < 0) now = 0;
	if (now == old) return false;
	int d = now - old, ad = d < 0 ? -d : d;
	A.scroll = now;
	bool button = (old < max) != (now < max);		// "More" becomes "Next", or back
	int total = A.scroll_shift + d;
	if (total < 0) total = -total;
	// nothing else waiting to be drawn (a part, or the whole)
	if (!A.dirty && (!A.part || A.scroll_shift) && !button && ad % 8 == 0 && total < view_h - 40 && !A.review) {
		int pending = A.scroll_shift;
		A.scroll_shift = 0;
		la_damage(0, HDR + 1, A.w, view_h);
		A.scroll_shift = pending + d;
	} else A.dirty = true;
	return true;
}

static void scrollbar(ld_page_t *p) {
	int view_h = A.h - HDR - FOOT - 1, max = A.step_height - view_h;
	if (max <= 0) return;
	int bar_h = view_h * view_h / A.step_height;
	int bar_y = HDR + 1 + (view_h - bar_h) * A.scroll / max;
	ld_fill(p, A.w - 4, bar_y, 2, bar_h, LD_INK);
}

void lv_scrolled(ld_page_t *p) {
	int s = A.scroll_shift;
	A.scroll_shift = 0;
	int v0 = HDR + 1, v1 = A.h - FOOT, vh = v1 - v0, b0 = v0, b1 = v1;
	// the view moved by the blitter, the band it leaves drawn; all of it
	// drawn when the pixels cannot be moved (another window over this one)
	ld_clip_reset(p);
	if (ld_scroll(p, 0, v0, A.w - 6, vh, -s)) {
		if (s > 0) b0 = v1 - s;		// the step moves up: the band at the bottom is new
		else b1 = v0 - s;
	}
	la_draw_part(p, 0, b0, A.w, b1 - b0);
	ld_clip_t was = ld_clip_push(p, A.w - 6, v0, A.w, v1);
	ld_fill(p, A.w - 6, v0, 6, vh, LD_DARK);
	scrollbar(p);
	ld_clip_pop(p, was);
}

// a lesson of the open course by its id (with or without the course's)
static int lesson_by_id(const char *want) {
	char id[200], full[200];
	snprintf(full, sizeof full, "%s/%s", A.course->m.id, want);
	for (int i = 0; i < A.course->nlessons; i++) {
		lf_lesson_id(A.course, i, id, sizeof id);
		if (!strcmp(id, full) || !strcmp(id, want)) return i;
	}
	return -1;
}

// a link (docs/learn_format.md): lesson:, course:, docs:, or the web
static void follow(const char *t) {
	if (!strncmp(t, "lesson:", 7) || !strncmp(t, "course:", 7)) {
		bool lesson = t[0] == 'l';
		const char *l = t + 7;
		if (lesson && !A.review) {
			int i = lesson_by_id(l);
			if (i >= 0) {
				if (A.step > 0) ls_at(A.lesson_id, A.step);		// this one's place is kept
				warmed = true;
				la_open_lesson(i);
				return;
			}
		}
		// another course: open it (and its lesson)
		char cid[LF_NAME];
		int n = 0;
		while (l[n] && l[n] != '/' && n < LF_NAME - 1) { cid[n] = l[n]; n++; }
		cid[n] = 0;
		const lf_info_t *m = lk_find(cid);
		if (!m) { snprintf(A.status, sizeof A.status, "The course %s is not on this computer.", cid); return; }
		if (!A.review && A.step > 0) ls_at(A.lesson_id, A.step);
		A.review = false;
		if (!la_open_course(m->path)) return;
		int i = lesson ? lesson_by_id(l) : -1;
		if (i >= 0) { warmed = true; la_open_lesson(i); }
		else la_go(SC_COURSE);
		return;
	}
	if (!strncmp(t, "docs:", 5)) {
		char path[200];
		snprintf(path, sizeof path, "%s/%.120s.md", Z_DIR_DOCS, t + 5);
		if (!LP->run("read", path)) snprintf(A.status, sizeof A.status, "Could not open %.130s.", path);
		return;
	}
	if (!strncmp(t, "http", 4) || !strncmp(t, "gemini://", 9)) {
		if (!LP->run("web", t)) snprintf(A.status, sizeof A.status, "Could not open the web browser.");
		return;
	}
	snprintf(A.status, sizeof A.status, "learn does not know where %.100s goes.", t);
}

bool lv_hot(int action, int arg) {
	switch (action) {
	case A_NEXT:
		if (A.screen == SC_DONE) return lv_done_key(K_ENTER);
		if (A.review && A.rv_done) return rv_done_key(K_ENTER);
		next_step();
		return true;
	case A_CHOICE: choose(arg / 16, arg % 16); return true;
	case A_CHECK: check_typed(arg); return true;
	case A_FIELD: A.typing = arg; return true;
	case A_HINT: if (A.checks[arg].hints_shown < A.checks[arg].q.nhints) A.checks[arg].hints_shown++; return true;
	case A_SAY: speak_step(); return true;
	case A_EXAMPLE: A.example_shown++; return true;
	case A_TRY: {
		const lf_step_t *s = cur();
		for (int i = s->first; i < s->first + s->count; i++) {
			const lf_block_t *b = &A.les->blocks[i];
			if (b->kind == LB_DATA && !strcmp(b->name, "try") && b->forms->head) {
				const char *app = lx_str_of(b->forms->head, "app");
				if (app && !LP->run(app, lx_str_of(b->forms->head, "open")))
					snprintf(A.status, sizeof A.status, "Could not open %s.", app);
			}
		}
		return true;
	}
	case A_LINK:
		if (arg >= 0 && arg < nstep_links) follow(step_links[arg]);
		return true;
	case A_AGAIN: la_open_lesson(A.lesson); return true;
	case A_TO_COURSE: la_go(SC_COURSE); return true;
	}
	return false;
}

// -- a lesson done --

void lv_done_draw(ld_page_t *p) {
	la_frame(p, A.crumb, A.les->nsteps, A.les->nsteps);
	ld_style_t h = ld_st(&LDF.serif, 28), b = ld_st(&LDF.body, 0), bb = ld_st(&LDF.body_b, 0);
	ld_textz(p, &h, 24, 72, "Lesson done", LD_INK);
	ld_textz(p, &bb, 24, 100, A.les->title ? A.les->title : "", LD_INK);
	char t[200];
	int y = 130;
	if (A.total) {
		snprintf(t, sizeof t, "You answered %d of %d question%s right the first time.", A.right, A.total, A.total == 1 ? "" : "s");
		y = ld_wrap(p, &b, 24, y, A.w - 48, 18, t, LD_INK) + 6;
		ld_bar(p, 24, y, 240, 12, A.right * 100 / A.total);
		y += 34;
		bool mastered = A.right * 5 >= A.total * 4;
		y = ld_wrap(p, &b, 24, y, A.w - 48, 18, mastered ?
			"That is mastery: you are ready for the next lesson." :
			"Go on, or do it again: the questions you missed are worth another look.", LD_INK) + 10;
	} else y = ld_wrap(p, &b, 24, y, A.w - 48, 18, "On to the next lesson.", LD_INK) + 10;
	bool has_next = A.lesson + 1 < A.course->nlessons;
	if (has_next) {
		const lf_lref_t *n = &A.course->lessons[A.lesson + 1];
		bool new_unit = n->unit != A.course->lessons[A.lesson].unit;
		if (new_unit) snprintf(t, sizeof t, "Next, a new unit: %s", A.course->units[n->unit].title);
		else snprintf(t, sizeof t, "Next: %s", la_lesson_title(A.lesson + 1));
		ld_textz(p, &b, 24, y + 16, t, LD_INK);
	} else ld_textz(p, &b, 24, y + 16, "That was the last lesson of the course.", LD_INK);
	int idx = A.nhot;
	la_hot(24, y + 40, 120, 24, A_AGAIN, 0);
	ld_button(p, 24, y + 40, 120, 24, "Again", false, A.focus == idx);
	idx = A.nhot;
	la_hot(156, y + 40, 120, 24, A_TO_COURSE, 0);
	ld_button(p, 156, y + 40, 120, 24, "The course", false, A.focus == idx);
	la_footer(p, "Enter|next lesson;A|again;Esc|the course", has_next ? "Next lesson" : "The course", A_NEXT);
}

bool lv_done_key(uint32_t key) {
	if (key == K_ENTER || key == ' ') {
		if (A.lesson + 1 < A.course->nlessons) la_open_lesson(A.lesson + 1);
		else la_go(SC_COURSE);
		return true;
	}
	if (key == 'a' || key == 'A') { la_open_lesson(A.lesson); return true; }
	if (key == K_ESC) { la_go(SC_COURSE); return true; }
	return false;
}

// -- review: due questions, one at a time (lreview.c, docs/learn.md "Review") --

// the lesson file of a question in its course: "01-start/02-questions.md"
static void rel_file(char *out, int cap) {
	char full[LF_PATH + 200];
	lf_lesson_path(A.course, A.lesson, full, sizeof full);
	size_t l = strlen(A.course->m.path);
	const char *rel = !strncmp(full, A.course->m.path, l) && full[l] == '/' ? full + l + 1 : full;
	int n = (int)strlen(rel);
	if (n >= cap) n = cap - 1;
	memcpy(out, rel, (size_t)n);
	out[n] = 0;
}

static void rv_record(la_check_t *k) {
	char key[48], file[160];
	if (A.review) {
		A.rv_total++;
		if (k->right_first) A.rv_right++;
		lr_answered(A.lesson_id, A.rv_key, A.step, A.rv_file, k->right_first, true);
		return;
	}
	lr_key(&k->q, key, sizeof key);
	rel_file(file, sizeof file);
	lr_answered(A.lesson_id, key, A.step, file, k->right_first, false);
}

// the check in step s whose key is `key`: loaded as A.checks[0]
static bool rv_find_in(int s, const char *key) {
	const lf_step_t *st = &A.les->steps[s];
	for (int i = st->first; i < st->first + st->count; i++) {
		const lf_block_t *b = &A.les->blocks[i];
		if (b->kind != LB_DATA) continue;
		for (lx_node_t *f = b->forms->head; f; f = f->next) {
			const lx_node_t *form = NULL;
			if (!strcmp(b->name, "check")) form = f;
			else if (!strcmp(b->name, "try")) { lx_node_t *ch = lx_find(f, "check"); if (ch) form = lx_nth(ch, 1); }
			if (!form) continue;
			la_check_t *k = &A.checks[0];
			memset(k, 0, sizeof *k);
			// the choices in another order than in the lesson
			if (!lq_load(&k->q, form, hash(key) + (uint32_t)lr_today() * 7919u)) continue;
			char kk[48];
			lr_key(&k->q, kk, sizeof kk);
			if (strcmp(kk, key)) continue;
			A.step = s;
			A.nchecks = 1;
			A.typing = k->q.kind >= LQ_NUMBER && k->q.kind <= LQ_TEXT ? 0 : -1;
			return true;
		}
	}
	return false;
}

// the next item that can be shown, from rv_pos: false when there is none
static bool rv_load(void) {
	for (; A.rv_pos < A.rv_n; A.rv_pos++) {
		const lr_item_t *it = lr_get(A.rv_idx[A.rv_pos]);
		if (!it) continue;
		const char *id = lr_name(it->id), *file = lr_name(it->file), *hashp = strchr(id, '#');
		if (!hashp || !file[0]) continue;
		char cid[LF_NAME];
		lr_course_of(id, cid, sizeof cid);
		const lf_info_t *m = lk_find(cid);
		if (!m) continue;		// a course no longer here
		char path[LF_PATH + 200];
		snprintf(path, sizeof path, "%s/%s", m->path, file);
		char *text = (char *)A.lesson_mem;
		long n = LP->read_at(path, 0, text, LF_MAX_LESSON);
		if (n < 0) continue;
		text[n] = 0;
		lx_arena_init(&A.les_arena, A.lesson_mem + LF_MAX_LESSON + 8, A.lesson_memsize - LF_MAX_LESSON - 8);
		char err[160];
		if (!lf_lesson_parse(&A.les_arena, text, n, A.les, err, sizeof err)) continue;
		snprintf(A.rv_key, sizeof A.rv_key, "%s", hashp + 1);
		snprintf(A.rv_file, sizeof A.rv_file, "%s", file);
		int il = (int)(hashp - id);
		snprintf(A.lesson_id, sizeof A.lesson_id, "%.*s", il < (int)sizeof A.lesson_id - 1 ? il : (int)sizeof A.lesson_id - 1, id);
		bool found = it->step < A.les->nsteps && rv_find_in(it->step, A.rv_key);
		for (int s = 0; !found && s < A.les->nsteps; s++) found = rv_find_in(s, A.rv_key);
		if (!found) continue;		// the question was changed or removed
		snprintf(A.rv_from, sizeof A.rv_from, "%.119s", A.les->title ? A.les->title : file);
		snprintf(A.crumb, sizeof A.crumb, "%s  >  %s  >  %d of %d", A.rv_then >= 0 ? "Warm-up" : "Review",
			m->title, A.rv_pos + 1, A.rv_n);
		A.scroll = 0;
		A.example_shown = 0;
		A.said[0] = 0;
		lc_forget();
		fig_course = m->path;
		fig_enter();
		A.dirty = true;
		return true;
	}
	return false;
}

bool lv_review_start(const char *course, int max, int then_lesson) {
	if (max > (int)(sizeof A.rv_idx / sizeof A.rv_idx[0])) max = (int)(sizeof A.rv_idx / sizeof A.rv_idx[0]);
	A.rv_n = lr_due(A.rv_idx, max, course);
	if (!A.rv_n) return false;
	A.review = true;
	A.rv_done = false;
	A.rv_pos = 0;
	A.rv_right = A.rv_total = 0;
	A.rv_then = then_lesson;
	la_go(SC_LESSON);
	if (!rv_load()) {
		// none could be shown (their lessons changed): nothing to review
		A.review = false;
		if (then_lesson >= 0) { warmed = true; la_open_lesson(then_lesson); }
		else la_go(SC_HOME);
		return then_lesson >= 0;
	}
	return true;
}

static void rv_finish(void) {
	A.rv_done = true;
	lg_unload();
	A.fig = false;
	snprintf(A.crumb, sizeof A.crumb, "%s", A.rv_then >= 0 ? "Warm-up" : "Review");
	A.dirty = true;
}

static void rv_next(void) {
	A.rv_pos++;
	if (!rv_load()) rv_finish();
}

static void rv_end(void) {
	int then = A.rv_then;
	A.review = false;
	A.rv_done = false;
	lg_unload();
	A.fig = false;
	if (then >= 0) { warmed = true; la_open_lesson(then); }
	else la_go(SC_HOME);
}

// Esc: a warm-up goes on to its lesson; a review goes home
static void rv_leave(void) { rv_end(); }

static void rv_done_draw(ld_page_t *p) {
	la_frame(p, A.crumb, A.rv_n, A.rv_n);
	ld_style_t h = ld_st(&LDF.serif, 28), b = ld_st(&LDF.body, 0), bb = ld_st(&LDF.body_b, 0);
	bool warm = A.rv_then >= 0;
	ld_textz(p, &h, 24, 72, warm ? "Warm-up done" : "Review done", LD_INK);
	char t[220];
	int y = 110;
	if (A.rv_total) {
		snprintf(t, sizeof t, "You remembered %d of %d question%s.", A.rv_right, A.rv_total, A.rv_total == 1 ? "" : "s");
		y = ld_wrap(p, &b, 24, y, A.w - 48, 18, t, LD_INK) + 6;
		ld_bar(p, 24, y, 240, 12, A.rv_right * 100 / A.rv_total);
		y += 34;
		y = ld_wrap(p, &b, 24, y, A.w - 48, 18, A.rv_right == A.rv_total ?
			"Each will come back later than last time." :
			"The ones you missed come back tomorrow; the others later than last time.", LD_INK) + 10;
	}
	int left = lr_count_due(NULL);
	if (warm) {
		snprintf(t, sizeof t, "Now: %s", la_lesson_title(A.rv_then));
		ld_textz(p, &bb, 24, y + 16, t, LD_INK);
	} else if (left > 0) {
		snprintf(t, sizeof t, "%d more due today: R to go on.", left);
		ld_textz(p, &b, 24, y + 16, t, LD_INK);
	}
	la_footer(p, warm ? "Enter|the lesson" : left > 0 ? "R|review more;Enter|home" : "Enter|home",
		warm ? "The lesson" : "Home", A_NEXT);
}

static bool rv_done_key(uint32_t key) {
	if (key == K_ENTER || key == ' ' || key == K_ESC) { rv_end(); return true; }
	if ((key == 'r' || key == 'R') && A.rv_then < 0) {
		if (!lv_review_start(NULL, 20, -1)) rv_end();
		return true;
	}
	return false;
}
