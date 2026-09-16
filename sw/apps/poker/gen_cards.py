#!/usr/bin/env python3
#
# Zeitlos poker -- card tile generator.
#
#   python3 gen_cards.py
#
# Emits cards.c / cards.h. Generated C is committed alongside the
# generator, the same arrangement sw/apps/chess/gen_pieces.py,
# sw/apps/gamedemo/gen_sprites.py and sw/data/icons use: the app builds
# with no Python in the loop, and where the art came from is still
# obvious.
#
# -- whole tiles, not sprites with a mask --
#
# Same reasoning as gen_pieces.py, and it applies harder here. A card
# is an OPAQUE white rectangle: it has to REPLACE what is under it, or
# a card that moves leaves a ghost. A masked sprite needs raster ops,
# which zgfx.h says to probe for because an older bitstream silently
# treats every rop as COPY, and it tears where there is no cookie-cut
# mode. One opaque z_fb_hw_blit_mem() per card needs none of that.
#
# -- two sizes, and why --
#
# FULL (20x28) for the hero's own cards and the community board: rank,
# a small index pip, and a large central pip.
#
# MINI (12x16) for the other players: rank over suit letter, both
# inside columns 2..6. That placement is the entire point
# -- seven-card stud gives a seat up to seven cards and a six-handed
# table leaves about 64 pixels of width per seat, so the cards are
# drawn OVERLAPPED at a 7-pixel stride, exactly the way a hand of
# cards is fanned. With the rank and suit both in the left five
# columns, every card in the fan is still readable.
#
# -- suits on a display with no colour --
#
# On a FULL card, spades and clubs are SOLID pips and hearts and
# diamonds are HOLLOW ones. That is the conventional 1bpp answer and
# it works because the shapes differ as well: a hollow diamond cannot
# be mistaken for a solid spade.
#
# On a MINI card the pip would be four pixels across, where solid and
# hollow are the same picture. So minis use the suit LETTER instead.
# Unambiguous at any size, and it is what the rest of this app already
# prints in messages.
#
# -- cost --
#
#   52 full  x 28 rows x 4 bytes = 5,824
#   52 mini  x 16 rows x 4 bytes = 3,328
#   2 backs                      =   176
#                                  -------
#                                   9,328 bytes of read-only data
#
# Against sw/apps/chess spending 24KB on a transposition table out of
# the same process allocation, that is affordable. Nothing here is
# allocated at runtime.
#
# -- the bit order is NOT the font's --
#
# z_fb_hw_blit_mem() reads its source with the LEAST significant bit
# leftmost (see zgfx.h and zbm.h), which is the opposite of the
# MSB-first convention z_font_t glyphs and zicon.h icons use. Getting
# this backwards produces every card mirrored, which is obvious the
# moment you look at one and completely invisible in a geometry
# assertion.

CARD_W, CARD_H = 20, 28
MINI_W, MINI_H = 12, 17

# -- a 5x7 face for ranks and suit letters ---------------------------

GLYPHS = {
'2': ["####.", "....#", "....#", "..##.", ".#...", "#....", "#####"],
'3': ["####.", "....#", "....#", "..##.", "....#", "....#", "####."],
'4': ["#...#", "#...#", "#...#", "#####", "....#", "....#", "....#"],
'5': ["#####", "#....", "####.", "....#", "....#", "#...#", ".###."],
'6': [".###.", "#....", "#....", "####.", "#...#", "#...#", ".###."],
'7': ["#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."],
'8': [".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."],
'9': [".###.", "#...#", "#...#", ".####", "....#", "....#", ".###."],
'T': ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
'J': ["..###", "....#", "....#", "....#", "#...#", "#...#", ".###."],
'Q': [".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"],
'K': ["#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"],
'A': ["..#..", ".#.#.", "#...#", "#...#", "#####", "#...#", "#...#"],
'c': [".....", ".....", ".###.", "#...#", "#....", "#...#", ".###."],
'd': ["....#", "....#", ".####", "#...#", "#...#", "#...#", ".####"],
'h': ["#....", "#....", "#.##.", "##..#", "#...#", "#...#", "#...#"],
's': [".....", ".....", ".####", "#....", ".###.", "....#", "####."],
}

# -- pips -------------------------------------------------------------
#
# Drawn solid. The hollow versions used for hearts and diamonds are
# COMPUTED from these by keeping only the pixels that touch a hole,
# rather than drawn separately, so the two can never drift apart.

