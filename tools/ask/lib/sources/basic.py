#
# The remaining source adapters. Grouped in one file because each is
# twenty lines; split them out when one stops being twenty lines.
#
#   medline   MedlinePlus health topics (Ark Medium)
#   books     gzipped plain-text books (Ark Lite)
#   scroll    the Ark Scroll, one file, split on its own headings
#   mdtree    a directory of markdown -- this is how zeitlos/docs gets in
#   plain     a directory of .txt, for anything else
#
# All of them take `prefix=` to override the dataset name, which is
# what the card subdirectory is named after. Two markdown trees in one
# distribution need two prefixes or they collide.
#

import glob
import html
import gzip
import os
import re
import tarfile

from .. import html2md
from ..source import Document, adapter


# ---------------------------------------------------------------------
# Project Gutenberg boilerplate
#
# Every PG text carries a licence header and footer, and they are
# IDENTICAL across the whole collection. Left in, a corpus of 600 books
# contains 600 near-copies of the same few thousand words -- about
# lawyers, disks, copying, distributing, refunds and DELETING FILES.
#
# That is not a tidiness problem, it is a retrieval problem. "how to
# delete a file" returned Alice in Wonderland, then Sun Tzu, then
# Hamlet: not because anything in those books is relevant, but because
# all three carry a licence that says "if you either delete this
# file..." and "keep this file on your own disk". The one genuinely
# relevant passage, the Codex's Unix article, came fourth.
#
# Alice is 250 lines of licence before the first word of the story.
#
# TWO FORMATS. Modern texts mark the body with
# "*** START OF THE PROJECT GUTENBERG EBOOK ... ***". The 2001-era
# etexts in Ark Lite predate that and end their header with
# "*END*THE SMALL PRINT! ... *END*" instead. Both are handled; a file
# matching neither is left alone, because guessing at where a book
# starts is worse than shipping a licence.
# ---------------------------------------------------------------------

_PG_START = re.compile(
    r"^\*\*\*\s*START OF (?:THE|THIS) PROJECT GUTENBERG.*$", re.M | re.I)
_PG_END = re.compile(
    r"^\*\*\*\s*END OF (?:THE|THIS) PROJECT GUTENBERG.*$", re.M | re.I)
_PG_SMALLPRINT_END = re.compile(
    r"^\*END\*\s*THE SMALL PRINT!.*$", re.M | re.I)
_PG_OLD_END = re.compile(
    r"^\s*End of (?:The )?Project Gutenberg(?:'s)? "
    r"(?:Etext|EBook|E-text).*$", re.M | re.I)


def strip_gutenberg(text):
    """Returns the body, or the original if no markers are found."""
    start = 0
    m = _PG_START.search(text)
    if m:
        start = m.end()
    else:
        m = _PG_SMALLPRINT_END.search(text)
        if m:
            start = m.end()

    end = len(text)
    m = _PG_END.search(text, start)
    if m:
        end = m.start()
    else:
        m = _PG_OLD_END.search(text, start)
        if m:
            end = m.start()

    body = text[start:end]
    # A marker in the wrong place -- a table of contents that mentions
    # one, say -- could leave almost nothing. Keep the original rather
    # than ship a truncated book.
    if len(body.strip()) < 0.2 * len(text.strip()):
        return text
    return body.strip() + "\n"


def _read(path):
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8", errors="replace") as fh:
        return fh.read()


# -- MedlinePlus -------------------------------------------------------
#
# `medline.lst` maps `en/NNNN.txt` to a human title. Without it the
# documents are numbered files and the titles in a result list would be
# "1591.txt", which is how the first prototype presented a perfectly
# good answer about cryptosporidiosis.

