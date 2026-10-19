#ifndef ZARCADE_H
#define ZARCADE_H

/*
 * Zeitlos -- the arcade runtime: one 320x240 game screen, in a window
 * or full screen, in colour or monochrome. See docs/arcade.md.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * -- what every arcade game here had to invent for itself --
 *
 * space3d, chip8 and gamedemo each carry their own answers to the same
 * questions: how to draw into a window that other windows overlap, how
 * to go full screen (and come back) without wm drawing on top, which
 * key does that, how colour turns into bitplanes, how a keyboard and a
 * gamepad become one set of buttons, how the frame is paced. This file
 * is those answers, once, for snake, blocks and bricks.
 *
 * -- the model --
 *
 * A game draws one 320x240 picture in LOGICAL coordinates (0,0 is the
 * top-left of the play area) and in colour INDICES 0..15. Where it ends
 * up is the runtime's business:
 *
 *   AR_MODE_WINDOW      a 320x240 content area in a fixed-size window,
 *                       monochrome, clipped to the part no other window
 *                       covers
 *   AR_MODE_FULL_MONO   game mode, the picture pixel-doubled, mono
 *   AR_MODE_FULL_COLOR  game mode with 16 colours: four bitplanes in
 *                       the four quadrants (docs/color.md, z_color_16)
 *
 * In colour an index is a palette entry. In monochrome each index has
 * an INK -- an 8x8 pattern, anchored to the screen -- so the game says
 * "this is colour 9" once and the mono picture follows from the table.
 *
 * -- single-buffered, by design --
 *
 * A window has no off-screen VRAM to draw into, and 16 colours use all
 * four quadrants. So there is no back buffer in any mode, and the
 * runtime is built around drawing only what changed:
 *
 *   PAINT     the game's paint() callback draws the static picture
 *             inside one rectangle: background, walls, the snake's
 *             body, the stack of blocks. The runtime calls it for each
 *             rectangle marked dirty (ar_dirty()), clipped to it.
 *
 *   OVERLAYS  things that move every frame -- a ball, sparks, a head
 *             sliding between cells -- are submitted each frame with
 *             ar_ov_*(). The runtime remembers where each was drawn and
 *             repaints those rectangles when it moves or goes away, so
 *             a game never erases anything itself. An overlay that did
 *             not change is not touched at all.
 *
 * Everything is drawn straight after the frame boundary, so a frame's
 * changes land while the beam is at the top and almost never tear.
 *
 * -- the loop --
 *
 *     ar_init(&desc);
 *     for (;;) {
 *         ar_event_t e;
 *         while (ar_poll(&e)) handle(&e);
 *         for (i = 0; i < dt; i++) step();      // game logic
 *         submit overlays, mark dirty rectangles;
 *         dt = ar_frame();                      // draw, wait, repeat
 *     }
 *
 * ar_frame() returns the display frames that actually elapsed (1 when
 * keeping up), so the game can step the world that many times and keep
 * its speed when the machine is busy (docs/gamedemo.md, "The frame rate
 * is the smoothness").
 *
 * -- keys the runtime owns --
 *
 *   F1       help box (window only; docs/app_help.md)
 *   F11      full screen, or back to the window (docs/keys.md)
 *   Esc      leave full screen (armed on release, as space3d)
 *   C        full screen: colour on or off
 *   M        music on or off (zsfx)
 *
 * Everything else reaches the game as AR_EV_KEY, and as AR_EV_BUTTON
 * for the player controls below.
 *
 * -- players --
 *
 * Two players, each with the same buttons: four directions, A, B,
 * START. Player 0 is the arrow keys (Space = A, Enter = START,
 * Backspace = B) and the first gamepad; player 1 is W A S D (Q = A,
 * E = B, Tab = START) and the second gamepad. A one-player game reads
 * ar_any_*() and gets all of them.
 *
 * Nothing in this file is about a particular game.
 */

#include <stdint.h>
#include <stdbool.h>

#include "../zfont.h"

#define AR_W 320
#define AR_H 240

/* Dirty tracking granularity: 8x8 tiles, 40x30 of them. */
#define AR_TILE 8
#define AR_TW   (AR_W / AR_TILE)
#define AR_TH   (AR_H / AR_TILE)

typedef enum {
	AR_MODE_WINDOW = 0,
	AR_MODE_FULL_MONO,
	AR_MODE_FULL_COLOR
} ar_mode_t;

