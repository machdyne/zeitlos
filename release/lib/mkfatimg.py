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
#   - The core apps (CORE_APPS in the top-level Makefile) are
#     DELIBERATELY ABSENT, and build() refuses a list that has one.
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
#     and it is what makes putting one app at /apps/<name> a hot-swap
#     rather than an accident. And every board this release
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
import re
import shutil
import subprocess

# Every card destination comes from sw/common/zpaths.h, the registry
# the C code opens the same files by (docs/layout.md). zcard("Z_DIR_APPS",
# "files") is "apps/files": no leading slash, as this file writes them.
from zpaths import P, card as zcard, dirname as zcard_dir

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
# -- sets (docs/update.md, "Sets") --
#
# A set is a label on a card file: which group it comes in. `update -s`
# adds sets to a card, and the images below are choices of sets. Every
# card file has exactly one; plan() refuses a file it cannot place, so
# a new app cannot ship without someone saying where it goes. Sets
# depend only on `base`, which every card has.
#
# (name, title, description), in the order people see them.
SETS = [
    ("base",         "Base",
     "the shell, settings, the reader, documents and fonts"),
    ("productivity", "Productivity",
     "sheet, slide, print, convert, draw, view, automate"),
    ("audio",        "Audio",       "music, MIDI, trackers and speech"),
    ("net",          "Network",     "web, IRC, BBS, mesh and servers"),
    ("dev",          "Development", "the C compiler, FPGA tools, hex editor"),
    ("embedded",     "Embedded",
     "BASIC, bench, logic, I2C, serial and zlink"),
    ("games",        "Games",       "arcade, casino, chess, chip8 and more"),
    ("arklite",      "Ark Lite",
     "an offline library: the Ark Codex and selected texts"),
]
SET_NAMES = [n for n, _t, _d in SETS]

# Programs (/apps/<name>) by set. An app's own data, /data/<name>/...,
# goes with it.
APP_SET = {
    "posix": "base", "vi": "base", "zetta": "base", "settings": "base",
    "info": "base", "calc": "base", "clock": "base", "cal": "base",
    "keyboard": "base", "jfont": "base", "read": "base", "ask": "base",
    "update": "base", "backup": "base",
    "sheet": "productivity", "slide": "productivity", "print": "productivity",
    "convert": "productivity", "draw": "productivity", "view": "productivity",
    "automate": "productivity",
    "play": "audio", "midi": "audio", "mmod": "audio", "track": "audio",
    "tts": "audio", "audiotest": "audio",
    "web": "net", "irc": "net", "netserve": "net", "zerdesk": "net",
    "mesh": "net", "bbs": "net", "fed": "net",
    "zcc": "dev", "zfpga": "dev", "hex": "dev", "cryptobench": "dev",
    "hello_win": "dev", "portdemo": "dev", "ttytest": "dev",
    "basic": "embedded", "i2c": "embedded", "ls99": "embedded",
    "bench": "embedded", "logic": "embedded", "sechs": "embedded",
    "serial": "embedded", "zlink": "embedded",
    "casino": "games", "poker": "games", "roulette": "games",
    "blackjack": "games", "slots": "games", "craps": "games",
    "snake": "games", "blocks": "games", "bricks": "games",
    "mausrun": "games", "chess": "games", "kidgames": "games",
    "chip8": "games", "space3d": "games", "gpu3d": "games",
    "gpudemo": "games",
}
# /data/<name> folders that are not an app's: the core apps' (in flash,
# their data on the card).
DATA_SET = {"net": "base"}
# Everything else, by where it goes on the card: (folder, set).
PLACE_SET = [
    (zcard("Z_DIR_DOCS"), "base"),
    (zcard("Z_DIR_SYS"), "base"),
    (zcard("Z_DIR_ARK"), "base"),             # the ARK scroll
    (zcard("Z_DIR_MEDIA_AUDIO"), "audio"),
    (zcard("Z_DIR_MEDIA_IMAGES"), "productivity"),
]
# The ask packs an update knows, by set. Others (arkmed) are only on an
# image: not in its manifest, never touched by an update.
PACK_SET = {"zdocs": "base", "arklite": "arklite"}


def set_of(dest):
    """The set of card file `dest` ("apps/sheet"). FatError if none."""
    parts = dest.strip("/").split("/")
    apps, data = zcard("Z_DIR_APPS"), zcard("Z_DIR_DATA")
    if len(parts) == 2 and parts[0] == apps and parts[1] in APP_SET:
        return APP_SET[parts[1]]
    if len(parts) >= 3 and parts[0] == data:
        if parts[1] in APP_SET:
            return APP_SET[parts[1]]
        if parts[1] in DATA_SET:
            return DATA_SET[parts[1]]
    d = dest.strip("/")
    for place, st in PLACE_SET:
        if d.startswith(place + "/"):
            return st
    raise FatError("%s has no set: add it to APP_SET, DATA_SET or PLACE_SET "
                   "in release/lib/mkfatimg.py (docs/update.md, \"Sets\")" % dest)


# The images a release builds: (key, file stem, sets, ask packs). The
# packs in PACK_SET come with their set; the others are the image's
# alone. zeitlos-arkmedium is published on GitHub only, not on the
# distribution's server (docs/distribution.md).
_ALL = tuple(SET_NAMES)
VARIANTS = [
    ("base",      "zeitlos-base",      ("base",), ("zdocs",)),
    ("serious",   "zeitlos-serious",
     tuple(n for n in _ALL if n != "games"), ("zdocs", "arklite")),
    ("full",      "zeitlos",           _ALL, ("zdocs", "arklite")),
    ("arkmedium", "zeitlos-arkmedium",
     tuple(n for n in _ALL if n != "arklite"), ("zdocs", "arkmed")),
]
SERVER_VARIANTS = ("base", "serious", "full")


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
# and FREE_MB left over for the person -- /home, zcc output, text
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

