#
# Zeitlos hwmap -- a minimal PDF writer.
#
# Just enough PDF for vector line art and text in the three core fonts:
# one content stream per page, Flate-compressed, fonts referenced by
# name and never embedded. The output is deterministic for a given
# drawing (no timestamps, no random IDs), so an unchanged RTL tree
# yields a byte-identical file.
#

import zlib

K = 0.5523


def _n(v):
    s = ("%.2f" % v).rstrip("0").rstrip(".")
    return s if s not in ("-0", "") else "0"


def _rgb(c):
    return " ".join(_n(x) for x in c)


def _esc(s):
    b = s.encode("cp1252", errors="replace")
    out = bytearray()
    for ch in b:
        if ch in (0x28, 0x29, 0x5C):
            out += b"\\" + bytes([ch])
        elif ch < 32 or ch > 126:
            out += ("\\%03o" % ch).encode()
        else:
            out.append(ch)
    return out.decode("latin-1")


FONTNAMES = {"H": ("F1", "Helvetica"), "HB": ("F2", "Helvetica-Bold"),
             "C": ("F3", "Courier")}


def _page_stream(cv):
    H = cv.h
    o = []

    def Y(y):
        return H - y

    for op in cv.ops:
        kind = op[0]
        if kind == "rect":
            _, x, y, w, h, fill, stroke, lw, dash, r = op
            if fill is None and stroke is None:
                continue
            o.append("q")
            if fill is not None:
                o.append("%s rg" % _rgb(fill))
            if stroke is not None:
                o.append("%s RG %s w" % (_rgb(stroke), _n(lw)))
                o.append("[%s] 0 d" % " ".join(_n(d) for d in dash) if dash else "[] 0 d")
            x0, y0, x1, y1 = x, Y(y + h), x + w, Y(y)
            r = min(r, w / 2, h / 2)
            if r > 0:
                k = r * K
                o.append("%s %s m" % (_n(x0 + r), _n(y0)))
                o.append("%s %s l" % (_n(x1 - r), _n(y0)))
                o.append("%s %s %s %s %s %s c" % (_n(x1 - r + k), _n(y0), _n(x1),
                                                  _n(y0 + r - k), _n(x1), _n(y0 + r)))
                o.append("%s %s l" % (_n(x1), _n(y1 - r)))
                o.append("%s %s %s %s %s %s c" % (_n(x1), _n(y1 - r + k),
                                                  _n(x1 - r + k), _n(y1),
                                                  _n(x1 - r), _n(y1)))
                o.append("%s %s l" % (_n(x0 + r), _n(y1)))
                o.append("%s %s %s %s %s %s c" % (_n(x0 + r - k), _n(y1), _n(x0),
                                                  _n(y1 - r + k), _n(x0),
                                                  _n(y1 - r)))
                o.append("%s %s l" % (_n(x0), _n(y0 + r)))
                o.append("%s %s %s %s %s %s c" % (_n(x0), _n(y0 + r - k),
                                                  _n(x0 + r - k), _n(y0),
                                                  _n(x0 + r), _n(y0)))
                o.append("h")
            else:
                o.append("%s %s %s %s re" % (_n(x0), _n(y0), _n(w), _n(h)))
            o.append("B" if fill is not None and stroke is not None
                     else ("f" if fill is not None else "S"))
            o.append("Q")
        elif kind == "line":
            _, pts, stroke, lw, dash, arrow, arrow_start = op
            o.append("q %s RG %s w 1 J 1 j" % (_rgb(stroke), _n(lw)))
            o.append("[%s] 0 d" % " ".join(_n(d) for d in dash) if dash else "[] 0 d")
            o.append("%s %s m" % (_n(pts[0][0]), _n(Y(pts[0][1]))))
            for px, py in pts[1:]:
                o.append("%s %s l" % (_n(px), _n(Y(py))))
            o.append("S Q")
            for want, a, b in ((arrow, pts[-2], pts[-1]),
                               (arrow_start, pts[1], pts[0])):
                if not want:
                    continue
                head = _arrow(a, b, 3.2 if want is True else want)
                o.append("q %s rg [] 0 d" % _rgb(stroke))
                o.append("%s %s m" % (_n(head[0][0]), _n(Y(head[0][1]))))
                for px, py in head[1:]:
                    o.append("%s %s l" % (_n(px), _n(Y(py))))
                o.append("h f Q")
        elif kind == "poly":
            _, pts, fill, stroke, lw = op
            o.append("q")
            if fill is not None:
                o.append("%s rg" % _rgb(fill))
            if stroke is not None:
                o.append("%s RG %s w" % (_rgb(stroke), _n(lw)))
            o.append("%s %s m" % (_n(pts[0][0]), _n(Y(pts[0][1]))))
            for px, py in pts[1:]:
                o.append("%s %s l" % (_n(px), _n(Y(py))))
            o.append("h")
            o.append("B" if fill is not None and stroke is not None
                     else ("f" if fill is not None else "S"))
            o.append("Q")
        elif kind == "text":
            _, x, y, s, size, font, color = op
            fn = FONTNAMES[font][0]
            o.append("BT /%s %s Tf %s rg %s %s Td (%s) Tj ET"
                     % (fn, _n(size), _rgb(color), _n(x), _n(Y(y)), _esc(s)))
    return "\n".join(o).encode("latin-1")


