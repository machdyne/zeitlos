#!/usr/bin/env python3
#
# Zeitlos OS
# Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
#
# Converts a raw framebuffer dump produced by the shell's `ss` command
# (sw/os/sh.c's screenshot(), written to ss.bin) into a viewable PNG.
#
# Format of ss.bin: WIDTH x HEIGHT, 1 bit per pixel, packed 8 pixels
# per byte, WIDTH/8 bytes per row, no header -- exactly the
# framebuffer's own in-VRAM layout (see docs/gpu_raster.md and
# docs/gpu_blitter.md for the hardware side of this). Within each
# byte, bit 0 (the LSB) is the leftmost pixel of that byte's 8-pixel
# group and bit 7 (the MSB) is the rightmost -- this matches the
# line rasterizer's own pixel-to-bit mapping in rtl/gpu/gpu_raster.v
# (`pixel_bit_pos = cur_x[4:0]`, `pixel_mask = 1 << pixel_bit_pos`).
# A set bit is a white pixel, a clear bit is black (0=black, 1=white,
# same convention the GPU registers use).
#
# The size is the screen mode's (docs/screen_modes.md). `ss` writes it
# beside the dump, as "<width>x<height>" in ss.txt; this reads it from
# there (the .bin's name with .txt), or takes --size, or else knows the
# size by the file's length: 640x480, 800x600, 1024x768, 1280x720, or
# 640x360 (1280x720 doubled). An older `ss` wrote 640x480 always.
#
# USAGE
#
#   python3 tools/ssconv.py ss.bin ss.png
#   python3 tools/ssconv.py ss.bin                # writes ss.png
#   python3 tools/ssconv.py --size 1024x768 ss.bin
#
# Requires Pillow (`pip install pillow`), same as this project's other
# image-conversion tools (sw/data/icons/gen_dock_icon_data.py).

import os
import re
import sys

# the screen modes' desktops, by their dumps' length
SIZES = [(640, 480), (800, 600), (1024, 768), (1280, 720), (640, 360)]


def parse_size(text):
	m = re.match(r"\s*(\d+)\s*[xX]\s*(\d+)", text)
	if not m:
		return None
	w, h = int(m.group(1)), int(m.group(2))
	if w <= 0 or h <= 0 or w % 8:
		return None
	return (w, h)


def find_size(in_path, length, given):
	if given:
		return given, "--size"
	txt = os.path.splitext(in_path)[0] + ".txt"
	if os.path.exists(txt):
		with open(txt) as f:
			size = parse_size(f.read())
		if size:
			return size, txt
		print(f"warning: {txt} has no <width>x<height>; going by the "
			f"file's length", file=sys.stderr)
	for w, h in SIZES:
		if w * h // 8 == length:
			return (w, h), "its length"
	return (640, 480), "the default"


def convert(in_path, out_path, given=None):

	from PIL import Image

	with open(in_path, "rb") as f:
		data = f.read()

	(width, height), where = find_size(in_path, len(data), given)
	stride = width // 8

	if len(data) != stride * height:
		print(f"warning: {in_path} is {len(data)} bytes, expected "
			f"{stride * height} ({width}x{height} 1bpp, from {where}) -- "
			f"output may look wrong if this isn't a Zeitlos screenshot",
			file=sys.stderr)

	im = Image.new("1", (width, height), 0)
	px = im.load()

	for y in range(height):
		row_off = y * stride
		if row_off + stride > len(data):
			break
		for byte_x in range(stride):
			byte = data[row_off + byte_x]
			base_x = byte_x * 8
			for bit in range(8):
				px[base_x + bit, y] = (byte >> bit) & 1

	im.save(out_path)
	print(f"wrote {out_path} ({width}x{height}, from {where})")


def main():

	args = sys.argv[1:]
	given = None
	if len(args) >= 2 and args[0] == "--size":
		given = parse_size(args[1])
		if not given:
			print("ssconv.py: --size wants <width>x<height>, width a "
				"multiple of 8", file=sys.stderr)
			sys.exit(1)
		args = args[2:]

	if len(args) < 1 or len(args) > 2:
		print("usage: ssconv.py [--size WxH] <in.bin> [out.png]", file=sys.stderr)
		sys.exit(1)

	in_path = args[0]
	out_path = args[1] if len(args) == 2 else "ss.png"

	convert(in_path, out_path, given)


if __name__ == "__main__":
	main()
