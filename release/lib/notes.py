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
import textwrap


# -- card images --
#
# A release ships one card per corpus size (mkfatimg.VARIANTS). These
# accept either shape -- a list of card records, or the single record
# releases made before variants -- so an older MANIFEST.json merged
# into a newer build still describes itself.

PACK_DESC = {
    "zdocs": "the Zeitlos documentation",
    "arklite": "Ark Lite (the Codex, selected books, the Scroll)",
    "arkmed": "Ark Medium (Wikipedia's 10K vital articles, the Gutenberg "
              "CD-ROM, MedlinePlus, the CIA Factbook, and all of Ark Lite)",
}


def cards(sdcard):
    if not sdcard:
        return []
    if isinstance(sdcard, dict):
        return [sdcard]
    return list(sdcard)


def card_size(c):
    """Image size in MB and the smallest card it fits, in words."""
    b = c.get("image_bytes", 64 * 1024 * 1024)
    mb = b // (1024 * 1024)
    if c.get("needs_8gb"):
        fits = "needs an 8 GB card"
    elif b > 1024 * 1024 * 1024:
        fits = "fits a 4 GB card"
    else:
        fits = "fits any card"
    return mb, fits


def card_contents(c):
    packs = c.get("packs") or ["zdocs", "arklite"]
    return "; ".join(PACK_DESC.get(p, p) for p in packs)


def manifest(version, commit, dirty, targets, sdcard, layout, ark_commit):
    cs = cards(sdcard)
    base = next((c for c in cs if c.get("file") == "zeitlos.img.gz"),
                cs[0] if cs else None)
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
        "sdcards": cs,
        # The single-card key older tooling reads: the base image.
        "sdcard": base,
        "targets": targets,
    }


# Where the DFU upgrade guide lives.
#
# The REPOSITORY, not a release asset and not the sdcard. Somebody who
# needs this has a board that cannot run Zeitlos yet -- telling them to
# read it from the sdcard is telling them to read it on the machine
# they are trying to get working, and telling them to find it among
# the release assets assumes they have already downloaded the right
# ones. A URL works from the phone in their other hand.
def _at_offset(flash_cmd, offset, filename):
    """A board's flash command, aimed at `offset` instead of 0.

    Every board's flash_cmd writes the assembled image at offset 0, so
    it carries a literal `-o 0`. Rewriting that one token is what turns
    the whole-image command into a per-region one, and it keeps the
    programmer, the cable and the flags the board actually needs --
    which a hardcoded example in a document could not.

    Falls back to leaving the command alone if the `-o 0` is not there,
    so a board whose flash_cmd has some other shape prints something
    harmless rather than something wrong.
    """
    if "-o 0 " not in flash_cmd:
        return None
    return flash_cmd.replace("-o 0 ", "-o 0x%06x " % offset, 1) \
                    .format(file=filename)


