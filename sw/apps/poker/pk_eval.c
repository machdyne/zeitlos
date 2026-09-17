/*
 * Zeitlos poker -- hand ranking.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See pk_eval.h for the encoding and why it is one integer. This file
 * is the whole of the ranking logic; there is no table anywhere in it.
 */

#include "pk_eval.h"

/* -- combinations ---------------------------------------------------
 *
 * An index set of k positions into n items, advanced in lexicographic
 * order. Shared by pk_eval_best() and pk_eval_constrained(), which
 * differ only in what they enumerate over.
 */

static void comb_first(int *idx, int k)
{
    int i;
    for (i = 0; i < k; i++) idx[i] = i;
}

static bool comb_next(int *idx, int k, int n)
{
    int i = k - 1, j;

    /* Find the rightmost index that still has room to move. */
    while (i >= 0 && idx[i] == n - k + i) i--;
    if (i < 0) return false;

    idx[i]++;
    for (j = i + 1; j < k; j++) idx[j] = idx[j - 1] + 1;

    return true;
}

/* -- the straight test ----------------------------------------------
 *
 * `mask` has one bit per rank present. Returns the high card's rank,
 * or -1.
 *
 * Walking down from the ace means the first match found is the best
 * straight, which matters for seven-card hands where a six-card run
 * contains two of them.
 */
static int straight_high(uint32_t mask)
{
    int h;

    for (h = Z_RANK_A; h >= Z_RANK_5 + 1; h--)
        if (((mask >> (h - 4)) & 0x1fu) == 0x1fu) return h;

    /* The wheel: ace plus deuce through five. Tested last and
     * separately because the ace is at the far end of the mask rather
     * than four bits below the five, so no shift of a contiguous
     * window can find it. Reported as five-high, which is what makes
     * it sort below every other straight without a special case in
     * the comparison. */
    if ((mask & (1u << Z_RANK_A)) && ((mask & 0x0fu) == 0x0fu))
        return Z_RANK_5;

    return -1;
}

uint32_t pk_eval5(const uint8_t *cards)
{
    int rc[Z_NRANKS];
    int sc[Z_NSUITS];
    uint32_t mask = 0;
    int i, r, cnt;
    int ord[5];
    int nord = 0;
    int maxcount = 0;
    int shigh;
    bool flush;
    int cat;
    uint32_t v;

    for (i = 0; i < Z_NRANKS; i++) rc[i] = 0;
    for (i = 0; i < Z_NSUITS; i++) sc[i] = 0;

    for (i = 0; i < 5; i++) {
        r = Z_RANK(cards[i]);
        rc[r]++;
        sc[Z_SUIT(cards[i])]++;
        mask |= 1u << r;
    }

    /* The tiebreak order: by multiplicity, then by rank, both
     * descending. See pk_eval.h -- this one loop is what makes every
     * category share a single encoding path. */
    for (cnt = 4; cnt >= 1; cnt--) {
        for (r = Z_RANK_A; r >= 0; r--) {
            if (rc[r] != cnt) continue;
            ord[nord++] = r;
            if (cnt > maxcount) maxcount = cnt;
        }
    }

    flush = (sc[0] == 5 || sc[1] == 5 || sc[2] == 5 || sc[3] == 5);

    /* Only worth asking when every rank is distinct. With a pair on
     * board the mask has four bits or fewer and cannot hold a run of
     * five, so this is an optimisation rather than a correctness
     * requirement -- but it also documents the fact, which is easy to
     * forget when extending this to seven cards. */
    shigh = (nord == 5) ? straight_high(mask) : -1;

    if (flush && shigh >= 0)          cat = PK_STRAIGHT_FLUSH;
    else if (maxcount == 4)           cat = PK_QUADS;
    else if (maxcount == 3 && nord == 2) cat = PK_FULL_HOUSE;
    else if (flush)                   cat = PK_FLUSH;
    else if (shigh >= 0)              cat = PK_STRAIGHT;
    else if (maxcount == 3)           cat = PK_TRIPS;
    else if (maxcount == 2 && nord == 3) cat = PK_TWO_PAIR;
    else if (maxcount == 2)           cat = PK_PAIR;
    else                              cat = PK_HIGH_CARD;

    v = (uint32_t)cat << 20;

    if (cat == PK_STRAIGHT || cat == PK_STRAIGHT_FLUSH) {
        /* The high card says everything. The other four nibbles stay
         * zero rather than carrying the remaining ranks, which would
         * be the same four numbers in every straight of that height
         * and could never break a tie. */
        v |= (uint32_t)shigh << 16;
        return v;
    }

    for (i = 0; i < nord && i < 5; i++)
        v |= (uint32_t)ord[i] << (16 - 4 * i);

    return v;
}

