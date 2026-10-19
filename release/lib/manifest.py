#
# Zeitlos release tooling -- the update manifest (docs/update.md).
#
# One text file that says what a card holds, file by file: its hash,
# its size, and what an update may do with it. The same file is
#
#   - /sys/manifest on a card the image builder wrote (mkfatimg.build),
#     and on one `update` brought up to date: what IS there;
#   - <name>/manifest in an update release (store.py): what SHOULD be.
#
# `update` compares the two. It never compares version strings, which
# is what lets v0.0.7 be published again and again during development:
# a rebuilt app has a new hash, so it is written, and nothing else is.
#
# -- the format --
#
#   zeitlos-manifest 1
#   name v0.0.7
#   version 0.0.7
#   snapshot 3f2a9c01e4b7
#   source v0.0.6-45-g58e067a-dirty
#   made 2026-10-19T09:40:12Z
#   packs zdocs arklite
#   d home
#   f <sha256 hex> <crc32 hex> <size> <policy> <path>
#   ...
#
# Header lines are "key value"; a reader skips keys it does not know.
# Then `d` lines (folders a card has even when empty) and `f` lines
# (files), each group sorted by path, bytewise, so two manifests can be
# merged in one pass with no memory per file -- which is how the board
# reads them. The path is the rest of the line, so it may hold spaces.
# Paths have no leading slash, as mkfatimg writes them.
#
# Policies (mkfatimg.REPLACE / KEEP): r and k.
#
# -- two hashes --
#
# SHA-256 names a file's object in the store, makes the snapshot id, and
# is what `update -v` checks -- and, once manifests are signed (HTTPS,
# docs/update.md), what makes a file trustworthy. CRC-32 (zlib's, IEEE
# 802.3) is for the board's everyday checks: is the card's copy the
# shipped one, did a download arrive whole. In software SHA-256 costs a
# Lakritz about 690 cycles a byte (~70 KB/s, docs/sha256_hw.md), slower
# than TFTP brings the bytes in; a table CRC is some twenty times faster.
# The CRC guards against accident, not against an attacker, which a
# plain TFTP transfer could not keep out anyway.
#
# Format 1 (round 0) had no CRC: `f <sha256> <size> <policy> <path>`.
# It is still read; this writes 2.
#
# -- the snapshot id --
#
# The first 12 hex digits of SHA-256 over the `d` and `f` lines alone.
# Content only: no time, no commit. Two publishes of the same files
# have the same id, a card written from an image and an update release
# made from the same tree have the same id, and "is this card up to
# date?" is one comparison. `source` and `made` are for people.
#
# The `packs` line records which ask packs an image carried; their
# files are not listed (docs/update.md, "Not yet").
#

import datetime
import hashlib
import os
import subprocess
import zlib

FORMAT = 2
MAGIC = "zeitlos-manifest"
ID_DIGITS = 12

# Objects in an update release are named by this many hex digits of the
# hash (80 bits): short enough that release/o/ab/<name> stays far below
# net's 63-byte TFTP filename limit, long enough that a collision is not
# a thing to plan for. store.py still refuses one.
OBJECT_DIGITS = 20


class ManifestError(Exception):
    pass


class Manifest(object):
    def __init__(self):
        self.header = {}        # key -> value text
        self.dirs = []          # paths
        self.files = []         # [(path, sha256, size, policy, crc32)]
        self.format = FORMAT
        self.text = ""

    @property
    def id(self):
        return self.header.get("snapshot", "")

    @property
    def name(self):
        return self.header.get("name", "")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def hashes_file(path):
    """(sha256 hex, crc32 as 8 hex digits) of a file, in one read."""
    h = hashlib.sha256()
    c = 0
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
            c = zlib.crc32(b, c)
    return h.hexdigest(), "%08x" % (c & 0xFFFFFFFF)


def object_path(sha):
    """Where an update release keeps a file's contents: o/ab/abcdef..."""
    return "o/%s/%s" % (sha[:2], sha[:OBJECT_DIGITS])


def describe(root):
    """`git describe` of the tree the release is made from, for people.

    "unknown" outside a git checkout (the card tests build from scratch
    trees). Never part of the snapshot id.
    """
    try:
        r = subprocess.run(["git", "-C", root, "describe", "--always",
                            "--dirty", "--tags", "--abbrev=7"],
                           capture_output=True, text=True, timeout=20)
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip().split()[0]
    except (OSError, subprocess.SubprocessError):
        pass
    return "unknown"


def _check_path(p):
    if not p or p.startswith("/") or p != p.strip() or "\n" in p \
            or "\r" in p or "//" in p or p.endswith("/"):
        raise ManifestError("not a manifest path: %r" % p)
    for part in p.split("/"):
        if part in (".", ".."):
            raise ManifestError("not a manifest path: %r" % p)


