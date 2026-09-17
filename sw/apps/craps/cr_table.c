/*
 * Zeitlos craps -- the bets, and what each one does on a given roll.
 * See cr_table.h for why the edge is derived by the tests, not declared.
 */

#include "cr_table.h"

bool cr_is_hard(int d1, int d2)
{
    return d1 == d2;
}

static bool is_point_number(int n)
{
    return n == 4 || n == 5 || n == 6 || n == 8 || n == 9 || n == 10;
}

bool cr_bet_valid(int type, int sel)
{
    switch (type) {
    case CR_PASS:
    case CR_DONT_PASS:
    case CR_FIELD:
    case CR_ANY7:
    case CR_ANY_CRAPS:
    case CR_ELEVEN:
    case CR_TWO:
    case CR_TWELVE:
        return sel == 0;

    case CR_PASS_ODDS:
    case CR_DONT_ODDS:
        /* Odds only exist behind a point. There is nothing to lay odds
         * on during a come-out, which is why this is not simply
         * "sel is a number". */
        return is_point_number(sel);

    case CR_COME:
    case CR_DONT_COME:
        /* A come bet starts with no number and takes one on the next
         * roll, so both are legal. */
        return sel == 0 || is_point_number(sel);

    case CR_COME_ODDS:
    case CR_DONT_COME_ODDS:
        return is_point_number(sel);

    case CR_PLACE:
        return is_point_number(sel);

    case CR_HARD:
        return sel == 4 || sel == 6 || sel == 8 || sel == 10;
    }

    return false;
}

int cr_resolve(int type, int sel, int roll, int point)
{
    if (roll < 2 || roll > 12) return CR_STAND;
    if (!cr_bet_valid(type, sel)) return CR_STAND;

    switch (type) {

    case CR_PASS:
        if (point == 0) {
            if (roll == 7 || roll == 11) return CR_WIN;
            if (roll == 2 || roll == 3 || roll == 12) return CR_LOSE;
            return CR_STAND;          /* the point is set */
        }
        if (roll == point) return CR_WIN;
        if (roll == 7) return CR_LOSE;
        return CR_STAND;

    case CR_DONT_PASS:
        if (point == 0) {
            if (roll == 2 || roll == 3) return CR_WIN;
            /* THE BAR. Twelve is a push, not a win -- and that single
             * exception is the entire house edge on this bet. Without
             * it the don't pass would be a winning proposition and no
             * casino would offer it. */
            if (roll == 12) return CR_PUSH;
            if (roll == 7 || roll == 11) return CR_LOSE;
            return CR_STAND;
        }
        if (roll == 7) return CR_WIN;
        if (roll == point) return CR_LOSE;
        return CR_STAND;

    case CR_PASS_ODDS:
        if (point == 0) return CR_STAND;
        if (roll == sel) return CR_WIN;
        if (roll == 7) return CR_LOSE;
        return CR_STAND;

    case CR_DONT_ODDS:
        if (point == 0) return CR_STAND;
        if (roll == 7) return CR_WIN;
        if (roll == sel) return CR_LOSE;
        return CR_STAND;

    case CR_COME:
        if (sel == 0) {
            /* Behaves exactly like a pass line bet on its first roll,
             * whatever the table's point is -- which is the whole idea
             * of a come bet. */
            if (roll == 7 || roll == 11) return CR_WIN;
            if (roll == 2 || roll == 3 || roll == 12) return CR_LOSE;
            return CR_MOVE;
        }
        if (roll == sel) return CR_WIN;
        if (roll == 7) return CR_LOSE;
        return CR_STAND;

    case CR_DONT_COME:
        if (sel == 0) {
            if (roll == 2 || roll == 3) return CR_WIN;
            if (roll == 12) return CR_PUSH;
            if (roll == 7 || roll == 11) return CR_LOSE;
            return CR_MOVE;
        }
        if (roll == 7) return CR_WIN;
        if (roll == sel) return CR_LOSE;
        return CR_STAND;

    case CR_COME_ODDS:
        if (roll == sel) return CR_WIN;
        if (roll == 7) return CR_LOSE;
        return CR_STAND;

    case CR_DONT_COME_ODDS:
        if (roll == 7) return CR_WIN;
        if (roll == sel) return CR_LOSE;
        return CR_STAND;

    case CR_PLACE:
        /* OFF ON THE COME-OUT, which is the convention and is worth
         * honouring: a place bet working through a come-out would face
         * a roll where sevens are the likeliest thing on the table. */
        if (point == 0) return CR_STAND;
        if (roll == sel) return CR_WIN;
        if (roll == 7) return CR_LOSE;
        return CR_STAND;

    case CR_HARD:
        /* Decided by the TOTAL here; whether it came the hard way is
         * the caller's business, because this function is not given the
         * dice. cr_hard_resolve() below takes them. */
        if (roll == 7) return CR_LOSE;
        if (roll == sel) return CR_STAND;   /* caller checks hardness */
        return CR_STAND;

    case CR_FIELD:
        if (roll == 2 || roll == 3 || roll == 4 || roll == 9 ||
            roll == 10 || roll == 11 || roll == 12) return CR_WIN;
        return CR_LOSE;

    case CR_ANY7:
        return roll == 7 ? CR_WIN : CR_LOSE;

    case CR_ANY_CRAPS:
        return (roll == 2 || roll == 3 || roll == 12) ? CR_WIN : CR_LOSE;

    case CR_ELEVEN:
        return roll == 11 ? CR_WIN : CR_LOSE;

    case CR_TWO:
        return roll == 2 ? CR_WIN : CR_LOSE;

    case CR_TWELVE:
        return roll == 12 ? CR_WIN : CR_LOSE;
    }

    return CR_STAND;
}

