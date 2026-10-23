#!/usr/bin/env python3
#
# pixconv.py -- pix's files to and from a build host's (docs/pix_app.md,
# "Into the tree"): art drawn on the board, brought into the source.
#
# Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
#
#   pixconv.py zic  FILE.zic DIR       every icon as DIR/icon-<name>.png,
#                                      32x32 black and white: what
#                                      sw/data/icons wants (then run
#                                      gen_dock_icon_data.py there);
#                                      apps.txt lines for them on stdout
#   pixconv.py zsp  FILE.zsp DIR [--as N] [--sheet]
#                                      every sprite as DIR/<name>.png
#                                      (RGBA: see-through is alpha 0),
#                                      or with --sheet one picture of
#                                      them all; --as 64/16/4/2 shows
#                                      the sheet as a board with that
#                                      many colours would
#   pixconv.py zbm  FILE.zbm OUT.png [--as N]
#                                      a picture, one-bit or colour
#   pixconv.py zfn  FILE.zfn OUT.bdf   a font as BDF
#
# Nothing here writes pix's own formats: pix brings PNG in itself, and
# gen_dock_icon_data.py and mausrun's gen_sprites.py make the tree's
# .zic and .zsp from their sources.
#
# DEPENDENCIES: Pillow (zic, zsp, zbm).

import os
import struct
import sys

# -- the palette (sw/common/zsprite.c, by the same arithmetic) --

C64 = [0x000, 0xfff, 0x633, 0x7aa, 0x748, 0x584, 0x327, 0xbc7,
       0x752, 0x430, 0x965, 0x444, 0x666, 0x9c8, 0x66b, 0x999]
GIVEN_TO64, GIVEN_TO16, GIVEN_SLOT, GIVEN_INK = 1, 2, 4, 8


def cdiv(a, b):
	"""C's integer division: toward zero."""
	q = abs(a) // abs(b)
	return q if (a >= 0) == (b >= 0) else -q


def shade(rgb, t):
	out = 0
	for k in (0, 4, 8):
		v = (rgb >> k) & 15
		v = v + cdiv(v * t, 16) if t < 0 else v + cdiv((15 - v) * t, 16)
		out |= v << k
	return out


def lum(rgb):
	r, g, b = (rgb >> 8) & 15, (rgb >> 4) & 15, rgb & 15
	return (r * 2 + g * 5 + b + 4) // 8


def dist(a, b):
	dr = ((a >> 8) & 15) - ((b >> 8) & 15)
	dg = ((a >> 4) & 15) - ((b >> 4) & 15)
	db = (a & 15) - (b & 15)
	return dr * dr * 2 + dg * dg * 4 + db * db


