# Speech data: `tools/speech`

The data pipeline behind `sw/apps/tts` ([tts.md](tts.md)). It turns
public-domain sources into the **speech pack** `tts` loads from the card
-- a pronunciation lexicon, trained letter-to-sound rules, and a
**recorded voice** -- and it is where the voice is **measured**. Paths
below are relative to `tools/speech` unless they say otherwise.

**Zeitlos does not depend on this tool.** A release ships the built
pack; without one, `tts` still speaks, with its built-in dictionary and
rules and its formant voice. A board with no sdcard still speaks.

    tools/speech/
      speech               the CLI
      dist/en.spec         the English voice: sources, licences, hashes
      lib/spec.py          recipe parsing
      lib/fetch.py         resumable downloads, extraction, checksums
      lib/moby.py          the Moby pronunciations, and their inflections
      lib/lexicon.py       the lexicon sections
      lib/lts_train.py     letter-to-sound trees, learned from the lexicon
      lib/pack.py          the ZSPK container
      lib/align.py         alignment of a speech corpus (lib/aligner.py, lib/audio.py)
      lib/diphone.py       THE RECORDED VOICE: inventory, synthesis, pack section
      lib/evalset.py       the frozen test material
      lib/asr.py           the machine listener (Whisper; pocketsphinx for plumbing)
      lib/score.py         rendering and scoring
      lib/ledger.py        every score, keyed on everything it depends on
      lib/compare.py       side-by-side comparisons with intervals
      lib/prosody.py, acoustics.py, source.py, tune.py   fitting the formant voice (optional)
      .cache/              downloads (gitignored)
      build/               everything built (gitignored)

Same shape as [`tools/ask`](ask_app.md), and for the same reasons: one
command, recipes readable on their own, a cache that is never committed.

## Status

| | |
|---|---|
| **done** | the tool, recipes, `sources`/`fetch`, provenance and checksums |
| **done** | the ZSPK pack, `speech build`, its reader in `sw/apps/tts`, the card and release plumbing |
| **done** | the lexicon: 391,159 pronunciations from Moby, inflections included |
| **done** | letter-to-sound rules learned from the lexicon |
| **done** | alignment of LJSpeech (13,099 of 13,100 clips) |
| **done** | machine intelligibility scoring: frozen material, Whisper, a ledger, paired intervals |
| **done** | the recorded voice: diphones from LJSpeech, TD-PSOLA, measured, and ported to the device (`sw/apps/tts/dsyn.c`) -- the default voice |
| optional | fitting the formant voice to the speaker (prosody, formants, loci, source): measured, no intelligibility gain, kept out of the default pack |
| next | closing the gap to the recordings: several candidates per diphone, chosen at run time by how well they join; a better aligner |

Where it stands, by machine listener (Whisper medium.en, material v3,
words correctly identified):

| | sentences | isolated words | ordinary sentences |
|---|---|---|---|
| no pack (built-in rules, formant voice) | -- | -- | 83% (v2) |
| formant voice, with the lexicon | 75.3% | 38.8% | 88.1% |
| **recorded voice** (prototype, before the pause and join fixes) | 73.7% | **47.6%** | 86.6% |
| LJSpeech's own recordings | | | 94.2% |

---

## Using it

```
./tools/speech/speech fetch dist/en.spec          # the sources (resumes; checks hashes)
./tools/speech/speech fetch --only ljspeech --limit 400   # a part of one
./tools/speech/speech sources                     # what goes in, under what terms
./tools/speech/speech align                       # phones placed in the LJSpeech clips
./tools/speech/speech diphones                    # the recorded voice's inventory
./tools/speech/speech build                       # -> build/en/speech.zspk
./tools/speech/speech show build/en/speech.zspk   # what is in one
./tools/speech/speech listen                      # the listening set, formant and recorded, side by side
./tools/speech/speech listen --echo               # a few sentences four ways, to hear where a fault comes from
./tools/speech/speech compare --diphone           # scored: formant, recorded (prototype and device engine)
./tools/speech/speech compare --ablation          # scored: each pack stage added in turn
```

`speech build` takes `--fitted` to add the fitted formant-voice sections,
and `--no-diphones` to leave the recorded voice out. `speech mirror`
makes and checks a mirror of a source (below).

Downloads go to `.cache` (override with `SPEECH_CACHE`) and **resume**:
an interrupted 2.6GB transfer continues with a range request. Extraction
takes only the members the spec names, and every extracted file is
checked against the spec's `sha256`; a file with no hash yet prints one,
which is how a new source gets pinned.

---

## The pack

`speech build` writes `build/<voice>/speech.zspk`. **It is not committed
and never will be**: it is built from public-domain sources by whoever
cuts a release, and published with that release beside the bitstreams.

| section | size | |
|---|---|---|
| `MANIFEST` | 800 B | what it was built from, with licences and hashes |
| `PHONES` | 107 B | the phoneme names, in the order the ids use |
| `LTS` | 91 KB | trained letter-to-sound trees (resident on the device) |
| `LEXIDX` | 191 KB | the lexicon's block index |
| `LEXDAT` | 5.5 MB | 391,159 pronunciations |
| `DIPHONE` | 1.8 MB | the recorded voice: 1,404 diphones, 8-bit mu-law, with pitch marks |
| `PROSODY`, `FORMANTS` | < 1 KB | the fitted formant voice; only with `--fitted` |

About 7.5 MB in all. Its shape, little-endian:

- **Header:** `ZSPK`, version, section count, file size, CRC32, then a
  16-byte record per section (8-byte name, offset, length). Sections are
  found by name, so an older `tts` reads a newer pack and ignores what it
  does not know -- a `tts` without the recorded voice ignores `DIPHONE`.
- **`LEXIDX`:** fixed 16-byte records -- a block's data offset, then
  twelve characters of its first word.
- **`LEXDAT`:** blocks of 32 entries, front-coded against each other; a
  block is readable on its own. Each entry is a shared-prefix length,
  the rest of the word, and one byte per phoneme (stress in the top two
  bits, id in the low six).
- **`PHONES`** makes those ids mean anything: `tts` maps the names onto
  its own table and **refuses a pack naming a phoneme it does not have**.
- **`DIPHONE`:** described in `lib/diphone.py` ("the pack section"): a
  phone-pair table, a record per unit (where its samples and marks are,
  and a flag for a pause half that holds no pause), the pitch marks,
  then the samples.

