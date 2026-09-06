/*
 * chip8 -- CHIP-8 / SUPER-CHIP / XO-CHIP emulator.
 *
 * This file is the only one in the app that knows about the OS. The
 * guest machine (core.c) and the display expansion (render.c) both
 * include nothing from sw/common and are tested on a host; what is
 * left here is the window, the input, the pacing and the blit.
 *
 * See docs/chip8_app.md.
 *
 *   > run wm
 *   > run chip8              -- opens a file picker
 *
 * or launch it from `files` with a .ch8 selected, which arrives
 * through wm's launch-argument slot (Z_WM_SET_ARG in zwm.h).
 *
 * Keys:
 *
 *   1 2 3 4        1 2 3 C
 *   Q W E R   ->   4 5 6 D      the guest's hex keypad
 *   A S D F        7 8 9 E
 *   Z X C V        A 0 B F
 *
 *   SPACE and ENTER alias the gamepad's A and START buttons, so they
 *   follow whatever `pad a=` and `pad start=` say for this ROM.
 *
 *   F1/F2/F3   scale 1x / 2x / 4x
 *   F4         cycle compatibility profile (resets)
 *   F5         reset
 *   F6         full-screen game mode
 *   F7/F8      slower / faster
 *   F9         screenshot to CHIP8SS.ZBM
 *   F10        debugger pane
 *   F11        single step        F12  run / pause
 *   Shift+F1   cycle the XO-CHIP grey mapping
 *   ESC        leave game mode, or quit
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zgfx.h"
#include "../../common/zfont.h"
#include "../../common/zkbd.h"
#include "../../common/zpad.h"
#include "../../common/zdialog.h"
#include "../../common/zfsapp.h"

#include "../../common/zbm.h"

#include "core.h"
#include "render.h"
#include "sound.h"
#include "config.h"
#include "debug.h"

/* -- state ---------------------------------------------------------- */

static c8_t vm;
static c8_render_t rnd;
static z_win_t win;

static int  profile = C8_PROFILE_CHIP8;
static int  ipf;                    /* guest instructions per frame */
static bool quit;
static bool game_mode;
static bool need_full_redraw = true;

static bool dbg_on;                 /* the debugger pane is shown */
static bool paused;
static bool step_pending;

static c8_config_t cfg;

/* Chrome overhead: window size minus content size. Measured once from
 * a real window rather than assumed, because it is wm's business and
 * has changed before -- see z_win_content_rect()'s own comment in
 * zwin.h about two callers keeping their own copy of that formula and
 * one of them going stale. */
static int chrome_w, chrome_h;
static bool chrome_known;

/* Where the guest image sits inside the content area. Non-zero only
 * when the window ended up bigger than the image, which happens when
 * wm imposes a minimum size. */
static int img_x, img_y;

static char rom_path[Z_WM_ARG_MAX];
static char rom_name[48];
static char rom_dir[Z_WM_ARG_MAX];
static char flags_path[Z_WM_ARG_MAX];

/* Last RPL flag block written to disk, so a save only happens when the
 * guest actually changed something. Sixteen bytes to compare against a
 * filesystem write on a bit-banged SD card is not a close call. */
static uint8_t flags_saved[C8_FLAGS];   /* what is on disk */
static uint8_t flags_last[C8_FLAGS];    /* what the guest had last frame */
static int flags_settle;
static bool flags_have_file;

/* Frames of no further change before the flags are written.
 *
 * A game that saves does so in a burst -- FX75 writes at most sixteen
 * registers, so a sixteen-flag save is up to sixteen separate
 * instructions and, with a low tickrate, several frames. Writing on
 * every one of those would be several full file writes to a
 * bit-banged SD card for one logical save, during gameplay. Half a
 * second of quiet is far longer than any burst and far shorter than a
 * player notices. */
#define C8_FLAGS_SETTLE 30

/* -- keypad --------------------------------------------------------- */

/* The conventional CHIP-8 layout. Indexed by GUEST key value, giving
 * the host key that produces it, in both the forms this app needs to
 * recognise it in.
 *
 * Two tables and not one because the two input paths ask different
 * questions. A windowed app is told "the key that types 'w' went
 * down"; a full-screen one reads the USB report and asks "is the
 * physical W key held". Neither can answer the other's question:
 * a keysym is layout-resolved and has no repeat-free level, and a
 * usage code is not a character. gamedemo.c makes the same split for
 * the same reason. */
static const char hex_keysym[16] = {
	'x', '1', '2', '3',
	'q', 'w', 'e', 'a',
	's', 'd', 'z', 'c',
	'4', 'r', 'f', 'v'
};

static const uint8_t hex_usage[16] = {
	0x1B, 0x1E, 0x1F, 0x20,     /* x 1 2 3 */
	0x14, 0x1A, 0x08, 0x04,     /* q w e a */
	0x16, 0x07, 0x1D, 0x06,     /* s d z c */
	0x21, 0x15, 0x09, 0x19      /* 4 r f v */
};

/* Gamepad, for game mode. The d-pad covers both of the two movement
 * conventions CHIP-8 games actually use -- 2/4/6/8 and Q/E -- because
 * a pad mapped to only one of them is useless for half the catalogue.
 * Phase 6 makes this per-ROM; until then this is the default that
 * works for the most titles. */
static const uint32_t pad_mask[C8_PAD_COUNT] = {
	Z_PAD_UP, Z_PAD_DOWN, Z_PAD_LEFT, Z_PAD_RIGHT,
	Z_PAD_A, Z_PAD_B, Z_PAD_X, Z_PAD_Y,
	Z_PAD_START, Z_PAD_SELECT
};

