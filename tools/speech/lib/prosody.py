"""
Zeitlos
Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.

Fitting the prosody parameters to a real speaker.

sw/apps/tts/phon.c shapes timing and pitch with a dozen numbers
(phon.h, PHON_PRO_*) plus a duration per phoneme. They were set by
hand. This measures the same quantities in an aligned corpus
(lib/corpus.py) and writes them into a pack's PROSODY section, which
the device loads in place of the hand-set values.

It is deliberately the SAME model with measured numbers, not a new
model: a dozen parameters fitted to 2,000 sentences are well
determined, cannot overfit, and fail safe -- the device clamps each
one to a sane range. A learned model with thousands of weights is a
later step, and would be judged against this.

Every measurement is a median over many occurrences, because the
alignment is imperfect: a boundary a frame or two off moves a median
very little and a mean a lot.
"""

import os
import struct

import numpy as np

from . import aligner, audio

# The order the device reads them in: phon.h, PHON_PRO_*. Append only.
FIELDS = [
    "unstressed", "final_comma", "final_stop",
    "pause_comma", "pause_stop", "pause_para",
    "top", "decl", "step", "step_max",
    "stress1", "stress2", "comma_rise", "stop_fall",
    "open", "tilt",
]

# The hand-set values: what is used for anything not measured.
DEFAULTS = dict(unstressed=55, final_comma=135, final_stop=155,
                pause_comma=190, pause_stop=300, pause_para=420,
                top=112, decl=17, step=4, step_max=16,
                stress1=17, stress2=8, comma_rise=13, stop_fall=10,
                open=50, tilt=0)

# Sane ranges, the same as the device's. A value outside one means the
# measurement went wrong, and the default is kept instead.
LIMITS = dict(unstressed=(30, 90), final_comma=(100, 220), final_stop=(100, 250),
              pause_comma=(60, 500), pause_stop=(120, 900), pause_para=(200, 1500),
              top=(95, 140), decl=(0, 40), step=(0, 12), step_max=(0, 40),
              stress1=(0, 40), stress2=(0, 30), comma_rise=(-10, 40), stop_fall=(0, 40),
              open=(35, 80), tilt=(0, 24))

FRAME_MS = 1000.0 * audio.HOP / audio.FS


def _base(name):
    return name.rstrip("012")


def _is_vowel(name):
    return name[:1] in "AEIOU" and name[-1:].isdigit()


def _stress(name):
    return int(name[-1]) if name[-1:].isdigit() else 0


def phrases(phones):
    """Splits a clip's phones at its pauses: [(phones, pause_kind)]."""
    out, cur = [], []
    for p in phones:
        if p[0].startswith("_"):
            if cur:
                out.append((cur, p[0]))
            cur = []
        else:
            cur.append(p)
    if cur:
        out.append((cur, "_end"))
    return out


