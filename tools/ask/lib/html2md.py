#
# Zeitlos ask -- HTML into markdown.
#
# The 2008 Wikipedia selection ships as HTML, and an HTML file on the
# card is wrong twice over:
#
#   - `ask` indexes the file it emits. Tags and attributes become
#     TERMS -- `div`, `class`, `href`, `jpg`, every colour in a style
#     attribute -- and a preview, which is a byte range of that same
#     file shown verbatim, shows markup.
#   - `read` renders markdown. Handed HTML it shows the markup, and
#     everything on the card is `.md` by deliberate policy (cardfs.py,
#     "EVERYTHING IS EMITTED AS .MD").
#
# So HTML is converted at BUILD time and the card carries markdown.
# The alternative -- ship the HTML and open it in `web` -- keeps the
# original bytes but leaves the index full of markup, which is the
# half that actually decides whether anything is findable.
#
# This is html.parser from the standard library, no dependency. It is
# not a general HTML renderer and does not try to be: headings,
# paragraphs, lists, tables flattened to lines, links reduced to their
# text. What it must get right is that the OUTPUT IS PROSE -- no tags,
# no attributes, entities decoded -- because that is what is indexed.
#
# Dropped entirely: script, style, head content, and elements whose
# class or id marks them as navigation or page furniture. A Wikipedia
# page is mostly furniture by volume.
#

import re
from html.parser import HTMLParser

DROP_TAGS = {"script", "style", "head", "meta", "link", "noscript",
             "object", "embed", "iframe", "svg", "form", "input",
             "button", "select", "textarea"}

# Substrings of class/id that mark furniture on a MediaWiki page: the
# sidebar, the edit links, the category boxes, the print footer.
DROP_CLASS = ("navbox", "nav-box", "vertical-navbox", "metadata",
              "editsection", "toc", "catlinks", "printfooter",
              "siteSub", "jump-to-nav", "mw-jump", "sidebar",
              "portal", "sistersitebox", "hatnote", "ambox",
              "navigation", "footer", "mw-editsection", "reference",
              "reflist", "noprint", "infobox-image",
              # MediaWiki portlets: p-views, p-lang, p-search, p-tb...
              "p-views", "p-lang", "p-search", "p-tb", "p-personal",
              "p-navigation", "p-interaction", "p-cactions", "mw-panel",
              "mw-head", "column-one", "sitenotice", "sitesub")

# Sections dropped by their HEADING, after parsing.
#
# MediaWiki's sidebar portlets (Views, Interaction, Search, Languages,
# Toolbox) survive tag-level furniture rules in some dumps because
# they are ordinary divs with headings, and they carry no information
# at all -- "Views / Read / Edit / View history".
#
# The link-only sections go too. References, See also and External
# links are lists of link TEXT once the links themselves are gone: a
# reader cannot follow them, and `ask` indexes them as though the
# article were about every work it cites. "See also" is the arguable
# one -- it is a list of related article names -- but on this corpus
# it reads as a wall of bare titles, so it goes with the rest and
# `sections=keep` brings all of them back.
DROP_SECTIONS = (
    "views", "interaction", "search", "languages", "navigation",
    "toolbox", "personal tools", "variants", "actions", "in other "
    "projects", "contents", "references", "reference", "see also",
    "external links", "further reading", "notes", "bibliography",
    "sources", "citations", "footnotes", "categories", "category",
    "retrieved from", "navigation menu",
)

BLOCK_TAGS = {"p", "div", "section", "article", "br", "hr", "tr",
              "blockquote", "pre", "dd", "dt", "figcaption", "caption"}

HEADINGS = {"h1": 1, "h2": 2, "h3": 3, "h4": 4, "h5": 5, "h6": 6}


