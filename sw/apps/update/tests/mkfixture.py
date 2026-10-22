#!/usr/bin/env python3
#
# Fixtures for test_update: two releases of a small card, published by
# release/lib's own manifest.py and store.py -- the code `zrelease tftp`
# runs -- so the C side is tested against what the Python side writes.
#
#   python3 mkfixture.py OUT
#
#   OUT/store/            the store (release/ on a TFTP server)
#   OUT/src/v1/, src/v2/  each release's files, laid out as on a card
#   OUT/ids               "v1 <snapshot id>" and "v2 <snapshot id>"
#   OUT/v1.manifest1      v1's manifest in format 1 (no CRC)
#   OUT/store2/           v2 as a format-2 store (raw objects)
#   OUT/keys              "k1 <hex>" and "k2 <hex>": the fixture's public keys
#
# In the store besides v1, v2 and v2abi (docs/update.md, "Sets";
# docs/distribution.md), all signed with k1 unless said:
#
#   s1, s2       a card in sets (base, games, dev; s2 adds net): s2 moves
#                apps/mover from games to dev, adds apps/tetris (games)
#                and apps/web (net), and drops apps/chess
#   unsig        s1's files, not signed
#   other        s1's files, signed with k2 only
#   tamper       signed, then its manifest changed
#   wrongname    s1's manifest and signature, under another name
#   shacheck     a file whose object has the right size and CRC but not
#                the SHA-256 the manifest says
#

import os
import random
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "release", "lib"))
sys.dont_write_bytecode = True

import ed25519          # noqa: E402
import manifest as mf   # noqa: E402
import store            # noqa: E402

K1 = bytes(range(1, 33))            # the fixture's keys: not secret
K2 = bytes(range(101, 133))

rnd = random.Random(7)
BIG = bytes(rnd.getrandbits(8) for _ in range(300 * 1024 + 17))

V1 = {
    "apps/a": ("r", b"A version one\n" * 50),
    "apps/b": ("r", b"B is the same in both\n"),
    "apps/big": ("r", BIG),
    "data/x/cfg": ("k", b"# cfg, as v1 ships it\n#key: value\n"),
    "data/x/old.txt": ("r", b"shipped in v1 only\n"),
    "docs/d.md": ("r", b"# D\n\nThe same doc in both.\n"),
    "docs/my notes.md": ("r", b"a name with a space\n"),
}

V2 = {
    "apps/a": ("r", b"A version two, longer\n" * 80),
    "apps/b": V1["apps/b"],
    "apps/big": V1["apps/big"],
    "apps/new": ("r", b"new in v2\n"),
    "data/x/cfg": ("k", b"# cfg, as v2 ships it (a new comment)\n#key: value\n"),
    "data/x/ex.c": ("k", b"int main(void) { return 0; }\n"),
    "data/y/deep/f": ("r", b"a new folder, two deep\n"),
    "docs/d.md": V1["docs/d.md"],
    "docs/my notes.md": V1["docs/my notes.md"],
}


def lay_out(files, dest):
    for path, v in files.items():
        data = v[1]
        p = os.path.join(dest, path)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)


ABI = "0123456789abcdef"     # the fixture's ABI id (test_update.c, FX_ABI)


SETS1 = [("base", "Base", "the shell and documents"),
         ("games", "Games", "snake, chess"),
         ("dev", "Development", "the compiler")]
SETS2 = SETS1 + [("net", "Network", "the web")]

S1 = {
    "apps/posix": ("r", b"posix one\n", "base"),
    "docs/d.md": ("r", b"# D\n", "base"),
    "apps/snake": ("r", b"snake\n" * 20, "games"),
    "apps/chess": ("r", b"chess, dropped in s2\n", "games"),
    "apps/mover": ("r", b"mover, in games\n", "games"),
    "apps/zcc": ("r", b"zcc\n" * 30, "dev"),
    "data/zcc/hello.c": ("k", b"int main(void) { return 0; }\n", "dev"),
}
S2 = {
    "apps/posix": ("r", b"posix two, longer\n", "base"),
    "docs/d.md": S1["docs/d.md"],
    "apps/snake": S1["apps/snake"],
    "apps/mover": ("r", b"mover, now in dev\n", "dev"),
    "apps/tetris": ("r", b"tetris\n", "games"),
    "apps/zcc": S1["apps/zcc"],
    "data/zcc/hello.c": S1["data/zcc/hello.c"],
    "apps/web": ("r", b"web\n" * 10, "net"),
}


