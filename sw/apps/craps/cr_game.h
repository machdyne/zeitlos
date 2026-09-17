#ifndef CR_GAME_H
#define CR_GAME_H

/*
 * Zeitlos craps -- a shooter's round.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * No I/O. The point, the bets on the table, and what a roll does to
 * them.
 *
 * -- the order a roll is resolved in matters --
 *
 * Every bet is resolved against the point as it was BEFORE the roll,
 * and only then does the point move. Do it the other way round and a
 * seven-out pays the place bets it should have taken, because by the
 * time they are looked at the table is back on a come-out and place
 * bets are off.
 *
 * -- what stays up and what comes down --
 *
 * A winning pass or come bet is paid and taken down; it has done its
 * job. A winning PLACE or HARD bet is paid and LEFT WORKING, which is
 * what a real table does and what makes a place bet a running position
 * rather than a one-shot. One-roll bets are gone whatever happens.
 *
 * Getting that wrong does not crash anything. It quietly changes how
 * much is at risk on every subsequent roll, which is the sort of error
 * that only shows up as "the money goes faster than it should".
 */

#include <stdint.h>
#include <stdbool.h>
#include "cr_table.h"

#define CR_MAX_BETS 16

typedef struct {
    uint8_t type;
    uint8_t sel;      /* the bet's number; 0 for a come bet with none yet */
    int32_t amount;
} cr_bet_t;

typedef struct {

    int       point;        /* 0 on the come-out */
    cr_bet_t  bet[CR_MAX_BETS];
    int       nbets;

    int32_t   staked;       /* everything committed since the round began */
    int32_t   returned;     /* everything paid back */

    int       d1, d2;       /* the last roll */
    int       rolls;

    /* What the last roll did, for the display to talk about. */
    int32_t   last_won;
    int32_t   last_lost;
    bool      point_made;
    bool      seven_out;

} cr_game_t;

void cr_game_init(cr_game_t *g);

/* True if this bet may be placed right now. The phase rules live here
 * rather than in the caller: a pass line bet during a point, or odds
 * with no flat bet behind them, are not bets a table would take. */
bool cr_can_place(const cr_game_t *g, int type, int sel, int32_t amount);

/* Adds to an identical bet rather than making a second entry, so
 * repeatedly clicking one spot does not exhaust CR_MAX_BETS. */
bool cr_place(cr_game_t *g, int type, int sel, int32_t amount);

/* Takes a bet down. Only some may be: a pass line bet is contract and
 * stays until it resolves, while a don't pass -- already past the
 * dangerous roll -- may be pulled. Returns what came back. */
int32_t cr_take_down(cr_game_t *g, int type, int sel);
bool cr_can_take_down(int type);

/* Rolls, resolves everything, and moves the point. Returns what the
 * roll paid back, stake included. */
int32_t cr_roll(cr_game_t *g, int d1, int d2);

/* What is on a particular spot. */
int32_t cr_bet_on(const cr_game_t *g, int type, int sel);

/* What is at risk in total -- every bet still on the table. */
int32_t cr_at_risk(const cr_game_t *g);

/* The most that may be laid behind a flat bet right now, given what is
 * already there. Zero if there is nothing to back. */
int32_t cr_odds_room(const cr_game_t *g, int type, int sel);

#endif
