#
# Zeitlos ask -- training pairs.
#
# The encoder is trained contrastively: a query should land near the
# passage that answers it and far from every other passage. That needs
# (query, passage) pairs, and nobody is going to hand-write ten
# thousand of them.
#
# THREE SOURCES, none of which requires a language model:
#
#   ict     Inverse Cloze Task. Take a sentence out of a passage and
#           use it AS the query; the passage minus that sentence is
#           the positive. The removal matters -- leave the sentence in
#           and the model learns "find the passage containing these
#           exact words", which is what BM25 already does better.
#
#           This is how ORQA and DPR pretrained their retrievers
#           without labels, and it is the workhorse here: every chunk
#           in the corpus produces a fresh pair on every epoch.
#
#   title   The document title and heading path as the query, the
#           passage as the positive. Short, noun-ish, and shaped much
#           more like what somebody actually types than a sentence
#           lifted out of prose. Fewer of them, and worth more each.
#
#   terms   A handful of the passage's most distinctive terms, in a
#           bag. Crude, and it covers the keyword-ish end of what
#           people type -- "water purification tablet" rather than a
#           grammatical question.
#
# AND ONE THAT DOES: `import`, which reads (query, chunk-id) pairs
# from a JSONL file. That is where LLM-generated questions go -- run a
# 7B locally over the corpus, ask it for questions each passage
# answers, write them out, point the recipe at the file. Strictly
# better than the three above and strictly more work; the framework
# does not require it and improves if you do it.
#
# -- why not just use the gold set --
#
# 63 questions is an evaluation set. Training on it would make the
# numbers go up and mean nothing. lib/evalset.py's questions must
# never appear here.
#

import json
import random
import re

# Sentence split, deliberately crude. A real segmenter is not worth a
# dependency for something that only has to produce approximately
# sentence-shaped strings.
_SENT = re.compile(r"(?<=[.!?])\s+|\n\n+")

# A query this short is noise; this long is not a query.
MIN_Q_CHARS = 16
MAX_Q_CHARS = 180


def _sentences(text):
    out = []
    for s in _SENT.split(text):
        s = " ".join(s.split())
        if MIN_Q_CHARS <= len(s) <= MAX_Q_CHARS:
            out.append(s)
    return out


def ict_pairs(chunks, texts, rng, per_chunk=1):
    """Inverse Cloze pairs. Returns [(query, chunk_index), ...].

    The positive is the chunk MINUS the sampled sentence, so the model
    cannot win by matching the sentence to itself. That means the
    positive text differs per pair, which is why this returns the
    removed sentence alongside -- the caller rebuilds the passage.
    """
    out = []
    for i, c in enumerate(chunks):
        sents = _sentences(c.text)
        if len(sents) < 2:
            continue
        for _ in range(per_chunk):
            q = rng.choice(sents)
            out.append((q, i, q))       # (query, positive chunk, to remove)
    return out


def title_pairs(docs, chunks, rng):
    """Title and heading path as the query."""
    out = []
    for i, c in enumerate(chunks):
        t = docs[c.doc].title
        parts = [p for p in (t, c.heading) if p]
        if not parts:
            continue
        q = " ".join(parts)
        q = " ".join(q.replace(">", " ").split())
        if MIN_Q_CHARS <= len(q) <= MAX_Q_CHARS:
            out.append((q, i, None))
    return out


def term_pairs(chunks, texts, rng, lexicon, k=6):
    """A bag of the passage's most distinctive terms as the query.

    Distinctive = highest idf among the terms the lexicon kept, which
    is already the "informative" set after stopwords and the
    DF_MAX_RATIO cut.
    """
    import bisect
    out = []
    for i, t in enumerate(texts):
        toks = lexicon.tokenize(t)
        if len(toks) < k:
            continue
        seen, scored = set(), []
        for tok in toks:
            if tok in seen:
                continue
            seen.add(tok)
            j = bisect.bisect_left(lexicon_terms(lexicon), tok)
            terms = lexicon_terms(lexicon)
            if j < len(terms) and terms[j] == tok:
                scored.append((lexicon.idf(j) if hasattr(lexicon, "idf")
                               else 0.0, tok))
        if len(scored) < k:
            continue
        scored.sort(reverse=True)
        q = " ".join(tok for _s, tok in scored[:k])
        out.append((q, i, None))
    return out


def lexicon_terms(lx):
    return lx.terms