@adapter("medline")
def load_medline(path, opts):
    src = opts.get("prefix", "medline")
    root = path
    tmp = None
    if not os.path.isdir(path):
        import tempfile
        tmp = tempfile.mkdtemp(prefix="ask-medline-")
        with tarfile.open(path, "r:*") as tf:
            tf.extractall(tmp)
        root = tmp

    lst = None
    for cand in ("medline.lst", "medline/medline.lst"):
        p = os.path.join(root, cand)
        if os.path.exists(p):
            lst = p
            break
    titles = {}
    if lst:
        for line in open(lst, encoding="utf-8", errors="replace"):
            parts = line.strip().split(None, 1)
            if len(parts) == 2:
                titles[os.path.basename(parts[0])] = parts[1]

    docs = []
    for f in sorted(glob.glob(os.path.join(root, "**", "*.txt"),
                              recursive=True)):
        base = os.path.basename(f)
        if base == "medline.lst":
            continue
        title = titles.get(base)
        if title is None:
            # No title means no entry in the list, which means this is
            # not a topic file. Skipping is right; guessing a title
            # from the number is not.
            continue
        docs.append(Document(base, title, _read(f), src))
    return docs


# -- books -------------------------------------------------------------
#
# Long. The Bible alone is ~8MB of a 15MB set, and a recipe that wants
# the practical texts without it should say so with `exclude=`.
#
# `split=<bytes>` breaks anything larger into separate documents, which
# matters more than it looks. sw/apps/read/read.c indexes lazily and
# its frontier only moves forward, so opening a document at byte N
# streams N bytes off the card first. A hit in the back of an 8MB Bible
# is a very long wait; the same hit in a 60KB chapter is instant.
#
# Splitting prefers chapter headings and falls back to paragraph
# boundaries, so a split never lands mid-sentence.

# A chapter heading: the keyword, then a NUMBER -- roman, arabic, or a
# single letter for "Appendix B". The number used to be
# `[A-Za-z0-9IVXLC]+`, which is just "any word" (IVXLC adds nothing to
# A-Za-z), so "ACT New Zealand [Rodney HIDE]; Green Party..." in the
# CIA Factbook matched as a chapter heading. It was the only match in
# 13.5MB, so the whole book became one piece named after that line and
# the size pass cut it into "... (2)", "... (3)". Any line beginning
# Part, Book, Section, Scene or Act followed by a word did this.
_CHAPTER = re.compile(
    r"^\s*((?:CHAPTER|Chapter|BOOK|Book|PART|Part|APPENDIX|Appendix|LETTER|"
    r"ACT|SCENE|PSALM|Section|SECTION)\s+"
    r"(?:[IVXLCDM]+|[0-9]+|[A-Z])\.?)(?![A-Za-z0-9])\s*(.{0,70})$")


def _heads_re(opts):
    """The heading pattern for a source. `head=<regex>` overrides the
    chapter default -- a corpus whose sections are country names or
    `@Afghanistan` lines can say so rather than being cut by size."""
    pat = opts.get("head")
    if not pat:
        return _CHAPTER
    try:
        return re.compile(pat, re.M)
    except re.error as e:
        raise ValueError("head=%s is not a regular expression: %s"
                         % (pat, e))


