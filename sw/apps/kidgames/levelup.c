/*
 * kidgames -- the level-up screen. See levelup.h.
 */

#include "levelup.h"
#include "kgui.h"
#include "kgpad.h"
#include "kgfont.h"
#include "kgsound.h"
#include "wordlist.h"	// WORDLIST_MAX_LEVEL -- the star row is one per level

static int shown_level;

static void draw(void *user)
{
	char num[8];
	int scale;
	int y;

	(void)user;

	kg_clear();

	/*
	 * Stars across the top, growing with the level.
	 *
	 * The original draws a cake here. A cake at 320x240 in one bit
	 * would have to be a sprite, and the art pipeline is a later
	 * phase -- but more usefully, a row of stars SAYS SOMETHING a cake
	 * does not: there are more of them than last time. That is the
	 * whole message of the screen, delivered without a number or a
	 * word.
	 *
	 * kg_tries_left() draws exactly this row and is already shared by
	 * Number Guessing and Word Guess, so reusing it here keeps one
	 * star shape in the app rather than two that drift apart.
	 */
	kg_tries_left(KG_PLAY_Y + 8, shown_level, WORDLIST_MAX_LEVEL);

	kg_center_text(KG_PLAY_Y + 30, "LEVEL UP!", 1);

	kg_utoa(num, sizeof(num), (unsigned long)shown_level);

	/* Sized to the space rather than to a constant, so level 10 is as
	 * big as it can be rather than as big as level 1 happened to fit. */
	y = KG_PLAY_Y + 48;
	scale = kg_big_best_scale(num, KG_W - 40, KG_H - y - 16);
	kg_big_puts_centered(y, num, scale, KG_BIG_SOLID);
}

void levelup_celebrate(int new_level)
{
	kg_repaint_fn saved_fn;
	void *saved_user;
	bool pad_was = kg_pad_visible();

	saved_fn = kg_get_repaint(&saved_user);
	shown_level = new_level;

	/* The keyboard has no place on a screen with nothing to type, and
	 * leaving it up would waste the bottom 69 pixels of the one screen
	 * in the app that wants to be big. */
	kg_pad_hide();
	kg_set_repaint(draw, 0);

	draw(0);
	kg_sound_play(KG_SND_LEVELUP);

	/* Two seconds, as the original. Keys pressed during it are
	 * discarded by kg_pause_ms(), which matters here more than
	 * anywhere: a kid mid-answer has just pressed GO, and that
	 * keystroke must not carry into the next question. */
	kg_pause_ms(2000);

	kg_set_repaint(saved_fn, saved_user);
	if (pad_was) kg_pad_show(KG_CS_ALPHA);
	if (saved_fn) saved_fn(saved_user);
}
