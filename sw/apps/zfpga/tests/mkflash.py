#!/usr/bin/env python3
"""A flash image for tests/run.sh's zfpga flash / run tests.

  mkflash.py OUT SIZE ZAR_END [JUMPLOADER] [FILE@ADDR ...]

SIZE bytes of FF; a ZAR2 archive at 0x140000 whose one entry ends at
ZAR_END; the jumploader, if given (and not "-"), at 0x200000 -- past the
first 2 MB, so only in an image larger than that; and any
FILE@ADDR placed as is -- Zeitlos's own gateware, a bootloader, a user
design already flashed."""
import struct, sys
out, size, zar_end = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0)
img = bytearray(b"\xff" * size)
zar = 0x140000
entry_off = 16 + 16 + 4                   # the entry's file, after the table and its name
hdr = b"ZAR2" + struct.pack("<I", 1) + b"\0" * 8
ent = struct.pack("<IIII", 32, 0, entry_off, zar_end - zar - entry_off) + b"wm\0\0"
img[zar:zar + len(hdr) + len(ent)] = hdr + ent
img[zar + entry_off:zar_end] = bytes((i * 7) & 0xFF for i in range(zar_end - zar - entry_off))
if len(sys.argv) > 4 and sys.argv[4] != "-":
    j = open(sys.argv[4], "rb").read()
    if size < 0x200000 + len(j):
        sys.exit("mkflash.py: a jumploader needs an image past 2 MB")
    img[0x200000:0x200000 + len(j)] = j
for arg in sys.argv[5:]:
    path, addr = arg.rsplit("@", 1)
    d = open(path, "rb").read()
    a = int(addr, 0)
    img[a:a + len(d)] = d
open(out, "wb").write(img)
