/*
 * Zeitlos blackjack -- basic strategy.
 * See bj_hint.h for why this is a table and what it does not model.
 */

#include "bj_hint.h"

/* -- the tables ---------------------------------------------------------
 *
 * Columns are the dealer's up-card, 2 through 10 then ace: index 0 is a
 * deuce and index 9 is an ace. Rows are the player's hand.
 *
 *   h  hit
 *   s  stand
 *   d  double, or hit if doubling is unavailable
 *   D  double, or STAND if unavailable -- soft 18 against 3 to 6 is
 *      the only place this differs, and getting it wrong turns a
 *      standing hand into a hit
 *   p  split
 *   r  surrender, or hit if unavailable
 *
 * Written out rather than derived. Every attempt to compress basic
 * strategy into rules produces something that is nearly right, and
 * "nearly right" in a strategy table is a losing player who believes
 * they are playing correctly.
 */

/* Hard totals, 5 through 21. Row 0 is a total of 5. */
static const char hard[17][10] = {
/* dealer:   2    3    4    5    6    7    8    9    T    A   */
/*  5 */  { 'h', 'h', 'h', 'h', 'h', 'h', 'h', 'h', 'h', 'h' },
/*  6 */  { 'h', 'h', 'h', 'h', 'h', 'h', 'h', 'h', 'h', 'h' },
/*  7 */  { 'h', 'h', 'h', 'h', 'h', 'h', 'h', 'h', 'h', 'h' },
/*  8 */  { 'h', 'h', 'h', 'h', 'h', 'h', 'h', 'h', 'h', 'h' },
/*  9 */  { 'h', 'd', 'd', 'd', 'd', 'h', 'h', 'h', 'h', 'h' },
/* 10 */  { 'd', 'd', 'd', 'd', 'd', 'd', 'd', 'd', 'h', 'h' },
/* 11 */  { 'd', 'd', 'd', 'd', 'd', 'd', 'd', 'd', 'd', 'h' },
/* 12 */  { 'h', 'h', 's', 's', 's', 'h', 'h', 'h', 'h', 'h' },
/* 13 */  { 's', 's', 's', 's', 's', 'h', 'h', 'h', 'h', 'h' },
/* 14 */  { 's', 's', 's', 's', 's', 'h', 'h', 'h', 'h', 'h' },
/* 15 */  { 's', 's', 's', 's', 's', 'h', 'h', 'h', 'r', 'h' },
/* 16 */  { 's', 's', 's', 's', 's', 'h', 'h', 'r', 'r', 'r' },
/* 17 */  { 's', 's', 's', 's', 's', 's', 's', 's', 's', 's' },
/* 18 */  { 's', 's', 's', 's', 's', 's', 's', 's', 's', 's' },
/* 19 */  { 's', 's', 's', 's', 's', 's', 's', 's', 's', 's' },
/* 20 */  { 's', 's', 's', 's', 's', 's', 's', 's', 's', 's' },
/* 21 */  { 's', 's', 's', 's', 's', 's', 's', 's', 's', 's' }
};

/* Soft totals, ace plus 2 through ace plus 9 -- that is, 13 to 20.
 * Row 0 is A-2. */
static const char soft[8][10] = {
/* dealer:   2    3    4    5    6    7    8    9    T    A   */
/* A2 */  { 'h', 'h', 'h', 'd', 'd', 'h', 'h', 'h', 'h', 'h' },
/* A3 */  { 'h', 'h', 'h', 'd', 'd', 'h', 'h', 'h', 'h', 'h' },
/* A4 */  { 'h', 'h', 'd', 'd', 'd', 'h', 'h', 'h', 'h', 'h' },
/* A5 */  { 'h', 'h', 'd', 'd', 'd', 'h', 'h', 'h', 'h', 'h' },
/* A6 */  { 'h', 'd', 'd', 'd', 'd', 'h', 'h', 'h', 'h', 'h' },
/* A7 */  { 's', 'D', 'D', 'D', 'D', 's', 's', 'h', 'h', 'h' },
/* A8 */  { 's', 's', 's', 's', 's', 's', 's', 's', 's', 's' },
/* A9 */  { 's', 's', 's', 's', 's', 's', 's', 's', 's', 's' }
};

/* Pairs, deuces through aces. Row 0 is a pair of twos; row 9 is aces.
 * Row 8 is tens -- NEVER split, which is the cell people most often
 * get wrong because twenty is already a winning hand. */
static const char pairs[10][10] = {
/* dealer:   2    3    4    5    6    7    8    9    T    A   */
/* 22 */  { 'p', 'p', 'p', 'p', 'p', 'p', 'h', 'h', 'h', 'h' },
/* 33 */  { 'p', 'p', 'p', 'p', 'p', 'p', 'h', 'h', 'h', 'h' },
/* 44 */  { 'h', 'h', 'h', 'p', 'p', 'h', 'h', 'h', 'h', 'h' },
/* 55 */  { 'd', 'd', 'd', 'd', 'd', 'd', 'd', 'd', 'h', 'h' },
/* 66 */  { 'p', 'p', 'p', 'p', 'p', 'h', 'h', 'h', 'h', 'h' },
/* 77 */  { 'p', 'p', 'p', 'p', 'p', 'p', 'h', 'h', 'h', 'h' },
/* 88 */  { 'p', 'p', 'p', 'p', 'p', 'p', 'p', 'p', 'p', 'p' },
/* 99 */  { 'p', 'p', 'p', 'p', 'p', 's', 'p', 'p', 's', 's' },
/* TT */  { 's', 's', 's', 's', 's', 's', 's', 's', 's', 's' },
/* AA */  { 'p', 'p', 'p', 'p', 'p', 'p', 'p', 'p', 'p', 'p' }
};

