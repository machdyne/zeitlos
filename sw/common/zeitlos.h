#ifndef ZEITLOS_H
#define ZEITLOS_H

#include <stdint.h>
#include <stdbool.h>
#include "zobj.h"
#include "zmsg.h"
#include "zproc.h"	// z_proc_info_t / z_mem_stats_args_t, used by the
						// z_proc_list()/z_mem_stats() declarations below

typedef uint32_t *(*z_kernel_ptr_t)(uint32_t, uint32_t *, uint32_t);

#define reg_kernel (*(volatile uint32_t*)0x0000000c)

#define reg_uart0_data (*(volatile uint8_t*)0xf0000000)
#define reg_uart0_dlbl (*(volatile uint8_t*)0xf0000000)
#define reg_uart0_dlbh (*(volatile uint8_t*)0xf0000004)
#define reg_uart0_ier (*(volatile uint8_t*)0xf0000004)
#define reg_uart0_fcr (*(volatile uint8_t*)0xf0000008)
#define reg_uart0_iir (*(volatile uint8_t*)0xf0000008)
#define reg_uart0_lcr (*(volatile uint8_t*)0xf000000c)
#define reg_uart0_mcr (*(volatile uint8_t*)0xf0000010)
#define reg_uart0_lsr (*(volatile uint8_t*)0xf0000014)
#define reg_uart0_msr (*(volatile uint8_t*)0xf0000018)

#define reg_led (*(volatile uint32_t*)0xe0000000)
#define reg_leds (*(volatile uint32_t*)0xe0000004)

// Two independent USB HID ports (rtl/usb_hid.v's usb_hid_wb, two
// instances in rtl/sysctl.v) -- Obst and Lakritz both break out two
// USB host ports (boards/*.lpf's usb_host_dp[1:0]/usb_host_dm[1:0]).
// Both instances share the same 256MB top-nibble slot, discriminated
// by address bit 5 -- see sysctl.v's cs_usb0/cs_usb1 comment for why
// it's bit 5 and not bit 4 (usb_hid_wb's own internal register select
// uses wb_adr_i[2:0], and wb_adr_i is a word-shifted address, so bit
// 4 of the byte address collides with that). Which physical device
// (keyboard/mouse/gamepad) ends up on which port isn't fixed by
// hardware at all: each port's own `info` register's typ field (bits
// 25:24 -- 0=none, 1=keyboard, 2=mouse, 3=gamepad) says what's
// currently plugged into THAT port, and software decides which to
// treat as "the" keyboard/mouse from that -- see sw/os/hid.c and
// sw/apps/wm/wm.c, which both poll/read both ports and pick
// dynamically rather than assuming a fixed port-to-device mapping.
//
// reg_usb0_* are new names for the exact same registers/addresses as
// the pre-existing reg_usb_* below (kept as-is, unchanged, since
// sw/bios/bios.c and sw/apps/gpu3d/gpu3d.c both have their own private
// copies of the old names and don't need updating) -- use reg_usb0_*
// in any new code that also needs to talk about reg_usb1_*, purely so
// the two ports read symmetrically side by side.
#define reg_usb0_info   (*(volatile uint32_t*)0xc0000000)
#define reg_usb0_keys   (*(volatile uint32_t*)0xc0000004)
#define reg_usb0_mouse  (*(volatile uint32_t*)0xc0000008)
#define reg_usb0_cursor (*(volatile uint32_t*)0xc000000c)

#define reg_usb1_info   (*(volatile uint32_t*)0xc0000020)
#define reg_usb1_keys   (*(volatile uint32_t*)0xc0000024)
#define reg_usb1_mouse  (*(volatile uint32_t*)0xc0000028)
#define reg_usb1_cursor (*(volatile uint32_t*)0xc000002c)

// pre-existing names, unchanged -- always port 0. kept for every
// existing caller (sw/bios/bios.c, sw/apps/gpu3d/gpu3d.c have their
// own private copies of these same four lines and don't go through
// this header at all, but anything that *does* include this header
// and only ever cared about a single port can keep using these).
#define reg_usb_info (*(volatile uint32_t*)0xc0000000)
#define reg_usb_keys (*(volatile uint32_t*)0xc0000004)
#define reg_usb_mouse (*(volatile uint32_t*)0xc0000008)
#define reg_usb_cursor (*(volatile uint32_t*)0xc000000c)

