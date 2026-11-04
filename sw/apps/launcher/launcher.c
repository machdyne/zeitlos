/*
 * launcher -- the console profile's front end (docs/launcher_app.md,
 * docs/profiles.md).
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * On a console bitstream (z_profile() == Z_PROFILE_CONSOLE) there is no
 * desktop: wm starts this instead of the dock, and starts it again if it
 * ever stops. It is a menu of what is in /apps, in game mode, on the
 * arcade runtime (docs/arcade.md): the games and demos first, then
 * everything else. Choosing one starts it and gives it the screen; when
 * it ends, the menu is back.
 *
 * It runs on a desktop too, as an ordinary arcade app -- which is how it
 * is tried without a console bitstream.
 *
 *   Up, Down        choose (the list scrolls)
 *   Left, Right     a page at a time
 *   Space, Enter    start it
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zfont.h"
#include "../../common/zpaths.h"
#include "../../common/games/zarcade.h"

#define MAX_APPS 128
#define NAME_MAX 24
#define ROWS     11              /* on screen at once */

static char names[MAX_APPS][NAME_MAX];
static bool game[MAX_APPS];
static int napps, sel, top;
static char status[48];

/* what is shown first: the games and the demos */
static const char *const games[] = {
	"mausrun3d", "mausrun", "space3d", "snake", "blocks", "bricks", "chess",
	"mahjong", "kidgames", "casino", "chip8", "planedemo", "gldemo", "gpu3d",
	"gpudemo", "basic", NULL
};
/* never shown: the system's own, and this */
static const char *const hidden[] = {
	"launcher", "wm", "net", "fs", "usb", "jfont", "tts", "update", NULL
};

static bool in_list(const char *n, const char *const *l) {
	int i;
	for (i = 0; l[i]; i++) if (!strcmp(n, l[i])) return true;
	return false;
}

static int game_rank(const char *n) {
	int i;
	for (i = 0; games[i]; i++) if (!strcmp(n, games[i])) return i;
	return 1000;
}

static int cmp(const void *a, const void *b) {
	const char *x = a, *y = b;
	int rx = game_rank(x), ry = game_rank(y);
	if (rx != ry) return rx - ry;
	return strcmp(x, y);
}

/* the names in /apps, programs only (".bin" away), sorted */
static void scan(void) {
	static char buf[4096];
	uint8_t isdir[MAX_APPS * 2];
	bool trunc = false;
	int n = ar_dir_list(Z_DIR_APPS, buf, (int)sizeof(buf), isdir, MAX_APPS * 2, &trunc), i;
	const char *p = buf;
	napps = 0;
	for (i = 0; i < n && napps < MAX_APPS; i++, p += strlen(p) + 1) {
		char nm[NAME_MAX];
		size_t l = strlen(p);
		if (isdir[i]) continue;
		if (l > 4 && !strcmp(p + l - 4, ".bin")) l -= 4;
		if (l == 0 || l >= NAME_MAX) continue;
		memcpy(nm, p, l);
		nm[l] = 0;
		if (in_list(nm, hidden)) continue;
		strcpy(names[napps++], nm);
	}
	qsort(names, (size_t)napps, NAME_MAX, cmp);
	for (i = 0; i < napps; i++) game[i] = game_rank(names[i]) < 1000;
	if (sel >= napps) sel = napps ? napps - 1 : 0;
}

/* -- the screen --------------------------------------------------------------- */

enum { C_BG, C_TEXT, C_DIM, C_SEL, C_SELTEXT, C_TITLE, C_LINE };

