#!/usr/bin/env python3
#
# Zeitlos -- the hardware cursor's power-on shapes (docs/cursor.md).
#
# Prints the `initial` lines rtl/gpu/gpu_video.v carries for the four
# cursor slots in its own block RAM (cbuf) -- or, with GPU_CURSOR_LUTRAM,
# in distributed RAM (curl), so both are written: today's 5x5 X and Z (slots 0
# and 1, exactly the pixels the old comparator cursor drew), and a 9x9
# X and Z (slots 2 and 3, until wm loads a set of the user's).
#
#   python3 tools/gen_cursor_init.py            print the lines
#   python3 tools/gen_cursor_init.py --check    fail if gpu_video.v differs
#
# A slot is 16 rows, one 32-bit word each: bit c of bits 15:0 is column
# c (least significant bit leftmost, as the framebuffer), 1 = INVERT
# the pixel under it; bits 31:16 are not used. The cell's centre,
# (8, 8), is drawn at the pointer position.

import os
import re
import sys

C = 8	# the centre

def x_shape(r):
    pts = set()
    for d in range(-r, r + 1):
        pts.add((C + d, C + d))
        pts.add((C + d, C - d))
    return pts

def z_shape(r):
    pts = set()
    for d in range(-r, r + 1):
        pts.add((C + d, C - r))	# top bar
        pts.add((C + d, C + r))	# bottom bar
        pts.add((C - d, C + d))	# the anti-diagonal: top right to bottom left
    return pts

SLOTS = [x_shape(2), z_shape(2), x_shape(4), z_shape(4)]
NAMES = ["5x5 X (normal)", "5x5 Z (busy)", "9x9 X", "9x9 Z"]
BASE = 0

def words(pts):
    out = []
    for row in range(16):
        data = 0
        for (cx, cy) in pts:
            if cy == row:
                data |= 1 << cx
        out.append(data)
    return out

def lines():
    out = []
    for s, pts in enumerate(SLOTS):
        out.append("\t\t// slot %d: %s" % (s, NAMES[s]))
        for row, w in enumerate(words(pts)):
            if w:
                pic = "".join("#" if (w >> c) & 1 else "." for c in range(16))
                a = BASE + 16 * s + row
                out.append("\t\tcbuf[%d] = 32'h%08x; curl[%d] = 16'h%04x;\t// %s" % (a, w, a, w, pic))
    return out

def main():
    text = "\n".join(lines()) + "\n"
    if "--check" in sys.argv[1:]:
        here = os.path.dirname(os.path.abspath(__file__))
        v = open(os.path.join(here, "..", "rtl", "gpu", "gpu_video.v")).read()
        m = re.search(r"// -- cursor shapes, generated --\n(.*?)\t\t// -- end of cursor shapes --", v, re.S)
        if not m or m.group(1) != text:
            print("error: gpu_video.v's cursor shapes are not tools/gen_cursor_init.py's", file=sys.stderr)
            sys.exit(1)
        print("gpu_video.v's cursor shapes are current")
        return
    sys.stdout.write(text)

if __name__ == "__main__":
    main()
