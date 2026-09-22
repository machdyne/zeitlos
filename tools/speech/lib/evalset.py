"""
Zeitlos
Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.

Test material for measuring intelligibility by machine.

A modern recogniser is too good at guessing: give it an ordinary
sentence and its language model fills in words the voice never made
clear, which hides exactly what is being measured. So two kinds of
material that leave it nothing to guess from:

  SEMANTICALLY UNPREDICTABLE SENTENCES -- grammatical, made of common
  words, and meaningless: "The green law eats a slow door." Every word
  has to be heard; none can be inferred. The standard material for
  testing synthetic speech (Benoit et al., 1996 -- the method, not any
  published list: these are generated here).

  MINIMAL PAIRS -- single common words that differ from another common
  word in one sound: "bat"/"pat", "seat"/"sheet". Spoken alone, only
  the sound decides. A wrong answer here says WHICH sound failed.

Both are generated from our own lexicon, deterministically from a
seed, and split into a DEV set the tuner optimises against and a TEST
set it never sees -- the score that is reported is the held-out one, so
an improvement that only fooled the recogniser on the dev sentences
shows up as none.
"""

import os
import random

from . import moby

# Enough common words to build sentences from when no corpus is at hand
# (the sandbox has none). With a corpus, word frequency comes from its
# transcripts instead.
FALLBACK_COMMON = """
time year people way day man thing woman life child world school state family
student group country problem hand part place case week company system program
question work government number night point home water room mother area money
story fact month lot right study book eye job word business issue side kind head
house service friend father power hour game line end member law car city name
president team minute idea body information back parent face others level office
door health person art war history party result change morning reason research
girl guy moment air teacher force education foot boy age policy music bed street
table dog cat bird tree road river hill ship train horse field window garden
wall floor chair paper letter box bag hat coat shoe bread milk salt stone glass
good new first last long great little own other old right big high different
small large next early young important few public bad same able red green blue
black white brown dark cold warm hot soft hard slow fast quiet loud clean dry wet
sharp round thin thick short tall deep bright heavy light empty full rich poor
take make know see come think look want give use find tell ask work seem feel
try leave call keep hold bring begin show hear play run move live believe
write sit stand lose pay meet include continue set learn lead understand watch
follow stop create speak read spend grow open walk win offer remember love
consider appear buy wait serve die send build stay fall cut reach kill raise pass
sell decide return explain hope carry break drive eat drink sing climb throw
catch paint wash push pull fill burn cook cross dig draw fly hide jump kick
""".split()

NOUN_POS, VERB_POS, ADJ_POS = "N", "tV", "A"


def _pos_table(path):
    """Moby part-of-speech: word -> set of codes. Latin-1, CR lines."""
    out = {}
    if not os.path.exists(path):
        return out
    data = open(path, "rb").read().decode("latin-1")
    for line in data.replace("\r", "\n").split("\n"):
        if "\u00d7" in line:
            w, codes = line.split("\u00d7", 1)
        elif "\xd7" in line:
            w, codes = line.split("\xd7", 1)
        else:
            continue
        out[w.strip().lower()] = codes.strip()
    return out


NAMES = set()      # words the corpus writes capitalised mid-sentence: names


def word_frequencies(corpus_dir):
    """
    Word counts from a corpus's transcripts, when there is one. Also
    notes which words are NAMES -- capitalised mid-sentence most of the
    time -- because "the craig drinks" is not a test of the voice.
    """
    meta = os.path.join(corpus_dir or "", "metadata.csv")
    counts, caps = {}, {}
    if not os.path.exists(meta):
        return counts
    for line in open(meta, encoding="utf-8"):
        parts = line.rstrip("\n").split("|")
        text = parts[2] if len(parts) > 2 and parts[2] else (parts[1] if len(parts) > 1 else "")
        for i, raw in enumerate(text.split()):
            w = "".join(c for c in raw if c.isalpha())
            if not w:
                continue
            lw = w.lower()
            counts[lw] = counts.get(lw, 0) + 1
            if i > 0 and w[0].isupper():
                caps[lw] = caps.get(lw, 0) + 1
    NAMES.clear()
    NAMES.update(w for w, n in caps.items() if n > counts[w] / 2)
    return counts


def third_person(verb):
    """He ___s: tries, washes, goes, sees."""
    if verb.endswith("y") and len(verb) > 2 and verb[-2] not in "aeiou":
        return verb[:-1] + "ies"
    if verb.endswith(("s", "sh", "ch", "x", "z", "o")):
        return verb + "es"
    return verb + "s"


