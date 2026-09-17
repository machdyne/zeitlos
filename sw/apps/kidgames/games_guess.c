/*
 * kidgames -- Number Guessing, Word Guess and Letter Hunt.
 *
 * The three games with a BOARD rather than a plain question: a range
 * with a higher/lower hint, a word revealing itself letter by letter,
 * and a word with a moving cursor over it. All three carry state
 * across several answers, which is why none of them fits the
 * question-and-answer shape games_word.c and games_number.c share --
 * they score a ROUND, not an answer.
 *
 * They still use kground.h for the header, the face and the saving,
 * and they do their own scoring, because in each the reward depends on
 * how the round went rather than only on whether it was won.
 */

#include <string.h>

#include "game.h"
#include "kground.h"
#include "kgui.h"
#include "kgpad.h"
#include "kgfont.h"
#include "wordlist.h"
#include "levelup.h"
#include "kgsave.h"
#include "kgrand.h"

#define GUESS_MAX_LEVEL 5
#define WORD_MAX 16

static kg_round_t *q_round;

/* -- Number Guessing ---------------------------------------------------- */

typedef struct { int min, max, tries; } range_t;

static const range_t NUM_LEVELS[GUESS_MAX_LEVEL] = {
	{ 1, 10,  8 },
	{ 1, 20,  8 },
	{ 1, 50,  9 },
	{ 1, 100, 10 },
	{ 1, 200, 10 },
};

/* Three, not five. The original's, and deliberate: a round here takes
 * several guesses, so five rounds at five guesses each is a long way
 * between rewards for a six-year-old. */
#define NUMGUESS_STREAK 3

static int ng_min, ng_max, ng_tries_left, ng_tries_total;
static int ng_hint;		/* -1 lower, +1 higher, 0 none yet */

static void numguess_draw(void *user)
{
	char smin[8], smax[8];
	int scale = 6;
	int wmin, wmax, dash, total, x;
	int y = KG_PLAY_Y + 20;

	(void)user;

	kg_clear();
	kg_round_header(q_round, "GUESS MY NUMBER!");

	kg_utoa(smin, sizeof(smin), (unsigned long)ng_min);
	kg_utoa(smax, sizeof(smax), (unsigned long)ng_max);

	wmin = kg_big_w(smin, scale);
	wmax = kg_big_w(smax, scale);
	dash = 5 * scale;
	total = wmin + dash + wmax;
	x = (KG_W - total) / 2;

	kg_big_puts(x, y, smin, scale, KG_BIG_SOLID);
	kg_fill(x + wmin + scale, y + kg_big_h(scale) / 2, dash - 2 * scale, 3, 1);
	kg_big_puts(x + wmin + dash, y, smax, scale, KG_BIG_SOLID);

	kg_tries_left(y + kg_big_h(scale) + 8, ng_tries_left, ng_tries_total);

	/*
	 * The higher/lower hint, as a TRIANGLE.
	 *
	 * The original shows arrow art plus the words HIGHER and LOWER in
	 * green and red. Neither the colour nor the words survive here --
	 * the audience includes pre-readers, and this is the one piece of
	 * information a round actually turns on. A triangle pointing up or
	 * down needs no reading and no colour.
	 *
	 * Drawn as stacked rows rather than through the line rasterizer,
	 * for the reason kg_frame() gives: there is text immediately
	 * below it and the two do not order against each other.
	 */
	if (ng_hint) {

		int hy = y + kg_big_h(scale) + 26;
		int i;
		int h = 18;

		for (i = 0; i < h; i++) {
			int w = ng_hint > 0 ? 2 * i + 1 : 2 * (h - i) - 1;
			kg_fill(KG_W / 2 - w / 2, hy + i, w, 1, 1);
		}

		kg_center_text(hy + h + 3, ng_hint > 0 ? "BIGGER" : "SMALLER", 1);

	}
}

