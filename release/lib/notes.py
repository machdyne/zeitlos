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
import re
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


def card_sets(c):
    """Which sets a card has, for people (docs/update.md, "Sets")."""
    sets = c.get("sets")
    if not sets:
        return "everything"
    import mkfatimg
    every = [n for n in mkfatimg.SET_NAMES if n not in ("arklite",)]
    apps = [n for n in sets if n != "arklite"]
    if apps == ["base"]:
        return "the base set only"
    if set(every) <= set(apps):
        return "every set"
    missing = [n for n in every if n not in apps]
    return "every set but %s" % ", ".join(missing)


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


# -- how close each target came --
#
# The release body is GitHub markdown, which strips colours and inline
# styles, so a red or yellow background cannot be had there. A marker
# at the start of the cell does the same job and survives anywhere,
# plain text included:
#
#   RED     a clock that missed its target, or a resource over 100%
#   GREEN   met, with room to spare
#   YELLOW  a clock within WARN_MHZ of its target, or a resource over
#           WARN_PCT (or at it) -- it met, but the next change may not
#
# An advisory miss (an IO pseudo-domain, build.is_pseudo_domain()) does
# not block a release and is shown yellow, not red.
#
# Some resources are single hard blocks a design either uses or does
# not: the PLL, the configuration clock pin that reaches the SPI flash.
# At 100% they are where they are meant to be, so green; over 100%
# (which place-and-route would refuse anyway) is still red.
GREEN = "\U0001F7E2"		# green circle
RED = "\U0001F534"			# red circle
YELLOW = "\U0001F7E1"		# yellow circle
WARN_MHZ = 0.5
WARN_PCT = 98

_RANK = {None: 0, YELLOW: 1, RED: 2}


def _worst(a, b):
    return a if _RANK[a] >= _RANK[b] else b


def _clock_mark(c, advisory):
    target = c.get("target_mhz")
    if c.get("result") == "FAIL":
        return YELLOW if c in advisory else RED
    if target is not None and c["achieved_mhz"] - target < WARN_MHZ:
        return YELLOW
    return None


# Resources whose 100% is expected (ECP5 names; others as they come).
FULL_OK = re.compile(r"^(EHXPLLL|USRMCLK|JTAGG|GSR|SGSR|OSCG|DCCA|DCSC|"
                     r"EXTREFB|DTR|START|SEDGA|PLLE2_ADV|MMCME2_ADV|STARTUPE2)$")


def _util_mark(pct, name=None):
    if pct > 100:
        return RED
    if name and FULL_OK.match(name):
        return None
    if pct >= WARN_PCT:
        return YELLOW
    return None


def _cell(mark, text):
    """Bold when flagged; a passing cell gets the green dot, plain."""
    return "%s **%s**" % (mark, text) if mark else "%s %s" % (GREEN, text)


# Block RAM, by the names place-and-route reports it under: DP16KD
# (ECP5), RAMB18E1/RAMB36E1 (7-series), CC_BRAM_20K/40K (GateMate),
# ICESTORM_RAM (iCE40).
_BRAM = re.compile(r"DP16KD|RAMB|BRAM|ICESTORM_RAM")


def bram_cell(u):
    """Block RAM used of what the part has, every kind the log lists."""
    cells = []
    worst = None
    for name in sorted(u):
        if not _BRAM.search(name):
            continue
        m = _util_mark(u[name]["percent"])
        worst = _worst(worst, m)
        cells.append(_cell(m, "%s %d/%d" % (name, u[name]["used"], u[name]["total"])))
    return worst, "; ".join(cells) or "-"


