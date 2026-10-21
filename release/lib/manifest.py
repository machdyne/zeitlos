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
#   zeitlos-manifest 4
#   name v0.0.7
#   version 0.0.7
#   snapshot 3f2a9c01e4b7
#   abi 03a25f4cbf544cf0
#   source v0.0.6-45-g58e067a-dirty
#   made 2026-10-19T09:40:12Z
#   packs arkmed
#   set base Base | the shell, settings, documents, fonts
#   set games Games | arcade, casino, chess, chip8
#   installed base games
#   d home
#   f <sha256 hex> <crc32 hex> <size> <policy> <set> <path>
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
#
# -- format 3: the objects are LZ4 blocks --
#
# The lines are format 2's; what changes is the store. Format 3's
# objects (`o/ab/<20 hex>.lz`) are the file in 32 KB blocks, each
# preceded by a 4-byte little-endian word: bit 31 set, the block is
# stored as it is; clear, it is an LZ4 block (lz4_Block_format.md, as
# sw/common/zexec_lz4.c reads it). The low 31 bits are the length of
# what follows. Every block but the last holds 32768 bytes of the file;
# the manifest's size says how many the last holds. Blocks are
# independent, so the board needs no history window, and each is
# decoded into a 32 KB buffer.
#
# Not gzip: measured under QEMU on the real card, inflate costs ~100-140
# instructions per byte here (~9 cycles each on this CPU), which would
# make an update 2-4x SLOWER than uncompressed; LZ4 is ~10. The base
# card is 17.4 MB and 10.4 MB as LZ4 blocks. (The same lesson that
# moved compressed executables from DEFLATE to LZ4: docs/executables.md.)
#
# The CRC-32 and the SHA-256 are still of the file as it is on the
# card, so a card's /sys/manifest means the same in every format; the
# snapshot id, over the lines alone, is the same for 2 and 3. An update
# that reads only format 2 refuses 3 and prints how to fetch the
# release's own updater (docs/update.md).
#
# Formats 1 to 3 are still read; this writes 4.
#
# -- the snapshot id --
#
# The first 12 hex digits of SHA-256 over the `d` and `f` lines alone.
# Content only: no time, no commit. Two publishes of the same files
# have the same id, a card written from an image and an update release
# made from the same tree have the same id, and "is this card up to
# date?" is one comparison. `source` and `made` are for people.
#
# -- the abi line --
#
# The ABI id of the tree the files were built from (sw/common/zabi.h,
# tools/gen_abi.py): what the kernel and core apps in flash must report
# for these files to run (docs/update.md, "ABI"). A header line, so not
# part of the snapshot id, and readers that predate it skip it. Written
# when the tree has a zabi.h; a manifest without one says nothing about
# its ABI, and `update` treats it like a kernel that cannot say.
#
# -- format 4: sets --
#
# A set is a label on a file (docs/update.md, "Sets"): each `f` line
# says which set the file belongs to, after its policy. A set name is
# 1-16 of [a-z0-9-], starting with a letter; "-" is a file whose set is
# not known (a card's entry carried over from a manifest older than
# format 4). The snapshot id is over the lines, so the set is part of
# it: moving an app to another set is a new snapshot.
#
# `set <name> <title> | <description>` lines, in the release's order,
# say which sets a release offers, for people (update's window, -s
# all). `installed <names>` is on a card: the sets it has. A card with
# no `installed` line was written before sets, when every card had
# everything: it has every set.
#
# The `packs` line records which ask packs an image carried; their
# files are not listed (docs/update.md, "Not yet").
#

import datetime
import hashlib
import os
import re
import struct
import subprocess
import sys
import zlib

FORMAT = 4
MAGIC = "zeitlos-manifest"
ID_DIGITS = 12

# Objects in an update release are named by this many hex digits of the
# hash (80 bits): short enough that release/o/ab/<name> stays far below
# net's 63-byte TFTP filename limit, long enough that a collision is not
# a thing to plan for. store.py still refuses one.
OBJECT_DIGITS = 20


class ManifestError(Exception):
    pass


SET_RE = re.compile(r"[a-z][a-z0-9-]{0,15}$")
NO_SET = "-"


def check_set(name):
    if not SET_RE.match(name or ""):
        raise ManifestError("not a set name: %r" % name)


class Manifest(object):
    def __init__(self):
        self.header = {}        # key -> value text
        self.dirs = []          # paths
        self.files = []         # [(path, sha256, size, policy, crc32)]
        self.set_of = {}        # path -> set (format 4)
        self.sets = []          # [(name, title, description)] offered
        self.installed = None   # [names] on a card; None: not said
        self.format = FORMAT
        self.text = ""

    @property
    def id(self):
        return self.header.get("snapshot", "")

    @property
    def name(self):
        return self.header.get("name", "")

    @property
    def abi(self):
        return self.header.get("abi", "")


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


