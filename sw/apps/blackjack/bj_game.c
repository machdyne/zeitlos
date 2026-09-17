/*
 * Zeitlos blackjack -- the rules.
 * See bj_game.h for the ace rule and why 21 is not always blackjack.
 */

#include "bj_game.h"

void bj_rules_default(bj_rules_t *r)
{
    r->ndecks = 6;
    r->dealer_hits_soft17 = false;
    r->double_after_split = true;
    r->resplit_aces = false;
    r->surrender = true;
    r->bj_pay_num = 3;
    r->bj_pay_den = 2;
    r->max_hands = BJ_MAX_HANDS;
    r->penetration = 75;
}

/* -- values ------------------------------------------------------------- */

int bj_card_value(uint8_t card)
{
    int r;

    if (card >= Z_NCARDS) return 0;

    r = Z_RANK(card);

    if (r == Z_RANK_A) return 11;
    if (r >= Z_RANK_T) return 10;      /* ten, jack, queen, king */

    return r + 2;                       /* rank 0 is the deuce */
}

int bj_total(const uint8_t *cards, int n, bool *soft)
{
    int total = 0, aces = 0, i;

    for (i = 0; i < n; i++) {
        if (cards[i] >= Z_NCARDS) continue;
        if (Z_RANK(cards[i]) == Z_RANK_A) { aces++; total += 1; }
        else total += bj_card_value(cards[i]);
    }

    /* ONE ace may be promoted to eleven, not every ace.
     *
     * Written as a single `if` rather than a loop for exactly that
     * reason: a loop over every ace turns A-A into 22 and A-A-A into
     * 33, which busts hands that are actually 12 and 13. The hand stays
     * playable, so nothing crashes -- it is just valued wrong, quietly,
     * for the whole session. */
    if (aces > 0 && total + 10 <= 21) {
        total += 10;
        if (soft) *soft = true;
    } else {
        if (soft) *soft = false;
    }

    return total;
}

bool bj_is_natural(const uint8_t *cards, int n)
{
    /* EXACTLY two cards. Twenty-one from three cards, or from a split,
     * is not a natural: it pays even money and loses the tie to a
     * dealer's natural. */
    return n == 2 && bj_total(cards, 2, 0) == 21;
}

bool bj_dealer_draws(const bj_rules_t *r, const uint8_t *cards, int n)
{
    bool soft = false;
    int t = bj_total(cards, n, &soft);

    if (t < 17) return true;
    if (t == 17 && soft && r->dealer_hits_soft17) return true;

    return false;
}

/* -- setup --------------------------------------------------------------- */

static void clear_game(bj_game_t *g)
{
    int i;
    for (i = 0; i < (int)sizeof(bj_game_t); i++) ((char *)g)[i] = 0;
}

void bj_game_init(bj_game_t *g, const bj_rules_t *r)
{
    bj_rules_t rules;

    if (r) rules = *r;
    else bj_rules_default(&rules);

    clear_game(g);
    g->rules = rules;

    zdeck_init(&g->shoe, rules.ndecks);
    zdeck_shuffle(&g->shoe);

    g->phase = BJ_PHASE_BETTING;
}

static uint8_t draw(bj_game_t *g)
{
    uint8_t c = zdeck_deal(&g->shoe);

    /* An exhausted shoe mid-hand cannot be reshuffled -- the cards
     * already dealt would come back. Reshuffling happens between
     * rounds, at the cut card; this is the guard for a shoe so small
     * the penetration rule never fired. */
    if (c == Z_CARD_NONE) {
        zdeck_init(&g->shoe, g->rules.ndecks);
        zdeck_shuffle(&g->shoe);
        c = zdeck_deal(&g->shoe);
    }

    return c;
}

static void play_dealer(bj_game_t *g);

static void hand_add(bj_hand_t *h, uint8_t c)
{
    if (h->n < BJ_MAX_CARDS) h->card[h->n++] = c;
}

