#!/usr/bin/env python3
"""
Converts a BDF bitmap font into a .mem source for gen_font_data.py
(sw/data/font/font<W>x<H>.mem). BDF is a common distribution format
for public-domain bitmap fonts, so this is the easiest path for
importing a new one -- run this, then gen_font_data.py.

Only extracts ASCII 0x20-0x7f (96 codepoints) -- everything else a BDF
might contain (and public-domain BDFs often cover a lot: Cyrillic,
Greek, box-drawing, ...) is out of scope for zfont.h's z_font_t, which
only ever covers that one 96-codepoint range (see its own comment).

Assumes a fixed-width font where every glyph's BBX width matches the
target width exactly (true of "misc-fixed"-style BDFs, which is what
most public-domain monospace BDFs are) -- doesn't handle proportional
fonts or per-glyph width/offset variation.

Usage:
    python3 bdf_to_mem.py font.bdf WIDTH HEIGHT
    python3 bdf_to_mem.py 5x7.bdf 5 7
        -> writes font5x7.mem in this directory
"""

import sys
from pathlib import Path

FIRST_CODEPOINT = 0x20
LAST_CODEPOINT = 0x7f


def parse_bdf(path, w, h):

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
				if encoding is not None and FIRST_CODEPOINT <= encoding <= LAST_CODEPOINT:
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

	missing = [cp for cp in range(FIRST_CODEPOINT, LAST_CODEPOINT + 1) if cp not in glyphs]
	if missing:
		# matches this codebase's existing documented behavior for a
		# source missing a codepoint (zfont.h: "codepoints missing
		# from a given source ... are just blank") -- 0x7f (DEL) is
		# routinely absent from BDFs since it isn't a printable
		# character, not a sign the source file is broken.
		print(f"note: {path} has no glyph for: {[hex(c) for c in missing]} "
			"-- leaving blank", file=sys.stderr)
		blank_rows = [[0] * w for _ in range(h)]
		for cp in missing:
			glyphs[cp] = blank_rows

	return glyphs


def main():
	if len(sys.argv) != 4:
		sys.exit(f"usage: {sys.argv[0]} font.bdf WIDTH HEIGHT")

	bdf_path = Path(sys.argv[1])
	w, h = int(sys.argv[2]), int(sys.argv[3])

	if w > 8:
		sys.exit(f"width {w} > 8 not supported -- see gen_font_data.py's "
			"own note on why")

	glyphs = parse_bdf(bdf_path, w, h)

	out_path = Path(__file__).parent / f"font{w}x{h}.mem"
	with open(out_path, "w") as f:
		for cp in range(FIRST_CODEPOINT, LAST_CODEPOINT + 1):
			for row in glyphs[cp]:
				f.write(" ".join(str(b) for b in row) + "\n")

	print(f"wrote {out_path} ({(LAST_CODEPOINT - FIRST_CODEPOINT + 1) * h} rows)")


if __name__ == "__main__":
	main()