def object_path(sha, fmt=FORMAT):
    """Where an update release keeps a file's contents: o/ab/abcdef...,
    as LZ4 blocks in o/ab/abcdef....lz from format 3."""
    return "o/%s/%s%s" % (sha[:2], sha[:OBJECT_DIGITS],
                          ".lz" if fmt >= 3 else "")


LZ_BLOCK = 32768
LZ_STORED = 0x80000000


def _mkzar():
    # tools/mkzar.py's compressor: the one the flash archive uses, whose
    # output sw/common/zexec_lz4.c reads. Pure Python, no dependency.
    here = os.path.dirname(os.path.abspath(__file__))
    tools = os.path.normpath(os.path.join(here, "..", "..", "tools"))
    if tools not in sys.path:
        sys.path.insert(0, tools)
    import mkzar
    return mkzar


def lz_blocks(data):
    """A format-3 object: `data` as LZ4 blocks (see the top of this
    file). Deterministic, so a republish rewrites nothing; each block
    checked by decoding it again before it is used."""
    mk = _mkzar()
    out = bytearray()
    for i in range(0, len(data), LZ_BLOCK):
        blk = data[i:i + LZ_BLOCK]
        z = mk.lz4_compress(blk) if len(blk) > 16 else blk
        if len(z) >= len(blk) or mk.lz4_decompress(z, len(blk)) != blk:
            out += struct.pack("<I", LZ_STORED | len(blk)) + blk
        else:
            out += struct.pack("<I", len(z)) + z
    return bytes(out)


def lz_unblock(obj, size):
    """The file back from a format-3 object (for checking and tests)."""
    mk = _mkzar()
    out = bytearray()
    p = 0
    while len(out) < size:
        w = struct.unpack("<I", obj[p:p + 4])[0]
        p += 4
        n = w & ~LZ_STORED
        want = min(LZ_BLOCK, size - len(out))
        part = obj[p:p + n]
        p += n
        out += part if w & LZ_STORED else mk.lz4_decompress(part, want)
    if p != len(obj):
        raise ManifestError("data after the last block")
    return bytes(out)


def lz_check(path, size):
    """Whether a format-3 object's block headers add up to `size`
    bytes and end where the file does -- a damaged or truncated object,
    found without decoding it."""
    total = os.path.getsize(path)
    p = got = 0
    with open(path, "rb") as f:
        while got < size:
            h = f.read(4)
            if len(h) < 4:
                return False
            w = struct.unpack("<I", h)[0]
            n = w & ~LZ_STORED
            want = min(LZ_BLOCK, size - got)
            if (w & LZ_STORED and n != want) or (not w & LZ_STORED and not 0 < n < want):
                return False
            f.seek(n, 1)
            p += 4 + n
            got += want
    return p == total


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
    # f: (path, sha, size, policy, crc) or (..., set)
    path, sha, size, pol, crc = f[:5]
    if fmt == 1:
        return "f %s %d %s %s" % (sha, size, pol, path)
    if fmt < 4:
        return "f %s %s %d %s %s" % (sha, crc, size, pol, path)
    if len(f) < 6 or not f[5]:
        raise ManifestError("%s: no set" % path)
    return "f %s %s %d %s %s %s" % (sha, crc, size, pol, f[5], path)


def _body(dirs, files, fmt=FORMAT):
    lines = ["d %s" % d for d in sorted(dirs, key=lambda s: s.encode())]
    lines += [_line(f, fmt) for f in sorted(files, key=lambda f: f[0].encode())]
    return lines


def snapshot_id(dirs, files, fmt=FORMAT):
    h = hashlib.sha256()
    for line in _body(dirs, files, fmt):
        h.update(line.encode("utf-8") + b"\n")
    return h.hexdigest()[:ID_DIGITS]


ABI_H = os.path.join("sw", "common", "zabi.h")
ABI_RE = re.compile(r'^#define\s+Z_ABI_ID\s+"([0-9a-f]+)"\s*$', re.M)


def read_abi(root):
    """The ABI id sw/common/zabi.h commits, or "" when the tree has none.
    `zrelease check` and the builds make sure it is current."""
    try:
        with open(os.path.join(root, ABI_H), encoding="utf-8") as f:
            m = ABI_RE.search(f.read())
    except OSError:
        return ""
    return m.group(1) if m else ""


def make(pl, name, version, source, packs=(), made=None, abi="",
         sets=None, installed=None):
    """A manifest for a mkfatimg.Plan. Hashes every file. `sets`: the
    [(name, title, description)] offered (a release); `installed`: the
    set names a card has (an image)."""
    files = []
    for dest, src, policy in pl.manifest_files():
        sha, crc = hashes_file(src)
        files.append((dest, sha, os.path.getsize(src), policy, crc,
                      pl.set_of[dest]))
    dirs = [d.strip("/") for d in pl.dirs]
    return build(name, version, source, dirs, files, packs=packs, made=made,
                 abi=abi, sets=sets, installed=installed)