# Programs, to /apps under their natural names: the directory name in
# sw/apps, which is also the name `run` and the dock use. /apps is flat
# and the kernel finds a bare name there before the flash archive
# (docs/layout.md, "Finding a program").
# Everything in sw/apps that is not a CORE app.
#
# Core apps (CORE_APPS in the top-level Makefile: `make core-apps`)
# live in flash and must NOT be duplicated here: a card copy would
# shadow the flash copy, which is then never updated by a release.
# build() refuses a list that names one.
#
# Everything else ships. The lists below are grouped for reading only;
# nothing depends on which group an app is in.
SUPPLEMENTAL = [
    (zcard("Z_DIR_APPS", "sheet"), "sw/apps/sheet/sheet.bin"),
    (zcard("Z_DIR_APPS", "slide"), "sw/apps/slide/slide.bin"),
    (zcard("Z_DIR_APPS", "print"), "sw/apps/print/print.bin"),
    (zcard("Z_DIR_APPS", "convert"), "sw/apps/convert/convert.bin"),
    (zcard("Z_DIR_APPS", "read"), "sw/apps/read/read.bin"),
    (zcard("Z_DIR_APPS", "ask"), "sw/apps/ask/ask.bin"),
    (zcard("Z_DIR_APPS", "draw"), "sw/apps/draw/draw.bin"),
    (zcard("Z_DIR_APPS", "info"), "sw/apps/info/info.bin"),
    (zcard("Z_DIR_APPS", "calc"), "sw/apps/calc/calc.bin"),
    (zcard("Z_DIR_APPS", "clock"), "sw/apps/clock/clock.bin"),
    (zcard("Z_DIR_APPS", "cal"), "sw/apps/cal/cal.bin"),
    (zcard("Z_DIR_APPS", "settings"), "sw/apps/settings/settings.bin"),
    (zcard("Z_DIR_APPS", "track"), "sw/apps/track/track.bin"),
    (zcard("Z_DIR_APPS", "view"), "sw/apps/view/view.bin"),
    (zcard("Z_DIR_APPS", "web"), "sw/apps/web/web.bin"),
    (zcard("Z_DIR_APPS", "irc"), "sw/apps/irc/irc.bin"),
    (zcard("Z_DIR_APPS", "netserve"), "sw/apps/net/netserve/netserve.bin"),
    (zcard("Z_DIR_APPS", "zerdesk"), "sw/apps/zerdesk/zerdesk.bin"),
    (zcard("Z_DIR_APPS", "hex"), "sw/apps/hex/hex.bin"),
    (zcard("Z_DIR_APPS", "play"), "sw/apps/play/play.bin"),
    (zcard("Z_DIR_APPS", "midi"), "sw/apps/midi/midi.bin"),
    (zcard("Z_DIR_APPS", "mmod"), "sw/apps/mmod/mmod.bin"),
    (zcard("Z_DIR_APPS", "sechs"), "sw/apps/sechs/sechs.bin"),
    (zcard("Z_DIR_APPS", "bench"), "sw/apps/bench/bench.bin"),
    (zcard("Z_DIR_APPS", "ls99"), "sw/apps/ls99/ls99.bin"),
    (zcard("Z_DIR_APPS", "i2c"), "sw/apps/i2c/i2c.bin"),
    (zcard("Z_DIR_APPS", "zlink"), "sw/apps/zlink/zlink.bin"),
    (zcard("Z_DIR_APPS", "logic"), "sw/apps/logic/logic.bin"),
    (zcard("Z_DIR_APPS", "serial"), "sw/apps/serial/serial.bin"),
    (zcard("Z_DIR_APPS", "mesh"), "sw/apps/mesh/mesh.bin"),
    (zcard("Z_DIR_APPS", "tts"), "sw/apps/tts/tts.bin"),
    (zcard("Z_DIR_APPS", "jfont"), "sw/apps/jfont/jfont.bin"),
    (zcard("Z_DIR_APPS", "keyboard"), "sw/apps/keyboard/keyboard.bin"),
    (zcard("Z_DIR_APPS", "automate"), "sw/apps/automate/automate.bin"),
    (zcard("Z_DIR_APPS", "cryptobench"), "sw/apps/cryptobench/cryptobench.bin"),
    # Brings the card up to date with a newer release (docs/update.md).
    # On the card, so it updates itself along with everything else.
    (zcard("Z_DIR_APPS", "update"), "sw/apps/update/update.bin"),
    # Dated backups of the card's files on a second drive (docs/backup.md).
    (zcard("Z_DIR_APPS", "backup"), "sw/apps/backup/backup.bin"),
]

# The BBS and its zfed node (docs/bbs.md, docs/fed.md). Their data is in
# /data/bbs and /data/fed -- NETWORK_FILES below: a BBS that runs as soon as it is
# started, and a node that runs alone until it is given a network.
NETWORK = [
    (zcard("Z_DIR_APPS", "bbs"), "sw/apps/bbs/bbs.bin"),
    (zcard("Z_DIR_APPS", "fed"), "sw/apps/fed/fed.bin"),
]

# The casino. One dock icon (apps/casino) launches the rest, so the
# games have to be on the card even though nothing on the dock points
# at them directly -- z_proc_run() resolves them by name from here.
CASINO = [
    (zcard("Z_DIR_APPS", "casino"), "sw/apps/casino/casino.bin"),
    (zcard("Z_DIR_APPS", "poker"), "sw/apps/poker/poker.bin"),
    (zcard("Z_DIR_APPS", "roulette"), "sw/apps/roulette/roulette.bin"),
    (zcard("Z_DIR_APPS", "blackjack"), "sw/apps/blackjack/blackjack.bin"),
    (zcard("Z_DIR_APPS", "slots"), "sw/apps/slots/slots.bin"),
    (zcard("Z_DIR_APPS", "craps"), "sw/apps/craps/craps.bin"),
]

GAMES_DEMOS = [
    (zcard("Z_DIR_APPS", "space3d"), "sw/apps/space3d/space3d.bin"),
    (zcard("Z_DIR_APPS", "snake"), "sw/apps/snake/snake.bin"),
    (zcard("Z_DIR_APPS", "blocks"), "sw/apps/blocks/blocks.bin"),
    (zcard("Z_DIR_APPS", "bricks"), "sw/apps/bricks/bricks.bin"),
    (zcard("Z_DIR_APPS", "mausrun"), "sw/apps/mausrun/mausrun.bin"),
    (zcard("Z_DIR_APPS", "gpu3d"), "sw/apps/gpu3d/gpu3d.bin"),
    (zcard("Z_DIR_APPS", "gpudemo"), "sw/apps/gpudemo/gpudemo.bin"),
    (zcard("Z_DIR_APPS", "chip8"), "sw/apps/chip8/chip8.bin"),
    (zcard("Z_DIR_APPS", "chess"), "sw/apps/chess/chess.bin"),
    (zcard("Z_DIR_APPS", "kidgames"), "sw/apps/kidgames/kidgames.bin"),
    (zcard("Z_DIR_APPS", "basic"), "sw/apps/basic/basic.bin"),
]

MISC = [
    (zcard("Z_PATH_WEB_ROOTS"), "sw/apps/web/roots.der"),
    (zcard("Z_DIR_APPS", "portdemo"), "sw/apps/portdemo/portdemo.bin"),
    (zcard("Z_DIR_APPS", "hello_win"), "sw/apps/hello_win/hello_win.bin"),
    (zcard("Z_DIR_APPS", "audiotest"), "sw/apps/audiotest/audiotest.bin"),
]

