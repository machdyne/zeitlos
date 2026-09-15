# ask

`sw/apps/ask` — answers a question from the datasets shipped with the
machine.

It is not a chatbot and it does not write prose. It finds the passages
in the corpus that answer a question, shows a preview built from those
passages verbatim, and hands the document to `read` when you want the
whole thing.

```
+- ask ---------------------------------------------------------------+
| > water from a stream, no filter                                     |
|                                                                      |
| FM 21-76 Survival > Water Procurement                           .91  |
|   The most common method of purifying water is boiling. Bring it     |
|   to a rolling boil for one minute...                                |
|                                                                      |
|   2. Drinking water > Treatment                                 .84  |
|   3. The Art of Travel > To purify water that is muddy          .77  |
|                                                                      |
| ENTER read   1-9 open   TAB browse   ESC cancel                      |
+----------------------------------------------------------------------+
```

---

## The rule

**`ask` finds text. It does not write text. Every word it shows is
verbatim from the corpus, attributed to a named dataset, and one
keypress from its source.**

Note what that does *not* claim. Some of the corpus is itself
LLM-generated — the Ark Scroll and the Ark Codex both are, and Ark
describes the Scroll as exactly that. So "not written by a machine"
would be false, and the property that actually holds is weaker and more
useful: **`ask` never adds anything.** What it shows is what is on the
card, unchanged, with its origin attached.

That is a constraint in the code, not a tendency. The preview is
assembled from whole sentences taken byte-for-byte out of the passages
that ranked, never paraphrased, and never stitched across two sources
in one block — two sources joined together *reads* like synthesis even
when every word is quoted, and a user who cannot tell which parts are
trustworthy has lost the only thing this app offers over a search box.

The reason is not philosophical. A machine meant to be useful in fifty
years with no internet must not be able to confidently make things up
about tourniquets or water purification. A model small enough to run
here is exactly the size that produces fluent, plausible, wrong text.
Declining to generate is the feature.

### Which is why model-written datasets are marked

A corpus that is itself model-written has the same failure one step
removed, and hiding that would undo most of what this design buys.

A recipe names them:

```
generated = codex, scroll
```

and the flag reaches three places: `DS_GENERATED` in the pack index's
per-dataset record, a line at the top of that dataset's generated
`index.md`, and the manifest.

```
- [books](books/index.md) -- 87 documents
- [codex](codex/index.md) -- 363 documents  *(model-written)*
- [scroll](scroll/index.md) -- 96 documents  *(model-written)*
```

It is named in the recipe rather than inferred, because it is a fact
about the upstream corpus that no amount of looking at the text can
establish.

Somebody deciding whether to trust a passage on water purification
should be able to see whether it came from FM 21-76 or from a summary
of one. `aidx.c` does not read the per-dataset flag yet — the format
carries it and the app should show it in the result line, which is the
next small thing to do.

### Why this is possible at all on a 48MHz machine

An LLM is a lossy compression of a corpus, and hallucination is the
decompression artefact. `ask` keeps the corpus. Megabytes of text that
people wrote sit on the card, verifiable, and the model never stores a
fact — it only computes *where to look*.

You cannot compress useful knowledge into a few million parameters.
You can build a pointer into a corpus with them, because understanding
a question is a far smaller problem than knowing everything. Knowing is
delegated to storage, which is cheap and reliable. The neural part is
spent only on the part that needs intelligence.

### Not "search"

`ask` does not index the user's files, the card, or anything the user
put there. It answers from **the datasets in the installed
distribution** and nothing else. A result always names its dataset, and
"no passage in this corpus answers that" is a legitimate and common
answer.

---

## Architecture: a server with a client attached

`ask` follows `web`'s shape (`sw/common/zweb.h`): the app that draws
the window and the service other processes call are the same process,
and the service is addressed by registered name rather than by a fixed
pid.

```
    any app  --Z_ASK_QUERY-->  ask0  --> index, corpus
             <--Z_ASK_RESULT--
```

Three reasons this is a service and not a library:

- The resident index is **1.5MB at the smallest tier** and several
  megabytes at the largest. Linking that into every caller is not an
  option; there is one copy and one owner.
- Loading it costs seconds off the card. A service pays that once at
  startup, not per caller.
- `read`, `files`, `repl` and the shell all have reasons to want an
  answer, and none of them should contain a retrieval engine.

The full protocol is in `sw/common/zask.h`. The short version:

| | |
|---|---|
| `Z_ASK_QUERY` → `Z_ASK_RESULT` | ask a question, get ranked hits |
| `Z_ASK_PREVIEW` → `Z_ASK_TEXT` | the bytes of one hit |
| `Z_ASK_BROWSE` → `Z_ASK_LIST` | the dataset/title tree |
| `Z_ASK_CANCEL` | abandon a query in flight |
| `Z_ASK_INFO` → `Z_ASK_INFO_REPLY` | what distribution is installed |

---

## Responsiveness

Zeitlos is a responsive system and `ask` must not be the app that
breaks that promise. A few seconds for an answer is fine. A frozen
machine for a few seconds is not.

**The scan is incremental and interruptible.** The coarse pass over the
vector array is chunked into slices of `Z_ASK_SCAN_SLICE` vectors. The
app returns to its message loop between slices, so the window redraws,
the progress bar moves, `ESC` cancels, and `wm` keeps getting its
messages. Nothing in `ask` ever holds the CPU across a whole query.

This is the same discipline `docs/app_runtime.md` records for every
other app in the tree under "Every app now yields", and for the same
reason: a process that spins is a process that makes the whole desktop
feel dead.

**Cancellation is immediate and cheap.** The scan holds no lock and
allocates nothing per slice, so `Z_ASK_CANCEL` just stops. A query the
user has already given up on never costs another cycle.

**Progress is honest.** The bar tracks vectors scanned, which is a real
fraction of a known total, not a guess.

---

## Packs, and what is in them

Packs are named after **Ark's own tiers** — Scroll, Lite, Medium,
Heavy — rather than a parallel scale of our own, because two size
vocabularies for the same content is a reliable way to end up with a
`medium` that is not Ark Medium.

| pack | contents | text |
|---|---|---|
| `zdocs` | Zeitlos `docs/` | 1.7 MB |
| `arkscrl` | Ark Scroll R1 | 0.4 MB |
| `arklite` | Ark Codex + selected texts + Scroll | 18 MB |
| `arkmed` | **Ark Medium** — Wikipedia 10K vital, Gutenberg CD-ROM, CIA Factbook, MedlinePlus, plus Lite | hundreds of MB |

`zdocs` is deliberately its own pack rather than a component of the
others. `docs/` changes every release and Ark changes once in a while;
since a pack owns its documents, bundling them would mean rebuilding
the whole Ark corpus to pick up a documentation edit. It is also small
enough that a Zeitlos release could reasonably ship it by default.

`arkmed` is the largest set we intend to support. Ark sizes Medium for
a 4GB partition or a DVD, which is an ordinary microSD card.

### Fetching `arkmed`

```
./tools/ask/ask fetch dist/arkmed.spec     # ~400MB, once
./tools/ask/ask build dist/arkmed.spec
```

