"""
Zeitlos
Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.

The Moby Pronunciator, in our phoneme set.

Moby writes a pronunciation as a run of plain letters for the ordinary
consonants, with everything else in slashes:

    A-Bomb      '/eI/,b/A/m
    Aalborg     '/O/lb/O/rg
    lead-burner l/i/d_'b/[@]/rn/@/r

  '  the syllable that follows carries primary stress
  ,  ...secondary stress
  _  word boundary inside a multi-word entry
  /../  a symbol: vowels, diphthongs, and the consonants with no
        single letter (/S/ = "sh", /N/ = "ng", /dZ/ = "j", /tS/ = "ch",
        /T/ = thin, /D/ = this, /Z/ = measure)
  [@] (@)  a schwa that may or may not be pronounced

This module turns that into the ARPAbet-style symbols sw/apps/tts
understands (see phon.c), with a stress digit on every vowel. It
reports what it could not convert rather than guessing: an entry using
a symbol not in the table below is DROPPED, and the counts are printed,
because a silently mangled pronunciation is worse than a missing one --
a missing one falls through to the letter-to-sound rules, which are
right most of the time.
"""

import re

# Moby symbol -> our phonemes. Vowels are marked so the stress pass
# knows where the digits go.
VOWELS = {
    "@":   "AX",        # schwa
    "I":   "IH",
    "i":   "IY",
    "E":   "EH",
    "&":   "AE",
    "A":   "AA",
    "oU":  "OW",
    "eI":  "EY",
    "aI":  "AY",
    "u":   "UW",
    "O":   "AO",
    "U":   "UH",
    "AU":  "AW",
    "Oi":  "OY",
    "OI":  "OY",
    "Ou":  "OW",
    "ju":  "Y UW",
    "y":   "UW",        # the French u; nearest we have
    "R":   "ER",        # r-coloured vowel in some entries
}

CONSONANTS = {
    "S":   "SH",
    "N":   "NG",
    "dZ":  "JH",
    "tS":  "CH",
    "T":   "TH",
    "D":   "DH",
    "Z":   "ZH",
    "j":   "Y",
    "hw":  "W",         # "which"; merged with "witch", as most speakers do
    "x":   "K",         # loch, Bach
    "z":   "Z",
    "nv":  "N V",
}

LETTERS = {
    "b": "B", "d": "D", "f": "F", "g": "G", "h": "HH", "k": "K",
    "l": "L", "m": "M", "n": "N", "p": "P", "r": "R", "s": "S",
    "t": "T", "v": "V", "w": "W", "z": "Z", "j": "JH", "c": "K",
}

# An optional schwa: "[@]" and "(@)". Kept -- these are syllabic
# consonants ("button", "rhythm") where dropping it makes the word a
# syllable short.
OPTIONAL = {"[@]": "AX", "(@)": "AX"}

_SYM = re.compile(r"/([^/]*)/|(\[@\])|(\(@\))|(.)")


class Stats:

    def __init__(self):
        self.entries = 0
        self.converted = 0
        self.dropped = 0
        self.unknown = {}

    def report(self, limit=12):
        print("  %d entries, %d converted, %d dropped" %
              (self.entries, self.converted, self.dropped))
        if self.unknown:
            worst = sorted(self.unknown.items(), key=lambda kv: -kv[1])[:limit]
            print("  unconvertible symbols: " +
                  ", ".join("%r x%d" % (k, v) for k, v in worst))


