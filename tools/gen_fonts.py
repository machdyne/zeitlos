#!/usr/bin/env python3
"""
Builds the fonts slide and zcanvas draw with (docs/fonts.md):

    python3 tools/gen_fonts.py                 # everything, from the tree
    python3 tools/gen_fonts.py --sheet DIR     # also PNG proof sheets
    python3 tools/gen_fonts.py --bdf DIR       # misc-fixed BDFs (see below)

Two kinds, both public domain:

  * Hershey stroke fonts, sw/data/font/hershey/*.jhf -> sw/data/font/zhf/
    (A. V. Hershey, U.S. National Bureau of Standards; the .jhf format by
    James Hurt, Cognition, Inc. -- the acknowledgement the original
    distribution asks for travels with the data: sw/data/font/hershey/
    NOTICE, and /sys/fonts/hershey.txt on the card.)

    The .jhf files cover ASCII. Everything else in Latin-9, and a few
    typographic characters (curly quotes, dashes, bullet, ellipsis,
    arrows, minus, trade mark), is COMPOSED here from strokes already in
    the font plus small accents and symbols drawn below. Those additions
    are dedicated to the public domain like the rest.

  * misc-fixed bitmaps (X11; "Public domain font. Share and enjoy.")
    -> sw/data/font/zfn/. The sources are not in the tree: they are the
    PCF files of xfonts-base, turned into BDF with pcf2bdf:

        for f in 6x13 6x13B ... 10x20; do
            zcat /usr/share/fonts/X11/misc/$f.pcf.gz > /tmp/f.pcf
            pcf2bdf -o bdf/$f.bdf /tmp/f.pcf
        done
        python3 tools/gen_fonts.py --bdf bdf

    Without --bdf the committed .zfn files are left alone.

-- ZHF1, a stroke font (all little-endian) --

    offset  size  field
    0       4     "ZHF1"
    4       1     version (1)
    5       1     flags: bit 0 italic, bit 1 bold
    6       1     units per em (32): a font drawn at size S px scales by S/32
    7       1     cap height, units above the baseline
    8       1     x-height
    9       1     ascent (highest ink above the baseline, accents included)
    10      1     descent (lowest ink below the baseline)
    11      1     advance of a space
    12      2     glyph count
    14      2     0
    16      16    family name, NUL-padded ("sans", "serif", ...)
    32      4*n   index: u16 codepoint (ascending, BMP), u16 offset into data
    ...           data. Per glyph:
                    u8 advance; i8 xmin, ymin, xmax, ymax (ink box);
                    u8 npoints; npoints x (i8 x, i8 y)
                  x from the glyph's origin (left side), y DOWN from the
                  baseline (cap tops are negative). x == -128 is pen-up.

-- ZFN1, a bitmap font (the format tools/gen_jfont.py writes; byte 7 is
   now the ascent, 0 meaning "the bottom row is the baseline") --

    0 "ZFN1"; 4 w; 5 h; 6 bytes per row; 7 ascent; 8 u32 count;
    12 u16 codepoints ascending, padded to 4; then count*h*stride rows,
    MSB-first.
"""

import math
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HDIR = os.path.join(ROOT, "sw", "data", "font", "hershey")
ZHF_DIR = os.path.join(ROOT, "sw", "data", "font", "zhf")
ZFN_DIR = os.path.join(ROOT, "sw", "data", "font", "zfn")

# name on the card, source, family, flags (1 italic, 2 bold), and the
# Hershey Greek merged into it (U+0391..U+03C9; also where micro and
# sharp s come from).
HERSHEY = [
	("sans",    "rowmans",  "sans",   0, "greek"),
	("sansb",   "rowmand",  "sans",   2, "greek"),
	("serif",   "timesr",   "serif",  0, "greekc"),
	("serifb",  "timesrb",  "serif",  2, "greekc"),
	("serifi",  "timesi",   "serif",  1, "greekc"),
	("serifbi", "timesib",  "serif",  3, "greekc"),
	("heavy",   "rowmant",  "heavy",  2, "greekc"),
	("script",  "scripts",  "script", 1, "greek"),
	("scriptb", "scriptc",  "script", 3, "greekc"),
	("gothic",  "gothiceng", "gothic", 0, "greekc"),
	("symbol",  "symbolic", "symbol", 0, None),
]

