/*
 * Zeitlos -- the arcade runtime, the half that talks to the machine.
 * See zarcade.h for the model and zarcade_int.h for the split.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * -- the window --
 *
 * Asked for at 324x255 so its CONTENT is exactly 320x240: zwin.c's
 * content rectangle is the window inset by 2 on every side, below an
 * 11-row titlebar (z_win_content_rect()). The window and full screen
 * then show the same picture pixel for pixel. Not resizable.
 *
 * Z_WIN_FLAG_CLOSE_ICON without CLOSE_KILLS_OWNER, as chip8: a game
 * owns mixer channels that read its own memory, and being killed would
 * leave them reading memory that has just been freed. The close icon
 * arrives as Z_WM_CLOSE and the game shuts down in order.
 *
 * -- full screen --
 *
 * space3d's sequence (docs/space3d_app.md): z_game_set_enabled() tells
 * wm to stop managing windows and routes the keyboard here; the whole
 * framebuffer is cleared, since in colour all four quadrants are
 * bitplanes; leaving gives the screen back and wm repaints the desktop
 * and then asks this window to redraw.
 *
 * Wrap is ON in full screen. The picture never scrolls, but the shake
 * moves the viewport by a few pixels either way, and a negative origin
 * only exists on the torus (docs/game_mode.md, "Clamp and wrap").
 *
 * -- drawing in the window --
 *
 * Each frame the window's visible region is read once (it is what
 * z_win_content_rect() loads into zgfx), turned into rectangles in
 * vis[], and zgfx's own region is then CLEARED: zarcade.c cuts every
 * operation to those rectangles itself and hands zgfx only pieces that
 * are entirely visible. That is the only correct way to do copies
 * (docs/gpu_blitter.md, "Copy and the scissor"), and it lets fills,
 * bitmaps and sprites share one clipping path.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "../zeitlos.h"
#include "../zsoc.h"
#include "../zwm.h"
#include "../zwin.h"
#include "../zgfx.h"
#include "../zcolor.h"
#include "../zkbd.h"
#include "../zpad.h"
#include "../zdialog.h"
#include "../zfsapp.h"
#include "../zdata.h"
#include "../zcfg.h"
#include "../zspeak.h"

#include "zarcade_int.h"
#include "zsfx.h"

static z_win_t own_win;
static z_win_t *wp = &own_win;  /* the window: its own, or the app's (ar_attach()) */
static bool have_win;
/* ar_attach(): the window is the app's, its desktop size kept here, and
 * the resize the runtime asked for not yet answered */
static bool attached;
static bool core_up;             /* ar__core_init() has run */
static uint32_t desk_w, desk_h;
static bool resize_ours;
static void desktop_event(void);
static bool redraw_ack;          /* a Z_WM_REDRAW is waiting for its ack */
static bool exit_armed;          /* Esc released since full screen began */
static bool esc_kept;            /* ar_escape_leaves(false) */
static void (*msg_hook)(void *msg);
static uint32_t last_frame;      /* the display frame counter, last seen */
static uint32_t last_tick;       /* uptime ticks, without a frame counter */
static bool have_counter;

#define WIN_W (AR_W + 4)
#define WIN_H (AR_H + Z_WM_TITLEBAR_H + 4)

/* ===================================================================
 * the machine (zarcade_int.h)
 * =================================================================== */

/* The blitter has work queued that a line must not overtake: set by
 * every blitter call, cleared by the wait before a line. */
static bool blit_pending;
/* ...and the other way: lines queued that a blit must not overtake */
static bool line_pending;

static void after_lines(void) {
	if (line_pending) {
		z_fb_hw_sync();
		line_pending = false;
	}
	blit_pending = true;
}

void arh_fill(int x, int y, int w, int h, int bit) {
	after_lines();
	z_fb_hw_fill_rect(x, y, w, h, bit);
}

void arh_pattern(int x, int y, int w, int h, const uint8_t *pat8) {
	after_lines();
	z_fb_hw_fill_pattern(x, y, w, h, pat8);
}

void arh_blit(const void *src, int stride, int sx, int sy,
	int x, int y, int w, int h, int rop) {
	after_lines();
	z_fb_hw_blit_mem_rop(src, stride, sx, sy, x, y, w, h,
		rop == ARH_COPY ? Z_ROP_COPY : rop == ARH_OR ? Z_ROP_OR : Z_ROP_ANDN);
}

bool arh_copy_ok(void) {
	static int ok = -1;
	if (ok < 0) ok = z_fb_hw_rop_available() ? 1 : 0;
	return ok == 1;
}