def publish(name, files, src, st, latest, abi="", sets=None,
            made="2026-10-19T12:00:00Z", keys=(K1,)):
    entries, sources = [], {}
    for path, v in files.items():
        pol, data = v[0], v[1]
        p = os.path.join(src, path)
        sha, crc = mf.hashes_file(p)
        entries.append((path, sha, len(data), pol, crc) + tuple(v[2:3]))
        sources[sha] = p
    m = mf.build(name, "0.0.7", "fixture", ["home"], entries,
                 made=made, abi=abi, sets=sets)
    store.publish(st, m, sources, latest=latest)
    if keys:
        store.sign(st, name, list(keys))
    return m


def main():
    out = sys.argv[1]
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    st = os.path.join(out, "store")
    ids = []
    for name, files in (("v1", V1), ("v2", V2)):
        src = os.path.join(out, "src", name)
        lay_out(files, src)
        m = publish(name, files, src, st, latest=(name == "v2"))
        ids.append("%s %s\n" % (name, m.id))
        if name == "v1":
            # The same release as round 0 wrote manifests: format 1, no
            # CRC -- what a card updated before the CRC has.
            lines = mf._body(m.dirs, m.files, 1)
            head = ["zeitlos-manifest 1", "name v1", "version 0.0.7",
                    "snapshot %s" % mf.snapshot_id(m.dirs, m.files, 1),
                    "source fixture", "made 2026-10-19T12:00:00Z", "packs"]
            with open(os.path.join(out, "v1.manifest1"), "w") as f:
                f.write("\n".join(head + lines) + "\n")
        if name == "v2":
            # v2 again, saying which ABI it was built for (docs/update.md,
            # "ABI"); v1 and v2 say nothing, as releases before it.
            publish("v2abi", files, src, st, latest=False, abi=ABI)
            # The same release as round 1 published it: format 2, raw
            # objects -- a store not yet republished.
            st2 = os.path.join(out, "store2")
            m2 = mf.parse(mf.text_in_format(m, 2))
            os.makedirs(os.path.join(st2, "v2"))
            for path, sha, _s, _p, _c in m2.files:
                o = os.path.join(st2, mf.object_path(sha, 2))
                os.makedirs(os.path.dirname(o), exist_ok=True)
                shutil.copyfile(os.path.join(src, path), o)
            with open(os.path.join(st2, "v2", "manifest"), "w") as f:
                f.write(m2.text)

    # -- sets and signatures --
    for name, files, sets, made in (("s1", S1, SETS1, "2026-10-21T10:00:00Z"),
                                    ("s2", S2, SETS2, "2026-10-21T11:00:00Z")):
        src = os.path.join(out, "src", name)
        lay_out(files, src)
        m = publish(name, files, src, st, latest=False, sets=sets, made=made)
        ids.append("%s %s\n" % (name, m.id))
    src1 = os.path.join(out, "src", "s1")
    publish("unsig", S1, src1, st, False, sets=SETS1, keys=())
    publish("other", S1, src1, st, False, sets=SETS1, keys=(K2,))
    publish("tamper", S1, src1, st, False, sets=SETS1)
    mp = os.path.join(st, "tamper", "manifest")
    t = open(mp).read().replace("source fixture", "source someone else")
    open(mp, "w").write(t)
    os.makedirs(os.path.join(st, "wrongname"))
    for f in ("manifest", "manifest.sig"):
        shutil.copyfile(os.path.join(st, "s1", f), os.path.join(st, "wrongname", f))
    # shacheck: apps/posix's object holds its bytes, but the manifest's
    # SHA-256 is another (the object is named by it): size and CRC match.
    data = S1["apps/posix"][1]
    real, crc = mf.hashes_file(os.path.join(src1, "apps/posix"))
    fake = "f" * 8 + real[8:]
    m = mf.build("shacheck", "0.0.7", "fixture", ["home"],
                 [("apps/posix", fake, len(data), "r", crc, "base")],
                 made="2026-10-21T10:00:00Z", sets=SETS1[:1])
    obj = os.path.join(st, mf.object_path(fake))
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    open(obj, "wb").write(mf.lz_blocks(data))
    os.makedirs(os.path.join(st, "shacheck"))
    open(os.path.join(st, "shacheck", "manifest"), "w").write(m.text)
    store.sign(st, "shacheck", [K1])

    # big: a card of 3,000 small files, a 400 KB manifest -- larger than
    # update could once hold in memory (upd.c, "the manifests").
    BIG_CARD = {"data/big/%04d/file-with-a-longer-name-%04d.txt" % (i // 100, i):
                ("r", b"%d\n" % i) for i in range(3000)}
    src = os.path.join(out, "src", "big")
    lay_out(BIG_CARD, src)
    publish("big", BIG_CARD, src, st, False)

    with open(os.path.join(out, "keys"), "w") as f:
        f.write("k1 %s\nk2 %s\n" % (ed25519.public_key(K1).hex(),
                                      ed25519.public_key(K2).hex()))
    with open(os.path.join(out, "ids"), "w") as f:
        f.writelines(ids)


if __name__ == "__main__":
    main()
