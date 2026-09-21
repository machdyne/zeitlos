#!/usr/bin/env python3
#
# Zeitlos
# Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
#
# How often do the letter-to-sound rules get a word right?
#
#     cd sw/apps/tts && make lts_eval
#
# Scores lts.c against CMUdict, which ships inside the pocketsphinx pip
# wheel, on a fixed random sample of ordinary words. CMUdict is the
# ANSWER KEY here and nothing more: no entry from it is copied into
# the rules or the exception dictionary (docs/tts.md, "Clean-room
# provenance"). Stress is ignored, and the schwa is counted as AH/IH as
# CMUdict writes it.

import os, random, re, subprocess, sys
from pocketsphinx import get_model_path

HERE = os.path.dirname(os.path.abspath(__file__))
dic = {}
for line in open(os.path.join(get_model_path(), "en-us", "cmudict-en-us.dict")):
    parts = line.split()
    w = parts[0]
    if "(" in w or not re.fullmatch(r"[a-z]{3,10}", w):
        continue
    dic.setdefault(w, []).append(" ".join(re.sub(r"\d", "", p) for p in parts[1:]).upper())

# Ordinary words, not surnames: CMUdict is mostly names, and UI text is
# not. The recogniser's language model vocabulary is a list of words
# people actually say; keep only dictionary words that appear in it.
lm = open(os.path.join(get_model_path(), "en-us", "en-us.lm.bin"), "rb").read()
common = set(m.decode() for m in re.findall(rb"(?<![a-z'])([a-z]{3,10})(?![a-z'])", lm))
pool = sorted(w for w in dic if w in common) if "--all" not in sys.argv else sorted(dic)

random.seed(7)
n = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 2000
sample = random.sample(pool, min(n, len(pool)))
print("lts_eval: sampling %d of %d %s words" % (len(sample), len(pool),
      "dictionary" if "--all" in sys.argv else "common"))

out = subprocess.run(["/tmp/lts_cli"], input="\n".join(sample) + "\n",
                     capture_output=True, text=True).stdout.splitlines()

def norm(ph):
    ph = re.sub(r"\d", "", ph)
    return ph.replace("AX", "AH").replace("IX", "IH").split()

def dist(a, b):
    d = list(range(len(b) + 1))
    for i in range(1, len(a) + 1):
        prev, d[0] = d[0], i
        for j in range(1, len(b) + 1):
            prev, d[j] = d[j], min(d[j] + 1, d[j - 1] + 1, prev + (a[i - 1] != b[j - 1]))
    return d[len(b)]

words = errs = total = 0
bad = []
for line in out:
    w, ph = line.split("\t")
    got = norm(ph)
    best = min((dist(got, r.split()), r) for r in dic[w])
    errs += best[0]
    total += len(best[1].split())
    if best[0] == 0:
        words += 1
    else:
        bad.append((w, " ".join(got), best[1]))

print("lts_eval: %d/%d words exactly right (%.0f%%), phoneme error rate %.1f%%" %
      (words, len(sample), 100.0 * words / len(sample), 100.0 * errs / total))
if "-v" in sys.argv:
    for b in bad[:60]:
        print("  %-12s got %-24s want %s" % b)
