#!/usr/bin/env python3
#
# Zeitlos chess -- board tile generator.
#
#   python3 gen_pieces.py
#
# Emits pieces.c / pieces.h. Generated C is committed alongside the
# generator, the same arrangement sw/apps/gamedemo/gen_sprites.py and
# sw/data/icons/gen_dock_icon_data.py use: the app builds with no
# Python in the loop, and where the art came from is still obvious.
#
# -- why whole TILES and not sprites with a mask --
#
# The obvious design is a transparent piece sprite drawn over a square
# that was filled separately. That needs the blitter's masked-sprite
# mode, which needs raster ops, which zgfx.h says to probe for with
# z_fb_hw_rop_available() because an older bitstream silently treats
# every rop as COPY -- and a masked sprite that degrades to COPY is an
# opaque box around every piece. It also tears: z_fb_hw_blit_sprite()
# is two passes where there is no cookie-cut mode, so the sprite's
# footprint is momentarily blank, which is fine in a back buffer and
# not fine in a window drawn straight to the visible page.
#
# So each of the 24x24 squares is precomposed here, once, against both
# square colours, and the app draws a square with ONE opaque
# z_fb_hw_blit_mem(). That needs no raster ops, cannot tear, works on
# every bitstream, and makes a move two blits instead of a fill plus
# two masked sprites.
#
# The cost is memory, and it is small: 13 contents (empty plus twelve
# pieces) x 2 backgrounds x 24 rows x 4 bytes = 2,496 bytes of
# read-only data.
#
# -- the colour scheme, on a display with two colours --
#
# Light squares are white. Dark squares are a 50% checkerboard dither,
# which reads as grey -- and reads as grey rather than as texture
# because the video hardware doubles every framebuffer pixel, so one
# dither cell is two physical pixels across.
#
# Each piece is drawn once as a BODY, and the generator computes a
# one-pixel outline ring around it. The two colours are then:
#
#   white piece   body white, ring black
#   black piece   body black, ring white
#
# which is what makes both readable on both backgrounds. A white piece
# on a white square is defined by its black ring; a black piece on the
# grey dither is separated from it by its white ring. Drawing the
# pieces as plain silhouettes instead -- black for one side, outline
# only for the other -- is the version that looks fine on paper and
# turns into a smudge on the dark squares.
#
# -- why the shapes are drawn from primitives --
#
# The alternative is 24 lines of ASCII art per piece, which is 288
# hand-placed characters where a single misplaced one is a notch in a
# crown that nobody sees until it is on a screen. Circles, trapezoids
# and polygons are fewer numbers, and each number means something
# ("the head is four pixels across"), so the shapes can be adjusted by
# reasoning instead of by counting.

import sys
import math

W = H = 24

# ---------------------------------------------------------------- #
# a tiny rasterizer
# ---------------------------------------------------------------- #

def blank():
    return [[0] * W for _ in range(H)]

def put(g, x, y):
    if 0 <= x < W and 0 <= y < H:
        g[y][x] = 1

def rect(g, x0, y0, x1, y1):
    for y in range(int(y0), int(y1) + 1):
        for x in range(int(x0), int(x1) + 1):
            put(g, x, y)

def disc(g, cx, cy, r):
    # Half-pixel centres, so a disc of radius r is symmetrical about
    # the centre rather than a pixel wider on one side.
    rr = r * r
    for y in range(H):
        for x in range(W):
            dx = x + 0.5 - cx
            dy = y + 0.5 - cy
            if dx * dx + dy * dy <= rr:
                put(g, x, y)

def ring(g, cx, cy, r_out, r_in):
    for y in range(H):
        for x in range(W):
            dx = x + 0.5 - cx
            dy = y + 0.5 - cy
            d = dx * dx + dy * dy
            if r_in * r_in <= d <= r_out * r_out:
                put(g, x, y)

