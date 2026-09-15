#
# Zeitlos ask -- getting the upstream corpora.
#
# A recipe names the repositories it needs and this module makes sure
# they are on disk. No submodules: the corpora are large, they move on
# their own schedule, and most people building Zeitlos will never build
# a distribution at all. Pinning them into this tree would make every
# clone pay for something almost nobody uses.
#
#     repo = ark https://github.com/machdyne/ark
#     source = codex @ark/data/arklite/codex.tgz
#
# `@ark/...` resolves against the checkout. A plain relative path still
# works and still resolves against the recipe file, so a local corpus
# that is not in any repository needs no ceremony.
#
# -- where checkouts go --
#
# tools/ask/.cache/<name>/ by default, which is gitignored. Override
# with ASK_CACHE for a shared or pre-seeded location -- useful on a
# machine that already has the ark repo somewhere, and necessary on one
# with no network at all.
#
# -- pinning --
#
# `repo = ark <url> ref=<commit-or-tag>` pins a checkout. A recipe
# without a ref tracks the default branch, which is convenient while
# developing a distribution and wrong for one you intend to ship: the
# build hash covers document bytes, so an unpinned recipe produces a
# different distribution every time upstream moves, with nothing in the
# recipe recording which one you got.
#
# `ask ingest` prints the resolved commit for exactly this reason. Put
# it in the recipe before you ship.
#

import os
import subprocess


class FetchError(Exception):
    pass


def cache_root(here):
    return os.environ.get("ASK_CACHE") or os.path.join(here, ".cache")


def _git(args, cwd=None):
    r = subprocess.run(["git"] + args, cwd=cwd, capture_output=True, text=True)
    if r.returncode != 0:
        raise FetchError("git %s failed:\n%s%s"
                         % (" ".join(args), r.stdout, r.stderr))
    return r.stdout.strip()


def ensure(name, url, ref=None, root=None, update=False, quiet=False):
    """Clone `url` to <root>/<name> if absent. Returns (path, commit).

    Existing checkouts are left alone unless `update` is set. A corpus
    somebody has edited locally -- which is a perfectly reasonable way
    to test an adapter -- should not be silently reset by a build.
    """
    if root is None:
        raise FetchError("no cache root")
    path = os.path.join(root, name)

    if not os.path.isdir(os.path.join(path, ".git")):
        if os.path.exists(path):
            # A directory that is not a checkout: somebody pre-seeded
            # it, or ASK_CACHE points at a tree of plain copies. Use it
            # and report no commit rather than refusing.
            return path, None
        os.makedirs(root, exist_ok=True)
        if not quiet:
            print("  cloning %s -> %s" % (url, path))
        args = ["clone", "--quiet"]
        # A full clone only when a ref is pinned: `git clone --depth 1`
        # cannot check out an arbitrary commit afterwards, and finding
        # that out after a 400MB download is a bad time.
        if not ref:
            args += ["--depth", "1"]
        args += [url, path]
        _git(args)
    elif update:
        if not quiet:
            print("  updating %s" % path)
        _git(["fetch", "--quiet", "--all"], cwd=path)

    if ref:
        try:
            _git(["checkout", "--quiet", ref], cwd=path)
        except FetchError:
            _git(["fetch", "--quiet", "origin", ref], cwd=path)
            _git(["checkout", "--quiet", ref], cwd=path)

    try:
        commit = _git(["rev-parse", "HEAD"], cwd=path)
    except FetchError:
        commit = None
    return path, commit


def resolve(loc, repos, recipe_dir):
    """Turn a source location into an absolute path.

    `@name/rest` -> inside repo `name`. Anything else is relative to
    the recipe file, as before.
    """
    if loc.startswith("@"):
        name, _, rest = loc[1:].partition("/")
        if name not in repos:
            raise FetchError(
                "source refers to @%s but the recipe has no "
                "`repo = %s <url>` line" % (name, name))
        return os.path.join(repos[name]["path"], rest)
    if os.path.isabs(loc):
        return loc
    return os.path.normpath(os.path.join(recipe_dir, loc))


