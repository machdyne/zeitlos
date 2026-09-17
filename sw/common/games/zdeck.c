/*
 * Zeitlos -- a deck, or a shoe of several.
 * See zdeck.h for why the generator lives in zrand.c and not here.
 */

#include "zdeck.h"
#include "zrand.h"

void zdeck_init(zdeck_t *d, int ndecks)
{
    int i, k;

    if (ndecks < 1) ndecks = 1;
    if (ndecks > Z_DECK_MAX_DECKS) ndecks = Z_DECK_MAX_DECKS;

    d->ndecks = ndecks;
    d->n = 0;
    d->pos = 0;

    for (k = 0; k < ndecks; k++)
        for (i = 0; i < Z_NCARDS; i++) d->card[d->n++] = (uint8_t)i;
}

void zdeck_init_excluding(zdeck_t *d, const uint8_t *seen, int n)
{
    bool gone[Z_NCARDS];
    int i;

    for (i = 0; i < Z_NCARDS; i++) gone[i] = false;

    for (i = 0; i < n; i++) {
        uint8_t c = seen[i];
        /* Z_CARD_NONE is expected here, not exceptional: an opponent's
         * unknown hole cards arrive that way, and a folded seat's cards
         * are never dealt at all. */
        if (c < Z_NCARDS) gone[c] = true;
    }

    d->ndecks = 1;
    d->n = 0;
    d->pos = 0;

    for (i = 0; i < Z_NCARDS; i++)
        if (!gone[i]) d->card[d->n++] = (uint8_t)i;
}

void zdeck_shuffle(zdeck_t *d)
{
    int i;

    /* Downward, swapping each card with one at or below it. See
     * zdeck.h: the upward variant is not equivalent and is biased. */
    for (i = d->n - 1; i > d->pos; i--) {
        int j = d->pos + (int)zg_rng_below((uint32_t)(i - d->pos + 1));
        uint8_t t = d->card[i];
        d->card[i] = d->card[j];
        d->card[j] = t;
    }
}

uint8_t zdeck_deal(zdeck_t *d)
{
    if (d->pos >= d->n) return Z_CARD_NONE;
    return d->card[d->pos++];
}

int zdeck_remaining(const zdeck_t *d)
{
    return d->n - d->pos;
}

int zdeck_dealt(const zdeck_t *d)
{
    return d->pos;
}

bool zdeck_stack(zdeck_t *d, const uint8_t *cards, int n)
{
    int k, i;
    bool used[Z_DECK_MAX];

    if (n < 0 || d->pos + n > d->n) return false;

    for (i = 0; i < d->n; i++) used[i] = false;

    /* Validated in full first, by finding a distinct POSITION for each
     * entry. Counting positions rather than rejecting repeated cards is
     * what lets a six-deck shoe be stacked with two aces of spades,
     * which is a perfectly ordinary blackjack hand. */
    for (k = 0; k < n; k++) {
        bool found = false;

        if (cards[k] >= Z_NCARDS) return false;

        for (i = d->pos; i < d->n; i++) {
            if (used[i] || d->card[i] != cards[k]) continue;
            used[i] = true;
            found = true;
            break;
        }

        if (!found) return false;
    }

    for (k = 0; k < n; k++) {
        int dst = d->pos + k;

        for (i = dst; i < d->n; i++) {
            if (d->card[i] != cards[k]) continue;
            d->card[i] = d->card[dst];
            d->card[dst] = cards[k];
            break;
        }
    }

    return true;
}