void arh_sprite(const void *data, const void *mask, int stride,
	int sx, int sy, int x, int y, int w, int h) {
	after_lines();
	z_fb_hw_blit_sprite(data, mask, stride, sx, sy, x, y, w, h);
}

void arh_vcopy(int sx, int sy, int dx, int dy, int w, int h) {
	after_lines();
	z_fb_hw_blit_vram(sx, sy, dx, dy, w, h);
}

void arh_line(int x0, int y0, int x1, int y1, int op,
	int cx0, int cy0, int cx1, int cy1) {
	z_clip_t c;
	c.x0 = cx0; c.y0 = cy0; c.x1 = cx1; c.y1 = cy1;
	/* the blitter's writes land first: the rasterizer and the blitter
	 * are two masters on VRAM. Once, not per line: a run of lines
	 * queues behind one wait. */
	if (blit_pending) {
		z_fb_hw_sync();
		blit_pending = false;
	}
	line_pending = true;
	z_fb_hw_line(x0, y0, x1, y1, op, &c);
}

/* Pad buttons -> player buttons. X, Y and Select are their own buttons
 * (blocks holds with X); a game that wants "any face button" asks for
 * A | X or B | Y. */
uint32_t arh_pad(int player) {
	uint32_t p = z_pad_read(player), b = 0;
	if (p & Z_PAD_LEFT)  b |= AR_BTN_LEFT;
	if (p & Z_PAD_RIGHT) b |= AR_BTN_RIGHT;
	if (p & Z_PAD_UP)    b |= AR_BTN_UP;
	if (p & Z_PAD_DOWN)  b |= AR_BTN_DOWN;
	if (p & Z_PAD_A) b |= AR_BTN_A;
	if (p & Z_PAD_B) b |= AR_BTN_B;
	if (p & Z_PAD_X) b |= AR_BTN_X;
	if (p & Z_PAD_Y) b |= AR_BTN_Y;
	if (p & Z_PAD_START) b |= AR_BTN_START;
	if (p & Z_PAD_SELECT) b |= AR_BTN_SELECT;
	return b;
}

bool arh_pad_present(int player) {
	return z_pad_present(player);
}

int ar_dir_list(const char *path, char *names, int cap, uint8_t *isdir,
	int max, bool *truncated) {
	uint32_t count = 0, trunc = 0, i;
	int n = 0, w = 0;
	const char *p = names;
	if (truncated) *truncated = false;
	if (cap <= 0 || max <= 0) return -1;
	if (!fs_list_into(path, names, (uint32_t)cap, isdir, (uint32_t)max,
		&count, &trunc)) {
		/* an empty folder and a failure look the same here
		 * (zflist.c): an empty listing either way */
		return 0;
	}
	/* full paths, cut down to their names in place (never longer) */
	for (i = 0; i < count; i++) {
		size_t l = strlen(p);
		const char *base = strrchr(p, '/');
		size_t bl;
		base = base ? base + 1 : p;
		bl = l - (size_t)(base - p);
		if (bl && !(base[0] == '.' && (base[1] == 0 ||
			(base[1] == '.' && base[2] == 0)))) {
			memmove(names + w, base, bl + 1);
			isdir[n] = isdir[i] == Z_FS_TYPE_DIR;
			n++;
			w += (int)bl + 1;
		}
		p += l + 1;
	}
	if (truncated) *truncated = trunc != 0;
	return n;
}

int arh_read_scores(char *buf, int cap) {
	char path[64];
	if (!z_data_file(path, sizeof path, "scores.txt")) return -1;
	return fs_read_file(path, buf, cap);
}

bool arh_write_scores(const char *buf, int len) {
	char path[64];
	if (!z_data_file(path, sizeof path, "scores.txt")) return false;
	return fs_write_file(path, (char *)buf, len) >= 0;
}

int arh_read_settings(char *buf, int cap) {
	char path[64];
	if (!z_data_file(path, sizeof path, "settings.txt")) return -1;
	return fs_read_file(path, buf, cap);
}

bool arh_write_settings(const char *buf, int len) {
	char path[64];
	if (!z_data_file(path, sizeof path, "settings.txt")) return false;
	return fs_write_file(path, (char *)buf, len) >= 0;
}

int arh_cfg_skill(void) {
	char v[16];
	if (!z_cfg_get("apps.arcade.skill", v, sizeof v)) return -1;
	return ar_skill_word(v);
}

void arh_say(const char *text) {
	z_speak(text, Z_TTS_F_INTERRUPT);
}

uint32_t arh_seed(void) {
	return z_uptime_ticks() * 2654435761u ^ (uint32_t)z_game_frame();
}

/* ===================================================================
 * where to draw
 * =================================================================== */

