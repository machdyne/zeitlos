/*
 * Zeitlos mahjong -- mahjong solitaire.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A dual app (docs/app_kinds.md): a resizable black-and-white window
 * on the desktop, and the same table on a 320x240 screen in colour --
 * in the window (F10, or the colour icon) or full screen (F11).
 * docs/mahjong_app.md.
 *
 * -- who does what --
 *
 *   mj_game    the rules: the deal, the free rule, undo, shuffles
 *   mj_board   the picture: where each tile lands, drawn on a canvas
 *   mj_play    what a click or a key does, and what that changes
 *   mahjong.c  this file: the window, the two faces, files, sound
 *
 * Nothing in the first three does I/O, which is what lets the host
 * tests drive the shipped code; this file is the part only a board
 * can run.
 *
 * -- one canvas, two ways to show it --
 *
 * The table is composed in memory (mj_canvas.h for why) and shown by
 * copying: on the desktop face, z_fb_hw_blit_mem() into the window's
 * visible pieces; on the screen face, the runtime's paint() callback
 * copies one bitplane per colour bit with ar_plane() (in colour) or
 * the mono plane with ar_bitmap_opaque(). A change -- a pair taken, a
 * tile picked up -- recomposes only the rectangles it touched and
 * copies only those, so a move costs a few hundred words of arithmetic
 * and two small copies.
 *
 * The canvas's memory is one static array the two faces take turns
 * with: the desktop's mono picture (up to 640x480, one plane) or the
 * screen's (320x240, up to six planes for 64 colours). Switching faces
 * recomposes the whole table, which is a few tens of milliseconds.
 */

#include <stdio.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zwin.h"
#include "../../common/zwm.h"
#include "../../common/zgfx.h"
#include "../../common/zsoc.h"
#include "../../common/zkbd.h"
#include "../../common/zdialog.h"
#include "../../common/zfsapp.h"
#include "../../common/zdata.h"
#include "../../common/zspeak.h"
#include "../../common/ztts.h"
#include "../../common/games/zrand.h"
#include "../../common/games/zarcade.h"
#include "../../common/games/zsfx.h"

#include "mj_play.h"
#include "mj_face.h"
#include "mj_music.h"

/* The window: the Turtle at the large size fits its content. */
#define WIN_W 466
#define WIN_H 356

/* Its own folder, /data/mahjong, named by z_data_file() (zdata.h):
 * stats.txt there, and layout files in its layouts folder. */
#define STATS_LEAF   "stats.txt"
#define LAYOUTS_LEAF "layouts"

static z_win_t     win;
static mj_play_t   P;

/* the canvas and its memory are mj_face.c's */
#define cv mj_cv

static bool running = true;
static int  mouse_x = -1, mouse_y = -1;   /* the pointer on the screen face */
static bool scr;                /* the screen face: the runtime has the window */
static bool want_desktop;
static bool hw_blit;
static int  cw, ch;             /* the desktop canvas, as last set up */
static int  ox, oy;             /* where it sits on the screen */
static uint32_t frames;

static char help_text[1200];

static void flush(void);
static void show(int x, int y, int w, int h);

/* -- layouts: the built-in ones, then the files ------------------------------ */

static int  nbuiltin;
static char file_of[MJ_LAYOUTS][40];   /* a layout file's name, by layout index */

/* The `name` line of a layout's text, without parsing the rest. */
static void name_of(const char *text, char *out, int len)
{
	const char *s = text;
	out[0] = 0;
	while (*s) {
		const char *e = s;
		while (*e && *e != '\n') e++;
		if (e - s > 5 && !memcmp(s, "name ", 5)) {
			int i = 0;
			s += 5;
			while (*s == ' ') s++;
			while (s < e && *s != '\r' && i < len - 1) out[i++] = *s++;
			while (i && out[i - 1] == ' ') i--;
			out[i] = 0;
			return;
		}
		s = *e ? e + 1 : e;
	}
}

static const char *layout_src(void *user, int i, char *buf, int cap)
{
	char leaf[64], path[96];
	int n;
	(void)user;
	if (i < nbuiltin) return mj_layout_builtin_text(i);
	snprintf(leaf, sizeof(leaf), LAYOUTS_LEAF "/%.39s", file_of[i]);
	if (!z_data_file(path, sizeof(path), leaf)) return NULL;
	n = fs_read_file(path, buf, cap - 1);
	if (n < 0) return NULL;
	buf[n] = 0;
	return buf;
}