# Words that look like base verbs to a part-of-speech list but are not
# ones "will ___" and "he ___s" work with.
NOT_BASE = set("""
might must shall should would could may can will brought wrote made rode
went came became began gave took saw said told found thought knew grew
threw drew flew spoke broke chose froze stole wore tore swore bore drove
rose arose fell held kept left lost meant met paid sent spent stood sold
built bought caught taught fought sought felt dealt heard led fed read
""".split())

NUMBERS = set("""
one two three four five six seven eight nine ten eleven twelve thirteen
fourteen fifteen sixteen seventeen eighteen nineteen twenty thirty forty
fifty sixty seventy eighty ninety hundred thousand million first second
third fourth fifth
""".split())


def _classify(words, lexicon, pos, freq):
    """Nouns, verbs and adjectives among `words`, by primary part of speech."""
    nouns, verbs, adjs = [], [], []
    for w in words:
        if not (3 <= len(w) <= 8) or w not in lexicon:
            continue
        if w in NAMES or w in NUMBERS or w.endswith("ing"):
            continue
        # Moby lists a word's parts of speech most likely first; the
        # first is the one a listener will assume.
        first = pos.get(w, "")[:1]
        if first == "N":
            nouns.append(w)
        elif first in ("V", "t", "i"):
            # A base form only, and -- with a corpus -- only if it
            # actually uses its "he ___s" form: that is what rules out
            # "broughts", "mights" and "trys" without a list of every
            # irregular verb.
            if w in NOT_BASE or w.endswith("ed"):
                continue
            if freq and third_person(w) not in freq:
                continue
            verbs.append(w)
        elif first == "A":
            adjs.append(w)
    return nouns, verbs, adjs


def vocabulary(lexicon, pos, freq, top=4000):
    """
    Common, unambiguous nouns, verbs and adjectives that the lexicon can
    pronounce: the only words fair to test with. Rare words fail for
    reasons that have nothing to do with the voice.
    """
    if not pos:
        # No part-of-speech data: the built-in list's own ordering.
        return ([w for w in FALLBACK_COMMON[:200] if w in lexicon],
                [w for w in FALLBACK_COMMON[330:] if w in lexicon],
                [w for w in FALLBACK_COMMON[200:330] if w in lexicon])
    common = ([w for w, _ in sorted(freq.items(), key=lambda kv: -kv[1])[:top]]
              if freq else FALLBACK_COMMON)
    nouns, verbs, adjs = _classify(common, lexicon, pos, freq)
    # A thin corpus can leave a category nearly empty -- no verb whose
    # "he ___s" form it happens to use, say. Top a short one up from the
    # built-in list, classified the same way, rather than fail or build
    # sentences from five words.
    fn, fv, fa = _classify(FALLBACK_COMMON, lexicon, pos, None)
    for have, extra in ((nouns, fn), (verbs, fv), (adjs, fa)):
        if len(have) < 20:
            have.extend(w for w in extra if w not in have)
    return nouns, verbs, adjs


TEMPLATES = [
    "the {a} {n} {vs} the {n2}.",
    "the {n} {vs} {an} {a} {n2}.",
    "{v} the {n} and the {a} {n2}.",
    "{An} {a} {n} will {v} the {n2}.",
    "the {n2} {vs} near the {a} {n}.",
]


def _article(word, lexicon):
    """'a' or 'an', by the word's first SOUND ("an hour", "a union")."""
    ph = lexicon.get(word) if lexicon else None
    first = ph[0] if ph else word[:1].upper()
    return "an" if first[:1] in "AEIOU" else "a"


def sus(n, seed, nouns, verbs, adjs, lexicon=None):
    """`n` semantically unpredictable sentences, reproducibly."""
    rng = random.Random(seed)
    out = []
    for _ in range(n):
        t = rng.choice(TEMPLATES)
        a, v = rng.choice(adjs), rng.choice(verbs)
        art = _article(a, lexicon)
        s = t.format(a=a, n=rng.choice(nouns), n2=rng.choice(nouns), v=v,
                     vs=third_person(v), an=art, An=art)
        out.append(s[0].upper() + s[1:])
    return out


def minimal_pairs(lexicon, words, limit, seed):
    """
    Words from `words` that differ from another word in `words` by
    exactly one phoneme (stress ignored). Returns [(word, other, index
    of the differing phoneme)].
    """
    rng = random.Random(seed)
    by_len = {}
    strip = {w: tuple(p.rstrip("012") for p in lexicon[w]) for w in words if w in lexicon}
    for w, ph in strip.items():
        by_len.setdefault(len(ph), []).append((w, ph))
    pairs = []
    for group in by_len.values():
        for i in range(len(group)):
            for j in range(i + 1, len(group)):
                a, b = group[i][1], group[j][1]
                diff = [k for k in range(len(a)) if a[k] != b[k]]
                if len(diff) == 1:
                    pairs.append((group[i][0], group[j][0], diff[0]))
    rng.shuffle(pairs)
    return pairs[:limit]


