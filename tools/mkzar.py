#!/usr/bin/env python3
#
# Zeitlos
# Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
#
# Builds the flash-resident core app archive ("ZAR") programmed
# immediately after the kernel. See sw/os/zar.h for the layout (ZAR2)
# and the reasoning; this is just the other end of it.
#
#   tools/mkzar.py [--budgets "wm=112K/140K ..."] output/apps.zar \
#       wm=sw/apps/wm/wm.bin ... docs/welcome.txt=output/welcome.txt ...
#
# An entry whose name has a "/" in it is a FILE, read-only under the card
# at /<name> (docs/flash_apps.md, "Files in flash"); one without is an
# app. Files take no budget of their own (CORE_RESERVE is what they come
# out of) and must not be named under apps/.
#
# --budgets is the Makefile's CORE_BUDGETS: for each app, the most it
# may take in flash (its .bin) and in RAM (code, data and .bss, from
# the ZEXE header). Given, every app must have one and be within it,
# or there is no archive (docs/flash_apps.md, "Budgets").
#
# Each app is stored COMPRESSED (docs/executables.md, "Compressed
# executables"): its ZEXE header kept, with flags bit 8 set and `entry`
# set to the image's size, and the image after it as an LZ4 block
# (lz4_compress() below). The kernel unpacks it into the process's
# memory as it loads. --store "name ..." (CORE_STORE) keeps the named apps verbatim;
# so is anything that is not a plain ZEXE file, or would not get
# smaller. The budgets' flash figure is the size STORED; their RAM
# figure is the image's, unpacked. Files are stored verbatim.

import os
import struct
import sys

MAGIC = b"ZAR2"
NAME_MAX = 127            # keep in sync with Z_ZAR_NAME_MAX
HEADER_SIZE = 16
ENTRY_SIZE = 16
MAX_ENTRIES = 32          # keep in sync with Z_ZAR_MAX_ENTRIES
FLAG_FILE = 1             # Z_ZAR_FILE

EXEC_COMPRESSED = 0x0100  # Z_EXEC_FLAG_COMPRESSED, sw/common/zexec.h
EXEC_KNOWN = 0x000F | EXEC_COMPRESSED | 0x8000   # Z_EXEC_FLAGS_KNOWN


def lz4_compress(src, depth=48):
    """An LZ4 block (lz4_Block_format.md), as sw/common/zexec_lz4.c reads
    it: hash chains `depth` deep, and one step of lazy matching -- about
    the size lz4's own high-compression mode gets (68% of a core app).
    The format's end rules are kept (no match starts in the last 12
    bytes, the last 5 are literals), so any LZ4 decoder reads it."""
    n = len(src)
    out = bytearray()
    head = {}
    prev = [-1] * (n + 1)
    mflimit = n - 12          # no match starts past here
    matchlimit = n - 5        # and none runs past here

    def find(i):
        if i > mflimit:
            return 0, 0
        j = head.get(src[i:i + 4], -1)
        best, boff, d, lim = 0, 0, 0, matchlimit - i
        while j >= 0 and i - j <= 65535 and d < depth:
            if best == 0 or src[j + best] == src[i + best]:
                l = 4
                while l < lim and src[j + l] == src[i + l]:
                    l += 1
                if l > best:
                    best, boff = l, i - j
                    if l == lim:
                        break
            j = prev[j]
            d += 1
        return (best, boff) if best >= 4 else (0, 0)

    def length(r):
        while r >= 255:
            out.append(255)
            r -= 255
        out.append(r)

    def emit(a, b, ml, off):
        ll = b - a
        out.append((min(ll, 15) << 4) | (min(ml - 4, 15) if ml else 0))
        if ll >= 15:
            length(ll - 15)
        out.extend(src[a:b])
        if ml:
            out.append(off & 255)
            out.append(off >> 8)
            if ml >= 19:
                length(ml - 19)

    i = anchor = inserted = 0   # every position before `inserted` is in the chains

    def insert_to(end):
        nonlocal inserted
        end = min(end, mflimit + 1)
        while inserted < end:
            k = src[inserted:inserted + 4]
            prev[inserted] = head.get(k, -1)
            head[k] = inserted
            inserted += 1

    while i <= mflimit:
        insert_to(i)
        l, o = find(i)
        if not l:
            i += 1
            continue
        insert_to(i + 1)
        l2, o2 = find(i + 1)
        if l2 > l + 1:              # a longer match a byte on: take that
            i += 1
            l, o = l2, o2
        emit(anchor, i, l, o)
        i += l
        anchor = i
    emit(anchor, n, 0, 0)
    return bytes(out)


