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

// -- system clock --
//
// Fixed at 48MHz across every board. Not a per-board value and not
// discoverable at runtime: 48 was chosen over an otherwise-rounder 50
// specifically because it divides cleanly for a 1Mbaud UART, and
// nothing in the current lineup has a reason to differ.
//
// NOTHING ON THIS SOC CAN MEASURE ITS OWN CLOCK. rtl/sysctl.v's
// rtc_ctr (which generates the KTIMER interrupt) is clocked from
// sys_clk, the same clock rdcycle counts -- so cycles-per-tick is
// always exactly 65536 by construction, whatever the real frequency
// is. Any "measured MHz" derived from those two is a tautology that
// reports this constant back. The UART baud divisor is derived from
// sys_clk too, so there is no independent time reference anywhere on
// chip. If the PLL is ever misconfigured, the symptom is everything
// running proportionally fast or slow with nothing reporting it --
// which is exactly why this lives here as a stated assumption rather
// than pretending to be a measurement.
#define Z_SYSCLK_HZ 48000000u

// KTIMER rate: rtc_ctr is a free-running 16-bit counter on sys_clk and
// fires the interrupt on wrap, so this is exactly SYSCLK / 65536 --
// 732.42Hz at 48MHz. The integer 732 used throughout the tree is that
// truncated, a 0.06% error, which is immaterial for the timeouts and
// delays it's used for.
#define Z_TICK_HZ (Z_SYSCLK_HZ / 65536u)

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

// CPU extensions. Unlike everything above, these describe the core
// rather than a peripheral -- see z_soc_check_cpu_arch() below.
#define Z_FEATURE_CPU_MUL     (1u << 20)
#define Z_FEATURE_CPU_DIV     (1u << 21)
#define Z_FEATURE_CPU_MUL_FAST (1u << 22)

// Which core, not what it can do. Set for rtl/cpu/zeitlos32, clear for
// picorv32 -- and clear also on any bitstream built before this bit
// existed, which is correct, since those are all picorv32. See
// z_soc_cpu_name().
#define Z_FEATURE_CPU_ZEITLOS32 (1u << 23)

// rtl/rtc.v -- the wall clock (seconds since the Unix epoch, plus a
// 1/1024s fraction). Mirrors `RTC in rtl/boards.vh, which defines it
// at the universal level, so this is set on every board by default.
// Clear means either a board that deliberately turned it off, or a
// bitstream built before the RTC existed at all -- and software wants
// the same thing in both cases, which is to not use a clock that
// isn't there.
//
// Checking this bit is not a nicety, and it must come FIRST. The
// RTC's own registers are at 0x7000_03xx, and a pre-RTC `ICACHE
// bitstream decodes nothing there -- an undecoded address gets no ack
// on this bus and the CPU waits for it forever. So reading the RTC's
// magic as a first probe can hang; reading THIS bit, at 0x7000_0008,
// cannot, because every bitstream ever built decodes it.
// z_rtc_available() (sw/common/zrtc.h) is that check in the safe
// order and is what apps should call.
#define Z_FEATURE_RTC         (1u << 24)

// rtl/trng.v -- the ring-oscillator entropy source. Same hazard and
// same rule as Z_FEATURE_RTC directly above: check THIS bit before
// reading the TRNG's own magic, because on a bitstream built before
// rtl/trng.v existed that address is decoded by nothing and the read
// hangs the CPU rather than returning garbage. z_rng_present()
// (sw/common/zrng.h) is that check in the safe order.
//
// Note this bit says the hardware is BUILT, not that it WORKS. A ring
// oscillator bank that synthesis optimised away sets this bit and
// produces predictable words; z_rng_secure() is the question worth
// asking before generating a key. See docs/trng.md.
#define Z_FEATURE_TRNG        (1u << 25)

