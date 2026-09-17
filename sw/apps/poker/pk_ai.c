/*
 * Zeitlos poker -- the opponents.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See pk_ai.h for why weak levels are weak the way they are, why the
 * clock is injected, and why there is no floating point in here.
 */

#include "pk_ai.h"

#define PK_PERMILLE 1000

/* How often the rollout loop stops to service the message queue.
 *
 * THIS IS A DESKTOP-RESPONSIVENESS NUMBER, NOT A TUNING KNOB. wm
 * blocks in wait_for_redraw_done() after it repairs a region, so an
 * opponent that goes away for a second without pumping freezes every
 * other window on screen for that second -- and, worse, wm's chrome
 * pass has already CLEARED this window's content by then, so the table
 * is blank until the redraw is finally answered.
 *
 * It was originally written as two nested guards, `(r & 63) == 0`
 * around `(r & 255) == 0`, which at the default level's 450 rollouts
 * fired exactly once per decision and at levels 1 to 3 never fired at
 * all. Six-handed, five opponents act before the person does, so the
 * app serviced about five messages in the several seconds after
 * startup. The symptoms were a permanently blank window and wm timing
 * out waiting for the redraw acknowledgement.
 *
 * tests/ai_test.c asserts a minimum rate now, because "how often does
 * this pump" is not something anybody re-derives when they change a
 * rollout count.
 */
#define PK_POLL_EVERY 16

static pk_clock_fn ai_clock = 0;
static pk_poll_fn  ai_poll = 0;
static void       *ai_poll_ctx = 0;

void pk_ai_set_clock(pk_clock_fn fn) { ai_clock = fn; }

void pk_ai_set_poll(pk_poll_fn fn, void *ctx)
{
    ai_poll = fn;
    ai_poll_ctx = ctx;
}

/* -- the ladder ------------------------------------------------------
 *
 * Every number here is a deliberate handicap except `rollouts` and
 * `budget_ms`, which are the honest ones.
 *
 * `slack` is added to the equity before it is compared with the pot
 * odds, so a positive slack is a player who calls with the worst of
 * it. That single number is most of what separates a beginner from a
 * competent player, and it produces recognisable BAD POKER rather than
 * random poker -- which is what a weak level should feel like.
 */
typedef struct {
    const char *name;
    int      rollouts;
    uint32_t budget_ms;
    int      noise;      /* % of decisions taken at random */
    int      bluff;      /* % of checked-to spots turned into a bluff */
    int32_t  slack;      /* per-mille added to equity vs the pot odds */
    int32_t  raise_eq;   /* equity needed to raise for value */
    bool     model;      /* watch the other players */
} pk_level_t;

static const pk_level_t levels[PK_MAX_LEVEL] = {
    /* 1 */ { "Beginner",  60,  200, 35,  0,  260, 820, false },
    /* 2 */ { "Casual",   120,  300, 22,  0,  170, 780, false },
    /* 3 */ { "Novice",   250,  500, 12,  4,   90, 730, false },
    /* 4 */ { "Club",     450,  800,  6,  6,   40, 690, false },
    /* 5 */ { "Steady",   700, 1100,  0,  6,    0, 655, false },
    /* 6 */ { "Strong",  1100, 1500,  0,  9,  -10, 630, false },
    /* 7 */ { "Hard",    1800, 2000,  0, 12,  -20, 610, true  },
    /* 8 */ { "Toughest",3000, 2600,  0, 15,  -25, 590, true  }
};

static const pk_level_t *lv(int level)
{
    if (level < PK_MIN_LEVEL) level = PK_MIN_LEVEL;
    if (level > PK_MAX_LEVEL) level = PK_MAX_LEVEL;
    return &levels[level - 1];
}

const char *pk_level_name(int level)
{
    return lv(level)->name;
}

void pk_ai_init(pk_ai_t *a, int level)
{
    int i;
    for (i = 0; i < (int)sizeof(pk_ai_t); i++) ((char *)a)[i] = 0;
    pk_ai_set_level(a, level);
}

void pk_ai_set_level(pk_ai_t *a, int level)
{
    if (level < PK_MIN_LEVEL) level = PK_MIN_LEVEL;
    if (level > PK_MAX_LEVEL) level = PK_MAX_LEVEL;
    a->level = level;
}

void pk_ai_forget(pk_ai_t *a)
{
    int i;
    for (i = 0; i < PK_MAX_SEATS; i++) {
        a->read[i].actions = 0;
        a->read[i].folds = 0;
        a->read[i].raises = 0;
    }
}