# The shell a term window connects to that lives on the card: posix
# (about 4MB of RAM, and its compiler's runtime is on the card). The
# other, repl, is a core app in flash (docs/flash_apps.md, "Why repl is
# a core app"), so it is not here. posix is here rather than in SELFHOST
# because that is what it is to a user; the compiler and editor it
# hosts stay below.
SHELLS = [
    (zcard("Z_DIR_APPS", "posix"), "sw/apps/posix/posix.bin"),
]

# The self-hosting set: a compiler and an editor, driven from posix.
#
# These are what make the card able to extend itself rather than only
# run what was cross-compiled onto it (docs/posix.md). `zcc` is useless
# without libz/ below -- see LIBZ_FILES.
SELFHOST = [
    (zcard("Z_DIR_APPS", "zcc"), "sw/apps/zcc/zcc.bin"),
    (zcard("Z_DIR_APPS", "zfpga"), "sw/apps/zfpga/zfpga.bin"),
    (zcard("Z_DIR_APPS", "vi"), "sw/apps/vi/vi.bin"),
    (zcard("Z_DIR_APPS", "zetta"), "sw/apps/zetta/zetta.bin"),
    (zcard("Z_DIR_APPS", "ttytest"), "sw/apps/ttytest/ttytest.bin"),
]

# The speech pack: the pronunciation lexicon and the recorded voice
# sw/apps/tts reads (docs/tts.md). NOT built from this tree and NOT
# committed to it -- tools/speech builds it from public-domain sources,
# and it is published with the release -- which is why it is looked for
# rather than required. Without it, speech still works: the built-in
# dictionary and letter-to-sound rules, and the formant voice.
#
# SPEECH_PACK in the environment ships a pack built elsewhere.
SPEECH_PACK = "tools/speech/build/en/speech.zspk"
SPEECH_DEST = P["Z_PATH_TTS_PACK"]   # tts's PACK_PATH (sw/apps/tts/pack.h)

# -- ask packs --
#
# Shipped BY DEFAULT: a release carries `ask` and the data it needs,
# because an app that boots to "no packs in /data/ask" looks broken
# rather than incomplete.
#
# Each pack contributes /opt/ark/<name> and /data/ask/<name>, copied
# from tools/ask/out/<name>. Override or disable with the environment:
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

# The two halves of a pack: its documents, and ask's index of them
# (docs/layout.md, "Ask and Ark"). tools/ask lays ASK_OUT/<pack>/ out
# exactly as the card is -- ASK_OUT/<pack>/opt/ark/<pack> and
# ASK_OUT/<pack>/data/ask/<pack> -- taking both from the same registry,
# so each half goes to the card at the path it already has.
PACK_HALVES = (zcard("Z_DIR_ARK"), zcard("Z_DIR_ASK_PACKS"))
# What `zrelease sdcard` (and so tools/mkfatimg.sh) puts on a card when
# nothing says otherwise: the development card, unchanged from before
# variants. A release builds VARIANTS instead.
ASK_PACKS_DEFAULT = ("zdocs", "arklite")

# -- the zcc runtime, in zcc's folder: /data/zcc/libz --
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
    (zcard("Z_DIR_ZCC_LIBZ", "libz.bin"), "sw/apps/zcc/libz/libz.bin"),
    (zcard("Z_DIR_ZCC_LIBZ", "libz.sym"), "sw/apps/zcc/libz/libz.sym"),
    (zcard("Z_DIR_ZCC_INCLUDE", "libz.h"), "sw/apps/zcc/libz/libz.h"),
]

LIBZ_HEADER_DIRS = [
    (zcard("Z_DIR_ZCC_INCLUDE"), "sw/common", (".h",)),
    (zcard("Z_DIR_ZCC_INCLUDE"), "sw/apps/zcc/include", (".h",)),
]

# zeitlos.h generates its syscall enum from this by X-macro, so it is a
# header in everything but name and extension.
LIBZ_EXTRA = [
    (zcard("Z_DIR_ZCC_INCLUDE", "syscalls.def"), "sw/common/syscalls.def"),
]

# -- the zfpga chip databases, in zfpga's folder --
#
# zfpga looks in /data/zfpga by default, the way zcc looks in
# /data/zcc/libz, so the layout is fixed and nothing needs to be
# typed. One .zdb per die: lfe5u-25f.zdb also serves the 12F (identical tilegrid, docs/zfpga.md
# sec. 2.2). 45F and 85F databases will join this list when their
# vendored data does (sw/apps/zfpga/ext/prjtrellis-db/README.zeitlos.md).
# Without one, zfpga refuses at the .device line and names the file it
# looked for.
FPGA_FILES = [
    # The name zfpga opens (sw/apps/zfpga/db.c). It used to be shortened
    # from lfe5u-25f.zdb because a nine-character name could not be
    # opened when FatFs was built without long names (docs/zfpga.md
    # sec. 22). Long names work now; the lookup name is unchanged.
    (zcard("Z_DIR_ZFPGA_DB", "lfe5u25f.zdb"), "sw/apps/zfpga/db/lfe5u25f.zdb"),
    # the 45F: Mozart ML1 and ML2, Sergei ML1 and ML2, Schoko, Noir
    (zcard("Z_DIR_ZFPGA_DB", "lfe5u45f.zdb"), "sw/apps/zfpga/db/lfe5u45f.zdb"),
    # board profiles and their pins: `zfpga build design.v -b lakritz`
    (zcard("Z_DIR_ZFPGA_DB", "boards/lakritz.brd"), "sw/apps/zfpga/db/boards/lakritz.brd"),
    (zcard("Z_DIR_ZFPGA_DB", "boards/lakritz.lpf"), "sw/apps/zfpga/db/boards/lakritz.lpf"),
    (zcard("Z_DIR_ZFPGA_DB", "boards/obst.brd"), "sw/apps/zfpga/db/boards/obst.brd"),
    (zcard("Z_DIR_ZFPGA_DB", "boards/obst.lpf"), "sw/apps/zfpga/db/boards/obst.lpf"),
    (zcard("Z_DIR_ZFPGA_DB", "boards/mozart1.brd"), "sw/apps/zfpga/db/boards/mozart1.brd"),
    (zcard("Z_DIR_ZFPGA_DB", "boards/mozart1.lpf"), "sw/apps/zfpga/db/boards/mozart1.lpf"),
    (zcard("Z_DIR_ZFPGA_DB", "boards/sergei1.brd"), "sw/apps/zfpga/db/boards/sergei1.brd"),
    (zcard("Z_DIR_ZFPGA_DB", "boards/sergei1.lpf"), "sw/apps/zfpga/db/boards/sergei1.lpf"),
    (zcard("Z_DIR_ZFPGA_DB", "boards/mozart0.brd"), "sw/apps/zfpga/db/boards/mozart0.brd"),
    (zcard("Z_DIR_ZFPGA_DB", "boards/sergei0.brd"), "sw/apps/zfpga/db/boards/sergei0.brd"),
    (zcard("Z_DIR_ZFPGA_DB", "boards/mozart2.brd"), "sw/apps/zfpga/db/boards/mozart2.brd"),
    (zcard("Z_DIR_ZFPGA_DB", "boards/mozart2.lpf"), "sw/apps/zfpga/db/boards/mozart2.lpf"),
    (zcard("Z_DIR_ZFPGA_DB", "boards/sergei2.brd"), "sw/apps/zfpga/db/boards/sergei2.brd"),
    (zcard("Z_DIR_ZFPGA_DB", "boards/sergei2.lpf"), "sw/apps/zfpga/db/boards/sergei2.lpf"),
    (zcard("Z_DIR_ZFPGA_DB", "boards/konfekt.brd"), "sw/apps/zfpga/db/boards/konfekt.brd"),
    (zcard("Z_DIR_ZFPGA_DB", "boards/konfekt.lpf"), "sw/apps/zfpga/db/boards/konfekt.lpf"),
    (zcard("Z_DIR_ZFPGA_DB", "boards/minze.brd"), "sw/apps/zfpga/db/boards/minze.brd"),
    (zcard("Z_DIR_ZFPGA_DB", "boards/minze.lpf"), "sw/apps/zfpga/db/boards/minze.lpf"),
    (zcard("Z_DIR_ZFPGA_DB", "boards/schoko.brd"), "sw/apps/zfpga/db/boards/schoko.brd"),
    (zcard("Z_DIR_ZFPGA_DB", "boards/schoko.lpf"), "sw/apps/zfpga/db/boards/schoko.lpf"),
    (zcard("Z_DIR_ZFPGA_DB", "boards/noir.brd"), "sw/apps/zfpga/db/boards/noir.brd"),
    (zcard("Z_DIR_ZFPGA_DB", "boards/noir.lpf"), "sw/apps/zfpga/db/boards/noir.lpf"),
    (zcard("Z_DIR_ZFPGA_DB", "boards/klinge.brd"), "sw/apps/zfpga/db/boards/klinge.brd"),
    (zcard("Z_DIR_ZFPGA_DB", "boards/klinge.lpf"), "sw/apps/zfpga/db/boards/klinge.lpf"),
]