void arh_targets(void) {

	ar__s.targets_stale = false;

	if (ar__s.mode != AR_MODE_WINDOW) {
		ar__s.ox = 0;
		ar__s.oy = 0;
		ar__s.vis[0].x = 0; ar__s.vis[0].y = 0;
		ar__s.vis[0].w = AR_W; ar__s.vis[0].h = AR_H;
		ar__s.nvis = 1;
		ar__s.vis_whole = true;
		ar__s.can_draw = true;
		z_gfx_clear_visible();
		z_gfx_blit_scissor_reset();
		return;
	}

	ar__s.nvis = 0;
	ar__s.vis_whole = false;
	ar__s.can_draw = have_win && !z_win_frozen(wp);
	if (!ar__s.can_draw) return;

	{
		z_clip_t cr;
		int n, i;

		z_win_content_rect(wp, &cr);    /* loads the region */
		ar__s.ox = cr.x0;
		ar__s.oy = cr.y0;

		n = z_gfx_visible_count();
		if (n == 0) {
			/* Unrestricted: only before wm has spoken, which
			 * zwin maps to empty -- but be exact either way. */
			ar__s.vis[0].x = (int16_t)cr.x0;
			ar__s.vis[0].y = (int16_t)cr.y0;
			ar__s.vis[0].w = (int16_t)(cr.x1 - cr.x0 + 1);
			ar__s.vis[0].h = (int16_t)(cr.y1 - cr.y0 + 1);
			ar__s.nvis = 1;
		} else {
			for (i = 0; i < n && ar__s.nvis < AR_MAX_VIS; i++) {
				z_clip_t out;
				ar_rect_t *r;
				if (!z_gfx_visible_clip(i, &cr, &out)) continue;
				r = &ar__s.vis[ar__s.nvis++];
				r->x = (int16_t)out.x0;
				r->y = (int16_t)out.y0;
				r->w = (int16_t)(out.x1 - out.x0 + 1);
				r->h = (int16_t)(out.y1 - out.y0 + 1);
			}
		}

		ar__s.vis_whole = ar__s.nvis == 1 &&
			ar__s.vis[0].x == cr.x0 && ar__s.vis[0].y == cr.y0 &&
			ar__s.vis[0].w == AR_W && ar__s.vis[0].h == AR_H;

		z_gfx_clear_visible();
		z_gfx_blit_scissor_reset();
	}
}

/* ===================================================================
 * modes
 * =================================================================== */

static void mode_event(void) {
	ar_event_t e;
	memset(&e, 0, sizeof(e));
	e.type = AR_EV_MODE;
	ar__queue(&e);
}

bool ar_color_available(void) {
	return z_color_available();
}

static void clear_framebuffer(void) {
	z_gfx_clear_visible();
	z_gfx_blit_scissor_reset();
	z_fb_hw_fill_rect(0, 0, Z_SCREEN_W, Z_SCREEN_H, 0);
}

static void apply_color(void) {
	if (ar__s.want_color && z_color_available()) {
		int n = ar__choose_planes(z_color_planes_max());
		z_color_begin(n == 8 ? &z_color_256 : n == 6 ? &z_color_64 : &z_color_16);
		ar__s.mode = AR_MODE_FULL_COLOR;
		ar__pal_all_dirty();
	} else {
		if (z_color_available()) z_color_end();
		ar__s.mode = AR_MODE_FULL_MONO;
	}
}

int ar_set_colours(int n) {
	ar__s.colours_want = n >= 256 ? 256 : n >= 64 ? 64 : 16;
	if (ar__s.mode == AR_MODE_FULL_COLOR) {
		apply_color();
		ar__invalidate();
		ar__s.targets_stale = true;
	} else {
		ar__choose_planes(z_color_available() ? z_color_planes_max() : 4);
	}
	return 1 << ar__s.planes;
}

/* -- colour in the window (docs/color.md, "The colour window") --
 *
 * A program that asks (ar_window_colour(), before ar_init()) creates
 * its window with Z_WIN_FLAG_COLOR. wm tells the focused one it may
 * have the colour (Z_WM_COLOR 1) and clears it on a focus change, a
 * freeze or a narrowing; the window takes it itself, at a frame, while
 * it is wholly uncovered and the desktop is showing: COLWIN set to its
 * content, the picture drawn in colour. Covered, or on another frame
 * without it, the window clears it and draws in black and white. */
static bool wc_wanted;      /* the program asked */
static void colour_want(bool on);
static bool wc_focus;       /* wm said this window may have it */
static int16_t wc_x, wc_y;  /* where COLWIN was set */

void ar_window_colour(bool on) { wc_wanted = on; }

