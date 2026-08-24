#!/usr/bin/env python3
#
# Zeitlos OS
# Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
#
# Converts a raw framebuffer dump produced by the shell's `ss` command
# (sw/os/sh.c's screenshot(), written to ss.bin) into a viewable PNG.
#
# Format of ss.bin: FB_WIDTH x FB_HEIGHT, 1 bit per pixel, packed 8
# pixels per byte, FB_WIDTH/8 bytes per row, no header -- exactly the
# framebuffer's own in-VRAM layout (see docs/gpu_raster.md and
# docs/gpu_blitter.md for the hardware side of this). Within each
# byte, bit 0 (the LSB) is the leftmost pixel of that byte's 8-pixel
# group and bit 7 (the MSB) is the rightmost -- this matches the
# line rasterizer's own pixel-to-bit mapping in rtl/gpu/gpu_raster.v
# (`pixel_bit_pos = cur_x[4:0]`, `pixel_mask = 1 << pixel_bit_pos`).
# A set bit is a white pixel, a clear bit is black (0=black, 1=white,
# same convention the GPU registers use).
#
# USAGE
#
#   python3 tools/ssconv.py ss.bin ss.png
#   python3 tools/ssconv.py ss.bin                # writes ss.png
#
# Requires Pillow (`pip install pillow`), same as this project's other
# image-conversion tools (sw/data/icons/gen_dock_icon_data.py).

import sys

FB_WIDTH = 640
FB_HEIGHT = 480
FB_STRIDE = FB_WIDTH // 8  # bytes per row
FB_SIZE = FB_STRIDE * FB_HEIGHT


def convert(in_path, out_path):

	from PIL import Image

	with open(in_path, "rb") as f:
		data = f.read()

	if len(data) != FB_SIZE:
		print(f"warning: {in_path} is {len(data)} bytes, expected "
			f"{FB_SIZE} ({FB_WIDTH}x{FB_HEIGHT} 1bpp) -- output may "
			f"look wrong if this isn't a Zeitlos screenshot", file=sys.stderr)

	im = Image.new("1", (FB_WIDTH, FB_HEIGHT), 0)
	px = im.load()

	for y in range(FB_HEIGHT):
		row_off = y * FB_STRIDE
		if row_off + FB_STRIDE > len(data):
			break
		for byte_x in range(FB_STRIDE):
			byte = data[row_off + byte_x]
			base_x = byte_x * 8
			for bit in range(8):
				x = base_x + bit
				if x >= FB_WIDTH:
					break
				px[x, y] = (byte >> bit) & 1

	im.save(out_path)
	print(f"wrote {out_path} ({FB_WIDTH}x{FB_HEIGHT})")


def main():

	if len(sys.argv) < 2 or len(sys.argv) > 3:
		print("usage: ssconv.py <in.bin> [out.png]", file=sys.stderr)
		sys.exit(1)

	in_path = sys.argv[1]
	out_path = sys.argv[2] if len(sys.argv) == 3 else "ss.png"

	convert(in_path, out_path)


if __name__ == "__main__":
	main()
