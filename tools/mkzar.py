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
# The .bin files are stored VERBATIM -- they are already ZEXE files
# produced by each app's own Makefile, and re-encoding them here would
# mean two places that have to agree about the format instead of one.

import os
import struct
import sys

MAGIC = b"ZAR2"
NAME_MAX = 127            # keep in sync with Z_ZAR_NAME_MAX
HEADER_SIZE = 16
ENTRY_SIZE = 16
MAX_ENTRIES = 32          # keep in sync with Z_ZAR_MAX_ENTRIES
FLAG_FILE = 1             # Z_ZAR_FILE


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
    bss = struct.unpack("<I", data[8:12])[0]
    return len(data) - 16 + bss


def main(argv):
    budgets = None
    files_max = None
    while len(argv) > 2 and argv[1] in ("--budgets", "--files-max"):
        if argv[1] == "--budgets":
            budgets = parse_budgets(argv[2])
        else:
            files_max = size(argv[2])
        argv = argv[:1] + argv[3:]
    if len(argv) < 3:
        print("usage: mkzar.py [--budgets \"name=FLASH/RAM ...\"] [--files-max N] "
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
            print("         %-16s %7d of %7d flash  %7d of %7d RAM"
                  % (name, len(data), bf, ram_image(data), br))
        else:
            print("         %-16s %7d" % (name, len(data)))

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
