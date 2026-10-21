/*
 * mausrun -- a side-scrolling run-and-jump demo for Zeitlos game mode.
 *
 * You are a mouse. You collect cheese, jump over boxes and pits, and
 * avoid cats. The level is 2048 pixels long against a 320 pixel
 * viewport, which is the entire reason this exists: it exercises the
 * scrolling machinery rather than fitting neatly on one screen.
 *
 * What it demonstrates, and where each piece lives:
 *
 *   game mode + pixel doubling   rtl/gpu/gpu_video.v, via zgame.h
 *   toroidal horizontal scroll   z_game_fold(), one camera register
 *   double buffering             z_game_flip(), one register write
 *   incremental redraw           z_game_scroll_span()
 *   hardware tile blitting       z_fb_hw_blit_mem() (gpu_blit.v)
 *   software masked sprites      spr_draw() below, and see why
 *   gamepad, two ports, hotplug  zpad.h
 *   keyboard fallback            input_read() below
 *   sound and music              zsfx (sw/common/games), the mixer
 *
 * -- the frame, in order --
 *
 *   1. read input (pad or keyboard, merged)
 *   2. step the world
 *   3. repair the back page where sprites were two frames ago
 *   4. draw newly scrolled-in columns into the back page
 *   5. draw sprites into the back page
 *   6. flip
 *
 * Steps 3 and 4 are both "draw background tiles over a world-space
 * rectangle", so they are one function. Step 3 is what makes double
 * buffering work with incremental redraw: the back page still holds
 * the sprites drawn into it LAST time it was the back page, which is
 * two frames ago, and nothing else will erase them.
 *
 * -- why sprites are drawn in software --
 *
 * gpu_blit.v's copy mode overwrites the destination rectangle
 * wholesale; it has no raster op. A mouse blitted over a brick wall
 * would arrive inside an opaque 16x16 box of its own background. So
 * sprites carry a mask plane and are composited here as
 * (dst & ~mask) | data.
 *
 * Tiles have no such problem -- every pixel of a tile cell belongs to
 * that tile -- so they go through the hardware blitter, which is the
 * right split: hundreds of tiles per frame on the fast path, a handful
 * of sprites on the slow one. If gpu_blit.v ever grows an OR/mask op
 * (see docs/game_mode.md), spr_draw() becomes a hardware call and
 * nothing else here changes.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zwm.h"
#include "../../common/zgame.h"
#include "../../common/zgfx.h"
#include "../../common/zcolor.h"
#include "../../common/zpad.h"
#include "../../common/zkbd.h"
#include "../../common/zaudio.h"
#include "../../common/zfont.h"
#include "../../common/zspeak.h"
#include "../../common/zcfg.h"
#include "../../common/zfsapp.h"
#include "../../common/zdata.h"
#include "../../common/zpaths.h"

#include "sprites.h"
#include "sound.h"
#include "level.h"
#include "phys.h"
#include "progress.h"

#define VRAM_BASE 0x20000000u
#define FB_STRIDE_WORDS 20          /* 640 px / 32 bits */

/* Grey level for the sub-surface, 0..Z_SHADE_MAX. High enough to read
 * as solid ground at a glance, textured enough that the ground is not
 * a flat black slab. */
#define MR_GROUND_SHADE 13

/* Blitter operations this frame: printed with MAUSRUN_DEBUG. It used
 * to be on the HUD, with dt (display frames per update) beside it; the
 * HUD is the player's now, and the console the developer's. */
static uint32_t hud_blits;

/* Physics, units and the actor: phys.h, shared with the host tools.
 * The cats' pace is the skill's (cat_speed below). */
static const int8_t cat_speeds[MR_SKILLS] = { 6, 9, 12 };
static int cat_speed = 9;

/* -- who lives in a level (docs/mausrun.md, "Who lives there") --
 *
 * A foe is an actor with a kind and a state. Jumping on one never hurts
 * it: it goes dizzy (stars), then naps (zzz), then gets up again. Only
 * a foe that is up and about hurts, and the bell sends those running. */
enum {
	FS_WALK = 0,     /* a cat walking its beat */
	FS_DIZZY,        /* jumped on: stars round its head */
	FS_NAP,          /* asleep after the dizziness, for a while */
	FS_SLEEP,        /* a sleeping cat: until the mouse lands too near */
	FS_SIT,          /* a frog between hops */
	FS_HOP,          /* a frog in the air */
	FS_FLY           /* an owl on its beat */
};

typedef struct {
	mr_actor_t a;
	uint8_t kind;              /* MR_FOE_* */
	uint8_t state;             /* FS_* */
	int16_t timer;             /* frames left in the state */
	int16_t dir;               /* frog and owl: which way next */
	int32_t home_x, home_y;    /* pixels: where it lives */
} mr_foe_t;

static mr_actor_t player;
static mr_foe_t foes[MR_MAX_FOES];
static int foe_count;

static z_game_t game;

static int score;
static int lives;
static int cheese;           /* collected in this level */
static uint32_t level_frames;
static bool sound_ok;
static bool game_over;
static bool won;

/* -- the skill's say in it (docs/mausrun.md, "Skill") -- */
static const int16_t nap_frames[MR_SKILLS] = { 600, 420, 300 };
static const int16_t power_frames[MR_SKILLS] = { 1800, 1200, 900 };
static const int8_t wake_dist[MR_SKILLS] = { 24, 40, 56 };
static const int8_t crumble_frames[MR_SKILLS] = { 60, 40, 30 };
static const int8_t music_base[MR_SKILLS] = { 1, 3, 5 };
#define MR_HEARTS_MAX   7
#define MR_DIZZY_FRAMES 90
#define MR_HURT_FRAMES  120      /* blinking, and nothing hurts */

/* -- the power-ups: four on a clock, and the bubble until it pops -- */
enum { PW_FEATHER = 0, PW_MAGNET, PW_BELL, PW_SNEAKERS, PW_TIMED };
static int16_t pw_left[PW_TIMED];
static bool pw_bubble;
static int hurt_frames;
static int32_t check_x, check_y;  /* pixels: where the mouse comes back */
static int32_t goal_x;            /* pixels: the cheese wheel, or the end */
static bool star_found;           /* this level's hidden star */
static uint8_t clear_stars;       /* MR_STAR_* won at the end of a level */
static int stars_shown;           /* the results: lit so far */
static uint32_t idle_frames;      /* standing still: the easy arrow */
static int qbox_next;

/* -- the screens --
 *
 *   TITLE    the menu over the level 1 scenery, the camera drifting
 *   INTRO    "LEVEL 2", the level's name (spoken), then play
 *   PLAY
 *   PAUSE    P, Start or Esc; Esc again goes back to the title
 *   DYING    a second, then the start of the level again
 *   CLEAR    the level's results, counted up
 *   OVER     out of hearts: try the level again, or the title
 *   DONE     every level finished
 */
typedef enum {
	ST_TITLE = 0, ST_INTRO, ST_PLAY, ST_PAUSE, ST_DYING, ST_CLEAR, ST_OVER,
	ST_DONE
} mr_state_t;

static mr_state_t st;
static uint32_t st_frames;
static int level_no = 1;      /* the level in play, 1.. */
static int level_count = 1;   /* levels there are */
static int skill = MR_SKILL_EASY;
static mr_progress_t prog;
static int menu;              /* the title's line */
static int count_shown;       /* CLEAR: cheese counted so far */

static const uint8_t lives_for[MR_SKILLS] = { 5, 3, 3 };

/* -- damage tracking: GONE --
 *
 * This file used to track, per page, where sprites had been drawn two
 * frames ago, and repair those rectangles before drawing the next
 * frame. That was correct and necessary when the background scrolled
 * incrementally.
 *
 * Parallax killed it. A cloud moving at a quarter of the camera speed
 * exposes background that the tilemap walk has no reason to think is
 * dirty, so the "only redraw what scrolled in" premise stops holding.
 * And at 30-plus ambient sprites, repairing each one would touch most
 * of the screen anyway.
 *
 * So the whole mechanism went, and the renderer got simpler rather
 * than more complex: fill the sky, draw the far layers, draw the
 * non-empty tiles over them, draw the near sprites. Nothing needs to
 * remember anything between frames.
 */

/* -- input --
 *
 * One struct fed by EITHER a gamepad or the keyboard, merged rather
 * than switched between. A machine with no pad plugged in must still
 * be playable, and a machine with one should not stop responding to
 * the keyboard -- so both are read every frame and OR'd. There is no
 * mode to get stuck in and no "press a key to select input" screen. */
typedef struct {
	bool left, right, jump;
	bool jump_edge;      /* jump pressed THIS frame */
	/* pressed this frame (edges), for menus and the toggles */
	bool up, down, lpress, rpress, ok, back, pause;
	bool vol_down, vol_up;
	bool tog_col, tog_music;
} mr_input_t;

/* -- reading the keyboard as LEVELS, not events --
 *
 * This deliberately does NOT use hid_read_key().
 *
 * That ring is POPPED, and sw/apps/wm drains it too. Every event this
 * process takes is one wm never sees, and vice versa -- the two race
 * for one queue and each gets roughly the half the other did not.
 *
 * For a game that is not merely unfair, it is broken in a specific and
 * maddening way. Miss a PRESS and the key never registers, which is
 * annoying. Miss a RELEASE and the key is stuck down FOREVER -- so the
 * jump edge never fires again and jumping stops working entirely,
 * until some later press/release pair happens to arrive intact.
 *
 * "Space only jumps sometimes" is exactly that, and it looks like a
 * lost interrupt or a full FIFO, which it is not.
 *
 * The fix is to stop consuming events at all. rtl/usb_hid.v's report
 * registers hold the CURRENT state -- up to four held keys plus the
 * modifier byte, overwritten in place by hardware on every report. It
 * is a level, readable as often as you like, and reading it consumes
 * nothing. wm keeps every event; this reads the same hardware wm's
 * kernel-side ISR reads.
 *
 * That is the right shape for a game regardless of the sharing
 * problem: a game asks "is this key down right now", and rebuilding a
 * level out of an edge stream is inventing state the hardware already
 * has.
 *
 * Which port is the keyboard is not fixed anywhere -- see
 * docs/gamepad.md. Both are checked, exactly as zpad.h does for pads.
 *
 * The one thing this cannot see is a key pressed AND released entirely
 * between two polls. At 60Hz that is a 16ms keystroke; nobody types
 * that fast and no game needs it.
 */
#define MR_HID_TYP_KEYBOARD 1

static bool kbd_held(uint8_t usage) {

	for (int port = 0; port < 2; port++) {

		uint32_t info = port ? reg_usb1_info : reg_usb0_info;
		uint32_t keys;

		/* bits [25:24] of the packed info word -- see rtl/usb_hid.v.
		 * NOT [23:22], which falls inside the constant padding and
		 * reads zero regardless of what is plugged in; that exact
		 * slip was a real bug in sw/os/hid.c once. */
		if (((info >> 24) & 0x3u) != MR_HID_TYP_KEYBOARD) continue;

		keys = port ? reg_usb1_keys : reg_usb0_keys;

		if ((uint8_t)(keys >> 24) == usage) return true;
		if ((uint8_t)(keys >> 16) == usage) return true;
		if ((uint8_t)(keys >> 8)  == usage) return true;
		if ((uint8_t)(keys)       == usage) return true;

	}

	return false;

}

/* USB HID usage codes -- see sw/common/zkbd.h. Used raw rather than
 * translated to keysyms because this app wants the physical key held,
 * not a character: z_kbd_usage_to_keysym() answers "what would this
 * type", which is the wrong question for a movement key, and holding
 * a key produces no repeated events to translate anyway. */
#define HID_LEFT   0x50
#define HID_RIGHT  0x4F
#define HID_UP     0x52
#define HID_DOWN   0x51
#define HID_SPACE  0x2C
#define HID_ENTER  0x28
#define HID_KPENTER 0x58
#define HID_ESC    0x29
#define HID_MINUS  0x2D    /* - and _ */
#define HID_EQUAL  0x2E    /* = and + */
#define HID_C      0x06    /* c -- colour on or off */
#define HID_M      0x10    /* m -- music on or off */
#define HID_P      0x13    /* p -- pause */

/* Levels this frame and last, as bits, so a press is an edge. */
enum {
	K_LEFT = 1 << 0, K_RIGHT = 1 << 1, K_UP = 1 << 2, K_DOWN = 1 << 3,
	K_JUMP = 1 << 4, K_OK = 1 << 5, K_BACK = 1 << 6, K_PAUSE = 1 << 7,
	K_VDOWN = 1 << 8, K_VUP = 1 << 9, K_COL = 1 << 10, K_MUS = 1 << 11
};
static uint32_t keys_prev;

static void input_read(mr_input_t *in) {

	uint32_t pad, k = 0, e;

	if (kbd_held(HID_LEFT)) k |= K_LEFT;
	if (kbd_held(HID_RIGHT)) k |= K_RIGHT;
	if (kbd_held(HID_UP)) k |= K_UP | K_JUMP;
	if (kbd_held(HID_SPACE)) k |= K_JUMP;
	if (kbd_held(HID_DOWN)) k |= K_DOWN;
	if (kbd_held(HID_ENTER) || kbd_held(HID_KPENTER)) k |= K_OK;
	if (kbd_held(HID_ESC)) k |= K_BACK;
	if (kbd_held(HID_P)) k |= K_PAUSE;
	if (kbd_held(HID_MINUS)) k |= K_VDOWN;
	if (kbd_held(HID_EQUAL)) k |= K_VUP;
	if (kbd_held(HID_C)) k |= K_COL;
	if (kbd_held(HID_M)) k |= K_MUS;

	/* Pad 0, whichever port it is in. Reads 0 when absent, which is
	 * exactly "nothing pressed" -- so no special case is needed for a
	 * machine with no pad, and unplugging one mid-jump releases every
	 * button rather than leaving the mouse running forever. See
	 * docs/gamepad.md. */
	pad = z_pad_read(0);
	if (pad & Z_PAD_LEFT)  k |= K_LEFT;
	if (pad & Z_PAD_RIGHT) k |= K_RIGHT;
	if (pad & Z_PAD_UP)    k |= K_UP | K_JUMP;
	if (pad & Z_PAD_DOWN)  k |= K_DOWN;
	if (pad & (Z_PAD_A | Z_PAD_B)) k |= K_JUMP | K_OK;
	if (pad & Z_PAD_START) k |= K_PAUSE | K_OK;
	if (pad & Z_PAD_SELECT) k |= K_BACK;

	e = k & ~keys_prev;
	keys_prev = k;

	in->left = (k & K_LEFT) != 0;
	in->right = (k & K_RIGHT) != 0;
	in->jump = (k & K_JUMP) != 0;
	in->jump_edge = (e & K_JUMP) != 0;
	in->up = (e & K_UP) != 0;
	in->down = (e & K_DOWN) != 0;
	in->lpress = (e & K_LEFT) != 0;
	in->rpress = (e & K_RIGHT) != 0;
	in->ok = (e & (K_OK | K_JUMP)) != 0 && !(e & K_UP);
	in->back = (e & K_BACK) != 0;
	in->pause = (e & K_PAUSE) != 0;
	in->vol_down = (e & K_VDOWN) != 0;
	in->vol_up = (e & K_VUP) != 0;
	in->tog_col = (e & K_COL) != 0;
	in->tog_music = (e & K_MUS) != 0;
}

/* -- drawing --------------------------------------------------------
 *
 * Everything below draws in FRAMEBUFFER coordinates. The conversion
 * from world space happens in one place, world_to_fb(), so the torus
 * fold and the page offset are not scattered through the renderer.
 */

/* Framebuffer x for a world x, folded into the page's 640-column
 * torus. Note this can put the right half of a sprite at a much
 * SMALLER framebuffer x than its left half, when it straddles the
 * seam -- callers that draw spans have to handle that, which is what
 * draw_tiles_span() does by walking tile columns individually rather
 * than computing one rectangle. */
/* -- colour (docs/color.md, docs/mausrun.md "Colour") --
 *
 * On a board with game-mode colour the demo runs in four colours,
 * double-buffered, and the page layout changes to make room:
 *
 *       (0,0)    page 0, plane 0      (320,0)    page 1, plane 0
 *       (0,240)  page 0, plane 1      (320,240)  page 1, plane 1
 *
 * z_color_4_below. The monochrome layout's two 640-wide pages are the
 * room a scroll used to need; a renderer that redraws the whole frame
 * every frame (docs/mausrun.md, "Parallax deleted the damage
 * tracking") needs none, so in colour the camera is simply subtracted
 * -- framebuffer x = page x + (world x - camera) -- and there is no
 * torus at all.
 *
 * What that costs is slack: a page is exactly the viewport, so
 * anything drawn past its edge would land in the other page, which is
 * the one on screen. Every colour primitive below therefore clips to
 * the page. The monochrome path never needed to, and is unchanged.
 *
 * The art is still 1bpp. Colour is per OBJECT, not per pixel: a tile
 * is one colour where its bitmap is set and another where it is clear
 * (in bands, so grass is green blades on sky over earth), a sprite one
 * colour where its data is set and an outline colour where only its
 * mask is. With two bitplanes that is one to three blitter passes per
 * plane per object. C toggles colour at run time, for comparison. */
static bool col_on;
static int col_back;                /* page drawn next: x = 0 or 320 */
static uint32_t col_last_frame;

#define MR_C_SKY    0
#define MR_C_DARK   1
#define MR_C_GREEN  2
#define MR_C_CREAM  3

/* sky blue, dark brown, leaf green, cream (cheese, clouds, the mouse):
 * the meadow's, and each theme's own -- mr_lights[] below */

/* Rows of the grass tile that are blades over sky, before its turf --
 * see gen_sprites.py. Drawn transparent, in both modes. */
#define MR_GRASS_BLADES 3

/* How much cream speckles the earth: a dither level, 0..16. */
#define MR_EARTH_SHADE 3

/* Whether the blitter has raster ops. Set once at startup; the colour
 * path and transparent tiles depend on it, so it lives up here. */
static bool rop_hw;

/* sprite colours, set by whoever is about to call spr_draw() */
static int spr_c = MR_C_CREAM, spr_o = MR_C_DARK;

static inline int col_page_x(void) {
	return col_back * Z_GAME_VIEW_W;
}

static inline int world_to_fb_x(int32_t wx) {
	if (col_on) return col_page_x() + (int)(wx - game.cam);
	return z_game_fold(game.orient, wx);
}

static inline int world_to_fb_y(int32_t wy) {
	if (col_on) return (int)wy;
	return (int)wy + z_game_back_y(&game);
}

/* Clip a rectangle, and the source offset that goes with it, to the
 * page being drawn. False if nothing is left. */
static bool col_clip(int *x, int *y, int *w, int *h, int *sx, int *sy) {
	int x0 = col_page_x(), x1 = x0 + Z_GAME_VIEW_W;
	int y0 = 0, y1 = Z_GAME_VIEW_H;
	if (*x < x0) { *sx += x0 - *x; *w -= x0 - *x; *x = x0; }
	if (*x + *w > x1) *w = x1 - *x;
	if (*y < y0) { *sy += y0 - *y; *h -= y0 - *y; *y = y0; }
	if (*y + *h > y1) *h = y1 - *y;
	return *w > 0 && *h > 0;
}

static void c_fill(int x, int y, int w, int h, int c) {
	int sx = 0, sy = 0;
	if (!col_clip(&x, &y, &w, &h, &sx, &sy)) return;
	hud_blits += 2;
	z_color_fill_rect(&z_color_4_below, x, y, w, h, c);
}

/* A 1bpp bitmap in fg where set, TRANSPARENT where clear: per plane,
 * OR where fg has the bit and ANDN where it does not, which touches
 * only the bitmap's own pixels. One pass a plane. */