/* Overwritten per ROM from CHIP8.CFG -- see config.h. Not const for
 * that reason. */
static uint8_t pad_key[C8_PAD_COUNT] = {
	0x2, 0x8, 0x4, 0x6,
	0x5, 0x0, 0x1, 0x3,
	0xF, 0xE
};

static int keysym_to_hex(uint32_t keysym) {

	int i;

	if (keysym >= 'A' && keysym <= 'Z') keysym = keysym - 'A' + 'a';

	/* SPACE and ENTER are not on the hex keypad, and a machine with a
	 * keyboard should still do something sensible when you press the
	 * obvious "fire" key. They alias the GAMEPAD's A and START rather
	 * than fixed hex values, so one `pad a=` line in CHIP8.CFG moves
	 * the pad button and the space bar together -- which is what
	 * somebody editing that line means, and it avoids a second
	 * mapping that could drift out of step with the first.
	 *
	 * Default A is 5: the centre of the QWER/ASDF block and the key
	 * most Octo titles use to act. */
	if (keysym == ' ')  return pad_key[C8_PAD_A] & 0xF;
	if (keysym == '\r' || keysym == '\n') return pad_key[C8_PAD_START] & 0xF;

	for (i = 0; i < 16; i++)
		if ((uint32_t)hex_keysym[i] == keysym) return i;

	return -1;

}

/* Is this HID usage in either port's current report? Same shape as
 * gamedemo's kbd_held() -- the report is a level, which is exactly
 * what CHIP-8's EX9E/EXA1 want. */
static bool usage_held(uint8_t usage) {
	int port;
	for (port = 0; port < 2; port++) {
		uint32_t keys = port ? reg_usb1_keys : reg_usb0_keys;
		if ((uint8_t)(keys >> 24) == usage) return true;
		if ((uint8_t)(keys >> 16) == usage) return true;
		if ((uint8_t)(keys >> 8)  == usage) return true;
		if ((uint8_t)(keys)       == usage) return true;
	}
	return false;
}

/* Clear any guest key we believe is down that the hardware says is not.
 *
 * wm only sends key events to the FOCUSED window, so if focus moves
 * while a key is held, the release is delivered to somebody else and
 * this app believes the key is still down forever. On a CHIP-8 that is
 * not a cosmetic glitch: the guest reads keys as a level, so the
 * player's character runs into a wall and stays there.
 *
 * Only ever CLEARS. Keys are still set from wm's events alone, so this
 * cannot make an unfocused window respond to typing -- which is the
 * thing the focus rule exists to prevent. */
static void reconcile_keys(void) {
	int k;
	if (vm.keys == 0) return;
	for (k = 0; k < 16; k++) {
		if (!(vm.keys & (1u << k))) continue;
		if (usage_held(hex_usage[k])) continue;
		/* A guest key can also be held via SPACE or ENTER, which are
		 * not that key's own usage code. Miss this and the alias
		 * releases itself a frame after it is pressed. */
		if ((pad_key[C8_PAD_A] & 0xF) == k && usage_held(0x2C)) continue;
		if ((pad_key[C8_PAD_START] & 0xF) == k && usage_held(0x28)) continue;
		c8_key(&vm, k, false);
	}
}

/* -- frame clock ----------------------------------------------------
 *
 * The guest's timers and its display wait are both specified in
 * DISPLAY frames, so the display's own counter is the right clock:
 * reading it makes them exact under scheduler jitter rather than
 * approximately right.
 *
 * That counter only exists on a bitstream with game mode, and reads a
 * hardwired zero otherwise -- which would freeze the emulator rather
 * than run it slowly, so the fallback is not optional. The kernel tick
 * is 732Hz, and 60/732 is 5/61 exactly. */
static bool have_frame_counter;

static uint32_t frame_now(void) {
	if (have_frame_counter) return z_game_frame();
	return (z_uptime_ticks() * 5u) / 61u;
}

/* -- window --------------------------------------------------------- */

static void set_title(void) {
	char t[64];
	snprintf(t, sizeof(t), "chip8 %s %dx [%s]",
		rom_name, rnd.scale, c8_profile_name((c8_profile_t)profile));
	z_win_set_title(&win, t);
}

static void on_msg(z_msg_t *msg, void *user);
static bool load_rom(const char *path);
static void load_config(void);
static void set_flags_path(void);
static void reset_machine(void);
static void save_flags(bool force);

/* Content size the window needs: the guest image, plus the debugger
 * pane below it when it is showing. The pane is wider than the image
 * at 1x, which is why the width is a max rather than just the image. */
static int content_w(void) {
	int w = rnd.w;
	if (dbg_on && C8_DBG_W > w) w = C8_DBG_W;
	return w;
}

static int content_h(void) {
	return rnd.h + (dbg_on ? C8_DBG_H : 0);
}