// -- feature table (sw/common/zsoc.c) --
//
// The human-readable half of the Z_FEATURE_* bits above, kept in the
// same directory so that everything which has to track rtl/sysctl.v's
// CSR_FEATURES lives in one place. See zsoc.c's own header comment.
//
// Data only -- no printing. A consumer that wants to display these
// (k_soc_report(), sw/os/kernel.c) owns its own formatting; a shared
// file pulling in printf() would be unusable from contexts without
// stdio. Link sw/common/zsoc.c to use these; a translation unit that
// only wants the inline helpers below needs no extra object.
typedef enum {
	// CPU first: it describes the core itself rather than a
	// peripheral, and it is the one line worth seeing before
	// anything else if a build is about to die on illegal
	// instructions (see z_soc_check_cpu_arch()).
	Z_FEAT_GROUP_CPU = 0,
	Z_FEAT_GROUP_MEMORY,
	Z_FEAT_GROUP_GPU,
	Z_FEAT_GROUP_INPUT,
	Z_FEAT_GROUP_STORAGE,
	Z_FEAT_GROUP_NETWORK,
	// The wall clock (rtl/rtc.v). Its own group rather than folded
	// into an existing one because it fits none of them -- it is not
	// memory, not input, not storage. A group with one member reads
	// oddly in a list until the alternative is tried, which is
	// filing a clock under "input".
	Z_FEAT_GROUP_CLOCK,
	// The entropy source (rtl/trng.v). Its own group for the same
	// reason the clock has one: it is not memory, input, storage or
	// network, and filing it under any of those would be worse than a
	// short list.
	Z_FEAT_GROUP_ENTROPY,
	Z_FEAT_GROUP_LED,
	// Adding a group here REQUIRES adding its display name to
	// z_soc_feature_groups[] in zsoc.c, at the same position. That
	// table is indexed by this enum and nothing links the two but
	// order; getting it wrong shifts every later name and runs the
	// last group off the end of the array, which crashed the kernel
	// mid-boot the first time it happened. zsoc.c now carries a
	// compile-time size check so the mistake cannot reach a board
	// again.
	Z_FEAT_GROUP_COUNT
} z_feat_group_t;

typedef struct {
	uint32_t	bit;	// one of the Z_FEATURE_* values above
	const char	*name;	// short display name, e.g. "usb-hid"
	uint8_t		group;	// a z_feat_group_t
} z_feature_info_t;

// Sorted by `group` -- see zsoc.c on why that matters to consumers.
extern const z_feature_info_t z_soc_features[];
extern const int z_soc_features_count;

// Indexed by z_feat_group_t, padded to a common width for column
// output.
extern const char *const z_soc_feature_groups[];

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

