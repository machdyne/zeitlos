#!/usr/bin/env python3
"""
The glyphs the hardware fonts lack, for the languages Zeitlos speaks
beyond Latin-9: sw/data/font/script5x8.zfn and script6x12.zfn, which
jfont holds for every app (zlang.h) and zgfx.c draws in software. See
docs/i18n.md, "Scripts".

    python3 tools/gen_script_fonts.py

From the same public-domain misc-fixed BDFs the hardware fonts come
from (sw/data/font/5x8.bdf, 6x12.bdf; Markus Kuhn's ucs-fonts), so the
new letters match the old ones. The format is ZFN1 (tools/gen_jfont.py)
with one byte per row: the glyph in the top `w` bits, MSB-first.

Which characters: Latin Extended-A (Polish, Czech, Slovak, Hungarian,
Romanian, Croatian, Slovenian, Turkish, the Baltic languages), the
comma-below letters Romanian uses, the spacing accents, Greek, Cyrillic,
and the punctuation those languages' text uses that Latin-9 lacks
(typographic quotes and dashes, the ellipsis, the numero sign). Whatever
a BDF has no glyph for is left out, and draws as the box.
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = os.path.join(HERE, "..", "sw", "data", "font")
sys.path.insert(0, FONT)
import bdf_to_mem  # noqa: E402

RANGES = [
    (0x0100, 0x017F),   # Latin Extended-A
    (0x0218, 0x021B),   # S, T with comma below (Romanian)
    (0x02C6, 0x02DD),   # spacing accents: circumflex, caron, breve, ogonek ...
    (0x0370, 0x03FF),   # Greek
    (0x0400, 0x04FF),   # Cyrillic
    (0x2010, 0x2027),   # dashes, typographic quotes, bullet, ellipsis
    (0x2030, 0x2030),   # per mille
    (0x2039, 0x203A),   # single guillemets
    (0x2116, 0x2116),   # numero sign
    (0x2122, 0x2122),   # trade mark
]


def bdf_cps(path):
    cps = set()
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("ENCODING"):
            cps.add(int(line.split()[1]))
    return cps


def make(bdf, w, h, dst):
    have = bdf_cps(bdf)
    wanted = sorted(cp for a, b in RANGES for cp in range(a, b + 1) if cp in have)
    glyphs = bdf_to_mem.parse_bdf(bdf, w, h, set(wanted))
    out = bytearray(b"ZFN1")
    out += struct.pack("<BBBBI", w, h, 1, 0, len(wanted))
    for cp in wanted:
        out += struct.pack("<H", cp)
    while len(out) % 4:
        out += b"\0"
    for cp in wanted:
        for row in glyphs[cp]:
            v = 0
            for i, bit in enumerate(row):
                if bit:
                    v |= 0x80 >> i
            out.append(v)
    with open(dst, "wb") as f:
        f.write(out)
    print("wrote %s: %d glyphs, %d bytes" % (os.path.relpath(dst), len(wanted), len(out)))


def main():
    make(os.path.join(FONT, "5x8.bdf"), 5, 8, os.path.join(FONT, "script5x8.zfn"))
    make(os.path.join(FONT, "6x12.bdf"), 6, 12, os.path.join(FONT, "script6x12.zfn"))


if __name__ == "__main__":
    main()