# misc-fixed: card name, BDF name
FIXED = [
	("m6x13", "6x13"), ("m6x13b", "6x13B"), ("m6x13o", "6x13O"),
	("m7x13", "7x13"), ("m7x13b", "7x13B"), ("m7x13o", "7x13O"),
	("m7x14", "7x14"), ("m7x14b", "7x14B"),
	("m8x13", "8x13"), ("m8x13b", "8x13B"), ("m8x13o", "8x13O"),
	("m9x15", "9x15"), ("m9x15b", "9x15B"),
	("m9x18", "9x18"), ("m9x18b", "9x18B"),
	("m10x20", "10x20"),
]

BASE = 9	# the .jhf baseline: 'H' runs from y=-12 to y=9

# -- glyphs ------------------------------------------------------------
#
# A glyph here is (left, right, strokes): left/right the .jhf side
# bearings, strokes a list of point lists, in .jhf units (y down,
# baseline at BASE).

def parse_jhf(path):
	out = []
	for line in open(path, encoding="latin-1"):
		line = line.rstrip("\n")
		if not line.strip():
			continue
		n = int(line[5:8])
		d = line[8:]
		left, right = ord(d[0]) - 82, ord(d[1]) - 82
		strokes, cur = [], []
		for i in range(1, n):
			a = d[2 * i:2 * i + 2]
			if a == " R":
				if cur:
					strokes.append(cur)
				cur = []
				continue
			cur.append((ord(a[0]) - 82, ord(a[1]) - 82))
		if cur:
			strokes.append(cur)
		out.append((left, right, strokes))
	return out


def pts(g):
	return [p for s in g[2] for p in s]


def bbox(g):
	p = pts(g)
	if not p:
		return (0, 0, 0, 0)
	xs = [a for a, _ in p]
	ys = [b for _, b in p]
	return (min(xs), min(ys), max(xs), max(ys))


def xf(strokes, fn):
	return [[fn(x, y) for x, y in s] for s in strokes]


def shift(strokes, dx, dy):
	return xf(strokes, lambda x, y: (x + dx, y + dy))


def scale(strokes, k, cx=0, cy=BASE):
	return xf(strokes, lambda x, y: (cx + (x - cx) * k, cy + (y - cy) * k))


def rot180(strokes, cx, cy):
	return xf(strokes, lambda x, y: (2 * cx - x, 2 * cy - y))


def ring(cx, cy, r, n=10):
	return [[(cx + r * math.cos(2 * math.pi * i / n),
		cy + r * math.sin(2 * math.pi * i / n)) for i in range(n + 1)]]


def dot(cx, cy, r=1.0):
	# Hershey draws a period as a small closed polygon; so do we.
	return ring(cx, cy, r, 6) + ring(cx, cy, r * 0.5, 4)


# Where each Greek letter sits in the two Hershey Greek files: greek.jhf
# is laid out like a Symbol font (C is chi, F phi, Q theta...), greekc.jhf
# alphabetically.
GREEK_UC = "ΑΒΓΔΕΖΗΘΙΚΛΜΝΞΟΠΡΣΤΥΦΧΨΩ"
GREEK_LC = "αβγδεζηθικλμνξοπρστυφχψω"
GREEK_MAP = {
	"greek": dict(zip("ABGDEZHQIKLMNXOPRSTUFCYW", GREEK_UC)) |
		dict(zip("abgdezhqiklmnxoprstufcyw", GREEK_LC)),
	"greekc": dict(zip("ABCDEFGHIJKLMNOPQRSTUVWX", GREEK_UC)) |
		dict(zip("abcdefghijklmnopqrstuvwx", GREEK_LC)),
}


