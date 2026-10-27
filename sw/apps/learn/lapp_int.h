/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- the app's state, shared by its screens (lapp.c) and the
 * lesson (lstep.c). Not for anything outside the app.
 */

#ifndef LAPP_INT_H
#define LAPP_INT_H

#include "lapp.h"
#include "lformat.h"
#include "lquiz.h"
#include "lstore.h"
#include "lcat.h"
#include "ltext.h"
#include "lfig.h"
#include "lreview.h"

enum {
	SC_LEARNERS = 0,	// who is learning
	SC_HOME,
	SC_CATALOGUE,
	SC_COURSE,
	SC_LESSON,
	SC_DONE,			// a lesson finished
	SC_LOADING,			// finding courses / opening one
};

// a place on the page the mouse can press, and Tab can reach
typedef struct {
	int16_t		x, y, w, h;
	uint8_t		action;
	int16_t		arg;
} la_hot_t;

#define LA_MAX_HOT 64

enum {	// actions
	A_NONE = 0, A_LEARNER, A_NEW_LEARNER, A_CONTINUE, A_EXPLORE, A_COURSE_ROW,
	A_AREA, A_CAT_ROW, A_START, A_UNIT, A_LESSON, A_NEXT, A_BACK, A_CHOICE,
	A_CHECK, A_HINT, A_SAY, A_TRY, A_EXAMPLE, A_AGAIN, A_TO_COURSE, A_LINK,
	A_FIELD, A_HOME, A_REPORT, A_OPEN, A_REVIEW,
};

// the state of one check in the lesson being learned
typedef struct {
	lq_q_t		q;
	uint32_t	chosen;			// choices picked (display order)
	char		typed[64];
	int			cursor;
	int			attempts;
	int			hints_shown;
	bool		answered;		// finished with (right, or shown the answer)
	bool		skipped;		// passed over with F8: not answered, not reviewed
	bool		right_first;	// right at the first attempt
	lq_result_t	last;			// the last judgement
	bool		judged;
	int			focus;			// the choice the keyboard is on
	int16_t		fx, fy, fw, fh;	// its answer field: x, y from the step's top (scrolled: lv_draw), size
} la_check_t;

#define LA_MAX_CHECKS 8

typedef struct {
	int			screen, back_to;
	int			w, h;
	bool		dirty;				// draw the whole page again
	bool		part, part_now;		// only part of it changed (la_damage), so far / in this event
	bool		fig_part;			// and that part is the figure's alone (lv_fig_part())
	bool		keep_fig;		// the part drawn next leaves the figure as it is (a Check)
	bool		menu_fresh;		// the next menu screen copied whole (lapp.c, draw_menu)
	bool		quiet;				// the event was taken and changed nothing
	int			part_x0, part_y0, part_x1, part_y1;
	la_hot_t	hot[LA_MAX_HOT];
	int			nhot, focus;		// focus: index into hot, -1 none
	char		title[96];
	char		status[160];		// a line of news at the foot (an error, "saved")
	bool		quit;

	// memory
	uint8_t		*mem;
	uint32_t	memsize, used;
	lf_course_t	*course;			// the open course
	bool		course_open;
	uint8_t		*lesson_mem;		// the open lesson's arena
	uint32_t	lesson_memsize;
	uint8_t		*rec_mem;
	uint32_t	rec_memsize;
	lx_arena_t	scratch;			// for laying out text while drawing

	// learners
	ls_who_t	who[LS_MAX_LEARNERS];
	int			nwho, who_sel;

	// catalogue
	int			area, cat_sel, cat_top, cat_col;	// col: 0 areas, 1 courses
	int			cat_idx[LK_MAX];		// courses in the area shown
	int			cat_n;
	char		search[48];

	// course
	int			unit_sel, lesson_sel, course_col;	// col 0 units, 1 lessons
	char		want_course[LF_PATH];				// to open after a scan
	char		want_lesson[160];

	// lesson
	int			lesson;				// index in the course
	lf_lesson_t	*les;
	lx_arena_t	les_arena;
	int			step;
	int			scroll;				// pixels the step is scrolled
	int			scroll_shift;		// a scroll not drawn yet (lv_wheel): the pixels to move
	int			step_height;		// how tall it is laid out
	la_check_t	checks[LA_MAX_CHECKS];
	int			nchecks;			// in this step
	int			example_shown;		// steps of an example revealed
	int			right, total;		// for the lesson
	uint32_t	started_ms;
	int			typing;				// the check whose field has the keyboard, -1 none
	char		lesson_id[160];
	char		crumb[200];
	char		said[1024];			// the step's words, for S

	// the step's figure (lfig.c, docs/learn_figures.md)
	bool		fig;				// the step has one
	bool		fig_need;			// run a frame before it is drawn
	bool		fig_wait;			// not read yet (la_figure_now())
	bool		fig_paused;			// P: a moving figure stopped
	uint32_t	fig_t0, fig_last;	// ms: when it was shown, its last frame
	double		fig_pause_t;
	int			fig_sel;			// the control Left and Right change
	int			fig_drag;			// a control being dragged, -1 none
	bool		fig_shown;			// drawn in the last page
	int			fig_x, fig_y, fig_w, fig_h;		// the figure on the page
	int			fig_ctl_y[LG_MAX_CTL];			// each control's row, -1 hidden
	int			fig_track_x, fig_track_w;		// a slider's track

	// review (lreview.c): due questions one at a time, in place of a lesson
	bool		review;				// the lesson screen shows review
	bool		rv_done;			// all answered: the summary
	int			rv_idx[24];			// the items (lr_get)
	int			rv_n, rv_pos, rv_right, rv_total;
	int			rv_then;			// a warm-up: the lesson after it, -1 home
	char		rv_key[48];			// the question shown, its key
	char		rv_file[160];		// and its lesson's file, in the course
	char		rv_from[120];		// the lesson it is from
	uint8_t		*rv_mem;			// the items' memory
	uint32_t	rv_memsize;
} la_t;

extern la_t A;

// lapp.c
void la_hot(int x, int y, int w, int h, int action, int arg);
// only this part of the page changed: drawn and shown alone, unless
// something else asks for the whole page
void la_damage(int x, int y, int w, int h);
void la_go(int screen);
bool la_open_course(const char *path);
void la_open_lesson(int lesson);
void la_frame(ld_page_t *p, const char *crumb, int step, int steps);
void la_footer(ld_page_t *p, const char *keys, const char *button, int action);
void *la_alloc(unsigned long n);
const char *la_lesson_title(int lesson);	// its # line, read once

// lstep.c: the lesson
void lv_enter(void);
void lv_fig_part(ld_page_t *p);
void lv_draw(ld_page_t *p);
bool lv_key(uint32_t key, uint8_t mods);
bool lv_hot(int action, int arg);
void lv_done_draw(ld_page_t *p);
bool lv_done_key(uint32_t key);
bool lv_ready(void);			// may the learner go on from this step?
bool lv_wheel(int notches);		// false if the view did not move
void lv_scrolled(ld_page_t *p);	// draws a pending scroll: pixels moved, the band uncovered
bool lv_mouse(int type, int x, int y);	// LA_DOWN/MOVE/UP: the figure's; true if taken
void lv_course_lib(void);		// the open course's lib/*.scm, read for its figures
// Review the due questions (of `course`, or all if NULL), at most max;
// then open `then_lesson` of the open course (-1: home). False if none due.
bool lv_review_start(const char *course, int max, int then_lesson);

#endif
