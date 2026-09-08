#!/usr/bin/env python3
# Zeitlos -- builds the PNG corpus for test_png.c.
#
#   python3 gen_png_corpus.py /tmp/pngc
#
# Written by PIL, i.e. by libpng, so the decoder is checked against an
# encoder it had no hand in. Not checked in: regenerating takes a
# second and a corpus in git is a corpus nobody looks at.
import sys, os
from PIL import Image, ImageDraw

out = sys.argv[1] if len(sys.argv) > 1 else "."
os.makedirs(out, exist_ok=True)
def p(n): return os.path.join(out, n)

# Hard edges and a flat field: filter choices vary per row, so libpng
# exercises all five on one image.
im = Image.new('RGB', (120, 90), (255, 255, 255))
d = ImageDraw.Draw(im)
d.ellipse([20, 15, 100, 75], fill=(40, 40, 50))
d.rectangle([0, 80, 120, 90], fill=(10, 10, 10))
for x in range(0, 120, 10):
    d.line([x, 0, x, 12], fill=(0, 0, 0))

im.save(p('rgb.png'))                                        # ctype 2
im.convert('L').save(p('gray.png'))                          # ctype 0
im.convert('P', palette=Image.ADAPTIVE, colors=64).save(p('pal.png'))
im.convert('RGBA').save(p('rgba.png'))                       # ctype 6
im.convert('L').convert('1').save(p('bilevel.png'))          # depth 1
im.convert('L').save(p('gray_a.png'))
Image.new('RGB', (3000, 50), (0, 0, 0)).save(p('wide.png'))  # too wide

# Reference, for comparing pixel for pixel.
im.convert('L').save(p('rgb.pgm'))

# -- deliberately broken --
#
# PIL will not write these, so they are made by editing bytes. CRCs go
# stale, which does not matter: this decoder does not check them, and
# says so.
raw = open(p('rgb.png'), 'rb').read()

b = bytearray(raw); b[28] = 1                    # IHDR interlace = Adam7
open(p('adam7.png'), 'wb').write(bytes(b))

b = bytearray(raw); b[24] = 16                   # bit depth 16
open(p('depth16.png'), 'wb').write(bytes(b))

b = bytearray(raw); b[100] ^= 0xff               # corrupt IDAT
open(p('corrupt.png'), 'wb').write(bytes(b))

open(p('truncated.png'), 'wb').write(raw[:200])

print("corpus in", out)