static void c_over(const void *src, int stride, int sx, int sy,
	int x, int y, int w, int h, int fg) {
	int k;
	if (!col_clip(&x, &y, &w, &h, &sx, &sy)) return;
	for (k = 0; k < 2; k++) {
		hud_blits++;
		z_color_plane_blit(&z_color_4_below, k, src, stride, sx, sy,
			x, y, w, h, ((fg >> k) & 1) ? Z_ROP_OR : Z_ROP_ANDN);
	}
}

/* A 1bpp bitmap, fg where set and bg where clear, opaque. Per plane:
 * the same bit either way is a fill; set only in fg is a copy; set
 * only in bg is a fill with the bitmap's shape then cut out of it. */
static void c_two(const void *src, int stride, int sx, int sy,
	int x, int y, int w, int h, int fg, int bg) {
	int k;
	if (!col_clip(&x, &y, &w, &h, &sx, &sy)) return;
	for (k = 0; k < 2; k++) {
		int f = (fg >> k) & 1, b = (bg >> k) & 1;
		hud_blits++;
		if (f == b) {
			z_color_plane_fill(&z_color_4_below, k, x, y, w, h, f);
		} else if (f) {
			z_color_plane_blit(&z_color_4_below, k, src, stride, sx, sy,
				x, y, w, h, Z_ROP_COPY);
		} else {
			hud_blits++;
			z_color_plane_fill(&z_color_4_below, k, x, y, w, h, 1);
			z_color_plane_blit(&z_color_4_below, k, src, stride, sx, sy,
				x, y, w, h, Z_ROP_ANDN);
		}
	}
}

/* Tiles in colour: bands of rows, each fg-on-bg. A band whose bg is
 * the sky is drawn TRANSPARENT instead (c_over()): "sky" in a tile is
 * really "nothing here", and painting it opaque punched a sky-coloured
 * hole through whatever the ambient layer had drawn behind -- the foot
 * of a tree behind the grass blades, a cloud behind a floating cheese. */
typedef struct { uint8_t y0, y1, fg, bg; } mr_band_t;

static const mr_band_t band_ground[] = { { 0, 16, MR_C_DARK, MR_C_CREAM } };
static const mr_band_t band_grass[]  = {
	{ 0, 5, MR_C_GREEN, MR_C_SKY },         /* blades on sky, then turf */
	{ 5, 16, MR_C_DARK, MR_C_CREAM }        /* earth, as GROUND */
};
static const mr_band_t band_box[]    = { { 0, 16, MR_C_DARK, MR_C_CREAM } };
static const mr_band_t band_cheese[] = { { 0, 16, MR_C_CREAM, MR_C_SKY } };

/* round 3 */
static const mr_band_t band_spring[] = {
	{ 0, 5, MR_C_CREAM, MR_C_SKY },         /* the plate */
	{ 5, 16, MR_C_DARK, MR_C_SKY }          /* the coil */
};
static const mr_band_t band_shelf[]  = {
	{ 0, 3, MR_C_DARK, MR_C_CREAM },        /* the board */
	{ 3, 6, MR_C_DARK, MR_C_SKY }           /* its brackets */
};
static const mr_band_t band_qbox[]   = { { 0, 16, MR_C_GREEN, MR_C_CREAM } };
static const mr_band_t band_thorns[] = { { 0, 16, MR_C_GREEN, MR_C_SKY } };
static const mr_band_t band_water[]  = { { 0, 16, MR_C_CREAM, MR_C_GREEN } };
static const mr_band_t band_dark[]   = { { 0, 16, MR_C_DARK, MR_C_SKY } };
/* round 4 */
static const mr_band_t band_plank[]  = {
	{ 0, 3, MR_C_DARK, MR_C_CREAM },        /* the board */
	{ 3, 5, MR_C_DARK, MR_C_SKY }           /* its feet */
};

/* -- tile strips: a run of the same tile is one draw --
 *
 * Every tile also exists as a strip of MR_STRIP_TILES copies side by
 * side, made once at start (strips_init()). A run of identical tiles in
 * a map row -- twenty grass tiles along the ground -- is then drawn
 * from the strip in one blit a band and plane, not one a tile: in
 * colour, five blits for the whole row where it was a hundred. The
 * single biggest share of a colour frame was exactly that (docs/
 * mausrun.md, "Colour, and what a frame costs"). */
#define MR_STRIP_TILES  21                   /* a view's width and one */
#define MR_STRIP_STRIDE 48                   /* 336 px: 11 words, + 1 the blitter may read */
static uint32_t mr_strips[MR_TILE_COUNT][MR_TILE_H * (MR_STRIP_STRIDE / 4)];

static void strips_init(void) {
	int t, r, k;
	memset(mr_strips, 0, sizeof(mr_strips));
	for (t = 0; t < MR_TILE_COUNT; t++)
		for (r = 0; r < MR_TILE_H; r++) {
			uint32_t v = mr_tiles[t][r] & 0xffffu;
			for (k = 0; k < MR_STRIP_TILES; k++) {
				int x = k * MR_TILE_W;
				mr_strips[t][r * (MR_STRIP_STRIDE / 4) + (x >> 5)] |= v << (x & 31);
			}
		}
}

/* n tiles of one kind side by side, 1..MR_STRIP_TILES. */
static void c_tile_n(uint8_t t, int x, int y, int nt) {
	const void *src = mr_strips[t];
	const mr_band_t *b;
	int n, i;
	switch (t) {
		case MR_TILE_GROUND: b = band_ground; n = 1; break;
		case MR_TILE_GRASS:  b = band_grass;  n = 2; break;
		case MR_TILE_BOX:    b = band_box;    n = 1; break;
		case MR_TILE_CRUMBLE: case MR_TILE_CRACKED: case MR_TILE_USEDBOX:
		case MR_TILE_CHIMNEY:
			b = band_box; n = 1; break;
		case MR_TILE_FLOOR: case MR_TILE_ROOF:
			b = band_ground; n = 1; break;
		case MR_TILE_PLATFORM: b = band_plank; n = 2; break;
		case MR_TILE_TRAP:   b = band_dark;   n = 1; break;
		case MR_TILE_STAR:   b = band_cheese; n = 1; break;
		case MR_TILE_SPRING: b = band_spring; n = 2; break;
		case MR_TILE_ONEWAY: b = band_shelf;  n = 2; break;
		case MR_TILE_QBOX:   b = band_qbox;   n = 1; break;
		case MR_TILE_THORNS: b = band_thorns; n = 1; break;
		case MR_TILE_WATER:  b = band_water;  n = 1; break;
		case MR_TILE_MAGNET: case MR_TILE_SNEAKER: case MR_TILE_HEART:
			b = band_dark; n = 1; break;
		case MR_TILE_CHEESE: case MR_TILE_GOAL: case MR_TILE_FEATHER:
		case MR_TILE_BUBBLE: case MR_TILE_BELL:
			b = band_cheese; n = 1; break;
		case MR_TILE_HOLE: case MR_TILE_HOLE_LIT:
			/* a green mound, dark inside; the lit one has a cheese in
			 * it, whose pixels are all within columns 3..12 */
			for (i = 1; i < nt; i++) c_tile_n(t, x + i * MR_TILE_W, y, 1);
			c_fill(x + 2, y + 7, 12, 8, MR_C_DARK);
			c_over(mr_tiles[MR_TILE_HOLE], MR_TILE_STRIDE, 0, 0, x, y,
				MR_TILE_W, MR_TILE_H, MR_C_GREEN);
			if (t == MR_TILE_HOLE_LIT)
				c_over(mr_tiles[t], MR_TILE_STRIDE, 3, 9, x + 3, y + 9, 10, 5,
					MR_C_CREAM);
			return;
		default: return;
	}
	for (i = 0; i < n; i++) {
		if (b[i].bg == MR_C_SKY)
			c_over(src, MR_STRIP_STRIDE, 0, b[i].y0, x, y + b[i].y0,
				MR_TILE_W * nt, b[i].y1 - b[i].y0, b[i].fg);
		else
			c_two(src, MR_STRIP_STRIDE, 0, b[i].y0, x, y + b[i].y0,
				MR_TILE_W * nt, b[i].y1 - b[i].y0, b[i].fg, b[i].bg);
	}
}

/* A masked sprite: data pixels in c, mask-only pixels (the outline) in
 * o, everything else untouched. Per plane, the fewest passes that do
 * it -- the colour's bit and the outline's bit for that plane decide:
 *
 *   both 1      OR the mask                       one pass
 *   both 0      ANDN the mask                     one pass
 *   colour 1    ANDN the mask, OR the data        two
 *   outline 1   OR the mask, ANDN the data        two
 *
 * (it was always "ANDN the mask" first and then up to two more: a cat,
 * dark on dark, took three where two do). A sprite with no outline --
 * its mask IS its data, and the caller says so by passing the same
 * pointer for both -- is one pass a plane whatever its colour. Any
 * size, any stride: the far layer's clouds are drawn 32 wide and its
 * trees 32 tall, whole. */
static void c_sprite_wh(const void *data, const void *mask, int stride,
	int x, int y, int w, int h, int c, int o) {
	int k, sx = 0, sy = 0;
	const z_color_layout_t *l = &z_color_4_below;
	if (!col_clip(&x, &y, &w, &h, &sx, &sy)) return;
	for (k = 0; k < 2; k++) {
		int ck = (c >> k) & 1, ok = (o >> k) & 1;
		hud_blits++;
		if (ck == ok || data == mask) {
			z_color_plane_blit(l, k, mask, stride, sx, sy, x, y, w, h,
				ck ? Z_ROP_OR : Z_ROP_ANDN);
			continue;
		}
		hud_blits++;
		z_color_plane_blit(l, k, mask, stride, sx, sy, x, y, w, h,
			ck ? Z_ROP_ANDN : Z_ROP_OR);
		z_color_plane_blit(l, k, data, stride, sx, sy, x, y, w, h,
			ck ? Z_ROP_OR : Z_ROP_ANDN);
	}
}

/* The sub-surface: dark earth with cream speckles -- plane 0 solid,
 * plane 1 a screen-aligned dither, so colours 1 and 3 mixed with no
 * blit per tile, as the monochrome path's shaded fill does. */
static void c_earth(int x, int y, int w, int h) {
	int sx = 0, sy = 0, px, py;
	if (!col_clip(&x, &y, &w, &h, &sx, &sy)) return;
	hud_blits += 2;
	z_color_plane_fill(&z_color_4_below, 0, x, y, w, h, 1);
	z_color_plane_xy(&z_color_4_below, 1, x, y, &px, &py);
	z_fb_hw_fill_shade(px, py, w, h, MR_EARTH_SHADE);
}

/* -- palette effects: what four colours can do when they keep changing --
 *
 * docs/mausrun.md, "Palette effects". The demo's job is to show off
 * the hardware, and the one thing the palette does that no amount of
 * blitting can is change the colour of everything on screen at once,
 * for free: four register writes a frame, zero blits, no matter how
 * much of the picture they touch. Everything here is that.
 *
 *   time of day   the four colours drift through day, sunset, night and
 *                 dawn on a two-and-a-half-minute loop. Everything relights
 *                 together -- the sky, the clouds, the cheese, the
 *                 mouse -- because they share entries, which is exactly
 *                 how light works.
 *   stars         at night only, a fixed starfield on the farthest
 *                 parallax layer.
 *   lightning     at night, now and then, the sky flashes.
 *   waterfalls    the pits are water now, falling: an 8x8 pattern fill
 *                 on each plane, scrolled a row a frame. That one is
 *                 the blitter, not the palette -- but it is drawn in
 *                 palette entries, so it relights with everything else.
 *   events        cheese flashes the sky gold, a death tints the world
 *                 red, game over drains it to grey, and winning cycles
 *                 the sky through a rainbow -- classic colour cycling.
 *
 * A level's "time" line picks a fixed day, a fixed night, or the cycle. */

/* Two and a half minutes at 60Hz. It was one minute, and the night came
 * round too often to feel like anything but flicker; a level of level 1's
 * length now sees about one change of light. */
#define MR_DAY_FRAMES 9000u

/* Keyframes: frame of the day, and the four entries (sky, dark, green,
 * cream). The palette is interpolated between neighbours, a channel at
 * a time -- four bits a channel, so a fade is up to fifteen visible
 * steps, which reads as deliberate rather than banded at this pace. */
typedef struct { uint16_t at; uint16_t c[4]; } mr_key_t;

/* Each theme lights its own four colours (round 4, docs/mausrun.md
 * "Themes"): day, sunset, night and dawn. The meadow's are the ones the
 * game always had; the garden's night keeps its greens readable; the
 * kitchen's "sky" is its wall and its night is the lamp off; the roofs'
 * dark is brick. */
typedef struct { uint16_t day[4], sunset[4], night[4], dawn[4]; } mr_light_t;

static const mr_light_t mr_lights[MR_THEMES] = {
	{ { 0x6be, 0x320, 0x4a3, 0xfe8 }, { 0xe85, 0x312, 0x372, 0xfcb },
	  { 0x114, 0x001, 0x132, 0x9ac }, { 0xa8c, 0x211, 0x483, 0xfdc } },
	{ { 0x7cf, 0x420, 0x3b4, 0xfe8 }, { 0xe96, 0x412, 0x392, 0xfdb },
	  { 0x125, 0x001, 0x3a5, 0xbcd }, { 0xa9d, 0x311, 0x4a4, 0xfdc } },
	{ { 0x9bc, 0x431, 0x4a8, 0xffe }, { 0xca8, 0x421, 0x598, 0xffd },
	  { 0x335, 0x100, 0x386, 0xdcb }, { 0xaab, 0x321, 0x498, 0xffe } },
	{ { 0x8ad, 0x521, 0x5a5, 0xffe }, { 0xd86, 0x411, 0x494, 0xfec },
	  { 0x113, 0x420, 0x4a6, 0xeed }, { 0x99c, 0x411, 0x5a5, 0xffe } },
};

/* The day's keyframes for the level's theme: day_set() fills them. */
static mr_key_t mr_day[7];
#define MR_NKEYS 7

static void day_set(int theme) {
	static const uint16_t at[7] = { 0, 4050, 4950, 5580, 7920, 8550, 9000 };
	const mr_light_t *l = &mr_lights[theme < MR_THEMES ? theme : 0];
	const uint16_t *c[7] = { l->day, l->day, l->sunset, l->night, l->night, l->dawn, l->day };
	int i;
	for (i = 0; i < 7; i++) {
		mr_day[i].at = at[i];
		memcpy(mr_day[i].c, c[i], sizeof(mr_day[i].c));
	}
}

/* Stars between these: a little wider than the night keyframes, so
 * they are out in the late sunset and still there at first light. */
#define MR_STARS_FROM 5250u
#define MR_STARS_TO   8250u

static uint32_t fx_tod = 1500;       /* frame of the day: start mid-morning */
static int fx_lightning;             /* frames left in a flash sequence */
static int fx_cheese;                /* frames of gold sky left */
static int fx_death;                 /* frames of red left */
static uint32_t fx_anim;             /* frames since start, for the water */
static uint32_t fx_rng = 0x2545F491u;

static uint32_t fx_rand(void) {
	fx_rng = fx_rng * 1664525u + 1013904223u;
	return fx_rng >> 8;
}

static bool fx_night(void) {
	return fx_tod >= MR_STARS_FROM && fx_tod < MR_STARS_TO;
}

static uint16_t lerp444(uint16_t a, uint16_t b, int t, int n) {
	int k, out = 0;
	for (k = 8; k >= 0; k -= 4) {
		int x = (a >> k) & 15, y = (b >> k) & 15;
		int v = x + ((y - x) * t + (y > x ? n / 2 : -n / 2)) / n;
		if (v < 0) v = 0;
		if (v > 15) v = 15;
		out |= v << k;
	}
	return (uint16_t)out;
}

static uint16_t grey444(uint16_t c) {
	int r = c >> 8 & 15, g = c >> 4 & 15, b = c & 15;
	int y = (2 * r + 5 * g + b + 4) / 8;
	return (uint16_t)(y << 8 | y << 4 | y);
}

/* The winning screen's rainbow, cycled through the sky entry. */
static const uint16_t mr_rainbow[12] = {
	0xf00, 0xf60, 0xfb0, 0xdf0, 0x6f0, 0x0f6,
	0x0fd, 0x0bf, 0x06f, 0x60f, 0xb0f, 0xf0b
};

static void fx_palette(uint16_t out[4]) {
	int i, k;

	/* time of day */
	for (i = 0; i + 1 < MR_NKEYS && fx_tod >= mr_day[i + 1].at; i++)
		;
	if (i + 1 >= MR_NKEYS) i = MR_NKEYS - 2;
	{
		int n = mr_day[i + 1].at - mr_day[i].at;
		int t = (int)fx_tod - mr_day[i].at;
		for (k = 0; k < 4; k++)
			out[k] = n ? lerp444(mr_day[i].c[k], mr_day[i + 1].c[k], t, n)
			           : mr_day[i].c[k];
	}

	/* lightning: bright, dark, dark, dark, bright, then out */
	if (fx_lightning > 0) {
		int f = fx_lightning;
		if (f == 6 || f == 5 || f == 1) {
			out[0] = 0xdde;
			out[3] = 0xfff;
		}
	}

	if (fx_cheese > 0)
		out[0] = lerp444(out[0], 0xfd4, fx_cheese, 8);

	if (fx_death > 0)
		for (k = 0; k < 4; k++)
			out[k] = lerp444(out[k], 0xf00, fx_death, 120);

	if (game_over)
		for (k = 0; k < 4; k++)
			out[k] = grey444(out[k]);

	if (won)
		out[0] = mr_rainbow[(fx_anim >> 2) % 12];
}

/* Once a frame, straight after the flip's frame boundary, so a change
 * can never land half way down a picture. */
static void fx_tick(uint32_t dt) {
	uint16_t pal[4];

	fx_anim += dt;
	/* the level says how the light goes: a fixed day, a fixed night,
	 * or the two-and-a-half-minute day of level 1 */
	if (mr_level_info.time == MR_TIME_DAY) fx_tod = 1500;
	else if (mr_level_info.time == MR_TIME_NIGHT) fx_tod = 6750;
	else if (!game_over && !won)
		fx_tod = (fx_tod + dt) % MR_DAY_FRAMES;

	if (fx_lightning > 0) {
		fx_lightning -= (int)dt;
		if (fx_lightning < 0) fx_lightning = 0;
	} else if (fx_night() && mr_level_info.theme != MR_THEME_KITCHEN &&
			fx_tod > MR_STARS_FROM + 240 &&
			fx_rand() % 400 < dt) {
		fx_lightning = 6;
		snd_play(SND_LAND, -1);                /* a distant thud */
	}
	if (fx_cheese > 0) fx_cheese -= (int)dt;
	if (fx_death > 0) fx_death -= (int)dt;
	if (fx_cheese < 0) fx_cheese = 0;
	if (fx_death < 0) fx_death = 0;

	fx_palette(pal);
	z_color_palette_load_now(pal, 4);
}

/* -- stars: a 320x112 1bpp sky, made once --
 *
 * One plane's worth of bitmap, drawn in cream over the sky with
 * z_color_blit_mono() -- transparent, so the clouds drawn after it
 * cover it. Two blits a plane a frame (the parallax wraps it), and
 * none at all by day. */
#define MR_STARS_H      112
#define MR_STARS_STRIDE 44                  /* 10 words + 1 the blitter may read */
static uint32_t mr_stars[MR_STARS_H * (MR_STARS_STRIDE / 4) + 1];

