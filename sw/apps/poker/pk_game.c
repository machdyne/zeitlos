/*
 * Zeitlos poker -- one hand of poker.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See pk_game.h for the shape of the state machine and why the
 * variants are a table. This file is the rules.
 */

#include "pk_game.h"

/* -- the variant table ----------------------------------------------- */

const pk_variant_t pk_variant_holdem = {
    "holdem", "Texas hold'em",
    PK_MAX_SEATS, 4,
    /* down */  { 2, 0, 0, 0, 0 },
    /* up   */  { 0, 0, 0, 0, 0 },
    /* board*/  { 0, 3, 1, 1, 0 },
    /* burn */  { 0, 1, 1, 1, 0 },
    /* draw */  { false, false, false, false, false },
    PK_FORCED_BLINDS, PK_ORDER_BUTTON, PK_LIMIT_NONE,
    /* big bet from the turn, if played fixed limit */ 2,
    /* use_hole */ 0
};

const pk_variant_t pk_variant_draw5 = {
    "draw5", "Five-card draw",
    6, 2,
    { 5, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0 },
    { false, true, false, false, false },
    PK_FORCED_BLINDS, PK_ORDER_BUTTON, PK_LIMIT_FIXED,
    1,
    0
};

/* Five-card stud: one down and one up, then three more up. Six seats
 * because 5 x 8 = 40 cards plus burns is uncomfortably close to the
 * deck and the game is traditionally short-handed anyway. */
const pk_variant_t pk_variant_stud5 = {
    "stud5", "Five-card stud",
    6, 4,
    { 1, 0, 0, 0, 0 },
    { 1, 1, 1, 1, 0 },
    { 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0 },
    { false, false, false, false, false },
    PK_FORCED_ANTE, PK_ORDER_BOARD, PK_LIMIT_FIXED,
    2,
    0
};

/* Seven-card stud: two down and one up, three more up, one more down.
 * Eight seats need 56 cards, which the deck does not have -- see
 * deal_street()'s community-card fallback, which is what the rules
 * actually prescribe rather than a workaround. */
const pk_variant_t pk_variant_stud7 = {
    "stud7", "Seven-card stud",
    PK_MAX_SEATS, 5,
    { 2, 0, 0, 0, 1 },
    { 1, 1, 1, 1, 0 },
    { 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0 },
    { false, false, false, false, false },
    PK_FORCED_ANTE, PK_ORDER_BOARD, PK_LIMIT_FIXED,
    2,
    0
};

static const pk_variant_t *const pk_variants[] = {
    &pk_variant_holdem,
    &pk_variant_draw5,
    &pk_variant_stud5,
    &pk_variant_stud7
};

int pk_variant_count(void)
{
    return (int)(sizeof pk_variants / sizeof pk_variants[0]);
}

const pk_variant_t *pk_variant_at(int i)
{
    if (i < 0 || i >= pk_variant_count()) return 0;
    return pk_variants[i];
}

static bool streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

const pk_variant_t *pk_variant_find(const char *name)
{
    int i;
    if (!name) return 0;
    for (i = 0; i < pk_variant_count(); i++)
        if (streq(pk_variants[i]->name, name)) return pk_variants[i];
    return 0;
}

/* -- small helpers --------------------------------------------------- */

static bool dealt_in(const pk_seat_t *s)
{
    return s->state == PK_SEAT_FOLDED || s->state == PK_SEAT_LIVE ||
           s->state == PK_SEAT_ALLIN;
}

static bool live(const pk_seat_t *s)
{
    return s->state == PK_SEAT_LIVE || s->state == PK_SEAT_ALLIN;
}

int pk_live_count(const pk_game_t *g)
{
    int i, n = 0;
    for (i = 0; i < g->nseats; i++) if (live(&g->seat[i])) n++;
    return n;
}

int pk_can_act_count(const pk_game_t *g)
{
    int i, n = 0;
    for (i = 0; i < g->nseats; i++)
        if (g->seat[i].state == PK_SEAT_LIVE) n++;
    return n;
}

