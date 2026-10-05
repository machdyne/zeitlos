#!/usr/bin/env python3
"""
Wraps a raw objcopy binary in a Zeitlos executable header.

The format is defined in sw/common/zexec.h -- read that first; the
short version is that .bss becomes a NUMBER in a 16-byte header rather
than a region of literal zeros appended to the file, so the loader
memset()s it instead of reading it off the SD card.

The saving is real: repl.bin was 293KB, of which ~110KB was zeros.

Usage:
    mkexec.py <in.bin> <out.bin> <bss-size> [tier]

where <in.bin> is `objcopy -O binary` output WITHOUT --pad-to (so it
ends at _edata), and <bss-size> is (_end - _edata). Both are things the
app Makefiles already compute with nm.

[tier] is optional: SMALL, DEFAULT, MEDIUM, LARGE, BIG or HUGE. It is
written into flags bits 2:0 (sw/common/zexec.h). Omit it, and the field
stays 0, which the loader reads as "this program does not ask".
"""

import struct
import sys

MAGIC = b"ZEXE"
VERSION = 1
HEADER_SIZE = 16

# Keep in step with Z_EXEC_TIER_* in sw/common/zexec.h.
TIERS = {
    "SMALL": 1,
    "DEFAULT": 2,
    "MEDIUM": 3,
    "LARGE": 4,
    "BIG": 5,
    "HUGE": 6,
}


def main():
    if len(sys.argv) not in (4, 5):
        sys.exit("usage: mkexec.py <in.bin> <out.bin> <bss-size> [tier]")

    src, dst, bss = sys.argv[1], sys.argv[2], int(sys.argv[3], 0)
    tier_name = sys.argv[4] if len(sys.argv) == 5 else ""

    if bss < 0:
        sys.exit(f"mkexec: negative bss size ({bss}) -- check _end/_edata")

    if tier_name == "":
        flags = 0
    elif tier_name in TIERS:
        flags = TIERS[tier_name]
    else:
        names = ", ".join(TIERS)
        sys.exit(f"mkexec: unknown tier {tier_name!r} (one of {names})")

    data = open(src, "rb").read()

    # entry is reserved: every app is linked at and started from
    # 0x80000000 (riscv-app.ld, k_proc_create), so there is nothing to
    # record yet. 0 means "use the base address".
    header = struct.pack("<4sHHII", MAGIC, VERSION, flags, bss, 0)
    assert len(header) == HEADER_SIZE, len(header)

    with open(dst, "wb") as f:
        f.write(header)
        f.write(data)

    total = len(data) + bss
    saved = bss
    asked = f", tier {tier_name}" if flags else ""
    print(f"{dst}: {len(data)} data + {bss} bss = {total} image "
          f"({len(data) + HEADER_SIZE} on disk, {saved} bytes of zeros "
          f"no longer stored{asked})")


main()