PIP_BIG = {
'spade': ["....#....", "...###...", "..#####..", ".#######.", "#########",
          "#########", "#########", "..##.##..", "...###..."],
'heart': ["##.....##", "####.####", "#########", "#########", "#########",
          ".#######.", "..#####..", "...###...", "....#...."],
'diamond': ["....#....", "...###...", "..#####..", ".#######.", "#########",
            ".#######.", "..#####..", "...###...", "....#...."],
'club': ["...###...", "..#####..", "...###...", ".##.#.##.", "#########",
         "#########", ".##.#.##.", "....#....", "..#####.."],
}

PIP_SMALL = {
'spade':   ["..#..", ".###.", "#####", "#####", "..#.."],
'heart':   ["##.##", "#####", "#####", ".###.", "..#.."],
'diamond': ["..#..", ".###.", "#####", ".###.", "..#.."],
'club':    ["..#..", ".###.", "#####", ".#.#.", "..#.."],
}

SUITS = ['club', 'diamond', 'heart', 'spade']       # pk_cards.h order
SUIT_CH = ['c', 'd', 'h', 's']
RED = {'heart', 'diamond'}
RANK_CH = ['2','3','4','5','6','7','8','9','T','J','Q','K','A']


def grid(w, h):
    return [[0] * w for _ in range(h)]


def stamp(g, art, x0, y0, on=1):
    for j, row in enumerate(art):
        for i, ch in enumerate(row):
            if ch == '#':
                if 0 <= y0 + j < len(g) and 0 <= x0 + i < len(g[0]):
                    g[y0 + j][x0 + i] = on


def hollow(art):
    """Keep only the pixels of a solid shape that touch empty space.

    Computed rather than drawn, so a hollow pip is always exactly the
    outline of the solid one it pairs with. Drawing both by hand is how
    the diamond ends up a different size from the heart."""
    h, w = len(art), len(art[0])

    def solid(x, y):
        if x < 0 or y < 0 or x >= w or y >= h:
            return False
        return art[y][x] == '#'

    out = []
    for y in range(h):
        row = ''
        for x in range(w):
            edge = solid(x, y) and not (
                solid(x - 1, y) and solid(x + 1, y) and
                solid(x, y - 1) and solid(x, y + 1))
            row += '#' if edge else '.'
        out.append(row)
    return out


def blank_card(w, h):
    """A card FACE: white body, black one-pixel border, corners cut.

    1 is lit on this display, so the face is 1 and the ink is 0 -- the
    same polarity sw/apps/chess uses for its light squares. The first
    version of this file had it the other way round and produced
    photographic negatives of playing cards, which is obvious the
    instant you look at one and completely invisible in any geometry
    assertion. That is the whole argument for tests/render.c.

    The black border does not separate the card from the desktop --
    the desktop is black too -- it separates one card from the NEXT
    one, which matters because the other players' cards are drawn
    overlapping."""
    g = [[1] * w for _ in range(h)]
    for x in range(w):
        g[0][x] = 0
        g[h - 1][x] = 0
    for y in range(h):
        g[y][0] = 0
        g[y][w - 1] = 0
    for (x, y) in ((1, 1), (w - 2, 1), (1, h - 2), (w - 2, h - 2)):
        g[y][x] = 0
    return g


def full_card(rank, suit):
    g = blank_card(CARD_W, CARD_H)
    pip_big = PIP_BIG[suit]
    pip_small = PIP_SMALL[suit]
    if suit in RED:
        pip_big = hollow(pip_big)
        pip_small = hollow(pip_small)
    stamp(g, GLYPHS[RANK_CH[rank]], 2, 3, on=0)
    stamp(g, pip_small, 13, 4, on=0)
    stamp(g, pip_big, 6, 15, on=0)
    return g


def mini_card(rank, suit):
    g = blank_card(MINI_W, MINI_H)
    """Rank over suit letter, BOTH inside columns 2..6.

    That is not decoration. Overlapped at PK_ART_MINI_STRIDE the only
    columns of a card that stay visible are 0..6, so anything further
    right is hidden behind the next card in the fan."""
    stamp(g, GLYPHS[RANK_CH[rank]], 2, 2, on=0)
    stamp(g, GLYPHS[SUIT_CH[SUITS.index(suit)]], 2, 9, on=0)
    return g