def poly(g, pts):
    # Even-odd scanline fill. Vertices are floats; the scanline is
    # sampled at the middle of each row.
    ys = [p[1] for p in pts]
    for y in range(int(min(ys)), int(max(ys)) + 1):
        sy = y + 0.5
        xs = []
        n = len(pts)
        for i in range(n):
            x0, y0 = pts[i]
            x1, y1 = pts[(i + 1) % n]
            if y0 == y1:
                continue
            if min(y0, y1) <= sy < max(y0, y1):
                t = (sy - y0) / (y1 - y0)
                xs.append(x0 + t * (x1 - x0))
        xs.sort()
        for i in range(0, len(xs) - 1, 2):
            # A pixel is filled when its CENTRE is inside the span.
            # Rounding the edges instead biases every span one pixel to
            # the left, which is invisible on a single shape and very
            # visible on a board of them: the pieces sit half a pixel
            # off centre in their squares and the whole board looks
            # skewed. disc() already samples centres, so this is also
            # what makes the two agree.
            x0 = int(math.ceil(xs[i] - 0.5))
            x1 = int(math.floor(xs[i + 1] - 0.5))
            for x in range(x0, x1 + 1):
                put(g, x, y)

def trapezoid(g, y0, y1, half0, half1, cx=12.0):
    poly(g, [(cx - half0, y0), (cx + half0, y0),
             (cx + half1, y1 + 1), (cx - half1, y1 + 1)])

def carve(g, x0, y0, x1, y1):
    """Clear a rectangle -- for crenellations and the bishop's slit."""
    for y in range(int(y0), int(y1) + 1):
        for x in range(int(x0), int(x1) + 1):
            if 0 <= x < W and 0 <= y < H:
                g[y][x] = 0

def carve_poly(g, pts):
    """Clear the area of a polygon -- the bishop's slit, which has to
    run out through the edge of the mitre. A slit that stops short
    leaves the strip beyond it as a detached island, which at this size
    reads as a rendering fault rather than as a cut."""
    tmp = blank()
    poly(tmp, pts)
    for y in range(H):
        for x in range(W):
            if tmp[y][x]:
                g[y][x] = 0

def outline(body):
    """The one-pixel ring immediately outside the body."""
    r = blank()
    for y in range(H):
        for x in range(W):
            if body[y][x]:
                continue
            near = False
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    if dx == 0 and dy == 0:
                        continue
                    yy, xx = y + dy, x + dx
                    if 0 <= xx < W and 0 <= yy < H and body[yy][xx]:
                        near = True
            if near:
                r[y][x] = 1
    return r

# ---------------------------------------------------------------- #
# the pieces
#
# All six stand on a common base so they line up on the board, and all
# six are 22 pixels tall at most, leaving a pixel of margin top and
# bottom for the outline ring to live in.
# ---------------------------------------------------------------- #

# The foot every piece shares. It starts at 18 because that is where
# every body above ends -- leave a row between them and the outline
# ring fills it, which draws a bright line straight across the piece's
# waist. On a black piece that reads as a groove; on a white one it is
# a black bar through the middle of the base, and it looked like a
# rendering fault rather than a design.
BASE_Y0, BASE_Y1 = 18, 21

def add_base(g, half_top=5.0, half_bottom=8.0):
    trapezoid(g, BASE_Y0, BASE_Y1, half_top, half_bottom)
    rect(g, 12 - half_bottom, BASE_Y1, 12 + half_bottom - 1, BASE_Y1)

def pawn():
    g = blank()
    disc(g, 12, 6.0, 3.8)                  # head
    trapezoid(g, 9, 12, 1.4, 2.4)          # neck
    trapezoid(g, 12, 17, 3.6, 5.0)         # skirt
    rect(g, 8, 12, 15, 13)                 # collar
    add_base(g, 5.0, 7.0)
    return g

def rook():
    g = blank()
    rect(g, 5, 4, 18, 8)                   # battlement
    carve(g, 8, 3, 9, 6)                   # two notches in the top
    carve(g, 14, 3, 15, 6)
    trapezoid(g, 9, 17, 5.0, 6.0)          # tapering tower
    rect(g, 6, 17, 17, 18)
    add_base(g, 6.0, 8.0)
    return g