class Measure:

    def __init__(self):
        self.dur = {}               # (phone, stress, final kind) -> [ms]
        self.pause = {}             # kind -> [ms]
        self.f0_all = []
        self.top, self.decl, self.steps = [], [], []
        self.stress = {1: [], 2: []}
        self.comma_end, self.stop_end = [], []
        self.clips = 0

    # -- durations and pauses --

    def add_timing(self, phones):
        last_speech = max((i for i, p in enumerate(phones) if not p[0].startswith("_")),
                          default=-1)
        for i, (name, a, b) in enumerate(phones):
            if name.startswith("_"):
                # A pause after the last word is not a pause between
                # anything: the recording's trailing silence was
                # trimmed before alignment, so it measures as nothing.
                if name != "_" and i < last_speech:
                    self.pause.setdefault(name, []).append(b - a)
                continue
            # Final: the last vowel before a pause, and what follows it.
            nxt = None
            for later in phones[i + 1:]:
                if later[0].startswith("_"):
                    nxt = later[0]
                    break
                if _is_vowel(later[0]):
                    break
            final = nxt if nxt in ("_c", "_s", "_p") else ""
            self.dur.setdefault((_base(name), _stress(name), final), []).append(b - a)

    # -- pitch --

    def add_pitch(self, f0, phones):
        """
        f0 per 10ms frame; phones in the recording's time.

        Per phrase, a straight line is fitted to pitch against position
        in the phrase (0 at its start, 1 at its end), over the first 80%
        -- the part before any rise or fall at its end. Its intercept
        is the phrase's starting pitch and its slope the declination,
        which is exactly what phon.c's TOP and DECL mean. The end of the
        phrase is then measured as its departure from that line, which
        separates a rise at a comma from the decline that would have
        happened anyway.
        """

        def at(ms):
            i = int(ms / FRAME_MS)
            return f0[i] if 0 <= i < len(f0) else 0.0

        self.f0_all.extend(f0[f0 > 0].tolist())

        starts = []
        for seq, kind in phrases(phones):
            a, b = seq[0][1], seq[-1][2]
            if b - a < 500:
                starts.append(None)
                continue
            i0, i1 = int(a / FRAME_MS), int(b / FRAME_MS)
            seg = f0[i0:i1 + 1]
            pos = np.linspace(0.0, 1.0, len(seg))
            # The line is fitted WITHOUT the stressed vowels: they are
            # the bumps on it, and a line through them absorbs the very
            # lift being measured.
            bumps = np.zeros(len(seg), dtype=bool)
            for vn, va, vb in seq:
                if _is_vowel(vn) and _stress(vn) > 0:
                    bumps[max(0, int(va / FRAME_MS) - i0):max(0, int(vb / FRAME_MS) - i0 + 1)] = True
            body = (seg > 0) & (pos <= 0.8) & ~bumps
            if body.sum() < 12:
                starts.append(None)
                continue
            slope, icpt = np.polyfit(pos[body], seg[body], 1)
            starts.append(icpt)
            self.top.append(icpt)
            self.decl.append(-slope)

            # The last 200ms, against where the line says it would be.
            tail = (seg > 0) & (pos >= 1.0 - 20.0 / max(len(seg), 1))
            if tail.sum() >= 3:
                resid = float(np.mean(seg[tail] - (icpt + slope * pos[tail])))
                if kind == "_c":
                    self.comma_end.append(resid)
                elif kind in ("_s", "_p", "_end"):
                    self.stop_end.append(-resid)

            # Stress: a stressed vowel's pitch against the line at the
            # same point -- the lift phon.c's STRESS parameters add.
            for vn, va, vb in seq:
                if not _is_vowel(vn) or _stress(vn) == 0:
                    continue
                mid = at((va + vb) / 2.0)
                if not mid:
                    continue
                x = ((va + vb) / 2.0 - a) / max(b - a, 1.0)
                if x > 0.8:
                    continue
                self.stress[_stress(vn)].append(mid - (icpt + slope * x))

        for x, y in zip(starts, starts[1:]):
            if x and y:
                self.steps.append(x - y)

        self.clips += 1

    # -- the fit --

    def raw_pitch(self):
        """The pitch measurements, as % of this voice's median."""
        if len(self.f0_all) <= 100:
            return {}
        base = float(np.median(self.f0_all))

        def pct(v):
            return 100.0 * float(np.median(v)) / base if len(v) else None

        out = dict(top=pct(self.top), decl=pct(self.decl), step=pct(self.steps),
                   stress1=pct(self.stress[1]) if len(self.stress[1]) >= 20 else None,
                   stress2=pct(self.stress[2]) if len(self.stress[2]) >= 20 else None,
                   comma_rise=pct(self.comma_end) if len(self.comma_end) >= 10 else None,
                   stop_fall=pct(self.stop_end) if len(self.stop_end) >= 10 else None)
        if self.steps:
            out["step_max"] = 100.0 * float(np.percentile(self.steps, 90)) / base
        out["_median_hz"] = base
        return out

    def fit(self, table_durs, calib=None, rate_ratio=None):
        """
        Returns (fields, durations, notes). `table_durs` is the
        device's own table, {phone: ms}, which fixes the overall speed:
        the fitted durations keep the speaker's RELATIVE timing but
        are scaled so the voice speaks at the same rate for the same
        setting -- the rate is the user's choice, not the speaker's.
        """

        notes = []
        f = dict(DEFAULTS)
        measured = set()

        def med(v):
            return float(np.median(v)) if len(v) else None

        # Per-phone durations, CALIBRATED the way pitch is. What the
        # device's table calls a phone's duration is not always what
        # an alignment measures: for a stop or affricate it is only the
        # closure, and the burst and aspiration come on top. So each
        # phone is measured in our own voice too (rendering the same
        # sentences at the speaker's rate, from the table), and the
        # table's value is scaled by speaker / ours. That also takes
        # care of the speaking rate, and the result is normalised so
        # the overall speed at a given rate setting does not change --
        # the speaker's RELATIVE timing, the user's choice of speed.
        def phone_median(meas, ph):
            key = (ph, 1 if ph[:1] in "AEIOU" else 0, "")
            v = meas.dur.get(key, [])
            return (med(v), len(v)) if len(v) >= 20 else (None, len(v))

        ratios, weights = {}, {}
        for ph in table_durs:
            mine, n = phone_median(self, ph)
            if mine is None:
                continue
            if calib is not None:
                ours, _ = phone_median(calib, ph)
                if not ours:
                    continue
                ratios[ph] = mine / ours
            else:
                ratios[ph] = mine / float(table_durs[ph])
            weights[ph] = n

        durs = {}
        scale = 1.0
        if ratios:
            # The weighted mean ratio is the speaker's overall speed
            # relative to ours; divide it out.
            tot = sum(weights.values())
            mean = sum(ratios[p] * weights[p] for p in ratios) / tot
            for ph, r in ratios.items():
                durs[ph] = int(round(table_durs[ph] * r / mean))
            scale = 1.0 / mean if calib is None else 1.0
            notes.append("durations: %d phones measured%s" % (
                len(durs), ", calibrated against our own voice" if calib is not None
                else ", uncalibrated (no reference)"))

        # Pauses are measured in the speaker's time, and phon.c scales
        # them with the rate setting like everything else -- so they
        # come back to ours by the speaker's overall speed against the
        # 180wpm the table is written for.
        if rate_ratio:
            scale = 1.0 / rate_ratio

        # Unstressed against stressed, pooled over vowels.
        ratios = []
        for (ph, st, fin), v in self.dur.items():
            if st == 0 and not fin and ph[:1] in "AEIOU":
                s1 = self.dur.get((ph, 1, ""), [])
                if len(v) >= 10 and len(s1) >= 10:
                    ratios.append(med(v) / med(s1))
        if ratios:
            f["unstressed"] = round(100 * float(np.median(ratios)))
            measured.add("unstressed")

        # Phrase-final lengthening: final against non-final, same phone
        # and stress.
        for kind, key in (("_c", "final_comma"), ("_s", "final_stop")):
            r = []
            for (ph, st, fin), v in self.dur.items():
                # Vowels only: the lengthening is on the phrase's last
                # vowel (phon.c), and its consonants are not stretched.
                if fin != kind or len(v) < 5 or ph[:1] not in "AEIOU":
                    continue
                base = self.dur.get((ph, st, ""), [])
                if len(base) >= 10:
                    r.append(med(v) / med(base))
            if len(r) >= 3:
                f[key] = round(100 * float(np.median(r)))
                measured.add(key)

        # Pauses, in the device's time (the same scale as durations).
        for kind, key in (("_c", "pause_comma"), ("_s", "pause_stop"), ("_p", "pause_para")):
            v = self.pause.get(kind, [])
            if len(v) >= 10:
                f[key] = round(med(v) * scale)
                measured.add(key)

        # Pitch. Measured as % of the speaker's median, then CALIBRATED:
        # `calib` is the same measurement taken on our own voice
        # rendering the same sentences with the default parameters, so
        # measured/default says how this measurement reads each
        # parameter, and dividing the speaker's measurement by it
        # cancels the bias. Without it the raw measurement is used.
        raw = self.raw_pitch()
        ref = calib.raw_pitch() if calib is not None else {}
        if raw:
            notes.append("pitch: speaker median %.0f Hz; fitted as %% of it" % raw["_median_hz"])
            for key in ("top", "decl", "step", "step_max", "stress1", "stress2",
                        "comma_rise", "stop_fall"):
                v = raw.get(key)
                if v is None:
                    continue
                if key == "top":
                    # An intercept, not a size: calibrate as an offset.
                    if ref.get("top") is not None:
                        v = DEFAULTS["top"] + (v - ref["top"])
                    f[key] = round(v)
                    measured.add(key)
                    continue
                r = ref.get(key)
                if r is not None and (r > 0) != (DEFAULTS[key] > 0):
                    # Our own voice reads this parameter with the wrong
                    # sign -- the measurement is not seeing what the
                    # parameter does -- so dividing by it would only
                    # flip the answer. Refuse.
                    notes.append("%s: our own voice reads as %.1f against a setting "
                                 "of %d -- the wrong sign, not calibratable; default kept"
                                 % (key, r, DEFAULTS[key]))
                    continue
                if r is not None:
                    # A weak reading is fine to calibrate by when it is
                    # well sampled -- a comma rise ramps up over 200ms,
                    # so it reads at a fraction of its setting, but the
                    # same fraction for both voices. Only a reading
                    # that is BOTH tiny and scarce is refused.
                    if abs(r) < 0.10 * max(abs(DEFAULTS[key]), 1):
                        notes.append("%s: our own voice reads as %.1f against a "
                                     "setting of %d -- too weak to calibrate, kept "
                                     "the default" % (key, r, DEFAULTS[key]))
                        continue
                    v = v * DEFAULTS[key] / r
                f[key] = round(v)
                measured.add(key)
            # Secondary stress lifting pitch more than primary is not a
            # speaker, it is a measurement error.
            if f["stress2"] > f["stress1"]:
                notes.append("stress2 (%d) came out above stress1 (%d); capped at half of it"
                             % (f["stress2"], f["stress1"]))
                f["stress2"] = f["stress1"] // 2
            if f["step_max"] < f["step"]:
                f["step_max"] = f["step"]
                measured.add("step_max")

        # Anything outside its sane range is a measurement gone wrong:
        # keep the default and say so.
        for k, (lo, hi) in LIMITS.items():
            if not (lo <= f[k] <= hi):
                notes.append("%s measured %s, outside %d..%d -- kept the default %d"
                             % (k, f[k], lo, hi, DEFAULTS[k]))
                f[k] = DEFAULTS[k]
                measured.discard(k)

        # Vowels are the durations that matter most; say plainly if
        # none were measured.
        if durs and not any(ph[:1] in "AEIOU" for ph in durs):
            notes.append("no vowel durations measured -- are the alignments "
                         "missing stress marks?")

        return f, durs, notes, measured


