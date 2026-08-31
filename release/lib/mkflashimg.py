#
# Zeitlos release tooling -- the single flashable image.
#
# `make flash` writes four separate things to four separate offsets.
# This assembles those same four things, at those same four offsets,
# into one file -- so that a board owner runs one command instead of
# four and does not have to get any offsets right by hand.
#
# It is not a new format. It is the flash map from release/lib/layout.py
# materialised, which is why this file has no offsets of its own.
#
# TWO THINGS THAT MATTER AND ARE EASY TO GET WRONG:
#
# 1. THE FILL BYTE IS 0xFF, NOT ZERO. Erased NOR flash reads as 0xFF,
#    and two pieces of this system check for exactly that:
#    sw/os/logo.c skips drawing if it finds erased flash where the
#    splash should be, and sw/os/zar.c checks for the "ZAR1" magic
#    before trusting the archive. Filling the gaps with 0x00 would
#    write real zeros over regions that are supposed to read as erased,
#    which turns "no logo programmed" into "a logo made of black
#    pixels" and defeats both checks.
#
# 2. THE IMAGE IS TRIMMED, NOT PADDED TO 2MB. The last byte written is
#    the end of the ZAR, which lands around 1.5MB in practice. The
#    remaining ~0.5MB is erased flash either way, so padding it would
#    cost erase and programming time over JTAG for no change in the
#    result. Pass full=True for a literal 2MB image if a particular
#    flashing tool ever wants one.
#

import hashlib
import os


class ImageError(Exception):
    pass


FILL = 0xFF


class Part:
    def __init__(self, key, path, data):
        self.key = key
        self.path = path
        self.data = data


def build(layout, parts, full=False):
    """Assemble an image from {region_key: path}.

    Returns (bytes, [(region, part, used, limit)]).
    """
    regions = {r.key: r for r in layout["regions"]}

    loaded = []
    for key, path in parts.items():
        if key not in regions:
            raise ImageError("no flash region called %r" % key)
        if not os.path.exists(path):
            raise ImageError("%s: not found (build it first?)" % path)
        with open(path, "rb") as f:
            loaded.append(Part(key, path, f.read()))

    # -- region checks -------------------------------------------------
    #
    # Overrunning a region does not fail at flash time and does not fail
    # at boot in any way that names the cause -- an oversized gateware
    # simply eats the boot logo, and an oversized kernel eats the start
    # of the ZAR. Both then present as "that feature stopped working".
    # Fail here instead, where the numbers are in hand.
    rows = []
    for p in loaded:
        r = regions[p.key]
        if len(p.data) > r.limit:
            raise ImageError(
                "%s is %d bytes but the %s region holds %d "
                "(offset 0x%06x, %s).\n"
                "  Over by %d bytes. Either shrink it, or move the region "
                "-- and if you move it, move it in ALL of the places "
                "release/lib/layout.py reads it from, or that module will "
                "refuse the next build."
                % (os.path.basename(p.path), len(p.data), r.name, r.limit,
                   r.offset, r.source, len(p.data) - r.limit))
        rows.append((r, p, len(p.data), r.limit))

    end = max(regions[p.key].offset + len(p.data) for p in loaded)
    size = layout["flash_size"] if full else end
    if size > layout["flash_size"]:
        raise ImageError("image would be %d bytes, flash is %d"
                         % (size, layout["flash_size"]))

    img = bytearray([FILL]) * size
    for p in loaded:
        off = regions[p.key].offset
        img[off:off + len(p.data)] = p.data

    rows.sort(key=lambda x: x[0].offset)
    return bytes(img), rows


def describe(rows, size, flash_size):
    out = ["  offset    size       limit      used   contents",
           "  --------  ---------  ---------  -----  ------------------"]
    for r, p, used, limit in rows:
        out.append("  0x%06x  %9d  %9d  %4d%%  %s"
                   % (r.offset, used, limit, (used * 100) // limit,
                      os.path.basename(p.path)))
    out.append("")
    out.append("  image %d bytes (%.2f MB); flash is %d bytes -- the "
               "remaining %d bytes"
               % (size, size / 1048576.0, flash_size, flash_size - size))
    out.append("  are left erased rather than written.")
    return "\n".join(out)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()
