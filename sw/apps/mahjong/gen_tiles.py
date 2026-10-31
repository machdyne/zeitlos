#!/usr/bin/env python3
#
# Zeitlos mahjong -- tile face generator.
#
#   cd sw/apps/mahjong && python3 gen_tiles.py     (or: make tiles)
#
# Emits mj_art.c / mj_art.h, and tests/out/faces-<size>.png to look at.
# The C is committed, so the app builds with no Python in the loop (the
# arrangement of sw/common/games/gen_cards.py and sw/apps/chess's
# gen_pieces.py). It must regenerate identically:
#
#     python3 gen_tiles.py && git diff --stat
#
# -- what is generated, and what is not --
#
# Only the pictures ON the faces. The tile itself -- its face, its two
# sides, its outline, its colour by layer and state -- is drawn by
# mj_board.c from three numbers per size (face width, face height,
# depth), because it is the same rectangle for every face and changes
# with the view corner. Baking 42 faces x 4 corners x states would be
# most of a megabyte; the geometry is a few lines of C.
#
# -- the glyphs come from the tree --
#
# The characters (the numerals, the winds, the dragons, the flowers and
# seasons) are taken from sw/data/font/jp12.zfn: Shinonome 12x12, public
# domain, already in the tree for sw/apps/jfont. Nothing is downloaded
# and no font file is needed beyond what is committed. At 12x12 a glyph
# is drawn as it is; the large size scales it with EPX (Scale2x) to
# 24x24 and, where 24 does not fit, box-filters that down to 18x18,
# which keeps strokes one or two pixels wide rather than doubling every
# stair-step.
#
# The dots and the bamboo are drawn here, from circles and sticks,
# because no font has them and they are geometry anyway.
#
# Nobody has to read any of it: the game only asks that two faces be
# told apart, so the tiles carry no Western corner indices. What matters
# is that every face is different from every other at a glance, which
# the contact sheet in tests/out is for.
#
# -- the output --
#
# Per size and face, a list of PARTS: one colour (MJ_GC_INK, _RED,
# _GREEN, _BLUE) and a 1bpp mask, cropped to its bounding box, at an
# offset from the face's top-left. Rows are uint32 with the LEAST
# significant bit leftmost (zgfx's order, and mj_canvas.c's). The inner
# area is narrower than 32 at both sizes, so a part's row is one word.

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
JFONT = os.path.join(ROOT, "sw", "data", "font", "jp12.zfn")

# Colours of the glyphs (mj_art.h). 0 is transparent.
INK, RED, GREEN, BLUE = 1, 2, 3, 4
NAMES = {INK: "INK", RED: "RED", GREEN: "GREEN", BLUE: "BLUE"}

# The sizes: face width, face height, depth (the side's thickness, which
# is also how far a layer is offset). The usable area is inside a one
# pixel outline.
SIZES = [
	("small", 20, 26, 3),
	("large", 30, 40, 4),
]

# -- the font ---------------------------------------------------------------

def load_jfont():
	d = open(JFONT, "rb").read()
	if d[:4] != b"ZFN1":
		sys.exit("jp12.zfn: not a ZFN1 file")
	w, h, st = d[4], d[5], d[6]
	n = struct.unpack("<I", d[8:12])[0]
	cps = struct.unpack("<%dH" % n, d[12:12 + 2 * n])
	off = 12 + 2 * n
	off += (-off) % 4
	index = {cp: i for i, cp in enumerate(cps)}

	def glyph(ch):
		i = index.get(ord(ch))
		if i is None:
			sys.exit("jp12.zfn has no %r" % ch)
		b = d[off + i * h * st: off + (i + 1) * h * st]
		g = []
		for r in range(h):
			v = int.from_bytes(b[r * st: r * st + st], "big")
			g.append([(v >> (st * 8 - 1 - x)) & 1 for x in range(w)])
		return g
	return glyph

def scale2x(g):
	"""EPX: each pixel becomes four, corners rounded where the
	neighbours agree. Keeps diagonals diagonal."""
	h, w = len(g), len(g[0])
	def at(x, y):
		if 0 <= x < w and 0 <= y < h:
			return g[y][x]
		return 0
	out = [[0] * (2 * w) for _ in range(2 * h)]
	for y in range(h):
		for x in range(w):
			p = g[y][x]
			a, b, c, d = at(x, y - 1), at(x + 1, y), at(x - 1, y), at(x, y + 1)
			e0 = a if (c == a and c != d and a != b) else p
			e1 = b if (a == b and a != c and b != d) else p
			e2 = c if (d == c and d != b and c != a) else p
			e3 = d if (b == d and b != a and d != c) else p
			out[2 * y][2 * x] = e0
			out[2 * y][2 * x + 1] = e1
			out[2 * y + 1][2 * x] = e2
			out[2 * y + 1][2 * x + 1] = e3
	return out

