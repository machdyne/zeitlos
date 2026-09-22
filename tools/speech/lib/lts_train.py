"""
Zeitlos
Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.

Learning letter-to-sound rules from the lexicon.

The hand-written rules in sw/apps/tts/lts.c get about 41% of ordinary
words exactly right. They are a good floor and a fine fallback, but
they are a few hundred rules written by one person, and English has
more cases than that. This learns the same thing from 90,000 aligned
pronunciations instead.

One binary decision tree per letter. Every question is of the form
"is the letter n places away an `x`?", with n from -4 to +4, and each
leaf is the phoneme(s) that letter makes. Trees are grown greedily on
information gain and then pruned back to a size budget, because the
model has to sit in memory on the device: the tree is consulted for
every letter of every unknown word, which is no place for a seek.

Stress is NOT learned here. lts.c's stress pass still puts the digits
on afterwards -- it is a separate problem, it is mostly about
syllables and suffixes rather than letters, and getting it wrong
sounds odd rather than wrong.
"""

import math

CTX = list(range(-4, 5))        # the window, in letters
PAD = "#"


def samples(aligned_entries):
    """
    (context, class) pairs for each letter. The context is the nine
    letters around it; the class is what that letter produced, with
    stress removed.
    """
    for word, per_letter in aligned_entries:
        padded = PAD * 4 + word + PAD * 4
        for i, phones in enumerate(per_letter):
            ctx = padded[i:i + 9]
            cls = " ".join(p.rstrip("012") for p in phones)
            yield ctx, cls


class Node:
    __slots__ = ("cls", "off", "ch", "yes", "no", "n")

    def __init__(self):
        self.cls = None
        self.off = 0
        self.ch = ""
        self.yes = None
        self.no = None
        self.n = 0


def _entropy(counts, total):
    e = 0.0
    for c in counts.values():
        if c:
            p = c / total
            e -= p * math.log(p, 2)
    return e


def _counts(rows):
    d = {}
    for _, cls in rows:
        d[cls] = d.get(cls, 0) + 1
    return d


def grow(rows, depth=0, max_depth=16, min_rows=6):

    node = Node()
    node.n = len(rows)
    counts = _counts(rows)
    node.cls = max(counts.items(), key=lambda kv: kv[1])[0]

    if len(counts) == 1 or depth >= max_depth or len(rows) < min_rows:
        return node

    base = _entropy(counts, len(rows))
    best = (0.0, None, None)

    for pos in range(9):
        if pos == 4:
            continue                    # the letter itself: the tree is per letter
        seen = {}
        for ctx, cls in rows:
            seen.setdefault(ctx[pos], []).append(cls)
        if len(seen) < 2:
            continue
        for ch, classes in seen.items():
            if len(classes) < min_rows or len(rows) - len(classes) < min_rows:
                continue
            yes = {}
            for c in classes:
                yes[c] = yes.get(c, 0) + 1
            no = dict(counts)
            for c, k in yes.items():
                no[c] -= k
            ny, nn = len(classes), len(rows) - len(classes)
            gain = base - (ny * _entropy(yes, ny) + nn * _entropy(no, nn)) / len(rows)
            if gain > best[0]:
                best = (gain, pos, ch)

    if best[1] is None or best[0] <= 1e-6:
        return node

    _, pos, ch = best
    yes_rows = [r for r in rows if r[0][pos] == ch]
    no_rows = [r for r in rows if r[0][pos] != ch]

    node.off = pos
    node.ch = ch
    node.yes = grow(yes_rows, depth + 1, max_depth, min_rows)
    node.no = grow(no_rows, depth + 1, max_depth, min_rows)
    return node


def size(node):
    return 1 if node.yes is None else 1 + size(node.yes) + size(node.no)


def prune(node, keep):
    """
    Drops the splits that separate the fewest rows, until the tree has
    at most `keep` nodes. A split near the leaves that only moves a
    handful of words is the first to go, which is exactly the part
    that generalises least.
    """

    if node.yes is None:
        return

    # gather internal nodes with the count of rows they decide about
    internal = []

    def walk(n):
        if n.yes is None:
            return
        internal.append(n)
        walk(n.yes)
        walk(n.no)

    walk(node)
    while size(node) > keep and internal:
        # only nodes whose children are both leaves can be collapsed
        cand = [n for n in internal if n.yes.yes is None and n.no.yes is None]
        if not cand:
            break
        worst = min(cand, key=lambda n: min(n.yes.n, n.no.n))
        worst.yes = worst.no = None
        internal.remove(worst)


def predict(node, ctx):
    while node.yes is not None:
        node = node.yes if ctx[node.off] == node.ch else node.no
    return node.cls


def train(aligned_entries, budget=8000, max_depth=16, min_rows=6):
    """
    Trains one tree per letter. `budget` is nodes per tree after
    pruning. Returns {letter: root}.
    """

    rows = {}
    for ctx, cls in samples(aligned_entries):
        rows.setdefault(ctx[4], []).append((ctx, cls))

    trees = {}
    for letter in sorted(rows):
        if letter == PAD:
            continue
        t = grow(rows[letter], max_depth=max_depth, min_rows=min_rows)
        prune(t, budget)
        trees[letter] = t
    return trees


# -- serialisation --
#
# Nodes are 6 bytes: a question (which offset, which letter, and where
# to go) or a leaf (a class id). The classes are a table of phoneme
# sequences, so a leaf is two bytes rather than a string.
#
#   internal: off (0-8), ch ('a'-'z' or '#'), u16 yes, u16 no
#   leaf:     0xff, 0, u16 class id, u16 unused
#
# See docs/tts_data.md for how sw/apps/tts/pack.c (pack_lts) reads it.

import struct


def serialise(trees, phone_ids):

    classes = {}
    order = []

    def class_id(cls):
        if cls not in classes:
            classes[cls] = len(order)
            order.append(cls)
        return classes[cls]

    nodes = bytearray()
    roots = {}

    def emit(node):
        # depth-first; returns this node's index
        idx = len(nodes) // 6
        nodes.extend(b"\0" * 6)
        if node.yes is None:
            struct.pack_into("<BBHH", nodes, idx * 6, 0xff, 0, class_id(node.cls), 0)
        else:
            y = emit(node.yes)
            n = emit(node.no)
            ch = 0 if node.ch == PAD else ord(node.ch) - ord("a") + 1
            struct.pack_into("<BBHH", nodes, idx * 6, node.off, ch, y, n)
        return idx

    for letter in "abcdefghijklmnopqrstuvwxyz":
        roots[letter] = emit(trees[letter]) if letter in trees else 0xffff

    table = bytearray()
    for cls in order:
        phones = cls.split() if cls else []
        table.append(len(phones))
        for ph in phones:
            table.append(phone_ids[ph])

    head = struct.pack("<HHH", len(nodes) // 6, len(order), 26)
    head += b"".join(struct.pack("<H", roots[c]) for c in "abcdefghijklmnopqrstuvwxyz")
    head += struct.pack("<I", len(table))

    return head + bytes(table) + bytes(nodes)