**What the device keeps in memory, and what it reads.** The lexicon keeps
every 32nd index key resident (~6KB), so a word costs two reads -- the
run of index records, then the block. The recorded voice keeps its pair
table and unit records (~25KB) and reads each chunk's units once, front
to back, into a 64KB arena. Read ORDER matters on the card: the kernel's
FatFs, without fast seek, walked the file's cluster chain from its start
on every backward seek (see "Long passages repeated themselves", below);
the kernel now builds a cluster map for every file opened for reading.

### On the card, and in a release

The card gets it at `/speech/en.spk` (8.3 names, so not `speech.zspk`).
`release/lib/mkfatimg.py` copies it **if it is there** and counts it in
the image's space check; if not, it prints a note and builds a card
without one. `SPEECH_PACK=<path>` ships a pack built elsewhere.

By hand: `mkdir /speech` on the card and copy the pack there as
`en.spk`. `tts` logs `lexicon from /speech/en.spk` and `recorded voice
from /speech/en.spk` when it finds them.

### Lookup order

1. the built-in dictionary (213 curated words: interface vocabulary and
   irregulars that must be exact);
2. the pack's lexicon, when there is one;
3. the pack's trained letter-to-sound rules;
4. the built-in rules in `lts.c`.

---

## Letter-to-sound rules, learned

The rules in `lts.c` were written by hand, and a few hundred rules is
what one person can write. `speech build` learns the same thing from
90,849 aligned pronunciations instead.

**Alignment** (`lib/align.py`) is the part everything else rests on:
which phoneme did each letter produce?

    knight  ->  k:-  n:N  i:AY  g:-  h:-  t:T
    box     ->  b:B  a:AA  x:"K S"
    settle  ->  s:S  e:EH  t:-  t:T  l:"AX L"  e:-

A dynamic program over three choices per letter (silent, one phoneme,
two), scored by a hand-written table of what each letter is *allowed*
to sound like. 95.8% of the lexicon aligns; the rest is dropped rather
than forced, because a forced alignment teaches the trees nonsense.

**Training** (`lib/lts_train.py`) grows one binary decision tree per
letter. Every question is "is the letter *n* places away an *x*?",
with *n* from -4 to +4; every leaf is the phoneme(s) that letter makes.
Trees are grown on information gain and pruned back to a node budget,
`lts_nodes` in the spec, because this model is **resident** on the
device -- it is consulted per letter, which is no place for a seek.

Measured on 3,000 held-out words, scoring the phonemes without stress:

| model | size | words exactly right | phonemes right |
|---|---|---|---|
| `lts.c`, hand-written | 0 | **19.8%** | 67.0% |
| trained, 300 nodes/letter | 37 KB | 34.7% | 78.2% |
| trained, 600 | 65 KB | 40.9% | 81.4% |
| **trained, 1200 (the default)** | **91 KB** | **46.0%** | **83.3%** |
| trained, 8000 | 336 KB | 49.9% | 84.9% |

`sw/apps/tts/pack.c` walks the trees; the stress digits come from the
same pass `lts.c` uses, since stress is about syllables and suffixes
rather than letters. The C evaluator agrees with the Python model on
99.1% of held-out words -- the rest are training tie-breaks, not
disagreements about the tree.

**The memory cost is real and unconditional:** `PACK_LTS_MAX` is a
96KB static buffer, present whether or not a card is. On a 1MB board
that matters, and the table above is there so the budget can be turned
down for one. A model larger than the ceiling is skipped with a
message, and the built-in rules take over.

### Inflections

Moby lists base words: it has "set" but neither "setting" nor
"settings". Real text is full of them, so `speech build` generates the
plural, past, progressive and -ly forms with the ordinary spelling
rules (double the consonant in a one-syllable word, drop the silent e)
and the ordinary sound rules (-s is /s/ after a voiceless consonant and
/z/ otherwise; -ed is /t/, /d/, or a whole syllable). That takes 94,819
words to 391,159, which is most of the pack's size and most of its
value on running text.

---

## The sources

All public domain. That is enforced, not just intended: a source with
no `licence` line is refused by the parser.

| source | what | size | needed for |
|---|---|---|---|
| `moby` | the Moby Pronunciator (177,267 pronunciations) and the Moby part-of-speech list | 8MB | phase 5 |
| `ljspeech` | 13,100 clips, ~24h, one speaker reading seven non-fiction books | 2.6GB | phase 7-8 |
| `books` | Project Gutenberg texts, for word frequency | small | phase 5 |

**Moby** is public domain by the author's grant of January 2001, and is
mirrored at Project Gutenberg (ebook #3205). Its part-of-speech list is
what separates the two "read"s and the two "lead"s.

**LJSpeech** is public domain throughout: the texts were published
1884-1964, the recordings are LibriVox, and the alignment is dedicated
to the public domain. It is a female voice, so the formant work either
keeps that range or scales it. It derives from LibriVox, so it could be
rebuilt from there if it ever disappeared.

### CMUdict is deliberately excluded

The Moby distribution bundles a copy of CMUdict, which is
BSD-licensed rather than public domain. `dist/en.spec` names it in a
`refuse` line: the extractor leaves it alone and says so, and if a
`take` rule ever tried to pull it in, the fetch fails outright.

It is still used on the build machine as an **answer key** --
`sw/apps/tts/tests/lts_eval.py` scores our rules against it -- which is
fine because nothing from it reaches the device. Rules get corrected
when the score shows a class of error, never word by word out of the
dictionary. Same for the recogniser in `asr_eval.py`.

---

---

## The recorded voice

A **diphone** runs from the middle of one sound to the middle of the
next. Cut real speech there, and every join falls in the steadiest part
of a sound, while every transition -- bursts, aspiration, nasal
releases, the glides between vowels -- stays as a person said it: the
parts the formant rules never managed (see "How the voice got here").

**The inventory** (`speech diphones`, `lib/diphone.py`) takes, for each of
the ~1,400 diphones in the aligned LJSpeech corpus, the best of twelve
candidates. Best means: typical durations, a stressed vowel where there
is one, pitch near the speaker's middle, ends whose spectra are close to
that phone's typical spectrum (so units meet each other smoothly, and a
unit cut where the alignment was wrong is rejected), no sounding half
far quieter than that phone usually is, no flapped T or D ("ladder"),
and no "pause" half that holds speech. Each half is levelled to its
phone's typical loudness. Pitch marks -- one per glottal pulse, found
on a low-passed copy, each within 20% of a period of the last -- are
found here, once.