def back(w, h):
    """A face-down card: dark, with a white border and a lattice.

    The opposite polarity from a face, deliberately. A fan of face-down
    cards has to read as SEVERAL cards at a glance and has to be
    instantly distinguishable from a fan of faces, and on a display
    with two colours the only thing left to vary is which one
    dominates."""
    g = [[0] * w for _ in range(h)]
    for y in range(2, h - 2):
        for x in range(2, w - 2):
            g[y][x] = 1 if ((x + y) % 4 == 0 or (x - y) % 4 == 0) else 0
    for x in range(w):
        g[0][x] = 1
        g[h - 1][x] = 1
    for y in range(h):
        g[y][0] = 1
        g[y][w - 1] = 1
    for (x, y) in ((0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)):
        g[y][x] = 0
    return g


def rows_to_words(g):
    """One uint32 per row, LEAST significant bit leftmost -- the source
    format z_fb_hw_blit_mem() reads. NOT the font's MSB-first order."""
    out = []
    for row in g:
        word = 0
        for x, v in enumerate(row):
            if v:
                word |= 1 << x
        out.append(word)
    return out


def emit_array(f, name, cards, h):
    f.write("const uint32_t %s[] = {\n" % name)
    for idx, g in enumerate(cards):
        words = rows_to_words(g)
        f.write("    /* %s */\n" % idx_name(idx))
        for i in range(0, h, 4):
            f.write("    " + " ".join("0x%08x," % w for w in words[i:i + 4]))
            f.write("\n")
    f.write("};\n\n")


def idx_name(i):
    return "%s%s" % (RANK_CH[i >> 2], SUIT_CH[i & 3])


def main():
    full = [full_card(i >> 2, SUITS[i & 3]) for i in range(52)]
    mini = [mini_card(i >> 2, SUITS[i & 3]) for i in range(52)]

    with open("cards.h", "w") as f:
        f.write("""/* Generated by gen_cards.py -- do not hand-edit. */
/* Re-run that script after changing the art; the output is committed
 * so the build needs no Python. */

#ifndef PK_CARDS_ART_H
#define PK_CARDS_ART_H

#include <stdint.h>

/*
 * Card tiles, 1bpp, one uint32_t per row with the LEAST significant
 * bit leftmost -- the source format z_fb_hw_blit_mem() reads, which is
 * the opposite of the MSB-first order z_font_t and zicon.h use. A tile
 * is therefore 4 bytes of stride regardless of its width.
 *
 * Each tile is OPAQUE: white body, black border, and it replaces
 * whatever was underneath. See gen_cards.py for why that matters and
 * why there are two sizes.
 *
 * Index by the card byte from pk_cards.h, 0..51. Row r of card c is
 * pk_card_full[c * PK_ART_FULL_H + r].
 */

#define PK_ART_FULL_W %d
#define PK_ART_FULL_H %d
#define PK_ART_MINI_W %d
#define PK_ART_MINI_H %d
#define PK_ART_STRIDE 4

/* How far apart overlapped mini cards are drawn. The rank and the suit
 * letter both live in columns 2..6, so at this stride every card in a
 * fan still shows both, with a column of white to spare. Seven cards
 * -- a full seven-card stud hand -- occupy 60 pixels, which is what a
 * six-handed table has to spare per seat. */
#define PK_ART_MINI_STRIDE 8

extern const uint32_t pk_card_full[52 * PK_ART_FULL_H];
extern const uint32_t pk_card_mini[52 * PK_ART_MINI_H];
extern const uint32_t pk_back_full[PK_ART_FULL_H];
extern const uint32_t pk_back_mini[PK_ART_MINI_H];

#endif
""" % (CARD_W, CARD_H, MINI_W, MINI_H))

    with open("cards.c", "w") as f:
        f.write("/* Generated by gen_cards.py -- do not hand-edit. */\n\n")
        f.write('#include "cards.h"\n\n')
        emit_array(f, "pk_card_full", full, CARD_H)
        emit_array(f, "pk_card_mini", mini, MINI_H)
        f.write("const uint32_t pk_back_full[] = {\n")
        for w in rows_to_words(back(CARD_W, CARD_H)):
            f.write("    0x%08x,\n" % w)
        f.write("};\n\nconst uint32_t pk_back_mini[] = {\n")
        for w in rows_to_words(back(MINI_W, MINI_H)):
            f.write("    0x%08x,\n" % w)
        f.write("};\n")

    print("wrote cards.c and cards.h")


if __name__ == "__main__":
    main()
