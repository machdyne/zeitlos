#ifndef ZSOC_H
#define ZSOC_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * SOC capability CSRs (rtl/csrs.v) -- lets software ask, at runtime,
 * how much main RAM this bitstream was actually built with and which
 * optional peripherals it was actually synthesized with, instead of
 * assuming. See docs/csrs.md for the full design.
 *
 * Motivating problem: `sw/apps/net` needs to know whether THIS
 * SOC actually has the ethernet backend (SPI_ETH or ETH_RMII,
 * rtl/boards.vh) it was built for before touching any of that
 * backend's registers -- e.g. Lakritz has neither, and net.c's own
 * startup used to hang forever trying to talk to hardware that just
 * isn't there (see sw/os/sh.c's `init` -- historically it only
 * reserved net's pid rather than starting it, specifically because of
 * this). Reading an address nothing decodes doesn't fault on this
 * wishbone bus (rtl/sysctl.v's own `32'hzzzz_zzzz` default case), so
 * there was previously no reliable way to tell "not present" from
 * "present, reading whatever it happens to read".
 *
 * Header-only, no separate .c -- these are plain MMIO reads at a
 * fixed physical address, same as any other reg_* register already
 * used directly throughout this codebase (reg_sdcard, reg_eth, ...).
 * No kernel/syscall indirection needed, unlike z_msg_send()/
 * z_win_create() and friends, which genuinely need the kernel's
 * involvement for cross-process coordination -- a CSR read doesn't.
 * Safe to `#include from kernel-compiled code too (sw/os/kernel.c) --
 * unlike zeitlos.c, this has no symbols that could collide with
 * kruntime.c's own (see docs/networking.md's "sh.c: tget/tput shell
 * commands" section for that specific, unrelated collision story).
 *
 * ALWAYS check z_soc_csrs_present() (or compare reg_csr_magic against
 * Z_CSR_MAGIC yourself) before trusting reg_csr_mem_mb/
 * reg_csr_features directly -- an older bitstream built before
 * rtl/csrs.v existed at all has nothing mapped at 0x7000_0000, and
 * (see above) an unmapped read doesn't fault, it just reads back
 * whatever rtl/sysctl.v's data-mux's default case resolves to -- so a
 * naive read here would return some indeterminate value, not a clean
 * zero or a trapped error. z_soc_mem_mb()/z_soc_has_feature() already
 * do this check internally; only reach for the raw reg_csr_*
 * registers directly if you need something these helpers don't cover.
 */

#include <stdint.h>
#include <stdbool.h>

// register map -- word-addressed at the hardware level, see
// rtl/csrs.v's own header comment for the authoritative description.
#define reg_csr_magic    (*(volatile uint32_t*)0x70000000)
#define reg_csr_mem_mb   (*(volatile uint32_t*)0x70000004)
#define reg_csr_features (*(volatile uint32_t*)0x70000008)

#define Z_CSR_MAGIC 0x5A454954u	// "ZEIT" -- see rtl/csrs.v

// -- feature bits -- KEEP IN SYNC with rtl/sysctl.v's CSR_FEATURES
// localparam. Bit position is the only thing that has to match
// between the two sides; there's no single shared source for both
// Verilog and C here (same split as rtl/usb_hid.v/this file's own
// sibling zkbd.h for HID-usage translation) -- edit both together.
#define Z_FEATURE_MEM_SRAM    (1u << 0)
#define Z_FEATURE_MEM_SDRAM   (1u << 1)
#define Z_FEATURE_MEM_VRAM    (1u << 2)
#define Z_FEATURE_MEM_QQSPI   (1u << 3)
#define Z_FEATURE_MEM_ROM     (1u << 4)
#define Z_FEATURE_MEM_GLYPH   (1u << 5)
#define Z_FEATURE_GPU          (1u << 6)
#define Z_FEATURE_GPU_RASTER  (1u << 7)
#define Z_FEATURE_GPU_BLIT    (1u << 8)
#define Z_FEATURE_GPU_CURSOR  (1u << 9)
#define Z_FEATURE_GPU_VGA     (1u << 10)
#define Z_FEATURE_GPU_DDMI    (1u << 11)
#define Z_FEATURE_UART0       (1u << 12)
#define Z_FEATURE_USB_HID     (1u << 13)
#define Z_FEATURE_SPI_SDCARD  (1u << 14)
#define Z_FEATURE_SPI_ETH     (1u << 15)
#define Z_FEATURE_SPI_FLASH   (1u << 16)
#define Z_FEATURE_ETH_RMII    (1u << 17)
#define Z_FEATURE_LED_RGB     (1u << 18)
#define Z_FEATURE_LED_DEBUG   (1u << 19)

// true only if rtl/csrs.v is actually present in the running
// bitstream -- see this file's own header comment for why every
// other function/register here is meaningless until this is true.
static inline bool z_soc_csrs_present(void) {
	return reg_csr_magic == Z_CSR_MAGIC;
}

// total main RAM, in megabytes, this bitstream was built with
// (rtl/boards.vh's `MEM). Returns 0 if z_soc_csrs_present() is
// false -- don't treat 0 as "no RAM", treat it as "unknown, this
// bitstream predates CSRs" and fall back to whatever default made
// sense before this existed (see sw/bios/bios.c's own
// MEM_MAIN_SIZE_DEFAULT for the pattern).
static inline uint32_t z_soc_mem_mb(void) {
	if (!z_soc_csrs_present()) return 0;
	return reg_csr_mem_mb;
}

// true only if CSRs are present AND `feature` (one of the
// Z_FEATURE_* bits above) was actually synthesized into this
// bitstream. Always false if z_soc_csrs_present() is false -- an
// older build that predates CSRs entirely can't answer this, so the
// safe default is "can't confirm present" rather than guessing yes.
//
// NOTE: this is NOT simply the negation of
// z_soc_feature_confirmed_absent() below -- both can be false at
// once, when CSRs aren't present at all and the honest answer is
// "unknown" rather than either yes or no. See that function's own
// comment for when you actually want that distinction (usually: only
// when you're deciding whether it's SAFE to touch hardware you're
// not sure exists).
static inline bool z_soc_has_feature(uint32_t feature) {
	if (!z_soc_csrs_present()) return false;
	return (reg_csr_features & feature) != 0;
}

// true only if CSRs are present AND `feature` is confirmed ABSENT --
// i.e. real, positive evidence the hardware genuinely isn't there,
// not just "we don't know" (an older bitstream predating rtl/csrs.v
// makes this false too, same as a confirmed-present feature would --
// see z_soc_has_feature()'s own comment on why "unknown" isn't the
// same as either yes or no).
//
// This is the one to use when deciding whether it's safe to skip/
// refuse touching some piece of hardware -- e.g. sw/apps/net deciding
// whether to even call phy_init(). Simply negating z_soc_has_feature()
// would ALSO (wrongly) refuse on an older bitstream that predates
// CSRs entirely, where the honest answer is "we don't know, so don't
// change behavior" -- not "confirmed absent, refuse". Getting this
// backwards is exactly the kind of thing that would silently break
// every board that hasn't been rebuilt with rtl/csrs.v yet.
static inline bool z_soc_feature_confirmed_absent(uint32_t feature) {
	if (!z_soc_csrs_present()) return false;	// unknown, not absent
	return (reg_csr_features & feature) == 0;
}

#endif