static void list_layouts(void)
{
	static char names[1024], text[4096];
	uint8_t isdir[MJ_LAYOUTS];
	char nm[MJ_NAME];
	int i, n;
	const char *s;

	nbuiltin = mj_layout_builtin_count();
	for (i = 0; i < nbuiltin; i++) {
		name_of(mj_layout_builtin_text(i), nm, sizeof(nm));
		mj_play_add_layout(&P, nm);
	}

	{
		char dir[96];
		if (!z_data_file(dir, sizeof(dir), LAYOUTS_LEAF)) return;
		fs_mkdir(dir);  /* there to drop files into, from the start */
		n = ar_dir_list(dir, names, sizeof(names), isdir, MJ_LAYOUTS - nbuiltin, NULL);
	}
	for (i = 0, s = names; i < n; i++, s += strlen(s) + 1) {
		char leaf[80], path[96];
		int len = (int)strlen(s), k;
		if (isdir[i] || len < 5 || strcmp(s + len - 4, ".txt")) continue;
		snprintf(leaf, sizeof(leaf), LAYOUTS_LEAF "/%.60s", s);
		if (!z_data_file(path, sizeof(path), leaf)) continue;
		k = fs_read_file(path, text, sizeof(text) - 1);
		if (k <= 0) continue;
		text[k] = 0;
		name_of(text, nm, sizeof(nm));
		if (!nm[0]) {
			/* no name line: the file's name, less .txt */
			int j;
			for (j = 0; j < len - 4 && j < MJ_NAME - 1; j++) nm[j] = s[j];
			nm[j] = 0;
		}
		if (mj_play_find_layout(&P, nm) >= 0) continue;
		if (P.nlayouts >= MJ_LAYOUTS) break;
		if (len >= (int)sizeof(file_of[0])) continue;   /* too long to keep */
		memcpy(file_of[P.nlayouts], s, (size_t)len + 1);
		mj_play_add_layout(&P, nm);
	}
}

/* -- statistics and settings ---------------------------------------------- */

static void load_stats(void)
{
	static char buf[2048];
	char path[64];
	int n;
	if (!z_data_file(path, sizeof(path), STATS_LEAF)) return;
	n = fs_read_file(path, buf, sizeof(buf) - 1);
	if (n <= 0) return;
	buf[n] = 0;
	mj_play_load_stats(&P, buf);
}

static void save_stats(void)
{
	static char buf[2048];
	char path[64];
	int n = mj_play_save_stats(&P, buf, sizeof(buf));
	/* z_data_file() makes the folder if it has gone */
	if (!z_data_file(path, sizeof(path), STATS_LEAF) ||
	    fs_write_file(path, buf, n) != n)
		puts("mahjong: could not write its stats.txt");
}

static uint32_t new_seed(void)
{
	/* eight digits at most, so the number fits the bar */
	return 1 + zg_rng_below(99999999u);
}

static uint32_t now_s(void)
{
	return z_uptime_ticks() / Z_TICK_HZ;
}

/* -- the screen face ----------------------------------------------------------- */

static ar_desc_t desc = {
	.name = "mahjong",
	.help_title = "Mahjong help",
	.help_text = help_text,
	.help_doc = "mahjong_app",
	.paint = mj_face_paint,
	.pointer = true,
};

/* The screen face is composed once per mode, straight before the
 * frame that shows it (mj_face_check(), mj_face.h): entering it and
 * every change of mode after only say the canvas is stale. Before, a
 * switch composed the table up to three times -- in screen_on(), on the
 * AR_EV_MODE that followed, and on the planes changing -- at about a
 * second each on the board, which is what made switching slow and wm
 * wait on it. */

static void screen_on(bool full)
{
	if (!scr) {
		scr = true;
		mouse_x = -1;   /* until the pointer says where it is */
		mj_face_begin(full);
		ar_attach(&win, &desc);
		/* 64 colours where the board has them, full screen; the
		 * palette's first 16 are the picture in 16 */
		ar_set_colours(64);
		ar_pal_load(mj_palette, MJ_C_COLOURS);
		/* the mixer is the runtime's from ar_attach() to ar_detach() */
		zsfx_music_play(&mj_song);
	}
	if (full && !ar_set_fullscreen(true))
		puts("mahjong: this bitstream has no game mode");
	/* composed by mj_face_check(), before the next frame */
}

