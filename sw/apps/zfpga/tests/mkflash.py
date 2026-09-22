#!/usr/bin/env python3
"""A flash image for tests/run.sh's zfpga flash / run tests.

  mkflash.py OUT SIZE ZAR_END [JUMPLOADER]

SIZE bytes of FF; a ZAR1 archive at 0x140000 whose one entry ends at
ZAR_END; the jumploader, if given, at 0x1D0000."""
import struct, sys
out, size, zar_end = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0)
img = bytearray(b"\xff" * size)
zar = 0x140000
entry_off = 16 + 24                       # the entry's file, right after the table
hdr = b"ZAR1" + struct.pack("<I", 1) + b"\0" * 8
ent = b"wm".ljust(16, b"\0") + struct.pack("<II", entry_off, zar_end - zar - entry_off)
img[zar:zar + len(hdr) + len(ent)] = hdr + ent
img[zar + entry_off:zar_end] = bytes((i * 7) & 0xFF for i in range(zar_end - zar - entry_off))
if len(sys.argv) > 4:
    j = open(sys.argv[4], "rb").read()
    img[0x1D0000:0x1D0000 + len(j)] = j
open(out, "wb").write(img)