# Something to build with zfpga: docs/zfpga-test.md walks through
# these. Their own list because they are examples -- a person may edit
# them where they are, so an update keeps an edited one (KEEP, below).
FPGA_EXAMPLES = [
    (zcard("Z_DIR_ZFPGA_DB", "examples/blink.v"), "sw/apps/zfpga/db/examples/blink.v"),
    (zcard("Z_DIR_ZFPGA_DB", "examples/blinkf.v"), "sw/apps/zfpga/db/examples/blinkf.v"),
    (zcard("Z_DIR_ZFPGA_DB", "examples/empty.zn"), "sw/apps/zfpga/db/examples/empty.zn"),
    (zcard("Z_DIR_ZFPGA_DB", "examples/on.zn"), "sw/apps/zfpga/db/examples/on.zn"),
    (zcard("Z_DIR_ZFPGA_DB", "examples/blink.zn"), "sw/apps/zfpga/db/examples/blink.zn"),
    (zcard("Z_DIR_ZFPGA_DB", "examples/hand.zl"), "sw/apps/zfpga/db/examples/hand.zl"),
    # two modules, a parameter and an `include, blinking at half blink's
    # rate: zfpga synth's hierarchy, checked on the board (docs/zfpga.md
    # sec. 24)
    (zcard("Z_DIR_ZFPGA_DB", "examples/blinkh.v"), "sw/apps/zfpga/db/examples/blinkh.v"),
    (zcard("Z_DIR_ZFPGA_DB", "examples/blinkh.vh"), "sw/apps/zfpga/db/examples/blinkh.vh"),
]

# Something to compile.
#
# A card with a compiler and no example is a card where the first
# thing anyone does is guess at the include paths. These are small and
# they are the three stages: no runtime, the runtime, and output that
# reaches the terminal rather than the serial console.
EXAMPLES = [
    (zcard("Z_DIR_BENCH_EXAMPLES", "panel.net"), "sw/apps/bench/examples/panel.net"),
    (zcard("Z_DIR_BENCH_EXAMPLES", "basicpanel.net"), "sw/apps/bench/examples/basicpanel.net"),
    (zcard("Z_DIR_BENCH_EXAMPLES", "realmodule.net"), "sw/apps/bench/examples/realmodule.net"),
    (zcard("Z_DIR_BASIC_PROGRAMS", "BLINK.BAS"), "sw/apps/bench/examples/BLINK.BAS"),
    (zcard("Z_DIR_BENCH_EXAMPLES", "growlight.net"), "sw/apps/bench/examples/growlight.net"),
    (zcard("Z_DIR_BENCH_EXAMPLES", "modpanel.net"), "sw/apps/bench/examples/modpanel.net"),
    (zcard("Z_DIR_BASIC_PROGRAMS", "GROW.BAS"), "sw/apps/bench/examples/GROW.BAS"),
    (zcard("Z_DIR_BASIC_PROGRAMS", "PANEL.BAS"), "sw/apps/bench/examples/PANEL.BAS"),
    (zcard("Z_DIR_ZCC_EXAMPLES", "hello.c"), "sw/apps/zcc/examples/hello.c"),
    (zcard("Z_DIR_ZCC_EXAMPLES", "hello_libz.c"), "sw/apps/zcc/examples/hello_libz.c"),
    (zcard("Z_DIR_ZCC_EXAMPLES", "hello_term.c"), "sw/apps/zcc/examples/hello_term.c"),
]

# The configuration file, in /sys where the kernel reads it
# (sw/os/cfg.c, docs/config.md). /sys/version is written beside it by
# build(). Every setting in it is commented out,
# so a fresh card behaves exactly as if the file were absent -- it is
# there as the documented place to start editing, not to set anything.
CONFIG_FILES = [
    (zcard("Z_PATH_SYS_CONFIG"), "sw/data/zeitlos.cfg"),
    # net's settings (docs/networking.md, docs/esp32link.md), the same
    # kind of template: every line commented out. A file with nothing
    # set behaves exactly as no file -- netcfg_load() only notes that
    # one was there -- so shipping it changes no board's network; it is
    # the documented place to start, as zeitlos.cfg is.
    (zcard("Z_PATH_NET_CONFIG"), "sw/data/net.cfg"),
]

