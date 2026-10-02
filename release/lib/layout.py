#
# Zeitlos release tooling -- flash layout, derived rather than restated.
#
# The flash map already exists in this tree. It is spelled out in
# several places, none of which can check each other, and each carries a
# KEEP IN SYNC comment saying so:
#
#   sw/common/zsoc.h Z_FLASH_* -- the Zeitlos region (docs/boot.md): its
#                    size, the default base, and the offsets inside it;
#                    Z_JUMP_*, Z_KV_SIZE
#   sw/bios/bios.c   MEM_ROM, MEM_ROM_SIZE, FLASH_BASE_DEFAULT, ROM_OS_SIZE
#   sw/os/zar.h      Z_ZAR_FLASH_OFFSET (inside the region)
#   Makefile         FLASH_BASE's default, JUMP_ADDR
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
#   2. `zrelease layout` becomes a standing check on those
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
    # Integer suffixes (0x1D0000u, 64UL) are C, not arithmetic: drop them.
    expr = re.sub(r"\b(0[xX][0-9a-fA-F]+|[0-9]+)[uUlL]+\b", r"\1", expr)
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
    ascending offset order, for the DEFAULT region base. A board whose
    region sits elsewhere (an Artix-7 board: docs/boot.md) gets its own
    list from regions_at(lay, base).
    """
    bios_c = os.path.join(root, "sw/bios/bios.c")
    zar_h = os.path.join(root, "sw/os/zar.h")
    zsoc_h = os.path.join(root, "sw/common/zsoc.h")
    boot_c = os.path.join(root, "sw/apps/zfpga/boot.c")
    makefile = os.path.join(root, "Makefile")

    bios = _scan_defines(bios_c, {
        "MEM_ROM", "MEM_ROM_SIZE", "FLASH_BASE_DEFAULT", "ROM_OS_SIZE",
    }, {})
    for name in ("MEM_ROM", "MEM_ROM_SIZE", "FLASH_BASE_DEFAULT", "ROM_OS_SIZE"):
        if name not in bios:
            raise LayoutError("%s: could not resolve %s" % (bios_c, name))

    zsoc_names = ("Z_FLASH_REGION_SIZE", "Z_FLASH_BASE_DEFAULT",
                  "Z_FLASH_KERNEL_OFF", "Z_FLASH_ZAR_OFF", "Z_FLASH_ZAR_END",
                  "Z_FLASH_TEST_OFF", "Z_FLASH_KV_OFF",
                  "Z_JUMP_FLASH_OFFSET", "Z_JUMP_REGION_SIZE", "Z_KV_SIZE")
    zs = _scan_defines(zsoc_h, set(zsoc_names), {})
    for name in zsoc_names:
        if name not in zs:
            raise LayoutError("%s: could not resolve %s" % (zsoc_h, name))

    zar = _scan_defines(zar_h, {
        "Z_ZAR_ROM_BASE", "Z_ZAR_FLASH_OFFSET", "Z_ZAR_MAX_ENTRIES",
    }, {})
    mk = _scan_makefile(makefile, {"JUMP_ADDR"})
    # FLASH_BASE: the top-level default, not a board block's override
    # (those are indented, inside the board if-chain).
    with open(makefile) as f:
        for line in f:
            m = re.match(r"FLASH_BASE\s*\?=\s*(\S+)", line)
            if m:
                mk["FLASH_BASE"] = m.group(1)
    # zfpga flash / run carry their own copies, being an app built for
    # the host too (sw/apps/zfpga/boot.c)
    zboot = _scan_defines(boot_c, {
        "ZFPGA_ZAR_OFFSET", "ZFPGA_JUMP_OFFSET", "ZFPGA_JUMP_END",
        "ZFPGA_GW_DFU", "ZFPGA_LOGO_OFFSET", "ZFPGA_KV_SIZE",
    }, {}) if os.path.exists(boot_c) else None

    rom_base = bios["MEM_ROM"]
    flash_size = bios["MEM_ROM_SIZE"]
    region = zs["Z_FLASH_REGION_SIZE"]
    base = zs["Z_FLASH_BASE_DEFAULT"]
    kernel_off = zs["Z_FLASH_KERNEL_OFF"]
    zar_off = zs["Z_FLASH_ZAR_OFF"]
    zar_end = zs["Z_FLASH_ZAR_END"]
    test_off = zs["Z_FLASH_TEST_OFF"]
    kv_off = zs["Z_FLASH_KV_OFF"]
    kv_size = zs["Z_KV_SIZE"]
    jump_abs = zs["Z_JUMP_FLASH_OFFSET"]
    jump_size = zs["Z_JUMP_REGION_SIZE"]

    # --- the cross-checks, i.e. the KEEP IN SYNC comments, enforced ---

    problems = []

    def agree(what, a, a_src, b, b_src):
        if a != b:
            problems.append("%s: %s says 0x%x, %s says 0x%x"
                            % (what, a_src, a, b_src, b))

    agree("default region base", base, "sw/common/zsoc.h Z_FLASH_BASE_DEFAULT",
          bios["FLASH_BASE_DEFAULT"], "sw/bios/bios.c FLASH_BASE_DEFAULT")
    agree("default region base", base, "sw/common/zsoc.h Z_FLASH_BASE_DEFAULT",
          int(mk.get("FLASH_BASE", "-1"), 0), "Makefile FLASH_BASE ?=")
    agree("kernel size", zar_off - kernel_off, "sw/common/zsoc.h (ZAR - kernel)",
          bios["ROM_OS_SIZE"], "sw/bios/bios.c ROM_OS_SIZE")
    agree("core apps offset", zar_off, "sw/common/zsoc.h Z_FLASH_ZAR_OFF",
          zar.get("Z_ZAR_FLASH_OFFSET", -1), "sw/os/zar.h Z_ZAR_FLASH_OFFSET")
    agree("flash window base", rom_base, "sw/bios/bios.c MEM_ROM",
          zar.get("Z_ZAR_ROM_BASE", -1), "sw/os/zar.h")
    agree("key/value store", kv_off + kv_size, "sw/common/zsoc.h (KV end)",
          region, "the end of the region")
    agree("jumploader offset", jump_abs, "sw/common/zsoc.h Z_JUMP_FLASH_OFFSET",
          int(mk.get("JUMP_ADDR", "-1"), 16), "Makefile JUMP_ADDR")
    agree("jumploader offset", jump_abs, "sw/common/zsoc.h Z_JUMP_FLASH_OFFSET",
          base + zar_end, "default base + Z_FLASH_ZAR_END")
    agree("jumploader region end", jump_abs + jump_size, "sw/common/zsoc.h",
          base + region, "the end of the default region")
    if zboot is not None:
        agree("core apps offset", base + zar_off, "default base + Z_FLASH_ZAR_OFF",
              zboot.get("ZFPGA_ZAR_OFFSET", -1), "sw/apps/zfpga/boot.c")
        agree("jumploader offset", jump_abs, "sw/common/zsoc.h Z_JUMP_FLASH_OFFSET",
              zboot.get("ZFPGA_JUMP_OFFSET", -1), "sw/apps/zfpga/boot.c")
        agree("jumploader region end", jump_abs + jump_size, "sw/common/zsoc.h",
              zboot.get("ZFPGA_JUMP_END", -1), "sw/apps/zfpga/boot.c ZFPGA_JUMP_END")
        agree("key/value store size", kv_size, "sw/common/zsoc.h Z_KV_SIZE",
              zboot.get("ZFPGA_KV_SIZE", -1), "sw/apps/zfpga/boot.c ZFPGA_KV_SIZE")
        # zfpga flash puts user gateware in the gateware region's tail,
        # up to ZFPGA_LOGO_OFFSET -- a name from when the logo ended it.
        # It must not reach into the Zeitlos region.
        if zboot.get("ZFPGA_LOGO_OFFSET", base + 1) > base:
            problems.append("sw/apps/zfpga/boot.c ZFPGA_LOGO_OFFSET 0x%x runs "
                            "into the Zeitlos region at 0x%x"
                            % (zboot.get("ZFPGA_LOGO_OFFSET", -1), base))
        spec_dir = os.path.join(root, "release/hw/boards")
        if os.path.isdir(spec_dir):
            for fn in sorted(os.listdir(spec_dir)):
                if not fn.endswith(".spec"):
                    continue
                for line in open(os.path.join(spec_dir, fn)):
                    m = re.match(r"\s*dfu_base\s*=\s*(0x[0-9a-fA-F]+|[0-9]+)\s*$", line)
                    if m:
                        agree("DFU gateware start", int(m.group(1), 0),
                              "release/hw/boards/" + fn + " dfu_base",
                              zboot.get("ZFPGA_GW_DFU", -1), "sw/apps/zfpga/boot.c ZFPGA_GW_DFU")

    # The order inside the region, as assertions.
    if kernel_off != 0:
        problems.append("the kernel is not at the start of the region")
    if not kernel_off < zar_off < zar_end <= test_off < kv_off:
        problems.append("region offsets out of order: kernel 0x%x, apps 0x%x, "
                        "apps end 0x%x, test 0x%x, kv 0x%x"
                        % (kernel_off, zar_off, zar_end, test_off, kv_off))
    if test_off + 0x1000 > kv_off:
        problems.append("the flash test sector overlaps the key/value store")
    if kv_size & 0xFFF or kv_size <= 0:
        problems.append("the key/value store size 0x%x is not a whole number "
                        "of 4 KB sectors" % kv_size)
    if jump_abs & 0xFFFF:
        problems.append("the jumploader offset 0x%x is not 64 KB aligned "
                        "(a boot address is addr[23:16])" % jump_abs)
    if base & 0xFFFF:
        problems.append("the default region base 0x%x is not 64 KB aligned" % base)

    if problems:
        raise LayoutError("flash layout constants disagree:\n  "
                          + "\n  ".join(problems))

    lay = {
        "rom_base": rom_base,
        "flash_size": flash_size,
        "region_size": region,
        "default_base": base,
        "offsets": {"kernel": kernel_off, "apps": zar_off, "apps_end": zar_end,
                    "test": test_off, "kv": kv_off, "kv_size": kv_size},
        "jump": (jump_abs, jump_size),
        "max_zar_entries": zar.get("Z_ZAR_MAX_ENTRIES"),
    }
    lay["regions"] = regions_at(lay, base)
    return lay


def regions_at(lay, base):
    """The flash map for a board whose Zeitlos region starts at `base`.

    The jumploader appears only at the default base, which is the only
    place it exists today (ECP5 boards that set JUMP; docs/zboot.md).
    """
    o = lay["offsets"]
    regions = [
        Region("gateware", "gateware", 0, base,
               "start of flash, up to the Zeitlos region"),
        Region("kernel", "kernel", base + o["kernel"], o["apps"] - o["kernel"],
               "sw/common/zsoc.h Z_FLASH_KERNEL_OFF"),
        Region("apps", "core apps (ZAR)", base + o["apps"], o["apps_end"] - o["apps"],
               "sw/common/zsoc.h Z_FLASH_ZAR_OFF"),
    ]
    if base == lay["default_base"]:
        jump_abs, _ = lay["jump"]
        regions.append(Region("jump", "jumploader", jump_abs,
                              base + o["kv"] - jump_abs,
                              "sw/common/zsoc.h Z_JUMP_FLASH_OFFSET, up to the store"))
    # Never part of an image: the running system writes it. An image
    # padded over it (a full JTAG image, a DFU image) erases it, which
    # docs/kvstore.md says is allowed.
    regions.append(Region("kv", "key/value store (written by the running system)",
                          base + o["kv"], o["kv_size"], "sw/common/zsoc.h Z_FLASH_KV_OFF"))
    return regions


def describe(lay):
    lines = ["  offset    limit      region",
             "  --------  ---------  ------------------"]
    for r in lay["regions"]:
        lines.append("  0x%06x  %9d  %s" % (r.offset, r.limit, r.name))
    lines.append("  0x%06x  %9s  end of flash"
                 % (lay["flash_size"], ""))
    return "\n".join(lines)
