#
# Zeitlos ask -- the on-card binary formats.
#
# Everything is little-endian and 32-bit aligned, so the device reads a
# record by casting rather than by parsing. picorv32 has no data cache
# (see rtl/montmul.v's header): every field access is an SDRAM round
# trip of ~11-13 cycles, so a format that needs decoding is a format
# that costs real time on every query.
#
# EVERY FILE STARTS WITH THE SAME 32-BYTE HEADER, for one reason: an
# index built by a different version of this tool, or half-copied onto
# a card, has to fail as a clear message rather than as a wrong answer.
# The device checks magic, version and dataset id before it trusts a
# single byte, exactly as sw/apps/web/ecdsa.c checks montmul's MAGIC
# and CONFIG before trusting the accelerator.
#
#   0   u32  magic          per-file, see MAGIC_* below
#   4   u32  format version  FORMAT_VERSION
#   8   u32  flags
#   12  u32  count           records in this file
#   16  u32  a               per-file meaning (dim, record size, ...)
#   20  u32  b               per-file meaning
#   24  u32  dsid            dataset id: low 32 bits of the build hash
#   28  u32  sum             sum of words 0..6, mod 2^32
#
# `dsid` is what ties the files together. A `coarse.zcv` from one
# distribution and a `chunks.zct` from another would otherwise produce
# confidently ranked results pointing at the wrong paragraphs, which is
# the single worst failure this system can have.
#
# -- file inventory, all 8.3 (FatFs here is FF_USE_LFN 0) --
#
#   ask/index.zak    root: dims, counts, scales, what is present
#   ask/docs.zdt     document table: card path, title, uid
#   ask/chunks.zct   chunk table: doc, byte offset, length, heading
#   ask/coarse.zcv   coarse vectors, resident, scanned every query
#   ask/fine.zfv     fine vectors, on card, read for the shortlist
#   ask/lexicon.zlx  term dictionary, resident
#   ask/post.zlp     postings, on card, read per query term
#   ask/encoder.zmd  the query encoder
#

import struct

FORMAT_VERSION = 1

MAGIC_INDEX = 0x314B415A      # "ZAK1"
MAGIC_DOCS = 0x3154445A       # "ZDT1"
MAGIC_CHUNKS = 0x3154435A     # "ZCT1"
MAGIC_COARSE = 0x3156435A     # "ZCV1"
MAGIC_FINE = 0x3156465A       # "ZFV1"
MAGIC_LEXICON = 0x31584C5A    # "ZLX1"
MAGIC_POST = 0x31504C5A       # "ZLP1"
MAGIC_ENCODER = 0x31444D5A    # "ZMD1"

HEADER_SIZE = 32


def header(magic, count, a=0, b=0, dsid=0, flags=0):
    words = [magic, FORMAT_VERSION, flags, count, a, b, dsid]
    chk = sum(words) & 0xffffffff
    return struct.pack("<8I", *(words + [chk]))


class Pool:
    """A NUL-terminated string pool with interning.

    Interning is not a size optimisation so much as a correctness one:
    every MedlinePlus chunk under the same heading shares one copy, so
    a heading string that is wrong is wrong in exactly one place and
    shows up immediately rather than in one result out of forty.
    """

    def __init__(self):
        self.buf = bytearray()
        self.seen = {}
        # Offset 0 is a single NUL, so 0 can mean "no string" without
        # a separate sentinel and without a special case on the device.
        self.buf.append(0)
        self.seen[b""] = 0

    def add(self, s):
        b = s.encode("utf-8")
        if b in self.seen:
            return self.seen[b]
        off = len(self.buf)
        self.seen[b] = off
        self.buf.extend(b)
        self.buf.append(0)
        return off

    def bytes(self):
        # Pad to a word boundary so whatever follows stays aligned.
        out = bytearray(self.buf)
        while len(out) % 4:
            out.append(0)
        return bytes(out)


# -- docs.zdt ----------------------------------------------------------
#
# Record: path_off, title_off, uid_lo, uid_hi  (16 bytes)
#
# The uid is carried even though nothing reads it yet. It is the only
# identifier that survives a rebuild -- card paths are sequential and
# renumber when a source gains an article -- so a bookmark, a history
# entry or a cached answer that wants to outlive one distribution has
# something to hold. Adding it later would mean a format bump.

def pack_docs(docs, paths, dsid):
    pool = Pool()
    recs = bytearray()
    for d, p in zip(docs, paths):
        uid = d.uid
        recs.extend(struct.pack("<IIII", pool.add(p), pool.add(d.title),
                                uid & 0xffffffff, (uid >> 32) & 0xffffffff))
    body = pool.bytes()
    return (header(MAGIC_DOCS, len(docs), a=16, b=len(recs), dsid=dsid)
            + bytes(recs) + body)


# -- chunks.zct --------------------------------------------------------
#
# Record: doc, off, len, head_off  (16 bytes)
#
# `off` and `len` are BYTE positions in the document file on the card.
# The build verifies every one of them by re-reading the emitted tree
# (cardfs.verify) -- a wrong offset does not fail on the device, it
# quietly previews the wrong paragraph.

def pack_chunks(chunks, dsid):
    pool = Pool()
    recs = bytearray()
    for c in chunks:
        recs.extend(struct.pack("<IIII", c.doc, c.off, c.length,
                                pool.add(c.heading)))
    body = pool.bytes()
    return (header(MAGIC_CHUNKS, len(chunks), a=16, b=len(recs), dsid=dsid)
            + bytes(recs) + body)