def import_pairs(path, chunks):
    """(query, chunk-id) pairs from a JSONL file.

    One object per line, in order of preference:

        {"q": "...", "doc": "<key>", "off": 39006}   <- what genq writes
        {"q": "...", "doc": "<key>"}                 <- whole document
        {"q": "...", "chunk": 1234}                  <- fragile, see below

    `doc` + `off` is the durable form. A chunk INDEX is a position in a
    list that shifts whenever the corpus or `chunk_chars` changes, and
    hours of generated questions keyed to one would silently attach to
    the wrong passages after any such edit. Offsets survive both, and
    are resolved here with a report of anything that no longer matches.

    This is the hook for LLM-generated questions (lib/genq.py). Nothing
    else in the framework needs to know where they came from.
    """
    want_doc = {}
    by_off = []
    out = []
    for lineno, line in enumerate(open(path, encoding="utf-8"), 1):
        line = line.strip()
        if not line:
            continue
        try:
            o = json.loads(line)
        except ValueError:
            raise SystemExit("%s:%d: not valid JSON" % (path, lineno))
        if o.get("skip"):
            continue        # a passage genq's model declined; see genq.py
        q = o.get("q") or o.get("query")
        if not q:
            raise SystemExit("%s:%d: no `q` field" % (path, lineno))
        if "doc" in o and "off" in o:
            by_off.append((q, o["doc"], int(o["off"])))
        elif "chunk" in o:
            out.append((q, int(o["chunk"]), None))
        elif "doc" in o:
            want_doc.setdefault(o["doc"], []).append(q)
        else:
            raise SystemExit("%s:%d: needs `chunk`, or `doc`+`off`"
                             % (path, lineno))
    return out, want_doc, by_off


# ---------------------------------------------------------------------
# GROUNDING
#
# A generated question is only a training pair if the passage ACTUALLY
# ANSWERS IT. Models do not reliably respect that, and the failure is
# not random -- it is confident and plausible.
#
# Measured case: the first chunk of "The American Frugal Housewife"
# (1832) is a title page, a dedication and illustration captions
# listing cuts of mutton and pork. llama3.2 produced:
#
#     How do I purify water?
#     What are water purification tablets?
#     My water looks muddy
#     What is soap made from?
#
# None of that is in the passage. The model inferred what a book with
# that title probably contains. ("Purification tablets" in 1832 is the
# tell.) Trained on, those pairs teach the encoder to point "how do I
# purify water" at a title page -- actively worse than not training.
#
# THE FILTER: a question must share at least one content term with its
# passage. Cheap, and it costs almost nothing legitimate -- "my water
# looks muddy" against a passage about boiling water still shares
# "water". A question sharing NOTHING is either about a different
# passage or about nothing.
#
# It is deliberately weak. A stronger check (does the passage contain
# the ANSWER) needs a model, and the whole point here is to spend the
# model's time generating rather than verifying.
# ---------------------------------------------------------------------

MIN_SHARED_TERMS = 1


def ground(pairs, texts, tokenize, log=print):
    """Drops pairs whose question shares no term with its passage."""
    kept, dropped = [], 0
    cache = {}
    for p in pairs:
        q, ci = p[0], p[1]
        if ci not in cache:
            cache[ci] = set(tokenize(texts[ci]))
        shared = set(tokenize(q)) & cache[ci]
        if len(shared) >= MIN_SHARED_TERMS:
            kept.append(p)
        else:
            dropped += 1
    if dropped and log:
        log("  ungrounded questions dropped: %d of %d (%.0f%%)"
            % (dropped, len(pairs), 100.0 * dropped / max(len(pairs), 1)))
    return kept


def build(docs, chunks, texts, lx, sources="ict,title", seed=0,
          ict_per_chunk=1, import_path=None):
    """All the pairs a training run should see, as
    [(query, chunk_index, sentence_to_remove_or_None), ...].
    """
    rng = random.Random(seed)
    kinds = [s.strip() for s in sources.split(",") if s.strip()]
    out = []
    counts = {}

    for k in kinds:
        if k == "ict":
            got = ict_pairs(chunks, texts, rng, ict_per_chunk)
        elif k == "title":
            got = title_pairs(docs, chunks, rng)
        elif k == "terms":
            got = term_pairs(chunks, texts, rng, lx)
        elif k == "import":
            if not import_path:
                raise SystemExit("`pairs = import` needs `pairs_file = <path>`")
            got, by_doc, by_off = import_pairs(import_path, chunks)
            index = {}
            offs = {}
            for i, c in enumerate(chunks):
                index.setdefault(docs[c.doc].key, []).append(i)
                offs[(docs[c.doc].key, c.off)] = i
            lost = 0
            for q, dkey, off in by_off:
                i = offs.get((dkey, off))
                if i is None:
                    lost += 1
                    continue
                got.append((q, i, None))
            if lost:
                print("  note: %d imported questions no longer match a "
                      "passage\n        (the corpus or chunk_chars changed "
                      "since genq ran)" % lost)
            for key, qs in by_doc.items():
                for i in index.get(key, []):
                    for q in qs:
                        got.append((q, i, None))
        else:
            raise SystemExit("unknown pair source `%s` "
                             "(ict, title, terms, import)" % k)
        if k == "import":
            # Only imported questions can be ungrounded: ict, title and
            # terms are all derived FROM the passage by construction.
            from .lexicon import tokenize
            before = len(got)
            got = ground(got, texts, tokenize)
            if len(got) != before:
                counts[k + "(dropped)"] = before - len(got)
        counts[k] = len(got)
        out.extend(got)

    rng.shuffle(out)
    return out, counts
