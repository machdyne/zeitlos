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

// rtl/audio.v -- the sample FIFO and DAC output stage. Same hazard and
// same rule as Z_FEATURE_RTC and Z_FEATURE_TRNG above: check THIS bit
// before reading the audio block's own MAGIC, because on a bitstream
// built before rtl/audio.v existed 0x7000_05xx is decoded by nothing
// and that read hangs the CPU rather than returning garbage.
// z_audio_present() (sw/common/zaudio.h) is that check in the safe
// order.
//
// Unlike RTC and TRNG this is PER-BOARD: audio needs pins and a DAC on
// the other end of them, so `AUDIO in rtl/boards.vh is set per board
// rather than universally, and a board without it is normal rather
// than deliberately stripped.
//
// This bit does NOT say which DAC is wired -- the register interface
// is identical for the 1-bit and PT8211 output stages. Read the audio
// block's own CONFIG register for that.
#define Z_FEATURE_AUDIO       (1u << 26)

// rtl/gpu/gpu_video.v's game mode -- a 320x240 pixel-doubled viewport
// over the same 640x480 framebuffer. Mirrors `GAME in rtl/boards.vh,
// which defines it at the universal level, so this is set on every
// board by default.
//
// Unlike Z_FEATURE_RTC/_TRNG/_AUDIO above there is NO hang hazard here
// and no ordering rule to obey: game mode has no address window of its
// own, it lives inside socctl, and every bitstream with socctl already
// decodes and acks that whole window.
//
// Prefer z_game_available() (below) over testing this bit directly.
// This bit says `GAME was defined; that helper asks socctl what the
// hardware is actually willing to do, which additionally accounts for
// a board that has `GAME but no `GPU to scan out with.
#define Z_FEATURE_GAME        (1u << 27)

// Composite video out (rtl/gpu/gpu_video.v's `GPU_COMPOSITE).
//
// Worth checking separately from Z_FEATURE_GPU_VGA/_DDMI rather than
// folding into them, because software genuinely behaves differently: a
// composite board is 320x240 and CANNOT be anything else. The desktop
// is still a 640x480 surface -- it is just that only a quarter of it
// is on screen at a time, permanently, and the viewport is how the
// rest is reached.
//
// So an app deciding whether it has room for a wide window should ask
// z_video_is_composite() below rather than assuming 640x480 is
// visible, and it should not assume turning game mode OFF will give it
// the whole screen back, because on these boards it will not.
#define Z_FEATURE_COMPOSITE   (1u << 28)

// PAL rather than NTSC. Meaningless unless Z_FEATURE_COMPOSITE is set;
// check that first. The difference software can actually see is the
// frame rate -- 50Hz rather than 60 -- which matters to anything
// pacing itself off z_game_wait_frame().
#define Z_FEATURE_COMPOSITE_PAL (1u << 29)
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
	// Audio (rtl/audio.v). Its own group for the same reason the
	// clock and the entropy source have one: it is not memory, input,
	// storage or network. It sits before LED rather than after so the
	// rows in zsoc.c stay sorted by group, which consumers rely on.
	Z_FEAT_GROUP_AUDIO,
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
#define reg_socctl_game  (*(volatile uint32_t*)0x7000020c)
#define reg_socctl_view  (*(volatile uint32_t*)0x70000210)
#define reg_socctl_frame (*(volatile uint32_t*)0x70000214)

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

// -- game mode (rtl/gpu/gpu_video.v, via socctl's GAME/VIEW/FRAME) --
//
// A 320x240 VIEWPORT over the unchanged 640x480 framebuffer,
// pixel-doubled on scanout. The display timing does not change: the
// monitor sees the same 640x480@60Hz signal either way, which is the
// entire point on a TV that will not accept anything else.
//
// Nothing about the framebuffer changes when this is switched on or
// off. Every window is still where it was, every app is still running,
// the window manager still thinks the screen is 640x480 -- because it
// is. Only the part of it the display is pointed at changes, and
// Z_GAME_VIEW_W/H worth of it is visible at a time. That is why
// switching modes destroys nothing and why the whole desktop remains
// reachable in game mode by moving the viewport around
// (Ctrl+Alt+arrow, see sw/apps/wm).
//
// For a full-screen game the same mechanism is a double buffer. The
// framebuffer holds four non-overlapping 320x240 pages; a game draws
// into one while displaying another and flips with a single write to
// the origin, adopted at a frame boundary so it cannot tear. The line
// rasterizer (rtl/gpu/gpu_raster.v) and the blitter (rtl/gpu/gpu_blit.v)
// needed no changes at all for this and can draw into any page --
// as far as they are concerned there is still exactly one 640x480
// 1bpp surface, which is all there ever was.
#define Z_GAME_VIEW_W 320
#define Z_GAME_VIEW_H 240

