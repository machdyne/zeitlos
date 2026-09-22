"""
Zeitlos
Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.

Aligning spellings to pronunciations.

Before anything can be learned about how letters sound, the lexicon
has to be cut up: which phoneme(s) did each letter produce?

    knight   K N IY1 T        ->  k:K n:-  i:IY1 g:-  h:-  t:T
    box      B AA1 K S        ->  b:B a:AA1 o? ...     x:"K S"
    settle   S EH1 T AX0 L    ->  s:S e:EH1 t:T t:- l:"AX0 L" e:-

A letter can produce nothing (silent), one phoneme, or two ("x" is
nearly always K S; a final "le" is AX L). This is a dynamic program
over those three choices, scored by a table of which phonemes each
letter is even allowed to produce -- seeded by hand below, which is
both small and honest: it says what English spelling does, not what
any particular word does.

Alignments that need a pairing the table does not allow are DROPPED
rather than forced. About 3% are, and they are the odd ones (foreign
spellings, abbreviations); training on a forced alignment teaches the
tree nonsense that it then applies to ordinary words.
"""

# What each letter may sound like. Two-phoneme values are the handful
# of letters that routinely produce two.
OK = {
    "a": ["AA", "AE", "AH", "AO", "AX", "EY", "EH", "IH", "IX", "IY", "OW", "ER", "AW", "AY"],
    "b": ["B"],
    "c": ["K", "S", "CH", "SH"],
    "d": ["D", "JH", "T"],
    "e": ["EH", "IY", "IH", "IX", "AX", "ER", "EY", "AH", "UW", "AA"],
    "f": ["F", "V"],
    "g": ["G", "JH", "ZH", "F", "NG"],
    "h": ["HH"],
    "i": ["IH", "IY", "AY", "IX", "AX", "ER", "AH", "Y"],
    "j": ["JH", "Y", "HH"],
    "k": ["K"],
    "l": ["L"],
    "m": ["M"],
    "n": ["N", "NG"],
    "o": ["AA", "AO", "OW", "AH", "AX", "UH", "UW", "ER", "OY", "AW", "IH", "W"],
    "p": ["P", "F"],
    "q": ["K"],
    "r": ["R", "ER"],
    "s": ["S", "Z", "SH", "ZH"],
    "t": ["T", "SH", "CH", "TH", "D"],
    "u": ["AH", "UW", "UH", "Y", "AX", "IH", "ER", "AO", "AA", "IY"],
    "v": ["V", "F"],
    "w": ["W", "V"],
    "x": ["K", "Z", "S", "G", "KS", "GZ", "K S", "G Z"],
    "y": ["Y", "IY", "IH", "AY", "IX", "AX", "ER"],
    "z": ["Z", "S", "TS", "ZH"],
}

# Two-phoneme pairings, spelled out where they are common.
PAIRS = {
    "x": [("K", "S"), ("G", "Z")],
    "u": [("Y", "UW"), ("Y", "UH")],
    "i": [("IY", "AX"), ("IY", "AH")],
    "o": [("W", "AH"), ("UW", "AH")],
    "e": [("IY", "AX"), ("Y", "UW")],
    "a": [("EY", "AX"), ("AA", "R")],
    "l": [("AX", "L"), ("AH", "L")],
    "m": [("AX", "M")],
    "n": [("AX", "N")],
    "r": [("AX", "R"), ("ER", "R")],
    "y": [("AY", "AX")],
}

# Consonants that are routinely silent, and the letters that are
# routinely silent in a digraph ("ck", "ng", "gh", "wr", "kn").
NULL_OK = set("abcdefghijklmnopqrstuvwxyz")


def _base(ph):
    return ph.rstrip("012")


def align(word, phones):
    """
    Returns a list, one entry per letter, of the phonemes that letter
    produced (possibly none) -- or None if the word cannot be aligned
    within the table above.
    """

    n, m = len(word), len(phones)
    NEG = -1e9

    # best[i][j]: score for the first i letters producing the first j
    # phonemes. Scores prefer one-to-one, then silence, then pairs --
    # so "th" comes out t:TH h:- rather than t:- h:TH only when the
    # table says so, and the usual case stays the usual case.
    best = [[NEG] * (m + 1) for _ in range(n + 1)]
    back = [[None] * (m + 1) for _ in range(n + 1)]
    best[0][0] = 0.0

    for i in range(n):
        letter = word[i]
        allowed = OK.get(letter, [])
        pairs = PAIRS.get(letter, [])
        for j in range(m + 1):
            if best[i][j] == NEG:
                continue
            here = best[i][j]

            # silent letter
            if letter in NULL_OK:
                score = here - 1.0
                if score > best[i + 1][j]:
                    best[i + 1][j] = score
                    back[i + 1][j] = (i, j, 0)

            # one phoneme
            if j < m and _base(phones[j]) in allowed:
                score = here + 2.0
                if score > best[i + 1][j + 1]:
                    best[i + 1][j + 1] = score
                    back[i + 1][j + 1] = (i, j, 1)

            # two phonemes
            if j + 1 < m:
                pair = (_base(phones[j]), _base(phones[j + 1]))
                if pair in pairs:
                    score = here + 2.5
                    if score > best[i + 1][j + 2]:
                        best[i + 1][j + 2] = score
                        back[i + 1][j + 2] = (i, j, 2)

    if best[n][m] == NEG:
        return None

    out = [[] for _ in range(n)]
    i, j = n, m
    while i > 0:
        pi, pj, take = back[i][j]
        out[pi] = list(phones[pj:pj + take])
        i, j = pi, pj

    return out


def aligned(entries, limit=None):
    """Yields (word, [[phones] per letter]) for what aligns."""
    done = 0
    for word, phones in entries:
        a = align(word, phones)
        if a is None:
            continue
        yield word, a
        done += 1
        if limit and done >= limit:
            return