# The BBS's and fed's data (NETWORK above), where each looks for it
# (apps.bbs.dir, apps.fed.dir: /data/bbs and /data/fed). Unlike zeitlos.cfg, the
# BBS's settings are live -- a board's values, so that `run bbs` gives a
# working local BBS at once; fed.cfg names no network until one is
# joined. Copied with CONFIG_FILES.
NETWORK_FILES = [
    (zcard("Z_DIR_BBS", "bbs.cfg"), "sw/apps/bbs/data/bbs-board.cfg"),
    (zcard("Z_DIR_BBS", "forums.cfg"), "sw/apps/bbs/data/forums.cfg"),
    (zcard("Z_DIR_BBS", "bulletins/01-welcome.txt"), "sw/apps/bbs/data/bulletins/01-welcome.txt"),
    (zcard("Z_DIR_BBS", "text/README.txt"), "sw/apps/bbs/data/text/README.txt"),
    (zcard("Z_DIR_FED", "fed.cfg"), "sw/apps/fed/data/fed.cfg"),
]

# The remote desktop's viewer page, where sw/apps/zerdesk reads it on
# every request (docs/remote_desktop.md). zerdesk carries no copy of
# its own: without this file it serves nothing but a plain-text reason.
# It is the ESP32's page, the one file both paths serve
# (esp32/zeitlos-nic embeds it). Edit it on the card to change what the
# browser gets from this machine; the ESP32's copy is built in.
# Outside netserve's www on purpose: netserve serves that directory,
# and there the page would open its WebSocket to netserve.
DESK_FILES = [
    (zcard("Z_PATH_ZERDESK_PAGE"), "esp32/zeitlos-nic/web/index.html"),
]

# Fonts drawn in software (docs/text_encoding.md, "Japanese"): the 12x12
# Japanese font sw/apps/jfont holds for every app. Committed to the tree
# (tools/gen_jfont.py makes it from public-domain Shinonome), so it is
# required like the config file rather than looked for like the speech
# pack.
FONT_FILES = [
    (zcard("Z_PATH_JFONT_FONT"), "sw/data/font/jp12.zfn"),
    (zcard("Z_PATH_SYS_FONTS_NOTICE"), "sw/data/font/hershey/NOTICE"),
]

# zcanvas's fonts (docs/fonts.md), to /sys/fonts -- no app owns them
# (docs/layout.md, rule 3): every .zhf and .zfn
# tools/gen_fonts.py made, committed to the tree. Listed from the folders
# (the set is the generator's, not hand-kept here); the list is still
# fixed when the module loads, so a missing folder fails the build like
# any other missing input.
_FONT_ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                           "..", ".."))
for _sub, _ext in (("zhf", ".zhf"), ("zfn", ".zfn")):
    _d = os.path.join(_FONT_ROOT, "sw", "data", "font", _sub)
    for _n in sorted(os.listdir(_d)) if os.path.isdir(_d) else []:
        if _n.endswith(_ext):
            FONT_FILES.append((zcard("Z_DIR_SYS_FONTS", _n),
                               "sw/data/font/%s/%s" % (_sub, _n)))

# Tracker modules, from sw/data/audio. Whatever is there is shipped --
# a glob rather than a list, because these are data files somebody
# drops in, not build products with a Makefile rule each.
#
# To /media/audio, which sw/apps/track, play and midi list first, before
# /home (docs/layout.md).
AUDIO_DIR = "sw/data/audio"
AUDIO_EXT = ".mod"

# The demos (docs/demo.md): sw/apps/automate's scripts, in automate's
# folder, and the media they show off, in /media with the rest of the
# card's examples. Every file is listed, and a missing one
# fails the build: a card whose demo has silently lost its picture, or
# still carries last week's script, is worse than no card. The .pgm
# and .svg are made by sw/data/demo/gen_media.py and committed. Only
# things Zeitlos really does: the photo is a photo, the SVG is drawn
# by view's own renderer on the board.
DEMO_FILES = [
    (zcard_dir("Z_PATH_AUTOMATE_DEFAULT") + "/demo.zds",     "sw/data/demo/demo.zds"),
    (zcard_dir("Z_PATH_AUTOMATE_DEFAULT") + "/short.zds",    "sw/data/demo/short.zds"),
    (zcard_dir("Z_PATH_AUTOMATE_DEFAULT") + "/long.zds",     "sw/data/demo/long.zds"),
    (zcard_dir("Z_PATH_AUTOMATE_DEFAULT") + "/store.zds",    "sw/data/demo/store.zds"),
    (zcard_dir("Z_PATH_AUTOMATE_DEFAULT") + "/lang.zds",     "sw/data/demo/lang.zds"),
    (zcard_dir("Z_PATH_AUTOMATE_DEFAULT") + "/mandel.c",     "sw/data/demo/mandel.c"),
    (zcard_dir("Z_PATH_AUTOMATE_DEFAULT") + "/slide.zds",    "sw/data/demo/slide.zds"),
    (zcard_dir("Z_PATH_AUTOMATE_DEFAULT") + "/slide_demo.zsl", "sw/data/demo/slide_demo.zsl"),
    (zcard("Z_DIR_MEDIA_IMAGES", "squirrel.pgm"), "sw/data/demo/squirrel.pgm"),
    (zcard("Z_DIR_MEDIA_IMAGES", "zeitlos.svg"),  "sw/data/demo/zeitlos.svg"),
    (zcard("Z_DIR_MEDIA_AUDIO",  "lvb11.mid"),    "sw/data/audio/lvb11.mid"),
    (zcard("Z_DIR_MEDIA_IMAGES", "squirrel.jpg"), "sw/data/images/squirrel.jpg"),
]

# slide's tour, opened by /tour (docs/slide_app.md). Its picture is the
# squirrel in DEMO_FILES, by its card path.
SLIDE_FILES = [
    (zcard("Z_PATH_SLIDE_TOUR"), "sw/data/slide/tour.zsl"),
]
# and the example decks, small apps with scripts (docs/slide_scripting.md):
# every .zsl in sw/data/slide/examples, listed when the module loads
_SLIDE_EX = os.path.join(_FONT_ROOT, "sw", "data", "slide", "examples")
for _n in sorted(os.listdir(_SLIDE_EX)) if os.path.isdir(_SLIDE_EX) else []:
    if _n.endswith(".zsl"):
        SLIDE_FILES.append((zcard("Z_DIR_SLIDE_EXAMPLES", _n), "sw/data/slide/examples/" + _n))

# mausrun's levels (docs/mausrun.md, "Levels"): every .txt in
# sw/data/mausrun/levels, in order. Level 1 is also compiled in.
MAUSRUN_FILES = []
_MAUSRUN_LV = os.path.join(_FONT_ROOT, "sw", "data", "mausrun", "levels")
for _n in sorted(os.listdir(_MAUSRUN_LV)) if os.path.isdir(_MAUSRUN_LV) else []:
    if _n.endswith(".txt"):
        MAUSRUN_FILES.append((zcard("Z_DIR_MAUSRUN_LEVELS", _n), "sw/data/mausrun/levels/" + _n))