static bool open_window(void) {

	int want_w = content_w() + chrome_w;
	int want_h = content_h() + chrome_h;

	/* Z_WIN_FLAG_CLOSE_ICON WITHOUT Z_WIN_FLAG_CLOSE_KILLS_OWNER.
	 *
	 * The killing form is the usual choice for a single-window app and
	 * is wrong here: this process owns a hardware mixer channel that
	 * is a bus master reading a buffer in its own address space. Being
	 * killed skips c8_sound_shutdown(), so the channel keeps fetching
	 * from memory that has just been freed with the process. Handling
	 * Z_WM_CLOSE ourselves costs one case in the message loop and
	 * makes shutdown ordered. */
	/* Z_WIN_FLAG_OPEN_ICON puts a titlebar icon there; wm draws it and
	 * reports the click as Z_WM_TITLEBAR_ICON, and what it MEANS is
	 * this app's business. Loading a second ROM without going back to
	 * the shell is the obvious thing an emulator should offer, and
	 * there was no way to do it at all before. */
	if (z_win_create_cb(&win, "chip8", (uint32_t)want_w, (uint32_t)want_h,
		-1, -1, Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_OPEN_ICON,
		on_msg, NULL) != Z_OK)
		return false;

	if (!chrome_known) {

		chrome_w = (int)win.w - z_win_content_w(&win);
		chrome_h = (int)win.h - z_win_content_h(&win);
		chrome_known = true;

		/* The first window of the session was asked for at the image's
		 * size with no allowance for the frame, because the frame's
		 * size is wm's business and is only knowable from a real
		 * window. So it came back with a content area SMALLER than the
		 * image and the picture was cropped by exactly the border.
		 *
		 * Now that the frame has been measured, ask again for the size
		 * that gives the image its own pixels. Recursion is one level
		 * deep and cannot be more: chrome_known is set above, so the
		 * second call takes neither this branch nor another
		 * destroy. */
		if (chrome_w != 0 || chrome_h != 0) {
			z_win_destroy(&win);
			return open_window();
		}

	}

	img_x = (z_win_content_w(&win) - rnd.w) / 2;
	img_y = dbg_on ? 0 : (z_win_content_h(&win) - rnd.h) / 2;
	if (img_x < 0) img_x = 0;
	if (img_y < 0) img_y = 0;

	set_title();
	need_full_redraw = true;

	return true;

}

/* Rebuild the window at the current scale.
 *
 * Two creations at startup rather than a guess: the first one measures
 * the chrome, the second asks for exactly the size that gives the
 * image its own pixels. Sizing by guess leaves the image either
 * cropped or floating in a border, and which one it is changes
 * whenever wm's frame does. */
static void resize_window(void) {
	z_win_destroy(&win);
	/* The pane's cache describes pixels in a window that is about to
	 * stop existing. */
	c8_debug_invalidate();
	open_window();
}

/* -- drawing -------------------------------------------------------- */

static void blit_windowed(bool full) {

	z_clip_t cc;
	int i, n;
	int y0 = full ? 0 : rnd.dirty_y0;
	int y1 = full ? rnd.h : rnd.dirty_y1;

	if (y1 <= y0) return;

	/* Loads this window's visible region into zgfx as a side effect,
	 * which is what the scissor loop below then reads. */
	z_win_content_rect(&win, &cc);

	n = z_gfx_visible_count();
	if (n == 0) n = 1;

	for (i = 0; i < n; i++) {
		if (!z_gfx_blit_scissor(i, &cc)) continue;
		z_fb_hw_blit_mem(rnd.buf, rnd.stride, 0, y0,
			cc.x0 + img_x, cc.y0 + img_y + y0, rnd.w, y1 - y0);
	}

	z_gfx_blit_scissor_reset();

}

static void draw_panel(bool force) {
	if (!dbg_on || game_mode) return;
	c8_debug_draw(&win, 0, rnd.h, &vm, paused, ipf,
		c8_profile_name((c8_profile_t)profile), force);
}

/* Game mode: two pages side by side, 320 columns each, viewport at the
 * top of whichever is not on screen. Nothing scrolls, so this needs
 * none of zgame.h's camera machinery -- just the flip, whose ORDER is
 * the part worth getting right: point the viewport at the page just
 * drawn, then wait for the boundary at which the hardware adopts it.
 * A flip cannot tear because the origin is only ever adopted between
 * frames. */
#define GAME_PAGE_W 320

static int game_back;

/* Whether the exit key may act yet.
 *
 * Game mode is entered BY a keypress, and it is read as a level from
 * the USB report once inside -- so on the first poll after entering,
 * the very key that got us here is still physically down, and game
 * mode ends in the same frame it began. What that looks like from the
 * outside is F6 clearing the screen and doing nothing else, which is
 * exactly the symptom reported.
 *
 * So the exit key has to be RELEASED once before it counts. Not a
 * timeout, which would be a guess about how fast somebody lets go of a
 * key, and would still fail for anyone who held it a moment longer. */
static bool exit_armed;

static void blit_game(void) {

	int px = game_back * GAME_PAGE_W;
	int ox = px + (GAME_PAGE_W - rnd.w) / 2;
	int oy = (Z_GAME_VIEW_H - rnd.h) / 2;

	z_gfx_clear_visible();
	z_gfx_blit_scissor_reset();

	/* Whole image every frame rather than the dirty band. The two
	 * pages are drawn on alternate frames, so a band that is current
	 * for one is a frame stale for the other -- tracking that costs
	 * more than 4KB of blit at 2x.
	 *
	 * No per-frame clear of the surround. The image rect is fixed for
	 * as long as game mode lasts -- scale is forced to 2 and the
	 * output is 128*scale by 64*scale in BOTH guest resolutions, so a
	 * ROM switching to hires does not move or resize it -- which
	 * means the border is drawn once, by enter_game_mode(), and never
	 * needs redrawing. */
	z_fb_hw_blit_mem(rnd.buf, rnd.stride, 0, 0, ox, oy, rnd.w, rnd.h);

	z_game_set_view((uint32_t)px, 0);
	z_game_wait_frame();

	game_back ^= 1;

}

