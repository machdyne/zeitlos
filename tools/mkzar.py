#!/usr/bin/env python3
#
# Zeitlos
# Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
#
# Builds the flash-resident core app archive ("ZAR") programmed
# immediately after the kernel. See sw/os/zar.h for the layout and the
# reasoning; this is just the other end of it.
#
#   tools/mkzar.py output/apps.zar wm=sw/apps/wm/wm.bin ...
#
# The .bin files are stored VERBATIM -- they are already ZEXE files
# produced by each app's own Makefile, and re-encoding them here would
# mean two places that have to agree about the format instead of one.

import os
import struct
import sys

MAGIC = b"ZAR1"
NAME_MAX = 16
HEADER_SIZE = 16
ENTRY_SIZE = 24
MAX_ENTRIES = 32          # keep in sync with Z_ZAR_MAX_ENTRIES


def main(argv):
    if len(argv) < 3:
        print("usage: mkzar.py <output.zar> <name>=<file.bin> ...",
              file=sys.stderr)
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

        if len(name) > NAME_MAX:
            print("mkzar: name '%s' longer than %d chars"
                  % (name, NAME_MAX), file=sys.stderr)
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
        if data[:4] != b"ZEXE":
            print("mkzar: warning: %s has no ZEXE header (legacy binary)"
                  % path, file=sys.stderr)

        entries.append((name, data))

    # Payloads start after the header and the full entry table.
    offset = HEADER_SIZE + ENTRY_SIZE * len(entries)

    table = b""
    payload = b""
    for name, data in entries:
        table += name.encode("ascii").ljust(NAME_MAX, b"\0")
        table += struct.pack("<II", offset, len(data))
        payload += data
        offset += len(data)

    blob = MAGIC + struct.pack("<I", len(entries)) + b"\0" * 8
    blob += table + payload

    out_dir = os.path.dirname(out_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(blob)

    print("mkzar: %s: %d apps, %d bytes" % (out_path, len(entries),
                                            len(blob)))
    for name, data in entries:
        print("         %-16s %7d" % (name, len(data)))

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
