/*
 * settings -- system preferences
 *
 * One setting so far: the display phosphor mode. The app exists as
 * much for the shape as for the content -- somewhere for the next
 * preference to go that isn't a shell command.
 *
 *   > run wm
 *   > run settings
 *
 * -- keyboard-only --
 *
 * Fully usable with no pointer: Tab and Shift+Tab move between the
 * radio buttons, Enter or Space picks one, and the arrow keys move
 * within the group the way a radio group should. That is not an
 * afterthought here -- a settings app you can only reach with a mouse
 * is exactly the wrong thing to have on a machine whose pointer might
 * be the thing you are trying to fix.
 *
 * -- applying immediately --
 *
 * Choosing a mode applies it there and then; there is no OK button to
 * confirm and nothing to cancel. For a setting whose entire effect is
 * visible the instant it changes, a confirmation step asks the user to
 * commit to something they can already see, and an undo step is just
 * "pick the other one again".
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zgfx.h"
#include "../../common/zfont.h"
#include "../../common/zkbd.h"
#include "../../common/zwidget.h"

// Small, and fixed. There is nothing here that benefits from more
// room, and screen space is scarce -- see the note on window sizes in
// docs/window_manager.md. Not resizable for the same reason: a
// preferences panel has no content to reveal.
#define WIN_W   180
#define WIN_H   118

static z_win_t win;

#define MARGIN      6
#define LINE_H      (z_font_5x8.h + 2)
#define BTN_H       16
#define BTN_GAP     3

// -- the video mode group --
//
// One toggle per mode, all in one radio group, so the widget toolkit
// handles exclusivity and nothing here has to track which is on.
#define GROUP_VIDEO 1

static const struct {
	uint32_t	mode;
	const char	*label;
} modes[] = {
	{ Z_VIDEO_MODE_WHITE, "White"  },
	{ Z_VIDEO_MODE_AMBER, "Amber"  },
	{ Z_VIDEO_MODE_GREEN, "Green"  },
	{ Z_VIDEO_MODE_PAPER, "Paper"  },
};
#define MODE_COUNT (int)(sizeof(modes) / sizeof(modes[0]))

static z_widget_t widgets[MODE_COUNT];
static z_widget_set_t wset;

static int y_title, y_btn0, btn_w;

// True if this bitstream can actually change the mode. Probed once --
// see z_video_mode_present() in zsoc.h for why a socctl that answers
// its magic still might not have the register.
static bool can_set;

static void layout(void) {

	int cw = z_win_content_w(&win);

	y_title = MARGIN;
	y_btn0 = y_title + LINE_H + 3;

	btn_w = cw - 2 * MARGIN;
	if (btn_w < 8) btn_w = 8;

	for (int i = 0; i < MODE_COUNT; i++) {
		widgets[i].x = (int16_t)MARGIN;
		widgets[i].y = (int16_t)(y_btn0 + i * (BTN_H + BTN_GAP));
		widgets[i].w = (int16_t)btn_w;
		widgets[i].h = BTN_H;
	}

	z_widget_invalidate(&wset);

}

static void repaint(void) {

	// wm clears before most redraws but NOT after a move, so anything
	// not actively rewritten keeps its pre-move contents.
	z_win_clear(&win);

	z_win_draw_text(&win, MARGIN, y_title,
		can_set ? "Display" : "Display (unavailable)", 1, &z_font_5x8);

	z_widget_draw_all(&wset, true);

}

// Applies the mode behind widget `idx` and makes sure the group's
// selection matches what actually happened.
static void apply(int idx) {

	if (idx < 0 || idx >= MODE_COUNT) return;

	if (!can_set) {
		// The bitstream predates the register (an RTL change -- needs
		// `make flash`, not `make dev-flash`). Put the selection back
		// where it was rather than leaving the group showing a mode
		// the display is not in.
		z_widget_select(&wset, 0);
		z_widget_draw_all(&wset, false);
		return;
	}

	z_video_set_mode(modes[idx].mode);

	// Read back rather than trusting the write. z_video_set_mode()
	// can refuse, and the group must show what the display is
	// actually doing, not what was asked for.
	uint32_t now = z_video_get_mode();

	for (int i = 0; i < MODE_COUNT; i++) {
		if (modes[i].mode == now) {
			z_widget_select(&wset, i);
			break;
		}
	}

	z_widget_draw_all(&wset, false);

}

static void handle_mouse(uint32_t packed) {

	int cx, cy;
	bool inside = z_win_mouse_content_xy(&win, packed, &cx, &cy);

	uint8_t buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);

	// Samples over the titlebar reach us too -- wm's hit test is the
	// whole window rect. Without this, clicking the close icon also
	// lands on whatever widget happens to sit at the clamped
	// coordinates. Same guard as sw/apps/text and sw/apps/term.
	if (!inside && wset.pressed < 0) return;

	int act = z_widget_mouse(&wset, cx, cy, buttons);

	if (act >= 0) apply(act);

}

static void handle_key(uint32_t keysym, uint8_t mods) {

	switch (keysym) {

		case '\t':
			z_widget_focus_next(&wset, (mods & Z_KBD_MOD_SHIFT) != 0);
			z_widget_draw_all(&wset, false);
			return;

		// Arrows move within the group. A radio group is the one
		// place arrows have an unambiguous meaning -- there is
		// nothing else on this window for them to belong to.
		case Z_KEY_UP:
		case Z_KEY_LEFT:
			z_widget_focus_next(&wset, true);
			z_widget_draw_all(&wset, false);
			return;

		case Z_KEY_DOWN:
		case Z_KEY_RIGHT:
			z_widget_focus_next(&wset, false);
			z_widget_draw_all(&wset, false);
			return;

		case 0x0d:		// Enter
		case ' ':
			apply(z_widget_key_activate(&wset));
			return;

		default:
			return;

	}

}

static void widgets_init(void) {

	memset(widgets, 0, sizeof(widgets));

	for (int i = 0; i < MODE_COUNT; i++) {
		widgets[i].type = Z_WIDGET_TOGGLE;
		widgets[i].group = GROUP_VIDEO;
		widgets[i].label = modes[i].label;
		widgets[i].enabled = true;
	}

	z_widget_set_init(&wset, widgets, MODE_COUNT, &win);

	// Start on whatever the display is actually in, so the panel
	// opens agreeing with the screen rather than asserting a default.
	uint32_t now = z_video_get_mode();

	for (int i = 0; i < MODE_COUNT; i++) {
		if (modes[i].mode == now) {
			z_widget_select(&wset, i);
			z_widget_focus_set(&wset, i);
			break;
		}
	}

}

int main(void) {

	printf("settings: starting\n");

	can_set = z_video_mode_present();

	if (!can_set)
		printf("settings: this bitstream has no video mode register "
			"-- needs `make flash`, not `make dev-flash`\n");

	// CLOSE_KILLS_OWNER is correct: one window for the app's whole
	// lifetime, nothing to save, nothing to ask about on the way out.
	if (z_win_create_flags(&win, "settings", WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER) != Z_OK) {
		printf("settings: failed to create window -- is wm running?\n");
		return 1;
	}

	// Claimed and discarded: this app takes no argument, but leaving
	// one pending would hand it to whatever the user opens next.
	{
		char ignored[8];
		z_launch_arg_take(ignored, sizeof(ignored));
	}

	widgets_init();
	layout();
	repaint();

	for (;;) {

		z_msg_t msg;

		while (z_msg_read(&msg) == Z_OK) {

			switch (msg.subject) {

				case Z_WM_KEY:

					if (msg.obj.type != Z_UINT32) break;
					if (!Z_WM_UNPACK_KEY_PRESSED(msg.obj.val.uint32)) break;

					handle_key(Z_WM_UNPACK_KEY_KEYSYM(msg.obj.val.uint32),
						(uint8_t)Z_WM_UNPACK_KEY_MODIFIERS(msg.obj.val.uint32));

					break;

				case Z_WM_MOUSE:

					if (msg.obj.type == Z_UINT32)
						handle_mouse(msg.obj.val.uint32);

					break;

				// The part of this window not covered by the windows in front
		// of it. Confines every subsequent draw to it -- see
		// z_win_apply_clip() in zwin.c. The ack it sends is not
		// optional: wm waits for it when a region narrows.
		case Z_WM_SET_CLIP:
			z_win_apply_clip(&win, &msg.obj);
			break;

		case Z_WM_REDRAW:

					if (msg.obj.type != Z_UINT32) break;
					if (z_win_redraw_id(msg.obj.val.uint32) != win.id) break;

					z_win_apply_redraw(&win, msg.obj.val.uint32);
					repaint();
					z_win_redraw_done(&win);

					break;

				case Z_WM_WINDOW_MOVED:
					z_win_parse_rect(&win, &msg.obj);
					break;

				default:
					break;

			}

		}

		// Nothing here polls anything, so block until something
		// arrives rather than spinning. A preferences panel sitting
		// open should cost nothing at all.
		z_proc_wait(0);

	}

	return 0;

}