static void screen_off(void)
{
	want_desktop = false;
	if (!scr) return;
	ar_detach();        /* and the mixer with it: zsfx_shutdown() */
	scr = false;
	mj_face_end();
	cw = ch = 0;        /* the desktop canvas is set up again at REDRAW */
}

static void sparks(int t)
{
	int x, y;
	if (t < 0 || !scr) return;
	mj_tile_face(&P.v, t, &x, &y);
	ar_parts_burst(x + P.v.fw / 2, y + P.v.fh / 2, 10, MJ_C_RED, 3, 400, 22, 20);
}

/* -- the desktop face ----------------------------------------------------------- */

/* Puts canvas rectangle (x, y, w, h) on the screen, into the parts of
 * the window nothing covers. */
static void blit(int x, int y, int w, int h)
{
	z_clip_t c;
	int dx, dy, i, passes;

	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > cv.w) w = cv.w - x;
	if (y + h > cv.h) h = cv.h - y;
	if (w <= 0 || h <= 0) return;

	/* loads the window's visible region too (zwin.h) */
	z_win_content_rect(&win, &c);
	dx = c.x0 + x;
	dy = c.y0 + y;

	if (!hw_blit) {
		int px, py;
		for (py = 0; py < h; py++)
			for (px = 0; px < w; px++)
				z_fb_set_pixel(dx + px, dy + py, mj_cv_get(&cv, x + px, y + py), &c);
		return;
	}

	/* The scissor is persistent blitter state: set for every piece,
	 * reset after (bj_board.c, blit_card(), tells why). */
	passes = z_gfx_visible_count();
	if (passes < 1) passes = 1;
	for (i = 0; i < passes; i++) {
		z_clip_t r;
		r.x0 = dx; r.y0 = dy; r.x1 = dx + w - 1; r.y1 = dy + h - 1;
		if (!z_gfx_blit_scissor(i, &r)) continue;
		z_fb_hw_blit_mem(cv.plane[0], cv.stride * 4, x, y, dx, dy, w, h);
	}
	z_gfx_blit_scissor_reset();
}

/* Sets the canvas up for the window's content as it is now. True if
 * its size changed (and the table was composed again). */
static bool desk_setup(void)
{
	z_clip_t c;
	int w, h;
	z_win_content_rect(&win, &c);
	w = c.x1 - c.x0 + 1;
	h = c.y1 - c.y0 + 1;
	ox = c.x0;
	oy = c.y0;
	if (w > 640) w = 640;
	if (h > 480) h = 480;
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	if (w == cw && h == ch) return false;
	cw = w;
	ch = h;
	mj_cv_setup(&cv, mj_cvmem, MJ_CV_WORDS, cw, ch, 0, mj_inks, mj_ink_map);
	P.v.rich = false;
	mj_face_layout(&P, false, cw, ch);
	mj_render_all(&P.v, &cv);
	return true;
}

static void desk_paint(void)
{
	desk_setup();
	blit(0, 0, cv.w, cv.h);
}

/* -- what changed, onto the screen ----------------------------------------------- */

static void show(int x, int y, int w, int h)
{
	if (scr) ar_dirty(x, y, w, h);
	/* Just back from the screen face, the canvas is still the screen's
	 * until wm's REDRAW sets the desktop's up (cw is 0 until then):
	 * nothing to copy yet, and the REDRAW paints it all anyway. */
	else if (cw) blit(x, y, w, h);
}

static void flush(void)
{
	if (scr && !mj_face_ready) {
		/* not composed yet (mj_face_check()): it will be, whole */
		P.nrect = 0;
		P.all = P.relayout = P.bar = false;
		P.sdx = P.sdy = 0;
	} else {
		mj_face_flush(&P, scr, show);
	}

	if (P.say[0]) z_speak(P.say, Z_TTS_F_INTERRUPT);
	if (scr && P.sound > 0 && P.sound < mj_nfx)
		zsfx_play(&mj_fx[P.sound], 0);
	if (scr) {
		sparks(P.burst[0]);
		sparks(P.burst[1]);
	}
	if (P.save) save_stats();

	P.save = false;
	P.say[0] = 0;
	P.sound = 0;
	P.burst[0] = P.burst[1] = -1;
}

/* -- input ------------------------------------------------------------------------ */

