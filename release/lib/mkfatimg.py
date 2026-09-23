#
# Zeitlos release tooling -- the SD card image.
#
# THE file list for the SD card, and the only one.
#
# tools/mkfatimg.sh used to carry a second copy and `zrelease check`
# compared them -- which is what a duplicated list really costs: not
# just the drift, but a whole check whose only job is to police it. The
# script calls `zrelease sdcard` now, so there is nothing to compare.
#
# It had already drifted, for the record: the shell list was missing
# gpudemo, chip8 and chess.
#
# Originally tools/mkfatimg.sh with one change: mount + cp becomes mmd +
# mcopy. The filesystem itself is built by the same mkfs.fat with the
# same arguments (-F 32 -S 512 -s 1 -n ZEITLOS) and checked by the same
# fsck.fat, so the image is the same image.
#
# WHY THE CHANGE: mkfs.fat has always been happy to format a plain
# file. It was only `mount -o loop` that needed root, and that one line
# forced the whole release to run under sudo -- which in this tree is
# actively harmful, because a build running as root leaves root-owned
# .o files scattered through sw/ that then break every subsequent
# non-root build. The top-level Makefile's tftp-dist target carries a
# comment about exactly this hazard. mtools writes into the image file
# directly, needs no loop device, no mount point and no privileges, so
# the release can run as you.
#
# WHAT GOES ON THE CARD, and what does not:
#
#   - The core apps (wm, net, term, console) are DELIBERATELY ABSENT.
#     They live in flash, in the ZAR, and sw/os/zar.h's rule is that a
#     copy on the card wins over the flash copy -- so shipping them
#     here would shadow the flash build.
#
#     That used to be forced rather than chosen: `net` was compiled
#     against one NIC driver, so one shared card image could only ever
#     have carried the wrong one for some boards. It links all three
#     and picks at runtime now, so a card copy would be correct
#     everywhere and this is a choice again.
#
#     It stays a choice for two reasons. Flash winning unless you
#     deliberately put something on the card is a rule worth keeping,
#     and it is what makes dropping one app at the card root a
#     hot-swap rather than an accident. And every board this release
#     supports can hold the ZAR in flash -- including over DFU, where
#     the 256KB bootloader split leaves the flash map untouched. No
#     shipped configuration needs core apps on the card, so putting
#     them there would buy nothing and cost the rule.
#
#   - Everything else is board-independent, which is why there is ONE
#     card image per release rather than one per target. The apps here
#     detect optional hardware at runtime (z_audio_present(),
#     z_rtc_available(), z_game_available()) rather than being compiled
#     for it.
#
#   - It is still per RELEASE, not per anything longer-lived:
#     sw/common/syscalls.def is compiled into both the kernel and every
#     app, so a card built against one kernel and flashed alongside
#     another calls the wrong handler for every syscall past the point
#     the two diverge. Rebuild it every time.
#

import os
import shutil
import subprocess

LABEL = "ZEITLOS"

# -- image variants --
#
# A release ships one card image per corpus size. Everything but the
# `ask` packs is identical across them.
#
#   zeitlos.img.gz            zdocs only. Keeps the name v0.0.1 and
#                             v0.0.2 shipped, which the project README's
#                             releases/latest/download/ URL points at.
#   zeitlos-arklite.img.gz    + Ark Lite (Codex, selected books, Scroll)
#   zeitlos-arkmedium.img.gz  + Ark Medium (Wikipedia 10K vital, the
#                             Gutenberg CD, MedlinePlus, the Factbook,
#                             and Lite). arkmed CONTAINS Lite, so this
#                             image carries arkmed and not arklite.
#
# (key, file stem, packs), in the order they are built and listed.
VARIANTS = [
    ("base",      "zeitlos",           ("zdocs",)),
    ("arklite",   "zeitlos-arklite",   ("zdocs", "arklite")),
    ("arkmedium", "zeitlos-arkmedium", ("zdocs", "arkmed")),
]


def variant(key):
    for v in VARIANTS:
        if v[0] == key:
            return v
    raise FatError("no card variant `%s` (have: %s)"
                   % (key, ", ".join(v[0] for v in VARIANTS)))


