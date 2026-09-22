"""
Zeitlos
Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.

Comparing versions of the voice: several variants scored on the same
frozen material in one run, each against the one before it and against
the first, with 95% intervals on the differences. What session 0 of the
plan runs as an ablation: the shipped voice with nothing, then one
fitted stage added at a time.
"""

import os

from . import acoustics, ledger, pack as packlib, score

HELD = ["MANIFEST", "PHONES", "LEXIDX", "LEXDAT"]


def ablation_variants(built_pack, align_dir, out_dir):
    """
    [(name, pack path or "", note)]: cumulative, from nothing to
    everything fitted. A stage whose files are missing is skipped with a
    note rather than silently dropped.
    """
    os.makedirs(out_dir, exist_ok=True)
    out = [("shipped, no pack", "", "built-in dictionary and letter rules")]
    if not os.path.exists(built_pack):
        return out, ["no built pack at %s: only the no-pack variant" % built_pack]
    secs = packlib.read(built_pack)
    notes = []
    cur = {k: secs[k] for k in HELD if k in secs}

    def add(name, what):
        p = os.path.join(out_dir, "%d.zspk" % len(out))
        packlib.write(p, [(k, cur[k]) for k in score.ORDER if k in cur])
        out.append((name, p, what))

    add("+ lexicon", "391k pronunciations from Moby")
    if "LTS" in secs:
        cur["LTS"] = secs["LTS"]
        add("+ trained letter rules", "for words not in the lexicon")
    pb = os.path.join(align_dir, "prosody.bin")
    if os.path.exists(pb):
        cur["PROSODY"] = open(pb, "rb").read()
        add("+ fitted prosody", "timing and pitch from the speaker")
    else:
        notes.append("no %s: prosody stage skipped" % pb)
    fb = os.path.join(align_dir, "formants.bin")
    if os.path.exists(fb):
        blob = open(fb, "rb").read()
        rows = _parse_formants(blob)
        vowels = {k: v for k, v in rows.items() if k in acoustics.MEASURE}
        cur["FORMANTS"] = acoustics.serialise(vowels)
        add("+ fitted vowels", "formants of %d vowels and sonorants" % len(vowels))
        if len(rows) > len(vowels):
            cur["FORMANTS"] = blob
            add("+ fitted consonant loci", "%d loci" % (len(rows) - len(vowels)))
    else:
        notes.append("no %s: formant stages skipped" % fb)
    sb = os.path.join(align_dir, "prosody_source.bin")
    if os.path.exists(sb):
        cur["PROSODY"] = open(sb, "rb").read()
        add("+ fitted voice source", "open quotient and tilt")
    else:
        notes.append("no %s: voice source stage skipped" % sb)
    return out, notes


# Session 1b: the two experiments that did their job cleanly in session
# 1, and the two repaired (docs/tts_data.md, "Session 1: what it found").
EXPERIMENTS = [
    ("vowel-voicing", "shorter vowel before a voiceless final consonant"),
    ("nasal", "abrupt nasal releases"),
    ("vot", "longer aspiration, now ADDED before the vowel, not taken from it"),
    ("velar", "K and G's locus follows the vowel; higher before back vowels"),
]


def session1_variants(built_pack, out_dir):
    """
    Session 1: the default pack -- lexicon and trained letter rules, what
    session 0 showed helps -- then each experiment alone, then all
    together. [(name, pack, env, note)], every one compared against the
    first.
    """
    os.makedirs(out_dir, exist_ok=True)
    secs = packlib.read(built_pack)
    keep = HELD + ["LTS"]
    base = os.path.join(out_dir, "default.zspk")
    packlib.write(base, [(k, secs[k]) for k in score.ORDER if k in secs and k in keep])
    out = [("default (lexicon + rules)", base, {}, "what session 0 showed helps")]
    for name, what in EXPERIMENTS:
        out.append(("+ " + name, base, {"ZTTS_EXP": name}, what))
    out.append(("+ all four", base, {"ZTTS_EXP": ",".join(n for n, _ in EXPERIMENTS)},
                "every experiment at once"))
    return out


def diphone_variants(built_pack, inv_dir, out_dir, align_dir="", corpus_dir=""):
    """
    The diphone prototype against the voices it would replace: the formant
    voice as shipped (male) and as it would fall back (female), and the
    diphone voice on the same front end, pack and timing.
    """
    from . import ledger as ledgerlib
    os.makedirs(out_dir, exist_ok=True)
    secs = packlib.read(built_pack)
    base = os.path.join(out_dir, "default.zspk")
    packlib.write(base, [(k, secs[k]) for k in score.ORDER if k in secs and k in HELD + ["LTS"]])
    def dv(inv, **extra):
        # The front end's FEMALE pitch contour: the units are a woman's
        # voice, and dragging them to the male contour's ~118Hz lowered
        # them nearly an octave.
        # The engine's own source is part of what is measured: the
        # ledger keyed only the C code and the inventory, so a change to
        # the Python synthesis could have been served a stale score.
        e = {"DIPHONE": inv, "DIPHONE_HASH": ledgerlib.file_hash(os.path.join(inv, "units.pcm")),
             "DIPHONE_ENGINE": ledgerlib.file_hash(os.path.join(os.path.dirname(__file__), "diphone.py")),
             "ZTTS_VOICE": "female"}
        e.update(extra)
        return e

    # The C engine speaks from a pack carrying the CURRENT inventory as a
    # DIPHONE section, built here so it cannot drift from the prototype's.
    from . import diphone as diphonelib
    rec_pack = os.path.join(out_dir, "recorded.zspk")
    order = secs["PHONES"].decode("ascii").split()
    blob = diphonelib.serialise(inv_dir, order)
    keep = HELD + ["LTS"]
    packlib.write(rec_pack, [(k, secs[k]) for k in score.ORDER if k in secs and k in keep]
                  + [("DIPHONE", blob)])

    out = [("formant, male (shipped)", base, {}, "the voice today"),
           ("diphone, Python prototype", base, dv(inv_dir),
            "the current inventory, joined by the prototype"),
           ("diphone, C engine (device)", rec_pack, {"ZTTS_VOICE": "recorded"},
            "the same inventory as a pack section, joined by sw/apps/tts/dsyn.c")]
    return out


