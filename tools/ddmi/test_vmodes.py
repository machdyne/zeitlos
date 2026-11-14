#!/usr/bin/env python3
#
# Zeitlos
# Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
#
# Checks a capture from rtl/gpu/bench/tb_vmodes.v: one screen mode,
# off the wire (docs/screen_modes.md).
#
#   test_vmodes.py CAPTURE --mode M [--wide] [--islands]
#
# What it checks, in the receiver's terms (tools/ddmi/ddmi.py's
# decode()): the line and frame lengths and the sync widths and
# polarities against the mode's table; every whole frame's picture, row
# by row, against the framebuffer the bench's VRAM holds (its words made
# from their addresses, as vram_word() is in the bench), at the mode's
# row length -- or, for 1280x720 on a 25F build, the 640x360 desktop
# shown twice each way; and with islands, that they decode (preambles,
# guard bands, BCH parity) and that the AVI InfoFrame names the mode.

import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ddmi  # noqa: E402

# mode: (h_total, v_total, width, height, hsync, vsync, hsync positive,
#        vsync positive, vic, AVI picture aspect: 1 is 4:3, 2 is 16:9)
MODES = {
    0: (800, 525, 640, 480, 96, 2, False, False, 1, 1),
    1: (998, 628, 800, 600, 96, 4, True, True, 0, 1),
    2: (1328, 806, 1024, 768, 136, 6, False, False, 0, 1),
    3: (1680, 750, 1280, 720, 40, 5, True, True, 4, 2),
    4: (1200, 525, 800, 480, 48, 3, False, False, 0, 2),
    5: (1440, 875, 1280, 800, 32, 6, True, False, 0, 2),
    6: (2240, 1125, 1920, 1080, 44, 5, True, True, 34, 2),
}
# the desktop's row length in words; 1920x1080 is a 960x540 desktop
ROW_WORDS = {0: 20, 1: 25, 2: 32, 3: 40, 4: 25, 5: 40, 6: 30}
INK = 0x80


def vram_word(a):
    h = (a * 2654435761) & 0xFFFFFFFF
    return (h ^ (h >> 15) ^ ((a << 17) & 0xFFFFFFFF)) & 0xFFFFFFFF


def fb_pixel(x, y, rw):
    w = vram_word((y * rw + x // 32) & 0x7FFF)
    return (w >> (x % 32)) & 1


def main(argv):
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--mode", type=int, required=True)
    ap.add_argument("--wide", action="store_true")
    ap.add_argument("--islands", action="store_true")
    a = ap.parse_args(argv)

    ht, vt, w, h, hsw, vsw, hpos, vpos, vic, aspect = MODES[a.mode]
    chars = ddmi.read_capture(a.capture)
    errs = []

    # -- syncs, from the characters that carry them --
    hs_l, vs_l = [], []
    # Only control periods and data islands carry the syncs. The video
    # guard band's channel 0 (0x2CC) is also TERC4's 8, and a pixel can
    # be any code, so from the video guard band to the next control
    # character the syncs hold.
    last = (None, None)
    video = False
    for c0, c1, c2 in chars:
        if c0 in ddmi.CTRL_INV:
            video = False
            v = ddmi.CTRL_INV[c0]
            last = (v & 1, (v >> 1) & 1)
        elif c1 == 0x133 and c2 == 0x2CC:
            video = True
        elif not video and c0 in ddmi.TERC4_INV:
            v = ddmi.TERC4_INV[c0]
            last = (v & 1, (v >> 1) & 1)
        hs_l.append(last[0])
        vs_l.append(last[1])
    act = 1 if hpos else 0
    vact = 1 if vpos else 0
    # pulse starts: the edge into the active level
    hstarts = [i for i in range(1, len(hs_l)) if hs_l[i] == act and hs_l[i - 1] == 1 - act]
    vstarts = [i for i in range(1, len(vs_l)) if vs_l[i] == vact and vs_l[i - 1] == 1 - vact]
    if len(hstarts) < 3:
        errs.append("fewer than three hsync pulses")
    else:
        periods = set(hstarts[i + 1] - hstarts[i] for i in range(len(hstarts) - 1))
        if periods != {ht}:
            errs.append("line lengths %s, want %d" % (sorted(periods)[:8], ht))
        widths = set()
        for s in hstarts[:-1]:
            k = s
            while k < len(hs_l) and hs_l[k] == act:
                k += 1
            widths.add(k - s)
        if widths != {hsw}:
            errs.append("hsync widths %s, want %d" % (sorted(widths)[:8], hsw))
    if len(vstarts) < 2:
        errs.append("fewer than two vsync pulses")
    else:
        fl = set(vstarts[i + 1] - vstarts[i] for i in range(len(vstarts) - 1))
        if fl != {ht * vt}:
            errs.append("frame lengths %s characters, want %d (%d lines)" % (sorted(fl), ht * vt, vt))
        k = vstarts[0]
        while k < len(vs_l) and vs_l[k] == vact:
            k += 1
        if k - vstarts[0] != ht * vsw:
            errs.append("vsync of %d characters, want %d (%d lines)" % (k - vstarts[0], ht * vsw, vsw))

    # -- the picture --
    rep = ddmi.decode(chars)
    for e in rep.errors[:20]:
        errs.append("decode: " + e)
    full = [fr for fr in rep.frames if len(fr) == h]
    if not full:
        errs.append("no whole frame of %d rows (frames of %s)" % (h, [len(f) for f in rep.frames]))
    # 1280x720 from 640x360 (the 25F boards, and NATIVE720=0: check it
    # without --wide), and 1920x1080 from 960x540
    dbl = (a.mode == 3 and not a.wide) or a.mode == 6
    rw = 20 if (a.mode == 3 and dbl) else ROW_WORDS[a.mode]
    bad = 0
    for fi, fr in enumerate(full):
        for y, row in enumerate(fr):
            if len(row) != w:
                errs.append("frame %d row %d is %d wide, want %d" % (fi, y, len(row), w))
                bad += 1
                if bad > 10:
                    break
                continue
            fy = y // 2 if dbl else y
            for x in range(w):
                fx = x // 2 if dbl else x
                want = INK if fb_pixel(fx, fy, rw) else 0
                r, g, b = row[x]
                if (r, g, b) != (want, want, want):
                    errs.append("frame %d (%d, %d): %s, want %02x" % (fi, x, y, (r, g, b), want))
                    bad += 1
                    break
            if bad > 10:
                break

    # -- islands --
    if a.islands:
        summ = ddmi.check_packets(rep)
        for e in rep.errors[:20]:
            if "decode: " + e not in errs:
                errs.append(e)
        if rep.mode != "hdmi":
            errs.append("no data islands")
        avi = summ["infoframes"].get("AVI")
        if not avi:
            errs.append("no AVI InfoFrame")
        else:
            pb = avi[1]
            if pb[3] != vic:
                errs.append("AVI VIC %d, want %d" % (pb[3], vic))
            if (pb[1] >> 4) & 3 != aspect:
                errs.append("AVI picture aspect %d, want %d" % ((pb[1] >> 4) & 3, aspect))
        for k in ("GCP", "SPD"):
            if not summ["count"].get(k) and k != "SPD":
                errs.append("no %s" % k)

    name = "mode %d%s%s" % (a.mode, " wide" if a.wide else "", " islands" if a.islands else "")
    if errs:
        for e in errs[:30]:
            print("test_vmodes: %s: FAIL %s" % (name, e))
        return 1
    print("test_vmodes: %s: PASS (%d whole frames, %d characters)" % (name, len(full), len(chars)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
