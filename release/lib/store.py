#
# Zeitlos release tooling -- the update release (docs/update.md).
#
# What `zrelease tftp` writes, and what `update` on the board reads:
#
#   <store>/                     /srv/tftp/release by default
#     LATEST                     the name `update latest` means: "v0.0.7"
#     INDEX                      one line per release in the store
#     v0.0.7/manifest            what a card of that release holds
#     v0.0.7/manifest.prev       the one it replaced, kept only so a
#                                board halfway through an update from it
#                                still finds its files (below)
#     v0.0.7/update              apps/update, under a name a person can
#                                type: how a card without `update` gets
#                                one (`tget <server> release/v0.0.7/update
#                                /apps/update`)
#     o/3f/3f2a9c01e4b7d0a1c2e4  a file's contents, named by its hash
#
# -- why objects and not a copy of the card --
#
#   - TFTP filenames: net takes at most 63 bytes, and a card path under
#     release/v0.0.7/ is already 52 for the zcc headers. An object's
#     name is 33 bytes whatever the card path.
#   - TFTP cannot list a directory, so the board asks for exactly the
#     files the manifest names, and nothing else needs a name.
#   - A file is stored once, however many releases or paths have it.
#   - Publishing is atomic without moving directories: the objects are
#     written first, and the manifest is renamed into place last. A
#     board that read the old manifest keeps finding its objects.
#   - GitHub release assets are a flat list; objects and manifests map
#     onto that one-to-one when updates come from there.
#
# -- pruning --
#
# After publishing, objects that no release's manifest or manifest.prev
# names are deleted. manifest.prev is not a history (there is none, by
# decision): it is one step of grace for a board mid-update, the
# "publish while a board is updating" case. Publishing twice in quick
# succession can still pull files from under a board that read the
# manifest before both; it then reports the missing file and is run
# again.
#

import fcntl
import os
import re
import shutil

import manifest as manifest_mod

NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,31}$")
RESERVED = ("o",)
OBJ_RE = re.compile(r"^[0-9a-f]{%d}$" % manifest_mod.OBJECT_DIGITS)

# The longest name the board will ask TFTP for, under the store's own
# directory on the server ("release/" by default). net's limit is 63
# bytes (sw/apps/net/tftp.c, get_filename[64]).
TFTP_NAME_MAX = 63

# Where the updater is on a card (mkfatimg's list), copied to
# <name>/update in the store.
BOOT_PATH = "apps/update"


class StoreError(Exception):
    pass


class Stats(object):
    def __init__(self):
        self.objects_new = 0
        self.bytes_new = 0
        self.objects_kept = 0
        self.pruned = 0
        self.unchanged = False
        self.previous_id = None


def check_name(name):
    if not NAME_RE.match(name) or name in RESERVED:
        raise StoreError(
            "release name %r: use letters, digits, '.', '_' and '-', up "
            "to 32, starting with a letter or digit (and not 'o')" % name)


