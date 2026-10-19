#!/usr/bin/env python3
"""Convert the host renders (out/*.ppm) to PNG at 2x, the way the
display shows them (each pixel doubled), and put them on one contact
sheet, out/sheet.png. Needs PIL; without it the PPM files stand."""

import sys, os, glob

try:
    from PIL import Image, ImageDraw
except ImportError:
    print("ppm2png: no PIL, leaving the PPM files")
    sys.exit(0)

d = sys.argv[1] if len(sys.argv) > 1 else "out"
files = sorted(glob.glob(os.path.join(d, "*.ppm")))
thumbs = []
for f in files:
    im = Image.open(f).convert("RGB")
    big = im.resize((im.width * 2, im.height * 2), Image.NEAREST)
    big.save(f[:-4] + ".png")
    thumbs.append((os.path.basename(f)[:-4], im))

if thumbs:
    cols = 3
    rows = (len(thumbs) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * 330, rows * 262), (40, 40, 40))
    dr = ImageDraw.Draw(sheet)
    for i, (name, im) in enumerate(thumbs):
        x, y = (i % cols) * 330 + 5, (i // cols) * 262 + 16
        sheet.paste(im, (x, y))
        dr.text((x, y - 13), name, fill=(220, 220, 220))
    sheet.save(os.path.join(d, "sheet.png"))
    print("ppm2png: %d pictures, sheet.png" % len(thumbs))