class Font:

	def __init__(self, src, greek):
		self.ascii = parse_jhf(os.path.join(HDIR, src + ".jhf"))
		self.greek = parse_jhf(os.path.join(HDIR, greek + ".jhf")) \
			if greek else None
		self.greek_map = GREEK_MAP.get(greek, {})
		H = self.g("H")
		x = self.g("x")
		self.cap_top = bbox(H)[1]		# -12
		self.x_top = bbox(x)[1]		# -5
		# italic slant, from the stem of 'l': how far x moves per unit up
		l = bbox(self.g("l"))
		lp = pts(self.g("l"))
		top = [p for p in lp if p[1] <= l[1] + 1]
		bot = [p for p in lp if p[1] >= l[3] - 1]
		if top and bot:
			tx = sum(p[0] for p in top) / len(top)
			bx = sum(p[0] for p in bot) / len(bot)
			self.slant = (tx - bx) / float(l[3] - l[1] or 1)
		else:
			self.slant = 0.0

	def g(self, ch):
		return self.ascii[ord(ch) - 32]

	def gr(self, letter):
		"""A Greek letter, by its Unicode character."""
		for k, v in self.greek_map.items():
			if v == letter:
				return self.greek[ord(k) - 32]
		return None

	def width(self, ch):
		g = self.g(ch)
		return g[1] - g[0]

	# x where an accent goes above a glyph: the middle of its upper ink,
	# so it follows an italic slant.
	def anchor(self, g, top):
		p = pts(g)
		if not p:
			return 0
		up = [q for q in p if q[1] <= top + 3]
		if not up:
			up = p
		return (min(q[0] for q in up) + max(q[0] for q in up)) / 2.0


# Accents, drawn with their bottom at y=0 and centred on x=0. Sizes
# suit a cap height of 21 units; every family uses the same ones.
ACCENT = {
	"acute":      [[(-1.5, 0), (2, -4)]],
	"grave":      [[(-2, -4), (1.5, 0)]],
	"circumflex": [[(-3, 0), (0, -3.5), (3, 0)]],
	"caron":      [[(-3, -3.5), (0, 0), (3, -3.5)]],
	"tilde":      [[(-4, -0.5), (-3, -2.5), (-1.5, -3), (1.5, -1),
		(3, -1.5), (4, -3.5)]],
	"diaeresis":  dot(-3, -1.2) + dot(3, -1.2),
	"ring":       ring(0, -2.5, 2.5),
	"macron":     [[(-4, -1), (4, -1)]],
}

# Latin-9 letters made of a base and an accent.
COMPOSED = {
	0xC0: ("A", "grave"), 0xC1: ("A", "acute"), 0xC2: ("A", "circumflex"),
	0xC3: ("A", "tilde"), 0xC4: ("A", "diaeresis"), 0xC5: ("A", "ring"),
	0xC8: ("E", "grave"), 0xC9: ("E", "acute"), 0xCA: ("E", "circumflex"),
	0xCB: ("E", "diaeresis"),
	0xCC: ("I", "grave"), 0xCD: ("I", "acute"), 0xCE: ("I", "circumflex"),
	0xCF: ("I", "diaeresis"),
	0xD1: ("N", "tilde"),
	0xD2: ("O", "grave"), 0xD3: ("O", "acute"), 0xD4: ("O", "circumflex"),
	0xD5: ("O", "tilde"), 0xD6: ("O", "diaeresis"),
	0xD9: ("U", "grave"), 0xDA: ("U", "acute"), 0xDB: ("U", "circumflex"),
	0xDC: ("U", "diaeresis"), 0xDD: ("Y", "acute"),
	0xE0: ("a", "grave"), 0xE1: ("a", "acute"), 0xE2: ("a", "circumflex"),
	0xE3: ("a", "tilde"), 0xE4: ("a", "diaeresis"), 0xE5: ("a", "ring"),
	0xE8: ("e", "grave"), 0xE9: ("e", "acute"), 0xEA: ("e", "circumflex"),
	0xEB: ("e", "diaeresis"),
	0xEC: ("i", "grave"), 0xED: ("i", "acute"), 0xEE: ("i", "circumflex"),
	0xEF: ("i", "diaeresis"),
	0xF1: ("n", "tilde"),
	0xF2: ("o", "grave"), 0xF3: ("o", "acute"), 0xF4: ("o", "circumflex"),
	0xF5: ("o", "tilde"), 0xF6: ("o", "diaeresis"),
	0xF9: ("u", "grave"), 0xFA: ("u", "acute"), 0xFB: ("u", "circumflex"),
	0xFC: ("u", "diaeresis"), 0xFD: ("y", "acute"), 0xFF: ("y", "diaeresis"),
	0x160: ("S", "caron"), 0x161: ("s", "caron"),
	0x17D: ("Z", "caron"), 0x17E: ("z", "caron"),
	0x178: ("Y", "diaeresis"),
}


