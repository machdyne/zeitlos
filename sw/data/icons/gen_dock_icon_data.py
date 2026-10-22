#!/usr/bin/env python3
#
# Zeitlos OS
# Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
#
# Makes dock.zic -- the dock's icons, and what each app needs to run --
# from the 32x32 PNGs and apps.txt in this directory (docs/dock.md).
# dock.zic is checked in, so building the OS needs no Python/Pillow;
# only changing an icon does. The Makefile copies it into the flash
# archive as /data/wm/dock.zic (CORE_FILES).
#
# USAGE
#
#   python3 gen_dock_icon_data.py           writes dock.zic
#   python3 gen_dock_icon_data.py --check   fails if dock.zic is stale
#
# TO ADD AN ICON
#
#   1. Draw a 32x32 PNG in two colours, pure black (off) and pure white
#      (ink), anti-aliasing off: icon-<name>.png, <name> being the app's
#      file name in /apps (or in flash).
#   2. Add a line for it to apps.txt (spoken name, hardware it needs).
#   3. Run this script.
#   4. Name the app in a row of sw/data/wm.cfg if it should be in the
#      dock's main view (an app with an icon is otherwise reached
#      through @rest).
#
# Every PNG must have a line in apps.txt and every line a PNG; the
# script stops otherwise, so an icon cannot be added half-way.
#
# FORMAT (sw/apps/wm/dock_cfg.h has the reader)
#
#   0   "ZIC2"
#   4   u16 count, u16 entry size (32)
#   8   u32 bytes of spoken names, after the entries
#  12   u32 bytes of icons, after the spoken names
#  16   entries, in name order: name[16] u32 feature u32 feature2
#       u16 flags (1 = hidden) u16 spoken offset u16 icon offset
#       u16 icon length
#       spoken names, NUL-terminated ("" for none)
#       icons, each 32 rows of 4 bytes (MSB first, bit 1 = ink, white in
#       the PNG), every byte XORed with the one a row above, then
#       run-length coded: a byte c < 0x80 is c+1 literal bytes after
#       it, c >= 0x80 is (c & 0x7f)+1 zero bytes. Icons are mostly
#       empty and mostly vertical lines; this takes them to about 60%,
#       and wm keeps only the ones its dock shows, compressed, and
#       expands one when it draws it (docs/dock.md, "Memory").
#
# DEPENDENCIES: Pillow.

import os
import re
import struct
import sys

try:
	from PIL import Image
except ImportError:
	print("error: this script needs Pillow (pip install Pillow)", file=sys.stderr)
	sys.exit(1)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
APPS_TXT = os.path.join(HERE, "apps.txt")
OUT = os.path.join(HERE, "dock.zic")
ZSOC_H = os.path.join(ROOT, "sw", "common", "zsoc.h")

ICON = 32
NAME_MAX = 16
SPOKEN_MAX = 24
ENTRY = NAME_MAX + 16
HIDDEN = 1
# The biggest file wm reads (DK_ZIC_MAX, dock_cfg.h): well past what is
# needed, it only bounds a damaged file.
ZIC_MAX = 65536
# What wm's table holds (DK_ZIC_ENTRIES, DK_ZIC_NAMES): a file past
# either is refused whole.
ENTRIES_MAX = 64
NAMES_MAX = 384


def fail(msg):
	print("error: " + msg, file=sys.stderr)
	sys.exit(1)


def features():
	"""Z_FEATURE_X / Z_FEATURE2_X from zsoc.h: {"x": (word, bit)}."""
	out = {}
	pat = re.compile(r"#define\s+Z_FEATURE(2?)_(\w+)\s+\(1u\s*<<\s*(\d+)\)")
	with open(ZSOC_H) as f:
		for line in f:
			m = pat.match(line.strip())
			if m:
				key = m.group(2).lower()
				if key not in out:
					out[key] = (2 if m.group(1) else 1, 1 << int(m.group(3)))
	return out


def read_apps():
	feats = features()
	apps = []
	with open(APPS_TXT) as f:
		for n, raw in enumerate(f, 1):
			line = raw.split("#", 1)[0].strip()
			if not line:
				continue
			m = re.match(r'(\S+)\s*(?:"([^"]*)")?\s*(.*)$', line)
			name, spoken, rest = m.group(1), m.group(2) or "", m.group(3).split()
			if len(name) >= NAME_MAX:
				fail("apps.txt line %d: name longer than %d" % (n, NAME_MAX - 1))
			if len(spoken) >= SPOKEN_MAX:
				fail("apps.txt line %d: spoken name longer than %d" % (n, SPOKEN_MAX - 1))
			f1 = f2 = flags = 0
			for w in rest:
				if w == "hidden":
					flags |= HIDDEN
				elif w in feats:
					word, bit = feats[w]
					if word == 1:
						f1 |= bit
					else:
						f2 |= bit
				else:
					fail("apps.txt line %d: '%s' is not hidden or a feature in zsoc.h" % (n, w))
			if any(a[0] == name for a in apps):
				fail("apps.txt line %d: %s twice" % (n, name))
			apps.append((name, spoken, f1, f2, flags))
	return apps


