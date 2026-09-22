"""
Zeitlos
Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.

A diphone voice, built from the corpus: the prototype that decides
whether the device gets one.

A DIPHONE runs from the middle of one sound to the middle of the next.
Cutting speech there puts every join in the steadiest part of a sound,
and keeps every transition -- bursts, aspiration, nasal releases, the
glides between vowels -- as it was recorded. Those are exactly the parts
the formant rules kept failing to produce (docs/tts_data.md, sessions 1 and 1b).

  INVENTORY. From the aligned corpus (lib/corpus.py), the best recorded
  example of every diphone: typical durations, stressed vowels,
  pitch near the speaker's middle, nothing implausibly short. Its
  pitch marks -- one per glottal period where voiced, every 5ms where
  not -- are found here, once.

  SYNTHESIS. TD-PSOLA (Moulines and Charpentier, 1990): the utterance's
  phones, their durations and its pitch come from our own front end,
  unchanged (tts_wav's .phones and .f0). Output pitch marks are laid
  down at the target pitch; for each, the nearest source grain -- two
  source periods of audio under a Hann window -- is found through a
  time warp that maps each half of each unit onto its target half-phone,
  and added in. Time and pitch are changed independently, and the
  joins overlap-add like every other grain.

Written for clarity rather than speed: it runs on the build machine,
to be measured. The device version, if it earns one, is C.
"""

import json
import os
import struct

import numpy as np

from . import aligner, audio

FS = audio.FS

# Unit selection avoids flapped T and D (see _flapped). A module switch
# so the first inventory's choice can still be rebuilt for comparison.
FLAPS_AVOIDED = True
UNVOICED_STEP = 55          # samples between marks where there is no pitch: 5ms

# How much of a unit's PAUSE half is ever played, next to the join with
# the speech sound: enough for a release into silence or an onset out of
# it. A third of the pause units' "pause" halves turned out to hold
# speech -- the alignment put a pause where the reader did not pause --
# and played in full, a fragment of another word sounded in every gap.
PAUSE_KEEP = 441            # samples: 40ms

# Either side of a join between units, how far grains of both are blended.
XFADE = 132                 # samples: 12ms, two to three pitch periods


def _base(name):
    """"AE1" -> "AE"; every pause kind -> "_"."""
    return "_" if name.startswith("_") else name.rstrip("012")


def _padded(phones):
    ph = [p for p in phones if p[2] > p[1]]
    if not ph:
        return []
    if not ph[0][0].startswith("_"):
        ph = [("_", max(0, ph[0][1] - 80), ph[0][1])] + ph
    if not ph[-1][0].startswith("_"):
        ph = ph + [("_", ph[-1][2], ph[-1][2] + 80)]
    return ph


def _units_of(phones):
    """Consecutive pairs, each with its time span in the recording."""
    ph = _padded(phones)
    return list(zip(ph, ph[1:]))


def _is_vowel(name):
    return name[:1] in "AEIOU" and not name.startswith("_")


def _flapped(ph, i):
    """
    Whether the T or D at ph[i] is probably a FLAP: between vowels, and
    not starting a stressed syllable ("ladder", "water", "city"). An
    American speaker says those as a quick tap that sounds like a light L
    or R, and units cut from one made D heard as L (x24) in the first
    prototype.
    """
    if _base(ph[i][0]) not in ("T", "D") or i == 0 or i + 1 >= len(ph):
        return False
    before, after = ph[i - 1][0], ph[i + 1][0]
    return _is_vowel(before) and _is_vowel(after) and not after.endswith("1")


# -- the inventory --

def phone_stats(align_dir, clips):
    """Median duration of every phone across the corpus: what "typical" means."""
    d = {}
    for cid, _, _ in clips:
        path = os.path.join(align_dir, cid + ".phones")
        if not os.path.exists(path):
            continue
        for name, a, b in aligner.read_phones(path):
            d.setdefault(_base(name), []).append(b - a)
    return {k: float(np.median(v)) for k, v in d.items()}


