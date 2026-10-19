#!/usr/bin/env python3
"""
Reads a PWG raster file (PWG 5102.4) the way a printer does, written
from the standard rather than from pwg.c, so the two can disagree:
checks every header field print sets, decodes every line, and writes
each page as a PGM (grey) to look at.

  pwg_decode.py FILE.pwg [OUTDIR]

Exit 0 if the file is well formed.
"""

import struct
import sys


def fail(msg):
    print("pwg_decode: " + msg)
    sys.exit(1)


def u32(h, off):
    return struct.unpack(">I", h[off:off + 4])[0]


def cstr(h, off):
    return h[off:off + 64].split(b"\0")[0].decode("ascii", "replace")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    data = open(sys.argv[1], "rb").read()
    out = sys.argv[2] if len(sys.argv) > 2 else None
    if data[:4] != b"RaS2":
        fail("no RaS2 sync word")
    pos = 4
    page = 0
    while pos < len(data):
        if pos + 1796 > len(data):
            fail("short header at %d" % pos)
        h = data[pos:pos + 1796]
        pos += 1796
        page += 1
        if cstr(h, 0) != "PwgRaster":
            fail("page %d: MediaClass %r" % (page, cstr(h, 0)))
        xr, yr = u32(h, 276), u32(h, 280)
        w, ht = u32(h, 372), u32(h, 376)
        bpc, bpp, bpl = u32(h, 384), u32(h, 388), u32(h, 392)
        space, ncol = u32(h, 400), u32(h, 420)
        pw, ph = u32(h, 352), u32(h, 356)
        name = cstr(h, 1732)
        if bpc != 8 or bpp not in (8, 24) or bpl != w * bpp // 8:
            fail("page %d: bits %d/%d, bytes a line %d for %d" % (page, bpc, bpp, bpl, w))
        if (bpp == 8) != (space == 18) or (bpp == 24) != (space == 19):
            fail("page %d: colour space %d with %d bits" % (page, space, bpp))
        if ncol != bpp // 8:
            fail("page %d: %d colours" % (page, ncol))
        if abs(pw - w * 72 / xr) > 1 or abs(ph - ht * 72 / yr) > 1:
            fail("page %d: PageSize %dx%d for %dx%d at %d" % (page, pw, ph, w, ht, xr))
        if u32(h, 456) != 1 or u32(h, 460) != 1:
            fail("page %d: feed transforms" % page)
        n = bpp // 8
        rows = []
        y = 0
        while y < ht:
            if pos >= len(data):
                fail("page %d: ends at line %d of %d" % (page, y, ht))
            rep = data[pos] + 1
            pos += 1
            line = bytearray()
            while len(line) < bpl:
                if pos >= len(data):
                    fail("page %d line %d: ends inside a line" % (page, y))
                c = data[pos]
                pos += 1
                if c == 128:
                    line += b"\xff" * (bpl - len(line))
                elif c < 128:
                    px = data[pos:pos + n]
                    pos += n
                    line += px * (c + 1)
                else:
                    k = 257 - c
                    line += data[pos:pos + k * n]
                    pos += k * n
            if len(line) != bpl:
                fail("page %d line %d: %d bytes, not %d" % (page, y, len(line), bpl))
            if y + rep > ht:
                fail("page %d: repeat past the page end" % page)
            grey = bytes(line[0::n])
            if n == 3 and (line[0::3] != line[1::3] or line[0::3] != line[2::3]):
                fail("page %d line %d: RGB is not grey" % (page, y))
            rows.extend([grey] * rep)
            y += rep
        print("page %d: %dx%d at %d dpi, %s, %s, %d bytes so far" %
              (page, w, ht, xr, "sgray_8" if n == 1 else "srgb_8", name, pos))
        if out:
            with open("%s/pwg-%d.pgm" % (out, page), "wb") as f:
                f.write(b"P5\n%d %d\n255\n" % (w, ht))
                for r in rows:
                    f.write(r)
    print("pwg_decode: %d pages, well formed" % page)


main()
