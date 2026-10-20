#!/usr/bin/env python3
#
# Zeitlos release tooling -- the update manifest and the update release.
#
#   python3 release/lib/test_store.py
#
# No toolchain and no image tools needed: the apps are faked, as in
# test_mkfatimg.py (whose scratch tree this borrows), and nothing is
# formatted -- except the one check that a card image's /sys/manifest
# matches the update release made from the same tree, which needs
# mtools and is skipped without them.
#
# What it checks:
#
#   - the manifest: round trip, sorted and unique, content-only
#     snapshot ids, damage and newer formats refused, odd paths;
#   - every card file has a policy, and the configuration and examples
#     are KEEP;
#   - publishing: every object present, named by and holding its hash,
#     every name a board asks TFTP for within 63 bytes;
#   - publishing again: unchanged, nothing rewritten;
#   - a changed file: one new object, the old manifest kept as
#     manifest.prev with its objects, and pruned one publish later;
#   - LATEST and INDEX; a bad name refused;
#   - `zrelease tftp` end to end, from the command line;
#   - an image's /sys/manifest has the same snapshot id.
#

import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True

import manifest as mf      # noqa: E402
import mkfatimg            # noqa: E402
import store               # noqa: E402
import test_mkfatimg       # noqa: E402

FAILS = []


def check(cond, what):
    print("  %s  %s" % ("ok  " if cond else "FAIL", what))
    if not cond:
        FAILS.append(what)


def raises(fn, exc, what):
    try:
        fn()
        check(False, what)
    except exc:
        check(True, what)


