/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- figures: the ```figure blocks of a step, Scheme run in a
 * sandbox (docs/learn_figures.md).
 *
 * A figure is run whole for every frame it is drawn: what it draws goes
 * into a display list in its own 320x240 units, and the display list is
 * drawn on the page (ldraw.h: on the board, by the blitter and the line
 * rasterizer) -- one bit with a pattern for each colour role, or plane
 * by plane for the colour window. Its controls (sliders, toggles,
 * choices, points to drag) keep their values between frames; nothing
 * else does, so a frame depends only on the controls and the time.
 *
 * The Scheme is Machdyne Scheme (sw/ext/ms) without the Zeitlos API: no
 * files, no network, no programs, no records. A frame that runs too long
 * is stopped.
 */

#ifndef LFIG_H
#define LFIG_H

#include <stdint.h>
#include <stdbool.h>

#include "ldraw.h"

#define LG_W 320
#define LG_H 240

// colour roles (docs/learn_figures.md, "Colours")
enum {
	LG_NONE = 0, LG_INK, LG_PAPER, LG_WARM, LG_COOL, LG_GOOD, LG_BAD,
	LG_ACCENT, LG_SKY, LG_EARTH, LG_PLANT, LG_GREY,
	LG_ROLES
};
extern const char *const lg_role_name[LG_ROLES];

enum {	// display list
	LG_OP_LINE = 1,		// a[0..3] ends, a[4] width
	LG_OP_RECT,			// a[0..3] x y w h, filled
	LG_OP_FRAME,		// a[0..3], a[4] width
	LG_OP_DISC,			// a[0..2] cx cy r
	LG_OP_RING,			// a[0..2], a[3] width
	LG_OP_POLY,			// pts[off..off+2n), filled
	LG_OP_PATH,			// pts, a[0] width: open
	LG_OP_ARC,			// a[0..2] cx cy r, a[3..4] degrees from, to, a[5] width
	LG_OP_ARROW,		// a[0..3], a[4] width
	LG_OP_TEXT,			// a[0..1] x baseline, text[off], size, align
	LG_OP_MATH,			// the same, a formula
};

enum { LG_LEFT = 0, LG_CENTRE, LG_RIGHT };
// text sizes: the bitmap faces by name; a number is a size in px (serif)
enum { LG_TINY = 1, LG_SMALL, LG_BODY, LG_BIG };

typedef struct {
	uint8_t		kind, role, align, size;	// size: LG_TINY.. or px
	uint16_t	off, n;
	float		a[6];
} lg_op_t;

enum { LG_SLIDER = 1, LG_TOGGLE, LG_CHOICE, LG_DRAG };

#define LG_MAX_CTL 6
#define LG_MAX_OPTS 6

typedef struct {
	uint8_t		kind;
	bool		used;				// asked for in the last frame
	char		name[20];
	char		label[40];
	char		opt[LG_MAX_OPTS][16];	// a choice's options
	int			nopt;
	double		min, max, step, value;
	double		x, y, r;			// a point to drag (value unused)
} lg_ctl_t;

#define LG_MAX_OPS 1200
#define LG_MAX_PTS 6000
#define LG_MAX_TEXT 6144

typedef struct {
	lg_op_t		op[LG_MAX_OPS];
	int			nops;
	float		pts[LG_MAX_PTS];
	int			npts;
	char		text[LG_MAX_TEXT];
	int			ntext;
	lg_ctl_t	ctl[LG_MAX_CTL];
	int			nctl;
	bool		loaded;			// a figure is set
	bool		animated;		// asked for the time: draw again and again
	bool		colour;			// asked for colour ((colour)): the colour window, where there is one
	bool		full;			// it drew more than the list holds
	bool		outside;		// it drew outside 320x240 (learncheck says so)
	bool		cut;			// words that go past its edge: they would be cut off
	char		err[160];		// the first mistake, "" if none
	char		frame_err[160];	// the first mistake of the last frame
	uint32_t	frames;			// frames run: a new picture when it changes
	uint32_t	ms;				// how long the last frame ran (its Scheme), ms
	// (still ...): the shapes that do not move, kept at the front of the
	// list from frame to frame and drawn from a bitmap of their own
	// until a control changes (docs/learn_figures.md, "Time")
	int			nstill, nstill_pts, nstill_text;
	uint32_t	still_serial;	// a new set of still shapes
	uint32_t	ctl_gen;		// a control changed
	uint32_t	still_gen;		// the ctl_gen they were drawn for
	bool		still_seen;		// this frame asked for them
	// what the last frame changed, figure units: its moving shapes, where
	// they were and are (x0 y0 x1 y1); or all of it
	float		dirty[4];
	bool		dirty_all;
	bool		unchanged;		// it drew just what the frame before did
} lg_fig_t;

extern lg_fig_t LG;

// Milliseconds, for stopping a frame that runs too long (any origin).
void lg_set_clock(uint32_t (*ms)(void));
// Starts Scheme with the figure library (once). False if it cannot.
bool lg_start(void);
// A figure: a course's library (lib/*.scm, joined; may be NULL) and the
// step's figure blocks (joined). Controls start again. False, with
// LG.err, if it does not read.
bool lg_load(const char *lib, const char *src);
void lg_unload(void);
// One frame at `t` seconds since the figure was shown. False, with
// LG.frame_err, if it went wrong (what it drew before is kept).
bool lg_frame(double t, uint32_t budget_ms);

// Draws the display list into w x h at (x, y) (320x240 units scaled).
void lg_render(ld_page_t *p, int x, int y, int w, int h);
// One plane of the colour picture: a pixel is lit where the colour of
// the role drawn there has `bit` set (lg_colour_index()).
void lg_render_plane(ld_page_t *p, int x, int y, int w, int h, int bit, int planes);
// What the last frame changed, in pixels of the figure drawn at (x, y,
// w, h): r = x0 y0 x1 y1. False: all of it.
bool lg_dirty_px(int x, int y, int w, int h, int r[4]);
// The still shapes' pixels are kept from frame to frame (read back from
// where they were drawn); forgotten here when that may be stale.
void lg_forget_still(void);
// The colour a role shows in the colour window, and its RGB (4:4:4).
int lg_colour_index(int role, int planes);
uint32_t lg_colour_rgb(int index);

// Controls: set a value (clamped, a slider on its steps); a toggle flips.
void lg_ctl_set(int i, double v);
void lg_ctl_nudge(int i, int dir);			// a step (a tenth without one)
void lg_ctl_drag_to(int i, double x, double y);
// The control under (ux, uy) in figure units: a point to drag; -1 none.
int lg_drag_at(double ux, double uy);

#endif