def build_row(t):
    """(mark, fmax cell, utilisation cell, block RAM cell) for one
    target's timing."""
    tm = t.get("timing") or {}
    advisory = tm.get("advisory", [])
    worst = None
    parts = []
    for c in tm.get("clocks", []):
        m = _clock_mark(c, advisory)
        worst = _worst(worst, m)
        text = "%s %.1f MHz" % (_clock(c["clock"]), c["achieved_mhz"])
        if c.get("target_mhz") is not None:
            text += " (%g)" % c["target_mhz"]
        if m and c.get("result") == "FAIL":
            text += " FAILED" + (", advisory" if c in advisory else "")
        parts.append(_cell(m, text))
    fmax = "; ".join(parts) or "-"

    # The logic cells always; any other resource only when it is close.
    u = tm.get("utilisation", {})
    key = "TRELLIS_COMB" if "TRELLIS_COMB" in u else (
        sorted(u)[0] if u else None)
    cells = []
    for name in sorted(u):
        pct = u[name]["percent"]
        m = _util_mark(pct, name)
        full = pct >= WARN_PCT and FULL_OK.match(name)   # shown, but green
        if name != key and ((not m and not full) or _BRAM.search(name)):
            continue
        worst = _worst(worst, m)
        text = "%s %d%%" % (name, pct)
        cells.insert(0 if name == key else len(cells), _cell(m, text))
    util = "; ".join(cells) or "-"
    bmark, bram = bram_cell(u)
    worst = _worst(worst, bmark)
    return worst, fmax, util, bram