/* Once a frame, after the region is known (arh_targets()). */
static void window_colour_check(void) {
	bool want = wc_focus && !ar__s.wc_off && ar__s.mode == AR_MODE_WINDOW &&
		ar__s.can_draw && ar__s.vis_whole && !z_game_enabled();
	if (want) {
		/* again when the window moved, or the hardware dropped it --
		 * a game took the planes and the palette meanwhile */
		if (!ar__s.wcolor || wc_x != ar__s.ox || wc_y != ar__s.oy ||
		    !z_color_window_on()) {
			wc_x = (int16_t)ar__s.ox;
			wc_y = (int16_t)ar__s.oy;
			z_fb_set_rows((int)z_color_rows());
			z_color_window_set(ar__s.ox, ar__s.oy, ar__s.ox + AR_W,
				ar__s.oy + AR_H);
			ar__wcolor(true, (int)z_color_window_planes(),
				(int)z_color_rows() - 480);
		}
	} else if (ar__s.wcolor) {
		if (wc_focus) z_color_window_off();
		z_fb_set_rows(480);
		ar__wcolor(false, 0, 0);
	}
}

bool ar_set_fullscreen(bool on) {

	if (on == (ar__s.mode != AR_MODE_WINDOW)) return true;

	/* full screen is game mode's colour: entering it clears the
	 * window's (COLWIN), which comes back at a frame in the window */
	if (on && ar__s.wcolor) {
		ar__s.wcolor = false;
		z_fb_set_rows(480);
	}

	if (on) {
		if (!z_game_available()) return false;
		clear_framebuffer();
		z_game_set_pointer(ar__s.d && ar__s.d->pointer);
		z_game_set_enabled(true, true);
		z_game_set_view(0, 0);
		exit_armed = false;
		apply_color();
	} else {
		/* Releases wm's grab as well as the register; colour goes off
		 * with game mode, in hardware. wm repaints the desktop on the
		 * release and sends this window a REDRAW. */
		z_game_set_enabled(false, false);
		ar__s.mode = AR_MODE_WINDOW;
		ar__s.view_dx = ar__s.view_dy = 0;
	}

	ar__invalidate();
	ar__s.targets_stale = true;
	mode_event();
	return true;
}

void ar_set_color(bool on) {
	ar__s.want_color = on;
	if (ar__s.mode == AR_MODE_WINDOW) return;
	clear_framebuffer();
	apply_color();
	ar__invalidate();
	mode_event();
}

/* ===================================================================
 * help
 * =================================================================== */

static void render_now(void) {
	arh_targets();
	ar__render();
	z_fb_hw_sync();
}

/* While the help box has the message queue: keep the picture right
 * behind it. The game is paused meanwhile, so a redraw just repaints
 * what is there. */
static void help_on_msg(z_msg_t *m, void *user) {
	(void)user;
	if (m->subject == Z_WM_SET_CLIP) {
		z_win_apply_clip(wp, &m->obj);
		ar__s.targets_stale = true;
	} else if (m->subject == Z_WM_REDRAW) {
		z_win_apply_redraw(wp, m->obj.val.uint32);
		z_win_damage_ignore(wp);
		ar__invalidate();
		render_now();
		z_win_redraw_done(wp);
	} else if (m->subject == Z_WM_WINDOW_MOVED) {
		z_win_parse_rect(wp, &m->obj);
		ar__invalidate();
	}
}

static void release_all(void);

static void show_help(void) {
	z_dialog_ctx_t ctx = { wp, help_on_msg, NULL };
	ar_event_t e;
	const ar_desc_t *d = ar__s.d;

	if (!d->help_text) return;

	/* Pause first, so the game is already holding when the box comes
	 * up and nothing moves behind it. */
	memset(&e, 0, sizeof(e));
	e.type = AR_EV_SUSPEND;
	ar__queue(&e);

	zsfx_suspend(true);
	z_dialog_help(&ctx, d->help_title ? d->help_title : d->name,
		d->help_text, d->help_doc);
	zsfx_suspend(false);

	/* Releases went to the box: no button may stay held. */
	release_all();
	ar__invalidate();
}

/* The screen toolkit's help (ui/sui.h), when the program links it: on
 * its own screen, in the window and full screen alike. A weak
 * reference, so a program without the toolkit keeps the desktop's box
 * (show_help(), the window only) and pays nothing. The pause goes to
 * the game after the box -- the box reads the queue while it is up --
 * so the game is paused when it comes back. */
extern void sui_help(const char *title, const char *text) __attribute__((weak));