// -- hardware SPI master for the sdcard (rtl/spisd.v) --
//
// Replaces the old bit-bang GPIO register. SCLK is generated in
// gateware, so the transfer rate no longer depends on compiler
// codegen, ISA or cache behaviour -- see rtl/spisd.v's header for why
// that mattered.
#define reg_spisd_data   (*(volatile uint32_t*)0xb0000000)
#define reg_spisd_status (*(volatile uint32_t*)0xb0000004)
#define reg_spisd_ctrl   (*(volatile uint32_t*)0xb0000008)
#define reg_spisd_magic  (*(volatile uint32_t*)0xb000000c)

#define Z_SPISD_MAGIC     0x53504930u   // "SPI0"
#define Z_SPISD_BUSY      (1u << 0)     // STATUS: transfer in progress
#define Z_SPISD_CTRL_CS   (1u << 0)     // CTRL: 1 = assert (pin low)
#define Z_SPISD_CTRL_DIV(d) (((d) & 0xffu) << 8)

// SCLK = 48MHz / (2 * (DIV + 1)).
//   59 -> 400kHz, mandatory until the card leaves idle state
//    1 -> 12MHz
//    0 -> 24MHz
#define Z_SPISD_DIV_INIT  59
#define Z_SPISD_DIV_FAST  1

// kept for compatibility with anything still poking the old register
#define reg_sdcard (*(volatile uint32_t*)0xb0000000)

#define gpu_x0 (*(volatile uint32_t*)0xa0000000)
#define gpu_y0 (*(volatile uint32_t*)0xa0000004)
#define gpu_x1 (*(volatile uint32_t*)0xa0000008)
#define gpu_y1 (*(volatile uint32_t*)0xa000000c)
#define gpu_color (*(volatile uint32_t*)0xa0000010)
#define gpu_start (*(volatile uint32_t*)0xa0000014)
#define gpu_busy (*(volatile uint32_t*)0xa0000018)
#define gpu_pixel_count (*(volatile uint32_t*)0xa000001c)
#define gpu_debug_cur_x (*(volatile uint32_t*)0xa0000020)
#define gpu_debug_cur_y (*(volatile uint32_t*)0xa0000024)  
#define gpu_debug_fifo_count (*(volatile uint32_t*)0xa0000028)

// GPU blitter (rtl/gpu/gpu_blit.v) -- word-indexed registers, matching
// wb_adr_i[3:0] in the RTL. See docs/window_manager.md, "hardware
// glyph blitting" for the fill/copy vs glyph-blit protocol.
#define gpu_blit_ctrl        (*(volatile uint32_t*)0xd0000000)
#define gpu_blit_status      (*(volatile uint32_t*)0xd0000004)
#define gpu_blit_dst_x       (*(volatile uint32_t*)0xd0000008)
#define gpu_blit_dst_y       (*(volatile uint32_t*)0xd000000c)
#define gpu_blit_width       (*(volatile uint32_t*)0xd0000010)
#define gpu_blit_height      (*(volatile uint32_t*)0xd0000014)
#define gpu_blit_pattern     (*(volatile uint32_t*)0xd0000018)
#define gpu_blit_glyph_addr  (*(volatile uint32_t*)0xd000001c)
#define gpu_blit_glyph_w     (*(volatile uint32_t*)0xd0000020)
#define gpu_blit_glyph_h     (*(volatile uint32_t*)0xd0000024)
#define gpu_blit_fg_color    (*(volatile uint32_t*)0xd0000028)
#define gpu_blit_bg_color    (*(volatile uint32_t*)0xd000002c)