# -- sizing --
#
# An image is sized to what it holds, never below MIN_SIZE_MB (what
# every release before variants shipped, so the small images are the
# size they always were). Above that: the content, rounded up to whole
# clusters per file, plus HEADROOM for the filesystem's own structures
# and FREE_MB left over for the user -- `user/`, zcc output, text
# files. Rounded to SIZE_STEP_MB so sizes are not arbitrary.
#
# Free space costs nothing to download (zeros compress to nothing) but
# it does set the smallest card the image fits, which is why it is not
# more generous. Somebody with a larger card can grow the partition.
MIN_SIZE_MB = 64
FREE_MB = 16
HEADROOM = 1.05
SIZE_STEP_MB = 64

# A "4 GB" card holds about 3.7-3.9e9 bytes, and cards vary. Above this
# an image needs an 8 GB card, and the release notes say so.
CARD_4GB_BYTES = 3600000000

# -- cluster size --
#
# Small images keep 512-byte clusters (`-s 1`), as they always had:
# nothing on them is big, and slack stays negligible.
#
# Large ones get 8KB. Two costs of tiny clusters grow with the card:
#
#   - The FAT itself. 1.5GB at 512 bytes is 3M clusters, a 12MB FAT
#     written twice; at 8KB it is 750KB.
#   - Opening a file. sw/os/fsapi.c builds FatFs's fast-seek map on
#     every open, which walks the file's whole cluster chain. `ask`
#     opens its postings and dictionary on every query, and at arkmed
#     scale post.zlp is tens of megabytes: ~100K FAT entries to walk at
#     512 bytes, ~6K at 8KB.
#
# The price is slack: on average half a cluster per file, ~4KB, which
# for arkmed's tens of thousands of files is on the order of 100MB of a
# 1.5GB card. That is what `ask`'s query speed is bought with.
LARGE_IMAGE_BYTES = 1024 * 1024 * 1024
SECTORS_SMALL = 1
SECTORS_LARGE = 16


def sectors_per_cluster(image_bytes):
    return SECTORS_LARGE if image_bytes > LARGE_IMAGE_BYTES else SECTORS_SMALL

# Mirrors tools/mkfatimg.sh. Grouped the same way and in the same
# order, so the two can be read side by side.
# Destinations are apps/-prefixed: the card holds executables in
# apps/ rather than loose in the root, alongside docs/, ark/ and
# user/. sw/os/fs/fs.c's fs_exec_resolve() searches the root and then
# apps/, so a bare `run term` still works and a card written before
# the move still boots.
# Everything in sw/apps that is not a CORE app.
#
# Core apps (wm, net, term, console -- release/hw/boards/*.spec) live in flash
# and must NOT be duplicated here: a card copy would shadow the
# per-target `net` build with the wrong PHY driver, which is the bug
# check_against_script() was written after.
#
# Everything else ships. The lists below are grouped for reading only;
# nothing depends on which group an app is in.
SUPPLEMENTAL = [
    ("apps/files", "sw/apps/files/files.bin"),
    ("apps/text", "sw/apps/text/text.bin"),
    ("apps/sheet", "sw/apps/sheet/sheet.bin"),
    ("apps/read", "sw/apps/read/read.bin"),
    ("apps/ask", "sw/apps/ask/ask.bin"),
    ("apps/draw", "sw/apps/draw/draw.bin"),
    ("apps/info", "sw/apps/info/info.bin"),
    ("apps/calc", "sw/apps/calc/calc.bin"),
    ("apps/clock", "sw/apps/clock/clock.bin"),
    ("apps/cal", "sw/apps/cal/cal.bin"),
    ("apps/settings", "sw/apps/settings/settings.bin"),
    ("apps/track", "sw/apps/track/track.bin"),
    ("apps/view", "sw/apps/view/view.bin"),
    ("apps/web", "sw/apps/web/web.bin"),
    ("apps/hex", "sw/apps/hex/hex.bin"),
    ("apps/play", "sw/apps/play/play.bin"),
    ("apps/midi", "sw/apps/midi/midi.bin"),
    ("apps/mmod", "sw/apps/mmod/mmod.bin"),
    ("apps/logic", "sw/apps/logic/logic.bin"),
    ("apps/serial", "sw/apps/serial/serial.bin"),
    ("apps/tts", "sw/apps/tts/tts.bin"),
]

