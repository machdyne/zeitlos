#!/usr/bin/env python3
"""
Zeitlos Casino -- banner image generator.

    cd sw/apps/casino && python3 gen_banner.py [banner.png]

Reads a 1bpp-able PNG and emits cs_banner.c / cs_banner.h.

TO REPLACE THE BANNER: drop your own image in as banner.png and re-run
this. Anything closer to white than mid-grey becomes a lit pixel, so a
plain black-and-white image works and a greyscale one is thresholded.
The image is scaled to fit CS_BANNER_W x CS_BANNER_H if it is not
already that size -- nearest neighbour, because smoothing a picture
that is about to be reduced to one bit only blurs the edges it is
about to lose.

The generated C is committed, so this must regenerate identically --
check with `git diff` after running it.

The placeholder shipped here is exactly that: a frame, a wordmark and
a note saying so. It is deliberately plain, because a placeholder that
looks finished is one nobody replaces.
"""

import sys

W = 312

# 88, not 100.
#
# The window has to hold the banner, the net-worth lines, a row per
# game and three text rows. At 100 the fifth game did not fit, and the
# layout said so rather than drawing off the bottom -- which is the
# check doing its job, but it still meant choosing.
#
# 88 leaves room for seven games, which is more than there will be.
H = 88

# An 8x8 ordered (Bayer) matrix, as thresholds out of 64.
#
# ORDERED, NOT ERROR-DIFFUSED. Floyd-Steinberg gives a better-looking
# still image, but its noise is unstructured -- on a 1bpp panel that
# reads as grain, and any later rescale turns it to mush. An ordered
# matrix produces a regular texture that survives being looked at on a
# small screen, which is the same reason zgfx.c's shaded fill is an
# ordered dither.
BAYER8 = [
    [ 0, 32,  8, 40,  2, 34, 10, 42],
    [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44,  4, 36, 14, 46,  6, 38],
    [60, 28, 52, 20, 62, 30, 54, 22],
    [ 3, 35, 11, 43,  1, 33,  9, 41],
    [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47,  7, 39, 13, 45,  5, 37],
    [63, 31, 55, 23, 61, 29, 53, 21],
]