bool bj_hint_exact(const bj_rules_t *r)
{
    /* The table is the multi-deck, stand-on-soft-17, doubling-after-
     * split chart. Under other rules a handful of cells differ. */
    return !r->dealer_hits_soft17 && r->double_after_split &&
        r->ndecks >= 4;
}

/* The dealer's up-card as a column: 0 for a deuce, 9 for an ace. */
static int up_index(const bj_game_t *g)
{
    int v;

    if (g->ndealer < 1) return -1;

    v = bj_card_value(g->dealer[0]);
    if (v == 11) return 9;              /* ace */
    if (v < 2 || v > 10) return -1;

    return v - 2;
}

static char lookup(const bj_hand_t *h, int col)
{
    bool soft_hand = false;
    int total = bj_total(h->card, h->n, &soft_hand);

    /* A PAIR IS CHECKED FIRST, because the pair table disagrees with
     * the hard table for the same total: a pair of eights is sixteen
     * and the hard chart says stand or surrender, while the right play
     * is always to split. Consulting the totals first would give
     * exactly the wrong advice on the most famous hand in the game. */
    if (h->n == 2 && bj_card_value(h->card[0]) == bj_card_value(h->card[1])) {
        int v = bj_card_value(h->card[0]);
        int row = (v == 11) ? 9 : (v == 10) ? 8 : v - 2;
        if (row >= 0 && row < 10) return pairs[row][col];
    }

    if (soft_hand && total >= 13 && total <= 20)
        return soft[total - 13][col];

    if (total < 5) return 'h';
    if (total > 21) return 's';         /* bust -- nothing to advise */

    return hard[total - 5][col];
}

int bj_hint_ideal(const bj_game_t *g)
{
    int col;
    const bj_hand_t *h;
    char c;

    if (g->phase != BJ_PHASE_PLAYER) return -1;
    if (g->active < 0 || g->active >= g->nhands) return -1;

    h = &g->hand[g->active];
    if (h->done) return -1;

    col = up_index(g);
    if (col < 0) return -1;

    c = lookup(h, col);

    switch (c) {
    case 'h': return BJ_HINT_HIT;
    case 's': return BJ_HINT_STAND;
    case 'd':
    case 'D': return BJ_HINT_DOUBLE;
    case 'p': return BJ_HINT_SPLIT;
    case 'r': return BJ_HINT_SURRENDER;
    }

    return BJ_HINT_HIT;
}

int bj_hint(const bj_game_t *g)
{
    bj_options_t o;
    const bj_hand_t *h;
    int col;
    char c;

    if (g->phase != BJ_PHASE_PLAYER) return -1;
    if (g->active < 0 || g->active >= g->nhands) return -1;

    h = &g->hand[g->active];
    if (h->done) return -1;

    col = up_index(g);
    if (col < 0) return -1;

    bj_options(g, &o);
    c = lookup(h, col);

    /* REDUCED TO SOMETHING THE ENGINE WILL ACCEPT.
     *
     * A hint the player cannot follow is worse than no hint: it teaches
     * a rule and then refuses it. Every fallback below is the standard
     * one, and the distinction between 'd' and 'D' is the whole reason
     * the table carries two characters for doubling -- soft eighteen
     * against a five wants to DOUBLE, and if it cannot, it wants to
     * STAND, not hit. Collapsing them turns a standing hand into a hit
     * whenever the hand has already been hit once. */
    switch (c) {
    case 'p':
        if (o.can_split) return BJ_HINT_SPLIT;
        /* A pair that cannot be split is played on its total. */
        c = (bj_total(h->card, h->n, 0) >= 17) ? 's' : 'h';
        break;
    case 'r':
        if (o.can_surrender) return BJ_HINT_SURRENDER;
        c = 'h';
        break;
    case 'd':
        if (o.can_double) return BJ_HINT_DOUBLE;
        c = 'h';
        break;
    case 'D':
        if (o.can_double) return BJ_HINT_DOUBLE;
        c = 's';
        break;
    }

    if (c == 's') return o.can_stand ? BJ_HINT_STAND : BJ_HINT_HIT;
    return o.can_hit ? BJ_HINT_HIT : BJ_HINT_STAND;
}

const char *bj_hint_name(int hint)
{
    switch (hint) {
    case BJ_HINT_HIT:       return "hit";
    case BJ_HINT_STAND:     return "stand";
    case BJ_HINT_DOUBLE:    return "double";
    case BJ_HINT_SPLIT:     return "split";
    case BJ_HINT_SURRENDER: return "surrender";
    }
    return "";
}