int cr_hard_resolve(int sel, int d1, int d2, int point)
{
    int roll = d1 + d2;

    (void)point;      /* hardways work on the come-out too */

    if (roll == 7) return CR_LOSE;
    if (roll != sel) return CR_STAND;

    /* THE ONLY BET THAT CARES WHICH PAIR CAME UP. A six made of 4-2 is
     * a loser for hard six and a winner for place six, from the same
     * roll -- which is why the dice have to be carried around and not
     * just their total. */
    return cr_is_hard(d1, d2) ? CR_WIN : CR_LOSE;
}

void cr_payout(int type, int sel, int roll, int *num, int *den)
{
    *num = 1;
    *den = 1;

    switch (type) {

    case CR_PASS:
    case CR_DONT_PASS:
    case CR_COME:
    case CR_DONT_COME:
        return;                    /* even money */

    case CR_PASS_ODDS:
    case CR_COME_ODDS:
        /* TRUE ODDS, which is what makes the expectation exactly zero:
         * 2:1 against a probability of exactly 1/3, 3:2 against 2/5,
         * 6:5 against 5/11. */
        if (sel == 4 || sel == 10) { *num = 2; *den = 1; }
        else if (sel == 5 || sel == 9) { *num = 3; *den = 2; }
        else { *num = 6; *den = 5; }
        return;

    case CR_DONT_ODDS:
    case CR_DONT_COME_ODDS:
        /* Laying the odds: the inverse, because the bet is now the
         * favourite and has to pay less than it risks. */
        if (sel == 4 || sel == 10) { *num = 1; *den = 2; }
        else if (sel == 5 || sel == 9) { *num = 2; *den = 3; }
        else { *num = 5; *den = 6; }
        return;

    case CR_PLACE:
        /* NOT true odds -- this is where the house takes its cut, and
         * the size of the cut is visible in how far each of these sits
         * from the fair price above. Place 6 pays 7:6 against a fair
         * 6:5; place 4 pays 9:5 against a fair 2:1. */
        if (sel == 4 || sel == 10) { *num = 9; *den = 5; }
        else if (sel == 5 || sel == 9) { *num = 7; *den = 5; }
        else { *num = 7; *den = 6; }
        return;

    case CR_HARD:
        if (sel == 4 || sel == 10) { *num = 7; *den = 1; }
        else { *num = 9; *den = 1; }
        return;

    case CR_FIELD:
        /* The two ends pay extra, and without them the field would be a
         * losing bet by a mile: sixteen of the thirty-six ways win. */
        if (roll == 2) { *num = 2; *den = 1; }
        else if (roll == 12) { *num = 3; *den = 1; }
        return;

    case CR_ANY7:       *num = 4;  *den = 1; return;
    case CR_ANY_CRAPS:  *num = 7;  *den = 1; return;
    case CR_ELEVEN:     *num = 15; *den = 1; return;
    case CR_TWO:        *num = 30; *den = 1; return;
    case CR_TWELVE:     *num = 30; *den = 1; return;
    }
}

int32_t cr_max_odds(int point, int32_t flat)
{
    /* 3-4-5x: three times the flat bet on 4 and 10, four on 5 and 9,
     * five on 6 and 8.
     *
     * Those multiples are not arbitrary -- they are chosen so that the
     * WIN is always six times the flat bet whichever point is on, which
     * is what lets a dealer pay it without thinking. */
    if (point == 4 || point == 10) return flat * 3;
    if (point == 5 || point == 9) return flat * 4;
    if (point == 6 || point == 8) return flat * 5;
    return 0;
}

const char *cr_type_name(int type)
{
    switch (type) {
    case CR_PASS:            return "pass";
    case CR_DONT_PASS:       return "don't pass";
    case CR_PASS_ODDS:       return "odds";
    case CR_DONT_ODDS:       return "lay odds";
    case CR_COME:            return "come";
    case CR_DONT_COME:       return "don't come";
    case CR_COME_ODDS:       return "come odds";
    case CR_DONT_COME_ODDS:  return "come lay";
    case CR_PLACE:           return "place";
    case CR_HARD:            return "hard";
    case CR_FIELD:           return "field";
    case CR_ANY7:            return "any 7";
    case CR_ANY_CRAPS:       return "any craps";
    case CR_ELEVEN:          return "eleven";
    case CR_TWO:             return "two";
    case CR_TWELVE:          return "twelve";
    }
    return "?";
}