def dotless(f, g):
	"""'i' without its dot: drop strokes entirely above the x-height."""
	keep = [s for s in g[2] if max(y for _, y in s) > f.x_top + 1]
	return (g[0], g[1], keep)


def compose(f, base, accent):
	g = f.g(base)
	upper = base.isupper()
	if base in "ij":
		g = dotless(f, g)
	top = f.cap_top if upper else f.x_top
	if base in "ij":
		top = f.x_top
	cx = f.anchor(g, top)
	gap = 2 if upper else 2.5
	acc = ACCENT[accent]
	if f.slant:
		acc = xf(acc, lambda x, y: (x - y * f.slant, y))
	acc = shift(acc, cx, top - gap)
	return (g[0], g[1], g[2] + acc)


def side(f, a, b, overlap):
	"""Two glyphs side by side, b moved left by `overlap` units."""
	ga, gb = f.g(a), f.g(b)
	wa = ga[1] - ga[0]
	dx = ga[0] + wa - overlap - gb[0]
	st = ga[2] + shift(gb[2], dx, 0)
	return (ga[0], ga[1] + (gb[1] - gb[0]) - overlap, st)


def merge(*gs):
	left = min(g[0] for g in gs)
	right = max(g[1] for g in gs)
	return (left, right, [s for g in gs for s in g[2]])


def with_strokes(g, extra):
	return (g[0], g[1], g[2] + extra)


def small(f, ch, k=0.6, raise_to=None):
	"""A glyph scaled by k about its baseline origin, its top moved to
	`raise_to` (superscripts, ordinals)."""
	g = f.g(ch)
	st = scale(g[2], k, 0, BASE)
	if raise_to is not None:
		b = bbox((0, 0, st))
		st = shift(st, 0, raise_to - b[1])
	left, right = g[0] * k, g[1] * k
	return (left, right, st)