static void stars_init(void) {
	int i;
	memset(mr_stars, 0, sizeof(mr_stars));
	for (i = 0; i < 60; i++) {
		int x = (int)(fx_rand() % 320), y = (int)(fx_rand() % MR_STARS_H);
		mr_stars[y * (MR_STARS_STRIDE / 4) + (x >> 5)] |= 1u << (x & 31);
		/* one in six is a bright one: a small cross */
		if (i % 6 == 0 && x > 0 && x < 319 && y > 0 && y < MR_STARS_H - 1) {
			mr_stars[y * (MR_STARS_STRIDE / 4) + ((x - 1) >> 5)] |= 1u << ((x - 1) & 31);
			mr_stars[y * (MR_STARS_STRIDE / 4) + ((x + 1) >> 5)] |= 1u << ((x + 1) & 31);
			mr_stars[(y - 1) * (MR_STARS_STRIDE / 4) + (x >> 5)] |= 1u << (x & 31);
			mr_stars[(y + 1) * (MR_STARS_STRIDE / 4) + (x >> 5)] |= 1u << (x & 31);
		}
	}
}

static void c_mono(const void *src, int stride, int sx, int sy,
	int x, int y, int w, int h, int c) {
	if (!col_clip(&x, &y, &w, &h, &sx, &sy)) return;
	hud_blits += 2;
	z_color_blit_mono(&z_color_4_below, src, stride, sx, sy, x, y, w, h, c);
}

static void stars_draw(void) {
	int off, px = col_page_x();
	if (!fx_night() || mr_level_info.theme == MR_THEME_KITCHEN) return;
	off = (int)((game.cam >> 4) % 320);         /* farther than the clouds */
	c_mono(mr_stars, MR_STARS_STRIDE, off, 0, px, 0, 320 - off,
		MR_STARS_H, MR_C_CREAM);
	if (off)
		c_mono(mr_stars, MR_STARS_STRIDE, 0, 0, px + 320 - off, 0, off,
			MR_STARS_H, MR_C_CREAM);
}

/* -- waterfalls in the pits --
 *
 * A pit is a column with nothing at ground level or below. Its water
 * is two 8x8 patterns, one per plane, so each pixel is one of four
 * colours: plane 0 alone is dark (shadow lines), both planes cream
 * (foam), neither the sky's own colour (the water). Shifting both
 * patterns down a row a frame makes it fall; the fills are anchored to
 * the screen's 8-pixel grid, so adjacent pit columns join seamlessly,
 * and the pages sit at x = 0 and 320, both on that grid. */
static const uint8_t mr_fall_foam[8] = {
	0x81, 0x81, 0x80, 0x10, 0x14, 0x04, 0x40, 0x41
};
static const uint8_t mr_fall_dark[8] = {
	0x20, 0x00, 0x02, 0x22, 0x00, 0x08, 0x08, 0x00
};

static void c_pattern(int x, int y, int w, int h,
	const uint8_t *p0, const uint8_t *p1) {
	int sx = 0, sy = 0, px, py;
	if (!col_clip(&x, &y, &w, &h, &sx, &sy)) return;
	hud_blits += 2;
	z_fb_hw_fill_pattern(x, y, w, h, p0);
	z_color_plane_xy(&z_color_4_below, 1, x, y, &px, &py);
	z_fb_hw_fill_pattern(px, py, w, h, p1);
}

static void water_draw(int32_t wx0, int32_t wx1) {
	uint8_t p0[8], p1[8];
	int c = (int)(wx0 / MR_TILE_W), c1 = (int)((wx1 + MR_TILE_W - 1) / MR_TILE_W);
	int y = MR_GROUND_ROW * MR_TILE_H + 4;
	int r, s = (int)(fx_anim & 7);

	/* plane 0 = foam or shadow, plane 1 = foam: so foam is 3, shadow 1 */
	for (r = 0; r < 8; r++) {
		int src = (r - s) & 7;
		p0[r] = (uint8_t)(mr_fall_foam[src] | mr_fall_dark[src]);
		p1[r] = mr_fall_foam[src];
	}

	if (c < 0) c = 0;
	if (c1 > MR_LEVEL_COLS) c1 = MR_LEVEL_COLS;

	while (c < c1) {
		int run = 0;
		while (c < c1 && !(mr_level[MR_GROUND_ROW][c] == MR_TILE_EMPTY &&
				mr_level[MR_SUBSURFACE_ROW][c] == MR_TILE_EMPTY))
			c++;
		while (c + run < c1 &&
				mr_level[MR_GROUND_ROW][c + run] == MR_TILE_EMPTY &&
				mr_level[MR_SUBSURFACE_ROW][c + run] == MR_TILE_EMPTY)
			run++;
		if (run)
			c_pattern(world_to_fb_x((int32_t)c * MR_TILE_W), y,
				run * MR_TILE_W, Z_GAME_VIEW_H - y, p0, p1);
		c += run;
	}
}

/* The flip, in colour: the page just drawn goes on screen at the next
 * frame boundary, and the number of frames since the last flip comes
 * back, as z_game_flip() does for the monochrome layout. */
static uint32_t col_flip(void) {
	uint32_t f, dt;
	z_game_set_view((uint32_t)col_page_x(), 0);
	z_game_wait_frame();
	f = z_game_frame();
	dt = (f - col_last_frame) & 0xffffu;
	if (dt == 0) dt = 1;
	col_last_frame = f;
	col_back ^= 1;
	fx_tick(dt);                /* the palette, at the frame boundary */
	return dt;
}

/* Colour on or off. Both clear the whole framebuffer: the other
 * layout's pages hold pixels that mean something else in this one.
 * Off hands back to zgame's layout, whose next flip sets the view. */
static void col_set(bool on) {
	if (on) {
		if (!z_color_available()) return;
		z_fb_hw_sync();
		z_color_begin(&z_color_4_below);
		z_fb_hw_fill_rect(0, 0, 640, 480, 0);
		col_back = 0;
		col_last_frame = z_game_frame();
		z_color_palette_load(mr_lights[mr_level_info.theme].day, 4);
		col_on = true;
	} else {
		z_fb_hw_sync();
		z_color_end();
		z_fb_hw_fill_rect(0, 0, 640, 480, 0);
		col_on = false;
	}
}

/* One 16x16 tile through the hardware blitter.
 *
 * The tile data is 16 uint16 rows, so its stride is 2 bytes. The
 * blitter takes a stride in bytes and reads with the framebuffer's own
 * bit order, which is why gen_sprites.py emits LSB-first (see its
 * header). No shifting or masking is needed here at all: tiles are
 * opaque, and the blitter handles arbitrary destination alignment
 * itself. */
/* A blit from memory in monochrome, round the torus seam: a run of
 * tiles can cross framebuffer column 639 and go on at column 0. */
static void m_blit(const void *src, int stride, int sy, int fbx, int fby,
	int w, int h, int rop) {
	int w1 = Z_SCREEN_W - fbx;
	if (w1 > w) w1 = w;
	hud_blits++;
	z_fb_hw_blit_mem_async(src, stride, 0, sy, fbx, fby, w1, h, rop);
	if (w1 < w) {
		hud_blits++;
		z_fb_hw_blit_mem_async(src, stride, w1, sy, 0, fby, w - w1, h, rop);
	}
}

/* n tiles of one kind side by side (1..MR_STRIP_TILES), from the strip,
 * through the hardware blitter.
 *
 * The strip's rows are words, so its stride is 48 bytes. The blitter
 * takes a stride in bytes and reads with the framebuffer's own bit
 * order, which is why gen_sprites.py emits LSB-first (see its header).
 * Async: the next run's acquire() waits for this one, so the loop in
 * draw_tiles_rect() overlaps each blit with finding the next run. See
 * zgfx.h. */
static void draw_tile_n(uint8_t t, int fbx, int fby, int n) {
	const void *src;
	int w = MR_TILE_W * n;
	if (t >= MR_TILE_COUNT) return;
	if (col_on) { c_tile_n(t, fbx, fby, n); return; }
	src = mr_strips[t];
	/* The same transparency in monochrome, where the tile's "sky" is
	 * black: grass blades and cheese are ORed in, so the tree behind
	 * the blades and the cloud behind a cheese show through instead of
	 * being blanked to black. Needs raster ops; without them every
	 * tile is opaque, as it always was. */
	if (t == MR_TILE_WATER) {
		/* water is a grey of its own, the ripples over it */
		int w1 = Z_SCREEN_W - fbx;
		if (w1 > w) w1 = w;
		hud_blits++;
		z_fb_hw_fill_shade_async(fbx, fby, w1, MR_TILE_H, 4);
		if (w1 < w) z_fb_hw_fill_shade_async(0, fby, w - w1, MR_TILE_H, 4);
		m_blit(src, MR_STRIP_STRIDE, 0, fbx, fby, w, MR_TILE_H,
			rop_hw ? Z_ROP_OR : Z_ROP_COPY);
		return;
	}
	if (rop_hw && t != MR_TILE_GROUND && t != MR_TILE_BOX && t != MR_TILE_GRASS &&
	    t != MR_TILE_CRUMBLE && t != MR_TILE_CRACKED && t != MR_TILE_QBOX &&
	    t != MR_TILE_USEDBOX && t != MR_TILE_FLOOR && t != MR_TILE_ROOF &&
	    t != MR_TILE_CHIMNEY) {
		/* cheese, the power-ups, springs, thorns, holes: see-through */
		m_blit(src, MR_STRIP_STRIDE, 0, fbx, fby, w, MR_TILE_H, Z_ROP_OR);
		return;
	}
	if (rop_hw && t == MR_TILE_GRASS) {
		m_blit(src, MR_STRIP_STRIDE, 0, fbx, fby, w, MR_GRASS_BLADES, Z_ROP_OR);
		m_blit(src, MR_STRIP_STRIDE, MR_GRASS_BLADES, fbx, fby + MR_GRASS_BLADES,
			w, MR_TILE_H - MR_GRASS_BLADES, Z_ROP_COPY);
		return;
	}
	m_blit(src, MR_STRIP_STRIDE, 0, fbx, fby, w, MR_TILE_H, Z_ROP_COPY);
}

static void draw_tile(uint8_t t, int fbx, int fby) {
	draw_tile_n(t, fbx, fby, 1);
}

/* Draw every tile overlapping the world-space rectangle.
 *
 * Used for BOTH newly scrolled-in columns and sprite repair, because
 * they are the same operation: put the background back over a region.
 * Snapping outward to tile boundaries is what lets one function serve
 * both -- a repair rectangle rarely lands on a tile edge.
 *
 * Walks tile columns one at a time rather than computing a single
 * destination rectangle, so a span that straddles the torus seam is
 * handled with no special case: each column folds independently. */
/* -- the sub-surface, as RUN-LENGTH FILLS --
 *
 * Rows 12..14 are solid ground everywhere except the pits, which makes
 * them ~63 identical tile blits a frame -- the bulk of the frame's
 * blit count, spent drawing the same black rectangle over and over.
 *
 * They are drawn as one fill per contiguous run of non-empty ground
 * instead. This level has six pits, so a screen shows one or two runs:
 * roughly 2 fills where there were 63 blits.
 *
 * That matters more than it looks. This app's frame time scales with
 * blit count -- each one is eight peripheral register writes plus a
 * spin -- and with `wm`, `net` and `repl` all polling rather than
 * blocking, the game only gets about a quarter of the CPU to do them
 * in. Cutting the count is the one lever that works without touching
 * the scheduler.
 *
 * The cost is that the sub-surface loses its texture and becomes flat.
 * That is exactly what a SCREEN-ALIGNED PATTERNED FILL would give back
 * for free, which is the strongest practical argument for building
 * them: this is not a 3D-shading feature, it is how you draw a
 * textured background without a blit per tile.
 */
static void draw_subsurface(int32_t wx0, int32_t wx1) {

	int c = (int)(wx0 / MR_TILE_W);
	int c1 = (int)((wx1 + MR_TILE_W - 1) / MR_TILE_W);
	int fy = world_to_fb_y(MR_SUBSURFACE_ROW * MR_TILE_H);
	int hh = (MR_LEVEL_ROWS - MR_SUBSURFACE_ROW) * MR_TILE_H;

	if (c < 0) c = 0;
	if (c1 > MR_LEVEL_COLS) c1 = MR_LEVEL_COLS;

	while (c < c1) {

		int run;

		/* skip the pit -- and a pond, or anything else down there
		 * that is not earth: draw_tiles_rect() draws those */
		while (c < c1 && !mr_tile_earth(mr_level[MR_SUBSURFACE_ROW][c]))
			c++;
		if (c >= c1) break;

		run = 0;
		while (c + run < c1 &&
			mr_tile_earth(mr_level[MR_SUBSURFACE_ROW][c + run]))
			run++;

		/* One fill per run, split at the torus seam the same way the
		 * sky fill is -- a run can straddle framebuffer column 639. */
		if (col_on) {
			c_earth(world_to_fb_x((int32_t)c * MR_TILE_W), fy,
				run * MR_TILE_W, hh);
		} else {
			int fx = world_to_fb_x((int32_t)c * MR_TILE_W);
			int w = run * MR_TILE_W;
			int w1 = Z_SCREEN_W - fx;

			if (w1 > w) w1 = w;
			/* SHADED, not solid. Collapsing the sub-surface to
			 * run-length fills cost it its texture; a dithered fill
			 * gives that back for exactly the same number of
			 * operations, because the pattern is generated in
			 * hardware from a level rather than blitted from a tile.
			 *
			 * And it is screen-aligned, so the two halves of a run
			 * that straddles the torus seam join invisibly, and the
			 * texture does not shimmer as the world scrolls under it.
			 * A rectangle-relative pattern would fail at both. */
			hud_blits++;
			z_fb_hw_fill_shade_async(fx, fy, w1, hh, MR_GROUND_SHADE);

			if (w1 < w) {
				hud_blits++;
				z_fb_hw_fill_shade_async(0, fy, w - w1, hh,
					MR_GROUND_SHADE);
			}
		}

		c += run;

	}

}

/* What a tile looks like in the level's theme: the map says what it
 * IS (grass is the floor you walk on, thorns are what hurts); the theme
 * says how it looks -- a kitchen's floor tiles and mousetraps, the
 * roofs' tiles and chimneys. */
static uint8_t theme_tile(uint8_t t) {
	switch (mr_level_info.theme) {
	case MR_THEME_KITCHEN:
		if (t == MR_TILE_GRASS) return MR_TILE_FLOOR;
		if (t == MR_TILE_THORNS) return MR_TILE_TRAP;
		break;
	case MR_THEME_ROOFS:
		if (t == MR_TILE_GRASS) return MR_TILE_ROOF;
		if (t == MR_TILE_BOX) return MR_TILE_CHIMNEY;
		break;
	default:
		break;
	}
	return t;
}

static void draw_tiles_rect(int32_t wx0, int32_t wy0, int32_t wx1, int32_t wy1) {

	int c0 = (int)(wx0 / MR_TILE_W);
	int c1 = (int)((wx1 + MR_TILE_W - 1) / MR_TILE_W);
	int r0 = (int)(wy0 / MR_TILE_H);
	int r1 = (int)((wy1 + MR_TILE_H - 1) / MR_TILE_H);

	/* Rows 0..MR_FIRST_SOLID_ROW-1 of this level are pure sky and the
	 * fill already covers them. Skipping them outright saves walking
	 * ~150 cells a frame to discover they are all EMPTY -- cheap per
	 * cell, but this runs 60 times a second over the whole viewport. */
	if (r0 < MR_FIRST_SOLID_ROW) r0 = MR_FIRST_SOLID_ROW;

	/* Rows MR_SUBSURFACE_ROW and below are drawn by draw_subsurface()
	 * as run-length fills, not as tiles -- the earth is. Anything else
	 * down there (a pond, a spring at the bottom of a pit) is a tile. */
	if (wx0 < 0) c0 = 0;
	if (wy0 < 0) r0 = 0;
	if (c1 > MR_LEVEL_COLS) c1 = MR_LEVEL_COLS;
	if (r1 > MR_LEVEL_ROWS) r1 = MR_LEVEL_ROWS;

	/* A row at a time, in runs of the same tile (draw_tile_n()). */
	for (int r = r0; r < r1; r++) {
		int fby = world_to_fb_y((int32_t)r * MR_TILE_H);
		for (int c = c0; c < c1; ) {
			uint8_t t = mr_level[r][c], u;
			int n;
			/* EMPTY is skipped, not drawn as black. The sky was
			 * already filled, and the ambient layer has drawn clouds
			 * and trees over it -- blitting an empty tile here would
			 * erase them. That ordering is the whole reason the sky
			 * is a fill rather than 300 empty tile blits. */
			if (t == MR_TILE_EMPTY || t == MR_TILE_TRACK ||   /* a plank is drawn where it is */
			    (r >= MR_SUBSURFACE_ROW && mr_tile_earth(t) &&
			     mr_tile_earth(mr_level[MR_SUBSURFACE_ROW][c]))) {
				c++;
				continue;
			}
			u = theme_tile(t);
			for (n = 1; c + n < c1 && n < MR_STRIP_TILES; n++) {
				uint8_t t2 = mr_level[r][c + n];
				if (t2 != t) break;
				if (r >= MR_SUBSURFACE_ROW && mr_tile_earth(t2) &&
				    mr_tile_earth(mr_level[MR_SUBSURFACE_ROW][c + n])) break;
			}
			draw_tile_n(u, world_to_fb_x((int32_t)c * MR_TILE_W), fby, n);
			c += n;
		}
	}

}

/* Masked sprite blit -- HARDWARE where the bitstream has raster ops,
 * software otherwise.
 *
 * The hardware path is two blitter passes over the same rectangle:
 * ANDN the mask to clear exactly the pixels the sprite will occupy,
 * then OR the data to lay them in. That is what gpu_blit.v's raster
 * ops were added for; see docs/gpu.md.
 *
 * The software fallback below is the original code and is kept, not
 * as belt and braces but because the same app binary can be run
 * against an older bitstream. On such a bitstream the ROP field is
 * simply ignored and every op behaves as COPY, so a hardware masked
 * blit would draw the mask as a solid block and the data over it --
 * an opaque box, which reads as bad art rather than a missing hardware
 * feature. z_fb_hw_rop_available() tells the two apart.
 *
 * -- what did NOT move to hardware, and why --
 *
 * MIRRORING. The blitter has no reverse mode, so the mouse still needs
 * a left-facing copy of each frame. Rather than mirror at runtime
 * (which would mean building a temporary bitmap every frame, defeating
 * the point), gen_sprites.py now emits both facings and this picks the
 * pointer. Costs four extra 32-byte bitmaps.
 *
 * WRAPPING at the torus seam. z_fb_hw_blit_mem() clips to the screen,
 * so a sprite straddling framebuffer column 639 loses its right half
 * rather than continuing at column 0. The fix is the same one the
 * software path uses -- draw it twice, once at each end -- which is
 * two more blits on the rare frames it matters and no special case in
 * the hardware at all.
 */

/* rop_hw: declared with the colour block above, where draw_tile()
 * first needs it. Set once at startup. */

/* Software fallback: (dst & ~mask) | data, one row at a time. */
static void spr_draw_sw(const uint16_t *data, const uint16_t *mask,
	int fbx, int fby)
{
	volatile uint32_t *vram = (volatile uint32_t *)VRAM_BASE;
	int page_top = z_game_back_y(&game);
	int page_bot = page_top + Z_GAME_PAGE_H(game.orient);

	for (int row = 0; row < MR_SPR_H; row++) {

		int y = fby + row;
		uint32_t d, m;

		if (y < page_top || y >= page_bot) continue;

		d = data[row];
		m = mask[row];
		if (!m) continue;

		{
			int wordi = fbx >> 5;
			int bit = fbx & 31;
			uint32_t base = (uint32_t)y * FB_STRIDE_WORDS;

			uint32_t dlo = d << bit;
			uint32_t mlo = m << bit;

			if (mlo) {
				uint32_t v = vram[base + wordi];
				vram[base + wordi] = (v & ~mlo) | (dlo & mlo);
			}

			/* `bit` can be 0, and shifting a uint32_t right by 32 is
			 * UNDEFINED in C rather than merely useless -- hence the
			 * guard, which is required rather than defensive.
			 *
			 * The wrap is the torus seam: the page is 640 columns
			 * around, so a sprite across column 639 continues at
			 * column 0 of the SAME row, which is exactly where
			 * gpu_video.v's scanout looks for it. */
			if (bit != 0) {
				uint32_t dhi = d >> (32 - bit);
				uint32_t mhi = m >> (32 - bit);
				if (mhi) {
					int hw = wordi + 1;
					if (hw >= FB_STRIDE_WORDS) hw = 0;
					{
						uint32_t v = vram[base + hw];
						vram[base + hw] = (v & ~mhi) | (dhi & mhi);
					}
				}
			}
		}

	}

}

