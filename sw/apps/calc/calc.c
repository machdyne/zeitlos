/*
 * calc -- a four-function calculator
 *
 *   > run wm
 *   > run calc
 *
 * The arithmetic is not in this file. calc_core.c holds all of it,
 * with no drawing, windows or messages, so that calc_test.c can
 * exercise it on a host -- see that file. A calculator that is subtly
 * wrong is worse than no calculator, because nobody checks its
 * answers.
 *
 * -- keyboard-only --
 *
 * Everything works without a pointer, and not by way of Tab alone:
 * the digits, operators, Enter, Escape and Backspace all do the
 * obvious thing directly. Tab focus over the keypad exists as well,
 * for consistency with the rest of the system, but typing 2+3<Enter>
 * is the fast path and does not involve it.
 *
 * -- layout --
 *
 *   +--------------------+
 *   |            1234.56 |   display, right-aligned
 *   +--------------------+
 *   | C  | +- | <  | /   |
 *   | 7  | 8  | 9  | *   |
 *   | 4  | 5  | 6  | -   |
 *   | 1  | 2  | 3  | +   |
 *   | 0       | .  | =   |
 *   +--------------------+
 *
 * Fixed size and narrow -- screen space is scarce and a keypad has
 * nothing to reveal by growing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zgfx.h"
#include "../../common/zfont.h"
#include "../../common/zkbd.h"
#include "../../common/zwidget.h"
#include "calc_core.h"

#define MARGIN      4
#define GAP         3			// >= 2, so the focus ring has room --
								// see docs/widgets.md
#define BTN_W       32
#define BTN_H       17
#define COLS        4
#define ROWS        5

#define DISP_H      (z_font_5x8.h + 6)

// Derived rather than guessed, so the window is exactly as wide as
// the keypad needs and no wider.
#define WIN_W   (COLS * BTN_W + (COLS - 1) * GAP + 2 * MARGIN + 4)
#define WIN_H   (ROWS * BTN_H + (ROWS - 1) * GAP + DISP_H + 3 * MARGIN \
					+ Z_WM_TITLEBAR_H + 4)

static z_win_t win;
static calc_t calc;

// -- keypad --
//
// One entry per button, in reading order. `key` is what the button
// does, in the same alphabet calc_test.c uses for its keystroke
// sequences -- digits and operators as themselves, plus 'c' clear,
// 'n' negate, '<' backspace, '=' equals. That the tests and the
// buttons speak the same language is deliberate: a button is then
// exactly a tested keystroke, with nothing in between to get wrong.
typedef struct {
	const char	*label;
	char		key;
	uint8_t		span;		// columns wide
} calc_btn_t;

static const calc_btn_t keypad[] = {
	{ "C",  'c', 1 }, { "+-", 'n', 1 }, { "<", '<', 1 }, { "/", '/', 1 },
	{ "7",  '7', 1 }, { "8",  '8', 1 }, { "9", '9', 1 }, { "*", '*', 1 },
	{ "4",  '4', 1 }, { "5",  '5', 1 }, { "6", '6', 1 }, { "-", '-', 1 },
	{ "1",  '1', 1 }, { "2",  '2', 1 }, { "3", '3', 1 }, { "+", '+', 1 },
	{ "0",  '0', 2 },                   { ".", '.', 1 }, { "=", '=', 1 },
};
#define BTN_COUNT (int)(sizeof(keypad) / sizeof(keypad[0]))

static z_widget_t widgets[BTN_COUNT];
static z_widget_set_t wset;

static int y_disp, y_pad;

static void layout(void) {

	y_disp = MARGIN;
	y_pad = y_disp + DISP_H + MARGIN;

	int col = 0, row = 0;

	for (int i = 0; i < BTN_COUNT; i++) {

		widgets[i].x = (int16_t)(MARGIN + col * (BTN_W + GAP));
		widgets[i].y = (int16_t)(y_pad + row * (BTN_H + GAP));
		widgets[i].w = (int16_t)(keypad[i].span * BTN_W +
			(keypad[i].span - 1) * GAP);
		widgets[i].h = BTN_H;

		col += keypad[i].span;

		if (col >= COLS) { col = 0; row++; }

	}

	z_widget_invalidate(&wset);

}

// The display: the value right-aligned in a framed strip.
//
// Right-aligned because that is where a number belongs -- the digits
// that matter most stay in the same place as the value grows, instead
// of the whole thing shifting under the eye on every keypress.
static void draw_display(void) {

	int cw = z_win_content_w(&win);

	z_clip_t c;
	z_win_content_rect(&win, &c);

	int x0 = c.x0 + MARGIN;
	int y0 = c.y0 + y_disp;
	int x1 = c.x0 + cw - MARGIN - 1;
	int y1 = y0 + DISP_H - 1;

	if (x1 <= x0) return;

	// Interior, then frame, then text -- all through the blitter.
	// Mixing the line rasterizer in for the frame would put two
	// engines with no ordering between them on the same VRAM words;
	// see field_draw() in sw/common/zdialog.c.
	z_fb_hw_fill_rect(x0 + 1, y0 + 1, x1 - x0 - 1, y1 - y0 - 1, 0);

	z_fb_hw_fill_rect(x0, y0, x1 - x0 + 1, 1, 1);
	z_fb_hw_fill_rect(x0, y1, x1 - x0 + 1, 1, 1);
	z_fb_hw_fill_rect(x0, y0, 1, y1 - y0 + 1, 1);
	z_fb_hw_fill_rect(x1, y0, 1, y1 - y0 + 1, 1);

	char buf[32];
	int n = calc_format(&calc, buf, sizeof(buf));

	z_clip_t clip;
	clip.x0 = x0 + 2;
	clip.y0 = y0 + 1;
	clip.x1 = x1 - 2;
	clip.y1 = y1 - 1;

	if (clip.x1 < clip.x0) return;

	int tx = x1 - 3 - n * z_font_5x8.w;
	if (tx < clip.x0) tx = clip.x0;

	// Opaque, so the previous value's tail is overwritten rather than
	// needing a clear first -- the same no-flash reasoning as
	// sw/apps/info.
	z_fb_draw_text2(tx, y0 + 3, buf, 1, 0, &z_font_5x8, &clip);

}

static void repaint(void) {

	// wm clears before most redraws but NOT after a move, so anything
	// not actively rewritten keeps its pre-move contents.
	z_win_clear(&win);

	draw_display();
	z_widget_draw_all(&wset, true);

}

// Applies one key, in the same alphabet the test suite uses.
static void press(char key) {

	switch (key) {

		case 'c': calc_reset(&calc); break;
		case 'n': calc_sign(&calc); break;
		case '<': calc_backspace(&calc); break;
		case '=': calc_equals(&calc); break;

		case '+': case '-': case '*': case '/':
			calc_op(&calc, key);
			break;

		case '.': calc_point(&calc); break;

		default:
			if (key >= '0' && key <= '9') calc_digit(&calc, key - '0');
			else return;
			break;

	}

	draw_display();

}

static void handle_mouse(uint32_t packed) {

	int cx, cy;
	bool inside = z_win_mouse_content_xy(&win, packed, &cx, &cy);

	uint8_t buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);

	// Samples over the titlebar reach us too -- wm's hit test is the
	// whole window rect. Without this, clicking the close icon also
	// lands on whatever button sits at the clamped coordinates.
	if (!inside && wset.pressed < 0) return;

	int act = z_widget_mouse(&wset, cx, cy, buttons);

	if (act >= 0) press(keypad[act].key);

}

static void handle_key(uint32_t keysym, uint8_t mods) {

	// Tab moves the keypad focus, for consistency with the rest of
	// the system. It is not the fast path here and is not meant to
	// be -- the keys below do the obvious thing directly.
	if (keysym == '\t') {
		z_widget_focus_next(&wset, (mods & Z_KBD_MOD_SHIFT) != 0);
		z_widget_draw_all(&wset, false);
		return;
	}

	// Space presses the focused button. Enter does NOT -- it is '='
	// on a calculator, which is what a hand reaching for it expects,
	// and having it mean two different things depending on where
	// focus happens to be would be worse than not having focus.
	if (keysym == ' ') {
		int act = z_widget_key_activate(&wset);
		if (act >= 0) press(keypad[act].key);
		return;
	}

	switch (keysym) {

		case 0x0d: press('='); return;		// Enter
		case 0x1b: press('c'); return;		// Escape
		case 0x7f: press('<'); return;		// Backspace (DEL, see zkbd.c)

		case Z_KEY_DELETE: press('c'); return;

		default: break;

	}

	if ((keysym >= '0' && keysym <= '9') || keysym == '.' ||
		keysym == '+' || keysym == '-' || keysym == '*' ||
		keysym == '/' || keysym == '=')
		press((char)keysym);

}

static void widgets_init(void) {

	memset(widgets, 0, sizeof(widgets));

	for (int i = 0; i < BTN_COUNT; i++) {
		widgets[i].type = Z_WIDGET_BUTTON;
		widgets[i].label = keypad[i].label;
		widgets[i].enabled = true;
	}

	z_widget_set_init(&wset, widgets, BTN_COUNT, &win);

	// Focus starts somewhere visible so Tab has an obvious origin.
	z_widget_focus_set(&wset, 0);

}

int main(void) {

	printf("calc: starting\n");

	calc_reset(&calc);

	// CLOSE_KILLS_OWNER is correct: one window for the app's whole
	// lifetime and nothing to save on the way out.
	//
	// Not resizable -- a keypad has nothing to reveal by growing, and
	// screen space is scarce.
	if (z_win_create_flags(&win, "calc", WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER) != Z_OK) {
		printf("calc: failed to create window -- is wm running?\n");
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

		// Nothing here polls anything, so block rather than spin. A
		// calculator sitting open should cost nothing at all.
		z_proc_wait(0);

	}

	return 0;

}
