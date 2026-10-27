/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- the app, apart from the machine: its screens, what they do
 * with keys and the mouse, and how they are drawn (docs/learn_ui.md).
 * learn.c gives it the window's events and puts what it draws on the
 * screen; the tests give it scripted events and write what it draws to
 * pictures. Keys are Zeitlos keysyms (zkbd.h).
 */

#ifndef LAPP_H
#define LAPP_H

#include <stdint.h>
#include <stdbool.h>

#include "ldraw.h"

enum { LA_KEY = 0, LA_DOWN, LA_UP, LA_MOVE, LA_WHEEL };

typedef struct {
	uint8_t		type;
	uint8_t		mods;		// Z_KBD_MOD_*
	uint32_t	key;		// LA_KEY: the keysym
	int			x, y;		// the mouse, in the page's pixels
	int			notches;	// LA_WHEEL: up is positive
} la_ev_t;

// memory for courses, lessons, records and the catalogue (1.5 MB is
// plenty; less holds fewer courses in the catalogue)
void la_init(void *mem, uint32_t size);
void la_resize(int w, int h);
// true if the page should be drawn again
bool la_event(const la_ev_t *e);
// what a key found: the screen, the step, the answer being typed (the console)
const char *la_trace(void);
// What a press is about to do, to show at once while it happens: the
// target under the mouse, or the button Enter or Space presses. False if
// it presses nothing.
bool la_pressed(const la_ev_t *e, int *x, int *y, int *w, int *h);
// work to do between events (finding courses, opening one): do some, for
// about budget_ms; true while there is more
bool la_work(uint32_t budget_ms);
bool la_busy(void);
void la_stop_opening(void);	// Esc while a course opens
bool la_opening(void);		// a course is being opened
bool la_wants_draw(void);	// something changed since the last drawing
void la_forget_page(void);
void la_draw(ld_page_t *p);
// Only part changed (typing in a field, a figure's control): that part,
// to draw with la_draw_part() and put on the screen alone. False: draw
// the whole page (la_draw()).
bool la_partial(int *x, int *y, int *w, int *h);
void la_draw_part(ld_page_t *p, int x, int y, int w, int h);
// the window's title
const char *la_title(void);
// what F1's box says
const char *la_help(void);
// Esc on the home screen: the person wants to leave
bool la_quit(void);
// the window is closing: keep the learner's place (in a lesson, the step)
void la_close(void);
// open this course (a launch argument: a course's folder or id)
void la_open_arg(const char *arg);
// a moving figure is showing: call la_draw_figure() a dozen times a second
bool la_animating(void);
// A step's figure is read and run after its page is drawn without it,
// when this is set (the board): la_figure_due() says one is waiting,
// la_figure_now() runs it and asks for it to be drawn (la_partial()).
extern bool la_defer_figures;
bool la_figure_due(void);
void la_figure_now(void);
// the next frame of the moving figure, drawn on the page alone; false if
// it is not on the page. (x, y, w, h): what to put on the screen.
bool la_draw_figure(ld_page_t *p, int *x, int *y, int *w, int *h);

// -- the figure in colour (the colour window: docs/learn_figures.md) --
// Where the figure is on the page, if all of it shows (and it drew): the
// rectangle the colour window may cover.
bool la_figure_rect(int *x, int *y, int *w, int *h);
// the lesson's step has a figure (that drew), on the page or scrolled away
bool la_figure_present(void);
// Colour on (the window's planes: 2 or 4) or off (0): the page then
// holds the colour picture's lowest bit where the figure is.
void la_figure_colour(int planes);
// the same, the figure drawn again at once on p (nothing else is)
void la_figure_recolour(ld_page_t *p, int planes);
// One of the window's planes (1..planes) of the figure, drawn at (0, 0) of p.
void la_figure_plane(ld_page_t *p, int k);

#endif