void pk_ai_observe(pk_ai_t *a, const pk_game_t *g, int seat, int action)
{
    pk_read_t *r;

    (void)g;
    if (seat < 0 || seat >= PK_MAX_SEATS) return;

    r = &a->read[seat];

    /* Saturating rather than wrapping. A long session must not make a
     * read flip to nonsense at 65,536 actions, and the RATIO is all
     * that is used, so halving both counters preserves it exactly. */
    if (r->actions >= 60000) {
        r->actions /= 2; r->folds /= 2; r->raises /= 2;
    }

    r->actions++;
    if (action == PK_FOLD) r->folds++;
    if (action == PK_BET || action == PK_RAISE) r->raises++;
}

/* -- rollouts -------------------------------------------------------- */

/* One card off an already-excluded deck, without shuffling the whole
 * thing first. A rollout needs a dozen cards out of forty-odd, so a
 * full Fisher-Yates per sample would be most of the cost. */
static uint8_t take(zdeck_t *d, int *pos)
{
    int n = d->n - *pos;
    int j;
    uint8_t c;

    if (n <= 0) return Z_CARD_NONE;

    j = *pos + (int)zg_rng_below((uint32_t)n);
    c = d->card[j];
    d->card[j] = d->card[*pos];
    d->card[*pos] = c;
    (*pos)++;

    return c;
}

/* Wins count 1000, ties count 1000/n split n ways, losses count 0.
 * Accumulated in per-mille so the whole thing stays integer. */
static int32_t score(uint32_t hero, const uint32_t *opp, int n)
{
    int i, ties = 1;

    for (i = 0; i < n; i++) {
        if (opp[i] > hero) return 0;
        if (opp[i] == hero) ties++;
    }

    return PK_PERMILLE / ties;
}

static bool out_of_time(uint32_t start, uint32_t budget)
{
    if (!ai_clock || budget == 0) return false;
    return (uint32_t)(ai_clock() - start) >= budget;
}

int32_t pk_ai_equity_vs(const uint8_t *hero, int nhero,
    const uint8_t *opp, int nopp_cards,
    const uint8_t *board, int nboard, int nboard_final, int rollouts)
{
    uint8_t seen[Z_NCARDS];
    uint8_t hcards[PK_MAX_HOLE + PK_MAX_BOARD];
    uint8_t ocards[PK_MAX_HOLE + PK_MAX_BOARD];
    zdeck_t d;
    int nseen = 0, i, r;
    int need = nboard_final - nboard;
    int64_t total = 0;

    if (need < 0) need = 0;
    if (rollouts < 1) rollouts = 1;

    for (i = 0; i < nhero; i++) seen[nseen++] = hero[i];
    for (i = 0; i < nopp_cards; i++) seen[nseen++] = opp[i];
    for (i = 0; i < nboard; i++) seen[nseen++] = board[i];

    zdeck_init_excluding(&d, seen, nseen);
    if (zdeck_remaining(&d) < need) return PK_PERMILLE / 2;

    for (r = 0; r < rollouts; r++) {
        int pos = 0, nh = 0, no = 0, k;
        uint8_t run[PK_MAX_BOARD];
        uint32_t hv, ov;

        for (k = 0; k < need; k++) run[k] = take(&d, &pos);

        for (k = 0; k < nhero; k++) hcards[nh++] = hero[k];
        for (k = 0; k < nopp_cards; k++) ocards[no++] = opp[k];
        for (k = 0; k < nboard; k++) {
            hcards[nh++] = board[k];
            ocards[no++] = board[k];
        }
        for (k = 0; k < need; k++) {
            hcards[nh++] = run[k];
            ocards[no++] = run[k];
        }

        hv = pk_eval_best(hcards, nh > 7 ? 7 : nh, 0);
        ov = pk_eval_best(ocards, no > 7 ? 7 : no, 0);

        total += score(hv, &ov, 1);
    }

    return (int32_t)(total / rollouts);
}