static void paint(void *user, const ar_rect_t *r) {
	int i, y;
	(void)user;
	ar_fill(r->x, r->y, r->w, r->h, C_BG);
	ar_text(16, 10, "ZEITLOS", C_TITLE, &z_font_5x8, 2);
	ar_fill(16, 30, 288, 1, C_LINE);
	if (!napps) {
		ar_text(16, 60, "NOTHING IN /APPS", C_TEXT, &z_font_5x8, 1);
		ar_text(16, 76, "IS THE CARD IN?", C_DIM, &z_font_5x8, 1);
		return;
	}
	for (i = 0, y = 40; i < ROWS && top + i < napps; i++, y += 16) {
		int k = top + i;
		bool s = k == sel;
		if (s) ar_fill(12, y - 3, 296, 15, C_SEL);
		ar_text(20, y, names[k], s ? C_SELTEXT : (game[k] ? C_TEXT : C_DIM), &z_font_5x8, 1);
	}
	if (top > 0) ar_text(296, 40, "^", C_DIM, &z_font_5x8, 1);
	if (top + ROWS < napps) ar_text(296, 40 + (ROWS - 1) * 16, "v", C_DIM, &z_font_5x8, 1);
	ar_fill(16, 222, 288, 1, C_LINE);
	ar_text(16, 226, status[0] ? status : "SPACE: START    ARROWS: CHOOSE", C_DIM, &z_font_5x8, 1);
}

static ar_desc_t desc = {
	.name = "launcher",
	.help_title = "Launcher",
	.help_text = "The console's menu: what is in /apps.\n\n"
		"Up, Down\tChoose\n"
		"Left, Right\tA page at a time\n"
		"Space, Enter\tStart it",
	.help_doc = "launcher_app",
	.palette = {
		[C_BG] = 0x112, [C_TEXT] = 0xfff, [C_DIM] = 0x89a, [C_SEL] = 0x37c,
		[C_SELTEXT] = 0xfff, [C_TITLE] = 0xfc3, [C_LINE] = 0x456
	},
	.ink = {
		[C_TEXT] = { { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } },
		[C_DIM] = { { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } },
		[C_SEL] = { { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } },
		[C_TITLE] = { { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } },
		[C_LINE] = { { 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55 } },
	},
	.paint = paint,
};

static void move(int to) {
	if (!napps) return;
	if (to < 0) to = 0;
	if (to >= napps) to = napps - 1;
	sel = to;
	if (sel < top) top = sel;
	if (sel >= top + ROWS) top = sel - ROWS + 1;
	ar_dirty_all();
}

/* start it, give it the screen, and wait for it to end */
static void start(void) {
	uint32_t pid, serial;
	if (!napps) return;
	pid = z_proc_run(names[sel]);
	if (!pid) {
		snprintf(status, sizeof(status), "%s WOULD NOT START", names[sel]);
		ar_dirty_all();
		return;
	}
	serial = z_proc_serial(pid);
	status[0] = 0;
	/* The screen stays ours until the program takes it: an arcade
	 * program takes game mode from us as it opens (wm hands the grab
	 * to the newest), anything else has wm revoke ours. Giving it up
	 * here first raced the program for game mode's register -- and
	 * turned off the game's game mode after it had turned it on.
	 * Nothing of ours is drawn while it runs. */
	for (;;) {
		ar_event_t e;
		while (ar_poll(&e)) { }
		if (z_proc_serial(pid) != serial) break;
		z_proc_wait(Z_TICK_HZ / 20);
	}
	scan();                             /* the card may have changed */
	/* the program's end gave the screen back to wm: take it again */
	ar_set_fullscreen(false);
	ar_set_fullscreen(true);
	ar_dirty_all();
}

int main(void) {
	bool console = z_profile() == Z_PROFILE_CONSOLE;
	printf("launcher: %s\n", console ? "the console's menu" : "a menu of the apps");
	if (!ar_init(&desc)) {
		printf("launcher: no window manager\n");
		return 1;
	}
	scan();
	ar_set_fullscreen(true);
	for (;;) {
		ar_event_t e;
		while (ar_poll(&e)) {
			if (e.type == AR_EV_QUIT) {
				/* on a console there is nowhere else to go */
				if (!console) { ar_shutdown(); return 0; }
			}
			if (e.type == AR_EV_MODE) ar_dirty_all();
			if (e.type == AR_EV_BUTTON && e.pressed) {
				if (e.key == AR_BTN_UP) move(sel - 1);
				else if (e.key == AR_BTN_DOWN) move(sel + 1);
				else if (e.key == AR_BTN_LEFT) move(sel - ROWS);
				else if (e.key == AR_BTN_RIGHT) move(sel + ROWS);
				else if (e.key == AR_BTN_A || e.key == AR_BTN_START) start();
			}
			if (e.type == AR_EV_KEY && e.pressed && e.key == '\r') start();
		}
		ar_frame();
	}
}
