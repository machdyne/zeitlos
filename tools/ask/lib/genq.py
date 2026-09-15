#
# Zeitlos ask -- generating training questions with a local LLM.
#
# The measured problem this exists to solve:
#
#   Training the encoder on Inverse Cloze pairs moved gold-set recall
#   by ONE question out of 53. ICT takes a sentence out of a passage
#   and uses it as the query, so the model learns to match declarative
#   documentation prose to the paragraph it came from -- which is not
#   the task. Nobody types "The blitter is its own bus master and does
#   not go through the MTU." They type "how do I delete a file", and
#   get Alice in Wonderland.
#
#   The bottleneck was never the optimiser. It is that we had no
#   QUESTIONS. This produces them.
#
# -- what it does --
#
# Walks the corpus a passage at a time, asks a local model for the
# questions that passage answers, and appends them to a JSONL file
# that lib/synth.py's `import` source reads.
#
#     ./tools/ask/ask genq dist/arklite.spec --model qwen3.8:27b
#     # ... hours ...
#     ./tools/ask/ask train dist/arklite.spec
#     ./tools/ask/ask eval  dist/arklite.spec
#
# -- it is RESUMABLE, and that is not optional --
#
# 10,059 passages at a few seconds each is hours. Anything that long
# gets interrupted. Every line is flushed as it is written and a re-run
# skips passages already present, so stopping and restarting costs the
# passage in flight and nothing else.
#
# -- pairs are keyed by DOCUMENT AND OFFSET, not chunk index --
#
# A chunk index is a position in a list that changes whenever the
# corpus or the chunk size does. Spending hours generating questions
# keyed to something that invalidates on the next `chunk_chars` tweak
# would be a bad trade. `doc` + `off` survive both, and lib/synth.py
# resolves them back to chunks at training time, reporting any that no
# longer match instead of silently training on the wrong passage.
#

import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

DEFAULT_HOST = os.environ.get("OLLAMA_HOST", "http://127.0.0.1:11434")

# Long enough to be answerable, short enough that a 27B is not reading
# two kilobytes for every one of ten thousand passages.
PASSAGE_CHARS = 1400

# Output cap, in tokens. Four short questions is well under a hundred;
# 400 leaves room and bounds the damage when a model decides to
# explain itself.
#
# WITHOUT THIS, a reasoning model (the qwen3 family, deepseek-r1,
# openthinker) emits a <think> block of THOUSANDS of tokens before it
# answers, and a passage takes minutes rather than seconds. It also
# tends to hit whatever limit exists mid-thought, so nothing parses and
# the passage looks like one the model declined.
NUM_PREDICT = 400

# The prompt plus a 1400-character passage is roughly 700 tokens.
# ollama's default num_ctx has historically been 2048, which is enough
# -- but not by much, and a truncated passage produces questions about
# the half the model could see.
NUM_CTX = 4096

# Keeps the model resident between passages. Reloading 6GB per request
# would dominate everything else.
KEEP_ALIVE = "30m"

# JSON schema for the reply. ollama constrains generation to match,
# which removes the fences, the commentary and most of the parsing.
SCHEMA = {
    "type": "object",
    "properties": {
        "skip": {"type": "boolean"},
        "questions": {"type": "array", "items": {"type": "string"}},
    },
    "required": ["questions"],
}


PROMPT = """You are helping build a search index for an offline \
reference library. It will be used by someone with no internet access \
who needs practical information.

Below is one passage from that library. Write {n} SHORT questions that \
this passage answers.

Rules:
- Write questions a real person would type, not exam questions.
- Vary them: some full questions ("how do I purify water"), some \
keyword-style ("water purification tablets"), some describing a \
situation ("my water looks muddy").
- Use the words someone would use BEFORE reading the passage. Do not \
reuse the passage's technical vocabulary unless it is the obvious \
search term.
- Every question must be answerable from this passage alone.
- If the passage is boilerplate, a table of contents, a licence or a \
list of links, return an empty questions array and set skip to true.

Answer with JSON only: {{"questions": ["...", "..."], "skip": false}}
Do not explain. Do not think out loud.

PASSAGE ({title}):
{text}
"""


class GenError(Exception):
    pass


