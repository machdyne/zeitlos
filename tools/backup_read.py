#!/usr/bin/env python3
#
# Zeitlos
# Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
#
# backup_read.py -- read a Zeitlos backup drive on any computer with
# Python 3, without Zeitlos (docs/backup.md, "On a PC").
#
#   backup_read.py list    DRIVE                 the backups, oldest first
#   backup_read.py files   DRIVE [BACKUP]        what a backup had
#   backup_read.py history DRIVE PATH            every copy of one file
#   backup_read.py restore DRIVE OUT [BACKUP] [PATH]
#                                                everything (or PATH) as it was, into OUT
#   backup_read.py check   DRIVE                 every index and copy, by CRC
#
# DRIVE is the backup folder on the drive (e.g. /media/stick/backup, the
# apps.backup.to folder), or a computer's folder inside it. BACKUP is a
# backup's name or the start of one ("2026-10-21"); the newest if left
# out. --name NAME picks a computer when the drive has backups of more
# than one.
#
# It reads only; nothing on the drive is changed. It follows the same
# rules as sw/apps/backup/bk.c, and its test (sw/apps/backup/tests/
# test_read.py) checks it against a drive bk.c wrote.

import os
import re
import sys
import zlib

NAME = re.compile(r"^(\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2})(?:\+(\d{4}))?([wW]?)$")


def name_key(n):
    m = NAME.match(n)
    return (m.group(1), int(m.group(2) or 0), m.group(3) != "")


def is_name(n):
    m = NAME.match(n)
    return bool(m) and (m.group(2) is None or int(m.group(2)) > 0)


class Index:
    """An INDEX.TXT. state: 1 complete, 0 cut short, -1 damaged."""

    def __init__(self, path):
        self.state = 0
        self.kind = "full"
        self.entries = {}   # path -> (crc, size, date, where or None)
        self.time = ""
        self.copied = 0
        try:
            with open(path, "rb") as f:
                t = f.read()
        except OSError:
            return
        if not t.endswith(b"\n"):
            return
        ls = t.rfind(b"\n", 0, len(t) - 1) + 1
        last = t[ls:-1].decode("utf-8", "replace")
        if not last.startswith("end "):
            return
        parts = last.split(" ")
        try:
            cnt, crc = int(parts[1]), int(parts[2], 16)
        except (IndexError, ValueError):
            self.state = -1
            return
        if len(parts) != 3 or zlib.crc32(t[:ls]) != crc:
            self.state = -1
            return
        n = 0
        for raw in t[:ls].split(b"\n"):
            line = raw.decode("utf-8", "replace")
            if line[:2] in ("f ", "s "):
                p = line[2:].split(" ", 4)
                if len(p) != 5 or not p[4].startswith("/"):
                    self.state = -1
                    return
                where = None if line[0] == "s" else p[3]
                self.entries[p[4]] = (int(p[0], 16), int(p[1]), p[2], where)
                n += 1
            elif line.startswith("kind "):
                self.kind = line[5:]
            elif line.startswith("time "):
                self.time = "" if line[5:] == "-" else line[5:]
            elif line.startswith("copied "):
                self.copied = int(line.split(" ")[1])
        self.state = 1 if n == cnt else -1


class Drive:
    def __init__(self, top, name=None):
        top = os.path.abspath(top)
        if any(is_name(d) for d in self._dirs(top)):
            self.root = top
        else:
            comps = [d for d in self._dirs(top)
                     if any(is_name(x) for x in self._dirs(os.path.join(top, d)))]
            if name:
                comps = [c for c in comps if c.lower() == name.lower()]
            if not comps:
                sys.exit("backup_read: no backups in %s" % top)
            if len(comps) > 1:
                sys.exit("backup_read: backups of %s here; say which with --name" % ", ".join(comps))
            self.root = os.path.join(top, comps[0])
        self.names = sorted((d for d in self._dirs(self.root) if is_name(d)), key=name_key)
        self.idx = {n: Index(os.path.join(self.root, n, "INDEX.TXT")) for n in self.names}

    @staticmethod
    def _dirs(p):
        try:
            return [d for d in os.listdir(p) if os.path.isdir(os.path.join(p, d))]
        except OSError:
            return []

    def complete(self):
        return [n for n in self.names if self.idx[n].state == 1]

    def pick(self, b):
        done = self.complete()
        if not done:
            sys.exit("backup_read: there is no complete backup here")
        if not b:
            return done[-1]
        if b in done:
            return b
        hits = [n for n in done if n.startswith(b)]
        if not hits:
            sys.exit("backup_read: no backup %s" % b)
        return hits[-1]

    def effective(self, b):
        """The state at backup b: the newest full backup up to it, with
        the watch backups after it laid over."""
        upto = [n for n in self.complete() if name_key(n) <= name_key(b)]
        full = [n for n in upto if self.idx[n].kind != "watch"]
        state = dict(self.idx[full[-1]].entries) if full else {}
        start = upto.index(full[-1]) + 1 if full else 0
        for n in upto[start:]:
            if self.idx[n].kind == "watch":
                state.update(self.idx[n].entries)
        return state

    def copy_path(self, where, path):
        return os.path.join(self.root, where, *path.lstrip("/").split("/"))


