#!/usr/bin/env python3
#
# Zeitlos mausrun -- sprite and tile sheet generator.
#
# Emits sprites.c / sprites.h from the two sprite sheets in
# sw/data/mausrun (below, "THE ART"). Changing the art means editing a
# sheet -- in pix, or as text -- and re-running, not touching any C.
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
#   uint16_t rows  -- the software fallback in mausrun.c, which
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

# THE ART IS IN TWO SPRITE SHEETS, sw/data/mausrun/tiles.zsp and
# sprites.zsp (docs/zsp.md), which pix edits (docs/pix_app.md) and mausrun
# also reads from the card at start (docs/mausrun.md, "Art"). This
# script reads them in their order -- a tile's id is its place in
# tiles.zsp, and the level maps store ids -- and turns each pixel back
# into the characters the code below was written for:
#
#   tiles    0 background '.', anything else set '#'
#   sprites  . see-through, 2 outline 'o' (mask only), anything else '#'
#
# `--legacy` writes sprites.c exactly as it was before the sheets: the
# check that moving the art changed nothing (make -C tests sheets).

import os

HERE = os.path.dirname(os.path.abspath(__file__))
SHEETS = os.path.join(HERE, "..", "..", "data", "mausrun")


def read_zsp(path, sprite):
    """[(NAME, rows)] in file order, rows as '#', 'o' and '.'."""
    out, cur, rows, wide = [], None, [], 1
    with open(path) as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\r\n")
            if cur is not None:
                r = line.lstrip("\t")
                cells = [r[i:i + wide] for i in range(0, len(r), wide)]
                if len(cells) != cur[1]:
                    sys.exit("%s line %d: %d pixels, want %d" % (path, n, len(cells), cur[1]))
                if sprite:
                    rows.append("".join("." if c.strip(".") == "" else
                                        "o" if int(c, 16) == 2 else "#" for c in cells))
                else:
                    rows.append("".join("." if c.strip(".") == "" or int(c, 16) == 0
                                        else "#" for c in cells))
                if len(rows) == cur[2]:
                    out.append((cur[0], rows))
                    cur, rows = None, []
                continue
            w = line.split()
            if not w:
                continue
            if w[0] == "colours":
                wide = 2 if int(w[1]) > 16 else 1
            elif w[0] == "sprite":
                cur = (w[1].upper(), int(w[2]), int(w[3]))
    if cur is not None:
        sys.exit("%s: %s is cut short" % (path, cur[0]))
    return out


TILES = dict(read_zsp(os.path.join(SHEETS, "tiles.zsp"), False))
SPRITES = dict(read_zsp(os.path.join(SHEETS, "sprites.zsp"), True))


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
    is why the cat has an outline -- the outline IS the mask edge.

    'o' (round 3) is mask without data: an outline pixel, drawn in the
    sprite's outline colour in colour mode, black in monochrome."""
    words = []
    for r in rows:
        v = 0
        for x, c in enumerate(r):
            if c == '#' or c == 'o':     # 'o': outline, in the mask only
                v |= (1 << x)
        words.append(v)
    return words


# "const " in --legacy; nothing otherwise, so mausrun can lay the
# card's sheets over the art at start (mausrun.c, art_load()).
Q = "const "


def emit_rows(f, name, words, ctype="uint16_t", width=4):
    f.write("%s%s %s[%d] = {\n" % (Q, ctype, name, len(words)))
    per = 8 if width == 4 else 6
    for i in range(0, len(words), per):
        f.write("\t" + " ".join(("0x%0*x," % (width, w)) for w in words[i:i + per]) + "\n")
    f.write("};\n\n")


