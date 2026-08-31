#!/usr/bin/env python3
#
# Zeitlos gamedemo -- sprite and tile sheet generator.
#
# Emits sprites.c / sprites.h from the ASCII art below. Placeholder art
# for now; replacing it with real art means editing the strings (or
# pointing LOAD_PNG at a file) and re-running, not touching any C.
#
#   python3 gen_sprites.py
#
# Same shape as sw/data/icons/gen_dock_icon_data.py -- generated C
# committed alongside the generator, so the app builds with no Python
# in the loop and the provenance is still obvious.
#
# -- two output formats, and why --
#
# TILES are opaque and blitted by HARDWARE. gpu_blit.v's main-memory
# copy mode (CTRL_SRCMEM) wants exactly one thing: a 1bpp bitmap whose
# bit order matches the framebuffer's, i.e. pixel x at bit (x & 31) of
# word (x >> 5), least significant bit leftmost. Background tiles are
# always opaque -- every pixel of a tile cell belongs to that tile --
# so there is nothing a mask would add, and the blitter does the work.
#
# Tiles are emitted as uint32_t rows, one per row, so the source STRIDE
# IS 4 BYTES. That is not padding for tidiness -- it is required.
# gpu_blit.v walks the source by adding src_stride to a BYTE address
# (mem_row_addr) while issuing WORD reads that ignore the low two bits.
# A 16-bit-wide tile stored as uint16_t has a 2-byte stride, so rows 0
# and 1 resolve to the same word and every odd row renders as a repeat
# of the even row above it. The upper 16 bits of each word are unused
# and read as zero, which the blitter masks off anyway since the blit
# is only 16 pixels wide.
#
# SPRITES need TRANSPARENCY, and the blitter has no raster op yet: its
# copy mode overwrites the destination rectangle wholesale, so a mouse
# blitted over a brick wall arrives inside an opaque 16x16 box of its
# own background. So sprites get a MASK plane as well as a DATA plane
# and are drawn in software as (dst & ~mask) | data.
#
# That is not a workaround so much as the honest split: the hardware is
# very good at the opaque case and has no answer for the masked one
# until gpu_blit.v grows a ROP (see docs/game_mode.md). Sprites are a
# handful per frame and tiles are hundreds, so the expensive path is
# the rare one.
#
# Sprites are emitted TWICE, in two formats, because there are two
# consumers:
#
#   uint16_t rows  -- the software fallback in gamedemo.c, which
#                     shifts each row into two adjacent framebuffer
#                     words itself. One 16-bit value per row makes that
#                     a single expression rather than a carry between
#                     words.
#
#   uint32_t rows  -- the hardware blitter, which needs the same
#                     4-byte stride the tiles do (see above: a 2-byte
#                     stride makes gpu_blit.v read rows 0 and 1 from
#                     the same word).
#
# And in two FACINGS. The blitter has no reverse mode, so the mirroring
# the software path did with three shift-mask steps has to come from
# the sheet instead. Mirroring at runtime into a temporary bitmap would
# mean rebuilding it every frame, which defeats the point of moving the
# blit to hardware in the first place.
#
# Four extra 32-byte bitmaps per sprite. That is the trade: a little
# more ROM for a blit that costs the CPU nothing.

import sys

TILE_W = TILE_H = 16
SPR_W = SPR_H = 16

# '.' transparent (sprites) or background (tiles), '#' set, ' ' also
# transparent -- allowed so art can be drawn with spaces where that is
# easier to read.

TILES = {}

TILES["EMPTY"] = [
    "................",
] * 16

BLANK = [
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
]

TILES["GROUND"] = [
    "################",
    "################",
    "##.##.##.##.##.#",
    "#.##.##.##.##.##",
    "################",
    "#.##.##.##.##.##",
    "##.##.##.##.##.#",
    "################",
    "################",
    "##.##.##.##.##.#",
    "#.##.##.##.##.##",
    "################",
    "#.##.##.##.##.##",
    "##.##.##.##.##.#",
    "################",
    "################",
]

