"""
Zeitlos
Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.

Recognising what the voice said, and counting what went wrong.

Two recognisers behind one interface:

  whisper  faster-whisper on a GPU (pip install faster-whisper). The
           one to optimise against: close to human on clear speech.
           Run with no prompt, no conditioning on earlier text, and
           greedy decoding, so each clip is judged on its own sound.
  sphinx   pocketsphinx on the CPU. Much weaker; here so the loop can be
           tested anywhere, and as a second opinion -- speech that only
           ONE recogniser likes is suspect.

Scores are word error rate over the unpredictable sentences and word
accuracy over the isolated words, plus -- the part that says what to
fix -- which phonemes were heard as which, from aligning the
lexicon's pronunciations of what was said against what was heard.
"""

import os
import wave

import numpy as np


def normalise(text):
    out = []
    for w in text.lower().replace("-", " ").split():
        w = "".join(c for c in w if c.isalpha() or c == "'")
        w = w.strip("'")
        if w:
            out.append(w)
    return out


def canonical(words, lexicon):
    """
    Words replaced by their pronunciations where the lexicon knows
    them, so homophones compare equal: "know" and "no", "new" and
    "knew", "right" and "write" are the same SOUND, and the voice is
    being scored on sound -- a recogniser choosing another spelling is
    not a mistake the voice made.
    """
    out = []
    for w in words:
        ph = lexicon.get(w) if lexicon else None
        out.append(" ".join(p.rstrip("012") for p in ph) if ph else w)
    return out


def align_words(ref, hyp):
    """Levenshtein alignment: [(ref word or None, hyp word or None)]."""
    n, m = len(ref), len(hyp)
    d = [[0] * (m + 1) for _ in range(n + 1)]
    for i in range(n + 1):
        d[i][0] = i
    for j in range(m + 1):
        d[0][j] = j
    for i in range(1, n + 1):
        for j in range(1, m + 1):
            d[i][j] = min(d[i - 1][j] + 1, d[i][j - 1] + 1,
                          d[i - 1][j - 1] + (ref[i - 1] != hyp[j - 1]))
    out, i, j = [], n, m
    while i > 0 or j > 0:
        if i > 0 and j > 0 and d[i][j] == d[i - 1][j - 1] + (ref[i - 1] != hyp[j - 1]):
            out.append((ref[i - 1], hyp[j - 1]))
            i, j = i - 1, j - 1
        elif i > 0 and d[i][j] == d[i - 1][j] + 1:
            out.append((ref[i - 1], None))
            i -= 1
        else:
            out.append((None, hyp[j - 1]))
            j -= 1
    return out[::-1]


def wer(ref, hyp):
    pairs = align_words(ref, hyp)
    errors = sum(1 for a, b in pairs if a != b)
    return errors, len(ref)


def phone_confusions(pairs, lexicon, counts):
    """
    For each substituted word both of whose pronunciations are known
    and the same length, which phonemes changed. Adds to `counts`:
    {(said, heard): n}. Whole-word differences of other lengths are
    left out -- they say the word was lost, not which sound.
    """
    for a, b in pairs:
        if not a or not b or a == b:
            continue
        pa, pb = lexicon.get(a), lexicon.get(b)
        if not pa or not pb or len(pa) != len(pb):
            continue
        for x, y in zip(pa, pb):
            x, y = x.rstrip("012"), y.rstrip("012")
            if x != y:
                counts[(x, y)] = counts.get((x, y), 0) + 1


class Whisper:

    name = "whisper"

    def __init__(self, model="medium.en", device="cuda"):
        try:
            from faster_whisper import WhisperModel
        except ImportError:
            raise SystemExit("speech: whisper needs `pip install faster-whisper`")
        compute = "float16" if device == "cuda" else "int8"
        self.model = WhisperModel(model, device=device, compute_type=compute)
        self.label = "whisper %s (%s)" % (model, device)

    def transcribe(self, paths):
        out = []
        for p in paths:
            segs, _ = self.model.transcribe(
                p, language="en", beam_size=1, temperature=0.0,
                condition_on_previous_text=False, initial_prompt=None,
                vad_filter=False)
            out.append(" ".join(s.text for s in segs))
        return out


class Sphinx:

    name = "sphinx"

    def __init__(self):
        from pocketsphinx import Decoder, get_model_path
        model = get_model_path()
        self.dec = Decoder(hmm=os.path.join(model, "en-us", "en-us"),
                           lm=os.path.join(model, "en-us", "en-us.lm.bin"),
                           dict=os.path.join(model, "en-us", "cmudict-en-us.dict"),
                           loglevel="FATAL")
        self.label = "pocketsphinx (CPU)"

    @staticmethod
    def _load16k(path):
        w = wave.open(path, "rb")
        rate = w.getframerate()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)
        w.close()
        if rate != 16000:
            t = np.arange(int(len(x) * 16000 / rate)) * rate / 16000.0
            x = np.interp(t, np.arange(len(x)), x)
        return x.astype("<i2").tobytes()

    def transcribe(self, paths):
        out = []
        for p in paths:
            self.dec.start_utt()
            self.dec.process_raw(self._load16k(p), full_utt=True)
            self.dec.end_utt()
            hyp = self.dec.hyp()
            out.append(hyp.hypstr if hyp else "")
        return out


def recogniser(kind, model=None, device=None):
    if kind == "whisper":
        return Whisper(model or "medium.en", device or "cuda")
    if kind == "sphinx":
        return Sphinx()
    raise SystemExit("speech: unknown recogniser %r (whisper, sphinx)" % kind)