static void enter_game_mode(void) {

	if (!z_game_available()) {
		printf("chip8: this bitstream has no game mode.\n");
		printf("chip8: rebuild the gateware with `GAME in rtl/boards.vh.\n");
		return;
	}

	/* 2x is not a preference here, it is the only scale that fits: the
	 * viewport is 320x240 framebuffer pixels and 4x would be 512x256.
	 * The hardware then doubles it, so 2x reaches the screen as
	 * 512x256 physical -- twice the size of the desktop presentation,
	 * with the 4x4 dither cell intact. */
	c8_render_set_scale(&rnd, 2);

	game_mode = true;
	game_back = 0;
	exit_armed = false;

	/* Clears BOTH pages, which is why blit_game() does not have to.
	 * Done before the mode change so the first frame scanned out in
	 * game mode is already black rather than whatever the desktop
	 * left there. */
	z_gfx_clear_visible();
	z_gfx_blit_scissor_reset();
	z_fb_hw_fill_rect(0, 0, 640, 480, 0);

	z_game_set_enabled(true, false);

	need_full_redraw = true;

}

static void leave_game_mode(void) {

	uint32_t wm_pid;

	z_game_set_enabled(false, false);
	game_mode = false;
	c8_debug_invalidate();

	/* Hand the framebuffer back. Every window is still alive and still
	 * where it was, but this app drew over all of their pixels and
	 * none of them know. wm repairs damage it caused itself; this came
	 * from outside, so it has to be told. See Z_WM_REPAINT in zwm.h. */
	if (z_pid_lookup("wm0", &wm_pid))
		z_msg_new_send(wm_pid, Z_WM_REPAINT, 0, z_obj_uint32(0));

	need_full_redraw = true;

}

/* -- machine control ------------------------------------------------ */

static int default_ipf(int prof) {
	switch (prof) {
	case C8_PROFILE_SCHIP:  return 30;
	case C8_PROFILE_XOCHIP: return 200;
	default:                return 15;
	}
}

static uint8_t rom_image[C8_RAM_SIZE];
static uint32_t rom_len;

/* -- RPL flags ------------------------------------------------------
 *
 * FX75/FX85 are persistent storage from the guest's point of view: on
 * a real HP48 they survived the calculator being switched off, and
 * SUPER-CHIP games use them for high scores. A copy that lives only in
 * RAM makes them work within a session and lose everything on exit,
 * which is the shape of bug that looks like "high scores do not save"
 * rather than like an unimplemented feature.
 *
 * Stored alongside the ROM with the extension replaced, so the save
 * travels with the game and a whole ROM directory can be copied
 * without losing anything. */
static void set_flags_path(void) {

	int n = 0, dot = -1;

	while (rom_path[n] && n < (int)sizeof(flags_path) - 5) {
		if (rom_path[n] == '.') dot = n;
		if (rom_path[n] == '/') dot = -1;
		flags_path[n] = rom_path[n];
		n++;
	}

	if (dot >= 0) n = dot;

	flags_path[n++] = '.';
	flags_path[n++] = 'F';
	flags_path[n++] = 'L';
	flags_path[n++] = 'G';
	flags_path[n] = '\0';

}

static void load_flags(void) {

	int size = fs_size(flags_path);
	char *buf;

	flags_have_file = false;
	memset(flags_saved, 0, sizeof(flags_saved));

	if (size <= 0) return;

	buf = fs_mallocfile(flags_path);
	if (!buf) return;

	if (size > (int)sizeof(flags_saved)) size = (int)sizeof(flags_saved);
	memcpy(flags_saved, buf, (size_t)size);
	memcpy(vm.flags, flags_saved, sizeof(vm.flags));
	memcpy(flags_last, flags_saved, sizeof(flags_last));
	free(buf);

	flags_have_file = true;

}

/* Called once a frame, and once more with force at shutdown.
 *
 * Sixteen bytes compared against a write to a bit-banged SD card is
 * not a close call, so the comparison is unconditional and the write
 * is not.
 *
 * The result is announced. "C to erase save file" is a thing games
 * offer, and without a line here there is no way to tell whether it
 * did anything: an erase is the guest writing zeros, which looks
 * exactly like a game that ignored the keypress. */
static void save_flags(bool force) {

	int n;

	if (memcmp(vm.flags, flags_last, sizeof(flags_last)) != 0) {
		memcpy(flags_last, vm.flags, sizeof(flags_last));
		flags_settle = 0;
	}

	if (memcmp(vm.flags, flags_saved, sizeof(flags_saved)) == 0) return;

	if (!force && flags_settle < C8_FLAGS_SETTLE) {
		flags_settle++;
		return;
	}

	memcpy(flags_saved, vm.flags, sizeof(flags_saved));

	if (fs_write_file(flags_path, (char *)flags_saved,
		(int)sizeof(flags_saved)) == (int)sizeof(flags_saved)) {
		flags_have_file = true;
		for (n = 0; n < (int)sizeof(flags_saved); n++)
			if (flags_saved[n]) break;
		printf("chip8: %s %s\n",
			n == (int)sizeof(flags_saved) ? "cleared" : "saved",
			flags_path);
	} else {
		printf("chip8: could not write %s\n", flags_path);
	}

}

/* A path in the ROM's own directory.
 *
 * Length-checked rather than snprintf-truncated: a silently shortened
 * path does not fail, it names a DIFFERENT file, and for the flags
 * save that means one game's high scores landing in another's. */
static bool sibling_path(char *out, int outlen, const char *leaf) {

	int dn = (int)strlen(rom_dir);
	int ln = (int)strlen(leaf);

	if (dn + ln + 1 > outlen) return false;

	memcpy(out, rom_dir, (size_t)dn);
	memcpy(out + dn, leaf, (size_t)ln + 1);

	return true;

}