/* Software path with runtime mirroring -- three shift-mask steps per
 * row, which is why the software version never needed a second copy of
 * the art. Only reached on a bitstream without raster ops. */
static void spr_draw_sw_mirror(const uint16_t *data, const uint16_t *mask,
	int fbx, int fby, bool mirror)
{
	uint16_t d2[MR_SPR_H], m2[MR_SPR_H];

	if (!mirror) { spr_draw_sw(data, mask, fbx, fby); return; }

	for (int i = 0; i < MR_SPR_H; i++) {
		uint32_t d = data[i], m = mask[i];
		d = ((d & 0x00ffu) << 8) | ((d & 0xff00u) >> 8);
		d = ((d & 0x0f0fu) << 4) | ((d & 0xf0f0u) >> 4);
		d = ((d & 0x3333u) << 2) | ((d & 0xccccu) >> 2);
		d = ((d & 0x5555u) << 1) | ((d & 0xaaaau) >> 1);
		m = ((m & 0x00ffu) << 8) | ((m & 0xff00u) >> 8);
		m = ((m & 0x0f0fu) << 4) | ((m & 0xf0f0u) >> 4);
		m = ((m & 0x3333u) << 2) | ((m & 0xccccu) >> 2);
		m = ((m & 0x5555u) << 1) | ((m & 0xaaaau) >> 1);
		d2[i] = (uint16_t)d;
		m2[i] = (uint16_t)m;
	}

	spr_draw_sw(d2, m2, fbx, fby);
}

static void spr_draw(const uint16_t *data16, const uint16_t *mask16,
	const void *data_hw, const void *mask_hw, int fbx, int fby);

static void spr_draw_any(const uint16_t *data16, const uint16_t *mask16,
	const void *data_hw, const void *mask_hw, int fbx, int fby,
	bool mirror_sw)
{
	hud_blits++;
	if (!rop_hw) { spr_draw_sw_mirror(data16, mask16, fbx, fby, mirror_sw); return; }
	spr_draw(data16, mask16, data_hw, mask_hw, fbx, fby);
}

/* A masked sprite of any size from 32-bit rows, through the raster ops
 * (colour, or monochrome with rop_hw): the far layer's whole clouds and
 * trees, and every 16x16 sprite. */
static void spr_draw_wh(const void *data_hw, const void *mask_hw, int stride,
	int w0, int h0, int fbx, int fby)
{
	if (col_on) {
		c_sprite_wh(data_hw, mask_hw, stride, fbx, fby, w0, h0, spr_c, spr_o);
		return;
	}

	/* Clipped to the page, not the screen. Drawing past the bottom of
	 * a 640x240 page would spill into the OTHER page, which is on
	 * screen -- a hazard specific to this layout that zgfx.h's own
	 * screen clipping cannot know about. */
	{
		int page_top = z_game_back_y(&game);
		int page_bot = page_top + Z_GAME_PAGE_H(game.orient);
		int sy = 0;
		int h = h0;

		if (fby < page_top) { sy = page_top - fby; h -= sy; fby = page_top; }
		if (fby + h > page_bot) h = page_bot - fby;
		if (h <= 0) return;

		/* no outline (the mask is the data): one OR, round the seam */
		if (data_hw == mask_hw) {
			int w = w0;
			if (fbx + w > Z_SCREEN_W) w = Z_SCREEN_W - fbx;
			if (w > 0)
				z_fb_hw_blit_mem_async(data_hw, stride, 0, sy, fbx, fby, w, h, Z_ROP_OR);
			if (fbx + w0 > Z_SCREEN_W)
				z_fb_hw_blit_mem_async(data_hw, stride, Z_SCREEN_W - fbx, sy, 0, fby,
					w0 - (Z_SCREEN_W - fbx), h, Z_ROP_OR);
			return;
		}

		/* The part that fits before column 640. */
		{
			int w = w0;
			if (fbx + w > Z_SCREEN_W) w = Z_SCREEN_W - fbx;
			if (w > 0)
				z_fb_hw_blit_sprite_async(data_hw, mask_hw, stride,
					0, sy, fbx, fby, w, h);
		}

		/* ...and the part that wrapped round to column 0. */
		if (fbx + w0 > Z_SCREEN_W) {
			int cut = Z_SCREEN_W - fbx;
			z_fb_hw_blit_sprite_async(data_hw, mask_hw, stride,
				cut, sy, 0, fby, w0 - cut, h);
		}
	}
}

static void spr_draw(const uint16_t *data16, const uint16_t *mask16,
	const void *data_hw, const void *mask_hw, int fbx, int fby)
{
	if (!col_on && !rop_hw) {
		/* The software path READS AND WRITES VRAM WITH THE CPU, so it
		 * must not run while an async tile blit is still in flight --
		 * it would read a half-drawn background and composite the
		 * sprite onto it. Another blit would be fine; only CPU access
		 * needs this barrier. See zgfx.h. */
		z_fb_hw_sync();
		spr_draw_sw(data16, mask16, fbx, fby);
		return;
	}
	spr_draw_wh(data_hw, mask_hw, MR_TILE_STRIDE, MR_SPR_W, MR_SPR_H, fbx, fby);
}


/* -- ambient layer: parallax clouds and trees, plus birds --
 *
 * Here to push the blitter, which is the demo's actual job. 20-50
 * sprites on screen with two blitter passes each is 40-100 masked
 * blits a frame on top of the background.
 *
 * -- why this changed how the background is drawn --
 *
 * The incremental scroll-span redraw this file used to do only works
 * when everything on screen moves at the SAME rate as the tilemap.
 * Parallax breaks that by definition: a cloud at half speed exposes
 * background the tile walk does not know is dirty.
 *
 * So the background is now redrawn every frame instead: one fill for
 * the sky, then only the NON-EMPTY tiles over it. That deleted the
 * per-page damage tracking entirely -- no mark lists, no repair pass,
 * no "which page was this drawn into two frames ago". It costs more
 * blits and is dramatically simpler, and it is the right trade once
 * the sprite count is high enough that per-sprite repair would touch
 * most of the screen anyway.
 *
 * Measured: the sky fill is ~9600 cycles and a tile is ~120, so a
 * screen that is mostly sky costs LESS than the 300-tile full walk it
 * replaces.
 *
 * -- parallax and the torus --
 *
 * Ambient sprites live in their own coordinate space: world position
 * times a parallax factor. They are not in the tilemap and never
 * collide. Their x is folded into the page the same way everything
 * else is, so they wrap at the seam for free.
 */

#define MR_MAX_CLOUDS 10
#define MR_KITCHEN_SHELF_Y 104    /* the far shelf the jars stand on */
#define MR_MAX_TREES  10
#define MR_TREE_SINK  5     /* blades (3) + TREE_BOT's empty last row (1) + 1 */
#define MR_MAX_BIRDS  12

typedef struct {
	int32_t x, y;        /* position in the layer's own space */
	int16_t vx;          /* birds only, 1/16 px per frame */
	uint8_t anim;
} mr_amb_t;

static mr_amb_t clouds[MR_MAX_CLOUDS];
static mr_amb_t trees[MR_MAX_TREES];
static mr_amb_t birds[MR_MAX_BIRDS];

/* Parallax denominators. Clouds move at a quarter of the camera, trees
 * at three quarters -- so trees read as "just behind the action" and
 * clouds as "far away", which is the whole illusion. Powers of two so
 * the divide is a shift; picorv32 has no divider unless `CPU_DIV is
 * built and this runs per sprite per frame. */
#define MR_CLOUD_SHIFT 2
#define MR_TREE_SHIFT  1

/* The far layer's two-cell sprites, joined (far_init()): a cloud is
 * one 32x16 draw, a tree or a house one 16x32 -- half the draws of two
 * cells each, and the far layer was most of a colour frame after the
 * tiles. Rows of 32 bits; the cloud's stride is two words, the second
 * for the blitter to read past the end. */
#define MR_CLOUD_STRIDE 8
static uint32_t far_cloud_d[MR_SPR_H * 2], far_cloud_m[MR_SPR_H * 2];
enum { FAR_TREE = 0, FAR_HOUSE, FAR_TALL };
static uint32_t far_tall_d[FAR_TALL][2 * MR_SPR_H], far_tall_m[FAR_TALL][2 * MR_SPR_H];
static bool far_cloud_solid, far_tall_solid[FAR_TALL];     /* mask == data */

static void far_init(void) {
	int r;
	for (r = 0; r < MR_SPR_H; r++) {
		far_cloud_d[r * 2] = (mr_hspr_cloud_l[r] & 0xffffu) | (mr_hspr_cloud_r[r] << 16);
		far_cloud_m[r * 2] = (mr_hmsk_cloud_l[r] & 0xffffu) | (mr_hmsk_cloud_r[r] << 16);
		far_cloud_d[r * 2 + 1] = far_cloud_m[r * 2 + 1] = 0;
		far_tall_d[FAR_TREE][r] = mr_hspr_tree_top[r];
		far_tall_m[FAR_TREE][r] = mr_hmsk_tree_top[r];
		far_tall_d[FAR_TREE][MR_SPR_H + r] = mr_hspr_tree_bot[r];
		far_tall_m[FAR_TREE][MR_SPR_H + r] = mr_hmsk_tree_bot[r];
		far_tall_d[FAR_HOUSE][r] = mr_hspr_house_top[r];
		far_tall_m[FAR_HOUSE][r] = mr_hmsk_house_top[r];
		far_tall_d[FAR_HOUSE][MR_SPR_H + r] = mr_hspr_house_bot[r];
		far_tall_m[FAR_HOUSE][MR_SPR_H + r] = mr_hmsk_house_bot[r];
	}
	far_cloud_solid = !memcmp(far_cloud_d, far_cloud_m, sizeof(far_cloud_d));
	for (r = 0; r < FAR_TALL; r++)
		far_tall_solid[r] = !memcmp(far_tall_d[r], far_tall_m[r], sizeof(far_tall_d[r]));
}

/* One whole far sprite, if any of it is on screen; raster ops only (the
 * software path draws the cells, amb_cell()). */
static void amb_big(const void *hd, const void *hm, int stride, int w, int h,
	int32_t lx, int32_t ly, int32_t cam_layer)
{
	int32_t sx = lx - cam_layer;
	if (sx <= -w || sx >= Z_GAME_VIEW_W) return;
	if (ly <= -h || ly >= Z_GAME_VIEW_H) return;
	hud_blits++;
	spr_draw_wh(hd, hm, stride, w, h, world_to_fb_x(game.cam + sx), world_to_fb_y(ly));
}

static void ambient_init(void) {

	/* Deterministic placement from a small LCG rather than z_rand():
	 * the demo should look the same every run, so a visual regression
	 * is visible as one. */
	uint32_t r = 0x1234567u;
	#define NEXT_R (r = r * 1103515245u + 12345u, (r >> 16) & 0x7fff)

	for (int i = 0; i < MR_MAX_CLOUDS; i++) {
		clouds[i].x = (int32_t)(NEXT_R % (MR_WORLD_W >> MR_CLOUD_SHIFT));
		clouds[i].y = (int32_t)(8 + NEXT_R % 40);
		clouds[i].anim = 0;
	}

	for (int i = 0; i < MR_MAX_TREES; i++) {
		trees[i].x = (int32_t)(NEXT_R % (MR_WORLD_W >> MR_TREE_SHIFT));
		/* Stand them ON the floor, not in it.
		 *
		 * A tree is TWO 16px cells (TREE_TOP above TREE_BOT), so its
		 * base is at y + 2*MR_SPR_H, not y + MR_SPR_H. Subtracting
		 * only one cell height buried the whole lower half below the
		 * floor surface.
		 *
		 * And +MR_TREE_SINK: the ground row's top is not its tile's
		 * top. A grass tile is three rows of blades and then turf, and
		 * TREE_BOT's last row is empty, so a tree whose cells end at
		 * the tile boundary floats five pixels above the turf. Sunk by
		 * that much, the base sits at the turf line and the blades
		 * (drawn after the ambient layer) stand in front of it. */
		trees[i].y = MR_GROUND_ROW * MR_TILE_H - 2 * MR_SPR_H + MR_TREE_SINK;
		trees[i].anim = 0;
	}

	for (int i = 0; i < MR_MAX_BIRDS; i++) {
		birds[i].x = (int32_t)(NEXT_R % MR_WORLD_W);
		birds[i].y = (int32_t)(16 + NEXT_R % 72);
		birds[i].vx = (int16_t)((NEXT_R & 1) ? 6 : -6);
		birds[i].anim = (uint8_t)(NEXT_R & 31);
	}

	#undef NEXT_R

}

static void ambient_update(uint32_t frames) {

	for (int i = 0; i < MR_MAX_BIRDS; i++) {
		birds[i].x += birds[i].vx * (int32_t)frames;
		if (birds[i].x < 0) birds[i].x += MR_WORLD_W;
		if (birds[i].x >= MR_WORLD_W) birds[i].x -= MR_WORLD_W;
		birds[i].anim += (uint8_t)frames;
	}

}

/* Draw one 16x16 ambient cell if any part of it is on screen.
 *
 * `lx` is already in the layer's space; the camera offset applied is
 * the parallax-scaled one. Culled against the viewport first -- with
 * 30-plus ambient sprites and only a third on screen at a time, the
 * cull saves more than it costs. */
static void amb_cell(const void *hd, const void *hm,
	const uint16_t *sd, const uint16_t *sm, int32_t lx, int32_t ly,
	int32_t cam_layer)
{
	int32_t sx = lx - cam_layer;

	if (sx <= -MR_SPR_W || sx >= Z_GAME_VIEW_W) return;
	if (ly <= -MR_SPR_H || ly >= Z_GAME_VIEW_H) return;

	spr_draw_any(sd, sm, hd, hm,
		world_to_fb_x(game.cam + sx), world_to_fb_y(ly), false);
}

static void ambient_draw(void) {

	int32_t cam_c = game.cam >> MR_CLOUD_SHIFT;
	int32_t cam_t = game.cam >> MR_TREE_SHIFT;

	spr_c = MR_C_CREAM; spr_o = MR_C_SKY;   /* clouds: no outline */
	for (int i = 0; i < MR_MAX_CLOUDS && mr_level_info.theme != MR_THEME_KITCHEN; i++) {
		if (rop_hw) {
			amb_big(far_cloud_d, far_cloud_solid ? far_cloud_d : far_cloud_m,
				MR_CLOUD_STRIDE, 2 * MR_SPR_W, MR_SPR_H,
				clouds[i].x, clouds[i].y, cam_c);
			continue;
		}
		amb_cell(mr_hspr_cloud_l, mr_hmsk_cloud_l,
			mr_spr_cloud_l, mr_msk_cloud_l,
			clouds[i].x, clouds[i].y, cam_c);
		amb_cell(mr_hspr_cloud_r, mr_hmsk_cloud_r,
			mr_spr_cloud_r, mr_msk_cloud_r,
			clouds[i].x + MR_SPR_W, clouds[i].y, cam_c);
	}

	for (int i = 0; i < MR_MAX_TREES; i++) {
		switch (mr_level_info.theme) {
		case MR_THEME_GARDEN:            /* sunflowers */
			spr_c = MR_C_CREAM; spr_o = MR_C_DARK;
			amb_cell(mr_hspr_flower_top, mr_hmsk_flower_top,
				mr_spr_flower_top, mr_msk_flower_top,
				trees[i].x, trees[i].y, cam_t);
			spr_c = MR_C_GREEN;
			amb_cell(mr_hspr_flower_bot, mr_hmsk_flower_bot,
				mr_spr_flower_bot, mr_msk_flower_bot,
				trees[i].x, trees[i].y + MR_SPR_H, cam_t);
			break;
		case MR_THEME_ROOFS:             /* the houses behind, higher up the hill */
			spr_c = MR_C_DARK; spr_o = MR_C_DARK;
			if (rop_hw) {
				amb_big(far_tall_d[FAR_HOUSE],
					far_tall_solid[FAR_HOUSE] ? far_tall_d[FAR_HOUSE] : far_tall_m[FAR_HOUSE],
					MR_TILE_STRIDE,
					MR_SPR_W, 2 * MR_SPR_H, trees[i].x, trees[i].y - 40, cam_t);
				break;
			}
			amb_cell(mr_hspr_house_top, mr_hmsk_house_top,
				mr_spr_house_top, mr_msk_house_top,
				trees[i].x, trees[i].y - 40, cam_t);
			amb_cell(mr_hspr_house_bot, mr_hmsk_house_bot,
				mr_spr_house_bot, mr_msk_house_bot,
				trees[i].x, trees[i].y - 40 + MR_SPR_H, cam_t);
			break;
		case MR_THEME_KITCHEN:           /* jars and cups on a shelf */
			spr_c = (i & 1) ? MR_C_GREEN : MR_C_CREAM; spr_o = MR_C_DARK;
			if (i & 1)
				amb_cell(mr_hspr_cup, mr_hmsk_cup, mr_spr_cup, mr_msk_cup,
					trees[i].x, MR_KITCHEN_SHELF_Y - MR_SPR_H, cam_t);
			else
				amb_cell(mr_hspr_jar, mr_hmsk_jar, mr_spr_jar, mr_msk_jar,
					trees[i].x, MR_KITCHEN_SHELF_Y - MR_SPR_H, cam_t);
			break;
		default:
			spr_c = MR_C_GREEN; spr_o = MR_C_DARK;
			if (rop_hw) {
				amb_big(far_tall_d[FAR_TREE],
					far_tall_solid[FAR_TREE] ? far_tall_d[FAR_TREE] : far_tall_m[FAR_TREE],
					MR_TILE_STRIDE,
					MR_SPR_W, 2 * MR_SPR_H, trees[i].x, trees[i].y, cam_t);
				break;
			}
			amb_cell(mr_hspr_tree_top, mr_hmsk_tree_top,
				mr_spr_tree_top, mr_msk_tree_top,
				trees[i].x, trees[i].y, cam_t);
			amb_cell(mr_hspr_tree_bot, mr_hmsk_tree_bot,
				mr_spr_tree_bot, mr_msk_tree_bot,
				trees[i].x, trees[i].y + MR_SPR_H, cam_t);
			break;
		}
	}

}

/* The moon, at night outdoors: far enough away not to move. */
static void moon_draw(void) {
	if (!fx_night() || mr_level_info.theme == MR_THEME_KITCHEN) return;
	spr_c = MR_C_CREAM; spr_o = MR_C_CREAM;
	spr_draw_any(mr_spr_moon, mr_msk_moon, mr_hspr_moon, mr_hmsk_moon,
		world_to_fb_x(game.cam + 268), world_to_fb_y(22), false);
}

/* The kitchen has a wall, not a sky: stripes of wallpaper (a pattern
 * fill a plane, moving at half the camera's pace), and a long shelf
 * along it for the jars. Colour only: in monochrome the wall is black,
 * as the sky is. */
static void kitchen_wall_draw(void) {
	static const uint8_t none[8];
	uint8_t p[8];
	int r, sh = (int)((game.cam >> 1) & 7);
	/* a thin green line every eight pixels: plane 1 only */
	for (r = 0; r < 8; r++)
		p[r] = (uint8_t)(1u << sh);
	c_pattern(col_page_x(), 12, Z_GAME_VIEW_W, MR_GROUND_ROW * MR_TILE_H - 12, none, p);
	c_fill(col_page_x(), MR_KITCHEN_SHELF_Y, Z_GAME_VIEW_W, 3, MR_C_DARK);
}

