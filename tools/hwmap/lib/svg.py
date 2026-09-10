#
# Zeitlos hwmap -- SVG output of the same display list as the PDF.
#
# One file per page. Fonts are named as the PDF core fonts with generic
# fallbacks; a viewer without Helvetica substitutes Arial or similar,
# which has near-identical metrics, so text still fits its boxes.
#

from . import pdf as P
from xml.sax.saxutils import escape

FAMILY = {"H": ("Helvetica, Arial, sans-serif", "normal"),
          "HB": ("Helvetica, Arial, sans-serif", "bold"),
          "C": ("Courier, 'Courier New', monospace", "normal")}


def _c(rgb):
    return "#%02x%02x%02x" % tuple(int(round(v * 255)) for v in rgb)


def _n(v):
    return P._n(v)


def render(cv):
    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%smm" height="%smm" '
         'viewBox="0 0 %s %s">' % (_n(cv.w * 25.4 / 72), _n(cv.h * 25.4 / 72),
                                   _n(cv.w), _n(cv.h)),
         '<rect width="100%" height="100%" fill="#ffffff"/>']
    for op in cv.ops:
        kind = op[0]
        if kind == "rect":
            _, x, y, w, h, fill, stroke, lw, dash, r = op
            a = ['x="%s" y="%s" width="%s" height="%s"' % (_n(x), _n(y), _n(w), _n(h))]
            if r:
                a.append('rx="%s"' % _n(r))
            a.append('fill="%s"' % (_c(fill) if fill is not None else "none"))
            if stroke is not None:
                a.append('stroke="%s" stroke-width="%s"' % (_c(stroke), _n(lw)))
                if dash:
                    a.append('stroke-dasharray="%s"' % " ".join(_n(d) for d in dash))
            o.append("<rect %s/>" % " ".join(a))
        elif kind == "line":
            _, pts, stroke, lw, dash, arrow, arrow_start = op
            d = " ".join("%s,%s" % (_n(px), _n(py)) for px, py in pts)
            extra = ' stroke-dasharray="%s"' % " ".join(_n(x) for x in dash) if dash else ""
            o.append('<polyline points="%s" fill="none" stroke="%s" stroke-width="%s" '
                     'stroke-linecap="round" stroke-linejoin="round"%s/>'
                     % (d, _c(stroke), _n(lw), extra))
            for want, a, b in ((arrow, pts[-2], pts[-1]), (arrow_start, pts[1], pts[0])):
                if want:
                    head = P._arrow(a, b, 3.2 if want is True else want)
                    o.append('<polygon points="%s" fill="%s"/>'
                             % (" ".join("%s,%s" % (_n(px), _n(py)) for px, py in head),
                                _c(stroke)))
        elif kind == "poly":
            _, pts, fill, stroke, lw = op
            o.append('<polygon points="%s" fill="%s"%s/>'
                     % (" ".join("%s,%s" % (_n(px), _n(py)) for px, py in pts),
                        _c(fill) if fill is not None else "none",
                        (' stroke="%s" stroke-width="%s"' % (_c(stroke), _n(lw)))
                        if stroke is not None else ""))
        elif kind == "text":
            _, x, y, s, size, font, color = op
            fam, weight = FAMILY[font]
            o.append('<text x="%s" y="%s" font-family="%s" font-weight="%s" '
                     'font-size="%s" fill="%s" xml:space="preserve">%s</text>'
                     % (_n(x), _n(y), fam, weight, _n(size), _c(color), escape(s)))
    o.append("</svg>")
    return "\n".join(o) + "\n"


def write(path, cv):
    with open(path, "w", encoding="utf-8") as f:
        f.write(render(cv))