uint32_t pk_eval_upcards(const uint8_t *cards, int n)
{
    int rc[Z_NRANKS];
    int i, r, cnt;
    int ord[5];
    int nord = 0, maxcount = 0;
    int cat;
    uint32_t v;

    if (n < 1 || n > 5) return PK_EVAL_NONE;

    for (i = 0; i < Z_NRANKS; i++) rc[i] = 0;
    for (i = 0; i < n; i++) rc[Z_RANK(cards[i])]++;

    /* The same multiplicity-then-rank order pk_eval5() uses. It is
     * reproduced rather than shared because the two differ in what
     * they do next, and a common helper taking three flags to serve
     * both would be harder to read than either. */
    for (cnt = 4; cnt >= 1; cnt--) {
        for (r = Z_RANK_A; r >= 0; r--) {
            if (rc[r] != cnt) continue;
            ord[nord++] = r;
            if (cnt > maxcount) maxcount = cnt;
        }
    }

    if (maxcount == 4)                   cat = PK_QUADS;
    else if (maxcount == 3 && nord == 2) cat = PK_FULL_HOUSE;
    else if (maxcount == 3)              cat = PK_TRIPS;
    else if (maxcount == 2 && nord <= 3 && n - nord == 2)
                                         cat = PK_TWO_PAIR;
    else if (maxcount == 2)              cat = PK_PAIR;
    else                                 cat = PK_HIGH_CARD;

    v = (uint32_t)cat << 20;
    for (i = 0; i < nord && i < 5; i++)
        v |= (uint32_t)ord[i] << (16 - 4 * i);

    return v;
}

uint32_t pk_eval_best(const uint8_t *cards, int n, uint8_t *best)
{
    int idx[5];
    uint8_t hand[5];
    uint32_t bestval = PK_EVAL_NONE;
    int i;

    if (n < 5 || n > 7) return PK_EVAL_NONE;

    comb_first(idx, 5);

    for (;;) {
        uint32_t v;

        for (i = 0; i < 5; i++) hand[i] = cards[idx[i]];
        v = pk_eval5(hand);

        /* Strictly greater, so the FIRST subset achieving the best
         * value is the one reported. Any of them is a correct answer
         * -- they tie by definition -- but picking one deterministic-
         * ally means the showdown display does not change what it
         * highlights between two runs of the same hand. */
        if (v > bestval) {
            bestval = v;
            if (best) for (i = 0; i < 5; i++) best[i] = hand[i];
        }

        if (!comb_next(idx, 5, n)) break;
    }

    return bestval;
}