int pk_next_seat(const pk_game_t *g, int from, bool skip_folded)
{
    int i, k;

    for (i = 1; i <= g->nseats; i++) {
        k = (from + i) % g->nseats;
        if (g->seat[k].state == PK_SEAT_EMPTY) continue;
        if (skip_folded && !live(&g->seat[k])) continue;
        if (!skip_folded && g->seat[k].state == PK_SEAT_OUT) continue;
        return k;
    }

    return -1;
}

/* The next seat that can still put chips in. */
static int next_can_act(const pk_game_t *g, int from)
{
    int i, k;

    for (i = 1; i <= g->nseats; i++) {
        k = (from + i) % g->nseats;
        if (g->seat[k].state == PK_SEAT_LIVE) return k;
    }

    return -1;
}

/* The bet unit for this street. Only fixed limit has two of them. */
static int32_t bet_size(const pk_game_t *g)
{
    if (g->limit != PK_LIMIT_FIXED) return g->big_blind;
    return (g->street >= g->v->big_bet_street) ?
        2 * g->big_blind : g->big_blind;
}

static void pay(pk_game_t *g, int i, int32_t amt)
{
    pk_seat_t *s = &g->seat[i];

    if (amt > s->stack) amt = s->stack;
    if (amt <= 0) amt = 0;

    s->stack -= amt;
    s->bet += amt;
    s->committed += amt;
    g->pot += amt;

    if (s->stack == 0 && s->state == PK_SEAT_LIVE) s->state = PK_SEAT_ALLIN;
}

/* An ante is DEAD money: it goes to the pot without becoming a street
 * bet. Paying it through pay() would make bet_to_match equal the ante
 * and turn the first voluntary action into a call, which is wrong in
 * every stud game and is the reason this is a separate function
 * rather than a flag. */
static void pay_dead(pk_game_t *g, int i, int32_t amt)
{
    pk_seat_t *s = &g->seat[i];

    if (amt > s->stack) amt = s->stack;
    if (amt <= 0) return;

    s->stack -= amt;
    s->committed += amt;
    g->pot += amt;

    if (s->stack == 0 && s->state == PK_SEAT_LIVE) s->state = PK_SEAT_ALLIN;
}

/* -- setup ----------------------------------------------------------- */

void pk_game_init(pk_game_t *g, const pk_variant_t *v, int nseats,
    int32_t stack, int32_t small_blind, int32_t big_blind)
{
    int i;

    for (i = 0; i < (int)sizeof(pk_game_t); i++) ((char *)g)[i] = 0;

    g->v = v ? v : &pk_variant_holdem;
    g->limit = g->v->limit;

    if (nseats < 2) nseats = 2;
    if (nseats > g->v->max_seats) nseats = g->v->max_seats;
    g->nseats = nseats;

    g->small_blind = small_blind;
    g->big_blind = big_blind;

    /* Stud has no blinds, so its forced bets are derived from the same
     * two numbers the caller already supplied rather than from four
     * more parameters nobody would have a feel for. The proportions
     * are the conventional ones: a small ante, a bring-in of about
     * half a small bet, and a small bet equal to the big blind. */
    g->ante = small_blind / 2;
    if (g->ante < 1) g->ante = 1;
    g->bring_in = small_blind;

    for (i = 0; i < nseats; i++) {
        g->seat[i].state = PK_SEAT_OUT;
        g->seat[i].stack = stack;
    }

    /* One seat BEFORE the first, because pk_hand_begin() moves the
     * button before it posts anything. Starting at 0 would make the
     * first hand of a session play with the button on seat 1, which
     * is harmless in play and deeply confusing in a test. */
    g->button = nseats - 1;
    g->phase = PK_PHASE_IDLE;
    g->actor = -1;
}

void pk_game_set_limit(pk_game_t *g, int limit)
{
    if (limit < PK_LIMIT_NONE || limit > PK_LIMIT_POT) return;
    g->limit = (uint8_t)limit;
}

/* -- dealing --------------------------------------------------------- */