# ======================================================================
# Downloads
# ======================================================================
#
# `ask fetch` does its own downloads rather than shelling out to ark's
# scripts/build.sh. Three reasons, none of them about that script being
# badly written:
#
#   1. It is meant to be SOURCED (`. scripts/build.sh`) and leaks
#      $BUILD/$ARK/$DATA into the caller's shell.
#   2. It writes into the ark clone, which ensure() above manages and
#      may re-checkout at a pinned ref.
#   3. It has no `set -e`, so a failed step is invisible -- which is
#      how two real bugs survived in it. As of the version this was
#      written against, `wget -nc -P $BUILD/ciaimg.zip` creates a
#      DIRECTORY of that name rather than a file (-P is
#      --directory-prefix, not --output-document), so the Factbook
#      images are never extracted; and the last line copies
#      `data/scroll.gz`, which does not exist -- the repo has
#      scroll-r0.gz and scroll-r1.gz.
#
# Those are upstream's to fix. This module does the same downloads
# declaratively from the recipe so that a pack build does not depend on
# the script being correct on the day somebody runs it.
#
# WHAT THIS ADDS over the script: resumable transfers, optional sha256
# pinning, a real exit code, and a git-lfs pointer check that says what
# to do instead of failing inside tar.

import hashlib
import shutil
import tarfile
import zipfile

try:
    from urllib.request import Request, urlopen
except ImportError:                                     # pragma: no cover
    Request = urlopen = None

# A git-lfs pointer is a ~130 byte text file. Anything claiming to be a
# corpus archive and smaller than this is one.
LFS_POINTER_MAX = 1024

UA = "zeitlos-ask/1 (+https://github.com/machdyne/zeitlos)"


def is_lfs_pointer(path):
    try:
        if os.path.getsize(path) > LFS_POINTER_MAX:
            return False
        with open(path, "rb") as fh:
            return fh.read(64).startswith(b"version https://git-lfs")
    except OSError:
        return False


def check_lfs(path):
    """Raise with instructions if `path` is an unfetched LFS pointer.

    Without this the symptom is `xz: File format not recognized` from
    inside tar, several lines after the actual problem, with no mention
    of git-lfs anywhere.
    """
    if is_lfs_pointer(path):
        raise FetchError(
            "%s is still a git-lfs pointer -- the real file was never "
            "fetched.\n"
            "    sudo apt install git-lfs\n"
            "    cd %s && git lfs install && git lfs pull"
            % (path, os.path.dirname(os.path.dirname(path)) or "."))


def lfs_pull(repo_path, quiet=False):
    """Best-effort `git lfs pull`. Returns True if it ran cleanly."""
    if shutil.which("git-lfs") is None:
        return False
    try:
        if not quiet:
            print("  git lfs pull in %s" % repo_path)
        _git(["lfs", "pull"], cwd=repo_path)
        return True
    except FetchError:
        return False


def _sha256(path, chunk=1 << 20):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        while True:
            b = fh.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def download(url, dest, sha256=None, quiet=False, force=False):
    """Fetch `url` to `dest`, resuming a partial transfer.

    The partial file is `dest + ".part"` and is only moved into place
    once the transfer completes and the checksum (if given) matches.
    That is the difference that matters against `wget -nc`: -nc keeps
    whatever is already at the destination, so an interrupted download
    leaves a truncated file that is never retried and fails later
    inside an unzip.
    """
    if os.path.exists(dest) and not force:
        if sha256 and _sha256(dest) != sha256:
            if not quiet:
                print("  checksum mismatch, refetching %s"
                      % os.path.basename(dest))
        else:
            return dest, False

    os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
    part = dest + ".part"
    have = os.path.getsize(part) if os.path.exists(part) else 0

    req = Request(url, headers={"User-Agent": UA})
    if have:
        req.add_header("Range", "bytes=%d-" % have)

    try:
        resp = urlopen(req, timeout=60)
    except Exception as e:
        raise FetchError("%s: %s" % (url, e))

    # A server that ignores Range answers 200 with the whole file; only
    # append when it actually agreed to resume.
    append = (getattr(resp, "status", resp.getcode()) == 206)
    if have and not append:
        have = 0

    total = resp.headers.get("Content-Length")
    total = (int(total) + have) if total else 0

    if not quiet:
        print("  %s %s" % ("resuming" if append else "fetching",
                           os.path.basename(dest)), end="", flush=True)

    got = have
    with open(part, "ab" if append else "wb") as fh:
        while True:
            buf = resp.read(1 << 16)
            if not buf:
                break
            fh.write(buf)
            got += len(buf)
            if not quiet and total:
                print("\r  %s %s  %3d%%  %.1f MB"
                      % ("resuming" if append else "fetching",
                         os.path.basename(dest), 100 * got // total,
                         got / 1e6), end="", flush=True)
    if not quiet:
        print("\r  fetched  %s  %.1f MB%s"
              % (os.path.basename(dest), got / 1e6, " " * 12))

    if sha256:
        actual = _sha256(part)
        if actual != sha256:
            raise FetchError(
                "%s: sha256 mismatch\n    expected %s\n    got      %s\n"
                "  The partial file is at %s; delete it to retry."
                % (url, sha256, actual, part))

    os.replace(part, dest)
    return dest, True