/* -- screenshot -----------------------------------------------------
 *
 * ZBM (zbm.h), because the renderer's output is ALREADY in exactly
 * that pixel format -- the framebuffer's own, LSB leftmost -- so a
 * screenshot is a 16-byte header and the buffer, with no conversion
 * anywhere. It also means `draw` and `view` can open it. */
static void screenshot(void) {

	char path[Z_WM_ARG_MAX];
	z_bm_header_t hdr;
	int h;

	if (!sibling_path(path, (int)sizeof(path), "CHIP8SS.ZBM")) {
		printf("chip8: ROM path too long for a screenshot beside it\n");
		return;
	}

	z_bm_header_init(&hdr, (uint32_t)rnd.w, (uint32_t)rnd.h);

	h = fs_open_write(path);
	if (h < 0) {
		printf("chip8: could not write %s\n", path);
		return;
	}

	/* The stride the renderer uses is exactly ((w + 31) / 32) * 4 for
	 * every scale it supports, so the buffer is contiguous in ZBM's
	 * layout and goes out in one write rather than row by row. */
	fs_write_chunk(h, &hdr, (int)sizeof(hdr));
	fs_write_chunk(h, rnd.buf, rnd.stride * rnd.h);
	fs_close_handle(h);

	printf("chip8: wrote %s (%dx%d)\n", path, rnd.w, rnd.h);

}

/* -- per-ROM configuration ------------------------------------------
 *
 * See config.h for the file format and why it is one file per
 * directory. */
static void load_config(void) {

	char path[Z_WM_ARG_MAX];
	char *text;
	int i, sz;

	c8_config_defaults(&cfg);

	if (!sibling_path(path, (int)sizeof(path), "CHIP8.CFG")) return;

	sz = fs_size(path);
	if (sz <= 0) return;

	/* Bounded, because this one IS read through malloc() and a
	 * process's heap is its stack+heap allowance -- 16KB by default
	 * (sw/os/kernel.h). A config file is a few lines per ROM, so 8KB
	 * is a large pack; anything past that is a mistake worth naming
	 * rather than a quiet allocation failure. */
	if (sz > 8192) {
		printf("chip8: %s is %d bytes, too large to read; ignoring\n",
			path, sz);
		return;
	}

	text = fs_mallocfile(path);
	if (!text) return;

	c8_config_parse(&cfg, text, rom_name);
	free(text);

	if (!cfg.found) return;

	if (cfg.profile >= 0) profile = cfg.profile;

	for (i = 0; i < C8_PAD_COUNT; i++)
		if (cfg.pad[i] != 0xFF) pad_key[i] = cfg.pad[i];

	if (cfg.name[0])
		snprintf(rom_name, sizeof(rom_name), "%s", cfg.name);

	if (cfg.palette >= 0) c8_render_set_palette(&rnd, cfg.palette);

	/* Said out loud rather than ignored. A typo in a quirk name would
	 * otherwise present as "this ROM still misbehaves", with a config
	 * file that looks correct. */
	if (cfg.bad_lines)
		printf("chip8: %s: %d unparsable line%s, first at line %d\n",
			path, cfg.bad_lines, cfg.bad_lines == 1 ? "" : "s",
			cfg.first_bad_line);

}

static void reset_machine(void) {

	c8_quirks_t q = *c8_profile((c8_profile_t)profile);

	/* Config overrides go on TOP of the profile, which is why they are
	 * applied here and not folded into the profile table: F4 cycles
	 * the base profile at run time and the ROM's own overrides must
	 * survive that. */
	c8_config_apply(&cfg, &q);

	c8_init(&vm, &q, z_uptime_ticks() | 1u);

	if (!c8_load(&vm, rom_image, rom_len))
		printf("chip8: ROM does not fit this profile's address space\n");

	ipf = cfg.speed ? cfg.speed : default_ipf(profile);

	load_flags();

	paused = false;
	step_pending = false;
	need_full_redraw = true;

	set_title();

}

/* -- messages ------------------------------------------------------- */

