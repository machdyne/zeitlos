#
# Zeitlos release tooling -- the SD card image.
#
# This is tools/mkfatimg.sh with one change: mount + cp becomes mmd +
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
#   - The core apps (wm, net, repl, term) are DELIBERATELY ABSENT.
#     They live in flash, in the ZAR. sw/os/zar.h's rule is that a copy
#     on the card wins over the flash copy, so shipping them here would
#     shadow the flash build -- and for `net` that is not academic:
#     the flash copy is built per target with the right driver
#     (ENC28J60 or RMII) and a card copy could only be built with one
#     of them, so an SD image shared across targets would hand half of
#     them the wrong NIC driver.
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
SUPPLEMENTAL = [
    ("apps/files", "sw/apps/files/files.bin"),
    ("apps/text", "sw/apps/text/text.bin"),
    ("apps/read", "sw/apps/read/read.bin"),
    ("apps/draw", "sw/apps/draw/draw.bin"),
    ("apps/info", "sw/apps/info/info.bin"),
    ("apps/calc", "sw/apps/calc/calc.bin"),
    ("apps/clock", "sw/apps/clock/clock.bin"),
    ("apps/settings", "sw/apps/settings/settings.bin"),
    ("apps/track", "sw/apps/track/track.bin"),
]

GAMES_DEMOS = [
    ("apps/space3d", "sw/apps/space3d/space3d.bin"),
    ("apps/gamedemo", "sw/apps/gamedemo/gamedemo.bin"),
    ("apps/gpu3d", "sw/apps/gpu3d/gpu3d.bin"),
]

MISC = [
    ("apps/portdemo", "sw/apps/portdemo/portdemo.bin"),
]

DIRS = ["apps", "docs", "ark", "user"]

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


def check_against_script(root):
    """Compare this module's file list with tools/mkfatimg.sh's.

    The shell script stays in the tree as the readable reference and
    for one-off use, so there are two lists of what goes on the card.
    Two lists drift. This is not a hypothetical: the version of the
    script in git and the version actually being used had already
    diverged -- the committed one still copied the core apps onto the
    card, which is precisely the thing that would shadow a target's
    per-PHY `net` build with the wrong driver.

    Returns a list of problems; empty means they agree.
    """
    path = os.path.join(root, "tools/mkfatimg.sh")
    if not os.path.exists(path):
        return []

    import re
    with open(path) as f:
        text = f.read()

    # cp sw/apps/foo/foo.bin "$MOUNT_DIR/foo"
    script = {}
    for m in re.finditer(r'^\s*cp\s+(\S+\.bin)\s+"\$MOUNT_DIR/([\w./]+)"',
                         text, re.M):
        script[m.group(2)] = m.group(1)

    mine = dict((n, p) for n, p in SUPPLEMENTAL + GAMES_DEMOS + MISC)

    problems = []
    for name in sorted(set(mine) | set(script)):
        if name not in script:
            problems.append("release/lib/mkfatimg.py ships '%s' but "
                            "tools/mkfatimg.sh does not" % name)
        elif name not in mine:
            problems.append("tools/mkfatimg.sh ships '%s' but "
                            "release/lib/mkfatimg.py does not" % name)
        elif mine[name] != script[name]:
            problems.append("'%s' comes from %s in mkfatimg.py and %s in "
                            "mkfatimg.sh" % (name, mine[name], script[name]))

    # The core apps must not be on the card at all -- see this module's
    # header. Worth checking separately because it is the one difference
    # that is silently harmful rather than merely inconsistent.
    for core in ("wm", "net", "repl", "term",
                 "apps/wm", "apps/net", "apps/repl", "apps/term"):
        if core in script:
            problems.append(
                "tools/mkfatimg.sh copies the core app '%s' onto the card. "
                "sw/os/zar.h gives a card copy precedence over the flash "
                "copy, so this would shadow the per-target build -- and for "
                "'net' that means the wrong NIC driver on some boards."
                % core)
        if core in mine:
            problems.append("release/lib/mkfatimg.py ships core app '%s'"
                            % core)

    return problems


def build(root, out_path, ark_dir=None, verbose=True):
    """Build the SD card image. Returns a list of (name, size) shipped."""
    preflight()

    ark_dir = ark_dir or os.path.join(root, "sw/data/ark")

    apps = SUPPLEMENTAL + GAMES_DEMOS + MISC

    # Check every input up front. Finding out that gamedemo.bin was
    # never built after formatting a 64MB image and copying twelve
    # other files is a slower way to learn the same thing.
    missing = [p for _, p in apps
               if not os.path.exists(os.path.join(root, p))]
    if missing:
        raise FatError("not built yet:\n  %s\n"
                       "  Run the app builds first." % "\n  ".join(missing))

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

    for name, rel in apps:
        copy(os.path.join(root, rel), "/" + name)
    for d in docs:
        copy(os.path.join(root, "docs", d), "/docs/" + d)
    for a in ark:
        copy(os.path.join(ark_dir, a), "/ark/" + a)

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