TILES["GRASS"] = [
    "................",
    "..#....#....#...",
    ".###..###..###..",
    "################",
    "################",
    "##.##.##.##.##.#",
    "#.##.##.##.##.##",
    "################",
    "#.##.##.##.##.##",
    "##.##.##.##.##.#",
    "################",
    "################",
    "##.##.##.##.##.#",
    "#.##.##.##.##.##",
    "################",
    "################",
]

TILES["BOX"] = [
    "################",
    "#..............#",
    "#.############.#",
    "#.#..........#.#",
    "#.#.########.#.#",
    "#.#.#......#.#.#",
    "#.#.#.####.#.#.#",
    "#.#.#.#..#.#.#.#",
    "#.#.#.#..#.#.#.#",
    "#.#.#.####.#.#.#",
    "#.#.#......#.#.#",
    "#.#.########.#.#",
    "#.#..........#.#",
    "#.############.#",
    "#..............#",
    "################",
]

TILES["CHEESE"] = [
    "................",
    "................",
    "..############..",
    ".####.#########.",
    "..########.###..",
    "..############..",
    "...##########...",
    "....###.####....",
    ".....######.....",
    "......####......",
    ".......##.......",
    "................",
    "................",
    "................",
    "................",
    "................",
]

SPRITES = {}

# The mouse. FACES RIGHT -- eye toward the right edge, tail trailing at
# the left. The game mirrors it in software when running left.

SPRITES["MOUSE_STAND"] = [
    "................",
    "................",
    "................",
    "................",
    "................",
    "........##..##..",
    "......#####.##..",
    "##..##########..",
    ".###########.##.",
    "..##############",
    "..#############.",
    "....##########..",
    "....##....##....",
    "....##....##....",
    "....##....##....",
    "...###....###...",
]
 
SPRITES["MOUSE_RUN1"] = [
    "................",
    "................",
    "................",
    "................",
    "................",
    "...... .##..##..",
    "......####.##...",
    "....##########..",
    "...#########.##.",
    "...#############",
    "#..############.",
    ".##.##########..",
    "....##.....##...",
    "...##.......##..",
    "..##.........##.",
    ".###.........###",
]
 
SPRITES["MOUSE_RUN2"] = [
    "................",
    "................",
    "................",
    "................",
    "................",
    ".##....##.##....",
    "..#...####.##...",
    "..############..",
    "...#########.##.",
    "...#############",
    "...############.",
    "....##########..",
    ".....##..##.....",
    ".....##..##.....",
    "....###..###....",
    "................",
]
 
SPRITES["MOUSE_JUMP"] = [
    "................",
    "................",
    "................",
    "................",
    "......####.##...",
    "....##########..",
    "...#########.##.",
    "...#############",
    "...############.",
    ".#############..",
    "#...###...###...",
    "#....##....##...",
    ".....###...###..",
    "................",
    "................",
    "................",
]
 
SPRITES["CAT1"] = [
    "................",
    "................",
    ".###.......##...",
    "#..........###..",
    "#..........####.",
    "#.........###.##",
    "#.........######",
    "#..####..######.",
    "##############..",
    ".#############..",
    "..############..",
    "..###########...",
    "..###########...",
    "...###..######..",
    "..#####.##..###.",
    "..##.##.##...##.",
]
 
SPRITES["CAT2"] = [
    "................",
    ".###.......##...",
    "#..........###..",
    "#..........####.",
    "#.........###.##",
    "#.........######",
    "#..####..######.",
    "##############..",
    ".#############..",
    "..############..",
    "..###########...",
    "..##########....",
    "..####..###.....",
    "..##....###.....",
    "..###....###....",
    "..###....###....",
]

# -- background and ambient sprites --
#
# Clouds and trees are drawn as PAIRS of 16x16 cells rather than as one
# wide bitmap, because every sprite here shares one size and one code
# path. A 32x16 cloud is CLOUD_L next to CLOUD_R; a 32-tall tree is
# TREE_TOP above TREE_BOT. Two blits instead of one, and no special
# case anywhere.