static void handle_key(uint32_t packed) {

	uint32_t keysym = Z_WM_UNPACK_KEY_KEYSYM(packed);
	uint32_t mods = Z_WM_UNPACK_KEY_MODIFIERS(packed);
	bool pressed = Z_WM_UNPACK_KEY_PRESSED(packed) != 0;
	int k;

	if (!pressed) {
		k = keysym_to_hex(keysym);
		if (k >= 0) c8_key(&vm, k, false);
		return;
	}

	/* Shift+F1 cycles the XO-CHIP grey mapping.
	 *
	 * On a modifier because the twelve F-keys are spoken for, and on
	 * F1 because it sits next to the other display controls. It only
	 * does anything visible for a two-plane ROM; said out loud either
	 * way, so pressing it on a CHIP-8 game is not a silent no-op. */
	if (keysym == Z_KEY_F1 && (mods & Z_KBD_MOD_SHIFT)) {
		int p = (rnd.pal + 1) % C8_PAL_COUNT;
		c8_render_set_palette(&rnd, p);
		need_full_redraw = true;
		printf("chip8: grey mapping '%s'%s\n", c8_palette_name(p),
			vm.two_plane ? "" : " (no effect until a ROM uses plane 2)");
		return;
	}

	switch (keysym) {
	case Z_KEY_F1: case Z_KEY_F2: case Z_KEY_F3: {
		int s = (keysym == Z_KEY_F1) ? 1 : (keysym == Z_KEY_F2) ? 2 : 4;
		if (game_mode) return;          /* 2x is forced there */
		if (c8_render_set_scale(&rnd, s)) resize_window();
		return;
	}
	case Z_KEY_F4:
		profile = (profile + 1) % C8_PROFILE_COUNT;
		reset_machine();
		return;
	case Z_KEY_F5:
		reset_machine();
		return;
	case Z_KEY_F6:
		if (game_mode) leave_game_mode();
		else enter_game_mode();
		return;
	case Z_KEY_F7:
		if (ipf > 1) ipf -= (ipf > 20) ? 10 : 1;
		printf("chip8: %d instructions/frame\n", ipf);
		return;
	case Z_KEY_F8:
		ipf += (ipf >= 20) ? 10 : 1;
		printf("chip8: %d instructions/frame\n", ipf);
		return;
	case Z_KEY_F9:
		screenshot();
		return;
	case Z_KEY_F10:
		if (game_mode) return;
		dbg_on = !dbg_on;
		resize_window();
		return;
	case Z_KEY_F11:
		/* Stepping implies pausing. Pressing step on a running
		 * machine and having it execute one instruction and then
		 * thousands more is not what anybody means by it. */
		paused = true;
		step_pending = true;
		return;
	case Z_KEY_F12:
		paused = !paused;
		return;
	case 0x1B:                          /* ESC */
		if (game_mode) leave_game_mode();
		else quit = true;
		return;
	default:
		break;
	}

	k = keysym_to_hex(keysym);
	if (k >= 0) c8_key(&vm, k, true);

}

/* The titlebar's open icon: pick another ROM and start it.
 *
 * The picker is modal and pumps messages through on_msg() while it is
 * up, so this is reentrant with respect to redraws but not with
 * respect to itself -- `picking` stops a second click on the icon from
 * stacking a second dialog on the first. */
static bool picking;

static void open_another_rom(void) {

	z_dialog_ctx_t ctx;
	char path[Z_WM_ARG_MAX];

	if (game_mode || picking) return;

	/* The outgoing ROM's flags belong to the outgoing ROM. Written
	 * before anything can point flags_path at the new one. */
	save_flags(true);

	memset(&ctx, 0, sizeof(ctx));
	ctx.parent = &win;
	ctx.on_msg = on_msg;

	picking = true;
	if (!z_dialog_open(&ctx, rom_dir[0] ? rom_dir : NULL,
		path, sizeof(path))) {
		picking = false;
		need_full_redraw = true;
		return;
	}
	picking = false;

	if (!load_rom(path)) {
		need_full_redraw = true;
		return;
	}

	snprintf(rom_path, sizeof(rom_path), "%s", path);

	load_config();
	set_flags_path();
	reset_machine();

	printf("chip8: %s, %lu bytes, profile %s, %d ins/frame\n",
		rom_name, (unsigned long)rom_len,
		c8_profile_name((c8_profile_t)profile), ipf);

}

/* Shared by the main loop and by any dialog this app opens, which is
 * why it is a callback shape -- see z_dialog_ctx_t in zdialog.h. */
static void on_msg(z_msg_t *msg, void *user) {

	(void)user;

	switch (msg->subject) {

	case Z_WM_SET_CLIP:
		z_win_apply_clip(&win, &msg->obj);
		need_full_redraw = true;
		break;

	case Z_WM_REDRAW:
		z_win_apply_redraw(&win, msg->obj.val.uint32);
		if (!game_mode) {
			z_win_clear(&win);
			c8_render(&rnd, &vm, true);
			blit_windowed(true);
			draw_panel(true);
		}
		/* Acked even in game mode, where there is nothing to draw:
		 * wm blocks on this ack before letting the next window
		 * redraw, and a full-screen app that stopped answering would
		 * stall the whole desktop until wm's timeout fired -- which
		 * presents as the machine freezing for a second, not as this
		 * app being busy. */
		z_win_redraw_done(&win);
		break;

	case Z_WM_WINDOW_MOVED:
		z_win_parse_rect(&win, &msg->obj);
		/* The pane is redrawn at the new position by the redraw that
		 * follows, but the cache still describes the old one. */
		c8_debug_invalidate();
		break;

	case Z_WM_KEY:
		handle_key(msg->obj.val.uint32);
		break;

	case Z_WM_TITLEBAR_ICON:
		if (Z_WM_UNPACK_TBICON_KIND(msg->obj.val.uint32) == Z_WM_TBICON_OPEN)
			open_another_rom();
		break;

	case Z_WM_CLOSE:
		quit = true;
		break;

	default:
		break;

	}

}

static void drain_messages(void) {
	z_msg_t msg;
	while (z_msg_read(&msg) == Z_OK)
		on_msg(&msg, NULL);
}

/* -- game-mode input ------------------------------------------------
 *
 * Read as a LEVEL from the hardware, and turned into edges here rather
 * than taken as edges from wm: there is no focus in game mode and no
 * message traffic to read. The edges still matter -- FX0A completes on
 * a release -- so the previous mask is diffed against the current one
 * and c8_key() is called only where they differ. */
