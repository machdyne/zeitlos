#
# Zeitlos ask -- the eval harness.
#
# This is the most important file in the host framework, and it was
# written before the encoder for that reason.
#
# The failure this whole system has to be judged against is not "no
# result". It is a confident wrong result: a query that returns a
# plausible-looking passage about the wrong thing, with nothing
# anywhere saying so. On a machine whose purpose is to be useful when
# nothing else is available, that is the only failure that really
# matters, and it is invisible without measurement.
#
# So: a gold set of questions somebody actually wants answered, a
# pattern that says what a right answer looks like, and a number.
# Every subsequent decision -- chunk size, dimension, whether int8
# quantisation cost anything, whether the transformer beat `bow`,
# whether the accelerator is earning its LUTs -- gets settled here
# rather than argued about.
#
# -- the relevance judgement is crude, on purpose --
#
# A regex over the chunk text, not a hand-labelled id. Labelling 14,000
# chunks by hand is not going to happen, and a pattern that says "this
# passage must mention boiling or purification near water" is a good
# enough proxy to catch the difference between rank 1 and rank 22. It
# is NOT good enough to distinguish rank 1 from rank 2, so do not read
# these numbers to three significant figures.
#
# -- format --
#
#   # comments
#   q   = how do I make water from a stream safe to drink
#   hit = (boil|purif|iodine).{0,200}(water|drink)
#   src = books,medline           optional, restrict which datasets count
#
# Repeat. A `q` starts a new entry.
#

import random
import re


class Question:

    __slots__ = ("q", "rx", "sources", "tag")

    def __init__(self, q, rx, sources, tag="direct"):
        self.q = q
        self.rx = re.compile(rx, re.I | re.S)
        self.sources = sources
        # "direct"     the question uses the words the answer uses.
        # "paraphrase" it deliberately does not -- a situation
        #              described in the words somebody would reach for
        #              BEFORE knowing the technical term.
        #
        # The split exists because 53 of the first 63 questions shared
        # a literal term with the pattern defining a correct answer,
        # which is exactly what BM25 ranks on. Measuring only those
        # rewards term matching by construction and under-samples the
        # one case dense retrieval exists for.
        self.tag = tag