static bool begin(bj_game_t *g, int32_t bet, bool shuffle)
{
    int i;

    if (bet <= 0) return false;

    /* THE CUT CARD. Reshuffled between rounds once penetration is
     * reached, never during one. A shoe that reshuffled mid-hand would
     * put cards back that the player has already seen, which is not a
     * shuffle, it is a rewrite. */
    if (shuffle) {
        int dealt = zdeck_dealt(&g->shoe);
        int total = g->shoe.n;
        if (g->needs_shuffle || total == 0 ||
            dealt * 100 >= total * g->rules.penetration) {
            zdeck_init(&g->shoe, g->rules.ndecks);
            zdeck_shuffle(&g->shoe);
            g->needs_shuffle = false;
        }
    }

    for (i = 0; i < BJ_MAX_HANDS; i++) {
        int k;
        for (k = 0; k < (int)sizeof(bj_hand_t); k++)
            ((char *)&g->hand[i])[k] = 0;
    }

    g->nhands = 1;
    g->active = 0;
    g->ndealer = 0;
    g->insurance = 0;
    g->insurance_offered = false;
    g->returned = 0;
    g->staked = bet;
    g->hand[0].bet = bet;

    /* Dealt in the order a table deals them: player, dealer, player,
     * dealer. It makes no difference to the odds and every difference
     * to a stacked shoe in a test. */
    hand_add(&g->hand[0], draw(g));
    g->dealer[g->ndealer++] = draw(g);
    hand_add(&g->hand[0], draw(g));
    g->dealer[g->ndealer++] = draw(g);

    if (Z_RANK(g->dealer[0]) == Z_RANK_A) {
        g->insurance_offered = true;
        g->phase = BJ_PHASE_INSURANCE;
        return true;
    }

    g->phase = BJ_PHASE_PLAYER;

    /* A player natural against a dealer who cannot have one is settled
     * immediately -- there is nothing to decide.
     *
     * SETTLED, not merely moved to the dealer's phase. Setting the
     * phase without running it left the round parked forever with the
     * bet uncollected, which the tests caught as a natural paying
     * nothing at all. */
    if (bj_is_natural(g->hand[0].card, g->hand[0].n)) {
        g->hand[0].done = true;
        play_dealer(g);
    }

    return true;
}

bool bj_round_begin(bj_game_t *g, int32_t bet)
{
    return begin(g, bet, true);
}

bool bj_round_begin_stacked(bj_game_t *g, int32_t bet)
{
    return begin(g, bet, false);
}

uint8_t bj_dealer_up(const bj_game_t *g)
{
    return g->ndealer > 0 ? g->dealer[0] : Z_CARD_NONE;
}

bool bj_hole_shown(const bj_game_t *g)
{
    return g->phase == BJ_PHASE_DEALER || g->phase == BJ_PHASE_DONE;
}

/* -- settlement ---------------------------------------------------------- */

static void settle(bj_game_t *g)
{
    bool dealer_nat = bj_is_natural(g->dealer, g->ndealer);
    int dt = bj_total(g->dealer, g->ndealer, 0);
    int i;

    /* Insurance first: it is a side bet on the hole card and is
     * resolved whatever happens to the hand. */
    if (g->insurance > 0 && dealer_nat)
        g->returned += g->insurance * 3;      /* 2:1, stake included */

    for (i = 0; i < g->nhands; i++) {
        bj_hand_t *h = &g->hand[i];
        int pt = bj_total(h->card, h->n, 0);
        bool nat = bj_is_natural(h->card, h->n) && !h->from_split;

        h->won = 0;

        if (h->surrendered) {
            /* Half back, rounded in the player's favour -- an odd bet
             * is not the place to take an extra chip. */
            h->won = (h->bet + 1) / 2;
        } else if (pt > 21) {
            h->won = 0;
        } else if (nat && dealer_nat) {
            h->won = h->bet;                  /* push */
        } else if (nat) {
            /* 3:2 by default, 6:5 if configured. The stake comes back
             * too, so a 3:2 natural on 10 returns 25. */
            h->won = h->bet +
                (h->bet * g->rules.bj_pay_num) / g->rules.bj_pay_den;
        } else if (dealer_nat) {
            h->won = 0;
        } else if (dt > 21) {
            h->won = h->bet * 2;
        } else if (pt > dt) {
            h->won = h->bet * 2;
        } else if (pt == dt) {
            h->won = h->bet;                  /* push */
        } else {
            h->won = 0;
        }

        g->returned += h->won;
    }

    g->phase = BJ_PHASE_DONE;
}

static void play_dealer(bj_game_t *g)
{
    int i;
    bool anyone_live = false;

    g->phase = BJ_PHASE_DEALER;

    /* The dealer does not draw if every hand has busted or
     * surrendered: there is nothing left to beat, and drawing would
     * change the shoe for the next round on the strength of a decision
     * nobody had to make. */
    for (i = 0; i < g->nhands; i++) {
        const bj_hand_t *h = &g->hand[i];
        if (h->surrendered) continue;
        if (bj_total(h->card, h->n, 0) > 21) continue;
        anyone_live = true;
    }

    if (anyone_live && !bj_is_natural(g->dealer, g->ndealer)) {
        while (bj_dealer_draws(&g->rules, g->dealer, g->ndealer) &&
            g->ndealer < BJ_MAX_CARDS)
            g->dealer[g->ndealer++] = draw(g);
    }

    settle(g);
}

/* -- playing -------------------------------------------------------------- */

