#!/usr/bin/env python3
# A test picture for view's screen face: a photo-like gradient with
# shapes in many colours, 400x300 (fitted into 320x240), as PNG.
import sys, math
from PIL import Image, ImageDraw
out = sys.argv[1]
w, h = 400, 300
im = Image.new("RGB", (w, h))
px = im.load()
for y in range(h):
    for x in range(w):
        r = int(128 + 127 * math.sin(x / 37.0))
        g = int(128 + 127 * math.sin(y / 23.0 + 1))
        b = int(128 + 127 * math.sin((x + y) / 51.0 + 2))
        px[x, y] = (r, g, b)
d = ImageDraw.Draw(im)
d.ellipse((40, 40, 160, 160), fill=(240, 40, 40))
d.rectangle((220, 60, 360, 140), fill=(30, 200, 60))
d.polygon([(200, 280), (300, 170), (390, 280)], fill=(250, 220, 30))
d.text((20, 260), "view: colour", fill=(255, 255, 255))
im.save(out)