def _split_points(text, target, heads_re=None):
    """Byte-safe split points, preferring chapter headings.

    Two passes. The first finds chapter headings; the second subdivides
    any piece that is still too big at blank lines.

    The second pass is not optional. Chapter detection succeeds
    partially far more often than it fails outright -- a book with four
    `BOOK I` markers and no chapter markers under them produces four
    pieces of 700KB, which is exactly the case this whole option exists
    to avoid. Preferring headings is a nicety; bounding the size is the
    requirement, so the size wins.
    """
    lines = text.split("\n")
    offs, blanks = [], []
    off = 0
    heads = []
    rx = heads_re or _CHAPTER
    for ln in lines:
        offs.append(off)
        m = rx.match(ln)
        if m:
            # A pattern with a capture group labels the section with
            # it: `head=^@(.+)$` over the Factbook's "@Afghanistan
            # (South Asia)" gives "Afghanistan (South Asia)".
            lbl = (m.group(1) if m.groups() else ln).strip()
            heads.append((off, " ".join(lbl.split())))
        elif not ln.strip():
            blanks.append(off)
        off += len(ln) + 1

    pts = []
    last = -1
    # Merging runs of headings guards against front matter and tables
    # of contents, which produce dozens in a row. A recipe that gave
    # its own `head=` pattern has said what a section is, and its
    # sections may legitimately be small -- the Factbook's countries
    # run from a couple of KB to a couple of hundred -- so merging
    # them by size would put twenty countries in one document named
    # after the first.
    gap = 512 if heads_re else target // 2
    for pos, title in heads:
        if last < 0 or pos - last >= gap:
            pts.append((pos, title))
            last = pos
    if not pts or pts[0][0] != 0:
        pts.insert(0, (0, None))

    # Second pass: bound every piece.
    out = []
    limit = max(target, 4096)
    for i, (start, title) in enumerate(pts):
        end = pts[i + 1][0] if i + 1 < len(pts) else len(text)
        out.append((start, title))
        if end - start <= limit * 2:
            continue
        cut = start + limit
        part = 2
        while cut < end - limit // 2:
            # Nearest blank line at or after the target, so a cut never
            # lands mid-sentence.
            nxt = None
            for b in blanks:
                if b >= cut:
                    nxt = b
                    break
            if nxt is None or nxt >= end:
                break
            label = ("%s (%d)" % (title, part)) if title else "part %d" % part
            out.append((nxt, label))
            part += 1
            cut = nxt + limit
    out.sort()
    return out


@adapter("books")
def load_books(path, opts):
    src = opts.get("prefix", "books")
    exclude = set(x for x in opts.get("exclude", "").split(",") if x)
    split = int(opts.get("split", "0"), 0)
    titles = {}
    lst = os.path.join(path, "books.lst")
    if os.path.exists(lst):
        for line in open(lst, encoding="utf-8", errors="replace"):
            parts = line.strip().split(None, 1)
            if len(parts) == 2:
                titles[parts[0]] = parts[1]

    docs = []
    for f in sorted(glob.glob(os.path.join(path, "*"))):
        base = os.path.basename(f)
        if base == "books.lst" or os.path.isdir(f):
            continue
        if base in exclude or os.path.splitext(base)[0] in exclude:
            continue
        title = titles.get(base, os.path.splitext(base)[0])
        text = _read(f)
        if opts.get("gutenberg", "1") not in ("0", "no", "false"):
            text = strip_gutenberg(text)

        if not split or len(text) <= split:
            docs.append(Document(base, title, text, src))
            continue

        pts = _split_points(text, split, _heads_re(opts))
        for i, (start, head) in enumerate(pts):
            end = pts[i + 1][0] if i + 1 < len(pts) else len(text)
            body = text[start:end]
            if len(body.strip()) < 400:
                continue
            part = "%s -- %s" % (title, head) if head else title
            # Key carries the part index so the uid is stable across
            # rebuilds as long as the upstream text has not changed.
            docs.append(Document("%s#%d" % (base, i), part, body, src,
                                 {"of": base, "part": i}))
    return docs


# -- Ark Scroll --------------------------------------------------------
#
# One file with `# Topic` headings. Split into one Document per
# top-level heading rather than kept whole: a 4MB single document would
# be one entry in the doc table and every chunk of it would carry the
# same title, so a result would say "Ark Scroll R1" and nothing more.

_H1 = re.compile(r"^# +(.+)$", re.M)


@adapter("scroll")
def load_scroll(path, opts):
    src = opts.get("prefix", "scroll")
    text = _read(path)
    marks = list(_H1.finditer(text))
    if not marks:
        return [Document(os.path.basename(path), "Ark Scroll", text, src)]
    docs = []
    for i, m in enumerate(marks):
        start = m.start()
        end = marks[i + 1].start() if i + 1 < len(marks) else len(text)
        title = m.group(1).strip()
        body = text[start:end]
        if len(body.strip()) < 200:
            continue
        docs.append(Document(title, title, body, src))
    return docs


# -- markdown tree (zeitlos/docs) --------------------------------------
#
# The title is the first `# ` heading if there is one, else the
# filename. `docs/` here is consistent about having one, which is why
# a result can say "Memory Translation Unit" rather than "mtu.md".