static void birds_draw(void) {

	spr_c = MR_C_DARK; spr_o = MR_C_DARK;

	/* indoors no birds, and none out at night */
	if (mr_level_info.theme == MR_THEME_KITCHEN || fx_night()) return;

	for (int i = 0; i < MR_MAX_BIRDS; i++) {
		int flap = (birds[i].anim & 8) ? 1 : 0;
		bool left = birds[i].vx < 0;
		const void *hd = flap
			? (left ? (const void *)mr_hspr_bird2_m : (const void *)mr_hspr_bird2)
			: (left ? (const void *)mr_hspr_bird1_m : (const void *)mr_hspr_bird1);
		const void *hm = flap
			? (left ? (const void *)mr_hmsk_bird2_m : (const void *)mr_hmsk_bird2)
			: (left ? (const void *)mr_hmsk_bird1_m : (const void *)mr_hmsk_bird1);
		amb_cell(hd, hm,
			flap ? mr_spr_bird2 : mr_spr_bird1,
			flap ? mr_msk_bird2 : mr_msk_bird1,
			birds[i].x, birds[i].y, game.cam);
	}

}

/* -- collision: phys.c (mr_box_hits_solid(), mr_move_actor()) -- */

/* -- world ---------------------------------------------------------- */

/* -- particles (docs/mausrun.md, "Particles") --
 *
 * A pool of small things that fly about and go: dust from the feet,
 * crumbs, splashes, sparkles, confetti, feathers, zzz, a cheese pulled
 * by the magnet, and the level's weather. Each is a little fill (or,
 * for the zzz and the cheese, a little bitmap): one or two blits. The
 * weather never takes more than MR_WEATHER_MAX of the pool, so a burst
 * always finds room. */
#define MR_MAX_PARTS   96
#define MR_WEATHER_MAX 36

enum {
	P_NONE = 0, P_DUST, P_CRUMB, P_DROP, P_SPARK, P_CONFETTI, P_FEATHER,
	P_ZZZ, P_CHEESE,
	P_LEAF, P_SNOW, P_RAIN, P_FIREFLY      /* the weather: these last */
};

typedef struct {
	int32_t x, y;           /* 1/16 px, world */
	int16_t vx, vy;
	uint16_t life;          /* frames left */
	uint8_t kind, c;
} mr_part_t;

static mr_part_t parts[MR_MAX_PARTS];
static int weather_n;

static int rnd(int n) {
	return n > 0 ? (int)(fx_rand() % (uint32_t)n) : 0;
}

/* x, y in pixels; velocities in 1/16 px a frame */
static mr_part_t *part_add(int kind, int32_t x, int32_t y, int vx, int vy,
	int life, int c) {
	int i;
	for (i = 0; i < MR_MAX_PARTS; i++)
		if (parts[i].kind == P_NONE) break;
	if (i == MR_MAX_PARTS) return NULL;
	parts[i].x = TOFP(x);
	parts[i].y = TOFP(y);
	parts[i].vx = (int16_t)vx;
	parts[i].vy = (int16_t)vy;
	parts[i].life = (uint16_t)life;
	parts[i].kind = (uint8_t)kind;
	parts[i].c = (uint8_t)c;
	return &parts[i];
}

/* n of a kind from a point, flung up and out */
static void burst(int kind, int32_t x, int32_t y, int n, int speed, int c) {
	int i;
	for (i = 0; i < n; i++) {
		int cc = c;
		if (kind == P_CONFETTI) cc = 1 + rnd(3);   /* dark, green, cream */
		part_add(kind, x + rnd(5) - 2, y + rnd(5) - 2,
			rnd(2 * speed + 1) - speed, -rnd(speed + 1) - speed / 2,
			kind == P_CONFETTI ? 70 + rnd(40) : kind == P_FEATHER ? 60 + rnd(30) :
			kind == P_SPARK ? 12 + rnd(6) : 20 + rnd(14), cc);
	}
}

static void parts_clear(void) {
	memset(parts, 0, sizeof(parts));
	weather_n = 0;
}

static void cheese_got(int32_t vx_at);

static void parts_update(void) {
	int i;
	weather_n = 0;
	for (i = 0; i < MR_MAX_PARTS; i++) {
		mr_part_t *p = &parts[i];
		if (p->kind == P_NONE) continue;
		switch (p->kind) {
		case P_CRUMB: case P_DROP:
			p->vy += 4;
			break;
		case P_CONFETTI: case P_FEATHER:
			/* flutter: falls slowly, sways */
			p->vy += 1;
			if (p->vy > 10) p->vy = 10;
			p->vx = (int16_t)((p->vx * 15) / 16 + ((p->life & 16) ? 2 : -2));
			break;
		case P_DUST: case P_SPARK:
			p->vx = (int16_t)(p->vx * 7 / 8);
			p->vy = (int16_t)(p->vy * 7 / 8);
			break;
		case P_CHEESE: {
			/* to the mouse, faster and faster */
			int32_t tx = player.x + TOFP(MR_PLAYER_W / 2 - 8);
			int32_t ty = player.y + TOFP(MR_PLAYER_H / 2 - 8);
			int32_t dx = tx - p->x, dy = ty - p->y;
			int32_t ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
			if (ax < TOFP(6) && ay < TOFP(6)) p->life = 1;
			p->vx = (int16_t)(dx / 6);
			p->vy = (int16_t)(dy / 6);
			break;
		}
		case P_LEAF:
			p->vx = (int16_t)(((fx_anim + (uint32_t)i * 7) & 64) ? 6 : -6);
			weather_n++;
			break;
		case P_FIREFLY:
			if (!rnd(16)) { p->vx = (int16_t)(rnd(9) - 4); p->vy = (int16_t)(rnd(9) - 4); }
			weather_n++;
			break;
		case P_SNOW: case P_RAIN:
			weather_n++;
			break;
		default:
			break;
		}
		p->x += p->vx;
		p->y += p->vy;
		if (p->kind >= P_LEAF) {
			/* weather lives in the view: gone when it leaves it */
			int32_t sx = TOPX(p->x) - game.cam;
			if (TOPX(p->y) > Z_GAME_VIEW_H || sx < -24 || sx > Z_GAME_VIEW_W + 24)
				p->life = 1;
		}
		if (--p->life == 0) {
			if (p->kind == P_CHEESE) cheese_got(TOPX(p->x));
			p->kind = P_NONE;
		}
	}
}

/* The level's weather: a few new ones a frame, across the view. */
static void weather_update(void) {
	int32_t x = game.cam + rnd(Z_GAME_VIEW_W + 32) - 16;
	if (weather_n >= MR_WEATHER_MAX) return;
	switch (mr_level_info.weather) {
	case MR_WEATHER_LEAVES:
		if (!rnd(14)) part_add(P_LEAF, x, -2, 0, 6 + rnd(6), 600, MR_C_GREEN);
		break;
	case MR_WEATHER_SNOW:
		if (!rnd(4)) part_add(P_SNOW, x, -2, rnd(5) - 2, 6 + rnd(8), 900, MR_C_CREAM);
		break;
	case MR_WEATHER_RAIN:
		part_add(P_RAIN, x, -4, -6, 64 + rnd(16), 120, MR_C_CREAM);
		break;
	case MR_WEATHER_FIREFLIES:
		if (weather_n < 12 && !rnd(8))
			part_add(P_FIREFLY, x, 40 + rnd(140), 0, 0, 400 + rnd(300), MR_C_CREAM);
		break;
	default:
		break;
	}
}

/* -- the mouse --
 *
 * At the start of a level, or back at the last mouse hole after a pit.
 * A level begins at the M; a lit hole moves where the mouse comes back. */
static void player_reset(int32_t hx, int32_t hy) {
	player.x = TOFP(hx);
	player.y = TOFP(hy);
	player.vx = player.vy = 0;
	player.on_ground = false;
	player.face_left = false;
	player.anim = 0;
	player.alive = true;
	player.air_jumps = 0;
	player.in_water = false;
	idle_frames = 0;
}

static void foes_reset(void) {
	foe_count = mr_foe_count;
	for (int i = 0; i < foe_count; i++) {
		mr_foe_t *f = &foes[i];
		memset(f, 0, sizeof(*f));
		f->kind = mr_foe_spawns[i].kind;
		f->home_x = mr_foe_spawns[i].x;
		f->home_y = mr_foe_spawns[i].y;
		f->a.x = TOFP(f->home_x);
		f->a.y = TOFP(f->home_y);
		f->a.alive = true;
		f->dir = (i & 1) ? 1 : -1;
		switch (f->kind) {
		case MR_FOE_SLEEPER: f->state = FS_SLEEP; break;
		case MR_FOE_FROG:    f->state = FS_SIT; f->timer = (int16_t)(40 + (i * 37) % 80); break;
		case MR_FOE_OWL:     f->state = FS_FLY; f->a.face_left = f->dir < 0; break;
		default:
			/* as the cats always have: every other one starts right */
			f->state = FS_WALK;
			f->a.vx = (i & 1) ? cat_speed : -cat_speed;
			f->a.face_left = f->a.vx < 0;
			break;
		}
	}
}

/* -- the ? boxes' presents: power-ups that pop out and fall -- */
#define MR_MAX_ITEMS 6
typedef struct {
	mr_actor_t a;           /* hitbox 12 x 14, inset 2 in the tile */
	uint8_t tile;
	uint16_t age;
} mr_item_t;
static mr_item_t items[MR_MAX_ITEMS];

/* -- biscuits: stood on, they crack, crumble, and come back -- */
#define MR_MAX_CRUMBLES 32
typedef struct { int16_t c, r, t; uint8_t gone, used; } mr_crumble_t;
static mr_crumble_t crumbles[MR_MAX_CRUMBLES];

/* -- moving platforms (round 4, docs/mausrun.md "Moving platforms") --
 *
 * Each run of three or more track cells (P) in a row carries a plank
 * two tiles wide, to and fro at half a pixel a frame; it is solid from
 * above, as a shelf is, and carries whoever stands on it. */
#define MR_MAX_PLATS 16
#define MR_PLAT_W 32
#define MR_PLAT_SPEED 8
typedef struct { int32_t x, x0, x1, y; int16_t vx; } mr_plat_t;   /* x in 1/16 px */
static mr_plat_t plats[MR_MAX_PLATS];
static int plat_count;
static int riding = -1;           /* the platform the mouse stands on */

static void plats_init(void) {
	int r, c;
	plat_count = 0;
	riding = -1;
	for (r = 0; r < MR_LEVEL_ROWS; r++)
		for (c = 0; c < MR_LEVEL_COLS; ) {
			int n = 0;
			while (c + n < MR_LEVEL_COLS && mr_level[r][c + n] == MR_TILE_TRACK) n++;
			if (n >= 3 && plat_count < MR_MAX_PLATS) {
				mr_plat_t *p = &plats[plat_count++];
				p->x0 = TOFP(c * MR_TILE_W);
				p->x1 = TOFP((c + n) * MR_TILE_W - MR_PLAT_W);
				p->x = p->x0;
				p->y = r * MR_TILE_H;
				p->vx = MR_PLAT_SPEED;
			}
			c += n ? n : 1;
		}
}

/* Move every plank, and the mouse with the one it stands on. */
static void plats_update(void) {
	int i;
	for (i = 0; i < plat_count; i++) {
		mr_plat_t *p = &plats[i];
		int32_t before = p->x;
		p->x += p->vx;
		if (p->x >= p->x1) { p->x = p->x1; p->vx = -MR_PLAT_SPEED; }
		if (p->x <= p->x0) { p->x = p->x0; p->vx = MR_PLAT_SPEED; }
		if (i == riding && player.alive) {
			int32_t nx = player.x + (p->x - before);
			if (!mr_box_hits_solid(TOPX(nx), TOPX(player.y), MR_PLAYER_W, MR_PLAYER_H))
				player.x = nx;
		}
	}
}

/* After the mouse has moved: did it come down on a plank, or leave
 * one? A plank is a shelf: only from above. */
static bool plats_land(int32_t prev_y) {
	int i, was = riding;
	int32_t px = TOPX(player.x);
	int32_t ob = TOPX(prev_y) + MR_PLAYER_H, nb = TOPX(player.y) + MR_PLAYER_H;
	riding = -1;
	if (player.vy < 0) return false;
	for (i = 0; i < plat_count; i++) {
		const mr_plat_t *p = &plats[i];
		int32_t qx = TOPX(p->x);
		if (px + MR_PLAYER_W <= qx || px >= qx + MR_PLAT_W) continue;
		if (ob <= p->y && nb >= p->y) {
			player.y = TOFP(p->y - MR_PLAYER_H);
			player.vy = 0;
			player.on_ground = true;
			riding = i;
			return was != i;
		}
	}
	return false;
}

/* The mouse and everyone else back where the level starts them. The
 * map -- the cheese -- is put back only by loading the level again. */
static void world_reset(void) {

	int r, c;

	player_reset(mr_start_x + MR_PLAYER_XOFF, mr_start_y);
	check_x = TOPX(player.x);
	check_y = TOPX(player.y);
	foes_reset();
	memset(items, 0, sizeof(items));
	memset(crumbles, 0, sizeof(crumbles));
	memset(pw_left, 0, sizeof(pw_left));
	pw_bubble = false;
	hurt_frames = 0;
	qbox_next = 0;
	star_found = false;
	parts_clear();
	plats_init();
	day_set(mr_level_info.theme);

	/* the goal, for the easy arrow: the wheel, or the end */
	goal_x = MR_WORLD_W - MR_TILE_W;
	for (r = 0; r < MR_LEVEL_ROWS; r++)
		for (c = 0; c < MR_LEVEL_COLS; c++)
			if (mr_level[r][c] == MR_TILE_GOAL) goal_x = c * MR_TILE_W;

	/* Both pages are stale. Harmless now that every frame is a full
	 * redraw, but z_game_invalidate() is what a caller is expected to
	 * do after a level change and costs nothing. */
	z_game_invalidate(&game);

}

/* -- levels: from the card, or compiled in --
 *
 * /data/mausrun/levels/01.txt, 02.txt, ... (level.h has the format). A
 * file on the card wins; without one -- or with one that will not
 * parse, which is reported on the console -- the level of that number
 * compiled into the game is played (levels_builtin.c, every level of
 * the tree), so a card without the files still has every level. Files
 * past the last compiled-in level add levels. */
static char level_text[16384];

static void level_path(char *out, int n) {
	strcpy(out, Z_DIR_MAUSRUN_LEVELS "/00.txt");
	{
		char *d = out + strlen(Z_DIR_MAUSRUN_LEVELS) + 1;
		d[0] = (char)('0' + n / 10);
		d[1] = (char)('0' + n % 10);
	}
}

static bool level_load(int n) {
	char path[64], err[80];
	int len;
	level_path(path, n);
	len = fs_read_file(path, level_text, (int)sizeof(level_text) - 1);
	if (len > 0) {
		level_text[len] = 0;
		if (mr_level_parse(level_text, len, err, (int)sizeof(err)) == 0)
			return true;
		printf("mausrun: %s: %s\n", path, err);
	}
	return mr_level_builtin_n(n);
}

/* Levels in a row from 1: on the card, or compiled in. */
static int level_count_files(void) {
	int n;
	char path[64];
	for (n = 1; n <= MR_LEVELS_MAX; n++) {
		if (n <= mr_builtin_count) continue;
		level_path(path, n);
		if (fs_size(path) <= 0) break;
	}
	return n > 1 ? n - 1 : 1;
}


/* Level n from the start: the map, the scenery for its width, the mouse
 * and the cats. */
static bool level_begin(int n) {
	if (!level_load(n)) return false;
	level_no = n;
	cheese = 0;
	level_frames = 0;
	ambient_init();
	world_reset();
	return true;
}

/* -- progress: /data/mausrun/progress.txt (progress.h) -- */

static char prog_old[1024], prog_out[1024];

static void progress_load(void) {
	char path[64];
	int n;
	prog_old[0] = 0;
	if (z_data_file(path, sizeof path, "progress.txt")) {
		n = fs_read_file(path, prog_old, (int)sizeof(prog_old) - 1);
		if (n > 0) prog_old[n] = 0;
		else prog_old[0] = 0;
	}
	mr_progress_parse(prog_old, &prog);
	if (prog.skill < 0) {
		char v[16];
		if (z_cfg_get("apps.arcade.skill", v, sizeof v))
			prog.skill = mr_skill_word(v);
	}
	skill = prog.skill >= 0 ? prog.skill : MR_SKILL_EASY;
}

static void progress_save(void) {
	char path[64];
	int n;
	prog.skill = skill;
	n = mr_progress_format(prog_out, (int)sizeof(prog_out), prog_old, &prog);
	if (n > 0 && z_data_file(path, sizeof path, "progress.txt")) {
		if (fs_write_file(path, prog_out, n) <= 0)
			printf("mausrun: could not save %s\n", path);
		else
			memcpy(prog_old, prog_out, (size_t)n + 1);
	}
}

/* below, with the labels */
static void say(const char *text);
static void scr_fill(int sx, int sy, int w, int h, int c);
static void scr_bits(const void *bits, int stride, int w, int h,
	int sx, int sy, int c);

/* -- the rules (docs/mausrun.md, "What is in a level") ---------------- */

/* A world x in the view, for the pan of a sound. */
static int vx_of(int32_t wx) {
	int v = (int)(wx - game.cam);
	return v < 0 ? 0 : v > Z_GAME_VIEW_W - 1 ? Z_GAME_VIEW_W - 1 : v;
}

static bool on_screen(int32_t wx) {
	return wx > game.cam - MR_SPR_W && wx < game.cam + Z_GAME_VIEW_W;
}

static void cheese_got(int32_t wx) {
	score += 10;
	cheese++;
	snd_play(SND_COIN, vx_of(wx));
	fx_cheese = 8;              /* the sky flashes gold */
}

/* Walked into: a cheese or a power-up (at the pixel wx, wy). */
static void take(uint8_t t, int32_t wx, int32_t wy) {
	int pw = -1;
	switch (t) {
	case MR_TILE_CHEESE:
		cheese_got(wx);
		burst(P_SPARK, wx, wy, 5, 24, MR_C_CREAM);
		return;
	case MR_TILE_STAR:
		star_found = true;
		score += 100;
		snd_play(SND_GOAL, vx_of(wx));
		burst(P_CONFETTI, wx, wy, 24, 36, 0);
		say("You found the star!");
		return;
	case MR_TILE_HEART:
		if (lives < MR_HEARTS_MAX) lives++;
		else score += 50;
		snd_play(SND_HEART, vx_of(wx));
		burst(P_SPARK, wx, wy, 8, 28, MR_C_CREAM);
		say("A heart!");
		return;
	case MR_TILE_BUBBLE:
		pw_bubble = true;
		say("A bubble. One bump for free.");
		break;
	case MR_TILE_FEATHER:
		pw = PW_FEATHER;
		say("A feather. Jump again in the air!");
		break;
	case MR_TILE_MAGNET:
		pw = PW_MAGNET;
		say("A magnet for the cheese!");
		break;
	case MR_TILE_BELL:
		pw = PW_BELL;
		say("A bell. Everyone runs away!");
		break;
	case MR_TILE_SNEAKER:
		pw = PW_SNEAKERS;
		say("Fast shoes!");
		break;
	default:
		return;
	}
	if (pw >= 0) pw_left[pw] = power_frames[skill];
	score += 25;
	snd_play(SND_POWER, vx_of(wx));
	burst(t == MR_TILE_FEATHER ? P_FEATHER : P_SPARK, wx, wy, 8, 28, MR_C_CREAM);
}