def main():
    global Q
    legacy = "--legacy" in sys.argv
    out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else HERE
    if not legacy:
        Q = ""
    tiles = [(n, parse(r, TILE_W, TILE_H, n)) for n, r in TILES.items()]
    sprites = [(n, parse(r, SPR_W, SPR_H, n)) for n, r in SPRITES.items()]

    with open(os.path.join(out, "sprites.h"), "w") as f:
        f.write("/* GENERATED by gen_sprites.py -- do not edit.\n"
                + (" * Edit the art in that script and re-run it instead.\n" if legacy else
                   " * Edit the sheets in sw/data/mausrun and re-run it instead.\n") +
                " * See its header for the two formats and why they differ. */\n\n")
        f.write("#ifndef MAUSRUN_SPRITES_H\n#define MAUSRUN_SPRITES_H\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write("#define MR_TILE_W %d\n#define MR_TILE_H %d\n" % (TILE_W, TILE_H))
        f.write("#define MR_SPR_W %d\n#define MR_SPR_H %d\n\n" % (SPR_W, SPR_H))

        f.write("/* Tile ids -- index into mr_tiles[]. Order matters: the\n"
                " * level map stores these values directly, and MR_TILE_SOLID\n"
                " * below assumes every solid tile sorts at or after BOX. */\n")
        f.write("enum {\n")
        for i, (n, _) in enumerate(tiles):
            f.write("\tMR_TILE_%s = %d,\n" % (n, i))
        f.write("\tMR_TILE_COUNT = %d\n};\n\n" % len(tiles))

        f.write("/* Each tile is MR_TILE_H rows of MR_TILE_W bits, LSB =\n"
                " * leftmost pixel -- the framebuffer's own bit order (zgfx.h),\n"
                " * NOT z_font_t's MSB-first one. */\n")
        for n, _ in tiles:
            f.write("extern %suint32_t mr_tile_%s[MR_TILE_H];\n" % (Q, n.lower()))
        f.write("\nextern const uint32_t *const mr_tiles[MR_TILE_COUNT];\n\n")
        f.write("/* Source stride handed to z_fb_hw_blit_mem(). MUST be 4 --\n"
                " * see gen_sprites.py's header for what a 2-byte stride does\n"
                " * to the blitter's row walk. */\n")
        f.write("#define MR_TILE_STRIDE 4\n\n")

        f.write("/* Sprites carry a DATA plane and a MASK plane, because\n"
                " * gpu_blit.v's copy mode has no raster op and would draw an\n"
                " * opaque box. Composited in software as\n"
                " * (dst & ~mask) | data -- see mausrun.c's spr_draw(). */\n")
        for n, _ in sprites:
            ln = n.lower()
            f.write("extern " + Q + "uint16_t mr_spr_%s[MR_SPR_H];\n" % ln)
            f.write("extern " + Q + "uint16_t mr_msk_%s[MR_SPR_H];\n" % ln)
            f.write("extern " + Q + "uint32_t mr_hspr_%s[MR_SPR_H];\n" % ln)
            f.write("extern " + Q + "uint32_t mr_hmsk_%s[MR_SPR_H];\n" % ln)
            f.write("extern " + Q + "uint32_t mr_hspr_%s_m[MR_SPR_H];\n" % ln)
            f.write("extern " + Q + "uint32_t mr_hmsk_%s_m[MR_SPR_H];\n" % ln)
        if not legacy:
            f.write("\n/* Every tile and sprite by its name in the sheets (lower\n"
                    " * case), for laying the card's sheets over the art at start\n"
                    " * (mausrun.c, art_load()). A tile has `tile`; a sprite the\n"
                    " * other six. */\n")
            f.write("typedef struct {\n\tconst char *name;\n\tuint32_t *tile;\n"
                    "\tuint16_t *spr, *msk;\n"
                    "\tuint32_t *hspr, *hmsk, *hspr_m, *hmsk_m;\n} mr_art_t;\n\n")
            f.write("#define MR_ART_COUNT %d\n" % (len(tiles) + len(sprites)))
            f.write("extern const mr_art_t mr_art[MR_ART_COUNT];\n")
        f.write("\n#endif\n")

    with open(os.path.join(out, "sprites.c"), "w") as f:
        f.write("/* GENERATED by gen_sprites.py -- do not edit. */\n\n")
        f.write('#include "sprites.h"\n\n')
        for n, rows in tiles:
            emit_rows(f, "mr_tile_" + n.lower(), pack_rows(rows),
                      "uint32_t", 8)
        f.write("const uint32_t *const mr_tiles[MR_TILE_COUNT] = {\n")
        for n, _ in tiles:
            f.write("\tmr_tile_%s,\n" % n.lower())
        f.write("};\n\n")
        for n, rows in sprites:
            ln = n.lower()
            mirrored = [r[::-1] for r in rows]
            emit_rows(f, "mr_spr_" + ln, pack_rows(rows))
            emit_rows(f, "mr_msk_" + ln, mask_words(rows))
            emit_rows(f, "mr_hspr_" + ln, pack_rows(rows), "uint32_t", 8)
            emit_rows(f, "mr_hmsk_" + ln, mask_words(rows), "uint32_t", 8)
            emit_rows(f, "mr_hspr_" + ln + "_m", pack_rows(mirrored),
                      "uint32_t", 8)
            emit_rows(f, "mr_hmsk_" + ln + "_m", mask_words(mirrored),
                      "uint32_t", 8)

        if not legacy:
            f.write("const mr_art_t mr_art[MR_ART_COUNT] = {\n")
            for n, _ in tiles:
                f.write('\t{ "%s", mr_tile_%s, 0, 0, 0, 0, 0, 0 },\n' % (n.lower(), n.lower()))
            for n, _ in sprites:
                ln = n.lower()
                f.write('\t{ "%s", 0, mr_spr_%s, mr_msk_%s, mr_hspr_%s, mr_hmsk_%s,\n'
                        '\t\tmr_hspr_%s_m, mr_hmsk_%s_m },\n' % ((ln,) * 7))
            f.write("};\n")

    print("wrote sprites.h and sprites.c: %d tiles, %d sprites"
          % (len(tiles), len(sprites)))


if __name__ == "__main__":
    main()
