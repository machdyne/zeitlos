/*
 * kidgames -- the runtime layer. See kgui.h.
 *
 * On the arcade runtime (sw/common/games/zarcade.h, docs/arcade.md):
 * the window, full screen (F11) and redrawing what was covered are the
 * runtime's, and F1 shows the help on the playfield itself (ui/sui.h),
 * full screen too. Everything here draws in the runtime's logical
 * 320x240, which is the playfield, in both modes: the runtime puts it
 * where it is and cuts it to what shows.
 *
 * Games draw as they always did, straight away. When the runtime has to
 * repaint part of the picture -- uncovered, after the help, a mode
 * change -- it asks paint() for that part, and paint() runs the screen's
 * repaint callback (kg_set_repaint()), cut to it.
 *
 * Monochrome, full screen too (prefer_mono): the app is drawn in one
 * bit, with dither and patterns for its greys (kg.h).
 */

#include <stdio.h>		// puts() only -- see the Makefile's noprintf target
#include <string.h>

#include "../../common/zsoc.h"		// Z_TICK_HZ
#include "../../common/zfont.h"
#include "../../common/zkbd.h"
#include "../../common/games/zarcade.h"

#ifndef AR_HOST_TESTS
#include "../../common/zeitlos.h"	// z_uptime_ticks()
#include "../../common/zgfx.h"		// z_fb_hw_blit_mem_available()
#endif

#include "kgui.h"
#include "kgpad.h"

static kg_repaint_fn repaint_fn;
static void *repaint_user;

static char last_char;
static int click_x, click_y;

/* Pointer state, so a press is an EDGE rather than a level. The runtime
 * passes on every move, so without this a finger resting on the button
 * would fire a key per pixel of drift. */
static uint32_t last_buttons;

/* Debounce, per kgpad.h: the last key accepted and when. */
static kg_key_t last_key_sym;
static char last_key_ch;
static uint32_t last_key_at;

#define FONT (&z_font_5x8)
#define FONT_H 8
#define FONT_W 5

/* The time in Z_TICK_HZ ticks: on a build host it moves a frame at a
 * time, with ar_frame(). */
static uint32_t now_ticks(void)
{
#ifndef AR_HOST_TESTS
	return z_uptime_ticks();
#else
	return ar_ticks() * Z_TICK_HZ / 60u;
#endif
}

/* -- the runtime's side --------------------------------------------- */

static const char help_text[] =
	"Ten learning games for kids: spelling, words, counting, "
	"math and memory.\n\n"
	"Arrows\tMove (Space: next)\n"
	"Enter\tChoose, or answer\n"
	"Letters\tType an answer\n"
	"Backspace\tRub out a letter\n"
	"Esc\tBack to the menu; quits there\n"
	"F11\tFull screen or window\n\n"
	"Clicking works too: the menu, the keyboard on the screen, "
	"letters and cards.";

/* What the runtime repaints, inside r: the screen, through its
 * callback, every draw already cut to r; black without one. */
static void paint(void *user, const ar_rect_t *r)
{
	(void)user;
	if (repaint_fn) repaint_fn(repaint_user);
	else ar_fill(r->x, r->y, r->w, r->h, 0);
}

static ar_desc_t desc = {
	.name = "kidgames",
	.help_title = "Kidgames help",
	.help_text = help_text,
	.help_doc = "kidgames_app",
	.paint = paint,
	.prefer_mono = true,
	.pointer = true,
};

/* -- drawing --------------------------------------------------------
 *
 * Index 0 is black and 1 white (the runtime's inks, set in kg_init());
 * every draw is clipped to the playfield by the runtime, so a rectangle
 * that runs off the edge is cut rather than landing anywhere else. */

void kg_fill(int x, int y, int w, int h, int color)
{
	ar_fill(x, y, w, h, color ? 1 : 0);
}

void kg_shade(int x, int y, int w, int h, int level)
{
	ar_ink_t ink;
	if (level <= 0) { ar_fill(x, y, w, h, 0); return; }
	if (level >= 16) { ar_fill(x, y, w, h, 1); return; }
	/* the blitter's own 4x4 ordered dither, screen-aligned */
	ar_ink_level(&ink, level);
	ar_fill2(x, y, w, h, 0, 1, &ink);
}

void kg_pattern(int x, int y, int w, int h, const uint8_t *pat)
{
	if (!pat) return;
	ar_fill2(x, y, w, h, 0, 1, (const ar_ink_t *)(const void *)pat);
}

