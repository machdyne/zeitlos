#!/usr/bin/env python3
"""
Converts a JIS X 0208 BDF font -- Shinonome's shnmk12 -- into the wide
font file Zeitlos draws Japanese with: sw/data/font/jp12.zfn. See
docs/text_encoding.md, "Japanese".

    python3 tools/gen_jfont.py shnmk12.bdf sw/data/font/jp12.zfn

The BDF comes from the xfonts-shinonome package (shnmk12.pcf.gz, then
pcf2bdf). Shinonome is public domain: its authors declare they will not
exercise their rights, which is how Japanese law allows it (see the
package's copyright file). 12x12 is chosen because it is exactly two
cells of z_font_6x12, so a wide character sits in the two columns a
terminal and wcwidth() give it.

-- the file --

All little-endian.

    offset  size       field
    0       4          "ZFN1"
    4       1          glyph width in pixels (12)
    5       1          glyph height in pixels (12)
    6       1          bytes per glyph row (2: MSB-first, top `w` bits)
    7       1          0
    8       4          count
    12      2*count    Unicode codepoints, ascending (all in the BMP)
    ...     0 or 2     padding to a multiple of 4
    ...     count*h*2  glyphs, in the same order, `h` rows each

A reader finds a character by binary search on the codepoint table and
takes glyph i at data + i*h*stride. Nothing is compressed: the service
that holds it (sw/apps/jfont) hands every app its address, and a
lookup has to be cheap enough to do per character while drawing.
"""

import struct
import sys


def main():
	if len(sys.argv) != 3:
		sys.exit(__doc__)
	src, dst = sys.argv[1], sys.argv[2]

	glyphs = {}
	w = h = None
	enc = None
	rows = None
	bbx = None
	for line in open(src, encoding="latin-1"):
		line = line.rstrip("\n")
		if line.startswith("FONTBOUNDINGBOX"):
			_, fw, fh, _, _ = line.split()
			w, h = int(fw), int(fh)
		elif line.startswith("ENCODING"):
			enc = int(line.split()[1])
		elif line.startswith("BBX"):
			bbx = [int(x) for x in line.split()[1:]]
		elif line == "BITMAP":
			rows = []
		elif line == "ENDCHAR":
			if enc is not None and rows is not None:
				# JIS X 0208 row/cell -> EUC-JP -> Unicode.
				hi, lo = enc >> 8, enc & 0xFF
				try:
					ch = bytes([hi | 0x80, lo | 0x80]).decode("euc_jp")
				except UnicodeDecodeError:
					ch = None
				if ch and len(ch) == 1 and ord(ch) <= 0xFFFF:
					gw, gh, gx, gy = bbx
					if gw != w or gh != h:
						sys.exit(f"glyph {enc:#x} is {gw}x{gh}, not {w}x{h}")
					bits = [int(r, 16) for r in rows]
					glyphs[ord(ch)] = bits
			enc = None
			rows = None
		elif rows is not None:
			rows.append(line.strip())

	if w != 12 or h != 12:
		sys.exit(f"expected a 12x12 font, got {w}x{h}")

	codes = sorted(glyphs)
	out = bytearray(b"ZFN1")
	out += struct.pack("<BBBBI", w, h, 2, 0, len(codes))
	for c in codes:
		out += struct.pack("<H", c)
	while len(out) % 4:
		out += b"\0"
	for c in codes:
		for r in glyphs[c]:
			# BDF rows for a 12-wide glyph are two hex bytes, MSB-first,
			# the glyph in the top 12 bits -- already the stored form.
			out += struct.pack(">H", r & 0xFFF0)

	open(dst, "wb").write(out)
	print(f"wrote {dst}: {len(codes)} glyphs, {len(out)} bytes")


if __name__ == "__main__":
	main()