static void deal_street(pk_game_t *g)
{
    const pk_variant_t *v = g->v;
    int s = g->street;
    int i, k, start;
    int ndown = v->deal_down[s], nup = v->deal_up[s];

    for (i = 0; i < v->burn[s]; i++) (void)pk_deck_deal(&g->deck);

    for (i = 0; i < v->deal_board[s]; i++) {
        if (g->nboard >= PK_MAX_BOARD) break;
        g->board[g->nboard++] = pk_deck_deal(&g->deck);
    }

    if (ndown == 0 && nup == 0) return;

    /* Seven-card stud, eight players, seventh street: 8 x 7 = 56 cards
     * and the deck holds 52. The rule is a single community card
     * instead of a card each, and it is a rule rather than a fallback
     * -- so it is implemented here rather than being avoided by
     * capping the table at seven seats. */
    if (ndown + nup == 1 &&
        pk_deck_remaining(&g->deck) < pk_live_count(g)) {
        if (g->nboard < PK_MAX_BOARD)
            g->board[g->nboard++] = pk_deck_deal(&g->deck);
        return;
    }

    /* Clockwise from the button's left, one card at a time round the
     * table, which is how it is dealt at a real table and which
     * matters here only because it makes a stacked deck in a test
     * predictable. */
    start = g->button;

    for (k = 0; k < ndown + nup; k++) {
        int seat = start;
        for (i = 0; i < g->nseats; i++) {
            seat = pk_next_seat(g, seat, true);
            if (seat < 0) break;
            if (g->seat[seat].nhole < PK_MAX_HOLE) {
                pk_seat_t *sp = &g->seat[seat];
                sp->up[sp->nhole] = (k >= ndown);
                sp->hole[sp->nhole++] = pk_deck_deal(&g->deck);
            }
            if (seat == start) break;
        }
    }
}

/* -- act order ------------------------------------------------------- */

/* Stud third street: the lowest exposed card brings it in, and a tie
 * IS broken by suit with clubs lowest -- the only place in poker where
 * one suit outranks another. pk_cards.h's suit order is alphabetical
 * for exactly this reason, so the comparison is on the card byte. */
static int bring_in_seat(const pk_game_t *g)
{
    int i, best = -1;
    uint8_t low = PK_CARD_NONE;

    for (i = 0; i < g->nseats; i++) {
        const pk_seat_t *s = &g->seat[i];
        int k;
        if (!live(s)) continue;
        for (k = 0; k < s->nhole; k++) {
            if (!s->up[k]) continue;
            if (best < 0 || s->hole[k] < low) { low = s->hole[k]; best = i; }
        }
    }

    return best;
}

/* Stud, fourth street onward: the best hand SHOWING acts first. Ties
 * go to the seat nearest the button's left, which is arbitrary but
 * has to be decided by something. */
static int high_board_seat(const pk_game_t *g)
{
    uint8_t up[PK_MAX_HOLE];
    int i, seat, best = -1;
    uint32_t bestval = 0;

    seat = g->button;

    for (i = 0; i < g->nseats; i++) {
        int k, n = 0;
        uint32_t val;

        seat = pk_next_seat(g, seat, true);
        if (seat < 0) break;

        for (k = 0; k < g->seat[seat].nhole; k++)
            if (g->seat[seat].up[k]) up[n++] = g->seat[seat].hole[k];

        val = pk_eval_upcards(up, n);
        if (best < 0 || val > bestval) { bestval = val; best = seat; }

        if (seat == g->button) break;
    }

    return best;
}

static int first_actor(pk_game_t *g)
{
    int who;

    if (g->v->order == PK_ORDER_BOARD) {
        if (g->street == 0) return -1;   /* set by the bring-in */
        who = high_board_seat(g);
        if (who >= 0 && g->seat[who].state != PK_SEAT_LIVE)
            who = next_can_act(g, who);
        return who;
    }

    /* Blinds. Postflop, and preflop alike, the first actor is the
     * first live seat after a reference point -- the button after the
     * first street, the big blind before it.
     *
     * That single rule is also correct HEADS UP, which is where this
     * usually goes wrong. With two players the button is the small
     * blind: "after the big blind" wraps to the button preflop, and
     * "after the button" is the big blind afterwards. Exactly the
     * right answer both times, with no special case. */
    if (g->street == 0) {
        int sb = (g->nseats == 2) ? g->button : pk_next_seat(g, g->button, false);
        int bb = pk_next_seat(g, sb, false);
        who = next_can_act(g, bb);
    } else {
        who = next_can_act(g, g->button);
    }

    return who;
}