SPRITES["CLOUD_L"] = [
    "................",
    "................",
    "................",
    ".........#####..",
    ".......#########",
    "......##########",
    ".....###########",
    "....############",
    "...#############",
    "..##############",
    "..##############",
    "...#############",
    ".....###########",
    "................",
    "................",
    "................",
]

SPRITES["CLOUD_R"] = [
    "................",
    "................",
    "....####........",
    "##########......",
    "############....",
    "#############...",
    "##############..",
    "###############.",
    "###############.",
    "##############..",
    "#############...",
    "###########.....",
    "########........",
    "................",
    "................",
    "................",
]

SPRITES["TREE_TOP"] = [
    "......####......",
    ".....######.....",
    "....########....",
    "...##########...",
    "..############..",
    ".##############.",
    "################",
    ".##############.",
    "..############..",
    "...##########...",
    "..############..",
    ".##############.",
    "################",
    ".##############.",
    "..############..",
    "...##########...",
]

SPRITES["TREE_BOT"] = [
    "....########....",
    ".....######.....",
    "......####......",
    "......####......",
    "......####......",
    "......####......",
    "......####......",
    "......####......",
    "......####......",
    ".....######.....",
    ".....######.....",
    "....########....",
    "....########....",
    "...##########...",
    "..############..",
    "................",
]

SPRITES["BIRD1"] = [
    "................",
    "................",
    "................",
    "................",
    "..##........##..",
    "...##......##...",
    "....##....##....",
    ".....##..##.....",
    "......####......",
    ".......##.......",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
]

SPRITES["BIRD2"] = [
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "................",
    "..####....####..",
    "....##....##....",
    ".....######.....",
    "......####......",
    ".......##.......",
    "................",
    "................",
    "................",
    "................",
]


def parse(rows, w, h, name):
    if len(rows) != h:
        sys.exit("%s: %d rows, want %d" % (name, len(rows), h))
    out = []
    for y, r in enumerate(rows):
        if len(r) != w:
            sys.exit("%s row %d: %d cols, want %d" % (name, y, len(r), w))
        out.append(r)
    return out


def pack_rows(rows):
    """Rows as integers, LSB = leftmost pixel.

    LSB-first is the framebuffer's own convention (see zgfx.h's
    z_fb_set_pixel) and NOT the MSB-first order z_font_t glyphs use.
    Getting this backwards produces sprites that are mirrored per
    16-pixel group, which looks like a shuffled sprite sheet rather
    than a bit order mistake -- worth stating once here rather than
    rediscovering."""
    words = []
    for r in rows:
        v = 0
        for x, c in enumerate(r):
            if c == '#':
                v |= (1 << x)
        words.append(v)
    return words


def mask_words(rows):
    """1 wherever the sprite owns the pixel -- set OR explicitly clear.

    Every '#' is opaque, and so is nothing else: '.' and ' ' are both
    see-through. A sprite that needs a black pixel over a light
    background therefore cannot express it with this art format. That
    is a real limitation of one-bit sprites with a one-bit mask and it
    is why the cat has an outline -- the outline IS the mask edge."""
    words = []
    for r in rows:
        v = 0
        for x, c in enumerate(r):
            if c == '#':
                v |= (1 << x)
        words.append(v)
    return words


def emit_rows(f, name, words, ctype="uint16_t", width=4):
    f.write("const %s %s[%d] = {\n" % (ctype, name, len(words)))
    per = 8 if width == 4 else 6
    for i in range(0, len(words), per):
        f.write("\t" + " ".join(("0x%0*x," % (width, w)) for w in words[i:i + per]) + "\n")
    f.write("};\n\n")


