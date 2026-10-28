#!/usr/bin/env python3
#
# Zeitlos ask -- lens.zln, the chunk lengths for BM25.
#
#   python3 tools/ask/tests/test_lens.py [<pack-out-dir> ...]
#
# lens.zln is a copy of something the device can also work out for
# itself: chunks.zct's byte-length column over BYTES_PER_TOKEN, which is
# what sw/apps/ask/aidx.c computes for a pack built before lens.zln
# existed. The two must give the same numbers, or a pack ranks
# differently depending on its age. So:
#
#   - pack_lens() of synthetic chunks: header, clamping, padding;
#   - pack_lens() and pack_chunks() of the same chunks agree, read back
#     the way the device reads them;
#   - for each built pack named (tools/ask/out/zdocs), its lens.zln
#     matches its chunks.zct value for value, and the dsid agrees.
#
# sw/apps/ask/test/loadtest.c checks the device side of the same thing.
#
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.dont_write_bytecode = True
from lib import pack  # noqa: E402

FAILS = []


def check(cond, what):
    print("  %s  %s" % ("ok  " if cond else "FAIL", what))
    if not cond:
        FAILS.append(what)


class C:
    def __init__(self, length):
        self.doc, self.off, self.length, self.heading = 0, 0, length, "h"


def hdr(blob):
    w = struct.unpack("<8I", blob[:32])
    return w, (sum(w[:7]) & 0xffffffff) == w[7]


def lens_of_zct(blob):
    """The device's fallback (aidx.c, L_ZCT_READ), in Python."""
    w, _ = hdr(blob)
    out = []
    for i in range(w[3]):
        length = struct.unpack_from("<I", blob, 32 + i * 16 + 8)[0]
        out.append(max(1, min(65535, length // pack.BYTES_PER_TOKEN)))
    return out


def lens_of_zln(blob):
    w, _ = hdr(blob)
    return list(struct.unpack_from("<%dH" % w[3], blob, 32))


def synthetic():
    print("synthetic chunks")
    lengths = [0, 1, 5, 6, 7, 600, 6 * 65535, 6 * 65536 + 100, 1234]
    chunks = [C(n) for n in lengths]
    blob = pack.pack_lens(chunks, dsid=0x1234abcd)
    w, ok = hdr(blob)
    check(ok, "header checksum")
    check(w[0] == pack.MAGIC_LENS, "magic ZLN1")
    check(w[1] == pack.FORMAT_VERSION, "format version")
    check(w[3] == len(chunks) and w[4] == 2, "count and entry size")
    check(w[5] == pack.BYTES_PER_TOKEN, "bytes per token recorded")
    check(w[6] == 0x1234abcd, "dsid")
    check(len(blob) % 4 == 0, "padded to a word")
    check(len(blob) == 32 + ((2 * len(chunks) + 3) & ~3), "size")
    got = lens_of_zln(blob)
    check(got[0] == 1 and got[1] == 1, "zero and tiny lengths clamp to 1")
    check(got[6] == 65535 and got[7] == 65535, "long lengths clamp to 65535")
    check(got == lens_of_zct(pack.pack_chunks(chunks, 0x1234abcd)),
          "equals what the device computes from chunks.zct")


def built(outdir):
    askroot = os.path.join(outdir, "data", "ask")
    for name in sorted(os.listdir(askroot)):
        d = os.path.join(askroot, name)
        print("pack %s" % d)
        zln = os.path.join(d, "lens.zln")
        if not os.path.exists(zln):
            check(False, "lens.zln present (rebuild with this tools/ask)")
            continue
        with open(zln, "rb") as fh:
            lz = fh.read()
        with open(os.path.join(d, "chunks.zct"), "rb") as fh:
            cz = fh.read()
        wl, okl = hdr(lz)
        wc, _ = hdr(cz)
        check(okl and wl[0] == pack.MAGIC_LENS, "lens.zln header")
        check(wl[6] == wc[6], "same dsid as chunks.zct")
        check(wl[3] == wc[3], "one entry per chunk (%d)" % wc[3])
        a, b = lens_of_zln(lz), lens_of_zct(cz)
        diff = sum(1 for x, y in zip(a, b) if x != y)
        check(a == b, "values equal chunks.zct's (%d differ)" % diff)
        print("      %d bytes, against %d bytes of chunk records"
              % (len(lz), 16 * wc[3]))


def main():
    synthetic()
    for d in sys.argv[1:]:
        built(d)
    print("\n%s" % ("FAILED: %d" % len(FAILS) if FAILS else "all ok"))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
