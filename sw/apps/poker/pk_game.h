#ifndef PK_GAME_H
#define PK_GAME_H

/*
 * Zeitlos poker -- one hand of poker, from posting to settlement.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * No I/O. This file and pk_game.c know nothing about the framebuffer,
 * the window manager or the keyboard, which is what lets the whole of
 * it be driven from tests/game_test.c with a stacked deck.
 *
 * -- one betting engine, four games --
 *
 * Texas hold'em and seven-card stud differ in which cards are dealt
 * face up on which street, who acts first, and whether the forced bets
 * are blinds or antes. They do not differ in what a raise is, how a
 * side pot is built or how a showdown is settled. So the variants are
 * a TABLE (pk_variant_t) and there is exactly one implementation of
 * everything else.
 *
 * The alternative -- a function per variant -- is four copies of the
 * side-pot code, three of which are never exercised as hard as the
 * hold'em one and all four of which drift.
 *
 * -- the shape of the loop --
 *
 * The engine is a state machine that advances itself as far as it can
 * and then stops on something it needs a person (or an opponent) to
 * decide:
 *
 *     pk_hand_begin(g);
 *     while (g->phase != PK_PHASE_COMPLETE) {
 *         switch (g->phase) {
 *         case PK_PHASE_BETTING: pk_act(g, action, amount); break;
 *         case PK_PHASE_DRAW:    pk_draw(g, which, n);      break;
 *         }
 *     }
 *
 * Dealing the flop, moving to the next street, ending the hand when
 * everybody folds and settling the pots all happen inside pk_act().
 * A caller never asks the engine to advance, because a caller that
 * CAN ask can also forget, and the symptom of forgetting is a game
 * that silently stops.
 *
 * -- chips are integers and are conserved --
 *
 * Every amount is int32_t and there is no fractional chip anywhere.
 * A split pot that does not divide evenly gives the odd chips to the
 * players nearest the button's left, which is the usual rule and,
 * more importantly, is a RULE rather than a rounding.
 *
 * tests/game_test.c checks total chips before and after every hand it
 * plays. Conservation is the one invariant that catches most pot
 * arithmetic bugs at once.
 */

#include <stdint.h>
#include <stdbool.h>
#include "../../common/games/zcard.h"
#include "pk_eval.h"
#include "../../common/games/zdeck.h"

#define PK_MAX_SEATS    8
#define PK_MAX_STREETS  5
#define PK_MAX_HOLE     7
#define PK_MAX_BOARD    5
#define PK_MAX_POTS     (PK_MAX_SEATS + 1)

/* -- variants -------------------------------------------------------- */

#define PK_FORCED_BLINDS    0   /* small and big blind */
#define PK_FORCED_ANTE      1   /* ante from everyone, then a bring-in */

#define PK_ORDER_BUTTON     0   /* position relative to the button */
#define PK_ORDER_BOARD      1   /* bring-in, then high hand showing */

#define PK_LIMIT_NONE       0   /* no limit */
#define PK_LIMIT_FIXED      1   /* small bet, big bet, capped raises */
#define PK_LIMIT_POT        2   /* pot limit */

#define PK_FIXED_CAP        4   /* a bet and three raises */

typedef struct {

    const char *name;           /* "holdem" -- what `variant` accepts */
    const char *label;          /* "Texas hold'em" -- what is displayed */

    uint8_t max_seats;
    uint8_t nstreets;           /* betting rounds */

    /* Dealt at the START of street s, before its betting round. */
    uint8_t deal_down[PK_MAX_STREETS];
    uint8_t deal_up[PK_MAX_STREETS];
    uint8_t deal_board[PK_MAX_STREETS];
    uint8_t burn[PK_MAX_STREETS];

    /* A draw phase runs before street s's betting round and after the
     * previous one. Five-card draw is the only shipped user. */
    bool    draw_before[PK_MAX_STREETS];

    uint8_t forced;             /* PK_FORCED_* */
    uint8_t order;              /* PK_ORDER_* */
    uint8_t limit;              /* default structure; overridable */

    /* Fixed limit: the street at which the bet size doubles. */
    uint8_t big_bet_street;

    /* 0 = best five of everything. Otherwise exactly this many hole
     * cards must play (Omaha). No shipped variant sets it yet. */
    uint8_t use_hole;

} pk_variant_t;

extern const pk_variant_t pk_variant_holdem;
extern const pk_variant_t pk_variant_draw5;
extern const pk_variant_t pk_variant_stud5;
extern const pk_variant_t pk_variant_stud7;

/* NULL if the name is not one of them. */
const pk_variant_t *pk_variant_find(const char *name);
int pk_variant_count(void);
const pk_variant_t *pk_variant_at(int i);

/* -- seats ----------------------------------------------------------- */

#define PK_SEAT_EMPTY   0   /* nobody there */
#define PK_SEAT_OUT     1   /* there, but not in this hand (broke) */
#define PK_SEAT_FOLDED  2
#define PK_SEAT_LIVE    3
#define PK_SEAT_ALLIN   4

typedef struct {

    uint8_t state;
    int32_t stack;

    uint8_t hole[PK_MAX_HOLE];
    bool    up[PK_MAX_HOLE];    /* exposed to the other players */
    uint8_t nhole;

    int32_t bet;                /* committed on the CURRENT street */
    int32_t committed;          /* committed for the whole hand */

    /* Has this seat acted since the last bet or full raise? A round
     * ends when every seat that can still act has acted AND has
     * matched the current bet. */
    bool    acted;

    /* False when an incomplete all-in raise has moved the bet without
     * reopening the betting for this seat -- it may call or fold and
     * nothing else. See pk_game.c. */
    bool    may_raise;

    uint32_t value;             /* filled in at showdown */
    uint8_t  best[5];
    int32_t  won;               /* this hand, for the display */

} pk_seat_t;