/* -- betting rounds -------------------------------------------------- */

static bool round_complete(const pk_game_t *g)
{
    int i;

    for (i = 0; i < g->nseats; i++) {
        const pk_seat_t *s = &g->seat[i];
        if (s->state != PK_SEAT_LIVE) continue;
        if (!s->acted) return false;
        if (s->bet != g->bet_to_match) return false;
    }

    return true;
}

static bool start_betting_round(pk_game_t *g)
{
    int i;

    g->bet_to_match = 0;
    g->min_raise = bet_size(g);
    g->nraises = 0;
    g->last_aggressor = -1;

    for (i = 0; i < g->nseats; i++) {
        g->seat[i].bet = 0;
        g->seat[i].acted = false;
        g->seat[i].may_raise = true;
    }

    if (pk_can_act_count(g) < 2) return false;

    g->actor = first_actor(g);
    if (g->actor < 0) return false;

    g->phase = PK_PHASE_BETTING;
    return true;
}

/* The last bet nobody called comes back.
 *
 * Doing this at the end of every round, rather than trying to sort it
 * out at settlement, is what keeps the side-pot builder simple: after
 * this runs, no live seat has committed chips that no other seat could
 * have matched, so every pot layer has somebody eligible for it. */
static void return_uncalled(pk_game_t *g)
{
    int i, top = -1;
    int32_t hi = 0, second = 0;

    for (i = 0; i < g->nseats; i++) {
        int32_t b = g->seat[i].bet;
        if (b > hi) { second = hi; hi = b; top = i; }
        else if (b > second) second = b;
    }

    if (top < 0 || hi <= second) return;

    {
        int32_t back = hi - second;
        pk_seat_t *s = &g->seat[top];
        s->stack += back;
        s->bet -= back;
        s->committed -= back;
        g->pot -= back;

        /* A player whose own uncalled bet put them all in was never
         * actually all in. */
        if (s->state == PK_SEAT_ALLIN && s->stack > 0)
            s->state = PK_SEAT_LIVE;
    }
}

/* -- settlement ------------------------------------------------------ */

static void build_pots(pk_game_t *g)
{
    int32_t levels[PK_MAX_SEATS];
    int nlev = 0;
    int i, j;
    int32_t prev = 0;

    g->npots = 0;

    /* Distinct commitment levels, ascending. Insertion sort over at
     * most eight values. */
    for (i = 0; i < g->nseats; i++) {
        int32_t c = g->seat[i].committed;
        bool have = false;
        if (!dealt_in(&g->seat[i]) || c <= 0) continue;
        for (j = 0; j < nlev; j++) if (levels[j] == c) have = true;
        if (have) continue;
        for (j = nlev; j > 0 && levels[j - 1] > c; j--) levels[j] = levels[j - 1];
        levels[j] = c;
        nlev++;
    }

    for (i = 0; i < nlev; i++) {
        pk_pot_t *p;
        int32_t level = levels[i];
        int32_t amount = 0;

        for (j = 0; j < g->nseats; j++) {
            int32_t c = g->seat[j].committed;
            int32_t lo = (c < prev) ? c : prev;
            int32_t hi = (c < level) ? c : level;
            if (!dealt_in(&g->seat[j])) continue;
            amount += hi - lo;
        }

        if (amount <= 0) { prev = level; continue; }
        if (g->npots >= PK_MAX_POTS) break;

        p = &g->pots[g->npots++];
        p->amount = amount;
        p->nwinners = 0;
        for (j = 0; j < PK_MAX_SEATS; j++)
            p->eligible[j] = (j < g->nseats && live(&g->seat[j]) &&
                g->seat[j].committed >= level) ? 1 : 0;

        prev = level;
    }
}