def extras(f):
	"""Latin-9 punctuation and symbols, and a few typographic
	characters, as {codepoint: glyph}."""
	out = {}
	cap = f.cap_top
	xt = f.x_top
	mid = (BASE + xt) / 2.0		# middle of the x-height

	sp = f.g(" ")
	out[0xA0] = sp
	out[0xAD] = f.g("-")

	# inverted ! and ?: turned about the middle of the x-height, so
	# they hang from it the way Spanish sets them
	for cp, ch in ((0xA1, "!"), (0xBF, "?")):
		g = f.g(ch)
		b = bbox(g)
		cy = (b[1] + b[3]) / 2.0
		st = rot180(g[2], (b[0] + b[2]) / 2.0, cy)
		st = shift(st, 0, (xt + 16 - (b[3] - b[1])) - (cy * 2 - b[3]) + 0)
		bb = bbox((0, 0, st))
		st = shift(st, 0, (BASE + 6) - bb[3])
		out[cp] = (g[0], g[1], st)

	c = f.g("c")
	cb = bbox(c)
	ccx = (cb[0] + cb[2]) / 2.0
	out[0xA2] = with_strokes(c, [[(ccx, xt - 3), (ccx, BASE + 3)]])

	# pound: a stem with a hook, a bar and a foot, in cap units
	w = 14
	out[0xA3] = (-w / 2.0 - 2, w / 2.0 + 2, [
		[(5, cap + 3), (4, cap + 1), (2, cap), (-1, cap), (-3, cap + 2),
		 (-3, BASE - 6), (-4, BASE - 2), (-6, BASE)],
		[(-6, BASE), (6, BASE)],
		[(-7, mid - 1), (2, mid - 1)]])

	C = f.g("C")
	Cb = bbox(C)
	out[0x20AC] = with_strokes(C, [
		[(Cb[0] - 3, (cap + BASE) / 2.0 - 2), (Cb[0] + 9, (cap + BASE) / 2.0 - 2)],
		[(Cb[0] - 3, (cap + BASE) / 2.0 + 2), (Cb[0] + 7, (cap + BASE) / 2.0 + 2)]])

	Y = f.g("Y")
	Yb = bbox(Y)
	ycx = (Yb[0] + Yb[2]) / 2.0
	out[0xA5] = with_strokes(Y, [[(ycx - 5, mid - 1), (ycx + 5, mid - 1)],
		[(ycx - 5, mid + 3), (ycx + 5, mid + 3)]])

	# section: two small S, one above the other
	S1 = small(f, "S", 0.55)
	b1 = bbox(S1)
	st = shift(S1[2], 0, cap - b1[1])
	st2 = shift(S1[2], 0, cap - b1[1] + (b1[3] - b1[1]) * 0.55)
	out[0xA7] = (S1[0] - 1, S1[1] + 1, st + st2)

	# copyright, registered: a ring round a small letter
	for cp, ch in ((0xA9, "C"), (0xAE, "R")):
		r = (BASE - cap) / 2.0 + 2
		cy = (BASE + cap) / 2.0
		inner = small(f, ch, 0.5)
		ib = bbox(inner)
		ist = shift(inner[2], -(ib[0] + ib[2]) / 2.0, cy - (ib[1] + ib[3]) / 2.0)
		out[cp] = (-r - 2, r + 2, ring(0, cy, r, 20) + ist)

	for cp, ch in ((0xAA, "a"), (0xBA, "o")):
		g = small(f, ch, 0.55, raise_to=cap)
		b = bbox(g)
		out[cp] = (g[0], g[1], g[2] + [[(b[0], b[3] + 2), (b[2], b[3] + 2)]])

	out[0xAB] = (-6, 6, [[(-1, mid - 4), (-4, mid), (-1, mid + 4)],
		[(3, mid - 4), (0, mid), (3, mid + 4)]])
	out[0xBB] = (-6, 6, [[(-3, mid - 4), (0, mid), (-3, mid + 4)],
		[(1, mid - 4), (4, mid), (1, mid + 4)]])

	out[0xAC] = (-7, 7, [[(-5, mid - 1), (5, mid - 1), (5, mid + 3)]])
	out[0xAF] = (-6, 6, [[(-5, cap - 3), (5, cap - 3)]])
	out[0xB0] = (-4, 4, ring(0, cap + 3, 2.5))

	plus = f.g("+")
	pb = bbox(plus)
	out[0xB1] = (plus[0], plus[1], shift(plus[2], 0, -2) +
		[[(pb[0], pb[3] + 2), (pb[2], pb[3] + 2)]])

	for cp, ch in ((0xB9, "1"), (0xB2, "2"), (0xB3, "3")):
		out[cp] = small(f, ch, 0.55, raise_to=cap)

	if f.greek:
		for k, v in f.greek_map.items():
			out[ord(v)] = f.greek[ord(k) - 32]
		out[0xB5] = f.gr("μ")
		beta = f.gr("β")
		# a beta whose stem stops at the baseline is a sharp s
		st = xf(beta[2], lambda x, y: (x, min(y, BASE)))
		out[0xDF] = (beta[0], beta[1], st)

	out[0xB6] = (-7, 7, [
		[(1, cap), (1, BASE + 4)], [(4, cap), (4, BASE + 4)],
		[(6, cap), (-1, cap), (-3, cap + 1), (-5, cap + 3), (-5, cap + 6),
		 (-3, cap + 8), (-1, cap + 9), (1, cap + 9)]])

	period = f.g(".")
	pb = bbox(period)
	out[0xB7] = (period[0], period[1], shift(period[2], 0, mid - (pb[1] + pb[3]) / 2.0))

	out[0xD7] = (-7, 7, [[(-4, mid - 4), (4, mid + 4)], [(4, mid - 4), (-4, mid + 4)]])
	minus = [[(-6, mid), (6, mid)]]
	out[0xF7] = (-7, 7, minus + dot(0, mid - 4) + dot(0, mid + 4))

	# ligatures and the rest of the Latin-1 letters
	out[0xC6] = side(f, "A", "E", f.width("E") * 0.55)
	out[0xE6] = side(f, "a", "e", 3)
	out[0x152] = side(f, "O", "E", f.width("E") * 0.45)
	out[0x153] = side(f, "o", "e", 3)

	C = f.g("C")
	out[0xC7] = cedilla(f, C)
	out[0xE7] = cedilla(f, f.g("c"))

	for cp, ch, top in ((0xD8, "O", cap), (0xF8, "o", xt)):
		g = f.g(ch)
		b = bbox(g)
		out[cp] = with_strokes(g, [[(b[0] - 1, BASE + 1), (b[2] + 1, top - 1)]])

	D = f.g("D")
	Db = bbox(D)
	out[0xD0] = with_strokes(D, [[(Db[0] - 3, (cap + BASE) / 2.0),
		(Db[0] + 5, (cap + BASE) / 2.0)]])

	d = f.g("d")
	db = bbox(d)
	out[0xF0] = with_strokes(d, [[(db[2] - 5, cap + 4), (db[2] + 1, cap + 1)]])

	# thorn: P with its bowl moved down; th: b and p laid together
	P = f.g("P")
	Pb = bbox(P)
	st = []
	for s in P[2]:
		xs = [x for x, _ in s]
		ys = [y for _, y in s]
		stem = max(xs) - min(xs) <= 3 and min(xs) <= Pb[0] + 4
		serif = max(ys) - min(ys) <= 1 and (abs(ys[0] - Pb[1]) <= 1 or
			abs(ys[0] - Pb[3]) <= 1) and max(xs) <= Pb[0] + 8
		if stem or (serif and min(ys) > Pb[1] + 1):
			st.append(s)
		elif serif:
			st.append(s)
		else:
			st.append([(x, y + 4) for x, y in s])
	out[0xDE] = (P[0], P[1], st)
	# (P's stem, including a serif's short strokes at the top and the
	# foot, stays; the bowl drops by a fifth of the cap height)
	out[0xFE] = merge(f.g("b"), f.g("p"))

	# typographic characters beyond Latin-9: drawn, so text pasted from
	# elsewhere does not come out as boxes
	q = f.g("'")
	qb = bbox(q)
	qq = [q[2], shift(q[2], 4, 0)]
	out[0x2019] = q
	out[0x2018] = (q[0], q[1], rot180(q[2], (qb[0] + qb[2]) / 2.0, (qb[1] + qb[3]) / 2.0))
	out[0x201D] = (q[0], q[1] + 4, qq[0] + qq[1])
	l = out[0x2018]
	out[0x201C] = (l[0], l[1] + 4, l[2] + shift(l[2], 4, 0))
	comma = f.g(",")
	out[0x201A] = comma
	out[0x201E] = (comma[0], comma[1] + 4, comma[2] + shift(comma[2], 4, 0))
	hy = f.g("-")
	hb = bbox(hy)
	hy_y = (hb[1] + hb[3]) / 2.0
	out[0x2013] = (-7, 7, [[(-6, hy_y), (6, hy_y)]])
	out[0x2014] = (-12, 12, [[(-11, hy_y), (11, hy_y)]])
	out[0x2212] = (-7, 7, [[(-6, mid), (6, mid)]])
	out[0x2022] = (-5, 5, [ring(0, mid, r)[0] for r in (3, 2.2, 1.4, 0.6)])
	p3 = period[2] + shift(period[2], 6, 0) + shift(period[2], 12, 0)
	out[0x2026] = (period[0], period[1] + 12, p3)
	T = small(f, "T", 0.5, raise_to=cap)
	M = small(f, "M", 0.5, raise_to=cap)
	out[0x2122] = merge(T, (T[1] - M[0], T[1] - M[0] + M[1] - M[0],
		shift(M[2], T[1] - M[0], 0)))
	# arrows, left/up/right/down, in the x-height
	a = 7
	out[0x2192] = (-9, 9, [[(-a, mid), (a, mid)], [(a - 4, mid - 3), (a, mid), (a - 4, mid + 3)]])
	out[0x2190] = (-9, 9, [[(a, mid), (-a, mid)], [(-a + 4, mid - 3), (-a, mid), (-a + 4, mid + 3)]])
	out[0x2191] = (-6, 6, [[(0, BASE), (0, cap)], [(-3, cap + 4), (0, cap), (3, cap + 4)]])
	out[0x2193] = (-6, 6, [[(0, cap), (0, BASE)], [(-3, BASE - 4), (0, BASE), (3, BASE - 4)]])
	return out


