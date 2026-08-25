#!/usr/bin/env python3
"""
Wraps a raw objcopy binary in a Zeitlos executable header.

The format is defined in sw/common/zexec.h -- read that first; the
short version is that .bss becomes a NUMBER in a 16-byte header rather
than a region of literal zeros appended to the file, so the loader
memset()s it instead of reading it off the SD card.

The saving is real: repl.bin was 293KB, of which ~110KB was zeros.

Usage:
    mkexec.py <in.bin> <out.bin> <bss-size>

where <in.bin> is `objcopy -O binary` output WITHOUT --pad-to (so it
ends at _edata), and <bss-size> is (_end - _edata). Both are things the
app Makefiles already compute with nm.
"""

import struct
import sys

MAGIC = b"ZEXE"
VERSION = 1
HEADER_SIZE = 16


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: mkexec.py <in.bin> <out.bin> <bss-size>")

    src, dst, bss = sys.argv[1], sys.argv[2], int(sys.argv[3], 0)

    if bss < 0:
        sys.exit(f"mkexec: negative bss size ({bss}) -- check _end/_edata")

    data = open(src, "rb").read()

    # entry is reserved: every app is linked at and started from
    # 0x80000000 (riscv-app.ld, k_proc_create), so there is nothing to
    # record yet. 0 means "use the base address".
    header = struct.pack("<4sHHII", MAGIC, VERSION, 0, bss, 0)
    assert len(header) == HEADER_SIZE, len(header)

    with open(dst, "wb") as f:
        f.write(header)
        f.write(data)

    total = len(data) + bss
    saved = bss
    print(f"{dst}: {len(data)} data + {bss} bss = {total} image "
          f"({len(data) + HEADER_SIZE} on disk, {saved} bytes of zeros "
          f"no longer stored)")


main()