static void award(pk_game_t *g, pk_pot_t *p)
{
    int i, seat, nwin = 0;
    int winner[PK_MAX_SEATS];
    uint32_t best = PK_EVAL_NONE;
    int32_t share, odd;

    for (i = 0; i < g->nseats; i++) {
        if (!p->eligible[i]) continue;
        if (g->seat[i].value > best) best = g->seat[i].value;
    }

    /* No eligible live seat at this level. return_uncalled() is meant
     * to make this impossible; if it ever happens, the chips go back
     * to whoever put them in rather than vanishing, and the
     * conservation check in the tests stays green while the pot count
     * gives it away. */
    if (best == PK_EVAL_NONE) {
        for (i = 0; i < g->nseats; i++) {
            if (!dealt_in(&g->seat[i])) continue;
            g->seat[i].stack += p->amount;
            g->seat[i].won += p->amount;
            return;
        }
        return;
    }

    /* Collected clockwise from the button's left, so the odd chips
     * below go out in that order without a second pass. */
    seat = g->button;
    for (i = 0; i < g->nseats; i++) {
        seat = pk_next_seat(g, seat, false);
        if (seat < 0) break;
        if (p->eligible[seat] && g->seat[seat].value == best)
            winner[nwin++] = seat;
        if (seat == g->button) break;
    }

    if (nwin == 0) return;

    p->nwinners = (uint8_t)nwin;
    share = p->amount / nwin;
    odd = p->amount - share * nwin;

    for (i = 0; i < nwin; i++) {
        int32_t amt = share + (i < odd ? 1 : 0);
        g->seat[winner[i]].stack += amt;
        g->seat[winner[i]].won += amt;
    }
}

static void settle(pk_game_t *g)
{
    int i;

    for (i = 0; i < g->nseats; i++) {
        g->seat[i].value = PK_EVAL_NONE;
        if (live(&g->seat[i]))
            g->seat[i].value = pk_seat_value(g, i, g->seat[i].best);
    }

    /* Cards are only turned over when more than one player is still
     * in. Everybody folding is not a showdown, and showing the winner's
     * hand there would be giving away information the game does not
     * give away. */
    g->showdown = pk_live_count(g) > 1;

    build_pots(g);
    for (i = 0; i < g->npots; i++) award(g, &g->pots[i]);

    for (i = 0; i < g->nseats; i++)
        if (g->seat[i].state == PK_SEAT_LIVE ||
            g->seat[i].state == PK_SEAT_ALLIN ||
            g->seat[i].state == PK_SEAT_FOLDED) {
            if (g->seat[i].stack <= 0) g->seat[i].state = PK_SEAT_OUT;
        }

    g->phase = PK_PHASE_COMPLETE;
    g->actor = -1;
}

/* -- street transitions ---------------------------------------------- */

static bool start_draw(pk_game_t *g)
{
    int i;

    for (i = 0; i < g->nseats; i++) g->seat[i].acted = false;

    /* All-in players draw too: they are still in the hand and their
     * cards still have to be able to win it. So this walks live seats,
     * not seats that can bet. */
    g->actor = pk_next_seat(g, g->button, true);
    if (g->actor < 0) return false;

    g->phase = PK_PHASE_DRAW;
    return true;
}

static void go_next_street(pk_game_t *g)
{
    for (;;) {
        return_uncalled(g);

        g->street++;

        if (g->street >= g->v->nstreets) { settle(g); return; }

        if (g->v->draw_before[g->street] && start_draw(g)) return;

        deal_street(g);

        if (start_betting_round(g)) return;

        if (pk_live_count(g) <= 1) { settle(g); return; }
    }
}

static void advance_after_action(pk_game_t *g)
{
    if (pk_live_count(g) <= 1) {
        return_uncalled(g);
        settle(g);
        return;
    }

    if (!round_complete(g)) {
        int who = next_can_act(g, g->actor);
        if (who >= 0) { g->actor = who; return; }
    }

    go_next_street(g);
}

/* -- beginning a hand ------------------------------------------------ */

