"""
Zeitlos
Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.

Finding where each phone is in a recording.

The usual way is a trained acoustic model and a forced aligner. This
does without both: it SYNTHESISES the transcript with our own voice,
which knows exactly where every phone begins and ends (tts_wav with
ZTTS_MARKS writes them out), and then warps that reference onto the
recording with dynamic time warping. Each boundary in the reference
lands on the frame of the recording it was warped to.

Two very different voices can be compared this way because the
features are normalised per utterance (audio.features): each voice's
average colour is removed and what remains is how the spectrum
MOVES, which is where the phones are. It is coarser than a trained
aligner -- a boundary can be off by a frame or two -- and that is
fine for what it feeds, which is averages over thousands of phones.

Tested against synthetic recordings with known boundaries in
tests/aligner_test.py.
"""

import numpy as np

from . import audio


def dtw(a, b, band=0.25):
    """
    The cheapest path through the frame-distance matrix, as an array
    mapping each frame of `a` to a frame of `b`.

    Steps are (1,1), (1,2) and (2,1): the local speaking rate may be
    anywhere from half to twice the reference, which comfortably covers
    a person reading against a synthesiser at a similar overall rate,
    and it means each row depends only on the two before it -- so the
    whole thing is vectorised along rows rather than a Python loop per
    cell. `band` limits how far the path may stray from the diagonal.
    """

    n, m = len(a), len(b)
    if n == 0 or m == 0:
        raise ValueError("empty feature sequence")

    # Euclidean distances, all pairs.
    aa = (a ** 2).sum(axis=1)[:, None]
    bb = (b ** 2).sum(axis=1)[None, :]
    d = np.sqrt(np.maximum(aa + bb - 2.0 * a @ b.T, 0.0))

    # Outside the band is forbidden.
    width = max(int(band * max(n, m)), abs(n - m) + 4)
    i_idx = np.arange(n)[:, None]
    j_idx = np.arange(m)[None, :]
    diag = i_idx * (m - 1) / max(n - 1, 1)
    d[np.abs(j_idx - diag) > width] = np.inf

    INF = np.inf
    cost = np.full((n, m), INF)
    step = np.zeros((n, m), dtype=np.int8)    # 0:(1,1) 1:(1,2) 2:(2,1)

    cost[0, 0] = d[0, 0]
    if n > 1 and m > 1:
        cost[1, 1] = d[0, 0] + d[1, 1]

    for i in range(1, n):
        c = np.full(m, INF)
        s = np.zeros(m, dtype=np.int8)

        # (1,1): from i-1, j-1
        prev = np.full(m, INF)
        prev[1:] = cost[i - 1, :-1]
        cand = prev + d[i]
        better = cand < c
        c[better] = cand[better]
        s[better] = 0

        # (1,2): from i-1, j-2, passing through j-1
        prev = np.full(m, INF)
        prev[2:] = cost[i - 1, :-2] + d[i, 1:-1]
        cand = prev + d[i]
        better = cand < c
        c[better] = cand[better]
        s[better] = 1

        # (2,1): from i-2, j-1, passing through i-1
        if i >= 2:
            prev = np.full(m, INF)
            prev[1:] = cost[i - 2, :-1] + d[i - 1, 1:]
            cand = prev + d[i]
            better = cand < c
            c[better] = cand[better]
            s[better] = 2

        if i == 1:
            c[1] = min(c[1], cost[1, 1])
        cost[i] = c
        step[i] = s

    if not np.isfinite(cost[n - 1, m - 1]):
        return None

    # Walk back, recording for each frame of a the frame of b.
    path = np.zeros(n, dtype=np.int64)
    i, j = n - 1, m - 1
    path[i] = j
    while i > 0 and j > 0:
        st = step[i, j]
        if st == 0:
            i, j = i - 1, j - 1
            path[i] = j
        elif st == 1:
            i, j = i - 1, j - 2
            path[i] = j
        else:
            path[i - 1] = j - 1
            i, j = i - 2, j - 1
            path[i] = j
        if i <= 1 and j <= 1:
            break
    path[:i + 1] = np.minimum(path[:i + 1], j)
    # the path must never go backwards
    return np.maximum.accumulate(path)


def read_phones(path):
    """A tts_wav .phones file: [(name, start_ms, end_ms)]."""
    out = []
    for line in open(path):
        parts = line.split()
        if len(parts) == 3:
            out.append((parts[0], int(parts[1]), int(parts[2])))
    return out


def align(ref_wav, ref_phones, rec_wav):
    """
    The reference's phones, re-timed to the recording.

    Returns [(name, start_ms, end_ms)] in the recording's time, or None
    if the two could not be aligned (a transcript that does not match
    the audio, usually, which is worth dropping rather than keeping).
    """

    ref = audio.read_wav(ref_wav)
    rec = audio.read_wav(rec_wav)
    phones = read_phones(ref_phones)
    if not phones:
        return None

    # Trim silence at the ends of both; the reference's leading and
    # trailing pause is re-added at the recording's own speech edges.
    r0, r1 = audio.speech_span(ref)
    s0, s1 = audio.speech_span(rec)

    fa = audio.features(ref)[r0:r1]
    fb = audio.features(rec)[s0:s1]
    if len(fa) < 5 or len(fb) < 5:
        return None
    # A recording more than twice as long or half as long as the
    # reference cannot be followed by these steps; drop it.
    ratio = len(fb) / float(len(fa))
    if ratio < 0.5 or ratio > 2.0:
        return None

    path = dtw(fa, fb)
    if path is None:
        return None

    frame_ms = 1000.0 * audio.HOP / audio.FS

    def to_rec(ms):
        f = int(round(ms / frame_ms)) - r0
        if f <= 0:
            return s0 * frame_ms
        if f >= len(path):
            return s1 * frame_ms
        return (path[f] + s0) * frame_ms

    out = []
    for name, a, b in phones:
        ra, rb = to_rec(a), to_rec(b)
        out.append((name, int(round(ra)), int(round(max(rb, ra)))))
    return out