// -- memory copy source (rtl/gpu/gpu_blit.v, CTRL_SRCMEM) --
//
// gpu_blit_src_addr is a PHYSICAL byte address, word aligned. The
// blitter is its own bus master and does not go through the MTU
// (rtl/mtu.v), so an app's virtual 0x8000_xxxx pointer is meaningless
// to it -- see z_mtu_phys() below and z_fb_hw_blit_mem() in zgfx.h.
//
// gpu_blit_src_shift packs two things: bits [4:0] are the bit offset,
// within the word at src_addr, of the pixel that lands on bit 0 of the
// first destination word; bit [8] says to start the shifter's window
// with zeros instead of reading that word at all. Software never sets
// these by hand -- z_fb_hw_blit_mem() derives both.
#define gpu_blit_src_addr    (*(volatile uint32_t*)0xd0000030)
#define gpu_blit_src_stride  (*(volatile uint32_t*)0xd0000034)
#define gpu_blit_src_shift   (*(volatile uint32_t*)0xd0000038)

#define GPU_BLIT_CTRL_START  (1u << 0)
#define GPU_BLIT_CTRL_FILL   (1u << 1)
#define GPU_BLIT_CTRL_CLIP   (1u << 2)
#define GPU_BLIT_CTRL_GLYPH  (1u << 3)
#define GPU_BLIT_CTRL_SRCMEM (1u << 4)

#define GPU_BLIT_SRC_PRIME_ZERO (1u << 8)

// MTU translation base (rtl/mtu.v). Readable from an app: only
// 0x8xxx_xxxx is translated, so a load from here reaches the MTU
// itself rather than being remapped. Reads as 0 in a context with no
// translation active (the kernel's own), in which case virtual and
// physical addresses are already the same thing.
#define reg_mtu_base         (*(volatile uint32_t*)0x90000000)

// glyph memory (rtl/mem/glyph.v) -- byte-addressable, 4096 bytes.
// software writes font data here (see z_gfx_hw_font_load() in
// zgfx.c); the blitter reads it back via its own direct port, never
// through this address.
#define GLYPH_MEM_BASE       0x30000000
#define GLYPH_MEM_SIZE       4096

// ETH SPI bit-bang interface (rtl/spibb_eth.v) for the ENC28J60
// Ethernet controller (PMOD). Same bit layout as reg_sdcard: bit0 =
// MISO (read), bit1 = MOSI (write), bit2 = SCK (write), bit3 = SS
// (write), bit4 = the chip's INT line (read, active low -- not used
// as a real interrupt yet, just readable). See sw/apps/net/enc28j60.c
// for the driver.
#define reg_eth (*(volatile uint32_t*)0x50000000)

// RMII Ethernet MAC (rtl/ethmac_rmii.v), mozart_ml1 only. Alternative
// to reg_eth (SPI ENC28J60) for boards with an RMII PHY instead. See
// rtl/ethmac_rmii.v's header comment for the full register map and
// the reasoning behind it (CDC, the single-buffer-not-double-buffer
// choice, etc.) -- this is just the C-side view of the same thing.
//
// RX poll loop:
//   if (reg_ethmac_status & REG_ETHMAC_RX_READY) {
//       uint16_t len = reg_ethmac_rxlen;
//       // read len bytes from reg_ethmac_rxbuf (word-at-a-time,
//       // len is NOT rounded up -- handle the last partial word
//       // like enc28j60_recv() does)
//       reg_ethmac_rxctrl = 1;  // release the buffer -- any value works
//   }
//
// TX: write the frame into reg_ethmac_txbuf (word-at-a-time, no FCS
// -- hardware appends it), then reg_ethmac_txlen = len, then
// reg_ethmac_txctrl = 1 (any value) to start sending. Poll
// REG_ETHMAC_TX_BUSY and don't touch TX_BUF/TX_LEN/TX_CTRL again
// until it clears.
#define reg_ethmac_status (*(volatile uint32_t*)0x60000000)
#define reg_ethmac_rxlen  (*(volatile uint32_t*)0x60000004)
#define reg_ethmac_rxctrl (*(volatile uint32_t*)0x60000008)
#define reg_ethmac_txlen  (*(volatile uint32_t*)0x6000000c)
#define reg_ethmac_txctrl (*(volatile uint32_t*)0x60000010)
#define reg_ethmac_rxbuf  ((volatile uint32_t*)0x60000100)
#define reg_ethmac_txbuf  ((volatile uint32_t*)0x60000a00)
#define REG_ETHMAC_RXBUF_WORDS 512  // 2048 bytes
#define REG_ETHMAC_TXBUF_WORDS 512  // 2048 bytes