def crc_of(p):
    c = 0
    with open(p, "rb") as f:
        while True:
            b = f.read(65536)
            if not b:
                return c
            c = zlib.crc32(b, c)


def human(n):
    if n < 10 * 1024:
        return "%d bytes" % n
    if n < 10 * 1024 * 1024:
        return "%d KB" % (n // 1024)
    return "%d MB" % (n // (1024 * 1024))


def cmd_list(d, args):
    for n in d.names:
        x = d.idx[n]
        if x.state == 1:
            print("%-26s %-5s %5d files, %d copied%s" % (
                n, x.kind, len(x.entries), x.copied,
                "" if "+" not in n else "  (clock not set)"))
        else:
            print("%-26s %s" % (n, "cut short" if x.state == 0 else "INDEX DAMAGED"))


def cmd_files(d, args):
    b = d.pick(args[0] if args else None)
    for p, (crc, size, date, where) in sorted(d.effective(b).items()):
        if where is None:
            note = "installed by update"
        elif not os.path.exists(d.copy_path(where, p)):
            note = "removed to make room"
        else:
            note = where
        print("%10d  %-19s  %s  (%s)" % (size, date, p, note))


def cmd_history(d, args):
    if not args:
        sys.exit("usage: backup_read.py history DRIVE PATH")
    found = 0
    for n in d.complete():
        p = d.copy_path(n, args[0])
        if os.path.isfile(p):
            print("%-26s %10d  %s" % (n, os.path.getsize(p), p))
            found += 1
    if not found:
        print("no copy of %s" % args[0])
        return 1
    return 0


def cmd_restore(d, args):
    if not args:
        sys.exit("usage: backup_read.py restore DRIVE OUT [BACKUP] [PATH]")
    out = args[0]
    b = d.pick(args[1] if len(args) > 1 else None)
    sel = args[2] if len(args) > 2 else "/"
    sel = sel.rstrip("/") or "/"
    done = bad = gone = 0
    for p, (crc, size, date, where) in sorted(d.effective(b).items()):
        if sel != "/" and p != sel and not p.startswith(sel + "/"):
            continue
        if where is None:
            continue
        src = d.copy_path(where, p)
        if not os.path.isfile(src):
            print("  %s: removed to make room" % p)
            gone += 1
            continue
        dst = os.path.join(out, *p.lstrip("/").split("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(src, "rb") as f:
            data = f.read()
        if len(data) != size or zlib.crc32(data) != crc:
            print("  %s: the copy is damaged; not restored" % p)
            bad += 1
            continue
        with open(dst + ".part", "wb") as f:
            f.write(data)
        os.replace(dst + ".part", dst)
        done += 1
    print("restored %d files from %s into %s%s%s" % (
        done, b, out, ", %d removed to make room" % gone if gone else "",
        ", %d damaged" % bad if bad else ""))
    return 1 if bad or gone else 0


def cmd_check(d, args):
    problems = 0
    for n in d.names:
        x = d.idx[n]
        if x.state != 1:
            print("  %s: %s" % (n, "cut short" if x.state == 0 else "index damaged"))
            problems += x.state < 0
            continue
        good = here = 0
        for p, (crc, size, date, where) in x.entries.items():
            if where != n:
                continue
            here += 1
            c = d.copy_path(n, p)
            if not os.path.isfile(c):
                later = [m for m in d.complete() if name_key(m) > name_key(n)
                         and os.path.isfile(d.copy_path(m, p))]
                if not later:
                    print("  %s%s: missing" % (n, p))
                    problems += 1
                continue
            if os.path.getsize(c) != size or crc_of(c) != crc:
                print("  %s%s: damaged" % (n, p))
                problems += 1
            else:
                good += 1
        print("  %s: %d of %d copies good" % (n, good, here))
    print("%d problem%s" % (problems, "" if problems == 1 else "s"))
    return 1 if problems else 0


def main(argv):
    name = None
    if "--name" in argv:
        i = argv.index("--name")
        name = argv[i + 1]
        del argv[i:i + 2]
    if len(argv) < 3:
        print(__doc__ if __doc__ else "", end="")
        sys.exit("usage: backup_read.py list|files|history|restore|check DRIVE ...")
    cmd, top, rest = argv[1], argv[2], argv[3:]
    d = Drive(top, name)
    f = {"list": cmd_list, "files": cmd_files, "history": cmd_history,
         "restore": cmd_restore, "check": cmd_check}.get(cmd)
    if not f:
        sys.exit("backup_read: no command %s" % cmd)
    return f(d, rest) or 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