// Name of the CPU core in the running bitstream. Returns "unknown" if
// this build predates rtl/csrs.v, because on such a bitstream the bit
// is not merely clear, it is unreadable -- and "picorv32" would be a
// guess dressed up as a fact.
//
// The two cores are drop-in compatible, so the same kernel binary runs
// on either and nothing else in a running system distinguishes them.
static inline const char *z_soc_cpu_name(void) {
	if (!z_soc_csrs_present()) return "unknown";
	return (reg_csr_features & Z_FEATURE_CPU_ZEITLOS32) ? "zeitlos32"
	                                                    : "picorv32";
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

// -- SOC control (rtl/socctl.v) --
//
// Writable global configuration, at 0x7000_02xx. The third tenant of
// nibble 0x7 alongside the read-only CSRs (0x7000_00xx) and the
// instruction cache (0x7000_01xx).
//
// Kept separate from the CSRs deliberately: that block is documented
// as read-only and side-effect-free, and its value is precisely that
// inertness. Configuration software SETS lives here instead.
#define reg_socctl_ctrl  (*(volatile uint32_t*)0x70000200)
#define reg_socctl_magic (*(volatile uint32_t*)0x70000204)
#define reg_socctl_video (*(volatile uint32_t*)0x70000208)

#define Z_SOCCTL_MAGIC 0x5A435452u	// "ZCTR" -- see rtl/socctl.v

// Mouse cursor shape: 0 = normal pointer (X), 1 = busy pointer (Z).
// The sprite is drawn in hardware (rtl/gpu/gpu_cursor.v) and
// composited at scanout, so this is the only way to change it --
// software cannot draw over it.
#define Z_SOCCTL_CURSOR_BUSY (1u << 0)

// true only if this bitstream actually has rtl/socctl.v. Same magic
// check, and the same reason for it, as z_soc_csrs_present().
static inline bool z_socctl_present(void) {
	return reg_socctl_magic == Z_SOCCTL_MAGIC;
}

// Show the busy (Z) cursor, or the normal (X) one.
//
// Safe to call on a bitstream without socctl -- the write goes to an
// address csrs.v decodes and ignores, so it is acked and discarded
// rather than hanging the bus. (An address NOTHING decodes gets no ack
// at all and stalls the CPU forever; see rtl/cache.v's own note.)
static inline void z_cursor_set_busy(bool busy) {
	reg_socctl_ctrl = busy ? Z_SOCCTL_CURSOR_BUSY : 0;
}

// -- virtual phosphor modes (rtl/gpu/gpu_video.v) --
//
// The display's colour scheme, in a register instead of an `ifdef.
// GPU_AMBER/GPU_GREEN used to be synthesis-time choices baked into
// gpu_video.v; they now select only the POWER-ON DEFAULT (rtl/sysctl.v
// derives socctl's reset value from them), and this changes it live.
//
// "Paper" is black-on-white. It is not a fourth colour but the white
// mode with the pixel sense inverted, which is why it is exactly as
// legible as the white one rather than approximately so.
//
// A change takes effect at the next frame boundary rather than
// immediately -- gpu_video.v adopts the new value only when its
// counters wrap, so no frame is ever drawn half in one mode and half
// in another. Worst case that is 16.7ms.
#define Z_VIDEO_MODE_WHITE 0u	// white on black (default)
#define Z_VIDEO_MODE_AMBER 1u	// amber on black
#define Z_VIDEO_MODE_GREEN 2u	// green on black
#define Z_VIDEO_MODE_PAPER 3u	// black on white
#define Z_VIDEO_MODE_COUNT 4u

// top half of reg_socctl_video -- KEEP IN SYNC with rtl/socctl.v's
// own VIDEO_SIG. "VC", video colour.
#define Z_VIDEO_SIG 0x5643u

// true only if this bitstream's socctl actually has the VIDEO
// register.
//
// z_socctl_present() is NOT sufficient here and checking it instead
// would be a real bug: socctl shipped before this register existed, so
// a bitstream can answer the ZCTR magic correctly and still have
// nothing at 0x7000_0208. On one of those, the read falls through
// socctl's own default case and returns 0 -- which is bit-for-bit
// identical to a working block reporting Z_VIDEO_MODE_WHITE. Hence a
// second signature in the upper half, same approach as
// z_icache_present().
static inline bool z_video_mode_present(void) {
	return ((reg_socctl_video >> 16) & 0xffffu) == Z_VIDEO_SIG;
}

// Current mode, or Z_VIDEO_MODE_WHITE if this bitstream predates the
// register -- white is what such a board is actually displaying, so
// the fallback is the truth rather than a placeholder.
static inline uint32_t z_video_get_mode(void) {
	if (!z_video_mode_present()) return Z_VIDEO_MODE_WHITE;
	return reg_socctl_video & 0x3u;
}

// Set the mode. Returns false if this bitstream can't do it (an RTL
// change -- needs `make flash`, not `make dev-flash`) or if `mode` is
// out of range; in neither case is anything written.
static inline bool z_video_set_mode(uint32_t mode) {
	if (mode >= Z_VIDEO_MODE_COUNT) return false;
	if (!z_video_mode_present()) return false;
	reg_socctl_video = mode;
	return true;
}

// Display name for a mode, for anything that prints one. Always
// returns a valid string, "unknown" for a value out of range, so a
// caller can print the result without checking first.
static inline const char *z_video_mode_name(uint32_t mode) {
	switch (mode) {
		case Z_VIDEO_MODE_WHITE: return "white";
		case Z_VIDEO_MODE_AMBER: return "amber";
		case Z_VIDEO_MODE_GREEN: return "green";
		case Z_VIDEO_MODE_PAPER: return "paper";
		default: return "unknown";
	}
}

// Parse a mode name, case-sensitive, as typed by a person (sh.c's
// `color` command, the Scheme API's (video-mode ...)). Returns
// Z_VIDEO_MODE_COUNT for anything unrecognised -- deliberately an
// out-of-range value rather than defaulting to white, so a typo is
// reported instead of silently resetting the display.
//
// Written out longhand rather than with strcmp() so this header stays
// usable from anywhere without pulling in <string.h>; it is four
// short words.
static inline uint32_t z_video_mode_from_name(const char *s) {
	if (!s) return Z_VIDEO_MODE_COUNT;
	if (s[0] == 'w' && s[1] == 'h' && s[2] == 'i' && s[3] == 't' &&
		s[4] == 'e' && s[5] == '\0') return Z_VIDEO_MODE_WHITE;
	if (s[0] == 'a' && s[1] == 'm' && s[2] == 'b' && s[3] == 'e' &&
		s[4] == 'r' && s[5] == '\0') return Z_VIDEO_MODE_AMBER;
	if (s[0] == 'g' && s[1] == 'r' && s[2] == 'e' && s[3] == 'e' &&
		s[4] == 'n' && s[5] == '\0') return Z_VIDEO_MODE_GREEN;
	if (s[0] == 'p' && s[1] == 'a' && s[2] == 'p' && s[3] == 'e' &&
		s[4] == 'r' && s[5] == '\0') return Z_VIDEO_MODE_PAPER;
	return Z_VIDEO_MODE_COUNT;
}

// -- instruction cache (rtl/cache.v) --
//
// Registers live at 0x7000_01xx, sharing nibble 0x7 with the CSRs
// above (0x7000_00xx) and selected by address bit 8 -- see
// rtl/sysctl.v's cs_cache. They are deliberately NOT part of the CSR
// block itself: that block is documented as read-only and
// side-effect-free, and a flush register is neither.
#define reg_icache_ctrl   (*(volatile uint32_t*)0x70000100)
#define reg_icache_hits   (*(volatile uint32_t*)0x70000104)
#define reg_icache_misses (*(volatile uint32_t*)0x70000108)
#define reg_icache_info   (*(volatile uint32_t*)0x7000010c)

#define Z_ICACHE_CTRL_ENABLE  (1u << 0)
#define Z_ICACHE_CTRL_FLUSH   (1u << 1)

// top half of reg_icache_info -- KEEP IN SYNC with rtl/cache.v
#define Z_ICACHE_MAGIC 0x1CACu

// true only if this bitstream was actually built with `ICACHE.
//
// Same reasoning as z_soc_csrs_present() above: an unmapped read
// doesn't fault on this bus, it returns whatever rtl/sysctl.v's data
// mux resolves to, so a magic constant is the only reliable way to
// tell "not built in" from "built in and reporting zero".
static inline bool z_icache_present(void) {
	return ((reg_icache_info >> 16) & 0xffffu) == Z_ICACHE_MAGIC;
}

// cache geometry, meaningless unless z_icache_present()
static inline uint32_t z_icache_kb(void) {
	return reg_icache_info & 0xffu;
}

static inline uint32_t z_icache_line_words(void) {
	return (reg_icache_info >> 8) & 0xffu;
}

// Invalidate every cache line.
//
// MUST be called after writing code into main memory and BEFORE
// jumping to it. Only two places in this codebase do that:
// fs_load_exec() (sw/os/fs/fs.c) and load_zeitlos() (sw/bios/bios.c).
//
// The failure this prevents is worth stating plainly, because it is
// intermittent and allocation-order dependent rather than
// reproducible: app A loads at base X, exits, k_mem_free() releases
// X, then app B loads at that same base. The cache still holds A's
// instructions for those physical addresses, so B executes A's code.
// Nothing about B is wrong; it just runs somebody else's program.
//
// Safe to call unconditionally, but ONLY because rtl/sysctl.v
// guarantees this address is decoded on every build: without `ICACHE,
// csrs_wb keeps the whole 0x7 nibble and acks it. Do not assume the
// general "unmapped access is harmless" rule applies here -- it does
// not. An address nothing decodes gets NO ACK on this bus and
// picorv32_wb waits for that ack forever, which is a dead hang, not a
// read of undefined data. An earlier version of this comment claimed
// otherwise and hung the BIOS on every non-ICACHE build.
//
// Costs NUM_LINES cycles (~11us at 48MHz for 512 lines) while the
// cache walks its lines, which is nothing against the SD card read
// that precedes it.
static inline void z_icache_flush(void) {
	reg_icache_ctrl = Z_ICACHE_CTRL_ENABLE | Z_ICACHE_CTRL_FLUSH;
}

// Turn the cache off/on at runtime. This exists so that "is the cache
// causing this?" can be answered on hardware with a single register
// write instead of a re-synthesis. Disabling forces every fetch to go
// to main memory, exactly as a bitstream built without `ICACHE would.
static inline void z_icache_enable(bool on) {
	reg_icache_ctrl = on ? Z_ICACHE_CTRL_ENABLE : 0;
}

// -- rv32im gateware/software agreement check --
//
// The asymmetry that makes this worth a function:
//
//   rv32i  software on rv32im gateware -- fine, M just goes unused.
//   rv32im software on rv32i  gateware -- FATAL, and not gracefully.
//
// In the second case every mul/div is an illegal instruction. picorv32
// is built here with CATCH_ILLINSN and ENABLE_IRQ (rtl/sysctl.v), so
// that raises IRQ 1 -- which sw/os/kernel.c has no handler for (it
// knows about KTIMER/UART/HID only), so the handler returns, the same
// instruction executes again, and the machine spins. No message, no
// trap output, just a hang somewhere unrelated-looking.
//
// The software half is known at build time, the hardware half from the
// CSR feature bits. Comparing them turns that hang into one clear line
// at boot.
//
// Z_ARCH_HAS_MUL/Z_ARCH_HAS_DIV come from sw/common/arch.mk, which
// derives them from ARCH itself. That is deliberate: GCC's
// __riscv_mul/__riscv_div are used only as a fallback for code not
// built through those Makefiles. Depending on the compiler alone would
// mean that on a toolchain that spells them differently, this check
// quietly evaluates to "nothing to verify" and the safety net vanishes
// without any indication -- and this tree targets a deliberately old
// toolchain (picorv32 pins riscv-gnu-toolchain rev 411d134, 2018).
//
// __riscv_zmmul is checked too. -march=rv32i_zmmul does NOT define
// __riscv_mul, so a check looking only for that would decide a
// zmmul binary has no multiply while its text section is full of them.
#if defined(Z_ARCH_HAS_MUL)
#  define Z_BUILD_MUL Z_ARCH_HAS_MUL
#elif defined(__riscv_mul) || defined(__riscv_muldiv) || defined(__riscv_zmmul)
#  define Z_BUILD_MUL 1
#else
#  define Z_BUILD_MUL 0
#endif

#if defined(Z_ARCH_HAS_DIV)
#  define Z_BUILD_DIV Z_ARCH_HAS_DIV
#elif defined(__riscv_div) || defined(__riscv_muldiv)
#  define Z_BUILD_DIV 1
#else
#  define Z_BUILD_DIV 0
#endif

// Returns true if this binary can safely run on this bitstream.
// Deliberately contains no multiply or divide itself, so it is safe to
// call before knowing the answer.
static inline bool z_soc_check_cpu_arch(void) {
#if Z_BUILD_MUL
	if (!z_soc_has_feature(Z_FEATURE_CPU_MUL)) return false;
#endif
#if Z_BUILD_DIV
	if (!z_soc_has_feature(Z_FEATURE_CPU_DIV)) return false;
#endif
	return true;
}

// What this binary was compiled for, for printing alongside the above.
static inline const char *z_soc_build_arch(void) {
#if Z_BUILD_MUL && Z_BUILD_DIV
	return "rv32im";
#elif Z_BUILD_MUL
	return "rv32i_zmmul";
#else
	return "rv32i";
#endif
}

#endif
