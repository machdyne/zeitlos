#
# Zeitlos release tooling -- flash layout, derived rather than restated.
#
# The flash map already exists in this tree. It is spelled out in four
# separate places, none of which check each other, and all four carry a
# KEEP IN SYNC comment saying so:
#
#   sw/bios/bios.c   MEM_ROM, MEM_ROM_SIZE, ROM_OS_ADDR, ROM_OS_SIZE,
#                    ROM_LOGO_ADDR, ROM_LOGO_SIZE
#   sw/os/logo.h     Z_BOOT_LOGO_FLASH_OFFSET, Z_BOOT_LOGO_{W,H}
#   sw/os/zar.h      Z_ZAR_FLASH_OFFSET
#   Makefile         LOGO_FLASH_OFFSET_HEX / LOGO_FLASH_OFFSET_DEC
#
# A release image is a fifth copy of that map, and the one place where
# getting it wrong is most expensive: a bad `make flash_os` costs a
# reflash, a bad release image costs however many people downloaded it.
#
# So this module does not define the map. It READS all four sources,
# cross-checks them against each other, and fails loudly if they have
# drifted. Two useful consequences:
#
#   1. The release tool cannot disagree with the running system,
#      because it has no numbers of its own to disagree with.
#   2. `zrelease layout` becomes a standing check on those four
#      KEEP IN SYNC comments -- run it after touching any of them and
#      it will tell you if a copy was missed.
#

import os
import re


class LayoutError(Exception):
    pass


class Region:
    """One contiguous span of flash, and what goes in it."""

    def __init__(self, key, name, offset, limit, source):
        self.key = key
        self.name = name
        self.offset = offset
        self.limit = limit          # bytes available before the next region
        self.source = source        # where the number came from, for errors

    def __repr__(self):
        return "Region(%s @ 0x%06x, limit %d)" % (self.key, self.offset,
                                                  self.limit)


# ---------------------------------------------------------------------
# A very small C-preprocessor-expression evaluator.
#
# The constants we want are written as C macros, and several of them are
# arithmetic over other macros ("(MEM_ROM + (1024 * 1024 * 1))"). Rather
# than hardcode the answers -- which is exactly the duplication this
# module exists to avoid -- we substitute the macros we have already
# resolved and evaluate what is left.
#
# eval() is used on text taken from files in this repository, with a
# namespace containing nothing at all. The regex below refuses anything
# that is not digits, hex literals, operators and whitespace, so a macro
# body that is not plain arithmetic is rejected rather than executed.
# ---------------------------------------------------------------------

_SAFE_EXPR = re.compile(r"^[0-9a-fA-FxX\s()+\-*/<>|&]+$")


def _eval_c_expr(expr, known):
    expr = expr.strip()
    # Substitute already-resolved macro names, longest first so that
    # e.g. MEM_ROM_SIZE is not clobbered by MEM_ROM.
    for name in sorted(known, key=len, reverse=True):
        expr = re.sub(r"\b%s\b" % re.escape(name), "(%d)" % known[name], expr)
    if not _SAFE_EXPR.match(expr):
        raise LayoutError("cannot evaluate C expression %r "
                          "(unresolved macro, or not plain arithmetic)" % expr)
    try:
        return int(eval(expr, {"__builtins__": {}}, {}))
    except Exception as e:
        raise LayoutError("cannot evaluate C expression %r: %s" % (expr, e))


def _scan_defines(path, wanted, known):
    """Pull `#define NAME <expr>` out of a C file, in file order.

    In file order matters: ROM_OS_ADDR is defined in terms of MEM_ROM,
    which appears above it, so a single forward pass resolves everything
    without needing a dependency graph.
    """
    out = {}
    if not os.path.exists(path):
        raise LayoutError("%s: not found" % path)
    with open(path) as f:
        for line in f:
            m = re.match(r"\s*#define\s+(\w+)\s+(.+?)\s*(?://.*)?$", line)
            if not m:
                continue
            name, body = m.group(1), m.group(2)
            if name not in wanted:
                continue
            # Strip a trailing block comment if there is one.
            body = re.sub(r"/\*.*?\*/", "", body).strip()
            try:
                val = _eval_c_expr(body, dict(known, **out))
            except LayoutError:
                # Not every wanted macro is arithmetic (some are chars).
                # Skip quietly; the caller checks for what it needs.
                continue
            out[name] = val
    return out


def _scan_makefile(path, wanted):
    out = {}
    with open(path) as f:
        for line in f:
            m = re.match(r"\s*(\w+)\s*\??=\s*(\S+)", line)
            if m and m.group(1) in wanted:
                out[m.group(1)] = m.group(2)
    return out


