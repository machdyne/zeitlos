#!/usr/bin/env python3
"""PPM -> PNG, pixel for pixel (the mockup already drew at its display size)."""
import sys, glob, os
from PIL import Image
for f in sorted(glob.glob(os.path.join(sys.argv[1], "*.ppm")) + glob.glob(os.path.join(sys.argv[1], "*.pgm"))):
    Image.open(f).convert("RGB").save(f[:-4] + ".png", optimize=True)
