#!/usr/bin/env python3
#
# Zeitlos OS
# Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
#
# Generates sw/apps/wm/dock_icons.c/.h from the 32x32 PNG source
# images in this directory -- see dock_icons.h's own header comment
# for the runtime side (draw_dock() in wm.c). Run this whenever an
# icon PNG here is added or changed; the .c/.h it produces are
# checked in (same "generated, do not hand-edit, regenerate if the
# source changes" convention sw/common/zfont_data.c and
# sw/os/logo_data.c already use for font/logo data -- see those
# files' own header comments) rather than generated at build time,
# so building the OS doesn't require Python/Pillow, only adding an
# icon does.
#
# USAGE
#
#   python3 gen_dock_icon_data.py
#
#   Regenerates dock_icons.c/.h from every icon-*.png in this
#   directory. That's the normal case -- to add a new dock icon:
#
#     1. Export a 32x32 PNG from your editor of choice (LibreSprite,
#        Aseprite, GIMP, ...) as icon-<name>.png, where <name> matches
#        the `name` field you're about to add to wm.c's dock_apps[]
#        table (see that table's own comment) -- e.g. icon-term.png
#        for the "term" entry. The <name> becomes part of the
#        generated C symbol (z_icon_<name>_data), so keep it a valid
#        C identifier fragment: letters, digits, underscores only.
#     2. Drop the PNG in this directory.
#     3. Run this script (no arguments).
#     4. Add the new entry to dock_apps[] in wm.c, including
#        `.bitmap = &z_icon_<name>_data[0]` -- see that table.
#     5. Rebuild wm.
#
#   Explicit filenames also work, if you only want to (re)generate
#   specific icons rather than everything in the directory:
#
#     python3 gen_dock_icon_data.py icon-term.png icon-gpu3d.png
#
# SOURCE IMAGE REQUIREMENTS
#
#   - Exactly 32x32 pixels (DOCK_ICON_SIZE in wm.c).
#   - Exactly two colors: pure black (0,0,0) as background/"off",
#     pure white (255,255,255) as foreground/ink/"on". No
#     anti-aliasing, no partial transparency, no gray -- every pixel
#     must resolve to exactly one of those two colors, or this script
#     stops and tells you which pixel didn't. In LibreSprite: draw in
#     indexed/2-color mode with anti-aliasing off, so this is true by
#     construction rather than something to fix up after exporting.
#   - Any other 32x32 combination of "clearly meant to be black" and
#     "clearly meant to be white" pixels will likely also work (this
#     script treats anything closer to white as "on"), but isn't the
#     tested/intended path -- the strict two-color check exists so a
#     half-exported or accidentally-anti-aliased icon fails loudly
#     here, at conversion time, rather than rendering as static on
#     the actual 1bpp display.
#
# OUTPUT FORMAT
#
#   Each icon becomes a `static const uint8_t z_icon_<name>_data[128]`
#   array: 32 rows of 4 bytes each (32 bits = 4 bytes per row, one
#   row per scanline, top to bottom), MSB-first within each byte --
#   the same "row-major, MSB-first, top bits of each byte" convention
#   sw/common/zfont.h already documents for font glyph data (this is
#   just that same convention at a width of exactly 32, so every byte
#   is fully used, no partial top-bits-of-a-byte case to think about).
#   Bit value 1 = "on"/ink/foreground (the white pixels), matching how
#   every existing wm.c draw call already uses color=1 for foreground
#   (z_fb_hw_box(..., 1, NULL), z_fb_draw_text(..., 1, ...)).
#
# DEPENDENCIES
#
#   Needs Pillow (`pip install Pillow --break-system-packages` or
#   equivalent). Nothing else -- stdlib otherwise.

import sys
import glob
import os
import re

try:
	from PIL import Image
except ImportError:
	print("error: this script needs Pillow (pip install Pillow)", file=sys.stderr)
	sys.exit(1)

ICON_SIZE = 32
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "apps", "wm")
OUT_C = os.path.join(OUT_DIR, "dock_icons.c")
OUT_H = os.path.join(OUT_DIR, "dock_icons.h")

