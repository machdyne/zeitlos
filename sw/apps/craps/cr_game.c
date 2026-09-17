/*
 * Zeitlos craps -- a shooter's round.
 * See cr_game.h for the resolution order and what stays working.
 */

#include "cr_game.h"

void cr_game_init(cr_game_t *g)
{
    int i;
    for (i = 0; i < (int)sizeof(cr_game_t); i++) ((char *)g)[i] = 0;
    g->point = 0;
}

static int find(const cr_game_t *g, int type, int sel)
{
    int i;
    for (i = 0; i < g->nbets; i++)
        if (g->bet[i].type == type && g->bet[i].sel == sel) return i;
    return -1;
}

int32_t cr_bet_on(const cr_game_t *g, int type, int sel)
{
    int i = find(g, type, sel);
    return i < 0 ? 0 : g->bet[i].amount;
}

int32_t cr_at_risk(const cr_game_t *g)
{
    int32_t t = 0;
    int i;
    for (i = 0; i < g->nbets; i++) t += g->bet[i].amount;
    return t;
}

/* Which flat bet a given odds bet sits behind. */
static int flat_for_odds(int type)
{
    switch (type) {
    case CR_PASS_ODDS:      return CR_PASS;
    case CR_DONT_ODDS:      return CR_DONT_PASS;
    case CR_COME_ODDS:      return CR_COME;
    case CR_DONT_COME_ODDS: return CR_DONT_COME;
    }
    return -1;
}

int32_t cr_odds_room(const cr_game_t *g, int type, int sel)
{
    int flat_type = flat_for_odds(type);
    int32_t flat, cap, have;

    if (flat_type < 0) return 0;

    /* The line odds sit behind the pass or don't pass bet, which has no
     * number of its own -- the POINT is its number. A come bet carries
     * its own. */
    if (flat_type == CR_PASS || flat_type == CR_DONT_PASS) {
        if (g->point == 0 || sel != g->point) return 0;
        flat = cr_bet_on(g, flat_type, 0);
    } else {
        flat = cr_bet_on(g, flat_type, sel);
    }

    if (flat <= 0) return 0;

    cap = cr_max_odds(sel, flat);
    have = cr_bet_on(g, type, sel);

    return cap > have ? cap - have : 0;
}

bool cr_can_place(const cr_game_t *g, int type, int sel, int32_t amount)
{
    if (amount <= 0) return false;
    if (!cr_bet_valid(type, sel)) return false;

    switch (type) {

    case CR_PASS:
    case CR_DONT_PASS:
        /* Only on the come-out. A pass line bet made after the point is
         * set would be taking the same 7-or-point proposition without
         * the come-out roll that pays for it -- which is why a table
         * will not accept one, and why the come bet exists. */
        return g->point == 0;

    case CR_COME:
    case CR_DONT_COME:
        /* And only once there IS a point, for the mirror reason. */
        return g->point != 0 && sel == 0;

    case CR_PASS_ODDS:
    case CR_DONT_ODDS:
    case CR_COME_ODDS:
    case CR_DONT_COME_ODDS:
        return cr_odds_room(g, type, sel) >= amount;

    case CR_PLACE:
    case CR_HARD:
    case CR_FIELD:
    case CR_ANY7:
    case CR_ANY_CRAPS:
    case CR_ELEVEN:
    case CR_TWO:
    case CR_TWELVE:
        return true;
    }

    return false;
}

bool cr_place(cr_game_t *g, int type, int sel, int32_t amount)
{
    int i;

    if (!cr_can_place(g, type, sel, amount)) return false;

    i = find(g, type, sel);
    if (i >= 0) {
        g->bet[i].amount += amount;
        g->staked += amount;
        return true;
    }

    if (g->nbets >= CR_MAX_BETS) return false;

    g->bet[g->nbets].type = (uint8_t)type;
    g->bet[g->nbets].sel = (uint8_t)sel;
    g->bet[g->nbets].amount = amount;
    g->nbets++;
    g->staked += amount;

    return true;
}

bool cr_can_take_down(int type)
{
    /* THE LINE BETS ARE CONTRACT. Once a pass line bet has a point it
     * stays until it resolves -- it has already had the come-out roll,
     * where most of its winning chances are, so pulling it would be
     * taking the good half and leaving the bad. A don't pass is the
     * opposite: it has survived the dangerous roll and is now the
     * favourite, so the house is happy to let it go.
     *
     * Everything else is the player's to move. */
    switch (type) {
    case CR_PASS:
    case CR_COME:
        return false;
    }
    return true;
}

int32_t cr_take_down(cr_game_t *g, int type, int sel)
{
    int i = find(g, type, sel);
    int32_t back;

    if (i < 0) return 0;
    if (!cr_can_take_down(type)) return 0;

    back = g->bet[i].amount;
    g->staked -= back;
    g->bet[i] = g->bet[g->nbets - 1];
    g->nbets--;

    return back;
}

/* A winning bet's return, stake included. */
static int32_t pay(const cr_bet_t *b, int roll)
{
    int num, den;
    cr_payout(b->type, b->sel, roll, &num, &den);
    return b->amount + (b->amount * num) / den;
}

/* Some bets are paid and left working; others are done once they
 * resolve. */
static bool stays_up(int type)
{
    return type == CR_PLACE || type == CR_HARD;
}

int32_t cr_roll(cr_game_t *g, int d1, int d2)
{
    int roll = d1 + d2;
    int point = g->point;        /* BEFORE the roll -- see cr_game.h */
    int32_t back = 0;
    int i;

    g->d1 = d1;
    g->d2 = d2;
    g->rolls++;
    g->last_won = 0;
    g->last_lost = 0;
    g->point_made = false;
    g->seven_out = false;

    for (i = 0; i < g->nbets; ) {
        cr_bet_t *b = &g->bet[i];
        int r;

        /* The hardways are the only bets that need the dice rather than
         * the total: a six made of 4-2 loses hard six and wins place
         * six, from the same roll. */
        if (b->type == CR_HARD) r = cr_hard_resolve(b->sel, d1, d2, point);
        else r = cr_resolve(b->type, b->sel, roll, point);

        if (r == CR_WIN) {
            int32_t p = pay(b, roll);
            back += p;
            g->last_won += p - b->amount;
            if (stays_up(b->type)) {
                /* Paid, and still working. The stake is counted again
                 * because it is at risk again. */
                back -= b->amount;
                g->staked += 0;
                i++;
                continue;
            }
        } else if (r == CR_PUSH) {
            back += b->amount;
        } else if (r == CR_LOSE) {
            g->last_lost += b->amount;
        } else if (r == CR_MOVE) {
            /* A come bet takes the number just rolled and stays. Its
             * odds, if any, are placed separately afterwards -- there
             * are none yet, because until this roll it had no number to
             * put them on. */
            b->sel = (uint8_t)roll;
            i++;
            continue;
        } else {
            i++;
            continue;                 /* stands */
        }

        /* Resolved and off the table. Swapping the last bet down is
         * safe because nothing here depends on the order. */
        g->bet[i] = g->bet[g->nbets - 1];
        g->nbets--;
    }

    /* AND ONLY NOW DOES THE POINT MOVE. */
    if (point == 0) {
        if (roll != 7 && roll != 11 && roll != 2 && roll != 3 && roll != 12)
            g->point = roll;
    } else if (roll == point) {
        g->point = 0;
        g->point_made = true;
    } else if (roll == 7) {
        g->point = 0;
        g->seven_out = true;
    }

    g->returned += back;

    return back;
}
