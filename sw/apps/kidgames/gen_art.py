#!/usr/bin/env python3
"""
Turn art/*.png into kgart.c / kgart.h.

    python3 gen_art.py                    # default: Floyd-Steinberg
    python3 gen_art.py --mode ordered     # Bayer 4x4, as kg_shade uses
    python3 gen_art.py --mode threshold   # no dither at all
    python3 gen_art.py --preview          # also write art-preview.png
    python3 gen_art.py --white 240        # fixed paper cutoff
    python3 gen_art.py --white off        # keep every shade

Source images may be ANY size. They are fitted to KG_ART_W x KG_ART_H
(96x48, 2:1), contrast-stretched and reduced to one bit. Anything that
is not 2:1 is letterboxed onto white rather than squashed -- the whole
art direction here is silhouette proportion, and a stretched animal is
a different animal.

LOOK AT art-preview.png BEFORE COMMITTING. It is a contact sheet of
every piece at 4x with its name, and it is the only way to find out
that a dither turned a spider into a smudge. Same argument as
`make render` for the screens: the arithmetic cannot tell you whether
a silhouette still reads.

The generated files are COMMITTED, the same convention
sw/data/icons/gen_dock_icon_data.py and chess's gen_pieces.py follow:
the build needs no Python and no Pillow, and a change to the art is a
reviewable diff rather than a silent difference between two people's
builds.

-- SIZE --

96x48 is 2:1, matching the source art, and the HEIGHT is the pinned
dimension. The piece is drawn at 2x, so 96x48 becomes 192x96 on the
playfield. It sits between the prompt and the answer field, and the
field cannot move because the on-screen keyboard is below it -- that
leaves 107 screen pixels, so 53 source rows is the ceiling and 48 fits
with slack. Width is not tight: 320 pixels of playfield is 160 source
columns, and 96 uses well under that.

-- THE FORMAT --

1bpp, in the framebuffer's own bit order: pixel x lives at bit (x & 7)
of byte (x >> 3), LEAST SIGNIFICANT BIT LEFTMOST. That is what
z_fb_hw_blit_mem() requires (zgfx.h) and it is the opposite of what
almost every image tool writes, so it is done explicitly below rather
than by packing bytes the obvious way.

Row pitch is in BYTES -- KG_ART_STRIDE, 12 for a 96-pixel-wide piece.

Each array carries FOUR BYTES OF PADDING past the last real row.
zgfx.h: the blitter may read up to one word beyond the last source
word it needs whenever source and destination are not word-aligned to
each other, and warns that `src` should not end exactly at the last
valid byte of a mapping. Our destination x is whatever centres the
sprite, so misalignment is the normal case, not the exception.

-- WHY THERE IS NO MASK PLANE --

z_fb_hw_blit_sprite() takes a data plane and a mask plane and does two
passes, ANDN then OR, so a sprite lands over an arbitrary background.
This app does not need that and pays real bytes for it if it emits one.

Every animal is drawn onto a rectangle this app has just cleared to
black, so a plain COPY of the data plane is exactly right. It is also
ONE blit rather than two, which matters more than it sounds: this app
does not page-flip (docs/kidgames_app.md), and a two-pass sprite leaves
its footprint momentarily blank on the visible page -- a one-frame
hole, on the screen a kid is looking at.
"""

import os
import sys

try:
    from PIL import Image, ImageOps, ImageDraw
except ImportError:
    sys.exit("gen_art.py needs Pillow: pip install Pillow")

W, H = 96, 48
STRIDE = W // 8

HERE = os.path.dirname(os.path.abspath(__file__))
ART = os.path.join(HERE, "art")

# Bayer 4x4, the same matrix kg_shade() dithers with on the target.
# Using it here means a piece in `ordered` mode shares its cell
# structure with every shaded surface in the app, so the art and the UI
# look like they belong to each other. Floyd-Steinberg does not have
# that property and usually reads better on an illustration anyway --
# hence it being the default and this being the option.
BAYER = [[0, 8, 2, 10],
         [12, 4, 14, 6],
         [3, 11, 1, 9],
         [15, 7, 13, 5]]