bool kg_sprite_available(void)
{
#ifndef AR_HOST_TESTS
	return z_fb_hw_blit_mem_available();
#else
	return true;
#endif
}

bool kg_sprite(int x, int y, const uint8_t *bits, int stride, int w, int h)
{
	if (!kg_sprite_available()) return false;
	/* Lit where the bitmap has a bit and dark elsewhere, in one pass:
	 * nothing is masked, every piece goes on a background this app has
	 * just cleared (kgui.h). The runtime cuts it to the playfield and
	 * moves the source with the cut. */
	ar_bitmap_opaque(bits, stride, 0, 0, x, y, w, h);
	return true;
}

void kg_clear(void)
{
	kg_fill(0, 0, KG_W, KG_H, 0);
}

void kg_frame(int x, int y, int w, int h, int color)
{
	if (w <= 0 || h <= 0) return;

	/* Four fills, not lines: the blitter and the rasterizer do not
	 * order against each other, and nearly every frame here has text
	 * inside it (docs/widgets.md). A one-pixel fill is a perfectly
	 * good line. */
	kg_fill(x, y, w, 1, color);
	kg_fill(x, y + h - 1, w, 1, color);
	kg_fill(x, y, 1, h, color);
	kg_fill(x + w - 1, y, 1, h, color);
}

int kg_text_w(const char *s)
{
	/* 5x8 advances by exactly five pixels a character -- no gap, no
	 * kerning. */
	return s ? (int)strlen(s) * FONT_W : 0;
}

/* Text in a solid cell, as the glyph blitter drew it: the cells'
 * background first, then the letters. Ink 0 on cell 0 therefore erases
 * what it was drawn over rather than being merely invisible -- what
 * kgui.h promises kg_text() does. */
void kg_text2(int x, int y, const char *s, int fg, int bg)
{
	if (!s) return;
	kg_fill(x, y, kg_text_w(s), FONT_H, bg);
	if (fg != bg) ar_text(x, y, s, fg ? 1 : 0, FONT, 1);
}

void kg_text(int x, int y, const char *s, int color)
{
	kg_text2(x, y, s, color, 0);
}

void kg_center_text(int y, const char *s, int color)
{
	kg_text((KG_W - kg_text_w(s)) / 2, y, s, color);
}

void kg_center_text2(int y, const char *s, int fg, int bg)
{
	kg_text2((KG_W - kg_text_w(s)) / 2, y, s, fg, bg);
}

/* -- header, status, tries ------------------------------------------ */

void kg_header(const char *name, int level, int score)
{
	char buf[40];

	kg_fill(0, 0, KG_W, KG_HDR_H, 0);

	if (name) kg_text(2, 2, name, 1);

	if (level > 0 || score > 0) {
		buf[0] = '\0';
		kg_append(buf, sizeof(buf), "LVL ");
		kg_append_num(buf, sizeof(buf), level);
		kg_append(buf, sizeof(buf), "  SCORE ");
		kg_append_num(buf, sizeof(buf), score);
		kg_text(KG_W - 2 - kg_text_w(buf), 2, buf, 1);
	}

	kg_fill(0, KG_RULE_Y, KG_W, 1, 1);
}

void kg_status(const char *hint)
{
	/* With the keyboard up there is no bottom row to put this in, and
	 * the keyboard is a better hint than a sentence anyway. Silently
	 * dropping it here rather than at every call site keeps the games
	 * from each having to ask. */
	if (kg_pad_visible()) return;

	kg_fill(0, KG_H - FONT_H - 2, KG_W, FONT_H + 2, 0);
	if (hint) kg_center_text(KG_H - FONT_H - 1, hint, 1);
}

void kg_tries_left(int y, int tries_left, int tries_total)
{
	int i, x;
	int star_w = 9;
	int total_w;

	if (tries_total <= 0) return;
	if (tries_total > 20) tries_total = 20;
	if (tries_left < 0) tries_left = 0;

	kg_fill(0, y, KG_W, 12, 0);

	total_w = tries_total * star_w;
	x = (KG_W - total_w) / 2;

	for (i = 0; i < tries_total; i++) {

		int cx = x + i * star_w + 1;

		if (i < tries_left) {
			/* A remaining try: a solid diamond. Drawn rather than
			 * written as '*' because the 5x8 asterisk sits high in
			 * the cell and a row of them reads as specks. */
			kg_fill(cx + 2, y + 1, 3, 7, 1);
			kg_fill(cx, y + 3, 7, 3, 1);
		} else {
			/* A used try: an outline of the same size. Shape, not
			 * shade -- see kg.h. */
			kg_frame(cx + 1, y + 2, 5, 5, 1);
		}

	}
}