def load(path):
    out = []
    cur = None
    for lineno, raw in enumerate(open(path, encoding="utf-8"), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if "=" not in line:
            raise ValueError("%s:%d: not `key = value`" % (path, lineno))
        k, v = (x.strip() for x in line.split("=", 1))
        if k == "q":
            cur = {"q": v, "hit": None, "src": None, "tag": "direct"}
            out.append(cur)
        elif cur is None:
            raise ValueError("%s:%d: `%s` before any `q`" % (path, lineno, k))
        else:
            cur[k] = v
    qs = []
    for e in out:
        if not e["hit"]:
            raise ValueError("question has no `hit` pattern: %s" % e["q"])
        src = set(x.strip() for x in e["src"].split(",")) if e["src"] else None
        qs.append(Question(e["q"], e["hit"], src, e.get("tag", "direct")))
    return qs


def relevant_set(question, chunks, chunk_texts, doc_sources):
    rel = set()
    for i, t in enumerate(chunk_texts):
        if question.sources is not None:
            if doc_sources[chunks[i].doc] not in question.sources:
                continue
        if question.rx.search(t):
            rel.add(i)
    return rel


def first_hit(order, rel):
    for pos, cid in enumerate(order):
        if cid in rel:
            return pos + 1
    return None


def report_by_tag(rows, tags, methods, ks=(1, 3, 10)):
    """Totals split by question tag.

    A single number over a mixed set hides the thing worth knowing:
    lexical retrieval is strong on `direct` questions and is supposed
    to be weak on `paraphrase` ones. If the dense half is earning its
    place anywhere, it is there.
    """
    lines = []
    for tag in sorted(set(tags)):
        sel = [r for r, t in zip(rows, tags) if t == tag and r[1] > 0]
        if not sel:
            continue
        lines.append("")
        lines.append("  %s (%d questions)" % (tag, len(sel)))
        for m in methods:
            got = [r[2].get(m) for r in sel]
            cells = "  ".join("@%d=%d/%d" % (k, sum(1 for g in got
                                                    if g and g <= k),
                                             len(sel)) for k in ks)
            lines.append("    %-12s %s" % (m, cells))
    return "\n".join(lines)


def report(rows, methods, ks=(1, 3, 10), show_all=False):
    """rows: list of (question, nrel, {method: rank_or_None})

    Questions with NO relevant chunk are reported separately and left
    out of the totals. A question whose `hit` pattern matches nothing
    in the corpus is not a hard question -- it is a broken question, or
    a question about something this corpus does not contain -- and
    counting it as a failure makes every method look worse by the same
    amount while hiding the fact that the gold set needs fixing.
    """
    dead = [r for r in rows if r[1] == 0]
    rows = [r for r in rows if r[1] > 0]
    width = max(len(r[0]) for r in rows) if rows else 10
    width = min(width, 46)
    head = "%-*s %5s" % (width, "question", "rel")
    for m in methods:
        head += " %>7s".replace(">", "") % m[:7]
    lines = [head, "-" * len(head)]
    for q, nrel, ranks in rows:
        line = "%-*s %5d" % (width, q[:width], nrel)
        for m in methods:
            r = ranks.get(m)
            line += " %7s" % (r if r else "--")
        lines.append(line)
    lines.append("")
    if dead:
        lines.append("%d question%s matched NOTHING in this corpus and "
                     "are excluded:" % (len(dead),
                                        "" if len(dead) == 1 else "s"))
        for q, _n, _r in dead:
            lines.append("    %s" % q)
        lines.append("")
    n = len(rows)
    for m in methods:
        got = [r[2].get(m) for r in rows]
        cells = "  ".join("@%d=%d/%d" % (k, sum(1 for g in got if g and g <= k), n)
                          for k in ks)
        lines.append("%-12s %s" % (m, cells))
    return "\n".join(lines)


def shortlist_report(rows, method="coarse",
                     depths=(32, 64, 128, 256, 512, 1024, 2048)):
    """How deep the coarse shortlist has to be to keep the answer.

    This is not a quality metric, it is a SIZING metric, and it is the
    number the device implementation needs.

    The two-stage design only works if the coarse pass keeps the right
    chunk somewhere in the shortlist that the fine pass re-ranks. If it
    does not, no amount of fine-stage accuracy recovers it -- the
    answer was discarded before anything good looked at it. So the
    shortlist depth is a floor set by measurement, not a tuning knob,
    and it directly sets how many fine vectors get read off the card
    per query.
    """
    rows = [r for r in rows if r[1] > 0]
    n = len(rows)
    out = ["shortlist depth needed (method: %s)" % method, ""]
    for d in depths:
        got = sum(1 for _q, _nrel, ranks in rows
                  if (ranks.get(method) or 10 ** 9) <= d)
        bar = "#" * int(40.0 * got / n) if n else ""
        out.append("  top %-5d  %2d/%d  %-40s" % (d, got, n, bar))
    return "\n".join(out)


def paired_bootstrap(rows, a, b, k=1, n=2000, seed=0):
    """Is method `a` really better than `b` at rank k, or is it noise?

    Resamples the QUESTIONS with replacement and recomputes the
    difference each time. Paired, because both methods are scored on
    the same questions -- which is a much tighter comparison than
    treating the two totals as independent.

    Returns (diff, lo, hi, p_better) with a 90% interval, in questions.

    WHY THIS IS HERE. A 63-question gold set has a standard error of
    about three questions at these rates. A sweep over five epoch
    counts then produces a spread of four, picks the maximum, and
    reports it as the best setting -- which is the maximum of five
    noisy draws and is biased high by construction. Without an
    interval there is no way to see that from the table.
    """
    rows = [r for r in rows if r[1] > 0]
    if not rows:
        return 0.0, 0.0, 0.0, 0.5
    ha = [1 if (r[2].get(a) and r[2][a] <= k) else 0 for r in rows]
    hb = [1 if (r[2].get(b) and r[2][b] <= k) else 0 for r in rows]
    m = len(rows)
    rng = random.Random(seed)
    diffs = []
    for _ in range(n):
        d = 0
        for _j in range(m):
            i = rng.randrange(m)
            d += ha[i] - hb[i]
        diffs.append(d)
    diffs.sort()
    lo = diffs[int(0.05 * n)]
    hi = diffs[int(0.95 * n) - 1]
    better = sum(1 for d in diffs if d > 0) / float(n)
    return sum(ha) - sum(hb), lo, hi, better