/* A foe, or the thorns: a heart less, then a while of blinking when
 * nothing hurts. The mouse stays where it is -- only a pit sends it
 * back. The bubble takes the bump instead. */
static void hurt(void) {
	int32_t cx = TOPX(player.x) + MR_PLAYER_W / 2;
	if (!player.alive || hurt_frames > 0) return;
	if (pw_bubble) {
		pw_bubble = false;
		hurt_frames = 60;
		snd_play(SND_POP, vx_of(cx));
		burst(P_DROP, cx, TOPX(player.y) + 6, 10, 24, MR_C_CREAM);
		return;
	}
	lives--;
	snd_play(SND_OUCH, vx_of(cx));
	player.vy = -56;
	player.on_ground = false;
	hurt_frames = MR_HURT_FRAMES;
	fx_death = 40;
	if (lives <= 0) {
		lives = 0;
		player.alive = false;
		fx_death = 120;
	}
}

/* Up again after the nap: back on its beat, the way it was going -- a
 * sleeping cat woken by a landing mouse comes for it. */
static void foe_wake(mr_foe_t *f, bool toward_mouse) {
	int32_t ax = TOPX(f->a.x);
	f->state = f->kind == MR_FOE_FROG ? FS_SIT : f->kind == MR_FOE_OWL ? FS_FLY : FS_WALK;
	f->timer = 60;
	if (f->state == FS_WALK) {
		int d = f->dir ? f->dir : 1;
		if (toward_mouse) d = TOPX(player.x) < ax ? -1 : 1;
		f->a.vx = d * cat_speed;
		f->a.face_left = d < 0;
	}
	if (on_screen(ax)) snd_play(SND_WAKE, vx_of(ax));
}

/* The mouse came down on its head. Never hurt: dizzy, then a nap. */
static void stomp(mr_foe_t *f) {
	int32_t ax = TOPX(f->a.x);
	player.vy = MR_BOUNCE_SPEED;
	player.on_ground = false;
	snd_play(SND_STOMP, vx_of(ax));
	burst(P_SPARK, ax + 8, TOPX(f->a.y) + 4, 6, 24, MR_C_CREAM);
	switch (f->state) {
	case FS_SLEEP:
		break;                  /* sound asleep: a bounce, that is all */
	case FS_NAP:
		f->timer = nap_frames[skill];
		break;
	case FS_DIZZY:
		f->timer = MR_DIZZY_FRAMES;
		break;
	default:
		if (f->a.vx) f->dir = f->a.vx < 0 ? -1 : 1;
		f->state = FS_DIZZY;
		f->timer = MR_DIZZY_FRAMES;
		score += 20;
		break;
	}
	f->a.vx = 0;
}

static bool foe_harmful(const mr_foe_t *f) {
	if (pw_left[PW_BELL]) return false;
	return f->state == FS_WALK || f->state == FS_SIT || f->state == FS_HOP ||
		f->state == FS_FLY;
}

/* Where the foe's head is, from the top of its 16x16 cell. */
static int foe_top(const mr_foe_t *f) {
	if (f->kind == MR_FOE_OWL) return 2;
	if (f->kind == MR_FOE_FROG) return f->state == FS_HOP ? 2 : 6;
	return (f->state == FS_NAP || f->state == FS_SLEEP) ? 8 : 3;
}

/* A gentle bob for the owls, pixels. */
static const int8_t owl_bob[16] = {
	0, 2, 4, 5, 6, 5, 4, 2, 0, -2, -4, -5, -6, -5, -4, -2
};

static void foe_update(mr_foe_t *f) {

	mr_actor_t *a = &f->a;
	bool fleeing = pw_left[PW_BELL] > 0 && player.alive;
	int32_t ax = TOPX(a->x);

	if (!a->alive) return;
	if (f->timer > 0) f->timer--;
	a->anim++;

	switch (f->state) {
	case FS_DIZZY:
		if (!f->timer) {
			f->state = FS_NAP;
			f->timer = nap_frames[skill];
		}
		break;
	case FS_NAP:
	case FS_SLEEP:
		if ((a->anim & 63) == 0 && on_screen(ax))
			part_add(P_ZZZ, ax + 10, TOPX(a->y) + (f->kind == MR_FOE_OWL ? 0 : 6),
				4, -6, 50, MR_C_CREAM);
		if (f->state == FS_NAP && !f->timer) foe_wake(f, false);
		break;
	case FS_SIT:
		if (!f->timer) {
			int32_t d = ax - f->home_x;
			if (d > 24) f->dir = -1;
			else if (d < -24) f->dir = 1;
			else if (fleeing) f->dir = TOPX(player.x) < ax ? 1 : -1;
			else f->dir = (int16_t)-f->dir;
			a->vy = -72;
			a->vx = (int32_t)f->dir * 12;
			a->face_left = f->dir < 0;
			a->on_ground = false;
			f->state = FS_HOP;
			if (on_screen(ax)) snd_play(SND_CROAK, vx_of(ax));
		}
		break;
	case FS_FLY: {
		int32_t d;
		a->x += (int32_t)f->dir * (fleeing ? 14 : 8);
		d = TOPX(a->x) - f->home_x;
		if (d > 48) f->dir = -1;
		if (d < -48) f->dir = 1;
		a->face_left = f->dir < 0;
		a->y = TOFP(f->home_y + (fleeing ? -24 : 0) + owl_bob[(a->anim >> 3) & 15]);
		if ((a->anim & 511) == 0 && on_screen(ax)) snd_play(SND_HOOT, vx_of(ax));
		return;
	}
	case FS_WALK:
		/* the bell: away from the mouse, half as fast again */
		if (fleeing) {
			int s = cat_speed + cat_speed / 2;
			a->vx = TOPX(player.x) < ax ? s : -s;
		} else {
			a->vx = a->vx < 0 ? -cat_speed : cat_speed;
		}
		a->face_left = a->vx < 0;
		break;
	default:
		break;
	}

	/* an owl dizzy or asleep hangs where it was */
	if (f->kind == MR_FOE_OWL) return;
	if (f->state != FS_WALK && f->state != FS_HOP) a->vx = 0;

	a->vy += MR_GRAVITY;
	if (a->vy > MR_MAX_FALL) a->vy = MR_MAX_FALL;

	{
		int32_t before = a->x;
		mr_move_actor(a, MR_SPR_W, MR_SPR_H);
		if (f->state == FS_WALK) {
			/* Turned around because something blocked us. */
			if (a->x == before && a->vy == 0) {
				a->vx = -a->vx;
				a->face_left = a->vx < 0;
			}
			/* Turn around at a ledge rather than walking off it.
			 * Sample just beyond the leading foot: if there is no
			 * floor there, reverse. Without this every cat ends up at
			 * the bottom of the nearest pit within a few seconds and
			 * the level looks empty. */
			if (a->on_ground) {
				int32_t probe_x = TOPX(a->x) + (a->vx < 0 ? -2 : MR_SPR_W + 1);
				int32_t probe_y = TOPX(a->y) + MR_SPR_H + 2;
				uint8_t t = mr_tile_at(probe_x, probe_y);
				if (!mr_tile_solid(t) && t != MR_TILE_ONEWAY) {
					a->vx = -a->vx;
					a->face_left = a->vx < 0;
				}
			}
		} else if (f->state == FS_HOP && a->on_ground) {
			f->state = FS_SIT;
			f->timer = (int16_t)(90 + rnd(60));
			a->vx = 0;
		}
	}
	if (TOPX(a->y) > MR_WORLD_H) a->alive = false;
}

/* The mouse against everyone, after both have moved. Coming down onto
 * a head is a stomp; anything else, with a foe that is up and about,
 * is a bump. */
static void foes_touch(int32_t prev_y) {
	int32_t px = TOPX(player.x), py = TOPX(player.y);
	int32_t pbot0 = TOPX(prev_y) + MR_PLAYER_H;
	int i;
	for (i = 0; i < foe_count && player.alive; i++) {
		mr_foe_t *f = &foes[i];
		int32_t fx = TOPX(f->a.x), fy = TOPX(f->a.y), top = fy + foe_top(f);
		if (!f->a.alive) continue;
		if (px >= fx + MR_SPR_W - 2 || px + MR_PLAYER_W <= fx + 2) continue;
		if (py >= fy + MR_SPR_H - 2 || py + MR_PLAYER_H <= top) continue;
		if (player.y > prev_y && pbot0 <= top + 6) {
			stomp(f);
			continue;
		}
		if (foe_harmful(f)) hurt();
	}
}

/* A ? box bumped from below: used, and out pops a present -- a heart
 * when the mouse is short of them, otherwise the next in turn. */
static void qbox_open(int c, int r) {
	static const uint8_t gifts[5] = {
		MR_TILE_FEATHER, MR_TILE_BUBBLE, MR_TILE_SNEAKER, MR_TILE_MAGNET, MR_TILE_BELL
	};
	mr_item_t *it;
	int i;
	mr_level[r][c] = MR_TILE_USEDBOX;
	snd_play(SND_BUMP, vx_of(c * MR_TILE_W));
	burst(P_SPARK, c * MR_TILE_W + 8, r * MR_TILE_H, 6, 24, MR_C_CREAM);
	for (i = 0; i < MR_MAX_ITEMS && items[i].tile; i++)
		;
	if (i == MR_MAX_ITEMS) i = 0;
	it = &items[i];
	memset(it, 0, sizeof(*it));
	it->tile = lives < lives_for[skill] ? MR_TILE_HEART : gifts[qbox_next++ % 5];
	it->a.x = TOFP(c * MR_TILE_W + 2);
	it->a.y = TOFP(r * MR_TILE_H - 14);
	it->a.vy = -64;
	it->a.vx = player.face_left ? -10 : 10;
	it->a.alive = true;
}

static void items_update(void) {
	int i;
	int32_t px = TOPX(player.x), py = TOPX(player.y);
	for (i = 0; i < MR_MAX_ITEMS; i++) {
		mr_item_t *it = &items[i];
		mr_actor_t *a = &it->a;
		int32_t ix, iy;
		if (!it->tile) continue;
		it->age++;
		a->vy += MR_GRAVITY;
		if (a->vy > MR_MAX_FALL) a->vy = MR_MAX_FALL;
		if (a->on_ground) a->vx = 0;
		mr_move_actor(a, 12, 14);
		ix = TOPX(a->x);
		iy = TOPX(a->y);
		if (iy > MR_WORLD_H) { it->tile = 0; continue; }
		if (player.alive && it->age > 12 &&
		    px < ix + 12 && px + MR_PLAYER_W > ix &&
		    py < iy + 14 && py + MR_PLAYER_H > iy) {
			take(it->tile, ix + 6, iy + 7);
			it->tile = 0;
		}
	}
}

static void crumble_start(int c, int r) {
	int i, free_i = -1;
	for (i = 0; i < MR_MAX_CRUMBLES; i++) {
		if (crumbles[i].used && crumbles[i].c == c && crumbles[i].r == r) return;
		if (!crumbles[i].used && free_i < 0) free_i = i;
	}
	if (free_i < 0) return;
	crumbles[free_i].c = (int16_t)c;
	crumbles[free_i].r = (int16_t)r;
	crumbles[free_i].t = crumble_frames[skill];
	crumbles[free_i].gone = 0;
	crumbles[free_i].used = 1;
}

static void crumbles_update(void) {
	int i;
	for (i = 0; i < MR_MAX_CRUMBLES; i++) {
		mr_crumble_t *k = &crumbles[i];
		int32_t x0, y0;
		if (!k->used) continue;
		x0 = k->c * MR_TILE_W;
		y0 = k->r * MR_TILE_H;
		if (--k->t > 0) {
			if (!k->gone && k->t == crumble_frames[skill] / 2)
				mr_level[k->r][k->c] = MR_TILE_CRACKED;
			continue;
		}
		if (!k->gone) {
			mr_level[k->r][k->c] = MR_TILE_EMPTY;
			k->gone = 1;
			k->t = 300;
			burst(P_CRUMB, x0 + 8, y0 + 6, 10, 20, MR_C_DARK);
			snd_play(SND_CRUMBLE, vx_of(x0));
		} else {
			/* baked again -- unless the mouse is in the way */
			int32_t px = TOPX(player.x), py = TOPX(player.y);
			if (player.alive && px < x0 + MR_TILE_W && px + MR_PLAYER_W > x0 &&
			    py < y0 + MR_TILE_H && py + MR_PLAYER_H > y0) {
				k->t = 30;
				continue;
			}
			mr_level[k->r][k->c] = MR_TILE_CRUMBLE;
			k->used = 0;
		}
	}
}

static void update_player(const mr_input_t *in) {

	mr_step_ev_t ev;
	unsigned powers = 0;
	int dir = in->left ? -1 : in->right ? 1 : 0;
	int32_t px, py, cx, cy;
	int c, r;

	if (!player.alive) return;
	if (pw_left[PW_FEATHER]) powers |= MR_PW_FEATHER;
	if (pw_left[PW_SNEAKERS]) powers |= MR_PW_SNEAKERS;

	/* on a plank: as good as ground, to jump from */
	if (riding >= 0) player.on_ground = true;
	{
		int32_t prev_y = player.y;
		mr_player_step(&player, dir, in->jump_edge, powers, &ev);
		if (plats_land(prev_y) && !ev.jumped) ev.landed = true;
	}
	px = TOPX(player.x);
	py = TOPX(player.y);
	cx = px + MR_PLAYER_W / 2;
	cy = py + MR_PLAYER_H / 2;
	if (dir || in->jump) idle_frames = 0;
	else idle_frames++;

	if (ev.jumped) snd_play(SND_JUMP, vx_of(cx));
	if (ev.air_jump) {
		snd_play(SND_JUMP, vx_of(cx));
		burst(P_FEATHER, cx, py + MR_PLAYER_H, 3, 12, MR_C_CREAM);
	}
	if (ev.landed) {
		snd_play(SND_LAND, vx_of(cx));
		part_add(P_DUST, px, py + MR_PLAYER_H - 2, -14, -4, 14, MR_C_CREAM);
		part_add(P_DUST, px + MR_PLAYER_W - 2, py + MR_PLAYER_H - 2, 14, -4, 14, MR_C_CREAM);
	}
	if (ev.sprung) {
		snd_play(SND_SPRING, vx_of(cx));
		burst(P_DUST, cx, py + MR_PLAYER_H, 6, 20, MR_C_CREAM);
	}
	if (ev.splash) {
		snd_play(SND_SPLASH, vx_of(cx));
		burst(P_DROP, cx, py + MR_PLAYER_H / 2, 10, 28, MR_C_CREAM);
	}
	if (ev.bumped) qbox_open(ev.bump_c, ev.bump_r);

	/* Cheese and the power-ups. Checked at the hitbox centre rather
	 * than by overlap, which is close enough at this tile size and
	 * means a cheese is collected when you are clearly on it rather
	 * than when a corner grazes it. Nothing to repaint: every frame
	 * redraws the visible area from the map. */
	c = (int)(cx / MR_TILE_W);
	r = (int)(cy / MR_TILE_H);
	if (cx >= 0 && cy >= 0 && c < MR_LEVEL_COLS && r < MR_LEVEL_ROWS &&
	    mr_tile_item(mr_level[r][c])) {
		uint8_t t = mr_level[r][c];
		mr_level[r][c] = MR_TILE_EMPTY;
		take(t, c * MR_TILE_W + 8, r * MR_TILE_H + 8);
	}

	/* the magnet: cheese nearby flies to the mouse */
	if (pw_left[PW_MAGNET] && cy >= 0) {
		int rr, cc;
		for (rr = r - 3; rr <= r + 3; rr++)
			for (cc = c - 4; cc <= c + 4; cc++) {
				if (rr < 0 || cc < 0 || rr >= MR_LEVEL_ROWS || cc >= MR_LEVEL_COLS) continue;
				if (mr_level[rr][cc] != MR_TILE_CHEESE) continue;
				mr_level[rr][cc] = MR_TILE_EMPTY;
				if (!part_add(P_CHEESE, cc * MR_TILE_W, rr * MR_TILE_H, 0, 0, 120, MR_C_CREAM))
					cheese_got(cc * MR_TILE_W);
			}
	}

	/* a mouse hole: lit, and the mouse comes back here after a pit */
	if (py >= 0) {
		int c0 = (int)(px / MR_TILE_W), c1 = (int)((px + MR_PLAYER_W - 1) / MR_TILE_W);
		int r0 = (int)(py / MR_TILE_H), r1 = (int)((py + MR_PLAYER_H - 1) / MR_TILE_H);
		for (r = r0; r <= r1 && r < MR_LEVEL_ROWS; r++)
			for (c = c0; c <= c1 && c < MR_LEVEL_COLS; c++)
				if (mr_level[r][c] == MR_TILE_HOLE) {
					mr_level[r][c] = MR_TILE_HOLE_LIT;
					check_x = c * MR_TILE_W + (MR_TILE_W - MR_PLAYER_W) / 2;
					check_y = r * MR_TILE_H + MR_TILE_H - MR_PLAYER_H;
					snd_play(SND_CHECK, vx_of(c * MR_TILE_W));
					burst(P_CONFETTI, c * MR_TILE_W + 8, r * MR_TILE_H + 4, 14, 24, 0);
				}
	}

	/* the end: the cheese wheel, or without one the last two columns */
	if (mr_level_info.goal ?
	    mr_box_touches(px, py, MR_PLAYER_W, MR_PLAYER_H, MR_TILE_GOAL) :
	    px >= MR_WORLD_W - 2 * MR_TILE_W) {
		won = true;
		snd_play(SND_GOAL, vx_of(cx));
		burst(P_CONFETTI, cx, py, 30, 40, 0);
	}

	if (mr_thorns_hit(&player)) hurt();

	/* standing on a biscuit starts it crumbling */
	if (player.on_ground) {
		int32_t fy = py + MR_PLAYER_H;
		int k;
		for (k = 0; k < 2; k++) {
			int32_t fx = px + (k ? MR_PLAYER_W - 1 : 0);
			uint8_t t = mr_tile_at(fx, fy);
			if (t == MR_TILE_CRUMBLE || t == MR_TILE_CRACKED)
				crumble_start((int)(fx / MR_TILE_W), (int)(fy / MR_TILE_H));
		}
	}

	/* landing near a sleeping cat wakes it */
	if (ev.landed) {
		int i;
		for (i = 0; i < foe_count; i++) {
			mr_foe_t *f = &foes[i];
			int32_t dx = TOPX(f->a.x) + MR_SPR_W / 2 - cx;
			int32_t dy = TOPX(f->a.y) + MR_SPR_H / 2 - cy;
			if (f->state != FS_SLEEP || !f->a.alive) continue;
			if (dx < 0) dx = -dx;
			if (dy < 0) dy = -dy;
			if (dx < wake_dist[skill] + MR_SPR_W / 2 && dy < 32) foe_wake(f, true);
		}
	}

	/* Fell into a pit: leaving the bottom of the world. A heart, and
	 * back to the last mouse hole (ST_DYING). */
	if (py > MR_WORLD_H) {
		player.alive = false;
		lives--;
		if (lives < 0) lives = 0;
		snd_play(SND_FALL, vx_of(cx));
		fx_death = 120;
	}

}

/* One frame of play: the mouse, then everyone else, then who touched
 * whom. */
static void play_step(const mr_input_t *in) {
	int32_t prev_y = player.y;
	int i;
	plats_update();
	update_player(in);
	for (i = 0; i < foe_count; i++) foe_update(&foes[i]);
	if (player.alive) foes_touch(prev_y);
	items_update();
	crumbles_update();
	for (i = 0; i < PW_TIMED; i++)
		if (pw_left[i] > 0) {
			pw_left[i]--;
			if (pw_left[i] == 0) snd_play(SND_TICK, -1);
		}
	if (hurt_frames > 0) hurt_frames--;
}

