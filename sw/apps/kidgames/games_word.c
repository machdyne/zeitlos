/*
 * kidgames -- the three word games that share one screen shape.
 *
 * Spelling Practice, Word Unscramble and Missing Letter are the same
 * screen: a word in big letters, a text field under it, an answer
 * checked against the word list. In the original they are three files
 * that differ in about fifteen lines each and are otherwise identical,
 * including their bugs -- the praise wording and the pause lengths had
 * already drifted apart between them.
 *
 * They are one file here, with the differences named and visible. The
 * differences are real and worth reading in one place:
 *
 *   Spelling      shows the word, wants the word back
 *   Unscramble    shows the word shuffled, wants the word
 *   Missing       shows the word with one letter blanked, wants that
 *                 letter -- and accepts ANY letter that spells a word
 *                 the list knows, not just the one drawn
 *
 * The fourth word game, Word Guess, is NOT here: it has its own board,
 * its own tries counter and no text field, so folding it in would mean
 * a flag for every line. See game_wordguess.c.
 */

#include <string.h>

#include "game.h"
#include "kground.h"
#include "kgui.h"
#include "kgfont.h"
#include "wordlist.h"
#include "kgrand.h"

/* Longest word in the list is nine letters ("butterfly", "vegetable").
 * Sized for that plus slack, and static rather than stack for the
 * reason kgsave.c gives: these live inside a game that is already
 * several frames deep into a 16KB allocation. */
#define WORD_MAX 16

static char shown[WORD_MAX];	/* what is drawn: the word, or scrambled */
static char target[WORD_MAX];	/* the uppercased real word */
static char answer[WORD_MAX];
static int blank_idx;		/* Missing Letter only; -1 otherwise */

/* The question, drawn by both the game loop and the repaint callback.
 * Sized to the word rather than to a constant, so a three-letter word
 * is big and "vegetable" still fits. */
static const char *q_prompt;
static kg_round_t *q_round;

static void question_draw(void *user)
{
	int scale;

	(void)user;

	kg_clear();
	kg_round_header(q_round, q_prompt);

	scale = kg_big_best_scale(shown, KG_W - 16, 56);

	if (blank_idx >= 0)
		kg_big_puts_blank((KG_W - kg_big_w(shown, scale)) / 2,
			KG_PLAY_Y + 20, shown, scale, KG_BIG_SOLID,
			blank_idx, KG_BIG_BLANK);
	else
		kg_big_puts_centered(KG_PLAY_Y + 20, shown, scale, KG_BIG_SOLID);
}

/*
 * Fisher-Yates, retried a bounded number of times if it lands back on
 * the original order -- which is common for three-letter words and for
 * words with repeated letters, and hands the player a free answer.
 *
 * Bounded because some words genuinely have very few distinct
 * permutations ("ball" has twelve, and several read the same), and a
 * game that hung looking for a different one would be a worse bug than
 * an occasional easy round.
 */
static void scramble(const char *upper, char *out)
{
	int len = (int)strlen(upper);
	int attempt;

	for (attempt = 0; attempt < 20; attempt++) {

		int i;

		memcpy(out, upper, (size_t)len);
		out[len] = '\0';

		for (i = len - 1; i > 0; i--) {
			int j = (int)kg_rand((uint32_t)(i + 1));
			char t = out[i];
			out[i] = out[j];
			out[j] = t;
		}

		if (len <= 1) return;
		if (strcmp(out, upper) != 0) return;

	}
}

/* What the three games differ by.
 *
 * `word_mode_t`, not `mode_t`: POSIX has a mode_t in <sys/types.h> and
 * the host render harness pulls that in, so the short name is a
 * redefinition there. The target build never sees it and would have
 * compiled either way -- which is precisely why the collision is worth
 * avoiding rather than working around in the test. */
typedef enum { MODE_SPELL, MODE_SCRAMBLE, MODE_MISSING } word_mode_t;

static void run_word_game(word_mode_t mode, const char *id, const char *title,
	const char *prompt)
{
	kg_round_t r;
	const char *last = 0;

	kg_round_begin(&r, id, title, WORDLIST_MAX_LEVEL);

	q_round = &r;
	q_prompt = prompt;

	for (;;) {

		const char *word = wordlist_pick_excluding(r.level, last);
		int wlen;
		bool correct;

		last = word;
		kg_upper(target, sizeof(target), word);
		wlen = (int)strlen(target);

		blank_idx = -1;

		switch (mode) {

		case MODE_SCRAMBLE:
			scramble(target, shown);
			break;

		case MODE_MISSING:
			memcpy(shown, target, (size_t)wlen + 1);
			/*
			 * An INTERIOR letter only, never the first or last.
			 *
			 * English has huge rhyme families at word edges -- _AT,
			 * CA_, _OG -- each with many valid completions, and far
			 * fewer in the middle (C_T is about CAT, COT and CUT).
			 * This does not eliminate ambiguity, which is why the
			 * answer check below accepts any real word; it makes it
			 * rare enough that the check is a safety net rather than
			 * the main mechanism. Every word in the list is at least
			 * three letters, so this range is never empty.
			 */
			blank_idx = 1 + (int)kg_rand((uint32_t)(wlen - 2));
			break;

		default:
			memcpy(shown, target, (size_t)wlen + 1);
			break;

		}

		kg_set_repaint(question_draw, 0);
		question_draw(0);

		/* Missing Letter wants one character; the others want the
		 * whole word, with a little slack so a kid who types an extra
		 * letter sees it and can backspace rather than having the
		 * keystroke silently swallowed. */
		if (!kg_input_line(40, -1, answer, mode == MODE_MISSING ? 1 : wlen + 3,
			KG_CS_ALPHA)) break;

		if (mode == MODE_MISSING) {

			/*
			 * Accept any letter that reconstructs a word the list
			 * knows, not only the one that happened to be drawn.
			 *
			 * Blank the first interior letter of "cat" and "cot" and
			 * "cut" are equally correct English. A game that insisted
			 * on the drawn word would be telling a five-year-old that
			 * a word they know is wrong, which is the one thing this
			 * app must never do.
			 */
			char candidate[WORD_MAX];

			memcpy(candidate, target, (size_t)wlen + 1);
			candidate[blank_idx] = answer[0];

			correct = wordlist_contains(r.level, candidate);

		} else {
			/* Both sides are already uppercase -- kg_upper() for the
			 * target, and the pump uppercases every letter key -- so
			 * a plain strcmp avoids the non-C99 strcasecmp. */
			correct = strcmp(answer, target) == 0;
		}

		kg_round_finish(&r, correct, "GREAT JOB!", "NICE TRY!", target);

	}

	kg_round_end(&r);
}

void game_spelling_run(void)
{
	run_word_game(MODE_SPELL, "spelling", "SPELLING", "TYPE THIS WORD");
}

void game_unscramble_run(void)
{
	run_word_game(MODE_SCRAMBLE, "unscramble", "UNSCRAMBLE",
		"WHAT WORD IS THIS?");
}

void game_missing_run(void)
{
	run_word_game(MODE_MISSING, "missingletter", "MISSING LETTER",
		"TYPE THE MISSING LETTER");
}