#define REG_ETHMAC_CRS_DV      (1u << 0)
#define REG_ETHMAC_REFCLK_HB   (1u << 1)
#define REG_ETHMAC_RX_READY    (1u << 2)
#define REG_ETHMAC_TX_BUSY     (1u << 3)
#define REG_ETHMAC_RX_DROP_SHIFT  4  // 4-bit saturating count, buffer-full drops
#define REG_ETHMAC_RX_DROP_MASK   0xf
#define REG_ETHMAC_RX_ERR_SHIFT   8  // 4-bit saturating count, bad CRC / too short
#define REG_ETHMAC_RX_ERR_MASK    0xf
#define gpu_clip_x0     (*(volatile uint32_t*)0xa000002c)  // Left bound
#define gpu_clip_y0     (*(volatile uint32_t*)0xa0000030)  // Top bound  
#define gpu_clip_x1     (*(volatile uint32_t*)0xa0000034)  // Right bound
#define gpu_clip_y1     (*(volatile uint32_t*)0xa0000038)  // Bottom bound
#define gpu_clip_enable (*(volatile uint32_t*)0xa000003c)  // Enable clipping

#define reg_mtu (*(volatile uint32_t*)0x90000000)

// PicoRV32's maskirq custom instruction -- a raw CPU instruction, not
// privilege-gated, no syscall needed, safe for any app to use
// directly. Returns the *previous* mask (so callers can restore it
// afterward) and sets the new one. Same definition as sw/os/kernel.h's
// kernel-side copy (that one's kept separate since the kernel doesn't
// otherwise depend on zeitlos.h). Originally lived only in
// sw/apps/net/enc28j60.c (to protect SPI bit-bang transactions from
// interrupt preemption -- a timer/UART IRQ firing mid-transaction
// stretches a clock pulse by however long the interrupt takes to
// service, a real SPI timing violation); moved here once zgfx.c's
// hardware line rasterizer support needed the same pattern, for the
// same underlying reason: a handful of MMIO register writes plus a
// trigger need to complete as one atomic unit, since the rasterizer's
// registers (rtl/gpu/gpu_raster.v) are global, shared peripheral
// state with no per-process isolation -- see zgfx.c's z_fb_hw_line().
static inline uint32_t maskirq(uint32_t new_mask) {
	uint32_t old_mask;
	__asm__ volatile (
		".insn r 0x0B, 0x6, 0x03, %0, %1, zero"
		: "=r"(old_mask)
		: "r"(new_mask)
		: "memory"
	);
	return old_mask;
}

// --

int getch(void);
void readline(char *buf, int maxlen);
void echo(void);
void noecho(void);

// pops the next queued raw USB HID keyboard event, or -1 if none is
// pending -- see sw/os/hid.c and sw/common/zkbd.h. Currently only
// used by wm.c (see docs/window_manager.md), which owns turning raw
// input into per-app messages the same way it already does for the
// mouse.
int32_t hid_read_key(void);

// --

// send a pre-built message
z_rv z_msg_send(z_msg_t *msg);

// build and send a message in one call
z_rv z_msg_new_send(uint32_t to, uint32_t subject, uint32_t tag, z_obj_t obj);

// pop the next available message, if any (non-blocking)
z_rv z_msg_read(z_msg_t *msg);

// block until a message matching subject/tag arrives, discarding
// anything else that shows up in the meantime
// Give up the CPU until a message arrives or `timeout_ticks` elapse
// (0 = indefinitely). Returns immediately if a message is already
// queued. This is what makes an idle app cost the scheduler nothing --
// see Z_PROC_FLAG_BLOCKED in sw/os/kernel.h.
uint32_t z_exec_exists(const char *name);

