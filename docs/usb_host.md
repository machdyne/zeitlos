# Zeitlos USB Host Controller

**STATUS: phases 0 through 3 complete. WORKING ON HARDWARE** --
a low-speed keyboard and mouse enumerate together on mozart_ml1 and
both deliver reports through the auto-poll slots.

The controller is instantiated in `rtl/sysctl.v`, synthesises as part
of the whole SoC, and the kernel builds with the stack linked in. The
driver and the gateware are exercised against each other in
simulation -- `make test_usb_cosim` enumerates a device end to end and
moves the hardware cursor. Nothing has run on a board.

The bit level, the port front end, the transaction engine, the
Wishbone top, the HID compatibility blocks and the auto-poll engine
are written and pass simulation at full speed, at low speed wired
directly, and at low speed behind a hub. A boot-mouse report drives the
cursor through hardware with no CPU in the path, which is the phase 2
exit criterion that mattered most.

The enumeration software is written, compiles clean under `-Wall`,
links into the kernel and fits. What has NOT happened is
`rtl/sysctl.v` instantiating the controller, and nothing has run on
hardware or against the RTL.

This is the Phase 0 deliverable of the project to replace
`rtl/ext/usb_hid_host` with a Zeitlos-native, dual-port, full-speed USB
host controller supporting HID, hubs, mass storage and CDC. It is the
document to argue with before any RTL exists, because everything after
Phase 1 is expensive to change and the register map in particular is a
contract with `sw/os/hid.c`, `sw/apps/wm/wm.c` and every future class
driver at once.

Read [user_input.md](user_input.md) first for what the *current*
`usb_hid_host`-based ports do and how software consumes them. This
document says what replaces it and why.

## Contents