def knight():
    g = blank()
    # A horse's head in profile, facing left: muzzle at bottom left,
    # up over the nose to two ears, down the crest of the neck, and
    # back along the chest to the jaw. Listed clockwise.
    poly(g, [
        (4.0, 10.5),      # muzzle, top
        (7.0, 8.0),       # bridge of the nose
        (9.5, 5.0),       # forehead
        (10.8, 2.5),      # near ear
        (12.0, 5.5),      # between the ears
        (13.8, 2.8),      # far ear
        (15.5, 6.5),
        (17.2, 10.0),     # crest of the neck
        (17.5, 14.0),
        (17.0, 18.0),     # back of the neck, at the base
        (7.5, 18.0),      # chest
        (9.0, 14.5),      # throat
        (6.5, 13.5),      # jaw
        (4.0, 13.0),      # muzzle, bottom
    ])
    carve(g, 9, 8, 10, 9)                  # eye
    add_base(g, 6.0, 8.0)
    return g

def bishop():
    g = blank()
    disc(g, 12, 2.6, 1.5)                  # finial
    poly(g, [(12.0, 3.2), (16.5, 12.0), (7.5, 12.0)])   # mitre
    disc(g, 12, 9.0, 4.2)
    # The bishop's slit: a thin wedge from just below the apex out
    # through the right-hand edge. It has to REACH the edge. A slit
    # that stops inside the mitre leaves the sliver beyond it floating
    # free, which is what the first two versions of this did -- and a
    # two-pixel island next to a two-pixel gap does not read as a cut,
    # it reads as a corrupted tile.
    carve_poly(g, [(11.5, 3.8), (17.5, 11.5), (17.5, 8.5)])
    rect(g, 7, 12, 16, 13)                 # brim
    trapezoid(g, 14, 17, 3.0, 5.0)
    add_base(g, 5.5, 7.5)
    return g

def queen():
    g = blank()
    # Five points, each tipped with a bead.
    for cx, ty in ((4.5, 5.0), (8.0, 3.0), (12.0, 1.8), (16.0, 3.0),
                   (19.5, 5.0)):
        disc(g, cx, ty, 1.5)
        poly(g, [(cx - 1.2, ty), (cx + 1.2, ty), (12.0 + (cx - 12.0) * 0.35, 12.0)])
    poly(g, [(4.5, 5.0), (19.5, 5.0), (17.0, 13.0), (7.0, 13.0)])
    carve(g, 6, 5, 7, 7)                   # scallops between the points
    carve(g, 10, 4, 10, 6)
    carve(g, 13, 4, 13, 6)
    carve(g, 16, 5, 17, 7)
    rect(g, 6, 13, 17, 14)                 # collar
    trapezoid(g, 15, 17, 4.0, 5.5)
    add_base(g, 6.0, 8.0)
    return g

def king():
    g = blank()
    rect(g, 11, 0, 12, 5)                  # cross
    rect(g, 9, 2, 14, 3)
    poly(g, [(6.0, 6.0), (18.0, 6.0), (16.5, 13.0), (7.5, 13.0)])  # crown
    disc(g, 7.0, 7.0, 1.8)
    disc(g, 17.0, 7.0, 1.8)
    rect(g, 6, 13, 17, 14)
    trapezoid(g, 15, 17, 4.0, 5.5)
    add_base(g, 6.0, 8.0)
    return g

PIECES = [
    ("pawn", pawn), ("knight", knight), ("bishop", bishop),
    ("rook", rook), ("queen", queen), ("king", king),
]

# ---------------------------------------------------------------- #
# composition
# ---------------------------------------------------------------- #

def background(dark):
    """Light squares are white; dark squares are a 50% dither."""
    g = blank()
    if dark:
        for y in range(H):
            for x in range(W):
                g[y][x] = (x + y) & 1
    return g

def compose(dark, body, ring_, white_piece):
    """A finished 24x24 tile. 1 is ink."""
    tile = background(dark)
    if body is None:
        return tile
    for y in range(H):
        for x in range(W):
            if body[y][x]:
                tile[y][x] = 0 if white_piece else 1
            elif ring_[y][x]:
                tile[y][x] = 1 if white_piece else 0
    return tile

def pack(tile):
    """One uint32 per row. Pixel x is bit (x & 31) -- least
    significant bit LEFTMOST, which is the framebuffer's own order and
    what z_fb_hw_blit_mem() reads (see sw/common/zbm.h)."""
    rows = []
    for y in range(H):
        word = 0
        for x in range(W):
            if tile[y][x]:
                word |= 1 << x
        rows.append(word)
    return rows

