#ifndef PK_AI_H
#define PK_AI_H

/*
 * Zeitlos poker -- the opponents.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * -- weak levels choose badly from a correct analysis --
 *
 * They are NOT given a broken hand evaluator. sw/apps/chess reached
 * the same conclusion for the same reason: an opponent with a broken
 * evaluation plays moves that are BAFFLING rather than moves that are
 * WEAK, and losing to a computer that plays incomprehensibly is not
 * enjoyable. A level 1 poker opponent here computes its equity exactly
 * like level 8 does -- with far fewer samples -- and then calls when
 * it should fold, never bluffs, and ignores its own conclusion a third
 * of the time. That is what a bad player actually does.
 *
 * -- wall clock first, sample count second --
 *
 * Also chess's rule, and for the same reason. A fixed number of
 * rollouts takes wildly different times on a board running from QQSPI
 * PSRAM without an instruction cache and one running from SDRAM with
 * one (docs/icache.md), so a level tuned on one board is a different
 * level on the other. Bounding by time instead means the levels stay
 * levels and a faster board simply gets a better estimate in the same
 * two seconds.
 *
 * With no clock installed the budget is ignored and the sample count
 * governs alone. That is what makes the host tests deterministic and
 * bounded, and it is the only reason the clock is injected rather than
 * read directly.
 *
 * -- there is no floating point anywhere in here --
 *
 * Some boards in this tree are rv32i (see sw/common/arch.mk), so a
 * single double would link the soft-float runtime. Every probability
 * is therefore an integer in PARTS PER THOUSAND: equity, pot odds and
 * thresholds are all 0..1000. `make nofloat` checks the object files
 * for soft-float helpers, because a stray `/ 2.0` compiles perfectly
 * and costs tens of kilobytes that nothing reports.
 *
 * -- not freezing the desktop --
 *
 * A search that does not service its message queue stalls the WINDOW
 * MANAGER, not just itself: wm blocks waiting for a redraw
 * acknowledgement (docs/window_manager.md). So the rollout loop calls
 * a poll callback, which pumps messages and can abort. Aborting is
 * always safe: the samples taken so far are already a usable estimate,
 * which is the whole reason this is sampled rather than solved.
 */

#include <stdint.h>
#include <stdbool.h>
#include "pk_game.h"

#define PK_MIN_LEVEL 1
#define PK_MAX_LEVEL 8

/* Milliseconds since something arbitrary but fixed. */
typedef uint32_t (*pk_clock_fn)(void);

/* Called every few hundred rollouts. Return false to abort. */
typedef bool (*pk_poll_fn)(void *ctx);

void pk_ai_set_clock(pk_clock_fn fn);
void pk_ai_set_poll(pk_poll_fn fn, void *ctx);

/* What a level is like, for the settings display. */
const char *pk_level_name(int level);

/* -- opponent modelling ----------------------------------------------
 *
 * Levels 7 and 8 watch how often each seat folds and how often it
 * raises, and adjust. Against somebody who folds to most bets they
 * bluff more; against somebody who calls everything they stop bluffing
 * entirely and value bet thinner.
 *
 * Deliberately crude. A model with more parameters than the few
 * hundred hands a session actually contains fits noise, and an
 * opponent that has concluded something confident and wrong from
 * eleven hands is worse than one that concluded nothing.
 */
typedef struct {
    uint16_t actions;
    uint16_t folds;
    uint16_t raises;
} pk_read_t;

typedef struct {
    int       level;
    pk_read_t read[PK_MAX_SEATS];
    uint32_t  rollouts_last;   /* samples actually taken, for `bench` */
    bool      aborted_last;
} pk_ai_t;

void pk_ai_init(pk_ai_t *a, int level);
void pk_ai_set_level(pk_ai_t *a, int level);

/* Forget everything learned about the other players. Call when the
 * table changes, not between hands -- the whole value of a read is
 * that it survives the hand it was taken in. */
void pk_ai_forget(pk_ai_t *a);

/* Record that `seat` took `action`. The app calls this for EVERY
 * action including its own human's, which is what gives levels 7 and
 * 8 something to read. */
void pk_ai_observe(pk_ai_t *a, const pk_game_t *g, int seat, int action);

/* Chooses an action for `seat`, which must be g->actor.
 *
 * Always produces something legal. If the analysis is aborted or the
 * position is one it has no opinion about, it checks or folds rather
 * than returning a failure the caller has to handle -- a decision
 * function that can decline is a decision function every caller
 * eventually forgets to check.
 */
void pk_ai_decide(pk_ai_t *a, const pk_game_t *g, int seat,
    int *action, int32_t *to);

/* -- equity ----------------------------------------------------------
 *
 * Both return parts per thousand: 1000 is certain to win, 500 is a
 * coin flip. A tie counts as a fraction of a win, split between the
 * players tying, which is what makes equity comparable with pot odds.
 */

/* Against `nopp` unknown opponents, using everything visible in the
 * game: the board, and the other seats' exposed cards in stud. */
int32_t pk_ai_equity_game(pk_ai_t *a, const pk_game_t *g, int seat,
    int rollouts);

/* Against one KNOWN opponent holding. For tests, and for the `odds`
 * command. `board` may be empty. */
int32_t pk_ai_equity_vs(const uint8_t *hero, int nhero,
    const uint8_t *opp, int nopp_cards,
    const uint8_t *board, int nboard, int nboard_final, int rollouts);

#endif
