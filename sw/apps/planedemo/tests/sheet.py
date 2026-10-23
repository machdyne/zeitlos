#!/usr/bin/env python3
# planedemo -- a contact sheet: one frame every two seconds (every 60th
# picture at 30 a second), six across. sheet.py FRAMES_DIR OUT.png
import os, sys
from PIL import Image
d, out = sys.argv[1], sys.argv[2]
names = sorted(n for n in os.listdir(d) if n.endswith('.ppm'))[::60]
w, h, cols = 320, 240, 6
rows = (len(names) + cols - 1) // cols
sheet = Image.new('RGB', (cols * w, rows * h))
for i, n in enumerate(names):
    sheet.paste(Image.open(os.path.join(d, n)), ((i % cols) * w, (i // cols) * h))
sheet.save(out)
print("%s: %d pictures" % (out, len(names)))