def test_manifest():
    print("manifest")
    files = [("apps/b", "b" * 64, 2, "r", "0000000b"), ("apps/a", "a" * 64, 1, "r", "0000000a"),
             ("data/my notes.txt", "c" * 64, 3, "k", "0000000c")]
    m = mf.build("v0.0.7", "0.0.7", "abc", ["home"], files,
                 made="2026-10-19T00:00:00Z")
    back = mf.parse(m.text)
    check([f[0] for f in back.files] == ["apps/a", "apps/b", "data/my notes.txt"],
          "files sorted by path; a path with a space survives")
    check(back.id == m.id and len(m.id) == mf.ID_DIGITS, "snapshot id read back")
    check(back.dirs == ["home"] and back.name == "v0.0.7", "dirs and name read back")
    m2 = mf.build("other", "0.0.8", "def", ["home"], list(reversed(files)),
                  made="2027-01-01T00:00:00Z")
    check(m2.id == m.id, "the id is content only: name, source, time do not change it")
    m3 = mf.build("v0.0.7", "0.0.7", "abc", ["home"],
                  files[:1] + [("apps/a", "a" * 64, 1, "k", "0000000a")] + files[2:],
                  made="2026-10-19T00:00:00Z")
    check(m3.id != m.id, "a policy change is a new snapshot")
    raises(lambda: mf.parse(m.text.replace("f " + "a" * 64, "f " + "d" * 64)),
           mf.ManifestError, "a changed hash no longer matches the id")
    raises(lambda: mf.parse(m.text.replace("zeitlos-manifest 3", "zeitlos-manifest 4")),
           mf.ManifestError, "a newer format is refused")
    raises(lambda: mf.parse("hello\n"), mf.ManifestError, "not a manifest")
    lines = m.text.splitlines()
    swapped = "\n".join(lines[:-3] + [lines[-2], lines[-3], lines[-1]]) + "\n"
    raises(lambda: mf.parse(swapped), mf.ManifestError, "unsorted entries refused")
    for bad in ("/apps/a", "apps/../sys", "apps/", "apps//a", " a"):
        raises(lambda b=bad: mf.build("n", "v", "s", [], [(b, "a" * 64, 1, "r", "00000000")]),
               mf.ManifestError, "path %r refused" % bad)
    check("my notes.txt" in m.text and m.text.endswith("\n"), "text form ends in a newline")
    # Format 1 (round 0, no CRC) is still read: cards and stores have it.
    old = ("zeitlos-manifest 1\nname v0\nversion 0.0.7\nsnapshot %s\n"
           "f %s 5 r apps/x\n" % (mf.snapshot_id([], [("apps/x", "e" * 64, 5, "r", None)], 1),
                                  "e" * 64))
    o = mf.parse(old)
    check(o.format == 1 and o.files[0][4] is None and o.files[0][2] == 5,
          "a format-1 manifest is read (no CRC)")
    check(m.text.startswith("zeitlos-manifest 3\n") and " 0000000a 1 r apps/a\n" in m.text,
          "format 3 writes the CRC after the SHA-256, as format 2 did")
    m2l = "zeitlos-manifest 2\n" + m.text.split("\n", 1)[1]
    o2 = mf.parse(m2l)
    check(o2.format == 2 and o2.id == m.id, "a format-2 manifest is read; same snapshot id")
    check(mf.object_path("ab" + "0" * 62, 2) == "o/ab/ab" + "0" * 18 and
          mf.object_path("ab" + "0" * 62) == "o/ab/ab" + "0" * 18 + ".lz",
          "format 3's objects are .lz, format 2's are not")
    # LZ4 blocks: sizes around the block boundary, and what is not compressible
    import random as _r
    rnd = _r.Random(3)
    for n in (0, 1, 15, 16, 17, 32767, 32768, 32769, 65536, 100001):
        txt = (b"the quick brown fox " * 6000)[:n]
        noise = bytes(rnd.getrandbits(8) for _ in range(n))
        for d in (txt, noise):
            z = mf.lz_blocks(d)
            ok = mf.lz_unblock(z, n) == d
            if not ok:
                break
        if not ok:
            break
    check(ok, "LZ4 blocks round-trip, 0 to 100001 bytes, text and noise")
    check(len(mf.lz_blocks(b"x" * 100000)) < 2000, "text shrinks")
    check(len(mf.lz_blocks(noise)) == len(noise) + 4 * 4, "noise is stored, 4 bytes a block")
    raises(lambda: mf.build("n", "v", "s", [], [("apps/x", "a" * 64, 1, "r", "xyz")]),
           mf.ManifestError, "a CRC that is not 8 hex digits is refused")
    vt = mf.version_text(m)
    check(vt.startswith("zeitlos 0.0.7\nsnapshot v0.0.7 %s " % m.id),
          "/sys/version: the old first line, then the snapshot")


def test_policies(root):
    print("policies")
    pl = mkfatimg.plan(root, packs=())
    pol = {d: p for d, _s, p in pl.files}
    check(all(p in ("r", "k") for p in pol.values()), "every file has a policy")
    zc = mkfatimg.zcard
    for d in (zc("Z_PATH_SYS_CONFIG"), zc("Z_PATH_NET_CONFIG"),
              zc("Z_DIR_BBS", "bbs.cfg"), zc("Z_DIR_FED", "fed.cfg"),
              zc("Z_PATH_ZERDESK_PAGE"), zc("Z_DIR_ZCC_EXAMPLES", "hello.c")):
        check(pol.get(d) == "k", "%s is KEEP" % d)
    for d in (zc("Z_DIR_APPS", "sheet"), zc("Z_DIR_DOCS", "update.md"),
              zc("Z_DIR_ZCC_LIBZ", "libz.bin")):
        check(pol.get(d) == "r", "%s is REPLACE" % d)
    check(not [d for d in pol if d.startswith(zc("Z_DIR_ASK_PACKS") + "/")],
          "no ask pack files in the plan")
    check(zc("Z_DIR_HOME") in pl.dirs, "/home is a listed folder")
    return pl


def publish(root, st_dir, name="v0.0.7", latest=True):
    pl = mkfatimg.plan(root, packs=())
    m = mf.make(pl, name, "0.0.7", "test")
    return m, store.publish(st_dir, m, store.sources_for(pl, m), latest=latest)


