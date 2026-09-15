#
# Zeitlos ask -- what lands on the SD card, and under what names.
#
# ============================ PACKS =================================
#
# A distribution is not one monolithic thing. It is a set of PACKS,
# each self-contained, each independently installable by unzipping it
# onto the card:
#
#     /ask/minimal/index.zak     Codex + Scroll + docs
#     /ask/medium/index.zak      Ark Medium
#     /ask/recipes/index.zak     something somebody built themselves
#
# `ask` enumerates the subdirectories of /ask at startup and queries
# every pack it finds. Dropping in a new one requires no rebuild of
# anything already on the card, and deleting one is `rm -r`.
#
# This is the structure that matters most for what this is FOR. The
# corpus is going to keep growing, from sources that are not Ark and
# not this repository, on schedules nobody controls. A design where
# adding Ark Medium means regenerating the index for `docs/` would make
# every addition a whole-distribution event.
#
# -- how results from different packs combine --
#
# By RANK, not by score. Two packs may have been built with different
# encoders, whose vectors live in different spaces and whose dot
# products are not comparable. Reciprocal rank fusion does not care: it
# consumes orderings. That is already how the lexical and dense halves
# are fused inside one pack, so combining packs needs no new machinery
# and no requirement that packs agree about anything.
#
# Each pack names the encoder it needs (`encoder_id` in its index) and
# `ask` loads each distinct one once. Packs that share an encoder --
# the normal case -- share the loaded weights.
#
# ========================= NAMING POLICY ============================
#
# sw/os/fs/fatfs/ffconf.h sets FF_USE_LFN 0, so FatFs on the device can
# only see 8.3 names. A host tool writing with mtools can create
# "Alexander Graham Bell.txt" happily and the device will see some
# mangled alias, or not find it at all.
#
# Not hypothetical and not new: 36 of the 85 files in docs/ already
# exceed 8.3 and are reachable on the card only through whatever alias
# the FAT driver invented.
#
# So the default policy is NUMERIC 8.3, with the index owning the
# mapping back to a title:
#
#     /ark/codex/00000042.txt     "Amino acid"
#
# THIS IS A POLICY, NOT AN ASSUMPTION. If FF_USE_LFN is turned on later
# -- and the Codex's real filenames are worth having, they are the
# topic names -- switch a recipe to `naming = long` and rebuild.
# Nothing else changes: the index stores whatever path was assigned,
# the device opens it by name, and `read` does not care. The one
# constraint that survives either way is Z_WM_ARG_MAX (96 bytes,
# zwm.h), which a long name plus a "#offset" suffix can exceed and a
# numeric one cannot -- so `long` clamps and warns.
#
# What numeric costs: browsing /ark in `files` shows a wall of digits.
# The mitigation is that `ask` browses by title, because it has the
# index. What it buys: short paths, no long-name parsing on the device,
# and deterministic numbering, so the same recipe over the same inputs
# writes the same tree and a distribution is diffable.
#

import os
import re
import unicodedata

# Dataset directories live under this, UNDER THE PACK THAT BUILT THEM:
#
#     /ark/<pack>/<dataset>/00000042.txt
#
# A pack owns every byte it indexed. That costs duplication -- two
# packs that both include docs/ ship two copies of it -- and it buys
# two things that are not negotiable.
#
# FIRST, packs cannot collide. `minimal` ships Scroll R1 and `medium`
# ships Scroll R0; both are the dataset named "scroll" and both number
# from 1. Sharing /ark/scroll would have one silently overwrite the
# other, and the symptom would be confidently ranked results showing
# text from the wrong release. This was not hypothetical -- it was
# found by co-installing two packs onto one card and looking.
#
# SECOND, and more important: A PACK MUST NOT INDEX FILES IT DOES NOT
# OWN. The obvious economy is to point the index at the /docs that
# tools/mkfatimg.sh already writes. But docs/ changes every release and
# a pack is rebuilt once in a while -- that is the entire point of
# separating the two cadences -- so the next release would shift every
# byte offset in an installed pack, and nothing anywhere would say so.
# Every preview would quietly show the wrong paragraph.
#
# The shared /docs stays exactly where it is, for `read` and `files`.
# `ask` just does not point at it.
CARD_ROOT = "ark"