# chip8's own ROMs, where its open dialog starts (docs/chip8_app.md).
# An XO-CHIP demo; at 65,000 bytes it picks its own profile, so the
# folder needs no chip8.cfg.
CHIP8_FILES = [
    (zcard("Z_DIR_CHIP8_ROMS", "redoct.ch8"), "sw/data/chip8/roms/redoct.ch8"),
]

# Folders a card has even with nothing in them. /home is the person's,
# and a release leaves it empty (docs/layout.md, rule 4). /data and
# the rest are made by the files that go in them; /tmp by the kernel,
# and only on a board without a ramdisk.
EMPTY_DIRS = [zcard("Z_DIR_HOME")]

TOOLS = ["mkfs.fat", "fsck.fat", "mmd", "mcopy"]


class FatError(Exception):
    pass


def tree_version(root):
    """Z_OS_VERSION from sw/common/zversion.h, or "unknown".

    Read here rather than through build.py, which this module does not
    otherwise need; and not fatal, because the card tests build images
    from scratch trees that carry no zversion.h.
    """
    try:
        with open(os.path.join(root, "sw", "common", "zversion.h")) as f:
            m = re.search(r'#define\s+Z_OS_VERSION\s+"([^"]*)"', f.read())
        return m.group(1) if m else "unknown"
    except OSError:
        return "unknown"


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
    with an app and no data -- which boots to "no packs in /data/ask" and
    looks like the app is broken.
    """
    base = os.path.join(root, ASK_OUT, name)
    out = []
    for half in PACK_HALVES:
        d = os.path.join(base, half, name)
        if not os.path.isdir(d):
            raise FatError(
                "ask pack '%s' was requested but %s does not exist.\n"
                "  Build it first:  ./tools/ask/ask build "
                "tools/ask/dist/%s.spec" % (name, d, name))
        for dp, _dn, fn in os.walk(d):
            for f in sorted(fn):
                src = os.path.join(dp, f)
                rel = os.path.relpath(src, d).replace(os.sep, "/")
                out.append(("/%s/%s/%s" % (half, name, rel), src))
    return sorted(out)


# FatFs as this tree builds it (sw/os/fs/fatfs/ffconf.h). create_name()
# in ff.c is what decides whether the board can open a component.
# mcopy stores a long name either way, and finding out on the board
# that the file is not the one we asked for is the failure this check
# exists to move in front of mkfs.
#
# FF_USE_LFN is 1: the working buffer is static, in .bss. Not 2. The
# kernel stack is the resource that has actually run out, and FatFs is
# only entered from one syscall at a time (the note above f_stat() in
# sw/os/fs/fs.c, and docs/sdcard.md). FF_MAX_LFN is 255 UTF-16 code
# units, the longest name the specification allows and the longest
# create_name() will open. FF_LFN_BUF is 255 bytes of the API encoding,
# which is what a directory listing can report: a name whose UTF-8 is
# longer still opens by the path that created it, and is listed under
# its 8.3 alias (sw/common/zfs.h). This check is the open. The API
# encoding is UTF-8 (FF_LFN_UNICODE 2). FF_FS_RPATH is 0, so "." and
# ".." are not special entries: trailing dots are snipped and what is
# left has to be a name.
FF_MAX_LFN = 255
# create_name() rejects these when they are ASCII. '\\' is a separator
# (IsSeparator), so it never becomes part of a component either.
_LFN_ILLEGAL = set(ord(c) for c in '*:<>|"?\x7f')


def _utf8_next(buf, i):
    """One code point, decoded the way tchar2uni() does for UTF-8.

    Raises ValueError on the sequences tchar2uni() turns into
    0xFFFFFFFF. An over-long encoding that still decodes to a scalar
    at or above U+0080 is accepted, because FatFs accepts it; one that
    decodes below U+0080 is not.
    """
    if i >= len(buf):
        raise ValueError("truncated utf-8")
    uc = buf[i]
    i += 1
    if uc < 0x80:
        return uc, i
    if (uc & 0xE0) == 0xC0:
        uc &= 0x1F
        nf = 1
    elif (uc & 0xF0) == 0xE0:
        uc &= 0x0F
        nf = 2
    elif (uc & 0xF8) == 0xF0:
        uc &= 0x07
        nf = 3
    else:
        raise ValueError("bad utf-8")
    for _ in range(nf):
        if i >= len(buf):
            raise ValueError("truncated utf-8")
        b = buf[i]
        i += 1
        if (b & 0xC0) != 0x80:
            raise ValueError("bad utf-8")
        uc = (uc << 6) | (b & 0x3F)
    if uc < 0x80 or 0xD800 <= uc <= 0xDFFF or uc >= 0x110000:
        raise ValueError("bad utf-8")
    return uc, i


def _component_problem(raw):
    """None if create_name() would accept this component, else why not.

    `raw` is the component's bytes, with no slash in it. The length is
    counted in UTF-16 units, and a scalar above U+FFFF takes two: the
    high half is stored before the length check and the low half still
    needs a unit of its own, which is the test in create_name().
    Trailing spaces and dots are snipped after the length check, so
    they count, and a name that snips down to nothing is rejected.
    """
    i = 0
    di = 0
    cps = []
    while i < len(raw):
        try:
            cp, i = _utf8_next(raw, i)
        except ValueError:
            return "not valid UTF-8"
        # create_name() stops at any unit below 0x20, and a C string
        # ends at NUL, so the board would open a shorter name than the
        # one mcopy stored. '\\' splits the path the same way.
        if cp < 0x20 or cp == 0x5C or cp == 0x2F:
            return "contains a separator or a control character"
        if cp < 0x80 and cp in _LFN_ILLEGAL:
            return "contains a character FatFs rejects in a long name"
        if cp >= 0x10000:
            di += 1
            if di >= FF_MAX_LFN:
                return "longer than %d UTF-16 units" % FF_MAX_LFN
        if di >= FF_MAX_LFN:
            return "longer than %d UTF-16 units" % FF_MAX_LFN
        di += 1
        cps.append(cp)
    while cps and cps[-1] in (0x20, 0x2E):
        cps.pop()
    if not cps:
        return "empty once trailing spaces and dots are removed"
    return None


def check_card_path(dest):
    """Every component of a card path, against FatFs as built.

    A duplicated slash is not a component: follow_path() collapses
    those. Raises FatError before anything is formatted.
    """
    if isinstance(dest, str):
        raw = dest.encode("utf-8")
        shown = dest
    else:
        raw = bytes(dest)
        shown = raw.decode("utf-8", "replace")
    for part in raw.strip(b"/").split(b"/"):
        if not part:
            continue
        problem = _component_problem(part)
        if problem:
            raise FatError(
                "'%s' cannot go on the card (%s). FatFs here is "
                "FF_USE_LFN 1, FF_MAX_LFN %d, UTF-8 "
                "(sw/os/fs/fatfs/ffconf.h); mcopy would store a name "
                "the board cannot open" % (shown, problem, FF_MAX_LFN))


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


# -- what an update does with each file (docs/update.md) --
#
# Every file a card ships has a policy, carried into the manifest
# (/sys/manifest on the card, <name>/manifest in an update release):
#
#   REPLACE  the card copy is made to match the release. Programs, the
#            zcc runtime and headers, docs, fonts, chip databases, media.
#   KEEP     files a person edits where they are: configuration and
#            examples. Written when missing. Replaced only while the
#            card copy is still exactly what an earlier release shipped
#            (its hash is the one in /sys/manifest); an edited copy is
#            never touched.
#
# The policy is declared per LIST, below, so a new list has to choose.
REPLACE = "r"
KEEP = "k"

# The ask packs are not in the manifest (docs/update.md, "Not yet"):
# an update leaves /opt/ark/<pack> and /data/ask/<pack> alone. The ARK
# scroll is: a few small .md files every card has.


class Plan(object):
    """Everything a card holds, checked, before anything is written.

    files  [(dest, src_abs, policy)] in the order the image copies them;
           dest has no leading slash ("apps/files"). Packs are copied
           after the first `early` of them, which keeps the image's
           directory order what it always was.
    packs  [(packname, [(card_path, src_abs)])]
    dirs   folders a card has even when empty (dest form)
    """

    def __init__(self):
        self.files = []
        self.early = 0
        self.packs = []
        self.dirs = []
        self.sizes = []
        self.sets = []          # the set names this card or release has
        self.set_of = {}        # dest -> set, for every manifest file
        self.listed_packs = []  # packs whose files are in the manifest

    def manifest_files(self):
        """The files an update knows about: the card's files and the
        packs in PACK_SET, as (dest, src_abs, policy)."""
        out = list(self.files)
        for packname, files in self.packs:
            if packname in self.listed_packs:
                out += [(card.lstrip("/"), src, REPLACE) for card, src in files]
        return out

    def offered_sets(self):
        """SETS entries this plan has files in, in order."""
        have = set(self.set_of.values())
        return [s for s in SETS if s[0] in have]


def app_inputs():
    """Every (card path, source) list of programs, in image order."""
    return SUPPLEMENTAL + NETWORK + CASINO + GAMES_DEMOS + MISC + SHELLS + SELFHOST


def data_inputs():
    """Every (card path, source) list of required data files.

    One place, used by plan() and by the tests' scratch tree, so a new
    list cannot be added to one and not the other.
    """
    return (LIBZ_FILES + LIBZ_EXTRA + CONFIG_FILES + NETWORK_FILES
            + FONT_FILES + DESK_FILES + EXAMPLES + FPGA_FILES
            + FPGA_EXAMPLES + DEMO_FILES + CHIP8_FILES + SLIDE_FILES
            + MAUSRUN_FILES)


def plan(root, ark_dir=None, packs=None, verbose=True, sets=None):
    """Gather and check everything a card holds. Writes nothing.

    `sets`: the set names to include (SETS); None, all of them. An ask
    pack in PACK_SET must be in one of them.

    The one card list, shared by build() (the image) and by the update
    release (release/lib/store.py). Raises FatError for anything
    missing, listed twice, or unopenable by FatFs.
    """
    ark_dir = ark_dir or os.path.join(root, "sw/data/ark")

    apps = app_inputs()

    # A name listed twice copies once and then fails, halfway through a
    # 64MB image, with mcopy's own silence for an error message. Said
    # here instead.
    seen = set()
    dupes = sorted(n for n, _ in apps if n in seen or seen.add(n))
    if dupes:
        raise FatError("listed more than once: %s" % ", ".join(dupes))

    # No core app on the card: the card copy would win over the flash
    # one (docs/flash_apps.md), and a release never updates it.
    import spec
    core = set(spec.core_apps(root)[0])
    pre = zcard("Z_DIR_APPS", "x")[:-1]	# "apps/"
    on_card = sorted(n for n, _ in apps
                     if n.startswith(pre) and n[len(pre):] in core)
    if on_card:
        raise FatError("core apps must not be on the card (they are in "
                       "flash; CORE_APPS in the Makefile): %s"
                       % ", ".join(on_card))

    data_lists = data_inputs()

    # Check every input up front. Finding out that mausrun.bin was
    # never built after formatting a 64MB image and copying twelve
    # other files is a slower way to learn the same thing.
    #
    # libz.bin and libz.sym come from a separate make (sw/apps/zcc
    # builds them as a dependency), and a card whose zcc cannot find
    # them compiles only freestanding programs -- so their absence is
    # an error here rather than a quiet omission.
    missing = [p for _, p in apps + data_lists
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
        print("    WARNING: the demos (/demo, docs/demo.md) switch to the recorded")
        print("          voice, which is IN the speech pack: without it they speak")
        print("          in the synthesised voice throughout. Build it with")
        print("          tools/speech/speech (docs/tts_data.md), or set SPEECH_PACK.")
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

    pl = Plan()

    def add(dest, src_abs, policy):
        pl.files.append((dest, src_abs, policy))

    def add_list(lst, policy):
        for dest, rel in lst:
            add(dest, os.path.join(root, rel), policy)

    # -- the order the image has always been written in --
    add_list(apps, REPLACE)
    for a in audio:
        add(zcard("Z_DIR_MEDIA_AUDIO", a), os.path.join(audio_src, a), REPLACE)
    add_list(DEMO_FILES, REPLACE)
    if speech:
        add(SPEECH_DEST.lstrip("/"), speech, REPLACE)
    for d in docs:
        add(zcard("Z_DIR_DOCS", d), os.path.join(root, "docs", d), REPLACE)
    for a in ark:
        add(zcard("Z_DIR_ARK", a), os.path.join(ark_dir, a), REPLACE)
    pl.early = len(pl.files)        # the ask packs go here in the image
    add_list(LIBZ_FILES + LIBZ_EXTRA, REPLACE)
    add_list(EXAMPLES, KEEP)
    add_list(FPGA_FILES, REPLACE)
    add_list(FPGA_EXAMPLES, KEEP)
    add_list(CONFIG_FILES + NETWORK_FILES, KEEP)
    add_list(FONT_FILES, REPLACE)
    add_list(DESK_FILES, KEEP)      # "can be edited in place" (remote_desktop.md)
    add_list(CHIP8_FILES, REPLACE)
    add_list(SLIDE_FILES, KEEP)     # the person may have edited the tour
    add_list(MAUSRUN_FILES, REPLACE)  # the game's own levels (docs/mausrun.md)
    # Headers by directory rather than by name: the set is "whatever
    # sw/common exports", and a list here would go stale silently --
    # the symptom being a missing include on the device, long after.
    for dest, srcdir, exts in LIBZ_HEADER_DIRS:
        d = os.path.join(root, srcdir)
        for h in sorted(os.listdir(d)):
            if h.endswith(exts):
                add("%s/%s" % (dest, h), os.path.join(d, h), REPLACE)

    pl.dirs = list(EMPTY_DIRS)

    # -- sets: every file has one; keep those of the sets asked for --
    want = list(SET_NAMES) if sets is None else list(sets)
    for st in want:
        if st not in SET_NAMES:
            raise FatError("no set `%s` (have: %s)" % (st, ", ".join(SET_NAMES)))
    if "base" not in want:
        raise FatError("every card has the base set")
    pl.sets = [n for n in SET_NAMES if n in want]
    every = {dest: set_of(dest) for dest, _src, _p in pl.files}
    keep = [i for i, f in enumerate(pl.files) if every[f[0]] in pl.sets]
    pl.early = sum(1 for i in keep if i < pl.early)
    pl.files = [pl.files[i] for i in keep]
    pl.set_of = {f[0]: every[f[0]] for f in pl.files}

    # Every destination once, across every list: an update could not
    # say which of two sources a path is.
    seen = set()
    dupes = sorted(d for d, _s, _p in pl.files
                   if d.lower() in seen or seen.add(d.lower()))
    if dupes:
        raise FatError("card path listed more than once: %s"
                       % ", ".join(dupes))

    # -- ask packs: resolve them BEFORE anything is formatted --
    for packname in (ask_packs_requested() if packs is None else packs):
        files = ask_pack_files(root, packname)
        pl.packs.append((packname, files))
        st = PACK_SET.get(packname)
        if st is None:
            continue            # the image's alone (arkmed)
        if st not in pl.sets:
            raise FatError("ask pack %s is in the set %s, which this card "
                           "does not have" % (packname, st))
        pl.listed_packs.append(packname)
        for card, _src in files:
            pl.set_of[card.lstrip("/")] = st

    # Every name, checked BEFORE formatting. mcopy would store a
    # component create_name() rejects, and the board would not open
    # it; finding that out halfway through the image is the failure
    # this function is arranged to avoid. Headers are included because
    # their set is "whatever sw/common exports", which is also where
    # zsubjects.h (nine characters, one dot) stopped the old 8.3 check
    # after the image had already been formatted.
    for dest, _s, _p in pl.files:
        check_card_path("/" + dest)
    for _packname, files in pl.packs:
        for card, _src in files:
            check_card_path(card)

    pl.sizes = [os.path.getsize(src) for _n, fs in pl.packs for _c, src in fs]
    pl.sizes += [os.path.getsize(src) for _d, src, _p in pl.files]
    return pl


def build(root, out_path, ark_dir=None, verbose=True, packs=None,
          name=None, sets=None):
    """Build the SD card image. Returns a list of (name, size) shipped.

    `packs` names the ask packs to ship; None means
    ask_packs_requested() (the environment, or ASK_PACKS_DEFAULT). A
    pack ships as a directory and is listed as one entry,
    "opt/ark/<pack>/", rather than as its tens of thousands of files --
    MANIFEST.json would otherwise carry every Wikipedia article's
    number.

    `sets` names the sets to put on the card (SETS; None, all of them);
    /sys/manifest says which it has (docs/update.md, "Sets").

    `name` is the release name /sys/manifest records ("v" + the tree's
    version when not given), so an update knows what the card is.
    """
    preflight()
    pl = plan(root, ark_dir=ark_dir, packs=packs, verbose=verbose, sets=sets)

    sizes = pl.sizes
    nfiles_pack = sum(len(fs) for _n, fs in pl.packs)
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

    shipped = []

    def copy(src_abs, dest):
        mkdir_p(dest.rpartition("/")[0])
        _run(["mcopy", "-i", out_path, src_abs, "::" + dest])
        shipped.append((dest.lstrip("/"), os.path.getsize(src_abs)))

    made_dirs = set()

    def mkdir_p(path):
        """mmd each missing level. Every directory on the card is made
        here, when the first file that needs it is copied -- there is
        no separate list of directories to keep in step with the
        files, or with sw/common/zpaths.h."""
        if not path.strip("/"):
            return              # the root: always there
        parts = path.strip("/").split("/")
        for i in range(1, len(parts) + 1):
            sub = "/".join(parts[:i])
            if sub in made_dirs:
                continue
            _run(["mmd", "-i", out_path, "::/%s" % sub])
            made_dirs.add(sub)

    for dest, src, _p in pl.files[:pl.early]:
        copy(src, "/" + dest)

    # -- ask packs (resolved in plan()) --
    # Packs go in as whole directory trees, one `mcopy -s` per tree.
    # Copying file by file is a subprocess per file, each re-reading the
    # FAT: fine for arklite's thousand files, hours for arkmed's tens of
    # thousands. Every name was checked against FatFs before mkfs.
    for packname, files in pl.packs:
        base = os.path.join(root, ASK_OUT, packname)
        total = sum(os.path.getsize(src) for _c, src in files)
        for half in PACK_HALVES:
            mkdir_p("/" + half)
            _run(["mcopy", "-s", "-i", out_path,
                  os.path.join(base, half, packname), "::/%s/" % half])
            made_dirs.add("%s/%s" % (half, packname))
        shipped.append(("%s/" % zcard("Z_DIR_ARK", packname), total))
        if verbose:
            print("  ask pack %-10s %6d files  %7.1f MB"
                  % (packname, len(files), total / 1e6))

    for dest, src, _p in pl.files[pl.early:]:
        copy(src, "/" + dest)

    # /sys/manifest: what this card holds and what an update may do
    # with each file (docs/update.md); /sys/version beside it, which
    # release wrote the card (docs/layout.md). Neither is in the
    # manifest -- the updater writes them itself, last. The tree's
    # Z_OS_VERSION, which `zrelease build` has already checked against
    # the version it was asked for.
    import manifest as manifest_mod
    version = tree_version(root)
    m = manifest_mod.make(pl, name or ("v" + version), version,
                          manifest_mod.describe(root),
                          packs=[n for n, _f in pl.packs
                                 if n not in pl.listed_packs],
                          abi=manifest_mod.read_abi(root),
                          installed=pl.sets)
    for text, dest in ((m.text, P["Z_PATH_SYS_MANIFEST"]),
                       (manifest_mod.version_text(m), P["Z_PATH_SYS_VERSION"])):
        tmpf = out_path + ".sysfile"
        with open(tmpf, "w") as f:
            f.write(text)
        copy(tmpf, dest)
        os.unlink(tmpf)

    for d in pl.dirs:
        mkdir_p("/" + d)

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
