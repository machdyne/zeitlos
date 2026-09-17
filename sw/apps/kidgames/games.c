/*
 * kidgames -- the game registry.
 *
 * To add a game:
 *   1. Write game_yourgame.h/.c, using game_spelling.c as the template.
 *   2. #include its header here.
 *   3. Add one line to GAMES[].
 *   4. Add the .c to OBJS in the Makefile.
 * The menu picks it up automatically. Nothing else changes -- which is
 * the point of the table, and is why the menu scrolls (kgwidget.c)
 * even though ten games currently fit without it.
 */

#include "game.h"
#include "../../common/zsoc.h"

/*
 * Order is the original's, which is not alphabetical and not by
 * difficulty. It opens with Spelling because it is the most immediately
 * legible of the ten to an adult setting a kid up, and it interleaves
 * word games with number games so a kid scrolling the list keeps
 * meeting something different from the last one.
 */
const game_t GAMES[] = {
	{ "spelling",      "Spelling Practice", game_spelling_run,   0 },
	{ "numguess",      "Number Guessing",   game_numguess_run,   0 },
	{ "unscramble",    "Word Unscramble",   game_unscramble_run, 0 },
	{ "missingletter", "Missing Letter",    game_missing_run,    0 },
	{ "nameanimal",    "Name That Animal",  game_nameanimal_run, 0 },
	{ "letterhunt",    "Letter Hunt",       game_letterhunt_run, 0 },
	{ "counting",      "Counting",          game_counting_run,   0 },
	{ "simplemath",    "Simple Math",       game_simple_math_run, 0 },
	{ "memorymatch",   "Memory Match",      game_memorymatch_run, 0 },
	{ "wordguess",     "Word Guess",        game_wordguess_run,  0 },
};

const int GAMES_COUNT = (int)(sizeof(GAMES) / sizeof(GAMES[0]));

int games_visible(const game_t **out, const char **labels)
{
	int i, n = 0;

	for (i = 0; i < GAMES_COUNT; i++) {

		/* z_soc_has_feature() asks the hardware rather than testing a
		 * compile-time define, so one binary behaves correctly on
		 * every board -- the same reason docs/game_mode.md prefers
		 * z_game_available() over Z_FEATURE_GAME. */
		if (GAMES[i].needs && !z_soc_has_feature(GAMES[i].needs)) continue;

		out[n] = &GAMES[i];
		labels[n] = GAMES[i].label;
		n++;

	}

	return n;
}
