#ifndef KGPAD_H
#define KGPAD_H

/*
 * kidgames -- the on-screen keyboard.
 *
 * The original is keyboard-only. Six of its ten games ask for a typed
 * answer, so "playable with the mouse alone" is not achievable by
 * adding hit boxes to what is already drawn -- there has to be
 * somewhere to click a letter. This is that.
 *
 * It turned out to earn its space for two other reasons, both of which
 * matter more than the mouse:
 *
 * -- IT SHOWS WHERE THE KEY IS --
 *
 * A five-year-old knows the letter and cannot find it. The layout here
 * is therefore QWERTY, matching the physical keyboard, NOT alphabetical.
 * Alphabetical would be easier to scan and completely useless for the
 * actual task, which is "the screen says K, now find K on the thing in
 * front of me". Same for digits: they are drawn as a row in physical
 * order, not as a calculator keypad, because the row is where they are
 * on the keyboard.
 *
 * -- IT SHOWS THE KEY IS STILL DOWN --
 *
 * Kids hold keys. Zeitlos does not auto-repeat -- sw/os/hid.c diffs
 * each USB report against the last and emits press/release EDGES only
 * -- so a key held for three seconds produces exactly one character,
 * and nothing goes wrong. But the kid does not know that, and holding
 * a key is a habit worth unlearning before it meets a machine that
 * does repeat.
 *
 * So a key that is down is drawn inverted, and a key held past
 * KG_HOLD_NUDGE_TICKS pulses. The pump keeps waking while a key is
 * held to drive that (see kg_getkey), which costs nothing: it is a
 * fill or two, ten times a second, only while a finger is actually
 * down.
 *
 * Presses from the physical keyboard and from the mouse light the same
 * key, because they go through the same index. That is the whole point
 * -- clicking C and pressing C must be visibly the same act.
 *
 * -- CLICKS BECOME KEYS --
 *
 * A pointer press inside the pad is turned into an ordinary key event
 * by the pump and never surfaces as KG_KEY_CLICK. Games that only take
 * typed answers therefore need no mouse code at all: they are already
 * mouse-playable via this file. Only games with their own clickable
 * board (Memory Match, Letter Hunt, the menu) ever see a click.
 */

#include "kg.h"

/* Rows, and the key geometry. 320 wide divides into ten 30px keys on a
 * 32px stride with a pixel to spare each side, which sets everything
 * else: the letter rows are indented to sit where they do on a real
 * keyboard, and the wide keys fill what is left. */
#define KG_PAD_ROWS      4
#define KG_PAD_ROW_H     16
#define KG_PAD_ROW_STEP  17
#define KG_PAD_KEY_W     30
#define KG_PAD_KEY_STEP  32
#define KG_PAD_MAX_KEYS  32

/* Held longer than this and the key starts pulsing. About two thirds
 * of a second at Z_TICK_HZ -- long enough not to nag on an ordinary
 * press, short enough to associate with the finger still being down. */
#define KG_HOLD_NUDGE_TICKS  (Z_TICK_HZ * 2 / 3)

/* A second press of the SAME key inside this window is dropped.
 * Contact bounce, and a kid drumming on one key, look identical from
 * up here and neither should produce two letters. */
#define KG_DEBOUNCE_TICKS    (Z_TICK_HZ / 20)

typedef struct {
	kg_key_t	key;		/* what this key produces */
	char		ch;		/* for KG_KEY_CHAR */
	int16_t		x, y, w, h;	/* playfield-relative */
	const char	*label;
} kg_padkey_t;

/* Show the pad for `cs`, or hide it. Showing it does not draw --
 * the caller's next repaint does, so a screen is never half-updated. */
void kg_pad_show(kg_charset_t cs);
void kg_pad_hide(void);
bool kg_pad_visible(void);

/* Draw the whole pad. Cheap enough to call on any repaint: at most
 * KG_PAD_MAX_KEYS frames, fills and labels. */
void kg_pad_draw(void);

/* Redraw one key in place, for a press or release. Avoids repainting
 * the pad thirty times a second while a key pulses. */
void kg_pad_draw_key(int idx);

/* The y of the topmost key currently laid out, or KG_H if the pad is
 * hidden. Not the same as KG_PAD_Y: the digits layout is bottom-aligned
 * into the band, so its keys start well below it. A caller that wants
 * to know how much play area it actually has must ask this, not the
 * constant. */
int kg_pad_top(void);

/* The key at playfield (x,y), or -1. */
int kg_pad_hit(int x, int y);

/* The index of the key that would produce this event, or -1 -- used to
 * light the pad when the press came from a real keyboard. */
int kg_pad_find(kg_key_t key, char ch);

const kg_padkey_t *kg_pad_key(int idx);
int kg_pad_count(void);

/* Which key is currently down, and since when. -1 for none. The pump
 * owns this; it is exposed so the pulse animation and the tests can
 * both see it. */
void kg_pad_set_held(int idx, uint32_t now_ticks);
int  kg_pad_held(void);

/* Advance the pulse. Returns true if something was redrawn, so the
 * pump knows whether to keep waking. */
bool kg_pad_animate(uint32_t now_ticks);

#endif
