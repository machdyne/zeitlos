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
#   - the abi line: written, read, not in the snapshot id, and a new
#     one republishes an otherwise unchanged release;
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
    raises(lambda: mf.parse(m.text.replace("zeitlos-manifest 4", "zeitlos-manifest 5")),
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
    check(m.text.startswith("zeitlos-manifest 4\n") and " 0000000a 1 r base apps/a\n" in m.text,
          "format 4 writes the CRC after the SHA-256, the set after the policy")
    o2 = mf.parse(mf.text_in_format(m, 2))
    check(o2.format == 2 and not o2.set_of, "a format-2 manifest is read, without sets")
    o3 = mf.parse(mf.text_in_format(m, 3))
    check(o3.format == 3 and o3.id == o2.id, "formats 2 and 3 have the same id")
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
    # The abi line (docs/update.md, "ABI"): a header, not content.
    ma = mf.build("v0.0.7", "0.0.7", "abc", ["home"], files,
                  made="2026-10-19T00:00:00Z", abi="03a25f4cbf544cf0")
    check("\nabi 03a25f4cbf544cf0\n" in ma.text and mf.parse(ma.text).abi == "03a25f4cbf544cf0",
          "the abi line is written and read back")
    check(ma.id == m.id and "\nabi " not in m.text and mf.parse(m.text).abi == "",
          "it is not part of the snapshot id; without one there is no line")
    raises(lambda: mf.build("n", "v", "s", [], [], abi="XYZ"), mf.ManifestError,
           "an abi that is not hex is refused")
    tree_abi = mf.read_abi(ROOT)
    check(len(tree_abi) == 16, "read_abi() reads the tree's sw/common/zabi.h")
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
        m_old = mf.parse(mf.text_in_format(m_new, 2))
        os.makedirs(os.path.join(st2, "v0.0.7"))
        for _p, sha, _s, _pol, _c in m_old.files:
            o = os.path.join(st2, mf.object_path(sha, 2))
            os.makedirs(os.path.dirname(o), exist_ok=True)
            shutil.copyfile(store.sources_for(pl, m_old)[sha], o)
        open(os.path.join(st2, "v0.0.7", "manifest"), "w").write(m_old.text)
        stx = store.publish(st2, m_new, store.sources_for(pl, m_new))
        cur = mf.parse(open(os.path.join(st2, "v0.0.7", "manifest")).read())
        check(not stx.unchanged and cur.format == mf.FORMAT,
              "rewritten as format %d" % mf.FORMAT)
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

        print("the same files, another ABI")
        # An interface changed and no binary did: same snapshot id, but
        # the manifest must say the new ABI or `update` checks the old.
        st3 = os.path.join(tmp, "abi")
        pl = mkfatimg.plan(root, packs=())
        store.publish(st3, mf.make(pl, "v0.0.7", "0.0.7", "a", abi="1111"),
                      store.sources_for(pl, m_new))
        sty = store.publish(st3, mf.make(pl, "v0.0.7", "0.0.7", "b", abi="2222"),
                            store.sources_for(pl, m_new))
        cur = mf.parse(open(os.path.join(st3, "v0.0.7", "manifest")).read())
        check(not sty.unchanged and cur.abi == "2222",
              "a new abi rewrites the manifest though the snapshot is the same")
        stz = store.publish(st3, mf.make(pl, "v0.0.7", "0.0.7", "c", abi="2222"),
                            store.sources_for(pl, m_new))
        check(stz.unchanged, "and the same abi again is unchanged")

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


def test_sets():
    print("sets in the manifest (format 4)")
    files = [("apps/a", "a" * 64, 1, "r", "0000000a", "base"),
             ("apps/g", "b" * 64, 2, "r", "0000000b", "games")]
    sets = [("base", "Base", "the shell"), ("games", "Games", "arcade, chess")]
    m = mf.build("v1", "0.0.7", "x", ["home"], files, sets=sets,
                 made="2026-10-21T00:00:00Z")
    back = mf.parse(m.text)
    check(back.set_of == {"apps/a": "base", "apps/g": "games"},
          "each file's set is read back")
    check(back.sets == sets and back.installed is None,
          "a release lists the sets it offers, with titles; no installed line")
    m2 = mf.build("v1", "0.0.7", "x", ["home"],
                  files[:1] + [("apps/g", "b" * 64, 2, "r", "0000000b", "base")],
                  sets=sets, made="2026-10-21T00:00:00Z")
    check(m2.id != m.id, "moving a file to another set is a new snapshot")
    m3 = mf.build("v1", "0.0.7", "x", ["home"], files, installed=["base"],
                  made="2026-10-21T00:00:00Z")
    check(mf.parse(m3.text).installed == ["base"] and m3.id == m.id,
          "a card's installed line; not part of the id")
    raises(lambda: mf.build("n", "v", "s", [], [("apps/x", "a" * 64, 1, "r",
                                                  "00000000", "Games")]),
           mf.ManifestError, "a set name with a capital is refused")
    raises(lambda: mf.parse(m.text.replace(" r games apps/g", " r apps/g")),
           mf.ManifestError, "a format-4 line without a set is refused")
    carried = mf.build("v1", "0.0.7", "x", [], [("apps/old", "c" * 64, 3, "r",
                                                  "0000000c", mf.NO_SET)])
    check(mf.parse(carried.text).set_of["apps/old"] == "-",
          "\"-\": an entry whose set is not known")


def test_sign():
    import ed25519
    print("signatures")
    seed = bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
    check(ed25519.public_key(seed).hex() ==
          "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a" and
          ed25519.sign(seed, b"").hex() ==
          "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
          "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
          "Ed25519 matches RFC 8032's first test vector")
    seed3 = bytes.fromhex("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7")
    sig3 = ed25519.sign(seed3, bytes.fromhex("af82"))
    check(ed25519.public_key(seed3).hex() ==
          "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025" and
          sig3.hex() == "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
          "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a",
          "and its third")
    tmp = tempfile.mkdtemp(prefix="sig-")
    try:
        st_dir = os.path.join(tmp, "release")
        rdir = os.path.join(st_dir, "v1")
        os.makedirs(rdir)
        m = mf.build("v1", "0.0.7", "x", ["home"], [], made="2026-10-21T00:00:00Z")
        open(os.path.join(rdir, "manifest"), "w").write(m.text)
        pub = ed25519.public_key(seed)
        other = ed25519.public_key(seed3)
        check(store.sign(st_dir, "v1", [seed]), "sign() writes manifest.sig")
        check(not store.sign(st_dir, "v1", [seed]), "and again changes nothing")
        check(store.check_sig(st_dir, "v1", [pub]), "the signature checks")
        check(not store.check_sig(st_dir, "v1", [other]), "not by another key")
        store.sign(st_dir, "v1", [seed3, seed])
        sig = open(os.path.join(rdir, "manifest.sig")).read()
        check(sig.count("\ned25519 ") == 1 and store.check_sig(st_dir, "v1", [pub])
              and store.check_sig(st_dir, "v1", [other]),
              "two keys: a line each, either checks (changing keys)")
        open(os.path.join(rdir, "manifest"), "a").write("x")
        check(not store.check_sig(st_dir, "v1", [pub, other]),
              "a changed manifest no longer checks")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_dist():
    import dist
    print("the distribution (release/dist.cfg)")
    d = dist.load()
    check(d.name and d.url.startswith("http://") and d.url.endswith("/"),
          "dist.cfg: %s, %s" % (d.name, d.url))
    hdr = open(os.path.join(ROOT, dist.HEADER)).read()
    check(hdr == dist.header_text(d), "sw/apps/update/zdist.h is current")
    tmp = tempfile.mkdtemp(prefix="dist-")
    old = os.environ.get("ZEITLOS_HOME")
    try:
        cfg = os.path.join(tmp, "dist.cfg")
        open(cfg, "w").write("name = A Fork\nurl = http://example.org:8080/z/release/\n")
        f = dist.load(cfg)
        check(f.name == "A Fork" and not f.keys, "a fork's dist.cfg")
        for bad in ("url = https://example.org/release/\n",
                    "url = http://example.org/release\n",
                    "key = 1234\n", "colour = blue\n"):
            open(cfg, "w").write("name = X\nurl = http://e.org/r/\n" + bad)
            raises(lambda: dist.load(cfg), dist.DistError, "refused: %s" % bad.strip())
        os.environ["ZEITLOS_HOME"] = os.path.join(tmp, "home")
        pub = dist.keygen()
        kp = dist.key_path()
        check(os.path.isfile(kp) and (os.stat(kp).st_mode & 0o777) == 0o600,
              "keygen: a private key, mode 600, in $ZEITLOS_HOME")
        import ed25519
        check(ed25519.public_key(dist.load_seed()).hex() == pub, "and it loads")
        raises(lambda: dist.keygen(), dist.DistError, "an existing key is not replaced")
        os.chmod(kp, 0o644)
        raises(lambda: dist.load_seed(), dist.DistError, "a key others can read is refused")
        os.chmod(kp, 0o600)
        raises(lambda: dist.keygen(os.path.join(ROOT, "release", "x.key")),
               dist.DistError, "a key inside the repository is refused")
        open(cfg, "w").write("name = X\nurl = http://e.org/r/\n")
        check(dist.add_key(pub, cfg) and dist.load(cfg).keys == [pub]
              and not dist.add_key(pub, cfg), "--add: the public key into dist.cfg, once")
        r = subprocess.run([sys.executable, os.path.join(ROOT, "release", "zrelease"),
                            "http", "--dry-run"], capture_output=True, text=True,
                           env=dict(os.environ))
        check(r.returncode == 0 or "not built" in r.stdout + r.stderr,
              "zrelease http --dry-run needs no key")
    finally:
        if old is None:
            os.environ.pop("ZEITLOS_HOME", None)
        else:
            os.environ["ZEITLOS_HOME"] = old
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
        rel = open(os.path.join(dest, "LATEST")).read().strip()
        mt = mf.parse(open(os.path.join(dest, rel, "manifest")).read())
        check(mt.abi == mf.read_abi(ROOT), "its manifest records the ABI id")
        names = [n for n, _t, _d in mt.sets]
        check(names[:1] == ["base"] and "games" in names and "arklite" in names
              and all(mt.set_of[f[0]] in names for f in mt.files),
              "it offers every set (%s), and each file is in one" % " ".join(names))
        check(not os.path.exists(os.path.join(dest, rel, "manifest.sig")),
              "TFTP stores are not signed")
        import dist
        home = os.path.join(tmp, "zhome")
        env = dict(os.environ, ZEITLOS_HOME=home)
        old = os.environ.get("ZEITLOS_HOME")
        os.environ["ZEITLOS_HOME"] = home
        try:
            dist.keygen()
        finally:
            if old is None:
                os.environ.pop("ZEITLOS_HOME", None)
            else:
                os.environ["ZEITLOS_HOME"] = old
        r = subprocess.run([sys.executable, os.path.join(root, "release", "zrelease"),
                            "http", "--dest", os.path.join(tmp, "h", "release")],
                           capture_output=True, text=True, env=env)
        check(r.returncode != 0 and "not in release/dist.cfg" in r.stderr,
              "zrelease http refuses a key dist.cfg does not list")
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
        check(m_img.abi and m_img.abi == mf.read_abi(root),
              "it records the tree's ABI id (%s)" % m_img.abi)
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
        test_sets()
        test_sign()
        test_dist()
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
