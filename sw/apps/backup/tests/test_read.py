#!/usr/bin/env python3
#
# Zeitlos
# Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
#
# test_read.py READER DUMP -- checks tools/backup_read.py against a drive
# bk.c made (test_bk --dump DUMP): every backup restored on the "PC" has
# exactly what the card had when it was made (DUMP/truth/<backup>.txt:
# crc, size and path of every file; a watch backup lists the files it
# stored). Run by `make -C sw/apps/backup/tests`.

import os
import subprocess
import sys
import tempfile
import zlib

reader, dump = sys.argv[1], sys.argv[2]
drive = os.path.join(dump, "usb", "backup")
checks = fails = 0


def check(ok, msg):
    global checks, fails
    checks += 1
    if not ok:
        fails += 1
        print("FAIL", msg)


def run(*args):
    p = subprocess.run([sys.executable, "-I", reader] + list(args), capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


truths = sorted(os.listdir(os.path.join(dump, "truth")))
check(len(truths) == 4, "four backups in the dump (%d)" % len(truths))

rc, out = run("list", drive)
check(rc == 0 and all(t[:-4] in out for t in truths), "list names every backup")
check("clock not set" in out, "list says which were made without a clock")

rc, out = run("check", drive)
check(rc == 0 and "0 problems" in out, "check: a good drive is good\n" + out)

for t in truths:
    name = t[:-4]
    with open(os.path.join(dump, "truth", t)) as f:
        kind = f.readline().strip()
        want = {}
        for line in f:
            crc, size, path = line.rstrip("\n").split(" ", 2)
            want[path] = (int(crc, 16), int(size))
    with tempfile.TemporaryDirectory() as out_dir:
        rc, out = run("restore", drive, out_dir, name)
        check(rc == 0, "restore %s: %s" % (name, out))
        got = {}
        for top, dirs, files in os.walk(out_dir):
            for fn in files:
                p = os.path.join(top, fn)
                with open(p, "rb") as f:
                    data = f.read()
                got["/" + os.path.relpath(p, out_dir).replace(os.sep, "/")] = (zlib.crc32(data), len(data))
        if kind == "full":
            check(got == want, "restore %s: got %s want %s" % (name, sorted(got), sorted(want)))
        else:
            for p, v in want.items():
                check(got.get(p) == v, "restore %s (watch): %s" % (name, p))

# one file, by the start of a name; and history
rc, out = run("history", drive, "/home/book.txt")
check(rc == 0 and out.count("\n") == 3, "history: three copies of the book\n" + out)
with tempfile.TemporaryDirectory() as out_dir:
    # a prefix names the newest backup that starts so: the last one, made
    # after a reboot, with notes.txt back again
    rc, out = run("restore", drive, out_dir, truths[0][:10], "/home/notes.txt")
    p = os.path.join(out_dir, "home", "notes.txt")
    check(rc == 0 and os.path.isfile(p) and open(p).read() == "back again\n",
          "restore one file by a date prefix: the newest that matches\n" + out)

# a damaged index (its end line intact) is not trusted
idx = os.path.join(drive, "zeitlos", truths[1][:-4], "INDEX.TXT")
with open(idx, "rb") as f:
    t = bytearray(f.read())
k = t.find(b"/home/")
t[k + 2] ^= 0x01
with open(idx, "wb") as f:
    f.write(t)
rc, out = run("list", drive)
check("INDEX DAMAGED" in out, "list: a damaged index\n" + out)
rc, out = run("check", drive)
check(rc == 1 and "index damaged" in out, "check: a damaged index\n" + out)
t[k + 2] ^= 0x01
with open(idx, "wb") as f:
    f.write(t)

# a damaged copy is found and not restored
victim = None
for top, dirs, files in os.walk(drive):
    if "book.txt" in files and os.path.basename(top) == "home":
        victim = os.path.join(top, "book.txt")
        break
with open(victim, "r+b") as f:
    b = f.read(1)
    f.seek(0)
    f.write(bytes([b[0] ^ 0x20]))
rc, out = run("check", drive)
check(rc == 1 and "damaged" in out, "check finds a damaged copy\n" + out)

print("test_read: %d checks, %d failed" % (checks, fails))
sys.exit(1 if fails else 0)
