#!/usr/bin/env python3
#
# Zeitlos ask -- document splitting.
#
#   python3 tools/ask/tests/test_split.py
#
# `split=` bounds document size. Two things have to hold: a piece is
# never larger than the bound (that is the requirement -- `read` jumps
# into these, and `ask` previews from them), and a heading is a real
# heading. The second is the one that went wrong: `ACT New Zealand
# [Rodney HIDE]` read as a chapter, and named 13.5MB of CIA Factbook
# after itself.
#
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.dont_write_bytecode = True
from lib.sources import basic  # noqa: E402

FAILS = []


def check(cond, what):
    print("  %s  %s" % ("ok  " if cond else "FAIL", what))
    if not cond:
        FAILS.append(what)


HEADINGS = ["CHAPTER XXVII", "Chapter 4. The Soil", "ACT I",
            "SECTION XXVI. FLOWER GARDENING", "Appendix B", "BOOK II"]
NOT_HEADINGS = [
    "ACT New Zealand [Rodney HIDE]; Green Party [Russel NORMAN]",
    "Part of the problem is this",
    "Chapter and verse were quoted at him",
    "Books are heavy",
]


def main():
    print("heading detection")
    for h in HEADINGS:
        check(basic._CHAPTER.match(h) is not None, "heading: %s" % h)
    for h in NOT_HEADINGS:
        check(basic._CHAPTER.match(h) is None, "not a heading: %.50s" % h)

    print("splitting")
    para = ("Boil the water for one minute and let it cool.\n"
            "It is then safe to drink.\n\n")
    body = para * 400                      # ~35KB per chapter
    text = "".join("CHAPTER %d\n\n%s" % (i, body) for i in range(1, 6))
    pts = basic._split_points(text, 20000)
    check(len(pts) >= 5, "%d pieces from 5 chapters at a 20KB bound"
          % len(pts))
    sizes = [(pts[i + 1][0] if i + 1 < len(pts) else len(text)) - p[0]
             for i, p in enumerate(pts)]
    check(max(sizes) <= 2 * 20000, "no piece over twice the bound (%d)"
          % max(sizes))
    check(all(text[p[0]:p[0] + 1] != " " for p in pts),
          "no piece starts mid-word")
    heads = [t for _o, t in pts if t]
    check(any(h.startswith("CHAPTER") for h in heads),
          "chapter headings are used as labels")

    # No headings at all: bounded by size, labelled by number -- what
    # the Factbook gets until a recipe gives it a `head=` pattern.
    flat = para * 2000
    pts = basic._split_points(flat, 20000)
    check(len(pts) > 5, "headingless text still splits (%d pieces)" % len(pts))
    labels = [t for _o, t in pts if t]
    check(all(l.startswith("part ") for l in labels),
          "headingless pieces are numbered, not named after prose")

    # head= lets a corpus name its own sections. The CIA Factbook:
    # "@Afghanistan  (South Asia)", sections far smaller than split=.
    doc = "".join("@Country%d  (Region)\n\n%s" % (i, para * 20)
                  for i in range(1, 12))
    rx = basic._heads_re({"head": r"^@(.+)$"})
    pts = basic._split_points(doc, 131072, rx)
    labels = [t for _o, t in pts if t]
    check(len(labels) == 11, "head= finds every section (%d of 11), even "
          "though all of them are far below split=" % len(labels))
    check(labels[0] == "Country1 (Region)",
          "the capture group is the label, without the marker: %r"
          % labels[0])
    check(not any("@" in l for l in labels), "no marker reaches the reader")
    # Through the ADAPTERS, not just _split_points. head= is parsed by
    # spec.py, handed to an adapter, and passed on by it -- and it
    # reached `books` and `listed` but not `onefile`, which is the one
    # the Factbook uses. A unit test on the splitter cannot see that:
    # the option was right, the plumbing was not.
    print("adapters pass head= through")
    import tempfile
    tmp = tempfile.mkdtemp(prefix="split-")
    front = "Front matter\n\nAppendix A: Abbreviations\n\n" + "filler\n" * 20000
    body = "data line about the country\n" * 1800          # ~50KB
    doc = front + "".join("@%s\n\n%s" % (c, body) for c in
                          ("Afghanistan  (South Asia)", "Albania  (Europe)",
                           "Algeria  (Africa)"))
    one = os.path.join(tmp, "35830.txt")
    open(one, "w").write(doc)
    opts = {"prefix": "factbk", "split": "131072", "head": r"^@(.+)$",
            "title": "CIA World Factbook 2010"}
    got = basic.load_onefile(one, dict(opts))
    titles = [d.title for d in got]
    check(any(t.endswith("-- Afghanistan (South Asia)") for t in titles),
          "onefile: sections named by head= (%d docs)" % len(got))
    check(not any("Appendix A" in t for t in titles),
          "onefile: the chapter default no longer names them")
    # listed, through a .lst file
    open(os.path.join(tmp, "one.txt"), "w").write(doc)
    open(os.path.join(tmp, "f.lst"), "w").write("one.txt A Book\n")
    got = basic.load_listed(tmp, {"lst": "f.lst", "prefix": "x",
                                  "split": "131072", "head": r"^@(.+)$"})
    check(any(d.title.endswith("-- Albania (Europe)") for d in got),
          "listed: sections named by head= (%d docs)" % len(got))
    import shutil
    shutil.rmtree(tmp, ignore_errors=True)

    print()
    if FAILS:
        print("%d FAILED" % len(FAILS))
        return 1
    print("all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