/* -- mode ------------------------------------------------------------ */

bool kg_in_game_mode(void) { return ar_fullscreen(); }

/* No window of its own any more: the runtime's. */
void *kg_window(void) { return NULL; }

void kg_toggle_mode(void)
{
	ar_set_fullscreen(!ar_fullscreen());
}

/* -- repaint --------------------------------------------------------- */

void kg_set_repaint(kg_repaint_fn fn, void *user)
{
	repaint_fn = fn;
	repaint_user = user;
}

kg_repaint_fn kg_get_repaint(void **user_out)
{
	if (user_out) *user_out = repaint_user;
	return repaint_fn;
}

void kg_repaint(void)
{
	if (repaint_fn) repaint_fn(repaint_user);
}

/* -- lifecycle ------------------------------------------------------- */

bool kg_init(void)
{
	int i;

	desc.ink[0] = ar_ink_black;
	for (i = 1; i < 16; i++) desc.ink[i] = ar_ink_white;
	desc.palette[0] = 0x000;
	for (i = 1; i < 16; i++) desc.palette[i] = 0xfff;

	if (!ar_init(&desc)) {
		puts("kidgames: failed to create window");
		return false;
	}

	/* Every key is the app's (C and M are letters a kid types), and
	 * Escape is "back" full screen too: F11 leaves. */
	ar_text_input(true);
	ar_escape_leaves(false);

	/* Full screen to start, where there is game mode: pixel-doubled
	 * text reads from across a room, and 1:1 5x8 text does not. */
	if (!ar_set_fullscreen(true))
		puts("kidgames: no game mode in this bitstream -- "
			"running in a window. Rebuild the gateware with GAME "
			"in rtl/boards.vh for the 320x240 viewport.");

	return true;
}

void kg_shutdown(void)
{
	ar_shutdown();
}

/* -- the pump -------------------------------------------------------- */

/* Translate a keysym into the small set of keys games see.
 * Everything else returns KG_KEY_NONE and is swallowed, which is the
 * original's "fool-proof input" rule: a kid mashing the keyboard
 * cannot reach a state a game did not plan for. */
static kg_key_t map_key(uint32_t keysym, char *ch_out)
{
	*ch_out = 0;

	if (keysym >= 'a' && keysym <= 'z') {
		*ch_out = (char)(keysym - 'a' + 'A');
		return KG_KEY_CHAR;
	}
	if (keysym >= 'A' && keysym <= 'Z') {
		*ch_out = (char)keysym;
		return KG_KEY_CHAR;
	}
	if (keysym >= '0' && keysym <= '9') {
		*ch_out = (char)keysym;
		return KG_KEY_CHAR;
	}

	switch (keysym) {
	case Z_KEY_UP:    return KG_KEY_UP;
	case Z_KEY_DOWN:  return KG_KEY_DOWN;
	case Z_KEY_LEFT:  return KG_KEY_LEFT;
	case Z_KEY_RIGHT: return KG_KEY_RIGHT;
	case ' ':         return KG_KEY_SPACE;
	case '\r':
	case '\n':        return KG_KEY_ENTER;
	case 0x1b:        return KG_KEY_ESC;
	case '\b':
	case 0x7f:        return KG_KEY_BACKSPACE;
	default:          return KG_KEY_NONE;
	}
}

/* Accept a key unless it is a repeat of the last one inside the
 * debounce window. See KG_DEBOUNCE_TICKS. */
static bool debounce_ok(kg_key_t k, char ch, uint32_t now)
{
	if (k == last_key_sym && ch == last_key_ch &&
		(uint32_t)(now - last_key_at) < KG_DEBOUNCE_TICKS)
		return false;

	last_key_sym = k;
	last_key_ch = ch;
	last_key_at = now;

	return true;
}

/*
 * The runtime's events since the last call, as far as the first key.
 *
 * Returns the key produced, or KG_KEY_NONE if there were none. F1, F11
 * and redrawing are the runtime's and never arrive here.
 */