**Synthesis** keeps the front end for everything but the sound: phones,
timing and pitch come from `phon.c` exactly as for the formant voice.
TD-PSOLA (Moulines and Charpentier, 1990) joins the units: ONE lattice
of output pitch marks for the utterance; for each, the unit covering
that moment, mapped half onto half-phone, gives its nearest grain -- two
source periods under a Hann window. Near a join, both units' grains are
blended over 12ms either side. A pause half is never stretched and
plays at most 40ms beside the join; one flagged as holding no pause
plays none.

**On the device** it is `sw/apps/tts/dsyn.c`, the same method in
integers, and the default voice whenever the pack has diphones
(`system.tts.voice = recorded`; Super+E switches to the formant voice
and back). The C engine and the prototype agree to within a recording's
distance from its own rebuild; `speech compare --diphone` scores both.

---
## Measuring

A voice is judged by a machine listener -- Whisper medium.en, on a GPU --
on frozen test material, every comparison paired and given an interval.
It is not a person, and a change that only a recogniser likes is
possible; `speech listen` exists so that every result is also heard.

### Rules

- **One frozen yardstick.** The test material is generated once and
  saved (`build/eval/material-v3.json`, with a version and a hash); a
  change to the generator is a new version, and scores across versions
  are not compared. One recogniser, one set of settings.
- **Paired, with an interval.** Every comparison is on the same items,
  and every difference comes with a 95% interval from resampling the
  items (`score.paired_ci`). Paired, because some sentences are simply
  harder, and that cancels. A difference counts only when its interval
  excludes zero -- the reports say "better", "WORSE", or "no real
  difference".
- **Anchors.** The real recordings of the ordinary sentences, recognised
  the same way, are what a human voice scores; the shipped voice with no
  pack is where we started.
- **Everything in the ledger.** `build/results.jsonl` gets every score as
  it is made, with the synthesiser's source hash, the pack's hash, the
  material, the recogniser, and -- for the recorded voice -- the
  inventory's and the Python engine's hashes. Nothing identical is measured twice, and
  a cancelled run loses only the item in progress. `speech tune` resumes
  from its own log in the same way.

### Three metrics

All three are **words correctly identified** -- higher is better -- which
is immune to a recogniser inventing words (see "The instrument, fixed"):

