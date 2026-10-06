#!/usr/bin/env python3
"""
Wraps a raw objcopy binary in a Zeitlos executable header.

The format is defined in sw/common/zexec.h -- read that first; the
short version is that .bss becomes a NUMBER in a 16-byte header rather
than a region of literal zeros appended to the file, so the loader
memset()s it instead of reading it off the SD card.

The saving is real: repl.bin was 293KB, of which ~110KB was zeros.

Usage:
    mkexec.py <in.bin> <out.bin> <bss-size> [stack]

where <in.bin> is `objcopy -O binary` output WITHOUT --pad-to (so it
ends at _edata), and <bss-size> is (_end - _edata). Both are things the
app Makefiles already compute with nm.

[stack] is optional: the stack+heap size the program wants, in bytes
or with a K or M suffix (8K, 64K, 1M, 4M). It must be a power of two
from 8K up to the kernel's cap (Z_PROC_STACK_CAP in sw/os/kernel.h,
read from there), and it is written into flags bits 3:0 as a size
code (sw/common/zexec.h). Omit it, and the field stays 0, which the
loader reads as "this program does not ask".
"""

import os
import re
import struct
import sys

MAGIC = b"ZEXE"
VERSION = 1
HEADER_SIZE = 16

# sw/common/zexec.h: code 1 is 8KB, each code doubles, 14 is the last.
STACK_UNIT = 8 * 1024
STACK_MAX_CODE = 14

KERNEL_H = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "..", "sw", "os", "kernel.h")


def kernel_cap():
    """Z_PROC_STACK_CAP, from sw/os/kernel.h, so the cap is one number."""
    m = re.search(r"^#define\s+Z_PROC_STACK_CAP\s+\(?([0-9\s*]+)\)?\s*$",
                  open(KERNEL_H).read(), re.M)
    if not m:
        sys.exit(f"mkexec: no Z_PROC_STACK_CAP in {KERNEL_H}")
    cap = 1
    for factor in m.group(1).split("*"):
        cap *= int(factor)
    return cap


def parse_size(text):
    m = re.fullmatch(r"([0-9]+)([KkMm]?)", text)
    if not m:
        sys.exit(f"mkexec: stack size {text!r} is not a number of bytes, "
                 f"or of K or M")
    n = int(m.group(1))
    return n * {"": 1, "k": 1024, "m": 1024 * 1024}[m.group(2).lower()]


def stack_code(text):
    size = parse_size(text)
    cap = kernel_cap()
    if size < STACK_UNIT or size > cap or size & (size - 1):
        sys.exit(f"mkexec: stack size {text} ({size} bytes) must be a power "
                 f"of two from {STACK_UNIT // 1024}K to the kernel's cap, "
                 f"{cap // 1024}K")
    code = size.bit_length() - STACK_UNIT.bit_length() + 1
    assert 1 <= code <= STACK_MAX_CODE and STACK_UNIT << (code - 1) == size
    return code


def main():
    if len(sys.argv) not in (4, 5):
        sys.exit("usage: mkexec.py <in.bin> <out.bin> <bss-size> [stack]")

    src, dst, bss = sys.argv[1], sys.argv[2], int(sys.argv[3], 0)
    stack = sys.argv[4] if len(sys.argv) == 5 else ""

    if bss < 0:
        sys.exit(f"mkexec: negative bss size ({bss}) -- check _end/_edata")

    flags = stack_code(stack) if stack else 0

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
    asked = f", stack {stack} (code {flags})" if flags else ""
    print(f"{dst}: {len(data)} data + {bss} bss = {total} image "
          f"({len(data) + HEADER_SIZE} on disk, {saved} bytes of zeros "
          f"no longer stored{asked})")


main()