def fit(im):
    """Any size -> W x H on white, aspect preserved, letterboxed."""

    # Composite away any alpha FIRST. A transparent background read as
    # black would make every animal a solid rectangle -- a spectacular
    # failure, and a confusing one, because the shape is still in there
    # underneath.
    if im.mode in ("RGBA", "LA", "P"):
        im = im.convert("RGBA")
        flat = Image.new("RGBA", im.size, (255, 255, 255, 255))
        im = Image.alpha_composite(flat, im)

    im = im.convert("L")

    # LANCZOS, not the default. Reducing 1408x704 to 96x48 is a 14x
    # reduction; a box filter at that ratio throws away most of the
    # information before the dither ever sees it.
    if abs(im.size[0] / im.size[1] - (W / H)) < 0.01:
        return im.resize((W, H), Image.LANCZOS)

    scale = min(W / im.size[0], H / im.size[1])
    nw = max(1, int(im.size[0] * scale))
    nh = max(1, int(im.size[1] * scale))
    out = Image.new("L", (W, H), 255)
    out.paste(im.resize((nw, nh), Image.LANCZOS), ((W - nw) // 2, (H - nh) // 2))

    return out


# How far below the paper peak still counts as paper. Wide enough to
# swallow a gradient or a JPEG ring, narrow enough to leave real light
# shading alone.
PAPER_TOL = 24


def paper_level(im):
    """The cutoff above which everything is background.

    The brightest peak in the histogram, minus a tolerance. Only the
    light half is considered: a mostly-dark picture's peak is its
    SUBJECT, and snapping that to white would erase the animal."""

    h = im.histogram()
    best, best_n = 255, 0

    for v in range(128, 256):
        if h[v] > best_n:
            best, best_n = v, h[v]

    # A peak that is not actually a flat area -- fewer than a twentieth
    # of the pixels -- is not a background, so leave the image alone.
    if best_n < (im.size[0] * im.size[1]) // 20:
        return 255

    return max(128, best - PAPER_TOL)


def to_bilevel(im, mode, thresh):
    """L -> 1bpp PIL image, 0 = ink."""

    if mode == "fs":
        return im.convert("1")          # PIL's own Floyd-Steinberg

    if mode == "ordered":
        out = Image.new("1", im.size)
        px, op = im.load(), out.load()
        for y in range(im.size[1]):
            for x in range(im.size[0]):
                # +8 centres the 0..15 matrix on the 0..255 range so the
                # midpoint stays the midpoint. Without it every piece
                # comes out systematically darker.
                t = BAYER[y & 3][x & 3] * 16 + 8
                op[x, y] = 255 if px[x, y] > t else 0
        return out

    return im.point(lambda p: 255 if p >= thresh else 0, mode="1")


def pack(im):
    """1bpp PIL image -> bytes, LSB-leftmost, one row at a time."""

    px = im.load()
    out = bytearray()

    for y in range(H):
        row = bytearray(STRIDE)
        for x in range(W):
            # Ink is DARK in the source and 1 in the framebuffer, so the
            # test is inverted relative to the pixel value. Backwards
            # gives a perfect negative, which reads as "the art is
            # wrong" rather than "the threshold is".
            if not px[x, y]:
                row[x >> 3] |= 1 << (x & 7)
        out += row

    out += bytes(4)      # see the module comment

    return out


def emit(f, name, data):
    f.write("const uint8_t kg_art_%s[KG_ART_BYTES] = {\n" % name)
    for i in range(0, len(data), 12):
        f.write("\t" + " ".join("0x%02x," % b for b in data[i:i + 12]) + "\n")
    f.write("};\n\n")


def write_preview(names, previews):
    """A contact sheet at 4x, three across, each labelled.

    Ink is drawn BLACK on white here, the inverse of the framebuffer's
    convention -- this image is for a screen with a white background,
    not for the one the app runs on."""

    cols = 3
    rows = (len(names) + cols - 1) // cols
    cw, ch = W * 4 + 8, H * 4 + 20
    sheet = Image.new("L", (cols * cw, rows * ch), 255)
    d = ImageDraw.Draw(sheet)

    for n, name in enumerate(names):
        x, y = (n % cols) * cw + 4, (n // cols) * ch + 4
        sheet.paste(previews[name].resize((W * 4, H * 4), Image.NEAREST), (x, y))
        d.rectangle([x - 1, y - 1, x + W * 4, y + H * 4], outline=128)
        d.text((x, y + H * 4 + 3), name, fill=0)

    sheet.save(os.path.join(HERE, "art-preview.png"))


def main():
    mode, thresh = "fs", 128
    preview, autocontrast = False, True
    white = "auto"

    args = sys.argv[1:]
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--mode":
            i += 1
            mode = args[i]
            if mode not in ("fs", "ordered", "threshold"):
                sys.exit("--mode must be fs, ordered or threshold")
        elif a == "--threshold":
            i += 1
            thresh = int(args[i])
        elif a == "--preview":
            preview = True
        elif a == "--white":
            i += 1
            white = args[i] if args[i] in ("auto", "off") else int(args[i])
        elif a == "--no-autocontrast":
            autocontrast = False
        else:
            sys.exit("unknown option %s -- see the comment at the top" % a)
        i += 1

    if not os.path.isdir(ART):
        sys.exit("no art/ directory next to gen_art.py.\n"
                 "Put one PNG per animal in %s, named for the animal in\n"
                 "lowercase (cat.png, crocodile.png), then run this again."
                 % ART)

    names = sorted(n[:-4] for n in os.listdir(ART) if n.endswith(".png"))
    if not names:
        sys.exit("art/ has no .png files")

    pieces, previews = {}, {}

    for name in names:

        if not name.isalpha() or not name.islower():
            sys.exit("%s.png: the name becomes a C identifier AND the word "
                     "a kid types,\nso it must be lowercase a-z only" % name)

        im = fit(Image.open(os.path.join(ART, name + ".png")))

        # Stretch whatever range the source used to full black-to-white
        # BEFORE dithering. Illustrations often sit in a narrow band --
        # dark grey on off-white -- and a dither applied to that gives a
        # flat texture with no silhouette in it at all.
        if autocontrast:
            im = ImageOps.autocontrast(im)

        # Snap the PAPER to pure white before dithering.
        #
        # Error diffusion has nowhere to put its error on a large flat
        # area that is nearly but not quite white, so it sprays isolated
        # dots across the whole background. On the preview sheet that
        # reads as dirt; on the black playfield it is worse, because
        # every one of those dots is lit. Illustrations almost never
        # have a mathematically pure white background -- a JPEG round
        # trip or a soft shadow is enough -- so this is the common case.
        #
        # Measured, not guessed. A fixed cutoff cannot work: 250 did
        # nothing at all on the first set of test images (autocontrast
        # had left the paper below it) while 235 cut isolated pixels by
        # six times, and which of those is right depends entirely on
        # the source. So `auto` finds the histogram's brightest peak --
        # for a picture of an animal on a background, that peak IS the
        # background -- and treats everything within PAPER_TOL of it as
        # paper.
        #
        # --white N forces a fixed cutoff, --white off keeps every
        # shade. Neither does anything in threshold mode, which has no
        # error to diffuse.
        cut = paper_level(im) if white == "auto" else white
        if cut != "off" and cut < 255:
            im = im.point(lambda p: 255 if p >= cut else p)

        bw = to_bilevel(im, mode, thresh)
        pieces[name] = pack(bw)
        previews[name] = bw

        # An ink percentage per piece, because the two ways this goes
        # wrong silently are both visible in one number: a light-on-dark
        # source comes out mostly ink, and a failed threshold comes out
        # nearly blank. Neither raises an exception and both look like
        # bad art rather than a bad conversion.
        ink = sum(bin(b).count("1") for b in pieces[name])
        pct = (100 * ink) // (W * H)
        note = ""
        if pct > 60:
            note = "   <-- mostly ink; is this one light-on-dark?"
        elif pct < 4:
            note = "   <-- almost blank; did the threshold miss?"
        print("  %-12s %3d%% ink%s" % (name, pct, note))

    with open(os.path.join(HERE, "kgart.c"), "w") as f:
        f.write("/*\n * GENERATED by gen_art.py -- do not edit.\n"
                " * Edit art/*.png and re-run it.\n */\n\n"
                '#include "kgart.h"\n\n')
        for name in names:
            emit(f, name, pieces[name])

    with open(os.path.join(HERE, "kgart.h"), "w") as f:
        f.write("""/*
 * GENERATED by gen_art.py -- do not edit.
 * Edit art/*.png and re-run it.
 *
 * 1bpp pieces, %dx%d, in the framebuffer's bit order (pixel x at bit
 * (x & 7) of byte (x >> 3), least significant bit leftmost) so they
 * can go straight to z_fb_hw_blit_mem() with no repacking.
 *
 * KG_ART_BYTES includes four bytes of padding past the last row --
 * the blitter may read one word beyond what it needs when source and
 * destination are not word-aligned. See gen_art.py.
 */
#ifndef KGART_H
#define KGART_H

#include <stdint.h>

#define KG_ART_W      %d
#define KG_ART_H      %d
#define KG_ART_STRIDE %d
#define KG_ART_BYTES  (KG_ART_STRIDE * KG_ART_H + 4)

""" % (W, H, W, H, STRIDE))
        for name in names:
            f.write("extern const uint8_t kg_art_%s[KG_ART_BYTES];\n" % name)
        f.write("\n#endif\n")

    if preview:
        write_preview(names, previews)
        print("gen_art.py: wrote art-preview.png -- LOOK AT IT")

    print("gen_art.py: wrote kgart.c and kgart.h (%d pieces, %s, %dx%d, "
          "%d bytes of art)"
          % (len(names), mode, W, H, sum(len(p) for p in pieces.values())))


main()