def _post(host, path, payload, timeout=300):
    req = urllib.request.Request(
        host.rstrip("/") + path,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8"))
    except urllib.error.URLError as e:
        raise GenError("%s: %s" % (host, e))


def check(host, model):
    """Confirms the server is up and the model is present."""
    try:
        req = urllib.request.Request(host.rstrip("/") + "/api/tags")
        with urllib.request.urlopen(req, timeout=10) as r:
            tags = json.loads(r.read().decode("utf-8"))
    except Exception as e:
        raise GenError(
            "cannot reach ollama at %s (%s).\n"
            "  Start it, or set OLLAMA_HOST." % (host, e))
    names = [m.get("name", "") for m in tags.get("models", [])]
    if model not in names:
        raise GenError(
            "ollama has no model named `%s`.\n  Available: %s"
            % (model, ", ".join(sorted(names)) or "(none)"))
    return names


_ARRAY = re.compile(r"\[.*\]", re.S)
_OBJ = re.compile(r"\{.*\}", re.S)

# What happened to one passage. The distinction matters: DECLINED is a
# verdict and gets recorded so a resume does not ask again, while
# FAILED is a transient -- a truncated reply, a dropped connection --
# and must NOT be recorded, or one bad minute permanently blanks a
# passage that would have produced good questions on a retry.
OK, DECLINED, FAILED = 0, 1, 2


def parse_questions(text, maxq):
    """Returns (status, questions).

    Handles the schema-constrained object, a bare array, and the mess
    an unconstrained model produces around either.
    """
    if not text or not text.strip():
        return FAILED, []

    # A reasoning model's <think> block. An UNCLOSED one means the
    # reply was cut off mid-thought -- that is a failure, not a
    # decline, and retrying with a lower num_predict or think=false is
    # the fix.
    if "<think>" in text and "</think>" not in text:
        return FAILED, []
    text = re.sub(r"<think>.*?</think>", " ", text, flags=re.S)

    arr = None
    m = _OBJ.search(text)
    if m:
        try:
            o = json.loads(m.group(0))
            if isinstance(o, dict):
                if o.get("skip") and not o.get("questions"):
                    return DECLINED, []
                arr = o.get("questions")
        except ValueError:
            arr = None
    if arr is None:
        m = _ARRAY.search(text)
        if m:
            try:
                arr = json.loads(m.group(0))
            except ValueError:
                arr = None
    if arr is None:
        if "SKIP" in text[:200]:
            return DECLINED, []
        return FAILED, []

    out = []
    for q in arr:
        if not isinstance(q, str):
            continue
        q = " ".join(q.split())
        if 8 <= len(q) <= 160:
            out.append(q)
    if not out:
        return DECLINED, []
    return OK, out[:maxq]


def done_keys(path):
    """(doc, off) pairs already generated, so a re-run resumes."""
    seen = set()
    if not os.path.exists(path):
        return seen
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        try:
            o = json.loads(line)
        except ValueError:
            continue
        if "doc" in o and "off" in o:
            seen.add((o["doc"], int(o["off"])))
    return seen


def ask_model(host, model, prompt, think=False, timeout=300):
    """One /api/generate call. Returns (text, stats).

    `think` is sent as False by default: the qwen3 family, deepseek-r1
    and openthinker all emit a reasoning block otherwise, which is
    thousands of tokens of latency for a reply that is four short
    questions. Older ollama builds reject the field, so a 400 retries
    without it.
    """
    payload = {
        "model": model,
        "prompt": prompt,
        "stream": False,
        "format": SCHEMA,
        "keep_alive": KEEP_ALIVE,
        "options": {
            "temperature": 0.8,
            "num_predict": NUM_PREDICT,
            "num_ctx": NUM_CTX,
        },
    }
    if think is not None:
        payload["think"] = think
    try:
        r = _post(host, "/api/generate", payload, timeout)
    except GenError:
        if think is None:
            raise
        payload.pop("think", None)
        r = _post(host, "/api/generate", payload, timeout)

    ev = r.get("eval_count") or 0
    ed = (r.get("eval_duration") or 0) / 1e9
    stats = {
        "out_tokens": ev,
        "seconds": (r.get("total_duration") or 0) / 1e9,
        "tok_s": (ev / ed) if ed > 0 else 0.0,
    }
    return r.get("response", ""), stats


def probe(docs, chunks, model, host=DEFAULT_HOST, n=4, log=print):
    """Runs ONE passage and shows everything, for diagnosing a slow or
    silent run without waiting for hours of it."""
    check(host, model)
    c = chunks[0]
    d = docs[c.doc]
    prompt = PROMPT.format(n=n, title=d.title, text=c.text[:PASSAGE_CHARS])

    t0 = time.time()
    text, st = ask_model(host, model, prompt)
    el = time.time() - t0

    log("  passage: %s (+%d, %d chars)" % (d.title, c.off, len(c.text)))
    log("  %.1fs wall, %d output tokens, %.1f tok/s"
        % (el, st["out_tokens"], st["tok_s"]))
    if st["out_tokens"] >= NUM_PREDICT:
        log("  *** hit the %d-token cap -- the reply was CUT OFF. ***"
            % NUM_PREDICT)
        log("      Almost always a reasoning model emitting <think>.")
        log("      Try a non-reasoning model, e.g. --model llama3.2:latest")
    log("")
    log("  --- raw response ---")
    for line in text.splitlines()[:24]:
        log("  | %s" % line[:96])
    log("  --- parsed ---")
    status, qs = parse_questions(text, n)
    log("  %s" % ("ok" if status == OK else
                  "DECLINED (recorded, not retried)" if status == DECLINED
                  else "FAILED to parse (will be retried)"))
    for q in qs:
        log("    %s" % q)

    if el > 0 and len(chunks):
        log("")
        log("  at this rate: %d passages is about %d minutes"
            % (len(chunks), int(len(chunks) * el / 60)))
    return 0


def _overlap(q, passage_terms, tokenize):
    """Fraction of the question's terms that do NOT appear in the
    passage.

    THE METRIC THAT MATTERS FOR THIS TASK, and it is not fluency.
    
    A generated question is only worth more than an Inverse Cloze pair
    if it uses the words somebody would type BEFORE reading the
    passage. A model that copies the passage's vocabulary produces
    "what does the blitter's BLIT_SRC_ADDR register contain" -- which
    is ICT with extra steps, and ICT already measured at plus one
    question out of 53.
    
    A model that writes "how do I copy an image to the screen" has done
    the thing that cannot be got from the corpus alone.
    
    So: higher is better, up to a point. Near 1.0 means the questions
    have stopped being about the passage.
    """
    qt = set(tokenize(q))
    if not qt:
        return 0.0
    return len(qt - passage_terms) / float(len(qt))


def compare(docs, chunks, models, host=DEFAULT_HOST, n=4, limit=20,
            log=print):
    """Runs the same passages through several models and reports what
    actually distinguishes them for this job.

    9,872 passages is hours; the difference between a 3B and a 27B is
    most of a working day. Worth twenty passages to decide with.
    """
    from .lexicon import tokenize

    sample = chunks[:limit]
    log("  %d passages x %d models\n" % (len(sample), len(models)))
    log("  %-22s %7s %8s %7s %7s %8s" %
        ("model", "sec/ps", "tok/s", "q/ps", "declin", "novel"))
    log("  " + "-" * 64)

    results = []
    for model in models:
        try:
            check(host, model)
        except GenError as e:
            log("  %-22s  %s" % (model, e.splitlines()[0]))
            continue

        t0 = time.time()
        nq = dec = fail = 0
        novel = []
        toks = 0
        for c in sample:
            d = docs[c.doc]
            pterms = set(tokenize(c.text))
            prompt = PROMPT.format(n=n, title=d.title,
                                   text=c.text[:PASSAGE_CHARS])
            try:
                text, st = ask_model(host, model, prompt)
                toks += st["out_tokens"]
                status, qs = parse_questions(text, n)
            except GenError:
                fail += 1
                continue
            if status == DECLINED:
                dec += 1
            elif status == FAILED:
                fail += 1
            for q in qs:
                nq += 1
                novel.append(_overlap(q, pterms, tokenize))

        el = time.time() - t0
        per = el / max(len(sample), 1)
        results.append({
            "model": model, "sec": per, "q": nq / max(len(sample), 1),
            "declined": dec / max(len(sample), 1),
            "novel": (sum(novel) / len(novel)) if novel else 0.0,
            "failed": fail,
            "tok_s": toks / max(el, 1e-6),
            "hours": per * len(chunks) / 3600.0,
        })
        r = results[-1]
        log("  %-22s %7.2f %8.1f %7.1f %6.0f%% %7.0f%%"
            % (model[:22], r["sec"], r["tok_s"], r["q"],
               100 * r["declined"], 100 * r["novel"]))

    if not results:
        return results

    log("")
    log("  projected for the full %d-passage corpus:" % len(chunks))
    for r in sorted(results, key=lambda x: x["sec"]):
        log("    %-22s %5.1f hours%s"
            % (r["model"][:22], r["hours"],
               "   %d failures" % r["failed"] if r["failed"] else ""))
    log("")
    log("  `novel` is the share of question words NOT in the passage.")
    log("  That is the whole point of generating questions rather than")
    log("  reusing sentences: a model that echoes the passage's own")
    log("  vocabulary has reinvented Inverse Cloze, which measured at")
    log("  plus one question out of 53. Prefer the fastest model whose")
    log("  novel rate is comparable to the others -- and read the")
    log("  questions before committing to a run of this length.")
    return results


def generate(docs, chunks, out_path, model, host=DEFAULT_HOST, n=4,
             limit=0, temperature=0.8, log=print):
    """Appends JSONL questions for every chunk not already covered."""
    check(host, model)

    seen = done_keys(out_path)
    todo = []
    for i, c in enumerate(chunks):
        key = (docs[c.doc].key, c.off)
        if key not in seen:
            todo.append(i)
    if limit:
        todo = todo[:limit]

    log("  %d passages, %d already done (%.0f%% of the corpus), "
        "%d to generate"
        % (len(chunks), len(seen), 100.0 * len(seen) / max(len(chunks), 1),
           len(todo)))
    if not todo:
        log("  nothing to do -- the corpus is fully covered.")
        return 0

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    fh = open(out_path, "a", encoding="utf-8")
    written = skipped = failed = capped = tok_total = 0
    t0 = time.time()

    for k, i in enumerate(todo):
        c = chunks[i]
        d = docs[c.doc]
        body = c.text[:PASSAGE_CHARS]
        prompt = PROMPT.format(n=n, title=d.title, text=body)

        status, qs = FAILED, []
        try:
            text, st = ask_model(host, model, prompt)
            tok_total += st["out_tokens"]
            status, qs = parse_questions(text, n)
            if status == FAILED and st["out_tokens"] >= NUM_PREDICT:
                capped += 1
        except GenError as e:
            log("    %s" % e)

        if status == DECLINED:
            # A VERDICT: the model looked and said there is nothing to
            # ask. Recorded, so a resume does not ask again -- about
            # one passage in eight is boilerplate and re-asking would
            # cost an hour over a full run.
            skipped += 1
            fh.write(json.dumps({"doc": d.key, "off": c.off,
                                 "skip": True}) + "\n")
        elif status == FAILED:
            # A TRANSIENT: truncated reply, dropped connection, a
            # reasoning model that never got to the answer. NOT
            # recorded -- marking it done would permanently blank a
            # passage that a retry would have handled, which is what
            # the first version did.
            failed += 1
        for q in qs:
            fh.write(json.dumps({"q": q, "doc": d.key, "off": c.off},
                                ensure_ascii=False) + "\n")
            written += 1
        # Flushed every passage: this runs for hours and will be
        # interrupted, and a buffered tail would lose the lot.
        fh.flush()

        if (k + 1) % 25 == 0 or k + 1 == len(todo):
            el = time.time() - t0
            rate = (k + 1) / max(el, 1e-6)
            left = (len(todo) - k - 1) / max(rate, 1e-6)
            log("    %d/%d  %d questions  %d declined  %d failed  "
                "%.2f/s  ~%dm left"
                % (k + 1, len(todo), written, skipped, failed,
                   rate, int(left / 60)))
            if capped and capped == failed and failed > 3:
                log("    *** every failure hit the %d-token cap. This "
                    "model is emitting a" % NUM_PREDICT)
                log("        reasoning block. Try a non-reasoning model "
                    "(llama3.2, gemma) ***")

    fh.close()
    return written
