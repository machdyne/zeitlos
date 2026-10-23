#ifndef ZARCADE_INT_H
#define ZARCADE_INT_H

/*
 * Zeitlos -- the arcade runtime's insides, shared by its two halves:
 *
 *   zarcade.c       everything that is arithmetic: dirty tiles,
 *                   overlays, colour and ink expansion, sprites, text,
 *                   the player buttons, scores, the palette and the
 *                   shake. No Zeitlos headers; the host tests link it.
 *   zarcade_sys.c   everything that talks to the machine: the window,
 *                   wm's messages, game mode, the frame counter, the
 *                   files, and the blitter (the arh_* calls below).
 *
 * tests/ar_host.c is a third implementation of the second half -- a
 * software framebuffer with a palette and a viewport -- which is how
 * the host tests draw real frames and write them out as pictures.
 */

#include "zarcade.h"

/* Rectangles of the screen plane 0 may be drawn into, in framebuffer
 * coordinates: the parts of the window nobody covers, or the whole
 * page in full screen. */
#define AR_MAX_VIS 16

typedef struct {
	const ar_desc_t *d;
	ar_mode_t mode;

	int ox, oy;                    /* framebuffer position of logical 0,0 */
	ar_rect_t vis[AR_MAX_VIS];     /* where drawing may land (plane 0) */
	int nvis;
	bool vis_whole;                /* one rectangle, the whole play area */

	bool can_draw;                 /* false while wm has us frozen */
	bool targets_stale;            /* the region changed: reload vis[] */
	bool want_color;               /* the C key's state */
	bool text_input;               /* C and M are the game's */
	bool wc_off;                   /* F10: no colour window, for now */
	bool modal;                    /* a box is up (ar_top): Esc, F1, C, M
	                                * are the program's keys */

	/* Colour in the window (docs/color.md, "The colour window"): wm
	 * gave this window the second VRAM sheet's planes. Logical plane 0
	 * is the window's own pixels (the desktop's bit), planes 1..wl-1
	 * the window's planes, at their folded places in rows 480 up
	 * (rows1: the second sheet's height, 240 or 480). */
	bool wcolor;
	int wl;
	int wl_hw;                     /* the window's bits in hardware:
	                                  1 + its planes (5 or 3) */
	int rows1;
	bool verifying;                /* host check: a repaint, not a frame */

	uint16_t pal[256];
	uint32_t pal_dirty[8];         /* one bit per entry */
	int colours_want;              /* ar_set_colours(): 16, 64, 256 */
	int planes;                    /* in colour: 4, 6 or 8 */
	const uint8_t *mono_map;       /* ar_mono_map() */

	int shake_amp, shake_left, shake_total;
	int view_dx, view_dy;          /* this frame's shake offset */

	uint32_t ticks;
} ar_state_t;

extern ar_state_t ar__s;

/* -- core, called by the system half -- */

/* Planes for colour on a board that shows at most `hw_max` (4, 6, 8),
 * from what the program asked: sets ar__s.planes and returns it. */
int ar__choose_planes(int hw_max);
/* Plane k's framebuffer offset from plane 0 (the z_color_16, _64 and
 * _256 layouts: quadrants, and planes 4.. 480 rows down). */
void ar__plane_off(int k, int *dx, int *dy);
/* Every palette entry to be written again. */
void ar__pal_all_dirty(void);

void ar__core_init(const ar_desc_t *d);
/* Repaint what is dirty and draw what changed. vis/ox/oy set first. */
void ar__render(void);
/* Forget what is on screen: everything dirty, no overlay is "already
 * drawn". After a mode change or a wm redraw. */
void ar__invalidate(void);
/* A key event from wm. Returns true if the runtime used it. */
void ar__key(uint32_t keysym, bool pressed, uint8_t mods);
/* Once a frame: read the pads, make their edges into events, start a
 * new frame of "pressed". */
void ar__frame_input(void);
/* Colour in the window on or off (docs/color.md, "The colour
 * window"): `extra` window planes (2 or 4) in a second sheet `rows1`
 * high. Repaints, and AR_EV_MODE when it changes. */
void ar__wcolor(bool on, int extra, int rows1);

/* Advance the shake: sets view_dx/view_dy. */
void ar__shake_step(void);
void ar__queue(const ar_event_t *e);
bool ar__dequeue(ar_event_t *e);

/* -- the machine, implemented by zarcade_sys.c or tests/ar_host.c --
 *
 * Framebuffer coordinates. Rectangles handed to these are already
 * clipped to the framebuffer and to one visible rectangle. */

#define ARH_COPY 0
#define ARH_OR   1
#define ARH_ANDN 3

/* arh_blit() can do ARH_COPY (the blitter has its ROPs) */
bool arh_copy_ok(void);

void arh_fill(int x, int y, int w, int h, int bit);
void arh_pattern(int x, int y, int w, int h, const uint8_t *pat8);
void arh_blit(const void *src, int stride, int sx, int sy,
	int x, int y, int w, int h, int rop);
void arh_sprite(const void *data, const void *mask, int stride,
	int sx, int sy, int x, int y, int w, int h);
void arh_vcopy(int sx, int sy, int dx, int dy, int w, int h);
/* A line, framebuffer coordinates, clipped to the inclusive rectangle
 * cx0..cx1, cy0..cy1: op 0 clear, 1 set, 2 XOR. */
void arh_line(int x0, int y0, int x1, int y1, int op,
	int cx0, int cy0, int cx1, int cy1);

/* Reload vis/ox/oy/can_draw for the current mode and window region.
 * Called by the system half at every frame, and by zarcade.c before
 * drawing outside a frame when targets_stale is set. */
void arh_targets(void);

uint32_t arh_pad(int player);         /* AR_BTN_* bits, 0 if absent */
bool arh_pad_present(int player);

/* /data/<app>/scores.txt */
int arh_read_scores(char *buf, int cap);
bool arh_write_scores(const char *buf, int len);

/* /data/<app>/settings.txt (the skill) */
int arh_read_settings(char *buf, int cap);
bool arh_write_settings(const char *buf, int len);

/* apps.arcade.skill from the configuration: AR_SKILL_*, or -1 unset */
int arh_cfg_skill(void);

/* Speech, interrupting whatever was being said; silent without tts. */
void arh_say(const char *text);

/* The skill is read once, at setup (both halves call it). */
void ar__skill_load(void);

uint32_t arh_seed(void);

#endif