static void show_screen_help(void) {
	ar_event_t e;
	const ar_desc_t *d = ar__s.d;
	if (!d->help_text) return;
	release_all();
	zsfx_suspend(true);
	sui_help(d->help_title ? d->help_title : d->name, d->help_text);
	zsfx_suspend(false);
	release_all();
	memset(&e, 0, sizeof(e));
	e.type = AR_EV_SUSPEND;
	ar__queue(&e);
}

/* A box opened from full screen: the system's boxes are wm's windows,
 * and game mode is not the desktop, so the box is shown in the window
 * and full screen comes back after it -- at the next frame, so that a
 * second box straight after the first (a question, then a file) does
 * not flick the screen to full and back between them. */
static bool dlg_was_full, dlg_back_full;

void ar_dialog_begin(void *out) {
	z_dialog_ctx_t *ctx = out;
	if (dlg_back_full) dlg_back_full = false, dlg_was_full = true;
	else dlg_was_full = ar__s.mode != AR_MODE_WINDOW;
	if (ar__s.mode != AR_MODE_WINDOW) ar_set_fullscreen(false);
	render_now();
	zsfx_suspend(true);
	ctx->parent = wp;
	ctx->on_msg = help_on_msg;
	ctx->user = NULL;
}

void ar_dialog_end(void) {
	zsfx_suspend(false);
	release_all();
	ar__invalidate();
	dlg_back_full = dlg_was_full;
	dlg_was_full = false;
}

/* ===================================================================
 * keys and messages
 * =================================================================== */

/* Releases for everything held, through the normal path, so the game
 * sees each button come up. */
static void release_all(void) {
	static const uint32_t keys[] = {
		Z_KEY_LEFT, Z_KEY_RIGHT, Z_KEY_UP, Z_KEY_DOWN, ' ', 8, '\r',
		'a', 'd', 'w', 's', 'q', 'e', '\t'
	};
	unsigned i;
	for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		ar__key(keys[i], false, 0);
}

static void handle_key(uint32_t packed) {

	uint32_t k = Z_WM_UNPACK_KEY_KEYSYM(packed);
	bool pressed = Z_WM_UNPACK_KEY_PRESSED(packed) != 0;
	uint8_t mods = (uint8_t)Z_WM_UNPACK_KEY_MODIFIERS(packed);
	bool full = ar__s.mode != AR_MODE_WINDOW;

	/* A box is up (ar_top): every key but F11 is the box's -- Esc
	 * answers it rather than leaving full screen, F1 opens no other
	 * box, and C and M are letters. */
	if (ar__s.modal && k != Z_KEY_FULL_SCREEN) {
		ar__key(k, pressed, mods);
		return;
	}

	if (k == Z_KEY_HELP) {
		if (pressed && sui_help) show_screen_help();
		else if (pressed && !full) show_help();
		return;
	}

	if (k == Z_KEY_FULL_SCREEN) {
		if (pressed) {
			release_all();
			ar_set_fullscreen(!full);
		}
		return;
	}

	/* F10: colour on or off -- full screen as C does, in the window
	 * the colour window (where the program asked for it and the board
	 * has it); the next frame takes it or gives it back */
	if (k == Z_KEY_COLOUR) {
		if (pressed) {
			if (attached) {
				/* a dual app: back to its desktop face */
				release_all();
				desktop_event();
			} else if (full) {
				if (z_color_available()) ar_set_color(!ar__s.want_color);
			} else if (wc_wanted && z_color_window_available()) {
				ar__s.wc_off = !ar__s.wc_off;
				colour_want(!ar__s.wc_off);
			}
		}
		return;
	}

	/* Escape leaves full screen once it has been RELEASED there:
	 * entering by any route that leaves a key down would otherwise see
	 * a release at once and leave again (space3d, chess). */
	if (full && k == 0x1b && !esc_kept) {
		if (!pressed) exit_armed = true;
		else if (exit_armed) {
			release_all();
			ar_set_fullscreen(false);
		}
		return;
	}

	if (ar__s.text_input) {
		ar__key(k, pressed, mods);
		return;
	}

	if (full && (k == 'c' || k == 'C')) {
		if (pressed && z_color_available()) ar_set_color(!ar__s.want_color);
		return;
	}

	if (k == 'm' || k == 'M') {
		if (pressed) zsfx_music_enable(!zsfx_music_enabled());
		return;
	}

	ar__key(k, pressed, mods);
}