def load(root):
    """Read the flash map out of the tree at `root`, cross-checking it.

    Returns a dict with the resolved constants plus a `regions` list in
    ascending offset order.
    """
    bios_c = os.path.join(root, "sw/bios/bios.c")
    logo_h = os.path.join(root, "sw/os/logo.h")
    zar_h = os.path.join(root, "sw/os/zar.h")
    makefile = os.path.join(root, "Makefile")

    bios = _scan_defines(bios_c, {
        "MEM_ROM", "MEM_ROM_SIZE", "ROM_OS_ADDR", "ROM_OS_SIZE",
        "ROM_LOGO_ADDR", "ROM_LOGO_SIZE",
    }, {})

    for name in ("MEM_ROM", "MEM_ROM_SIZE", "ROM_OS_ADDR", "ROM_OS_SIZE",
                 "ROM_LOGO_ADDR", "ROM_LOGO_SIZE"):
        if name not in bios:
            raise LayoutError("%s: could not resolve %s" % (bios_c, name))

    logo = _scan_defines(logo_h, {
        "Z_BOOT_LOGO_W", "Z_BOOT_LOGO_H", "Z_BOOT_LOGO_BYTES",
        "Z_BOOT_LOGO_ROM_BASE", "Z_BOOT_LOGO_FLASH_OFFSET",
    }, {})
    zar = _scan_defines(zar_h, {
        "Z_ZAR_ROM_BASE", "Z_ZAR_FLASH_OFFSET", "Z_ZAR_MAX_ENTRIES",
    }, {})
    mk = _scan_makefile(makefile, {
        "LOGO_FLASH_OFFSET_HEX", "LOGO_FLASH_OFFSET_DEC",
    })

    rom_base = bios["MEM_ROM"]
    flash_size = bios["MEM_ROM_SIZE"]

    logo_off = bios["ROM_LOGO_ADDR"] - rom_base
    logo_size = bios["ROM_LOGO_SIZE"]
    os_off = bios["ROM_OS_ADDR"] - rom_base
    os_size = bios["ROM_OS_SIZE"]
    zar_off = zar["Z_ZAR_FLASH_OFFSET"]

    # --- the cross-checks, i.e. the KEEP IN SYNC comments, enforced ---

    problems = []

    def agree(what, a, a_src, b, b_src):
        if a != b:
            problems.append("%s: %s says 0x%x, %s says 0x%x"
                            % (what, a_src, a, b_src, b))

    agree("logo flash offset", logo_off, "sw/bios/bios.c ROM_LOGO_ADDR",
          logo["Z_BOOT_LOGO_FLASH_OFFSET"], "sw/os/logo.h")
    agree("logo flash offset", logo_off, "sw/bios/bios.c ROM_LOGO_ADDR",
          int(mk.get("LOGO_FLASH_OFFSET_HEX", "-1"), 16),
          "Makefile LOGO_FLASH_OFFSET_HEX")
    agree("logo flash offset", logo_off, "sw/bios/bios.c ROM_LOGO_ADDR",
          int(mk.get("LOGO_FLASH_OFFSET_DEC", "-1")),
          "Makefile LOGO_FLASH_OFFSET_DEC")
    agree("logo size", logo_size, "sw/bios/bios.c ROM_LOGO_SIZE",
          logo.get("Z_BOOT_LOGO_BYTES", -1), "sw/os/logo.h")
    agree("flash window base", rom_base, "sw/bios/bios.c MEM_ROM",
          logo.get("Z_BOOT_LOGO_ROM_BASE", -1), "sw/os/logo.h")
    agree("flash window base", rom_base, "sw/bios/bios.c MEM_ROM",
          zar.get("Z_ZAR_ROM_BASE", -1), "sw/os/zar.h")

    # The ZAR sits immediately above the kernel's region. This is stated
    # as prose in zar.h ("1MB + 256KB") and as two independent numbers;
    # here it becomes an assertion.
    if zar_off != os_off + os_size:
        problems.append("core apps offset: sw/os/zar.h says 0x%x, but the "
                        "kernel region (sw/bios/bios.c) ends at 0x%x"
                        % (zar_off, os_off + os_size))

    if logo_off + logo_size > os_off:
        problems.append("logo (0x%x + %d) overlaps the kernel at 0x%x"
                        % (logo_off, logo_size, os_off))
    if zar_off >= flash_size:
        problems.append("core apps offset 0x%x is past the end of flash "
                        "(0x%x)" % (zar_off, flash_size))

    if problems:
        raise LayoutError("flash layout constants disagree:\n  "
                          + "\n  ".join(problems))

    regions = [
        Region("gateware", "gateware", 0, logo_off,
               "start of flash, up to the logo"),
        Region("logo", "boot logo", logo_off, os_off - logo_off,
               "sw/os/logo.h Z_BOOT_LOGO_FLASH_OFFSET"),
        Region("kernel", "kernel", os_off, os_size,
               "sw/bios/bios.c ROM_OS_ADDR"),
        Region("apps", "core apps (ZAR)", zar_off, flash_size - zar_off,
               "sw/os/zar.h Z_ZAR_FLASH_OFFSET"),
    ]

    return {
        "rom_base": rom_base,
        "flash_size": flash_size,
        "logo_bytes": logo_size,
        "max_zar_entries": zar.get("Z_ZAR_MAX_ENTRIES"),
        "regions": regions,
    }


def describe(lay):
    lines = ["  offset    limit      region",
             "  --------  ---------  ------------------"]
    for r in lay["regions"]:
        lines.append("  0x%06x  %9d  %s" % (r.offset, r.limit, r.name))
    lines.append("  0x%06x  %9s  end of flash"
                 % (lay["flash_size"], ""))
    return "\n".join(lines)
