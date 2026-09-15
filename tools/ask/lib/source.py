#
# Zeitlos ask -- source adapters.
#
# A source adapter turns one upstream thing (the Ark Codex tarball,
# a directory of markdown, a pile of gzipped Gutenberg texts) into a
# list of Documents. That is all it does. It does not chunk, embed,
# rename, or know anything about the card.
#
# WHY THE SEPARATION IS STRICT: the corpus will come from repos that
# are not this one and that move on their own schedule. Ark releases
# when Ark releases; zeitlos/docs changes every week. An adapter is
# the only thing that has to change when an upstream layout changes,
# and when one does change, nothing downstream needs rebuilding
# differently -- it just gets different Documents.
#
# A Document is deliberately plain:
#
#   key     stable identity WITHIN its source, e.g. "Amino acid".
#           Used for the doc id hash, so the same article keeps the
#           same id across rebuilds even if its neighbours change.
#           This is what makes an index incremental-friendly later.
#   title   what a human sees. May be long, may have spaces.
#   text    the whole document, already decoded to str, LF line
#           endings, no trailing whitespace runs.
#   source  the adapter's dataset name ("codex", "medline", ...).
#           Becomes the card subdirectory.
#   meta    free-form dict, carried into the manifest for provenance.
#
# TEXT IS NORMALISED HERE AND NOWHERE ELSE. The device reads bytes at
# an offset and length recorded at build time; if any later stage
# rewrote the text, every offset in the index would point into the
# wrong place. So normalise in the adapter, then treat text as
# immutable.
#

import hashlib
import re

REGISTRY = {}


def adapter(name):
    """Decorator registering a callable as a source adapter."""
    def wrap(fn):
        REGISTRY[name] = fn
        return fn
    return wrap


class Document:

    __slots__ = ("key", "title", "text", "source", "meta")

    def __init__(self, key, title, text, source, meta=None):
        self.key = key
        self.title = title
        self.text = text
        self.source = source
        self.meta = meta or {}

    @property
    def uid(self):
        """Stable 64-bit id from source + key.

        Not the array index: indices shift when a source gains an
        article. This does not, which is what lets a saved bookmark or
        a cached result survive a rebuild.
        """
        h = hashlib.sha256(("%s\x00%s" % (self.source, self.key))
                           .encode("utf-8")).digest()
        return int.from_bytes(h[:8], "little")

    def __repr__(self):
        return "Document(%s/%s, %d bytes)" % (self.source, self.key,
                                              len(self.text))


# -- normalisation -----------------------------------------------------
#
# Minimal on purpose. The one job is to make the bytes the device reads
# match the bytes the indexer measured, on any host, for any input
# encoding. It is not a cleanup pass and must not become one: stripping
# markdown, collapsing lists or de-hyphenating would all change offsets
# relative to what a reader sees, and `read` shows the file, not our
# idea of it.

_CRLF = re.compile(r"\r\n?")
_TRAILING_WS = re.compile(r"[ \t]+$", re.M)
_MANY_BLANK = re.compile(r"\n{4,}")


def normalise(text):
    text = _CRLF.sub("\n", text)
    text = _TRAILING_WS.sub("", text)
    text = _MANY_BLANK.sub("\n\n\n", text)
    if not text.endswith("\n"):
        text += "\n"
    return text


def load(adapter_name, path, opts):
    """Run one adapter. Raises KeyError if the adapter is unknown."""
    if adapter_name not in REGISTRY:
        raise KeyError("unknown source adapter `%s` (have: %s)"
                       % (adapter_name, ", ".join(sorted(REGISTRY))))
    docs = list(REGISTRY[adapter_name](path, opts))
    for d in docs:
        d.text = normalise(d.text)
    return docs