# Where packs go, one subdirectory each.
ASK_ROOT = "ask"

# Documents per directory before splitting into numbered
# subdirectories. A FAT directory lookup is a linear scan whether or
# not LFN is on; 1,023 MedlinePlus topics is 32KB to walk, which is
# tolerable, and Ark Medium's article set is not. 0 disables.
SPLIT_AT = 2048

# EVERYTHING IS EMITTED AS .md, INCLUDING PLAIN TEXT.
#
# sw/common/ztype.c maps "MD" to `read` and "TXT" to `text` -- the
# editor. So a corpus emitted as .txt would have every link in a
# generated index.md open an EDITOR on a 344KB book, and `files` would
# do the same on a double-click. `read` is the right viewer for all of
# this: it renders, it follows links, it indexes lazily, and it has no
# maximum file size. `text` has none of those properties.
#
# The cost is that plain-text sources are rendered THROUGH the markdown
# parser. In practice this is mild and mostly an improvement -- the
# Codex summaries are already markdown, MedlinePlus is clean prose, and
# a field manual's `*` bullets become real bullets. The visible
# artefacts are Gutenberg's indented passages, which md.c reads as code
# blocks and draws in the body font anyway.
#
# The alternative was adding a fourth extension to ztype.c mapping to
# `read`. That is a system-wide change to serve one app, and it would
# still leave `text` as the handler for the .txt files already on
# cards. Not worth it; revisit if plain-text rendering turns out worse
# than this paragraph claims.
EXT_BY_SOURCE = {}
DEFAULT_EXT = "md"

# Generated per-directory indexes. Not part of the corpus and never
# indexed -- they are written after the chunker has run.
INDEX_NAME = "index.md"

# Z_WM_ARG_MAX, from sw/common/zwm.h. A path must leave room for
# "#<offset>" -- ten digits and the separator -- or `ask` cannot hand
# the document to `read` at the right place.
ARG_MAX = 96
FRAGMENT_MAX = 11


class CardError(Exception):
    pass


def _check_83(name):
    stem, _, ext = name.partition(".")
    if len(stem) > 8 or len(ext) > 3:
        raise CardError("`%s` is not an 8.3 name and cannot be written to "
                        "the card (FatFs here is FF_USE_LFN 0)" % name)
    return name


_SLUG_BAD = re.compile(r"[^A-Za-z0-9 ._-]+")


def _slug(title, maxlen=48):
    """A long but still FAT-legal filename.

    Only used by `naming = long`. FAT forbids  " * / : < > ? \\ |  and
    control characters; everything else is allowed, including spaces.
    Non-ASCII is transliterated rather than passed through, because
    FF_LFN_UNICODE is 0 as well -- a long-name build would be OEM code
    page, not UTF-16, and a Cyrillic title would not survive the trip.
    """
    s = unicodedata.normalize("NFKD", title)
    s = s.encode("ascii", "ignore").decode("ascii")
    s = _SLUG_BAD.sub(" ", s).strip()
    s = re.sub(r"\s+", " ", s)
    return (s[:maxlen].strip() or "untitled")


def dataset_dir(source, pack):
    _check_83(source)
    _check_83(pack)
    return "%s/%s/%s" % (CARD_ROOT, pack, source)


def pack_dir(pack):
    _check_83(pack)
    return "%s/%s" % (ASK_ROOT, pack)


