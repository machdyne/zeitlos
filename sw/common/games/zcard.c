/*
 * Zeitlos -- card naming and parsing.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Nothing here is on a hot path. zcard_parse() exists for the tests
 * and the `deal` debug command; zcard_str() for the message line.
 * The renderer does not use either -- it indexes a tile by card byte.
 */

#include "zcard.h"

static const char zcard_rank_chars[Z_NRANKS] = {
    '2', '3', '4', '5', '6', '7', '8', '9', 'T', 'J', 'Q', 'K', 'A'
};

static const char zcard_suit_chars[Z_NSUITS] = { 'c', 'd', 'h', 's' };

char zcard_rank_char(int rank)
{
    if (rank < 0 || rank >= Z_NRANKS) return '?';
    return zcard_rank_chars[rank];
}

char zcard_suit_char(int suit)
{
    if (suit < 0 || suit >= Z_NSUITS) return '?';
    return zcard_suit_chars[suit];
}

char *zcard_str(uint8_t card, char *buf)
{
    if (card == Z_CARD_NONE || card >= Z_NCARDS) {
        buf[0] = '-';
        buf[1] = '-';
        buf[2] = '\0';
        return buf;
    }

    buf[0] = zcard_rank_char(Z_RANK(card));
    buf[1] = zcard_suit_char(Z_SUIT(card));
    buf[2] = '\0';
    return buf;
}

/* Lowercase without <ctype.h>, which on this libc is a locale table
 * this app has no other reason to link. */
static char zcard_lower(char c)
{
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

uint8_t zcard_parse(const char *s)
{
    int rank = -1, suit = -1, i;
    char rc, sc;

    if (!s || !s[0] || !s[1]) return Z_CARD_NONE;

    /* Rank is matched case-insensitively and so is suit. Poker has no
     * equivalent of chess notation's Bxc4/bxc4 distinction -- there is
     * exactly one card called the ace of hearts and no second reading
     * of "AH" to collide with. */
    rc = zcard_lower(s[0]);
    sc = zcard_lower(s[1]);

    for (i = 0; i < Z_NRANKS; i++)
        if (zcard_lower(zcard_rank_chars[i]) == rc) { rank = i; break; }

    for (i = 0; i < Z_NSUITS; i++)
        if (zcard_suit_chars[i] == sc) { suit = i; break; }

    if (rank < 0 || suit < 0) return Z_CARD_NONE;

    return Z_CARD(rank, suit);
}
