# Speech data: `tools/speech`

The voice in `sw/apps/tts` is rules and tables. The rules that ship
today were written by hand and go only so far -- 41% of ordinary words
pronounced exactly right ([tts.md](tts.md)), which is not enough for
reading books. `tools/speech` is where better ones come from: it turns
public-domain sources into a small data pack that `tts` loads.

    tools/speech/
      speech               the CLI
      lib/spec.py          recipe parsing
      lib/fetch.py         resumable downloads, extraction, checksums
      dist/en.spec         the English voice: sources, licences, hashes
      .cache/              downloads (gitignored)

Same shape as [`tools/ask`](ask_app.md), and for the same reasons: one
command, recipes that are readable on their own, a cache that is never
committed.

**Zeitlos does not depend on this tool.** It ships the built pack, and
`tts` falls back to its built-in rules when no pack is present -- a
board with no sdcard still speaks.

---

## Status

Phase 4 of [tts.md](tts.md)'s plan, partly done.

| | |
|---|---|
| **done** | the tool, the recipe format, `sources` and `fetch`, provenance and checksums |
| **done** | the ZSPK pack format, `speech build`, the reader in `sw/apps/tts` and its tests, the card and release plumbing |
| **done** | the lexicon: 391,159 pronunciations from Moby, inflections included |
| **done** | letter-to-sound rules LEARNED from the lexicon, and their evaluator in `sw/apps/tts/pack.c` |
| **done** | alignment of a speech corpus (`speech align`) |
| **done** | prosody fitted to the alignments, calibrated against our own voice, shipped as a `PROSODY` pack section and read by the device |
| **done** | acoustics: vowel formants measured from the corpus, calibrated at the speaker's pitch and tract, shipped as a `FORMANTS` pack section |
| **done** | consonant loci by analysis-by-synthesis (voiced stops; nasals measured only) |
| **done** | the voice source: open quotient and spectral tilt, fitted by analysis-by-synthesis |
| next | listening: whether the fitted values, together, make the voice better |

---

## Using it

```
./tools/speech/speech build                       # -> build/en/speech.zspk
./tools/speech/speech show build/en/speech.zspk   # what is in one
./tools/speech/speech sources                     # what goes in, and under what terms
./tools/speech/speech fetch dist/en.spec          # from the original sources
./tools/speech/speech fetch dist/en.spec --mirror # from each source's mirror
./tools/speech/speech fetch --only moby           # just one
./tools/speech/speech fetch --only ljspeech --limit 400   # only 400 clips
```

Downloads go to `tools/speech/.cache` (override with `SPEECH_CACHE`)
and **resume**: an interrupted 2.6GB transfer continues with a range
request instead of starting again. Extraction takes only the members
the spec names, and every extracted file is checked against the
`sha256` line in the spec. A file with no hash yet is not an error --
its hash is printed so it can be pasted into the spec, which is how a
new source gets pinned.

---

## The pack

`speech build` writes `build/<voice>/speech.zspk`. **It is not
committed and never will be**: it is built from public-domain sources
by whoever cuts a release, and published with that release beside the
bitstreams. This repository holds the tools that make it, nothing more.
`build/` is gitignored for exactly that reason.

Today's pack, from Moby alone:

| section | size | |
|---|---|---|
| `MANIFEST` | 800 B | what it was built from, with licences and hashes |
| `PHONES` | 107 B | the phoneme names, in the order the ids use |
| `LTS` | 91 KB | trained letter-to-sound trees |
| `PROSODY` | 70 B | timing and pitch fitted to a speaker, when `speech prosody` has run |
| `FORMANTS` | 300 B | vowel formants fitted to a speaker, when `speech acoustics` has run |
| `LEXIDX` | 191 KB | the lexicon's block index |
| `LEXDAT` | 5.5 MB | 391,159 pronunciations |

**Nothing is resident.** `sw/apps/tts/pack.c` binary-searches the index
by SEEKING -- sixteen bytes a probe, about twenty probes -- and then
reads one block of a few hundred bytes. A word costs roughly a
kilobyte of reads, against a service that speaks a few words a second.
The alternative, holding a 191 KB index in memory, would not fit the
service's allowance and would buy very little.

Its shape, all little-endian:

- **Header:** `ZSPK`, version, section count, file size, CRC32, then a
  16-byte record per section (8-byte name, offset, length). Sections
  are found by name, so an older `tts` reads a newer pack and ignores
  what it does not know.
- **`LEXIDX`:** fixed 16-byte records -- a block's data offset, then
  twelve characters of its first word. Fixed so it can be searched by
  seeking; truncation is safe because the search only has to land in
  the right block, which is then scanned.
- **`LEXDAT`:** blocks of 32 entries, front-coded against each other.
  The first entry of a block shares nothing, so a block is readable on
  its own. Each entry is a shared-prefix length, the rest of the word,
  and one byte per phoneme: stress in the top two bits, phoneme id in
  the low six.
- **`PHONES`** is what makes those ids mean anything. `tts` maps the
  names onto its own table when it opens the pack and **refuses a pack
  naming a phoneme it does not have**, rather than speaking something
  else.

### On the card, and in a release

The card gets it at `/speech/en.spk` (8.3 names, so not `speech.zspk`).
`release/lib/mkfatimg.py` copies it **if it is there**, and prints a
note if it is not; a tree that has never run `tools/speech` still
builds a usable card. `SPEECH_PACK` points the release at a pack built
elsewhere.

`tts` opens it at startup, and everything it cannot answer falls
through to the built-in dictionary and rules. With no card at all,
nothing changes.

### Lookup order

1. the built-in dictionary (213 curated words: interface vocabulary and
   irregulars that must be exact);
2. the pack's lexicon, when there is one;
3. the pack's trained letter-to-sound rules;
4. the built-in rules in `lts.c`.

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

## Prosody

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

## Acoustics

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

### Consonant loci

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

## The voice source

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

## Tuning by machine

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

## Calibration uses a bare pack

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
