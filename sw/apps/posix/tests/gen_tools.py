#!/usr/bin/env python3
"""
Fixtures for find, diff, unzip and tar (sw/apps/posix/tools.c), and the
cases whose expected output comes from another implementation:

  diff   GNU `diff -u --label A --label B` on pairs of files -- the same
         output as posix's diff, whose ---/+++ lines carry no timestamp
  find   a sorted, depth-first walk in Python

Usage: gen_tools.py ROOT OUT_CASES. ROOT is the fixture tree run.sh
built; everything here goes in directories of its own under it.
"""

import gzip
import io
import os
import random
import subprocess
import sys
import tarfile
import zipfile

root, out_path = sys.argv[1], sys.argv[2]
cases = []


def write(rel, data):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data.encode() if isinstance(data, str) else data)


# -- find --

for rel in ["tree/b.txt", "tree/a.c", "tree/sub/x.c", "tree/sub/deep/y.h",
            "tree/Sub2/z.c", "tree/.hidden", "tree/sub/deep/A.C"]:
    write(rel, rel + "\n")
os.makedirs(os.path.join(root, "tree/empty"), exist_ok=True)

# more than FIND_DEPTH (32) levels
deep = "deep/" + "/".join(["d"] * 34)
write(deep + "/bottom.txt", "x\n")


def walk(path, shown):
    out = [shown]
    if os.path.isdir(path):
        for name in sorted(os.listdir(path)):
            out += walk(os.path.join(path, name), shown + "/" + name)
    return out


cases.append(("find tree", walk(os.path.join(root, "tree"), "tree"), 0))
cases.append(("find tree/sub/", walk(os.path.join(root, "tree/sub"), "tree/sub"), 0))

# -- diff --

random.seed(7)
base = ["line %d %s" % (i, random.choice(["alpha", "beta", "gamma", "delta"]))
        for i in range(300)]


def mutate(lines, n, seed):
    r = random.Random(seed)
    out = list(lines)
    for _ in range(n):
        op = r.choice("cdi")
        k = r.randrange(len(out) + (op == "i"))
        if op == "c" and k < len(out):
            out[k] = out[k] + " changed"
        elif op == "d" and k < len(out):
            del out[k]
        else:
            out.insert(k, "new %d" % r.randrange(1000))
    return out


def text(lines, nl=True):
    return "\n".join(lines) + ("\n" if nl and lines else "")


pairs = [
    ("same", text(base[:20]), text(base[:20])),
    ("one", text(base[:20]), text(base[:9] + ["CHANGED"] + base[10:20])),
    ("ends", text(base[:20]), text(["first"] + base[:19])),
    ("apart", text(base[:40]), text(base[:5] + ["x"] + base[6:30] + ["y"] + base[31:40])),
    ("close", text(base[:40]), text(base[:5] + ["x"] + base[6:12] + ["y"] + base[13:40])),
    ("seven", text(base[:40]), text(base[:5] + ["x"] + base[6:13] + ["y"] + base[14:40])),
    ("nonl", text(base[:5]), text(base[:5], nl=False)),
    ("nonl2", text(base[:5], nl=False), text(base[:4] + ["other"], nl=False)),
    ("empty1", "", text(base[:3])),
    ("empty2", text(base[:3]), ""),
    ("unlike", text(base[:10]), text(["zz %d" % i for i in range(8)])),
    ("repeat", "a\nb\na\nb\na\n", "b\na\nb\na\nb\n"),
    ("blank", "a\n\nb\n\nc\n", "a\nb\n\nc\n\n"),
]
for seed in range(1, 9):
    pairs.append(("rand%d" % seed, text(base), text(mutate(base, seed * 3, seed))))

for name, a, b in pairs:
    write("diffs/%s.a" % name, a)
    write("diffs/%s.b" % name, b)
    pa, pb = "diffs/%s.a" % name, "diffs/%s.b" % name
    r = subprocess.run(["diff", "-u", "--label", pa, "--label", pb,
                        os.path.join(root, pa), os.path.join(root, pb)],
                       capture_output=True, text=True)
    cases.append(("diff %s %s" % (pa, pb), r.stdout.splitlines(), r.returncode))

# -- archives --

big = bytes((i * 7 + (i >> 9)) & 0xFF for i in range(300000))

z = io.BytesIO()
with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as zf:
    zf.writestr("top.txt", "at the top\n")
    zf.writestr("dir/", "")
    zf.writestr("dir/inner.txt", "inside a directory\n" * 50)
    zf.writestr("dir/sub/stored.txt", "stored, not deflated\n",
                compress_type=zipfile.ZIP_STORED)
write("arch/t.zip", z.getvalue())

z = io.BytesIO()
with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as zf:
    zf.writestr("fine.txt", "fine\n")
    zf.writestr("../evil.txt", "climbs out\n")
write("arch/evil.zip", z.getvalue())

z = io.BytesIO()
with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as zf:
    zf.writestr("big.bin", big)
write("arch/big.zip", z.getvalue())


def tar_bytes(gz):
    t = io.BytesIO()
    with tarfile.open(fileobj=t, mode="w", format=tarfile.USTAR_FORMAT) as tf:
        for name, data in [("pkg/README", b"read me\n"),
                           ("pkg/src/main.c", b"int main(void) { return 0; }\n")]:
            ti = tarfile.TarInfo(name)
            ti.size = len(data)
            ti.mtime = 0
            tf.addfile(ti, io.BytesIO(data))
        ti = tarfile.TarInfo("pkg/empty")
        ti.type = tarfile.DIRTYPE
        ti.mtime = 0
        tf.addfile(ti)
    data = t.getvalue()
    return gzip.compress(data, mtime=0) if gz else data


write("arch/t.tar", tar_bytes(False))
write("arch/t.tgz", tar_bytes(True))
write("arch/t.tar.gz", tar_bytes(True))

t = io.BytesIO()
with tarfile.open(fileobj=t, mode="w:gz") as tf:
    ti = tarfile.TarInfo("big.bin")
    ti.size = len(big)
    tf.addfile(ti, io.BytesIO(big))
write("arch/big.tgz", t.getvalue())

with open(out_path, "w") as f:
    f.write("# generated by tests/gen_tools.py -- expected output from GNU diff\n"
            "# and from a sorted walk in Python\n\n")
    for cmd, lines, status in cases:
        f.write("$ %s\n" % cmd)
        for ln in lines:
            assert ln and ln[0] not in "$#?!", (cmd, ln)
            f.write(ln + "\n")
        if status:
            f.write("? %d\n" % status)
        f.write("\n")