static void help_on_msg(z_msg_t *msg, void *user)
{
	(void)user;
	if (msg->subject == Z_WM_SET_CLIP) {
		z_win_apply_clip(&win, &msg->obj);
	} else if (msg->subject == Z_WM_REDRAW && msg->obj.type == Z_UINT32 &&
		   z_win_redraw_id(msg->obj.val.uint32) == win.id) {
		z_win_apply_redraw(&win, msg->obj.val.uint32);
		desk_paint();
		z_win_redraw_done(&win);
	}
}

static void show_help(void)
{
	z_dialog_ctx_t ctx = { &win, help_on_msg, NULL };
	z_dialog_help(&ctx, "Mahjong help", help_text, "mahjong_app");
	desk_paint();
}

static void key(uint32_t sym, bool pressed, int mods)
{
	if (!pressed) return;
	if (!scr) {
		/* the desktop face's; on the screen face the runtime's */
		if (sym == Z_KEY_FULL_SCREEN) { screen_on(true); return; }
		if (sym == Z_KEY_COLOUR) { screen_on(false); return; }
		if (sym == Z_KEY_HELP) { show_help(); return; }
	}
	mj_play_key(&P, sym, mods, new_seed());
	flush();
}

static void mouse(int x, int y, bool down)
{
	static bool was;
	if (down && !was) {
		mj_play_click(&P, x, y, new_seed());
		flush();
	}
	was = down;
}

/* A gamepad on the screen face, as keys: the stick moves the cursor,
 * A picks up and takes, B puts back, X hints, Y undoes, Start shuffles,
 * Select deals again. The keyboard's own presses arrive as AR_EV_KEY
 * as well, so only the pad's (source 1) are taken here. */
static void pad(uint32_t button)
{
	static const struct { uint32_t b, key; } map[] = {
		{ AR_BTN_LEFT, Z_KEY_LEFT }, { AR_BTN_RIGHT, Z_KEY_RIGHT },
		{ AR_BTN_UP, Z_KEY_UP }, { AR_BTN_DOWN, Z_KEY_DOWN },
		{ AR_BTN_A, '\r' }, { AR_BTN_B, 0x1b }, { AR_BTN_X, 'h' },
		{ AR_BTN_Y, 'u' }, { AR_BTN_START, 's' }, { AR_BTN_SELECT, 'n' },
	};
	unsigned i;
	for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (button == map[i].b) {
			mj_play_key(&P, map[i].key, 0, new_seed());
			flush();
			return;
		}
}

/* Shift, Ctrl or Alt held while the wheel turns: across. A held
 * modifier sends no key events, so the keyboard is asked directly
 * (zkbd.h) -- as wm's Alt+double-click and gpu3d's Alt+drag do. Super
 * is wm's (the viewport). */
static bool across(void)
{
	return (z_kbd_live_mods() &
		(Z_KBD_MOD_SHIFT | Z_KBD_MOD_CTRL | Z_KBD_MOD_LALT | Z_KBD_MOD_RALT)) != 0;
}

