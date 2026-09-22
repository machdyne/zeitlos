"""
Zeitlos
Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.

Measuring where a real speaker puts each vowel.

sw/apps/tts/phon.c's formant table -- F1-F3 for every vowel and
sonorant, and the end targets of each diphthong -- was written from the
textbook averages for American English. This measures the same numbers
in an aligned corpus, so the voice's vowels are placed where a person
actually says them.

Three steps, the same shape as lib/prosody.py:

  1. MEASURE. Each vowel's formants (audio.formants: linear
     prediction) over the middle of every occurrence -- 30-70% of the
     way through, clear of the transitions at either end -- stressed
     vowels only, since unstressed ones drift toward schwa. Diphthongs
     are measured twice, near the start and near the end. The median
     over thousands of occurrences.

  2. CALIBRATE. LPC is biased, differently for different vowels and
     more so on a high voice. So the same tracker is run over our own
     voice rendering the same sentences, and each measurement is taken
     as speaker / ours, times the table. The bias cancels.

  3. NORMALISE. The speaker's vocal tract is not ours: a woman's is
     about 17% shorter, which raises every formant together. That
     overall factor is estimated (the median ratio over all vowels) and
     divided out, leaving the SHAPE of her vowel space in our voice's
     tract. How big a tract to speak with is system.tts.formants, the
     user's choice.

Anything that still lands more than 25% from the table is refused and
the table's value kept: that is a measurement gone wrong, not an
accent. (The device refuses beyond 40% as well.)
"""

import os
import re
import struct

import numpy as np

from . import aligner, audio

FRAME_MS = 1000.0 * audio.HOP / audio.FS

# Measured: vowels, and the sonorants whose formants are steady enough
# to mean something. Nasals and stops are not -- their "formants" are
# loci that the transitions point at, not places the voice rests.
MEASURE = ["IY", "IH", "EH", "AE", "AA", "AO", "UH", "UW", "AH", "ER", "AX",
           "EY", "OW", "AY", "AW", "OY", "W", "Y", "R", "L"]
DIPHTHONGS = {"EY", "OW", "AY", "AW", "OY"}


def table_formants(repo):
    """
    phon.c's own values: {name: [f1, f2, f3, e1, e2, e3]}. Vowels are
    V("IY", dur, f1, f2, f3, ...) and diphthongs D("AY", dur, f1, f2,
    f3, e1, e2, e3, ...); semivowels are { "W", K_SEMI, v, dur, f1,
    f2, f3, ... }.
    """
    src = open(os.path.join(repo, "sw", "apps", "tts", "phon.c")).read()
    out = {}
    for m in re.finditer(r'\bV\("([A-Z]{1,2})",\s*\d+,\s*(\d+),\s*(\d+),\s*(\d+)', src):
        out[m.group(1)] = [int(m.group(2)), int(m.group(3)), int(m.group(4)), 0, 0, 0]
    for m in re.finditer(r'\bD\("([A-Z]{1,2})",\s*\d+,\s*(\d+),\s*(\d+),\s*(\d+),'
                         r'\s*(\d+),\s*(\d+),\s*(\d+)', src):
        out[m.group(1)] = [int(m.group(k)) for k in range(2, 8)]
    for m in re.finditer(r'\{\s*"([A-Z]{1,2})",\s*K_SEMI,\s*\d+,\s*\d+,\s*(\d+),\s*(\d+),\s*(\d+)',
                         src):
        out.setdefault(m.group(1), [int(m.group(2)), int(m.group(3)), int(m.group(4)), 0, 0, 0])
    return out


