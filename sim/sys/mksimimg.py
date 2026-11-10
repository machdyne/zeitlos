#!/usr/bin/env python3
#
# A sim image (docs/sim_parity.md, "Images"): a flash image laid out as
# a release's zeitlos-<board>.img is -- the kernel, the core apps'
# archive, the key/value store, at the offsets release/lib/layout.py
# reads from the tree -- with the BIOS where the gateware would be.
#
#   mksimimg.py --bios sw/bios/bios.bin --kernel sw/os/kernel.bin \
#               [--zar output/lakritz/apps.zar] -o zeitlos-sim.img
#
# The gateware region starts with "ZSIMBIOS", the BIOS's length (32
# bits, little-endian) and the BIOS. zsys loads that into BRAM, where
# the bitstream's BRAM initialisation would have put it on a board. So
# a page that runs Zeitlos never sends a visitor the gateware, which the
# emulator has no use for. The rest of the region is erased (0xFF), as
# is everything not written.
#
# The key/value store is left erased: a new machine.

import argparse
import os
import struct
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "release", "lib"))

import layout as layout_mod  # noqa: E402

MAGIC = b"ZSIMBIOS"


def zar_apps(zar):
    """The apps in a core apps' archive (tools/mkzar.py's ZAR2: a header,
    16-byte entries {name offset, flags, offset, size}, the names), not
    its files. None if it is not one."""
    if len(zar) < 16 or zar[:4] != b"ZAR2":
        return None
    n = struct.unpack_from("<I", zar, 4)[0]
    apps = []
    for i in range(n):
        noff, flags, _off, _size = struct.unpack_from("<IIII", zar, 16 + 16 * i)
        name = zar[noff:zar.index(b"\0", noff)].decode("ascii", "replace")
        if not flags & 1:
            apps.append(name)
    return apps


def build(bios, kernel, zar=None, root=ROOT):
    """The image's bytes."""
    lay = layout_mod.load(root)
    regions = {r.key: r for r in lay["regions"]}
    img = bytearray(b"\xff") * lay["flash_size"]

    def put(key, data, what):
        r = regions[key]
        if len(data) > r.limit:
            raise SystemExit("mksimimg: %s is %d bytes; the %s region holds %d"
                             % (what, len(data), r.name, r.limit))
        img[r.offset:r.offset + len(data)] = data

    put("gateware", MAGIC + struct.pack("<I", len(bios)) + bios, "the BIOS")
    put("kernel", kernel, "the kernel")
    if zar is not None:
        put("apps", zar, "the core apps' archive")
    return bytes(img)


def main(argv):
    ap = argparse.ArgumentParser(description="build a sim image")
    ap.add_argument("--bios", required=True)
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--zar")
    ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args(argv)
    read = lambda p: open(p, "rb").read()
    img = build(read(a.bios), read(a.kernel), read(a.zar) if a.zar else None)
    with open(a.out, "wb") as f:
        f.write(img)
    print("mksimimg: %s, %d bytes" % (a.out, len(img)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