static void pump(void)
{
	z_msg_t msg;

	if (scr) {
		ar_event_t e;
		while (ar_poll(&e)) {
			switch (e.type) {
			case AR_EV_KEY:
				key(e.key, e.pressed, e.mods);
				break;
			case AR_EV_BUTTON:
				if (e.pressed && e.source == 1) pad(e.key);
				break;
			case AR_EV_MOUSE:
				mouse_x = e.inside ? e.x : -1;
				mouse_y = e.y;
				mouse(e.x, e.y, (e.buttons & 1) != 0);
				break;
			case AR_EV_WHEEL:
				mj_play_wheel(&P, e.notches, across());
				flush();
				break;
			case AR_EV_MODE:
				/* recomposed once, by mj_face_check(), if the planes
				 * changed; the palette is cheap */
				ar_pal_load(mj_palette, MJ_C_COLOURS);
				break;
			case AR_EV_QUIT:
				running = false;
				break;
			case AR_EV_DESKTOP:
				want_desktop = true;
				break;
			default:
				break;
			}
		}
		return;
	}

	while (z_msg_read(&msg) == Z_OK) {
		switch (msg.subject) {
		case Z_WM_KEY:
			if (msg.obj.type == Z_UINT32)
				key(Z_WM_UNPACK_KEY_KEYSYM(msg.obj.val.uint32),
				    Z_WM_UNPACK_KEY_PRESSED(msg.obj.val.uint32) != 0,
				    (int)Z_WM_UNPACK_KEY_MODIFIERS(msg.obj.val.uint32));
			break;
		case Z_WM_MOUSE:
			if (msg.obj.type == Z_UINT32) {
				uint32_t u = msg.obj.val.uint32;
				mouse((int)Z_WM_UNPACK_MOUSE_X(u) - ox,
				      (int)Z_WM_UNPACK_MOUSE_Y(u) - oy,
				      (Z_WM_UNPACK_MOUSE_BUTTONS(u) & Z_MOUSE_BTN_LEFT) != 0);
			}
			break;
		case Z_WM_WHEEL:
			if (msg.obj.type == Z_UINT32) {
				mj_play_wheel(&P, Z_WM_WHEEL_NOTCHES(msg.obj.val.uint32), across());
				flush();
			}
			break;
		case Z_WM_TITLEBAR_ICON:
			if (msg.obj.type == Z_UINT32 &&
			    Z_WM_UNPACK_TBICON_KIND(msg.obj.val.uint32) == Z_WM_TBICON_COLOUR)
				screen_on(false);
			break;
		case Z_WM_SET_CLIP:
			if (!z_win_apply_clip(&win, &msg.obj))
				puts("mahjong: bad clip region message");
			break;
		case Z_WM_REDRAW:
			if (msg.obj.type != Z_UINT32) break;
			if (z_win_redraw_id(msg.obj.val.uint32) != win.id) break;
			z_win_apply_redraw(&win, msg.obj.val.uint32);
			desk_paint();
			z_win_redraw_done(&win);
			break;
		case Z_WM_WINDOW_MOVED:
			z_win_parse_rect(&win, &msg.obj);
			{
				z_clip_t c;
				z_win_content_rect(&win, &c);
				ox = c.x0;
				oy = c.y0;
			}
			break;
		case Z_WM_WINDOW_RESIZED:
			if (z_win_apply_resized(&win, &msg.obj))
				desk_paint();
			break;
		case Z_WM_CLOSE:
			running = false;
			break;
		default:
			break;
		}
	}
}

/* -- startup -------------------------------------------------------------------------- */

static void build_help(void)
{
	snprintf(help_text, sizeof(help_text), "%s", mj_play_help());
}

int main(void)
{
	char arg[Z_WM_ARG_MAX];
	int i;

	puts("mahjong: starting");
	zg_rng_use_system();

	mj_play_init(&P, layout_src, NULL);
	list_layouts();
	load_stats();

	/* `run mahjong Fortress` starts on that layout. Claimed whether
	 * or not it is used (blackjack.c says why). */
	if (z_launch_arg_take(arg, sizeof(arg)) && arg[0])
		snprintf(P.want, sizeof(P.want), "%.23s", arg);

	i = mj_play_find_layout(&P, P.want);
	if (i < 0) i = 0;
	P.now = now_s();
	if (!mj_play_layout(&P, i, new_seed()) && i != 0)
		mj_play_layout(&P, 0, new_seed());
	P.save = false;

	build_help();
	memcpy(desc.palette, mj_palette, sizeof(desc.palette));
	memcpy(desc.ink, mj_inks, sizeof(desc.ink));
	ar_window_colour(true);
	hw_blit = z_fb_hw_blit_mem_available();

	if (z_win_create_flags(&win, "mahjong", WIN_W, WIN_H, -1, -1,
	    Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER |
	    Z_WIN_FLAG_RESIZABLE | ar_window_flags(&desc)) != Z_OK) {
		puts("mahjong: failed to create window -- is wm running?");
		return 1;
	}

	desk_paint();
	if (P.v.msg[0] == 0) {
		snprintf(P.v.msg, sizeof(P.v.msg), "F1 for help -- F10 for colour");
		P.msg_until = P.now + 6;
		P.bar = true;
	}
	flush();

	while (running) {
		pump();
		if (!running) break;
		if (want_desktop) {
			screen_off();
			continue;
		}

		mj_play_tick(&P, now_s());
		if (scr) {
			mj_face_check(&P);
			if (mouse_x >= 0) mj_play_hover(&P, mouse_x, mouse_y);
			mj_face_pulse(&P, frames);
		}
		flush();

		if (scr) {
			ar_frame();
			frames++;
		} else {
			z_proc_wait(Z_TICK_HZ / 4);
		}
	}

	if (P.counted) save_stats();
	screen_off();
	z_win_destroy(&win);
	puts("mahjong: bye");
	return 0;
}
