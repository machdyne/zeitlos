#!/usr/bin/env python3
# pix -- pictures for test_import (PIL): written to the directory given.
import sys, os
from PIL import Image

d = sys.argv[1]
os.makedirs(d, exist_ok=True)

# a palette PNG: five colours, index 4 see-through (tRNS)
pal = [0x00,0x00,0x00, 0xff,0xff,0xff, 0xcc,0x44,0x22, 0x22,0x88,0xee, 0x11,0x22,0x33]
im = Image.new('P', (20, 10), 0)
im.putpalette(pal + [0] * (768 - len(pal)))
for y in range(10):
    for x in range(20):
        im.putpixel((x, y), (x // 4) % 5)
im.save(os.path.join(d, 'pal.png'), transparency=4)

# RGBA: left half opaque red, right half see-through
im = Image.new('RGBA', (16, 8), (0, 0, 0, 0))
for y in range(8):
    for x in range(8):
        im.putpixel((x, y), (0xff, 0x00, 0x00, 0xff))
im.save(os.path.join(d, 'rgba.png'))

# a gradient of many colours: median cut
im = Image.new('RGB', (64, 64))
for y in range(64):
    for x in range(64):
        im.putpixel((x, y), (x * 4, y * 4, (x * y) & 255))
im.save(os.path.join(d, 'many.png'))

# GIF and BMP of the palette picture
Image.open(os.path.join(d, 'pal.png')).convert('RGB').save(os.path.join(d, 'pal.gif'))
Image.open(os.path.join(d, 'pal.png')).convert('RGB').save(os.path.join(d, 'pal.bmp'))

# bigger than 640 x 480: halved
im = Image.new('RGB', (800, 600), (0x40, 0x80, 0xc0))
im.save(os.path.join(d, 'big.png'))
