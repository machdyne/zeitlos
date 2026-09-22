"""
Zeitlos
Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.

Measuring the voice source: how pressed or breathy a speaker is.

Two standard measures, taken in the middle of stressed vowels:

  H1-H2   the first harmonic's level over the second's. A longer open
          phase (a breathier voice) puts more energy in the first
          harmonic. Driven mostly by the OPEN QUOTIENT.
  H1-A3   the first harmonic over the strongest harmonic near F3. A
          steeper source roll-off lowers A3. Driven mostly by the TILT.

Both are read off a spectrum at the harmonics of the measured pitch, so
they are independent of the recording's overall level. Both are also
biased by the vowel (a harmonic sitting on F1 is boosted), which is why
they are compared against our own voice saying the same vowels at the
same pitch, and fitted by analysis-by-synthesis (see speech's
cmd_source), never used raw.
"""

import os

import numpy as np

from . import aligner, audio

FRAME_MS = 1000.0 * audio.HOP / audio.FS
NFFT = 1024


def _harmonic_db(spec, f0, n):
    """Level in dB of harmonic n: the peak within +/-15% of n*f0."""
    freqs = np.fft.rfftfreq(NFFT, 1.0 / audio.FS)
    lo, hi = n * f0 * 0.85, n * f0 * 1.15
    band = (freqs >= lo) & (freqs <= hi)
    if not band.any():
        return None
    return 20.0 * np.log10(spec[band].max() + 1e-9)


def _a3_db(spec, f3):
    freqs = np.fft.rfftfreq(NFFT, 1.0 / audio.FS)
    band = (freqs >= f3 * 0.9) & (freqs <= f3 * 1.1)
    if not band.any():
        return None
    return 20.0 * np.log10(spec[band].max() + 1e-9)


def measure_clip(x, phones):
    """[(h1h2, h1a3)] for the stressed vowels of one clip."""
    f0 = audio.pitch(x)
    fm = audio.formants(x)
    out = []
    win = np.hanning(512)
    for name, a, b in phones:
        if not (name[:1] in "AEIOU" and name[-1:] in "12"):
            continue
        i0, i1 = int(a / FRAME_MS), int(b / FRAME_MS)
        if i1 - i0 < 8:
            continue
        mid = (i0 + i1) // 2
        if mid >= len(f0) or f0[mid] <= 0 or mid >= len(fm) or fm[mid, 2] <= 0:
            continue
        c = mid * audio.HOP + audio.WIN // 2
        seg = x[max(0, c - 256):c + 256]
        if len(seg) < 512:
            continue
        spec = np.abs(np.fft.rfft(seg * win, NFFT))
        h1 = _harmonic_db(spec, f0[mid], 1)
        h2 = _harmonic_db(spec, f0[mid], 2)
        a3 = _a3_db(spec, fm[mid, 2])
        if h1 is None or h2 is None or a3 is None:
            continue
        out.append((h1 - h2, h1 - a3))
    return out


def _job(job):
    path, wav = job
    if not os.path.exists(path) or not os.path.exists(wav):
        return []
    phones = aligner.read_phones(path)
    return measure_clip(audio.read_wav(wav), phones) if phones else []


def measure(clips, align_dir, prefix=""):
    """Median (H1-H2, H1-A3) over a corpus, and how many vowels."""
    import multiprocessing
    jobs = []
    for cid, _, wav in clips:
        path = os.path.join(align_dir, prefix + cid + ".phones")
        if prefix:
            wav = os.path.join(align_dir, prefix + cid + ".wav")
        jobs.append((path, wav))
    with multiprocessing.Pool(max(1, (os.cpu_count() or 2) - 1)) as pool:
        vals = [v for part in pool.map(_job, jobs, chunksize=8) for v in part]
    if not vals:
        return None, 0
    a = np.array(vals)
    return (float(np.median(a[:, 0])), float(np.median(a[:, 1]))), len(a)


# The settings our voice is rendered at to learn how the measures
# respond: a small grid around the default, enough for a plane.
GRID = [(50, 0), (70, 0), (50, 12), (70, 12), (60, 6)]


def fit(speaker, grid):
    """
    speaker: (h1h2, h1a3). grid: [((open, tilt), (h1h2, h1a3))].

    Each measure is modelled as a plane in (open, tilt), fitted to the
    grid, and the two planes solved together for the setting that gives
    the speaker's pair. Both controls move both measures a little, which
    is why it is solved jointly rather than one at a time.
    """
    X = np.array([[1.0, g[0][0], g[0][1]] for g in grid])
    Y = np.array([g[1] for g in grid])
    ca, _, _, _ = np.linalg.lstsq(X, Y[:, 0], rcond=None)      # h1h2 plane
    cb, _, _, _ = np.linalg.lstsq(X, Y[:, 1], rcond=None)      # h1a3 plane
    A = np.array([[ca[1], ca[2]], [cb[1], cb[2]]])
    rhs = np.array([speaker[0] - ca[0], speaker[1] - cb[0]])
    try:
        oq, tilt = np.linalg.solve(A, rhs)
    except np.linalg.LinAlgError:
        return None
    notes = []
    raw = (oq, tilt)
    oq = min(80.0, max(35.0, oq))
    tilt = min(24.0, max(0.0, tilt))
    if (round(oq), round(tilt)) != (round(raw[0]), round(raw[1])):
        notes.append("the speaker is outside what the synthesiser can do "
                     "(open %.0f%%, tilt %.0fdB): clamped" % raw)
    return (int(round(oq)), int(round(tilt))), notes