typedef struct {
	int16_t x, y, w, h;
} ar_rect_t;

/* -- inks: what an index looks like in monochrome --
 *
 * Eight rows, MSB-first, the same convention as z_pattern_table and
 * z_fb_hw_fill_pattern(). Anchored to the framebuffer's 8-pixel grid,
 * so two fills of the same ink join without a seam. */
typedef struct {
	uint8_t row[8];
} ar_ink_t;

extern const ar_ink_t ar_ink_black;
extern const ar_ink_t ar_ink_white;
extern const ar_ink_t ar_ink_grey12;   /* one pixel in eight */
extern const ar_ink_t ar_ink_grey25;
extern const ar_ink_t ar_ink_grey50;   /* checkerboard */
extern const ar_ink_t ar_ink_grey75;
extern const ar_ink_t ar_ink_hlines;   /* every other row */
extern const ar_ink_t ar_ink_vlines;
extern const ar_ink_t ar_ink_diag;
extern const ar_ink_t ar_ink_dots;     /* sparse dots, for backgrounds */

/* An ordered-dither ink for a level 0..16 (0 black, 16 white): the 4x4
 * Bayer matrix the blitter's own shade fills use, repeated to 8x8. */
void ar_ink_level(ar_ink_t *out, int level);

/* -- the game's description of itself -- */

typedef struct {
	const char *name;          /* window title, and the score file's app */
	const char *help_title;    /* F1 box title */
	const char *help_text;     /* F1 box text (z_dialog_help) */
	const char *help_doc;      /* docs/<doc>.md behind "Read docs" */

	uint16_t palette[16];      /* RGB444, the colour picture */
	ar_ink_t ink[16];          /* the monochrome picture */

	/* Draw the static picture inside `r` (logical coordinates). Every
	 * ar_* drawing call is already clipped to `r` while this runs, so a
	 * paint() may draw whole cells that overlap it and not care. */
	void (*paint)(void *user, const ar_rect_t *r);
	void *user;

	/* Initial mode wish for full screen: colour where the board has
	 * it. The C key changes it at run time. */
	bool prefer_mono;

	/* Called at the start of every frame's drawing, straight after the
	 * frame boundary and before paint(): the place for ar_copy(), so a
	 * copy lands with the rest of the frame rather than mid-picture.
	 * May be NULL. A copy moves whatever is in its rectangle, overlays
	 * included: keep overlays out of what you copy. */
	void (*before_paint)(void *user);
} ar_desc_t;

/* -- events -- */

enum {
	AR_EV_KEY = 1,      /* key: keysym (zkbd.h), pressed, mods */
	AR_EV_BUTTON,       /* player, button (AR_BTN_*), pressed */
	AR_EV_MOUSE,        /* x, y logical, buttons; inside */
	AR_EV_WHEEL,        /* notches */
	AR_EV_MODE,         /* the mode changed: ar_mode() says to what */
	AR_EV_SUSPEND,      /* the game should pause: help box, focus... */
	AR_EV_QUIT          /* the window was closed */
};

#define AR_BTN_LEFT   (1u << 0)
#define AR_BTN_RIGHT  (1u << 1)
#define AR_BTN_UP     (1u << 2)
#define AR_BTN_DOWN   (1u << 3)
#define AR_BTN_A      (1u << 4)
#define AR_BTN_B      (1u << 5)
#define AR_BTN_START  (1u << 6)
#define AR_BTN_X      (1u << 7)   /* gamepad only */
#define AR_BTN_Y      (1u << 8)   /* gamepad only */
#define AR_BTN_SELECT (1u << 9)   /* gamepad only */
#define AR_BTN_DIRS   (AR_BTN_LEFT | AR_BTN_RIGHT | AR_BTN_UP | AR_BTN_DOWN)

#define AR_PLAYERS 2

typedef struct {
	uint8_t type;
	uint8_t player;      /* AR_EV_BUTTON */
	uint8_t pressed;     /* KEY, BUTTON */
	uint8_t mods;        /* KEY */
	uint32_t key;        /* KEY: keysym; BUTTON: one AR_BTN_* bit */
	int16_t x, y;        /* MOUSE (logical; may be outside 0..319) */
	uint8_t buttons;     /* MOUSE */
	uint8_t inside;      /* MOUSE */
	int16_t notches;     /* WHEEL */
	uint8_t source;      /* BUTTON: 0 keyboard, 1 gamepad */
} ar_event_t;

