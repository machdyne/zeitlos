#
# Zeitlos ask -- the lexical half.
#
# Dense retrieval alone loses on exactly the queries where an exact
# token is the whole point: `lakritz`, `0x7000_0600`, `nextpnr-ecp5`, a
# drug name. Embeddings blur a rare string toward whatever it
# resembles. BM25 does not, and it is nearly free.
#
# Measured on a 14,071-chunk prototype over real Ark data, first
# relevant hit at rank 1 across a 12-question set:
#
#     lexical alone   8/12
#     dense alone     7/12
#     hybrid         10/12
#
# The two failure modes are uncorrelated -- lexical dies on polysemy
# ("matches" found a shell-globbing article), dense dies on topical
# drift ("build a fire" found the build documentation) -- which is why
# fusing them beats either. Hybrid is not an optimisation here, it is
# the design.
#
# -- where the postings live --
#
# The DICTIONARY is resident: term string, document frequency, and the
# offset of its postings list. At ~39K terms over a 22MB corpus that is
# well under a megabyte, and it grows with vocabulary rather than with
# corpus size, so it stays small as the corpus does not.
#
# The POSTINGS are on the card and read per query term. A query has a
# handful of terms and each list is small, except for the common terms,
# which are precisely the ones carrying least information. Terms above
# DF_MAX_RATIO are dropped entirely at build time rather than read and
# discarded at query time.
#
# The alternative -- postings resident -- was costed and rejected: 2.8M
# postings at 5 bytes is 14MB, which is most of the machine.
#

import math
import re
import struct

# Terms appearing in more than this fraction of chunks are dropped.
# 0.15 removes "the"-class words and the corpus's own filler without
# touching anything a question is actually about.
DF_MAX_RATIO = 0.15

# Terms appearing in fewer than this many chunks are kept -- rare is
# the point -- but a term appearing exactly once is usually an OCR
# artefact or a page number, and there are a great many of them.
DF_MIN = 2

# BM25 parameters. The defaults from the literature; there is no
# evidence in this corpus for moving them and a tuned constant nobody
# can re-derive is worse than a standard one.
K1 = 1.2
# b was 0 for a while, to match a device that had no per-chunk length.
# sw/apps/ask/aidx.c now derives one from chunks.zct's byte-length
# column, so both sides normalise again -- and they must agree, or
# `ask eval` measures something the hardware does not do.
B = 0.75

# Tokenisation. Deliberately keeps digits and underscores joined to
# letters, so `0x7000_0600`, `rv32im` and `picorv32` survive as single
# terms. That is the whole reason this half exists.
_TOKEN = re.compile(r"[A-Za-z0-9_]+(?:[.\-][A-Za-z0-9_]+)*")

STOP = set("""
a an and are as at be by for from has have how i in is it its of on or
that the this to was were what when where which who will with you your
""".split())


def tokenize(text):
    out = []
    for m in _TOKEN.finditer(text.lower()):
        t = m.group(0)
        if len(t) < 2 or t in STOP:
            continue
        out.append(t)
    return out


class Lexicon:

    def __init__(self):
        self.terms = []        # sorted term strings
        self.df = []           # document frequency, parallel to terms
        self.postings = []     # list of (chunk_id, tf) lists
        self.avg_len = 0.0
        self.nchunks = 0

    def idf(self, i):
        n = self.nchunks
        d = self.df[i]
        return math.log(1.0 + (n - d + 0.5) / (d + 0.5))


def build(chunk_texts):
    """Build a Lexicon from the text each chunk is indexed under.

    `chunk_texts` should be the same strings handed to the encoder --
    heading path included -- so that a heading term is findable
    lexically too. A section called "Water Procurement" should match
    `water procurement` even if the paragraph under it never repeats
    the phrase.
    """
    n = len(chunk_texts)
    post = {}
    lengths = []
    for cid, text in enumerate(chunk_texts):
        toks = tokenize(text)
        lengths.append(len(toks))
        seen = {}
        for t in toks:
            seen[t] = seen.get(t, 0) + 1
        for t, tf in seen.items():
            post.setdefault(t, []).append((cid, min(tf, 255)))

    lx = Lexicon()
    lx.nchunks = n
    lx.avg_len = (sum(lengths) / float(n)) if n else 0.0

    df_max = int(n * DF_MAX_RATIO)
    kept = [t for t, pl in post.items() if DF_MIN <= len(pl) <= df_max]
    kept.sort()
    lx.terms = kept
    lx.df = [len(post[t]) for t in kept]
    lx.postings = [post[t] for t in kept]
    lx.lengths = lengths
    return lx


def score(lx, query, limit=None):
    """Host-side BM25, used by the eval harness and by nothing else.

    The device implements the same formula against the packed index;
    this exists so the two can be compared on identical input, which is
    how a packing bug gets caught before it ships.
    """
    import bisect
    scores = {}
    for t in tokenize(query):
        i = bisect.bisect_left(lx.terms, t)
        if i >= len(lx.terms) or lx.terms[i] != t:
            continue
        idf = lx.idf(i)
        for cid, tf in lx.postings[i]:
            dl = lx.lengths[cid]
            denom = tf + K1 * (1 - B + B * dl / (lx.avg_len or 1.0))
            scores[cid] = scores.get(cid, 0.0) + idf * (tf * (K1 + 1)) / denom
    ranked = sorted(scores.items(), key=lambda kv: -kv[1])
    return ranked[:limit] if limit else ranked


# -- packing -----------------------------------------------------------
#
# Two blobs. The dictionary is one array of fixed records plus a string
# pool, so the device can binary-search it without parsing anything.
# The postings are varint deltas, because a sorted list of chunk ids
# compresses to roughly two bytes an entry and the device already has
# to walk it linearly.

def _varint(n, out):
    while n >= 0x80:
        out.append((n & 0x7f) | 0x80)
        n >>= 7
    out.append(n)


def pack(lx):
    """Return (dict_records, string_pool, postings_blob)."""
    pool = bytearray()
    pool_off = {}

    def intern(s):
        b = s.encode("utf-8")
        if b in pool_off:
            return pool_off[b]
        off = len(pool)
        pool_off[b] = off
        pool.extend(b)
        pool.append(0)
        return off

    postings = bytearray()
    recs = bytearray()
    for i, term in enumerate(lx.terms):
        toff = intern(term)
        poff = len(postings)
        prev = 0
        buf = bytearray()
        for cid, tf in lx.postings[i]:
            _varint(cid - prev, buf)
            buf.append(tf)
            prev = cid
        postings.extend(buf)
        # term offset, postings offset, postings bytes, df
        recs.extend(struct.pack("<IIII", toff, poff, len(buf), lx.df[i]))
    return bytes(recs), bytes(pool), bytes(postings)