`ask fetch` does the downloads itself rather than shelling out to Ark's
`scripts/build.sh`. Not because that script is badly written — because
it is meant to be *sourced* (`. scripts/build.sh`, which leaks
`$BUILD`/`$ARK`/`$DATA` into the caller's shell), it writes into the
ark clone that `lib/fetch.py` manages and may re-checkout, and it has
no `set -e`.

That last one matters. As of ark `2650ce9` the script has two bugs that
leave Ark Medium incomplete and silent about it:

- `wget -nc -P $BUILD/ciaimg.zip <url>` — `-P` is
  `--directory-prefix`, not `--output-document`, so this creates a
  *directory* named `ciaimg.zip` and `7z` is then handed a directory.
  **The Factbook images are never extracted.** The line above it
  correctly uses `-O`.
- `cp -R data/scroll.gz $ARK` — there is no `data/scroll.gz`; the repo
  has `scroll-r0.gz` and `scroll-r1.gz`. **Medium gets no Scroll at
  all.**

Both fail non-fatally and the build looks like it worked, which is how
they survived. They are upstream's to fix; the recipe here does the
same downloads declaratively so a pack build does not depend on the
script being correct on the day someone runs it.

What `ask fetch` adds: resumable transfers into a `.part` file that is
only moved into place once complete, optional `sha256=` pinning, a real
exit code, and a git-lfs pointer check that says what to install rather
than letting `tar` fail with `File format not recognized` several steps
later.

A source that has not been fetched yet fails with the command to run,
not with an empty pack.

### Images

The Factbook's country maps and flags become `maps.md` and `flags.md`
in the pack root — plain markdown indexes of links:

```
# Flags

8 images. Links open in `view`.

- [Brazil](flags/00000001.gif)
- [Chad](flags/00000002.gif)
```

`read` resolves the relative link and hands off by extension through
`ztype.h`, so a click opens `view` with no further plumbing. Validated
across a built `arkmed`: 1,878 links in generated markdown, zero
dangling, 1,861 routing to `read` and 17 to `view`.

**Images are transcoded to GIF on the host.** `sw/common/zimg.h`
compiles PNG *out* by default — `Z_IMG_HAVE_PNG` is 0, because its
inflate window alone is 32KB of `.bss`, more than every other decoder
put together. A card full of PNGs would give "this format is not built
in" on every one, and fixing that on the device to accommodate a
host-side choice is the wrong direction.

GIF rather than the other three that *are* built in: JPEG rings badly
on the hard edges of flags and line-art maps, and BMP and PNM are
uncompressed at ~300KB per image. GIF is LZW-compressed and
palette-based, which is exactly what flat-colour art wants. They are
also downscaled to 640×640 and quantised to a grey palette here, since
`view` decodes into a fixed 640×480 1bpp document and dithers anyway —
anything larger is decode work thrown away.

**Images are not retrievable.** An image has no text, so no embedding
and no chunk, and `ask` will never return one as a hit. Classification
into maps vs flags is by filename pattern and is a guess — the
archive's naming is not documented — so anything matching neither lands
in `images.md` rather than being dropped.

### Sizing

The resident cost is what matters, not the card total. The coarse index
is `coarse_dim` bytes per chunk, so at 32 dimensions 250,000 chunks is
8MB resident — comfortable on a 32MB board, and also the point where
the coarse scan stops being free in software (~8M MACs, about four
seconds) and starts being the reason to build `rtl/zml.v`.

Real build output, `arklite` + `zdocs` against the actual repos:

```
  1590 documents, 13211 chunks
  card tree: 21.81 MB
  offsets verified: 13211 chunks resolve exactly
  generated 6 index.md, so the corpus is browsable in `read` without `ask`
  resident on device: 2.55 MB
```

### How packs work

A distribution is not one monolithic thing. It is a set of **packs**,
each self-contained, each installed by unzipping it onto the card:

```
/ask/arklite/index.zak      Codex + Scroll R1 + docs
/ask/arkmed/index.zak       Ark Medium
/ask/recipes/index.zak      something you built yourself
```

`ask` enumerates the subdirectories of `/ask` at startup and queries
every pack it finds. **Adding a pack rebuilds nothing** and removing
one is `rm -r`. This is the structure that matters most for what this
is for: the corpus will keep growing, from sources that are not Ark and
not this repository, on schedules nobody controls.

Results from different packs combine **by rank, not by score**. Two
packs may have been built with different encoders, whose vectors live
in different spaces and whose dot products are not comparable.
Reciprocal rank fusion consumes orderings and does not care — it is
already how the lexical and dense halves are fused inside one pack, so
combining packs needs no new machinery. Each pack records an
`encoder_id` (a hash of the weights, not the backend name) so `ask`
loads one copy per distinct encoder.

#### A pack owns every byte it indexed

Documents live under the pack that built them:

```
/ark/<pack>/<dataset>/00000042.txt
```

That costs duplication — two packs that both include `docs/` ship two
copies — and it buys two things that are not negotiable.

**Packs cannot collide.** `arklite` ships Scroll R1 and `arkmed` ships
Scroll R0; both are the dataset named `scroll` and both number from 1.
Sharing `/ark/scroll` would have one silently overwrite the other. This
was not hypothetical — it was found by co-installing two packs onto one
card and looking at the result.

**A pack must not index files it does not own.** The obvious economy is
to point the index at the `/docs` that `tools/mkfatimg.sh` already
writes. But `docs/` changes every release and a pack is rebuilt once in
a while — that is the entire point of separating the two cadences — so
the next release would shift every byte offset in an installed pack and
nothing anywhere would say so. Every preview would quietly show the
wrong paragraph. The shared `/docs` stays where it is for `read` and
`files`; `ask` simply does not point at it.

---

## Where things live on the card

The whole card, with who owns each part. **Nothing `ask` writes
overlaps anything the release writes**, which is what lets the two
ship on separate schedules.

```
/                          owner          cadence
  zeitlos.cfg              release        every release
  apps/                    release        every release
    ask                      "            the app binary
    read, files, text, ...   "
  docs/                    release        every release
  audio/                   release
  user/                    the user       never touched
  libz/                    release

  ark/                     A PACK         when its corpus changes
    arklite/                 "            one directory per pack
      index.md               "            generated, links to datasets
      codex/
        index.md             "            generated, links to documents
        00000001.md ...      "            the corpus itself
      scroll/
      docs/                  "            the pack's OWN copy -- see below
    medium/                  "            a second pack, installed later
      index.md
      medline/
        index.md
        00000001.md ...

  ask/                     A PACK         same cadence as its /ark tree
    arklite/
      index.zak              "            root: dims, counts, encoder_id
      docs.zdt               "            card path, title, uid
      chunks.zct             "            doc, byte offset, length, heading
      coarse.zcv             "            resident, scanned every query
      fine.zfv               "            on card, read for the shortlist
      lexicon.zlx            "            term dictionary, resident
      post.zlp               "            postings, read per query term
      encoder.zmd            "            the query encoder
    medium/
      ...
```

A pack is exactly `/ark/<pack>/` plus `/ask/<pack>/`. Installing one is
unzipping it; removing one is `rm -r` on those two directories. Nothing
else on the card refers to it.

Measured across a co-installed `arklite` + `arkmed` card: 1,948 paths,
no collisions, longest path 32 characters — 43 of `Z_WM_ARG_MAX`'s 96
bytes once a ten-digit `#offset` fragment is appended.

### Everything is `.md`, including plain text

`sw/common/ztype.c` maps `MD` to `read` and `TXT` to **`text`** — the
editor. A corpus emitted as `.txt` would have every link in a generated
index open an editor on a 344KB book, and `files` would do the same on
a double-click. `read` is the right viewer for all of this: it renders,
it follows links, it indexes lazily and it has no maximum file size.

The cost is that plain-text sources render through the markdown parser.
In practice that is mild and often an improvement — the Codex summaries
are already markdown, MedlinePlus is clean prose, and a field manual's
`*` bullets become real bullets. The visible artefact is Gutenberg's
indented passages, which `md.c` reads as code blocks and draws in the
body font anyway.

### Generated indexes, so the card works without `ask`

The numbered filenames are unreadable by design, and `ask` is the
intended way around that. It should not be the *only* way around it: a
card is a physical object that outlives the software on it, and a
directory of eight-digit files with no key is a corpus nobody can
recover by hand.

So every directory gets an `index.md` of links:

```
# codex

363 documents. Links open in `read`.

- [Abstraction](00000001.md)
- [Academic journal](00000002.md)
- [Active transport](00000003.md)
```

`read` already resolves a relative link against the directory of the
file it is showing (read.c:2480) and hands off by extension through
`ztype.h`. So the whole corpus is navigable with **nothing but `read`**
— no index, no encoder, no `ask` at all — and `files` reaches it too.

These are written after chunking and are never themselves indexed.
They are navigation, not corpus; indexing a page of links would put a
list of titles into the retrieval results.

### Handing a document to `read`

**Yes — any byte offset, in any text file, of any size, rendered
correctly.** The machinery already exists and is already exercised.

`sw/apps/read/read.c` builds a **lazy sparse index**: every
`IDX_STRIDE` source lines it checkpoints the byte offset, the line
number, and the markdown parser state (`md_state_t`). Saving the parser
state is what makes a jump into the middle of a document render
correctly instead of resuming in the wrong block — land inside a fenced
code block without it and every `#` in the code becomes a heading.

`line_at_offset(uint32_t off)` at read.c:800 already converts a byte
offset to a source line, extending the index as needed. It is not a
function written for this — the scrollbar drag calls it at read.c:2815.
`read` also applies no extension filter, so `.txt` and `.md` take the
same path and Gutenberg books work.

So `ask` hands over a path with a fragment:

```
/ark/lite/books/00000009.txt#48213
```

**The only change `read` needs** is to parse an optional `#<decimal>`
off the launch argument and call the function it already has. A `read`
without that change still opens the file, just at the top, so the two
can be flashed independently.

#### The one real cost

`extend_index()` never moves backwards, so opening a document at byte N
means streaming N bytes off the card first. `docs/sdcard.md` measures
604 KB/s through FatFs and that is an optimistic figure; at a realistic
300–600 KB/s, a hit in the back of an 8MB Bible is a ten to thirty
second stare at nothing.

The fix is upstream of the device, not in `read`: the `books` adapter
takes `split=<bytes>` and breaks large works into separate documents at
chapter boundaries, falling back to blank lines to *guarantee* the
bound rather than merely prefer it. Preferring headings is a nicety;
bounding the size is the requirement, because a book with four `BOOK I`
markers and no chapter markers under them otherwise produces four
700KB pieces — which is exactly the case the option exists to avoid.

With `split=262144` the largest document in `arklite` is 344KB, about a
second worst case. The build reports anything still over `seek_warn`.

Splitting also **measurably improved retrieval** — dense went 7/12 to
8/12 at rank 1 — because smaller documents carry tighter heading paths
into the embedding.

---

## Shipping

**A distribution is not part of a release.** Zeitlos releases go out
monthly; a corpus is rebuilt when the corpus changes and a model is
retrained when there is a reason to, perhaps once a year. Tying the two
would mean either rebuilding a 500MB corpus every month or freezing
releases behind a training run.

So `tools/ask/ask build` produces a **self-contained directory tree**
that is unzipped onto a card. It carries its own version, its own
manifest and its own `dsid` (dataset id, the low 32 bits of a hash over
the recipe and every document's bytes). Every index file carries that
`dsid` in its header, and `ask` refuses to run if they disagree — a
`coarse.zcv` from one distribution against a `chunks.zct` from another
would otherwise produce confidently ranked results pointing at the
wrong paragraphs, which is the worst failure this system has.

`release/` ships the app **and `zdocs` + `arklite` by default**. An app
that boots to "no packs in /ask" looks broken rather than incomplete,
so the data goes with it.

| pack | files | size |
|---|---|---|
| `zdocs` | 92 | 2.4 MB |
| `arklite` | 547 | 21.4 MB |
| **total** | 639 | **23.8 MB of a 64 MB image (37%)** |

Override with the environment — and setting it *empty* is distinct from
not setting it:

```
ZEITLOS_ASK_PACKS="zdocs arklite arkmed"    # add one
ZEITLOS_ASK_PACKS=                          # ship none
```

**A release now depends on those packs being built**, which is a real
coupling. `zdocs` builds from this tree alone in seconds; `arklite`
needs the ark clone, which `lib/fetch.py` caches after the first time.
A pack that is not built is an error naming the command to build it,
not a quiet omission.

Both checks happen in preflight, before anything is formatted, because
this file already documents both failure modes for apps: a missing
input found after the image is half written, and running out of room
"halfway through a 64MB image, with mcopy's own silence for an error
message". `arkmed` will refuse there and say to raise `SIZE_MB`. `ask` with no distribution installed says so on one
line and offers `browse` over `/docs`, which is always there.

| | cadence | versioned by |
|---|---|---|
| Zeitlos release | monthly | `v0.0.N` |
| a pack | when its corpus changes | recipe `version` + `dsid` |
| the encoder | when retrained | `encoder_id` in each pack's `index.zak` |

Because a pack owns its documents and its index together, and records
the upstream commit it was built from, these three can move completely
independently. That is why `docs/` is copied into a pack rather than
referenced.

---

## Software first, hardware later

`ask` is **software-only today and will always work software-only.**
The accelerator is an optimisation, not a dependency, and a board that
cannot fit it loses seconds rather than the feature.

The costs, at the `arklite` tier:

| Stage | Work | Software | With a MAC engine |
|---|---|---|---|
| Encode query (`bow`) | ~10K MACs | negligible | negligible |
| Coarse scan, 4,698 × 32 | 150K MACs | ~0.08 s | ~0.01 s |
| Lexical, ~5 terms | postings read | ~0.1 s | ~0.1 s |
| Re-rank top 256 × 128 | 33K MACs | ~0.02 s | ~0.01 s |

At this tier the whole thing is already fast enough in software and the
hardware buys little. That changes with corpus size: the coarse scan is
linear in chunk count, so at 250,000 chunks it is 8M MACs — about four
seconds in software against roughly half a second accelerated.

**The coarse scan is the accelerator's workload** and it is the easiest
one imaginable: a contiguous `int8` array, read once, no branches, no
random access, no dependency between rows. One descriptor, one
streaming read. It needs less state than `rtl/montmul.v`, which is 983
LUT4 with **no BRAM**, so it should fit an ECP5-25F (Lakritz) with
nothing to spare and go faster on a 45F (Mozart ML1) with a wider array
and a scratchpad. That is a `boards.vh` define, a `CSR_FEATURES2` bit
and a self-test-then-fall-back check on the software side — `web`'s
arrangement for montmul, for the same reason.

None of that is built yet. See "Status".

---

## Retrieval: measured, not assumed

Both halves are real and both are necessary.

**Lexical (BM25).** Wins where an exact token is the point: `lakritz`,
`0x7000_0600`, `nextpnr-ecp5`, a drug name. Embeddings blur a rare
string toward whatever it resembles; BM25 does not.

**Dense.** Wins where the words differ from the text: "my water might
be contaminated" finds the boiling passage that never says
"contaminated".

**They are fused with reciprocal rank fusion** — scale-free, one
constant, and it does not require BM25 scores and `int32` dot products
to be comparable, which they are not.

### Three query styles, and the dense half loses all three

The gold set is 127 questions in three tagged halves, because people do
not type one way:

- **`direct`** — a question using the words the answer uses.
- **`paraphrase`** — a situation in the words somebody reaches for
  *before* knowing the term. "my hands went white and numb out in the
  cold" must find the frostbite passage without sharing a word.
- **`keyword`** — two or three words, no grammar. `ask`'s input box is
  forty columns; this is probably the commonest style in practice.

Untrained `bow`, on arklite:

| | lexical @1 | dense @1 | hybrid @1 |
|---|---|---|---|
| `keyword` (23) | **21/23 — 91%** | 10/23 | 16/23 |
| `direct` (53) | 39/53 | 30/53 | 39/53 |
| `paraphrase` (36) | 13/36 | 7/36 | 9/36 |
| **all (112)** | **73/112** | 47/112 | 64/112 |

**BM25 alone beats the fused result in every style.** Fusion costs nine
questions at rank 1 overall and five of twenty-three on keywords, where
BM25 is at 91% and has nothing to gain from a second opinion.

The keyword result is structural rather than bad luck. A `bow` query
vector is the idf-weighted mean of its term vectors, and averaging two
or three is a far noisier estimate than averaging the ten or twelve a
written question supplies. There is no syntax for the encoder to
recover, because there was none.

### More questions flipped the sign

The same sweep at two levels of genq coverage, dense @1 against its own
untrained baseline:

| imported questions | baseline | trained | |
|---|---|---|---|
| 7,763 (~20% of the corpus) | 30/53 | 25/53 | **−5** |
| 16,621 (~42%) | 37/89 | 42/89 | **+5** |

Doubling the training questions turned training from actively harmful
into a gain. Hybrid at rank 1 went 48/89 → 52/89 at two epochs, with a
paired bootstrap of +4 [−2, +10], P=0.83.

That does not clear the usual 0.95 bar and should not be reported as
if it did. But one and two epochs BOTH improve (P=0.76 and 0.83) and
four, eight and sixteen all get worse, monotonically — a fluke would
not be ordered like that and then reverse cleanly. The 8-epoch row at
P=0.05 is the same evidence read the other way: overtraining hurts, and
it hurts reliably.

The sweep now also breaks the change down **by question style**, which
is the measurement that actually decides whether to keep the dense
half. A gain concentrated on `direct` or `keyword` is the encoder
getting better at a job BM25 already does better, and is not worth
0.9MB resident. A gain on `paraphrase` is the encoder doing the one
thing only it can do.

### The decision, and how it came out

**`dense = no`. `ask` ships BM25 only.**

The pre-registered rule required a paraphrase gain of +4 or more. The
full sweep, on 39,395 generated questions over the whole corpus,
produced at best **+3**. The rule failed on its own terms.

It then failed a second, more generous test. The dense half genuinely
improved with full coverage — trained at one epoch it gained **+7 at
rank 1** on its own, the largest effect measured anywhere in this
work — but the fused output moved by one question. So the fusion weight
was swept, with no retraining, to find out how much of that gain was
recoverable:

| w_dense | @1 | @3 | @10 | direct | keyword | paraphrase |
|---|---|---|---|---|---|---|
| **0** | **73/112** | 88/112 | **100/112** | 39/53 | **21/23** | **13/36** |
| 0.25 | 69/112 | **89/112** | 98/112 | 41/53 | 19/23 | 9/36 |
| 0.5 | 68/112 | 84/112 | 98/112 | **42/53** | 18/23 | 8/36 |
| 1 | 67/112 | 80/112 | 93/112 | 40/53 | 17/23 | 10/36 |
| 2 | 57/112 | 72/112 | 84/112 | 36/53 | 13/23 | 8/36 |
| 4 | 55/112 | 67/112 | 84/112 | 35/53 | 12/23 | 8/36 |

Monotone toward zero, and w = 0 is lexical alone. **No fusion weight
beats not fusing.** Lexical wins overall at rank 1 and rank 10, and
wins both styles that matter — keyword 21/23 against 19, and
paraphrase 13/36 against 10.

Paraphrase is the one that settles it. That is the case the dense half
exists for, and BM25 is *better at it* than any hybrid configuration
tried.

### What that buys

`coarse.zcv` and `encoder.zmd` are the only resident structures, so
dropping them takes an `arklite` pack from 0.90MB resident to
**nothing**. No `Z_PROC_STACK_SIZE_HUGE`, no 4MB allocation, no
training step, and the app would fit a 1MB board. For better answers.

### What was learned, and what is still there

The negative result is worth as much as a positive one would have been,
and it took a specific sequence to reach honestly: a gold set that had
to be caught favouring BM25 by construction, a second half of questions
written to test the case dense retrieval exists for, a third for the
keyword style that turned out to be BM25's strongest, a significance
test to stop noise being read as signal, and a decision rule written
down before the data.

Without any one of those, "hybrid 52/89, best at 2 epochs" would have
shipped as a win.

Nothing is deleted. `encoder = trained` and `dense = yes` still work,
`ask genq`/`train`/`eval --fuse-sweep` still measure, and the device
still reads a pack with vectors if one is installed. The bar for
turning it back on is now precise and published: **beat 13/36 on
paraphrase without losing keyword.** A better encoder — a trained
transformer rather than a bag of embeddings, or an asymmetric setup
with a real teacher on the document side — is a live option, and this
is the harness to evaluate it with.

### The decision, written down before the data

Recorded in advance deliberately. Once a sweep is on the screen it is
easy to find a reading that justifies whichever answer one already
preferred, and the effects here are small enough that a reading can
always be found.

Re-run after genq completes:

```
./tools/ask/ask train dist/arklite.spec --sweep 1,2,4,8,16
```

with the full 127-question gold set. Then:

**Keep the dense half** (`dense = yes`, `train_epochs` = whichever of 1
or 2 wins) if BOTH hold:

- `paraphrase` hybrid@1 improves by **+4 or more**, and
- `keyword` hybrid@1 does not fall by more than **1**.

The first is the only job the encoder has that BM25 cannot do. The
second is the guard: keyword queries are BM25 at 91% and the commonest
style in practice, so a paraphrase gain paid for by keyword losses is
not a gain.

**Ship `dense = no`** if the paraphrase gain is under +2, or if
keywords regress by 2 or more. That is not a failure — it is a 0.9MB
saving, no HUGE tier, no training step, and a pack that fits a 1MB
board.

**Anything between those** is undecided, and the answer is more gold
questions rather than a judgement call. Paraphrase is 36 questions;
doubling it costs an evening and would resolve a ±3 effect that
currently cannot be resolved at all.

One thing that is already settled regardless: **4 or more epochs is
worse than not training**, consistently, at every coverage level
measured. If the winner comes back as 8 or 16, something changed and
that is the thing to look at first.

### `dense = no`

So a recipe can ship a pack with no vectors and no encoder:

```
  dense half OFF -- lexical only, nothing held resident
  resident on device: 0.00 MB
  fits kernel tier:   BIG (needs 0.10 MB incl. stack headroom)
```

against 0.90MB and a HUGE tier with the dense half on. `coarse.zcv` and
`encoder.zmd` are the *only* things `ask` holds resident, so dropping
them removes essentially all of it — the dictionary is binary-searched
on the card and postings are read per term.

**That is a different machine.** No 4MB allocation, no HUGE tier, no
training step, and it would fit a 1MB board. For better answers.

The argument for keeping the dense half is `paraphrase`, where
everything is weak (36% at best) and there is real headroom. That is
the case a trained encoder has to win, and until it does, `dense = no`
is the honest default.

### The gold set was rigged toward BM25, and I built it that way

53 of the first 63 questions shared a literal term between the question
and the regex defining a correct answer. That is exactly what BM25
ranks on, so a passage could only count as relevant if it contained a
word the query also contained — the metric rewarded term matching by
construction.

Not purely artificial: people really do type the right words most of
the time. But it systematically under-samples the one case dense
retrieval exists for, which is when they do not.

So the set is now **99 questions in two tagged halves**, and `ask eval`
reports them separately:

| | lexical @1 | dense @1 | hybrid @1 |
|---|---|---|---|
| `direct` (53) | 39/53 | 30/53 | 39/53 |
| `paraphrase` (36) | **13/36** | 7/36 | 9/36 |

`paraphrase` questions describe a situation in the words somebody
reaches for *before* knowing the technical term — "my hands went white
and numb out in the cold" must find the frostbite passage without
either sharing a word.

**Everything collapses there.** Lexical drops from 74% to 36%, dense
from 57% to 19%. This is the half of the problem the system does not
solve, and it was invisible until the questions existed.

It also changes what the dense half is for. On `direct` questions BM25
is unbeatable and fusion is a net negative below rank 1. On
`paraphrase` the whole field is weak, so there is somewhere to go —
and that is where a trained encoder has to prove itself.

### The numbers, and what they say

`ask eval` against a hand-written **63-question** gold set, on
`arklite` + `zdocs` (1,590 documents, 13,211 chunks). Rank of the first
relevant hit:

| | @1 | @3 | @10 |
|---|---|---|---|
| lexical | 42/63 | 55/63 | **62/63** |
| dense (`bow`) | 40/63 | 52/63 | 55/63 |
| coarse only (32-dim) | 16/63 | 25/63 | 44/63 |
| hybrid | **43/63** | 51/63 | 59/63 |

**Read that honestly. Hybrid wins by one at rank 1 and loses at ranks
3 and 10.** BM25 alone puts the answer in the top ten for 62 of 63
questions; fusing the untrained dense half *costs* three of them.

The gold set was twelve questions before this and said hybrid 8/12
against lexical 9/12 — the right shape, but the run-to-run variation
from a chunking change was the same size as the effect. At 63 the
statement is sharp enough to act on: **a trained encoder has to beat
42/63 at rank 1 and 62/63 at rank 10, and today's untrained one does
neither.**

That is the whole reason the harness was built before the encoder.
Anyone who skipped measuring would have shipped the dense half on the
strength of it being the interesting part.

### Shortlist depth, which is a sizing number

The two-stage design only works if the coarse pass keeps the right
chunk in the shortlist the fine pass re-ranks. If it does not, no
fine-stage accuracy recovers it — the answer was thrown away before
anything good looked at it.

```
  top 32     48/63  ##############################
  top 64     52/63  #################################
  top 128    54/63  ##################################
  top 256    60/63  ######################################
  top 512    62/63  #######################################
  top 1024   63/63  ########################################
```

So the shortlist is **512**, not the 256 assumed earlier. That is
512 × 128 bytes = 64KB of fine vectors read per query, about 110–220ms
at a realistic 300–600 KB/s — and it is a floor set by measurement, not
a knob.

It also says something uncomfortable about the coarse stage: a 32-dim
truncation of an untrained `bow` vector is a weak filter. Either
`coarse_dim` grows, or the encoder is trained with Matryoshka losses so
truncation degrades gracefully, or the shortlist stays deep. The third
is free today and the second is the right answer.

### Chunking earns its keep

Every chunk is embedded with its heading path prepended:

```
Survival > First Aid > Bleeding: If you cannot remember the exact
location of the pressure points...
```

The heading is **not** part of the stored byte range — it costs nothing
on the card and nothing at query time. Without it, a mid-document
paragraph is embedded with no idea what it is about, which measurably
wrecks retrieval on long manuals.

---

## Generating questions with a local LLM

Training on Inverse Cloze pairs moved gold-set recall by **one
question out of 53**, because ICT teaches the model to match
declarative documentation prose to its own paragraph — which is not
the task. Nobody types *"The blitter is its own bus master."* They type
*"how do I delete a file"*.

The bottleneck was never the optimiser. It was that there were no
questions. `ask genq` produces them:

```
./tools/ask/ask genq  dist/arklite.spec --model qwen3.8:27b --limit 50
./tools/ask/ask genq  dist/arklite.spec --model qwen3.8:27b
./tools/ask/ask train dist/arklite.spec
./tools/ask/ask eval  dist/arklite.spec
```

**Start with `--probe`.** It runs one passage and shows the raw reply,
the token count, the tok/s and an estimate for the full corpus. A bad
model choice otherwise costs hours before anything is visible.

```
$ ./tools/ask/ask genq dist/arklite.spec --probe -m qwen3.5:9b
  passage: The Magna Carta (+0, 1863 chars)
  12.4s wall, 400 output tokens, 32.2 tok/s
  *** hit the 400-token cap -- the reply was CUT OFF. ***
      Almost always a reasoning model emitting <think>.
```

### Reasoning models

The qwen3 family, deepseek-r1 and openthinker emit a `<think>` block of
thousands of tokens before answering. For a reply that is four short
questions, that is minutes per passage instead of seconds — and the
reply usually hits the output cap mid-thought, so nothing parses.

`genq` sends `think: false`, caps output at 400 tokens, pins
`num_ctx` to 4096 and sets `keep_alive` so the model is not reloaded
between passages. Older ollama builds that reject `think` are retried
without it. If failures still pile up, the run says so:

```
*** every failure hit the 400-token cap. This model is emitting a
    reasoning block. Try a non-reasoning model (llama3.2, gemma) ***
```

The reply is also constrained to a JSON schema, which removes the
fences and commentary entirely on any ollama new enough to support it.

### Which model

The task is comprehension and rephrasing, not reasoning: read a
passage, say what it answers. Small non-reasoning models do it well,
and against ~10,000 passages throughput dominates. `llama3.2:latest`
at 2GB produces questions like these, from the Magna Carta:

```
How do I pay my barony's relief?
What is a knight's fee?
How do I inherit land directly from the Crown?
```

That is the thing Inverse Cloze cannot produce — situational phrasing
somebody would type *before* reading anything.

`--compare m1,m2,m3 --limit 20` benchmarks models against each other on
the same passages and reports seconds per passage, projected hours for
the full corpus, decline rate, and **`novel`**: the share of question
words that do *not* appear in the passage. That last one is the metric
that matters. A model echoing the passage's own vocabulary has
reinvented ICT, which measured at plus one question out of 53. Prefer
the fastest model whose novel rate matches the others.

### Generated questions are not automatically grounded

The first chunk of *The American Frugal Housewife* (1832) is a title
page, a dedication and illustration captions listing cuts of mutton and
pork. llama3.2 produced:

```
How do I purify water?
What are water purification tablets?
My water looks muddy
What is soap made from?
```

None of that is in the passage. The model inferred what a book with
that title probably contains — "purification tablets" in 1832 being
the tell. Trained on, those pairs teach the encoder to point *"how do
I purify water"* at a title page, which is worse than not training at
all.

So an imported question must share **at least one content term** with
its passage. Weak on purpose: "my water looks muddy" against a passage
about boiling still shares `water`, while a question sharing nothing is
either about a different passage or about nothing. A stronger check —
does the passage contain the *answer* — needs a model, and the point is
to spend the model's time generating rather than verifying.

Only `import` pairs are filtered; `ict`, `title` and `terms` are
derived from the passage by construction. The drop rate is reported,
and a high one means the corpus is full of front matter rather than
that the model is bad.

### Duplicate questions and false negatives

Generated questions create a training hazard that ICT mostly avoids.

One document yields many chunks, and their questions overlap by
construction — two chunks of the Magna Carta both produce "What is the
Magna Carta?". InfoNCE treats every other passage in a batch as a
negative, so those two chunks in one batch trains the model to push a
*correct* passage away from a *correct* query. The loss cannot tell
that from a real negative.

So batches now take **at most one pair per document**, and exact
duplicate questions are dropped. Implemented as a round robin over
document buckets: the obvious version — scan, defer collisions,
rebuild — is quadratic, and at 18,517 pairs that is minutes per epoch
rather than seconds.

Two bugs found building it, both of the silent kind:

- With fewer documents than the batch size (`zdocs` has 85 against a
  default batch of 128) no batch could satisfy the constraint, so the
  batcher yielded **nothing** and training silently did not run —
  reporting a loss of exactly `0.0000`, which is a division by a batch
  count of zero. The batch size is now reduced to fit and says so, and
  a run that produces no batches raises instead of pretending.
- The tail batch is dropped rather than padded: a partial batch has
  fewer negatives, so its gradient is on a different scale from every
  other batch.

### Declined is not the same as failed

A passage the model *looked at and declined* — boilerplate, a licence,
a contents page — is recorded, so a resume does not ask again.

A passage that **failed** — a truncated reply, a dropped connection, a
reasoning block that never reached the answer — is **not** recorded, so
a retry picks it up.

The first version conflated them and wrote `skip: true` for both, which
permanently blanked every passage a transient failure touched. If a
`.jsonl` from that version has `skip` markers that should not be there,
delete it and start again; a run against a working model produces no
skips at all for ordinary prose.

**It is resumable, and that is not optional.** 10,059 passages at a few
seconds each is hours, and anything that long gets interrupted. Every
line is flushed as written, and a re-run skips what is already there —
including passages the model *declined*, which get a `skip` marker.
Without that, the one-in-eight boilerplate passages would be retried on
every resume.

**Pairs are keyed by document and byte offset, not chunk index.** A
chunk index shifts whenever the corpus or `chunk_chars` changes, and
hours of generated questions silently re-attaching to the wrong
passages after a tweak would be a bad trade. Anything that no longer
resolves is reported, not ignored.

Model output is messy no matter what the prompt says — fences,
commentary, `<think>` blocks — so the parser finds the JSON array
rather than trusting the whole reply. Verified against a mock server
that reproduces all of those.

## The corpus mattered more than the model

`how to delete a file` returned **Alice in Wonderland**, then Sun Tzu,
then Hamlet. Nothing in those books is relevant. All three carry the
Project Gutenberg licence, which talks about disks, copying,
distributing and *"if you either delete this file"* — so a corpus of
600 books contained 600 near-copies of the same few thousand words
about deleting files.

Alice is **250 lines of licence before the first word of the story.**

The `books` adapter now strips it. Two formats: modern texts mark the
body with `*** START OF THE PROJECT GUTENBERG EBOOK ***`, and the
2001-era etexts in Ark Lite predate that and end their header with
`*END*THE SMALL PRINT!`. A file matching neither is left alone —
guessing where a book starts is worse than shipping a licence.

| | @1 | @3 | @10 |
|---|---|---|---|
| dense, before | 31/53 | 41/53 | 46/53 |
| dense, after | 30/53 | **43/53** | 46/53 |
| hybrid, before | 37/53 | 44/53 | 51/53 |
| hybrid, after | **39/53** | 44/53 | 51/53 |

Two questions on hybrid, and the qualitative change is larger than that
suggests: Alice, Hamlet and Sun Tzu are gone from the results
entirely, and the Unix article moved from rank 4 to rank 2. The
remaining rank-1 hit is *Carpentry for Boys*, where a "file" is a
genuine woodworking tool — a real ambiguity rather than a corpus
defect.

Worth remembering before reaching for the model again: this was a
bigger win than training, and it was an hour of reading the corpus.

## Training the encoder

The device computes exactly one thing (`aidx.c`, `encode_query`):

```
v = normalise( sum over query terms t of  a[t] * W[t] )
```

`W` is a vocab × dim table of int8 vectors and `a` is a per-term weight
in Q8.8. Untrained, both come from an SVD of the corpus.

**So the thing to train is `W` and `a`.** That is a linear
bag-of-embeddings dual encoder, and training it needs **no new C on the
device, no format change, and no GPU** — 4096 × 128 is 524,288
parameters and it converges in minutes of numpy.

```
./tools/ask/ask train dist/arklite.spec
./tools/ask/ask eval  dist/arklite.spec
./tools/ask/ask build dist/arklite.spec
```

Training pairs come from the corpus itself, no labels required:

- **`ict`** — Inverse Cloze. A sentence is removed from a passage and
  used as the query; the passage minus that sentence is the positive.
  The removal matters: leave it in and the model learns "find the
  passage containing these exact words", which BM25 already does
  better. This is how ORQA and DPR pretrained.
- **`title`** — the document title and heading path as the query.
  Shorter and more noun-ish, so closer to what people type.
- **`terms`** — the passage's most distinctive terms as a bag.
- **`import`** — `(query, chunk)` pairs from JSONL. This is where
  LLM-generated questions go.

### What it is measured to do, which is not much

`arklite` + `zdocs`, 53 gold questions with answers, 18,809 pairs:

| | @1 | @3 | @10 |
|---|---|---|---|
| dense, untrained | 31/53 | 41/53 | 46/53 |
| dense, trained (4 epochs) | **32/53** | 41/53 | **47/53** |
| hybrid, untrained | 37/53 | 44/53 | 51/53 |
| hybrid, trained | **38/53** | **45/53** | 51/53 |

**One question.** On the smaller `zdocs` corpus it is no better than
the baseline at all, and worse past about five epochs.

That is a real result and worth stating plainly rather than tuning
until a number moves. The framework is sound — `--epochs 0` reproduces
the untrained encoder *exactly*, to the question, so anything that
changes is training and nothing else. The training signal is what is
weak: ICT sentences taken from documentation prose look almost nothing
like "how do I stop bleeding from a leg wound", and a model trained to
match declarative statements to their own paragraphs learns a task
nobody is asking it to do.

**The bottleneck is the questions, not the optimiser.** That is what
the 16GB GPU is for, and what `pairs = import` exists for: run a 7B
locally over the corpus, ask it for questions each passage answers,
write JSONL, point `pairs_file` at it. Everything downstream is already
built and measured, so the experiment is one file and one re-run.

### Two bugs this exposed, both instructive

**Training started from uniform term weights.** `s = 0` gives
`softplus(0) = 0.693` for every term, discarding the idf. So epoch 0
was already far below the baseline it was meant to refine, and recall
fell monotonically with training — which reads exactly like
overfitting and was not. Now `s = inv_softplus(idf)`, and epoch 0
reproduces the baseline to the question.

**`-e 0` was silently ignored**, because `if args.epochs:` treats zero
as absent, so the "zero epochs" control ran 25. Both of these made
training look actively harmful when it was merely useless.

## The host framework

```
tools/ask/
  ask                  the CLI
  lib/spec.py          recipe parsing (release/*.spec's format)
  lib/source.py        adapter registry, Document, normalisation
  lib/sources/         codex, medline, books, scroll, mdtree, plain
  lib/segment.py       chunking with heading paths and byte offsets
  lib/cardfs.py        8.3 naming, card tree, offset verification
  lib/lexicon.py       BM25 build and packing
  lib/embed.py         encoders: bow, trained, lsa (eval), torch (next)
  lib/synth.py         training pairs: ict, title, terms, import
  lib/genq.py          question generation via ollama, resumable
  lib/train.py         contrastive trainer, numpy, no GPU needed
  lib/fetch.py         repo cloning + resumable downloads
  lib/images.py        transcode + maps.md / flags.md
  lib/pack.py          the on-card binary formats
  lib/evalset.py       the gold set harness
  dist/common.spec     settings every pack shares
  dist/zdocs.spec      Zeitlos docs
  dist/arkscrl.spec    Ark Scroll
  dist/arklite.spec    Ark Lite
  dist/arkmed.spec     Ark Medium -- the largest set supported
  eval/gold.txt        63 gold questions
  requirements.txt     numpy + scikit-learn; ingest needs neither
```

```
$ pip install -r tools/ask/requirements.txt

$ ./tools/ask/ask sources
$ ./tools/ask/ask fetch  dist/arkmed.spec
$ ./tools/ask/ask train  dist/arklite.spec
$ ./tools/ask/ask ingest dist/arklite.spec
$ ./tools/ask/ask build  dist/arklite.spec
$ ./tools/ask/ask eval   dist/arklite.spec --sort
$ ./tools/ask/ask query  dist/arklite.spec "how do I purify water"
```

**Upstream repos are cloned on demand**, into `tools/ask/.cache/` or
`$ASK_CACHE`. No submodules: the corpora are large, they move on their
own schedule, and most people building Zeitlos will never build a pack.

```
repo   = ark https://github.com/machdyne/ark
source = codex @ark/data/arklite/codex.tgz
```

`repo = ark <url> ref=<commit>` pins a checkout. A recipe without a
`ref` tracks the default branch, which is convenient while developing
and wrong for anything you intend to ship — the build hash covers
document bytes, so an unpinned recipe silently becomes a different
distribution whenever upstream moves. `ask ingest` prints the commit it
resolved and says so.

**Adding a source is one line in a recipe** if an adapter exists, and
one file in `lib/sources/` if it does not. An adapter's only job is to
turn an upstream thing into `Document`s; it does not chunk, embed,
rename, or know anything about the card. That matters because the
corpus comes from repos that move on their own schedule — Ark releases
when Ark releases, `docs/` changes every week — and an adapter is the
only thing that has to change when an upstream layout does.

**Builds are reproducible.** Source order is sorted, card numbering is
sequential in sorted order, and no timestamp reaches anything except
the manifest's own `built` field, which is excluded from the build
hash. Same recipe plus same inputs produces the same bytes.

### The check that matters most

After emitting the card tree, the builder **re-reads every file and
verifies that every chunk's byte range still contains exactly the text
that was indexed.** A wrong offset does not fail on the device — it
quietly previews the wrong paragraph, or half of one, and nothing
anywhere says so.

This is not defensive decoration. It caught a real bug on its first
run: the chunker built its text with `"\n".join(buf)` while computing
the range from line offsets, and the two disagreed about trailing
newlines and blank split lines. All 4,200 chunks were wrong. The
durable fix was to make the two impossible to disagree — a chunk's text
is now *defined* as the slice the device will read.

---

## Status

| | |
|---|---|
| Host framework, recipes, formats, packing | **done**, runs on real data |
| Eval harness and gold set | **done**, 63 questions |
| Adapters: codex, books, scroll, mdtree, listed, onefile, medline, plain | **done** |
| `arkmed` recipe | **done**; fetch and adapters exercised against fixtures and reachable hosts, **not** against gutenberg.org or the real 137MB Wikipedia object |
| `ask fetch` — resumable, checksummed, git-lfs aware | **done** |
| Image transcode + `maps.md` / `flags.md` | **done**; classification patterns are a guess until someone sees the real archive |
| `bow` encoder (ships, no training needed) | **done**, and not yet good enough |
| Pack layout, co-install verified | **done** |
| On-demand repo cloning | **done** |
| `naming = long`, for when LFN lands | **done**, untested (LFN is off) |
| Generated `index.md` per directory | **done** |
| `requirements.txt` | **done** |
| `sw/apps/ask` — window, incremental scan, pack loading | **done**, not yet run on hardware |
| `aidx.c` index engine, host-tested against a real pack | **done**, rankings match the Python reference exactly |
| Device lexical (BM25) half | **not done** — see below |
| `read` `#offset` support | patch written, `sw/apps/ask/read-offset.patch.md` |
| Trained encoder (`torch` backend) | after the app, against the 42/63 and 62/63 bars |
| `rtl/zml.v` coarse-scan accelerator | after a corpus large enough to need it |
| `release/` integration | after the app |

## The device build, and one honest gap

`sw/apps/ask` is a 208-wide window — `read` is 320, and the two are
meant to be open together on a 640-wide screen. A result is two lines:
title, then heading with the score right-aligned. The score is shown
always, because a confident wrong result that looks like a right one is
this program's worst failure.

`aidx.c` is the index reader and query engine and contains no window
code, no messaging and no printf. It reaches the card through a
six-function shim that is `fs_*` on the device and stdio on the host,
so `test/hosttest.c` runs **the engine that ships** over **the pack the
device reads**:

```
$ ./sw/apps/ask/test/hosttest tools/ask/out/arklite "how do I treat a snake bite"
packs: 1   resident: 0.90 MB
  arklite  dsid 0x3673bf3d  546 docs  10059 chunks  coarse 32  fine 128
scanned 10059 of 10059 chunks in 6 slices, 8 hits

1. [arklite] FM 21-11 FIRST AID -- CHAPTER 5
   score 882   /ark/arklite/books/00000072.md  +39006  1848 bytes
   read arg: /ark/arklite/books/00000072.md#39006 (37/96 bytes)
   > 6-3. Snakebites a. Poisonous snakes DO NOT always inject venom...
```

### `ask query` is not the reference. `ask refcheck` is.

`ask query -m dense` encodes from the **float** term vectors held in
memory during the build. The device reads the **int8** ones the packer
wrote, multiplies by a Q8.8 idf, and normalises with an integer square
root. Those are different computations, and comparing the C against the
float one showed a different top hit on some queries — which looked
like a C bug and was a reference bug.

`ask refcheck` mirrors `encode_query()` in `aidx.c` step for step, in
integers, from the quantised weights actually on the card:

```
$ ./tools/ask/ask refcheck dist/zdocs.spec "how does the mtu work"
qvec: 29 -5 2 10 3 -13 8 12 0 -2 -8 13 -5 -1 -7 1 -6 4 -10 12 ...
```

and the C produces that vector bit for bit. **Top-5 identical on 14 of
14 queries.**

`refcheck` covers the DENSE path — encoding, the coarse scan and the
fine re-rank. The lexical half and the fusion sit on top of it and are
measured by `ask eval` rather than by conformance.

The earlier claim that the two matched was three queries where the
difference happened not to change the order. That was luck, not
validation.

### Residency, and the ceiling it runs into

What the device holds is **the coarse vector array plus one encoder,
per pack**. Nothing else.

| pack | chunks | resident | tier |
|---|---|---|---|
| `arkscrl` | 945 | 0.54 MB | BIG |
| `zdocs` | 1,773 | 0.64 MB | BIG |
| `arklite` | 10,059 | 0.90 MB | BIG (only just) |
| `arklite` + `zdocs` | 11,832 | 1.54 MB | HUGE |

The build tool reported 2.10 MB for `arklite` until recently, against
the 0.90 MB the device actually reports. That figure was left over from
the first design, where everything was loaded — and it is exactly the
number someone uses to pick a kernel tier. `ask build` and `ask ingest`
now print what `aidx.c` will really allocate, and check it against
`sw/os/kernel.h`'s tiers.

**The ceiling matters more than the tier.** Residency is
`coarse_dim` bytes per chunk, so it grows *linearly with the corpus*:

| text | chunks | coarse | resident | fits HUGE? |
|---|---|---|---|---|
| 100 MB | 46,100 | 1.48 MB | 2.16 MB | yes |
| 225 MB | 103,725 | 3.32 MB | 4.00 MB | just |
| 500 MB | 230,500 | 7.38 MB | 8.06 MB | **no** |
| 700 MB | 322,700 | 10.33 MB | 11.01 MB | **no** |

At the observed 461 chunks per MB, **`HUGE` covers about 237 MB of
text** at `coarse_dim = 32`, or 475 MB at 16. Ark Medium is larger than
both. So a full `arkmed` does not fit any tier, and a bigger tier buys
a constant factor against a term that grows — it postpones the problem
rather than solving it.

The levers, cheapest first:

- **`coarse_dim = 16`** halves memory and the scan. Measure before
  taking it: `ask eval` reports how deep the coarse shortlist must be,
  and a narrower vector needs a deeper one. At 32 dims the shortlist is
  already 512.
- **Split the corpus into two packs** and install one.
- **A clustered coarse index** — centroids resident, per-cluster
  vectors on the card. The only option that stops this growing, and it
  is not built.

`ask ingest` reports the projection before any download, which is the
cheap place to find out.

### What is resident, and why so little

Only the coarse vector array and the query encoder: 0.90MB for
`arklite`, 1.54MB with `zdocs` alongside it. The chunk table, document
table, term dictionary, postings and fine vectors stay on the card.

That is the shape of the workload, not a saving. The coarse array is
touched in its entirety every query. The chunk and document tables are
touched about eight times per query, once per displayed hit. The fine
vectors are read for the shortlist only. None of those justify holding
megabytes — loading everything would be 2.5MB and about six seconds of
card read instead of 2.4.

### Two bugs the host test could not have caught

Both found on hardware, both invisible to `hosttest`, and both worth
recording because they are the shape of bug this arrangement produces.

**The file shim had `fs_seek`'s polarity inverted.** `fs_seek()`
returns 1 for success; `fseek()` returns 0. The shim returned each
verbatim and every caller tested `!= 0`. So every seek "failed" on the
device and succeeded on the host. Loading is purely sequential, so a
pack loaded perfectly, the scan ran to 100%, and then every query
returned nothing — because `fill_hit()` and `fine_score()` do nothing
but seek. It reads exactly like a retrieval problem.

The shim is the one part of `aidx.c` the host test cannot exercise, by
construction. `ai_selftest()` now opens a file, reads eight bytes,
seeks back to zero and reads them again; a shim whose seek is broken
differs there and nowhere else. It runs once per pack load and reports
`file seek is broken` instead of loading happily and answering
nothing.

**The re-rank did the whole shortlist in one step.** 512 candidates,
each opening and closing `fine.zfv` and re-encoding the query — seconds
of frozen window immediately after the progress bar reached 100%, which
is exactly what the slicing everywhere else exists to prevent. Now:
the query is encoded once per pack up front, `fine.zfv` is held open
across slices, and `AI_RERANK_SLICE` is 64. Smaller than
`AI_SCAN_SLICE` because the work is not comparable — a scan item is a
dot product against resident memory, a re-rank item is a seek and a
read from the card.

### Both halves now run on the device

`AI_Q_LEXICAL` was a passthrough in the first build, which meant the
device ran dense-only — the half that measures *worse* (40/63 against
lexical's 42/63 at rank 1, and 55/63 against 62/63 at rank 10). It also
meant queries whose terms the untrained encoder handles badly returned
nothing useful at all: "what games are available" produced no relevant
result, where BM25 finds `chess` and `chip8` immediately.

BM25 is now implemented against the packed lexicon, and the two halves
are fused by reciprocal rank — the same mechanism that already fuses
results from two packs, for the same reason: BM25 scores and int8 dot
products are not comparable, and RRF consumes orderings.

**Length normalisation, and a claim that was false.** The device ran
with `b = 0` for a while, and the comment justifying it asserted that
`tools/ask` did too "so the gold-set numbers and the device agree about
what they are measuring."

That was never true — `lib/lexicon.py` has always used `b = 0.75`. So
every number quoted from `ask eval` was measuring a better system than
the hardware was running, and it showed: *"how to start a fire"*
returned the **I2C** document, which is full of the I2C START
condition, ahead of the survival manual. Without the length term a long
chunk repeating one query term beats a short one that is actually about
the subject.

The per-chunk count turned out not to need a new file. `chunks.zct`'s
third word is the chunk's *byte* length, already there for the preview,
and bytes over six is a good enough proxy for tokens. `load_pack()`
streams that column into a resident array at startup — two bytes a
chunk, 20KB for `arklite` — and both sides are at `b = 0.75` now.

Neither the dictionary nor the postings are resident. The dictionary is
binary-searched in place — about fifteen probes over twenty thousand
terms — and a term's postings are read as one block.

### Looking at the layout

`make -C sw/apps/ask render` draws the panel on the build machine and
writes a PBM. `sw/common/tests/zrender.h` exists because sheet's and
logic's panels shipped wrong three times each while their arithmetic
tests passed, and its header lists the third bug as:

> `z_win_hw_box()`/`z_win_hw_line()` take ABSOLUTE SCREEN COORDINATES
> while everything else an app draws with is content-relative.

Which is the bug the pack buttons shipped with — every label exactly
one content-inset below its own box. No assertion I would have thought
to write catches that; one render catches it in a second. The boxes are
four `z_win_fill_rect()` calls now, window-relative like everything
else.

The same render showed the window was 300 tall with roughly 150 pixels
of nothing in it. `WIN_H` is now computed from what it has to hold —
query line, rule, status, eight two-line results, buttons — and comes
out at **174**, which on a 480-tall screen gives `read` back a third of
the column.

### The kernel leaked a file handle per launch

Symptom: after relaunching `ask` eight times without a reboot, it
reported **`file seek is broken`** — and seeking was fine.

`sw/common/zfs.h` had carried this as a known limitation since the
handle table was written:

> a handle isn't released if its owning process exits (crashes, or is
> killed) without closing it — there's no process-exit hook wired up to
> sweep abandoned handles ... *worth a real fix if it proves to matter
> in practice.*

It proved to matter. `ask` is the first app to hold a handle across an
interactive lifetime (`fine.zfv`, across re-rank slices), and every app
with `Z_WIN_FLAG_CLOSE_KILLS_OWNER` dies by `k_proc_kill()` without
running any cleanup. `Z_FS_MAX_OPEN` is 8, so eight launches exhausted
the table and only a reboot recovered it.

`k_fs_release_all(pid)` now runs in `kernel.c`'s reap path, right
beside `k_pidreg_release_all(pid)` — the same shape, for the same
reason.

**Read handles are closed; write handles are freed without flushing.**
That path runs in the interrupt context, and `kernel.c`'s own comment
there is explicit that nothing may wait on another interrupt to make
progress. `f_close()` on a handle with no dirty state does no I/O —
FatFs's `f_sync()` returns immediately unless `FA_MODIFIED` is set — so
a read handle costs nothing. A write handle would need a real disk
write, and the data is lost either way because the process writing it
is already dead; losing it beats hanging the scheduler.

`ask` also no longer relies on that: it closes its handle on Ctrl+Q,
and `ai_query_begin()` closes one a previous query may still hold
before `memset` erases the fact that it exists — a second query typed
mid-re-rank leaked one every time.

**And the diagnostic was wrong**, which is what sent the investigation
after seeking. `ai_selftest()` returned one failure code for open,
read, seek and mismatch, and the message named seek. It now
distinguishes them, and an open failure says
`cannot open index -- out of file handles?`.

### Three things the host could not have shown

**String reads were one byte at a time.** `read_str()` looped on
one-byte reads, which on the host is a buffered `fread` costing
nothing and on the device is a syscall into FatFs each. Roughly 224 per
hit across a heading, a path and a title, times eight hits. That was
15–30 seconds of apparent hang *after* the progress bar reached 100%.
Reading one block and scanning it in memory took a query from 2,237
reads to 800.

**The re-rank read `fine.zfv` in heap order** — 512 seeks scattered
across the file. The shortlist is now sorted by `(pack, chunk)` before
the re-rank, so the same 512 reads walk forwards, and since a
128-dimension vector is a quarter of a 512-byte sector, four
consecutive ones come from a sector FatFs has already read. Backward
seeks fell to 72 of 693.

**The selected row drew as solid bars with no text.**
`z_win_draw_text()` paints a solid cell and forces the background to 0,
so drawing with colour 0 gave ink of 0 on a cell of 0 — the cell
*erased* the highlight it was drawn over. `zwin.h` documents exactly
this and points at `z_win_draw_text2()`, which takes both colours.

## Putting it on a card

What to copy, exactly, to test the app:

**1. The app.** Build it and drop the binary at `apps/ask`:

```
make -C sw/apps/ask
cp sw/apps/ask/ask.bin /media/<card>/apps/ask
```

No extension — that is how every other app on the card is named, and
`fs_exec_resolve()` searches the root and then `apps/`.

**2. A pack.** At minimum one, and `arklite` is the one to start with
because everything it needs is inside the ark repo, so it builds from a
clone with no downloads:

```
pip install -r tools/ask/requirements.txt
./tools/ask/ask build dist/arklite.spec
cp -r tools/ask/out/arklite/* /media/<card>/
```

That writes exactly two directories and touches nothing else:

```
/ark/arklite/     the corpus -- 24 MB, plus index.md in every directory
/ask/arklite/     the index  -- 8 files, 2.1 MB
```

`MANIFEST.json` is deliberately *outside* the tree, at
`tools/ask/out/arklite.manifest.json`, because a four-character
extension cannot be written to a FatFs card here. Do not copy it.

**3. Nothing else.** No kernel change is needed to *try* it — but
without the `sw/os/kernel.h` tier edit in
`sw/apps/ask/INTEGRATION.md`, `ask` gets the 16KB default allowance and
will fail to allocate its index. That edit is the one thing that must
be flashed, not copied.

Then:

```
> run wm
> run ask
```

Expect ~2.4 s of loading with a progress bar, then `10059 passages,
1 pack` on the status line.

### If it says something else

The status line carries the reason, and so does UART0. The four that
happen:

| message | cause |
|---|---|
| `out of memory -- needs HUGE tier` | the `kernel.h` edit above was missed. **By far the most likely first-run failure** — everything else can be correct and the app still finds nothing |
| `/ask has no subdirectories` | the pack was copied to `/ask/` rather than `/ask/<name>/`, or only `/ark/` was copied |
| `pack files are from different builds` | two distributions mixed in one directory. Every file carries the `dsid`; this is the check that stops confidently wrong answers |
| `missing file in pack` | an incomplete copy — a pack is eight files in `/ask/<name>/` |

`hosttest` prints the same string, so pointing it at the mounted card
tells you what the device will say before you eject it.

### Checking the card before you eject it

```
$ ls /media/<card>/ask/arklite/
chunks.zct  coarse.zcv  docs.zdt  encoder.zmd
fine.zfv    index.zak   lexicon.zlx  post.zlp

$ ./sw/apps/ask/test/hosttest /media/<card> "how do I purify water"
```

`hosttest` takes a card root, so pointing it at the mounted card runs
the device's own engine over the actual bytes you are about to boot.
If it works there and not on hardware, the problem is the app or the
kernel tier, not the data.

## See also

- `sw/common/zask.h` — the service protocol
- `docs/read_app.md` — the reader `ask` hands documents to
- `docs/web_app.md` — the server/client shape this follows
- `docs/montmul.md` — what an accelerator in this tree looks like
- `docs/sdcard.md` — the 604 KB/s the preview path is budgeted against