// The four page origins, for software that wants to think in pages.
// The HARDWARE HAS NO CONCEPT OF A PAGE -- the origin is an arbitrary
// (x,y) and these are just the four values that happen to tile the
// framebuffer without overlapping. A game is free to ignore them
// entirely and use, say, two half-height 640x240 buffers instead (two
// pages side by side), which gives double buffering plus 320px of
// horizontal scroll room in each. Nothing in the RTL cares.
#define Z_GAME_PAGE0_X 0
#define Z_GAME_PAGE0_Y 0
#define Z_GAME_PAGE1_X 320
#define Z_GAME_PAGE1_Y 0
#define Z_GAME_PAGE2_X 0
#define Z_GAME_PAGE2_Y 240
#define Z_GAME_PAGE3_X 320
#define Z_GAME_PAGE3_Y 240

#define Z_GAME_ENABLE (1u << 0)

// Toroidal wrapping. Off (the default) the viewport is CLAMPED so it
// can never hang off the edge of the framebuffer: the origin is
// limited to x <= 320, y <= 240. That is what the desktop wants --
// scrolling right to find the dock and having the screen wrap around
// to the left instead would be disorienting rather than useful.
//
// On, column 639 is followed by column 0 and row 479 by row 0. That
// turns the 640x480 surface into an infinitely scrollable world where
// only the leading edge has to be redrawn as it comes around, instead
// of a bounded playfield two screens wide. It is what a scrolling game
// wants and is almost certainly wrong for anything else.
//
// The clamp is applied in hardware at the frame boundary, not on the
// write path, so it does not depend on the order GAME and VIEW were
// written in. A consequence worth knowing: z_game_get_view() reads
// back what you WROTE, not the clamped value actually being scanned.
#define Z_GAME_WRAP   (1u << 1)

// read-only, reported by the hardware -- see z_game_available()
#define Z_GAME_AVAIL  (1u << 2)

// top half of reg_socctl_game -- KEEP IN SYNC with rtl/socctl.v's own
// GAME_SIG. "ZG".
#define Z_GAME_SIG 0x5A47u

// true only if this bitstream's socctl actually has the GAME register.
//
// Same reasoning as z_video_mode_present(), and the same trap:
// z_socctl_present() is not sufficient, because socctl shipped before
// these registers existed and on one of those bitstreams the read
// falls through socctl's default case and returns 0 -- which is
// indistinguishable from a working block reporting "game mode off".
static inline bool z_game_present(void) {
	return ((reg_socctl_game >> 16) & 0xffffu) == Z_GAME_SIG;
}

// true if this machine can actually enter game mode.
//
// This is the question to ask, in preference to testing
// Z_FEATURE_GAME. That bit says `GAME was defined in rtl/boards.vh;
// this asks the hardware, which additionally accounts for a board
// built with `GAME but no `GPU -- there being no scanout to double.
// rtl/sysctl.v ands the two before telling socctl, so the answer here
// is the one that matters.
// True on a board whose only display output is composite video.
//
// The practical consequence: the visible area is 320x240 and always
// will be. Game mode is not a mode here, it is the permanent state of
// the display -- gpu_video.v's FIXED_VIEWPORT makes the viewport
// unconditional and does not consult the game bit at all.
//
// That is a bandwidth fact rather than a choice. Drawing 640 distinct
// pixels across a 52us active line needs 12.6MHz of luma; composite
// carries about 4.2 (NTSC) or 5.5 (PAL). A "640 wide" composite
// picture is a blur of the correct average brightness.
static inline bool z_video_is_composite(void) {
	if (!z_soc_csrs_present()) return false;
	return (reg_csr_features & Z_FEATURE_COMPOSITE) != 0;
}

// 50 on a PAL composite board, 60 everywhere else. For anything that
// converts frames to seconds; z_game_wait_frame() paces itself off the
// hardware either way and needs no help.
static inline uint32_t z_video_frame_hz(void) {
	if (!z_video_is_composite()) return 60;
	return (reg_csr_features & Z_FEATURE_COMPOSITE_PAL) ? 50 : 60;
}