def load(path):
    """Scale to width, crop the sky, then dither."""
    try:
        from PIL import Image
    except ImportError:
        return None
    try:
        im = Image.open(path).convert("L")
    except (IOError, OSError):
        return None

    # Fit the WIDTH and crop the height. A banner that is scaled to fit
    # both is a squashed banner, and on a photograph of a building that
    # is immediately obvious. The crop is anchored to the BOTTOM: on a
    # night shot the top is sky, which is the part with nothing in it.
    sw, sh = im.size
    th = max(1, int(round(sh * W / float(sw))))
    im = im.resize((W, th), Image.LANCZOS)
    if th > H:
        im = im.crop((0, th - H, W, th))
    elif th < H:
        pad = Image.new("L", (W, H), 0)
        pad.paste(im, (0, (H - th) // 2))
        im = pad

    # A BLACK POINT AND A GAMMA, not autocontrast.
    #
    # Autocontrast was the first attempt and it was exactly backwards
    # for a night photograph: stretching the levels lifted the sky into
    # the dither and the whole banner came out as grain with a building
    # somewhere in it.
    #
    # A night shot wants its blacks CRUSHED. Everything below 24 goes to
    # pure black, which is the sky, and the rest is pulled down by a
    # gentle gamma so the lit stonework does not blow out. Chosen by
    # rendering four curves and looking at them.
    black, gamma = 24, 1.3
    im = im.point([0 if i <= black else
        min(255, int(255 * (((i - black) / (255.0 - black)) ** gamma)))
        for i in range(256)])

    px = im.load()
    out = []
    for y in range(H):
        row = []
        for x in range(W):
            t = (BAYER8[y & 7][x & 7] + 1) * 255 // 65
            row.append(1 if px[x, y] > t else 0)
        out.append(row)
    return out

def placeholder():
    g = [[0] * W for _ in range(H)]

    def rect(x0, y0, x1, y1, v=1):
        for y in range(max(0, y0), min(H, y1 + 1)):
            for x in range(max(0, x0), min(W, x1 + 1)):
                g[y][x] = v

    def frame(x0, y0, x1, y1, v=1):
        rect(x0, y0, x1, y0, v); rect(x0, y1, x1, y1, v)
        rect(x0, y0, x0, y1, v); rect(x1, y0, x1, y1, v)

    frame(0, 0, W - 1, H - 1)
    frame(2, 2, W - 3, H - 3)

    # A row of lamps along the top and bottom, which is the one thing
    # every casino frontage has.
    for x in range(8, W - 8, 10):
        rect(x, 5, x + 2, 7)
        rect(x, H - 8, x + 2, H - 6)

    # ZEITLOS CASINO, in a 5x7 block alphabet -- large enough to read
    # at this size, and only the letters that word needs.
    A = {
        'Z': ["#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"],
        'E': ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
        'I': ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####"],
        'T': ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
        'L': ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
        'O': [".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
        'S': [".####", "#....", "#....", ".###.", "....#", "....#", "####."],
        'C': [".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."],
        'A': [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
        'N': ["#...#", "##..#", "##..#", "#.#.#", "#..##", "#..##", "#...#"],
        ' ': [".....", ".....", ".....", ".....", ".....", ".....", "....."],
    }

    def word(s, x, y, scale):
        for ch in s:
            rows = A[ch]
            for j, row in enumerate(rows):
                for i, c in enumerate(row):
                    if c != '#':
                        continue
                    for dy in range(scale):
                        for dx in range(scale):
                            px, py = x + i * scale + dx, y + j * scale + dy
                            if 0 <= px < W and 0 <= py < H:
                                g[py][px] = 1
            x += (5 + 1) * scale
        return x

    text = "ZEITLOS CASINO"
    scale = 2
    tw = len(text) * 6 * scale - scale
    word(text, (W - tw) // 2, 16, scale)

    # A 3x5 note underneath saying what this is.
    small = {
        'p': ["###", "#.#", "###", "#..", "#.."],
        'l': ["#..", "#..", "#..", "#..", "###"],
        'a': [".#.", "#.#", "###", "#.#", "#.#"],
        'c': [".##", "#..", "#..", "#..", ".##"],
        'e': ["###", "#..", "##.", "#..", "###"],
        'h': ["#.#", "#.#", "###", "#.#", "#.#"],
        'o': ["###", "#.#", "#.#", "#.#", "###"],
        'd': ["##.", "#.#", "#.#", "#.#", "##."],
        'r': ["###", "#.#", "##.", "#.#", "#.#"],
        ' ': ["...", "...", "...", "...", "..."],
        '-': ["...", "...", "###", "...", "..."],
    }
    # Only the letters the small alphabet above actually has. The
    # first version said "replace banner.png" and came out as
    # "REPLACE A ER P", which is a placeholder failing at the one job
    # a placeholder has.
    note = "placeholder - replace"
    x = 10
    for ch in note:
        rows = small.get(ch)
        if rows is None:
            x += 4
            continue
        for j, row in enumerate(rows):
            for i, c in enumerate(row):
                if c == '#' and 0 <= x + i < W:
                    g[H - 18 + j][x + i] = 1
        x += 4

    return g

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "banner.png"
    g = load(path)
    src = path
    if g is None:
        g = placeholder()
        src = "the built-in placeholder (no readable %s)" % path

    stride = (W + 31) // 32
    words = []
    for y in range(H):
        row = [0] * stride
        for x in range(W):
            if g[y][x]:
                row[x >> 5] |= 1 << (x & 31)
        words.extend(row)

    with open("cs_banner.h", "w") as f:
        f.write("""#ifndef CS_BANNER_IMG_H
#define CS_BANNER_IMG_H

/*
 * Zeitlos Casino -- the banner.
 *
 * GENERATED by gen_banner.py. Do not edit; replace banner.png and
 * re-run it.
 *
 * One uint32_t per word with the LEAST significant bit leftmost, the
 * same packing the card and slot tiles use and what z_fb_hw_blit_mem()
 * expects -- the opposite of the font's MSB-first rows.
 *
 * Row r starts at cs_banner[r * CS_BANNER_STRIDE_W].
 */

#include <stdint.h>

#define CS_BANNER_W %d
#define CS_BANNER_H %d
#define CS_BANNER_STRIDE_W %d
#define CS_BANNER_STRIDE (%d * 4)   /* BYTES, as z_fb_hw_blit_mem takes it */

extern const uint32_t cs_banner[];

#endif
""" % (W, H, stride, stride))

    with open("cs_banner.c", "w") as f:
        f.write("/*\n * Zeitlos Casino -- the banner.\n *\n"
                " * GENERATED by gen_banner.py from %s.\n"
                " * Do not edit; replace banner.png and re-run it.\n */\n\n"
                '#include "cs_banner.h"\n\n'
                "const uint32_t cs_banner[] = {\n" % src)
        for i in range(0, len(words), 4):
            f.write("    " + " ".join("0x%08xu," % w
                    for w in words[i:i + 4]) + "\n")
        f.write("};\n")

    print("wrote cs_banner.c and cs_banner.h (%dx%d) from %s" % (W, H, src))

main()
