/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB host controller register map.
 *
 * The software half of the contract documented in docs/usb_host.md.
 * KEEP IN SYNC with rtl/usb/usb_host.v -- bit position is the only
 * thing that has to match and nothing checks that it does, the same
 * hand-maintained split as rtl/csrs.vh and sw/common/zsoc.h.
 *
 * Check Z_FEATURE2_USB_HOST before touching any of this. On a board
 * built without the block every address below reads zero, which is
 * survivable, but a driver that enumerates into the void is harder to
 * diagnose than one that says the hardware is not there.
 */

#ifndef Z_USBH_HW_H
#define Z_USBH_HW_H

#include <stdint.h>

#define Z_USBH_BASE         0xc0000000

// -- register access --
//
// Everything below is an ADDRESS, and every access goes through these
// four accessors rather than dereferencing a pointer at the call site.
//
// In an ordinary build these are the pointer dereference and generate
// exactly the code the macros used to: same instruction, same volatile
// semantics, no call. The indirection exists so rtl/tb/cosim can build
// this same driver for the host and route each access through a real
// Wishbone transaction against the RTL -- which is the only way the
// software and the gateware get tested against each other before a
// board exists. See docs/usb_host.md.
#ifdef Z_USBH_COSIM
uint32_t z_usbh_rd(uint32_t a);
void z_usbh_wr(uint32_t a, uint32_t v);
uint8_t z_usbh_rb(uint32_t a);
void z_usbh_wb(uint32_t a, uint8_t v);
#else
static inline uint32_t z_usbh_rd(uint32_t a)
{
    return *(volatile uint32_t *)a;
}
static inline void z_usbh_wr(uint32_t a, uint32_t v)
{
    *(volatile uint32_t *)a = v;
}
static inline uint8_t z_usbh_rb(uint32_t a)
{
    return *(volatile uint8_t *)a;
}
static inline void z_usbh_wb(uint32_t a, uint8_t v)
{
    *(volatile uint8_t *)a = v;
}
#endif

// -- HID compatibility blocks, unchanged from usb_hid_host --
//
// sw/os/hid.c, sw/apps/wm/wm.c, sw/apps/gpu3d/gpu3d.c and
// sw/bios/bios.c use these and need no change. See docs/user_input.md
// for the layout and its two traps (typ at [25:24] of info; the cursor
// word truncated from the top).
//
// ONE THING IS NEW: the info register is now WRITABLE, and writing it
// is how the kernel assigns the device type. Nothing infers typ any
// more. Only bits [25:24] are stored; everything else is ignored.
#define Z_USBH_HID0_INFO    0xc0000000u
#define Z_USBH_HID0_KEYS    0xc0000004u
#define Z_USBH_HID0_MOUSE   0xc0000008u
#define Z_USBH_HID0_CURSOR  0xc000000cu
#define Z_USBH_HID0_PAD     0xc0000010u
#define Z_USBH_HID1_INFO    0xc0000020u
#define Z_USBH_HID1_KEYS    0xc0000024u
#define Z_USBH_HID1_MOUSE   0xc0000028u
#define Z_USBH_HID1_CURSOR  0xc000002cu
#define Z_USBH_HID1_PAD     0xc0000030u

#define Z_USBH_TYP_NONE     0
#define Z_USBH_TYP_KBD      1
#define Z_USBH_TYP_MOUSE    2
#define Z_USBH_TYP_PAD      3

#define Z_USBH_TYP_SET(t)   (((uint32_t)(t) & 3u) << 24)

// -- host control --

#define Z_USBH_CTRL         0xc0000100u
#define Z_USBH_PORTSTAT     0xc0000104u
#define Z_USBH_IRQSTAT      0xc0000108u
#define Z_USBH_IRQEN        0xc000010cu
#define Z_USBH_XACT_A       0xc0000110u
#define Z_USBH_XACT_B       0xc0000114u
#define Z_USBH_XACT_S       0xc0000118u
#define Z_USBH_CONFIG       0xc000011cu

// Bring-up counters, not part of the programming model. DEBUG0 is
// {rx_started[31:16], tx_packets[15:0]}, DEBUG1 is
// {rx_bad[15:8], rx_good[7:0]}.
//
// The split that matters is tx versus rx_started: a host that is
// transmitting into a device that never answers and a host that is
// not transmitting at all both look like a timeout from software.
#define Z_USBH_DEBUG0       0xc0000120u
#define Z_USBH_DEBUG1       0xc0000124u


#define Z_USBH_CTRL_SRST        (1u << 0)
#define Z_USBH_CTRL_FRAME_EN    (1u << 1)
#define Z_USBH_CTRL_POLL_EN     (1u << 2)
#define Z_USBH_CTRL_PORT_EN(p)  (1u << (8 + (p) * 8))
#define Z_USBH_CTRL_PORT_RST(p) (1u << (9 + (p) * 8))
#define Z_USBH_CTRL_PORT_SUS(p) (1u << (10 + (p) * 8))

