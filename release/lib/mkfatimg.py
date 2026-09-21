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
#   - The core apps (wm, net, term) are DELIBERATELY ABSENT.
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

SIZE_MB = 64
LABEL = "ZEITLOS"

# Mirrors tools/mkfatimg.sh. Grouped the same way and in the same
# order, so the two can be read side by side.
# Destinations are apps/-prefixed: the card holds executables in
# apps/ rather than loose in the root, alongside docs/, ark/ and
# user/. sw/os/fs/fs.c's fs_exec_resolve() searches the root and then
# apps/, so a bare `run term` still works and a card written before
# the move still boots.
# Everything in sw/apps that is not a CORE app.
#
# Core apps (wm, net, term -- release/hw/boards/*.spec) live in flash
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
        "fpga", "fpga/boards", "fpga/examples"]

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
# SIZE_MB and the preflight will say so before formatting anything.
ASK_OUT = "tools/ask/out"
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
    # board profiles and their pins: `zfpga build design.v -b lakritz`
    ("fpga/boards/lakritz.brd", "sw/apps/zfpga/db/boards/lakritz.brd"),
    ("fpga/boards/lakritz.lpf", "sw/apps/zfpga/db/boards/lakritz.lpf"),
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


def build(root, out_path, ark_dir=None, verbose=True):
    """Build the SD card image. Returns a list of (name, size) shipped."""
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
    for packname in ask_packs_requested():
        pack_files.append((packname, ask_pack_files(root, packname)))

    pack_bytes = sum(os.path.getsize(src)
                     for _n, fs in pack_files for _c, src in fs)
    other_bytes = 0
    for _n, rel in apps:
        other_bytes += os.path.getsize(os.path.join(root, rel))
    # Rough: the rest (docs, libz, headers, audio, ark scroll) is a few
    # megabytes and the slack below covers it.
    need = pack_bytes + other_bytes
    room = SIZE_MB * 1024 * 1024
    if need > room * 0.85:
        raise FatError(
            "the card image has no room for this.\n"
            "  %.1f MB of apps and ask packs against a %d MB image.\n"
            "  Raise SIZE_MB, or ship fewer packs:\n"
            "      ZEITLOS_ASK_PACKS=zdocs ./release/zrelease ...\n"
            "  Packs: %s"
            % (need / 1e6, SIZE_MB,
               ", ".join("%s %.1fMB"
                         % (n, sum(os.path.getsize(s) for _c, s in fs) / 1e6)
                         for n, fs in pack_files) or "(none)"))

    if os.path.exists(out_path):
        os.unlink(out_path)
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)

    with open(out_path, "wb") as f:
        f.truncate(SIZE_MB * 1024 * 1024)

    # Same arguments as tools/mkfatimg.sh, deliberately.
    _run(["mkfs.fat", "-F", "32", "-S", "512", "-s", "1",
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
    for d in docs:
        copy(os.path.join(root, "docs", d), "/docs/" + d)
    for a in ark:
        copy(os.path.join(ark_dir, a), "/ark/" + a)

    # -- ask packs (resolved in preflight above) --
    for packname, files in pack_files:
        total = 0
        for card, src in files:
            # The tree is deeper than DIRS covers: /ask/<pack>/ and
            # /ark/<pack>/<dataset>/ plus any split subdirectories.
            mkdir_p(card.rsplit("/", 1)[0])
            copy(src, card)
            total += os.path.getsize(src)
        if verbose:
            print("  ask pack %-10s %5d files  %6.1f MB"
                  % (packname, len(files), total / 1e6))

    # -- the zcc runtime --
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