def measure_corpus(clips, align_dir, limit=0, prefix=""):
    """
    clips: [(id, text, wav)] as corpus.read_metadata returns. With
    `prefix`, the wavs are read from align_dir too -- how the
    references (t_<id>.wav beside t_<id>.phones) are measured for
    calibration.
    """
    m = Measure()
    for cid, _, wav in clips:
        if prefix:
            wav = os.path.join(align_dir, prefix + cid + ".wav")
        path = os.path.join(align_dir, prefix + cid + ".phones")
        if not os.path.exists(path):
            continue
        phones = aligner.read_phones(path)
        if not phones:
            continue
        m.add_timing(phones)
        x = audio.read_wav(wav)
        m.add_pitch(audio.pitch(x), phones)
        if limit and m.clips >= limit:
            break
    return m


def serialise(fields, durs):
    """
    PROSODY section, version 1:
      u16 version, u16 field count, i16 fields[] (FIELDS order),
      u16 phone count, then per phone: 2 name bytes, u16 ms.
    """
    out = struct.pack("<HH", 1, len(FIELDS))
    out += b"".join(struct.pack("<h", int(fields[k])) for k in FIELDS)
    out += struct.pack("<H", len(durs))
    for ph, ms in sorted(durs.items()):
        nm = ph.encode("ascii")[:2]
        out += nm + b"\0" * (2 - len(nm)) + struct.pack("<H", int(ms))
    return out