- **sentences**: semantically unpredictable sentences ("Suspect the hat
  and the medical night."), where every word has to be heard;
- **isolated words**: minimal-pair words, each in the carrier phrase
  "Would you write ___ now.", only that word scored;
- **ordinary sentences**: sentences from the corpus -- closest to real
  use, and the one with a human anchor: the same sentences' recordings.

Every report also tabulates the targeted confusions (T heard as D, K as
T, N as L...) per 100 of the sound said, counted in scored words only.

### The instrument, fixed

The first two sessions' sentence metric was word error rate, and the
recogniser sometimes LOOPS on unclear audio -- "wiggly-wiggly-wiggly..."
for 113 invented words on a six-word sentence. One sentence could be a
fifth of a variant's errors, and which ones looped flipped at random
between variants: intervals of +/-10-15 points, too wide to see any
single rule. So, from material v3:

- **Every metric is words correctly identified**, immune to invented
  words -- the long-standing measure for synthetic speech. Re-scoring
  sessions 0 and 1 this way shrank the sentence intervals to +/-1-2.
- **Isolated words are spoken in a carrier phrase**, "Would you write
  ___ now.", and only the word is scored: alone, a short clip made the
  recogniser spell ("S I E D"), write digits, or answer with a stock
  phrase ("please", "see you"). 300 words per set instead of 80.
- **Every report tabulates the targeted confusions**, per 100 of the
  sound said, for every variant -- the aggregate can hide a rule that
  fixes its target and breaks something else, which is exactly what
  session 1 did.
- Reproducibility checked: the same audio from two builds scored
  identically, to the transcript.

---

## How the voice got here

The record of each measured step, in order: what was tried, what the
numbers said, and what was decided. The first sessions worked on the
formant voice; after they showed nothing gained, the recorded voice
replaced it.

### Session 0: what it found

Whisper medium.en on material v2 (27922d8f5291), RE-SCORED as words
correctly identified (below, "The instrument") from the ledger's
transcripts; the shipped voice with no pack, then each stage in turn:

| | sentence words ok | isolated words ok | ordinary words ok |
|---|---|---|---|
| shipped, no pack | 68.0% | 26.2% | 83.4% |
| + lexicon | **76.3%** (+8.3 [+5.6, +11.2]) | 28.7% | **88.2%** (+4.8 [+2.8, +6.8]) |
| + trained letter rules | 75.6% | 28.7% | 88.1% |
| + prosody, vowels, loci, source | 73.3-74.6% | 26.9-29.4% | 87.2-87.6% -- no real difference |
| real recordings | | | **94.2%** |

- **The lexicon is the one real improvement.** Nothing fitted to the
  speaker made the voice measurably more intelligible, so the default
  pack is the lexicon and the letter rules; `speech build --fitted` adds
  the rest, which may still help naturalness, untested.
- **The gap is consonants**: voiceless stops heard as voiced (T->D,
  P->B, K->D), velars as alveolars (K->T), nasals as liquids (N->L,
  M->L). Vowel confusions are minor.

### Session 1: what it found

Five rules from the phonetics of English, each behind a switch
(`phon.h`, `PHON_EXP_*`; set only by the rendering harness, from
`ZTTS_EXP`; off by default and byte-identical to the shipped voice when
off). Re-scored the same way:

| | sentence words ok | targeted confusions (count) |
|---|---|---|
| default | 75.6% | T->D 24, P->B 7, K->T 12, M->L 8, N->L 7 |
| + all five | **72.5%** (-3.2 [-5.5, -0.9]) | T->D **12**, P->B **1**, K->T 9, M->L 5, N->L 4 |

**The experiments fixed what they aimed at, and broke something else.**
With all five, voicing confusions halved, yet sentences got worse; new
confusions appeared (AE->HH, K->HH, K->P, N->T). Reading the code:

- `vot` took its extra aspiration OUT of the vowel, shortening the voiced
  part until it was heard as "h" (AE->HH, K->HH). Now the time is added.
- `velar` put K's locus near a back vowel's own F2 -- where P's is -- and K
  was heard as P. Now well above it.
- `f1-cutback` changed nothing measurable; removed.
- `vowel-voicing` (T->D 24->18) and `nasal` (M->L 8->4) did their jobs
  with no significant loss.

Session 1b tests those four, alone and together:
`./tools/speech/speech compare --session1b`.

### Session 1b: what it found

With the instrument fixed (below), none of the four rules helped:
vowel-voicing made isolated words worse (-4.1 [-7.0, -1.4]), all four
together likewise (-4.7), and the rest showed no real difference. Three
sessions, nine rules, no gain: the formant voice cannot be tuned toward
DECtalk one rule at a time -- DECtalk is a decade of expert rule-writing,
and each rule here interacted with the others.

### The change of approach: diphones

A DIPHONE runs from the middle of one sound to the middle of the next.
A voice built from them keeps every transition, burst, aspiration and
nasal release as a person actually said it -- exactly what the rules
kept failing to make -- and joins them in the steady middles of sounds.
It was the approach that matched and then passed formant synthesis in
the 1990s, and it fits the device better than the formant voice:
roughly 1-2MB of units on the card, and a few multiply-adds per sample
(TD-PSOLA) against a five-resonator cascade.

    ./tools/speech/speech diphones             # the inventory, from the aligned corpus
    ./tools/speech/speech compare --diphone    # the prototype against the formant voices

`lib/diphone.py` is the prototype, in Python, to decide whether the
device gets a C version. The inventory keeps the best recorded example
of each diphone (typical durations, stressed vowels, pitch near the
speaker's middle) with its pitch marks. Synthesis keeps our front end
for everything but the sound -- phones, timing and pitch come from
`tts_wav`'s `.phones` and `.f0` -- and joins the units by TD-PSOLA
(Moulines and Charpentier, 1990): two-period grains under a Hann
window, laid down at the target pitch through a time warp that maps
each half-unit onto its half-phone. A diphone the corpus lacks is made
from the first half of one unit and the second half of another, rather
than left silent.

The voice is LJSpeech's reader, a woman's, following the front end's
FEMALE pitch contour (dragging it to the male contour lowered it nearly
an octave). The formant voice stays as the fallback when there is no
pack, and now defaults to female too, so a missing pack does not also
change who is speaking. (Reversed after the next run: the female
formant voice measured 10-15 points less intelligible, so the fallback
stays male.)

What would count as success: isolated words from ~39% to 70% or more,
and ordinary sentences closing most of the gap from 88% to the
recordings' 94%. If the prototype cannot clearly beat the formant
voice, it stops here.

### The first diphone prototype: what it found

Material v3, Whisper medium.en; 1,404 diphones from the whole corpus:

| | sentence words ok | isolated words ok | ordinary words ok |
|---|---|---|---|
| formant, male (shipped) | 75.3% | 38.8% | 88.1% |
| formant, female | 60.4% (-14.9) | 28.3% (-10.5) | 77.8% (-10.3) |
| diphone prototype | 45.4% (-29.9) | 39.4% (no difference) | 58.3% (-29.7) |

- **The female formant voice is much less intelligible**, so the formant
  default stays male; consistency with the recorded voice is not worth
  10-15 points to someone who depends on it. Fixing the female formant
  voice is its own piece of work.
- **The recorded units carry the consonant cues the rules never did**:
  K->D, G->D and M->L fell to zero, N->L to 0.2 per hundred.
- **But connected speech collapsed**, with new confusions: D->L x24 (the
  signature of FLAPPING -- units cut from "ladder"-like contexts), D->T,
  UH->IH, UW->IY.

The next run separates the suspects: `speech compare --diphone` scores
a rebuilt inventory (each half-phone levelled to its phone's typical
loudness; flapped T and D avoided in selection), the same keeping each
unit's recorded pitch (what pitch-shifting costs), and COPY SYNTHESIS of
the ordinary sentences -- each rebuilt from units with its own
recording's timing and pitch, taking our front end out entirely. Near
the recordings' 94%, and the units and joins are sound and the problem
is the front end's prosody for this voice; low, and it is the units or
the joins.

### The second diphone run: what it found

| | sentence words ok | isolated | ordinary |
|---|---|---|---|
| formant, male (shipped) | 75.3% | 38.8% | 88.1% |
| diphone, levels + no flaps | 43.1% | 42.9% | 60.8% |
| ...keeping its own pitch | 42.9% | 40.8% | 61.2% |
| ...copy synthesis | | | **56.7%** |

**Copy synthesis was as bad as everything else**: rebuilt with each
recording's own timing and pitch, the ordinary sentences still scored
57%. So neither our front end nor pitch-shifting is the problem -- the
units, or the joins between them, are. Listening, the first prototype
had many short gaps.

What was tested next, here, on the synthetic corpus:

- **The engine is sound.** Rebuilding a recording from its OWN units
  with its own pulse spacing comes back as close as a copy with 1%
  noise added (mel-cepstral distance, the harmonic-blind measure; raw
  log-spectral distance compares individual harmonics and misleads
  when pitch differs by a few percent). A steady vowel: 2.0dB log-
  spectral distance, below a barely-audible noise copy's 3.0dB.
- Two engine faults were found and fixed on the way, though neither was
  the main damage: the pitch lattice restarted at every unit, breaking
  the voice's rhythm at every join; and pitch marks were sought over
  3/4-1 3/4 periods, so 16% of consecutive spacings jumped by more
  than 15% (now 3%, searching +/-20% on a low-passed copy).
- **So selection**: units had been chosen by timing and pitch alone,
  never checked for their SOUND. Now each unit's ends -- where it is
  joined -- must be close to that phone's typical spectrum (measured
  over hundreds of candidates): a unit cut where the alignment was
  wrong is far from typical and rejected, and units whose ends are all
  near typical meet each other smoothly. And a unit whose vowel or
  sonorant half is far quieter than that phone usually is -- a cut
  that wandered into a pause, which plays as a GAP -- is rejected
  outright (stops exempt: their closures are silent). Twelve
  candidates per diphone instead of six, to choose among.

### The third diphone run: the turnaround

With the engine fixes (one pitch lattice; steady pitch marks) and the new
unit selection (typical spectra at the joins; no near-silent halves):

| | sentence words ok | isolated | ordinary |
|---|---|---|---|
| formant, male (shipped) | 75.3% | 38.8% | 88.1% |
| diphone, second run | 43.1% | 42.9% | 60.8% |
| **diphone, now** | **73.7%** | **47.6%** (+8.7 [+4.1, +14.0], better) | **86.6%** |
| ...copy synthesis | | | 85.4% |
| real recordings | | | 94.2% |

- **Level with the shipped voice on sentences, clearly better on isolated
  words** -- the first approach in any session to beat it on anything --
  and it is a human voice, which none of these numbers measure.
- **Copy synthesis now matches the front-end version** (85.4 against
  86.6): our timing and pitch are no longer what holds it back. The
  remaining gap to the recordings is in the units and the joins.
- The two inventories in that run were byte-identical (the inventory was
  evidently built twice), so it cannot say whether the engine fixes or
  the selection made the jump.

Two measurement faults it exposed, both fixed:

- The ledger keyed on the C code and the inventory but not on the Python
  synthesis engine, so an engine change without a rebuilt inventory
  could have been served a stale score. The engine's hash is now part of
  every diphone variant's key.
- Confusions were counted over every word of a carrier-phrase item, not
  just the scored one: ~500 copies of "Would YOU write ___ now" put "you"
  heard as "me", "be", "thee" at the top of the table (UW->IY x81, Y->M
  x48). Only scored words count now. (Moby already drops the American
  yod -- "new" is N UW -- so that was not it.)

`speech listen` renders the listening sentences in both voices into
`build/listen/formant` and `build/listen/diphone`.

### The device port

`sw/apps/tts/dsyn.c` is `lib/diphone.py`'s synthesis in integer C, and
`speech build` puts the inventory in the pack as a DIPHONE section
(format: `lib/diphone.py`, "the pack section"): resident tables of
~25KB, and 8-bit mu-law samples streamed from the card per unit.
`system.tts.voice` defaults to `recorded`, falling back to the male
formant voice when the pack has no diphones.

- mu-law keeps a quiet (-30dB) signal 37dB clean; the section is 1.77MB.
- C against the prototype, same inventory and sentences: identical
  lengths, correlation 0.70; not bit-exact, because the prototype reads
  phone boundaries in whole milliseconds and C in samples, so the two
  sometimes pick a neighbouring pitch mark. What counts is the
  recogniser's verdict, so `speech compare --diphone` scores the C
  engine as its own variant beside the prototype, from a pack built
  from the current inventory.
- No per-sample divisions but one 32-bit one, and no 64-bit arithmetic
  in the compiled rv32im code at all (checked in the object file).

### Echo and bumps: measured on real speech

With a sample of the real inventory and four LJSpeech recordings to work
on directly (`lib/diphone.py`):

- **The engine is clean on real speech**: a recording rebuilt from its own
  units comes back at a distance of 22-30 (another sentence: ~300); its
  pitch marks are 8-10% jumpy, with almost no octave errors; and there is
  no doubled pulse between the voice's real ones (the LPC residual's
  correlation between pulses is lower in the renderings than in the
  recordings). So the echo was not the engine.
