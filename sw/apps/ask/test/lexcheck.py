#!/usr/bin/env python3
#
# Zeitlos ask -- does the DEVICE's BM25 rank what BM25 ranks?
#
#   make && python3 lexcheck.py ../../../../tools/ask/out/arklite
#   python3 lexcheck.py ../../../../tools/ask/out/arklite --tile 20
#
# `ask refcheck` checks the dense path bit for bit. Nothing checked the
# lexical path, which is the one `ask` ships -- and it was not doing
# what `ask eval` measured. This does, for every gold question:
#
#   exact    BM25 in floating point over the PACKED files -- the same
#            dictionary, postings and per-chunk lengths (bytes / 6) the
#            device reads, with no truncation and no accumulator limit.
#   device   `hosttest`, which is aidx.c -- the engine that ships --
#            compiled for the host.
#
# Any difference between them is the device's engineering (fixed
# point, bounded memory, how much of a list it reads), not a different
# formula. It reports rank-1 agreement, top-8 overlap, and how often
# each one puts a gold-relevant chunk first.
#
# --tile K builds a K-times copy of the pack at the binary level: every
# document and chunk repeated, every postings list K times as long.
# Term statistics stay realistic and the SIZE is what the corpus will
# be at `arkmed` scale (K = 20 on `arklite` is ~200K chunks), so the
# limits that only bite at scale -- how much of a postings list is
# read, how many candidates fit -- bite here too. Copies share card
# paths, so a chunk and its copies are the same key and ties between
# them do not count as disagreements.
#

import argparse
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "tools", "ask"))

from lib import evalset, lexicon  # noqa: E402

K1, B = 1.2, 0.75
BYTES_PER_TOKEN = 6          # aidx.h AI_BYTES_PER_TOKEN


def _hdr(b):
    return list(struct.unpack("<8I", b[:32]))


def _with_hdr(h, body):
    h = list(h)
    h[7] = sum(h[:7]) & 0xffffffff
    return struct.pack("<8I", *h) + body


def _cstr(pool, off):
    return pool[off:pool.index(b"\0", off)].decode("utf-8", "replace")


def _varints(b):
    i, cid, out = 0, 0, []
    while i < len(b):
        v = sh = 0
        while True:
            x = b[i]
            i += 1
            v |= (x & 0x7f) << sh
            if not x & 0x80:
                break
            sh += 7
        tf = b[i]
        i += 1
        cid += v
        out.append((cid, tf))
    return out


def _enc(post):
    out, prev = bytearray(), 0
    for cid, tf in post:
        n = cid - prev
        prev = cid
        while n >= 0x80:
            out.append((n & 0x7f) | 0x80)
            n >>= 7
        out.append(n)
        out.append(tf)
    return bytes(out)


