#
# Zeitlos release tooling -- release notes and the manifest.
#
# Two outputs from the same data:
#
#   NOTES.md        what a human reads on the release page. The most
#                   important thing on it is the flash command, per
#                   target, spelled out -- the whole point of shipping
#                   a combined image is that getting started is one
#                   command, and a release page that makes somebody
#                   derive that command has given the advantage back.
#
#   MANIFEST.json   what a machine reads: every define that went into
#                   every bitstream, the achieved Fmax, the utilisation,
#                   the region sizes, the source commit. This is the
#                   thing that answers "what exactly did v0.0.2 ship?"
#                   two years from now without a git archaeology
#                   session, and it is why the build records timing
#                   rather than just checking it.
#

import json


def manifest(version, commit, dirty, targets, sdcard, layout, ark_commit):
    return {
        "version": version,
        "commit": commit,
        "tree_dirty": dirty,
        "ark_commit": ark_commit,
        "flash_layout": [
            {"region": r.key, "name": r.name, "offset": r.offset,
             "limit": r.limit, "source": r.source}
            for r in layout["regions"]
        ],
        "flash_size": layout["flash_size"],
        "sdcard": sdcard,
        "targets": targets,
    }


def _clock(name):
    """nextpnr reports '$glbnet$clk_48'; humans want 'clk_48'."""
    return name.rsplit("$", 1)[-1]


def _target_table(targets):
    rows = ["| Target | Hardware | Networking | Image |",
            "| --- | --- | --- | --- |"]
    for t in targets:
        net = t.get("nic") or "none"
        rows.append("| `%s` | %s | %s | `%s` |"
                    % (t["target"], t["description"], net,
                       t["artifacts"].get("flash_image", "-")))
    return "\n".join(rows)


