"""
Zeitlos
Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.

Scoring speech: render the test material, recognise it, count the
errors -- per item, so that two versions of the voice can be compared
on the SAME items, with a confidence interval on the difference.

Three metrics, all "words correctly identified" -- higher is better
(lib/evalset.py for the material):

  sus   over semantically unpredictable sentences: every word has to be
        heard, none can be guessed.
  word  minimal-pair words, each in a carrier phrase ("Would you write
        ___ now."), scoring only that word.
  ord   over ordinary sentences from the corpus -- closest to real use.
        The corpus's own recordings, recognised the same way, are the
        anchor: what a human voice scores.

WORDS CORRECTLY IDENTIFIED, not word error rate: the first scores with
WER were dominated by the recogniser looping ("wiggly-wiggly-wiggly..."
for 113 invented words on a six-word sentence) -- one sentence could be
a fifth of a variant's errors, and which sentences looped flipped at
random between variants. Counting the reference words that were heard
correctly is immune to invented words, and took the intervals from
+/-10-15 points to +/-1-2. It is also what intelligibility tests of
synthetic speech have long used.

The carrier phrase is for the same reason: a clip of one short word
alone made the recogniser spell ("S I E D"), write digits, or produce a
stock phrase ("please", "see you"), none of it about the voice.

Words are compared by sound, not spelling (asr.canonical).
"""

import os
import random
import subprocess

from . import asr

ORDER = ["MANIFEST", "PHONES", "LTS", "PROSODY", "FORMANTS", "LEXIDX", "LEXDAT"]
METRICS = ("sus", "word", "ord")


def _clean(t):
    return " ".join(t.replace("|", " ").split())


def items_of(material, which):
    """
    [(kind, text to speak, target word or None)] for "dev", "test",
    "ordinary" or "all". Isolated words are spoken in the material's
    carrier phrase.
    """
    out = []
    carrier = material.get("carrier", "%s")
    parts = ["dev", "test", "ordinary"] if which == "all" else [which]
    for p in parts:
        if p == "ordinary":
            # The clip id rides along: copy synthesis needs the recording.
            out += [("ord", o["text"], o["id"]) for o in material.get("ordinary", [])]
        else:
            out += [("sus", s, None) for s in material[p]["sus"]]
            out += [("word", carrier % w, w) for w in material[p]["words"]]
    return out


def render(renderer, items, out_dir, pack_path, env=None):
    """Renders [(kind, text)]; returns the wav paths, in order."""
    os.makedirs(out_dir, exist_ok=True)
    listing = os.path.join(out_dir, "set.txt")
    wavs = []
    with open(listing, "w") as f:
        for i, (kind, text, _) in enumerate(items):
            f.write("t_%04d|%s|\n" % (i, _clean(text)))
            wavs.append(os.path.join(out_dir, "t_%04d.wav" % i))
    e = dict(os.environ)
    for k in ("ZTTS_MARKS", "ZTTS_OPEN", "ZTTS_TILT", "ZTTS_PITCH", "ZTTS_FORMANTS",
              "ZTTS_EXPRESSION", "ZTTS_VOICE", "ZTTS_EXP"):
        e.pop(k, None)
    e.update(env or {})
    # A diphone variant: our front end still decides the phones, their
    # timing and the pitch (it writes them beside each wav), and the
    # diphone prototype replaces the audio (lib/diphone.py).
    dip = e.pop("DIPHONE", None)
    e.pop("DIPHONE_HASH", None)
    e.pop("DIPHONE_ENGINE", None)
    mode = e.pop("DIPHONE_MODE", "")
    align_dir = e.pop("DIPHONE_ALIGN", "")
    corpus_dir = e.pop("DIPHONE_CORPUS", "")
    if dip:
        e["ZTTS_MARKS"] = "1"
    subprocess.check_call([renderer, listing, out_dir, "180", pack_path or ""],
                          env=e, stdout=subprocess.DEVNULL)
    if dip:
        from . import diphone
        if _inv.get(dip) is None:
            _inv[dip] = diphone.Inventory(dip)
        copies = None
        if mode == "copy":
            copies = {i: (os.path.join(align_dir, it[2] + ".phones"),
                          os.path.join(corpus_dir, "wavs", it[2] + ".wav"))
                      for i, it in enumerate(items) if it[0] == "ord" and it[2]}
        diphone.render_items(_inv[dip], out_dir, len(items), mode=mode, copies=copies)
    return wavs


