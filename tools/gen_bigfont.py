#!/usr/bin/env python3
"""
Compiles a misc-fixed .zfn into C for sw/common/zbigline.h -- the fonts
wider than the glyph blitter's 8 pixels, drawn a line at a time from
memory (docs/fonts.md, "Big text"):

    python3 tools/gen_bigfont.py                  # every font below
    python3 tools/gen_bigfont.py --check          # fail if one is stale

The source is the .zfn already in the tree (sw/data/font/zfn/, made by
tools/gen_fonts.py from X11 misc-fixed: "Public domain font. Share and
enjoy."), so the compiled font is exactly the one on the card.

Compiled in rather than read from /sys/fonts: text is a core app in
flash and runs on a machine with no card, and its big print must too.

Each glyph row is stored as a uint16_t in FRAMEBUFFER order -- bit k is
pixel column k, least significant leftmost (zgfx.h, z_fb_hw_blit_mem())
-- the opposite of the .zfn's MSB-first rows, so drawing is a shift and
an OR with no bit reversal at run time.
"""

import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ZFN = os.path.join(ROOT, "sw", "data", "font", "zfn")
OUT = os.path.join(ROOT, "sw", "common")

# (source .zfn, C name, output file)
FONTS = [
	("m10x20.zfn", "10x20", "zbigfont_10x20.c"),
]


def read_zfn(path):
	d = open(path, "rb").read()
	if d[:4] != b"ZFN1":
		raise SystemExit("%s: not a ZFN1 font" % path)
	w, h, stride, asc = d[4], d[5], d[6], d[7]
	if w > 16:
		raise SystemExit("%s: %d wide; a row is a uint16_t" % (path, w))
	count = struct.unpack_from("<I", d, 8)[0]
	cps = list(struct.unpack_from("<%dH" % count, d, 12))
	off = 12 + 2 * count
	off += (-off) % 4
	glyphs = []
	for i in range(count):
		rows = []
		for r in range(h):
			p = off + (i * h + r) * stride
			v = int.from_bytes(d[p:p + stride], "big")
			fb = 0
			for c in range(w):
				if v & (1 << (stride * 8 - 1 - c)):
					fb |= 1 << c
			rows.append(fb)
		glyphs.append(rows)
	return w, h, cps, glyphs


def emit(src, name, w, h, cps, glyphs):
	out = []
	out.append("/*")
	out.append(" * Made by tools/gen_bigfont.py from sw/data/font/zfn/%s -- do not" % src)
	out.append(" * edit; run the tool. X11 misc-fixed %s: \"Public domain font." % name)
	out.append(" * Share and enjoy.\" Rows in framebuffer order: bit k = column k.")
	out.append(" */")
	out.append("")
	out.append('#include "zbigline.h"')
	out.append("")
	out.append("static const uint16_t cps_%s[%d] = {" % (name, len(cps)))
	for i in range(0, len(cps), 10):
		out.append("\t" + " ".join("0x%04x," % c for c in cps[i:i + 10]))
	out.append("};")
	out.append("")
	out.append("static const uint16_t rows_%s[%d * %d] = {" % (name, len(cps), h))
	for cp, rows in zip(cps, glyphs):
		# a printable label, but never a backslash: a comment ending in
		# one runs on into the next line
		label = chr(cp) if 0x20 < cp < 0x7f and cp != 0x5c else "U+%04X" % cp
		out.append("\t" + " ".join("0x%04x," % r for r in rows) + "  // " + label)
	out.append("};")
	out.append("")
	out.append("const z_bigfont_t z_bigfont_%s = { %d, %d, %d, cps_%s, rows_%s };"
		% (name, w, h, len(cps), name, name))
	out.append("")
	return "\n".join(out)


def main():
	check = "--check" in sys.argv[1:]
	stale = 0
	for src, name, dst in FONTS:
		w, h, cps, glyphs = read_zfn(os.path.join(ZFN, src))
		text = emit(src, name, w, h, cps, glyphs)
		path = os.path.join(OUT, dst)
		old = open(path).read() if os.path.exists(path) else None
		if check:
			if old != text:
				print("gen_bigfont: %s is stale: run tools/gen_bigfont.py" % dst)
				stale += 1
			continue
		if old != text:
			open(path, "w").write(text)
			print("gen_bigfont: wrote %s (%d glyphs, %dx%d)" % (dst, len(cps), w, h))
	return 1 if stale else 0


if __name__ == "__main__":
	sys.exit(main())
