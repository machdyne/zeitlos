#
# Zeitlos hwmap -- a display list.
#
# Layout draws onto a Canvas in points, origin top-left, y downwards.
# The PDF and SVG writers replay the same list, so both outputs are the
# same drawing. Text uses the PDF core fonts (Helvetica, Helvetica-Bold,
# Courier) and is measured with their real metrics (afm.py), which is
# what lets layout fit text into boxes without a renderer to ask.
#
# Text is restricted to WinAnsi (cp1252): the core fonts have no other
# glyphs. Characters outside it are replaced rather than silently
# rendered as boxes.
#

from . import afm

FONTS = {"H": afm.HELVETICA, "HB": afm.HELVETICA_BOLD}


def clean(s):
    return s.encode("cp1252", errors="replace").decode("cp1252")


def text_width(s, size, font="H"):
    if font == "C":
        return 0.6 * size * len(s)
    table = FONTS[font]
    w = 0
    for ch in clean(s).encode("cp1252"):
        if 32 <= ch <= 255:
            w += table[ch - 32]
        else:
            w += 556
    return w * size / 1000.0


def fit(s, size, font, width):
    """Truncate with an ellipsis to fit a width."""
    if text_width(s, size, font) <= width:
        return s
    ell = "\u2026"
    while s and text_width(s + ell, size, font) > width:
        s = s[:-1]
    return (s + ell) if s else ""


class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.ops = []

    def rect(self, x, y, w, h, fill=None, stroke=None, lw=0.5, dash=None,
             r=0.0):
        self.ops.append(("rect", x, y, w, h, fill, stroke, lw, dash, r))

    def line(self, pts, stroke=(0, 0, 0), lw=0.5, dash=None, arrow=None,
             arrow_start=False):
        self.ops.append(("line", list(pts), stroke, lw, dash, arrow, arrow_start))

    def poly(self, pts, fill=None, stroke=None, lw=0.5):
        self.ops.append(("poly", list(pts), fill, stroke, lw))

    def text(self, x, y, s, size=6.0, font="H", color=(0, 0, 0), anchor="l"):
        s = clean(s)
        if anchor != "l":
            w = text_width(s, size, font)
            x = x - (w / 2 if anchor == "c" else w)
        self.ops.append(("text", x, y, s, size, font, color))

    def measure(self, s, size=6.0, font="H"):
        return text_width(s, size, font)
