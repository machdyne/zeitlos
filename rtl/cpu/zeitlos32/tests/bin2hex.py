#!/usr/bin/env python3
"""
Flat binary -> one hex word per line, for $readmemh in tb_zeitlos32.v.

Deliberately separate from sw/bios/makehex.py: that one pads to a fixed
BRAM depth and lives with the BIOS build, and coupling the CPU test
suite to it would mean the tests break when the BIOS memory map moves.
"""
import sys

if len(sys.argv) < 2:
    sys.exit("usage: bin2hex.py <file.bin> [words]")

data = open(sys.argv[1], "rb").read()
data += b"\x00" * ((-len(data)) % 4)
words = len(data) // 4

pad = int(sys.argv[2]) if len(sys.argv) > 2 else words

for i in range(pad):
    if i < words:
        print("%08x" % int.from_bytes(data[i * 4:i * 4 + 4], "little"))
    else:
        print("00000000")
