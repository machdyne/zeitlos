#!/usr/bin/env python3
"""
Zeitlos slots -- symbol tile generator.

    cd sw/apps/slots && python3 gen_slots.py

Emits sl_art.c / sl_art.h. The generated C is committed alongside this
script, so it MUST regenerate identically -- check with `git diff` after
running it. sw/common/games/gen_cards.py drifted from its own output
once, when the file names were renamed but the strings written into them
were not.

Packing matches the card art (sw/common/games/zcardart.h): one uint32_t
per row, LEAST significant bit leftmost, which is what
z_fb_hw_blit_mem() expects and the opposite of the font's MSB-first
rows. A 32-wide tile is exactly one word.

Drawn here rather than by hand because a slot symbol is mostly symmetry
-- a bell, a cherry pair, a seven -- and symmetry is easier to get right
in arithmetic than in a bitmap editor. The one thing that is NOT
symmetric is the BAR wordmark, which is drawn from a small font below.
"""

W = 32
H = 32

def blank():
    return [[0] * W for _ in range(H)]

def rect(g, x0, y0, x1, y1, v=1):
    for y in range(max(0, y0), min(H, y1 + 1)):
        for x in range(max(0, x0), min(W, x1 + 1)):
            g[y][x] = v

def frame(g, x0, y0, x1, y1, v=1):
    rect(g, x0, y0, x1, y0, v); rect(g, x0, y1, x1, y1, v)
    rect(g, x0, y0, x0, y1, v); rect(g, x1, y0, x1, y1, v)

def disc(g, cx, cy, r, v=1):
    for y in range(H):
        for x in range(W):
            dx, dy = x - cx, y - cy
            if dx * dx + dy * dy <= r * r:
                g[y][x] = v

def ring(g, cx, cy, r0, r1, v=1):
    for y in range(H):
        for x in range(W):
            dx, dy = x - cx, y - cy
            d = dx * dx + dy * dy
            if r0 * r0 <= d <= r1 * r1:
                g[y][x] = v

# A 4x5 digit/letter set, just enough for BAR and 7.
GLYPH = {
    'B': ["###.", "#..#", "###.", "#..#", "###."],
    'A': [".##.", "#..#", "####", "#..#", "#..#"],
    'R': ["###.", "#..#", "###.", "#.#.", "#..#"],
}

def text(g, x, y, s, v=1):
    for ch in s:
        rows = GLYPH[ch]
        for j, row in enumerate(rows):
            for i, c in enumerate(row):
                if c == '#' and 0 <= x + i < W and 0 <= y + j < H:
                    g[y + j][x + i] = v
        x += 5
    return x

def sym_blank():
    return blank()

