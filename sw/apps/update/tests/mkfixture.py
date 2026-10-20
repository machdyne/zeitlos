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
#

import os
import random
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "release", "lib"))
sys.dont_write_bytecode = True

import manifest as mf   # noqa: E402
import store            # noqa: E402

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
    for path, (_pol, data) in files.items():
        p = os.path.join(dest, path)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)


def publish(name, files, src, st, latest):
    entries, sources = [], {}
    for path, (pol, data) in files.items():
        p = os.path.join(src, path)
        sha, crc = mf.hashes_file(p)
        entries.append((path, sha, len(data), pol, crc))
        sources[sha] = p
    m = mf.build(name, "0.0.7", "fixture", ["home"], entries,
                 made="2026-10-19T12:00:00Z")
    store.publish(st, m, sources, latest=latest)
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
            # The same release as round 1 published it: format 2, raw
            # objects -- a store not yet republished.
            st2 = os.path.join(out, "store2")
            m2 = mf.parse("zeitlos-manifest 2\n" + m.text.split("\n", 1)[1])
            os.makedirs(os.path.join(st2, "v2"))
            for path, sha, _s, _p, _c in m2.files:
                o = os.path.join(st2, mf.object_path(sha, 2))
                os.makedirs(os.path.dirname(o), exist_ok=True)
                shutil.copyfile(os.path.join(src, path), o)
            with open(os.path.join(st2, "v2", "manifest"), "w") as f:
                f.write(m2.text)
    with open(os.path.join(out, "ids"), "w") as f:
        f.writelines(ids)


if __name__ == "__main__":
    main()
