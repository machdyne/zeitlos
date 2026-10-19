#!/usr/bin/env python3
"""
The constant packets rtl/gpu/ddmi_island.v sends, as Verilog
localparams, from this directory's model (ddmi.py). Run it and paste
its output into ddmi_island.v when a packet changes; test_ddmi.py
checks the two still agree.

    python3 tools/ddmi/gen_packets.py

Layout of each 248-bit vector: bits 23:0 are HB0..HB2 (HB0 in 7:0),
then subpacket k's SB0..SB6 at 24 + 56k (SB0 lowest). Parity is NOT
included -- the gateware computes it as the bits go out.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ddmi as D


def vec(p):
    v = 0
    for i, b in enumerate(p.hb):
        v |= b << (8 * i)
    for k in range(4):
        for j, b in enumerate(p.sb[k]):
            v |= b << (24 + 56 * k + 8 * j)
    return v


PACKETS = [
    ("PK_GCP", D.gcp(clear_avmute=True), "general control: clear AV mute"),
    ("PK_AVI", D.avi_infoframe(), "AVI InfoFrame: VIC 1, RGB full range, 4:3, IT, underscan"),
    ("PK_SPD", D.spd_infoframe(), "SPD InfoFrame: Zeitlos, PC general"),
    ("PK_AIF", D.audio_infoframe(), "Audio InfoFrame: 2 channels, the rest from the stream"),
]


def verilog():
    out = []
    for name, p, what in PACKETS:
        out.append("\t// %s" % what)
        out.append("\tlocalparam [247:0] %s = 248'h%062x;" % (name, vec(p)))
    return "\n".join(out)


if __name__ == "__main__":
    print(verilog())