def sniff(path):
    """What is this file really? Returns "zip", "tar", "html" or "plain".

    Content, not extension. Two reasons that matters here:
    Project Gutenberg serves plain text from URLs whose basename ends
    in .txt.utf-8, and it answers a dead URL with an HTML error page
    carrying a 200 -- so a "download succeeded" can still be a web page
    where an archive was expected. Feeding either to tarfile produces
    `not a gzip file / not a bzip2 file / not an lzma file / invalid
    header`, which says nothing useful about what actually went wrong.
    """
    try:
        if zipfile.is_zipfile(path):
            return "zip"
    except OSError:
        pass
    try:
        if tarfile.is_tarfile(path):
            return "tar"
    except (OSError, tarfile.TarError):
        pass
    try:
        with open(path, "rb") as fh:
            head = fh.read(512).lstrip()[:64].lower()
        if head.startswith(b"<!doctype html") or head.startswith(b"<html"):
            return "html"
    except OSError:
        pass
    return "plain"


def _common_root(names):
    """The single top-level directory every entry shares, or None.

    "" is returned as None too: an archive with anything at its root
    has no common directory to strip.
    """
    root = None
    for n in names:
        parts = [p for p in n.replace("\\", "/").split("/") if p and p != "."]
        if len(parts) < 2:
            return None                 # a file at the archive root
        if root is None:
            root = parts[0]
        elif parts[0] != root:
            return None
    return root


def unpack(archive, into, strip="auto", quiet=False):
    """Extract a zip or tar into `into`, stripping leading path
    components.

    `strip="auto"` (the default) strips ONE level when every entry in
    the archive shares a single top-level directory, and nothing
    otherwise. That is what somebody means by "unpack this into here",
    and it is the difference between a recipe that works and one that
    silently produces wikipedia/wikipedia/articles.lst.

    Auto rather than a hand-written number per line, because the number
    is a property of an archive nobody building a recipe has
    necessarily downloaded yet -- wiki2008.txz is a 137MB git-lfs
    object. Guessing it wrong fails much later, as an adapter that
    cannot find its list file.

    `strip=<n>` still forces an exact count when an archive needs it.

    ark's scripts/build.sh does the same job with `mv .../pg/* ...`
    followed by rmdir, which misses dotfiles and breaks if the layout
    changes.
    """
    os.makedirs(into, exist_ok=True)

    kind = sniff(archive)
    if kind == "html":
        raise FetchError(
            "%s is an HTML page, not an archive.\n"
            "  The server answered with a page (often a 404 carrying a 200)\n"
            "  rather than the file. Check the URL -- Project Gutenberg in\n"
            "  particular has moved its download paths."
            % archive)
    if kind == "plain":
        raise FetchError(
            "%s is not a zip or a tar.\n"
            "  If it is meant to be a single file rather than an archive,\n"
            "  give the fetch line `unpack=no` and make <dest> the filename."
            % archive)

    # Resolve the strip count BEFORE extracting anything, so target()
    # closes over a settled number.
    if kind == "zip":
        with zipfile.ZipFile(archive) as zf:
            names = [i.filename for i in zf.infolist() if not i.is_dir()]
    else:
        with tarfile.open(archive, "r:*") as tf:
            names = [m.name for m in tf.getmembers() if m.isfile()]

    if strip == "auto":
        root = _common_root(names)
        nstrip = 1 if root else 0
        if root and not quiet:
            print("  stripping leading %s/" % root)
    else:
        nstrip = strip

    def target(name):
        parts = [p for p in name.replace("\\", "/").split("/") if p
                 and p != "."]
        if any(p == ".." for p in parts):
            return None                     # never write outside `into`
        parts = parts[nstrip:]
        return os.path.join(into, *parts) if parts else None

    n = 0
    if kind == "zip":
        with zipfile.ZipFile(archive) as zf:
            for info in zf.infolist():
                if info.is_dir():
                    continue
                out = target(info.filename)
                if not out:
                    continue
                os.makedirs(os.path.dirname(out), exist_ok=True)
                with zf.open(info) as src, open(out, "wb") as dst:
                    shutil.copyfileobj(src, dst)
                n += 1
    else:
        with tarfile.open(archive, "r:*") as tf:
            for m in tf.getmembers():
                if not m.isfile():
                    continue
                out = target(m.name)
                if not out:
                    continue
                os.makedirs(os.path.dirname(out), exist_ok=True)
                src = tf.extractfile(m)
                if src is None:
                    continue
                with src, open(out, "wb") as dst:
                    shutil.copyfileobj(src, dst)
                n += 1
    if not quiet:
        print("  unpacked %d files -> %s" % (n, into))
    return n