def convert(pron, stats=None):
    """
    One Moby pronunciation -> a list of our phonemes with stress
    digits, or None if it used something we do not have.
    """

    out = []
    stress = 0          # pending stress for the next vowel

    # A hand-written scanner rather than a regex, because of one quirk:
    # Moby often writes a symbol with DOUBLED slashes -- b//Oi// for
    # "boy", //dZ//oIn for "join" -- which a /x/ pattern reads as an
    # empty symbol and then loose letters: "boy" came out B AO IY, and
    # "join" as D ZH. Adjacent symbols (/A//S/) also produce "//", but
    # only between two symbols, where the first has already been
    # consumed, so an empty symbol seen HERE is always the doubled form.
    i, n = 0, len(pron)
    while i < n:
        c = pron[i]

        if pron.startswith("[@]", i) or pron.startswith("(@)", i):
            out.append(("V", "AX", 0))
            i += 3
            continue

        if c == "/":
            j = pron.find("/", i + 1)
            if j < 0:
                if stats is not None:
                    stats.unknown["/"] = stats.unknown.get("/", 0) + 1
                return None
            sym = pron[i + 1:j]
            i = j + 1
            if sym == "":
                # The doubled form: the symbol runs to the next "//".
                k = pron.find("//", i)
                if k > i and (pron[i:k] in VOWELS or pron[i:k] in CONSONANTS
                              or pron[i:k] in OPTIONAL):
                    sym = pron[i:k]
                    i = k + 2
                else:
                    continue                # a genuinely empty one
            if sym in OPTIONAL:
                out.append(("V", OPTIONAL[sym], 0))
                continue
            if sym == "-":
                continue
            if sym in VOWELS:
                for ph in VOWELS[sym].split():
                    kind = "V" if ph[0] in "AEIOU" else "C"
                    out.append((kind, ph, stress if kind == "V" else 0))
                stress = 0
                continue
            if sym in CONSONANTS:
                for ph in CONSONANTS[sym].split():
                    out.append(("C", ph, 0))
                continue
            if stats is not None:
                stats.unknown[sym] = stats.unknown.get(sym, 0) + 1
            return None

        i += 1
        if c == "'":
            stress = 1
            continue
        if c == ",":
            stress = 2
            continue
        if c in ("_", "-", " ", "."):
            continue
        if c in LETTERS:
            out.append(("C", LETTERS[c], 0))
            continue
        # One-character symbols written without their slashes.
        if c in VOWELS:
            out.append(("V", VOWELS[c], stress))
            stress = 0
            continue
        if c in CONSONANTS:
            out.append(("C", CONSONANTS[c], 0))
            continue
        if stats is not None:
            stats.unknown[c] = stats.unknown.get(c, 0) + 1
        return None

    if not out:
        return None

    # Stress digits. Moby marks the syllable, we mark the vowel, and an
    # entry with no mark at all is a one-syllable word: stress it, so
    # it is not spoken as a reduced-sounding aside.
    vowels = [i for i, (kind, _, _) in enumerate(out) if kind == "V"]
    if not vowels:
        return None
    if not any(out[i][2] for i in vowels):
        # A one-syllable word with no mark. Stress it -- but a schwa
        # cannot carry stress, so promote it: "of" is AH, not AX.
        kind, ph, _ = out[vowels[0]]
        out[vowels[0]] = (kind, "AH" if ph == "AX" else ph, 1)

    # Syllabic consonants. Moby writes "button" as b/@/tn and
    # "rhythm" as r/I/D/@/m: the final n or l is a syllable of its own,
    # with no vowel. phon.c has no syllabic consonants, so give it the
    # schwa that English speakers hear there anyway.
    i = 1
    while i < len(out):
        kind, ph, _ = out[i]
        prev = out[i - 1]
        if (kind == "C" and ph in ("N", "M", "L", "NG") and prev[0] == "C"
                and (prev[1] not in ("N", "M", "L", "NG") or ph == "L")
                and (i + 1 == len(out) or out[i + 1][0] == "C")):
            out.insert(i, ("V", "AX", 0))
            i += 1
        i += 1

    phones = []
    for kind, ph, st in out:
        phones.append(ph + str(st) if kind == "V" else ph)
    return phones


WORD = re.compile(r"^[a-z]+$")


def read(path, stats=None, words_only=True):
    """
    Yields (word, [phonemes]) from mobypron.unc. Lines are
    CR-terminated and the file is Latin-1.

    Multi-word and hyphenated entries are skipped by default: they are
    more than half the file, the front end splits on spaces and hyphens
    anyway, and their parts are nearly always present as entries of
    their own.
    """

    seen = set()

    with open(path, "rb") as f:
        data = f.read().decode("latin-1")

    for line in data.replace("\r\n", "\n").replace("\r", "\n").split("\n"):

        if not line.strip():
            continue
        if " " not in line:
            continue

        word, pron = line.split(" ", 1)
        word = word.strip().lower()
        if words_only and not WORD.match(word):
            continue
        if word in seen:
            continue

        if stats is not None:
            stats.entries += 1

        phones = convert(pron.strip(), stats)
        if phones is None:
            if stats is not None:
                stats.dropped += 1
            continue

        seen.add(word)
        if stats is not None:
            stats.converted += 1
        yield word, phones
