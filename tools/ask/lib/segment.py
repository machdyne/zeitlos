#
# Zeitlos ask -- turning documents into retrievable chunks.
#
# A Chunk is a byte range of a document, plus the heading path it sits
# under. The byte range is the contract with the device: `ask` shows a
# preview by reading exactly those bytes off the card, and `read` opens
# the same file at the same offset. Nothing may rewrite the text after
# this point.
#
# -- why the heading path is separate from the text --
#
# The heading path is prepended to the chunk when it is EMBEDDED and is
# not part of the stored range. A chunk from FM 21-76 about pressure
# points embeds as
#
#     Survival > First Aid > Bleeding: If you cannot remember the exact
#     location of the pressure points...
#
# but the range on the card is the paragraph alone. Without this, a
# mid-document paragraph is embedded with no idea what it is about,
# which measurably wrecks retrieval on long manuals: the early
# prototype returned Zeitlos build documentation for "how do I build a
# fire without matches", because "build" was the strongest signal in a
# context-free paragraph and nothing said "this is the fire chapter".
#
# Prepending costs nothing at query time and nothing on the card.
#
# -- why not fixed-size windows --
#
# Fixed windows cut mid-sentence and mid-procedure, and step 3 of a
# splinting sequence is not useful on its own. Sections first,
# paragraphs within a section, and a size target that is a target
# rather than a limit.
#

import re

# Target chunk size in characters. Not bytes: the split is done on the
# decoded string and converted at the end, so a multi-byte character
# cannot land in the middle of a range.
#
# 1800 is a compromise measured against the corpus rather than chosen:
# large enough that a MedlinePlus topic is usually one chunk (so a
# result is the whole answer), small enough that a Gutenberg chapter is
# several (so a result is not a chapter). Raise it and recall@1 climbs
# while previews get vaguer; lower it and the index doubles.
TARGET_CHARS = 1800

# A chunk shorter than this is dropped. Section headings with nothing
# under them, tables of contents, page-break artefacts -- all of which
# embed as noise and none of which anyone wants as an answer.
MIN_CHARS = 200

# Markdown ATX headings, and the all-caps/numbered section headings the
# field manuals use. The second pattern is deliberately conservative:
# matching too eagerly in a book turns every capitalised line of
# dialogue into a section.
_MD_HEAD = re.compile(r"^(#{1,6})\s+(.+?)\s*$")
_FM_HEAD = re.compile(r"^\s*((?:CHAPTER|PART|APPENDIX|Section)\s+[A-Z0-9IVX\-]+"
                      r"|\d+-\d+\.)\s*(.*)$")


class Chunk:

    __slots__ = ("doc", "off", "length", "heading", "text")

    def __init__(self, doc, off, length, heading, text):
        self.doc = doc            # index into the document list
        self.off = off            # BYTE offset into the document file
        self.length = length      # BYTE length
        self.heading = heading    # "A > B > C", may be ""
        self.text = text          # the decoded chunk, for the host only

    def embed_text(self, title):
        """What the encoder sees. Never what the device stores."""
        parts = [p for p in (title, self.heading) if p]
        prefix = " > ".join(parts)
        return ("%s: %s" % (prefix, self.text)) if prefix else self.text


def _heading_of(line):
    m = _MD_HEAD.match(line)
    if m:
        return len(m.group(1)), m.group(2)
    m = _FM_HEAD.match(line)
    if m:
        tail = m.group(2).strip()
        if tail and len(tail) < 80:
            return 2, ("%s %s" % (m.group(1).strip(), tail))
    return None


def segment(doc_index, text, target=TARGET_CHARS, min_chars=MIN_CHARS):
    """Split one normalised document into Chunks.

    Offsets are byte offsets into `text.encode("utf-8")`, which is what
    lands on the card.
    """
    lines = text.split("\n")

    # Character offset of the start of each line, so a chunk boundary
    # can be converted to a byte offset exactly once at the end.
    line_off = []
    pos = 0
    for ln in lines:
        line_off.append(pos)
        pos += len(ln) + 1

    stack = []          # (level, title)
    spans = []          # (char_start, char_end, heading path)
    start = 0           # char offset where the current span began
    path_at_start = ""

    def heading_path():
        return " > ".join(t for _l, t in stack)

    # A span is recorded as a RANGE and its text is sliced out of the
    # document afterwards. It is not accumulated line by line.
    #
    # That is not a style preference. The first version built the text
    # with "\n".join(buf) while the range was computed from line
    # offsets, and the two disagreed -- the range carried the final
    # newline and any blank line that had been used as a split point,
    # the joined string did not. Every one of 4,200 chunks was off.
    # cardfs.verify() caught it, which is what it is for, but the
    # durable fix is to make the two impossible to disagree: there is
    # now exactly one definition of a chunk's text, and it is the
    # slice the device will read.
    def close(end_char):
        nonlocal start, path_at_start
        if end_char > start and len(text[start:end_char].strip()) >= min_chars:
            spans.append((start, end_char, path_at_start))
            start = end_char
            path_at_start = heading_path()
            return True
        return False

    for i, ln in enumerate(lines):
        h = _heading_of(ln)
        if h is not None:
            level, title = h
            if not close(line_off[i]):
                # Too short to stand alone -- a heading directly under
                # another heading, most often. Absorb it into the next
                # span rather than emitting a stub or dropping the
                # bytes, so the ranges stay contiguous and gapless.
                pass
            while stack and stack[-1][0] >= level:
                stack.pop()
            stack.append((level, title))
            if start == line_off[i]:
                # The heading line begins this span, so a result opened
                # in `read` lands on the heading rather than under it.
                path_at_start = heading_path()
            continue

        if not ln.strip() and (line_off[i] - start) >= target:
            close(line_off[i])

    close(len(text))
    if not spans and len(text.strip()) >= min_chars:
        spans.append((0, len(text), ""))

    out = []
    for (cs, ce, path) in spans:
        boff = len(text[:cs].encode("utf-8"))
        blen = len(text[cs:ce].encode("utf-8"))
        out.append(Chunk(doc_index, boff, blen, path, text[cs:ce]))
    return out


def segment_all(docs, target=TARGET_CHARS, min_chars=MIN_CHARS):
    chunks = []
    for i, d in enumerate(docs):
        chunks.extend(segment(i, d.text, target, min_chars))
    return chunks