int32_t pk_ai_equity_game(pk_ai_t *a, const pk_game_t *g, int seat,
    int rollouts)
{
    const pk_variant_t *v = g->v;
    uint8_t seen[Z_NCARDS];
    uint8_t opp_known[PK_MAX_SEATS][PK_MAX_HOLE];
    int opp_nknown[PK_MAX_SEATS];
    zdeck_t d;
    int nseen = 0, nopp = 0, i, s, r;
    int final_hole = 0, final_board = 0;
    int need_board, need_hero, need_total;
    int64_t total = 0;
    uint32_t start = ai_clock ? ai_clock() : 0;
    const pk_level_t *L = lv(a->level);

    a->rollouts_last = 0;
    a->aborted_last = false;

    if (seat < 0 || seat >= g->nseats) return 0;
    if (rollouts < 1) rollouts = 1;

    for (s = 0; s < v->nstreets; s++) {
        final_hole += v->deal_down[s] + v->deal_up[s];
        final_board += v->deal_board[s];
    }
    /* Seven-card stud eight-handed substitutes a community card for
     * the last hole card, so the board can be larger than the table
     * says. Trust what is actually there. */
    if (g->nboard > final_board) final_board = g->nboard;

    for (i = 0; i < g->seat[seat].nhole; i++)
        seen[nseen++] = g->seat[seat].hole[i];
    for (i = 0; i < g->nboard; i++) seen[nseen++] = g->board[i];

    for (s = 0; s < g->nseats; s++) {
        int n;
        if (s == seat) continue;
        if (g->seat[s].state != PK_SEAT_LIVE &&
            g->seat[s].state != PK_SEAT_ALLIN) continue;

        /* Their exposed cards are real information and stud hands
         * turn on them, so they are held fixed and only the hidden
         * part is sampled. Treating a visible pair of kings as an
         * unknown hand would make every stud read wrong. */
        n = pk_seat_shown(g, s, opp_known[nopp], false);
        opp_nknown[nopp] = n;
        for (i = 0; i < n; i++) seen[nseen++] = opp_known[nopp][i];
        nopp++;
    }

    if (nopp == 0) return PK_PERMILLE;

    zdeck_init_excluding(&d, seen, nseen);

    need_board = final_board - g->nboard;
    if (need_board < 0) need_board = 0;
    need_hero = final_hole - g->seat[seat].nhole;
    if (need_hero < 0) need_hero = 0;

    need_total = need_board + need_hero;
    for (i = 0; i < nopp; i++) need_total += final_hole - opp_nknown[i];

    /* A full stud table can want more cards than are left. Rather
     * than estimating against a deck that cannot exist, drop the
     * furthest opponents until it fits -- an equity against five
     * opponents instead of seven is slightly optimistic and is far
     * better than no estimate at all. */
    while (nopp > 1 && need_total > zdeck_remaining(&d)) {
        nopp--;
        need_total -= final_hole - opp_nknown[nopp];
    }
    if (need_total > zdeck_remaining(&d)) return PK_PERMILLE / 2;

    for (r = 0; r < rollouts; r++) {
        int pos = 0, k, nh = 0;
        uint8_t run[PK_MAX_BOARD];
        uint8_t hc[PK_MAX_HOLE + PK_MAX_BOARD];
        uint32_t ov[PK_MAX_SEATS];
        uint32_t hv;

        /* r == 0 is included deliberately: a decision with fewer
         * rollouts than the interval must still pump, and the weakest
         * levels have the fewest rollouts.
         *
         * But the first poll can only PUMP -- it cannot abort. An
         * estimate built from zero samples is not an estimate, and the
         * caller would get the neutral 500 that means "no idea" rather
         * than the rough answer that a handful of samples already
         * gives. One extra interval of latency on an abort is nothing
         * next to that. */
        if ((r & (PK_POLL_EVERY - 1)) == 0) {
            bool carry_on = !ai_poll || ai_poll(ai_poll_ctx);
            if (r > 0 && (!carry_on || out_of_time(start, L->budget_ms))) {
                a->aborted_last = true;
                break;
            }
        }

        for (k = 0; k < need_board; k++) run[k] = take(&d, &pos);

        for (k = 0; k < g->seat[seat].nhole; k++) hc[nh++] = g->seat[seat].hole[k];
        for (k = 0; k < need_hero; k++) hc[nh++] = take(&d, &pos);
        for (k = 0; k < g->nboard; k++) hc[nh++] = g->board[k];
        for (k = 0; k < need_board; k++) hc[nh++] = run[k];

        hv = pk_eval_best(hc, nh > 7 ? 7 : nh, 0);

        for (i = 0; i < nopp; i++) {
            uint8_t oc[PK_MAX_HOLE + PK_MAX_BOARD];
            int no = 0;
            for (k = 0; k < opp_nknown[i]; k++) oc[no++] = opp_known[i][k];
            for (k = opp_nknown[i]; k < final_hole; k++) oc[no++] = take(&d, &pos);
            for (k = 0; k < g->nboard; k++) oc[no++] = g->board[k];
            for (k = 0; k < need_board; k++) oc[no++] = run[k];
            ov[i] = pk_eval_best(oc, no > 7 ? 7 : no, 0);
        }

        total += score(hv, ov, nopp);
        a->rollouts_last++;
    }

    if (a->rollouts_last == 0) return PK_PERMILLE / 2;

    return (int32_t)(total / (int64_t)a->rollouts_last);
}

/* -- deciding -------------------------------------------------------- */

