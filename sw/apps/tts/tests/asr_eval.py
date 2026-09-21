#!/usr/bin/env python3
#
# Zeitlos
# Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
#
# An automatic stand-in for a listening test:
#
#     cd sw/apps/tts && make wav && python3 tests/asr_eval.py
#
# Runs an off-the-shelf speech recogniser (pocketsphinx, whose English
# model ships inside its pip wheel: pip install pocketsphinx) over the
# single-word WAVs from tests/testset.txt, constrained to that same
# vocabulary -- a closed-set identification test, like a listener
# choosing from a word list. Chance is 1/N.
#
# It EVALUATES the synthesiser; nothing from it goes into the
# synthesiser. A recogniser trained on natural speech is a harsher
# judge of formant speech than a person is, so the number is a floor
# and a comparison between versions, not a verdict. The verdict is
# listening to the files.

import os, sys, wave
import numpy as np
from pocketsphinx import Decoder, get_model_path

HERE = os.path.dirname(os.path.abspath(__file__))
WAVDIR = sys.argv[1] if len(sys.argv) > 1 else "/tmp/tts_wav"

items = []
for line in open(os.path.join(HERE, "testset.txt")):
    line = line.strip()
    if not line or line.startswith("#"): continue
    parts = line.split("|")
    if len(parts) >= 3 and parts[2]:
        items.append((parts[0], parts[2]))

vocab = sorted(set(w for _, w in items))
gram = os.path.join("/tmp", "tts_vocab.gram")
with open(gram, "w") as g:
    g.write("#JSGF V1.0;\ngrammar w;\npublic <w> = " + " | ".join(vocab) + ";\n")

model = get_model_path()
dec = Decoder(hmm=os.path.join(model, "en-us", "en-us"),
              dict=os.path.join(model, "en-us", "cmudict-en-us.dict"),
              jsgf=gram, logfn="/dev/null")

def load16k(path):
    with wave.open(path) as w:
        fs = w.getframerate()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64)
    # pad with silence both sides; the recogniser wants some lead-in
    x = np.concatenate([np.zeros(fs // 5), x, np.zeros(fs // 5)])
    n = int(len(x) * 16000 / fs)
    y = np.interp(np.linspace(0, len(x) - 1, n), np.arange(len(x)), x)
    peak = np.max(np.abs(y)) or 1
    return (y / peak * 20000).astype(np.int16).tobytes()

ok = 0
miss = []
for name, word in items:
    dec.start_utt()
    dec.process_raw(load16k(os.path.join(WAVDIR, name + ".wav")), full_utt=True)
    dec.end_utt()
    hyp = dec.hyp().hypstr if dec.hyp() else ""
    if hyp.strip() == word:
        ok += 1
    else:
        miss.append((word, hyp or "-"))

print("asr_eval: %d/%d recognised (%.0f%%), chance %.0f%%" %
      (ok, len(items), 100.0 * ok / len(items), 100.0 / len(vocab)))
if miss:
    print("asr_eval: missed: " + ", ".join("%s->%s" % m for m in miss))