void game_numguess_run(void)
{
	kg_round_t r;
	char guess[8], shown[8];

	kg_round_begin(&r, "numguess", "NUMBER GUESSING", GUESS_MAX_LEVEL);
	q_round = &r;

	for (;;) {

		const range_t *lv = &NUM_LEVELS[r.level - 1];
		int secret = lv->min + (int)kg_rand((uint32_t)(lv->max - lv->min + 1));
		int used = 0;
		bool escaped = false;
		bool won = false;

		ng_min = lv->min;
		ng_max = lv->max;
		ng_tries_total = lv->tries;
		ng_hint = 0;

		while (used < lv->tries) {

			int val = 0, i;

			ng_tries_left = lv->tries - used;

			kg_set_repaint(numguess_draw, 0);
			numguess_draw(0);

			if (!kg_input_line(60, -1, guess, 3, KG_CS_DIGITS)) {
				escaped = true;
				break;
			}

			for (i = 0; guess[i]; i++) val = val * 10 + (guess[i] - '0');

			used++;

			if (val == secret) { won = true; break; }

			ng_hint = val < secret ? 1 : -1;

		}

		if (escaped) break;

		if (won) {

			bool leveled = false;

			/*
			 * Paid by how few guesses it took, so finding it in three
			 * is worth more than grinding it out in ten. The
			 * original's formula.
			 */
			r.score += r.level * (lv->tries - used + 1);
			r.streak++;

			if (r.streak >= NUMGUESS_STREAK && r.level < GUESS_MAX_LEVEL) {
				r.level++;
				r.streak = 0;
				leveled = true;
			}

			kg_round_end(&r);

			/* kg_round_show(), not kg_round_finish(): the scoring
			 * above is this game's own -- paid by how few guesses it
			 * took -- and finish() would apply the standard one on
			 * top of it. */
			kg_round_show(&r, true, "YOU FOUND IT!", 0);

			if (leveled) levelup_celebrate(r.level);

		} else {

			r.streak = 0;
			kg_round_end(&r);

			kg_utoa(shown, sizeof(shown), (unsigned long)secret);
			kg_round_show(&r, false, "SO CLOSE!", shown);

		}

	}

	kg_round_end(&r);
}

/* -- Word Guess ---------------------------------------------------------- */

#define WG_TRIES 6

static char wg_word[WORD_MAX];
static bool wg_revealed[WORD_MAX];
static bool wg_guessed[26];
static int wg_wrong;

static void wordguess_draw(void *user)
{
	int wlen = (int)strlen(wg_word);
	int scale = kg_big_best_scale(wg_word, KG_W - 16, 44);
	int x = (KG_W - kg_big_w(wg_word, scale)) / 2;
	int y = KG_PLAY_Y + 18;
	int i;
	char tried[40];

	(void)user;

	kg_clear();
	kg_round_header(q_round, "GUESS THE LETTERS!");

	for (i = 0; i < wlen; i++)
		x += kg_big_putc(x, y, wg_word[i], scale,
			/* A revealed letter is the letter; an unrevealed one is a
			 * GHOST -- an empty outlined box, not a filled block.
			 * Filled would be right for "a letter goes here and you
			 * know which", which is Missing Letter. Here the kid
			 * knows nothing about it yet, and an empty box says so.
			 * The two stay apart by shape, not shade. */
			wg_revealed[i] ? KG_BIG_SOLID : KG_BIG_GHOST);

	kg_tries_left(y + kg_big_h(scale) + 8, WG_TRIES - wg_wrong, WG_TRIES);

	/* The letters already tried, so nobody wastes a guess repeating
	 * one -- and so a kid can see the alphabet being worked through.
	 * Twenty-six letters and a space each is 51 characters at 5px,
	 * which fits 320 with room to spare. */
	tried[0] = '\0';
	{
		int n = 0, c;
		for (c = 0; c < 26; c++) {
			if (!wg_guessed[c]) continue;
			if (n) tried[n++] = ' ';
			tried[n++] = (char)('A' + c);
		}
		tried[n] = '\0';
	}

	if (tried[0])
		kg_center_text(y + kg_big_h(scale) + 24, tried, 1);
}

void game_wordguess_run(void)
{
	kg_round_t r;
	const char *last = 0;
	char letter[4];

	kg_round_begin(&r, "wordguess", "WORD GUESS", WORDLIST_MAX_LEVEL);
	q_round = &r;

	for (;;) {

		const char *word = wordlist_pick_excluding(r.level, last);
		int wlen, i;
		bool escaped = false;
		bool won;

		last = word;
		kg_upper(wg_word, sizeof(wg_word), word);
		wlen = (int)strlen(wg_word);

		for (i = 0; i < WORD_MAX; i++) wg_revealed[i] = false;
		for (i = 0; i < 26; i++) wg_guessed[i] = false;
		wg_wrong = 0;

		for (;;) {

			bool all = true;
			int c, hit = 0;

			for (i = 0; i < wlen; i++) if (!wg_revealed[i]) { all = false; break; }
			if (all || wg_wrong >= WG_TRIES) break;

			kg_set_repaint(wordguess_draw, 0);
			wordguess_draw(0);

			if (!kg_input_line(60, -1, letter, 1, KG_CS_ALPHA)) {
				escaped = true;
				break;
			}

			c = letter[0] - 'A';
			if (c < 0 || c >= 26) continue;

			/* A repeated letter costs nothing and just asks again.
			 * Charging for it would punish a kid for forgetting what
			 * they tried, which is exactly what the tried-list above
			 * exists to help with. */
			if (wg_guessed[c]) continue;

			wg_guessed[c] = true;

			for (i = 0; i < wlen; i++)
				if (wg_word[i] == letter[0]) { wg_revealed[i] = true; hit = 1; }

			if (!hit) wg_wrong++;

		}

		if (escaped) break;

		won = true;
		for (i = 0; i < wlen; i++) if (!wg_revealed[i]) { won = false; break; }

		kg_round_finish(&r, won, "YOU GOT IT!", "SO CLOSE!", wg_word);

	}

	kg_round_end(&r);
}