def notes(version, commit, targets, sdcard, layout, prev_version=None):
    L = {r.key: r for r in layout["regions"]}

    out = []
    out.append("Zeitlos %s -- a work-in-progress SOC and OS for FPGA "
               "computers." % version)
    out.append("")
    out.append("Each target below ships a single flashable image "
               "containing the gateware, boot logo, kernel and core apps "
               "at their fixed offsets. Flash one file and the board boots "
               "to a desktop; an SD card is optional.")
    out.append("")
    out.append("## Targets")
    out.append("")
    out.append(_target_table(targets))
    out.append("")

    out.append("## Flashing")
    out.append("")
    for t in targets:
        img = t["artifacts"].get("flash_image")
        if not img or not t.get("flash_cmd"):
            continue
        out.append("**%s** -- %s" % (t["target"], t["description"]))
        for n in t.get("notes", []):
            out.append("")
            out.append("%s" % n)
        out.append("")
        out.append("```")
        out.append(t["flash_cmd"].format(file=img))
        out.append("```")
        out.append("")

    out.append("Adjust `-c dirtyJtag` to match your programming cable. "
               "This writes the whole image starting at flash offset 0, "
               "which is what `make flash` does today -- on a board that "
               "shipped with a DFU bootloader in that space, this replaces "
               "it.")
    out.append("")

    if sdcard:
        out.append("## SD card")
        out.append("")
        out.append("`%s` is a 64MB FAT32 image with the non-core apps, the "
                   "documentation and the ARK scroll. It is the same for "
                   "every target." % sdcard["file"])
        out.append("")
        out.append("```")
        out.append("gzip -dc %s | sudo dd of=/dev/sdX bs=4M "
                   "status=progress conv=fsync" % sdcard["file"])
        out.append("```")
        out.append("")
        out.append("Applications live in `apps/` on the card, alongside "
                   "`docs/`, `ark/` and `user/`. You never need to type "
                   "that path: `run term` searches the card root, then "
                   "`apps/`, then the flash archive.")
        out.append("")
        out.append("The core apps (`wm`, `repl`, `term`, and `net` where "
                   "the hardware has a NIC) are not on the card -- they are "
                   "in flash. A copy at the card ROOT takes precedence over "
                   "the flash copy, so dropping one there is how you "
                   "hot-swap a single app during development.")
        out.append("")

    out.append("## Flash layout")
    out.append("")
    out.append("| Offset | Size | Contents |")
    out.append("| --- | --- | --- |")
    for key in ("gateware", "logo", "kernel", "apps"):
        r = L[key]
        out.append("| `0x%06x` | %d KB | %s |"
                   % (r.offset, r.limit // 1024, r.name))
    out.append("")
    out.append("Images are trimmed to the end of the core app archive "
               "rather than padded to the full %d KB, so the tail of flash "
               "is left erased instead of being written."
               % (layout["flash_size"] // 1024))
    out.append("")

    out.append("`README.txt` in the assets below describes every file, "
               "offline.")
    out.append("")
    out.append("## Build")
    out.append("")
    out.append("Built from `%s`." % commit[:12])
    out.append("")
    out.append("| Target | Fmax | Utilisation |")
    out.append("| --- | --- | --- |")
    for t in targets:
        tm = t.get("timing") or {}
        fmax = "; ".join("%s %.1f MHz" % (_clock(c["clock"]),
                                          c["achieved_mhz"])
                         for c in tm.get("clocks", [])) or "-"
        u = tm.get("utilisation", {})
        key = "TRELLIS_COMB" if "TRELLIS_COMB" in u else (
            sorted(u)[0] if u else None)
        util = ("%s %d%%" % (key, u[key]["percent"])) if key else "-"
        out.append("| `%s` | %s | %s |" % (t["target"], fmax, util))
    out.append("")

    return "\n".join(out)


def asset_readme(version, commit, targets, sdcard, layout):
    """A plain-text description of every file in the release.

    Ships as an asset rather than living only in the release body,
    because the release body is a web page and these files end up in a
    downloads folder. Somebody coming back to a directory of .img files
    a month later needs to be able to tell which board each one is for
    without a browser.

    This is also the only place the full board/PMOD list belongs. The
    project README deliberately carries one example and a pointer here,
    so that adding a target does not mean editing the front page.
    """
    W = 74
    out = []

    def rule(ch="="):
        out.append(ch * W)

    rule()
    out.append("Zeitlos %s" % version)
    rule()
    out.append("")
    out.append("A work-in-progress SOC and OS for FPGA computers.")
    out.append("https://github.com/machdyne/zeitlos")
    out.append("")
    out.append("Built from commit %s" % commit)
    out.append("")

    out.append("-" * W)
    out.append("WHICH FILE DO I WANT?")
    out.append("-" * W)
    out.append("")
    out.append("One image per hardware configuration. It contains the")
    out.append("gateware, boot splash, kernel and core apps, so flashing it")
    out.append("is a single command and the board boots to a desktop with")
    out.append("no sdcard.")
    out.append("")

    for t in targets:
        img = t["artifacts"].get("flash_image")
        if not img:
            continue
        out.append("  %s" % img)
        out.append("      %s" % t["description"])
        out.append("      Networking: %s" % (
            "ENC28J60 (SPI)" if t.get("nic") == "ENC28J60" else
            "RMII ethernet MAC" if t.get("nic") == "RMII" else
            "none in this build"))
        for n in t.get("notes", []):
            out.append("      %s" % n)
        if t.get("flash_cmd"):
            out.append("")
            out.append("      $ %s" % t["flash_cmd"].format(file=img))
        out.append("")

    if sdcard:
        out.append("  %s" % sdcard["file"])
        out.append("      Optional sdcard image (64MB, FAT32). The same for")
        out.append("      every board. Holds the additional apps (in")
        out.append("      apps/), the documentation (docs/) and the ARK")
        out.append("      scroll (ark/).")
        out.append("")
        out.append("      $ gzip -dc %s \\" % sdcard["file"])
        out.append("          | sudo dd of=/dev/sdX bs=4M status=progress "
                   "conv=fsync")
        out.append("")

    out.append("-" * W)
    out.append("THE REST OF THE FILES")
    out.append("-" * W)
    out.append("")
    out.append("Only needed for partial reflashes during development --")
    out.append("everything here is already inside the images above.")
    out.append("Rewriting one piece over JTAG is much faster than")
    out.append("rewriting the whole image.")
    out.append("")
    L = {r.key: r for r in layout["regions"]}
    out.append("  zeitlos-<board>-gateware.bit   bitstream, flash 0x%06x"
               % L["gateware"].offset)
    out.append("  zeitlos-kernel.bin             kernel,    flash 0x%06x"
               % L["kernel"].offset)
    out.append("  zeitlos-apps.zar               core apps, flash 0x%06x"
               % L["apps"].offset)
    out.append("  zeitlos-logo.bin               splash,    flash 0x%06x"
               % L["logo"].offset)
    out.append("")
    out.append("  MANIFEST.json   every RTL define, achieved Fmax and")
    out.append("                  utilisation for each build")
    out.append("  SHA256SUMS      checksums for everything above")
    out.append("")
    out.append("Only the gateware differs between boards. The kernel,")
    out.append("the core apps and the splash are identical everywhere --")
    out.append("the `net` app carries both NIC drivers and picks one at")
    out.append("startup from the SOC feature register.")
    out.append("")

    out.append("-" * W)
    out.append("FLASH LAYOUT")
    out.append("-" * W)
    out.append("")
    for key in ("gateware", "logo", "kernel", "apps"):
        r = L[key]
        out.append("  0x%06x  %6d KB  %s"
                   % (r.offset, r.limit // 1024, r.name))
    out.append("  0x%06x            end of flash" % layout["flash_size"])
    out.append("")
    out.append("Images stop at the end of the core app archive rather than")
    out.append("padding to the full %d KB, so the tail of flash is left"
               % (layout["flash_size"] // 1024))
    out.append("erased instead of being written.")
    out.append("")

    out.append("-" * W)
    out.append("NOTES")
    out.append("-" * W)
    out.append("")
    out.append("Adjust the -c option to match your programming cable.")
    out.append("")
    out.append("Flashing writes from offset 0, which is what `make flash`")
    out.append("does -- on a board that shipped with a DFU bootloader in")
    out.append("that space, this replaces it.")
    out.append("")
    out.append("Applications live in apps/ on the card, but you never")
    out.append("need to type that path: `run term` searches the card root,")
    out.append("then apps/, then the flash archive.")
    out.append("")
    out.append("The core apps are in flash, not on the sdcard. A copy at")
    out.append("the card ROOT takes precedence over the flash copy, which")
    out.append("is how a single app is hot-swapped during development.")
    out.append("")
    out.append("If your board is not listed, please open an issue.")
    out.append("")

    return "\n".join(out)


def write(path, obj):
    with open(path, "w") as f:
        if path.endswith(".json"):
            json.dump(obj, f, indent=2)
            f.write("\n")
        else:
            f.write(obj)