- **It was the pauses.** Copy synthesis's pauses came out 10dB louder than
  the recordings'. 55 of the 74 pause units' "pause" halves held part of a
  word -- the alignment puts a pause at every comma, and the reader often
  did not pause there -- and the engine stretched them over every gap: a
  fragment of another word echoing in the silences. Now a unit whose pause
  half is no pause (louder than -20dB against its speech half) plays none
  of it -- flagged per unit in the pack for the device -- a real pause half
  plays at most 40ms next to the join, never stretched, and selection
  rejects such units outright. Pauses: -4dB -> -33dB (recordings: -14,
  with breath and room).
- **The bumps are the joins**: a hard switch from one recording to another
  made the spectral step at joins 1.4x the recording's own at the same
  moments. Grains of both units are now blended across 12ms either side
  (1.3-1.4x; no added echo). The rest is the units themselves differing:
  one candidate per diphone. Choosing among several at run time, by how
  well they join, is the known next step.
- C and Python agree to within a recording's distance from its own
  rebuild (22-29).

### Long passages repeated themselves: the card

On the board, an 18-second passage in the recorded voice repeated whole
phrases and took minutes: rendering ran at 482% of real time, the ring
ran dry, and the mixer -- which loops over the ring -- replayed the last
1.5s. The cause was the ORDER of reads from the pack. The kernel's FatFs
has fast seek off, so every backward seek walks the file's cluster chain
from its start -- about 380ms at the far end of a 7.5MB file on the
bit-banged SPI card driver -- and the engine read each unit's pitch marks
and then its samples, megabytes apart: a backward seek per unit.

- `dsyn.c` reads a chunk's units in two front-to-back passes (all marks,
  then all samples) into a 64KB arena; the pack's unit records and the
  lexicon's sampled index load in blocks. Backward seeks for the passage:
  228 -> 29, reads 1,924 -> 498 (the host build counts both,
  `ZTTS_READS=1`), audio byte-identical.
- The lexicon keeps every 32nd index key resident (~6KB): two reads a
  word instead of about twenty.
- The kernel (`sw/os/fsapi.c`, `ffconf.h`): FatFs fast seek, a cluster map
  per read handle, so a seek is a table lookup -- for every app.
- The audio backend never replays: the ring is cleared behind the reader,
  a writer that falls behind skips ahead (heard as a gap, and logged), and
  the reader's lap comes from elapsed time, so a stall cannot lose count.
  The reported CPU now includes the per-chunk work that was hidden.

---

## Alignment

Before anything can be learned about how a person times and pitches
their speech, every phone in the corpus has to be placed in time.

    ./tools/speech/speech fetch --only ljspeech --limit 2000
    ./tools/speech/speech build          # the pack: better references
    ./tools/speech/speech align

The usual way is a trained acoustic model and a forced aligner. This
does without both. It **synthesises each transcript with our own
voice**, which knows exactly where every phone starts and ends
(`tts_wav` with `ZTTS_MARKS=1` writes them out), and then **warps that
reference onto the recording** with dynamic time warping. Each
boundary lands on the frame of the recording it was warped to.

Two very different voices can be compared because the features --
cepstra and their deltas, 10ms frames -- are normalised per
utterance: each voice's average colour is removed and what remains is
how the spectrum MOVES, which is where the phones are.