void z_proc_wait(uint32_t timeout_ticks);

z_rv z_msg_wait(z_msg_t *msg, uint32_t subject, uint32_t tag);

// ticks since boot, ~732Hz (the KTIMER IRQ rate -- see
// rtl/sysctl.v's rtc_ctr). for elapsed-time measurement; not
// wall-clock/calendar time.
uint32_t z_uptime_ticks(void);

// busy-waits for at least `ms` milliseconds, built on z_uptime_ticks()
// above -- see delay_ms()'s own comment (zeitlos.c) for why this
// exists (replacing ad-hoc, uncalibrated `for(volatile int i=0;...)`
// spin loops with something expressed in real time) and what "busy"
// means here (this process keeps its own CPU slice the whole time;
// there's no sleep/yield primitive to hand it back). Not a substitute
// for z_msg_wait()/z_msg_wait_timeout() if what you're actually doing
// is waiting on a message/event that might take a while or arrive at
// an unpredictable time -- this is for short, fixed delays only.
void delay_ms(uint32_t ms);

// -- PID name registry (sw/os/pidreg.c/h) --
//
// Registers `basename` for the calling process; the kernel appends a
// number unique among currently-registered instances of that base
// name (even the first one -- "term" becomes "term0", not bare
// "term") and writes the result into `out`, up to `outlen` bytes
// including the NUL. Returns true on success. A process may call this
// more than once to register several names for itself (e.g. multiple
// ports) -- each call is independent.
bool z_pid_register(const char *basename, char *out, uint32_t outlen);

// Resolves a full name (as returned by z_pid_register(), e.g.
// "term3") to its owning pid. Returns true and writes *pid on
// success, false if nothing currently active matches. Cache the
// result if sending more than one message to the same name -- this
// walks a small fixed-size table in the kernel on every call, cheap
// but not free. A cached pid can go stale if its owner later exits;
// z_msg_send() already fails safely against a dead/reused pid (same
// as it always has for hardcoded pids like Z_PID_WM), so this isn't a
// new failure mode to handle, just the existing one reached a
// different way.
bool z_pid_lookup(const char *name, uint32_t *pid);

// the calling process's own pid. Mainly useful for the same reason
// wm.c needs it: to tell "is this thing mine?" apart from "is this
// thing owned by whatever pid I happen to have been started as" --
// see z_getpid()'s comment in sw/os/kernel.c.
uint32_t z_getpid(void);

// launches a new process from a named file on the FAT filesystem
// (e.g. z_proc_run("term")) -- see zeitlos.c for the full writeup.
// Returns the new pid, or 0 on failure.
uint32_t z_proc_run(const char *name);

// kills another process outright, by pid -- see k_proc_kill_syscall()
// in sw/os/kernel.c for the full writeup. No ownership check: any
// process can kill any other, same trust model as the rest of this
// kernel. Added for sw/apps/wm's Z_WIN_FLAG_CLOSE_KILLS_OWNER
// (sw/common/zwm.h) -- wm calling this on a window's owner_pid when
// its titlebar close icon is clicked with that flag set.
//
// Returns Z_OK/Z_FAIL (it returned void until the Scheme API's (kill
// ...) needed a real answer -- see zeitlos.c). Existing callers that
// ignore the value are unaffected and need no change.
z_rv z_proc_kill(uint32_t pid);

// snapshot of the live process table, and of the kernel memory pool --
// the data behind sh.c's `ps` and `free`, returned rather than printed.
// See sw/common/zproc.h for the structs and the full reasoning, and
// zeitlos.c for these two wrappers' own contracts.
//
// Declared here rather than in zproc.h itself for the same reason
// z_proc_run()/z_proc_kill() above are: this file is the established
// home for app-facing syscall wrappers, while zproc.h stays a pure
// shared-wire-format header includable from both sides (exactly the
// split sw/common/zfs.h and sw/os/fsapi.h already use).
uint32_t z_proc_list(z_proc_info_t *out, uint32_t max, uint32_t *truncated);
bool z_mem_stats(z_mem_stats_args_t *out);