class Measure:

    def __init__(self):
        self.vals = {}          # (phone, part) -> [(f1, f2, f3)], part 0 start/steady, 1 end
        self.clips = 0

    def add(self, fm, phones):
        n = len(fm)
        for name, a, b in phones:
            base = name.rstrip("012")
            if base not in MEASURE:
                continue
            # Stressed vowels only; sonorants have no stress digit.
            if name[-1:].isdigit() and name[-1] == "0" and base != "AX":
                continue
            i0, i1 = int(a / FRAME_MS), int(b / FRAME_MS)
            if i1 - i0 < 4 or i1 > n:
                continue
            spans = [(0.3, 0.7)] if base not in DIPHTHONGS else [(0.15, 0.35), (0.65, 0.85)]
            for part, (lo, hi) in enumerate(spans):
                j0 = i0 + int((i1 - i0) * lo)
                j1 = i0 + max(int((i1 - i0) * hi), int((i1 - i0) * lo) + 1)
                seg = fm[j0:j1]
                seg = seg[(seg[:, 0] > 0) & (seg[:, 1] > 0) & (seg[:, 2] > 0)]
                if len(seg):
                    self.vals.setdefault((base, part), []).append(np.median(seg, axis=0))
        self.clips += 1

    def medians(self, min_n=20):
        out = {}
        for key, v in self.vals.items():
            if len(v) >= min_n:
                out[key] = (np.median(np.array(v), axis=0), len(v))
        return out


def _analyse(job):
    """One clip: its phones, formant track and pitch track (worker)."""
    path, wav = job
    if not os.path.exists(path) or not os.path.exists(wav):
        return None
    phones = aligner.read_phones(path)
    if not phones:
        return None
    x = audio.read_wav(wav)
    return phones, audio.formants(x), audio.pitch(x)


def _tracks(clips, align_dir, prefix):
    """
    Formant and pitch tracks for every clip, in parallel: LPC per frame
    in Python is the slow part of the whole pipeline, and it is
    embarrassingly parallel.
    """
    import multiprocessing
    jobs = []
    for cid, _, wav in clips:
        path = os.path.join(align_dir, prefix + cid + ".phones")
        if prefix:
            wav = os.path.join(align_dir, prefix + cid + ".wav")
        jobs.append((path, wav))
    n = max(1, (os.cpu_count() or 2) - 1)
    with multiprocessing.Pool(n) as pool:
        return [t for t in pool.map(_analyse, jobs, chunksize=16) if t]


def measure(clips, align_dir, prefix="", limit=0):
    """Vowel formants AND consonant loci, from one analysis pass."""
    m, lo = Measure(), Loci()
    for phones, fm, f0 in _tracks(clips[:limit] if limit else clips, align_dir, prefix):
        m.add(fm, phones)
        lo.add(fm, phones, f0)
    m.loci = lo
    return m


def fit(speaker, ours, table, tract_done=1.0, cap=0.35):
    """
    Returns ({phone: [f1, f2, f3, e1, e2, e3]}, tract factor, notes).
    Entries of 0 keep the table's value.
    """

    notes = []
    sp, us = speaker.medians(), ours.medians()

    # speaker / ours, per phone, part and formant
    ratios = {}
    for key, (v, n) in sp.items():
        if key in us:
            ratios[key] = v / us[key][0]

    if not ratios:
        return {}, 1.0, ["nothing measured"]

    # The speaker's tract relative to ours: the median ratio over every
    # vowel and formant. One number, because a shorter tract raises
    # them all together; what is left after dividing it out is accent.
    allr = np.concatenate([r for (ph, part), r in ratios.items() if ph not in ("W", "Y", "R", "L")])
    tract = float(np.median(allr)) if len(allr) else 1.0
    # `tract_done`: the calibration was already rendered at this tract
    # size, so the ratios are relative to it; the remaining factor is
    # whatever it missed.
    overall = tract * tract_done
    notes.append("the speaker's formants run %.0f%% %s ours overall (vocal tract size);"
                 " divided out" % (abs(overall - 1) * 100, "above" if overall >= 1 else "below"))

    out = {}
    refused = 0
    for (ph, part), r in ratios.items():
        tab = table.get(ph)
        if not tab:
            continue
        row = out.setdefault(ph, [0] * 6)
        for k in range(3):
            base = tab[k + 3 * part]
            if not base:
                continue
            v = base * r[k] / tract
            # Beyond `cap` is a measurement gone wrong rather than an
            # accent. 35%, not less: a fronted UW -- common in modern
            # English -- is a 28% move in F2, and a tighter cap refused
            # exactly the kind of thing this is here to find.
            if abs(v / base - 1.0) > cap:
                refused += 1
                continue
            row[k + 3 * part] = int(round(v))
    if refused:
        notes.append("%d formant values more than %d%% from the table: refused, table kept"
                     % (refused, int(cap * 100)))
    notes.append("%d phonemes measured" % len(out))
    return out, overall, notes