def sym_cherry():
    g = blank()
    # Two solid cherries hanging from a forked stem.
    #
    # The first version put a dark highlight inside each cherry, meaning
    # to stop them reading as blobs. At this size it read as a pair of
    # eyes over a mouth -- a face, unmistakably, once seen. Solid discs
    # with a gap between them are what says "fruit"; the stem does the
    # rest of the work.
    for t in range(12):                       # the fork, left branch
        rect(g, 15 - t // 2, 6 + t, 16 - t // 2, 6 + t)
    for t in range(12):                       # right branch
        rect(g, 17 + t // 2, 6 + t, 18 + t // 2, 6 + t)
    rect(g, 15, 3, 18, 7)                     # where they meet
    disc(g, 9, 24, 6)
    disc(g, 23, 24, 6)
    rect(g, 15, 19, 17, 30, 0)                # keep the two apart
    for t in range(7):                        # a leaf off the top
        rect(g, 19 + t, 2 - t // 3, 23 + t, 4 - t // 3)
    return g

def bar_tile(n):
    """One, two or three stacked BAR plaques."""
    g = blank()
    gap = 2
    total = H - 4
    h = (total - gap * (n - 1)) // n
    y = 2
    for k in range(n):
        frame(g, 2, y, W - 3, y + h - 1)
        rect(g, 3, y + 1, W - 4, y + h - 2)
        # The wordmark, knocked out of the plaque. Three stacked
        # plaques leave 8 rows each, so the threshold has to admit that
        # -- at 9 the 3BAR tile came out as three blank stripes, which
        # is not a slot symbol, it is a barcode.
        tw = 5 * 3 - 1
        if h >= 7:
            text(g, (W - tw) // 2, y + (h - 5) // 2, "BAR", 0)
        y += h + gap
    return g

def sym_bell():
    g = blank()
    # body: a dome over a flared skirt
    disc(g, 16, 16, 9)
    rect(g, 7, 16, 25, 23)
    for t in range(4):
        rect(g, 6 - t, 20 + t, 26 + t, 23)
    rect(g, 4, 24, 28, 25)           # the lip
    rect(g, 14, 2, 18, 7)            # the crown
    disc(g, 16, 28, 3)               # the clapper
    rect(g, 11, 12, 14, 19, 0)       # a highlight, so it is not a blob
    return g

def sym_seven():
    g = blank()
    rect(g, 4, 3, 27, 8)             # the bar across the top
    for t in range(20):
        x = 26 - t
        rect(g, x - 6, 9 + t, x, 9 + t)
        if 9 + t >= H - 2:
            break
    return g

SYMS = [
    ("SL_BLANK",  sym_blank),
    ("SL_CHERRY", sym_cherry),
    ("SL_BAR",    lambda: bar_tile(1)),
    ("SL_BAR2",   lambda: bar_tile(2)),
    ("SL_BAR3",   lambda: bar_tile(3)),
    ("SL_BELL",   sym_bell),
    ("SL_SEVEN",  sym_seven),
]

def pack(g):
    out = []
    for y in range(H):
        w = 0
        for x in range(W):
            if g[y][x]:
                w |= (1 << x)
        out.append(w)
    return out

def main():
    rows = []
    for name, fn in SYMS:
        rows.append((name, pack(fn())))

    with open("sl_art.h", "w") as f:
        f.write("""#ifndef SL_ART_TILES_H
#define SL_ART_TILES_H

/*
 * Zeitlos slots -- symbol tiles.
 *
 * GENERATED by gen_slots.py. Do not edit; edit the generator and
 * re-run it. The output is committed, so it must regenerate
 * identically.
 *
 * One uint32_t per row with the LEAST significant bit leftmost, the
 * same packing the card art uses and what z_fb_hw_blit_mem() expects
 * -- the opposite of the font's MSB-first rows. A 32-wide tile is
 * exactly one word, so the stride is 1.
 *
 * Row r of symbol s is sl_sym_tile[s * SL_ART_H + r].
 *
 * The include guard is SL_ART_TILES_H, not SL_ART_H: the obvious guard
 * name collides with the tile-height macro two lines below it, which
 * the compiler reports as a redefinition and which would leave the
 * guard testing a number rather than its own presence.
 */

#include <stdint.h>

#define SL_ART_W %d
#define SL_ART_H %d
#define SL_ART_STRIDE 4      /* BYTES per row, as z_fb_hw_blit_mem takes it */

extern const uint32_t sl_sym_tile[];

#endif
""" % (W, H))

    with open("sl_art.c", "w") as f:
        f.write('/*\n * Zeitlos slots -- symbol tiles.\n *\n'
                ' * GENERATED by gen_slots.py. Do not edit.\n */\n\n'
                '#include "sl_art.h"\n\n'
                'const uint32_t sl_sym_tile[] = {\n')
        for name, words in rows:
            f.write("\n    /* %s */\n" % name)
            for i in range(0, H, 4):
                f.write("    " + " ".join("0x%08xu," % w
                        for w in words[i:i + 4]) + "\n")
        f.write("};\n")

    print("wrote sl_art.c and sl_art.h (%d symbols, %dx%d)" % (len(rows), W, H))

main()
