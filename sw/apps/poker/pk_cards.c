/*
 * Zeitlos poker -- card naming and parsing.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Nothing here is on a hot path. pk_card_parse() exists for the tests
 * and the `deal` debug command; pk_card_str() for the message line.
 * The renderer does not use either -- it indexes a tile by card byte.
 */

#include "pk_cards.h"

static const char pk_rank_chars[PK_NRANKS] = {
    '2', '3', '4', '5', '6', '7', '8', '9', 'T', 'J', 'Q', 'K', 'A'
};

static const char pk_suit_chars[PK_NSUITS] = { 'c', 'd', 'h', 's' };

char pk_rank_char(int rank)
{
    if (rank < 0 || rank >= PK_NRANKS) return '?';
    return pk_rank_chars[rank];
}

char pk_suit_char(int suit)
{
    if (suit < 0 || suit >= PK_NSUITS) return '?';
    return pk_suit_chars[suit];
}

char *pk_card_str(uint8_t card, char *buf)
{
    if (card == PK_CARD_NONE || card >= PK_NCARDS) {
        buf[0] = '-';
        buf[1] = '-';
        buf[2] = '\0';
        return buf;
    }

    buf[0] = pk_rank_char(PK_RANK(card));
    buf[1] = pk_suit_char(PK_SUIT(card));
    buf[2] = '\0';
    return buf;
}

/* Lowercase without <ctype.h>, which on this libc is a locale table
 * this app has no other reason to link. */
static char pk_lower(char c)
{
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

uint8_t pk_card_parse(const char *s)
{
    int rank = -1, suit = -1, i;
    char rc, sc;

    if (!s || !s[0] || !s[1]) return PK_CARD_NONE;

    /* Rank is matched case-insensitively and so is suit. Poker has no
     * equivalent of chess notation's Bxc4/bxc4 distinction -- there is
     * exactly one card called the ace of hearts and no second reading
     * of "AH" to collide with. */
    rc = pk_lower(s[0]);
    sc = pk_lower(s[1]);

    for (i = 0; i < PK_NRANKS; i++)
        if (pk_lower(pk_rank_chars[i]) == rc) { rank = i; break; }

    for (i = 0; i < PK_NSUITS; i++)
        if (pk_suit_chars[i] == sc) { suit = i; break; }

    if (rank < 0 || suit < 0) return PK_CARD_NONE;

    return PK_CARD(rank, suit);
}
