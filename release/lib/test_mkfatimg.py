#!/usr/bin/env python3
#
# Zeitlos release tooling -- the card image builder, for real.
#
#   python3 release/lib/test_mkfatimg.py
#
# selftest.py and test_incremental.py stub mkfatimg.build() out, which
# is right for them and means nothing there ever formats a card. This
# does: mkfs.fat, mcopy and fsck.fat on real images, read back with
# mtools. It needs dosfstools and mtools, and the ask packs it ships
# built (`ask build dist/zdocs.spec`, and arklite for that variant);
# anything missing is skipped and says so, rather than failing.
#
# The apps are FAKED -- a scratch tree with a few bytes in place of
# every .bin, because this checks what the builder does with files,
# not the files. Docs, ARK and the packs are the real ones, linked in.
#
# What it checks:
#
#   - every variant builds, fsck-clean, at the size plan_size() chose;
#   - a pack arrives complete: every file, at its card path, same size;
#   - the manifest lists a pack as ONE entry, not tens of thousands;
#   - a name that is not 8.3 fails BEFORE anything is formatted;
#   - the large-image path (8KB clusters) formats and holds a pack --
#     forced with a lowered threshold, since writing a real 1GB image
#     here would test the disk more than the builder.
#

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True

import mkfatimg  # noqa: E402

FAILS = []


def check(cond, what):
    print("  %s  %s" % ("ok  " if cond else "FAIL", what))
    if not cond:
        FAILS.append(what)