def table_durations(repo):
    """
    The device's own duration per phoneme, read from phon.c: vowels
    are written V("IY", dur, ...) or D("AY", dur, ...), everything else
    { "W", K_SEMI, voiced, dur, ... }. Read rather than copied so the
    two cannot drift apart.
    """
    import re
    src = open(os.path.join(repo, "sw", "apps", "tts", "phon.c")).read()
    out = {}
    for m in re.finditer(r'\b[VD]\("([A-Z]{1,2})",\s*(\d+)', src):
        out[m.group(1)] = int(m.group(2))
    for m in re.finditer(r'\{\s*"([A-Z]{1,2})",\s*K_[A-Z]+,\s*\d+,\s*(\d+)', src):
        out.setdefault(m.group(1), int(m.group(2)))
    return out


def parse(blob):
    """The inverse of serialise(): (fields, durations)."""
    fields = dict(DEFAULTS)
    durs = {}
    if not blob or len(blob) < 4:
        return fields, durs
    version, nf = struct.unpack("<HH", blob[:4])
    p = 4
    for i in range(nf):
        v = struct.unpack("<h", blob[p:p + 2])[0]
        if i < len(FIELDS):
            fields[FIELDS[i]] = v
        p += 2
    if p + 2 <= len(blob):
        n = struct.unpack("<H", blob[p:p + 2])[0]
        p += 2
        for _ in range(n):
            nm = blob[p:p + 2].rstrip(b"\0").decode("ascii")
            durs[nm] = struct.unpack("<H", blob[p + 2:p + 4])[0]
            p += 4
    return fields, durs