def icon_name_from_path(path):
	base = os.path.basename(path)
	base = re.sub(r"\.png$", "", base, flags=re.IGNORECASE)
	base = re.sub(r"^icon-", "", base)
	name = re.sub(r"[^A-Za-z0-9_]", "_", base)
	if not name or not (name[0].isalpha() or name[0] == "_"):
		print(f"error: '{path}' doesn't produce a usable C identifier "
			f"(got '{name}') -- rename the file (icon-<name>.png, "
			f"<name> = letters/digits/underscores)", file=sys.stderr)
		sys.exit(1)
	return name

def convert_one(path):

	name = icon_name_from_path(path)
	im = Image.open(path).convert("RGB")

	if im.size != (ICON_SIZE, ICON_SIZE):
		print(f"error: '{path}' is {im.size[0]}x{im.size[1]}, "
			f"need exactly {ICON_SIZE}x{ICON_SIZE}", file=sys.stderr)
		sys.exit(1)

	px = im.load()
	rows = []

	for y in range(ICON_SIZE):
		row_bits = []
		for x in range(ICON_SIZE):
			r, g, b = px[x, y]
			if (r, g, b) == (0, 0, 0):
				row_bits.append(0)
			elif (r, g, b) == (255, 255, 255):
				row_bits.append(1)
			else:
				print(f"error: '{path}' pixel ({x},{y}) is "
					f"{(r, g, b)}, not pure black or pure white -- "
					f"re-export with anti-aliasing off (see this "
					f"script's own header comment)", file=sys.stderr)
				sys.exit(1)
		row_bytes = []
		for byte_i in range(ICON_SIZE // 8):
			b = 0
			for bit_i in range(8):
				b = (b << 1) | row_bits[byte_i * 8 + bit_i]
			row_bytes.append(b)
		rows.append(row_bytes)

	return name, rows

def main():

	args = sys.argv[1:]
	if args:
		paths = args
	else:
		paths = sorted(glob.glob(os.path.join(
			os.path.dirname(os.path.abspath(__file__)), "*.png")))

	if not paths:
		print("error: no PNGs given and none found alongside this script",
			file=sys.stderr)
		sys.exit(1)

	icons = [convert_one(p) for p in paths]

	with open(OUT_C, "w") as f:
		f.write("/* Generated by sw/data/icons/gen_dock_icon_data.py -- do not hand-edit. */\n")
		f.write("/* Regenerate if a source PNG in that directory changes or is added. */\n\n")
		f.write("#include <stdint.h>\n")
		f.write("#include \"dock_icons.h\"\n\n")
		for name, rows in icons:
			f.write(f"const uint8_t z_icon_{name}_data[{ICON_SIZE} * {ICON_SIZE // 8}] = {{\n")
			for row_bytes in rows:
				f.write("\t" + ", ".join(f"0x{b:02x}" for b in row_bytes) + ",\n")
			f.write("};\n\n")

	with open(OUT_H, "w") as f:
		f.write("/* Generated by sw/data/icons/gen_dock_icon_data.py -- do not hand-edit. */\n")
		f.write("/* Regenerate if a source PNG in that directory changes or is added. */\n\n")
		f.write("#ifndef Z_DOCK_ICONS_H\n#define Z_DOCK_ICONS_H\n\n")
		f.write("#include <stdint.h>\n\n")
		f.write("/*\n")
		f.write(" * 32x32 1bpp dock icon bitmaps -- see gen_dock_icon_data.py (this\n")
		f.write(" * file's generator, sw/data/icons/) for the source PNGs and exact\n")
		f.write(" * format. Each array is 32 rows of 4 bytes, MSB-first, bit 1 = ink.\n")
		f.write(" * Referenced from wm.c's dock_apps[] table -- see that table's own\n")
		f.write(" * comment for how to wire a new icon in after regenerating this file.\n")
		f.write(" */\n\n")
		for name, _ in icons:
			f.write(f"extern const uint8_t z_icon_{name}_data[{ICON_SIZE} * {ICON_SIZE // 8}];\n")
		f.write("\n#endif\n")

	print(f"wrote {OUT_C} and {OUT_H} ({len(icons)} icon(s): "
		f"{', '.join(n for n, _ in icons)})")

if __name__ == "__main__":
	main()