class Palette:
	def __init__(self, colours):
		self.colours = colours
		self.slots = [0x000, 0x555, 0xaaa, 0xfff]
		self.rgb = [0] * 256
		self.to64 = [0] * 256
		self.to16 = [0] * 256
		self.slot = [0] * 256
		self.ink = [0] * 256
		self.given = [0] * 256
		for i in range(16):
			self.rgb[i] = C64[i]
		for i in range(16, 64):
			self.rgb[i] = shade(C64[i & 15], (-8, 8, -12)[(i - 16) // 16])
		for i in range(64, 256):
			k = (i - 64) // 16
			self.rgb[i] = shade(C64[i & 15], -2 - k * 2 if k < 6 else (k - 5) * 2)
		for i in range(256):
			self.defaults(i)

	def nearest(self, rgb, n):
		best, bd = 0, 1 << 30
		for i in range(n):
			d = dist(self.rgb[i], rgb)
			if d < bd:
				bd, best = d, i
		return best

	def defaults(self, i):
		l = lum(self.rgb[i])
		g = self.given[i]
		if not g & GIVEN_TO64:
			self.to64[i] = i if i < 64 else self.nearest(self.rgb[i], 64)
		if not g & GIVEN_TO16:
			self.to16[i] = i if i < 16 else self.nearest(self.rgb[i], 16)
		if not g & GIVEN_SLOT:
			self.slot[i] = l * 4 // 16
		if not g & GIVEN_INK:
			self.ink[i] = (l * 16 + 7) // 15

	def shown(self, idx, target):
		"""RGB444 of idx as a board of `target` colours shows it (zsp_shown_rgb)."""
		if target >= self.colours:
			return self.rgb[idx]
		r = idx
		if target <= 2:
			return 0xfff if self.ink[idx] >= 8 else 0x000
		if target <= 4:
			return self.slots[self.slot[idx] & 3]
		if target <= 16:
			r = self.to16[idx] if self.colours > 16 else idx
		elif target <= 64:
			r = self.to64[idx]
		return self.rgb[r]


def rgb888(c):
	return (((c >> 8) & 15) * 17, ((c >> 4) & 15) * 17, (c & 15) * 17)


# -- .zsp --

def num(w):
	"""A number with an optional ! (given)."""
	bang = w.endswith("!")
	return int(w.rstrip("!"), 0), bang


def parse_zsp(text):
	lines = text.split("\n")
	pal = Palette(2)
	present = set()
	sprites = []
	cur = None
	have_colours = False
	for ln in lines:
		if cur is not None and cur["rows"] < cur["h"]:
			body = ln.lstrip("\t")
			need = cur["w"] * (2 if pal.colours > 16 else 1)
			if len(body) > need:
				body = body.lstrip(" ")
			px = []
			if pal.colours > 16:
				for x in range(cur["w"]):
					p = body[2 * x:2 * x + 2]
					px.append(None if p in ("..", "  ", "") else int(p, 16))
			else:
				for x in range(cur["w"]):
					ch = body[x] if x < len(body) else "."
					if ch in ". ":
						px.append(None)
					elif not have_colours:
						px.append(0 if ch == "0" else 1)
					else:
						px.append(int(ch, 16))
			cur["px"].append(px)
			cur["rows"] += 1
			continue
		w = ln.split()
		if not w or w[0].startswith("#"):
			continue
		if w[0] == "colours":
			pal = Palette(int(w[1]))
			have_colours = True
		elif w[0] == "slots":
			pal.slots = [int(x, 16) for x in w[1:5]]
		elif w[0] == "colour":
			i = int(w[1])
			pal.rgb[i] = int(w[2], 16)
			present.add(i)
			fields = []
			if pal.colours > 64:
				fields.append(("to64", GIVEN_TO64))
			if pal.colours > 16:
				fields.append(("to16", GIVEN_TO16))
			fields += [("slot", GIVEN_SLOT), ("ink", GIVEN_INK)]
			for (name, bit), word in zip(fields, w[3:]):
				v, bang = num(word)
				getattr(pal, name)[i] = v
				if bang:
					pal.given[i] |= bit
		elif w[0] == "sprite":
			cur = {"name": w[1], "w": int(w[2]), "h": int(w[3]), "px": [], "rows": 0}
			sprites.append(cur)
	# what the file did not give, from the RGB (as zsp_parse does)
	for i in range(256):
		g = pal.given[i]
		if i not in present:
			pal.given[i] = 0
		pal.defaults(i)
		pal.given[i] = g
	return pal, sprites


def cmd_zsp(path, out, target, sheet):
	from PIL import Image
	pal, sprites = parse_zsp(open(path).read())
	t = target or pal.colours

	def img(s):
		im = Image.new("RGBA", (s["w"], s["h"]), (0, 0, 0, 0))
		for y, row in enumerate(s["px"]):
			for x, v in enumerate(row):
				if v is not None:
					im.putpixel((x, y), rgb888(pal.shown(v, t)) + (255,))
		return im

	if sheet:
		cols = 8
		cw = max(s["w"] for s in sprites) + 2
		chh = max(s["h"] for s in sprites) + 2
		rows = (len(sprites) + cols - 1) // cols
		im = Image.new("RGBA", (cols * cw, rows * chh), (0, 0, 0, 0))
		for i, s in enumerate(sprites):
			im.paste(img(s), ((i % cols) * cw + 1, (i // cols) * chh + 1))
		im.save(out)
		print("%s: %d sprites" % (out, len(sprites)))
		return
	os.makedirs(out, exist_ok=True)
	for s in sprites:
		img(s).save(os.path.join(out, s["name"] + ".png"))
	print("%s: %d sprites, %d colours%s" % (out, len(sprites), pal.colours,
		" shown as %d" % t if target else ""))


# -- .zbm --

def cmd_zbm(path, out, target):
	from PIL import Image
	d = open(path, "rb").read()
	if d[:4] != b"ZBM1":
		sys.exit("%s: not a ZBM" % path)
	w, h, flags = struct.unpack("<III", d[4:16])
	planes = flags & 15
	stride = (w + 31) // 32 * 4
	im = Image.new("RGB", (w, h))
	if planes == 0:
		for y in range(h):
			row = d[16 + y * stride:16 + (y + 1) * stride]
			for x in range(w):
				im.putpixel((x, y), (255, 255, 255) if (row[x >> 3] >> (x & 7)) & 1 else (0, 0, 0))
		im.save(out)
		print("%s: %d x %d, one bit" % (out, w, h))
		return
	colours, nd = struct.unpack("<HH", d[16:20])
	pal = Palette(colours)
	pal.slots = list(struct.unpack("<4H", d[20:28]))
	o = 28
	for i in range(colours):
		rgb, to64, to16, slot, ink, given = struct.unpack("<HBBBBB", d[o:o + 7])
		pal.rgb[i], pal.to64[i], pal.to16[i], pal.slot[i], pal.ink[i], pal.given[i] = \
			rgb & 0xfff, to64, to16, slot, ink, given
		o += 8
	o = 16 + ((12 + 8 * colours + 2 * nd + 3) & ~3)
	t = target or colours
	for y in range(h):
		base = o + y * planes * stride
		for x in range(w):
			v = 0
			for k in range(planes):
				v |= ((d[base + k * stride + (x >> 3)] >> (x & 7)) & 1) << k
			im.putpixel((x, y), rgb888(pal.shown(v if v < colours else 0, t)))
	im.save(out)
	print("%s: %d x %d, %d colours%s" % (out, w, h, colours, " shown as %d" % t if target else ""))


# -- .zic --

def zic_decode(blob):
	x = bytearray()
	i = 0
	while i < len(blob):
		c = blob[i]
		i += 1
		if c & 0x80:
			x += b"\0" * ((c & 0x7f) + 1)
		else:
			x += blob[i:i + c + 1]
			i += c + 1
	for k in range(4, len(x)):
		x[k] ^= x[k - 4]
	return bytes(x)


def cmd_zic(path, out):
	from PIL import Image
	d = open(path, "rb").read()
	if d[:4] != b"ZIC2":
		sys.exit("%s: not a ZIC2 icon file" % path)
	count, esize, slen, ilen = struct.unpack("<HHII", d[4:16])
	sp0 = 16 + count * esize
	ic0 = sp0 + slen
	os.makedirs(out, exist_ok=True)
	for i in range(count):
		e = d[16 + i * esize:16 + (i + 1) * esize]
		name = e[:16].split(b"\0")[0].decode()
		f1, f2, flags, soff, ioff, il = struct.unpack("<IIHHHH", e[16:32])
		spoken = d[sp0 + soff:sp0 + slen].split(b"\0")[0].decode() if soff < slen else ""
		raw = zic_decode(d[ic0 + ioff:ic0 + ioff + il])
		im = Image.new("RGB", (32, 32))
		for y in range(32):
			for x in range(32):
				on = (raw[y * 4 + (x >> 3)] >> (7 - (x & 7))) & 1
				im.putpixel((x, y), (255, 255, 255) if on else (0, 0, 0))
		im.save(os.path.join(out, "icon-%s.png" % name))
		line = "%-11s %s" % (name, '"%s"' % spoken if spoken else "")
		if flags & 1:
			line += " hidden"
		if f1 or f2:
			line += "   # needs features %#x %#x" % (f1, f2)
		print(line.rstrip())


# -- .zfn --

def cmd_zfn(path, out):
	d = open(path, "rb").read()
	if d[:4] != b"ZFN1":
		sys.exit("%s: not a ZFN1 font" % path)
	w, h, stride, ascent = d[4], d[5], d[6], d[7]
	count = struct.unpack("<I", d[8:12])[0]
	codes = struct.unpack("<%dH" % count, d[12:12 + 2 * count])
	o = 12 + ((2 * count + 3) & ~3)
	name = os.path.splitext(os.path.basename(path))[0]
	with open(out, "w") as f:
		f.write("STARTFONT 2.1\nFONT %s\nSIZE %d 75 75\n" % (name, h))
		f.write("FONTBOUNDINGBOX %d %d 0 %d\n" % (w, h, ascent - h))
		f.write("STARTPROPERTIES 2\nFONT_ASCENT %d\nFONT_DESCENT %d\nENDPROPERTIES\n" % (ascent, h - ascent))
		f.write("CHARS %d\n" % count)
		for i, c in enumerate(codes):
			f.write("STARTCHAR U+%04X\nENCODING %d\nSWIDTH %d 0\nDWIDTH %d 0\n" % (c, c, w * 1000 // h, w))
			f.write("BBX %d %d 0 %d\nBITMAP\n" % (w, h, ascent - h))
			for y in range(h):
				row = d[o + (i * h + y) * stride:o + (i * h + y + 1) * stride]
				f.write(row.hex().upper() + "\n")
			f.write("ENDCHAR\n")
		f.write("ENDFONT\n")
	print("%s: %d glyphs, %dx%d" % (out, count, w, h))


def main(argv):
	if len(argv) < 4:
		print(__doc__ or open(__file__).read().split("\n\n")[0], file=sys.stderr)
		sys.exit("usage: pixconv.py zic|zsp|zbm|zfn FILE OUT [--as N] [--sheet]")
	cmd, path, out = argv[1], argv[2], argv[3]
	target = 0
	if "--as" in argv:
		target = int(argv[argv.index("--as") + 1])
		if target not in (2, 4, 16, 64, 256):
			sys.exit("--as: 2, 4, 16, 64 or 256")
	if cmd == "zic":
		cmd_zic(path, out)
	elif cmd == "zsp":
		cmd_zsp(path, out, target, "--sheet" in argv)
	elif cmd == "zbm":
		cmd_zbm(path, out, target)
	elif cmd == "zfn":
		cmd_zfn(path, out)
	else:
		sys.exit("pixconv.py: zic, zsp, zbm or zfn")


if __name__ == "__main__":
	main(sys.argv)