def cedilla(f, g):
	b = bbox(g)
	cx = (b[0] + b[2]) / 2.0
	hook = [(cx, BASE), (cx + 0.5, BASE + 2), (cx + 2.5, BASE + 3),
		(cx + 2, BASE + 5), (cx - 1.5, BASE + 5.5)]
	return with_strokes(g, [hook])


def glyph_set(f, plain):
	"""All glyphs of a font, {codepoint: glyph}."""
	out = {}
	for i, g in enumerate(f.ascii):
		out[32 + i] = g
	# DEL is not a character (the Hershey files put a box or a ring
	# there); the renderer draws its own box for a missing glyph.
	out.pop(0x7F, None)
	if plain:
		return out
	# Some files have an up arrow where ASCII has the circumflex.
	b = bbox(out[0x5E])
	if b[3] - b[1] > 10:
		out[0x5E] = (-6, 6, [[(-4, f.cap_top + 6), (0, f.cap_top), (4, f.cap_top + 6)]])
	for cp, (base, acc) in COMPOSED.items():
		out[cp] = compose(f, base, acc)
	out.update(extras(f))
	return out


# -- ZHF1 ----------------------------------------------------------------

def q8(v):
	r = int(round(v))
	if r < -127 or r > 127:
		raise ValueError("coordinate out of range: %r" % v)
	return r


