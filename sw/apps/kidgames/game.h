#ifndef KG_GAME_H
#define KG_GAME_H

/*
 * kidgames -- the interface every game implements.
 *
 * Carried over from the original's src/game.h with one addition, and
 * the shape is deliberately unchanged otherwise: a game is a display
 * name plus a run() function, games.c is a static table of them, and
 * the menu calls run() directly. One binary, no fork/exec -- which
 * mattered on a slow embedded Linux box and matters more here, where
 * every process costs at least Z_MEM_MIN_BLOCK_SIZE (32KB) out of a
 * pool that may be 1MB in total (sw/os/mem.h).
 *
 * Conventions, all from the original and all worth keeping:
 *
 *   id     Short lowercase identifier, used as the save-file key.
 *          KEEP IT STABLE ONCE SHIPPED -- it is what a kid's saved
 *          progress is filed under, and changing it silently resets
 *          them to level 1.
 *
 *   label  What the menu shows. Kept short enough to fit the menu row
 *          at 5x8 in 320 pixels: 60 characters would fit, but see
 *          tests/test_layout.c, which holds it to something that also
 *          fits with the selection frame's inset.
 *
 *   run()  Owns the screen until the player backs out (Escape, or the
 *          MENU key on the on-screen keyboard). Must return with the
 *          runtime in a valid state -- i.e. do not call kg_shutdown(),
 *          and do leave the repaint callback as you found it if you
 *          replaced it.
 *
 * -- THE ADDITION --
 *
 * `needs` is a bitmask of optional hardware a game cannot run without.
 * It is zero for every game currently, and exists because the dock has
 * exactly the same problem one level up and solved it the same way
 * (dock_app_t's `feature` field in sw/apps/wm/wm.c): an entry for
 * something the machine cannot do is a button that starts a thing only
 * for it to fail. If a future game genuinely requires, say, audio,
 * games_visible() drops it rather than the game apologising after the
 * fact.
 */

#include "kg.h"

typedef struct {
	const char *id;
	const char *label;
	void (*run)(void);
	uint32_t needs;		/* Z_FEATURE_* bits, or 0 */
} game_t;

extern const game_t GAMES[];
extern const int GAMES_COUNT;

/*
 * Fills `out` with pointers to the games this machine can actually
 * run, and `labels` with their menu labels, returning how many. Both
 * arrays need GAMES_COUNT entries.
 *
 * Two arrays rather than one because the menu takes a plain
 * const char *const * and building that from a filtered list is the
 * caller's problem otherwise -- which is exactly the kind of parallel
 * bookkeeping that ends with the menu launching the game next to the
 * one that was clicked.
 */
int games_visible(const game_t **out, const char **labels);

/*
 * The games themselves. Declared here rather than in eight one-line
 * headers: a game's header would contain exactly this and nothing
 * else, and eight files that each say one thing are eight files to
 * keep in step with games.c for no benefit. The original had them
 * because each game was its own translation unit with its own header;
 * here several share a file.
 */
void game_spelling_run(void);
void game_unscramble_run(void);
void game_missing_run(void);
void game_counting_run(void);
void game_simple_math_run(void);
void game_numguess_run(void);
void game_wordguess_run(void);
void game_letterhunt_run(void);
void game_memorymatch_run(void);
void game_nameanimal_run(void);

#endif