// PORTSTAT, eight bits per port at [p*8 +: 8].
#define Z_USBH_PS(r, p)         (((r) >> ((p) * 8)) & 0xff)
#define Z_USBH_PS_CONNECTED     (1u << 0)
#define Z_USBH_PS_ENABLED       (1u << 1)
#define Z_USBH_PS_LOWSPEED      (1u << 2)
#define Z_USBH_PS_RESETTING     (1u << 3)
#define Z_USBH_PS_CHANGE        (1u << 4)
// Always zero. These ports are D+/D-, 22R and a 15k pull-down with no
// sense hardware behind them; the field exists so a board that gains a
// load switch does not move everything else. See docs/usb_host.md.
#define Z_USBH_PS_OVERCURRENT   (1u << 5)
// Raw, synchronised line state. Diagnostic only -- the protocol never
// looks at these. D+ high alone is full-speed idle (J); D- high alone
// is low-speed idle (K); both low is SE0, meaning nothing attached or
// a reset in progress; both high is illegal and means a wiring or
// threshold problem rather than a protocol one.
#define Z_USBH_PS_DM            (1u << 6)
#define Z_USBH_PS_DP            (1u << 7)
#define Z_USBH_PS_FRAME(r)      (((r) >> 16) & 0x7ff)

// IRQSTAT is write-one-to-clear, and clearing a port-change bit
// acknowledges the port that raised it. IRQ 9 is a LEVEL derived from
// (IRQSTAT & IRQEN), so a handler that returns without clearing is
// re-entered immediately -- the same contract Z_IRQ_UART has.
#define Z_USBH_IRQ_XACT_DONE    (1u << 0)
#define Z_USBH_IRQ_PORT0        (1u << 1)
#define Z_USBH_IRQ_PORT1        (1u << 2)
#define Z_USBH_IRQ_POLL_CHANGE  (1u << 3)
#define Z_USBH_IRQ_POLL_ERROR   (1u << 4)
#define Z_USBH_IRQ_SOF          (1u << 5)

#define Z_USBH_PID_SETUP    0
#define Z_USBH_PID_IN       1
#define Z_USBH_PID_OUT      2

// XACT_A. lowspeed and inverted are SEPARATE and are not a speed enum:
//
//   full speed, direct      (none)
//   low speed, direct       LOWSPEED | INVERTED
//   low speed behind a hub  LOWSPEED | USE_PRE        <- not INVERTED
//
// The third is the one that catches people. A low-speed device behind
// a full-speed hub sits on a segment the hub drives, so the packet
// leaves the root port with full-speed polarity at the low-speed bit
// rate. Set INVERTED there and it silently never works.
#define Z_USBH_XA_ADDR(a)   ((uint32_t)(a) & 0x7f)
#define Z_USBH_XA_ENDP(e)   (((uint32_t)(e) & 0xf) << 7)
#define Z_USBH_XA_PID(p)    (((uint32_t)(p) & 3) << 11)
#define Z_USBH_XA_LOWSPEED  (1u << 13)
#define Z_USBH_XA_INVERTED  (1u << 14)
#define Z_USBH_XA_USE_PRE   (1u << 15)
#define Z_USBH_XA_PORT(p)   (((uint32_t)(p) & 1) << 16)
#define Z_USBH_XA_TOGGLE    (1u << 17)
#define Z_USBH_XA_AUTOCONT  (1u << 18)
#define Z_USBH_XA_MPS(m)    (((uint32_t)(m) & 0x7f) << 19)

#define Z_USBH_XB_OFF(o)    ((uint32_t)(o) & 0x7ff)
#define Z_USBH_XB_LEN(l)    (((uint32_t)(l) & 0x7ff) << 11)
#define Z_USBH_XB_NAK(n)    (((uint32_t)(n) & 0xf) << 22)
#define Z_USBH_XB_START     (1u << 31)

#define Z_USBH_XS_LEN(s)    ((s) & 0x7ff)
#define Z_USBH_XS_STATUS(s) (((s) >> 11) & 0xf)
#define Z_USBH_XS_TOGGLE(s) (((s) >> 15) & 1)
// Set from the moment the start bit is written, not from when the
// engine gets round to it. "Write start, poll until not pending" is
// therefore safe; polling the engine alone would read the PREVIOUS
// transaction's status through a three-cycle window.
#define Z_USBH_XS_PENDING   (1u << 16)
#define Z_USBH_XS_NAKS(s)   (((s) >> 17) & 0xff)

#define Z_USBH_ST_OK        0
#define Z_USBH_ST_NAK       1
#define Z_USBH_ST_STALL     2
#define Z_USBH_ST_TIMEOUT   3
#define Z_USBH_ST_CRCERR    4
#define Z_USBH_ST_BABBLE    5
// A SUCCESS, not a failure. A bulk IN that returns fewer bytes than
// asked for is how a device says that is all there is, and the mass
// storage driver depends on telling it apart from a complete read.
#define Z_USBH_ST_SHORT     6
#define Z_USBH_ST_ABORT     7