static void begin_common(pk_game_t *g)
{
    int i, sb, bb, who;

    g->nboard = 0;
    g->pot = 0;
    g->street = 0;
    g->npots = 0;
    g->showdown = false;
    g->bet_to_match = 0;
    g->nraises = 0;
    g->last_aggressor = -1;

    for (i = 0; i < g->nseats; i++) {
        pk_seat_t *s = &g->seat[i];
        s->nhole = 0;
        s->bet = 0;
        s->committed = 0;
        s->acted = false;
        s->may_raise = true;
        s->value = PK_EVAL_NONE;
        s->won = 0;
        if (s->state == PK_SEAT_EMPTY) continue;
        s->state = (s->stack > 0) ? PK_SEAT_LIVE : PK_SEAT_OUT;
    }

    if (pk_live_count(g) < 2) { g->phase = PK_PHASE_COMPLETE; return; }

    /* The button only moves to a seat that is playing, so a busted
     * seat does not silently become the reference point for who posts
     * what. */
    who = pk_next_seat(g, g->button, true);
    if (who >= 0) g->button = who;

    g->min_raise = bet_size(g);

    if (g->v->forced == PK_FORCED_ANTE) {

        for (i = 0; i < g->nseats; i++)
            if (live(&g->seat[i])) pay_dead(g, i, g->ante);

        deal_street(g);

        who = bring_in_seat(g);
        if (who >= 0) {
            pay(g, who, g->bring_in);
            g->bet_to_match = g->seat[who].bet;
            g->nraises = 1;
            /* The bring-in is forced, so it is not an action. Leaving
             * acted false is what gives that seat its option if the
             * bet comes back round unraised. */
            g->seat[who].acted = false;
            g->actor = next_can_act(g, who);
        } else {
            g->actor = first_actor(g);
        }

    } else {

        sb = (g->nseats == 2) ? g->button : pk_next_seat(g, g->button, true);
        bb = pk_next_seat(g, sb, true);

        pay(g, sb, g->small_blind);
        pay(g, bb, g->big_blind);

        g->bet_to_match = g->big_blind;
        g->min_raise = g->big_blind;
        g->nraises = 1;

        /* Same reasoning as the bring-in: the big blind has not acted
         * voluntarily, so it keeps its option to raise when the action
         * comes back unraised. */
        g->seat[sb].acted = false;
        g->seat[bb].acted = false;

        deal_street(g);

        g->actor = first_actor(g);
    }

    if (g->actor < 0 || pk_can_act_count(g) < 2) {
        /* Everybody is already all in from the forced bets. Run the
         * rest of the board out and settle. */
        go_next_street(g);
        return;
    }

    g->phase = PK_PHASE_BETTING;
}

void pk_hand_begin(pk_game_t *g)
{
    pk_deck_init(&g->deck);
    pk_deck_shuffle(&g->deck);
    begin_common(g);
}

void pk_hand_begin_stacked(pk_game_t *g)
{
    begin_common(g);
}

/* -- options and legality -------------------------------------------- */

void pk_options(const pk_game_t *g, int seat, pk_options_t *o)
{
    const pk_seat_t *s;
    int32_t maxto, unit, callc;

    o->can_fold = o->can_check = o->can_call = false;
    o->can_bet = o->can_raise = false;
    o->call_cost = o->min_to = o->max_to = 0;

    if (seat < 0 || seat >= g->nseats) return;
    s = &g->seat[seat];
    if (s->state != PK_SEAT_LIVE) return;

    maxto = s->bet + s->stack;
    unit = bet_size(g);

    callc = g->bet_to_match - s->bet;
    if (callc < 0) callc = 0;
    if (callc > s->stack) callc = s->stack;
    o->call_cost = callc;

    o->can_fold = true;
    o->can_check = (g->bet_to_match <= s->bet);
    o->can_call = (g->bet_to_match > s->bet);

    if (g->bet_to_match == 0) {

        if (s->stack > 0) {
            o->can_bet = true;
            o->min_to = (unit < maxto) ? unit : maxto;
            o->max_to = maxto;
            if (g->limit == PK_LIMIT_FIXED)
                o->max_to = o->min_to;
            else if (g->limit == PK_LIMIT_POT) {
                int32_t cap = g->pot;
                o->max_to = (cap < maxto) ? cap : maxto;
                if (o->max_to < o->min_to) o->max_to = o->min_to;
            }
        }

    } else {

        /* Raising needs chips beyond the call, the right to reopen,
         * and -- in a fixed-limit game with more than two players --
         * room under the cap. */
        bool capped = (g->limit == PK_LIMIT_FIXED &&
            pk_can_act_count(g) > 2 && g->nraises >= PK_FIXED_CAP);

        if (s->stack > callc && s->may_raise && !capped) {
            int32_t full = g->bet_to_match + g->min_raise;
            o->can_raise = true;
            o->min_to = (full < maxto) ? full : maxto;
            o->max_to = maxto;
            if (g->limit == PK_LIMIT_FIXED) {
                int32_t to = g->bet_to_match + unit;
                o->min_to = (to < maxto) ? to : maxto;
                o->max_to = o->min_to;
            } else if (g->limit == PK_LIMIT_POT) {
                int32_t cap = g->bet_to_match + g->pot + callc;
                o->max_to = (cap < maxto) ? cap : maxto;
                if (o->max_to < o->min_to) o->max_to = o->min_to;
            }
        }
    }
}