def lz4_decompress(src, n):
    """The other way, for checking: what zexec_lz4.c does, in Python."""
    out = bytearray()
    p = 0
    while True:
        t = src[p]
        p += 1
        ll = t >> 4
        if ll == 15:
            while True:
                b = src[p]
                p += 1
                ll += b
                if b != 255:
                    break
        out += src[p:p + ll]
        p += ll
        if len(out) == n:
            assert p == len(src), "mkzar: LZ4 input left over"
            return bytes(out)
        d = src[p] | src[p + 1] << 8
        p += 2
        ml = (t & 15) + 4
        if ml == 19:
            while True:
                b = src[p]
                p += 1
                ml += b
                if b != 255:
                    break
        assert 0 < d <= len(out)
        for _ in range(ml):
            out.append(out[-d])


def compress(data):
    """A ZEXE file, compressed (bit 8, `entry` = the image's size); or
    the same bytes, for anything that is not one or would not shrink."""
    if len(data) < 16 or data[:4] != b"ZEXE":
        return data
    ver, flags, bss, entry = struct.unpack("<HHII", data[4:16])
    if ver != 1 or entry or flags & ~EXEC_KNOWN or flags & EXEC_COMPRESSED:
        return data
    image = data[16:]
    if not image:
        return data
    z = lz4_compress(image)
    if 16 + len(z) >= len(data):
        return data
    assert lz4_decompress(z, len(image)) == image, "mkzar: LZ4 did not round-trip"
    return struct.pack("<4sHHII", b"ZEXE", ver, flags | EXEC_COMPRESSED,
                       bss, len(image)) + z


def compressed(data):
    if len(data) < 16 or data[:4] != b"ZEXE":
        return False
    return bool(struct.unpack("<H", data[6:8])[0] & EXEC_COMPRESSED)


def size(text):
    """'112K' -> 114688; '1M'; a plain number is bytes."""
    t = text.strip().upper()
    mul = 1
    if t.endswith("K"):
        mul, t = 1024, t[:-1]
    elif t.endswith("M"):
        mul, t = 1024 * 1024, t[:-1]
    return int(t, 0) * mul


def parse_budgets(text):
    """'wm=112K/140K net=...' -> {'wm': (114688, 143360), ...}"""
    out = {}
    for item in text.split():
        name, _, pair = item.partition("=")
        flash, _, ram = pair.partition("/")
        out[name] = (size(flash), size(ram))
    return out


def ram_image(data):
    """Code + data + .bss, from a ZEXE header; the file size if none."""
    if data[:4] != b"ZEXE":
        return len(data)
    bss, entry = struct.unpack("<II", data[8:16])
    if compressed(data):
        return entry + bss
    return len(data) - 16 + bss