uint32_t pk_eval_constrained(const uint8_t *hole, int nhole,
    const uint8_t *board, int nboard, int use_hole, uint8_t *best)
{
    int hidx[5], bidx[5];
    uint8_t hand[5];
    uint32_t bestval = PK_EVAL_NONE;
    int use_board = 5 - use_hole;
    int i;

    if (use_hole < 0 || use_hole > 5) return PK_EVAL_NONE;
    if (nhole < use_hole || nboard < use_board) return PK_EVAL_NONE;

    comb_first(hidx, use_hole);

    for (;;) {
        comb_first(bidx, use_board);

        for (;;) {
            uint32_t v;

            for (i = 0; i < use_hole; i++) hand[i] = hole[hidx[i]];
            for (i = 0; i < use_board; i++)
                hand[use_hole + i] = board[bidx[i]];

            v = pk_eval5(hand);
            if (v > bestval) {
                bestval = v;
                if (best) for (i = 0; i < 5; i++) best[i] = hand[i];
            }

            if (use_board == 0) break;
            if (!comb_next(bidx, use_board, nboard)) break;
        }

        if (use_hole == 0) break;
        if (!comb_next(hidx, use_hole, nhole)) break;
    }

    return bestval;
}

/* -- naming ---------------------------------------------------------
 *
 * Built by hand rather than with snprintf, and that is not a style
 * choice. docs/app_runtime.md records that one conversion specifier
 * anywhere in an app links picolibc's formatter at a cost of about
 * 100KB, which is enough to push a binary past the space the loader
 * has for it. An app that names a poker hand every showdown must not
 * pay that.
 */

static const char *const pk_rank_one[Z_NRANKS] = {
    "deuce", "three", "four", "five", "six", "seven", "eight",
    "nine", "ten", "jack", "queen", "king", "ace"
};

static const char *const pk_rank_many[Z_NRANKS] = {
    "deuces", "threes", "fours", "fives", "sixes", "sevens", "eights",
    "nines", "tens", "jacks", "queens", "kings", "aces"
};

/* Appends `s` at *pos, never writing past len - 1, and always leaving
 * the buffer NUL terminated. */
static void app(char *buf, int len, int *pos, const char *s)
{
    while (*s && *pos < len - 1) buf[(*pos)++] = *s++;
    buf[*pos] = '\0';
}

static const char *one(int r)
{
    return (r >= 0 && r < Z_NRANKS) ? pk_rank_one[r] : "?";
}

static const char *many(int r)
{
    return (r >= 0 && r < Z_NRANKS) ? pk_rank_many[r] : "?";
}

void pk_eval_name(uint32_t value, char *buf, int len)
{
    int cat = PK_CATEGORY(value);
    int a = (int)((value >> 16) & 0xf);
    int b = (int)((value >> 12) & 0xf);
    int pos = 0;

    if (len <= 0) return;
    buf[0] = '\0';

    if (value == PK_EVAL_NONE) {
        app(buf, len, &pos, "no hand");
        return;
    }

    switch (cat) {

    case PK_STRAIGHT_FLUSH:
        /* Named separately because everybody calls it that, and a
         * showdown reading "ace-high straight flush" would be correct
         * and would still feel like a bug. */
        if (a == Z_RANK_A) { app(buf, len, &pos, "royal flush"); break; }
        app(buf, len, &pos, one(a));
        app(buf, len, &pos, "-high straight flush");
        break;

    case PK_QUADS:
        app(buf, len, &pos, "four ");
        app(buf, len, &pos, many(a));
        break;

    case PK_FULL_HOUSE:
        app(buf, len, &pos, many(a));
        app(buf, len, &pos, " full of ");
        app(buf, len, &pos, many(b));
        break;

    case PK_FLUSH:
        app(buf, len, &pos, one(a));
        app(buf, len, &pos, "-high flush");
        break;

    case PK_STRAIGHT:
        app(buf, len, &pos, one(a));
        app(buf, len, &pos, "-high straight");
        break;

    case PK_TRIPS:
        app(buf, len, &pos, "three ");
        app(buf, len, &pos, many(a));
        break;

    case PK_TWO_PAIR:
        app(buf, len, &pos, many(a));
        app(buf, len, &pos, " and ");
        app(buf, len, &pos, many(b));
        break;

    case PK_PAIR:
        app(buf, len, &pos, "pair of ");
        app(buf, len, &pos, many(a));
        break;

    default:
        app(buf, len, &pos, one(a));
        app(buf, len, &pos, " high");
        break;
    }
}