def main():
    shapes = []
    for name, fn in PIECES:
        body = fn()
        shapes.append((name, body, outline(body)))

    # index 0 = empty, 1..6 = white pawn..king, 7..12 = black pawn..king
    tiles = [[None] * 13 for _ in range(2)]
    for dark in (0, 1):
        tiles[dark][0] = pack(compose(dark, None, None, False))
        for i, (name, body, ring_) in enumerate(shapes):
            tiles[dark][1 + i] = pack(compose(dark, body, ring_, True))
            tiles[dark][7 + i] = pack(compose(dark, body, ring_, False))

    with open("pieces.h", "w") as f:
        f.write("""#ifndef CHESS_PIECES_H
#define CHESS_PIECES_H

/*
 * Zeitlos chess -- board tiles.
 *
 * GENERATED by gen_pieces.py. Do not edit; edit the generator and run
 * it. See that file for why a square is a precomposed opaque tile
 * rather than a background plus a masked sprite.
 *
 * Each tile is 24x24, one uint32_t per row, so the source STRIDE IS 4
 * BYTES -- which is not padding, it is what the blitter requires. It
 * walks the source by adding the stride to a BYTE address while
 * issuing word reads that ignore the low two bits, so a 24-pixel-wide
 * tile stored three bytes to the row would make rows resolve to the
 * same word and repeat.
 *
 * Bit order is the framebuffer's: pixel x at bit (x & 31), least
 * significant bit LEFTMOST. That is the opposite of font and icon
 * data (zfont.h, zicon.h). Getting it backwards mirrors each tile
 * horizontally, which on a chessboard looks like the knights facing
 * the wrong way and nothing else -- worth knowing, because that is a
 * symptom people mistake for an art choice.
 */

#include <stdint.h>

#define CP_TILE_W 24
#define CP_TILE_H 24
#define CP_TILE_STRIDE 4

/* Background: which colour square the tile is composed against. */
#define CP_LIGHT 0
#define CP_DARK  1

/* Contents: 0 is an empty square, then white pawn..king, then black
 * pawn..king. cp_tile_index() converts a ce_core.c piece byte. */
#define CP_EMPTY 0

extern const uint32_t cp_tiles[2][13][CP_TILE_H];

#endif
""")

    with open("pieces.c", "w") as f:
        f.write("/*\n"
                " * Zeitlos chess -- board tiles.\n"
                " * GENERATED by gen_pieces.py. Do not edit.\n"
                " */\n\n#include \"pieces.h\"\n\n")
        f.write("const uint32_t cp_tiles[2][13][CP_TILE_H] = {\n")
        for dark in (0, 1):
            f.write("\t{ /* %s squares */\n" % ("dark" if dark else "light"))
            names = ["empty"] + ["white " + n for n, _ in PIECES] + \
                    ["black " + n for n, _ in PIECES]
            for i in range(13):
                f.write("\t\t{ /* %s */\n" % names[i])
                rows = tiles[dark][i]
                for y in range(0, H, 4):
                    f.write("\t\t\t" + " ".join(
                        "0x%08x," % rows[y + k] for k in range(4)) + "\n")
                f.write("\t\t},\n")
            f.write("\t},\n")
        f.write("};\n")

    # A look at what was generated, so a shape can be judged without
    # putting it on hardware. Shows the composed TILE, on both
    # backgrounds, because a shape that reads fine as a silhouette can
    # still disappear into the dark square's dither.
    if "--show" in sys.argv:
        for idx, (name, body, ring_) in enumerate(shapes):
            for white_piece in (True, False):
                side = "white" if white_piece else "black"
                print("%s %s   (light square / dark square)" % (side, name))
                a = compose(0, body, ring_, white_piece)
                b = compose(1, body, ring_, white_piece)
                for y in range(H):
                    la = "".join("#" if a[y][x] else "." for x in range(W))
                    lb = "".join("#" if b[y][x] else "." for x in range(W))
                    print("  " + la + "  " + lb)
                print()

    print("gen_pieces.py: wrote pieces.c and pieces.h "
          "(%d bytes of tile data)" % (2 * 13 * H * 4))

if __name__ == "__main__":
    main()
