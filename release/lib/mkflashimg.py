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


# -- DFU images ---------------------------------------------------------

def build_dfu(layout, parts, base):
    """Assemble the same regions for a DFU user partition at `base`.

    Returns (bytes, [(region, part, used, limit)]) like build().

    A DFU-flashed board runs the bootloader from offset 0 and the user
    image from USERPART_START, so the GATEWARE has to sit at `base`
    rather than at 0. Everything above it does not move: the boot
    splash, the kernel and the ZAR keep the absolute flash offsets they
    have in a JTAG image, because the BIOS and zar.h read them from
    fixed addresses in the memory-mapped flash window and neither knows
    or cares how the bytes got there.

    That is the whole reason the partition split is 256K rather than
    512K. At 256K the gateware still ends before the splash at
    0x0F0000 and nothing else has to move; at 512K it would not, and
    the layout would have to be rearranged for DFU boards only.

    The returned image starts AT `base` -- it is what
    `dfu-util -a 0 -D` writes -- so a caller must not prepend anything.
    """
    regions = {r.key: r for r in layout["regions"]}
    flash_size = layout["flash_size"]

    if base <= 0 or base >= flash_size:
        raise ImageError("dfu_base 0x%06x is outside the flash" % base)

    gw = regions.get("gateware")
    if gw is None:
        raise ImageError("no gateware region in the layout")
    if gw.offset != 0:
        raise ImageError(
            "the gateware region is at 0x%06x, not 0. build_dfu() assumes "
            "the gateware starts the image and everything else keeps its "
            "absolute offset; that is no longer true." % gw.offset)

    # The gateware's room shrinks by exactly what the bootloader took,
    # because its start moved and the region above it did not.
    next_off = min((r.offset for r in layout["regions"] if r.offset > 0),
                   default=flash_size)
    gw_limit = next_off - base

    loaded = []
    for key, path in parts.items():
        if key not in regions:
            raise ImageError("no flash region called %r" % key)
        if not os.path.exists(path):
            raise ImageError("%s: not found (build it first?)" % path)
        with open(path, "rb") as f:
            loaded.append(Part(key, path, f.read()))

    rows = []
    for p in loaded:
        r = regions[p.key]
        if p.key == "gateware":
            off, limit, where = base, gw_limit, "DFU user partition"
        else:
            off, limit, where = r.offset, r.limit, r.source
        if off < base:
            raise ImageError(
                "the %s region is at 0x%06x, below the DFU user partition "
                "at 0x%06x -- it cannot be written over DFU at all."
                % (r.name, off, base))
        if len(p.data) > limit:
            raise ImageError(
                "%s is %d bytes but the %s region holds %d in a DFU image "
                "(offset 0x%06x, %s).\n"
                "  Over by %d bytes. The gateware has less room here than "
                "in a JTAG image -- %d bytes rather than %d -- because the "
                "bootloader occupies the first 0x%06x and the region above "
                "the gateware did not move."
                % (os.path.basename(p.path), len(p.data), r.name, limit,
                   off, where, len(p.data) - limit, gw_limit, gw.limit, base))
        rows.append((r, p, len(p.data), limit))

    img = bytearray([FILL]) * (flash_size - base)
    for p in loaded:
        off = base if p.key == "gateware" else regions[p.key].offset
        img[off - base:off - base + len(p.data)] = p.data

    rows.sort(key=lambda x: x[0].offset)
    return bytes(img), rows
