#!/usr/bin/env python3
#
# Zeitlos ask -- the HTML converter.
#
#   python3 tools/ask/tests/test_html2md.py
#
# What matters here is not that the markdown is pretty. It is that the
# OUTPUT IS PROSE: `ask` indexes the file that is emitted and quotes a
# byte range of it verbatim, so a tag that survives becomes a search
# term and appears in a preview.
#
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.dont_write_bytecode = True
from lib import html2md  # noqa: E402

FAILS = []


def check(cond, what):
    print("  %s  %s" % ("ok  " if cond else "FAIL", what))
    if not cond:
        FAILS.append(what)


PAGE = """<!DOCTYPE html><html><head><title>Water</title>
<style>.x{color:#fff}</style><script>var a=1;</script></head><body>
<div id="mw-navigation"><ul><li><a href="/wiki/Main_Page">Main page</a></li></ul></div>
<div id="content"><h1 class="firstHeading">Water</h1>
<div class="mw-editsection">[<a href="/edit">edit</a>]</div>
<p>Water is a <a href="/wiki/Chemical">chemical substance</a> with the
formula H<sub>2</sub>O.</p>
<table class="infobox"><tr><th>Boiling point</th><td>100&nbsp;&deg;C</td></tr></table>
<h2>Purification</h2>
<p>Bring it to a rolling boil for one minute &amp; let it cool.</p>
<ul><li>Boiling</li><li>Filtration<ul><li>Sand</li></ul></li></ul>
<div class="printfooter">Retrieved from "http://en.wikipedia.org/wiki/Water"</div>
<div class="catlinks"><a href="/wiki/Category:Water">Category: Water</a></div>
</div></body></html>"""


def main():
    out = html2md.convert(PAGE, "Water")
    print(out)
    for bad in ("<", ">", "href", "class=", "mw-", "wiki/"):
        check(bad not in out, "no %r survives" % bad)
    check("Main page" not in out, "navigation dropped")
    check("Retrieved from" not in out and "Category:" not in out,
          "page furniture dropped")
    check("rolling boil for one minute & let it cool" in out,
          "prose kept, entities decoded")
    check(out.startswith("# Water"), "heading first, for the heading path")
    check("## Purification" in out, "subheadings become markdown")
    check("\n  - Sand" in out, "nested list indented two spaces")
    check("100 °C" in out, "table cell text kept")

    # MediaWiki furniture that survives as SECTIONS, and the
    # link-only sections, which are lists of bare titles once the
    # links are gone.
    wiki = ("<html><body><h1>Water</h1><p>Water is vital.</p>"
            "<h2>Views</h2><ul><li>Read</li><li>Edit</li></ul>"
            "<h2>Languages</h2><ul><li>Deutsch</li></ul>"
            "<h2>Purification</h2><p>Boil it for one minute.</p>"
            "<h3>Filtration</h3><p>Sand works.</p>"
            "<h2>References</h2><ol><li>Smith, J. Water. 1999.</li></ol>"
            "<h2>External links</h2><ul><li>Water at DMOZ</li></ul>"
            "</body></html>")
    w = html2md.convert(wiki, "Water")
    for gone in ("Views", "Read", "Languages", "Deutsch", "References",
                 "Smith", "External links", "DMOZ"):
        check(gone not in w, "section dropped: %s" % gone)
    check("## Purification" in w and "Boil it for one minute." in w,
          "real sections kept")
    check("### Filtration" in w and "Sand works." in w,
          "subsections of a kept section survive")
    check("Deutsch" not in w, "a dropped section takes its list with it")
    check("Views" in html2md.convert(wiki, "Water", sections="keep"),
          "sections=keep turns the pruning off")
    # An article ABOUT one of those words keeps its body.
    about = ("<html><body><h1>Search</h1><p>Search is the process of "
             "looking for something.</p><h2>Methods</h2><p>Binary "
             "search halves the range.</p></body></html>")
    a = html2md.convert(about, "Search")
    check("looking for something" in a and "Binary search" in a,
          "an article titled Search is not dropped")

    check(html2md.convert("plain text about boiling water", "T")
          == "plain text about boiling water", "non-HTML untouched")
    check(html2md.convert("An essay about <p> tags in HTML.", "T")
          == "An essay about <p> tags in HTML.",
          "prose mentioning one tag is not HTML")
    broken = html2md.convert("<html><p>Broken <b>markup <div>oh dear</p>", "T")
    check("<" not in broken and "oh dear" in broken,
          "malformed markup still yields prose")
    big = "<html><body>" + "<p>Boil the water.</p>" * 500 + "</body></html>"
    check(html2md.convert(big, "W").count("Boil the water.") == 500,
          "nothing is lost on a large page")
    print()
    if FAILS:
        print("%d FAILED" % len(FAILS))
        return 1
    print("all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