def _parse_formants(blob):
    import struct
    rows = {}
    if len(blob) < 4:
        return rows
    _, n = struct.unpack("<HH", blob[:4])
    p = 4
    for _ in range(n):
        nm = blob[p:p + 2].rstrip(b"\0").decode("ascii")
        rows[nm] = list(struct.unpack("<6H", blob[p + 2:p + 14]))
        p += 14
    return rows


def measure(name, pack_path, env, items, which, material, rec, lexicon, renderer,
            led, code, out_dir, log=print):
    """Per-item results for one variant, from the ledger if already measured."""
    key = dict(code=code, pack=ledger.file_hash(pack_path), env=" ".join(
        "%s=%s" % kv for kv in sorted((env or {}).items())),
        material=material["hash"], asr=rec.label, set=which)
    old = led.find(**key)
    if old:
        log("  %-26s (from the ledger)" % name)
        return old["items"]
    log("  %-26s rendering and recognising %d items" % (name, len(items)))
    wavs = score.render(renderer, items, os.path.join(out_dir, "render"), pack_path, env)
    per = score.judge(items, rec.transcribe(wavs), lexicon)
    rec_ = dict(key)
    rec_.update(name=name, metrics=score.metrics(per), items=per)
    led.add(rec_)
    return per


def measure_recordings(material, rec, lexicon, led, log=print):
    """The anchor: the corpus's own recordings of the ordinary sentences."""
    ords = material.get("ordinary", [])
    if not ords:
        return None
    key = dict(code="-", pack="recordings", env="", material=material["hash"],
               asr=rec.label, set="ordinary")
    old = led.find(**key)
    if old:
        log("  %-26s (from the ledger)" % "real recordings")
        return old["items"]
    wavs = [os.path.join(material["corpus"], "wavs", o["id"] + ".wav") for o in ords]
    if not all(os.path.exists(w) for w in wavs):
        return None
    log("  %-26s recognising %d recordings" % ("real recordings", len(wavs)))
    per = score.judge([("ord", o["text"], None) for o in ords], rec.transcribe(wavs), lexicon)
    rec_ = dict(key)
    rec_.update(name="real recordings", metrics=score.metrics(per), items=per)
    led.add(rec_)
    return per


def fmt_metric(m, v):
    if v is None:
        return "   -   "
    return "%5.1f%%" % (100 * v)


def fmt_diff(d, metric):
    """A difference, with its interval, and whether it is an improvement."""
    if d is None:
        return ""
    pt, lo, hi = d
    sig = (lo > 0 or hi < 0)
    good = pt > 0                   # every metric is words correctly identified
    mark = (" better" if good else " WORSE") if sig else " (no real difference)"
    return "%+5.1f [%+.1f, %+.1f]%s" % (100 * pt, 100 * lo, 100 * hi, mark)


def target_table(results, lexicon):
    """The targeted confusions, per 100 of the sound, for every variant."""
    t = score.TARGETS
    lines = ["targeted confusions, per 100 of the sound said (lower is better):",
             "%-28s" % "" + "".join("%6s" % ("%s>%s" % p) for p in t)]
    for name, per, _ in results:
        rates = score.target_rates(per, lexicon)
        lines.append("%-28s" % name[:28] + "".join("%6.1f" % rates[p] for p in t))
    return lines


def table(results, anchor, against="previous"):
    """The report: one row per variant, then each step's paired differences."""
    lines = []
    lines.append("%-28s %8s %8s %8s" % ("", "sentence", "isolated", "ordinary"))
    lines.append("%-28s %8s %8s %8s" % ("", "words ok", "words ok", "words ok"))
    for name, per, _ in results:
        m = score.metrics(per)
        lines.append("%-28s %8s %8s %8s" % (name[:28], fmt_metric("sus", m.get("sus")),
                                            fmt_metric("word", m.get("word")),
                                            fmt_metric("ord", m.get("ord"))))
    if anchor:
        m = score.metrics(anchor)
        lines.append("%-28s %8s %8s %8s   <- a human voice" % (
            "real recordings", "", "", fmt_metric("ord", m.get("ord"))))
    lines.append("")
    lines.append("each %s (percentage points, 95%% interval):" % (
        "step against the one before it" if against == "previous" else
        "variant against the first"))
    for i in range(1, len(results)):
        a, b = (results[i - 1][1] if against == "previous" else results[0][1]), results[i][1]
        lines.append("  %s" % results[i][0])
        for m in score.METRICS:
            d = score.paired_ci(a, b, m)
            if d:
                lines.append("    %-9s %s" % ({"sus": "sentence", "word": "isolated",
                                                "ord": "ordinary"}[m], fmt_diff(d, m)))
    if len(results) > 2 and against == "previous":
        lines.append("")
        lines.append("the last against the first:")
        for m in score.METRICS:
            d = score.paired_ci(results[0][1], results[-1][1], m)
            if d:
                lines.append("    %-9s %s" % ({"sus": "sentence", "word": "isolated",
                                                "ord": "ordinary"}[m], fmt_diff(d, m)))
    return lines