static inline bool z_game_available(void) {
	if (!z_game_present()) return false;
	return (reg_socctl_game & Z_GAME_AVAIL) != 0;
}

// true if the machine is in game mode RIGHT NOW.
//
// Reads back the hardware's own enable bit, not a shadow copy, so on a
// bitstream without game mode this answers false however many times
// software has tried to turn it on.
static inline bool z_game_enabled(void) {
	if (!z_game_present()) return false;
	return (reg_socctl_game & Z_GAME_ENABLE) != 0;
}

static inline bool z_game_wrap_enabled(void) {
	if (!z_game_present()) return false;
	return (reg_socctl_game & Z_GAME_WRAP) != 0;
}

// Enter or leave game mode. Returns false if this bitstream can't (an
// RTL change -- needs `make flash`, not `make dev-flash`), writing
// nothing in that case.
//
// Takes effect at the next frame boundary, at most 16.7ms away, for
// the same reason and by the same mechanism as z_video_set_mode().
//
// Leaving game mode does NOT reset the viewport origin: the origin is
// simply ignored in desktop mode and comes back as it was on the next
// entry. That is usually what a caller wants (toggle out to see the
// whole desktop, toggle back to exactly where you were), and a caller
// that wants otherwise can write the origin itself.
static inline bool z_game_set_enabled(bool on, bool wrap) {
	if (!z_game_available()) return false;
	reg_socctl_game = (on ? Z_GAME_ENABLE : 0u) | (wrap ? Z_GAME_WRAP : 0u);
	return true;
}

// Move the viewport. Coordinates are FRAMEBUFFER pixels -- the same
// coordinate space z_fb_set_pixel() and every window position already
// use, not viewport-relative ones.
//
// Range-limited by hardware to 0..639 / 0..479 on the write path, and
// clamped further to 0..320 / 0..240 at scanout when wrap is off, so
// there is no value software can write here that produces a broken
// display. Returns false only if the bitstream has no game mode.
//
// The write is adopted at a frame boundary together with the GAME
// register, as one payload -- so setting the mode and the origin in
// two consecutive calls can never be seen as one frame at the old
// origin followed by one at the new. That is what makes this usable as
// a page flip.
static inline bool z_game_set_view(uint32_t x, uint32_t y) {
	if (!z_game_available()) return false;
	reg_socctl_view = ((y & 0x3ffu) << 16) | (x & 0x3ffu);
	return true;
}

// Reads back what was WRITTEN, not the clamped value in use -- see
// Z_GAME_WRAP above. Returns 0 on a bitstream without game mode.
static inline uint32_t z_game_get_view_x(void) {
	if (!z_game_present()) return 0;
	return reg_socctl_view & 0x3ffu;
}

static inline uint32_t z_game_get_view_y(void) {
	if (!z_game_present()) return 0;
	return (reg_socctl_view >> 16) & 0x3ffu;
}

// -- frame timing --
//
// A free-running 16-bit count of frames scanned out, and the current
// vertical blanking state. This is how a game gets vsync: there is no
// vblank interrupt, deliberately -- polling a counter costs no IRQ
// line, no latency budget and no kernel involvement, and a full-screen
// game's main loop is already a loop.
//
// The counter wraps every ~18 minutes at 60Hz. Compare for inequality
// or unsigned-subtract for elapsed frames; do not test with `>`.
#define Z_GAME_VBLANK (1u << 16)

static inline uint32_t z_game_frame(void) {
	if (!z_game_present()) return 0;
	return reg_socctl_frame & 0xffffu;
}

static inline bool z_game_in_vblank(void) {
	if (!z_game_present()) return false;
	return (reg_socctl_frame & Z_GAME_VBLANK) != 0;
}

// Spin until the frame counter changes, i.e. until the frame being
// scanned when this was called has finished.
//
// The usual page-flip sequence is: draw the back page, call
// z_game_set_view() to point at it, then call this -- at which point
// the flip has been adopted and the page just drawn is the one on
// screen, so the other one is free to draw into.
//
// Returns immediately on a bitstream without game mode, rather than
// spinning forever on a counter that is hardwired to zero. A game that
// depends on the pacing should check z_game_available() at startup;
// this only guarantees it will not hang.
static inline void z_game_wait_frame(void) {
	uint32_t start;
	if (!z_game_present()) return;
	start = reg_socctl_frame & 0xffffu;
	while ((reg_socctl_frame & 0xffffu) == start)
		;
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