# The casino. One dock icon (apps/casino) launches the rest, so the
# games have to be on the card even though nothing on the dock points
# at them directly -- z_proc_run() resolves them by name from here.
CASINO = [
    ("apps/casino", "sw/apps/casino/casino.bin"),
    ("apps/poker", "sw/apps/poker/poker.bin"),
    ("apps/roulette", "sw/apps/roulette/roulette.bin"),
    ("apps/blkjack", "sw/apps/blackjack/blackjack.bin"),
    ("apps/slots", "sw/apps/slots/slots.bin"),
    ("apps/craps", "sw/apps/craps/craps.bin"),
]

GAMES_DEMOS = [
    ("apps/space3d", "sw/apps/space3d/space3d.bin"),
    ("apps/gamedemo", "sw/apps/gamedemo/gamedemo.bin"),
    ("apps/gpu3d", "sw/apps/gpu3d/gpu3d.bin"),
    ("apps/gpudemo", "sw/apps/gpudemo/gpudemo.bin"),
    ("apps/chip8", "sw/apps/chip8/chip8.bin"),
    ("apps/chess", "sw/apps/chess/chess.bin"),
    ("apps/kidgames", "sw/apps/kidgames/kidgames.bin"),
]

MISC = [
    ("apps/portdemo", "sw/apps/portdemo/portdemo.bin"),
    ("apps/hellowin", "sw/apps/hello_win/hello_win.bin"),
    ("apps/audiotst", "sw/apps/audiotest/audiotest.bin"),
]

# The two shells a term window connects to. init() starts both from the
# card at boot, repl first -- neither is in the flash archive any more,
# so these copies are the ONLY copies. See docs/flash_apps.md, "Why repl
# is not a core app". posix is here rather than in SELFHOST because
# that is what it is to a user; the compiler and editor it hosts stay
# below.
SHELLS = [
    ("apps/repl", "sw/apps/repl/repl.bin"),
    ("apps/posix", "sw/apps/posix/posix.bin"),
]

# The self-hosting set: a compiler and an editor, driven from posix.
#
# These are what make the card able to extend itself rather than only
# run what was cross-compiled onto it (docs/posix.md). `zcc` is useless
# without libz/ below -- see LIBZ_FILES.
SELFHOST = [
    ("apps/zcc", "sw/apps/zcc/zcc.bin"),
    ("apps/zfpga", "sw/apps/zfpga/zfpga.bin"),
    ("apps/vi", "sw/apps/vi/vi.bin"),
    ("apps/ttytest", "sw/apps/ttytest/ttytest.bin"),
]

DIRS = ["apps", "audio", "docs", "ark", "user", "libz", "libz/include",
        "fpga", "fpga/boards", "fpga/examples", "speech"]

# The speech pack: the pronunciation lexicon and the recorded voice
# sw/apps/tts reads (docs/tts.md). NOT built from this tree and NOT
# committed to it -- tools/speech builds it from public-domain sources,
# and it is published with the release -- which is why it is looked for
# rather than required. Without it, speech still works: the built-in
# dictionary and letter-to-sound rules, and the formant voice.
#
# SPEECH_PACK in the environment ships a pack built elsewhere.
SPEECH_PACK = "tools/speech/build/en/speech.zspk"
SPEECH_DEST = "/speech/en.spk"      # tts's PACK_PATH; 8.3, so not "speech.zspk"