@adapter("mdtree")
def load_mdtree(path, opts):
    src = opts.get("prefix", "docs")
    exts = tuple(opts.get("ext", ".md").split(","))
    # `exclude=a.md,b.md` drops files by basename.
    #
    # Needed sooner than expected: docs/ask_app.md documents this
    # search engine and therefore QUOTES several dozen example queries
    # verbatim, which makes it the best lexical match in the corpus for
    # every one of them. "water treatment" returned the documentation
    # about water treatment queries rather than any passage about
    # water. Nothing is malfunctioning -- the document really does
    # contain those words -- but it is not the answer.
    exclude = set(x.strip() for x in opts.get("exclude", "").split(",")
                  if x.strip())
    docs = []
    for f in sorted(glob.glob(os.path.join(path, "**", "*"), recursive=True)):
        if os.path.isdir(f) or not f.endswith(exts):
            continue
        if os.path.basename(f) in exclude:
            continue
        text = _read(f)
        rel = os.path.relpath(f, path)
        m = _H1.search(text)
        title = m.group(1).strip() if m else os.path.basename(f)
        docs.append(Document(rel, title, text, src))
    return docs


@adapter("plain")
def load_plain(path, opts):
    src = opts.get("prefix", "text")
    exts = tuple(opts.get("ext", ".txt").split(","))
    docs = []
    for f in sorted(glob.glob(os.path.join(path, "**", "*"), recursive=True)):
        if os.path.isdir(f) or not f.endswith(exts):
            continue
        rel = os.path.relpath(f, path)
        docs.append(Document(rel, os.path.splitext(os.path.basename(f))[0],
                             _read(f), src))
    return docs


# -- listed ------------------------------------------------------------
#
# The shape Ark uses over and over: a `.lst` of `relpath<space>title`
# beside a directory of text files. medline.lst, books.lst,
# pgcdrom.lst and wikipedia's articles.lst are all this.
#
# One adapter rather than four near-identical ones. `medline` and
# `books` above stay as their own entry points because each has
# something extra (medline skips files with no list entry; books
# splits), but anything new that follows the pattern needs no code at
# all -- just a `source = listed ... lst=whatever.lst` line.
#
# DEDUPLICATION IS ON BY DEFAULT. pgcdrom.lst has five entries for
# "1001 Nights [Arabian Nights]" -- 11001108.txt through 51001108.txt,
# the CD's different encodings of one etext. Indexing all five puts
# five identical passages in every result list and inflates the index
# by the same factor. Which one is "right" is not knowable from the
# list, so the first in file order wins, which is at least stable.