/* -- actions --------------------------------------------------------- */

#define PK_FOLD   0
#define PK_CHECK  1
#define PK_CALL   2
#define PK_BET    3
#define PK_RAISE  4

/* What a seat may do right now.
 *
 * `bet_to` and `raise_to` are TOTALS the seat's street bet becomes,
 * not increments. That is the one ambiguity in a poker interface
 * worth designing out: "raise 100" means different things at different
 * tables, "raise to 100" never does. pk_act() takes the same
 * convention.
 */
typedef struct {
    bool    can_fold;
    bool    can_check;
    bool    can_call;
    bool    can_bet;
    bool    can_raise;
    int32_t call_cost;      /* chips to put in to call */
    int32_t min_to;         /* smallest legal bet_to / raise_to */
    int32_t max_to;         /* largest, i.e. all in */
} pk_options_t;

/* -- pots ------------------------------------------------------------ */

typedef struct {
    int32_t amount;
    uint8_t eligible[PK_MAX_SEATS];  /* 1 if this seat can win it */
    uint8_t nwinners;
} pk_pot_t;

/* -- the hand -------------------------------------------------------- */

#define PK_PHASE_IDLE      0
#define PK_PHASE_BETTING   1   /* g->actor must act */
#define PK_PHASE_DRAW      2   /* g->actor must discard */
#define PK_PHASE_COMPLETE  3   /* settled; results are in the seats */

typedef struct {

    const pk_variant_t *v;
    uint8_t  limit;             /* PK_LIMIT_*, from the variant or set */

    pk_seat_t seat[PK_MAX_SEATS];
    int      nseats;

    zdeck_t deck;

    int32_t  small_blind;
    int32_t  big_blind;
    int32_t  ante;
    int32_t  bring_in;

    uint8_t  board[PK_MAX_BOARD];
    int      nboard;

    int      button;
    int      street;
    int      phase;
    int      actor;             /* seat that must act, or -1 */

    int32_t  pot;               /* everything committed, all streets */
    int32_t  bet_to_match;      /* highest street bet */
    int32_t  min_raise;         /* size of the last full raise */
    int      nraises;           /* this street, for the fixed-limit cap */
    int      last_aggressor;    /* -1 if the street was checked round */

    /* Filled in by the settlement. */
    pk_pot_t pots[PK_MAX_POTS];
    int      npots;
    bool     showdown;          /* were cards actually turned over */

} pk_game_t;

/* -- lifecycle ------------------------------------------------------- */

/* Seats every player with `stack` chips and sets the button. Does not
 * deal. Call once per session, not once per hand. */
void pk_game_init(pk_game_t *g, const pk_variant_t *v, int nseats,
    int32_t stack, int32_t small_blind, int32_t big_blind);

/* Overrides the variant's default betting structure. */
void pk_game_set_limit(pk_game_t *g, int limit);

/* Moves the button, posts the forced bets, deals the first street and
 * leaves the game in PK_PHASE_BETTING (or PK_PHASE_COMPLETE if fewer
 * than two seats can play).
 *
 * The deck is shuffled here. A test that wants a known board calls
 * zdeck_stack() on g->deck AFTER this returns -- which works because
 * the first street has already been dealt and the stack therefore
 * lands on the board rather than in somebody's hand. To control the
 * hole cards too, stack before calling and pass a pre-shuffled deck
 * through pk_hand_begin_stacked(). */
void pk_hand_begin(pk_game_t *g);

/* Same, but the deck is left exactly as the caller arranged it rather
 * than shuffled. For tests. */
void pk_hand_begin_stacked(pk_game_t *g);

/* -- playing --------------------------------------------------------- */

void pk_options(const pk_game_t *g, int seat, pk_options_t *o);

/* True if `seat` may take this action for this amount right now. */
bool pk_legal(const pk_game_t *g, int seat, int action, int32_t to);

/* Takes the action for g->actor and advances the game as far as it
 * can: to the next seat, the next street, the draw, or settlement.
 *
 * Returns false and changes NOTHING if the action is illegal. A
 * caller that ignores the return value gets a game that refuses to
 * move rather than one that has quietly done something else. */
bool pk_act(pk_game_t *g, int action, int32_t to);

/* Discards `n` of g->actor's cards by index and draws replacements.
 * n may be 0 (stand pat). Returns false if any index is out of range
 * or repeated. Only legal in PK_PHASE_DRAW. */
bool pk_draw(pk_game_t *g, const uint8_t *idx, int n);

/* -- queries --------------------------------------------------------- */

int  pk_live_count(const pk_game_t *g);     /* not folded */
int  pk_can_act_count(const pk_game_t *g);  /* not folded, not all in */

/* The seat's best hand from everything it holds plus the board,
 * honouring the variant's use_hole constraint. PK_EVAL_NONE if it has
 * folded or has too few cards. */
uint32_t pk_seat_value(const pk_game_t *g, int seat, uint8_t *best);

/* Cards this seat shows to the others: its up-cards, plus everything
 * at a showdown. Writes at most PK_MAX_HOLE and returns the count. */
int pk_seat_shown(const pk_game_t *g, int seat, uint8_t *out, bool showdown);

/* The next occupied seat clockwise, skipping `skip_folded` ones.
 * Returns -1 if there is none. */
int pk_next_seat(const pk_game_t *g, int from, bool skip_folded);

const char *pk_action_name(int action);

/* Street names for the display: "flop", "fifth street", "draw". */
const char *pk_street_name(const pk_game_t *g, int street);

#endif