# -- ask packs --
#
# Shipped BY DEFAULT: a release carries `ask` and the data it needs,
# because an app that boots to "no packs in /ask" looks broken rather
# than incomplete.
#
# Each pack contributes /ark/<name> and /ask/<name>, copied from
# tools/ask/out/<name>. Override or disable with the environment:
#
#     ZEITLOS_ASK_PACKS="zdocs arklite arkmed"    # add one
#     ZEITLOS_ASK_PACKS=                          # ship none
#
# A release now DEPENDS on those packs being built, which is a real
# coupling and is why the check happens in preflight rather than
# halfway through copying. `zdocs` builds from this tree alone in
# seconds; `arklite` needs the ark clone, which lib/fetch.py caches
# after the first time.
#
# SIZES: zdocs 2.4MB, arklite ~22MB, arkmed hundreds. The image is
# sized to fit them (plan_size), before anything is formatted.
ASK_OUT = "tools/ask/out"
# What `zrelease sdcard` (and so tools/mkfatimg.sh) puts on a card when
# nothing says otherwise: the development card, unchanged from before
# variants. A release builds VARIANTS instead.
ASK_PACKS_DEFAULT = ("zdocs", "arklite")

# -- the zcc runtime, under libz/ --
#
# Two files and a pile of headers, and none of them is optional: a
# compiler that cannot find libz.bin produces a freestanding binary
# with no printf and no malloc, and one that cannot find the headers
# cannot compile anything that includes them.
#
# libz.bin is NOT the copy linked inside zcc.bin. That one is for the
# compiler's own use; this one is the runtime it embeds into the
# programs it builds, and they land at different processes'
# 0x8000_0000 so they cannot be shared. See docs/zcc.md, "Where libz
# comes from".
#
# The headers are copied rather than listed one by one because the set
# is "whatever sw/common exports", which changes as the system grows;
# a list here would go stale silently and the symptom would be a
# missing include on the device.
LIBZ_FILES = [
    ("libz/libz.bin", "sw/apps/zcc/libz/libz.bin"),
    ("libz/libz.sym", "sw/apps/zcc/libz/libz.sym"),
    ("libz/include/libz.h", "sw/apps/zcc/libz/libz.h"),
]

LIBZ_HEADER_DIRS = [
    ("libz/include", "sw/common", (".h",)),
    ("libz/include", "sw/apps/zcc/include", (".h",)),
]

# zeitlos.h generates its syscall enum from this by X-macro, so it is a
# header in everything but name and extension.
LIBZ_EXTRA = [
    ("libz/include/syscalls.def", "sw/common/syscalls.def"),
]

# -- the zfpga chip databases, under fpga/ --
#
# zfpga looks in /fpga by default, the way zcc looks in /libz, so the
# layout is fixed and nothing needs to be typed. One .zdb per die:
# lfe5u-25f.zdb also serves the 12F (identical tilegrid, docs/zfpga.md
# sec. 2.2). 45F and 85F databases will join this list when their
# vendored data does (sw/apps/zfpga/ext/prjtrellis-db/README.zeitlos.md).
# Without one, zfpga refuses at the .device line and names the file it
# looked for.
FPGA_FILES = [
    # 8.3: FatFs here has no long names. `lfe5u-25f.zdb` was nine
    # characters; mcopy wrote it with a VFAT long-name entry, Linux read
    # it back fine, and the board saw only a mangled alias
    # (docs/zfpga.md sec. 22).
    ("fpga/lfe5u25f.zdb", "sw/apps/zfpga/db/lfe5u25f.zdb"),
    # the 45F: Mozart ML1 and Sergei ML1
    ("fpga/lfe5u45f.zdb", "sw/apps/zfpga/db/lfe5u45f.zdb"),
    # board profiles and their pins: `zfpga build design.v -b lakritz`
    ("fpga/boards/lakritz.brd", "sw/apps/zfpga/db/boards/lakritz.brd"),
    ("fpga/boards/lakritz.lpf", "sw/apps/zfpga/db/boards/lakritz.lpf"),
    ("fpga/boards/obst.brd", "sw/apps/zfpga/db/boards/obst.brd"),
    ("fpga/boards/obst.lpf", "sw/apps/zfpga/db/boards/obst.lpf"),
    ("fpga/boards/mozart1.brd", "sw/apps/zfpga/db/boards/mozart1.brd"),
    ("fpga/boards/mozart1.lpf", "sw/apps/zfpga/db/boards/mozart1.lpf"),
    ("fpga/boards/sergei1.brd", "sw/apps/zfpga/db/boards/sergei1.brd"),
    ("fpga/boards/sergei1.lpf", "sw/apps/zfpga/db/boards/sergei1.lpf"),
    # something to build: docs/zfpga-test.md walks through these
    ("fpga/examples/blink.v", "sw/apps/zfpga/db/examples/blink.v"),
    ("fpga/examples/blinkf.v", "sw/apps/zfpga/db/examples/blinkf.v"),
    ("fpga/examples/empty.zn", "sw/apps/zfpga/db/examples/empty.zn"),
    ("fpga/examples/on.zn", "sw/apps/zfpga/db/examples/on.zn"),
    ("fpga/examples/blink.zn", "sw/apps/zfpga/db/examples/blink.zn"),
    ("fpga/examples/hand.zl", "sw/apps/zfpga/db/examples/hand.zl"),
    # two modules, a parameter and an `include, blinking at half blink's
    # rate: zfpga synth's hierarchy, checked on the board (docs/zfpga.md
    # sec. 24)
    ("fpga/examples/blinkh.v", "sw/apps/zfpga/db/examples/blinkh.v"),
    ("fpga/examples/blinkh.vh", "sw/apps/zfpga/db/examples/blinkh.vh"),
]