def png_for(name):
	return os.path.join(HERE, "icon-%s.png" % name.lstrip("@"))


def bitmap(path):
	im = Image.open(path).convert("RGB")
	if im.size != (ICON, ICON):
		fail("%s is %dx%d, need %dx%d" % (path, im.size[0], im.size[1], ICON, ICON))
	px = im.load()
	out = bytearray()
	for y in range(ICON):
		for xb in range(ICON // 8):
			b = 0
			for i in range(8):
				p = px[xb * 8 + i, y]
				if p == (0, 0, 0):
					bit = 0
				elif p == (255, 255, 255):
					bit = 1
				else:
					fail("%s pixel (%d,%d) is %s, not pure black or white -- "
					     "re-export with anti-aliasing off" % (path, xb * 8 + i, y, p))
				b = (b << 1) | bit
			out.append(b)
	return bytes(out)


def encode(raw):
	"""Row XOR, then run-length (see FORMAT)."""
	x = bytes(raw[i] ^ (raw[i - 4] if i >= 4 else 0) for i in range(len(raw)))
	out = bytearray()
	i = 0
	while i < len(x):
		if x[i] == 0:
			j = i
			while j < len(x) and x[j] == 0 and j - i < 128:
				j += 1
			out.append(0x80 | (j - i - 1))
		else:
			j = i
			while j < len(x) and x[j] != 0 and j - i < 128:
				j += 1
			out.append(j - i - 1)
			out += x[i:j]
		i = j
	return bytes(out)


def decode(blob):
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


def build():
	apps = sorted(read_apps(), key=lambda a: a[0])
	listed = set(os.path.basename(png_for(a[0])) for a in apps)
	for fn in sorted(os.listdir(HERE)):
		if fn.startswith("icon-") and fn.endswith(".png") and fn not in listed:
			fail("%s has no line in apps.txt" % fn)
	for a in apps:
		if not os.path.exists(png_for(a[0])):
			fail("apps.txt has %s, but there is no %s" % (a[0], os.path.basename(png_for(a[0]))))

	count = len(apps)
	if count > ENTRIES_MAX:
		fail("%d icons, more than wm reads (%d, DK_ZIC_ENTRIES in "
		     "sw/apps/wm/dock_cfg.h)" % (count, ENTRIES_MAX))
	names = sum(len(a[0]) + 1 for a in apps)
	if names > NAMES_MAX:
		fail("%d bytes of names, more than wm keeps (%d, DK_ZIC_NAMES in "
		     "sw/apps/wm/dock_cfg.h)" % (names, NAMES_MAX))
	spoken = bytearray()
	icons = bytearray()
	entries = bytearray()
	for name, sp, f1, f2, flags in apps:
		raw = bitmap(png_for(name))
		blob = encode(raw)
		if decode(blob) != raw:
			fail("%s: the icon does not survive its own coding" % name)
		soff = len(spoken)
		spoken += sp.encode() + b"\0"
		ioff = len(icons)
		icons += blob
		entries += name.encode().ljust(NAME_MAX, b"\0")
		entries += struct.pack("<IIHHHH", f1, f2, flags, soff, ioff, len(blob))
	if len(spoken) > 0xffff or len(icons) > 0xffff:
		fail("too many icons for 16-bit offsets")

	out = bytearray()
	out += b"ZIC2" + struct.pack("<HHII", count, ENTRY, len(spoken), len(icons))
	out += entries + spoken + icons
	size = len(out)
	if size > ZIC_MAX:
		fail("dock.zic would be %d bytes, more than wm reads (%d, DK_ZIC_MAX in "
		     "sw/apps/wm/dock_cfg.h)" % (size, ZIC_MAX))
	return bytes(out), apps


def main():
	data, apps = build()
	if "--check" in sys.argv[1:]:
		old = open(OUT, "rb").read() if os.path.exists(OUT) else b""
		if old != data:
			fail("dock.zic is out of date: run sw/data/icons/gen_dock_icon_data.py")
		print("dock.zic is current (%d icons, %d bytes)" % (len(apps), len(data)))
		return
	with open(OUT, "wb") as f:
		f.write(data)
	print("wrote %s: %d icons, %d bytes" % (OUT, len(apps), len(data)))


if __name__ == "__main__":
	main()
