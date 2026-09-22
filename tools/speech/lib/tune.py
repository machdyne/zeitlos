"""
Zeitlos
Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.

Tuning the voice against a recogniser, unattended.

The loop: change one parameter a step, render the dev material with it,
recognise it, keep the change if the score improved; when a full pass
over every parameter improves nothing, halve the steps; stop when the
budget is spent. Every evaluation is logged, so an overnight run can be
read -- or resumed -- afterwards.

What it may change is deliberately limited to what a speech pack
already carries, each within a range that is still a voice: timing,
pitch movement, the voice source, and the overall length of vowels and
consonants. Nothing it can reach can turn the voice into something a
recogniser likes and a person would not recognise as speech -- and the
number reported at the end is on the held-out TEST material, which the
search never saw.

Coordinate search rather than anything cleverer, because each
evaluation is a full render-and-recognise, the objective is noisy in
small steps, and a method that is easy to reason about is worth more
here than a few percent of efficiency.
"""

import json
import os
import random
import time

from . import pack as packlib
from . import prosody, score

# name, low, high, first step. Ranges are narrower than the device's
# clamps: these are the region worth searching, not merely the legal one.
SPACE = [
    ("unstressed", 35, 80, 5),
    ("final_comma", 100, 200, 10),
    ("final_stop", 100, 220, 10),
    ("pause_comma", 100, 400, 30),
    ("pause_stop", 150, 600, 40),
    ("top", 100, 130, 3),
    ("decl", 0, 40, 4),
    ("stress1", 0, 30, 3),
    ("stress2", 0, 20, 3),
    ("comma_rise", 0, 25, 3),
    ("stop_fall", 0, 25, 3),
    ("open", 35, 80, 5),
    ("tilt", 0, 24, 3),
    ("vowel_len", 70, 140, 8),        # % of every vowel's duration
    ("cons_len", 70, 140, 8),         # % of every consonant's duration
]


def start_point(base_pack, table_durs):
    """The parameters of `base_pack`, as a flat dict for the search."""
    secs = packlib.read(base_pack)
    fields, durs = prosody.parse(secs.get("PROSODY", b""))
    x = {k: fields.get(k, prosody.DEFAULTS.get(k)) for k, *_ in SPACE if k in prosody.DEFAULTS}
    x["vowel_len"] = 100
    x["cons_len"] = 100
    full = dict(table_durs)
    full.update(durs)
    return x, fields, full


def make_pack(base_pack, out_path, x, fields, durs):
    """A copy of `base_pack` with x's values in its PROSODY section."""
    secs = packlib.read(base_pack)
    f = dict(fields)
    for k, v in x.items():
        if k in f:
            f[k] = int(round(v))
    d = {}
    for ph, ms in durs.items():
        scale = x["vowel_len"] if ph[:1] in "AEIOU" else x["cons_len"]
        d[ph] = max(15, min(400, int(round(ms * scale / 100.0))))
    secs["PROSODY"] = prosody.serialise(f, d)
    packlib.write(out_path, [(k, secs[k]) for k in score.ORDER if k in secs])
    return out_path


def run(renderer, base_pack, material, rec, lexicon, table_durs, out_dir,
        budget=150, seed=1, log=print, tag=None):
    """
    `tag` identifies what is being tuned -- code, base pack, material,
    recogniser. Evaluations logged under the same tag are reused, so a
    cancelled run resumes where it stopped, from the best point found.
    """
    os.makedirs(out_dir, exist_ok=True)
    rng = random.Random(seed)
    x, fields, durs = start_point(base_pack, table_durs)
    steps = {k: st for k, lo, hi, st in SPACE}
    bounds = {k: (lo, hi) for k, lo, hi, st in SPACE}
    logpath = os.path.join(out_dir, "log.jsonl")
    tag = list(tag or [])
    cache = {}
    evals = [0]

    def key_of(cand, which):
        return (which, tuple(sorted((k, int(round(v))) for k, v in cand.items())))

    # Resume: everything already measured under this tag.
    resumed = 0
    best_logged = None
    if os.path.exists(logpath):
        for line in open(logpath):
            try:
                e = json.loads(line)
            except ValueError:
                continue
            if e.get("tag") != tag or e.get("set") != "dev":
                continue
            cache[key_of(e["x"], "dev")] = {"objective": e["objective"]}
            resumed += 1
            if best_logged is None or e["objective"] < best_logged[1]:
                best_logged = (e["x"], e["objective"])
    logf = open(logpath, "a")

    def evaluate(cand, which="dev"):
        k = key_of(cand, which)
        if k in cache and (which == "dev"):
            return cache[k]
        p = make_pack(base_pack, os.path.join(out_dir, "cand.zspk"), cand, fields, durs)
        items = score.items_of(material, which)
        wavs = score.render(renderer, items, os.path.join(out_dir, "render"), p)
        per = score.judge(items, rec.transcribe(wavs), lexicon)
        s = {"objective": score.objective(per), "items": per}
        cache[k] = s
        if which == "dev":
            evals[0] += 1
        logf.write(json.dumps({"t": time.time(), "tag": tag, "set": which, "x": cand,
                               "objective": s["objective"]}) + "\n")
        logf.flush()
        return s

    start = dict(x)
    if best_logged:
        x = {k: best_logged[0].get(k, v) for k, v in x.items()}
        best = {"objective": best_logged[1]}
        log("  resuming: %d evaluations in the log, best objective %.3f" % (resumed, best["objective"]))
    else:
        best = evaluate(x)
        log("  start: objective %.3f" % best["objective"])

    while evals[0] + resumed < budget:
        improved = False
        order = [k for k, *_ in SPACE]
        rng.shuffle(order)
        for k in order:
            if evals[0] + resumed >= budget:
                break
            for sign in (+1, -1):
                lo, hi = bounds[k]
                v = min(hi, max(lo, x[k] + sign * steps[k]))
                if v == x[k]:
                    continue
                cand = dict(x)
                cand[k] = v
                s = evaluate(cand)
                # A change must beat the current score by a margin: at a
                # few hundred words, one word is ~0.3%, and chasing
                # single-word flukes walks the voice around at random.
                if s["objective"] < best["objective"] - 0.004:
                    log("  %-11s %4d -> %4d   objective %.3f -> %.3f"
                        % (k, x[k], v, best["objective"], s["objective"]))
                    x, best, improved = cand, s, True
                    break
        if not improved:
            if all(steps[k] <= 1 for k in steps):
                log("  converged")
                break
            for k in steps:
                steps[k] = max(1, steps[k] // 2)
            log("  no single step helps; halving the steps")

    # The held-out verdict, per item so it can be compared pairwise.
    t0 = evaluate(start, "test")
    t1 = evaluate(x, "test")
    final = make_pack(base_pack, os.path.join(out_dir, "best.zspk"), x, fields, durs)
    logf.close()
    return {"start": start, "best": x, "test_start": t0["items"], "test_best": t1["items"],
            "pack": final, "evals": evals[0], "resumed": resumed}
