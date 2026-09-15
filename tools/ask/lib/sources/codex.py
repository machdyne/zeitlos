#
# Ark Codex -- one directory per topic, `summary.txt` plus `img_N.jpg`.
#
# Accepts either the extracted tree or `codex.tgz` straight out of the
# ark repo, because both are things somebody actually has. The tarball
# path is the reproducible one -- a tarball has a hash, a directory
# somebody rsynced does not -- so recipes should prefer it.
#
# 363 topics, ~3.2MB of text at the R1/2026-08 snapshot. Each topic is
# already one coherent self-contained article, which makes the Codex
# the easiest thing in Ark to retrieve over: no chunking heuristic has
# to guess where an article starts.
#
# THE IMAGES ARE NOT INGESTED. `img_0.jpg` is a JPEG and this tree
# decodes PNG (sw/common/zimg.c, docs/png.md), not JPEG. They are
# recorded in meta so a later pass can transcode them on the host
# without re-reading the tarball, and ignored otherwise.
#

import io
import os
import tarfile

from ..source import Document, adapter

SOURCE = "codex"


def _from_tar(path):
    with tarfile.open(path, "r:*") as tf:
        members = {}
        for m in tf.getmembers():
            if not m.isfile():
                continue
            parts = m.name.split("/")
            if len(parts) < 2:
                continue
            topic = parts[-2]
            base = parts[-1]
            members.setdefault(topic, {})[base] = m
        for topic in sorted(members):
            entry = members[topic]
            if "summary.txt" not in entry:
                continue
            fh = tf.extractfile(entry["summary.txt"])
            if fh is None:
                continue
            text = fh.read().decode("utf-8", "replace")
            imgs = sorted(k for k in entry if k.lower().endswith(
                (".jpg", ".jpeg", ".png")))
            yield Document(topic, topic, text, SOURCE,
                           {"images": imgs, "container": os.path.basename(path)})


def _from_dir(path):
    # The tarball unpacks to codex/topic/<Topic>/summary.txt; a hand
    # copy may be rooted anywhere above that. Walk for summary.txt
    # rather than assuming a depth, since assuming one is how this
    # silently produced zero documents the first time.
    for dirpath, _dirnames, filenames in os.walk(path):
        if "summary.txt" not in filenames:
            continue
        topic = os.path.basename(dirpath)
        with open(os.path.join(dirpath, "summary.txt"),
                  encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        imgs = sorted(f for f in filenames
                      if f.lower().endswith((".jpg", ".jpeg", ".png")))
        yield Document(topic, topic, text, SOURCE, {"images": imgs})


@adapter("codex")
def load_codex(path, opts):
    if os.path.isdir(path):
        docs = list(_from_dir(path))
    else:
        docs = list(_from_tar(path))
    if not docs:
        raise ValueError(
            "codex adapter found no summary.txt under %s -- check the path. "
            "(The tarball unpacks to codex/topic/<Topic>/summary.txt; an "
            "older layout used topic.txt and is NOT handled, deliberately, "
            "because silently ingesting nothing is worse than failing.)"
            % path)
    return docs
