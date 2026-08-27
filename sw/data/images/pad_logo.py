#!/usr/bin/env python3
"""
Pads the boot logo out to a full 640x480 framebuffer image.

zeitlos.bin is 512x384 1bpp (see xbmtobit.c). Centring that in a 640x480
framebuffer at boot means a row-by-row copy, because each 64-byte source
row lands at a different offset in an 80-byte-stride destination. Doing
the centring HERE, once, at build time, makes the boot-time job a single
memcpy of the whole framebuffer -- in both sw/bios/bios.c (where BIOS
space is measured in bytes) and sw/os/logo.c.

Same reasoning applies to inversion: --invert flips every bit here
rather than making every boot pay for a branch per byte. The source
.xbm's polarity was never confirmed against real hardware, so if the
splash shows up with foreground/background swapped, regenerate with
--invert instead of changing any C.

Usage: pad_logo.py <in.bin> <out.bin> [--invert]
"""

import sys

SRC_W, SRC_H = 512, 384
DST_W, DST_H = 640, 480

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    invert = "--invert" in sys.argv
    if len(args) != 2:
        sys.exit("usage: pad_logo.py <in.bin> <out.bin> [--invert]")

    src = open(args[0], "rb").read()
    src_stride, dst_stride = SRC_W // 8, DST_W // 8
    want = src_stride * SRC_H
    if len(src) != want:
        sys.exit(f"{args[0]}: expected {want} bytes ({SRC_W}x{SRC_H}), got {len(src)}")

    # 0x00 background, so the area around the logo is blank rather than
    # whatever happened to be in VRAM at reset.
    dst = bytearray(dst_stride * DST_H)

    x_byte = ((DST_W - SRC_W) // 2) // 8      # 64px in -> byte 8
    y_off = (DST_H - SRC_H) // 2              # 48 rows down

    for r in range(SRC_H):
        s = r * src_stride
        d = (y_off + r) * dst_stride + x_byte
        dst[d:d + src_stride] = src[s:s + src_stride]

    if invert:
        dst = bytearray(b ^ 0xFF for b in dst)

    open(args[1], "wb").write(dst)
    print(f"wrote {args[1]}: {len(dst)} bytes ({DST_W}x{DST_H} 1bpp)"
          f"{' inverted' if invert else ''}")

main()