def _candidates(align_dir, clips, med, keep=12):
    """
    The `keep` best-timed examples of every diphone, from the alignments
    alone -- cheap, so the whole corpus can be looked at before any audio
    is read.
    """
    best = {}
    for cid, _, wav in clips:
        path = os.path.join(align_dir, cid + ".phones")
        if not os.path.exists(path):
            continue
        ph = _padded(aligner.read_phones(path))
        for i in range(len(ph) - 1):
            (a, a0, a1), (b, b0, b1) = ph[i], ph[i + 1]
            ka, kb = _base(a), _base(b)
            da, db = a1 - a0, b1 - b0
            if da < 25 or db < 25:
                continue                    # too short to have a middle to cut in
            cost = 0.0
            for name, dur, k in ((a, da, ka), (b, db, kb)):
                if k != "_":
                    cost += abs(np.log(dur / max(med.get(k, dur), 1.0)))
                    # Prefer stressed vowels: unstressed ones drift toward
                    # schwa, and a unit made from one says less.
                    if name[-1:] == "0" and k not in ("AX", "IX"):
                        cost += 0.5
            if FLAPS_AVOIDED and (_flapped(ph, i) or _flapped(ph, i + 1)):
                cost += 2.0
            key = ka + "-" + kb
            lst = best.setdefault(key, [])
            lst.append((cost, cid, wav, (a0 + a1) / 2.0, a1, (b0 + b1) / 2.0))
            if len(lst) > 4 * keep:
                lst.sort()
                del lst[keep:]
    for lst in best.values():
        lst.sort()
        del lst[keep:]
    return best