static bool quitting;	/* the window was closed: QUIT from now on */

static kg_key_t pump_once(void)
{
	ar_event_t e;

	/* Closing is said once, and a game that took the QUIT as "back to
	 * the menu" leaves the menu to hear it too: every screen on the
	 * way out sees it. */
	if (quitting) return KG_KEY_QUIT;

	while (ar_poll(&e)) {

		switch (e.type) {

		case AR_EV_QUIT:
			quitting = true;
			return KG_KEY_QUIT;

		case AR_EV_KEY: {

			char ch;
			kg_key_t k;

			if (!e.pressed) {
				/* A release only ever un-lights the pad. Acting on
				 * releases would double every keystroke. */
				if (kg_pad_held() >= 0) kg_pad_set_held(-1, now_ticks());
				break;
			}

			k = map_key(e.key, &ch);
			if (k == KG_KEY_NONE) break;

			if (!debounce_ok(k, ch, now_ticks())) break;

			/* Light the matching pad key, so a press on the real
			 * keyboard and a click on the drawn one look identical --
			 * which is the whole reason the pad helps a kid find a
			 * key at all. */
			kg_pad_set_held(kg_pad_find(k, ch), now_ticks());

			last_char = ch;
			return k;
		}

		case AR_EV_MOUSE: {

			/* Press edge only: a held button would otherwise fire
			 * on every move. */
			uint32_t buttons = e.buttons;
			bool down = (buttons & 1) && !(last_buttons & 1);
			bool up = !(buttons & 1) && (last_buttons & 1);
			int idx;

			last_buttons = buttons;

			if (up) {
				if (kg_pad_held() >= 0) kg_pad_set_held(-1, now_ticks());
				break;
			}

			if (!down || !e.inside) break;
			if (e.x < 0 || e.x >= KG_W || e.y < 0 || e.y >= KG_H) break;

			idx = kg_pad_hit(e.x, e.y);

			if (idx >= 0) {
				/* A click on the pad becomes an ordinary key and
				 * never surfaces as KG_KEY_CLICK. This is what
				 * makes the six typing games mouse-playable
				 * without a line of mouse code in any of them. */
				const kg_padkey_t *pk = kg_pad_key(idx);

				if (!debounce_ok(pk->key, pk->ch, now_ticks())) break;

				kg_pad_set_held(idx, now_ticks());
				last_char = pk->ch;
				return pk->key;
			}

			click_x = e.x;
			click_y = e.y;
			return KG_KEY_CLICK;
		}

		default:
			break;

		}
	}

	return KG_KEY_NONE;
}

/* A frame: whatever the runtime has to repaint, at the next boundary.
 * It sleeps until then (docs/app_runtime.md, "Idling"). */
static void frame(void)
{
	if (kg_pad_held() >= 0) kg_pad_animate(now_ticks());
	ar_frame();
}

kg_key_t kg_getkey_timeout(uint32_t ticks)
{
	uint32_t deadline = now_ticks() + ticks;

	for (;;) {

		kg_key_t k = pump_once();

		if (k != KG_KEY_NONE) return k;
		if (ticks && (int32_t)(now_ticks() - deadline) >= 0) return KG_KEY_TIMEOUT;

		frame();

	}
}

kg_key_t kg_getkey(void)
{
	return kg_getkey_timeout(0);
}

char kg_last_char(void) { return last_char; }
int kg_click_x(void) { return click_x; }
int kg_click_y(void) { return click_y; }

bool kg_key_is_next(kg_key_t k)
{
	return k == KG_KEY_DOWN || k == KG_KEY_RIGHT || k == KG_KEY_SPACE;
}

bool kg_key_is_prev(kg_key_t k)
{
	return k == KG_KEY_UP || k == KG_KEY_LEFT;
}

void kg_pause_ms(int ms)
{
	uint32_t ticks = ((uint32_t)ms * Z_TICK_HZ) / 1000u;
	uint32_t deadline = now_ticks() + (ticks ? ticks : 1);

	for (;;) {

		/* Keys pressed during a pause are DISCARDED -- the original
		 * does the same, and it matters more here: these screens are
		 * the ones a pre-reader sits through, and a keypress banked
		 * during one would fire into whatever came next. */
		if (pump_once() == KG_KEY_QUIT) return;

		if ((int32_t)(now_ticks() - deadline) >= 0) return;

		frame();

	}
}