Measured against synthetic recordings with known boundaries, rendered
at a different rate from the reference:

| recording | median error | within 20ms | within 50ms |
|---|---|---|---|
| clean | 4 ms | 98% | 98% |
| 20dB noise | 5 ms | 90% | 92% |
| 20dB noise, different microphone colour | 5 ms | 91% | 93% |
| 10dB noise, different microphone colour | 6 ms | 87% | 92% |

A real speaker differs from our voice by far more than rate and noise,
so **those numbers are a ceiling, not a prediction**. That is why
`align` writes Audacity label files for ten clips (`build/align/labels`:
open the wav, then File > Import > Labels) and a `summary.txt` of
per-phone durations -- the alignment is checked against the real
audio before anything is fitted to it.

Everything is numpy, and the alignment runs in parallel across cores.

## Fitting the formant voice (optional)

These stages fit the formant voice to LJSpeech's reader: timing and
pitch, vowel formants, consonant loci, the voice source. Each recovers
known values on synthetic speech -- but measured together (session 0)
none made the voice more intelligible, so they are left out of the
default pack; `speech build --fitted` puts them back. They may still
make the formant voice sound more natural, which was never measured.
They share the alignment above.

### Prosody

    ./tools/speech/speech prosody        # after align
    ./tools/speech/speech build          # puts it in the pack

`sw/apps/tts/phon.c` shapes timing and pitch with fourteen numbers
(`PHON_PRO_*` in `phon.h`) and a duration per phoneme. They were set by
hand. `speech prosody` measures the same quantities in the aligned
corpus and writes them to `build/align/prosody.bin`; the next
`speech build` adds it to the pack as a `PROSODY` section, and the
device uses it in place of the hand-set values.

It is **the same model with measured numbers**, not a new model.
Fourteen parameters fitted to thousands of sentences are well
determined and cannot overfit, and they fail safe: the device clamps
every value to a sane range, and closing the pack restores the
defaults.

| parameter | measured as |
|---|---|
| phoneme durations | the median of each phone, stressed and not phrase-final, scaled so the overall speed at a given rate setting is unchanged -- the speaker's *relative* timing, the user's choice of speed |
| unstressed | an unstressed vowel against the same vowel stressed |
| final_comma, final_stop | a phrase-final phone against the same phone elsewhere |
| pauses | median pause at each kind of punctuation, pauses at the end of a clip excluded |
| top, decl | a line fitted to each phrase's pitch over its first 80%, stressed vowels left out -- its intercept and slope |
| step, step_max | how much lower each phrase starts than the one before |
| stress1, stress2 | a stressed vowel's pitch above that line |
| comma_rise, stop_fall | the last 200ms of a phrase against where the line says it would be |

**Calibration.** A measurement and a parameter are not the same thing:
a stress lift shaped as a triangle does not read as its peak, and a
fall over a fixed number of frames reads differently at a different
speed. So each pitch measurement is also taken on OUR OWN VOICE,
rendering the same sentences with the default settings **at the
speaker's rate** (measured from the alignments), and the speaker's
value is scaled by default / our-reading. The measurement bias cancels.

Tested by fitting to 400 synthetic sentences in our own voice at a
different speed (145 against 180wpm), where every true value is known:

| parameter | true | fitted |
|---|---|---|
| unstressed | 55 | 53 |
| final_comma / final_stop | 135 / 155 | 135 / 154 |
| pause_comma / pause_stop | 190 / 300 | 206 / 316 |
| top / decl / step / step_max | 112 / 17 / 4 / 16 | 113 / 18 / 4 / 18 |
| stress1 | 17 | 16 |
| comma_rise | 13 | 20 -- the least reliable |
| stress2, stop_fall | 8, 10 | not measured: our own voice reads both with the wrong sign, so the measurement is not seeing what the parameter does, and they keep their defaults rather than a flipped value |
| pause_para | 420 | not measured -- no paragraphs, and LJSpeech has none either |
| vowel durations | the table | within 1-6% |
| consonant durations | the table | mostly within 10%; CH and JH 25% short |

Durations are calibrated the same way as pitch: each phone is measured
in our own voice too, and the table's value scaled by speaker / ours.
Without that, a stop or affricate came out two to three times too long
-- the table's "duration" for those is the closure alone, and an
alignment measures the whole phone.

An earlier version of this table was made on 17 sentences and
overstated things: several of its rows were defaults kept for lack of
data, printed as if they were measurements that agreed. The output now
says "not measured" for those, and `speech prosody` refuses
alignments too old to carry the stress and pause marks it needs,
rather than quietly fitting half the parameters.

Getting to the table above found five bugs, all fixed:

- phrase-final lengthening applied only when the pause came IMMEDIATELY
  after the vowel, so it lengthened only words ending in a vowel; it
  now lengthens the phrase's last vowel whatever follows (`phon.c`);
- pauses at the end of a clip measured as nothing, because the trailing
  silence is trimmed before alignment;
- a stale token index on the synthesiser's final silent segment put a
  phantom phone after the last pause;
- calibrating at 180wpm against a slower speaker over-read pitch events
  by up to 2x;
- a calibration threshold that refused the comma rise, which reads at
  a fifth of its setting because it ramps up -- but the same fifth for
  both voices, so it calibrates fine when well sampled.

`build/align/prosody.txt` lists each parameter's default and fitted
value, the fitted durations, and a note for anything that could not be
measured or came out of range (the default is kept).

### Acoustics

    ./tools/speech/speech acoustics      # after align and prosody
    ./tools/speech/speech build          # adds a FORMANTS section

`phon.c`'s formant table -- F1-F3 of every vowel and sonorant, and the
end targets of each diphthong -- came from textbook averages. This
measures where a real speaker actually puts each vowel.

1. **Measure.** Linear prediction (12 poles, `audio.formants`) over the
   middle 30-70% of every stressed vowel, clear of the transitions;
   diphthongs near the start and near the end. The median over every
   occurrence.
2. **Calibrate, at the speaker's voice.** LPC is biased, and its bias
   depends on pitch -- a high voice's sparse harmonics pull F1 toward
   them. So our own voice is rendered again at the speaker's measured
   pitch and tract size, the same tracker is run over it, and each
   value is taken as table x speaker / ours.