def build_table(targets):
    """The Build section's table, and a line on what is marked, if any."""
    rows = ["| Target | Fmax (target) | Utilisation | Block RAM (used/total) |",
            "| --- | --- | --- | --- |"]
    red = yellow = 0
    for t in targets:
        mark, fmax, util, bram = build_row(t)
        red += mark == RED
        yellow += mark == YELLOW
        name = "`%s`" % t["target"]
        if not t.get("timing"):
            pass						# no data (an old manifest): no verdict
        elif mark:
            name = "%s %s" % (mark, name)
        else:
            name = "%s %s" % (GREEN, name)
        rows.append("| %s | %s | %s | %s |" % (name, fmax, util, bram))
    summary = []
    if red:
        summary.append("%s **%d target%s missed timing or utilisation.**"
                       % (RED, red, "" if red == 1 else "s"))
    if yellow:
        summary.append("%s %d target%s within %.1f MHz of a clock target "
                       "or at %d%%+ of a resource."
                       % (YELLOW, yellow, "" if yellow == 1 else "s",
                          WARN_MHZ, WARN_PCT))
    return rows, summary


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
               "containing the gateware, kernel and core apps "
               "at their fixed offsets. Flash one file and the board boots "
               "to a desktop; an SD card is optional.")
    out.append("")
    # A miss must not hide at the bottom of the page.
    _, summary = build_table(targets)
    if summary:
        for line in summary:
            out.append(line)
            out.append("")
        out.append("See **Build** below.")
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
    out.append("**Flashing a Machdyne board over USB?** Lakritz, Obst, "
               "Konfekt, Minze, Schoko, Noir and Klinge ship a DFU "
               "bootloader whose user partition is too small for Zeitlos, "
               "so the `-dfu.bin` will not fit until you update it. "
               "[Read this first](%s)." % DFU_DOC_URL)
    out.append("")

    cs = cards(sdcard)
    if cs:
        out.append("## SD card")
        out.append("")
        out.append("Optional, and the same for every target. The images "
                   "differ in which sets of apps they carry and how much "
                   "reference material `ask` can search; `update -s` adds "
                   "sets to a card later (docs/update.md).")
        out.append("")
        out.append("| image | size | apps | `ask` can search |")
        out.append("|---|---|---|---|")
        for c in cs:
            mb, fits = card_size(c)
            out.append("| `%s` | %d MB, %s | %s | %s |"
                       % (c["file"], mb, fits, card_sets(c), card_contents(c)))
        out.append("")
        out.append("```")
        out.append("gzip -dc %s | sudo dd of=/dev/sdX bs=4M "
                   "status=progress conv=fsync" % cs[0]["file"])
        out.append("```")
        out.append("")
        out.append("Applications live in `/apps` on the card, and each "
                   "one's data in `/data/<app>`; your own files go in "
                   "`/home`, which starts empty (docs/layout.md). You "
                   "never need to type `/apps`: `run term` looks there, "
                   "then in the flash archive.")
        out.append("")
        core = []
        for t in targets:
            for a in t.get("core_apps") or []:
                if a not in core:
                    core.append(a)
        out.append("The core apps (%s) are not on the card -- they are "
                   "in flash. A copy in `/apps` on the card takes precedence "
                   "over the flash copy, so dropping one there is how you "
                   "hot-swap a single app during development."
                   % (", ".join("`%s`" % a for a in core) or "see MANIFEST.json"))
        out.append("")
        out.append("**Upgrading a card from an older release:** an app that "
                   "has since become a core app may still be in its `/apps`, "
                   "and that old copy would keep winning over the new one in "
                   "flash. Boot says so (`init: /apps/text on the card "
                   "overrides the flash copy`). Write the new card image, or delete those files "
                   "from `/apps`.")
        out.append("")
        out.append("`posix` IS on the card; `repl` is in flash. `term` "
                   "starts one when its REPL or POSIX button is pressed. "
                   "Without a card POSIX cannot start; REPL and OPEN (F11, "
                   "telnet, ssh and serial) still work.")
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
               "core app archive -- rather than padded to the full %d KB, so the "
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
    rows, summary = build_table(targets)
    if not summary and any(t.get("timing") for t in targets):
        summary = ["%s Every target met timing with at least %.1f MHz to "
                   "spare, under %d%% of every resource."
                   % (GREEN, WARN_MHZ, WARN_PCT)]
    for line in summary:
        out.append(line)
        out.append("")
    out.extend(rows)
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
        out.append("      for every board: %s of apps (/apps)," % card_sets(c))
        out.append("      the documentation (/docs), the ARK scroll (/opt/ark),")
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
    if any((t.get("artifacts") or {}).get("apps") for t in targets):
        out.append("  zeitlos-<board>-apps.zar       instead of the above, for a")
        out.append("                                 board that omits a core app")
    if "jump" in L:
        out.append("  zeitlos-<board>-jump.bin       jumploader, flash 0x%06x"
                   % L["jump"].offset)
        out.append("                                 (boards that have one; a .bin,")
        out.append("                                 not a .bit, so it is written")
        out.append("                                 whole, header included)")
    out.append("")
    out.append("THE KERNEL AND THE CORE APPS GO TOGETHER. They share a")
    out.append("64KB erase block, and openFPGALoader erases whole")
    out.append("blocks on most flash chips: writing one wipes the")
    out.append("other's edge, in either order. Write them as one: the")
    out.append("whole image, or the kernel padded with 0xff to 160KB")
    out.append("with the archive after it (`make flash_sw` does this).")
    out.append("")
    out.append("THE OFFSETS ARE NOT OPTIONAL. The BIOS reads the kernel")
    out.append("from a fixed address, and the OS reads the")
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
                  ("kernel", "zeitlos-kernel.bin"),
                  ("apps", (t.get("artifacts") or {}).get("apps")
                   or "zeitlos-apps.zar")]
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

    out.append("The gateware is board-specific, which is why it is the")
    out.append("one with a board name.")
    out.append("")
    out.append("No jumploader. `reboot` reloads the FPGA from flash")
    out.append("offset 0. The 0.0.5 images put a jumploader where the")
    out.append("core apps now are; this image writes over it, and the")
    out.append("new gateware no longer looks for it.")
    out.append("")
    out.append("  MANIFEST.json   every RTL define, achieved Fmax and")
    out.append("                  utilisation for each build")
    out.append("  SHA256SUMS      checksums for everything above")
    out.append("")
    out.append("Only the gateware differs between")
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
    out.append("Images stop at the end of their last piece -- the core")
    out.append("app archive -- rather than padding to the full %d KB, so the"
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
    out.append("FLASHING OVER USB (dfu-util)? Lakritz, Obst, Konfekt,")
    out.append("Minze, Schoko, Noir and Klinge ship a DFU")
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
    out.append("need to type that path: `run term` looks in apps/, then")
    out.append("in the flash archive.")
    out.append("")
    out.append("The core apps are in flash, not on the sdcard. A copy in")
    out.append("apps/ on the card takes precedence over the flash copy,")
    out.append("which is how a single app is hot-swapped during")
    out.append("development -- and why a card from an older release may")
    out.append("need apps/text and apps/files deleted (or the new card")
    out.append("image written) for the flash copies to be used.")
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