def _arrow(a, b, size):
    import math
    dx, dy = b[0] - a[0], b[1] - a[1]
    ln = math.hypot(dx, dy) or 1.0
    ux, uy = dx / ln, dy / ln
    px, py = -uy, ux
    base = (b[0] - ux * size, b[1] - uy * size)
    return [b, (base[0] + px * size * 0.45, base[1] + py * size * 0.45),
            (base[0] - px * size * 0.45, base[1] - py * size * 0.45)]


def write(path, pages, title="", subject=""):
    objs = []

    def add(body):
        objs.append(body)
        return len(objs)

    catalog = add(None)
    pages_obj = add(None)
    fonts = {}
    for key, (ref, base) in FONTNAMES.items():
        fonts[ref] = add(("<< /Type /Font /Subtype /Type1 /BaseFont /%s "
                          "/Encoding /WinAnsiEncoding >>" % base).encode())
    font_dict = " ".join("/%s %d 0 R" % (k, v) for k, v in sorted(fonts.items()))
    kids = []
    for cv in pages:
        raw = _page_stream(cv)
        data = zlib.compress(raw, 9)
        content = add(b"<< /Length %d /Filter /FlateDecode >>\nstream\n" % len(data)
                      + data + b"\nendstream")
        page = add(("<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %s %s] "
                    "/Resources << /Font << %s >> >> /Contents %d 0 R >>"
                    % (pages_obj, _n(cv.w), _n(cv.h), font_dict, content)).encode())
        kids.append(page)
    objs[catalog - 1] = ("<< /Type /Catalog /Pages %d 0 R >>" % pages_obj).encode()
    objs[pages_obj - 1] = ("<< /Type /Pages /Kids [%s] /Count %d >>"
                           % (" ".join("%d 0 R" % k for k in kids),
                              len(kids))).encode()
    info = add(("<< /Title (%s) /Subject (%s) /Producer (Zeitlos hwmap) >>"
                % (_esc(title), _esc(subject))).encode())
    out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = []
    for i, body in enumerate(objs, 1):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % i + body + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    for off in offsets:
        out += b"%010d 00000 n \n" % off
    out += (b"trailer\n<< /Size %d /Root %d 0 R /Info %d 0 R >>\nstartxref\n%d\n%%%%EOF\n"
            % (len(objs) + 1, catalog, info, xref))
    with open(path, "wb") as f:
        f.write(out)