def _write_atomic(path, data, mode=0o644):
    tmp = "%s.tmp%d" % (path, os.getpid())
    with open(tmp, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.chmod(tmp, mode)
    os.replace(tmp, path)


def _mkdir(path):
    os.makedirs(path, exist_ok=True)
    os.chmod(path, 0o755)


def releases(store):
    """[(name, Manifest)] for every release in the store, sorted."""
    out = []
    if not os.path.isdir(store):
        return out
    for n in sorted(os.listdir(store)):
        p = os.path.join(store, n, "manifest")
        if n in RESERVED or not NAME_RE.match(n) or not os.path.isfile(p):
            continue
        with open(p, encoding="utf-8") as f:
            out.append((n, manifest_mod.parse(f.read())))
    return out


def _referenced(store):
    """Every object name a manifest or manifest.prev in the store uses.

    Refuses (raises) if any of them cannot be read: pruning on a partial
    view would delete files a release still needs.
    """
    refs = set()
    for n in os.listdir(store):
        if n in RESERVED or not NAME_RE.match(n):
            continue
        for leaf in ("manifest", "manifest.prev"):
            p = os.path.join(store, n, leaf)
            if not os.path.isfile(p):
                continue
            try:
                with open(p, encoding="utf-8") as f:
                    m = manifest_mod.parse(f.read())
            except (OSError, UnicodeDecodeError,
                    manifest_mod.ManifestError) as e:
                raise StoreError("%s: %s -- not pruning" % (p, e))
            for _path, sha, _size, _pol, _crc in m.files:
                refs.add(sha[:manifest_mod.OBJECT_DIGITS])
    return refs


def _prune(store):
    refs = _referenced(store)
    odir = os.path.join(store, "o")
    pruned = 0
    if not os.path.isdir(odir):
        return 0
    for fan in sorted(os.listdir(odir)):
        d = os.path.join(odir, fan)
        if not os.path.isdir(d) or not re.match(r"^[0-9a-f]{2}$", fan):
            continue
        for leaf in os.listdir(d):
            p = os.path.join(d, leaf)
            # Leftovers of an interrupted publish (we hold the lock, so
            # nobody else is writing one now), and objects nothing uses.
            stale_tmp = ".tmp" in leaf and OBJ_RE.match(leaf.split(".")[0])
            if stale_tmp or (OBJ_RE.match(leaf) and leaf not in refs):
                os.unlink(p)
                pruned += 0 if stale_tmp else 1
        if not os.listdir(d):
            os.rmdir(d)
    return pruned


def _index_text(store):
    lines = ["# name snapshot version source made files bytes"]
    for n, m in releases(store):
        lines.append("%s %s %s %s %s %d %d" % (
            n, m.id, m.header.get("version", "-"),
            m.header.get("source", "-"), m.header.get("made", "-"),
            len(m.files), sum(f[2] for f in m.files)))
    return "\n".join(lines) + "\n"


def publish(store, m, sources, latest=True, verbose=True):
    """Publish manifest `m` into `store`.

    `sources` maps each file's sha256 to a path holding those bytes.
    Returns Stats. Safe to run again at any point after an interruption.
    """
    check_name(m.name)
    st = Stats()

    longest = max(len("release/" + manifest_mod.object_path("0" * 64)),
                  len("release/%s/manifest.prev" % m.name))
    if longest > TFTP_NAME_MAX:
        raise StoreError("names would be %d bytes, past TFTP's %d"
                         % (longest, TFTP_NAME_MAX))

    _mkdir(store)
    lockf = open(os.path.join(store, ".lock"), "w")
    try:
        fcntl.flock(lockf, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        raise StoreError("%s: another zrelease is publishing here" % store)
    try:
        # 1. objects
        done = set()
        for path, sha, size, _pol, _crc in m.files:
            if sha in done:
                continue
            done.add(sha)
            obj = os.path.join(store, manifest_mod.object_path(sha))
            if os.path.exists(obj):
                if os.path.getsize(obj) != size:
                    raise StoreError(
                        "%s: is %d bytes, %s should be %d -- a damaged "
                        "store or a hash collision; delete it and publish "
                        "again" % (obj, os.path.getsize(obj), path, size))
                st.objects_kept += 1
                continue
            src = sources[sha]
            _mkdir(os.path.dirname(obj))
            tmp = "%s.tmp%d" % (obj, os.getpid())
            shutil.copyfile(src, tmp)
            # Checked after the copy: a source rebuilt while publishing
            # would otherwise land under the wrong name.
            if manifest_mod.sha256_file(tmp) != sha:
                os.unlink(tmp)
                raise StoreError("%s changed while publishing; run again"
                                 % src)
            os.chmod(tmp, 0o644)
            os.replace(tmp, obj)
            st.objects_new += 1
            st.bytes_new += size

        # 2. the manifest, last of the release's own files
        rdir = os.path.join(store, m.name)
        _mkdir(rdir)
        cur = os.path.join(rdir, "manifest")
        old_text = None
        if os.path.isfile(cur):
            with open(cur, encoding="utf-8") as f:
                old_text = f.read()
            try:
                st.previous_id = manifest_mod.parse(old_text).id
            except manifest_mod.ManifestError:
                st.previous_id = None
        if st.previous_id == m.id:
            # Same files: leave it, `made` and all, so INDEX still says
            # when these files were first published.
            st.unchanged = True
        else:
            if old_text is not None:
                _write_atomic(os.path.join(rdir, "manifest.prev"),
                              old_text.encode("utf-8"))
            _write_atomic(cur, m.text.encode("utf-8"))

        # The updater itself, by a fixed name, for a card that has none
        # yet (or one too old to read this manifest's format).
        boot = [sha for path, sha, _s, _p, _c in m.files if path == BOOT_PATH]
        bdst = os.path.join(rdir, "update")
        if boot:
            want = os.path.join(store, manifest_mod.object_path(boot[0]))
            if not (os.path.isfile(bdst) and
                    manifest_mod.sha256_file(bdst) == boot[0]):
                with open(want, "rb") as f:
                    _write_atomic(bdst, f.read())
        elif os.path.exists(bdst):
            os.unlink(bdst)

        # 3. prune, index, latest
        st.pruned = _prune(store)
        _write_atomic(os.path.join(store, "INDEX"),
                      _index_text(store).encode("utf-8"))
        if latest:
            _write_atomic(os.path.join(store, "LATEST"),
                          (m.name + "\n").encode("utf-8"))
    finally:
        lockf.close()
    return st


def sources_for(pl, m):
    """sha256 -> source path, for publish(), from a plan and its manifest."""
    by_path = {dest: src for dest, src, _p in pl.manifest_files()}
    return {sha: by_path[path] for path, sha, _s, _p, _c in m.files}