DFU_DOC_URL = ("https://github.com/machdyne/zeitlos/blob/main/"
               "docs/dfu_upgrade.md")


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
    out.append("`zeitlos-<target>-dfu.bin`, where a target provides one, is "
               "the same system packaged for `dfu-util -a 0 -D`.")
    out.append("")
    out.append("**Flashing a Lakritz or Obst over USB?** They ship a DFU "
               "bootloader whose user partition is too small for Zeitlos, "
               "so the `-dfu.bin` will not fit until you update it. "
               "[Read this first](%s)." % DFU_DOC_URL)
    out.append("")

    cs = cards(sdcard)
    if cs:
        out.append("## SD card")
        out.append("")
        out.append("Optional, and the same for every target. Each image has "
                   "the non-core apps, the documentation and the ARK scroll; "
                   "they differ only in how much reference material `ask` "
                   "can search.")
        out.append("")
        out.append("| image | size | `ask` can search |")
        out.append("|---|---|---|")
        for c in cs:
            mb, fits = card_size(c)
            out.append("| `%s` | %d MB, %s | %s |"
                       % (c["file"], mb, fits, card_contents(c)))
        out.append("")
        out.append("```")
        out.append("gzip -dc %s | sudo dd of=/dev/sdX bs=4M "
                   "status=progress conv=fsync" % cs[0]["file"])
        out.append("```")
        out.append("")
        out.append("Applications live in `apps/` on the card, alongside "
                   "`docs/`, `ark/` and `user/`. You never need to type "
                   "that path: `run term` searches the card root, then "
                   "`apps/`, then the flash archive.")
        out.append("")
        out.append("The core apps (`wm`, `term`, and `net` where "
                   "the hardware has a NIC) are not on the card -- they are "
                   "in flash. A copy at the card ROOT takes precedence over "
                   "the flash copy, so dropping one there is how you "
                   "hot-swap a single app during development.")
        out.append("")
        out.append("The two shells, `repl` and `posix`, ARE on the card, and "
                   "boot starts both when a card is present. Without a card "
                   "`term` opens with both shell buttons disabled; OPEN "
                   "(F11) still reaches telnet, ssh and serial.")
        out.append("")

    out.append("## Flash layout")
    out.append("")
    out.append("| Offset | Size | Contents |")
    out.append("| --- | --- | --- |")
    for r in layout["regions"]:
        out.append("| `0x%06x` | %d KB | %s |"
                   % (r.offset, r.limit // 1024, r.name))
    out.append("")
    out.append("Images are trimmed to the end of their last piece -- the "
               "jumploader, on boards that have one, otherwise the core app "
               "archive -- rather than padded to the full %d KB, so the "
               "rest of flash is left erased instead of being written."
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

    for c in cards(sdcard):
        mb, fits = card_size(c)
        out.append("  %s" % c["file"])
        out.append("      Optional sdcard image (%d MB, FAT32, %s). The same"
                   % (mb, fits))
        out.append("      for every board: the additional apps (apps/),")
        out.append("      the documentation (docs/), the ARK scroll (ark/),")
        for line in textwrap.wrap("and for `ask`: %s." % card_contents(c),
                                  W - 6):
            out.append("      " + line)
        out.append("")
        out.append("      $ gzip -dc %s \\" % c["file"])
        out.append("          | sudo dd of=/dev/sdX bs=4M status=progress "
                   "conv=fsync")
        out.append("")

    out.append("-" * W)
    out.append("THE REST OF THE FILES")
    out.append("-" * W)
    out.append("")
    out.append("The same system as the .img above, in pieces.")
    out.append("Everything here is already inside that image.")
    out.append("")
    out.append("Flash them separately if the whole-image write gives")
    out.append("you trouble, or when you have changed one piece and do")
    out.append("not want to rewrite the other 1.5MB over JTAG.")
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
    if "jump" in L:
        out.append("  zeitlos-<board>-jump.bin       jumploader, flash 0x%06x"
                   % L["jump"].offset)
        out.append("                                 (boards that have one; a .bin,")
        out.append("                                 not a .bit, so it is written")
        out.append("                                 whole, header included)")
    out.append("")
    out.append("THE OFFSETS ARE NOT OPTIONAL. The BIOS reads the splash")
    out.append("and the kernel from fixed addresses, and the OS reads the")
    out.append("archive from one -- a piece written to the wrong offset")
    out.append("gives a board that configures and then hangs.")
    out.append("")

    for t in targets:
        fc = t.get("flash_cmd")
        gw = (t.get("artifacts") or {}).get("gateware")
        if not fc or not gw:
            continue
        lines = []
        pieces = [("gateware", gw),
                  ("logo", "zeitlos-logo.bin"),
                  ("kernel", "zeitlos-kernel.bin"),
                  ("apps", "zeitlos-apps.zar")]
        jl = (t.get("artifacts") or {}).get("jumploader")
        if jl and "jump" in L:
            pieces.append(("jump", jl))
        for key, fn in pieces:
            c = _at_offset(fc, L[key].offset, fn)
            if c:
                lines.append("  $ " + c)
        if not lines:
            continue
        out.append("  %s -- %s" % (t["target"], t["description"]))
        out.append("")
        out += lines
        out.append("")

    out.append("The gateware and the jumploader are board-specific, which")
    out.append("is why they are the ones with a board name.")
    out.append("")
    out.append("THE JUMPLOADER. On a board that has one, the gateware")
    out.append("reloads from 0x%06x whenever the system reboots or boots"
               % (L["jump"].offset if "jump" in L else 0))
    out.append("other gateware. Flashing the gateware alone onto a board")
    out.append("that has never had a jumploader leaves `reboot` refusing")
    out.append("until it is flashed too -- the .img includes it.")
    out.append("")
    out.append("  MANIFEST.json   every RTL define, achieved Fmax and")
    out.append("                  utilisation for each build")
    out.append("  SHA256SUMS      checksums for everything above")
    out.append("")
    out.append("Only the gateware and the jumploader differ between")
    out.append("boards. The kernel,")
    out.append("the core apps and the splash are identical everywhere --")
    out.append("the `net` app carries both NIC drivers and picks one at")
    out.append("startup from the SOC feature register.")
    out.append("")

    out.append("-" * W)
    out.append("FLASH LAYOUT")
    out.append("-" * W)
    out.append("")
    for r in layout["regions"]:
        out.append("  0x%06x  %6d KB  %s"
                   % (r.offset, r.limit // 1024, r.name))
    out.append("  0x%06x            end of flash" % layout["flash_size"])
    out.append("")
    out.append("Images stop at the end of their last piece -- the")
    out.append("jumploader where a board has one, otherwise the core app")
    out.append("archive -- rather than padding to the full %d KB, so the"
               % (layout["flash_size"] // 1024))
    out.append("rest of flash is left erased instead of being written.")
    out.append("")

    out.append("-" * W)
    out.append("NOTES")
    out.append("-" * W)
    out.append("")
    out.append("Adjust the -c option to match your programming cable.")
    out.append("")
    out.append("zeitlos-<target>-dfu.bin is the same system packaged for")
    out.append("dfu-util -a 0 -D. It requires the 256KB bootloader.")
    out.append("")
    out.append("FLASHING OVER USB (dfu-util)? Lakritz and Obst ship a DFU")
    out.append("bootloader whose user partition is too small for Zeitlos,")
    out.append("so -dfu.bin will not work until you update it. Read this")
    out.append("first:")
    out.append("")
    out.append("  " + DFU_DOC_URL)
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