def build(name, version, source, dirs, files, packs=(), made=None, abi="",
          sets=None, installed=None):
    """files: (path, sha256, size, policy, crc32[, set]); a file without a
    set is in `base`."""
    files = [tuple(f) if len(f) >= 6 else tuple(f) + ("base",) for f in files]
    for f in files:
        path, _sha, _size, policy, crc, st = f
        _check_path(path)
        if st != NO_SET:
            check_set(st)
        if policy not in ("r", "k"):
            raise ManifestError("%s: unknown policy %r" % (path, policy))
        if len(crc) != 8 or crc != crc.lower():
            raise ManifestError("%s: crc %r is not 8 hex digits" % (path, crc))
        int(crc, 16)
    for d in dirs:
        _check_path(d)
    if abi and not re.fullmatch(r"[0-9a-f]{1,23}", abi):
        raise ManifestError("abi %r is not an id" % abi)
    m = Manifest()
    if made is None:
        made = datetime.datetime.now(datetime.timezone.utc) \
            .strftime("%Y-%m-%dT%H:%M:%SZ")
    for sname, title, desc in sets or ():
        check_set(sname)
        if "|" in title or "\n" in title + desc:
            raise ManifestError("set %s: a title has no | or newline" % sname)
    for sname in installed or ():
        check_set(sname)
    m.dirs = sorted(dirs, key=lambda s: s.encode())
    m.files = sorted((f[:5] for f in files), key=lambda f: f[0].encode())
    m.set_of = {f[0]: f[5] for f in files}
    m.sets = list(sets or ())
    m.installed = list(installed) if installed is not None else None
    m.header = {
        "name": name,
        "version": version,
        "snapshot": snapshot_id(dirs, files),
        "abi": abi,
        "source": source or "unknown",
        "made": made,
        "packs": " ".join(packs),
    }
    head = ["%s %d" % (MAGIC, FORMAT)]
    for k in ("name", "version", "snapshot", "abi", "source", "made", "packs"):
        if k == "abi" and not abi:
            continue
        head.append(("%s %s" % (k, m.header[k])).rstrip())
    for sname, title, desc in m.sets:
        head.append(("set %s %s | %s" % (sname, title, desc)).rstrip())
    if m.installed is not None:
        head.append(("installed %s" % " ".join(m.installed)).rstrip())
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
            st = None
            if fmt == 1:
                parts = line.split(" ", 4)
                crc = None
            elif fmt >= 4:
                parts = line.split(" ", 6)
                if len(parts) != 7:
                    raise ManifestError("line %d: %r" % (n, line))
                crc = parts.pop(2)
                st = parts.pop(4)
                if len(crc) != 8:
                    raise ManifestError("line %d: %r" % (n, line))
                int(crc, 16)
                if st != NO_SET:
                    check_set(st)
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
            if st is not None:
                m.set_of[parts[4]] = st
        elif line.startswith("d "):
            _check_path(line[2:])
            m.dirs.append(line[2:])
        elif m.files or m.dirs:
            raise ManifestError("line %d: header after entries" % n)
        else:
            k, _sp, v = line.partition(" ")
            if k == "set":
                sname, _sp, rest = v.partition(" ")
                title, _sp, desc = rest.partition(" | ")
                check_set(sname)
                m.sets.append((sname, title.strip(), desc.strip()))
            elif k == "installed":
                m.installed = v.split()
                for sname in m.installed:
                    check_set(sname)
            else:
                m.header[k] = v
    paths = [f[0] for f in m.files]
    if paths != sorted(paths, key=lambda s: s.encode()) \
            or len(set(paths)) != len(paths):
        raise ManifestError("file lines are not sorted and unique")
    if m.header.get("snapshot") != snapshot_id(m.dirs, full_files(m), fmt):
        raise ManifestError("snapshot id does not match the entries")
    return m


def text_in_format(m, fmt):
    """m written as an older format (for tests, and for reading what an
    older release tool wrote): no sets before 4, no CRC in 1."""
    head = []
    for line in m.text.split("\n")[1:]:
        if line.startswith(("d ", "f ")) or not line:
            break
        k = line.partition(" ")[0]
        if fmt < 4 and k in ("set", "installed"):
            continue
        if k == "snapshot":
            line = "snapshot %s" % snapshot_id(m.dirs, full_files(m), fmt)
        head.append(line)
    return "\n".join(["%s %d" % (MAGIC, fmt)] + head +
                     _body(m.dirs, full_files(m), fmt)) + "\n"


def full_files(m):
    """m's files with their sets: (path, sha, size, policy, crc, set)."""
    return [f + (m.set_of.get(f[0]),) for f in m.files]


def version_text(m):
    """/sys/version: which release wrote the card.

    The first line is what it has always been ("zeitlos 0.0.7"); the
    second says which snapshot of it, and is what `update` and people
    read to tell two builds of one version apart.
    """
    return "zeitlos %s\nsnapshot %s %s %s %s\n" % (
        m.header["version"], m.header["name"], m.id,
        m.header["source"], m.header["made"])
