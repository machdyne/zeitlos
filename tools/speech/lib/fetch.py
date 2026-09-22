"""
Zeitlos
Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.

Fetching the training inputs, for tools/speech.

Everything here downloads from the ORIGINAL source named in the spec.
That is deliberate for now and it is also the reason
docs/tts_data.md's "Open item: mirror the sources" is an open item: an original
can move, rate-limit or disappear, and a build that depends on one is
only reproducible until it does. Each source may already name a
`mirror`, used with --mirror.

Downloads land in tools/speech/.cache (override with SPEECH_CACHE),
which is gitignored. They are resumable: a partial transfer goes to
<name>.part and an interrupted run continues with an HTTP Range
request rather than starting a multi-gigabyte download again. Same
reasoning as tools/ask's lib/fetch.py, which this follows.

Extraction takes ONLY the members the spec lists, and refuses the ones
it names outright -- see the cmudict note in dist/en.spec.
"""

import hashlib
import os
import sys
import tarfile
import urllib.error
import urllib.request
import zipfile

UA = "zeitlos-speech/1 (+https://github.com/machdyne/zeitlos)"


class FetchError(Exception):
    pass


def cache_root(here):
    return os.environ.get("SPEECH_CACHE") or os.path.join(here, ".cache")


def _human(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return "%.1f%s" % (n, unit) if unit != "B" else "%dB" % n
        n /= 1024.0


def download(url, dest, quiet=False):
    """Resumable GET to `dest`. Returns dest. Skips a completed file."""

    if os.path.exists(dest):
        if not quiet:
            print("  have %s (%s)" % (os.path.basename(dest), _human(os.path.getsize(dest))))
        return dest

    os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
    part = dest + ".part"
    have = os.path.getsize(part) if os.path.exists(part) else 0

    req = urllib.request.Request(url, headers={"User-Agent": UA})
    if have:
        req.add_header("Range", "bytes=%d-" % have)

    try:
        resp = urllib.request.urlopen(req, timeout=60)
    except urllib.error.HTTPError as e:
        if have and e.code == 416:      # already complete
            os.rename(part, dest)
            return dest
        raise FetchError("%s: HTTP %s" % (url, e.code))
    except Exception as e:
        raise FetchError("%s: %s" % (url, e))

    # A server that ignores Range restarts the file; do not append to
    # what we already had, or the result is silently corrupt.
    if have and getattr(resp, "status", None) != 206:
        have = 0

    # file:// and some servers report no length; the progress line is
    # the only thing that needs it.
    length = getattr(resp, "length", None)
    total = length + have if length is not None else None
    mode = "ab" if have else "wb"
    got = have

    with open(part, mode) as f:
        while True:
            chunk = resp.read(256 * 1024)
            if not chunk:
                break
            f.write(chunk)
            got += len(chunk)
            if not quiet and sys.stdout.isatty():
                pct = (" %3d%%" % (100 * got / total)) if total else ""
                sys.stdout.write("\r  %s%s  " % (_human(got), pct))
                sys.stdout.flush()

    if not quiet and sys.stdout.isatty():
        sys.stdout.write("\r")
    if total and got < total:
        raise FetchError("%s: short read (%d of %d bytes)" % (url, got, total))

    os.rename(part, dest)
    if not quiet:
        print("  got  %s (%s)" % (os.path.basename(dest), _human(os.path.getsize(dest))))
    return dest


def download_parts(urls, dest, quiet=False):
    """
    A file published in pieces, joined back together.

    GitHub caps a release asset at 2GB and the speech corpus is larger,
    so a mirror of it is split. Each piece is fetched (and resumed)
    like any other download; they are concatenated only once all of
    them are present, so an interrupted run never leaves a
    half-assembled file that looks complete.
    """

    if os.path.exists(dest):
        if not quiet:
            print("  have %s (%s)" % (os.path.basename(dest), _human(os.path.getsize(dest))))
        return dest

    parts = []
    for i, url in enumerate(urls):
        part = "%s.part%02d" % (dest, i + 1)
        if not quiet:
            print("  piece %d of %d" % (i + 1, len(urls)))
        download(url, part, quiet)
        parts.append(part)

    tmp = dest + ".joining"
    with open(tmp, "wb") as out:
        for part in parts:
            with open(part, "rb") as f:
                while True:
                    chunk = f.read(1 << 20)
                    if not chunk:
                        break
                    out.write(chunk)
    os.rename(tmp, dest)

    for part in parts:
        os.remove(part)

    if not quiet:
        print("  joined %s (%s)" % (os.path.basename(dest), _human(os.path.getsize(dest))))
    return dest


def split_file(path, out_dir, piece_bytes, quiet=False):
    """The other half: cut a file into pieces small enough to publish."""

    os.makedirs(out_dir, exist_ok=True)
    base = os.path.basename(path)
    made = []

    with open(path, "rb") as f:
        i = 0
        while True:
            i += 1
            out = os.path.join(out_dir, "%s.part%02d" % (base, i))
            written = 0
            with open(out, "wb") as o:
                while written < piece_bytes:
                    chunk = f.read(min(1 << 20, piece_bytes - written))
                    if not chunk:
                        break
                    o.write(chunk)
                    written += len(chunk)
            if written == 0:
                os.remove(out)
                break
            made.append(out)
            if not quiet:
                print("  %s (%s)" % (os.path.basename(out), _human(written)))
            if written < piece_bytes:
                break

    return made


def subset_tar(archive, kind, dest, keep, quiet=False):
    """
    Writes a smaller archive holding the first `keep` clips and
    everything that is not a clip (the transcript, the readme).

    Phase 8 needs a few hours of speech, not twenty-four: every diphone
    appears many times over well before the end. A subset is a
    single publishable file and a download somebody will actually
    finish.
    """

    mode = "r:gz" if kind == "tar.gz" else "r:bz2"
    out_mode = "w:bz2" if kind == "tar.bz2" else "w:gz"
    clips = 0

    with tarfile.open(archive, mode) as src, tarfile.open(dest, out_mode) as dst:
        for m in src:
            if not m.isfile():
                continue
            is_clip = m.name.lower().endswith((".wav", ".flac", ".mp3"))
            if is_clip:
                if clips >= keep:
                    continue
                clips += 1
            fh = src.extractfile(m)
            dst.addfile(m, fh)

    if not quiet:
        print("  %s: %d clips, %s" % (os.path.basename(dest), clips,
                                      _human(os.path.getsize(dest))))
    return dest


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def _safe(name):
    """A member path that cannot escape the destination directory."""
    return not (name.startswith("/") or ".." in name.split("/"))


def extract(archive, kind, out_dir, takes, refuses, limit=None):
    """
    Extracts the listed members. `takes` is [(member, saved-as)];
    a member ending in '/' takes everything under it, at most `limit`
    files (used for the speech corpus, where a few hundred clips is
    enough to work with).
    """

    os.makedirs(out_dir, exist_ok=True)
    written = []

    refused = []

    def want(name):
        """
        Where this member should be saved, or None.

        A refused path is only an ERROR if a take rule would otherwise
        have pulled it in -- a `take` of a whole directory, say. Merely
        being present in the archive is not: the Moby distribution
        ships cmudict alongside everything else, and ignoring it is the
        normal case. Those are counted and reported instead.
        """
        rel = None
        for member, saved in takes:
            if member.endswith("/"):
                if name.startswith(member) and not name.endswith("/"):
                    rel = os.path.join(saved, os.path.basename(name))
            elif name == member:
                rel = saved
        if name in refuses:
            if rel is not None:
                raise FetchError(
                    "%s: a take rule would extract %s, which the spec refuses "
                    "(see dist/en.spec)" % (os.path.basename(archive), name))
            refused.append(name)
            return None
        return rel

    def put(name, data):
        rel = want(name)
        if rel is None:
            return False
        if limit and rel.count("/") and len(written) >= limit:
            return False
        dest = os.path.join(out_dir, rel)
        if not _safe(rel):
            raise FetchError("unsafe member path: %s" % rel)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as f:
            f.write(data)
        written.append(rel)
        return True

    if kind in ("tar.gz", "tar.bz2"):
        mode = "r:gz" if kind == "tar.gz" else "r:bz2"
        with tarfile.open(archive, mode) as tf:
            for m in tf:
                if not m.isfile():
                    continue
                if want(m.name) is None:
                    continue
                fh = tf.extractfile(m)
                put(m.name, fh.read())
    elif kind == "zip":
        with zipfile.ZipFile(archive) as zf:
            for name in zf.namelist():
                if name.endswith("/"):
                    continue
                if want(name) is None:
                    continue
                put(name, zf.read(name))
    else:
        raise FetchError("cannot extract kind %r" % kind)

    if refused:
        print("  left alone (excluded by the spec): %s" % ", ".join(refused))

    return written


def verify(out_dir, source, quiet=False):
    """
    Checks what was extracted against the spec's sha256 lines.

    A file with no recorded hash is NOT an error: it prints the
    computed one so it can be pasted into the spec. That is how a
    source gets pinned the first time somebody fetches it -- see
    docs/tts_data.md.
    """

    ok = True
    for name in sorted(set(saved for _, saved in source.takes)):
        path = os.path.join(out_dir, name)
        if not os.path.exists(path) or os.path.isdir(path):
            continue
        got = sha256(path)
        want = source.sums.get(name)
        if want is None:
            print("  %s: sha256 %s  <- not pinned; add to the spec" % (name, got))
        elif got != want:
            print("  %s: CHECKSUM MISMATCH\n    spec %s\n    file %s" % (name, want, got))
            ok = False
        elif not quiet:
            print("  %s: sha256 ok" % name)
    return ok