def serialise(fitted):
    out = struct.pack("<HH", 1, len(fitted))
    for ph, row in sorted(fitted.items()):
        nm = ph.encode("ascii")[:2]
        out += nm + b"\0" * (2 - len(nm)) + b"".join(struct.pack("<H", int(v)) for v in row)
    return out


# -- consonant loci --
#
# A consonant has no steady formants of its own worth measuring; what it
# has is a LOCUS, the frequency its transitions into a vowel point at.
# The locus equation finds it: across many CV pairs, F2 at the vowel's
# onset against F2 in the vowel's middle falls on a line,
#
#     onset = k * middle + c
#
# and the locus is where that line crosses onset = middle: c / (1 - k).
# phon.c's consonant entries ARE loci, so a fitted one drops into the
# table (and the FORMANTS section) like a vowel's formants do.

LOCI = ["B", "D", "G", "P", "T", "K", "M", "N", "NG",
        "F", "V", "TH", "DH", "S", "Z", "SH", "ZH"]

# Measured, reported, but NOT applied. In the synthetic-speaker test the
# nasals' fitted loci were off by up to 12% and moved from run to run:
# at a nasal onset the tracker sees the nasal murmur as much as the
# transition. Better the table's value than a confident wrong one.
NOT_APPLIED = {"M", "N", "NG"}