static void poll_game_input(void) {

	uint16_t mask = 0;
	uint32_t pad;
	int k, i;

	for (k = 0; k < 16; k++)
		if (usage_held(hex_usage[k])) mask |= (uint16_t)(1u << k);

	/* SPACE (0x2C) and ENTER (0x28), aliasing A and START exactly as
	 * the windowed path does. */
	if (usage_held(0x2C)) mask |= (uint16_t)(1u << (pad_key[C8_PAD_A] & 0xF));
	if (usage_held(0x28)) mask |= (uint16_t)(1u << (pad_key[C8_PAD_START] & 0xF));

	pad = z_pad_read(0);
	for (i = 0; i < C8_PAD_COUNT; i++)
		if (pad & pad_mask[i]) mask |= (uint16_t)(1u << (pad_key[i] & 0xF));

	for (k = 0; k < 16; k++) {
		bool now = (mask >> k) & 1;
		bool was = (vm.keys >> k) & 1;
		if (now != was) c8_key(&vm, k, now);
	}

	/* ESC (0x29) and F6 (0x3F) both leave. Read raw, since nothing is
	 * delivering messages here.
	 *
	 * Both must be seen UP once before either counts -- see
	 * exit_armed. */
	{
		bool held = usage_held(0x29) || usage_held(0x3F);
		if (!held) exit_armed = true;
		else if (exit_armed) leave_game_mode();
	}

}

/* -- ROM loading ---------------------------------------------------- */

/* Read the ROM straight into rom_image.
 *
 * NOT fs_mallocfile(), which is the obvious call and cannot work for a
 * large ROM. A process's malloc() heap comes out of its stack+heap
 * ALLOWANCE -- 16KB at Z_PROC_STACK_SIZE_DEFAULT (sw/os/kernel.h) --
 * while .bss is part of the binary image and is sized separately. So
 * rom_image, at 64KB of .bss, is free; a 65,000-byte malloc() on top
 * of a 16KB allowance cannot succeed no matter how much RAM the board
 * has. An XO-CHIP ROM near the 65,024-byte maximum failed here every
 * time, and fs_mallocfile()'s single NULL return could not say why:
 * its own header documents that NULL covers "missing", "allocation
 * failed" and "read failed" alike.
 *
 * Reading in chunks removes the second copy entirely -- there was
 * never a reason to buffer 64KB just to memcpy it into another 64KB --
 * and lets each failure say which step failed. */
static bool load_rom(const char *path) {

	int size = fs_size((char *)path);
	const char *slash;
	int h, got = 0;

	if (size <= 0) {
		printf("chip8: cannot read %s\n", path);
		return false;
	}

	if ((uint32_t)size > C8_ROM_MAX_64K) {
		printf("chip8: %s is %d bytes; the largest a CHIP-8 machine can\n",
			path, size);
		printf("chip8: address is %u (XO-CHIP, 64K minus the 512 below\n",
			(unsigned)C8_ROM_MAX_64K);
		printf("chip8: the 0x200 load address)\n");
		return false;
	}

	h = fs_open_read(path);
	if (h < 0) {
		printf("chip8: cannot open %s\n", path);
		return false;
	}

	while (got < size) {
		int n = fs_read_chunk(h, rom_image + got, size - got);
		if (n <= 0) break;
		got += n;
	}

	fs_close_handle(h);

	if (got != size) {
		printf("chip8: short read on %s: %d of %d bytes\n", path, got, size);
		return false;
	}

	rom_len = (uint32_t)size;

	/* Basename, truncated to fit the titlebar. Written out rather than
	 * done with snprintf because the truncation is INTENDED here, and
	 * a bounded copy says so where a format-truncation warning only
	 * suggests somebody forgot. */
	slash = strrchr(path, '/');

	/* Directory, WITH its trailing slash, so building a sibling path
	 * is one snprintf with no separator logic. A ROM opened by bare
	 * name has an empty directory, which concatenates to a relative
	 * path and is exactly right. */
	{
		int dn = slash ? (int)(slash - path) + 1 : 0;
		if (dn > (int)sizeof(rom_dir) - 1) dn = (int)sizeof(rom_dir) - 1;
		memcpy(rom_dir, path, (size_t)dn);
		rom_dir[dn] = '\0';
	}

	{
		const char *base = slash ? slash + 1 : path;
		size_t n = strlen(base);
		if (n >= sizeof(rom_name)) n = sizeof(rom_name) - 1;
		memcpy(rom_name, base, n);
		rom_name[n] = '\0';
	}

	profile = c8_profile_hint(rom_name, rom_len);

	/* Worth saying, because the alternative is a ROM that loads and
	 * then behaves as though it were a different machine -- which
	 * presents as a game that runs and is subtly broken. */
	if (rom_len > C8_ROM_MAX_4K)
		printf("chip8: %lu bytes is beyond any 4K machine; XO-CHIP assumed\n",
			(unsigned long)rom_len);

	return true;

}

/* -- main ----------------------------------------------------------- */