def main(argv):
    budgets = None
    files_max = None
    zar_max = None
    store = set()
    while len(argv) > 2 and argv[1] in ("--budgets", "--files-max", "--max", "--store"):
        if argv[1] == "--budgets":
            budgets = parse_budgets(argv[2])
        elif argv[1] == "--store":
            store = set(argv[2].split())
        elif argv[1] == "--max":
            zar_max = size(argv[2])
        else:
            files_max = size(argv[2])
        argv = argv[:1] + argv[3:]
    if len(argv) < 3:
        print("usage: mkzar.py [--budgets \"name=FLASH/RAM ...\"] [--store \"name ...\"] "
              "[--files-max N] [--max N] "
              "<output.zar> <name>=<file.bin> ... <dir/file>=<file> ...", file=sys.stderr)
        return 1

    out_path = argv[1]
    specs = argv[2:]

    if len(specs) > MAX_ENTRIES:
        print("mkzar: %d apps, kernel accepts at most %d"
              % (len(specs), MAX_ENTRIES), file=sys.stderr)
        return 1

    entries = []
    for spec in specs:
        if "=" not in spec:
            print("mkzar: expected <name>=<file>, got '%s'" % spec,
                  file=sys.stderr)
            return 1
        name, path = spec.split("=", 1)
        is_file = "/" in name

        if len(name) > NAME_MAX or not name:
            print("mkzar: name '%s' is empty or longer than %d chars"
                  % (name, NAME_MAX), file=sys.stderr)
            return 1
        if is_file and (name.startswith("/") or name.endswith("/") or
                        "//" in name or name.lower().startswith("apps/")):
            print("mkzar: '%s': a file is named by its path without the "
                  "leading slash, and not under apps/" % name, file=sys.stderr)
            return 1
        if any(n.lower() == name.lower() for n, _, _ in entries):
            print("mkzar: '%s' twice" % name, file=sys.stderr)
            return 1

        if not os.path.exists(path):
            print("mkzar: %s: not found (build it first?)" % path,
                  file=sys.stderr)
            return 1

        with open(path, "rb") as f:
            data = f.read()

        # Not fatal, but worth saying: a raw (pre-ZEXE) binary still
        # loads -- z_exec_parse() treats a missing header as a legacy
        # image -- it just carries its .bss as literal zeros and wastes
        # flash. Better to notice here than to wonder about the size.
        if not is_file and data[:4] != b"ZEXE":
            print("mkzar: warning: %s has no ZEXE header (legacy binary)"
                  % path, file=sys.stderr)
        if not is_file and name not in store:
            data = compress(data)

        entries.append((name, data, is_file))

    if budgets is not None:
        over = []
        for name, data, is_file in entries:
            if is_file:
                continue
            if name not in budgets:
                over.append("%s has no budget (CORE_BUDGETS in the Makefile)" % name)
                continue
            bf, br = budgets[name]
            if len(data) > bf:
                over.append("%s is %d bytes in flash, over its budget of %d by %d"
                            % (name, len(data), bf, len(data) - bf))
            if ram_image(data) > br:
                over.append("%s is %d bytes in RAM, over its budget of %d by %d"
                            % (name, ram_image(data), br, ram_image(data) - br))
        if files_max is not None:
            fb = sum(len(d) for _, d, f in entries if f)
            if fb > files_max:
                over.append("the files are %d bytes, over CORE_RESERVE (%d) by %d"
                            % (fb, files_max, fb - files_max))
        if over:
            print("mkzar: over budget (docs/flash_apps.md, \"Budgets\"):\n  "
                  + "\n  ".join(over), file=sys.stderr)
            return 1

    # The header, the entry table, the names, then the data -- each name
    # and each file 4-byte aligned, so the copy out of flash runs a word
    # at a time from the first byte.
    def align(n):
        return (n + 3) & ~3

    names = b""
    name_off = []
    pos = HEADER_SIZE + ENTRY_SIZE * len(entries)
    for name, _, _ in entries:
        name_off.append(pos + len(names))
        names += name.encode("ascii") + b"\0"
    pos = align(pos + len(names))
    names = names.ljust(pos - (HEADER_SIZE + ENTRY_SIZE * len(entries)), b"\0")

    table = b""
    payload = b""
    for (name, data, is_file), noff in zip(entries, name_off):
        table += struct.pack("<IIII", noff, FLAG_FILE if is_file else 0,
                             pos + len(payload), len(data))
        payload += data
        payload = payload.ljust(align(len(payload)), b"\0")

    blob = MAGIC + struct.pack("<I", len(entries)) + b"\0" * 8
    blob += table + names + payload

    # The region is the hard limit: past it is the flashtest sector.
    if zar_max is not None and len(blob) > zar_max:
        print("mkzar: the archive is %d bytes, over its flash region of %d by %d"
              % (len(blob), zar_max, len(blob) - zar_max), file=sys.stderr)
        for name, data, is_file in entries:
            print("         %-16s %7d" % (name, len(data)), file=sys.stderr)
        return 1

    out_dir = os.path.dirname(out_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(blob)

    print("mkzar: %s: %d entries, %d bytes" % (out_path, len(entries),
                                               len(blob)))
    for name, data, is_file in entries:
        if budgets is not None and name in budgets:
            bf, br = budgets[name]
            print("         %-16s %7d of %7d flash  %7d of %7d RAM%s"
                  % (name, len(data), bf, ram_image(data), br,
                     "" if compressed(data) else "  (stored)"))
        else:
            print("         %-16s %7d" % (name, len(data)))

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