// Optional redirect for stdout (fd 1 -- printf(), putchar(), anything
// through the FILE* stdout). NULL by default: every byte goes to the
// UART via _write(), exactly as it always has, and an app that never
// touches this sees no change whatsoever.
//
// Exists because a process whose real user is at the far end of a
// zport.h connection (sw/apps/repl, whose Scheme runs for someone
// sitting in a `term` window) has its output land in the wrong place
// entirely. `ms`'s printing procedures -- `display`, `write`, `print`,
// `newline`, `gc`, and `dump` -- all write straight to stdout, so
// running (dump) from `term` printed ~200 symbol names onto the serial
// console and showed the user nothing at all.
//
// A hook here rather than patching those call sites: ms.c has 50+
// scattered printf()/fputs()/putchar() calls with no output
// abstraction of its own, so redirecting them individually would mean
// a large diff against a submodule this project deliberately keeps
// close to upstream (see docs/scheme.md). Catching it at the one place
// every one of them already funnels through costs a single branch in
// _write() and covers procedures nobody has written yet.
//
// `data` is NOT NUL-terminated and is only valid for the duration of
// the call. The hook receives raw bytes with NO LF->CRLF expansion --
// _write()'s own expansion is skipped when a hook is installed, since
// only the hook knows what its transport wants. A hook must not itself
// write to stdout: see repl.c's own implementation for the
// buffer-then-flush structure that avoids re-entering this path.
typedef void (*z_stdout_hook_t)(const char *data, uint32_t len);
extern z_stdout_hook_t z_stdout_hook;

// NOTE: app-facing filesystem access (fs_size()/fs_mallocfile()/
// fs_write_file(), backed by the new Z_SYS_FS_SIZE/_READ/_WRITE
// syscalls) is DELIBERATELY NOT declared here, even though this file
// is otherwise the natural home for an app-facing syscall wrapper --
// this header is also pulled into KERNEL-side code (sw/os/kernel.h
// includes it), which separately includes sw/os/fs/fs.h, the
// kernel-native FatFs wrappers -- and fs.h already declares
// fs_size()/fs_mallocfile()/fs_write_file() itself, with slightly
// different signatures (uint32_t vs int, etc.) for the kernel's own
// direct-FatFs-call versions. Declaring the app-facing versions HERE
// under the same names would collide at kernel-compile time. See
// sw/common/zfsapp.h instead -- same names, app-only header, never
// pulled into kernel.c.
#define VT100_CURSOR_UP       "\e[A"
#define VT100_CURSOR_DOWN     "\e[B"
#define VT100_CURSOR_RIGHT    "\e[C"
#define VT100_CURSOR_LEFT     "\e[D"
#define VT100_CURSOR_HOME     "\e[;H"
#define VT100_CURSOR_MOVE_TO  "\e[%i;%iH"
#define VT100_CURSOR_CRLF     "\e[E"
#define VT100_CLEAR_HOME      "\e[;H"
#define VT100_ERASE_SCREEN    "\e[J"
#define VT100_ERASE_LINE      "\e[K"

// SGR. `term`'s own VT100 emulator implements exactly one attribute --
// reverse video (sw/common/zvt100.c's 'm' case, which handles 0/7/27
// and ignores everything else) -- because that's the one a monochrome
// framebuffer can actually represent; see zvt100.h's own header
// comment. These two names exist so callers reach for the supported
// attribute rather than hand-rolling an escape that will be silently
// dropped. Added for sw/apps/repl's `page` status line.
#define VT100_REVERSE         "\e[7m"
#define VT100_ATTR_RESET      "\e[0m"

#define CH_ESC	0x1b
#define CH_LF	0x0a
#define CH_CR	0x0d
#define CH_FF	0x0c
#define CH_BS	0x08
#define CH_DEL	0x7f

// --

#define Z_MKSYSCALL(name, fn) Z_SYS_##name,
typedef enum {
	Z_SYSCALL_NONE = 0,
	#include "syscalls.def"
	Z_SYSCALL_COUNT
} z_syscall_id_t;
#undef Z_MKSYSCALL

#endif