static void handle_msg(z_msg_t *m) {

	ar_event_t e;

	switch (m->subject) {

	case Z_WM_SET_CLIP:
		z_win_apply_clip(wp, &m->obj);
		/* Anything drawn before the next frame must use the new
		 * region: a copy into a part now covered would land on the
		 * window in front. */
		ar__s.targets_stale = true;
		break;

	case Z_WM_REDRAW:
		/* Repaint all of it: an animated picture cannot repaint by
		 * damage alone (zwin.h, z_win_damage_ignore()). The ack goes
		 * after the frame that does it. In full screen the window is
		 * not on the glass, so there is nothing to paint -- but wm
		 * still waits for the ack. */
		z_win_apply_redraw(wp, m->obj.val.uint32);
		z_win_damage_ignore(wp);
		if (ar__s.mode == AR_MODE_WINDOW) ar__invalidate();
		redraw_ack = true;
		break;

	case Z_WM_WINDOW_MOVED:
		z_win_parse_rect(wp, &m->obj);
		if (ar__s.mode == AR_MODE_WINDOW) ar__invalidate();
		ar__s.targets_stale = true;
		break;

	case Z_WM_KEY:
		handle_key(m->obj.val.uint32);
		break;

	case Z_WM_MOUSE:
		memset(&e, 0, sizeof(e));
		e.type = AR_EV_MOUSE;
		{
			uint32_t u = m->obj.val.uint32;
			int cx, cy;
			if (ar__s.mode == AR_MODE_WINDOW) {
				e.inside = z_win_mouse_content_xy(wp, u, &cx, &cy);
				/* That loaded the window's visible region into zgfx
				 * (z_win_content_rect()'s side effect). The runtime
				 * cuts its drawing to the region itself and draws
				 * the colour window's planes below row 480, outside
				 * any region: a draw made before the next frame --
				 * space3d's lines -- would lose its planes to it. */
				z_gfx_clear_visible();
				z_gfx_blit_scissor_reset();
			} else {
				/* Full screen: the pointer in desktop coordinates,
				 * halved -- the picture is pixel-doubled. */
				cx = (int)Z_WM_UNPACK_MOUSE_X(u) / 2;
				cy = (int)Z_WM_UNPACK_MOUSE_Y(u) / 2;
				e.inside = 1;
			}
			e.x = (int16_t)cx;
			e.y = (int16_t)cy;
			e.buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(u);
		}
		ar__queue(&e);
		break;

	case Z_WM_WHEEL:
		memset(&e, 0, sizeof(e));
		e.type = AR_EV_WHEEL;
		e.notches = (int16_t)Z_WM_WHEEL_NOTCHES(m->obj.val.uint32);
		ar__queue(&e);
		break;

	case Z_WM_COLOR:
		/* wm cleared COLWIN when it said no: the next frame draws in
		 * black and white without touching it */
		wc_focus = m->obj.val.uint32 != 0;
		break;

	case Z_WM_CLOSE:
		memset(&e, 0, sizeof(e));
		e.type = AR_EV_QUIT;
		ar__queue(&e);
		break;

	case Z_WM_TITLEBAR_ICON:
		/* the colour icon: as F10 */
		if (Z_WM_UNPACK_TBICON_KIND(m->obj.val.uint32) == Z_WM_TBICON_COLOUR) {
			if (attached) desktop_event();
			else if (wc_wanted) {
				ar__s.wc_off = !ar__s.wc_off;
				colour_want(!ar__s.wc_off);
			}
		} else if (msg_hook) {
			msg_hook(m);
		}
		break;

	case Z_WM_WINDOW_RESIZED:
		/* the size the runtime asked for (ar_attach()), or the user's
		 * drag of a resizable window's grip: a dual app goes back to
		 * its desktop face at that size */
		z_win_apply_resized(wp, &m->obj);
		ar__s.targets_stale = true;
		if (ar__s.mode == AR_MODE_WINDOW) ar__invalidate();
		if (resize_ours) resize_ours = false;
		else if (attached) {
			desk_w = 0;          /* keep the user's size */
			desktop_event();
		}
		break;

	default:
		if (msg_hook) msg_hook(m);
		break;
	}
}

void ar_msg_hook(void (*fn)(void *msg)) { msg_hook = fn; }
void ar_msg(void *msg) { handle_msg((z_msg_t *)msg); }
void ar_escape_leaves(bool on) { esc_kept = !on; }

bool ar_frame_due(void) {
	if (have_counter) return ((z_game_frame() - last_frame) & 0xffffu) != 0;
	return z_uptime_ticks() - last_tick >= (Z_TICK_HZ / 60u ? Z_TICK_HZ / 60u : 1u);
}

bool ar_poll(ar_event_t *e) {
	z_msg_t m;
	for (;;) {
		if (ar__dequeue(e)) return true;
		if (!have_win) return false;
		if (z_msg_read(&m) != Z_OK) return false;
		handle_msg(&m);
	}
}

/* ===================================================================
 * the frame
 * =================================================================== */

