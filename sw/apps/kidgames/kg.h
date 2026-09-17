#ifndef KIDGAMES_KG_H
#define KIDGAMES_KG_H

/*
 * kidgames -- educational games for kids, on Zeitlos.
 *
 * Ported from https://github.com/machdyne/kidgames, which targets
 * ncurses on Kakao Linux. This file is the part of that port that
 * everything else depends on: the fixed geometry, and the handful of
 * enums the original's ui.h had.
 *
 * -- ONE SCREEN SIZE, ALWAYS --
 *
 * The playfield is 320x240 in BOTH display modes, and that is the
 * single most load-bearing decision in this app.
 *
 * Game mode (docs/game_mode.md) is a 320x240 camera over the 640x480
 * framebuffer, pixel-doubled on scanout. A window, meanwhile, can be
 * any size the wm gives it. Supporting both properly would normally
 * mean a layout that reflows -- which for a screen full of hand-placed
 * big letters, a keyboard and a row of stars is a great deal of
 * arithmetic to get wrong, and `sw/apps/logic` is the standing
 * reminder of how that ends (three shipped layout bugs against a
 * passing test; see sw/common/tests/zrender.h).
 *
 * So the window is created at exactly the size that yields a 320x240
 * CONTENT AREA (KG_WIN_W/KG_WIN_H below) and is not resizable. Every
 * screen in this app is then laid out once, against constants, and
 * both modes draw the identical pixels to a different origin. There is
 * no reflow path because there is nothing to reflow.
 *
 * The practical payoff: a bug you can see in the windowed mode is the
 * same bug in game mode, and the host render harness (tests/render.c)
 * shows you exactly what a board will show you.
 *
 * -- WHY GAME MODE IS THE DEFAULT --
 *
 * At 1:1 on a 640x480 screen a 5x8 glyph is small and a kid at TV
 * distance cannot read it. Pixel-doubled it is comfortable. The app
 * therefore starts in game mode where the hardware has it, and F2
 * drops to a window for when somebody wants the desktop back.
 */

#include <stdint.h>
#include <stdbool.h>

/*
 * Two Zeitlos headers, and only two.
 *
 * KG_WIN_H is derived from Z_WM_TITLEBAR_H (zwm.h) and the tick
 * constants below from Z_TICK_HZ (zsoc.h), so both have to be here
 * rather than left to whichever .c happens to have included them
 * first -- a macro that only fails to compile at its USE site is a
 * trap for the next file that uses it.
 *
 * zwin.h and zgfx.h are deliberately NOT here. A game that can see
 * them can draw with the wrong coordinate convention, which is the
 * single most common mistake in this area (docs/widgets.md) and the
 * one this app's layer exists to make impossible. They are included
 * by kgui.c alone.
 */
#include "../../common/zsoc.h"		// Z_TICK_HZ
#include "../../common/zwm.h"		// Z_WM_TITLEBAR_H

/* -- the playfield ------------------------------------------------ */

#define KG_W            320
#define KG_H            240

/*
 * Window size that produces exactly KG_W x KG_H of content.
 *
 * z_win_content_rect() (sw/common/zwin.c) insets 2px on every edge and
 * puts the content below a Z_WM_TITLEBAR_H-tall titlebar:
 *
 *     x0 = x + 2                 x1 = x + w - 3
 *     y0 = y + Z_WM_TITLEBAR_H + 2   y1 = y + h - 3
 *
 * so content_w = w - 4 and content_h = h - Z_WM_TITLEBAR_H - 4.
 *
 * Derived here rather than written down as 324/255, because the inset
 * is zwin.c's to change and a hardcoded number would not follow it.
 * That is exactly the duplication z_win_content_rect()'s own comment
 * warns about -- two copies of the formula, one of which fell behind.
 */
#define KG_WIN_W        (KG_W + 4)
#define KG_WIN_H        (KG_H + Z_WM_TITLEBAR_H + 4)