int main(void) {

	uint32_t last_frame;

	printf("chip8: CHIP-8 / SUPER-CHIP / XO-CHIP\n");

	/* The blitter's memory-copy mode is not optional here. Without it
	 * the only way to get the image on screen is a software loop over
	 * VRAM, which at 2x is 8KB of word writes every frame -- and the
	 * point of saying so plainly is that a bitstream predating that
	 * mode makes z_fb_hw_blit_mem() draw NOTHING rather than fail, so
	 * the symptom would be a black window. */
	if (!z_fb_hw_blit_mem_available()) {
		printf("chip8: this bitstream's blitter has no memory copy mode.\n");
		return 1;
	}

	have_frame_counter = z_game_present();
	if (!have_frame_counter)
		printf("chip8: no video frame counter; pacing off the kernel tick\n");

	c8_render_init(&rnd, 2);

	if (!z_launch_arg_take(rom_path, sizeof(rom_path)))
		rom_path[0] = '\0';

	/* The window has to exist before a dialog can be centred on it and
	 * before its message callback means anything, so it is created
	 * first and the picker runs after -- with a placeholder ROM in the
	 * machine so a redraw arriving mid-dialog has something to draw. */
	rom_len = 0;
	snprintf(rom_name, sizeof(rom_name), "%s", "(no rom)");
	c8_init(&vm, c8_profile(C8_PROFILE_CHIP8), 1);

	if (!open_window()) {
		printf("chip8: failed to create window (is wm running?)\n");
		return 1;
	}

	if (rom_path[0] == '\0') {
		z_dialog_ctx_t ctx;
		memset(&ctx, 0, sizeof(ctx));
		ctx.parent = &win;
		ctx.on_msg = on_msg;
		if (!z_dialog_open(&ctx, NULL, rom_path, sizeof(rom_path))) {
			z_win_destroy(&win);
			return 0;
		}
	}

	if (!load_rom(rom_path)) {
		z_win_destroy(&win);
		return 1;
	}

	/* Config BEFORE reset_machine(), because it can change the
	 * profile, the speed, the quirks and the display name, and
	 * reset_machine() consumes all four. */
	load_config();
	set_flags_path();

	reset_machine();
	c8_sound_init();

	printf("chip8: %s, %lu bytes, profile %s, %d ins/frame\n",
		rom_name, (unsigned long)rom_len,
		c8_profile_name((c8_profile_t)profile), ipf);
	printf("chip8: %s\n", c8_sound_available()
		? "sound on the hardware mixer" : "no audio on this board");
	if (cfg.found)
		printf("chip8: configuration from CHIP8.CFG applied\n");
	if (flags_have_file)
		printf("chip8: restored saved flags from %s\n", flags_path);
	printf("chip8: F1/F2/F3 scale, F4 profile, F5 reset, F6 game mode,\n");
	printf("chip8: F7/F8 speed, F9 screenshot, F10 debugger,\n");
	printf("chip8: F11 step, F12 run/pause, shift+F1 greys, ESC quits.\n");

	last_frame = frame_now();

	while (!quit) {

		uint32_t now, elapsed;

		/* Messages are drained in BOTH modes. The window still
		 * exists in game mode and wm still talks to it; ignoring the
		 * queue there would leave wm waiting on redraw acks. Only the
		 * INPUT source differs. */
		drain_messages();

		if (game_mode) poll_game_input();
		else reconcile_keys();

		now = frame_now();
		elapsed = now - last_frame;

		/* Paused, and nothing has changed: still yield rather than
		 * spin. The pane is redrawn below when a frame ticks over,
		 * which is often enough to see a stepped instruction. */
		if (elapsed == 0) {
			/* Nothing to do until the display moves on. Yielding here
			 * rather than spinning is what stops this app taking a
			 * full scheduler share from everything behind it -- see
			 * docs/app_runtime.md. In game mode the flip below already
			 * blocks on the frame boundary, so there is nothing to
			 * wait for here. */
			if (!game_mode) z_proc_wait(1);
			continue;
		}

		/* Cap catch-up. A machine that has been descheduled for a
		 * second should resume, not fast-forward through a second of
		 * game logic -- which for a CHIP-8 means every timer expiring
		 * at once and the player dying for reasons they never saw. */
		if (elapsed > 4) elapsed = 4;
		last_frame = now;

		/* A paused machine does not age. Ticking DT and ST while
		 * stopped would expire every timer the moment you resumed,
		 * which for a game means dying during the pause you took in
		 * order to look at why you were dying. */
		if (!paused)
			while (elapsed--) c8_tick_timers(&vm);

		c8_frame(&vm);

		if (!paused) {
			if (!vm.halted && !vm.waiting)
				c8_run(&vm, ipf, NULL);
		} else if (step_pending) {
			c8_step(&vm);
			step_pending = false;
		}

		c8_sound_update(&vm);
		save_flags(false);

		/* 00FD -- the guest asked to exit.
		 *
		 * This used to do nothing at all, which is how a ROM offering
		 * "press X to exit" presented: the key reached the guest, the
		 * guest halted, and the app went on drawing the same frame
		 * forever with no instructions left to run. Indistinguishable
		 * from the keypress being ignored.
		 *
		 * Halting is the ROM ending, so the app ends with it -- but
		 * not while the debugger pane is open. Having it up is a
		 * statement that you want to watch the machine rather than
		 * play it, and the one moment you most want to look at is the
		 * instruction that stopped it. Flags are flushed either way,
		 * by the shutdown path below or by the force above. */
		if (vm.halted && !dbg_on) {
			printf("chip8: ROM executed exit (00FD)\n");
			quit = true;
		}

		if (game_mode) {
			c8_render(&rnd, &vm, true);
			blit_game();
		} else {
			if (c8_render(&rnd, &vm, need_full_redraw))
				blit_windowed(need_full_redraw);
			/* Every frame, whether or not the guest drew anything --
			 * a debugger that only updates when the picture changes
			 * is useless on exactly the ROM you are debugging. It is
			 * cheap because c8_debug_draw() only touches the cells
			 * that actually differ. */
			draw_panel(need_full_redraw);
			need_full_redraw = false;
		}

	}

	/* Ordered shutdown, and the order is the point: the mixer is a bus
	 * master pointed at this process's own memory, so it has to stop
	 * before the process does. */
	c8_sound_shutdown();
	save_flags(true);
	if (game_mode) leave_game_mode();
	z_win_destroy(&win);

	printf("chip8: %lu guest instructions executed\n",
		(unsigned long)vm.steps);

	return 0;

}
