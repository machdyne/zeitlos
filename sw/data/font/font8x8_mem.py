#!/usr/bin/env python3
"""
font8x8.mem: Daniel Hepper's public-domain 8x8 font
(https://github.com/dhepper/font8x8, commit 8e279d2), converted to this
directory's .mem format, ASCII and ISO 8859-15 (Latin-9) like the other
192-glyph fonts here, for gen_font_data.py (z_font_8x8).

Upstream stores a row with the leftmost pixel in the lowest bit; .mem
rows are written leftmost first. Glyphs upstream lacks or gets wrong are
drawn below, in its style (capitals with a mark squeezed below it, lower
case on the usual rows 2-6); they are public domain too:

  0x7F  the missing-glyph box (Z_GLYPH_MISSING), as in the other fonts
  0xA4  euro sign         } Latin-9's replacements for Latin-1's
  0xA6  S caron           }   currency sign, broken bar, diaeresis,
  0xA8  s caron           }   acute, cedilla, and one quarter, one
  0xB4  Z caron           }   half and three quarters
  0xB8  z caron           }
  0xBC  OE, 0xBD oe, 0xBE Y diaeresis
  0xAD  soft hyphen: drawn as a hyphen (upstream: empty)
  0xB2  superscript two (upstream: an accent), 0xB3 superscript three
        (upstream: empty)
  0xD1, 0xF1  N and n with a tilde (upstream: a bar, read as a macron)

Usage: font8x8_mem.py path/to/font8x8 > font8x8.mem
"""

import re
import sys
from pathlib import Path

DRAWN = {
    0x7F: ["#######.", "#.....#.", "#.....#.", "#.....#.",
           "#.....#.", "#.....#.", "#######.", "........"],
    0xA4: ["..####..", ".##..##.", "#####...", ".##.....",
           "#####...", ".##..##.", "..####..", "........"],
    0xA6: [".#..#...", "..##....", ".####...", "##......",
           ".####...", "....##..", "##..##..", ".####..."],
    0xA8: [".#..#...", "..##....", ".#####..", "##......",
           ".####...", "....##..", "#####...", "........"],
    0xAD: ["........", "........", "........", "######..",
           "........", "........", "........", "........"],
    0xB2: [".###....", "...##...", "..##....", ".##.....",
           ".####...", "........", "........", "........"],
    0xB3: [".###....", "...##...", "..##....", "...##...",
           ".###....", "........", "........", "........"],
    0xB4: [".#..#...", "..##....", "######..", "....##..",
           "...##...", "..##....", ".##.....", "######.."],
    0xB8: [".#..#...", "..##....", "######..", "#..##...",
           "..##....", ".##..#..", "######..", "........"],
    0xBC: [".#######", "##..##..", "##..##..", "##..####",
           "##..##..", "##..##..", ".#######", "........"],
    0xBD: ["........", "........", ".##.###.", "##.##.##",
           "##.####.", "##.##...", ".##.###.", "........"],
    0xBE: ["##..##..", "........", "##..##..", "##..##..",
           ".####...", "..##....", "..##....", ".####..."],
    0xD1: [".###.##.", "##.###..", "##...##.", "###..##.",
           "####.##.", "##.####.", "##..###.", "##...##."],
    0xF1: [".###.##.", "##.###..", "#####...", "##..##..",
           "##..##..", "##..##..", "##..##..", "........"],
}


def load(directory):
    glyphs = {}
    for name in ("font8x8_basic.h", "font8x8_ext_latin.h"):
        text = (Path(directory) / name).read_text()
        for m in re.finditer(r"\{\s*((?:0x[0-9A-Fa-f]{2},?\s*){8})\}\s*,?\s*"
                             r"//\s*U\+([0-9A-Fa-f]{4})", text):
            rows = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}", m.group(1))]
            glyphs[int(m.group(2), 16)] = [
                "".join("#" if (r >> b) & 1 else "." for b in range(8)) for r in rows]
    return glyphs


def main():
    upstream = load(sys.argv[1])
    out = []
    for code in list(range(0x20, 0x80)) + list(range(0xA0, 0x100)):
        rows = DRAWN.get(code) or upstream[code]
        assert len(rows) == 8 and all(len(r) == 8 for r in rows), hex(code)
        out += [" ".join("1" if c == "#" else "0" for c in r) for r in rows]
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