/* -- bands ---------------------------------------------------------
 *
 *   y 0..11     header: game name, level, score
 *   y 12        rule
 *   y 15..168   play area (154px)
 *   y 169       rule
 *   y 171..239  on-screen keyboard (69px)
 *
 * With the keyboard hidden the play area runs to the bottom instead.
 */

#define KG_HDR_H        12
#define KG_RULE_Y       12

#define KG_PLAY_Y       15
#define KG_PAD_Y        171
#define KG_PAD_H        (KG_H - KG_PAD_Y)

#define KG_PLAY_H       (KG_PAD_Y - 2 - KG_PLAY_Y)
#define KG_PLAY_H_FULL  (KG_H - KG_PLAY_Y)

/* Vertical centre of the play area, for each pad state. Games centre
 * on these rather than on KG_H/2, which would sit under the pad. */
#define KG_PLAY_MID     (KG_PLAY_Y + KG_PLAY_H / 2)
#define KG_PLAY_MID_FULL (KG_PLAY_Y + KG_PLAY_H_FULL / 2)

/* -- colour, on a display that has none ----------------------------
 *
 * The original carries meaning in eleven ncurses colour pairs. Here
 * there are two colours and seventeen dither levels
 * (z_fb_hw_fill_shade, zgfx.h).
 *
 * THE RULE: a shade never carries meaning on its own. z_fb_hw_fill_shade()
 * degrades to a plain black or white fill on a bitstream built without
 * dither support -- zgfx.h says so -- so a distinction drawn only in
 * shade silently disappears there. Every distinction in this app is
 * also a difference in shape, position or inversion. It is better for
 * a six-year-old on a small screen anyway.
 *
 * THE SECOND RULE, learned from a render: NEVER PUT TEXT ON A SHADE.
 * Inverted glyphs on an ordered dither are illegible -- the dither has
 * already lit half the pixels the letters need, so stroke and
 * background differ by almost nothing. A surface that carries text is
 * filled solid and the text inverted; a surface that carries no text
 * may be shaded. The menu's selected row was the first casualty and is
 * now a solid bar (kgwidget.c).
 */
#define KG_SHADE_SEL    6       /* wash on a surface with NO text on it */
#define KG_SHADE_HINT   7       /* the answer, shown after a wrong try */
#define KG_SHADE_DIM    4       /* disabled / used-up */
#define KG_SHADE_BACK   3       /* card backs, blanks */

/* -- input, normalised ---------------------------------------------
 *
 * Deliberately the same shape as the original's ui_key_t, so the ten
 * games port with their control flow intact. KG_KEY_CLICK is the one
 * addition: a pointer press somewhere this app has not already turned
 * into a key (see kgpad.h -- clicks on the on-screen keyboard arrive
 * as ordinary keys and never as clicks).
 */
typedef enum {
	KG_KEY_NONE = 0,
	KG_KEY_UP,
	KG_KEY_DOWN,
	KG_KEY_LEFT,
	KG_KEY_RIGHT,
	KG_KEY_ENTER,
	KG_KEY_ESC,
	KG_KEY_BACKSPACE,
	KG_KEY_SPACE,
	KG_KEY_CHAR,		/* kg_last_char(): 'A'-'Z' or '0'-'9' */
	KG_KEY_CLICK,		/* kg_click_x()/kg_click_y() */
	KG_KEY_TIMEOUT,		/* only from kg_getkey_timeout() */
	KG_KEY_QUIT		/* window closed, or the machine wants us gone */
} kg_key_t;

/* Which characters a text field will accept -- the original's
 * ui_charset_t. It also picks the on-screen keyboard's layout, so a
 * number field shows a number row and not the alphabet. */
typedef enum {
	KG_CS_ALPHA = 0,	/* A-Z */
	KG_CS_DIGITS,		/* 0-9 */
	KG_CS_ALNUM		/* both */
} kg_charset_t;

#endif
