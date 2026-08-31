/*
 * gamedemo -- a side-scrolling run-and-jump demo for Zeitlos game mode.
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
 *   hardware mixer sound effects  zaudio.h, if the board has one
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
#include "../../common/zpad.h"
#include "../../common/zkbd.h"
#include "../../common/zaudio.h"
#include "../../common/zfont.h"

#include "sprites.h"
#include "music.h"
#include "level.h"

#define VRAM_BASE 0x20000000u
#define FB_STRIDE_WORDS 20          /* 640 px / 32 bits */

/* First map row that can contain a non-empty tile. Everything above is
 * sky and is covered by the fill, so draw_tiles_rect() starts here.
 *
 * A constant rather than a scan of the map: it is a property of the
 * level's design (boxes and platforms start at row 3), and getting it
 * wrong shows up immediately as missing scenery rather than subtly. */
#define GD_FIRST_SOLID_ROW 3

/* Row 11 is the surface (grass); 12..14 are the fill underneath it.
 * The sky fill stops at GD_GROUND_ROW because everything from there
 * down is opaque and gets drawn anyway. */
#define GD_GROUND_ROW 11
#define GD_SUBSURFACE_ROW 12

/* Grey level for the sub-surface, 0..Z_SHADE_MAX. High enough to read
 * as solid ground at a glance, textured enough that the ground is not
 * a flat black slab. */
#define GD_GROUND_SHADE 13

/* Frame diagnostics, shown on the HUD -- see draw_hud(). */
static uint32_t hud_dt = 1;
static uint32_t hud_blits;
static bool show_ambient = true;

/* -- physics, in 1/16 pixel units --
 *
 * Fixed point rather than float: this runs on a core whose FPU is
 * libgcc. 1/16 is enough resolution that gravity accumulates smoothly
 * at 60fps and coarse enough that everything stays in 32 bits with
 * room to spare. */
#define FP 16
#define TOFP(px) ((px) * FP)
#define TOPX(v)  ((v) / FP)

/* Tuned together, not independently. With gravity 6 and a 1/16px
 * unit, a jump of -96 gives 32 frames of airtime, which at a run
 * speed of 32 covers 64px -- exactly FOUR tiles -- and reaches 48px
 * high, exactly THREE. Those two numbers are what the level is built
 * against: no gap is wider than 3 tiles and no step is taller than 2,
 * so every jump has a tile of margin. Change any of these four
 * constants and the level needs rechecking; gen_level_check below is
 * what does that. */
#define GD_GRAVITY      6
#define GD_RUN_SPEED    32
#define GD_JUMP_SPEED (-96)
#define GD_MAX_FALL    112
#define GD_CAT_SPEED     9

/* The player is narrower than its sprite. A 16-wide hitbox on a
 * 16-wide sprite means the visible ears and tail catch on edges the
 * player can obviously see they should fit through, which reads as
 * the game being unfair rather than as a hitbox choice. */
#define GD_PLAYER_W 10
#define GD_PLAYER_H 14
#define GD_PLAYER_XOFF 3      /* hitbox inset within the 16px sprite */

typedef struct {
	int32_t x, y;      /* fixed point, top-left of the HITBOX */
	int32_t vx, vy;
	bool on_ground;
	bool face_left;
	int anim;
	bool alive;
} gd_actor_t;

static gd_actor_t player;
static gd_actor_t cats[GD_MAX_CATS];
static int cat_count;

static z_game_t game;

static int score;
static int lives;
static bool sound_ok;
static bool game_over;
static bool won;

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
	bool quit;
	bool restart;
	bool vol_down, vol_up;
	bool mus_down, mus_up;
	bool tog_amb;
} gd_input_t;

static bool prev_jump;

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
#define GD_HID_TYP_KEYBOARD 1