def content(st_dir, sha, fmt=mf.FORMAT, size=None):
    """What an object holds, decoded if it is LZ4 blocks."""
    with open(os.path.join(st_dir, mf.object_path(sha, fmt)), "rb") as f:
        d = f.read()
    return mf.lz_unblock(d, size) if fmt >= 3 else d


def objects(st_dir):
    out = set()
    for dp, _dn, fn in os.walk(os.path.join(st_dir, "o")):
        out.update(fn)
    return out


def test_store(root):
    print("publishing")
    tmp = tempfile.mkdtemp(prefix="store-")
    st_dir = os.path.join(tmp, "release")
    try:
        m, st = publish(root, st_dir)
        uniq = len(set(f[1] for f in m.files))
        check(st.objects_new == uniq and not st.unchanged,
              "%d objects written for %d files" % (st.objects_new, len(m.files)))
        bad = [p for p, sha, size, _, _c in m.files
               if hashlib.sha256(content(st_dir, sha, size=size)).hexdigest() != sha]
        check(not bad, "every object holds the bytes its name says")
        import zlib
        badc = [p for p, sha, size, _, c in m.files
                if "%08x" % (zlib.crc32(content(st_dir, sha, size=size))
                             & 0xFFFFFFFF) != c]
        check(not badc, "and its CRC-32 is zlib's")
        some = max(m.files, key=lambda f: f[2])
        p = os.path.join(st_dir, mf.object_path(some[1]))
        check(mf.lz_check(p, some[2]) and os.path.getsize(p) < some[2],
              "objects are LZ4 blocks that add up to the file, and smaller "
              "(%s: %d -> %d)" % (some[0], some[2], os.path.getsize(p)))
        check(open(p, "rb").read() == mf.lz_blocks(content(st_dir, some[1], size=some[2])),
              "compressed deterministically")
        check(all(mf.lz_check(os.path.join(st_dir, mf.object_path(f[1])), f[2])
                  for f in m.files), "every object's blocks add up")
        longest = max(len("release/" + mf.object_path(f[1])) for f in m.files)
        check(longest <= 63, "longest TFTP name %d bytes (net takes 63)" % longest)
        with open(os.path.join(st_dir, "v0.0.7", "manifest")) as f:
            on_disk = mf.parse(f.read())
        check(on_disk.id == m.id, "the manifest is in place")
        check(open(os.path.join(st_dir, "LATEST")).read() == "v0.0.7\n", "LATEST")
        idx = open(os.path.join(st_dir, "INDEX")).read().splitlines()
        check(len(idx) == 2 and idx[1].startswith("v0.0.7 %s 0.0.7 test " % m.id),
              "INDEX has the release")
        mode = os.stat(os.path.join(st_dir, mf.object_path(m.files[0][1]))).st_mode
        check(mode & 0o004, "objects are world-readable")
        boot = [f for f in m.files if f[0] == store.BOOT_PATH]
        bcopy = os.path.join(st_dir, "v0.0.7", "update")
        check(boot and os.path.isfile(bcopy) and mf.sha256_file(bcopy) == boot[0][1],
              "release/v0.0.7/update is the release's own apps/update")
        check(len("release/v0.0.7/update") <= 63, "a name a TFTP request can carry")

        print("publishing again")
        made = on_disk.header["made"]
        m2, st2 = publish(root, st_dir)
        check(st2.unchanged and st2.objects_new == 0, "unchanged, nothing written")
        with open(os.path.join(st_dir, "v0.0.7", "manifest")) as f:
            check(mf.parse(f.read()).header["made"] == made,
                  "the manifest is left alone (its time too)")
        check(not os.path.exists(os.path.join(st_dir, "v0.0.7", "manifest.prev")),
              "no manifest.prev for an unchanged publish")

        print("a rebuilt app")
        app = os.path.join(root, "sw/apps/sheet/sheet.bin")
        first = open(app, "rb").read()
        old_sha = [f[1] for f in m.files if f[0] == "apps/sheet"][0]
        with open(app, "wb") as f:
            f.write(first + b" rebuilt")
        m3, st3 = publish(root, st_dir)
        check(st3.objects_new == 1 and st3.previous_id == m.id and m3.id != m.id,
              "one new object, snapshot %s -> %s" % (m.id, m3.id))
        check(old_sha[:mf.OBJECT_DIGITS] + ".lz" in objects(st_dir),
              "the old object stays while manifest.prev names it")
        prev = mf.parse(open(os.path.join(st_dir, "v0.0.7", "manifest.prev")).read())
        check(prev.id == m.id, "manifest.prev is the replaced one")
        with open(app, "wb") as f:
            f.write(first + b" again")
        m4, st4 = publish(root, st_dir)
        check(st4.pruned == 1 and old_sha[:mf.OBJECT_DIGITS] + ".lz" not in objects(st_dir),
              "pruned once nothing names it")
        with open(app, "wb") as f:
            f.write(first)

        print("a format-2 store, republished")
        # As round 1 left a store: format-2 manifest, raw objects.
        st2 = os.path.join(tmp, "old")
        pl = mkfatimg.plan(root, packs=())
        m_new = mf.make(pl, "v0.0.7", "0.0.7", "test")
        lines = m_new.text.split("\n", 1)[1]
        m_old = mf.parse("zeitlos-manifest 2\n" + lines)
        os.makedirs(os.path.join(st2, "v0.0.7"))
        for _p, sha, _s, _pol, _c in m_old.files:
            o = os.path.join(st2, mf.object_path(sha, 2))
            os.makedirs(os.path.dirname(o), exist_ok=True)
            shutil.copyfile(store.sources_for(pl, m_old)[sha], o)
        open(os.path.join(st2, "v0.0.7", "manifest"), "w").write(m_old.text)
        stx = store.publish(st2, m_new, store.sources_for(pl, m_new))
        cur = mf.parse(open(os.path.join(st2, "v0.0.7", "manifest")).read())
        check(not stx.unchanged and cur.format == 3,
              "same snapshot, but rewritten as format 3")
        leaves = objects(st2)
        nraw = sum(1 for x in leaves if not x.endswith(".lz"))
        ngz = sum(1 for x in leaves if x.endswith(".lz"))
        check(nraw and ngz, "raw objects stay while manifest.prev (format 2) "
              "names them (%d raw, %d LZ4)" % (nraw, ngz))
        store.publish(st2, mf.make(pl, "v0.0.7", "0.0.7", "test2"),
                      store.sources_for(pl, m_new))
        # unchanged now: manifest.prev is still the format-2 one
        os.unlink(os.path.join(st2, "v0.0.7", "manifest.prev"))
        store.publish(st2, mf.make(pl, "v0.0.7", "0.0.7", "test3"),
                      store.sources_for(pl, m_new))
        check(not [x for x in objects(st2) if not x.endswith(".lz")],
              "and are pruned once nothing names them")

        print("names and LATEST")
        _m5, _st5 = publish(root, st_dir, name="v0.0.7-audio", latest=False)
        check(open(os.path.join(st_dir, "LATEST")).read() == "v0.0.7\n",
              "--no-latest leaves LATEST alone")
        idx = open(os.path.join(st_dir, "INDEX")).read()
        check("v0.0.7-audio " in idx and "\nv0.0.7 " in idx, "INDEX lists both")
        for bad in ("o", "../x", "a" * 33, "-x", "a b"):
            raises(lambda b=bad: store.check_name(b), store.StoreError,
                   "name %r refused" % bad)
        # An object of the wrong size is damage, not something to skip.
        some = os.path.join(st_dir, mf.object_path(m4.files[0][1]))
        with open(some, "ab") as f:
            f.write(b"x")
        raises(lambda: publish(root, st_dir), store.StoreError,
               "a damaged object is reported")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_cli(root):
    print("zrelease tftp, from the command line")
    # zrelease takes ROOT from its own location (abspath, not realpath),
    # so a symlinked release/ makes the scratch tree its root.
    os.symlink(os.path.join(ROOT, "release"), os.path.join(root, "release"))
    # the LZ4 compressor the store uses (manifest.lz_blocks())
    os.symlink(os.path.join(ROOT, "tools", "mkzar.py"),
               os.path.join(root, "tools", "mkzar.py"))
    for h in ("zpaths.h", "zversion.h"):
        dst = os.path.join(root, "sw", "common", h)
        if not os.path.exists(dst):
            os.symlink(os.path.join(ROOT, "sw", "common", h), dst)
    tmp = tempfile.mkdtemp(prefix="cli-")
    try:
        dest = os.path.join(tmp, "release")
        r = subprocess.run([sys.executable, os.path.join(root, "release", "zrelease"),
                            "tftp", "--dest", dest], capture_output=True, text=True)
        out = r.stdout + r.stderr
        check(r.returncode == 0, "zrelease tftp exits 0" +
              ("" if r.returncode == 0 else ": " + out[-500:]))
        check("run update v" in out and "-t tftp" in out,
              "prints the command for the board")
        check(os.path.isfile(os.path.join(dest, "LATEST")), "the store was written")
        r = subprocess.run([sys.executable, os.path.join(root, "release", "zrelease"),
                            "tftp", "--dest", dest], capture_output=True, text=True)
        check(r.returncode == 0 and "unchanged" in r.stdout, "a second run says unchanged")
        r = subprocess.run([sys.executable, os.path.join(root, "release", "zrelease"),
                            "tftp", "--dest", os.path.join(tmp, "dry"), "--dry-run"],
                           capture_output=True, text=True)
        check(r.returncode == 0 and not os.path.exists(os.path.join(tmp, "dry")),
              "--dry-run writes nothing")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_image(root):
    print("an image's /sys/manifest")
    if any(shutil.which(t) is None for t in mkfatimg.TOOLS):
        print("  skip  needs dosfstools and mtools")
        return
    tmp = tempfile.mkdtemp(prefix="img-")
    try:
        img = os.path.join(tmp, "c.img")
        mkfatimg.build(root, img, verbose=False, packs=())
        back = os.path.join(tmp, "manifest")
        subprocess.run(["mcopy", "-n", "-i", img,
                        "::" + mkfatimg.P["Z_PATH_SYS_MANIFEST"], back],
                       check=True, capture_output=True)
        m_img = mf.parse(open(back).read())
        pl = mkfatimg.plan(root, packs=())
        m_rel = mf.make(pl, "v0.0.7", "0.0.7", "x")
        check(m_img.id == m_rel.id,
              "same snapshot id as the update release (%s)" % m_img.id)
        listed = set(f[0] for f in m_img.files)
        on_card = test_mkfatimg.card_files(img)
        missing = [p for p in listed if "/" + p.lower() not in on_card]
        check(not missing, "every listed file is on the card")
        # card_files() lists folders too, ending in "/"
        files_on_card = set(p for p in on_card if not p.endswith("/"))
        unlisted = sorted(files_on_card - {"/" + x.lower() for x in listed}
                          - {"/sys/manifest", "/sys/version"})
        check(not unlisted, "every file on the card is listed%s"
              % ("" if not unlisted else " (not: %s)" % unlisted[:3]))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    root = test_mkfatimg.fake_root()
    try:
        test_manifest()
        test_policies(root)
        test_store(root)
        test_image(root)
        test_cli(root)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print()
    if FAILS:
        print("%d FAILED" % len(FAILS))
        return 1
    print("all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