/* -- rendering ------------------------------------------------------ */

typedef struct {
	const uint16_t *d, *m;
	const uint32_t *hd, *hm, *hdm, *hmm;
} mr_spr_t;

#define SPR(n) { mr_spr_##n, mr_msk_##n, mr_hspr_##n, mr_hmsk_##n, \
	mr_hspr_##n##_m, mr_hmsk_##n##_m }

enum {
	S_CAT1 = 0, S_CAT2, S_CATZ, S_FROG1, S_FROG2, S_OWL1, S_OWL2, S_RING,
	S_STAND, S_RUN1, S_RUN2, S_JUMP
};

static const mr_spr_t spr_tab[] = {
	SPR(cat1), SPR(cat2), SPR(cat_sleep), SPR(frog1), SPR(frog2),
	SPR(owl1), SPR(owl2), SPR(ring),
	SPR(mouse_stand), SPR(mouse_run1), SPR(mouse_run2), SPR(mouse_jump)
};

/* A sprite at a world position, facing left or right: the software
 * path mirrors at runtime; the hardware path picks a pre-mirrored
 * bitmap, because the blitter has no reverse mode. */
static void spr_put(int s, int32_t wx, int32_t wy, bool left) {
	const mr_spr_t *p = &spr_tab[s];
	if (rop_hw)
		spr_draw(p->d, p->m, left ? p->hdm : p->hd, left ? p->hmm : p->hm,
			world_to_fb_x(wx), world_to_fb_y(wy));
	else
		spr_draw_sw_mirror(p->d, p->m, world_to_fb_x(wx), world_to_fb_y(wy), left);
}

/* A little rectangle in the view, if it is wholly in it. */
static void dot(int32_t sx, int32_t sy, int w, int h, int c) {
	if (sx < 0 || sy < 0 || sx + w > Z_GAME_VIEW_W || sy + h > Z_GAME_VIEW_H) return;
	scr_fill((int)sx, (int)sy, w, h, c);
}

/* An ellipse of eight points, for the dizzy stars. */
static const int8_t star_dx[8] = { 7, 5, 0, -5, -7, -5, 0, 5 };
static const int8_t star_dy[8] = { 0, 2, 3, 2, 0, -2, -3, -2 };

static void draw_foe(const mr_foe_t *f) {
	int32_t wx = TOPX(f->a.x), wy = TOPX(f->a.y);
	int s;
	if (!f->a.alive) return;
	if (wx + MR_SPR_W < game.cam || wx > game.cam + Z_GAME_VIEW_W) return;
	switch (f->kind) {
	case MR_FOE_FROG:
		spr_c = MR_C_GREEN; spr_o = MR_C_DARK;
		s = f->state == FS_HOP ? S_FROG2 : S_FROG1;
		break;
	case MR_FOE_OWL:
		spr_c = MR_C_CREAM; spr_o = MR_C_DARK;
		s = (f->state == FS_FLY && (f->a.anim & 16)) ? S_OWL2 : S_OWL1;
		break;
	default:
		spr_c = MR_C_DARK; spr_o = MR_C_DARK;
		s = (f->state == FS_NAP || f->state == FS_SLEEP) ? S_CATZ :
			(f->state == FS_WALK && (f->a.anim & 16)) ? S_CAT2 : S_CAT1;
		break;
	}
	spr_put(s, wx, wy, f->a.face_left);
	if (f->state == FS_DIZZY) {
		int k, a = (f->a.anim >> 2) & 7;
		int32_t sx = wx - game.cam + 7, sy = wy + foe_top(f) - 2;
		for (k = 0; k < 3; k++) {
			int j = (a + k * 3) & 7;
			dot(sx + star_dx[j], sy + star_dy[j], 2, 2, MR_C_CREAM);
		}
	}
}

/* the zzz: 4 x 4, LSB the leftmost pixel */
static const uint32_t bm_z[4] = { 0xf, 0x4, 0x2, 0xf };

static void parts_draw(void) {
	int i;
	for (i = 0; i < MR_MAX_PARTS; i++) {
		const mr_part_t *p = &parts[i];
		int32_t sx, sy;
		if (p->kind == P_NONE) continue;
		sx = TOPX(p->x) - game.cam;
		sy = TOPX(p->y);
		switch (p->kind) {
		case P_ZZZ:
			if (sx >= 0 && sy >= 0 && sx <= Z_GAME_VIEW_W - 4 && sy <= Z_GAME_VIEW_H - 4)
				scr_bits(bm_z, 4, 4, 4, (int)sx, (int)sy, p->c);
			break;
		case P_CHEESE:
			if (sy >= 0 && sy <= Z_GAME_VIEW_H - MR_TILE_H &&
			    sx > -MR_TILE_W && sx < Z_GAME_VIEW_W)
				draw_tile(MR_TILE_CHEESE, world_to_fb_x(TOPX(p->x)), world_to_fb_y(sy));
			break;
		case P_FIREFLY:
			if ((p->life >> 3) & 1) dot(sx, sy, 2, 2, p->c);
			break;
		case P_RAIN:    dot(sx, sy, 1, 4, p->c); break;
		case P_LEAF:    dot(sx, sy, 3, 2, p->c); break;
		case P_FEATHER: dot(sx, sy, 3, 1, p->c); break;
		case P_SNOW:    dot(sx, sy, (i & 1) + 1, (i & 1) + 1, p->c); break;
		default:        dot(sx, sy, 2, 2, p->c); break;
		}
	}
}

static void draw_actors(void) {

	int i;

	/* the planks */
	for (i = 0; i < plat_count; i++) {
		int32_t wx = TOPX(plats[i].x);
		int k;
		if (wx + MR_PLAT_W < game.cam || wx > game.cam + Z_GAME_VIEW_W) continue;
		for (k = 0; k < 2; k++)
			draw_tile(MR_TILE_PLATFORM, world_to_fb_x(wx + k * MR_TILE_W),
				world_to_fb_y(plats[i].y));
	}

	/* the presents out of the ? boxes */
	for (i = 0; i < MR_MAX_ITEMS; i++) {
		int32_t wx = TOPX(items[i].a.x) - 2, wy = TOPX(items[i].a.y) - 2;
		if (!items[i].tile) continue;
		if (wy < 0 || wy > Z_GAME_VIEW_H - MR_TILE_H) continue;
		if (wx + MR_TILE_W < game.cam || wx > game.cam + Z_GAME_VIEW_W) continue;
		draw_tile(items[i].tile, world_to_fb_x(wx), world_to_fb_y(wy));
	}

	/* Foes first, player last, so the player is never hidden behind a
	 * cat at the moment of the collision that matters. */
	for (i = 0; i < foe_count; i++) draw_foe(&foes[i]);

	/* blinking while nothing can hurt it */
	if (player.alive && !(hurt_frames & 4)) {
		int32_t wx = TOPX(player.x) - MR_PLAYER_XOFF;
		int32_t wy = TOPX(player.y);
		int s = !player.on_ground ? S_JUMP :
			player.vx ? ((player.anim & 8) ? S_RUN1 : S_RUN2) : S_STAND;
		spr_c = MR_C_CREAM; spr_o = MR_C_DARK;
		spr_put(s, wx, wy, player.face_left);
		if (pw_bubble) {
			spr_c = MR_C_CREAM; spr_o = MR_C_CREAM;
			spr_put(S_RING, wx, wy - 1, false);
		}
	}

	parts_draw();

}

/* -- the player's text: labels ---------------------------------------
 *
 * Words on screen -- the status line, the menu, the panels -- are
 * rendered once into a 1bpp bitmap (a label), at a scale, and drawn as
 * often as wanted with the same blits the art uses: transparent, in a
 * palette colour, over a filled box. A label is remade only when what
 * it says changes.
 *
 * The 8x8 font at the 320x240 game view is 16 screen pixels a letter:
 * readable from the sofa, which the HUD's 5x8 debug line never was.
 * Three glyphs are our own: \1 a heart, \2 a piece of cheese, \3 a
 * star.
 */
#define LBL_STRIDE 44                     /* 320 px + a word the blitter may read */
#define LBL_H 24                          /* 8 x 3 */

typedef struct {
	uint32_t bits[LBL_H * (LBL_STRIDE / 4) + 1];
	int16_t w, h;
	char text[44];
	int8_t scale;
} mr_label_t;

static const uint8_t glyph_heart[8] = { 0x00, 0x6c, 0xfe, 0xfe, 0x7c, 0x38, 0x10, 0x00 };
static const uint8_t glyph_cheese[8] = { 0x00, 0x06, 0x1e, 0x7a, 0xfe, 0xee, 0xfe, 0x00 };
static const uint8_t glyph_star[8] = { 0x10, 0x10, 0xfe, 0x7c, 0x38, 0x6c, 0x44, 0x00 };

static void label_make(mr_label_t *l, const char *s, int scale) {
	const z_font_t *f = &z_font_8x8;
	int x = 0, n, i, r, b, k;
	if (scale < 1) scale = 1;
	if (scale > 3) scale = 3;
	/* already says that */
	if (l->scale == scale && strcmp(l->text, s) == 0 && l->w) return;
	n = (int)strlen(s);
	if (n > (int)sizeof(l->text) - 1) n = (int)sizeof(l->text) - 1;
	memcpy(l->text, s, (size_t)n);
	l->text[n] = 0;
	l->scale = (int8_t)scale;
	memset(l->bits, 0, sizeof(l->bits));
	for (i = 0; i < n && (x + 8) * scale <= 320; i++) {
		const uint8_t *g;
		uint8_t c = (uint8_t)s[i];
		int gi;
		if (c == 1) g = glyph_heart;
		else if (c == 2) g = glyph_cheese;
		else if (c == 3) g = glyph_star;
		else {
			gi = z_font_index(f, c);
			if (gi < 0) gi = z_font_index(f, '?');
			g = f->glyphs + gi * f->h;
		}
		for (r = 0; r < 8; r++)
			for (b = 0; b < 8; b++) {
				if (!(g[r] & (0x80u >> b))) continue;
				for (k = 0; k < scale * scale; k++) {
					int px = (x + b) * scale + k % scale;
					int py = r * scale + k / scale;
					l->bits[py * (LBL_STRIDE / 4) + (px >> 5)] |= 1u << (px & 31);
				}
			}
		x += 8;
	}
	l->w = (int16_t)(x * scale);
	l->h = (int16_t)(8 * scale);
}

/* The monochrome pixel for a palette colour: the world is drawn light
 * on a black sky, so the light colours are 1. */
static int mono_of(int c) {
	return c == MR_C_CREAM || c == MR_C_GREEN;
}

/* A rectangle of the VIEW (0..319, 0..239) in a colour, through the
 * page layout and, in monochrome, round the torus seam. */
static void scr_fill(int sx, int sy, int w, int h, int c) {
	if (w <= 0 || h <= 0) return;
	if (col_on) {
		c_fill(col_page_x() + sx, sy, w, h, c);
		return;
	}
	{
		int fx = world_to_fb_x(game.cam + sx), fy = world_to_fb_y(sy);
		int w1 = Z_SCREEN_W - fx;
		if (w1 > w) w1 = w;
		hud_blits++;
		z_fb_hw_fill_rect_async(fx, fy, w1, h, mono_of(c));
		if (w1 < w) {
			hud_blits++;
			z_fb_hw_fill_rect_async(0, fy, w - w1, h, mono_of(c));
		}
	}
}

/* A 1bpp bitmap at a place in the view, set bits in colour c and clear
 * ones untouched. */
static void scr_bits(const void *bits, int stride, int w, int h,
	int sx, int sy, int c) {
	if (w <= 0 || h <= 0) return;
	if (col_on) {
		c_over(bits, stride, 0, 0, col_page_x() + sx, sy, w, h, c);
		return;
	}
	{
		int fx = world_to_fb_x(game.cam + sx), fy = world_to_fb_y(sy);
		int w1 = Z_SCREEN_W - fx;
		/* without raster ops every blit is a copy: opaque, which over
		 * a box of black is the same picture */
		int rop = !rop_hw ? Z_ROP_COPY : mono_of(c) ? Z_ROP_OR : Z_ROP_ANDN;
		if (w1 > w) w1 = w;
		hud_blits++;
		z_fb_hw_blit_mem_async(bits, stride, 0, 0, fx, fy, w1, h, rop);
		if (w1 < w) {
			hud_blits++;
			z_fb_hw_blit_mem_async(bits, stride, w1, 0, 0, fy, w - w1, h, rop);
		}
	}
}

static void label_draw(const mr_label_t *l, int sx, int sy, int c) {
	scr_bits(l->bits, LBL_STRIDE, l->w, l->h, sx, sy, c);
}

/* centred across the view */
static void label_centre(const mr_label_t *l, int sy, int c) {
	label_draw(l, (Z_GAME_VIEW_W - l->w) / 2, sy, c);
}

/* A panel: a cream frame round dark brown. */
static void panel(int sx, int sy, int w, int h) {
	scr_fill(sx, sy, w, h, MR_C_CREAM);
	scr_fill(sx + 2, sy + 2, w - 4, h - 4, MR_C_DARK);
}

static void say(const char *text) {
	z_speak(text, Z_TTS_F_INTERRUPT);
}

/* -- the status line --
 *
 * Cheese (got of the level's), hearts, the level, the score: a dark
 * bar along the top of the view, cream letters. */
static mr_label_t hud_left, hud_mid, hud_right;

static void utoa_pad(char *out, uint32_t v, int width) {
	char b[12];
	int n = 0, i;
	do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
	for (i = 0; i < width - n; i++) *out++ = '0';
	while (n) *out++ = b[--n];
	*out = 0;
}

static void hud_draw(void) {
	char b[44], *p = b;
	int i;
	*p++ = 2;
	*p++ = ' ';
	utoa_pad(p, (uint32_t)cheese, 2); p += strlen(p);
	*p++ = '/';
	utoa_pad(p, (uint32_t)mr_cheese_total, 2); p += strlen(p);
	*p++ = ' ';
	*p++ = ' ';
	for (i = 0; i < lives && i < 9; i++) *p++ = 1;
	if (star_found) { *p++ = ' '; *p++ = 3; }
	*p = 0;
	label_make(&hud_left, b, 1);
	strcpy(b, "LEVEL ");
	utoa_pad(b + 6, (uint32_t)level_no, 1);
	label_make(&hud_mid, b, 1);
	utoa_pad(b, (uint32_t)score, 6);
	label_make(&hud_right, b, 1);

	scr_fill(0, 0, Z_GAME_VIEW_W, 12, MR_C_DARK);
	label_draw(&hud_left, 4, 2, MR_C_CREAM);
	label_draw(&hud_mid, 176, 2, MR_C_CREAM);
	label_draw(&hud_right, Z_GAME_VIEW_W - 4 - hud_right.w, 2, MR_C_CREAM);

	/* the power-ups in hand, under the bar: each with how long it has
	 * left, blinking at the end; the bubble until it pops */
	{
		static const uint8_t icon[PW_TIMED] = {
			MR_TILE_FEATHER, MR_TILE_MAGNET, MR_TILE_BELL, MR_TILE_SNEAKER
		};
		int x = 4;
		for (i = 0; i < PW_TIMED; i++) {
			int full = power_frames[skill];
			if (!pw_left[i]) continue;
			if (!(pw_left[i] < 180 && (pw_left[i] & 8))) {
				draw_tile(icon[i], world_to_fb_x(game.cam + x), world_to_fb_y(14));
				scr_fill(x, 31, (16 * pw_left[i] + full - 1) / full, 2, MR_C_CREAM);
			}
			x += 22;
		}
		if (pw_bubble)
			draw_tile(MR_TILE_BUBBLE, world_to_fb_x(game.cam + x), world_to_fb_y(14));
	}

	/* easy: standing still a while, an arrow says which way to go */
	if (skill == MR_SKILL_EASY && st == ST_PLAY && idle_frames > 240 &&
	    (fx_anim & 32) && goal_x > TOPX(player.x) + Z_GAME_VIEW_W / 2) {
		static mr_label_t arrow;
		label_make(&arrow, ">", 3);
		label_draw(&arrow, Z_GAME_VIEW_W - 30, 104, MR_C_CREAM);
	}
}

/* -- the panels -- */

static mr_label_t ui[8];

static void ui_line(int i, const char *s, int scale, int sy) {
	label_make(&ui[i], s, scale);
	label_centre(&ui[i], sy, MR_C_CREAM);
}

enum { MENU_PLAY = 0, MENU_CONTINUE, MENU_SKILL, MENU_QUIT, MENU_ITEMS };

static bool menu_shown(int m) {
	return m != MENU_CONTINUE || prog.unlocked > 1;
}

static void menu_text(int m, char *out) {
	switch (m) {
	case MENU_PLAY:     strcpy(out, "PLAY"); break;
	case MENU_CONTINUE:
		strcpy(out, "CONTINUE: LEVEL ");
		utoa_pad(out + strlen(out), (uint32_t)(prog.unlocked > level_count ?
			level_count : prog.unlocked), 1);
		break;
	case MENU_SKILL:
		strcpy(out, "< ");
		strcat(out, mr_skill_name(skill));
		strcat(out, " >");
		break;
	default:            strcpy(out, "QUIT"); break;
	}
}

static void menu_say(void) {
	char b[48];
	switch (menu) {
	case MENU_PLAY: say("Play"); return;
	case MENU_CONTINUE:
		strcpy(b, "Continue, level ");
		utoa_pad(b + strlen(b), (uint32_t)prog.unlocked, 1);
		say(b);
		return;
	case MENU_SKILL:
		strcpy(b, "Skill, ");
		strcat(b, mr_skill_spoken(skill));
		say(b);
		return;
	default: say("Quit"); return;
	}
}

static void draw_title(void) {
	char b[44];
	int m, y = 76;
	panel(40, 22, 240, 136);
	ui_line(0, "MAUS RUN", 3, 32);
	for (m = 0; m < MENU_ITEMS; m++) {
		char t[44];
		if (!menu_shown(m)) continue;
		menu_text(m, t);
		if (m == menu && m != MENU_SKILL) { strcpy(b, "> "); strcat(b, t); strcat(b, " <"); }
		else if (m == MENU_SKILL && m != menu) strcpy(b, mr_skill_name(skill));
		else strcpy(b, t);
		label_make(&ui[1 + m], b, 1);
		label_centre(&ui[1 + m], y, m == menu ? MR_C_CREAM : MR_C_GREEN);
		y += 14;
	}
	label_make(&ui[5], "M MUSIC  C COLOUR  ESC QUIT", 1);
	label_centre(&ui[5], 146, MR_C_GREEN);
	/* the stars found so far, of every level's three */
	if (mr_progress_stars(&prog) > 0) {
		b[0] = 3;
		b[1] = ' ';
		utoa_pad(b + 2, (uint32_t)mr_progress_stars(&prog), 1);
		strcat(b, " OF ");
		utoa_pad(b + strlen(b), (uint32_t)(3 * level_count), 1);
		label_make(&ui[6], b, 1);
		label_centre(&ui[6], 132, MR_C_CREAM);
	}
}

static void draw_intro(void) {
	char b[44];
	panel(48, 80, 224, 72);
	strcpy(b, "LEVEL ");
	utoa_pad(b + 6, (uint32_t)level_no, 1);
	ui_line(0, b, 2, 90);
	ui_line(1, mr_level_info.name, 1, 114);
	label_make(&ui[2], "SPACE TO GO", 1);
	label_centre(&ui[2], 132, MR_C_GREEN);
}

static void draw_pause(void) {
	panel(64, 88, 192, 56);
	ui_line(0, "PAUSED", 2, 98);
	label_make(&ui[1], "P: GO ON   ESC: MENU", 1);
	label_centre(&ui[1], 124, MR_C_GREEN);
}