def assign_paths(docs, pack, naming="numeric"):
    """Give every document its card path. Returns (paths, warnings).

    Deterministic: sorted by (source, key), numbered from 1 within each
    source. Not derived from the uid hash -- a hash would scatter
    consecutive articles across the directory and make the linear FAT
    scan touch more sectors than it has to. The uid is still in the
    index for stable identity; this is only where the bytes live.
    """
    if naming not in ("numeric", "long"):
        raise CardError("unknown naming policy `%s` (numeric | long)"
                        % naming)

    order = sorted(range(len(docs)), key=lambda i: (docs[i].source,
                                                    docs[i].key))
    paths = [None] * len(docs)
    counters = {}
    used = set()
    warned = []

    for i in order:
        d = docs[i]
        n = counters.get(d.source, 0) + 1
        counters[d.source] = n
        ext = EXT_BY_SOURCE.get(d.source, DEFAULT_EXT)
        base = dataset_dir(d.source, pack)
        if SPLIT_AT and n > SPLIT_AT:
            base = "%s/%03d" % (base, (n - 1) // SPLIT_AT)

        if naming == "numeric":
            name = _check_83("%08d.%s" % (n, ext))
        else:
            stem = _slug(d.title)
            cand, k = "%s.%s" % (stem, ext), 1
            # Collisions are real: the Codex has distinct topics whose
            # titles differ only in punctuation, and slugging removes
            # punctuation. Suffix rather than silently overwrite.
            while ("%s/%s" % (base, cand)).lower() in used:
                k += 1
                cand = "%s~%d.%s" % (stem, k, ext)
            name = cand
            if len(base) + 1 + len(name) + FRAGMENT_MAX > ARG_MAX:
                # Too long to hand to `read` with an offset. Fall back
                # to numeric for this one file rather than emit a path
                # that silently truncates inside Z_WM_SET_ARG.
                name = _check_83("%08d.%s" % (n, ext))
                warned.append(d.title)

        full = "%s/%s" % (base, name)
        used.add(full.lower())
        paths[i] = full

    return paths, warned


def emit(docs, paths, outdir):
    """Write the document tree exactly as it will appear on the card.

    UTF-8 of the normalised text. The index's offsets were measured
    against these same bytes, so this must not transform anything.
    """
    written = 0
    for d, p in zip(docs, paths):
        full = os.path.join(outdir, p)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        data = d.text.encode("utf-8")
        with open(full, "wb") as fh:
            fh.write(data)
        written += len(data)
    return written


def verify(docs, paths, chunks, outdir):
    """Re-read the emitted tree and check every chunk range resolves.

    The most important check in the build. A chunk whose offset is
    wrong does not fail loudly on the device -- it shows the user a
    preview of the wrong paragraph, or half of one, and nothing
    anywhere says so.
    """
    cache = {}
    problems = []
    for c in chunks:
        p = paths[c.doc]
        if p not in cache:
            with open(os.path.join(outdir, p), "rb") as fh:
                cache[p] = fh.read()
        blob = cache[p]
        if c.off + c.length > len(blob):
            problems.append("%s: chunk %d+%d past end (%d bytes)"
                            % (p, c.off, c.length, len(blob)))
            continue
        got = blob[c.off:c.off + c.length].decode("utf-8", "replace")
        if got != c.text:
            problems.append("%s: chunk at %d does not match what was indexed"
                            % (p, c.off))
    return problems


def check_seek_cost(docs, paths, limit_bytes):
    """Flag documents big enough that jumping into them will be slow.

    sw/apps/read/read.c builds its index LAZILY and its frontier only
    ever moves forward, so opening a document at byte N means streaming
    N bytes off the card first. docs/sdcard.md measures 604 KB/s
    through FatFs and that is an optimistic figure; at a realistic
    300-600 KB/s a jump into the back of an 8MB book is a ten to
    thirty second stare at nothing.

    The fix is upstream of the device: split large works into smaller
    documents at build time -- see the `books` adapter's `split=`
    option. This reports which documents still need it.
    """
    big = []
    for d, p in zip(docs, paths):
        n = len(d.text.encode("utf-8"))
        if n > limit_bytes:
            big.append((p, d.title, n))
    return sorted(big, key=lambda x: -x[2])


# -- generated indexes -------------------------------------------------
#
# The numbered filenames are unreadable by design (see the naming
# policy above) and `ask` is the intended way around that. But `ask`
# should not be the ONLY way around it: a card is a physical object
# that outlives the software on it, and a directory of eight-digit
# files with no key is a corpus somebody cannot recover by hand.
#
# So every directory gets an index.md of links. sw/apps/read/read.c
# already resolves a relative link against the directory of the file it
# is showing (read.c:2480) and hands off by extension through
# ztype.h, so these are navigable with nothing but `read` -- no index,
# no encoder, no `ask` at all. `files` reaches them too, since .md
# opens in `read`.
#
# They are written AFTER chunking and are never themselves indexed:
# they are navigation, not corpus, and indexing a page of links would
# put a list of titles into the retrieval results.

def _md_escape(s):
    # Titles come from upstream and contain brackets often enough to
    # matter -- "[Sic]", "Water [Chapter 3]". An unescaped ] ends the
    # link text early and the rest of the title leaks into the page as
    # literal characters.
    return s.replace("\\", "\\\\").replace("[", "\\[").replace("]", "\\]")


def write_indexes(docs, paths, outdir, pack, meta=None):
    """Write index.md into the pack root and each dataset directory.

    Returns the number of files written.
    """
    by_dir = {}
    for d, p in zip(docs, paths):
        dsdir = "%s/%s" % (CARD_ROOT + "/" + pack, d.source)
        by_dir.setdefault(dsdir, []).append((d, p))

    written = 0
    root = "%s/%s" % (CARD_ROOT, pack)

    for dsdir in sorted(by_dir):
        entries = sorted(by_dir[dsdir], key=lambda dp: dp[0].title.lower())
        name = dsdir.rsplit("/", 1)[-1]
        out = ["# %s" % name, ""]
        out.append("%d document%s. Links open in `read`."
                   % (len(entries), "" if len(entries) == 1 else "s"))
        if name in ((meta or {}).get("generated") or []):
            out.append("")
            out.append("**This dataset was written by a language model, "
                       "not by people.** Treat it")
            out.append("as a starting point rather than as a source.")
        out.append("")
        for d, p in entries:
            # Relative to this index, which may be one level above a
            # split subdirectory (000/, 001/).
            rel = p[len(dsdir) + 1:]
            out.append("- [%s](%s)" % (_md_escape(d.title), rel))
        out.append("")
        full = os.path.join(outdir, dsdir, INDEX_NAME)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as fh:
            fh.write("\n".join(out).encode("utf-8"))
        written += 1

    m = meta or {}
    out = ["# %s" % pack, ""]
    if m.get("description"):
        out += [m["description"], ""]
    if m.get("version"):
        out.append("Version %s, built %s, %d document%s."
                   % (m.get("version"), m.get("built", "?"), len(docs),
                      "" if len(docs) == 1 else "s"))
        out.append("")
    out.append("## Datasets")
    out.append("")
    gen = (m.get("generated") or [])
    for dsdir in sorted(by_dir):
        name = dsdir.rsplit("/", 1)[-1]
        n = len(by_dir[dsdir])
        out.append("- [%s](%s/%s) -- %d document%s%s"
                   % (name, name, INDEX_NAME, n, "" if n == 1 else "s",
                      "  *(model-written)*" if name in gen else ""))
    out += ["",
            "Searchable with the `ask` app, which matches on meaning "
            "rather than",
            "on the words you happened to type. This page is the "
            "fallback: every",
            "document is reachable from here with nothing but `read`.",
            ""]

    # Provenance, on the machine rather than only in a host-side
    # manifest. A card that outlives the build machine should be able
    # to say where its contents came from -- which upstream, at which
    # commit -- without anything else being available.
    srcs = m.get("sources") or []
    repos = m.get("repos") or {}
    if srcs or repos:
        out += ["## Provenance", ""]
        for sr in srcs:
            out.append("- `%s` from `%s` -- %d documents, %.2f MB"
                       % (sr["adapter"], sr["path"], sr["docs"],
                          sr["bytes"] / 1e6))
        if repos:
            out.append("")
            for name in sorted(repos):
                r = repos[name]
                out.append("- `%s` %s at `%s`"
                           % (name, r["url"],
                              (r.get("commit") or "?")[:12]))
        out.append("")
    full = os.path.join(outdir, root, INDEX_NAME)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "wb") as fh:
        fh.write("\n".join(out).encode("utf-8"))
    written += 1
    return written