/* -- setup and the loop -- */

/* Open the window and set everything up. False if there is no window
 * manager to talk to. */
bool ar_init(const ar_desc_t *d);

/* Leave full screen, silence sound, close the window. */
void ar_shutdown(void);

/* Next event, or false when there are none. Drains wm's messages. */
bool ar_poll(ar_event_t *e);

/* Draw everything that changed, at the next frame boundary, and return
 * the display frames elapsed since the last call (1..4). */
uint32_t ar_frame(void);

/* Frames since ar_init(), counted as ar_frame() returned them. */
uint32_t ar_ticks(void);

ar_mode_t ar_mode(void);
bool ar_fullscreen(void);
bool ar_color(void);              /* drawing in colour right now */
bool ar_color_available(void);    /* the board can */
bool ar_set_fullscreen(bool on);  /* what F11 does; false if impossible */
/* While a game takes typed letters (initials for a high score), C and M
 * are letters, not the colour and music keys. */
void ar_text_input(bool on);
void ar_set_color(bool on);       /* what C does, in full screen */

/* -- players -- */

uint32_t ar_held(int player);              /* AR_BTN_* down now */
uint32_t ar_pressed(int player);           /* went down since last frame */
uint32_t ar_any_held(void);
uint32_t ar_any_pressed(void);
bool ar_pad_present(int player);           /* that player's gamepad */

/* Hold-to-repeat, for menus and sideways movement: true on the press,
 * then after `delay` frames every `rate` frames while held. One per
 * button the game wants to repeat; call once per frame. */
typedef struct {
	uint16_t count;
	bool was;
} ar_repeat_t;

bool ar_repeat(ar_repeat_t *r, bool held, int delay, int rate);

/* -- dirty rectangles -- */

void ar_dirty(int x, int y, int w, int h);
void ar_dirty_all(void);

/* -- drawing (inside paint(), or any time for immediate drawing) --
 *
 * Logical coordinates, colour indices. Clipped to the play area and,
 * inside paint(), to the rectangle being painted. */

void ar_fill(int x, int y, int w, int h, int c);

/* Two colours through a pattern: c1 where `pat` has a bit, c0 where it
 * has not. Screen-aligned like every pattern. In monochrome it picks
 * the pattern or a plain ink, whichever keeps the two apart. */
void ar_fill2(int x, int y, int w, int h, int c0, int c1, const ar_ink_t *pat);

/* A 1bpp bitmap (zgfx's layout: LSB leftmost, rows `stride` bytes,
 * stride a multiple of 4) drawn in colour c where its bits are set,
 * transparent elsewhere. */
void ar_bitmap(const void *bits, int stride, int sx, int sy,
	int x, int y, int w, int h, int c);

/* Text in colour c, transparent elsewhere, scaled by 1..4 (each pixel
 * becomes scale x scale). Returns the width drawn. */
int ar_text(int x, int y, const char *s, int c, const z_font_t *f, int scale);
int ar_text_width(const char *s, const z_font_t *f, int scale);

/* -- sprites --
 *
 * Built once from indexed pixels, kept as bitplanes plus a mask plus a
 * monochrome rendering, and drawn masked (atomic where the blitter has
 * its one-pass mode). The pixel source is rows of characters:
 * '.' or ' ' transparent, '0'..'9' and 'a'..'f' a colour index. */
typedef struct {
	int16_t w, h, stride;
	uint32_t *mask;
	uint32_t *plane[4];   /* NULL where the plane has no bits */
	uint32_t *mono;       /* inks applied at sprite-local coordinates */
} ar_sprite_t;

/* From `rows` (h strings of w chars). False if the sprite pool is full.
 * `recolor`, if not NULL, maps each index to another (16 entries) --
 * one drawing, several colourings. */
bool ar_sprite_make(ar_sprite_t *s, const char *const *rows, int w, int h,
	const uint8_t *recolor);

/* From an index array, w*h bytes, 0xff = transparent. */
bool ar_sprite_from_pixels(ar_sprite_t *s, const uint8_t *px, int w, int h);

void ar_sprite(const ar_sprite_t *s, int x, int y);

/* Part of one: the w x h rectangle at (sx, sy) inside it, drawn with
 * its top-left at (x, y). For a sprite that passes behind something
 * painted -- the part still in front is drawn, not all or nothing. */
void ar_sprite_part(const ar_sprite_t *s, int sx, int sy, int w, int h,
	int x, int y);