def encode_glyph(g):
	left, right, strokes = g
	data = []
	n = 0
	pts_all = []
	for si, s in enumerate(strokes):
		if si:
			data.append((-128, 0))
			n += 1
		prev = None
		for x, y in s:
			p = (q8(x - left), q8(y - BASE))
			if p == prev:
				continue
			data.append(p)
			pts_all.append(p)
			prev = p
			n += 1
	if n > 255:
		raise ValueError("glyph too long")
	if pts_all:
		xs = [p[0] for p in pts_all]
		ys = [p[1] for p in pts_all]
		box = (min(xs), min(ys), max(xs), max(ys))
	else:
		box = (0, 0, 0, 0)
	adv = q8(right - left)
	out = struct.pack("<Bbbbb B", max(adv, 0), *box, n)
	for x, y in data:
		out += struct.pack("<bb", x, y)
	return out, box


def write_zhf(path, family, flags, f, glyphs):
	cps = sorted(glyphs)
	blobs, offs = b"", []
	asc = desc = 0
	for cp in cps:
		off = len(blobs)
		if off > 0xFFFF:
			raise ValueError("font data over 64KB")
		b, box = encode_glyph(glyphs[cp])
		offs.append(off)
		blobs += b
		if box != (0, 0, 0, 0):
			asc = max(asc, -box[1])
			desc = max(desc, box[3])
	hdr = b"ZHF1" + struct.pack("<BBBBBBBB", 1, flags, 32,
		BASE - f.cap_top, BASE - f.x_top, asc, desc,
		max(1, f.g(" ")[1] - f.g(" ")[0]))
	hdr += struct.pack("<HH", len(cps), 0)
	hdr += family.encode().ljust(16, b"\0")[:16]
	idx = b"".join(struct.pack("<HH", cp, o) for cp, o in zip(cps, offs))
	with open(path, "wb") as fp:
		fp.write(hdr + idx + blobs)
	return len(hdr) + len(idx) + len(blobs), len(cps)


# -- ZFN1 from BDF -------------------------------------------------------

def latin9_set():
	cps = list(range(0x20, 0x7F))
	for b in range(0xA0, 0x100):
		cps.append(ord(bytes([b]).decode("iso8859-15")))
	cps += [0x2018, 0x2019, 0x201C, 0x201D, 0x2013, 0x2014, 0x2022, 0x2026,
		0x2122, 0x2190, 0x2191, 0x2192, 0x2193, 0x2212, 0x201A, 0x201E]
	return sorted(set(cps))


def read_bdf(path):
	glyphs = {}
	fw = fh = asc = None
	fbx = None
	enc = None
	bbx = None
	rows = None
	for line in open(path, encoding="latin-1"):
		line = line.rstrip("\n")
		if line.startswith("FONTBOUNDINGBOX"):
			fbx = [int(v) for v in line.split()[1:]]
			fw, fh = fbx[0], fbx[1]
		elif line.startswith("FONT_ASCENT"):
			asc = int(line.split()[1])
		elif line.startswith("ENCODING"):
			enc = int(line.split()[1])
		elif line.startswith("BBX"):
			bbx = [int(v) for v in line.split()[1:]]
		elif line == "BITMAP":
			rows = []
		elif line == "ENDCHAR":
			glyphs[enc] = (bbx, rows)
			rows = None
		elif rows is not None:
			rows.append(int(line, 16))
	return fw, fh, asc, fbx, glyphs