bool pk_legal(const pk_game_t *g, int seat, int action, int32_t to)
{
    pk_options_t o;

    if (g->phase != PK_PHASE_BETTING) return false;
    if (seat != g->actor) return false;

    pk_options(g, seat, &o);

    switch (action) {
    case PK_FOLD:  return o.can_fold;
    case PK_CHECK: return o.can_check;
    case PK_CALL:  return o.can_call || o.can_check;
    case PK_BET:   return o.can_bet && to >= o.min_to && to <= o.max_to;
    case PK_RAISE: return o.can_raise && to >= o.min_to && to <= o.max_to;
    }

    return false;
}

/* -- acting ---------------------------------------------------------- */

bool pk_act(pk_game_t *g, int action, int32_t to)
{
    int i, seat = g->actor;
    pk_seat_t *s;
    int32_t old_match, required;
    bool full;

    if (g->phase != PK_PHASE_BETTING) return false;
    if (seat < 0 || seat >= g->nseats) return false;
    if (!pk_legal(g, seat, action, to)) return false;

    s = &g->seat[seat];

    /* A call with nothing to call is a check. Accepted rather than
     * refused because every caller -- the AI, the command line, the
     * mouse -- otherwise has to make the same distinction, and one of
     * them will eventually get it wrong in a way that shows up as the
     * game refusing to move. */
    if (action == PK_CALL && g->bet_to_match <= s->bet) action = PK_CHECK;

    switch (action) {

    case PK_FOLD:
        s->state = PK_SEAT_FOLDED;
        s->acted = true;
        break;

    case PK_CHECK:
        s->acted = true;
        break;

    case PK_CALL:
        pay(g, seat, g->bet_to_match - s->bet);
        s->acted = true;
        break;

    case PK_BET:
    case PK_RAISE:
        old_match = g->bet_to_match;
        required = (old_match == 0) ? bet_size(g) : old_match + g->min_raise;
        full = (to >= required);

        pay(g, seat, to - s->bet);
        g->bet_to_match = s->bet;
        g->last_aggressor = seat;
        s->acted = true;

        if (full) {
            g->min_raise = g->bet_to_match - old_match;
            g->nraises++;
            for (i = 0; i < g->nseats; i++) {
                if (i == seat) continue;
                if (g->seat[i].state != PK_SEAT_LIVE) continue;
                g->seat[i].acted = false;
                g->seat[i].may_raise = true;
            }
        } else {
            /* An all-in for less than a full raise.
             *
             * It moves the bet, so everybody still has to match it,
             * but it does NOT reopen the betting for anybody who had
             * already called the old amount -- they may call the
             * difference or fold and nothing else. Players who had not
             * yet acted are unaffected and keep every option.
             *
             * This is the single most-often-wrong rule in a betting
             * engine, it only ever comes up when somebody is short
             * stacked, and the symptom is a re-raise that should not
             * have been allowed. tests/game_test.c checks both halves. */
            for (i = 0; i < g->nseats; i++) {
                if (i == seat) continue;
                if (g->seat[i].state != PK_SEAT_LIVE) continue;
                if (!g->seat[i].acted) continue;
                g->seat[i].acted = false;
                g->seat[i].may_raise = false;
            }
        }
        break;

    default:
        return false;
    }

    advance_after_action(g);
    return true;
}