3. **Normalise.** A shorter vocal tract raises every formant together.
   That one factor is divided out, leaving the SHAPE of the speaker's
   vowel space -- her accent -- in our voice's tract. How big a tract to
   speak with stays `system.tts.formants`, the user's choice.

Values more than 35% from the table are refused (the device refuses
beyond 40% too). Not less: a fronted UW, common in modern English, is a
28% move in F2, and an earlier 25% cap refused exactly the kind of
thing this is for.

Tested with a synthetic speaker: our voice with a known accent (six
vowels moved), a 17% shorter tract and a 200Hz pitch.

| | true | fitted |
|---|---|---|
| tract size | 17% | 17% |
| IY (moved) | 260 / 2420 / 2980 | 264 / 2415 / 2974 |
| UW F2 (fronted 28%) | 1150 | 1159 |
| AE (moved) | 780 / 1720 | 819 / 1729 |
| AY start -> end F2 (moved) | 760 / 1250 -> 2050 | 780 / 1251 -> 2037 |
| EY (unmoved) | 480 / 2000 | 473 / 2004 |
| W (unmoved) | 290 / 650 | 284 / 658 |

Most values within 5%. Calibrating at our own 110Hz instead left F1
10-20% out on several vowels, which is why the second pass exists.

#### Consonant loci

A consonant has no steady formants; what it has is a LOCUS, the
frequency its transitions into a vowel point at -- and `phon.c`'s
consonant entries are loci. The **locus equation** finds a speaker's:
across many consonant-vowel pairs, F2 at the vowel's first voiced frames
against F2 in its middle falls on a line, `onset = k x middle + c`, and
the locus is where it meets the diagonal, `c / (1 - k)`.

A locus is not something the synthesiser reproduces one-for-one -- a
transition only partly reaches it -- so a ratio like the vowels' is
wrong. The fit is **analysis by synthesis**: our voice is rendered with
the speaker's fitted vowels in place (a locus equation depends on which
vowels follow), measured, rendered again with every locus moved 10%,
and measured again. The difference is how far each consonant's
transitions RESPOND to its table value, and the fit is the table value
that would reproduce the speaker. Each fit is repeated on two halves of
the data and kept only if they agree within 4%.

Tested with the synthetic speaker, three loci moved:

| | table | true | fitted |
|---|---|---|---|
| D F2 | 1700 | 1950 | 1956 |
| B F2 (unmoved) | 900 | 900 | 913 |
| G F2 (unmoved) | 1900 | 1900 | 1873 |
| K F2 | 1900 | 1650 | not fittable |
| M F2 | 1000 | 1150 | measured, not applied |

**K, T and P are not fittable** this way, and that is the measurement's
limit rather than the synthesiser's. After a voiceless stop the next
vowel's first 90-120ms are aspirated, and the formant transition runs
its course inside that aspiration -- as it does in real speech. The
measurement starts at the first VOICED frame, after the transition is
over, so there is nothing left to see. (An earlier version of this page
called it a defect in `phon.c`; reading the stop code showed otherwise.)
Fitting them would need formants measured in the aspiration noise,
which linear prediction does badly. **Nasals are
measured but not applied**: their fits were off by up to 12% and moved
between runs, because at a nasal onset the tracker sees the murmur as
much as the transition. Fricatives mostly come out "unstable" (the two
halves disagree), and keep the table.

`speech acoustics` uses 3,000 clips by default -- ample for a few dozen
phonemes -- and analyses them in parallel, since LPC per frame in Python
is the slowest step in the pipeline.

Fitted vowels change the cascade's gain, so a pack can make the voice
louder: a test accent clipped nine samples at the female settings,
which is why a smaller tract gets 2dB more margin (`phon.c`).

### The voice source

    ./tools/speech/speech source         # after acoustics
    ./tools/speech/speech build          # merged into PROSODY

Two numbers shape a voice as much as its formants, and both were fixed
in `synth.c`: the **open quotient**, the share of each pitch period the
vocal folds are open, and the **spectral tilt**, how steeply the source
falls off with frequency. A longer open phase and a steeper tilt are a
softer, breathier voice; a short phase and no tilt, a pressed and buzzy
one. Both are now settable (`synth_set_source()`, and the prosody
fields `open` and `tilt`), and both default to the old fixed values,
so nothing changes without a fit.

Measured in the middle of stressed vowels (`lib/source.py`):

- **H1-H2**, the first harmonic's level over the second's -- driven
  mostly by the open quotient;
- **H1-A3**, the first harmonic over the strongest one near F3 --
  driven mostly by the tilt.

How our voice responds (same sentences, male defaults):

| open, tilt | H1-H2 | H1-A3 |
|---|---|---|
| 35%, 0dB | -5.6 | 5.4 |
| **50%, 0dB (the default)** | **-2.5** | **11.0** |
| 65%, 0dB | 1.9 | 13.0 |
| 80%, 0dB | 7.2 | 14.7 |
| 50%, 9dB | -2.3 | 19.4 |
| 50%, 18dB | -1.2 | 26.7 |

Human voices typically read 0-5dB H1-H2 for men and 5-10dB for women,
so the default voice is **more pressed than any real one** -- which is
what "buzzy" has meant all along, now with a number on it.

The fit is analysis-by-synthesis: our voice is rendered at five
settings, at the speaker's pitch, tract size and vowels (recorded by
`speech acoustics`), a plane is fitted for each measure, and the two
are solved together for the speaker's pair. Tested against a synthetic
speaker with a known source:

| | true | fitted |
|---|---|---|
| open quotient | 68% | 68% |
| tilt | 8dB | 6dB |

The fitted source applies to both voices. LJSpeech's reader is a woman,
and a breathier source may suit the male voice less; `ZTTS_OPEN` and
`ZTTS_TILT` override it in `make wav` for comparison.

### Tuning by machine

Listening to every change does not scale, and nobody's ears are a
stable instrument over weeks. So intelligibility is measured by a
speech recogniser, and the voice is tuned against it unattended:

    pip install faster-whisper              # once; uses the GPU
    ./tools/speech/speech score             # how intelligible is this pack?
    ./tools/speech/speech tune --budget 150 # improve it, overnight if need be