/* -- overlays: drawn every frame, erased by the runtime -- */

void ar_ov_fill(int x, int y, int w, int h, int c);
void ar_ov_sprite(const ar_sprite_t *s, int x, int y);
void ar_ov_sprite_part(const ar_sprite_t *s, int sx, int sy, int w, int h,
	int x, int y);
void ar_ov_bitmap(const void *bits, int stride, int sx, int sy,
	int x, int y, int w, int h, int c);

/* Keep overlays inside a rectangle: a sprite partly outside it is cut
 * at its edge rather than drawn over what is outside (snake's status
 * line, while a head slides out of the top of a wrapping field). The
 * whole picture by default. */
void ar_ov_area(int x, int y, int w, int h);

/* A text label rendered once into the caller's storage and drawn as an
 * overlay as often as wanted. `buf` holds stride*h bytes. */
typedef struct {
	uint32_t *bits;
	int16_t w, h, stride;
} ar_label_t;

bool ar_label_make(ar_label_t *l, uint32_t *buf, int buf_bytes,
	const char *s, const z_font_t *f, int scale);
void ar_ov_label(const ar_label_t *l, int x, int y, int c);

/* -- particles --
 *
 * Sparks, debris, confetti: 2x2 overlays with a velocity, an optional
 * pull downward and a lifetime, drawn in colours c0 .. c0+nc-1 (a
 * palette range that can flicker). Up to AR_MAX_PARTS at once; a burst
 * past that replaces the oldest. */
#define AR_MAX_PARTS 64

/* n sparks from (x, y), at up to `speed` pixels a frame (8.8 fixed:
 * 256 = one pixel), living `life` frames, falling by `gravity` (8.8 per
 * frame per frame, 0 for none). */
void ar_parts_burst(int x, int y, int n, int c0, int nc, int speed,
	int life, int gravity);
/* Step dt frames and submit the overlays. Once a frame. */
void ar_parts_frame(uint32_t dt);
void ar_parts_clear(void);
int ar_parts_live(void);

/* -- screen-wide effects -- */

/* Move a rectangle of the picture (VRAM to VRAM, in every plane). False
 * if it cannot be done here -- a window partly covered, say -- and the
 * caller should repaint the destination instead. */
bool ar_copy(int sx, int sy, int dx, int dy, int w, int h);

/* Shake the whole picture: up to `amp` pixels, decaying over `frames`.
 * Full screen only (the viewport register moves; nothing is redrawn).
 * Keep the outer `amp` pixels of the picture in colour 0 if the edges
 * must stay clean while it shakes. */
void ar_shake(int amp, int frames);

/* Palette entries, written at the next frame boundary. Colour only;
 * kept and reloaded when colour comes back on. */
void ar_pal(int i, uint16_t rgb444);
uint16_t ar_pal_get(int i);
void ar_pal_load(const uint16_t *rgb, int n);

/* Mix two RGB444 colours, t = 0..256. */
uint16_t ar_rgb_mix(uint16_t a, uint16_t b, int t);

/* -- scores --
 *
 * A top-ten table per game and per "mode" (the game's own integer), in
 * /data/<app>/scores.txt, one line per entry: "mode score initials". */
#define AR_SCORES 10

typedef struct {
	uint32_t score;
	char who[4];         /* three initials, NUL-terminated */
} ar_score_t;

typedef struct {
	ar_score_t e[AR_SCORES];
	int n;
} ar_table_t;

void ar_scores_load(int mode, ar_table_t *t);
bool ar_scores_save(int mode, const ar_table_t *t);
/* Where `score` would go (0..AR_SCORES-1), or -1 if it would not. */
int ar_scores_rank(const ar_table_t *t, uint32_t score);
/* Insert at its rank; returns the rank or -1. */
int ar_scores_insert(ar_table_t *t, uint32_t score, const char *who);

/* -- the text format, for the host tests -- */
int ar_scores_parse(const char *text, int mode, ar_table_t *t);
int ar_scores_format(char *out, int cap, const char *old_text, int mode,
	const ar_table_t *t);

/* -- small helpers every game wants -- */

uint32_t ar_rand(void);            /* xorshift, seeded at ar_init() */
uint32_t ar_rand_below(uint32_t n);
void ar_seed(uint32_t s);

/* Decimal, right-aligned in `width` with `pad` ('0' or ' '). */
void ar_utoa(char *out, uint32_t v, int width, char pad);

#endif