/* ar_vsync_exact(): where the boundaries fall, in 16ths of a tick --
 * one seen while watching for it, and the frame counter's value then --
 * so the next is expected at edge + frames * (Z_TICK_HZ/60). Learned
 * again at every boundary seen while watching; until one has been, the
 * wait watches the whole time. */
static bool vsync_exact;
static bool edge_known;
static uint32_t edge_fp, edge_frame;
static void (*frame_hook)(void);

#define FRAME_FP ((uint32_t)Z_TICK_HZ * 16u / 60u)
/* start watching this long before the boundary is expected */
#define EXACT_SPIN_FP (2u * 16u)

void ar_vsync_exact(bool on) { vsync_exact = on; edge_known = false; }
void ar_frame_hook(void (*fn)(void)) { frame_hook = fn; }

/* Wait for the next display frame and say how many went by. Yields
 * while it waits (docs/app_runtime.md): a game that spins takes a full
 * scheduler share from everything else on the machine -- except, with
 * ar_vsync_exact(), for the last two ticks before the boundary. */
static uint32_t wait_frame(void) {

	uint32_t n;

	if (have_counter) {
		uint32_t start = z_uptime_ticks();
		bool watching = false;
		for (;;) {
			uint32_t f = z_game_frame();
			n = (f - last_frame) & 0xffffu;
			if (n) {
				last_frame = f;
				if (watching) {
					edge_fp = z_uptime_ticks() * 16u;
					edge_frame = f;
					edge_known = true;
				}
				break;
			}
			/* A counter that has not moved for a tenth of a second
			 * is not counting (no scanout?): pace from the clock
			 * from now on rather than wait forever. */
			if (z_uptime_ticks() - start > Z_TICK_HZ / 10u) {
				have_counter = false;
				last_tick = z_uptime_ticks();
				n = 1;
				break;
			}
			if (vsync_exact) {
				uint32_t next = edge_fp + ((last_frame + 1u - edge_frame) & 0xffffu) * FRAME_FP;
				if (!edge_known || (int32_t)(z_uptime_ticks() * 16u + EXACT_SPIN_FP - next) >= 0) {
					watching = true;
					continue;
				}
			}
			z_proc_wait(1);
		}
	} else {
		/* No frame counter: 60Hz from the tick clock. */
		uint32_t per = Z_TICK_HZ / 60u;
		if (per == 0) per = 1;
		for (;;) {
			uint32_t t = z_uptime_ticks();
			n = (t - last_tick) / per;
			if (n) { last_tick += n * per; break; }
			z_proc_wait(1);
		}
	}

	if (n > 4) n = 4;
	return n;
}

uint32_t ar_frame(void) {

	uint32_t dt;

	/* full screen again after a box (ar_dialog_begin()) */
	if (dlg_back_full) {
		dlg_back_full = false;
		ar_set_fullscreen(true);
	}
	dt = wait_frame();
	int i;

	/* Straight after the boundary: palette first (it changes every
	 * pixel at once), then the viewport, then the drawing. */
	if (ar__s.mode == AR_MODE_FULL_COLOR || ar__s.wcolor) {
		int n = 1 << (ar__s.wcolor ? ar__s.wl : ar__s.planes);
		for (i = 0; i < n; i++)
			if (ar__s.pal_dirty[i >> 5] & (1u << (i & 31))) {
				if (ar__s.wcolor) {
					/* in the window, colour i is entry bit 0 | the
					 * rest from bit 4 (zsoc.h); and every index the
					 * window's planes can show with the same low
					 * bits is colour i too, so a plane this program
					 * does not draw -- the fifth, for 16 colours,
					 * holding what pix left there -- shows nothing */
					int j;
					for (j = i; j < (1 << ar__s.wl_hw); j += n)
						z_color_set_palette(
							z_color_window_entry((uint32_t)j), ar__s.pal[i]);
				} else {
					z_color_set_palette((uint32_t)i, ar__s.pal[i]);
				}
				ar__s.pal_dirty[i >> 5] &= ~(1u << (i & 31));
			}
	}

	if (ar__s.mode != AR_MODE_WINDOW) {
		int vx, vy;
		ar__shake_step();
		vx = (ar__s.view_dx + 640) % 640;
		vy = (ar__s.view_dy + 480) % 480;
		z_game_set_view((uint32_t)vx, (uint32_t)vy);
	}

	arh_targets();
	if (wc_wanted) window_colour_check();
	/* not while a box is up (ar_top): its drawing would land on the box */
	if (frame_hook && !ar__s.modal) frame_hook();
	ar__render();
	z_fb_hw_sync();

	if (redraw_ack && have_win) {
		z_win_redraw_done(wp);
		redraw_ack = false;
	}

	zsfx_tick(dt);
	ar__frame_input();

	ar__s.ticks += dt;
	return dt;
}