# Something to compile.
#
# A card with a compiler and no example is a card where the first
# thing anyone does is guess at the include paths. These are small and
# they are the three stages: no runtime, the runtime, and output that
# reaches the terminal rather than the serial console.
EXAMPLES = [
    ("user/hello.c", "sw/apps/zcc/examples/hello.c"),
    ("user/hellolz.c", "sw/apps/zcc/examples/hello_libz.c"),
    ("user/hellotrm.c", "sw/apps/zcc/examples/hello_term.c"),
]

# The configuration file, at the card root where the kernel reads it
# (sw/os/cfg.c, docs/config.md). Every setting in it is commented out,
# so a fresh card behaves exactly as if the file were absent -- it is
# there as the documented place to start editing, not to set anything.
CONFIG_FILES = [
    ("zeitlos.cfg", "sw/data/zeitlos.cfg"),
]

# Tracker modules, from sw/data/audio. Whatever is there is shipped --
# a glob rather than a list, because these are data files somebody
# drops in, not build products with a Makefile rule each.
#
# sw/apps/track scans /audio first and then the root, so a card
# written before this existed still plays and dropping one in the root
# still works.
AUDIO_DIR = "sw/data/audio"
AUDIO_EXT = ".mod"

TOOLS = ["mkfs.fat", "fsck.fat", "mmd", "mcopy"]


class FatError(Exception):
    pass


def preflight():
    missing = [t for t in TOOLS if shutil.which(t) is None]
    if missing:
        raise FatError(
            "missing tools: %s\n"
            "  Debian/Ubuntu:  sudo apt install dosfstools mtools\n"
            "  (dosfstools provides mkfs.fat and fsck.fat; mtools "
            "provides mmd and mcopy)" % ", ".join(missing))


def _run(cmd, quiet=True):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise FatError("%s failed:\n%s%s"
                       % (" ".join(cmd), r.stdout, r.stderr))
    return r.stdout


def ask_packs_requested():
    """Which packs to ship.

    $ZEITLOS_ASK_PACKS overrides the default; setting it EMPTY ships
    none, which is distinct from not setting it at all.
    """
    raw = os.environ.get("ZEITLOS_ASK_PACKS")
    if raw is None:
        return list(ASK_PACKS_DEFAULT)
    return [p for p in raw.replace(",", " ").split() if p]


def ask_pack_files(root, name):
    """Every file of pack `name`, as (card_path, source_path).

    Raises if the pack is not built, rather than shipping a release
    with an app and no data -- which boots to "no packs in /ask" and
    looks like the app is broken.
    """
    base = os.path.join(root, ASK_OUT, name)
    out = []
    for sub in ("ark/" + name, "ask/" + name):
        d = os.path.join(base, sub)
        if not os.path.isdir(d):
            raise FatError(
                "ask pack '%s' was requested but %s does not exist.\n"
                "  Build it first:  ./tools/ask/ask build "
                "tools/ask/dist/%s.spec" % (name, d, name))
        for dp, _dn, fn in os.walk(d):
            for f in sorted(fn):
                src = os.path.join(dp, f)
                rel = os.path.relpath(src, base).replace(os.sep, "/")
                out.append(("/" + rel, src))
    return sorted(out)