# -- vectors -----------------------------------------------------------
#
# count x dim int8, row-major, contiguous. No per-row scale: rows are
# L2-normalised before quantisation, so one global factor of 127 does
# for all of them and a dot product of two rows is an int32 that can be
# compared directly against another. Nothing is dequantised at query
# time, because ranking only needs the order.
#
# Contiguity is the whole point. This array is the accelerator's
# workload: one descriptor, one streaming read, no branches, no random
# access, no dependency between rows.

def quantise(vectors):
    import numpy as np
    v = np.asarray(vectors, dtype=np.float32)
    n = np.linalg.norm(v, axis=1, keepdims=True)
    n[n == 0] = 1.0
    q = np.rint((v / n) * 127.0)
    return np.clip(q, -127, 127).astype(np.int8)


def pack_vectors(vectors, magic, dsid):
    q = quantise(vectors)
    count, dim = q.shape
    return (header(magic, count, a=dim, b=127, dsid=dsid) + q.tobytes(order="C"))


# -- lexicon.zlx / post.zlp --------------------------------------------
#
# Dictionary record: term_off, post_off, post_len, df  (16 bytes),
# sorted by term so the device can binary-search it. Postings are a
# separate file because the dictionary is resident and the postings are
# not -- see lexicon.py's header for the arithmetic.

def pack_lexicon(lx, dsid):
    from . import lexicon as _lex
    recs, pool, postings = _lex.pack(lx)
    while len(pool) % 4:
        pool += b"\0"
    avg = int(round(lx.avg_len * 256))     # Q24.8, for BM25's length norm
    head = header(MAGIC_LEXICON, len(lx.terms), a=16, b=avg, dsid=dsid)
    lexblob = head + recs + pool
    postblob = header(MAGIC_POST, len(postings), a=0, b=0, dsid=dsid) + postings
    return lexblob, postblob


# -- encoder.zmd -------------------------------------------------------
#
# For the `bow` encoder:
#
#   header        count = vocabulary size, a = dim, b = 127
#   int8[count][dim]     term vectors, L2-normalised then quantised
#   u16[count]           idf, Q8.8
#   u32[count]           term string offsets
#   pool                 the terms, sorted, NUL-terminated
#
# The device encodes a query by tokenising, binary-searching each term,
# and accumulating `idf * vector` into an int32 accumulator. A few
# thousand MACs -- negligible next to the index scan, which is the
# point: on a board with no accelerator this half still costs nothing.

def pack_encoder(export, dsid):
    import numpy as np
    if export is None:
        return None
    if export["kind"] != "bow":
        raise ValueError("no packer for encoder kind `%s`" % export["kind"])

    terms = export["terms"]
    vecs = quantise(export["vectors"])
    idf = np.clip(np.rint(np.asarray(export["idf"]) * 256.0),
                  0, 65535).astype("<u2")

    pool = Pool()
    offs = [pool.add(t) for t in terms]

    count, dim = vecs.shape
    out = bytearray(header(MAGIC_ENCODER, count, a=dim, b=127, dsid=dsid))
    out.extend(vecs.tobytes(order="C"))
    while len(out) % 4:
        out.append(0)
    out.extend(idf.tobytes())
    while len(out) % 4:
        out.append(0)
    out.extend(np.asarray(offs, dtype="<u4").tobytes())
    out.extend(pool.bytes())
    return bytes(out)


# -- index.zak ---------------------------------------------------------
#
# The root. Read first, and the only file whose absence is a clean "no
# distribution installed" rather than an error.
#
#   header       count = number of datasets
#   u32 ndocs, nchunks, coarse_dim, fine_dim, nterms, vocab, flags, rsv
#   per dataset: u32 name_off, u32 first_doc, u32 ndocs, u32 rsv
#   pool: dataset names, distribution name, version string, build date

FLAG_HAS_FINE = 1 << 0
FLAG_HAS_LEXICON = 1 << 1
FLAG_HAS_ENCODER = 1 << 2

# Per-dataset flags, in the dataset record's fourth word.
#
# DS_GENERATED marks a dataset whose text was written by a language
# model rather than by a person -- the Ark Scroll and the Ark Codex
# both are. That is not a disclaimer bolted on: `ask` exists because a
# model small enough to run here would confabulate, and a corpus that
# is itself model-written has the same failure mode one step removed.
# A reader deciding whether to trust a passage about water purification
# should be able to see whether it came from FM 21-76 or from a
# summary, and the only place that distinction can come from is here.
DS_GENERATED = 1 << 0


def pack_index(meta, datasets, dsid):
    pool = Pool()
    name_off = pool.add(meta["name"])
    ver_off = pool.add(meta["version"])
    date_off = pool.add(meta["built"])
    enc_off = pool.add(meta.get("encoder", ""))

    body = bytearray()
    body.extend(struct.pack("<8I",
                            meta["ndocs"], meta["nchunks"],
                            meta["coarse_dim"], meta["fine_dim"],
                            meta["nterms"], meta["vocab"],
                            name_off, ver_off))
    # encoder_id identifies the WEIGHTS, not the backend name. Two
    # packs with the same id share a vector space and could in
    # principle have their scores compared directly; packs with
    # different ids cannot, and `ask` fuses them by rank instead. See
    # cardfs.py, "how results from different packs combine".
    body.extend(struct.pack("<4I", date_off, enc_off,
                            meta.get("encoder_id", 0), 0))
    for ds in datasets:
        body.extend(struct.pack("<4I", pool.add(ds["name"]),
                                ds["first_doc"], ds["ndocs"],
                                ds.get("flags", 0)))
    return (header(MAGIC_INDEX, len(datasets), a=len(body),
                   b=0, dsid=dsid, flags=meta["flags"])
            + bytes(body) + pool.bytes())