/* -- Letter Hunt ---------------------------------------------------------- */

static char lh_word[WORD_MAX];
static char lh_target[2];
static int lh_cursor;

static void letterhunt_draw(void *user)
{
	int scale = kg_big_best_scale(lh_word, KG_W - 16, 40);
	int x = (KG_W - kg_big_w(lh_word, scale)) / 2;
	/*
	 * KG_PLAY_Y + 86, not + 72.
	 *
	 * The cursor is a DOUBLE frame drawn 5 pixels above the letter's
	 * own top, and at + 72 that landed inside "IN THIS WORD" on the
	 * row above -- the box cut a line through the caption whenever the
	 * cursor was anywhere on screen, which is always. A geometry test
	 * would not have caught it: nothing was outside the playfield and
	 * no two rectangles a test knew about overlapped. It took looking
	 * at the render.
	 *
	 * The marker's own thickness is part of the row's height and has
	 * to be budgeted for, which is the general lesson here.
	 */
	int y = KG_PLAY_Y + 86;
	int i, adv;

	(void)user;

	kg_clear();
	kg_round_header(q_round, "FIND THIS LETTER");

	/* The letter to hunt for, on its own and large. */
	kg_big_puts_centered(KG_PLAY_Y + 16, lh_target, 6, KG_BIG_SOLID);
	kg_center_text(KG_PLAY_Y + 60, "IN THIS WORD", 1);

	adv = kg_big_advance(scale);

	for (i = 0; lh_word[i]; i++) {

		/*
		 * The cursor is a BOX AROUND the letter, not a change of
		 * fill.
		 *
		 * Inverting or dithering the selected letter would fight the
		 * letterform itself -- and the whole task is reading that
		 * letterform and comparing it to the one above. The letter
		 * has to stay exactly as legible selected as unselected, so
		 * the marker goes outside it.
		 */
		if (i == lh_cursor) {
			kg_frame(x - 2, y - 3, KG_BIG_COLS * scale + 4,
				kg_big_h(scale) + 6, 1);
			kg_frame(x - 4, y - 5, KG_BIG_COLS * scale + 8,
				kg_big_h(scale) + 10, 1);
		}

		kg_big_putc(x, y, lh_word[i], scale, KG_BIG_SOLID);
		x += adv;

	}

	kg_status("SPACE MOVES   ENTER PICKS");
}

void game_letterhunt_run(void)
{
	kg_round_t r;
	const char *last = 0;

	kg_round_begin(&r, "letterhunt", "LETTER HUNT", WORDLIST_MAX_LEVEL);
	q_round = &r;

	for (;;) {

		const char *word = wordlist_pick_excluding(r.level, last);
		int wlen, chosen = -1;
		bool escaped = false;

		last = word;
		kg_upper(lh_word, sizeof(lh_word), word);
		wlen = (int)strlen(lh_word);

		lh_target[0] = lh_word[kg_rand((uint32_t)wlen)];
		lh_target[1] = '\0';
		lh_cursor = 0;

		/* No text field here, so no keyboard -- the whole playfield is
		 * the board and the answer is a position, not a character. */
		kg_pad_hide();
		kg_set_repaint(letterhunt_draw, 0);

		for (;;) {

			kg_key_t k;

			letterhunt_draw(0);
			k = kg_getkey();

			if (k == KG_KEY_QUIT || k == KG_KEY_ESC) { escaped = true; break; }
			if (k == KG_KEY_ENTER) { chosen = lh_cursor; break; }

			if (kg_key_is_next(k)) {
				lh_cursor = (lh_cursor + 1) % wlen;
			} else if (kg_key_is_prev(k)) {
				lh_cursor = (lh_cursor + wlen - 1) % wlen;
			} else if (k == KG_KEY_CLICK) {

				/* Click a letter directly. This game is one of the
				 * three that sees clicks at all (kgpad.h), and the
				 * hit test has to match letterhunt_draw()'s advance
				 * exactly -- hence both using kg_big_advance() rather
				 * than either computing a stride of its own. */
				int scale = kg_big_best_scale(lh_word, KG_W - 16, 40);
				int adv = kg_big_advance(scale);
				int x0 = (KG_W - kg_big_w(lh_word, scale)) / 2;
				int cx = kg_click_x(), cy = kg_click_y();
				int y = KG_PLAY_Y + 86;

				if (cy >= y - 5 && cy < y + kg_big_h(scale) + 5) {
					int idx = (cx - x0) / adv;
					if (cx >= x0 && idx >= 0 && idx < wlen) {
						lh_cursor = idx;
						chosen = idx;
						break;
					}
				}

			}

		}

		if (escaped) break;

		kg_round_finish(&r, lh_word[chosen] == lh_target[0],
			"FOUND IT!", "NOT QUITE!", 0);

	}

	kg_round_end(&r);
}