class _Html2Md(HTMLParser):

    def __init__(self):
        HTMLParser.__init__(self, convert_charrefs=True)
        self.out = []
        self.drop_depth = 0       # inside a dropped element
        self.drop_tag = None
        self.heading = 0
        self.list_stack = []
        self.in_cell = False

    # -- helpers --

    def _emit(self, s):
        if self.drop_depth:
            return
        self.out.append(s)

    def _newline(self, n=1):
        if self.drop_depth:
            return
        while self.out and self.out[-1] == "\n":
            self.out.pop()
            n = max(n, 1)
        if self.out:
            self.out.append("\n" * n)

    @staticmethod
    def _furniture(attrs):
        v = " ".join(val or "" for key, val in attrs
                     if key in ("class", "id", "role"))
        low = v.lower()
        return any(c.lower() in low for c in DROP_CLASS)

    # -- parser callbacks --

    def handle_starttag(self, tag, attrs):
        if self.drop_depth:
            if tag == self.drop_tag:
                self.drop_depth += 1
            return
        if tag in DROP_TAGS or self._furniture(attrs):
            self.drop_depth = 1
            self.drop_tag = tag
            return
        if tag in HEADINGS:
            self._newline(2)
            self.heading = HEADINGS[tag]
            self._emit("#" * self.heading + " ")
        elif tag in ("ul", "ol"):
            # A list nested inside an item continues that item's list
            # rather than starting a new block.
            self._newline(1 if self.list_stack else 2)
            self.list_stack.append(tag)
        elif tag == "li":
            self._newline(1)
            depth = max(0, len(self.list_stack) - 1)
            self._emit("  " * depth + "- ")
        elif tag in ("td", "th"):
            if self.out and not self.out[-1].endswith("\n"):
                self._emit(" | ")
            self.in_cell = True
        elif tag == "img":
            alt = dict(attrs).get("alt")
            if alt:
                self._emit("(%s)" % alt.strip())
        elif tag in BLOCK_TAGS:
            self._newline(2 if tag in ("p", "div", "blockquote", "pre")
                          else 1)

    def handle_endtag(self, tag):
        if self.drop_depth:
            if tag == self.drop_tag:
                self.drop_depth -= 1
                if not self.drop_depth:
                    self.drop_tag = None
            return
        if tag in HEADINGS:
            self.heading = 0
            self._newline(2)
        elif tag in ("ul", "ol"):
            if self.list_stack:
                self.list_stack.pop()
            self._newline(1 if self.list_stack else 2)
        elif tag in ("td", "th"):
            self.in_cell = False
        elif tag in BLOCK_TAGS or tag == "li":
            self._newline(1 if tag in ("li", "tr") else 2)

    def handle_data(self, data):
        if self.drop_depth or not data:
            return
        # Whitespace, including newlines inside a paragraph, collapses:
        # the source's line breaks are not the document's.
        text = re.sub(r"\s+", " ", data)
        if not text.strip():
            if self.out and not self.out[-1].endswith((" ", "\n")):
                self._emit(" ")
            return
        if self.out and self.out[-1].endswith("\n"):
            text = text.lstrip()
        self._emit(text)

    def text(self):
        s = "".join(self.out)
        # Runs of spaces collapse, but a line's INDENT survives: two
        # spaces are what make a nested list item nested in markdown.
        s = "\n".join(re.match(r"[ \t]*", ln).group(0)
                      + re.sub(r"[ \t]+", " ", ln.strip())
                      for ln in s.split("\n"))
        s = re.sub(r"[ \t]+\n", "\n", s)
        # Leading spaces are dropped except where they indent a nested
        # list item, which is the one place they mean something.
        s = re.sub(r"\n[ \t]+(?![ \t]*- )", "\n", s)
        s = re.sub(r"\n{3,}", "\n\n", s)
        # A heading with nothing under it, and rows left empty by
        # dropped cells, are noise in an index and in a preview.
        s = re.sub(r"(?m)^#{1,6}\s*$\n?", "", s)
        s = re.sub(r"(?m)^\s*\|\s*$\n?", "", s)
        return s.strip() + "\n"


_TAGS = ("<html", "<!doctype html", "<body", "<div", "<p>", "<p ",
         "<ul", "<ol", "<li", "<table", "<h1", "<h2", "<h3", "<span",
         "<a href")


def looks_like_html(text, sniff=4096):
    """Cheap and deliberately conservative: a document that only
    MENTIONS HTML in prose should not be mangled, so this wants an
    actual tag near the start."""
    head = text[:sniff].lower()
    if "<!doctype html" in head or "<html" in head or "<body" in head:
        return True
    # Otherwise several tags, not one: a plain-text book that mentions
    # "<p>" in prose is not an HTML document.
    return sum(head.count(t) for t in _TAGS) >= 3


_HEAD_RE = re.compile(r"^(#{1,6})\s*(.*?)\s*$")


def drop_sections(md, names=DROP_SECTIONS):
    """Removes each section whose heading names it, down to the next
    heading of the same or a higher level."""
    out = []
    skip_level = 0
    for line in md.split("\n"):
        m = _HEAD_RE.match(line)
        if m:
            level = len(m.group(1))
            if skip_level and level <= skip_level:
                skip_level = 0
            if not skip_level:
                label = m.group(2).strip().lower().rstrip(":")
                # Never a level-1 heading: that is the article's own
                # title, and Wikipedia has an article called Search.
                if level > 1 and label in names:
                    skip_level = level
                    continue
        if not skip_level:
            out.append(line)
    text = "\n".join(out)
    return re.sub(r"\n{3,}", "\n\n", text).strip() + "\n"


def convert(text, title=None, sections=None):
    """HTML to markdown. Returns the text unchanged if it is not HTML.

    `title` is prepended as a level-1 heading when the document does
    not already start with one -- chunks are embedded with their
    heading path (docs/ask_app.md, "Chunking earns its keep"), and a
    Wikipedia article whose own <h1> sat in dropped furniture would
    otherwise have no heading at all.
    """
    if not looks_like_html(text):
        return text
    p = _Html2Md()
    try:
        p.feed(text)
        p.close()
    except Exception:
        # Malformed markup: fall back to tag stripping rather than
        # losing the document.
        body = re.sub(r"(?is)<(script|style)[^>]*>.*?</\1>", " ", text)
        body = re.sub(r"(?s)<[^>]+>", " ", body)
        import html as _html
        body = _html.unescape(body)
        return re.sub(r"\n{3,}", "\n\n", re.sub(r"[ \t]+", " ", body)).strip() + "\n"
    out = p.text()
    if sections != "keep":
        out = drop_sections(out)
    if title and not out.startswith("# "):
        out = "# %s\n\n%s" % (title, out)
    return out