class Pack:
    """The packed files of one pack, read the way aidx.c reads them."""

    def __init__(self, root, name):
        self.root = root
        d = os.path.join(root, "ask", name)
        docs = open(os.path.join(d, "docs.zdt"), "rb").read()
        h = _hdr(docs)
        pool = docs[32 + h[5]:]
        self.paths = [_cstr(pool, struct.unpack_from("<I", docs, 32 + i * 16)[0])
                      for i in range(h[3])]
        ch = open(os.path.join(d, "chunks.zct"), "rb").read()
        self.nch = _hdr(ch)[3]
        self.chunks = [struct.unpack_from("<IIII", ch, 32 + i * 16)
                       for i in range(self.nch)]
        self.lens = [max(1, min(65535, c[2] // BYTES_PER_TOKEN))
                     for c in self.chunks]
        self.avg = max(1, sum(self.lens) // len(self.lens))
        lx = open(os.path.join(d, "lexicon.zlx"), "rb").read()
        nt = _hdr(lx)[3]
        lpool = lx[32 + nt * 16:]
        self.terms = {}
        for i in range(nt):
            to, po, pl, df = struct.unpack_from("<IIII", lx, 32 + i * 16)
            self.terms[_cstr(lpool, to)] = (po, pl, df)
        self.post = open(os.path.join(d, "post.zlp"), "rb").read()[32:]

    def postings(self, t):
        po, pl, df = self.terms[t]
        return _varints(self.post[po:po + pl]), pl, df

    def score(self, q):
        sc, stats = {}, []
        for t in lexicon.tokenize(q):
            if t not in self.terms:
                continue
            post, nbytes, df = self.postings(t)
            stats.append((t, df, nbytes))
            idf = math.log(1 + (self.nch - df + .5) / (df + .5))
            for cid, tf in post:
                norm = 1 - B + B * self.lens[cid] / self.avg
                sc[cid] = sc.get(cid, 0) + idf * tf * (K1 + 1) / (tf + K1 * norm)
        return sorted(sc, key=lambda c: -sc[c]), stats

    def key(self, cid):
        c = self.chunks[cid]
        return (self.paths[c[0]].lstrip("/"), c[1])


def tile(src_root, name, k, dst_root):
    """A k-times copy of pack `name`, written under dst_root."""
    s = os.path.join(src_root, "ask", name)
    d = os.path.join(dst_root, "ask", name)
    os.makedirs(d)
    # Documents stay where they are: point dst_root/ark at the source's.
    os.symlink(os.path.join(os.path.abspath(src_root), "ark"),
               os.path.join(dst_root, "ark"))

    docs = open(os.path.join(s, "docs.zdt"), "rb").read()
    h = _hdr(docs)
    nd, rl = h[3], h[5]
    recs, pool = docs[32:32 + rl], docs[32 + rl:]
    h[3], h[5] = nd * k, rl * k
    open(os.path.join(d, "docs.zdt"), "wb").write(_with_hdr(h, recs * k + pool))

    ch = open(os.path.join(s, "chunks.zct"), "rb").read()
    h = _hdr(ch)
    nc, rl = h[3], h[5]
    out = bytearray()
    for c in range(k):
        for i in range(nc):
            doc, off, ln, ho = struct.unpack_from("<IIII", ch, 32 + i * 16)
            out += struct.pack("<IIII", doc + c * nd, off, ln, ho)
    h[3], h[5] = nc * k, len(out)
    open(os.path.join(d, "chunks.zct"), "wb").write(
        _with_hdr(h, bytes(out) + ch[32 + rl:]))

    lx = open(os.path.join(s, "lexicon.zlx"), "rb").read()
    post = open(os.path.join(s, "post.zlp"), "rb").read()
    lh, ph = _hdr(lx), _hdr(post)
    nt = lh[3]
    recs, newpost = bytearray(), bytearray()
    for i in range(nt):
        to, po, pl, df = struct.unpack_from("<IIII", lx, 32 + i * 16)
        base = _varints(post[32 + po:32 + po + pl])
        big = [(cid + c * nc, tf) for c in range(k) for cid, tf in base]
        b = _enc(big)
        recs += struct.pack("<IIII", to, len(newpost), len(b), df * k)
        newpost += b
    open(os.path.join(d, "lexicon.zlx"), "wb").write(
        _with_hdr(lh, bytes(recs) + lx[32 + nt * 16:]))
    ph[3] = len(newpost)
    open(os.path.join(d, "post.zlp"), "wb").write(_with_hdr(ph, bytes(newpost)))

    idx = open(os.path.join(s, "index.zak"), "rb").read()
    h = _hdr(idx)
    body = bytearray(idx[32:])
    ndocs, nchunks = struct.unpack_from("<II", body, 0)
    struct.pack_into("<II", body, 0, ndocs * k, nchunks * k)
    open(os.path.join(d, "index.zak"), "wb").write(_with_hdr(h, bytes(body)))


def device(hosttest, root, q):
    o = subprocess.run([hosttest, root, q], capture_output=True).stdout
    o = o.decode("utf-8", "replace")
    return [(m.group(1).lstrip("/"), int(m.group(2)))
            for m in re.finditer(r"score\s+-?\d+\s+(\S+)\s+\+(\d+)", o)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", help="a built pack, e.g. tools/ask/out/arklite")
    ap.add_argument("--pack", help="pack name (default: the root's basename)")
    ap.add_argument("--tile", type=int, default=1)
    ap.add_argument("--hosttest", default=os.path.join(HERE, "hosttest"))
    ap.add_argument("--gold", default=os.path.join(REPO, "tools", "ask",
                                                   "eval", "gold.txt"))
    args = ap.parse_args()
    name = args.pack or os.path.basename(os.path.normpath(args.root))
    root = args.root
    tmp = None
    if args.tile > 1:
        tmp = tempfile.mkdtemp(prefix="lexcheck-")
        tile(args.root, name, args.tile, tmp)
        root = tmp
    try:
        p = Pack(root, name)
        gold = evalset.load(args.gold)
        print("%s x%d: %d chunks, %d terms, postings %.1f MB" % (
            name, args.tile, p.nch, len(p.terms), len(p.post) / 1e6))

        def text(key):
            path, off = key
            with open(os.path.join(root, path), "rb") as f:
                f.seek(off)
                return f.read(2500).decode("utf-8", "replace")

        n = same1 = ov = ref_rel = dev_rel = long_lists = 0
        for g in gold:
            ranked, stats = p.score(g.q)
            if not ranked:
                continue
            ref = []
            for c in ranked:
                kk = p.key(c)
                if kk not in ref:
                    ref.append(kk)
                if len(ref) == 8:
                    break
            dev = device(args.hosttest, root, g.q)
            n += 1
            # With tiling, the exact list's first entries are copies of
            # one chunk; the device may pick any copy, and the keys are
            # equal by construction.
            same1 += bool(dev) and dev[0] == ref[0]
            ov += len(set(ref) & set(dev))
            ref_rel += bool(g.rx.search(text(ref[0])))
            dev_rel += bool(dev) and bool(g.rx.search(text(dev[0])))
            long_lists += any(nb > 4096 for _t, _df, nb in stats)
        if args.tile > 1:
            # Copies tie exactly, so the device's top 8 is often eight
            # copies of one chunk: overlap says nothing here.
            print("  queries %d, rank-1 identical %d" % (n, same1))
        else:
            print("  queries %d, rank-1 identical %d, top-8 overlap %.1f/8"
                  % (n, same1, ov / float(n or 1)))
        print("  gold-relevant at rank 1: exact %d, device %d" % (ref_rel, dev_rel))
        print("  queries using a postings list over 4 KB: %d" % long_lists)
        return 0 if same1 >= n - max(2, n // 25) else 1
    finally:
        if tmp:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