@adapter("listed")
def load_listed(path, opts):
    src = opts.get("prefix", "listed")
    split = int(opts.get("split", "0"), 0)
    dedupe = opts.get("dedupe", "1") not in ("0", "no", "false")

    lstname = opts.get("lst")
    if lstname:
        lst = os.path.join(path, lstname)
        if not os.path.exists(lst):
            # The directory existed (the fetch created it) but the list
            # file is not in it. Almost always a layout mismatch: the
            # archive unpacked one level deeper than expected. Say
            # where it actually is rather than raising
            # FileNotFoundError from inside open().
            found = sorted(glob.glob(os.path.join(path, "**", lstname),
                                     recursive=True))
            hint = ""
            if found:
                rel = os.path.relpath(found[0], path)
                hint = ("\n  It IS at %s -- the archive unpacked one level "
                        "deeper than the\n  fetch line expected. `strip=auto` "
                        "(the default) handles this; a\n  pack fetched by an "
                        "older version may need re-fetching:\n"
                        "      ./tools/ask/ask fetch <recipe> --force"
                        % rel)
            else:
                hint = ("\n  Nothing named %s exists anywhere under that "
                        "directory. Has the\n  matching `fetch =` line run? "
                        "`ask fetch <recipe>` does the downloads."
                        % lstname)
            raise ValueError("listed adapter: %s not found%s" % (lst, hint))
    else:
        cand = sorted(glob.glob(os.path.join(path, "*.lst")))
        if not cand:
            raise ValueError(
                "listed adapter found no .lst under %s -- pass lst=<name>"
                % path)
        lst = cand[0]

    root = os.path.join(path, opts["root"]) if "root" in opts \
        else os.path.dirname(lst)

    seen_titles = set()
    docs = []
    for line in open(lst, encoding="utf-8", errors="replace"):
        parts = line.strip().split(None, 1)
        if len(parts) != 2:
            continue
        rel, title = parts
        # Ark's lists are HTML-escaped: pgcdrom.lst has `Happy Prince
        # &amp; Other Tales` and `Number &quot;e&quot;`. The title is
        # what the device shows on every result line, so decode it.
        title = html.unescape(title)
        if dedupe and title in seen_titles:
            continue
        f = os.path.join(root, rel)
        if not os.path.exists(f):
            # A list entry with no file is normal: the lists ship in
            # the ark repo and the payloads are fetched separately by
            # scripts/build.sh. Skipping quietly would hide a wrong
            # path, so the caller counts these -- see the build output.
            continue
        seen_titles.add(title)
        text = _read(f)
        if opts.get("gutenberg", "0") not in ("0", "no", "false"):
            text = strip_gutenberg(text)
        # HTML sources (the 2008 Wikipedia selection) become markdown
        # here, because what is emitted is what is INDEXED and what a
        # preview quotes. `html=no` turns it off; `html=yes` forces it
        # on a file the sniff misses. See lib/html2md.py.
        mode = opts.get("html", "auto")
        if mode not in ("0", "no", "false"):
            if mode in ("1", "yes", "true") or html2md.looks_like_html(text):
                text = html2md.convert(text, title,
                                       sections=opts.get("sections"))
        if not split or len(text) <= split:
            docs.append(Document(rel, title, text, src))
            continue
        pts = _split_points(text, split, _heads_re(opts))
        for i, (start, head) in enumerate(pts):
            end = pts[i + 1][0] if i + 1 < len(pts) else len(text)
            body = text[start:end]
            if len(body.strip()) < 400:
                continue
            part = "%s -- %s" % (title, head) if head else title
            docs.append(Document("%s#%d" % (rel, i), part, body, src,
                                 {"of": rel, "part": i}))
    if not docs:
        raise ValueError(
            "listed adapter loaded 0 documents from %s.\n"
            "  The .lst was found but none of the files it names exist "
            "under %s.\n"
            "  Ark ships the lists in the repo and fetches the payloads "
            "separately -- run ark's scripts/build.sh first, or point "
            "root= at where they landed." % (lst, root))
    return docs


# -- onefile -----------------------------------------------------------
#
# A single large text that is one work: the CIA World Factbook is one
# 35830.txt of a few megabytes. `split=` is what makes it usable --
# without it the whole Factbook is one document and a hit in Zimbabwe
# means streaming past every other country first (read.c's index only
# moves forward).

@adapter("onefile")
def load_onefile(path, opts):
    src = opts.get("prefix", "text")
    split = int(opts.get("split", "0"), 0)
    title = opts.get("title", os.path.splitext(os.path.basename(path))[0])
    text = _read(path)
    # Off by default, as for `listed`; `gutenberg=yes` for a single
    # Project Gutenberg ebook such as the Factbook, whose licence would
    # otherwise be indexed as text -- the "Alice in Wonderland answers
    # `how do I delete a file`" failure in docs/ask_app.md.
    if opts.get("gutenberg", "0") not in ("0", "no", "false"):
        text = strip_gutenberg(text)
    if not split or len(text) <= split:
        return [Document(os.path.basename(path), title, text, src)]
    docs = []
    pts = _split_points(text, split, _heads_re(opts))
    for i, (start, head) in enumerate(pts):
        end = pts[i + 1][0] if i + 1 < len(pts) else len(text)
        body = text[start:end]
        if len(body.strip()) < 400:
            continue
        part = "%s -- %s" % (title, head) if head else title
        docs.append(Document("%s#%d" % (os.path.basename(path), i),
                             part, body, src, {"part": i}))
    return docs