class Loci:

    def __init__(self):
        self.pairs = {}         # (consonant, formant) -> [(middle, onset)]

    def add(self, fm, phones, f0=None):
        n = len(fm)
        for (c, ca, cb), (v, va, vb) in zip(phones, phones[1:]):
            if c not in LOCI or not (v[:1] in "AEIOU" and v[-1:].isdigit()):
                continue
            if v[-1] == "0":
                continue                    # a reduced vowel has no target to point at
            i0, i1 = int(va / FRAME_MS), int(vb / FRAME_MS)
            if i1 - i0 < 6 or i1 > n:
                continue
            # The onset is the first VOICED frames: after a voiceless
            # stop the vowel starts with aspiration, whose "formants" are
            # noise, and reading the transition there put T's and K's
            # loci above 3kHz.
            j = i0
            if f0 is not None:
                while j < i1 - 3 and (j >= len(f0) or f0[j] <= 0):
                    j += 1
            onset = fm[j + 1:j + 3]
            mid = fm[i0 + (i1 - i0) * 2 // 5:i0 + (i1 - i0) * 3 // 5 + 1]
            onset = onset[(onset[:, 1] > 0) & (onset[:, 2] > 0)]
            mid = mid[(mid[:, 1] > 0) & (mid[:, 2] > 0)]
            if len(onset) == 0 or len(mid) == 0:
                continue
            for k in (1, 2):                # F2 and F3; F1 at an onset is always low
                self.pairs.setdefault((c, k), []).append(
                    (float(np.median(mid[:, k])), float(np.median(onset[:, k]))))

    def halves(self):
        """Two Loci from alternate pairs: for checking a fit is stable."""
        a, b = Loci(), Loci()
        for key, pts in self.pairs.items():
            a.pairs[key] = pts[0::2]
            b.pairs[key] = pts[1::2]
        return a, b

    def loci(self, min_n=40):
        """{(consonant, formant): locus in Hz}, where the line is usable."""
        out = {}
        for key, pts in self.pairs.items():
            if len(pts) < min_n:
                continue
            a = np.array(pts)
            k, c = np.polyfit(a[:, 0], a[:, 1], 1)
            # A slope near 1 means the onset simply follows the vowel:
            # no locus to speak of, and c / (1 - k) is noise.
            if not (0.05 < k < 0.9):
                continue
            out[key] = c / (1.0 - k)
        return out


def measure_loci(clips, align_dir, prefix="", limit=0):
    return measure(clips, align_dir, prefix, limit).loci


def table_loci(repo):
    """{consonant: [f1, f2, f3]} from phon.c's { "B", K_STOP, v, dur, f1, f2, f3, ...}."""
    src = open(os.path.join(repo, "sw", "apps", "tts", "phon.c")).read()
    out = {}
    for m in re.finditer(r'\{\s*"([A-Z]{1,2})",\s*K_[A-Z]+,\s*\d+,\s*\d+,\s*(\d+),\s*(\d+),\s*(\d+)', src):
        if m.group(1) in LOCI:
            out[m.group(1)] = [int(m.group(2)), int(m.group(3)), int(m.group(4))]
    return out


def fit_loci(speaker, ours, shifted, table, tract, delta=0.10, cap=0.35):
    """
    Analysis by synthesis. A locus is not something the synthesiser
    reproduces one-for-one -- a transition only partly reaches it -- so
    a ratio like the vowels' is wrong. Instead our voice is measured
    twice: with the table's loci (`ours`) and with every locus moved up
    by `delta` (`shifted`). The difference says how much each
    consonant's measurable locus RESPONDS to its table value, and the
    fit is the table value that would make ours match the speaker's:

        fitted = table + (speaker - ours) / response

    A consonant whose transitions barely respond to its locus cannot be
    fitted this way, and is reported rather than guessed.
    """
    sp, us, sh = speaker.loci(), ours.loci(), shifted.loci()
    h1, h2 = (h.loci(min_n=20) for h in speaker.halves())
    out, notes, refused, flat, unstable, held = {}, [], 0, [], [], []
    for (c, k), L in sp.items():
        if (c, k) not in us or (c, k) not in sh or c not in table:
            continue
        base = table[c][k]
        response = (sh[(c, k)] - us[(c, k)]) / (delta * base)
        if response < 0.2:
            flat.append("%s F%d" % (c, k + 1))
            continue
        v = base + (L / tract - us[(c, k)]) / response

        # The same fit on each half of the speaker's data. A real
        # accent is the same in both; noise, amplified by dividing by a
        # response under 1, is not. Disagreement beyond 4% of the table
        # value means the data cannot say, and the table stands.
        if (c, k) in h1 and (c, k) in h2:
            v1 = base + (h1[(c, k)] / tract - us[(c, k)]) / response
            v2 = base + (h2[(c, k)] / tract - us[(c, k)]) / response
            if abs(v1 - v2) > 0.04 * base:
                unstable.append("%s F%d" % (c, k + 1))
                continue
        else:
            unstable.append("%s F%d" % (c, k + 1))
            continue

        if abs(v / base - 1.0) > cap:
            refused += 1
            continue
        # F3 loci are reported, not applied: every one came back 18-26%
        # above the table on the first real corpus, the same direction for
        # every consonant, which is a measurement bias (F3 at a vowel's
        # onset barely moves, so its locus equation is nearly flat and
        # the locus poorly determined), not an accent.
        if k == 2:
            held.append("%s F3 %d" % (c, int(round(v))))
            continue
        if c in NOT_APPLIED:
            held.append("%s F%d %d" % (c, k + 1, int(round(v))))
            continue
        out.setdefault(c, [0] * 6)[k] = int(round(v))
    notes.append("%d consonant loci measured" % len(out))
    if held:
        notes.append("measured but not applied (nasals, and all F3 loci: unreliable): %s"
                     % ", ".join(sorted(held)))
    if unstable:
        notes.append("unstable -- the two halves of the data disagree, table kept: %s"
                     % ", ".join(sorted(unstable)))
    if flat:
        notes.append("not fittable -- our voice's transitions barely respond to: %s"
                     % ", ".join(sorted(flat)))
    if refused:
        notes.append("%d loci more than %d%% from the table: refused" % (refused, int(cap * 100)))
    return out, notes


def shifted_loci(table, delta=0.10, vowels=None):
    """
    A FORMANTS section moving every consonant's F2 and F3 locus up --
    with the speaker's fitted vowels, if given, so the only difference
    from the speaker is the consonants.
    """
    rows = dict(vowels or {})
    for c, (f1, f2, f3) in table.items():
        rows[c] = [0, int(round(f2 * (1 + delta))), int(round(f3 * (1 + delta))), 0, 0, 0]
    return serialise(rows)