- [Why replace usb_hid_host](#why-replace-usb_hid_host)
- [Clean-room statement](#clean-room-statement)
- [Architecture](#architecture)
- [Where the hardware/software line goes](#where-the-hardwaresoftware-line-goes)
- [Speeds, polarity and PRE](#speeds-polarity-and-pre)
- [Ports, hubs and topology](#ports-hubs-and-topology)
- [Register map](#register-map)
- [The packet buffer](#the-packet-buffer)
- [Auto-poll](#auto-poll)
- [Interrupts](#interrupts)
- [Software stack](#software-stack)
- [Mass storage and the filesystem](#mass-storage-and-the-filesystem)
- [CDC](#cdc)
- [Resource budget](#resource-budget)
- [Board and electrical notes](#board-and-electrical-notes)
- [Phase plan](#phase-plan)
- [Phase 1 results](#phase-1-results)
- [Phase 2 results](#phase-2-results)
- [Phase 3 results](#phase-3-results)
- [Testing](#testing)
- [Open decisions](#open-decisions)
- [Deliberately not done](#deliberately-not-done)

---

## Why replace usb_hid_host

Not licensing, and not code quality. **Bus speed.**

`usb_hid_host` is a low-speed host: 1.5 Mbps, clocked from a single
12 MHz clock, as its own documentation states. That is not a limitation
you can extend around, because USB 2.0 §5.8.1 does not define bulk
transfers for low-speed devices *at all*. A low-speed device gets
control and interrupt transfers with a maximum packet size of 8 bytes,
and nothing else.

Mass storage is bulk-only by definition (USB Mass Storage Class
Bulk-Only Transport, "BOT"). So:

> **USB mass storage requires full speed (12 Mbps) as an absolute
> floor.** There is no low-speed path to a USB stick, at any level of
> cleverness.

Every USB stick is high-speed capable and falls back to full speed when
the host does not chirp at it during reset, so full speed is sufficient
— but it is also mandatory.

The second reason is structural. `usb_hid_host` is a 620-nibble
microcode ROM (`usb_hid_host_rom.hex`) driving a purpose-built
sequencer. That microcode hard-codes one enumeration sequence and
recognises three device shapes; anything it does not recognise does not
exist as far as the SoC is concerned. That design cannot grow a hub
driver, a SCSI layer or a CDC endpoint, because the thing that would
have to grow is a ROM image, not a C file. It is also why the existing
ports cannot set a keyboard's Num Lock LED: the microcode has no OUT
transfer capability, which `user_input.md` records as "not attempted".

So the replacement is a rewrite for reasons that would hold even if the
existing core were public domain.

## Clean-room statement

This core is written from published specifications only:

- **USB 2.0 specification** — chapter 7 (electrical), chapter 8
  (protocol: packets, PIDs, CRC5/CRC16, bit stuffing, NRZI, handshakes,
  timeouts), chapter 9 (device framework), chapter 11 (hub class).
- **USB Mass Storage Class Bulk-Only Transport, revision 1.0.**
- **SCSI Primary Commands (SPC) and SCSI Block Commands (SBC)** for the
  command subset in [Mass storage](#mass-storage-and-the-filesystem).
- **USB Communications Device Class, ACM subclass (CDC-ACM).**

`rtl/ext/usb_hid_host` (Apache 2.0) and `rtl/ext/usb_cdc` (its own
licence) are **not consulted while authoring**. No code, no microcode,
no state-machine structure and no register layout is taken from either.

**One distinction that matters and is easy to get wrong.**
`rtl/usb_hid.v` is *not* vendored code. It is Lone Dynamics copyright,
written for this project, and it is the file that holds the register
layout, the pointer acceleration, the `SENS_SHIFT` remainder-carry
arithmetic and the cursor saturation logic. **That file is ours and its
contents may be reused verbatim.** This is convenient rather than
incidental: that arithmetic is exactly the part which has to stay
bit-compatible, so being able to move it across unchanged removes the
main risk of a compatibility regression.

The dividing line, concretely:

| File | Origin | May we reuse it |
|---|---|---|
| `rtl/ext/usb_hid_host/**` | nand2mario, Apache 2.0 | **No** |
| `rtl/ext/usb_cdc/**` | third party | **No** |
| `rtl/usb_hid.v` | Lone Dynamics | **Yes, verbatim** |
| `sw/os/hid.c`, `sw/common/zkbd.*` | Lone Dynamics | **Yes** |

## Architecture

```
                 ┌──────────────────────────────────────────────┐
  usb_host_dp[0] │  usb_port.v  #0                              │
  usb_host_dm[0] │  tri-state, line state, attach/debounce,     │
   ──────────────┤  reset, speed detect, SOF/keepalive          │
                 └───────────────┬──────────────────────────────┘
                                 │
                 ┌───────────────┴───────────────┐
  usb_host_dp[1] │  usb_port.v  #1  (optional)   │   port mux
  usb_host_dm[1] │                               │   (one port
   ──────────────┤                               │    at a time)
                 └───────────────┬───────────────┘
                                 │
                 ┌───────────────┴──────────────────────────────┐
                 │  usb_sie.v                                   │
                 │  NRZI, bit stuff/unstuff, CRC5, CRC16,       │
                 │  RX DPLL (4x oversample), TX/RX shift,       │
                 │  programmable bit period (FS ÷4, LS ÷32),    │
                 │  mid-transaction rate switch for PRE         │
                 └───────────────┬──────────────────────────────┘
                                 │
                 ┌───────────────┴──────────────────────────────┐
                 │  usb_xact.v                                  │
                 │  one transaction: token, data, handshake,    │
                 │  DATA0/1 toggle, NAK retry, turnaround       │
                 │  timeout, multi-packet auto-continue         │
                 └───────────────┬──────────────────────────────┘
                                 │
        ┌────────────────────────┴───────────────────────────────┐
        │  usb_host.v                                            │
        │  frame timer (1 ms SOF), scheduler, auto-poll table,   │
        │  packet buffer (1x DP16KD), Wishbone registers         │
        └────────┬───────────────────────────────────┬───────────┘
                 │                                   │
     ┌───────────┴───────────┐            Wishbone / IRQ 9
     │ usb_hid_compat.v  x2  │
     │ reg_usbN_* images,    │
     │ accel/clamp cursor    ├──── curs_x/curs_y ──▶ gpu_cursor.v
     │ (from rtl/usb_hid.v)  ├──── IRQ 5 / IRQ 6 ──▶ sw/os/hid.c
     └───────────────────────┘
```

Files, all new:

```
rtl/usb/usb_sie.v          bit level
rtl/usb/usb_port.v         per-port analogue-adjacent front end
rtl/usb/usb_xact.v         one transaction
rtl/usb/usb_host.v         top: registers, buffer, scheduler, auto-poll
rtl/usb/usb_hid_compat.v   backwards-compatible reg_usbN_* block
rtl/tb/tb_usb_device.v     behavioural FS/LS device model
rtl/tb/tb_usb_hub.v        behavioural hub model
rtl/tb/tb_usb_host.v       the testbench itself
```

`rtl/usb_hid.v` and both `rtl/ext/usb_hid_host` entries leave
`RTL_PICO` in the Makefile when Phase 2 lands. `rtl/ext/usb_cdc` stays
— that is the USB *device* on the USB-C socket (`docs/usb_cdc.md`) and
is unrelated to this work.

### One clock domain

`rtl/sysctl.v` has `SYSCLK = 48_000_000` and `sys_clk = clk48mhz`, and
`wbm_clk = sys_clk`. 48 MHz is exactly 4x the full-speed bit rate,
which is the standard soft-PHY oversampling ratio, and it is also the
Wishbone clock.

**The whole core therefore runs in one clock domain with no CDC
anywhere.** This is worth stating loudly because `rtl/usb_hid.v` is
mostly clock-domain-crossing code today — `report_s0/s1/s2`,
`typ_s0/s1/s2`, the `dx_cap`/`dy_cap` capture-one-cycle-early trick,
and a long comment explaining a cursor bug that turned out to be a
missing synchroniser. All of that exists because `usb_hid_host` runs at
12 MHz. None of it is needed here, and the arithmetic those
synchronisers were feeding moves across unchanged.

`clk12mhz` keeps its other users; this core does not need it.

## Where the hardware/software line goes

This is the central decision and everything else follows from it.

> **Hardware owns what gateware consumes. Everything else is software,
> with no observable difference.**

Applying that rule:

| Thing | Where | Why |
|---|---|---|
| Sync, NRZI, bit stuffing, CRC5/CRC16, EOP | Hardware | Bit-rate work, impossible in software at 12 Mbps |
| Token/data/handshake sequencing, turnaround timeouts | Hardware | Sub-microsecond deadlines |
| 1 ms frames and SOF | Hardware | Devices suspend without it |
| Attach detect, debounce, reset, speed detect | Hardware | Long timers, trivial in logic |
| **Boot-protocol mouse → cursor position** | **Hardware** | `gpu_curs_x`/`gpu_curs_y` are *wires* into `gpu_cursor.v`. Gateware consumes this, so gateware must produce it |
| Boot-protocol keyboard → `reg_usbN_keys` | Hardware | Free: same fixed layout, same auto-poll path |
| Enumeration, descriptor parsing | Software | Variable-length TLV walking; a C loop, not a state machine |
| Hub port management | Software | It is just control transfers to an ordinary device |
| HID report descriptor parsing | Software | Arbitrary grammar; gamepads and wheels need it |
| SCSI, BOT, CDC | Software | Nothing in gateware consumes a file |
| Device → driver binding | Software | This is the whole point: a new class is a `.c` file |

### Why the cursor stays in hardware

The concern raised in review was that a software-driven cursor would be
noticeable. Taking it seriously:

A boot-protocol mouse reports at its endpoint interval, typically 10 ms
(`bInterval = 10`), sometimes 8. That is ~100 reports/sec. Servicing
one in software costs an ISR entry, a 4-byte read from the packet
buffer, the delta arithmetic and a register write — call it 500 cycles,
about 10 µs at 48 MHz. **0.1% CPU, and ~0.06% of a 16.7 ms display
frame of added latency.** On averages alone, invisible.

**The average is not the problem. Jitter is.** This kernel masks
interrupts in places — `k_hid_read_key()` wraps the ring update in
`maskirq(0xFFFFFFFF)`, `k_fs_enter()`/`k_fs_leave()` bracket FatFs — and
a long SD transaction or a large blit can stall the ISR long enough
that several reports bunch and the pointer moves in visible steps. That
is load-dependent, intermittent, and exactly the class of bug that gets
reported as "the mouse feels bad sometimes" and takes a week to find.

So it is not accepted. The cursor datapath stays in gateware, fed by
[auto-poll](#auto-poll), with **no CPU in the loop at all** — same as
today. What moves to software is *enumeration*, which happens once per
plug event and has hundreds of milliseconds of budget.

### Why not put enumeration in hardware too

Because it buys nothing and costs a lot:

- A hardware descriptor walker must parse variable-length TLV
  structures with nested interface/endpoint descriptors and vendor
  classes interleaved. Hundreds of LUTs.
- HID report descriptors are a small stack language. Parsing one in
  gateware to find where a scroll wheel's bits live is not a
  proportionate response.
- Hub port management is a *sequence of control transfers*, i.e. the
  same thing enumeration is. A hardware enumerator would need a second
  one, and then MSC would need a third, and all three would contend for
  one SIE.
- Most importantly it recreates `usb_hid_host`'s defining weakness:
  every device that deviates from what the state machine expects needs
  a **bitstream** fix rather than a kernel fix.

Software enumeration with a hardware fast path gets the responsiveness
without any of that.

## Speeds, polarity and PRE

At 48 MHz:

| Speed | Bit rate | Clocks per bit |
|---|---|---|
| Full speed | 12 Mbps | 4 |
| Low speed | 1.5 Mbps | 32 |

One SIE with a programmable bit-period divisor covers both. The RX DPLL
resynchronises on every NRZI transition; bit stuffing guarantees a
transition at least every 7 bits and the device's clock is specified to
±0.25%, so worst-case drift across a stuffed run is well under half a
bit period at 4x oversampling. This is a well-trodden ratio.

### Polarity — the trap

**Direct-attach low speed and low speed behind a hub are not the same
bit pattern on the wire.** Getting this backwards produces a core where
direct-attach LS works perfectly and LS-behind-a-hub silently never
does, which is a miserable debugging session.

| Case | Who pulls up | Idle (J) | Line polarity |
|---|---|---|---|
| FS device, direct | device pulls up D+ | D+ high | normal |
| LS device, direct | device pulls up D− | D− high | **inverted** |
| FS device via hub | hub port drives FS | D+ high | normal |
| **LS device via hub** | **hub port segment is FS** | **D+ high** | **normal** |

A low-speed device behind a full-speed hub sits on a bus segment the
*hub* drives. The hub is full speed toward us. So the packet leaves our
port with **full-speed polarity at the low-speed bit rate**, which is
the one combination that looks wrong and is right.

The SIE therefore takes polarity and bit rate as two independent
inputs, not one "speed" enum. `usb_port.v` reports which line has the
pull-up after attach; `usb_xact.v` supplies polarity from the *device's
topology*, not from the port's own detected speed.

### PRE

Reaching a low-speed device through a hub requires a PREamble packet:

```
  ┌─────────────────── full speed, 4 clk/bit ──────────────────┐
  SYNC | PRE PID |  (hub setup: 4 FS bit times, bus idle)
                    ┌──────── low speed, 32 clk/bit ───────────┐
                    SYNC | TOKEN ... | DATA ... | handshake
```

The host sends SYNC and the PRE PID at full speed, holds the bus idle
for the hub setup interval, then transmits the low-speed packet at the
low-speed rate and receives the response at the low-speed rate. The bus
returns to full speed at the end of the transaction.

This means **the SIE must change bit rate mid-transaction.** That is
about 100 LUT4 on top of the divisor we already need — but it is not
something you retrofit into a finished serialiser cleanly, because the
rate change has to be sequenced against the shift register, the stuffing
counter and the DPLL reset all at once.

> **PRE is designed into Phase 1 even though it cannot be tested against
> real hardware until Phase 4.** The behavioural hub model in
> `tb_usb_hub.v` exists to cover it in the meantime.

## Ports, hubs and topology

### No transaction translators

This is the single fact that makes hub support tractable here.

Transaction translators exist so a *high-speed* host can talk to
full- and low-speed devices behind a hub, and they are where the
genuinely hard parts of hub support live: split transactions,
start-split/complete-split scheduling, per-TT bandwidth accounting.

**This is a full-speed host.** A full-speed host sees a full-speed hub
as a repeater with a port controller. No TTs, no split transactions,
none of that machinery. What remains is:

- a class driver issuing ordinary control transfers to the hub;
- an interrupt-IN endpoint delivering a port-change bitmap;
- PRE, above, for low-speed devices behind it.

Phase 2 already provides control and interrupt transfers, so **the hub
driver needs no new transfer type at all.** It is a `.c` file plus the
PRE support in the SIE.

### Hub operations

All of these are standard control transfers to the hub's own address:

```
GET_DESCRIPTOR(HUB)            port count, power switching, characteristics
SET_PORT_FEATURE(PORT_POWER)   per downstream port
GET_PORT_STATUS                connected / enabled / speed / changes
SET_PORT_FEATURE(PORT_RESET)   hub performs the 10 ms reset for us
CLEAR_PORT_FEATURE(C_PORT_*)   acknowledge a change bit
```

Plus an interrupt-IN endpoint whose payload is a bitmap of which ports
have a pending change. That endpoint is a perfectly ordinary auto-poll
slot in `RAW` mode.

Note that the hub performs the port reset itself, in response to
`SET_PORT_FEATURE(PORT_RESET)`, and reports the resulting speed in
`GET_PORT_STATUS`. So `usb_port.v`'s own reset sequencer is only ever
used for the two root ports.

### Topology model

Bounded deliberately — unlimited topology buys nothing and costs kernel
RAM:

| Limit | Value | Note |
|---|---|---|
| Root ports | 1 or 2 | `USB_HOST_PORTS` |
| Devices | 8 | including hubs |
| Hub depth | 3 | spec allows 5; 3 covers anything real |
| Endpoints tracked | 16 | across all devices |
| Auto-poll slots | 4 | hardware table |

USB addresses 1..7 are allocated by the kernel from a bitmap. Address 0
is the enumeration address and is used by one device at a time, which
the kernel serialises with a lock — this matters with hubs, because two
devices plugged into the same hub within the same second both want
address 0 and must not get it simultaneously.

### Hubs make the second root port optional

Worth stating because it changes the area calculus: with hub support, a
board built `USB_HOST_PORTS 1` can still run mouse + keyboard + stick
simultaneously. On a tight part (Obst, ECP5-12F) that is how the area
this project spends gets paid back. See
[Resource budget](#resource-budget) and
[Open decisions](#open-decisions).

## Register map

### Address window and an existing bug to fix first

The 0xC top nibble is USB. The current decode in `rtl/sysctl.v` is:

```verilog
wire cs_usb0 = ((wbm_adr & 32'hf000_0020) == 32'hc000_0000);
wire cs_usb1 = ((wbm_adr & 32'hf000_0020) == 32'hc000_0020);
```

That mask covers the top nibble and **bit 5 only**. Every other address
bit is a don't-care, so `0xc000_1000` matches `cs_usb0`, `0xc000_ff20`
matches `cs_usb1`, and between them the two decodes claim the *entire
256 MB nibble*.

Today that is harmless — nothing else lives there, and the wide decode
means every address in the nibble acks, which is the safe direction
given that an unacked address hangs `picorv32_wb` forever (see
`sysctl.v`'s own note on the `0x7000_01xx` window). But it means
**narrowing these decodes is a prerequisite for adding anything else in
the nibble**, and `tools/hwmap/hwmap --check` would report the new
windows as `overlap` findings if we did not.

Proposed replacement:

```verilog
wire cs_usb_hid  = ((wbm_adr & 32'hf000_ff00) == 32'hc000_0000);  // compat
wire cs_usb_ctrl = ((wbm_adr & 32'hf000_ff00) == 32'hc000_0100);
wire cs_usb_poll = ((wbm_adr & 32'hf000_ff00) == 32'hc000_0200);
wire cs_usb_buf  = ((wbm_adr & 32'hf000_f800) == 32'hc000_1000);
wire cs_usb_rest = ((wbm_adr & 32'hf000_0000) == 32'hc000_0000) &&
                   !cs_usb_hid && !cs_usb_ctrl &&
                   !cs_usb_poll && !cs_usb_buf;
```

`cs_usb_rest` exists solely so the rest of the nibble still acks and
reads zero. **Do not omit it.** Dropping the catch-all would turn a
stale pointer into a dead hang rather than a zero read, which is the
same lesson `sysctl.v` records having learned on the cache window.

The masked-decode form above is what `tools/hwmap/hwmap` recognises
(`docs/hwmap.md`, "Decodes"), including the `&& !other_decode` terms.

### Overall map

| Range | Block | Notes |
|---|---|---|
| `0xc000_0000`–`0xc000_003f` | HID compat | **Unchanged.** Port block 0 at `+0x00`, block 1 at `+0x20` |
| `0xc000_0100`–`0xc000_01ff` | Host control/status | |
| `0xc000_0200`–`0xc000_027f` | Auto-poll table | 4 slots x 2 words |
| `0xc000_1000`–`0xc000_17ff` | Packet buffer | 2 KB, 32-bit accessible |
| everything else in `0xC` | acks, reads zero | |

### HID compatibility block — bit-for-bit unchanged

These are the registers `sw/os/hid.c`, `sw/apps/wm/wm.c`,
`sw/apps/gpu3d/gpu3d.c` and `sw/bios/bios.c` already use. **Their
layout does not change.** `user_input.md` documents them and stays
correct.

```c
reg_usb0_info   0xc0000000    reg_usb1_info   0xc0000020
reg_usb0_keys   0xc0000004    reg_usb1_keys   0xc0000024
reg_usb0_mouse  0xc0000008    reg_usb1_mouse  0xc0000028
reg_usb0_cursor 0xc000000c    reg_usb1_cursor 0xc000002c
reg_usb0_pad    0xc0000010    reg_usb1_pad    0xc0000030
```

Including the quirks: `typ` at bits `[25:24]` of `info`, and `cursor`
truncating from the top to `{ 4'b0, mouse_btn[27:20], curs_y[19:10],
curs_x[9:0] }`. Those are not tidied up. Tidying them would be a
gratuitous change to code that works, and `user_input.md` already
documents both as traps.

**One semantic change, and it needs writing down.** `typ` is now
assigned by the kernel's USB stack rather than inferred by microcode.
Between physical attach and the end of enumeration — USB requires
100 ms debounce, 10 ms reset and 10 ms recovery, so realistically
200–300 ms — `typ` reads 0. Zero already means "nothing on this port",
which every consumer handles, so the behaviour is a slightly longer
version of something they already cope with. But it is a real
difference from a core that inferred a device type from the first
descriptor it saw.

**A second change, less obvious and more important with hubs.** The
compat block index is *not* the physical root port any more. With a hub,
a mouse and a keyboard may both be behind physical port 0, yet software
still wants them to appear as `reg_usb0_*` and `reg_usb1_*`. So each
auto-poll slot names its compat target explicitly. The kernel assigns
block 0 to the first pointing device it enumerates and block 1 to the
first keyboard, or vice versa — `wm.c` and `hid.c` already decide which
block is which at runtime by reading `typ`, and keep working unchanged.

### Host control/status

All 32-bit, word addresses.

**`0xc000_0100` CTRL** (RW)

| Bits | Name | Meaning |
|---|---|---|
| 0 | `srst` | Soft reset of the whole controller |
| 1 | `frame_en` | Run the 1 ms frame timer, emit SOF/keepalive |
| 2 | `poll_en` | Enable the auto-poll scheduler |
| 8 | `p0_enable` | Port 0 enabled for traffic |
| 9 | `p0_reset` | Write 1: drive SE0 for 10 ms, then self-clear |
| 10 | `p0_suspend` | Stop SOF to port 0 |
| 16 | `p1_enable` | |
| 17 | `p1_reset` | |
| 18 | `p1_suspend` | |

**`0xc000_0104` PORTSTAT** (RO)

Per port `n`, eight bits at `[n*8 +: 8]`:

| Bit | Name | Meaning |
|---|---|---|
| 0 | `connected` | Line state shows a pull-up, debounced |
| 1 | `enabled` | |
| 2 | `low_speed` | Device pulls up D− |
| 3 | `resetting` | Reset sequencer is running |
| 4 | `conn_change` | Set on any connect/disconnect; W1C in IRQSTAT |
| 5 | `overcurrent` | **Always 0** — no sense hardware, see [Board notes](#board-and-electrical-notes) |

Bits `[26:16]` carry the current frame number.

**`0xc000_0108` IRQSTAT** (W1C) / **`0xc000_010c` IRQEN** (RW)

| Bit | Source |
|---|---|
| 0 | `xact_done` — transaction finished (any status) |
| 1 | `port0_change` |
| 2 | `port1_change` |
| 3 | `poll_change` — an auto-poll slot's data changed |
| 4 | `poll_error` — an auto-poll slot took a STALL or timeout |
| 5 | `sof` — frame boundary (diagnostics; normally masked) |

**`0xc000_0110` XACT_A** (RW) — addressing

| Bits | Name | Meaning |
|---|---|---|
| 6:0 | `dev_addr` | 0–127 |
| 10:7 | `endp` | 0–15 |
| 12:11 | `pid` | 0 = SETUP, 1 = IN, 2 = OUT |
| 13 | `lowspeed` | Use the low-speed bit rate |
| 14 | `inverted` | Use inverted line polarity — see [Polarity](#polarity--the-trap) |
| 15 | `use_pre` | Prefix with a PRE packet |
| 16 | `port` | Root port |
| 17 | `toggle` | Initial DATA0/1 |
| 18 | `auto_cont` | Split `length` into `mps`-sized packets automatically |
| 25:19 | `mps` | Max packet size, 8–64 |

`lowspeed` and `inverted` are deliberately separate bits rather than a
two-bit speed enum. Direct-attach LS sets both; LS behind a hub sets
`lowspeed` and `use_pre` but **not** `inverted`.

**`0xc000_0114` XACT_B** (RW) — buffer, length, go

| Bits | Name | Meaning |
|---|---|---|
| 10:0 | `buf_off` | Byte offset into the packet buffer |
| 21:11 | `length` | Bytes to transfer (0–2047) |
| 25:22 | `nak_retry` | NAK retry budget, 0 = fail on first NAK |
| 31 | `start` | Write 1 to launch |

**`0xc000_0118` XACT_S** (RO) — result

| Bits | Name | Meaning |
|---|---|---|
| 10:0 | `act_len` | Bytes actually transferred |
| 14:11 | `status` | see below |
| 15 | `toggle` | Toggle state after the transaction, for resuming |
| 16 | `busy` | |
| 24:17 | `naks` | NAKs absorbed, for diagnostics |

Status codes:

| Code | Meaning |
|---|---|
| 0 | OK |
| 1 | NAK (retry budget exhausted) |
| 2 | STALL |
| 3 | Timeout — no response within 18 bit times |
| 4 | CRC or bit-stuffing error |
| 5 | Babble / buffer overrun |
| 6 | Short packet (auto-continue ended early — normal for bulk IN) |
| 7 | Aborted (port went away mid-transaction) |

Code 6 is a success, not a failure: a bulk IN that returns fewer bytes
than requested is how a device says "that is all there is", and the MSC
driver depends on being able to tell it apart from a genuine error.

**`0xc000_011c` CONFIG** (RO)

| Bits | Name |
|---|---|
| 7:0 | `version` |
| 11:8 | `n_ports` |
| 15:12 | `n_poll_slots` |
| 19:16 | `buf_kb` |
| 31:20 | `MAGIC` |

Same pattern as `rtl/gpio.v`'s CONFIG: the CSR feature bit says the
block exists, this register says what shape it is. A driver built for 4
poll slots must not assume 4 on a board that built 2.

### Feature bits

- `CSR_FEATURES` bit 13 (`Z_FEATURE_USB_HID`) **stays set**, because
  the compat block is present and the question that bit answers — "is
  there a HID port here" — still gets the same answer.
- `CSR_FEATURES2` bit 4 (`Z_FEATURE2_USB_HOST`) is **new**: a general
  host controller capable of hubs, MSC and CDC. Bit 4 is the next free
  bit (0 GPIO, 1 UART1, 2 USB_CDC, 3 MONTMUL). Keep in sync with
  `sw/common/zsoc.h` as `rtl/csrs.vh` instructs.

## The packet buffer

2 KB of dual-port BRAM — one DP16KD — at `0xc000_1000`. CPU on one
port, SIE on the other, so a transaction can run while software reads
the previous result.

Suggested layout. **Software owns this**; the hardware only ever sees
the `buf_off` values it is given, so this is convention, not contract:

| Offset | Size | Use |
|---|---|---|
| `0x000` | 512 | MSC data sector |
| `0x200` | 8 | SETUP packet |
| `0x208` | 64 | Control transfer data stage |
| `0x248` | 31 | CBW (command block wrapper) |
| `0x268` | 13 | CSW (command status wrapper) |
| `0x280` | 256 | Descriptor scratch |
| `0x380` | 4 x 16 | Auto-poll slot buffers |
| `0x3c0` | 1088 | Free |

512 bytes for the sector is the important number: `ffconf.h` has
`FF_MIN_SS` = `FF_MAX_SS` = 512, so one FatFs sector is one buffer
region, and `auto_cont` turns it into a single transaction of eight
64-byte packets.

### Why not DMA into main memory

`rtl/dma.v` exists as a stub with the shape sketched, and
`arbiter_main` would take another master. A bus-mastering transaction
engine would remove the `memcpy` out of this buffer and lift the 2 KB
ceiling.

**Not in Phases 1–5.** It adds an arbiter port contending with the CPU,
the GPU and audio, and it makes the core's failure modes bus-wide
instead of local. Revisit in Phase 7 *with measurements* showing the
copy is actually the bottleneck. It probably is not — see
[throughput](#throughput).

## Auto-poll

The mechanism that keeps the cursor in hardware while enumeration stays
in software.

Software enumerates a device normally, sets boot protocol, then writes
one descriptor and stops paying attention. Hardware repeats the
interrupt-IN transfer every `interval` frames forever.

**`0xc000_0200 + n*8` POLL_A** (RW), `n` = 0..3

| Bits | Name |
|---|---|
| 6:0 | `dev_addr` |
| 10:7 | `endp` |
| 11 | `lowspeed` |
| 12 | `inverted` |
| 13 | `use_pre` |
| 14 | `port` |
| 21:15 | `mps` |
| 29:22 | `interval` (frames) |
| 30 | `enable` |
| 31 | `toggle` (maintained by hardware) |

**`0xc000_0204 + n*8` POLL_B** (RW)

| Bits | Name |
|---|---|
| 10:0 | `buf_off` |
| 13:11 | `mode` |
| 15:14 | `ctgt` — compat block 0 or 1 |
| 16 | `changed` (W1C) |
| 19:17 | `last_status` |

Modes:

| Mode | Name | Effect |
|---|---|---|
| 0 | `OFF` | |
| 1 | `RAW` | Data lands in the buffer, `changed` set, IRQ 9. Used for hub port-change endpoints and any non-boot device |
| 2 | `BOOT_MOUSE` | Bytes routed into the cursor datapath **and** the compat block |
| 3 | `BOOT_KBD` | Bytes routed into `reg_usbN_info`/`_keys`, IRQ 5 or 6 |

`BOOT_MOUSE` decodes the fixed boot layout:

| Byte | Field |
|---|---|
| 0 | buttons |
| 1 | dx, signed |
| 2 | dy, signed |
| 3 | wheel, signed — Phase 6, ignored before then |

and feeds bytes 1 and 2 into the acceleration, `SENS_SHIFT`
remainder-carry and saturation arithmetic **lifted verbatim from
`rtl/usb_hid.v`**, producing `curs_x`/`curs_y` exactly as now.

This works because boot protocol is a *fixed* layout. That is the whole
enabler: no report-descriptor parsing in gateware, just a byte mux. Boot
keyboard is equally fixed (modifier byte, reserved byte, six keycodes),
so `BOOT_KBD` costs almost nothing on top.

Gamepads get `RAW` and are decoded in software. That is a strict
improvement: `usb_hid_host` guesses among several pad layouts by
heuristic, and a software driver can parse the actual report
descriptor and write the result into `reg_usbN_pad`.

### Hot unplug

`rtl/usb_hid.v` currently clears `game_state` whenever `typ != 3`,
and raises an interrupt on `typ` change, specifically so `hid.c` can
synthesise key releases for a keyboard yanked mid-keypress. Both
comments in that file explain the failure they prevent at length.

**That behaviour is preserved exactly**, and is now more robust: the
kernel knows a device is gone (port change, or hub port status) and
clears the compat block and disables the poll slot deliberately, rather
than relying on a device-type field going to zero as a side effect.
`hid.c`'s existing release-flush path is unchanged and is the
regression test for Phase 3.

## Interrupts

| Line | Name | Source | Latched? |
|---|---|---|---|
| 5 | `Z_IRQ_HID` | compat block 0 | Latched (unchanged) |
| 6 | `Z_IRQ_HID1` | compat block 1 | Latched (unchanged) |
| **9** | **`Z_IRQ_USB`** | **host controller** | **Level, NOT latched** |

IRQ 9 is the next free line (0 cpu timer, 1 ebreak, 2 bus error,
3 ktimer, 4 uart, 5/6 HID, 7 audio, 8 ethernet).

**It must be non-latched**, and `LATCHED_IRQ` in `rtl/sysctl.v` must
change:

```verilog
// was
.LATCHED_IRQ(32'b1111_1111_1111_1111_1111_1111_0110_1111)
// becomes -- bit 9 cleared alongside 4 and 7
.LATCHED_IRQ(32'b1111_1111_1111_1111_1111_1101_0110_1111)
```

The reasoning is the one `sysctl.v` already states for bits 4 and 7: a
latched *level* source re-fires the instant the handler returns and the
machine stops making forward progress. IRQ 9 is a level derived from
`(IRQSTAT & IRQEN) != 0`, and the handler lowers it by writing 1s to
IRQSTAT — the same shape as the UART, which drains its FIFO. This is
the correct pattern precisely because the handler *can* clear the
source, which is what `sysctl.v` identifies as the property ethernet
lacks.

Keeping IRQ 5/6 latched is right for the opposite reason: they stay
pulse-shaped, one per report, exactly as today.

## Software stack

```
sw/os/usb/usbh.c        core: port FSM, control transfers, enumeration,
sw/os/usb/usbh.h              address allocation, driver binding
sw/os/usb/usbh_hw.h     register definitions (mirrors this document)
sw/os/usb/usbh_hid.c    boot keyboard, boot mouse, gamepad
sw/os/usb/usbh_hub.c    hub class driver
sw/os/usb/usbh_msc.c    BOT + SCSI                       (Phase 5)
sw/os/usb/usbh_cdc.c    CDC-ACM                          (Phase 6)
sw/os/fs/usbdisk.c      FatFs block device glue          (Phase 5)
```

### Enumeration must not block

Enumeration spans **hundreds of milliseconds**: 100 ms attach debounce,
10 ms reset, 10 ms recovery, then several control transfers each of
which may NAK. It absolutely must not be busy-waited, and it must not
run inside an ISR.

Shape: a state machine per device slot, advanced from two places —
the IRQ 9 handler (a transaction completed, a port changed) and the
existing ~732 Hz ktimer (a timer expired). Each entry does at most one
transaction and returns. No loops, no waits.

This matters more than it sounds. The scheduler is pre-emptive and
`k_fs_enter()`/`k_fs_leave()` already serialise FatFs; a USB stack that
blocked would deadlock against a filesystem operation that was waiting
on a USB disk.

### Throughput

Full speed is 12 Mbps raw. With 64-byte bulk packets and one CPU
interrupt per packet, a 48 MHz PicoRV32 manages perhaps **150–300 KB/s**.

With `auto_cont` — hardware walks eight 64-byte packets, incrementing
the buffer pointer and toggling DATA0/1 itself, one interrupt per
512-byte sector — the wire time per sector is about 420 µs and the
ceiling moves toward **700 KB/s–1 MB/s**, at which point the copy out of
the packet buffer and FatFs itself become the limit.

**Realistic target: 300–600 KB/s sequential.** Benchmark against the SPI
SD card using the existing `sw/os/fs/sdbench.c` machinery. Nobody should
expect USB 2.0 high-speed numbers from a 12 Mbps bus.

`auto_cont` is therefore not an optimisation to defer — MSC is not
usable without it, so it lands in Phase 5 alongside the driver rather
than in a later tuning phase.

## Mass storage and the filesystem

Bulk-Only Transport is three bulk transfers per command:

```
  CBW  (31 bytes, OUT)  →  data (IN or OUT, optional)  →  CSW (13 bytes, IN)
```

SCSI command subset, which is all a FAT filesystem needs:

| Command | Opcode | Use |
|---|---|---|
| `INQUIRY` | 0x12 | Device type, vendor strings |
| `TEST UNIT READY` | 0x00 | Media present? |
| `REQUEST SENSE` | 0x03 | Why did that fail |
| `READ CAPACITY (10)` | 0x25 | Block count and block size |
| `READ (10)` | 0x28 | |
| `WRITE (10)` | 0x2A | |
| `PREVENT ALLOW MEDIUM REMOVAL` | 0x1E | Optional, ignorable |

`READ CAPACITY` must be checked against `FF_MAX_SS` (512). Sticks with
4096-byte logical sectors are rare but exist; **refuse them cleanly
rather than corrupt them.**

### FatFs integration

The existing structure takes this with almost no disturbance.
`sw/os/fs/fatfs/diskio_mux.c` was written for exactly this — its header
comment already anticipates a dispatcher over multiple drives:

```c
// diskio_mux.c
//   drive 0   SD card   (sdmm.c)
//   drive 1   ramdisk   (../ramdisk.c)
//   drive 2   USB       (../usbdisk.c)      <- new
```

```c
// ffconf.h
#define FF_VOLUMES  3           // was 2
```

```c
// fs.c
} fs_mounts[] = {
    { "/ram", "1:" },
    { "/usb", "2:" },           // new
};
```

`fs_path_resolve()` then routes `/usb/...` to `2:/...` with no other
change, and every path function in `fs.c` plus `sw/os/fsapi.c` picks it
up for free. `FF_MULTI_PARTITION` is 0, which is correct: FatFs finds
the first FAT partition in the MBR by itself, and that is how a USB
stick is formatted.

### Removal while mounted

A stick pulled mid-write is not a theoretical concern. The disk layer
returns `RES_NOTRDY` once the device is gone and the kernel unmounts
volume 2. FatFs will have lost whatever was buffered — that is
unavoidable without a VBUS switch and an orderly shutdown, and it is
the same exposure the SD card already has. Document it; do not pretend
to solve it.

## CDC

CDC-ACM is a notification interrupt-IN endpoint plus bulk IN/OUT, with
`SET_LINE_CODING` and `SET_CONTROL_LINE_STATE` as control requests.
Once bulk exists for MSC this is a small driver with **no gateware
impact at all** — arguably simpler than MSC, since there is no SCSI
layer.

It fits the existing software: expose it as a serial device that
`sw/apps/serial/serial.c` and `sw/common/zuart.h` already know how to
talk to, alongside the 16550 and the `usb_cdc_uart.v` device side.

**Expectation to set.** CDC-ACM is the standard class and covers
Arduinos, most dev boards, modems, and generally anything that works on
Linux without a vendor driver. But **FTDI, CP210x, CH340 and PL2303 are
vendor-specific protocols, not CDC.** Each needs its own small driver
(mostly vendor control requests to set a baud divisor, then bulk).
"USB serial adapter" is not one thing, and the cheap ones in everybody's
drawer are usually CH340 or FTDI.

## Resource budget

**Measured for phases 0-1, estimated beyond.** The figures below are
yosys 0.33 `synth_ecp5 -abc9` on `rtl/usb/` alone -- the same tool
version `docs/audio.md`'s table was produced with, so they are directly
comparable to it. Reproduce with `make usb_area`.

| Build | LUT4 | CCU2C | TRELLIS_FF | DP16KD |
|---|---|---|---|---|
| **Phase 1, two root ports** | **1231** | 243 | 818 | **1** |
| **Phase 1, one root port** | **1130** | 179 | 754 | **1** |

Against the phase 0 estimate of 1600-1950 LUT4 for the whole core,
the built part came in at 1231 with the SIE, PRE, both port front ends,
the transaction engine, the scheduler, the register file and the buffer
all present. What is not in that number is the phase 2 work -- the HID
compatibility block and the auto-poll table, estimated at ~350 LUT4
together, most of it the cursor arithmetic lifted from `rtl/usb_hid.v`.

So the projection is now:

| | LUT4 |
|---|---|
| Phase 1, measured, two ports | 1231 |
| Phase 2 estimate: compat block + auto-poll | ~350 |
| **Projected total** | **~1580** |
| Removed: 2x `usb_hid_host` + `usb_hid.v` wrappers | ~600 |
| **Net** | **~+980** |

Two notes on the table. The one-port build saves only ~100 LUT4,
because a port front end is mostly timers and the expensive parts --
SIE, transaction engine, buffer -- are shared and stay. `USB_HOST_PORTS`
is therefore a weaker lever than phase 0 assumed, and the answer to
[open decision 1](#open-decisions) should not lean on it. And the carry
count started at 313: the port timers were 32 bits wide and counting to
numbers that fit in 24. Narrowing them cost nothing and returned 70
carry cells.

Against the figures in `docs/audio.md`:

| | Obst (12F) | Lakritz (25F) |
|---|---|---|
| DP16KD now | 52 / 56 | **55 / 56** |
| TRELLIS_COMB now | 79% | — |
| Projected COMB | **~84–85%** | — |

**BRAM is the tighter resource, and this project improves it.** Each
`usb_hid_host` instance carries a 620x4 microcode ROM plus a report
buffer; removing two returns about 2 EBR. The new core spends 1 EBR on
the packet buffer -- **confirmed in phase 1, exactly one DP16KD**, but
see [the no_rw_check trap](#the-trap-that-nearly-ended-the-project)
for how nearly it was not. **Net −1 EBR**, which matters given Lakritz
has exactly one free.

LUTs are the uncomfortable part. `docs/audio.md` is explicit that past
three-quarters "nextpnr's placer starts visibly struggling and timing
turns seed-sensitive", and 84–85% is well inside that. Mitigations, all
of which are defines from day one rather than retrofits:

| Define | Effect |
|---|---|
| `USB_HOST_PORTS` | 1 or 2 root ports |
| `USB_HOST_AUTOPOLL` | Number of slots, or 0 to drop the block |
| `USB_HOST_MSC` | `auto_cont` multi-packet path |
| `USB_HOST_HUB` | PRE support in the SIE |

A one-port, HID-only build should land **smaller than today**, and with
hub support that board loses no user-visible capability.

## Board and electrical notes

### The ports are D+/D−, 22R series, 15K pulldown, and nothing else

That is exactly the full-speed host configuration, so no board change is
needed for this project. But three things follow from "nothing else":

**No VBUS switching.** A wedged device can only be recovered by a USB
bus reset, never a power cycle. Most lockups do respond to reset;
some do not.

**No inrush control.** Hot-plugging a stick dumps an uncontrolled
inrush into the 5 V rail — sticks draw up to 500 mA and carry tens of µF
of bulk capacitance. That is a far larger disturbance than a mouse, and
on a marginal supply it can brown out the board. This is the most
likely real-world failure of the "unplug the mouse, plug in a stick"
scenario, and it is not a gateware problem.

**No overcurrent sense.** `PORTSTAT` bit 5 is hardwired 0 and exists
only so the field is allocated if a future board gains the hardware.

> **Recommendation for the next board revision: a current-limited load
> switch per port with an enable and a fault output.** It is the single
> hardware change that would most improve this feature. The design works
> without one and must continue to.

### Bus-powered hubs

Related, and worth telling users directly. A bus-powered hub budgets
100 mA per downstream port out of a 500 mA upstream allowance it cannot
verify. Mouse + keyboard + stick on a bus-powered hub sits right at the
edge, and the failure mode is the stick browning out mid-write — i.e.
filesystem corruption, not a clean error.

**Document that a self-powered hub is required for the
mouse + keyboard + stick case.** Bus-powered hubs are fine for HID only.

### Edge rates

FPGA LVCMOS33 drivers slew in roughly 2–4 ns. Full speed wants 4–20 ns,
which is fine. **Low speed wants 75–300 ns**, which we miss by an order
of magnitude, driving an unterminated low-speed cable.

`usb_hid_host` already violates this and works in the field on these
boards, so it is empirically acceptable — but it is a real spec
deviation and belongs in writing rather than being rediscovered. Use the
slowest available drive and slew settings on the host pins:
`boards/ulx3s.lpf` already has `DRIVE=4`; add `SLEWRATE=SLOW` on ECP5
where the constraint syntax allows it.

## Phase plan

Hubs come **before** MSC. Two reasons: a hub needs no transfer type
that Phase 2 has not already built, and it is what validates
multi-device addressing and topology. Building MSC first means building
it single-device and re-testing it through a hub afterwards; building
hubs first means MSC is developed through a hub from the start, which
is how it will actually be used.

| Phase | Content | Exit criteria |
|---|---|---|
| **0** | This document | **Done.** Register map and hw/sw line agreed |
| **1** | `usb_sie.v`, `usb_port.v`, `usb_xact.v`, `usb_host.v`, device model TB. FS, **with LS and PRE rate switching designed in** | **Done.** Control transfer completes in simulation; LS-direct and LS-via-PRE both exercised; utilisation measured -- see [Phase 1 results](#phase-1-results) |
| **2** | Software enumeration, HID driver, `usb_hid_compat.v`, **auto-poll and the hardware cursor datapath** | **Gateware done**, software outstanding. Cursor has zero CPU in its path, verified. `wm.c`, `gpu3d.c`, `bios.c` untouched |
| **3** | Second root port, hotplug, simultaneous mixed LS/FS | **Done in simulation.** Hot-swap either port in any order; mixed speeds concurrent; compat blocks and addresses returned on unplug. See [Phase 3 results](#phase-3-results) |
| **4** | Hub class driver, multi-device addressing, port-change endpoint, `tb_usb_hub.v`, **PRE validated against real hardware** | Mouse + keyboard on one hub, LS and FS mixed |
| **5** | Bulk, MSC, SCSI, FatFs drive 2 at `/usb`, `auto_cont` | Mouse + keyboard + stick simultaneously through a hub; copy and checksum both directions |
| **6** | CDC-ACM, scroll wheel, keyboard LEDs via `Set_Report` | |
| **7** | Hardening, error recovery, benchmarks against SD. Optionally USB Ethernet, optionally bus-mastering DMA | |

Each phase updates this document and `docs/user_input.md`.

### Scroll wheel — a note for Phase 6

`reg_usbN_mouse` is `{ 8'h00, mouse_btn, mouse_dy, mouse_dx }` today.
**Bits `[31:24]` are hardwired zero**, so a wheel field lands there with
zero compatibility risk — nothing can be reading a constant.

The register is the easy part. The real work is that many mice report a
usable fourth byte in boot protocol and plenty do not, so proper support
means switching to report protocol and parsing the report descriptor to
find where the wheel's bits actually live. That is a software job and it
is why this is Phase 6 rather than Phase 2.

Open design question for then: delta-per-report (matching `dx`/`dy`
semantics) or a free-running signed accumulator software reads and
subtracts. Leaning accumulator — wheel events are discrete notches and a
dropped notch is more annoying than a dropped pixel of motion.

---

## Phase 1 results

Built: `rtl/usb/usb_sie.v`, `rtl/usb/usb_port.v`, `rtl/usb/usb_xact.v`,
`rtl/usb/usb_host.v`, plus `rtl/tb/tb_usb_device.v` and
`rtl/tb/tb_usb_host.v`. Run with `make test_usb`, or
`make test_usb TRACE=1` for a packet-by-packet trace.

All eight sections pass, including a byte-for-byte comparison of an
18-byte device descriptor fetched as three packets -- two full and one
short -- at each of the three wiring modes.

### A scope correction

Phase 0 promised `rtl/tb/tb_usb_hub.v`. **It is not in this phase, and
that is deliberate rather than an omission.**

What PRE support actually needs testing against is a device that
presents *normal polarity at the low-speed bit rate* and expects a
preamble on every host packet. That is a mode of the device model, not
a separate component, and it is `tb_usb_device.v` MODE 2. Writing a
hub-shaped wrapper around the same behaviour would have added a file
and tested nothing further.

A real hub model -- one that enumerates as a hub, has downstream ports
and a port-change endpoint -- is needed by the hub *driver*, and it
belongs with it in phase 4.

### The trap that nearly ended the project

The packet buffer did not become a block RAM. It became 56,720 LUT4.

Two synchronous read/write ports over one array is exactly the shape a
DP16KD has, and yosys declines to use one anyway -- silently. No error,
no warning: it maps 2 KB into flip-flops and the design goes from
~1200 LUT4 to over 56,000. On a 12F that is not a regression, it is
the end of the project.

The reason is a genuine semantic gap. Verilog array semantics *define*
what port B reads when port A writes the same address in the same
cycle -- both see the old value, because the assignments are
non-blocking. The hardware defines nothing there. Rather than emit a
block RAM that might disagree with the simulation, yosys refuses.

The fix is `(* no_rw_check *)` on the array, which says the design does
not depend on that case. Here it does not, **but only because of a
contract with software, so the contract is now explicit:**

> **Software must not touch the packet buffer while a transaction is
> running.** Poll XACT_S until the pending bit clears first.

That was already the only sensible way to use the block. It is now
load-bearing, and a driver that ignores it gets undefined bytes rather
than a stale read.

Worth generalising: **check the DP16KD count after any change to a
memory in this project**, because the failure mode is silent and the
cost is four orders of magnitude.

### Register map change: the pending bit

XACT_S bit 16 was specified as `busy`, meaning the transaction engine
is running. That is not usable.

Writing the start bit sets an internal request; the scheduler picks it
up a cycle later; the engine raises its own busy a cycle after that.
Reporting only the engine's busy leaves a three-cycle window in which a
transaction has been accepted and nothing says so -- and the natural
driver loop is "write start, then poll until not busy", which lands
squarely in it. It reads the *previous* transaction's status as if it
were this one's, and fires the next request on top of one already
running.

Bit 16 now means **a transaction is outstanding**, covering the write,
the scheduler handoff and the engine. The three terms are contiguous by
construction. No software-visible layout change; the meaning is wider.

### Traps found in the RTL, for whoever touches it next

Each of these produced a failure that pointed somewhere other than its
cause, which is the only reason they are worth writing down.

**The last bit of every packet was overwritten by the EOP.** A bit's
line state is established by the tick that computes it and lasts until
the next tick, so asserting SE0 on the same edge that emitted the final
CRC bit meant that bit never reached the wire. Every packet went out
exactly one bit short; the receiver's last byte came up seven bits long
and was discarded as a partial. What arrived looked like a packet whose
final CRC byte had simply gone missing -- confirmed numerically when
the device received `... 12 00 e0` where the real CRC is `e0 f4`. SE0
is now asserted on the first tick *inside* the EOP state.

**A one-delta glitch on the line drivers.** Ending an EOP clears
`tx_se0` and sets `tx_j` on the same edge. Driving the pins
combinationally from that pair lets it be read with one updated and one
not, so the line passes through K on its way from SE0 to J. In hardware
that is a runt pulse of a few hundred picoseconds; in simulation it is
a zero-width K that a receiver hunting for a packet start latches onto,
and then decodes the following idle as data -- an idle line has no
transitions, so NRZI reads it as a run of ones. The drivers are
registered now, which is what they should have been regardless.

**The start bit was detected for two clocks.** `wb_cyc`/`wb_stb` stay
asserted for a whole bus cycle. The scheduler cleared the request on
the second clock and the detect, being later in the same block, set it
straight back -- so every transaction ran exactly twice, and the second
overwrote the first's result just as the driver went to read it. The
detect is now gated on `!wb_ack_o`.

**Stuff bits were fed to the receive CRC but not the transmit CRC.**
Stuffing happens after the CRC is computed and is undone before it is
checked, so the residue is taken over the unstuffed stream. Feeding the
stuffed bit on receive makes every packet containing a run of six ones
fail its CRC and leaves every packet that does not pass -- which reads
as an intermittent fault rather than a systematic one. It presented as
"the 18-byte descriptor works for 16 bytes and then fails".

**The host transmitted into a driver that was still on.** A transaction
ends when the SIE samples SE0, about a bit and a half into the other
end's EOP; the device keeps driving for roughly 2.5 bit times beyond
that. At full speed the software round trip hid it. At low speed a bit
is 667 ns, software is no slower, and the collision is reliable: the
device decodes a mangled token, answers nothing, and the host reports a
timeout that looks exactly like a dead device. There is now an explicit
wait for the bus to be idle before every transaction.

**The receive buffer address was off by one.** `buf_we` and `buf_wdat`
are registered, so the write lands a cycle after the byte arrives -- by
which time the byte counter has already incremented. A combinational
address put every byte one position too high and left a hole at the
start. The write address is latched alongside the write enable.

### Simulation timing

`usb_port.v` takes every interval as a parameter in clock cycles so the
testbench can run the same RTL with the 100 ms attach debounce
shortened to microseconds. Simulating a real attach at 48 MHz is 4.8
million cycles per plug event, which turns the suite into a coffee
break for no added coverage.

`usb_host.v` gained a separate `T_FRAME` for the frame timer, identical
to `T_MS` on hardware. They must not be shortened together: frames
arriving every two microseconds leave the engine permanently busy
emitting SOF and starve every transaction under test.

---

## Phase 2 results

Built: `rtl/usb/usb_hid_compat.v`, the auto-poll table and scheduler in
`usb_host.v`, and `sw/os/usb/usbh_hw.h`. Test section 9 covers the
cursor path end to end.

### The exit criterion that mattered

> A boot-mouse report moves `curs_x`/`curs_y` with the CPU doing
> nothing but the one-time setup.

Verified. The test writes `typ`, one poll descriptor, and `poll_en`,
and then never touches the controller again. What it then checks:

- an idle mouse NAKs every poll and **nothing moves and nothing is
  reported** -- a NAK is the device saying there is no news, and
  treating it as a report would interrupt the CPU a hundred times a
  second and re-deliver a keyboard's last report forever;
- a delta of +10/+6 moves the cursor 20/12, so the acceleration
  doubling above the threshold of 3 is intact;
- a delta of +2 moves the cursor 2, so fine positioning is still 1:1;
- a long sweep left saturates at 0 rather than wrapping.

The arithmetic producing those numbers is lifted verbatim from
`rtl/usb_hid.v`, which is Lone Dynamics code and so reusable. That was
deliberate: the acceleration curve, the `SENS_SHIFT` divide with its
carried remainder and the saturation are exactly the parts that must
stay bit-compatible, and re-deriving them would be the one sure way to
produce a pointer that "feels different" and cannot be diffed against
anything.

All of the clock-domain crossing in that file is dropped. It exists
because `usb_hid_host` runs at 12 MHz; this core is 48 MHz throughout,
so `report_edge` -- which exists to turn a foreign-domain level into a
local pulse -- would be actively wrong here, since the report arrives
as a local pulse already.

### typ is now written, and the info register with it

`reg_usbN_info` was read-only. It is now writable, and writing bits
[25:24] is how the kernel assigns the device type; every other bit of
the write is ignored. Nothing infers a device type any more.

### Utilisation

| Build | LUT4 | CCU2C | TRELLIS_FF | DP16KD |
|---|---|---|---|---|
| Phase 1, two ports | 1231 | 243 | 818 | 1 |
| **Phase 2, two ports** | **2129** | 355 | 1403 | **1** |
| Phase 2, one port | 2010 | 291 | 1339 | 1 |

**Phase 2 cost 898 LUT4 against an estimate of ~350.** That is the one
number in this project that came in materially worse than projected,
and it is worth being precise about where it went, because the estimate
was not merely low -- it was counting the wrong thing.

The estimate assumed one cursor datapath. There are two, because there
are two compat blocks, and each carries its own acceleration,
`SENS_SHIFT` divide, remainder carry and saturation -- 12-bit signed
arithmetic, duplicated. That mirrors what ships today (two
`usb_hid_wb` instances, each with a cursor, and `rtl/sysctl.v` picks
one), so it is a faithful replacement rather than a regression. But
**only one cursor sprite exists**, so one of the two datapaths can
never drive anything.

Sharing a single cursor datapath between the two compat blocks should
return something in the region of 200-300 LUT4. It is not free --
something has to arbitrate which block owns the pointer, and the honest
answer is "whichever one is `typ == 2`", which is what `wm.c` already
decides in software. Deferred to phase 7 rather than done now, because
it changes behaviour on a board where both ports somehow present as
mice and that wants its own test.

Revised projection:

| | LUT4 |
|---|---|
| Phase 2, measured, two ports | 2129 |
| Removed: 2x `usb_hid_host` + `usb_hid.v` wrappers | ~600 |
| **Net** | **~+1529** |
| Net with a shared cursor datapath (phase 7) | ~+1250 |

Against `docs/audio.md`'s 79% TRELLIS_COMB on Obst that is
uncomfortable and the one-port build saves almost nothing, so
`USB_HOST_PORTS` remains a weak lever. If the 12F gets tight, the
shared cursor datapath is the first thing to do and `USB_HOST_AUTOPOLL`
with fewer slots is the second.

### The no_rw_check trap, a second time

It happened again, to me, within an hour of documenting it. An edit
inserted the auto-poll table declarations between the `(* no_rw_check *)`
attribute and the `pbuf` array it applied to. The attribute silently
attached to the poll table instead, the packet buffer went back to
LUTs, and the design jumped from 2129 to 58308 LUT4 with no warning
of any kind.

The lesson from phase 1 stands and is now load-bearing rather than
theoretical: **check the DP16KD count after any edit near a memory.**
`make usb_area` prints it.

### Software

`sw/os/usb/usbh_hw.h` is the register contract. `usbh.c` is the core --
port state, control transfers, enumeration, address allocation.
`usbh_hid.c` walks a configuration descriptor for a boot keyboard or
mouse and programs an auto-poll slot.

Built with the toolchain `sw/common/arch.mk` names by default: xPack
`riscv-none-elf-gcc` 15.2.0-1, newlib, `rv32im`/`ilp32`.

| | kernel.bin | free of 262144 |
|---|---|---|
| Before | 212544 | 49600 |
| **With the USB stack** | **215264** | **46880** |

**2720 bytes**, linked and reachable rather than garbage-collected --
`z_usbh_init()` is called from `kernel.c` and `z_usbh_poll()` from both
the IRQ 9 dispatch and the ktimer. That matters against a 256 KB
ceiling the BIOS copies unconditionally.

Nothing in it blocks. Enumeration spans hundreds of milliseconds and
`z_usbh_poll()` does at most one thing per call and returns; it is
advanced by IRQ 9 when a transaction completes, and by the ~732 Hz
ktimer so that a device sitting in a timed state -- waiting out the
2 ms a device is allowed to take to adopt a new address -- still moves
along with no interrupt to prompt it. A stack that blocked would
deadlock against `k_fs_enter()`, which is the same argument that put it
in the kernel in the first place.

### Two integration facts that were not as documented

**`LATCHED_IRQ` appears twice, not once.** Phase 0 said to clear bit 9
in `rtl/sysctl.v`'s mask. There are two instantiations -- `zeitlos32_wb`
and the picorv32 one, selected by `CPU_ZEITLOS32` -- with identical
masks. Changing one gives a machine whose interrupt behaviour depends
on which core was built. Both are changed.

**`Z_FEATURE2_USB_HOST` did not exist yet.** Phase 0 allocated bit 4
and nothing had added it. It is now in `rtl/csrs.vh` and
`sw/common/zsoc.h`, with the distinction from `Z_FEATURE_USB_HID`
written down in both: that bit means "there is a HID port", which stays
true; this one means "there is a controller you can drive yourself",
which is what a mass storage or CDC driver needs.

### SoC integration, and a decode problem that dissolved

`rtl/sysctl.v` instantiates the controller under `` `USB_HOST ``, which
is mutually exclusive with `` `USB_HID `` and overrides it:

```
make BOARD=lakritz EXTRA_DEFINES=-DUSB_HOST ...
```

so a board can be built against the new core without editing
`rtl/boards.vh` for every board at once.

**Both cores stay supported.** `` `USB_HID `` is not a migration step:
`rtl/usb_hid.v` and `rtl/ext/usb_hid_host` remain in `RTL_PICO` and
remain the default for every board. A board opts in by defining
`` `USB_HOST ``, and opts back out by not doing so. That is not only
taste -- the new core is larger, and on a part where it does not fit,
the old one working beats the new one not building.

`CSR_FEATURES` bit 13 (`Z_FEATURE_USB_HID`) is set for **either** core,
because the question it answers -- is there a HID port here -- has the
same answer both ways. That needed fixing: `sysctl.v` `` `undef ``s
`USB_HID` before `csrs.vh` is included, so testing it alone reported no
HID port on exactly the boards that have the better one, and
`sw/apps/info` would have shown USBHID absent while a mouse moved the
cursor.

**Phase 0 called for narrowing the `cs_usb0`/`cs_usb1` decodes before
adding anything to the 0xC nibble. That turned out to be unnecessary.**
Those two decodes mask only the top nibble and bit 5, so between them
they claim all 256 MB -- which is harmless with exactly two slaves in
there and immediately wrong with a third. But `usb_host.v` is a
*single* slave that owns the whole nibble and sub-decodes internally,
so there is no third slave to collide with. The problem dissolved
rather than being solved.

What makes that safe is the module's internal catch-all, which is
therefore not optional: an unacked address hangs `picorv32_wb` forever,
so a stale pointer into this nibble has to land on a zero read rather
than a dead machine.

Also wired: `cpu_irq[9]`, and `curs_x0`/`curs_y0` into the existing
`gpu_curs_x` mux, which already picked whichever block reports
`typ == 2` and needed no change beyond seeing the new signals.

### Whole-SoC utilisation, measured

Old core versus new, same board, same tool:

| Lakritz (25F) | LUT4 | CCU2C | TRELLIS_FF | DP16KD |
|---|---|---|---|---|
| `USB_HID` | 15035 | 2185 | 9079 | **55** |
| `USB_HOST` | 15589 | 2365 | 9748 | **54** |
| delta | **+554** | +180 | +669 | **-1** |

| Obst (12F) | LUT4 | CCU2C | TRELLIS_FF | DP16KD |
|---|---|---|---|---|
| `USB_HID` | 14188 | 2081 | 8404 | **52** |
| `USB_HOST` | 16036 | 2281 | 9072 | **51** |
| delta | **+1848** | +200 | +668 | **-1** |

**The BRAM prediction held exactly: net -1 EBR on both.** Lakritz goes
from one free block RAM to two, which was the tightest constraint in
the whole project.

The LUT4 delta is the awkward part, because the two boards disagree by
a factor of three about what the same logic costs. The flop delta is
+669 and +668 -- identical, as it must be, since it is the same RTL --
so the difference is entirely in mapping. It is not mapper *noise*
either: re-measuring Obst with the legacy mapper instead of `-abc9`
gives +1938 against +1848, so both mappers agree that on Obst this
costs about 1900 LUT4 and on Lakritz about 550.

That inverts the two boards -- with `USB_HOST`, Obst maps LARGER than
Lakritz despite having fewer features -- which is the signature
`docs/audio.md` describes of this SoC behaving badly near its packing
limits rather than of a design difference.

**Plan against the Obst number.** The mitigation is the one phase 2
already identified: two compat blocks carry two full cursor datapaths
and only one cursor sprite exists, so sharing it should return
200-300 LUT4. That is now worth doing rather than deferring, and it is
the first thing to try if Obst does not fit.

Note also that the old core was far more expensive than phase 0
assumed. Standalone the new controller is 2129 LUT4 and the Lakritz
delta is +554, so `usb_hid_host` x2 plus the `usb_hid.v` wrappers were
worth roughly 1575 LUT4, not the ~600 estimated.

### What phase 2 still owes

- Nothing has been tested against the RTL. The software and the
  gateware have each been exercised separately and have never met.

### A scheduler bug worth recording

Both `sched_is_sof` and `sched_is_poll` are now set on *every*
scheduler branch, not just the one each belongs to. They decide where a
completed transaction's result goes, and a stale one sends it somewhere
it does not belong: leaving `sched_is_poll` set through a SOF meant the
SOF completed with status OK and was delivered to the compat block as a
mouse report assembled from whatever bytes were last captured. The
cursor went to X in simulation; on hardware it would have been a
pointer that jumped somewhere arbitrary once per frame.

---

## Phase 3 results

Hot plug and unplug, the second root port, and both speeds running at
once. Also closes the `SET_PROTOCOL`/`SET_IDLE` item phase 2 left open.

Every case is in `make test_usb_cosim`, driven by the real driver,
because all of them are sequencing across the software/gateware
boundary and none can be tested from either side alone.

| Case | Checked |
|---|---|
| Hot unplug | `typ` cleared, poll slot stopped |
| Replug, same port | Re-enumerates, cursor works again |
| Second port, low speed, simultaneously | Both enumerate, distinct addresses, both cursors move |
| Unplug one port | The other is untouched and keeps moving |
| Three plug cycles | Still binds |

### Boot protocol is now set explicitly

`SET_PROTOCOL(boot)` and `SET_IDLE(0)` are issued to the HID interface
before the poll slot starts. A device whose interface declares subclass
1 usually defaults to boot protocol, and "usually" was the problem: a
keyboard that comes up in report protocol sends a layout the hardware's
byte mux is not expecting, and the symptom is keycodes in the wrong
bytes rather than nothing working. `SET_IDLE(0)` stops a keyboard
re-sending its state on a timer, which the auto-poll slot would
otherwise deliver as repeated keypresses.

Both are optional: a STALL is not fatal, because plenty of devices
refuse one or both and are in boot protocol anyway.

### The compat-block leak

`blk_claim()` is a pool of two and was never released. Nothing gave a
block back on unplug, so the third plug cycle found the pool empty and
the device enumerated perfectly and then silently did not appear.

That shape is worth remembering: **not a failure on the first replug, a
failure on the third.** Anyone testing by hand would plug and unplug
once, see it work, and move on. The test runs three cycles for exactly
that reason.

Releasing it now lives in `dev_reset_state()` alongside the address
free, so the two cannot drift apart.

### The keepalive fired mid-packet

A low-speed keepalive is an EOP driven by `usb_port.v`, and the pin mux
gives a port's own drive priority over the SIE. It was derived from the
frame timer alone -- so it landed in the middle of whatever packet the
SIE was sending, truncating the host's own data packet with two bit
times of SE0 it did not ask for.

Invisible with one device attached: the only traffic on a low-speed
port is transactions to that port, and the frame boundary rarely fell
inside one. It appears the moment a second device is present, because a
control transfer at 1.5 Mbps is hundreds of microseconds wide and a
1 ms frame boundary lands inside it often. The symptom was a low-speed
device that enumerated, got an address, then failed partway through its
configuration descriptor -- which reads like a marginal device rather
than a scheduler bug.

The SOF path already had this right: the scheduler only issues one when
the engine is idle. The keepalive now follows the same rule.

### Transactions abort when the device goes away

`usb_xact.v` takes a `port_gone` input and finishes with `ST_ABORT`
rather than sitting out the response timeout. At low speed that timeout
is 1400 clocks of a bus the *other* port could be using, and every
in-flight packet is addressed to something no longer there.

### Utilisation after phase 3

| | LUT4 | TRELLIS_FF | DP16KD |
|---|---|---|---|
| Lakritz `USB_HID` | 15035 | 9079 | 55 |
| Lakritz `USB_HOST` | 15680 | 9748 | **54** |
| Obst `USB_HID` | 14188 | 8404 | 52 |
| Obst `USB_HOST` | 15548 | 9072 | **51** |

Obst improved from 16036 to 15548 across phase 3 *despite gaining
logic*, and now maps smaller than Lakritz as it should. More evidence
that the phase 2 Obst figure was a mapping artifact rather than a
design cost: the LUT4 delta on this SoC is not a stable quantity, and
the flop delta -- +669 on both boards, unchanged -- is the number to
trust.

Kernel with the stack linked: **215776 bytes**, 46368 free of 262144.

---

## Hardware bring-up

First silicon was mozart_ml1 (ECP5 45F). Everything below was found on
the board, and none of it was reachable from simulation. Recorded in
the order the failures were peeled apart, because several of them
masked each other.

### PULLMODE on the USB pins

`boards/*.lpf` set only `IO_TYPE=LVCMOS33`, and the ECP5 default is
`PULLMODE=UP`. Its internal pull-up is roughly 10-14 kOhm, comparable
to the board's 15 kOhm pull-downs, so an empty port read as a device
being present and the speed detect was meaningless.

`usb_hid_host` never noticed because it never reads the idle line
state -- it has no attach debounce and no speed detection. Ours is the
first thing on this board that cared what the lines do between
packets. `PULLMODE=NONE` on all four pins.

### Suspend during reset recovery -- the big one

A low-speed device suspends after **3 ms** without bus activity.
`p_ka` was gated on `p_enabled`, which `usb_port.v` does not set until
reset recovery *ends* -- so after every port reset the bus sat idle
for 10 ms with no keepalives and the device fell asleep before the
first SETUP.

The symptom was a device that answered nothing at all, and an optical
mouse whose LED stayed dark. It also explains why raising
`RECOVERY_MS` made things *worse*, which had looked like evidence
against every timing theory.

Keepalives now continue through `P_RECOVERY`; `P_KA` gained its own
timer and a return state so a keepalive taken during recovery resumes
recovery rather than cutting it short.

### The receive path had no timeout

`X_HS_RX` and `X_IN_RX` waited on `sie_rx_done` forever. Anything that
asserts `rx_active` without a real packet behind it -- and real
hardware does, at every bus handover -- wedged the engine with
`x_busy` stuck high. Both states now count against `rx_limit`.

### Auto-continue versus software NAK retry

The control data stage used hardware auto-continue while the driver
re-issued the whole stage on a NAK. The two accountings came apart: a
59-byte descriptor came back with exactly one 8-byte packet missing
and everything after it shifted 8 bytes early.

The data stage is now paced one packet at a time in software, with an
explicit offset, length and toggle. Auto-continue remains in the
hardware and is still used by the auto-poll slots, where a transfer is
one packet anyway.

### Two devices, one of everything

Three separate bugs, all invisible with a single device plugged in:

- **The descriptor walk** used one running `proto` variable. A
  keyboard has a second interface for its media keys, which cleared it
  *after* the endpoint had matched, so a boot keyboard bound as a
  mouse. A single-interface mouse never showed it.
- **The packet buffer** had one `OFF_SETUP`/`OFF_CTRL` region shared by
  both ports. Concurrent enumerations overwrote each other's
  descriptors. Each port now has 256 bytes of its own from 0x400,
  which also fixes a latent overflow -- the shared region had 64 bytes
  before the mass-storage block and a config read can ask for 200.
- **The control transfer engine itself** is a single shared instance:
  `ctrl_state`, `ctrl_dev`, `ctrl_dir_in`, `ctrl_len`, `ctrl_xferred`,
  `ctrl_tgl`. Both ports drove it at once, each judging the other's
  results with the wrong direction and length, producing a STALL on
  the data stage and babble on the status stage.

Enumeration is currently **serialised** by `enum_busy()` as the small
fix. The proper fix is moving the control state into `z_usbh_dev_t` so
transfers can overlap; that matters for hubs, where several devices
behind one enumerating strictly one at a time gets slow. See
[Phase 4](#phases).

### Parsing must use a saved copy

`bind` originally walked the live packet buffer, but `SET_PROTOCOL`
and `SET_IDLE` run between the configuration read and the bind, and
every control transfer reuses the same scratch. The descriptor was
gone by the time the walk ran. `lsusb` printed it correctly from its
own copy while the walk found nothing -- same bytes, two sources,
different answers. Descriptors are now parsed from `cfg_raw`.

### What the harness could not see

Every bug above needed something the device model does not do: a real
pull-up holding the idle line, a device that suspends, a device that
NAKs mid-descriptor, a second interface, two devices enumerating at
the same instant, and a bus handover with real skew (real devices emit
a one-sample SE1 at every transition; a model driving both lines from
one register never does).

The highest-value harness change is attaching **both** devices at t=0
in `tb_usb_cosim.v` so concurrent enumeration is the default case.
Hubs make that the normal situation rather than an edge case.

### Still open

`rx started` runs far ahead of `good` -- roughly 3400 against 63 in a
typical run. Those are receptions that begin at a bus handover and
never complete. They are not blocking anything now that the engine
cannot wedge, but they burn bus time and will matter for mass storage.
Two attempts to blank the receiver across the handover both regressed
the board and were reverted; it is worth another pass now that there
is a working baseline to measure against.

## Co-simulation

`make test_usb_cosim` runs the **real driver against the real
gateware**. `rtl/tb/cosim/usbh_vpi.c` joins them: the driver runs in a
thread, its register accessors post a bus request and block, and the
Verilog side runs an actual Wishbone cycle against the DUT for each
one. Simulation time is frozen while the driver computes, which is
correct -- that is code the CPU would have run between bus cycles, and
no CPU is modelled here.

The driver is compiled with `-DZ_USBH_COSIM`. That is the only
difference from the kernel build: the state machine, the descriptor
walk and the register layouts are the same source the kernel links.
The flag selects function-call register accessors instead of pointer
dereferences; in a kernel build they are `static inline` and generate
the identical instruction.

It enumerates a device end to end -- debounce, reset,
GET_DESCRIPTOR(8), SET_ADDRESS, GET_DESCRIPTOR(18), the configuration
descriptor, SET_CONFIGURATION, the HID bind -- and then checks that a
report moves the hardware cursor with the driver no longer running.

### What it found in its first four runs

Everything below had passed both halves' own tests. That is the point:
each side was correct against the thing it was tested with, and the
bugs lived in the space between.

**The port reset bit was not self-clearing.** Phase 0 specified "write
1: drive SE0 for 10 ms, then self-clear" and the RTL simply stored the
bit. The driver's natural sequence is "assert reset, wait for the port
to report enabled" -- and `usb_port.v` re-enters reset from `P_ENABLED`
whenever `ctl_reset` is high, so the port cycled between reset and
recovery forever while the driver waited for an enabled that could
never arrive. Neither side is wrong alone: the RTL testbench pulsed the
bit by hand, and the driver had never been run.

**The transaction result registers came out of reset undefined.**
`res_status`, `res_len`, `res_tgl` and `res_naks` were only ever
assigned on completion, so `XACT_S` read as X before the first
transaction -- and "write start, poll until not pending" reads it
immediately. In simulation the pending bit read back as a zero that was
not a zero. On silicon it would have been a stable random value, which
is worse, because it would have looked like it worked.

**The device model applied SET_ADDRESS in the wrong status stage.** A
control transfer with no data stage has an **IN** status stage, not an
OUT one. The model only handled the OUT case, because until a real
enumeration drove it the only control transfer it had ever seen was
GET_DESCRIPTOR. The host moved to the new address and the device kept
answering on zero.

**The device model had no configuration descriptor.** It answered every
GET_DESCRIPTOR with the device descriptor regardless of the type asked
for, so the configuration request returned 18 bytes of the wrong
structure and the HID driver never bound. It now carries a proper
config/interface/HID/endpoint chain -- including the HID descriptor,
which is there specifically so the driver's descriptor walk has to step
over something it does not recognise.

The first two are gateware defects that would have reached a board.

## Testing

Hardware this fiddly is not debugged on a board. Simulation carries the
weight, following the pattern `rtl/gpu/bench` and `rtl/tb` already
establish.

**`tb_usb_device.v`** — a behavioural device: responds to SETUP, returns
a configurable descriptor set, NAKs on demand, STALLs on demand, can be
switched between FS and LS, can inject CRC errors and can go away
mid-transaction.

**`tb_usb_hub.v`** — deferred to Phase 4 with the hub driver. What PRE
needs testing against is a device presenting normal polarity at the
low-speed bit rate, and that is a *mode* of the device model rather
than a separate component; see
[the scope correction](#a-scope-correction).

Cases that must exist because they are the ones that bite. Ticked ones
are in the phase 1 suite:

- [x] LS direct attach (inverted polarity) and LS behind a hub (normal
  polarity) in the same run. These two differing is the single most
  likely silent bug in the whole project.
- [x] DATA0/1 toggle preserved across a NAK retry.
- [x] Short packet terminating an `auto_cont` bulk IN.
- [x] STALL reported as STALL rather than as a timeout.
- [x] SOF generated, and a transaction still completing around it.
- [ ] Device disconnect in the middle of every transaction phase.
      (phase 3)
- [ ] Two devices requesting address 0 in the same frame. (phase 4)
- [ ] A transaction that would cross a frame boundary. (phase 3)

Plus `tools/hwmap/hwmap --check` after the `sysctl.v` changes — the
decode narrowing in [Register map](#register-map) is exactly the kind of
thing it catches, and `--diff` should show the new windows and nothing
else.

## Open decisions

Both deferred from review, both affecting this document when settled.

**1. `USB_HOST_PORTS 1` on the 12F boards?** With hubs, one root port
loses no functionality and buys back the area that separates ~85% from
something comfortable. The cost is that Obst users need a hub for two
HID devices where today they use two sockets.

**2. exFAT.** `ffconf.h` has `FF_FS_EXFAT 0` and `FF_USE_LFN 0`. A
factory-formatted 64 GB stick is exFAT and **will not mount**. A FAT32
stick mounts but shows 8.3 aliases rather than long file names. Both are
code-size decisions independent of everything above, but the first is
the one most likely to produce a bad first impression, and it needs
settling before the MSC section is implemented in Phase 5.

## Considered and declined: filesystem and USB as core apps

Raised in review and **decided against**. Recorded here because the
reasoning is worth keeping, and because there is a condition under
which it should be revisited.

The reason USB lands in the kernel today is not a preference, it is
that the dependency points inward. `k_fs_enter()` in `sw/os/kernel.c`
increments `k_no_preempt`, so a FatFs call runs with the scheduler
disabled -- and `disk_read()` is a plain synchronous call FatFs makes
from inside that region. A `/usb` volume served by a userspace process
could not be scheduled to answer it. HID has the same shape: the key
ring is kernel state behind `k_hid_read_key()`.

So the two are coupled: USB cannot move while FatFs stays, and the
decision that would unlock it is **moving FatFs out of the kernel** --
a filesystem-architecture change touching `fs.c`, `fsapi.c` and
`cfg.c`.

Why it was declined:

- **The prize was already collected elsewhere.** The strongest argument
  for a userspace USB stack was latency isolation for the pointer, and
  phase 2 put the cursor in gateware. What remains to move --
  enumeration, hubs, mass storage -- is not latency-critical.
- **The ramdisk would get slower.** The IPC boundary lands at the
  file-operation level, not the sector level, and with no MMU there is
  no copy tax, so two context switches per `f_read` are noise against a
  bit-banged SPI card or a 12 Mbps bus. Against `/ram`, where
  `rd_disk_read` is a `memcpy`, they are not. The fastest volume would
  become the one that suffers most.
- **The size win is modest.** Measured: `ff.o` 14664, `fs.o` 5196,
  `sdmm.o` 3304, `fsapi.o` 3562, plus the ramdisk and mux -- 27672
  bytes of a 215264-byte kernel, about 13%. Real, but it comes back as
  an app that still needs RAM.
- **It would add a permanent "core app" tier** -- apps loaded by a
  different mechanism, unkillable, required before anything else.
  `zar` already gives the kernel a flash loader that does not go
  through FatFs, so the bootstrap has somewhere to live, but the
  concept is new and would attract tenants.
- **The isolation argument is weaker than it looks.** A bug in a
  userspace app holding the only filesystem is not much better than a
  kernel bug; you still cannot read files.

And the framing that settled it: the current architecture is not
impure. `net` is an app because **nothing in the kernel consumes a
packet**. The rule is not "one subsystem, one app", it is "no kernel
consumer, no kernel residency" -- and FS and HID have kernel consumers.
The refactor would swap one principle for another, not add one.

**The condition to revisit** is the 256 KB ceiling `sw/bios/bios.c`
copies unconditionally. The kernel is at 215264 with 46880 free, and
MSC plus SCSI plus a hub driver is realistically 8-12 KB more. If that
headroom gets uncomfortable, moving FatFs out buys 27 KB and the
argument stops being about aesthetics.

**The free subset is still worth taking.** CDC-ACM and USB ethernet
have no kernel consumer, so in phase 6/7 they can be apps over a
"run one transfer" syscall -- which the register interface already
nearly is -- with none of the above applying.

## Deliberately not done

| | Why |
|---|---|
| High speed (480 Mbps) | Needs a real PHY and a differential receiver. Not possible on 22R-and-pulldowns wiring at any clock rate |
| Transaction translators | Meaningless for a full-speed host — see [Ports and hubs](#ports-hubs-and-topology) |
| Hub depth beyond 3 | Spec allows 5; nothing real needs it and the tables cost RAM |
| USB OTG / device mode | `rtl/ext/usb_cdc` on the USB-C socket already covers device mode |
| Isochronous transfers | Audio and video over USB. No use case here, and the bandwidth reservation logic is substantial |
| Runtime suspend/resume | Power saving on a board with no VBUS control saves nothing measurable |
| Bus-mastering DMA | Revisit in Phase 7 with measurements. See [the packet buffer](#the-packet-buffer) |