def _check_83_path(dest):
    """Every component of a card path, 8.3 -- see fits_83 in build()."""
    for part in dest.strip("/").split("/"):
        stem, _, ext = part.partition(".")
        if not stem or len(stem) > 8 or len(ext) > 3 or "." in ext:
            raise FatError(
                "'%s' does not fit an 8.3 name and cannot go on the card "
                "(FatFs here is FF_USE_LFN 0)" % dest)


def plan_size(sizes):
    """(image bytes, sectors per cluster) for files of these sizes."""
    def need(spc):
        c = spc * 512
        return sum((n + c - 1) // c * c for n in sizes) + 4096 * c

    size = MIN_SIZE_MB * 1024 * 1024
    for _ in range(3):      # the cluster size depends on the size
        spc = sectors_per_cluster(size)
        want = int(need(spc) * HEADROOM) + FREE_MB * 1024 * 1024
        step = SIZE_STEP_MB * 1024 * 1024
        want = (want + step - 1) // step * step
        if want <= size:
            break
        size = want
    return size, sectors_per_cluster(size)


def build(root, out_path, ark_dir=None, verbose=True, packs=None):
    """Build the SD card image. Returns a list of (name, size) shipped.

    `packs` names the ask packs to ship; None means
    ask_packs_requested() (the environment, or ASK_PACKS_DEFAULT). A
    pack ships as a directory and is listed as one entry,
    "ark/<pack>/", rather than as its tens of thousands of files --
    MANIFEST.json would otherwise carry every Wikipedia article's
    number.
    """
    preflight()

    ark_dir = ark_dir or os.path.join(root, "sw/data/ark")

    apps = SUPPLEMENTAL + CASINO + GAMES_DEMOS + MISC + SHELLS + SELFHOST

    # A name listed twice copies once and then fails, halfway through a
    # 64MB image, with mcopy's own silence for an error message. Said
    # here instead.
    seen = set()
    dupes = sorted(n for n, _ in apps if n in seen or seen.add(n))
    if dupes:
        raise FatError("listed more than once: %s" % ", ".join(dupes))

    # Check every input up front. Finding out that gamedemo.bin was
    # never built after formatting a 64MB image and copying twelve
    # other files is a slower way to learn the same thing.
    missing = [p for _, p in apps
               if not os.path.exists(os.path.join(root, p))]

    # libz.bin and libz.sym come from a separate make (sw/apps/zcc
    # builds them as a dependency), and a card whose zcc cannot find
    # them compiles only freestanding programs -- so their absence is
    # an error here rather than a quiet omission.
    missing += [p for _, p in LIBZ_FILES + LIBZ_EXTRA + CONFIG_FILES
                + EXAMPLES + FPGA_FILES
                if not os.path.exists(os.path.join(root, p))]

    if missing:
        raise FatError("not built yet:\n  %s\n"
                       "  Run the app builds first (sw/apps, and "
                       "sw/apps/zcc for libz)." % "\n  ".join(missing))

    audio_src = os.path.join(root, AUDIO_DIR)
    audio = sorted(f for f in os.listdir(audio_src)
                   if f.lower().endswith(AUDIO_EXT)) \
        if os.path.isdir(audio_src) else []

    # Not an error if there are none -- the directory is data, and a
    # tree without it still produces a usable card. It IS worth saying
    # so, because a silently music-less release is hard to notice.
    if not audio:
        print("    note: no %s files in %s, card will have none"
              % (AUDIO_EXT, AUDIO_DIR))

    speech = os.environ.get("SPEECH_PACK") or os.path.join(root, SPEECH_PACK)
    if not os.path.exists(speech):
        print("    note: no speech pack at %s, card will have none"
              % os.path.relpath(speech, root))
        print("          (speech will use its built-in dictionary, rules and formant voice)")
        speech = None

    docs = sorted(f for f in os.listdir(os.path.join(root, "docs"))
                  if f.endswith(".md"))
    if not docs:
        raise FatError("docs/: no .md files found")

    if not os.path.isdir(ark_dir):
        raise FatError(
            "%s: not found.\n"
            "  The ARK scroll ships on the card (see "
            "https://github.com/machdyne/ark). Point --ark at a checkout, "
            "or drop the .md files there." % ark_dir)
    ark = sorted(f for f in os.listdir(ark_dir) if f.endswith(".md"))
    if not ark:
        raise FatError("%s: no .md files found" % ark_dir)

    # -- ask packs: resolve and size them BEFORE formatting --
    #
    # Both failure modes here are ones this file already warns about
    # for apps: a missing input found after the image is half written,
    # and running out of room "halfway through a 64MB image, with
    # mcopy's own silence for an error message".
    pack_files = []
    for packname in (ask_packs_requested() if packs is None else packs):
        pack_files.append((packname, ask_pack_files(root, packname)))

    # Every pack name must be 8.3, checked BEFORE formatting: mcopy
    # would store a long name that FatFs here cannot open, and finding
    # that out after the image is half written is the failure this
    # function is arranged to avoid.
    for _packname, files in pack_files:
        for card, _src in files:
            _check_83_path(card)

    # Everything that goes on the card, for sizing.
    sizes = [os.path.getsize(src) for _n, fs in pack_files for _c, src in fs]
    nfiles_pack = len(sizes)
    for _n, rel in apps + LIBZ_FILES + LIBZ_EXTRA + EXAMPLES + FPGA_FILES \
            + CONFIG_FILES:
        sizes.append(os.path.getsize(os.path.join(root, rel)))
    for a in audio:
        sizes.append(os.path.getsize(os.path.join(audio_src, a)))
    if speech:
        sizes.append(os.path.getsize(speech))
    for d in docs:
        sizes.append(os.path.getsize(os.path.join(root, "docs", d)))
    for a in ark:
        sizes.append(os.path.getsize(os.path.join(ark_dir, a)))
    size_bytes, spc = plan_size(sizes)
    if verbose:
        print("    %d files (%d in ask packs), %.1f MB of content -> "
              "%d MB image, %d-byte clusters"
              % (len(sizes), nfiles_pack, sum(sizes) / 1e6,
                 size_bytes // (1024 * 1024), spc * 512))
        if size_bytes > CARD_4GB_BYTES:
            print("    NOTE: %.2f GB -- this image needs an 8 GB card"
                  % (size_bytes / 1e9))

    if os.path.exists(out_path):
        os.unlink(out_path)
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "wb") as f:
        f.truncate(size_bytes)

    _run(["mkfs.fat", "-F", "32", "-S", "512", "-s", str(spc),
          "-n", LABEL, out_path])

    for d in DIRS:
        _run(["mmd", "-i", out_path, "::/%s" % d])

    shipped = []

    def copy(src_abs, dest):
        _run(["mcopy", "-i", out_path, src_abs, "::" + dest])
        shipped.append((dest.lstrip("/"), os.path.getsize(src_abs)))

    made_dirs = set(DIRS)

    def mkdir_p(path):
        """mmd each missing level. DIRS covers the fixed tree; an ask
        pack is deeper and its shape depends on the recipe."""
        parts = path.strip("/").split("/")
        for i in range(1, len(parts) + 1):
            sub = "/".join(parts[:i])
            if sub in made_dirs:
                continue
            _run(["mmd", "-i", out_path, "::/%s" % sub])
            made_dirs.add(sub)

    def fits_83(dest):
        """Every component of a card path, 8.3. FatFs here is FF_USE_LFN
        0 (sw/os/fs/fatfs/ffconf.h), so a name that does not fit cannot
        be opened on the card: mcopy stores it under a long-name entry,
        Linux shows it as written, and FatFs sees only a mangled short
        alias."""
        for part in dest.strip("/").split("/"):
            stem, _, ext = part.partition(".")
            if not stem or len(stem) > 8 or len(ext) > 3 or "." in ext:
                raise FatError(
                    "'%s' does not fit an 8.3 name and cannot go on the card "
                    "(FatFs here is FF_USE_LFN 0)" % dest)

    for name, rel in apps:
        # 8.3 is not advice: FatFs here is FF_USE_LFN 0
        # (sw/os/fs/fatfs/ffconf.h), so a name longer than eight
        # characters cannot be written to the card at all. The header
        # copy below has always checked this; the app copy did not,
        # and `audiotest` and `hello_win` are both nine.
        base = name.split("/")[-1]
        stem, _, ext = base.partition(".")
        if len(stem) > 8 or len(ext) > 3:
            raise FatError(
                "'%s' does not fit an 8.3 name and cannot go on the card "
                "(FatFs here is FF_USE_LFN 0)" % name)
        copy(os.path.join(root, rel), "/" + name)
    for a in audio:
        copy(os.path.join(audio_src, a), "/audio/" + a)
    if speech:
        copy(speech, SPEECH_DEST)
    for d in docs:
        copy(os.path.join(root, "docs", d), "/docs/" + d)
    for a in ark:
        copy(os.path.join(ark_dir, a), "/ark/" + a)

    # -- ask packs (resolved in preflight above) --
    # Packs go in as whole directory trees, one `mcopy -s` per tree.
    # Copying file by file is a subprocess per file, each re-reading the
    # FAT: fine for arklite's thousand files, hours for arkmed's tens of
    # thousands. Every name is checked for 8.3 first, because mcopy
    # would happily store a long name that FatFs here cannot open.
    for packname, files in pack_files:
        base = os.path.join(root, ASK_OUT, packname)
        total = sum(os.path.getsize(src) for _c, src in files)
        for top in ("ark", "ask"):
            mkdir_p("/" + top)
            _run(["mcopy", "-s", "-i", out_path,
                  os.path.join(base, top, packname), "::/%s/" % top])
            made_dirs.add("%s/%s" % (top, packname))
        shipped.append(("ark/%s/" % packname, total))
        if verbose:
            print("  ask pack %-10s %6d files  %7.1f MB"
                  % (packname, len(files), total / 1e6))

    for name, rel in LIBZ_FILES + LIBZ_EXTRA + EXAMPLES:
        fits_83(name)
        copy(os.path.join(root, rel), "/" + name)

    # -- the zfpga databases --
    for name, rel in FPGA_FILES:
        fits_83(name)
        copy(os.path.join(root, rel), "/" + name)

    # -- the configuration template --
    for name, rel in CONFIG_FILES:
        fits_83(name)
        copy(os.path.join(root, rel), "/" + name)

    # Headers by directory rather than by name: the set is "whatever
    # sw/common exports", and a list here would go stale silently --
    # the symptom being a missing include on the device, long after.
    for dest, srcdir, exts in LIBZ_HEADER_DIRS:
        d = os.path.join(root, srcdir)
        for h in sorted(os.listdir(d)):
            if not h.endswith(exts):
                continue
            # 8.3 is not advice here: FatFs is built with FF_USE_LFN 0
            # (sw/os/fs/fatfs/ffconf.h), so a name that does not fit
            # cannot be written to the card at all. Caught at build
            # time rather than as a mysteriously absent header.
            stem, _, ext = h.rpartition(".")
            if len(stem) > 8 or len(ext) > 3:
                raise FatError(
                    "%s/%s does not fit an 8.3 name and cannot go on the "
                    "card (FatFs here is FF_USE_LFN 0)" % (srcdir, h))
            copy(os.path.join(d, h), "/%s/%s" % (dest, h))

    # fsck.fat is not a formality here. mcopy writing into an image it
    # has no exclusive claim on is the kind of thing that produces a
    # filesystem that mounts on Linux and confuses FatFs, and FatFs
    # (sw/os/fs/fatfs) is the only reader that matters. Cheap check,
    # expensive failure.
    out = _run(["fsck.fat", "-v", out_path])
    if verbose:
        tail = [l for l in out.splitlines() if l.strip()][-3:]
        for l in tail:
            print("    %s" % l.strip())

    return shipped