def build(lexicon_path, pos_path, corpus_dir, seed=7, n_sus=120, n_words=80):
    """
    The dev and test sets: {"dev": {"sus": [...], "words": [...]},
    "test": {...}}. Dev and test are disjoint by construction.
    """
    lexicon = dict(moby.read(lexicon_path))
    pos = _pos_table(pos_path)
    freq = word_frequencies(corpus_dir)
    nouns, verbs, adjs = vocabulary(lexicon, pos, freq)
    sentences = sus(2 * n_sus, seed, nouns, verbs, adjs, lexicon)
    common = [w for w in (sorted(freq, key=lambda w: -freq[w])[:3000] if freq else FALLBACK_COMMON)
              if w.isalpha() and 3 <= len(w) <= 7 and w not in NAMES and w not in NUMBERS]
    pairs = minimal_pairs(lexicon, common, 2 * n_words, seed)
    words = []
    for a, b, _ in pairs:
        for w in (a, b):
            if w not in words:
                words.append(w)
    words = words[:2 * n_words]
    half_s, half_w = len(sentences) // 2, len(words) // 2
    return {
        "dev": {"sus": sentences[:half_s], "words": words[:half_w]},
        "test": {"sus": sentences[half_s:], "words": words[half_w:]},
        "vocab": {"nouns": len(nouns), "verbs": len(verbs), "adjs": len(adjs)},
    }, lexicon


# -- the frozen test set --
#
# Generated once, saved, and never regenerated behind anyone's back: a
# score only means something next to others made with the SAME
# material. Changing the generator means a new version, and old and new
# scores are not compared.
VERSION = "v3"

# Isolated words are spoken in this frame, and only the word is scored:
# alone, a short clip made the recogniser spell, write digits or produce
# a stock phrase. The frame is the Modified Rhyme Test's, a long-standing
# choice for exactly this: neutral, and no help in guessing the word.
CARRIER = "Would you write %s now."


def ordinary(corpus_dir, n=150, seed=7):
    """
    Ordinary sentences, with the real recording of each: the corpus's own
    transcripts. Our voice reads them for the third metric -- closest to
    real use, reading a book -- and the recordings, recognised the same
    way, are the anchor: what a human voice scores on the same test.
    Chosen evenly through the corpus, deterministically.
    """
    meta = os.path.join(corpus_dir or "", "metadata.csv")
    if not os.path.exists(meta):
        return []
    rows = []
    for line in open(meta, encoding="utf-8"):
        parts = line.rstrip("\n").split("|")
        if len(parts) < 3 or not parts[2]:
            continue
        wav = os.path.join(corpus_dir, "wavs", parts[0] + ".wav")
        if os.path.exists(wav) and 4 <= len(parts[2].split()) <= 20:
            rows.append((parts[0], parts[2]))
    if not rows:
        return []
    step = max(1, len(rows) // n)
    return [{"id": cid, "text": text} for cid, text in rows[seed % step::step][:n]]


def load_or_build(path, lexicon_path, pos_path, corpus_dir, n_sus=120, n_words=300, n_ord=150):
    """
    The material at `path`, building and saving it the first time. Also
    returns the lexicon, which scoring needs for homophones.
    """
    import hashlib
    import json
    lexicon = dict(moby.read(lexicon_path))
    if os.path.exists(path):
        m = json.load(open(path))
        if m.get("version") != VERSION:
            raise SystemExit("speech: %s is material version %s, this tool makes %s -- "
                             "scores across versions are not comparable; move it aside to "
                             "start a new series" % (path, m.get("version"), VERSION))
        return m, lexicon
    sets, _ = build(lexicon_path, pos_path, corpus_dir, n_sus=n_sus, n_words=n_words)
    m = {"version": VERSION, "dev": sets["dev"], "test": sets["test"],
         "ordinary": ordinary(corpus_dir, n_ord), "corpus": corpus_dir or "",
         "carrier": CARRIER}
    blob = json.dumps({k: m[k] for k in ("dev", "test", "ordinary", "carrier")}, sort_keys=True)
    m["hash"] = hashlib.sha256(blob.encode()).hexdigest()[:12]
    os.makedirs(os.path.dirname(path), exist_ok=True)
    json.dump(m, open(path, "w"), indent=1)
    return m, lexicon