/* ===================================================================
 * setup
 * =================================================================== */

/* Everything but the window, once. */
static void core_up_once(const ar_desc_t *d) {
	if (core_up) { ar__s.d = d; return; }
	core_up = true;
	ar__core_init(d);
	ar_seed(arh_seed());

	have_counter = z_game_present();
	last_frame = z_game_frame();
	last_tick = z_uptime_ticks();

	if (!zsfx_init())
		printf("%s: no audio mixer on this board -- playing silently\n",
			d->name);
}

static uint32_t window_flags(const ar_desc_t *d) {
	bool cw = wc_wanted && z_color_window_available();
	return Z_WIN_FLAG_GAME_MODE |
		(cw ? Z_WIN_FLAG_COLOR | Z_WIN_FLAG_COLOUR_ICON : 0) |
		/* a game takes no mouse: no pointer over its picture */
		(d->pointer ? 0 : Z_WIN_FLAG_NO_POINTER);
}

/* A dual app's window: the colour window only once it is attached
 * (Z_WIN_FLAG_COLOUR_LATER) -- on its desktop face desktop colour
 * keeps the second sheet. */
uint32_t ar_window_flags(const ar_desc_t *d) {
	uint32_t f = window_flags(d);
	return (f & Z_WIN_FLAG_COLOR) ? f | Z_WIN_FLAG_COLOUR_LATER : f;
}

/* Whether this window wants the colour window when focused (wm,
 * Z_WM_COLOR_WANT): a screen app's F10, a dual app attaching and
 * detaching. */
static void colour_want(bool on) {
	uint32_t wm;
	if (!wc_wanted || wp->id < 0 || !z_pid_lookup("wm0", &wm)) return;
	z_msg_new_send(wm, Z_WM_COLOR_WANT, (uint32_t)wp->id, z_obj_uint32(on ? 1u : 0u));
}

bool ar_init(const ar_desc_t *d) {

	core_up_once(d);

	if (z_win_create_flags(wp, d->name, WIN_W, WIN_H, -1, -1,
	    Z_WIN_FLAG_CLOSE_ICON | window_flags(d)) != Z_OK) {
		printf("%s: could not create a window\n", d->name);
		return false;
	}
	have_win = true;

	ar__invalidate();
	return true;
}

/* A dual app's switch (docs/app_kinds.md): the runtime takes the app's
 * window -- its 320x240 screen face -- and gives it back. */
static void desktop_event(void) {
	ar_event_t e;
	memset(&e, 0, sizeof(e));
	e.type = AR_EV_DESKTOP;
	ar__queue(&e);
}

bool ar_attach(void *win, const ar_desc_t *d) {
	if (attached) return true;
	core_up_once(d);
	wp = (z_win_t *)win;
	desk_w = wp->w;
	desk_h = wp->h;
	attached = have_win = true;
	/* wm hands the colour window over (Z_WM_COLOR 1) once it has taken
	 * desktop colour's tints back from the apps */
	wc_focus = false;
	colour_want(true);
	ar__s.mode = AR_MODE_WINDOW;
	if (wp->w != WIN_W || wp->h != WIN_H) {
		resize_ours = true;
		z_win_resize(wp, WIN_W, WIN_H);
	}
	ar__invalidate();
	ar__s.targets_stale = true;
	return true;
}

void ar_detach(void) {
	ar_event_t e;
	if (!attached) return;
	if (ar__s.mode != AR_MODE_WINDOW) ar_set_fullscreen(false);
	if (ar__s.wcolor) {
		if (wc_focus) z_color_window_off();
		z_fb_set_rows(480);
		ar__s.wcolor = false;
	}
	if (redraw_ack) {
		z_win_redraw_done(wp);
		redraw_ack = false;
	}
	if (desk_w && (desk_w != WIN_W || desk_h != WIN_H))
		z_win_resize(wp, desk_w, desk_h);
	resize_ours = false;
	colour_want(false);
	attached = have_win = false;
	/* what is queued was the screen face's */
	while (ar__dequeue(&e)) ;
	wp = &own_win;
}

bool ar_attached(void) { return attached; }

void ar_shutdown(void) {
	if (attached) { ar_detach(); return; }
	if (ar__s.mode != AR_MODE_WINDOW) ar_set_fullscreen(false);
	if (ar__s.wcolor && wc_focus) z_color_window_off();
	zsfx_shutdown();
	if (have_win) z_win_destroy(wp);
	have_win = false;
}