_inv = {}


def _hits(ref, hyp, lexicon):
    """Reference words heard correctly, and how many there were, by sound."""
    r = asr.canonical(asr.normalise(ref), lexicon)
    h = asr.canonical(asr.normalise(hyp), lexicon)
    pairs = asr.align_words(r, h)
    return sum(1 for a, b in pairs if a is not None and a == b), len(r), pairs, r


def judge(items, hyps, lexicon):
    """
    Per item: {"kind", "ref", "target", "hyp", "ok", "n"}: reference words
    heard correctly, of how many. For a carrier-phrase word, only the
    target counts: 1 or 0 of 1.
    """
    out = []
    for (kind, ref, target), hyp in zip(items, hyps):
        ok, n, pairs, r = _hits(ref, hyp, lexicon)
        if kind == "word":
            want = asr.canonical([target], lexicon)[0]
            ok, n = int(any(a == want and b == want for a, b in pairs)), 1
        out.append({"kind": kind, "ref": ref, "target": target, "hyp": hyp.strip(),
                    "ok": ok, "n": n})
    return out


def metrics(per_item):
    """{"sus", "word", "ord"}: share of words correctly identified."""
    out = {}
    for m in METRICS:
        rows = [r for r in per_item if r["kind"] == m]
        if rows:
            out[m] = sum(r["ok"] for r in rows) / float(sum(r["n"] for r in rows))
    return out


def objective(per_item):
    """For the tuner: lower is better. Sentence and word misses, 0..2."""
    m = metrics(per_item)
    return (1.0 - m.get("sus", 1.0)) + (1.0 - m.get("word", 1.0))


def paired_ci(a, b, metric, rounds=2000, seed=1):
    """
    The difference b - a in one metric over the same items, with a 95%
    interval from resampling the items. Paired: each resample takes the
    same items from both, so variation between items -- some sentences
    are simply harder -- cancels out.
    """
    rows = [(x, y) for x, y in zip(a, b) if x["kind"] == metric]
    if not rows:
        return None
    rng = random.Random(seed)

    def value(sample, k):
        n = sum(p[k]["n"] for p in sample)
        return sum(p[k]["ok"] for p in sample) / float(n) if n else 0.0

    pairs = [(x, y) for x, y in rows]
    point = value(pairs, 1) - value(pairs, 0)
    diffs = []
    for _ in range(rounds):
        s = [pairs[rng.randrange(len(pairs))] for _ in range(len(pairs))]
        diffs.append(value(s, 1) - value(s, 0))
    diffs.sort()
    return point, diffs[int(0.025 * rounds)], diffs[int(0.975 * rounds)]


# The confusions sessions 1 and 1b are aimed at, reported as a rate per
# hundred of the sound at stake, for every variant.
TARGETS = [("T", "D"), ("P", "B"), ("K", "T"), ("K", "P"), ("K", "D"), ("G", "D"),
           ("N", "L"), ("M", "L"), ("M", "N"), ("N", "T")]


def _scored_pairs(r):
    """
    The aligned (said, heard) word pairs that are SCORED. For a carrier-
    phrase item that is the target word alone: counting the carrier too
    put "Would you write ... now" into every word item's confusions, and
    ~500 copies of "you" heard as "me", "be", "thee" swamped the table
    (UW->IY x81, Y->M x48) in the first run with it.
    """
    pairs = asr.align_words(asr.normalise(r["ref"]), asr.normalise(r["hyp"]))
    if r["kind"] == "word" and r.get("target"):
        t = r["target"].lower()
        pairs = [p for p in pairs if p[0] == t][:1]
    return pairs


def target_rates(per_item, lexicon):
    """{(said, heard): per 100 of `said` in the scored words}."""
    conf, stake = {}, {}
    for r in per_item:
        pairs = _scored_pairs(r)
        asr.phone_confusions(pairs, lexicon, conf)
        ref_words = [a for a, _ in pairs if a]
        for w in ref_words:
            for p in lexicon.get(w, ()):
                ph = p.rstrip("012")
                stake[ph] = stake.get(ph, 0) + 1
    return {(a, b): 100.0 * conf.get((a, b), 0) / stake[a] if stake.get(a) else 0.0
            for a, b in TARGETS}


def confusions(per_item, lexicon):
    counts = {}
    for r in per_item:
        asr.phone_confusions(_scored_pairs(r), lexicon, counts)
    return sorted(counts.items(), key=lambda kv: -kv[1])
