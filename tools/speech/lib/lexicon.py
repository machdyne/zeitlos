"""
Zeitlos
Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.

Building the pronunciation lexicon.

Two jobs:

  1. INFLECTIONS. Moby lists base words: "setting" and "settings" are
     not in it, "set" is. Real text is full of them, so the plural,
     past, progressive and -ly forms are generated from each base with
     the ordinary spelling rules (double the consonant, drop the silent
     e) and the ordinary sound rules (-s is /s/ after a voiceless
     consonant and /z/ otherwise; -ed is /t/, /d/ or a whole syllable).
     This roughly doubles the entries and covers far more running text
     than the extra size costs.

  2. ENCODING. Words sorted, front-coded in blocks, with a block index
     -- so the device binary-searches the index in memory and reads one
     small block off the card. See docs/tts_data.md for the layout.
"""

VOICELESS = set("P T K F TH S SH CH HH".split())
SIBILANT = set("S Z SH ZH CH JH".split())
VOWELCH = set("aeiou")


def _is_vowel(ph):
    return ph[0] in "AEIOU"


def inflect(word, phones):
    """Yields (word, phones) for the inflected forms of one base."""

    if len(word) < 3 or not phones:
        return

    last = phones[-1]
    base = last.rstrip("012")

    # -s / -es
    if base in SIBILANT:
        yield word + ("es" if not word.endswith("e") else "s"), phones + ["IX0", "Z"]
    elif base in VOICELESS:
        yield word + "s", phones + ["S"]
    else:
        yield word + "s", phones + ["Z"]

    # -ed, and the doubling/e-dropping that goes with it
    stem_e = word[:-1] if word.endswith("e") else word
    # Double the final consonant only in a one-syllable word ("set" ->
    # "setting"). English doubles in longer words too, but only when
    # the last syllable is stressed ("permit" -> "permitting"), and
    # applying it blindly turns "open" into "openning".
    groups = 0
    for i, c in enumerate(word):
        if c in VOWELCH and (i == 0 or word[i - 1] not in VOWELCH):
            groups += 1
    dbl = (word + word[-1]) if (groups == 1 and len(word) > 2
                                and word[-1] not in "aeiouwxy"
                                and word[-2] in VOWELCH
                                and word[-3] not in VOWELCH) else None

    if base in ("T", "D"):
        ed_ph = phones + ["IX0", "D"]
    elif base in VOICELESS:
        ed_ph = phones + ["T"]
    else:
        ed_ph = phones + ["D"]

    if word.endswith("e"):
        yield word + "d", ed_ph
        yield word[:-1] + "ing", phones + ["IX0", "NG"]
    elif word.endswith("y") and len(word) > 2 and word[-2] not in VOWELCH:
        yield word[:-1] + "ied", ed_ph
        yield word + "ing", phones + ["IX0", "NG"]
    elif dbl:
        yield dbl + "ed", ed_ph
        yield dbl + "ing", phones + ["IX0", "NG"]
    else:
        yield word + "ed", ed_ph
        yield word + "ing", phones + ["IX0", "NG"]

    # -ly, on words that look like adjectives rather than verbs. Cheap
    # and wrong sometimes ("newly" fine, "dogly" never appears in text,
    # so an unused entry is the whole cost).
    if not word.endswith(("ly", "s", "e")) and 3 < len(word) <= 6:
        yield word + "ly", phones + ["L", "IY0"]


def expand(entries):
    """base dict -> dict including inflections (bases always win)."""
    out = dict(entries)
    for word, phones in entries.items():
        for w, ph in inflect(word, phones):
            if w not in out:
                out[w] = ph
    return out


# -- encoding --

BLOCK = 32          # entries per block: index size against read size


def encode(entries, phone_ids):
    """
    entries: sorted [(word, [phones])]. Returns (index_blob, data_blob,
    count). Layout, all little-endian, documented in docs/tts_data.md:

      data: blocks of BLOCK entries. Within a block each entry is
            u8 shared-with-previous, u8 rest-length, rest bytes,
            u8 phone count, then one byte per phone:
            stress << 6 | phoneme id. The first entry of a block shares
            nothing, so a block can be read on its own.

      index: fixed 16-byte records, one per block: u32 data offset,
             then 12 bytes of the block's first word, NUL-padded and
             truncated. FIXED SIZE on purpose: the device binary-
             searches this by SEEKING, reading 16 bytes a probe, so
             nothing about the lexicon has to be resident. Truncation
             is safe because the search only has to land in the right
             block, and the block is then scanned.
    """

    data = bytearray()
    index = bytearray()
    offsets = []

    prev = ""
    for i, (word, phones) in enumerate(entries):

        if i % BLOCK == 0:
            offsets.append((len(data), word))
            prev = ""

        w = word.encode("ascii")
        p = prev.encode("ascii")
        shared = 0
        while shared < len(p) and shared < len(w) and shared < 255 and p[shared] == w[shared]:
            shared += 1

        rest = w[shared:]
        data.append(shared)
        data.append(len(rest))
        data += rest
        data.append(len(phones))
        for ph in phones:
            name = ph.rstrip("012")
            stress = int(ph[-1]) if ph[-1].isdigit() else 0
            data.append((min(stress, 3) << 6) | phone_ids[name])
        prev = word

    import struct
    for off, key in offsets:
        k = key.encode("ascii")[:12]
        index += struct.pack("<I", off) + k + b"\0" * (12 - len(k))

    return bytes(index), bytes(data), len(entries)
