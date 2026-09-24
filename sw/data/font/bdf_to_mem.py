#!/usr/bin/env python3
"""
Converts a BDF bitmap font into a .mem source for gen_font_data.py
(sw/data/font/font<W>x<H>.mem). BDF is a common distribution format
for public-domain bitmap fonts, so this is the easiest path for
importing a new one -- run this, then gen_font_data.py.

Extracts 192 glyphs: ASCII 0x20-0x7f, then the upper half of
ISO 8859-15 (Latin-9), bytes 0xa0-0xff. The .mem file is ordered by
BYTE value -- glyph 96 is byte 0xa0 -- and each Latin-9 byte is looked
up in the BDF by its Unicode codepoint (byte 0xa4 is the euro sign,
U+20AC). See docs/text_encoding.md for why Latin-9 rather than
Latin-1, and zfont.h for how the gap at 0x80-0x9f is skipped.

0x7f (DEL) is not a printable character and BDFs routinely have no
glyph for it; when it is missing it becomes the "missing glyph" box
that zgfx draws for any character the font cannot show.

With --ascii, only the first 96 glyphs are written, as before.

Assumes a fixed-width font where every glyph's BBX width matches the
target width exactly (true of "misc-fixed"-style BDFs, which is what
most public-domain monospace BDFs are) -- doesn't handle proportional
fonts or per-glyph width/offset variation.

Usage:
    python3 bdf_to_mem.py [--ascii] font.bdf WIDTH HEIGHT
    python3 bdf_to_mem.py 6x12.bdf 6 12
        -> writes font5x7.mem in this directory
"""

import sys
from pathlib import Path

FIRST_CODEPOINT = 0x20
LAST_CODEPOINT = 0x7f
DEL = 0x7f


def glyph_codepoints(ascii_only):
	"""(byte, unicode codepoint) for every glyph slot, in .mem order."""
	slots = [(b, b) for b in range(FIRST_CODEPOINT, LAST_CODEPOINT + 1)]
	if not ascii_only:
		for b in range(0xa0, 0x100):
			slots.append((b, ord(bytes([b]).decode("iso8859_15"))))
	return slots


def missing_box(w, h, cap_glyph):
	"""A hollow box as tall as a capital letter and one column narrower
	than the cell (so neighbouring boxes do not touch): the glyph for a
	character the font cannot show. cap_glyph is the font's own 'H',
	which gives the cap height and baseline this font actually uses."""
	ink = [y for y, row in enumerate(cap_glyph) if any(row)]
	top, bottom = (ink[0], ink[-1]) if ink else (0, h - 1)
	rows = [[0] * w for _ in range(h)]
	for y in range(top, bottom + 1):
		for x in range(0, w - 1):
			if y in (top, bottom) or x in (0, w - 2):
				rows[y][x] = 1
	return rows


def parse_bdf(path, w, h, wanted):

	glyphs = {}  # codepoint -> list of h rows, each a list of w 0/1 ints
	encoding = None
	bbx = None
	bitmap_rows = None
	in_bitmap = False

	with open(path, encoding="utf-8", errors="replace") as f:
		for line in f:
			line = line.rstrip("\n")

			if line.startswith("STARTCHAR"):
				encoding = None
				bbx = None
				bitmap_rows = []
				in_bitmap = False
				continue

			if line.startswith("ENCODING"):
				encoding = int(line.split()[1])
				continue

			if line.startswith("BBX"):
				parts = line.split()
				bbx = (int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4]))
				continue

			if line.startswith("BITMAP"):
				in_bitmap = True
				continue

			if line.startswith("ENDCHAR"):
				in_bitmap = False
				if encoding is not None and encoding in wanted:
					if bbx is None:
						sys.exit(f"{path}: char {encoding} has no BBX")
					gw, gh = bbx[0], bbx[1]
					if gw != w or gh != h:
						sys.exit(f"{path}: char {encoding} is {gw}x{gh}, "
							f"expected {w}x{h} -- this font may not be "
							"fixed-width, see this script's own docstring")
					rows = []
					for hex_row in bitmap_rows:
						val = int(hex_row, 16)
						nbits = len(hex_row) * 4
						bits = [(val >> (nbits - 1 - i)) & 1 for i in range(w)]
						rows.append(bits)
					if len(rows) != h:
						sys.exit(f"{path}: char {encoding} has {len(rows)} "
							f"BITMAP rows, expected {h}")
					glyphs[encoding] = rows
				continue

			if in_bitmap:
				bitmap_rows.append(line.strip())
				continue

	missing = [cp for cp in sorted(wanted) if cp not in glyphs]
	if missing:
		# 0x7f (DEL) is routinely absent from BDFs since it isn't a
		# printable character; it becomes the missing-glyph box. Any
		# OTHER gap is left blank (zfont.h: "codepoints missing from a
		# given source ... are just blank") but reported, since for a
		# letter it is usually a sign of the wrong source file.
		others = [c for c in missing if c != DEL]
		if others:
			print(f"note: {path} has no glyph for: {[hex(c) for c in others]} "
				"-- leaving blank", file=sys.stderr)
		blank_rows = [[0] * w for _ in range(h)]
		for cp in missing:
			glyphs[cp] = missing_box(w, h, glyphs.get(0x48, blank_rows)) if cp == DEL else blank_rows

	return glyphs


def main():
	args = sys.argv[1:]
	ascii_only = "--ascii" in args
	args = [a for a in args if a != "--ascii"]
	if len(args) != 3:
		sys.exit(f"usage: {sys.argv[0]} [--ascii] font.bdf WIDTH HEIGHT")

	bdf_path = Path(args[0])
	w, h = int(args[1]), int(args[2])

	if w > 8:
		sys.exit(f"width {w} > 8 not supported -- see gen_font_data.py's "
			"own note on why")

	slots = glyph_codepoints(ascii_only)
	glyphs = parse_bdf(bdf_path, w, h, set(cp for _, cp in slots))

	out_path = Path(__file__).parent / f"font{w}x{h}.mem"
	with open(out_path, "w") as f:
		for _, cp in slots:
			for row in glyphs[cp]:
				f.write(" ".join(str(b) for b in row) + "\n")

	print(f"wrote {out_path} ({len(slots)} glyphs, {len(slots) * h} rows)")


if __name__ == "__main__":
	main()
