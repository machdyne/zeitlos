#!/usr/bin/env python3
#
# Does the screen show what wm says it drew? (docs/zsys.md, "Testing")
#
#   check_screen.py LOG PNG [NAME ...]
#
# LOG is zsys's console output, PNG its --screenshot (without --cursor).
# wm logs every window it places ("wm: win 1 'welcome' pid 3 at 44,40
# 420x300"); each NAME given must be among them, and every window's
# frame -- its top row and its left column -- must be lit on the
# screen. A check on the GPU's work that does not change whenever wm's
# look does: it holds as long as windows have a frame.
#
#   check_screen.py --within WxH LOG PNG [NAME ...]
#
# also fails any window not wholly inside WxH: the screen size the
# system was told to report (system.screen.size, docs/screen.md), so
# the windows wm placed kept to it (`make test-screen`). With --named,
# only the windows NAMEd must show their frame: for a screen where one
# window has since covered another, or moved it.
#
# Reads the PNG zsys writes -- 1 bit a pixel, indexed, the index the
# framebuffer's bit -- or an 8-bit RGB or RGBA one (the page's canvas),
# with zlib alone.

import re
import struct
import sys
import zlib

WIN = re.compile(r"wm: win (\d+) '([^']*)' pid \d+ at (\d+),(\d+) (\d+)x(\d+)")


def unfilter(raw, h, stride, bpp):
    """PNG's five line filters undone: the rows, without their filter bytes."""
    out, prev = [], bytearray(stride)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out.append(bytes(line))
        prev = line
    return out


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("check_screen: %s is not a PNG" % path)
    pos, idat, w, h, kind_px = 8, b"", 0, 0, None
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, colour = struct.unpack(">IIBB", body[:10])
            kind_px = (depth, colour)
            if kind_px not in ((8, 2), (8, 6), (1, 3), (1, 0)):
                raise SystemExit("check_screen: %s: 1-bit, or 8-bit RGB or RGBA, only" % path)
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    lit = []
    if kind_px[0] == 8:
        bpp = 3 if kind_px[1] == 2 else 4
        for row in unfilter(raw, h, w * bpp, bpp):
            # bright, not merely not black: with desktop colour the
            # background is a dark tint, and the frames ink or a light one
            lit.append([sum(row[x * bpp:x * bpp + 3]) > 3 * 100 for x in range(w)])
    else:
        for row in unfilter(raw, h, (w + 7) // 8, 1):
            lit.append([bool(row[x >> 3] & (0x80 >> (x & 7))) for x in range(w)])
    return w, h, lit


def main(argv):
    within, named = None, False
    while argv[:1] in (["--within"], ["--named"]):
        if argv[0] == "--named":
            named, argv = True, argv[1:]
        elif len(argv) > 1:
            within = tuple(int(n) for n in argv[1].lower().split("x"))
            argv = argv[2:]
        else:
            break
    if len(argv) < 2:
        print("usage: check_screen.py LOG PNG [NAME ...]", file=sys.stderr)
        return 2
    log = open(argv[0], errors="replace").read()
    w, h, lit = read_png(argv[1])
    wins = {}
    for m in WIN.finditer(log):
        wins[int(m.group(1))] = (m.group(2), int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6)))
    if not wins:
        print("check_screen: wm placed no windows (no \"wm: win\" lines)")
        return 1
    bad = 0
    names = {v[0] for v in wins.values()}
    for want in argv[2:]:
        if want not in names:
            print("check_screen: no window '%s' (there are: %s)" % (want, ", ".join(sorted(names))))
            bad += 1
    for n, (name, x, y, ww, hh) in sorted(wins.items()):
        x1, y1 = min(x + ww, w), min(y + hh, h)
        top = [lit[y][i] for i in range(x, x1)] if y < h else []
        left = [lit[j][x] for j in range(y, y1)] if x < w else []
        ft = sum(top) / max(len(top), 1)
        fl = sum(left) / max(len(left), 1)
        ok = ft >= 0.9 and fl >= 0.9 or (named and name not in argv[2:])
        print("check_screen: win %d '%s' at %d,%d %dx%d: frame top %d%%, left %d%% -- %s"
              % (n, name, x, y, ww, hh, ft * 100, fl * 100, "ok" if ok else "NOT DRAWN"))
        bad += not ok
        if within and (x + ww > within[0] or y + hh > within[1]):
            print("check_screen: win %d '%s' at %d,%d %dx%d is not inside %dx%d"
                  % (n, name, x, y, ww, hh, within[0], within[1]))
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