void bj_options(const bj_game_t *g, bj_options_t *o)
{
    const bj_hand_t *h;

    o->can_hit = o->can_stand = o->can_double = false;
    o->can_split = o->can_surrender = false;

    if (g->phase != BJ_PHASE_PLAYER) return;
    if (g->active < 0 || g->active >= g->nhands) return;

    h = &g->hand[g->active];
    if (h->done) return;

    /* A split ace takes exactly one card and is finished. That single
     * rule is worth about a fifth of a percent of house edge and is the
     * most commonly omitted one in a hobby implementation. */
    if (h->split_ace && h->n >= 2) return;

    if (bj_total(h->card, h->n, 0) > 21) return;

    o->can_hit = h->n < BJ_MAX_CARDS;
    o->can_stand = true;

    /* Doubling and splitting are two-card decisions. */
    if (h->n == 2) {
        o->can_double = !h->from_split || g->rules.double_after_split;

        if (g->nhands < g->rules.max_hands &&
            bj_card_value(h->card[0]) == bj_card_value(h->card[1])) {
            /* PAIRS ARE MATCHED BY VALUE, not by rank: a king and a
             * jack are both ten and are a legal split at every table
             * that allows splitting tens at all. */
            bool aces = Z_RANK(h->card[0]) == Z_RANK_A;
            o->can_split = !(aces && h->from_split &&
                !g->rules.resplit_aces);
        }

        /* Late surrender: first decision on the original hand only,
         * and not after the dealer's natural has been checked for. */
        o->can_surrender = g->rules.surrender && !h->from_split &&
            g->nhands == 1;
    }
}

static void next_hand(bj_game_t *g)
{
    int i;

    for (i = g->active + 1; i < g->nhands; i++) {
        if (g->hand[i].done) continue;
        g->active = i;
        /* A split hand starts with one card and is dealt its second
         * here, so the player sees it complete before deciding. */
        if (g->hand[i].n == 1) {
            hand_add(&g->hand[i], draw(g));
            if (g->hand[i].split_ace) { g->hand[i].done = true; continue; }
            if (bj_total(g->hand[i].card, g->hand[i].n, 0) == 21) {
                g->hand[i].done = true;
                continue;
            }
        }
        return;
    }

    play_dealer(g);
}

bool bj_act(bj_game_t *g, int action)
{
    bj_options_t o;
    bj_hand_t *h;

    if (g->phase != BJ_PHASE_PLAYER) return false;
    if (g->active < 0 || g->active >= g->nhands) return false;

    bj_options(g, &o);
    h = &g->hand[g->active];

    switch (action) {

    case BJ_HIT:
        if (!o.can_hit) return false;
        hand_add(h, draw(g));
        if (bj_total(h->card, h->n, 0) >= 21) h->done = true;
        break;

    case BJ_STAND:
        if (!o.can_stand) return false;
        h->done = true;
        break;

    case BJ_DOUBLE:
        if (!o.can_double) return false;
        g->staked += h->bet;
        h->bet *= 2;
        h->doubled = true;
        hand_add(h, draw(g));
        h->done = true;
        break;

    case BJ_SPLIT: {
        bj_hand_t *nh;
        if (!o.can_split) return false;

        nh = &g->hand[g->nhands++];
        {
            int k;
            for (k = 0; k < (int)sizeof(bj_hand_t); k++)
                ((char *)nh)[k] = 0;
        }

        nh->card[0] = h->card[1];
        nh->n = 1;
        nh->bet = h->bet;
        nh->from_split = true;
        nh->split_ace = Z_RANK(h->card[0]) == Z_RANK_A;

        h->n = 1;
        h->from_split = true;
        h->split_ace = nh->split_ace;

        g->staked += nh->bet;

        /* The current hand draws its second card straight away. */
        hand_add(h, draw(g));
        if (h->split_ace) h->done = true;
        else if (bj_total(h->card, h->n, 0) == 21) h->done = true;
        break;
    }

    case BJ_SURRENDER:
        if (!o.can_surrender) return false;
        h->surrendered = true;
        h->done = true;
        break;

    default:
        return false;
    }

    if (h->done) next_hand(g);

    return true;
}

bool bj_insure(bj_game_t *g, int32_t amount)
{
    if (g->phase != BJ_PHASE_INSURANCE) return false;

    /* At most half the original bet, which is what makes the 2:1 payout
     * exactly cover the main bet when the dealer has a natural. */
    if (amount < 0 || amount > g->hand[0].bet / 2) return false;

    g->insurance = amount;
    g->staked += amount;

    g->phase = BJ_PHASE_PLAYER;

    /* Now the hole card is checked. A dealer natural ends the round
     * immediately -- there is nothing for the player to decide. */
    if (bj_is_natural(g->dealer, g->ndealer)) {
        int i;
        for (i = 0; i < g->nhands; i++) g->hand[i].done = true;
        play_dealer(g);
        return true;
    }

    if (bj_is_natural(g->hand[0].card, g->hand[0].n)) {
        g->hand[0].done = true;
        play_dealer(g);
    }

    return true;
}

const char *bj_action_name(int action)
{
    switch (action) {
    case BJ_HIT:       return "hit";
    case BJ_STAND:     return "stand";
    case BJ_DOUBLE:    return "double";
    case BJ_SPLIT:     return "split";
    case BJ_SURRENDER: return "surrender";
    }
    return "?";
}