/* How often this seat folds, per thousand, or -1 if we have not seen
 * enough of them to say anything. */
static int32_t fold_rate(const pk_ai_t *a, const pk_game_t *g, int me)
{
    int32_t actions = 0, folds = 0;
    int i;

    for (i = 0; i < g->nseats; i++) {
        if (i == me) continue;
        actions += a->read[i].actions;
        folds += a->read[i].folds;
    }

    /* Twenty actions is not a read. It is enough to be confidently
     * wrong about, which is worse than having no opinion. */
    if (actions < 20) return -1;

    return (folds * PK_PERMILLE) / actions;
}

/* A bet sized by how much of the pot it is worth putting in.
 * `pct` is a percentage of the pot. Clamped to what is legal, which
 * for fixed limit collapses to the one legal size. */
static int32_t size_bet(const pk_game_t *g, const pk_options_t *o, int pct)
{
    int32_t extra = (g->pot * pct) / 100;
    int32_t to = g->bet_to_match + o->call_cost + extra;

    if (to < o->min_to) to = o->min_to;
    if (to > o->max_to) to = o->max_to;

    return to;
}

void pk_ai_decide(pk_ai_t *a, const pk_game_t *g, int seat,
    int *action, int32_t *to)
{
    const pk_level_t *L = lv(a->level);
    pk_options_t o;
    int32_t eq, odds, pot_after;
    int bluff;

    *action = PK_FOLD;
    *to = 0;

    /* Once up front, before anything else can return early.
     *
     * The noise path below answers without running a single rollout,
     * so without this a weak opponent's whole turn would service no
     * messages at all -- and weak opponents are exactly the ones a
     * beginner sits down against. */
    if (ai_poll) (void)ai_poll(ai_poll_ctx);

    pk_options(g, seat, &o);

    /* Nothing to decide. This is reachable when the engine has moved
     * on between the caller looking and the caller asking, so it
     * answers rather than asserting. */
    if (!o.can_fold && !o.can_check) { *action = PK_CHECK; return; }
    if (o.can_check && !o.can_bet && !o.can_raise) { *action = PK_CHECK; return; }

    /* The random third of a beginner's decisions. Taken BEFORE the
     * rollouts, not after, so a weak level is also a fast one -- which
     * is most of what makes an easy game feel easy to sit at. */
    if (L->noise && (int)zg_rng_below(100) < L->noise) {
        int r = (int)zg_rng_below(100);
        if (r < 55 || (!o.can_bet && !o.can_raise)) {
            *action = o.can_check ? PK_CHECK : PK_CALL;
        } else if (r < 85) {
            *action = o.can_bet ? PK_BET : PK_RAISE;
            *to = size_bet(g, &o, 50);
        } else {
            *action = o.can_check ? PK_CHECK : PK_FOLD;
        }
        return;
    }

    eq = pk_ai_equity_game(a, g, seat, L->rollouts);

    pot_after = g->pot + o.call_cost;
    odds = pot_after > 0 ? (o.call_cost * PK_PERMILLE) / pot_after : 0;

    bluff = L->bluff;

    if (L->model) {
        int32_t fr = fold_rate(a, g, seat);
        if (fr >= 0) {
            /* Against a table that folds a lot, bluff more. Against a
             * table that never folds, stop bluffing entirely and bet
             * good-but-not-great hands for value instead -- there is
             * no point representing anything to somebody who is going
             * to call regardless. */
            if (fr > 400) bluff += 8;
            else if (fr < 150) bluff = 0;
        }
    }

    /* Value. */
    if (eq >= L->raise_eq && (o.can_bet || o.can_raise)) {
        int pct = 50;
        if (eq > 850) pct = 100;
        else if (eq > 730) pct = 75;
        *action = o.can_bet ? PK_BET : PK_RAISE;
        *to = size_bet(g, &o, pct);
        return;
    }

    /* A bluff, and only where one can actually work: nobody has bet,
     * so there is something to win uncontested. Bluff-raising over a
     * bet is a different and much harder judgement and is deliberately
     * not attempted at any level. */
    if (o.can_bet && eq < 400 && bluff > 0 &&
        (int)zg_rng_below(100) < bluff) {
        *action = PK_BET;
        *to = size_bet(g, &o, 55);
        return;
    }

    if (o.can_check) { *action = PK_CHECK; return; }

    /* Call if the price is right, with the level's slack added. A
     * positive slack is a player who calls with the worst of it, which
     * is what a weak player does and is why the weak levels lose money
     * in a way that reads as bad poker rather than as randomness. */
    if (eq + L->slack >= odds) { *action = PK_CALL; return; }

    *action = PK_FOLD;
}
