#ifndef Z_LOGO_H
#define Z_LOGO_H

/*
 * Zeitlos OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Boot splash logo -- see sw/data/images/zeitlos.xbm (source) and
 * zeitlos.bin (raw bit data, extracted via xbmtobit.c in that same
 * directory; logo_data.c below embeds that same data as a compiled-in
 * C array, same pattern sw/common/zfont_data.c already uses for font
 * glyphs).
 *
 * XBM's bit convention (LSB-first, left-to-right within each byte)
 * already matches ours (z_fb_set_pixel()'s mask = 1 << (bit_index %
 * 32), zgfx.c) exactly -- no bit-reversal needed, this is a
 * byte-for-byte copy into VRAM.
 */

#include <stdint.h>
#include <stdbool.h>

#define Z_BOOT_LOGO_W 512
#define Z_BOOT_LOGO_H 384

extern const uint8_t z_boot_logo_data[Z_BOOT_LOGO_W * Z_BOOT_LOGO_H / 8];

// draws the boot splash logo directly into VRAM, centered on the
// current 640x480 screen (offset 64,48 -- both conveniently already
// word-aligned, though this copy doesn't require that). Call as early
// as possible in the kernel's own boot sequence (see kernel.c's
// main()): this writes straight to VRAM at its fixed base address
// (0x20000000), no GPU/graphics subsystem dependency at all -- the
// kernel doesn't link zgfx.c -- so it works before anything else is
// initialized. Stays on screen until something else writes over it --
// normally wm's own startup clear_screen() call, whenever the user
// eventually runs wm.
//
// invert: the source .xbm's polarity wasn't confirmed against real
// hardware at authoring time -- pass true to flip every bit if it
// displays backwards (foreground/background swapped).
void z_boot_logo_show(bool invert);

#endif