#define Z_USBH_CFG_VERSION(c)   ((c) & 0xff)
#define Z_USBH_CFG_PORTS(c)     (((c) >> 8) & 0xf)
#define Z_USBH_CFG_SLOTS(c)     (((c) >> 12) & 0xf)
#define Z_USBH_CFG_BUFKB(c)     (((c) >> 16) & 0xf)
#define Z_USBH_CFG_MAGIC(c)     (((c) >> 20) & 0xfff)
#define Z_USBH_MAGIC            0x05b

// -- auto-poll --
//
// Set a slot up once, at enumeration, and then stop being involved.
// The hardware repeats the interrupt IN by itself and, in a boot mode,
// feeds the bytes straight into the compat block and the cursor
// datapath -- no interrupt, no ISR, no CPU in the path at all. That is
// what keeps the pointer free of the jitter it would inherit from
// k_hid_read_key() and k_fs_enter(), which mask interrupts.
//
// ctgt names the COMPAT BLOCK, not the physical port, and the two are
// deliberately independent: with a hub a mouse and a keyboard can both
// be behind port 0 and software still expects them at reg_usb0_* and
// reg_usb1_*.
#define Z_USBH_POLL_A(n)    (0xc0000200u + (n) * 8u)
#define Z_USBH_POLL_B(n)    (0xc0000204u + (n) * 8u)

#define Z_USBH_PA_ADDR(a)   ((uint32_t)(a) & 0x7f)
#define Z_USBH_PA_ENDP(e)   (((uint32_t)(e) & 0xf) << 7)
#define Z_USBH_PA_LOWSPEED  (1u << 11)
#define Z_USBH_PA_INVERTED  (1u << 12)
#define Z_USBH_PA_USE_PRE   (1u << 13)
#define Z_USBH_PA_PORT(p)   (((uint32_t)(p) & 1) << 14)
#define Z_USBH_PA_MPS(m)    (((uint32_t)(m) & 0x7f) << 15)
#define Z_USBH_PA_INTERVAL(f) (((uint32_t)(f) & 0xff) << 22)
#define Z_USBH_PA_ENABLE    (1u << 30)

#define Z_USBH_PB_OFF(o)    ((uint32_t)(o) & 0x7ff)
#define Z_USBH_PB_MODE(m)   (((uint32_t)(m) & 7) << 11)
#define Z_USBH_PB_CTGT(c)   (((uint32_t)(c) & 3) << 14)
#define Z_USBH_PB_CHANGED   (1u << 16)
#define Z_USBH_PB_STATUS(b) (((b) >> 17) & 7)

#define Z_USBH_MODE_OFF         0
// Payload lands in the packet buffer and raises IRQ 9. For a hub's
// port-change endpoint, a gamepad, or anything not boot protocol.
#define Z_USBH_MODE_RAW         1
#define Z_USBH_MODE_BOOT_MOUSE  2
#define Z_USBH_MODE_BOOT_KBD    3

// -- packet buffer --
//
// 2 KB, one block RAM. SOFTWARE MUST NOT TOUCH IT WHILE A TRANSACTION
// IS RUNNING: poll XACT_S until Z_USBH_XS_PENDING clears first. This
// is not merely good manners -- it is the condition under which the
// buffer is allowed to be a block RAM at all. See docs/usb_host.md.
//
// The layout below is convention, not hardware: the controller only
// ever sees the offsets it is given.
#define Z_USBH_BUF          0xc0001000u
#define Z_USBH_BUF_SIZE     2048

#define Z_USBH_OFF_SECTOR   0x000   // 512, one FatFs sector
#define Z_USBH_OFF_SETUP    0x200   // 8   (port 0, see below)
#define Z_USBH_OFF_CTRL     0x208   // 64, control data stage

// -- PER-PORT control scratch --
//
// The two root ports enumerate CONCURRENTLY, and every control
// transfer used to land on the single region above. Two devices
// plugged in together interleaved their transactions and overwrote
// each other's descriptors: each device worked perfectly on its own,
// and together one of them read back a configuration descriptor with
// another device's bytes in the first packet.
//
// 256 bytes per port from 0x400, which also fixes a second latent
// problem: the shared region had only 64 bytes before the mass-storage
// block at 0x248, and a configuration descriptor read can ask for 200.
//
// Port n: SETUP at BASE, data stage at BASE+8.
#define Z_USBH_OFF_PORT(n)  (0x400u + (unsigned)(n) * 0x200u)
#define Z_USBH_OFF_PSETUP(n) (Z_USBH_OFF_PORT(n))
#define Z_USBH_OFF_PCTRL(n) (Z_USBH_OFF_PORT(n) + 8u)
#define Z_USBH_OFF_CBW      0x248   // 31
#define Z_USBH_OFF_CSW      0x268   // 13
#define Z_USBH_OFF_DESC     0x280   // 256, descriptor scratch
#define Z_USBH_OFF_POLL(n)  (0x380 + (n) * 16)

#endif