**The material leaves nothing to guess.** A modern recogniser's language
model fills in words the voice never made clear, which hides exactly
what is being measured. So the test is **semantically unpredictable
sentences** -- grammatical, common words, meaningless ("A tall river will
send the question") -- and **minimal-pair words** spoken alone ("hat",
"cat"), where only the sound decides. Both are generated from our
lexicon and the corpus's word frequencies (`lib/evalset.py`), and split
into a **dev** set the tuner optimises against and a **held-out test**
set it never sees.

**The material has to be right.** The first real run's sentences
included "trys", "broughts" and "will rode" -- past tenses and irregular
verbs given an -s -- plus names used as nouns and "a equal". The
recogniser heard "tries" correctly and was marked wrong. Verbs are now
base forms whose "he ___s" form the corpus actually uses (which rules
out every irregular past without a list of them), names are the words
the corpus capitalises mid-sentence, numbers and -ing forms are out,
and the article follows the next word's first sound.

**Scored on sound, not spelling.** Words are compared by pronunciation,
so a recogniser writing "no" for "know" or "knew" for "new" is right.

**What it reports:** sentence word errors, isolated words right, and
which sounds were heard as which -- "M->B, N->D" says the nasals are
being taken for stops, which says where in the synthesiser to look.

**What the tuner may change** is limited to what a pack carries, each
within a range that is still a voice: timing, pitch movement, the
voice source, and the overall length of vowels and of consonants. It
is coordinate search: one parameter a step at a time, kept only if the
dev score improves by more than one word's worth, steps halved when a
whole pass finds nothing. Every evaluation is logged to
`build/tune/log.jsonl`.

**The verdict is the held-out score**, and the tuner says plainly when
there is none: in testing, a change that helped on 24 dev sentences
made the held-out ones worse, and the report said so rather than
offering the pack. Optimising against one recogniser can also teach the
voice things only that recogniser likes, which is why the reachable
parameters are bounded and physical, and why `score --asr sphinx`
exists as a second opinion.

Whisper `medium.en` needs about 2GB of GPU memory; an evaluation of the
default material (240 sentences and 160 words across both sets) takes
well under a minute, so a budget of 150 is an evening.

### Calibration uses a bare pack

Every calibration render -- `prosody`, `acoustics` and `source` alike --
means "our voice at its defaults", so it renders from the built pack
with every fitted section stripped: lexicon and letter-to-sound rules
only (`build/align/bare.zspk`). An earlier version rendered from the
built pack itself, which after one `speech build` carries the previous
run's fit, so each run calibrated against the last one's output. On the
first real corpus that moved the declination from 14 to 37 between two
runs on the same data, and made four pitch parameters unmeasurable.

F3 loci are measured and reported but not applied: on the same corpus
every consonant's came back 18-26% above the table, all the same way --
a bias (F3 barely moves at a vowel's onset, so its locus is poorly
determined), not an accent.

## Mirroring a source

`speech mirror` prepares one for publishing somewhere we control. It
downloads from the original, records its checksum, optionally cuts it
down, splits it into publishable pieces, and writes the provenance
beside them:

    ./tools/speech/speech mirror ljspeech --clips 2000

It does **not** publish -- that needs credentials -- it prints the
commands, including the `mirror_part` lines to paste into the spec.

**`--clips` is the useful part.** Phase 8 needs a few hours of speech,
not twenty-four: every diphone appears many times over well before the
end. 2,000 clips is roughly 300-500MB, fits a single release asset,
and is a download somebody will actually finish. Everything that is
not a clip -- the transcript, the readme -- is kept whatever the
number.

A mirror published in pieces is fetched with `--mirror` like any
other: each piece is downloaded (and resumed) on its own, and they are
joined only once all of them are present, so an interruption never
leaves a half-assembled file that looks complete. The join is
byte-identical to the file that was split.

## Open item: mirror the sources

**`fetch` downloads from the original sources, and that is temporary.**
An original can move, rate-limit, go offline, or change what it serves;
a build that depends on one is only reproducible until it does. The
checksums in `dist/en.spec` turn that into a loud failure rather than a
quiet substitution, but they cannot supply the bytes.

The plan is a Machdyne-controlled copy -- a `machdyne/speech-data`
repository, or the existing Ark LFS storage -- holding:

- the two Moby files as extracted,
- LJSpeech (or the subset actually used), as split release assets since
  the whole archive is over GitHub's 2GB per-asset limit,
- the Gutenberg book list,
- a provenance file repeating each licence statement and hash.

`dist/en.spec` already supports this: each source takes a `mirror`
line, with `mirror_kind` when the mirror packages things differently
(the GitHub copy of Moby is a tarball; Gutenberg's is a zip). Pointing
at a Machdyne mirror is a one-line change per source, plus swapping
which one is the default.

**Do this before phase 7**, when LJSpeech enters the picture. A 2.6GB
download that only works while one person's web host stays up is the
point at which "fetch from upstream" stops being reasonable.

The tooling for it is done (above); what remains is a Machdyne-held
copy and the `mirror_part` lines pointing at it.

---

## Provenance, and why it lives in the spec

`dist/en.spec` is the record of what went into a voice. Each source
carries its URL, its mirror, its licence statement in the words the
source itself uses, and a hash per extracted file. `speech sources`
prints it. A built pack carries the same information in its manifest,
so a card can be asked what it was made from.

The rule the whole pipeline follows: **public-domain inputs only, and
no existing speech synthesiser's code or data at any point.** See
[tts.md](tts.md), "Clean-room provenance", which this extends rather
than replaces.

---

## The recipe format

```
source moby
    url         https://www.gutenberg.org/cache/epub/3205/pg3205.zip
    mirror      https://codeload.github.com/Hyneman/moby-project/tar.gz/refs/heads/master
    kind        zip
    mirror_kind tar.gz
    take        moby-project-master/moby/mpron/mobypron.unc -> mpron.txt
    refuse      moby-project-master/moby/mpron/cmudict0.3
    sha256      mpron.txt eab1c6df...
    licence     public domain -- Grady Ward, grant of January 2001
    note        ...
```

- `kind` is `tar.gz`, `tar.bz2`, `zip`, `file` or `books`.
- `take` moves one member to a fixed name; a member ending in `/` takes
  everything under it, bounded by `--limit`.
- `books` fetches Project Gutenberg texts by `ids`.
- `note` lines are for whoever reads this in a year.

The Gutenberg copy of Moby is packaged differently from the GitHub one,
so its `take` lines and hashes are still to be filled in -- the first
fetch without `--mirror` reports exactly that, rather than failing
obscurely.
