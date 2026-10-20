#!/usr/bin/env python3
"""
Compares two renderings of the same sheets: ours (PWG, decoded by
pwg_decode.py) and another program's (pdftoppm for PDF, Ghostscript for
PostScript), both 300 dpi grey PGM. Ink is anything darker than mid
grey. What is compared is how much ink there is about each pixel (a
box two millimetres across): a stroke half a dot to one side, or a
pattern whose cells are 5 dots here and 4.98 there (and so drift out
of step across a large fill), is not a difference; a missing line, a
missing glyph or a misplaced shape is.

  compare.py OURS_PREFIX THEIRS_PREFIX PAGES [MAX_PERCENT]

Exit 0 when every page is within MAX_PERCENT (default 1) of its ink.
"""
import sys
from PIL import Image, ImageFilter

def page(prefix, p, pages):
    # pdftoppm pads the number to the width of the last one (pdf-01 of 10)
    import os
    for name in ["%s-%0*d.pgm" % (prefix, w, p) for w in (1, 2, 3)]:
        if os.path.exists(name):
            return name
    return "%s-%d.pgm" % (prefix, p)

def ink(path):
    im = Image.open(path).convert("L")
    return im.point(lambda v: 255 if v < 128 else 0)

def main():
    ours, theirs, pages = sys.argv[1], sys.argv[2], int(sys.argv[3])
    limit = float(sys.argv[4]) if len(sys.argv) > 4 else 1.0
    worst = 0.0
    for p in range(1, pages + 1):
        a = ink(page(ours, p, pages))
        b = ink(page(theirs, p, pages))
        if a.size != b.size:
            print("page %d: sizes differ %s %s" % (p, a.size, b.size))
            sys.exit(1)
        ha, hb = a.histogram()[255], b.histogram()[255]
        da, db = a.filter(ImageFilter.BoxBlur(12)), b.filter(ImageFilter.BoxBlur(12))
        # |da - db| over 48 of 255: a fifth of the ink about a pixel differs.
        # Tuned: the tour's pages score 0.4% or less against pdftoppm and
        # Ghostscript; a word erased scores 3.8%, a strip of letters 1.2%
        from PIL import ImageChops
        d = ImageChops.difference(da, db).point(lambda v: 255 if v > 48 else 0)
        apart = d.histogram()[255]
        pct = 100.0 * apart / max(1, (ha + hb) // 2)
        worst = max(worst, pct)
        print("page %d: %d / %d inked, %d apart (%.3f%%)" % (p, ha, hb, apart, pct))
    print("compare: worst %.3f%% (limit %.1f%%)" % (worst, limit))
    sys.exit(0 if worst <= limit else 1)

main()