def pitch_marks(x, f0):
    """
    Glottal pulses where voiced -- each period's strongest peak, a period
    from the last -- and a mark every 5ms where not. Returns [(sample,
    voiced)].
    """
    marks = []
    t = 0
    n = len(x)
    # Peaks of a LOW-PASSED copy: the raw wave's biggest peak in a period
    # can belong to a different harmonic from one period to the next, and
    # grains cut around such marks are out of step with each other. Below
    # ~800Hz the fundamental dominates, and its peak is the same point of
    # every period. (A 2-pole resonator at 0Hz is enough: this is a
    # smoothing, not a filter design.)
    lp = np.zeros(n)
    a = np.exp(-2 * np.pi * 800.0 / FS)
    acc = 0.0
    for i in range(n):
        acc = a * acc + (1 - a) * x[i]
        lp[i] = acc
    acc = 0.0
    for i in range(n - 1, -1, -1):          # and backward, so no delay
        acc = a * acc + (1 - a) * lp[i]
        lp[i] = acc
    prev = None                             # the last voiced mark, if the last mark was one
    while t < n:
        fr = min(len(f0) - 1, t // audio.HOP) if len(f0) else -1
        hz = f0[fr] if fr >= 0 else 0.0
        if hz > 0:
            period = int(FS / hz)
            if prev is not None:
                # The next pulse is about a period after the last: search
                # only +/-20% of one. A wider window (3/4 to 1 3/4 periods,
                # at first) let a mark jump to the wrong peak -- 16% of
                # consecutive spacings were off by more than 15% -- and
                # every such grain was cut out of step, smearing the
                # harmonics of every voiced sound.
                lo, hi = prev + (4 * period) // 5, min(n, prev + (6 * period) // 5 + 1)
            else:
                lo, hi = t, min(n, t + period)
            if hi - lo > 2:
                p = lo + int(np.argmax(lp[lo:hi]))
                marks.append((p, True))
                prev = p
                t = p + (4 * period) // 5
                continue
        prev = None
        marks.append((t, False))
        t += UNVOICED_STEP
    return marks


def _unit_audio(job):
    """Cut one candidate, measure it: its audio, marks, pitch, loudness."""
    key, (cost, cid, wav, s_ms, cut_ms, e_ms) = job
    try:
        x = audio.read_wav(wav)
    except Exception:
        return None
    s, c, e = int(s_ms * FS / 1000), int(cut_ms * FS / 1000), int(e_ms * FS / 1000)
    if e - s < 60 or s < 0 or e > len(x):
        return None
    f0 = audio.pitch(x)
    seg = x[s:e]
    fseg = f0[s // audio.HOP:e // audio.HOP + 1]
    voiced = fseg[fseg > 0]
    halves = (float(np.sqrt(np.mean(seg[:c - s] ** 2))) if c - s > 20 else 0.0,
              float(np.sqrt(np.mean(seg[c - s:] ** 2))) if e - c > 20 else 0.0)
    return (key, cost, cid, seg, c - s, pitch_marks(seg, fseg),
            float(np.median(voiced)) if len(voiced) else 0.0,
            halves, _edge_ceps(seg))


def _edge_ceps(seg):
    """
    The spectral envelope at each end of a unit -- the mid-phone points
    where it will be joined to its neighbours: cepstra c1..c12 over 25ms.
    """
    w = int(0.025 * FS)
    out = []
    for part in (seg[:w], seg[-w:]):
        if len(part) < 64:
            out.append(None)
            continue
        spec = np.abs(np.fft.rfft(part * np.hanning(len(part)), audio.WIN)) ** 2
        out.append((np.log(spec @ audio._BANK.T + 1e-10) @ audio._DCT.T)[1:13])
    return out


def build_inventory(clips, align_dir, out_path, log=print):
    """
    The best recorded example of every diphone, with its pitch marks, to
    `out_path` (a directory). Returns a coverage summary.
    """
    import multiprocessing
    med = phone_stats(align_dir, clips)
    cands = _candidates(align_dir, clips, med)
    log("  %d diphones seen in the corpus; measuring the best few of each" % len(cands))

    jobs = [(k, c) for k, lst in cands.items() for c in lst]
    with multiprocessing.Pool(max(1, (os.cpu_count() or 2) - 1)) as pool:
        measured = [m for m in pool.map(_unit_audio, jobs, chunksize=16) if m]

    # The speaker's typical pitch; a unit far from the pitch it will be
    # played at needs the most PSOLA, and sounds it.
    pitches = [m[6] for m in measured if m[6] > 0]
    mid_hz = float(np.median(pitches)) if pitches else 200.0

    # Every phone's TYPICAL spectrum at its middle, from every candidate
    # that has one there. The chosen unit is the one whose ends are
    # closest to typical: a unit cut where the alignment was wrong holds
    # part of the wrong sound and is far from typical, and units whose
    # ends are all near typical meet each other smoothly. The first
    # prototypes chose on timing and pitch alone, and even rebuilding a
    # recording with its own timing and pitch (copy synthesis) scored as
    # badly as everything else -- the units, not the prosody.
    edges = {}
    for key, cost, cid, seg, cut, marks, hz, halves, ceps in measured:
        a, b = key.split("-")
        for ph, c in ((a, ceps[0]), (b, ceps[1])):
            if c is not None and ph != "_":
                edges.setdefault(ph, []).append(c)
    centre = {ph: np.median(np.array(v), axis=0) for ph, v in edges.items() if len(v) >= 3}
    spread = {ph: float(np.median([np.linalg.norm(c - centre[ph]) for c in edges[ph]])) + 1e-6
              for ph in centre}

    # Each phone's typical loudness over its candidates' halves, and a
    # heavy cost for a half far quieter than that: a unit cut where the
    # alignment wandered into a pause holds silence where a vowel should
    # be, and plays as a GAP -- which is what the first prototype sounded
    # full of. Stops and affricates are exempt: their closures are silent.
    loud = {}
    for key, cost, cid, seg, cut, marks, hz, halves, ceps in measured:
        a, b = key.split("-")
        for ph, v in ((a, halves[0]), (b, halves[1])):
            if v > 0:
                loud.setdefault(ph, []).append(v)
    loud = {k: float(np.median(v)) for k, v in loud.items()}
    silent_ok = {"_", "P", "T", "K", "B", "D", "G", "CH", "JH"}

    def too_loud(ph, v, other):
        """A PAUSE half no quieter than 20dB below its speech half is not a pause."""
        if ph != "_" or other <= 0:
            return 0.0
        return 5.0 if v > 0.1 * other else 0.0

    def too_quiet(ph, v):
        if ph in silent_ok or ph not in loud:
            return 0.0
        return 3.0 if v < 0.15 * loud[ph] else 0.0

    def atypical(ph, c):
        if ph == "_" or c is None or ph not in centre:
            return 0.0
        return np.linalg.norm(c - centre[ph]) / spread[ph]

    chosen = {}
    for key, cost, cid, seg, cut, marks, hz, halves, ceps in measured:
        a, b = key.split("-")
        c = (cost + (abs(np.log(hz / mid_hz)) if hz > 0 else 0.0)
             + atypical(a, ceps[0]) + atypical(b, ceps[1])
             + too_quiet(a, halves[0]) + too_quiet(b, halves[1])
             + too_loud(a, halves[0], halves[1]) + too_loud(b, halves[1], halves[0]))
        if key not in chosen or c < chosen[key][0]:
            chosen[key] = (c, cid, seg, cut, marks, hz, halves)

    # Each phone's typical loudness, from the halves of every chosen
    # unit it appears in. Units come from different recordings, said at
    # different volumes, and joining them unevened the level of every
    # sentence: each half is brought to its phone's median.
    level = {}
    for key, (c, cid, seg, cut, marks, hz, rms) in chosen.items():
        a, b = key.split("-")
        for ph, part in ((a, seg[:cut]), (b, seg[cut:])):
            if len(part) > 20:
                level.setdefault(ph, []).append(float(np.sqrt(np.mean(part ** 2))) + 1e-6)
    level = {k: float(np.median(v)) for k, v in level.items()}

    os.makedirs(out_path, exist_ok=True)
    table, parts, off = {}, [], 0
    for key in sorted(chosen):
        c, cid, seg, cut, marks, hz, rms = chosen[key]
        a, b = key.split("-")
        ga = level.get(a, 1.0) / (float(np.sqrt(np.mean(seg[:cut] ** 2))) + 1e-6) if cut > 20 else 1.0
        gb = level.get(b, 1.0) / (float(np.sqrt(np.mean(seg[cut:] ** 2))) + 1e-6) if len(seg) - cut > 20 else 1.0
        # Silence is left alone; the gain changes smoothly across the unit.
        ga = 1.0 if a == "_" else min(4.0, max(0.25, ga))
        gb = 1.0 if b == "_" else min(4.0, max(0.25, gb))
        seg = seg * np.interp(np.arange(len(seg)), [0, max(1, len(seg) - 1)], [ga, gb])
        pcm = np.clip(seg * 32767.0, -32768, 32767).astype("<i2")
        table[key] = {"off": off, "len": len(pcm), "cut": cut, "clip": cid, "hz": hz,
                      "marks": [[int(p), int(v)] for p, v in marks]}
        parts.append(pcm)
        off += len(pcm)
    np.concatenate(parts).tofile(os.path.join(out_path, "units.pcm"))
    json.dump({"fs": FS, "speaker_hz": mid_hz, "units": table},
              open(os.path.join(out_path, "units.json"), "w"))

    phones = sorted(set(k.split("-")[0] for k in chosen) | set(k.split("-")[1] for k in chosen))
    return {"units": len(chosen), "phones": len(phones), "samples": off,
            "seconds": off / float(FS), "speaker_hz": mid_hz}


class Inventory:

    def __init__(self, path):
        meta = json.load(open(os.path.join(path, "units.json")))
        self.units = meta["units"]
        for k, u in self.units.items():
            u["_key"] = k
        self.pcm = np.fromfile(os.path.join(path, "units.pcm"), dtype="<i2").astype(np.float64) / 32768.0
        self.missing = {}

        # For stand-ins: some unit starting with each phone, and some
        # ending with it -- through a pause if there is one.
        self.starts, self.ends = {}, {}
        for k, u in sorted(self.units.items()):
            a, b = k.split("-")
            if a not in self.starts or b == "_":
                self.starts[a] = u
            if b not in self.ends or a == "_":
                self.ends[b] = u

    def unit(self, a, b):
        """The unit for a-b, or None (see halves())."""
        u = self.units.get(a + "-" + b)
        if u is None:
            self.missing[a + "-" + b] = self.missing.get(a + "-" + b, 0) + 1
        return u

    def halves(self, a, b):
        """
        A stand-in for a missing a-b: the first half of a unit starting
        with a, and the second half of one ending with b. The join then
        falls at the phone boundary instead of mid-phone, which is worse
        -- but a gap of silence, which is what a missing unit was, is
        worse still.
        """
        return self.starts.get(a), self.ends.get(b)


# -- synthesis --

def loud_pause(u, pcm):
    """
    Whether this unit's PAUSE half is no pause: louder than a tenth of its
    speech half's level (-20dB). Such a half holds part of a word the
    alignment mistook for a pause, and none of it is played. Worked out
    per unit here, and carried in the pack for the device.
    """
    k = u.get("_key", "")
    seg = pcm[u["off"]:u["off"] + u["len"]]
    c = u["cut"]
    if k.startswith("_-"):
        pause, speech = seg[:c], seg[c:]
    elif k.endswith("-_"):
        pause, speech = seg[c:], seg[:c]
    else:
        return False
    if len(pause) < 20 or len(speech) < 20:
        return False
    return float(np.mean(pause ** 2)) > 0.01 * float(np.mean(speech ** 2))


def _read_f0(path):
    t, f = [], []
    for line in open(path):
        a, b = line.split()
        t.append(float(a))
        f.append(float(b))
    return np.array(t), np.array(f)


def synthesise(inv, phones, f0_path, own_pitch=False, f0=None):
    """
    TD-PSOLA over the utterance: `phones` [(name, start_ms, end_ms)] and
    the pitch contour from our front end (or `f0` = (times_ms, hz)
    directly). With `own_pitch`, each unit keeps the pitch it was
    recorded at: only the timing is changed -- a diagnostic, separating
    what pitch-shifting costs from the rest. Returns float samples at FS.
    """
    ft, fv = f0 if f0 is not None else _read_f0(f0_path)
    total_ms = phones[-1][2] if phones else 0
    n_out = int(total_ms * FS / 1000) + FS // 10
    out = np.zeros(n_out)
    wsum = np.zeros(n_out)

    # Each unit covers target mid(A) .. mid(B); within it, the source's
    # first half maps onto [mid(A), end(A)] and its second onto
    # [start(B), mid(B)].
    segs = []
    for (a, a0, a1), (b, b0, b1) in _units_of(phones):
        t0, tc, t1 = (a0 + a1) / 2.0, a1, (b0 + b1) / 2.0
        o0, oc, o1 = t0 * FS / 1000, tc * FS / 1000, t1 * FS / 1000
        pa, pb = _base(a) == "_", _base(b) == "_"
        u = inv.unit(_base(a), _base(b))
        if u is not None:
            segs.append((o0, oc, o1, u, "both", pa, pb))
        else:
            ua, ub = inv.halves(_base(a), _base(b))
            if ua is not None:
                segs.append((o0, oc, oc, ua, "left", pa, False))
            if ub is not None:
                segs.append((oc, oc, o1, ub, "right", False, pb))

    def target_hz(sample):
        ms = sample * 1000.0 / FS
        i = int(np.searchsorted(ft, ms))
        if i >= len(fv):
            return 0.0
        return fv[i]

    # ONE lattice of output pitch marks for the whole utterance. Each
    # mark finds the unit it falls in, and the nearest source mark through
    # that unit's time warp. (The first prototype restarted the lattice at
    # every unit, which broke the rhythm of the voice at every join --
    # every 60ms or so.)
    if not segs:
        return out[:0]
    segs.sort(key=lambda z: z[0])
    prepared = []
    for (o0, oc, o1, u, part, pa, pb) in segs:
        if not u["marks"]:
            continue
        src = inv.pcm[u["off"]:u["off"] + u["len"]]
        mpos = np.array([m[0] for m in u["marks"]])
        mv = [m[1] for m in u["marks"]]
        lo_t, hi_t = (o0, oc) if part == "left" else ((oc, o1) if part == "right" else (o0, o1))
        keep = 0 if loud_pause(u, inv.pcm) else PAUSE_KEEP
        prepared.append((lo_t, hi_t, o0, oc, o1, part, u["cut"], u["len"], src, mpos, mv, pa, pb, keep))

    def grain_at(e, t):
        """
        The grain unit `e` gives for output time t: (samples, window, start
        in the output, source mark voiced, source period), or None where
        the unit is silent there (a pause half beyond what it keeps).
        """
        lo_t, hi_t, o0, oc, o1, part, cut, n_src, src, mpos, mv, pa, pb, keep = e
        first = part == "left" or (part == "both" and t < oc)
        if first and pa:
            s_t = cut - (oc - t)
            if s_t < max(0, cut - keep):
                return None
        elif not first and pb:
            s_t = cut + (t - oc)
            if s_t >= min(n_src, cut + keep):
                return None
        elif first:
            s_t = (t - o0) / max(oc - o0, 1.0) * cut
        else:
            s_t = cut + (t - oc) / max(o1 - oc, 1.0) * (n_src - cut)
        k = int(np.argmin(np.abs(mpos - s_t)))
        p_src = p_src_at(mpos, mv, k) if mv[k] else UNVOICED_STEP
        c = int(mpos[k])
        lo, hi = max(0, c - p_src), min(n_src, c + p_src)
        if hi - lo <= 2:
            return None
        w = np.hanning(hi - lo + 2)[1:-1]
        return src[lo:hi] * w, w, int(t) - (c - lo), mv[k], p_src

    def add(gr, weight):
        g, w, start, _, _ = gr
        a_, b_ = max(0, start), min(n_out, start + len(g))
        if b_ > a_:
            out[a_:b_] += weight * g[a_ - start:b_ - start]
            wsum[a_:b_] += weight * w[a_ - start:b_ - start]

    t = prepared[0][0]
    j = 0
    last = prepared[-1][1]
    while t < last:
        while j + 1 < len(prepared) and t >= prepared[j][1]:
            j += 1
        e = prepared[j]
        lo_t, hi_t = e[0], e[1]
        if t < lo_t:                         # a hole between units: step over it
            t = lo_t
            continue
        gr = grain_at(e, t)
        if gr is None:
            t += UNVOICED_STEP
            continue

        # CROSS-FADE at the joins: within XFADE of a boundary with a
        # neighbouring unit, that unit's grain for this moment is added
        # too, the two weighted from all-this-unit to half-and-half at the
        # boundary. A hard switch from one recording to another made the
        # joins 1.4x more abrupt than the recording's own at the same
        # moments -- the bumps between sounds.
        wt = 1.0
        if j + 1 < len(prepared) and prepared[j + 1][0] == hi_t and hi_t - t < XFADE:
            other = grain_at(prepared[j + 1], t)
            if other is not None:
                wn = 0.5 * (1.0 - (hi_t - t) / XFADE)
                add(other, wn)
                wt -= wn
        if j > 0 and prepared[j - 1][1] == lo_t and t - lo_t < XFADE:
            other = grain_at(prepared[j - 1], t)
            if other is not None:
                wp = 0.5 * (1.0 - (t - lo_t) / XFADE)
                add(other, wp)
                wt -= wp
        add(gr, wt)

        hz = target_hz(t)
        if own_pitch and gr[3]:
            hz = FS / float(gr[4])
        voiced = gr[3] and hz > 0
        t += max(20, int(FS / hz) if voiced else UNVOICED_STEP)

    # Where grains overlap more or less than half, even the level out.
    out = np.where(wsum > 0.2, out / np.maximum(wsum, 1e-9), out)
    return out[:int(total_ms * FS / 1000) + 1]


def p_src_at(mpos, mv, k):
    if k + 1 < len(mpos) and mv[k + 1]:
        return max(20, int(mpos[k + 1] - mpos[k]))
    if k > 0 and mv[k - 1]:
        return max(20, int(mpos[k] - mpos[k - 1]))
    return int(FS / 200)


def render_items(inv, ref_dir, n_items, mode="", copies=None, log=print):
    """
    Replaces each t_NNNN.wav in ref_dir with the diphone rendering.

    mode "own-pitch": units keep their recorded pitch.
    `copies`: {item index: (aligned phones path, recording path)} --
    COPY SYNTHESIS for those items: the recording's own phones, timing
    and pitch, rebuilt from diphones. Our front end is out of it
    entirely, so what is left is the units and the joins: the test that
    says which of the two is the problem.
    """
    lacking = 0
    for i in range(n_items):
        base = os.path.join(ref_dir, "t_%04d" % i)
        if copies and i in copies and not all(os.path.exists(p) for p in copies[i]):
            # Never leave the formant rendering standing in for a copy
            # synthesis: fall back to the diphone voice, and say so.
            lacking += 1
            copies_i = None
        else:
            copies_i = copies.get(i) if copies else None
        if copies_i:
            ph_path, rec_path = copies_i
            phones = aligner.read_phones(ph_path)
            real = audio.pitch(audio.read_wav(rec_path))
            t = np.arange(len(real)) * (1000.0 * audio.HOP / FS)
            y = synthesise(inv, phones, None, f0=(t, real))
        else:
            if not os.path.exists(base + ".phones"):
                continue
            phones = aligner.read_phones(base + ".phones")
            y = synthesise(inv, phones, base + ".f0", own_pitch=(mode == "own-pitch"))
        peak = np.max(np.abs(y)) if len(y) else 0
        if peak > 0.95:
            y = y * (0.95 / peak)
        audio.write_wav(base + ".wav", y)
    if lacking:
        log("  note: %d items had no recording or alignment for copy synthesis; "
            "rendered by the front end instead" % lacking)


# -- the pack section --
#
# DIPHONE, version 1, little-endian:
#
#   u16 version, u16 sample rate, u16 phone count (P), u16 unit count (U),
#   u32 offset of the marks, u32 offset of the samples   (16 bytes)
#   P x 2 bytes     phone names ("_" for a pause), NUL-padded
#   P x P x u16     unit id for each pair (first x P + second), 0xffff: none
#   P x u16         a unit STARTING with each phone (for stand-ins), 0xffff
#   P x u16         a unit ENDING with each phone
#   U x 16 bytes    per unit: u32 first sample, u32 first mark,
#                   u16 samples, u16 cut (where the first phone ends),
#                   u16 marks, u16 flags (bit 0: the pause half holds no
#                   pause -- play none of it; see loud_pause())
#   marks           u16 each: position in the unit, bit 15 set if voiced
#   samples         8-bit mu-law
#
# The tables up to the marks are small (a few tens of KB) and resident;
# a unit's marks and samples are read from the card when the voice
# reaches it (sw/apps/tts/dsyn.c).

MU = 255.0


def mulaw_encode(x):
    """Float samples in [-1, 1] -> 8-bit mu-law codes (0..255)."""
    x = np.clip(x, -1.0, 1.0)
    y = np.sign(x) * np.log1p(MU * np.abs(x)) / np.log1p(MU)
    return np.round((y + 1.0) * 127.5).astype(np.uint8)


def mulaw_decode_table():
    """The 256 codes back to int16, as the device's table holds them."""
    y = np.arange(256) / 127.5 - 1.0
    x = np.sign(y) * (np.power(1.0 + MU, np.abs(y)) - 1.0) / MU
    return np.round(x * 32767.0).astype(np.int16)


def serialise(inv_dir, phones_order):
    """The DIPHONE section for the inventory in `inv_dir`."""
    meta = json.load(open(os.path.join(inv_dir, "units.json")))
    pcm = np.fromfile(os.path.join(inv_dir, "units.pcm"), dtype="<i2").astype(np.float64) / 32768.0
    names = list(phones_order) + ["_"]
    index = {n: i for i, n in enumerate(names)}
    P = len(names)
    units = []
    for key, u in sorted(meta["units"].items()):
        a, b = key.split("-")
        if a in index and b in index:
            u["_key"] = key
            units.append((index[a], index[b], u))
    U = len(units)
    pair = np.full(P * P, 0xFFFF, dtype="<u2")
    start = np.full(P, 0xFFFF, dtype="<u2")
    end = np.full(P, 0xFFFF, dtype="<u2")
    recs, marks, data = bytearray(), [], []
    sample_off = 0
    for uid, (ia, ib, u) in enumerate(units):
        pair[ia * P + ib] = uid
        if start[ia] == 0xFFFF or ib == index["_"]:
            start[ia] = uid
        if end[ib] == 0xFFFF or ia == index["_"]:
            end[ib] = uid
        seg = pcm[u["off"]:u["off"] + u["len"]]
        m = [min(p, 0x7FFF) | (0x8000 if v else 0) for p, v in u["marks"] if p < 0x8000]
        flags = 1 if loud_pause(u, pcm) else 0
        recs += struct.pack("<IIHHHH", sample_off, len(marks), len(seg), u["cut"], len(m), flags)
        marks += m
        data.append(mulaw_encode(seg))
        sample_off += len(seg)
    head_len = 16 + 2 * P + 2 * P * P + 4 * P + 16 * U
    marks_off = head_len
    data_off = marks_off + 2 * len(marks)
    out = struct.pack("<HHHHII", 1, FS, P, U, marks_off, data_off)
    out += b"".join((n.encode("ascii") + b"\0\0")[:2] for n in names)
    out += pair.tobytes() + start.tobytes() + end.tobytes() + bytes(recs)
    out += np.array(marks, dtype="<u2").tobytes()
    out += np.concatenate(data).tobytes() if data else b""
    return out