def main():
    tiles = [(n, parse(r, TILE_W, TILE_H, n)) for n, r in TILES.items()]
    sprites = [(n, parse(r, SPR_W, SPR_H, n)) for n, r in SPRITES.items()]

    with open("sprites.h", "w") as f:
        f.write("/* GENERATED by gen_sprites.py -- do not edit.\n"
                " * Edit the art in that script and re-run it instead.\n"
                " * See its header for the two formats and why they differ. */\n\n")
        f.write("#ifndef GAMEDEMO_SPRITES_H\n#define GAMEDEMO_SPRITES_H\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write("#define GD_TILE_W %d\n#define GD_TILE_H %d\n" % (TILE_W, TILE_H))
        f.write("#define GD_SPR_W %d\n#define GD_SPR_H %d\n\n" % (SPR_W, SPR_H))

        f.write("/* Tile ids -- index into gd_tiles[]. Order matters: the\n"
                " * level map stores these values directly, and GD_TILE_SOLID\n"
                " * below assumes every solid tile sorts at or after BOX. */\n")
        f.write("enum {\n")
        for i, (n, _) in enumerate(tiles):
            f.write("\tGD_TILE_%s = %d,\n" % (n, i))
        f.write("\tGD_TILE_COUNT = %d\n};\n\n" % len(tiles))

        f.write("/* Each tile is GD_TILE_H rows of GD_TILE_W bits, LSB =\n"
                " * leftmost pixel -- the framebuffer's own bit order (zgfx.h),\n"
                " * NOT z_font_t's MSB-first one. */\n")
        for n, _ in tiles:
            f.write("extern const uint32_t gd_tile_%s[GD_TILE_H];\n" % n.lower())
        f.write("\nextern const uint32_t *const gd_tiles[GD_TILE_COUNT];\n\n")
        f.write("/* Source stride handed to z_fb_hw_blit_mem(). MUST be 4 --\n"
                " * see gen_sprites.py's header for what a 2-byte stride does\n"
                " * to the blitter's row walk. */\n")
        f.write("#define GD_TILE_STRIDE 4\n\n")

        f.write("/* Sprites carry a DATA plane and a MASK plane, because\n"
                " * gpu_blit.v's copy mode has no raster op and would draw an\n"
                " * opaque box. Composited in software as\n"
                " * (dst & ~mask) | data -- see gamedemo.c's spr_draw(). */\n")
        for n, _ in sprites:
            ln = n.lower()
            f.write("extern const uint16_t gd_spr_%s[GD_SPR_H];\n" % ln)
            f.write("extern const uint16_t gd_msk_%s[GD_SPR_H];\n" % ln)
            f.write("extern const uint32_t gd_hspr_%s[GD_SPR_H];\n" % ln)
            f.write("extern const uint32_t gd_hmsk_%s[GD_SPR_H];\n" % ln)
            f.write("extern const uint32_t gd_hspr_%s_m[GD_SPR_H];\n" % ln)
            f.write("extern const uint32_t gd_hmsk_%s_m[GD_SPR_H];\n" % ln)
        f.write("\n#endif\n")

    with open("sprites.c", "w") as f:
        f.write("/* GENERATED by gen_sprites.py -- do not edit. */\n\n")
        f.write('#include "sprites.h"\n\n')
        for n, rows in tiles:
            emit_rows(f, "gd_tile_" + n.lower(), pack_rows(rows),
                      "uint32_t", 8)
        f.write("const uint32_t *const gd_tiles[GD_TILE_COUNT] = {\n")
        for n, _ in tiles:
            f.write("\tgd_tile_%s,\n" % n.lower())
        f.write("};\n\n")
        for n, rows in sprites:
            ln = n.lower()
            mirrored = [r[::-1] for r in rows]
            emit_rows(f, "gd_spr_" + ln, pack_rows(rows))
            emit_rows(f, "gd_msk_" + ln, mask_words(rows))
            emit_rows(f, "gd_hspr_" + ln, pack_rows(rows), "uint32_t", 8)
            emit_rows(f, "gd_hmsk_" + ln, mask_words(rows), "uint32_t", 8)
            emit_rows(f, "gd_hspr_" + ln + "_m", pack_rows(mirrored),
                      "uint32_t", 8)
            emit_rows(f, "gd_hmsk_" + ln + "_m", mask_words(mirrored),
                      "uint32_t", 8)

    print("wrote sprites.h and sprites.c: %d tiles, %d sprites"
          % (len(tiles), len(sprites)))


if __name__ == "__main__":
    main()