static void draw_clear(void) {
	char b[44];
	uint32_t secs = level_frames / 60;
	int k;
	panel(40, 52, 240, 128);
	ui_line(0, "WELL DONE!", 2, 62);
	/* three stars: finished, every cheese, the hidden star -- lit one
	 * by one after the count */
	label_make(&ui[4], "\3", 2);
	for (k = 0; k < 3; k++) {
		bool lit = k < stars_shown && (clear_stars & (1 << k));
		label_draw(&ui[4], 160 - 40 + k * 32, 84, lit ? MR_C_CREAM : MR_C_GREEN);
	}
	b[0] = 2;
	b[1] = ' ';
	utoa_pad(b + 2, (uint32_t)count_shown, 1);
	strcat(b, " OF ");
	utoa_pad(b + strlen(b), (uint32_t)mr_cheese_total, 1);
	ui_line(1, b, 1, 110);
	strcpy(b, "TIME ");
	utoa_pad(b + 5, secs / 60, 1);
	strcat(b, ":");
	utoa_pad(b + strlen(b), secs % 60, 2);
	ui_line(2, b, 1, 126);
	label_make(&ui[3], level_no < level_count ? "SPACE: NEXT LEVEL" : "SPACE: GO ON", 1);
	label_centre(&ui[3], 160, MR_C_GREEN);
}

static void draw_over(void) {
	panel(48, 80, 224, 72);
	ui_line(0, "GAME OVER", 2, 90);
	label_make(&ui[1], "SPACE: TRY AGAIN", 1);
	label_centre(&ui[1], 116, MR_C_GREEN);
	label_make(&ui[2], "ESC: MENU", 1);
	label_centre(&ui[2], 132, MR_C_GREEN);
}

static void draw_done(void) {
	char b[44];
	panel(40, 72, 240, 88);
	ui_line(0, "YOU DID IT!", 2, 82);
	ui_line(1, "EVERY LEVEL, ALL DONE", 1, 108);
	strcpy(b, "SCORE ");
	utoa_pad(b + 6, (uint32_t)score, 1);
	strcat(b, "  \3 ");
	utoa_pad(b + strlen(b), (uint32_t)mr_progress_stars(&prog), 1);
	strcat(b, "/");
	utoa_pad(b + strlen(b), (uint32_t)(3 * level_count), 1);
	ui_line(2, b, 1, 124);
	label_make(&ui[3], "SPACE: MENU", 1);
	label_centre(&ui[3], 142, MR_C_GREEN);
}

/* -- the title's chase --
 *
 * A mouse running along the bottom of the title with three cats behind
 * it, in the view's own coordinates: not the physics, a picture. */
static int chase_x;

static void draw_chase(void) {
	int i, f = (int)((st_frames >> 3) & 1);
	int y = MR_GROUND_ROW * MR_TILE_H - MR_SPR_H;
	int x = chase_x % (Z_GAME_VIEW_W + 160) - 32;
	spr_c = MR_C_CREAM; spr_o = MR_C_DARK;
	if (x > -MR_SPR_W && x < Z_GAME_VIEW_W)
		spr_draw_any(f ? mr_spr_mouse_run1 : mr_spr_mouse_run2,
			f ? mr_msk_mouse_run1 : mr_msk_mouse_run2,
			f ? mr_hspr_mouse_run1 : mr_hspr_mouse_run2,
			f ? mr_hmsk_mouse_run1 : mr_hmsk_mouse_run2,
			world_to_fb_x(game.cam + x), world_to_fb_y(y), false);
	spr_c = MR_C_DARK; spr_o = MR_C_DARK;
	for (i = 0; i < 3; i++) {
		int cx = x - 40 - i * 26;
		int g = (int)(((st_frames >> 3) + (uint32_t)i) & 1);
		if (cx <= -MR_SPR_W || cx >= Z_GAME_VIEW_W) continue;
		spr_draw_any(g ? mr_spr_cat1 : mr_spr_cat2, g ? mr_msk_cat1 : mr_msk_cat2,
			g ? mr_hspr_cat1 : mr_hspr_cat2, g ? mr_hmsk_cat1 : mr_hmsk_cat2,
			world_to_fb_x(game.cam + cx), world_to_fb_y(y), false);
	}
}

/* -- moving between the screens -- */

static void set_state(mr_state_t s) {
	st = s;
	st_frames = 0;
}

/* How much music: calm first, more as the levels go by (docs/sfx.md,
 * "Calm first"); the fast shoes add a little while they last. */
static int music_now = -1;

static void music_follow(void) {
	int m = st == ST_TITLE ? 1 :
		music_base[skill] + (level_no - 1) / 2 + (pw_left[PW_SNEAKERS] ? 2 : 0);
	if (m > ZSFX_INTENSITY_FULL) m = ZSFX_INTENSITY_FULL;
	if (m != music_now) {
		music_now = m;
		zsfx_music_intensity(m);
	}
}

/* The level's tune -- its theme's -- started only when it changes, so
 * two meadow levels in a row play on without a seam. */
static const zsfx_song_t *song_now;

static void music_song(int music) {
	const zsfx_song_t *s = snd_song(music);
	if (s == song_now) return;
	song_now = s;
	zsfx_music_play(s);
}

static void title(void) {
	game_over = won = false;
	level_begin(1);
	music_song(0);
	player.alive = false;           /* the title has no mouse in play */
	game.cam = 0;
	chase_x = 0;
	if (!menu_shown(menu)) menu = MENU_PLAY;
	set_state(ST_TITLE);
}

static void intro(int n) {
	char b[64];
	game_over = won = false;
	if (!level_begin(n)) { level_begin(1); n = 1; }
	z_game_camera_to(&game, TOPX(player.x) - Z_GAME_VIEW_W / 2, MR_WORLD_W);
	music_song(mr_level_info.music);
	set_state(ST_INTRO);
	strcpy(b, "Level ");
	utoa_pad(b + strlen(b), (uint32_t)level_no, 1);
	strcat(b, ". ");
	strcat(b, mr_level_info.name);
	say(b);
}

static int level_start_score;

static void new_game(int from) {
	score = 0;
	lives = lives_for[skill];
	cat_speed = cat_speeds[skill];
	intro(from);
	level_start_score = score;
}

static void level_cleared(void) {
	int i;
	/* the magnet's cheese still on its way counts */
	for (i = 0; i < MR_MAX_PARTS; i++)
		if (parts[i].kind == P_CHEESE) {
			parts[i].kind = P_NONE;
			cheese++;
			score += 10;
		}
	clear_stars = MR_STAR_DONE;
	if (cheese >= mr_cheese_total) clear_stars |= MR_STAR_CHEESE;
	if (star_found) clear_stars |= MR_STAR_HIDDEN;
	if (level_no <= MR_LEVELS_MAX) prog.stars[level_no] |= clear_stars;
	stars_shown = 0;
	if (level_no + 1 > prog.unlocked && level_no < level_count)
		prog.unlocked = level_no + 1;
	if (cheese > prog.best[level_no] && level_no <= MR_LEVELS_MAX)
		prog.best[level_no] = (uint16_t)cheese;
	progress_save();
	count_shown = 0;
	won = false;
	set_state(ST_CLEAR);
	say("Well done!");
}

static void skill_change(int d) {
	skill = (skill + d + MR_SKILLS) % MR_SKILLS;
	cat_speed = cat_speeds[skill];
	snd_play(SND_SELECT, -1);
	progress_save();
	say(mr_skill_spoken(skill));
}

static void menu_move(int d) {
	int k;
	for (k = 0; k < MENU_ITEMS; k++) {
		menu = (menu + d + MENU_ITEMS) % MENU_ITEMS;
		if (menu_shown(menu)) break;
	}
	snd_play(SND_MENU, -1);
	menu_say();
}

/* -- one frame of the screens' rules; false to quit -- */
static bool screens(const mr_input_t *in, uint32_t steps) {

	/* the confetti and the weather go on over the panels */
	if (st == ST_INTRO || st == ST_DYING || st == ST_CLEAR || st == ST_OVER ||
	    st == ST_DONE) {
		uint32_t k;
		for (k = 0; k < steps && k < 4; k++) {
			parts_update();
			weather_update();
		}
	}

	switch (st) {

	case ST_TITLE:
		/* the scenery drifts; cats walk about in it */
		{
			uint32_t k;
			for (k = 0; k < steps && k < 4; k++) {
				for (int i = 0; i < foe_count; i++) foe_update(&foes[i]);
				parts_update();
			}
			game.cam += (int32_t)steps;
			if (game.cam > MR_WORLD_W - Z_GAME_VIEW_W) game.cam = 0;
			z_game_camera_to(&game, game.cam, MR_WORLD_W);
			chase_x += (int)(2 * steps);
		}
		if (in->up) menu_move(-1);
		if (in->down) menu_move(1);
		if (menu == MENU_SKILL && in->lpress) skill_change(-1);
		if (menu == MENU_SKILL && in->rpress) skill_change(1);
		if (in->back) return false;
		if (in->ok) {
			if (menu != MENU_SKILL) snd_play(SND_SELECT, -1);
			switch (menu) {
			case MENU_PLAY: new_game(1); break;
			case MENU_CONTINUE:
				new_game(prog.unlocked > level_count ? level_count : prog.unlocked);
				break;
			case MENU_SKILL: skill_change(1); break;
			default: return false;
			}
		}
		break;

	case ST_INTRO:
		if (in->ok || st_frames > 180) set_state(ST_PLAY);
		if (in->back) title();
		break;

	case ST_PLAY:
		if (in->pause || in->back) {
			set_state(ST_PAUSE);
			zsfx_suspend(true);
			break;
		}
		level_frames += steps;
		if (player.alive) {
			/* Capped. A long stall -- a debugger, a disk hit, another
			 * process hogging the bus -- would otherwise try to catch
			 * up in one go and teleport the player through a wall.
			 * Better to lose time than position. */
			uint32_t k, n = steps > 4 ? 4 : steps;
			mr_input_t once = *in;
			for (k = 0; k < n; k++) {
				play_step(&once);
				once.jump_edge = false;     /* a press is one jump */
				parts_update();
				weather_update();
				if (!player.alive || won) break;
			}
			z_game_camera_to(&game,
				TOPX(player.x) - Z_GAME_VIEW_W / 2 + MR_PLAYER_W / 2,
				MR_WORLD_W);
			if (won) level_cleared();
			else if (!player.alive) set_state(ST_DYING);
		}
		break;

	case ST_PAUSE:
		if (in->pause || in->ok) { set_state(ST_PLAY); zsfx_suspend(false); }
		else if (in->back) { zsfx_suspend(false); title(); }
		break;

	case ST_DYING:
		/* the hearts were counted when it happened: here, either the
		 * end, or back to the last mouse hole with everyone where they
		 * are now -- and a moment of blinking */
		if (st_frames > 60) {
			if (lives <= 0) {
				game_over = true;
				set_state(ST_OVER);
				say("Game over");
			} else {
				player_reset(check_x, check_y);
				riding = -1;
				hurt_frames = MR_HURT_FRAMES;
				z_game_camera_to(&game, TOPX(player.x) - Z_GAME_VIEW_W / 2,
					MR_WORLD_W);
				set_state(ST_PLAY);
			}
		}
		break;

	case ST_CLEAR:
		if (count_shown < cheese && st_frames >= (uint32_t)(20 + 4 * count_shown)) {
			count_shown++;
			snd_play(SND_TICK, -1);
		}
		/* then the stars, one by one: a chime for each one won */
		if (count_shown >= cheese && stars_shown < 3 &&
		    st_frames >= (uint32_t)(40 + 4 * cheese + 24 * stars_shown)) {
			if (clear_stars & (1 << stars_shown)) snd_play(SND_HEART, -1);
			stars_shown++;
		}
		if (in->ok && st_frames > 30) {
			if (count_shown < cheese || stars_shown < 3) {
				count_shown = cheese;
				stars_shown = 3;
			}
			else if (level_no < level_count) {
				level_start_score = score;
				intro(level_no + 1);
			} else {
				won = true;
				set_state(ST_DONE);
				say("You did it!");
			}
		}
		break;

	case ST_OVER:
		if (in->ok && st_frames > 30) {
			/* the same level again, every heart back, the score as
			 * it was when the level began -- never back to level 1 */
			score = level_start_score;
			lives = lives_for[skill];
			intro(level_no);
		} else if (in->back) {
			title();
		}
		break;

	case ST_DONE:
		if ((in->ok || in->back) && st_frames > 60) title();
		break;
	}
	return true;
}

static void draw_screens(void) {
	switch (st) {
	case ST_TITLE: draw_chase(); draw_title(); break;
	case ST_INTRO: hud_draw(); draw_intro(); break;
	case ST_PLAY:
	case ST_DYING: hud_draw(); break;
	case ST_PAUSE: hud_draw(); draw_pause(); break;
	case ST_CLEAR: hud_draw(); draw_clear(); break;
	case ST_OVER:  hud_draw(); draw_over(); break;
	case ST_DONE:  draw_done(); break;
	}
}

/* -- main ----------------------------------------------------------- */

/* The world, back to front, into the page being drawn: the frame
 * (docs/mausrun.md, "The frame"). */
/* Where a frame's blits go (the host render's profile, MAUSRUN_PROF):
 * the count at each step of draw_world(). Nothing in a normal build. */
#ifdef MAUSRUN_PROF
uint32_t mr_prof[8];
#define PROF(i) (mr_prof[i] = hud_blits)
#else
#define PROF(i) ((void)0)
#endif

static void draw_world(void) {

	/* Clear the sky ACROSS THE VISIBLE SPAN, which is not the start of
	 * the page: the viewport sits at fold(cam) and walks the whole
	 * 640-wide page as the camera scrolls. Two fills when the span
	 * crosses the seam. The WHOLE height, not just the sky: a pit has
	 * no tiles in the ground rows, and nothing else would clear it. */
	if (col_on) {
		c_fill(col_page_x(), 0, Z_GAME_VIEW_W, Z_GAME_VIEW_H, MR_C_SKY);
	} else {
		int fx = world_to_fb_x(game.cam);
		int fy = z_game_back_y(&game);
		int w1 = Z_SCREEN_W - fx;
		if (w1 > Z_GAME_VIEW_W) w1 = Z_GAME_VIEW_W;
		z_fb_hw_fill_rect_async(fx, fy, w1, Z_GAME_VIEW_H, 0);
		if (w1 < Z_GAME_VIEW_W)
			z_fb_hw_fill_rect_async(0, fy, Z_GAME_VIEW_W - w1, Z_GAME_VIEW_H, 0);
	}

	PROF(0);
	if (col_on && mr_level_info.theme == MR_THEME_KITCHEN) kitchen_wall_draw();
	if (col_on) stars_draw();             /* at night only */
	moon_draw();
	ambient_draw();                        /* clouds, trees */
	PROF(1);
	draw_subsurface(game.cam, game.cam + Z_GAME_VIEW_W);
	/* waterfalls in the pits outdoors on the ground: a kitchen's gap
	 * and a roof's edge fall to nothing */
	if (col_on && (mr_level_info.theme == MR_THEME_MEADOW ||
	               mr_level_info.theme == MR_THEME_GARDEN))
		water_draw(game.cam, game.cam + Z_GAME_VIEW_W);
	PROF(2);
	draw_tiles_rect(game.cam, 0, game.cam + Z_GAME_VIEW_W, MR_WORLD_H);
	PROF(3);
	birds_draw();
	draw_actors();
	PROF(4);
}

int main(void) {

	mr_input_t in;
	uint32_t steps = 1;
	bool running = true;
#ifdef MAUSRUN_DEBUG
	uint32_t dbg_frames = 0, dbg_dt = 0, dbg_blits = 0;
#endif

	printf("mausrun: the mouse, the cheese and the cats\n");

	if (!z_game_available()) {
		printf("mausrun: this bitstream has no game mode.\n");
		printf("mausrun: rebuild the gateware with `GAME in rtl/boards.vh\n");
		printf("mausrun: (`make flash`, not `make dev-flash`).\n");
		return 1;
	}

	if (!z_fb_hw_blit_mem_available()) {
		printf("mausrun: this bitstream's blitter has no memory copy mode.\n");
		return 1;
	}

	progress_load();
	level_count = level_count_files();
	stars_init();
	strips_init();
	far_init();
	sound_ok = zsfx_init();

	/* Probed once. On a bitstream without raster ops every op behaves
	 * as COPY, so a hardware masked blit would draw an opaque box --
	 * see spr_draw() above. */
	rop_hw = z_fb_hw_rop_available();

	printf("mausrun: %d level%s, skill %s, %s\n", level_count,
		level_count == 1 ? "" : "s", mr_skill_name(skill),
		sound_ok ? "music + sfx" : "no audio on this board");
	printf("mausrun: arrows/space or a gamepad; P pauses, Esc goes back.\n");
	printf("mausrun: - = volume, M music, C colour.\n");

	/* Wrap ON -- this is a scrolling game and the page is a torus.
	 * See docs/game_mode.md. */
	if (!z_game_begin(&game, Z_GAME_SCROLL_H, true)) {
		printf("mausrun: could not enter game mode\n");
		return 1;
	}

	title();
	say("Maus Run");
	music_follow();

	/* Clear both pages once; after this every pixel is redrawn every
	 * frame. */
	z_fb_hw_fill_rect(0, 0, 640, 480, 0);

	/* Colour by default where there is colour hardware and raster ops
	 * (the colour sprites are raster ops); C toggles it. */
	if (rop_hw) col_set(true);

	while (running) {

		uint32_t dt;

		input_read(&in);

		/* - and = the master volume, music and effects together */
		if (in.vol_down) {
			uint8_t v = zsfx_master_get();
			zsfx_master_volume(v > 16 ? (uint8_t)(v - 16) : 0);
			snd_play(SND_TICK, -1);
		}
		if (in.vol_up) {
			uint8_t v = zsfx_master_get();
			zsfx_master_volume(v < 240 ? (uint8_t)(v + 16) : 255);
			snd_play(SND_TICK, -1);
		}
		if (in.tog_music) zsfx_music_enable(!zsfx_music_enabled());
		if (in.tog_col && rop_hw) col_set(!col_on);

		ambient_update(steps);
		running = screens(&in, steps);
		music_follow();

		draw_world();
		draw_screens();

		/* BARRIER before the flip: every blit above was started and
		 * not waited for; the flip's view change lands at the next
		 * frame boundary, and the last blit must be in by then. */
		z_fb_hw_sync();

		dt = col_on ? col_flip() : z_game_flip(&game);
		steps = dt;
		st_frames += dt;

#ifdef MAUSRUN_DEBUG
		/* the old HUD's figures, once a second, on the console */
		dbg_frames += dt;
		dbg_dt += dt;
		dbg_blits += hud_blits;
		if (dbg_frames >= 60) {
			printf("mausrun: %u frames, dt %u, %u blits a frame\n",
				(unsigned)dbg_frames, (unsigned)(dbg_dt ? dbg_dt : 1),
				(unsigned)(dbg_blits / (dbg_frames ? dbg_frames : 1)));
			dbg_frames = dbg_dt = dbg_blits = 0;
		}
#endif
		hud_blits = 0;

		/* Music advances by the frames that ACTUALLY elapsed, so a
		 * frame the game overran does not drag the tempo down with
		 * the frame rate. */
		zsfx_tick(dt);
	}

	z_game_end(&game);

	/* Silences every channel, not just the ones this app used: a
	 * channel left enabled goes on fetching from a buffer about to be
	 * freed with this process. */
	zsfx_shutdown();

	/* Hand the framebuffer back: wm repaints damage it caused itself,
	 * and this came from outside. See Z_WM_REPAINT in zwm.h. */
	{
		uint32_t wm_pid;
		if (z_pid_lookup("wm0", &wm_pid))
			z_msg_new_send(wm_pid, Z_WM_REPAINT, 0, z_obj_uint32(0));
	}

	printf("mausrun: score %d\n", score);

	return 0;

}