def shrink(g, n):
	"""Box filter to n x n, threshold just under half, so thin strokes
	survive the shrink rather than breaking up."""
	h, w = len(g), len(g[0])
	out = [[0] * n for _ in range(n)]
	for y in range(n):
		for x in range(n):
			x0, x1 = x * w / n, (x + 1) * w / n
			y0, y1 = y * h / n, (y + 1) * h / n
			s = a = 0.0
			yy = int(y0)
			while yy < y1:
				xx = int(x0)
				while xx < x1:
					ox = min(xx + 1, x1) - max(xx, x0)
					oy = min(yy + 1, y1) - max(yy, y0)
					s += g[yy][xx] * ox * oy
					a += ox * oy
					xx += 1
				yy += 1
			out[y][x] = 1 if s / a >= 0.42 else 0
	return out

def crop_rows(g):
	"""Drops empty rows at the top and bottom."""
	rows = [r for r in g]
	while rows and not any(rows[0]):
		rows.pop(0)
	while rows and not any(rows[-1]):
		rows.pop()
	return rows

# -- a face being drawn -------------------------------------------------------

class Face:
	def __init__(self, w, h):
		self.w, self.h = w, h
		self.px = [[0] * w for _ in range(h)]

	def put(self, x, y, c):
		if 0 <= x < self.w and 0 <= y < self.h:
			self.px[y][x] = c

	def rect(self, x, y, w, h, c):
		for yy in range(y, y + h):
			for xx in range(x, x + w):
				self.put(xx, yy, c)

	def frame(self, x, y, w, h, c):
		self.rect(x, y, w, 1, c)
		self.rect(x, y + h - 1, w, 1, c)
		self.rect(x, y, 1, h, c)
		self.rect(x + w - 1, y, 1, h, c)

	def bits(self, g, x, y, c):
		for yy, row in enumerate(g):
			for xx, v in enumerate(row):
				if v:
					self.put(x + xx, y + yy, c)

	def disc(self, cx2, cy2, d, c):
		"""A filled circle of diameter d whose centre is at (cx2/2, cy2/2):
		half-pixel centres, so even diameters sit on pixel boundaries."""
		r2 = (d / 2.0) ** 2
		for y in range(self.h):
			for x in range(self.w):
				dx = x + 0.5 - cx2 / 2.0
				dy = y + 0.5 - cy2 / 2.0
				if dx * dx + dy * dy <= r2 + 0.25:
					self.put(x, y, c)

	def dot(self, cx2, cy2, d, c, centre=None):
		"""A dot as on the tiles: a disc, its middle punched out, and a
		pip of another colour (or none) at its heart."""
		self.disc(cx2, cy2, d, c)
		if d >= 5:
			self.disc(cx2, cy2, d - 2 - (d >= 9), 0)
			self.disc(cx2, cy2, max(1, d // 3), centre or c)
		elif d >= 4:
			self.disc(cx2, cy2, 2, 0 if centre is None else centre)

	def stick(self, x, y, w, h, c):
		"""One bamboo stick: a rod with a node at each end and one in the
		middle, the nodes a pixel wider where there is room."""
		self.rect(x, y, w, h, c)
		if w >= 3:
			for yy in (y + h // 2,):
				self.put(x - 1 if w >= 4 else x, yy, c)
				self.put(x + w if w >= 4 else x + w - 1, yy, c)
			# a highlight down the rod reads as round
			hl = x + w // 2
			for yy in range(y + 1, y + h - 1):
				if yy != y + h // 2:
					self.put(hl, yy, 0)

# -- the faces ---------------------------------------------------------------

NUMERALS = "一二三四五六七八九"
WINDS = "東南西北"
FLOWERS = "梅蘭菊竹"
SEASONS = "春夏秋冬"

def glyph_at(size, ch, glyph):
	"""A character at the size the face takes: 12 for small; 24 or 18
	for large (callers say which)."""
	g = glyph(ch)
	if size == 12:
		return g
	g2 = scale2x(g)
	if size == 24:
		return g2
	return shrink(g2, size)

def centre_x(fw, gw):
	return (fw - gw) // 2

# Dot layouts: (x, y) in thirds of the usable area, with colours.
# Coordinates are fractions 0..1 of the inner box.
def dot_layout(n):
	"""Dot centres as fractions of how far a dot can move inside the
	box: 0 is touching the left (or top) edge, 1 the right (bottom)."""
	B, R, G = BLUE, RED, GREEN
	L2, R2 = 0.06, 0.94
	L = {
		2: [(0.5, 0.1, G), (0.5, 0.9, B)],
		3: [(0.0, 0.0, B), (0.5, 0.5, R), (1.0, 1.0, G)],
		4: [(L2, 0.12, B), (R2, 0.12, G), (L2, 0.88, G), (R2, 0.88, B)],
		5: [(0.0, 0.05, B), (1.0, 0.05, G), (0.5, 0.5, R), (0.0, 0.95, G), (1.0, 0.95, B)],
		6: [(L2, 0.0, G), (R2, 0.0, G),
		    (L2, 0.55, R), (R2, 0.55, R), (L2, 1.0, R), (R2, 1.0, R)],
		7: [(0.0, 0.0, G), (0.5, 0.13, G), (1.0, 0.26, G),
		    (L2, 0.63, R), (R2, 0.63, R), (L2, 1.0, R), (R2, 1.0, R)],
		8: [(L2, y, B) for y in (0.0, 1 / 3.0, 2 / 3.0, 1.0)] +
		   [(R2, y, B) for y in (0.0, 1 / 3.0, 2 / 3.0, 1.0)],
		9: [(x, 0.0, B) for x in (0.0, 0.5, 1.0)] +
		   [(x, 0.5, R) for x in (0.0, 0.5, 1.0)] +
		   [(x, 1.0, G) for x in (0.0, 0.5, 1.0)],
	}
	return L[n]

def draw_dots(f, n, ix, iy, iw, ih, big):
	if n == 1:
		d = min(iw, ih) - (1 if big else 1)
		cx2, cy2 = 2 * ix + iw, 2 * iy + ih
		f.disc(cx2, cy2, d, BLUE)
		f.disc(cx2, cy2, d - 2, 0)
		f.disc(cx2, cy2, d - 4, GREEN)
		f.disc(cx2, cy2, d - 6, 0)
		f.disc(cx2, cy2, max(3, d - 10), RED)
		if d - 12 >= 1:
			f.disc(cx2, cy2, d - 12, 0)
		return
	# dot diameter by how many share a row/column
	if n == 2:
		d = 11 if big else 8
	elif n == 4 or n == 6:
		d = 10 if big else 7
	elif n >= 7:
		d = 8 if big else 5
	else:
		d = 8 if big else 6
	for (fx, fy, c) in dot_layout(n):
		# centres inside the box, a radius in from its edges
		cx = ix + d / 2.0 + fx * (iw - d)
		cy = iy + d / 2.0 + fy * (ih - d)
		f.dot(int(round(2 * cx)), int(round(2 * cy)), d,
		      c, RED if c != RED else BLUE)

BIRD_SMALL = [
	"....rr..........",
	"...rrrr.........",
	"..gr.rr.........",
	"...rrrgg........",
	"....ggggg.......",
	"...gg.gggg......",
	"..gg.ggg.gg.....",
	"..g.ggggg.g.....",
	"..gggg.ggg......",
	"...ggggggg.g....",
	"....gg.ggggg....",
	".....ggggg.gg...",
	"......gggg..gg..",
	"......g..g...gg.",
	".....rr..rr...g.",
	"....r.r.r.r.....",
]

def scale_ascii(rows, k):
	out = []
	for r in rows:
		line = "".join(ch * k for ch in r)
		for _ in range(k):
			out.append(line)
	return out

def draw_bird(f, ix, iy, iw, ih, big):
	rows = BIRD_SMALL
	if big:
		g = [[1 if ch != "." else 0 for ch in r] for r in rows]
		# scale the shape with EPX, then colour each pixel by the
		# colour of the source pixel it came from
		g2 = scale2x(g)
		rows = ["".join((rows[y // 2][x // 2] if g2[y][x] else ".")
				for x in range(len(g2[0]))) for y in range(len(g2))]
	h, w = len(rows), len(rows[0])
	x0 = ix + (iw - w) // 2
	y0 = iy + (ih - h) // 2
	for y, r in enumerate(rows):
		for x, ch in enumerate(r):
			if ch == "g":
				f.put(x0 + x, y0 + y, GREEN)
			elif ch == "r":
				f.put(x0 + x, y0 + y, RED)

def draw_bamboo(f, n, ix, iy, iw, ih, big):
	if n == 1:
		draw_bird(f, ix, iy, iw, ih, big)
		return
	sw = 4 if big else 3
	gap = 2 if big else 1
	def col_x(k, cols):
		sp = (gap + (2 if big else 1)) if cols < 4 else gap
		tot = cols * sw + (cols - 1) * sp
		return ix + (iw - tot) // 2 + k * (sw + sp)
	def rows_of(r):
		g = 2 if big else 1
		sh = (ih - (r - 1) * g) // r
		return sh, g
	G, R = GREEN, RED
	if n in (2, 3, 4, 6, 8, 9):
		layouts = {
			2: [(1, [G]), (1, [G])],
			3: [(1, [G]), (2, [G, G])],
			4: [(2, [G, G]), (2, [G, G])],
			6: [(3, [G, G, G]), (3, [G, G, G])],
			8: [(4, [G, G, G, G]), (4, [G, G, G, G])],
			9: [(3, [R, G, R]), (3, [R, G, R]), (3, [R, G, R])],
		}
		rows = layouts[n]
		sh, g = rows_of(len(rows))
		for ri, (cols, colours) in enumerate(rows):
			y = iy + ri * (sh + g)
			for k in range(cols):
				f.stick(col_x(k, cols), y, sw, sh, colours[k])
		if n == 8:
			# the eight's two rows lean in towards each other, as on
			# the real tile: the middle sticks of each row slant
			pass
		return
	if n == 5:
		sh, g = rows_of(2)
		for k in (0, 2):
			f.stick(col_x(k, 3), iy, sw, sh, G)
			f.stick(col_x(k, 3), iy + sh + g, sw, sh, G)
		f.stick(col_x(1, 3), iy + (ih - sh) // 2, sw, sh, R)
		return
	if n == 7:
		sh, g = rows_of(3)
		f.stick(col_x(1, 3), iy, sw, sh, R)
		for ri in (1, 2):
			for k in range(3):
				f.stick(col_x(k, 3), iy + ri * (sh + g), sw, sh, G)
		return

def draw_chars(f, n, glyph, ix, iy, iw, ih, big):
	size = 18 if big else 12
	num = crop_rows(glyph_at(size, NUMERALS[n], glyph))
	wan = glyph_at(size, "萬", glyph)
	wan = crop_rows(wan)
	gap = 1
	total = len(num) + gap + len(wan)
	y = iy + max(0, (ih - total) // 2)
	f.bits(num, ix + centre_x(iw, size), y, BLUE)
	f.bits(wan, ix + centre_x(iw, size), iy + ih - len(wan) - max(0, (ih - total) // 2), RED)

def draw_big_glyph(f, ch, glyph, c, ix, iy, iw, ih, big):
	size = 24 if big else 12
	g = glyph_at(size, ch, glyph)
	if not big:
		# 12 in an 18x24 box: draw it once, centred, one row lower than
		# the middle so the face does not look top-heavy
		f.bits(g, ix + centre_x(iw, 12), iy + (ih - 12) // 2 + 1, c)
		return
	f.bits(g, ix + centre_x(iw, size), iy + (ih - size) // 2, c)

def draw_white_dragon(f, ix, iy, iw, ih, big):
	m = 1 if big else 1
	t = 2 if big else 1
	x, y, w, h = ix + m, iy + m + 1, iw - 2 * m, ih - 2 * m - 2
	for k in range(t):
		f.frame(x + k, y + k, w - 2 * k, h - 2 * k, BLUE)
	k = t + 1
	for kk in range(t):
		f.frame(x + k + kk, y + k + kk, w - 2 * (k + kk), h - 2 * (k + kk), BLUE)

def draw_flower(f, n, glyph, ix, iy, iw, ih, big):
	size = 18 if big else 12
	g = crop_rows(glyph_at(size, FLOWERS[n], glyph))
	# a four-petal blossom above the character
	d = 9 if big else 7
	cx2 = 2 * ix + iw
	top = iy + (1 if big else 0)
	cy2 = 2 * top + d
	pr = 3 if big else 2
	for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0)):
		f.disc(cx2 + dx * (d - pr), cy2 + dy * (d - pr), pr * 2 - 1 if big else pr + 1, RED)
	f.disc(cx2, cy2, 3 if big else 2, GREEN)
	f.bits(g, ix + centre_x(iw, size), iy + ih - len(g), RED)

def draw_season(f, n, glyph, ix, iy, iw, ih, big):
	size = 18 if big else 12
	g = crop_rows(glyph_at(size, SEASONS[n], glyph))
	# a sun above the character: a disc and eight rays
	d = 7 if big else 5
	cx = ix + iw // 2
	top = iy + (2 if big else 1)
	cy = top + d // 2 + 1
	f.disc(2 * cx + 1, 2 * cy + 1, d, BLUE)
	r = d // 2 + (3 if big else 2)
	for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0), (-1, -1), (1, 1), (-1, 1), (1, -1)):
		f.put(cx + dx * r, cy + dy * r, BLUE)
		if big:
			f.put(cx + dx * (r - 1), cy + dy * (r - 1), BLUE)
	f.bits(g, ix + centre_x(iw, size), iy + ih - len(g), GREEN)

def draw_face(face, fw, fh, glyph):
	"""The picture on one face: a Face of the whole tile face, outline
	included (the outline itself is the board's to draw)."""
	big = fw >= 28
	f = Face(fw, fh)
	ix, iy = 1, 1
	iw, ih = fw - 2, fh - 2
	if big:
		ix, iy, iw, ih = 2, 2, fw - 4, fh - 4
	if face < 9:
		draw_dots(f, face + 1, ix + (1 if big else 0), iy + (1 if big else 0),
			  iw - (2 if big else 0), ih - (2 if big else 0), big)
	elif face < 18:
		draw_bamboo(f, face - 9 + 1, ix, iy + (1 if big else 0), iw, ih - (2 if big else 0), big)
	elif face < 27:
		draw_chars(f, face - 18, glyph, ix, iy, iw, ih, big)
	elif face < 31:
		draw_big_glyph(f, WINDS[face - 27], glyph, INK, ix, iy, iw, ih, big)
	elif face == 31:
		draw_big_glyph(f, "中", glyph, RED, ix, iy, iw, ih, big)
	elif face == 32:
		draw_big_glyph(f, "發", glyph, GREEN, ix, iy, iw, ih, big)
	elif face == 33:
		draw_white_dragon(f, ix, iy, iw, ih, big)
	elif face < 38:
		draw_flower(f, face - 34, glyph, ix, iy, iw, ih, big)
	else:
		draw_season(f, face - 38, glyph, ix, iy, iw, ih, big)
	return f

# -- output ---------------------------------------------------------------

def parts_of(f):
	"""One part per colour present: the bounding box and its rows."""
	out = []
	for c in (INK, RED, GREEN, BLUE):
		pts = [(x, y) for y in range(f.h) for x in range(f.w) if f.px[y][x] == c]
		if not pts:
			continue
		x0 = min(p[0] for p in pts); x1 = max(p[0] for p in pts)
		y0 = min(p[1] for p in pts); y1 = max(p[1] for p in pts)
		w, h = x1 - x0 + 1, y1 - y0 + 1
		if w > 32:
			sys.exit("a part wider than 32")
		rows = []
		for y in range(y0, y1 + 1):
			v = 0
			for x in range(x0, x1 + 1):
				if f.px[y][x] == c:
					v |= 1 << (x - x0)
			rows.append(v)
		out.append((c, x0, y0, w, h, rows))
	return out

PALETTE = {0: (0xff, 0xf8, 0xe8), INK: (0x22, 0x22, 0x22), RED: (0xc0, 0x22, 0x22),
	   GREEN: (0x11, 0x88, 0x33), BLUE: (0x22, 0x44, 0xaa)}

def contact_sheet(name, faces, fw, fh, path):
	try:
		from PIL import Image
	except ImportError:
		print("no Pillow: skipping", path)
		return
	cols = 9
	rows = (len(faces) + cols - 1) // cols
	pad = 2
	img = Image.new("RGB", (cols * (fw + pad) + pad, rows * (fh + pad) + pad), (0x15, 0x55, 0x33))
	for i, f in enumerate(faces):
		ox = pad + (i % cols) * (fw + pad)
		oy = pad + (i // cols) * (fh + pad)
		for y in range(fh):
			for x in range(fw):
				edge = x == 0 or y == 0 or x == fw - 1 or y == fh - 1
				c = (0x44, 0x44, 0x44) if edge else PALETTE[f.px[y][x]]
				img.putpixel((ox + x, oy + y), c)
	img = img.resize((img.width * 3, img.height * 3), Image.NEAREST)
	os.makedirs(os.path.dirname(path), exist_ok=True)
	img.save(path)

def main():
	glyph = load_jfont()
	all_parts = []
	rows = []
	sizes = []
	for name, fw, fh, depth in SIZES:
		faces = [draw_face(i, fw, fh, glyph) for i in range(42)]
		contact_sheet(name, faces, fw, fh, os.path.join(HERE, "tests", "out", "faces-%s.png" % name))
		first = []
		for i, f in enumerate(faces):
			first.append(len(all_parts))
			for (c, x, y, w, h, r) in parts_of(f):
				all_parts.append((i, c, x, y, w, h, len(rows)))
				rows.extend(r)
		first.append(len(all_parts))
		sizes.append((name, fw, fh, depth, first))

	h = []
	h.append("/* Generated by gen_tiles.py -- do not hand-edit. */")
	h.append("/* Re-run it (make tiles) after changing the art; the output is")
	h.append(" * committed so the build needs no Python. */")
	h.append("")
	h.append("#ifndef MJ_ART_H")
	h.append("#define MJ_ART_H")
	h.append("")
	h.append("#include <stdint.h>")
	h.append("")
	h.append("/*")
	h.append(" * The pictures on the faces, at each size. A face is a list of")
	h.append(" * PARTS, one per colour it uses: a 1bpp mask, cropped to its")
	h.append(" * bounding box, at (x, y) from the face's top-left. Rows are one")
	h.append(" * uint32_t each, LEAST significant bit leftmost. Part i's rows are")
	h.append(" * mj_art_rows[part.off .. part.off + part.h).")
	h.append(" *")
	h.append(" * Faces are numbered as mj_game.h's MJ_F_*. The parts of face f at")
	h.append(" * size s are mj_art_parts[first[f] .. first[f + 1]).")
	h.append(" */")
	h.append("")
	h.append("#define MJ_GC_INK   1")
	h.append("#define MJ_GC_RED   2")
	h.append("#define MJ_GC_GREEN 3")
	h.append("#define MJ_GC_BLUE  4")
	h.append("")
	h.append("#define MJ_ART_SIZES %d" % len(SIZES))
	for k, (name, fw, fh, depth, first) in enumerate(sizes):
		h.append("#define MJ_ART_%s %d   /* %dx%d, depth %d */" % (name.upper(), k, fw, fh, depth))
	h.append("")
	h.append("typedef struct {")
	h.append("\tuint8_t  face_w, face_h, depth;")
	h.append("\tuint16_t first[43];")
	h.append("} mj_art_size_t;")
	h.append("")
	h.append("typedef struct {")
	h.append("\tuint8_t  col, x, y, w, h;")
	h.append("\tuint16_t off;")
	h.append("} mj_art_part_t;")
	h.append("")
	h.append("extern const mj_art_size_t mj_art[MJ_ART_SIZES];")
	h.append("extern const mj_art_part_t mj_art_parts[%d];" % len(all_parts))
	h.append("extern const uint32_t mj_art_rows[%d];" % len(rows))
	h.append("")
	h.append("#endif")
	open(os.path.join(HERE, "mj_art.h"), "w").write("\n".join(h) + "\n")

	c = []
	c.append("/* Generated by gen_tiles.py -- do not hand-edit. */")
	c.append("")
	c.append('#include "mj_art.h"')
	c.append("")
	c.append("const mj_art_size_t mj_art[MJ_ART_SIZES] = {")
	for (name, fw, fh, depth, first) in sizes:
		c.append("\t{ %d, %d, %d, {" % (fw, fh, depth))
		for i in range(0, len(first), 12):
			c.append("\t\t" + ", ".join(str(v) for v in first[i:i + 12]) + ",")
		c.append("\t} },")
	c.append("};")
	c.append("")
	c.append("const mj_art_part_t mj_art_parts[%d] = {" % len(all_parts))
	for (face, col, x, y, w, hh, off) in all_parts:
		c.append("\t{ MJ_GC_%s, %d, %d, %d, %d, %d },\t/* face %d */" % (NAMES[col], x, y, w, hh, off, face))
	c.append("};")
	c.append("")
	c.append("const uint32_t mj_art_rows[%d] = {" % len(rows))
	for i in range(0, len(rows), 6):
		c.append("\t" + ", ".join("0x%08x" % v for v in rows[i:i + 6]) + ",")
	c.append("};")
	open(os.path.join(HERE, "mj_art.c"), "w").write("\n".join(c) + "\n")
	print("mj_art.c: %d parts, %d rows (%d bytes of rows)" % (len(all_parts), len(rows), 4 * len(rows)))

if __name__ == "__main__":
	main()
