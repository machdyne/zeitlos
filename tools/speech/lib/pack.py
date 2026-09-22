"""
Zeitlos
Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.

The ZSPK container: what tools/speech writes and sw/apps/tts reads.

    offset size  field
    0      4     magic "ZSPK"
    4      2     version (1)
    6      2     section count
    8      4     total file size
    12     4     crc32 of everything after this header
    16     ...   section table, 16 bytes each:
                   0  8  name, NUL-padded
                   8  4  offset from the start of the file
                   12 4  length
    ...          the sections themselves

Sections are found by name, so a reader takes what it knows and
ignores the rest: an older `tts` still works with a newer pack, and a
pack missing a section it has no data for is normal rather than an
error. Sections defined so far:

    MANIFEST  text: what this was built from, and under what licence
    PHONES    text: the phoneme names, in the order the ids use
    LEXIDX    the lexicon's block index (fixed 16-byte records)
    LEXDAT    the lexicon's blocks

PHONES exists so the byte-level ids in LEXDAT mean something to a
reader that has its own table: `tts` maps the names to its own
phonemes when it opens the pack, and refuses a pack naming a phoneme
it does not have, rather than speaking gibberish.
"""

import struct
import zlib

MAGIC = b"ZSPK"
VERSION = 1
HDR = 16
SECT = 16


def write(path, sections):
    """sections: [(name, bytes)] in the order they should appear."""

    table = bytearray()
    blobs = bytearray()
    off = HDR + SECT * len(sections)

    for name, blob in sections:
        n = name.encode("ascii")
        assert len(n) <= 8, name
        table += n + b"\0" * (8 - len(n)) + struct.pack("<II", off, len(blob))
        blobs += blob
        off += len(blob)

    body = bytes(table) + bytes(blobs)
    head = MAGIC + struct.pack("<HHII", VERSION, len(sections), HDR + len(body),
                               zlib.crc32(body) & 0xffffffff)

    with open(path, "wb") as f:
        f.write(head)
        f.write(body)

    return HDR + len(body)


def read(path):
    """The inverse, for tests and for `speech show`."""

    with open(path, "rb") as f:
        raw = f.read()

    if raw[:4] != MAGIC:
        raise ValueError("not a ZSPK file")
    version, nsect, size, crc = struct.unpack("<HHII", raw[4:HDR])
    if version != VERSION:
        raise ValueError("pack version %d, expected %d" % (version, VERSION))
    if size != len(raw):
        raise ValueError("size field %d, file %d" % (size, len(raw)))
    if zlib.crc32(raw[HDR:]) & 0xffffffff != crc:
        raise ValueError("crc mismatch")

    out = {}
    for i in range(nsect):
        rec = raw[HDR + i * SECT:HDR + (i + 1) * SECT]
        name = rec[:8].rstrip(b"\0").decode("ascii")
        off, length = struct.unpack("<II", rec[8:])
        out[name] = raw[off:off + length]
    return out
