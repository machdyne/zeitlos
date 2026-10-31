#ifndef MJ_PLAY_H
#define MJ_PLAY_H

/*
 * Zeitlos mahjong -- a session: what a click or a key does.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * No I/O. The app (mahjong.c) hands this the clicks, keys and the
 * time, and gets back what changed: rectangles of the canvas to repaint
 * (or "everything"), a line to say, a sound to play, sparks to throw,
 * and whether the settings want saving. Both faces drive the same
 * session, so the desktop and the screen cannot disagree about what a
 * key does, and tests/play_test.c drives it with no screen at all.
 *
 * The files it would read and write -- the statistics and settings in
 * /data/mahjong/stats.txt, the layouts in /data/mahjong/layouts -- go
 * through text: mj_play_load_stats() parses what the app read and
 * mj_play_save_stats() formats what the app will write.
 */

#include <stdint.h>
#include <stdbool.h>

#include "mj_game.h"
#include "mj_board.h"

#define MJ_LAYOUTS      24      /* built-in and from files */
#define MJ_STATS        24      /* layouts with statistics kept */
#define MJ_RECTS        48

/* Sounds the app plays (the screen face has a mixer to play them on). */
enum { MJ_SND_NONE = 0, MJ_SND_PICK, MJ_SND_TAKE, MJ_SND_NOPE, MJ_SND_WIN, MJ_SND_SHUFFLE };

typedef struct {
	char     name[MJ_NAME];
	uint32_t played, won, best;     /* best in seconds, 0 = none yet */
} mj_stat_t;

typedef struct { int16_t x, y, w, h; } mj_rect_t;

/* Where a layout's text comes from: the app reads built-ins from
 * mj_layouts.c and the rest from files. Returns the text, or NULL. */
typedef const char *(*mj_layout_src_fn)(void *user, int i, char *buf, int cap);

typedef struct {
	mj_layout_t L;
	mj_game_t   g;
	mj_view_t   v;

	/* the layouts: names in the order L cycles through them */
	char        lname[MJ_LAYOUTS][MJ_NAME];
	int         nlayouts, layout;
	mj_layout_src_fn src;
	void       *src_user;

	/* settings, kept in stats.txt */
	bool        random;             /* deal without the guarantee */
	int         zoom;               /* the screen face: 0 whole, 1 close */
	bool        can_zoom;           /* this face has a zoom */

	/* the clock: seconds of play, counted from the first pair */
	bool        timing, counted;
	uint32_t    t0, elapsed, now;
	uint32_t    msg_until;

	mj_stat_t   stats[MJ_STATS];
	int         nstats;
	char        want[MJ_NAME];      /* stats.txt's `layout` line */

	/* -- out: read and cleared by the app after each call -- */
	mj_rect_t   rect[MJ_RECTS];
	int         nrect;
	bool        all;                /* repaint everything */
	bool        relayout;           /* size, corner, zoom or layout changed */
	bool        bar;                /* repaint the bar */
	int         sdx, sdy;           /* the view scrolled by this, nothing else changed */
	char        say[64];
	int         sound;
	int         burst[2];           /* tiles just taken, for sparks; -1 */
	bool        save;               /* stats.txt wants writing */
	bool        keyboard;           /* the last move was by keyboard */
} mj_play_t;

/* Sets up with the layouts named in names[0..n) (the app has listed
 * them), the settings already loaded, and deals. */
void mj_play_init(mj_play_t *p, mj_layout_src_fn src, void *user);
void mj_play_add_layout(mj_play_t *p, const char *name);

/* A new deal of the current layout. seed is the deal's number. */
void mj_play_new(mj_play_t *p, uint32_t seed);
/* Switches to layout i (an index into lname) and deals `seed`. */
bool mj_play_layout(mj_play_t *p, int i, uint32_t seed);
int  mj_play_find_layout(const mj_play_t *p, const char *name);

/* Input. Coordinates are the canvas's. `seed` is a fresh number for a
 * new deal, should the input ask for one. */
void mj_play_click(mj_play_t *p, int x, int y, uint32_t seed);
void mj_play_key(mj_play_t *p, uint32_t sym, int mods, uint32_t seed);
void mj_play_button(mj_play_t *p, int b, uint32_t seed);
/* The wheel: down the board, or across it when `across` (Shift, Ctrl
 * or Alt held -- the app asks the keyboard, z_kbd_live_mods()). A plain
 * wheel on a board that only scrolls across scrolls across. */
void mj_play_wheel(mj_play_t *p, int notches, bool across);
/* The pointer resting at (x, y): near an edge of a board that scrolls,
 * the board scrolls. */
void mj_play_hover(mj_play_t *p, int x, int y);

/* Once in a while with the time in seconds: the clock, and messages
 * that have been up long enough. */
void mj_play_tick(mj_play_t *p, uint32_t now_s);

/* The bar's status line, from the game. */
void mj_play_status(mj_play_t *p);

/* stats.txt: statistics per layout, and the settings. */
void mj_play_load_stats(mj_play_t *p, const char *text);
int  mj_play_save_stats(const mj_play_t *p, char *out, int cap);
const mj_stat_t *mj_play_stat(const mj_play_t *p, const char *name);

/* The help, as one text (F1 on either face). */
const char *mj_play_help(void);

/* "m:ss" or "h:mm:ss". */
void mj_play_time(uint32_t s, char *buf, int len);

#endif
