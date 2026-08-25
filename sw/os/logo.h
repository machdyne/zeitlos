#ifndef Z_LOGO_H
#define Z_LOGO_H

/*
 * Zeitlos OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Boot splash logo.
 *
 * The flashed artifact is a full 640x480 1bpp framebuffer image --
 * sw/data/images/zeitlos_fb.bin, produced from the 512x384 zeitlos.bin
 * by sw/data/images/pad_logo.py. Centring and padding happen ONCE, at
 * build time, so displaying it is a single flat memcpy of the whole
 * framebuffer rather than a row-by-row copy with per-row offset
 * arithmetic. It also means the splash clears whatever was in VRAM at
 * reset, instead of leaving a border of garbage around the logo.
 *
 * Polarity is baked in the same way: pad_logo.py --invert flips every
 * bit at build time, so if the splash ever shows up with foreground and
 * background swapped, regenerate the image rather than changing any C.
 *
 * -- Why this lives in FLASH, not in kernel.bin --
 *
 * It used to be a compiled-in const array (sw/os/logo_data.c, now
 * unused). That put 24KB of .rodata into kernel.bin for something shown
 * once at boot -- and since k_proc_create() sizes a process's memory
 * block from its image, those 24KB cost 24KB of the 1MB main-memory
 * budget for the entire uptime of the system.
 *
 * Flash is memory-mapped on this SOC -- sw/bios/bios.c's load_zeitlos()
 * memcpy()s the kernel straight out of it -- so the logo needs no RAM
 * at all: it is read from flash and written to VRAM with nothing in
 * between. Note the flashed image is larger than the old array (38400
 * bytes vs 24576) because it is now full-screen; flash is the cheap
 * resource here, main memory is the scarce one.
 *
 * -- Flash layout --
 *
 * The gateware occupies the start of flash and the kernel sits at
 * offset 1MB (ROM_OS_ADDR, bios.c). The logo goes immediately below the
 * kernel at offset 0xF0000, which leaves the gateware headroom up to
 * 960KB (it is ~400KB today, and varies by board) and fits the 38400
 * byte image comfortably below the 1MB kernel offset.
 *
 * KEEP Z_BOOT_LOGO_FLASH_OFFSET BELOW IN SYNC with ROM_LOGO_ADDR
 * (sw/bios/bios.c) and LOGO_FLASH_OFFSET_HEX/_DEC (top-level Makefile).
 * Three copies of one constant, with no build-time link between them,
 * because all three live in separately-built artifacts: the BIOS is
 * baked into the bitstream's BRAM, the kernel is a flashed binary, and
 * the Makefile drives an external flashing tool.
 */

#include <stdint.h>

#define Z_BOOT_LOGO_W 640
#define Z_BOOT_LOGO_H 480
#define Z_BOOT_LOGO_BYTES (Z_BOOT_LOGO_W * Z_BOOT_LOGO_H / 8)	// 38400

// Base of the memory-mapped SPI flash window -- the same MEM_ROM
// sw/bios/bios.c defines and reads the kernel image out of.
#define Z_BOOT_LOGO_ROM_BASE 0x10000000
#define Z_BOOT_LOGO_FLASH_OFFSET 0x000F0000
#define Z_BOOT_LOGO_ADDR (Z_BOOT_LOGO_ROM_BASE + Z_BOOT_LOGO_FLASH_OFFSET)

// Copies the splash from flash into VRAM. Can run before anything else
// in the kernel's boot sequence (see kernel.c's main()): VRAM is plain
// memory-mapped hardware needing no init, and this touches no other
// subsystem. Stays on screen until something overwrites it -- normally
// wm's own startup clear_screen().
//
// The BIOS already draws the same image before loading the kernel, so
// this is visually a no-op on a current bitstream. Kept because it
// repairs the image if the kernel load disturbed VRAM, and because a
// board running an older bitstream has a BIOS that predates the splash.
//
// No `invert` argument any more -- polarity is baked into the flashed
// image by pad_logo.py, see this file's header comment.
void z_boot_logo_show(void);

#endif