def write_zfn(path, bdf):
	fw, fh, asc, fbx, glyphs = read_bdf(bdf)
	stride = (fw + 7) // 8
	cps = [cp for cp in latin9_set() if cp in glyphs]
	out = b"ZFN1" + struct.pack("<BBBB", fw, fh, stride, asc) + \
		struct.pack("<I", len(cps))
	out += b"".join(struct.pack("<H", cp) for cp in cps)
	while len(out) % 4:
		out += b"\0"
	# font-level origin: the cell's bottom-left is at (fbx[2], fbx[3])
	for cp in cps:
		(bw, bh, bx, by), rows = glyphs[cp]
		cell = [0] * fh
		nbytes = (bw + 7) // 8
		for r, v in enumerate(rows):
			# row r of the glyph sits at cell row (asc - by - bh + r)
			cy = asc - (by + bh) + r
			if 0 <= cy < fh:
				# v holds nbytes*8 bits, MSB = pixel 0 of the glyph box
				bits = v >> 0
				cellv = 0
				for px in range(bw):
					if bits & (1 << (nbytes * 8 - 1 - px)):
						cx = bx - fbx[2] + px
						if 0 <= cx < fw:
							cellv |= 1 << (stride * 8 - 1 - cx)
				cell[cy] |= cellv
		for v in cell:
			out += v.to_bytes(stride, "big")
	with open(path, "wb") as fp:
		fp.write(out)
	return len(out), len(cps)


# -- proof sheets ----------------------------------------------------------

def sheet(path, f, glyphs, title):
	from PIL import Image, ImageDraw
	cps = sorted(glyphs)
	S = 2.0
	cols = 16
	cw, ch = 34 * S, 40 * S
	rows = (len(cps) + cols - 1) // cols
	img = Image.new("L", (int(cols * cw) + 20, int(rows * ch) + 40), 255)
	d = ImageDraw.Draw(img)
	d.text((10, 5), title, fill=0)
	for i, cp in enumerate(cps):
		g = glyphs[cp]
		ox = 10 + (i % cols) * cw + 6
		oy = 30 + (i // cols) * ch + 26 * S
		d.text((ox - 4, oy - 26 * S + 2), "%04X" % cp, fill=160)
		d.line([(ox, oy), (ox + (g[1] - g[0]) * S, oy)], fill=200)
		for s in g[2]:
			p = [(ox + (x - g[0]) * S, oy + (y - BASE) * S) for x, y in s]
			if len(p) > 1:
				d.line(p, fill=0, width=2)
	img.save(path)


def main():
	args = sys.argv[1:]
	sheet_dir = bdf_dir = None
	if "--sheet" in args:
		sheet_dir = args[args.index("--sheet") + 1]
		os.makedirs(sheet_dir, exist_ok=True)
	if "--bdf" in args:
		bdf_dir = args[args.index("--bdf") + 1]
	os.makedirs(ZHF_DIR, exist_ok=True)
	for name, src, family, flags, greek in HERSHEY:
		f = Font(src, greek)
		plain = family in ("greek", "symbol")
		glyphs = glyph_set(f, plain)
		n, cnt = write_zhf(os.path.join(ZHF_DIR, name + ".zhf"), family, flags, f, glyphs)
		print("%-8s %-10s %3d glyphs %6d bytes" % (name, src, cnt, n))
		if sheet_dir:
			sheet(os.path.join(sheet_dir, name + ".png"), f, glyphs, name)
	if bdf_dir:
		os.makedirs(ZFN_DIR, exist_ok=True)
		for name, src in FIXED:
			n, cnt = write_zfn(os.path.join(ZFN_DIR, name + ".zfn"),
				os.path.join(bdf_dir, src + ".bdf"))
			print("%-8s %-10s %3d glyphs %6d bytes" % (name, src, cnt, n))


if __name__ == "__main__":
	main()