def fake_root():
    """A scratch tree: fake binaries, real docs / ark / packs."""
    r = tempfile.mkdtemp(prefix="fatimg-")
    lists = (mkfatimg.SUPPLEMENTAL + mkfatimg.CASINO + mkfatimg.GAMES_DEMOS
             + mkfatimg.MISC + mkfatimg.SHELLS + mkfatimg.SELFHOST
             + mkfatimg.LIBZ_FILES + mkfatimg.LIBZ_EXTRA + mkfatimg.EXAMPLES
             + mkfatimg.FPGA_FILES + mkfatimg.CONFIG_FILES)
    for _dest, rel in lists:
        p = os.path.join(r, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(b"FAKE " + rel.encode())
    for _dest, srcdir, _exts in mkfatimg.LIBZ_HEADER_DIRS:
        os.makedirs(os.path.join(r, srcdir), exist_ok=True)
    os.makedirs(os.path.join(r, "sw", "data"), exist_ok=True)
    os.symlink(os.path.join(ROOT, "docs"), os.path.join(r, "docs"))
    os.symlink(os.path.join(ROOT, "sw", "data", "ark"),
               os.path.join(r, "sw", "data", "ark"))
    os.makedirs(os.path.join(r, "tools", "ask"), exist_ok=True)
    os.symlink(os.path.join(ROOT, mkfatimg.ASK_OUT),
               os.path.join(r, mkfatimg.ASK_OUT))
    return r


def card_files(img):
    """{path: size} of every file on an image, via mdir."""
    out = subprocess.run(["mdir", "-/", "-a", "-b", "-i", img, "::/"],
                         capture_output=True, text=True).stdout
    return set(l.strip()[2:].lower() for l in out.splitlines()
               if l.startswith("::/"))


def pack_built(name):
    return os.path.isdir(os.path.join(ROOT, mkfatimg.ASK_OUT, name, "ask", name))


def test_variant(root, key, tmp):
    _k, stem, packs = mkfatimg.variant(key)
    missing = [p for p in packs if not pack_built(p)]
    if missing:
        print("  skip  %s: pack %s not built" % (key, ", ".join(missing)))
        return
    img = os.path.join(tmp, stem + ".img")
    shipped = mkfatimg.build(root, img, verbose=False, packs=packs)
    size = os.path.getsize(img)
    check(size >= mkfatimg.MIN_SIZE_MB * 1024 * 1024,
          "%s: %d MB image" % (key, size // (1024 * 1024)))
    names = dict(shipped)
    for p in packs:
        check(("ark/%s/" % p) in names,
              "%s: pack %s listed as one entry" % (key, p))
    check(len(shipped) < 400, "%s: manifest has %d entries, not one per "
          "pack file" % (key, len(shipped)))
    on_card = card_files(img)
    for p in packs:
        files = mkfatimg.ask_pack_files(root, p)
        absent = [c for c, _s in files if c.lower() not in on_card]
        check(not absent, "%s: all %d files of %s on the card%s" % (
            key, len(files), p, "" if not absent else
            " (missing e.g. %s)" % absent[0]))
    # Sizes, spot-checked: the largest file of each pack, read back.
    for p in packs:
        files = mkfatimg.ask_pack_files(root, p)
        card, src = max(files, key=lambda f: os.path.getsize(f[1]))
        back = os.path.join(tmp, "back")
        subprocess.run(["mcopy", "-n", "-i", img, "::" + card, back],
                       check=True, capture_output=True)
        with open(back, "rb") as a, open(src, "rb") as b:
            check(a.read() == b.read(), "%s: %s reads back identical"
                  % (key, card))
        os.unlink(back)
    os.unlink(img)


def main():
    if any(shutil.which(t) is None for t in mkfatimg.TOOLS + ["mdir"]):
        print("needs dosfstools and mtools -- skipped")
        return 0
    if not pack_built("zdocs"):
        print("needs the zdocs pack: ./tools/ask/ask build "
              "tools/ask/dist/zdocs.spec -- skipped")
        return 0
    root = fake_root()
    tmp = tempfile.mkdtemp(prefix="fatout-")
    try:
        print("plan_size")
        s, spc = mkfatimg.plan_size([1000] * 10)
        check(s == mkfatimg.MIN_SIZE_MB * 1024 * 1024 and spc == 1,
              "a near-empty card is the %d MB minimum, 512-byte clusters"
              % mkfatimg.MIN_SIZE_MB)
        s, spc = mkfatimg.plan_size([400 * 1024] * 4000)      # ~1.6 GB
        check(spc == mkfatimg.SECTORS_LARGE and s > 1600 * 1024 * 1024,
              "~1.6 GB of content -> %d MB, %d-byte clusters"
              % (s // (1024 * 1024), spc * 512))
        check(s % (mkfatimg.SIZE_STEP_MB * 1024 * 1024) == 0,
              "sizes are whole %d MB steps" % mkfatimg.SIZE_STEP_MB)
        s2, _ = mkfatimg.plan_size([3000] * 30000)
        check(s2 > 30000 * 3000, "slack is counted: 30K small files need "
              "more than their byte total (%d MB)" % (s2 // (1024 * 1024)))

        print("variants")
        for key, _stem, _packs in mkfatimg.VARIANTS:
            test_variant(root, key, tmp)

        print("8.3 names")
        bad = os.path.join(ROOT, mkfatimg.ASK_OUT, "zdocs", "ark", "zdocs",
                           "longfilename.md")
        img = os.path.join(tmp, "bad.img")
        try:
            open(bad, "w").write("x")
            try:
                mkfatimg.build(root, img, verbose=False, packs=("zdocs",))
                check(False, "a long name in a pack is refused")
            except mkfatimg.FatError as e:
                check("8.3" in str(e), "a long name in a pack is refused")
        finally:
            os.unlink(bad)
        check(not os.path.exists(img), "...before anything was formatted")

        print("large-image path (8KB clusters, threshold lowered)")
        saved = mkfatimg.LARGE_IMAGE_BYTES
        mkfatimg.LARGE_IMAGE_BYTES = 32 * 1024 * 1024
        try:
            img = os.path.join(tmp, "large.img")
            mkfatimg.build(root, img, verbose=False, packs=("zdocs",))
            out = subprocess.run(["minfo", "-i", img], capture_output=True,
                                 text=True).stdout
            check("cluster size: 16 sectors" in out,
                  "formatted with 16 sectors per cluster")
            on_card = card_files(img)
            n = len(mkfatimg.ask_pack_files(root, "zdocs"))
            check(sum(1 for f in on_card if f.startswith("/ark/zdocs/")
                      or f.startswith("/ask/zdocs/")) >= n,
                  "the pack is complete on it")
        finally:
            mkfatimg.LARGE_IMAGE_BYTES = saved
    finally:
        shutil.rmtree(root, ignore_errors=True)
        shutil.rmtree(tmp, ignore_errors=True)
    print()
    if FAILS:
        print("%d FAILED" % len(FAILS))
        return 1
    print("all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