bool pk_draw(pk_game_t *g, const uint8_t *idx, int n)
{
    pk_seat_t *s;
    int i, j, who;
    uint8_t keep[PK_MAX_HOLE];
    int nkeep = 0;
    bool drop[PK_MAX_HOLE];

    if (g->phase != PK_PHASE_DRAW) return false;
    if (g->actor < 0) return false;

    s = &g->seat[g->actor];

    if (n < 0 || n > s->nhole) return false;

    /* A null list with a nonzero count is caller error, and it used to
     * be a null dereference. Refused instead: this is reached from the
     * app, from the opponents and from the tests, and "stand pat" is
     * spelled (NULL, 0) so the shape is one typo away from a crash. */
    if (n > 0 && !idx) return false;

    for (i = 0; i < PK_MAX_HOLE; i++) drop[i] = false;

    for (i = 0; i < n; i++) {
        if (idx[i] >= s->nhole) return false;
        if (drop[idx[i]]) return false;
        drop[idx[i]] = true;
    }

    for (i = 0; i < s->nhole; i++)
        if (!drop[i]) keep[nkeep++] = s->hole[i];

    for (i = 0; i < nkeep; i++) s->hole[i] = keep[i];

    for (j = nkeep; j < s->nhole; j++) {
        uint8_t c = pk_deck_deal(&g->deck);
        /* An exhausted deck cannot serve the draw. Standing pat is
         * the only correct answer -- reshuffling the discards is a
         * real casino rule and needs a discard pile this does not
         * keep, so the hand is short rather than wrong. */
        if (c == PK_CARD_NONE) { s->nhole = (uint8_t)j; break; }
        s->hole[j] = c;
        s->up[j] = false;
    }

    s->acted = true;

    who = g->actor;
    for (i = 0; i < g->nseats; i++) {
        who = pk_next_seat(g, who, true);
        if (who < 0) break;
        if (!g->seat[who].acted) { g->actor = who; return true; }
    }

    deal_street(g);

    if (!start_betting_round(g)) {
        if (pk_live_count(g) <= 1) settle(g);
        else go_next_street(g);
    }

    return true;
}

/* -- queries --------------------------------------------------------- */

uint32_t pk_seat_value(const pk_game_t *g, int seat, uint8_t *best)
{
    uint8_t all[PK_MAX_HOLE + PK_MAX_BOARD];
    const pk_seat_t *s;
    int i, n = 0;

    if (seat < 0 || seat >= g->nseats) return PK_EVAL_NONE;
    s = &g->seat[seat];
    if (!live(s)) return PK_EVAL_NONE;

    if (g->v->use_hole)
        return pk_eval_constrained(s->hole, s->nhole, g->board, g->nboard,
            g->v->use_hole, best);

    for (i = 0; i < s->nhole && n < (int)sizeof all; i++) all[n++] = s->hole[i];
    for (i = 0; i < g->nboard && n < (int)sizeof all; i++) all[n++] = g->board[i];

    if (n < 5) return PK_EVAL_NONE;
    if (n > 7) n = 7;

    return pk_eval_best(all, n, best);
}

int pk_seat_shown(const pk_game_t *g, int seat, uint8_t *out, bool showdown)
{
    const pk_seat_t *s;
    int i, n = 0;

    if (seat < 0 || seat >= g->nseats) return 0;
    s = &g->seat[seat];

    for (i = 0; i < s->nhole; i++)
        if (showdown || s->up[i]) out[n++] = s->hole[i];

    return n;
}

const char *pk_action_name(int action)
{
    switch (action) {
    case PK_FOLD:  return "fold";
    case PK_CHECK: return "check";
    case PK_CALL:  return "call";
    case PK_BET:   return "bet";
    case PK_RAISE: return "raise";
    }
    return "?";
}

const char *pk_street_name(const pk_game_t *g, int street)
{
    static const char *const holdem[] = {
        "preflop", "flop", "turn", "river", "" };
    static const char *const draw[] = {
        "pre-draw", "post-draw", "", "", "" };
    static const char *const stud[] = {
        "third street", "fourth street", "fifth street",
        "sixth street", "seventh street" };

    if (street < 0 || street >= PK_MAX_STREETS) return "";

    if (g->v->order == PK_ORDER_BOARD) return stud[street];
    if (g->v->draw_before[1]) return draw[street];
    return holdem[street];
}
