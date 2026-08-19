#ifndef ZEITLOS_H
#define ZEITLOS_H

#include <stdint.h>
#include <stdbool.h>
#include "zobj.h"
#include "zmsg.h"

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

#define GPU_BLIT_CTRL_START  (1u << 0)
#define GPU_BLIT_CTRL_FILL   (1u << 1)
#define GPU_BLIT_CTRL_CLIP   (1u << 2)
#define GPU_BLIT_CTRL_GLYPH  (1u << 3)

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
z_rv z_msg_wait(z_msg_t *msg, uint32_t subject, uint32_t tag);

// ticks since boot, ~732Hz (the KTIMER IRQ rate -- see
// rtl/sysctl.v's rtc_ctr). for elapsed-time measurement; not
// wall-clock/calendar time.
uint32_t z_uptime_ticks(void);

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