def _line(f, fmt=FORMAT):
    path, sha, size, pol, crc = f
    if fmt == 1:
        return "f %s %d %s %s" % (sha, size, pol, path)
    return "f %s %s %d %s %s" % (sha, crc, size, pol, path)


def _body(dirs, files, fmt=FORMAT):
    lines = ["d %s" % d for d in sorted(dirs, key=lambda s: s.encode())]
    lines += [_line(f, fmt) for f in sorted(files, key=lambda f: f[0].encode())]
    return lines


def snapshot_id(dirs, files, fmt=FORMAT):
    h = hashlib.sha256()
    for line in _body(dirs, files, fmt):
        h.update(line.encode("utf-8") + b"\n")
    return h.hexdigest()[:ID_DIGITS]


def make(pl, name, version, source, packs=(), made=None):
    """A manifest for a mkfatimg.Plan. Hashes every file."""
    files = []
    for dest, src, policy in pl.manifest_files():
        sha, crc = hashes_file(src)
        files.append((dest, sha, os.path.getsize(src), policy, crc))
    dirs = [d.strip("/") for d in pl.dirs]
    return build(name, version, source, dirs, files, packs=packs, made=made)


def build(name, version, source, dirs, files, packs=(), made=None):
    for path, _sha, _size, policy, crc in files:
        _check_path(path)
        if policy not in ("r", "k"):
            raise ManifestError("%s: unknown policy %r" % (path, policy))
        if len(crc) != 8 or crc != crc.lower():
            raise ManifestError("%s: crc %r is not 8 hex digits" % (path, crc))
        int(crc, 16)
    for d in dirs:
        _check_path(d)
    m = Manifest()
    if made is None:
        made = datetime.datetime.now(datetime.timezone.utc) \
            .strftime("%Y-%m-%dT%H:%M:%SZ")
    m.dirs = sorted(dirs, key=lambda s: s.encode())
    m.files = sorted(files, key=lambda f: f[0].encode())
    m.header = {
        "name": name,
        "version": version,
        "snapshot": snapshot_id(dirs, files),
        "source": source or "unknown",
        "made": made,
        "packs": " ".join(packs),
    }
    head = ["%s %d" % (MAGIC, FORMAT)]
    for k in ("name", "version", "snapshot", "source", "made", "packs"):
        head.append(("%s %s" % (k, m.header[k])).rstrip())
    m.text = "\n".join(head + _body(dirs, files)) + "\n"
    return m


def parse(text):
    """Read a manifest. Raises ManifestError on anything malformed."""
    m = Manifest()
    m.text = text
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    if not lines:
        raise ManifestError("empty manifest")
    magic = lines[0].split()
    if len(magic) != 2 or magic[0] != MAGIC or not magic[1].isdigit():
        raise ManifestError("not a manifest (first line %r)" % lines[0])
    if int(magic[1]) > FORMAT:
        raise ManifestError("manifest format %s is newer than this tool's %d"
                            % (magic[1], FORMAT))
    m.format = fmt = int(magic[1])
    for n, line in enumerate(lines[1:], 2):
        if line.startswith("f "):
            if fmt == 1:
                parts = line.split(" ", 4)
                crc = None
            else:
                parts = line.split(" ", 5)
                if len(parts) == 6:
                    crc = parts.pop(2)
                    if len(crc) != 8:
                        raise ManifestError("line %d: %r" % (n, line))
                    int(crc, 16)
            if len(parts) != 5 or len(parts[1]) != 64 \
                    or not parts[2].isdigit():
                raise ManifestError("line %d: %r" % (n, line))
            int(parts[1], 16)
            _check_path(parts[4])
            m.files.append((parts[4], parts[1], int(parts[2]), parts[3], crc))
        elif line.startswith("d "):
            _check_path(line[2:])
            m.dirs.append(line[2:])
        elif m.files or m.dirs:
            raise ManifestError("line %d: header after entries" % n)
        else:
            k, _sp, v = line.partition(" ")
            m.header[k] = v
    paths = [f[0] for f in m.files]
    if paths != sorted(paths, key=lambda s: s.encode()) \
            or len(set(paths)) != len(paths):
        raise ManifestError("file lines are not sorted and unique")
    if m.header.get("snapshot") != snapshot_id(m.dirs, m.files, fmt):
        raise ManifestError("snapshot id does not match the entries")
    return m


def version_text(m):
    """/sys/version: which release wrote the card.

    The first line is what it has always been ("zeitlos 0.0.7"); the
    second says which snapshot of it, and is what `update` and people
    read to tell two builds of one version apart.
    """
    return "zeitlos %s\nsnapshot %s %s %s %s\n" % (
        m.header["version"], m.header["name"], m.id,
        m.header["source"], m.header["made"])