static bool kbd_held(uint8_t usage) {

	for (int port = 0; port < 2; port++) {

		uint32_t info = port ? reg_usb1_info : reg_usb0_info;
		uint32_t keys;

		/* bits [25:24] of the packed info word -- see rtl/usb_hid.v.
		 * NOT [23:22], which falls inside the constant padding and
		 * reads zero regardless of what is plugged in; that exact
		 * slip was a real bug in sw/os/hid.c once. */
		if (((info >> 24) & 0x3u) != GD_HID_TYP_KEYBOARD) continue;

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
#define HID_SPACE  0x2C
#define HID_ESC    0x29
#define HID_R      0x15
#define HID_MINUS  0x2D    /* - and _ */
#define HID_EQUAL  0x2E    /* = and + */
#define HID_LBRACK 0x2F    /* [ */
#define HID_RBRACK 0x30    /* ] */
#define HID_A      0x04    /* a -- toggle the ambient layer */

static void input_read(gd_input_t *in) {

	uint32_t pad;
	bool jump_now;

	in->left    = kbd_held(HID_LEFT);
	in->right   = kbd_held(HID_RIGHT);
	jump_now    = kbd_held(HID_UP) || kbd_held(HID_SPACE);
	in->quit    = kbd_held(HID_ESC);
	in->restart = kbd_held(HID_R);
	in->vol_down = kbd_held(HID_MINUS);
	in->vol_up   = kbd_held(HID_EQUAL);
	in->mus_down = kbd_held(HID_LBRACK);
	in->mus_up   = kbd_held(HID_RBRACK);
	in->tog_amb  = kbd_held(HID_A);

	/* Pad 0, whichever port it is in. Reads 0 when absent, which is
	 * exactly "nothing pressed" -- so no special case is needed for a
	 * machine with no pad, and unplugging one mid-jump releases every
	 * button rather than leaving the mouse running forever. See
	 * docs/gamepad.md. */
	pad = z_pad_read(0);
	if (pad & Z_PAD_LEFT)  in->left = true;
	if (pad & Z_PAD_RIGHT) in->right = true;
	if (pad & (Z_PAD_A | Z_PAD_B | Z_PAD_UP)) jump_now = true;
	if (pad & Z_PAD_START) in->restart = true;
	if (pad & Z_PAD_SELECT) in->quit = true;

	in->jump = jump_now;
	in->jump_edge = jump_now && !prev_jump;
	prev_jump = jump_now;

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
static inline int world_to_fb_x(int32_t wx) {
	return z_game_fold(game.orient, wx);
}

static inline int world_to_fb_y(int32_t wy) {
	return (int)wy + z_game_back_y(&game);
}

/* One 16x16 tile through the hardware blitter.
 *
 * The tile data is 16 uint16 rows, so its stride is 2 bytes. The
 * blitter takes a stride in bytes and reads with the framebuffer's own
 * bit order, which is why gen_sprites.py emits LSB-first (see its
 * header). No shifting or masking is needed here at all: tiles are
 * opaque, and the blitter handles arbitrary destination alignment
 * itself. */
static void draw_tile(uint8_t t, int fbx, int fby) {
	if (t >= GD_TILE_COUNT) return;
	/* Async: the next tile's acquire() waits for this one, so the
	 * loop in draw_tiles_rect() overlaps each blit with computing the
	 * next tile's address and coordinates. See zgfx.h. */
	hud_blits++;
	z_fb_hw_blit_mem_async(gd_tiles[t], GD_TILE_STRIDE, 0, 0, fbx, fby,
		GD_TILE_W, GD_TILE_H, Z_ROP_COPY);
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

	int c = (int)(wx0 / GD_TILE_W);
	int c1 = (int)((wx1 + GD_TILE_W - 1) / GD_TILE_W);
	int fy = world_to_fb_y(GD_SUBSURFACE_ROW * GD_TILE_H);
	int hh = (GD_LEVEL_ROWS - GD_SUBSURFACE_ROW) * GD_TILE_H;

	if (c < 0) c = 0;
	if (c1 > GD_LEVEL_COLS) c1 = GD_LEVEL_COLS;

	while (c < c1) {

		int run;

		/* skip the pit */
		while (c < c1 && gd_level[GD_SUBSURFACE_ROW][c] == GD_TILE_EMPTY)
			c++;
		if (c >= c1) break;

		run = 0;
		while (c + run < c1 &&
			gd_level[GD_SUBSURFACE_ROW][c + run] != GD_TILE_EMPTY)
			run++;

		/* One fill per run, split at the torus seam the same way the
		 * sky fill is -- a run can straddle framebuffer column 639. */
		{
			int fx = world_to_fb_x((int32_t)c * GD_TILE_W);
			int w = run * GD_TILE_W;
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
			z_fb_hw_fill_shade_async(fx, fy, w1, hh, GD_GROUND_SHADE);

			if (w1 < w) {
				hud_blits++;
				z_fb_hw_fill_shade_async(0, fy, w - w1, hh,
					GD_GROUND_SHADE);
			}
		}

		c += run;

	}

}

static void draw_tiles_rect(int32_t wx0, int32_t wy0, int32_t wx1, int32_t wy1) {

	int c0 = (int)(wx0 / GD_TILE_W);
	int c1 = (int)((wx1 + GD_TILE_W - 1) / GD_TILE_W);
	int r0 = (int)(wy0 / GD_TILE_H);
	int r1 = (int)((wy1 + GD_TILE_H - 1) / GD_TILE_H);

	/* Rows 0..GD_FIRST_SOLID_ROW-1 of this level are pure sky and the
	 * fill already covers them. Skipping them outright saves walking
	 * ~150 cells a frame to discover they are all EMPTY -- cheap per
	 * cell, but this runs 60 times a second over the whole viewport. */
	if (r0 < GD_FIRST_SOLID_ROW) r0 = GD_FIRST_SOLID_ROW;

	/* Rows GD_SUBSURFACE_ROW and below are drawn by draw_subsurface()
	 * as run-length fills, not as tiles. */
	if (r1 > GD_SUBSURFACE_ROW) r1 = GD_SUBSURFACE_ROW;

	if (wx0 < 0) c0 = 0;
	if (wy0 < 0) r0 = 0;
	if (c1 > GD_LEVEL_COLS) c1 = GD_LEVEL_COLS;
	if (r1 > GD_LEVEL_ROWS) r1 = GD_LEVEL_ROWS;

	for (int c = c0; c < c1; c++) {
		int fbx = world_to_fb_x((int32_t)c * GD_TILE_W);
		for (int r = r0; r < r1; r++) {
			uint8_t t = gd_level[r][c];
			/* EMPTY is skipped, not drawn as black. The sky was
			 * already filled, and the ambient layer has drawn clouds
			 * and trees over it -- blitting an empty tile here would
			 * erase them. That ordering is the whole reason the sky
			 * is a fill rather than 300 empty tile blits. */
			if (t == GD_TILE_EMPTY) continue;
			draw_tile(t, fbx, world_to_fb_y((int32_t)r * GD_TILE_H));
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

static bool rop_hw;      /* set once at startup */

/* Software fallback: (dst & ~mask) | data, one row at a time. */
static void spr_draw_sw(const uint16_t *data, const uint16_t *mask,
	int fbx, int fby)
{
	volatile uint32_t *vram = (volatile uint32_t *)VRAM_BASE;
	int page_top = z_game_back_y(&game);
	int page_bot = page_top + Z_GAME_PAGE_H(game.orient);

	for (int row = 0; row < GD_SPR_H; row++) {

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
	uint16_t d2[GD_SPR_H], m2[GD_SPR_H];

	if (!mirror) { spr_draw_sw(data, mask, fbx, fby); return; }

	for (int i = 0; i < GD_SPR_H; i++) {
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

static void spr_draw(const uint16_t *data16, const uint16_t *mask16,
	const void *data_hw, const void *mask_hw, int fbx, int fby)
{
	if (!rop_hw) {
		/* The software path READS AND WRITES VRAM WITH THE CPU, so it
		 * must not run while an async tile blit is still in flight --
		 * it would read a half-drawn background and composite the
		 * sprite onto it. Another blit would be fine; only CPU access
		 * needs this barrier. See zgfx.h. */
		z_fb_hw_sync();
		spr_draw_sw(data16, mask16, fbx, fby);
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
		int h = GD_SPR_H;

		if (fby < page_top) { sy = page_top - fby; h -= sy; fby = page_top; }
		if (fby + h > page_bot) h = page_bot - fby;
		if (h <= 0) return;

		/* The part that fits before column 640. */
		{
			int w = GD_SPR_W;
			if (fbx + w > Z_SCREEN_W) w = Z_SCREEN_W - fbx;
			if (w > 0)
				z_fb_hw_blit_sprite_async(data_hw, mask_hw, GD_TILE_STRIDE,
					0, sy, fbx, fby, w, h);
		}

		/* ...and the part that wrapped round to column 0. */
		if (fbx + GD_SPR_W > Z_SCREEN_W) {
			int cut = Z_SCREEN_W - fbx;
			z_fb_hw_blit_sprite_async(data_hw, mask_hw, GD_TILE_STRIDE,
				cut, sy, 0, fby, GD_SPR_W - cut, h);
		}
	}

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

#define GD_MAX_CLOUDS 10
#define GD_MAX_TREES  10
#define GD_MAX_BIRDS  12

typedef struct {
	int32_t x, y;        /* position in the layer's own space */
	int16_t vx;          /* birds only, 1/16 px per frame */
	uint8_t anim;
} gd_amb_t;

static gd_amb_t clouds[GD_MAX_CLOUDS];
static gd_amb_t trees[GD_MAX_TREES];
static gd_amb_t birds[GD_MAX_BIRDS];

/* Parallax denominators. Clouds move at a quarter of the camera, trees
 * at three quarters -- so trees read as "just behind the action" and
 * clouds as "far away", which is the whole illusion. Powers of two so
 * the divide is a shift; picorv32 has no divider unless `CPU_DIV is
 * built and this runs per sprite per frame. */
#define GD_CLOUD_SHIFT 2
#define GD_TREE_SHIFT  1

static void ambient_init(void) {

	/* Deterministic placement from a small LCG rather than z_rand():
	 * the demo should look the same every run, so a visual regression
	 * is visible as one. */
	uint32_t r = 0x1234567u;
	#define NEXT_R (r = r * 1103515245u + 12345u, (r >> 16) & 0x7fff)

	for (int i = 0; i < GD_MAX_CLOUDS; i++) {
		clouds[i].x = (int32_t)(NEXT_R % (GD_WORLD_W >> GD_CLOUD_SHIFT));
		clouds[i].y = (int32_t)(8 + NEXT_R % 40);
		clouds[i].anim = 0;
	}

	for (int i = 0; i < GD_MAX_TREES; i++) {
		trees[i].x = (int32_t)(NEXT_R % (GD_WORLD_W >> GD_TREE_SHIFT));
		/* Stand them ON the floor, not in it.
		 *
		 * A tree is TWO 16px cells (TREE_TOP above TREE_BOT), so its
		 * base is at y + 2*GD_SPR_H, not y + GD_SPR_H. Subtracting
		 * only one cell height buried the whole lower half below the
		 * floor surface. */
		trees[i].y = GD_GROUND_ROW * GD_TILE_H - 2 * GD_SPR_H;
		trees[i].anim = 0;
	}

	for (int i = 0; i < GD_MAX_BIRDS; i++) {
		birds[i].x = (int32_t)(NEXT_R % GD_WORLD_W);
		birds[i].y = (int32_t)(16 + NEXT_R % 72);
		birds[i].vx = (int16_t)((NEXT_R & 1) ? 6 : -6);
		birds[i].anim = (uint8_t)(NEXT_R & 31);
	}

	#undef NEXT_R

}

static void ambient_update(uint32_t frames) {

	for (int i = 0; i < GD_MAX_BIRDS; i++) {
		birds[i].x += birds[i].vx * (int32_t)frames;
		if (birds[i].x < 0) birds[i].x += GD_WORLD_W;
		if (birds[i].x >= GD_WORLD_W) birds[i].x -= GD_WORLD_W;
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

	if (sx <= -GD_SPR_W || sx >= Z_GAME_VIEW_W) return;
	if (ly <= -GD_SPR_H || ly >= Z_GAME_VIEW_H) return;

	spr_draw_any(sd, sm, hd, hm,
		world_to_fb_x(game.cam + sx), world_to_fb_y(ly), false);
}

static void ambient_draw(void) {

	int32_t cam_c = game.cam >> GD_CLOUD_SHIFT;
	int32_t cam_t = game.cam >> GD_TREE_SHIFT;

	for (int i = 0; i < GD_MAX_CLOUDS; i++) {
		amb_cell(gd_hspr_cloud_l, gd_hmsk_cloud_l,
			gd_spr_cloud_l, gd_msk_cloud_l,
			clouds[i].x, clouds[i].y, cam_c);
		amb_cell(gd_hspr_cloud_r, gd_hmsk_cloud_r,
			gd_spr_cloud_r, gd_msk_cloud_r,
			clouds[i].x + GD_SPR_W, clouds[i].y, cam_c);
	}

	for (int i = 0; i < GD_MAX_TREES; i++) {
		amb_cell(gd_hspr_tree_top, gd_hmsk_tree_top,
			gd_spr_tree_top, gd_msk_tree_top,
			trees[i].x, trees[i].y, cam_t);
		amb_cell(gd_hspr_tree_bot, gd_hmsk_tree_bot,
			gd_spr_tree_bot, gd_msk_tree_bot,
			trees[i].x, trees[i].y + GD_SPR_H, cam_t);
	}

}

static void birds_draw(void) {

	for (int i = 0; i < GD_MAX_BIRDS; i++) {
		int flap = (birds[i].anim & 8) ? 1 : 0;
		bool left = birds[i].vx < 0;
		const void *hd = flap
			? (left ? (const void *)gd_hspr_bird2_m : (const void *)gd_hspr_bird2)
			: (left ? (const void *)gd_hspr_bird1_m : (const void *)gd_hspr_bird1);
		const void *hm = flap
			? (left ? (const void *)gd_hmsk_bird2_m : (const void *)gd_hmsk_bird2)
			: (left ? (const void *)gd_hmsk_bird1_m : (const void *)gd_hmsk_bird1);
		amb_cell(hd, hm,
			flap ? gd_spr_bird2 : gd_spr_bird1,
			flap ? gd_msk_bird2 : gd_msk_bird1,
			birds[i].x, birds[i].y, game.cam);
	}

}

/* -- collision ------------------------------------------------------ */

static bool box_hits_solid(int32_t px, int32_t py, int w, int h) {
	/* Sample the four corners plus the midpoints of the vertical
	 * edges. With 16px tiles and a 14px-tall hitbox a tile cannot fit
	 * entirely between the samples, so corners alone would be enough
	 * -- the midpoints are there because the hitbox is allowed to grow
	 * and this is the cheap way to keep that safe. */
	static const int fx[6] = { 0, 1, 0, 1, 0, 1 };
	static const int fy[6] = { 0, 0, 1, 1, 2, 2 };
	for (int i = 0; i < 6; i++) {
		int32_t sx = px + (fx[i] ? (w - 1) : 0);
		int32_t sy = py + (fy[i] == 0 ? 0 : fy[i] == 1 ? (h - 1) : (h / 2));
		if (gd_tile_solid(gd_tile_at(sx, sy))) return true;
	}
	return false;
}

/* Move one axis at a time and back out on contact.
 *
 * Axis-separated because moving both at once and then resolving means
 * deciding which axis caused the overlap, which is the classic way to
 * get a character that sticks to walls while falling past them. Two
 * passes costs a few more samples and has no such ambiguity. */
static void move_actor(gd_actor_t *a, int w, int h) {

	int32_t nx = a->x + a->vx;
	if (box_hits_solid(TOPX(nx), TOPX(a->y), w, h)) {
		a->vx = 0;
	} else {
		a->x = nx;
	}

	{
		int32_t ny = a->y + a->vy;
		if (box_hits_solid(TOPX(a->x), TOPX(ny), w, h)) {
			a->vy = 0;
		} else {
			a->y = ny;
		}
	}

	/* on_ground is PROBED, not inferred from whether the vertical move
	 * was blocked. Inferring it looks right and is subtly wrong: an
	 * actor resting on a floor has vy driven to 0 by gravity-then-
	 * collision, so on a frame where it does not actually move there
	 * is no blocked move to infer from, and the flag would clear.
	 *
	 * That produced a character that was grounded on alternate frames
	 * while standing perfectly still -- so a jump pressed on the wrong
	 * frame silently did nothing, roughly half the time. It reads as
	 * unresponsive controls rather than as a state bug, which is
	 * exactly why it is worth probing the one true thing instead:
	 * is there something solid immediately under the feet? */
	{
		int32_t fx = TOPX(a->x);
		int32_t fy = TOPX(a->y) + h;
		a->on_ground =
			gd_tile_solid(gd_tile_at(fx, fy)) ||
			gd_tile_solid(gd_tile_at(fx + w - 1, fy));
	}

}

/* -- world ---------------------------------------------------------- */

static void world_reset(bool full) {

	player.x = TOFP(gd_start_x + GD_PLAYER_XOFF);
	player.y = TOFP(gd_start_y);
	player.vx = player.vy = 0;
	player.on_ground = false;
	player.face_left = false;
	player.anim = 0;
	player.alive = true;

	cat_count = gd_cat_spawn_count;
	for (int i = 0; i < cat_count; i++) {
		cats[i].x = TOFP(gd_cat_spawns[i].x);
		cats[i].y = TOFP(gd_cat_spawns[i].y);
		cats[i].vx = (i & 1) ? GD_CAT_SPEED : -GD_CAT_SPEED;
		cats[i].vy = 0;
		cats[i].on_ground = false;
		cats[i].face_left = cats[i].vx < 0;
		cats[i].alive = true;
	}

	if (full) {
		score = 0;
		lives = 3;
		game_over = false;
		won = false;
		gd_level_load();
	}

	/* Both pages are stale. Harmless now that every frame is a full
	 * redraw, but z_game_invalidate() is what a caller is expected to
	 * do after a level change and costs nothing. */
	z_game_invalidate(&game);

}

static void update_player(const gd_input_t *in) {

	if (!player.alive) return;

	player.vx = 0;
	if (in->left)  { player.vx = -GD_RUN_SPEED; player.face_left = true; }
	if (in->right) { player.vx =  GD_RUN_SPEED; player.face_left = false; }

	if (in->jump_edge && player.on_ground) {
		player.vy = GD_JUMP_SPEED;
		player.on_ground = false;
		sfx_play(SFX_JUMP);
	}

	{
		bool was_air = !player.on_ground;
		player.vy += GD_GRAVITY;
		if (player.vy > GD_MAX_FALL) player.vy = GD_MAX_FALL;

		move_actor(&player, GD_PLAYER_W, GD_PLAYER_H);

		/* on_ground is now correct on every frame (see move_actor), so
		 * this edge fires once per landing rather than continuously. */
		if (was_air && player.on_ground) sfx_play(SFX_LAND);
	}

	if (player.vx) player.anim++;

	/* Cheese. Checked at the hitbox centre rather than by overlap,
	 * which is close enough at this tile size and means a cheese is
	 * collected when you are clearly on it rather than when a corner
	 * grazes it. */
	{
		int32_t cx = TOPX(player.x) + GD_PLAYER_W / 2;
		int32_t cy = TOPX(player.y) + GD_PLAYER_H / 2;
		int c = (int)(cx / GD_TILE_W), r = (int)(cy / GD_TILE_H);
		if (c >= 0 && c < GD_LEVEL_COLS && r >= 0 && r < GD_LEVEL_ROWS &&
			gd_level[r][c] == GD_TILE_CHEESE) {
			gd_level[r][c] = GD_TILE_EMPTY;
			score += 10;
			sfx_play(SFX_COIN);
			/* Nothing to repaint. The tile is gone from the map and
			 * every frame redraws the whole visible area from the
			 * map, so both pages correct themselves on their next
			 * turn with no bookkeeping at all. This used to need a
			 * repaint here plus a damage mark for the other page. */
		}
	}

	/* Fell into a pit. There is no pit tile -- a hole in the floor is
	 * a pit by construction, and leaving the bottom of the world is
	 * how you find out. */
	if (TOPX(player.y) > GD_WORLD_H) {
		player.alive = false;
		sfx_play(SFX_HIT);
	}

	if (TOPX(player.x) >= GD_WORLD_W - 2 * GD_TILE_W) won = true;

}

static void update_cats(void) {

	for (int i = 0; i < cat_count; i++) {

		gd_actor_t *c = &cats[i];
		if (!c->alive) continue;

		c->vy += GD_GRAVITY;
		if (c->vy > GD_MAX_FALL) c->vy = GD_MAX_FALL;

		{
			int32_t before = c->x;
			move_actor(c, GD_SPR_W, GD_SPR_H);
			/* Turned around because something blocked us. */
			if (c->x == before && c->vy == 0) {
				c->vx = -c->vx;
				c->face_left = c->vx < 0;
			}
		}

		/* Turn around at a ledge rather than walking off it. Sample
		 * just beyond the leading foot: if there is no floor there,
		 * reverse. Without this every cat ends up at the bottom of the
		 * nearest pit within a few seconds and the level looks empty. */
		if (c->on_ground) {
			int32_t probe_x = TOPX(c->x) + (c->vx < 0 ? -2 : GD_SPR_W + 1);
			int32_t probe_y = TOPX(c->y) + GD_SPR_H + 2;
			if (!gd_tile_solid(gd_tile_at(probe_x, probe_y))) {
				c->vx = -c->vx;
				c->face_left = c->vx < 0;
			}
		}

		c->anim++;

		/* Hit the player? Plain AABB overlap in pixels. */
		if (player.alive) {
			int32_t px = TOPX(player.x), py = TOPX(player.y);
			int32_t cxp = TOPX(c->x), cyp = TOPX(c->y);
			if (px < cxp + GD_SPR_W - 2 && px + GD_PLAYER_W > cxp + 2 &&
				py < cyp + GD_SPR_H - 2 && py + GD_PLAYER_H > cyp + 2) {
				player.alive = false;
				sfx_play(SFX_HIT);
			}
		}

	}

}

/* -- rendering ------------------------------------------------------ */

static void draw_actors(void) {

	/* Cats first, player last, so the player is never hidden behind a
	 * cat at the moment of the collision that matters. */
	for (int i = 0; i < cat_count; i++) {
		gd_actor_t *c = &cats[i];
		int32_t wx = TOPX(c->x), wy = TOPX(c->y);
		if (wx + GD_SPR_W < game.cam || wx > game.cam + Z_GAME_VIEW_W)
			continue;
		if (!c->alive) continue;
		{
			int f2 = (c->anim & 16) ? 1 : 0;
			const uint16_t *d = f2 ? gd_spr_cat2 : gd_spr_cat1;
			const uint16_t *m = f2 ? gd_msk_cat2 : gd_msk_cat1;
			const void *hd, *hm;
			if (c->face_left) {
				hd = f2 ? (const void *)gd_hspr_cat2_m : (const void *)gd_hspr_cat1_m;
				hm = f2 ? (const void *)gd_hmsk_cat2_m : (const void *)gd_hmsk_cat1_m;
			} else {
				hd = f2 ? (const void *)gd_hspr_cat2 : (const void *)gd_hspr_cat1;
				hm = f2 ? (const void *)gd_hmsk_cat2 : (const void *)gd_hmsk_cat1;
			}
			/* the software path mirrors at runtime; the hardware path
			 * picks a pre-mirrored bitmap, because the blitter has no
			 * reverse mode. Both are handed over and spr_draw() uses
			 * whichever pair its path needs. */
			if (rop_hw) spr_draw(d, m, hd, hm,
				world_to_fb_x(wx), world_to_fb_y(wy));
			else spr_draw_sw_mirror(d, m, world_to_fb_x(wx),
				world_to_fb_y(wy), c->face_left);
		}
	}

	if (player.alive) {
		int32_t wx = TOPX(player.x) - GD_PLAYER_XOFF;
		int32_t wy = TOPX(player.y);
		const uint16_t *d, *m;
		const void *hd, *hm;
		if (!player.on_ground) {
			d = gd_spr_mouse_jump; m = gd_msk_mouse_jump;
			hd = player.face_left ? (const void *)gd_hspr_mouse_jump_m
			                      : (const void *)gd_hspr_mouse_jump;
			hm = player.face_left ? (const void *)gd_hmsk_mouse_jump_m
			                      : (const void *)gd_hmsk_mouse_jump;
		} else if (player.vx) {
			if (player.anim & 8) {
				d = gd_spr_mouse_run1; m = gd_msk_mouse_run1;
				hd = player.face_left ? (const void *)gd_hspr_mouse_run1_m
				                      : (const void *)gd_hspr_mouse_run1;
				hm = player.face_left ? (const void *)gd_hmsk_mouse_run1_m
				                      : (const void *)gd_hmsk_mouse_run1;
			} else {
				d = gd_spr_mouse_run2; m = gd_msk_mouse_run2;
				hd = player.face_left ? (const void *)gd_hspr_mouse_run2_m
				                      : (const void *)gd_hspr_mouse_run2;
				hm = player.face_left ? (const void *)gd_hmsk_mouse_run2_m
				                      : (const void *)gd_hmsk_mouse_run2;
			}
		} else {
			d = gd_spr_mouse_stand; m = gd_msk_mouse_stand;
			hd = player.face_left ? (const void *)gd_hspr_mouse_stand_m
			                      : (const void *)gd_hspr_mouse_stand;
			hm = player.face_left ? (const void *)gd_hmsk_mouse_stand_m
			                      : (const void *)gd_hmsk_mouse_stand;
		}
		if (rop_hw) spr_draw(d, m, hd, hm,
			world_to_fb_x(wx), world_to_fb_y(wy));
		else spr_draw_sw_mirror(d, m, world_to_fb_x(wx),
			world_to_fb_y(wy), player.face_left);
	}

}

/* HUD, drawn in framebuffer coordinates at the top-left of the current
 * VIEW rather than of the page -- it has to stay put while the world
 * scrolls underneath it.
 *
 * Marked like a sprite so it is repaired along with everything else;
 * there is nothing special about text here. */
static void draw_hud(void) {

	char buf[40];
	int32_t wx = game.cam + 4;
	int32_t wy = 4;
	int fbx = world_to_fb_x(wx);
	int fby = world_to_fb_y(wy);

	/* dt is on the HUD deliberately. Motion smoothness IS the frame
	 * rate -- there is no amount of interpolation that makes 12
	 * distinct positions a second look like 60 -- so the only useful
	 * question when movement looks wrong is "how many display frames
	 * is one update covering", and that is this number.
	 *
	 * 1 means the game is keeping up. 2 means half rate. 4 or more
	 * means it is being starved, and since this app's own work is
	 * mostly spinning on the blitter, the likeliest cause is other
	 * processes holding the CPU rather than the drawing itself. */
	snprintf(buf, sizeof(buf), "%d x%d  dt%d  b%d",
		score, lives, (int)hud_dt, (int)hud_blits);

	/* Text through the software path, not the glyph blitter: the HUD
	 * sits over sky, but near the torus seam it can straddle the fold,
	 * and the hardware glyph blit is unclipped by design. A dozen
	 * characters a frame is not worth the hazard. */
	z_fb_draw_text(fbx, fby, buf, 1, &z_font_5x8, NULL);



}

static void draw_banner(const char *s) {
	int32_t wx = game.cam + Z_GAME_VIEW_W / 2 - 40;
	int32_t wy = Z_GAME_VIEW_H / 2 - 8;
	z_fb_draw_text(world_to_fb_x(wx), world_to_fb_y(wy), s, 1,
		&z_font_5x8, NULL);

}

/* -- main ----------------------------------------------------------- */

int main(void) {

	gd_input_t in;
	int death_timer = 0;

	/* Frames elapsed during the PREVIOUS iteration. The world is
	 * stepped this many times, so movement is measured in display
	 * frames rather than in loop iterations.
	 *
	 * Without this the game runs slower in real time whenever a frame
	 * is missed: the loop still advances the player by one step, but
	 * that step now covers two frames of wall clock. Adding the
	 * ambient layer pushed some frames over budget and the mouse
	 * visibly slowed down -- which reads as "the game got slower"
	 * rather than as "the frame rate dropped", because at 30fps the
	 * animation is still perfectly smooth, just at half speed.
	 *
	 * Stepping N times rather than scaling the velocities keeps the
	 * collision code exact: a doubled velocity could tunnel through a
	 * tile that two single steps would land on. */
	uint32_t steps = 1;
	bool prev_vol_down = false, prev_vol_up = false;
	bool prev_mus_down = false, prev_mus_up = false;
	bool prev_tog_amb = false;

	printf("gamedemo: mouse vs cats\n");

	if (!z_game_available()) {
		printf("gamedemo: this bitstream has no game mode.\n");
		printf("gamedemo: rebuild the gateware with `GAME in rtl/boards.vh\n");
		printf("gamedemo: (`make flash`, not `make dev-flash`).\n");
		return 1;
	}

	if (!z_fb_hw_blit_mem_available()) {
		printf("gamedemo: this bitstream's blitter has no memory copy mode.\n");
		return 1;
	}

	gd_level_load();
	ambient_init();
	sound_ok = music_init();

	/* Probed once. On a bitstream without raster ops every op behaves
	 * as COPY, so a hardware masked blit would draw an opaque box --
	 * see spr_draw() above. */
	rop_hw = z_fb_hw_rop_available();

	printf("gamedemo: %d cats, %d px of level, %s\n",
		gd_cat_spawn_count, GD_WORLD_W,
		sound_ok ? "music + sfx" : "no audio on this board");
	printf("gamedemo: %d ambient sprite cells + %d birds\n",
		GD_MAX_CLOUDS * 2 + GD_MAX_TREES * 2, GD_MAX_BIRDS);
	printf("gamedemo: sprites in %s\n",
		rop_hw ? "hardware (blitter raster ops)" : "software (no ROP support)");
	printf("gamedemo: arrows/space or a gamepad. R restarts, ESC quits.\n");
	printf("gamedemo: - = master volume, [ ] music only, A = ambient on/off.\n");
	printf("gamedemo: HUD shows dt (frames per update) and blits per frame.\n");

	/* Wrap ON -- this is a scrolling game and the page is a torus.
	 * See docs/game_mode.md. */
	if (!z_game_begin(&game, Z_GAME_SCROLL_H, true)) {
		printf("gamedemo: could not enter game mode\n");
		return 1;
	}

	world_reset(true);
	music_play(0);

	/* Clear both pages once. After this nothing ever clears again --
	 * every pixel is either redrawn as background or covered by a
	 * sprite, which is the whole point of tracking damage. */
	z_fb_hw_fill_rect(0, 0, 640, 480, 0);

	for (;;) {

		int32_t from, to;
		uint32_t dt;

		input_read(&in);
		if (in.quit) break;

		/* - and = adjust the master volume, which covers music and
		 * effects together. Stepped rather than held-repeat: these are
		 * read as levels (see kbd_held), so a plain "if held, change"
		 * would sweep from silent to full in a fifth of a second. */
		if (in.vol_down && !prev_vol_down) {
			uint8_t v = music_get_volume();
			music_set_volume(v > 16 ? (uint8_t)(v - 16) : 0);
		}
		if (in.vol_up && !prev_vol_up) {
			uint8_t v = music_get_volume();
			music_set_volume(v < 240 ? (uint8_t)(v + 16) : 255);
		}
		/* [ and ] move the MUSIC only, which is the knob that
		 * actually matters here: the complaint that effects cannot be
		 * heard is about the balance, not the overall level, and the
		 * master moves both together so it cannot fix a balance. */
		if (in.mus_down && !prev_mus_down) {
			uint8_t v = music_get_music_volume();
			music_set_music_volume(v > 24 ? (uint8_t)(v - 24) : 0);
		}
		if (in.mus_up && !prev_mus_up) {
			uint8_t v = music_get_music_volume();
			music_set_music_volume(v < 231 ? (uint8_t)(v + 24) : 255);
		}

		prev_vol_down = in.vol_down;
		prev_vol_up = in.vol_up;
		if (in.tog_amb && !prev_tog_amb) show_ambient = !show_ambient;
		prev_tog_amb = in.tog_amb;

		prev_mus_down = in.mus_down;
		prev_mus_up = in.mus_up;

		if (in.restart && (game_over || won || !player.alive))
			world_reset(true);

		ambient_update(steps);

		if (!game_over && !won) {
			if (player.alive) {
				/* Capped. A long stall -- a debugger, a disk hit,
				 * another process hogging the bus -- would otherwise
				 * try to catch up in one go and teleport the player
				 * through a wall. Better to lose time than position. */
				uint32_t k;
				uint32_t n = steps > 4 ? 4 : steps;
				for (k = 0; k < n; k++) {
					update_player(&in);
					update_cats();
					if (!player.alive) break;
				}
				z_game_camera_to(&game,
					TOPX(player.x) - Z_GAME_VIEW_W / 2 + GD_PLAYER_W / 2,
					GD_WORLD_W);
			} else if ((death_timer += (int)steps) > 60) {
				death_timer = 0;
				if (--lives <= 0) game_over = true;
				else world_reset(false);
			}
		}

		/* -- the frame, drawn back to front --
		 *
		 * No damage tracking and no incremental scroll span any more:
		 * parallax makes both wrong (a cloud at quarter speed exposes
		 * background the tile walk does not know is dirty), and at
		 * this sprite count per-sprite repair would touch most of the
		 * screen regardless. Redrawing everything is simpler AND, on a
		 * mostly-sky screen, cheaper -- one ~9600 cycle fill plus the
		 * non-empty tiles beats a 300-tile walk.
		 *
		 * Order matters and is the reason the sky is a fill: empty
		 * tiles are skipped in draw_tiles_rect(), so anything already
		 * drawn on the sky survives the tile pass. */
		/* Clear the sky ACROSS THE VISIBLE SPAN, which is not the same
		 * as the start of the page.
		 *
		 * The page is 640 columns wide and the viewport is 320, sitting
		 * at fold(cam) -- which walks the whole width as the camera
		 * scrolls. Filling from the page origin, as this first did,
		 * clears columns 0..319 forever: once the camera passes column
		 * 320 the visible half is never cleared at all, so sprites
		 * drawn there in earlier frames stay put and the screen fills
		 * with copies of them. It corrects itself when the camera wraps
		 * back into the region that IS being cleared, which is what
		 * makes it look intermittent rather than broken.
		 *
		 * Two fills when the span crosses the seam, one otherwise --
		 * the same shape as everything else that has to live on a
		 * torus. */
		{
			int fx = world_to_fb_x(game.cam);
			int fy = z_game_back_y(&game);
			/* The WHOLE viewport height, not just the sky.
			 *
			 * This stopped at GD_GROUND_ROW on the reasoning that
			 * every row below is covered by opaque ground tiles
			 * anyway. That is true everywhere EXCEPT a pit -- pit
			 * columns have no tiles at all in the ground rows, and
			 * draw_subsurface() deliberately skips them. So the
			 * bottom of every pit was never cleared by anything.
			 *
			 * Both reported symptoms were that one gap: the player
			 * falling down a pit left a copy of itself on every
			 * frame, which reads as "falling forever", and the
			 * accumulated leftovers showed as a solid block sitting
			 * at the top of each pit.
			 *
			 * The saving was about a third of one fill per frame.
			 * Not worth a region of the screen that nothing owns. */
			int ph = Z_GAME_VIEW_H;
			int w1 = Z_SCREEN_W - fx;

			if (w1 > Z_GAME_VIEW_W) w1 = Z_GAME_VIEW_W;

			z_fb_hw_fill_rect_async(fx, fy, w1, ph, 0);

			if (w1 < Z_GAME_VIEW_W)
				z_fb_hw_fill_rect_async(0, fy,
					Z_GAME_VIEW_W - w1, ph, 0);
		}

		/* `a` toggles the ambient layer. This is a DIAGNOSTIC, not a
		 * feature: it is the decisive experiment for whether the
		 * frame rate is limited by drawing or by CPU share. Watch the
		 * HUD -- if blits drop sharply and dt does not move, the
		 * drawing was never the bottleneck and the answer is in the
		 * scheduler. */
		if (show_ambient) ambient_draw();      /* clouds, trees */
		draw_subsurface(game.cam, game.cam + Z_GAME_VIEW_W);
		draw_tiles_rect(game.cam, 0,
			game.cam + Z_GAME_VIEW_W, GD_WORLD_H);
		birds_draw();
		draw_actors();
		draw_hud();

		if (game_over) draw_banner("GAME OVER - R");
		else if (won) draw_banner("YOU WIN! - R");

		/* BARRIER before the flip.
		 *
		 * Every blit above was started and not waited for, so the last
		 * one may still be running. z_game_flip() writes the viewport
		 * origin, which the hardware adopts at the next frame boundary
		 * -- and if the final blit has not landed by then, the frame
		 * shown is one blit short. In practice it always would have
		 * landed (vblank is thousands of cycles away), which is
		 * exactly what makes the omission the kind of bug that appears
		 * once a year on a loaded system.
		 *
		 * z_fb_hw_sync() also drains the rasterizer, so it is the
		 * right barrier even once this app starts drawing lines. */
		z_fb_hw_sync();

		dt = z_game_flip(&game);
		steps = dt;
		hud_dt = dt;
		hud_blits = 0;

		/* Music advances by the frames that ACTUALLY elapsed, so a
		 * frame the game overran does not drag the tempo down with
		 * the frame rate. */
		music_tick(dt);

	}

	z_game_end(&game);

	/* Silences every channel, not just the ones this app used: a
	 * channel left enabled goes on fetching from a buffer about to be
	 * freed with this process. */
	music_shutdown();

	/* Hand the framebuffer back.
	 *
	 * Every window is still alive, still owned and still exactly where
	 * it was -- but this app drew over all of their pixels, and none of
	 * them know that happened. wm repaints damage it caused itself;
	 * this damage came from outside, so it has to be told.
	 *
	 * Without this you return to a desktop that is fine underneath and
	 * garbage on screen, which is a confusing pair of facts to be
	 * handed. See Z_WM_REPAINT in sw/common/zwm.h.
	 *
	 * Fire-and-forget: there is nothing useful to do if wm is not
	 * running, and if it is not, there are no windows to repaint
	 * either. */
	{
		uint32_t wm_pid;
		if (z_pid_lookup("wm0", &wm_pid))
			z_msg_new_send(wm_pid, Z_WM_REPAINT, 0, z_obj_uint32(0));
	}

	printf("gamedemo: score %d\n", score);

	return 0;

}
