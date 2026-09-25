# Zeitlos USB Host Controller

**STATUS: phases 0-5 working on hardware; hub support, including
low-speed devices behind a hub, reliable since round 27 (cause of the
earlier intermittent failures not identified -- Known issues, item 5). Since round 19 a low-speed
keyboard, a low-speed mouse, a mass storage stick and a CDC-ACM device
all work behind one hub (05e3:0608) at the same time. Apps reach CDC
through `serial`. The bring-up tools (`usbcap`, `usbtune`, `usbbench`
and others) are in [debug builds](#debug-build) only. The road
there, and what is still open, is in
[Hardware: working behind the hub](#hardware-working-behind-the-hub) and
[Known issues](#known-issues).**

Measured on mozart_ml1 (ECP5 45F):

| | |
|---|---|
| Low-speed keyboard and mouse, both ports at once | enumerate, bind, auto-poll slots live |
| Full-speed CDC | enumerates |
| Full-speed mass storage -- stick and card reader | enumerates, `class=msc` |
| USB hub (05e3:0608) | keyboard, mouse, stick and CDC device behind it at once (round 19) |
| `usbmount` then `ls /usb` | lists a real FAT filesystem over USB |
| FatFs file reads over `/usb` | a text file and an 11 KB PNG, intact |
| FatFs WRITE then read-back over `/usb` | intact |
| Receive errors | **zero** at both speeds |
| Whole-SoC Fmax | 56.50 MHz against a 48 MHz clock (after the Ethernet MAC BRAM fix; see [Fmax after these changes](#fmax-after-these-changes)) |

Phase 4's hub class driver (`usbh_hub.c`) passes `make test_usb_hub`,
and on hardware a stick and a low-speed mouse work behind a real hub --
which also confirms the round-7 PRE fix. A low-speed keyboard behind
the hub failed; the cause and fix are in
[Hardware: retries behind a hub](#hardware-retries-behind-a-hub).

Phase 5's open items are in [Also outstanding](#also-outstanding);
unplug handling, STALL / Reset Recovery and the IN toggle check are
done in simulation and not yet exercised on hardware.

The controller is instantiated in `rtl/sysctl.v`, and
`make test_usb_cosim` runs the real driver against the real gateware.

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

This replaces `rtl/ext/usb_hid_host` with a Zeitlos-native, dual-port,
full-speed USB host controller supporting HID, hubs, mass storage and
CDC.

It began as the Phase 0 design document -- the thing to argue with
before any RTL existed -- and the register map is still a contract
with `sw/os/hid.c`, `sw/apps/wm/wm.c` and every class driver at once.
The design sections below are unchanged from that; the results and
bring-up sections were written afterwards, against hardware, and
several of them record where the design's assumptions turned out to be
wrong.

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
- [Mass storage status](#mass-storage-status)
- [CDC](#cdc)
- [USB serial devices](#usb-serial-devices)
- [USB ethernet](#usb-ethernet)
- [Resource budget](#resource-budget)
- [Debug build](#debug-build)
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
rtl/tb/tb_usb_hub.v        behavioural hub model -- became HUB=1 mode of
                           tb_usb_device.v; see "Hub class driver"
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
  full speed          low speed (32 clk/bit)
  SYNC | PRE PID | J x4 | SYNC | TOKEN ... | EOP        host
                                     device reply (no PRE) | EOP
  SYNC | PRE PID | J x4 | SYNC | handshake or DATA | EOP  host
```

**Every low-speed packet the host sends gets its own PRE**: the token,
the data of an OUT or SETUP, and the host's handshake after IN data.
The host sends SYNC and the PRE PID at full speed, holds the bus idle
(J) for the hub setup interval -- 4 full-speed bit times, the spec
minimum, adjustable with `usbtune` in a [debug build](#debug-build) --
then transmits the packet at the
low-speed rate. The device's replies come back at the low-speed rate
with no PRE. The hub enables its low-speed ports for each PRE'd packet
and disables them again at its EOP.

**At most one PRE'd transaction per frame** (round 26): the scheduler
starts no second one until the next frame tick, and a PRE'd request
never re-issues within a frame -- the next auto-continue packet or a
retry ends the request with its progress, and software resumes it in a
later frame. This is how Pico-PIO-USB, TinyUSB's software full-speed
host, schedules low-speed devices behind a hub (one transaction per
endpoint per frame), and the limit ESP-IDF and TinyUSB's DWC2 fix hit.
It did not by itself end the hub failures of rounds 20-27 (see Known
issues, item 5), but it is the reference hosts' discipline and stays.

This means **the SIE must change bit rate mid-transaction.** That is
about 100 LUT4 on top of the divisor we already need — but it is not
something you retrofit into a finished serialiser cleanly, because the
rate change has to be sequenced against the shift register, the stuffing
counter and the DPLL reset all at once.

> **PRE is designed into Phase 1 even though it cannot be tested against
> real hardware until Phase 4.** The behavioural hub model in
> `tb_usb_hub.v` exists to cover it in the meantime.
>
> It turned out the model that covered it was not enough: PRE went out
> with a wrong PID check field from phase 1 until the hub bench found it
> in phase 4. See [Two core bugs the hub found](#two-core-bugs-the-hub-found).

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
| Hubs | 2 | one 32-byte scratch area each; see [Hub class driver](#hub-class-driver) |
| Ports per hub | 7 | one byte of change bitmap |
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

**`0xc000_0120` DEBUG0, `0xc000_0124` DEBUG1** (RO, bring-up counters)

Not part of the programming model; `lsusb` prints them as the `wire:`
lines.

| Register | Bits | Meaning |
|---|---|---|
| DEBUG0 | 15:0 | packets this host transmitted |
| DEBUG0 | 31:16 | receptions started |
| DEBUG1 | 7:0 | receptions ending with a good CRC and PID |
| DEBUG1 | 15:8 | receptions that did not |
| DEBUG1 | 31:16 | frames in which port 0's line was not idle in the last `EOF_ZONE` (4 us) before the frame tick -- a hub's EOF points (round 23) |

**`0xc000_0128` TUNE** (RW, bring-up)

Low-speed timings, adjustable at run time with the shell's `usbtune`
([debug builds](#debug-build)).
Resets to `0x041414af`, the values the design always used, so it changes
nothing until written.

| Bits | Meaning | Reset |
|---|---|---|
| 7:0 | low-speed response timeout, in 8-clock units (1/6 us) | 175 = 29 us |
| 15:8 | low-speed turnaround inside a transaction, 8-clock units | 20 = 3.3 us |
| 23:16 | gap after a low-speed packet, 8-clock units | 20 = 3.3 us |
| 27:24 | J after a PRE (hub setup), full-speed bit times | 4, the spec minimum |

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

The layout in use (round 34). **Software owns this**; the hardware only
ever sees the `buf_off` values it is given, so this is convention, not
contract. The authoritative list is the defines named in the last
column -- this table was the phase-0 plan until round 34 and had
drifted from the code.

| Offset | Size | Use | Defined in |
|---|---|---|---|
| `0x000` | 512 | MSC data sector, **and** USB ethernet's frame chunks | `usbh_msc.c`, `MSC_OFF_DATA`; `usbh_ecm.c`, `ECM_OFF` |
| `0x200` | 31 | MSC CBW (command block wrapper) | `MSC_OFF_CBW` |
| `0x240` | 13 | MSC CSW (command status wrapper) | `MSC_OFF_CSW` |
| `0x260` | 8 | MSC's own SETUP packets | `MSC_OFF_SETUP` |
| `0x270` | 16 | USB ethernet notifications | `usbh_ecm.c`, `ECM_OFF_INT` |
| `0x280` | 64 | CDC receive | `usbh_cdc.c`, `CDC_OFF_RX` |
| `0x2c0` | 64 | CDC transmit | `CDC_OFF_TX` |
| `0x300` | 16 x 2 | Keyboard LED `SET_REPORT`, one per HID block, only while in flight | `usbh.c`, `SCR_LED0/1` |
| `0x320` | 96 | Free | |
| `0x380` | 4 x 16 | Auto-poll slot buffers | `usbh_hw.h`, `Z_USBH_OFF_POLL` |
| `0x3c0` | 32 x 2 | Hub port requests, one per hub | `usbh.c`, `SCR_HUB0/1` |
| `0x400` | 512 x 2 | Enumeration scratch (SETUP + configuration descriptor), taken while a device enumerates | `usbh.c`, `SCR_BIG0/1` |

Two users share 0x000, which is safe for a reason worth stating: mass
storage touches it only from inside FatFs and USB ethernet only inside
`Z_SYS_USBNET`, and both run with the scheduler held
(`k_syscall_touches_fs()`), so neither can be switched out with the
other's data half-copied. Nothing in the ISR touches it. See
[usb_ethernet.md](usb_ethernet.md).

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
sw/os/usb/usbh.h              address allocation, driver binding,
                              transaction-engine ownership
sw/os/usb/usbh_hw.h     register definitions (mirrors this document)
sw/os/usb/usbh_hid.c    boot keyboard, boot mouse, gamepad
sw/os/usb/usbh_msc.c    BOT + SCSI, STALL / Reset Recovery    (Phase 5)
sw/os/usb/usbh_hub.c    hub class driver                      (Phase 4)
sw/os/usb/usbh_cdc.c    CDC-ACM                               (Phase 6)
sw/os/usb/usbh_ecm.c    CDC-ECM, USB ethernet                 (Phase 7)
sw/os/usbnetapi.c       Z_SYS_USBNET, the syscall net uses    (Phase 7)
sw/os/usb/usbh_int.h    internal: device table, control engine,
                              shared by usbh.c and usbh_hub.c
sw/os/fs/fatfs/diskio_mux.c  FatFs drive dispatch, drive 2 = USB (Phase 5)

```

The block device glue was planned as `sw/os/fs/usbdisk.c`; it became
drive 2 in `diskio_mux.c` instead, next to the SD card and ramdisk.

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

**Measuring it: `usbbench [file]`** ([debug builds](#debug-build);
round 40, `sw/os/fs/sdbench.c`,
next to `sdbench` and in the same format, so the two compare line for
line). Layer 1 reads through `disk_read()` on drive 2 one sector per
call; layer 2 several per call; with a `/usb` filename, layer 3 is an
`f_read()` of that file. Each layer reports the SCSI commands it issued
(`z_usbh_msc_cmds`, counted in `scsi_cmd()`). While the driver issues
one READ(10) per sector, layers 1 and 2 measure the same and issue the
same number of commands; a multi-sector driver shows up as fewer
commands and a faster layer 2. Up to 256 KB from sector 0, or the whole
device if smaller. Read-only. Kill `wm`, `net` and `repl` first, as for
`sdbench`.

**Baseline (round 40, hardware):** 187 KB/s raw, one sector per call
or four -- 512 commands either way -- and 178 KB/s through FatFs: ~2.7
ms a sector, of which the data is ~0.4 ms (eight 64-byte packets) and
the rest per-command overhead (CBW, CSW, the device's own handling).

**Multi-sector transfers: tried in round 41, reverted in round 43.**
The driver issued one READ(10)/WRITE(10) per run of up to 32 sectors,
running the data phase 512 bytes at a time through the landing area.
On hardware it did what it should -- layer 2 went from 512 commands to
128 -- but only from 191 to 206 KB/s. The two layers split the time:
3.25 ms a one-sector command, 9.68 ms a four-sector one, so about 1.1 ms
of per-command overhead and **2.1 ms per sector of data**, five times
the ~0.4 ms eight packets take on the wire. The data phase, not command
overhead, is the limit with that device (16c0:05e1), and batching could
not pass ~240 KB/s. An ~8% gain was not worth the complexity in the
storage write path, so the driver is back to one sector per command.
`usbbench` stays, and reports the bulk requests and NAKs of each layer
(round 42): NAKs in the thousands mean the device is slow to supply
data; few mean the time is the host's -- the question to answer before
trying again. The MSC device model keeps honouring the CDB transfer
length, and `test_usb_msc` keeps its 4-sector read and write checks,
which now test that a 4-sector *call* is correct through single-sector
commands.

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

A stick pulled mid-write is not a theoretical concern. What happens:

- The port reports the disconnect; `usbh.c`'s detach path
  (`dev_reset_state()`) calls `z_usbh_msc_unbind()`, from the ISR. It
  touches no bus and clears nothing a running command is using: it
  marks the drive gone and bumps a generation counter.
- A command in flight notices at its next step and fails -- in
  simulation about 11 us after the detach, not after transfer
  timeouts. No Reset Recovery is attempted on a port with nothing
  connected.
- `diskio_mux.c` then reports `STA_NOINIT | STA_NODISK` and returns
  `RES_NOTRDY`, so FatFs reports "not ready", and `/usb` disappears
  from directory listings.
- Plugging a drive back in rebinds it. The mount itself is kept, and
  FatFs re-reads the medium from scratch on the first access; a dirty
  sector buffer from the old drive is dropped, never written to the new
  one. Handles opened on the old drive fail rather than touch the new
  one. `usbunmount` still clears the mount explicitly.

The generation counter is what makes a replug safe: an operation
records it on entry, and if it has moved the drive it started on is
gone -- even if another is already bound in its place.

FatFs will have lost whatever was buffered. That is unavoidable without
a VBUS switch and an orderly shutdown, and it is the same exposure the
SD card already has.

## CDC

**Status (round 17): works on hardware** -- a composite device
(16c0:05e1) binds and `usbcdc` talks to it. Apps cannot reach it yet;
see below.
`sw/os/usb/usbh_cdc.c`. At enumeration, a configuration with an ACM
communications interface (class 2, subclass 2) and a data interface
(class 0x0a) with bulk IN and OUT gets `SET_LINE_CODING` (115200 8N1)
and `SET_CONTROL_LINE_STATE` (DTR and RTS set -- many devices, the
Pico's stdio among them, send nothing without DTR), then binds; either
request may be STALLed without stopping the bind. The data path is bulk
IN and OUT, one packet per transaction, in process context holding the
transaction engine per transaction, with receive and transmit areas at
0x280 and 0x2c0 in the packet buffer. The notification endpoint is not
polled. One device at a time. On a composite device (class 239, an
Interface Association Descriptor) only the CDC function is bound; its
other interfaces are left alone.

**First hardware try (round 16): not recognized.** `lsusb` showed
`cfg desc (64 of 98 bytes)`: the driver kept only the first 64 bytes of
each configuration descriptor, and every class probe reads that copy.
The device's CDC data interface had its bulk endpoints at byte 64
onward, so the probe saw an ACM interface with no endpoints and
declined it. Round 17 keeps the whole descriptor, up to the 200 bytes
fetched. Checked offline on the device's own descriptor bytes: the full
descriptor binds (IN 3, OUT 3, 64 bytes); the first 64 alone do not.
This affected every probe, not only CDC -- any device with a long
configuration, which in practice means most composite devices.

**For apps (round 29):** `sw/apps/serial` owns both UART1 and the USB
device, on one port, `serial0`, with a connection each -- both can be
in use at once, from different windows. A CONNECT whose argument is
`Z_CONN_USBSERIAL_ARG` (`sw/common/zconnect.h`) gets the USB device;
anything else is UART1's baud rate, so `serial [baud]` and `port
serial0` are unchanged. From a term window, `usbserial` (or
`apps.term.auto_connect: usbserial`). `serial` stays resident on a
bitstream without UART1, serving the USB side. (Rounds 20-28 built this
as a second binary, `usbserial`, registered `usbserial0`.) It
reaches the kernel driver through three syscalls, `Z_SYS_USBCDC_PRESENT`
/ `_READ` / `_WRITE` (`sw/common/zusbcdc.h`, `sw/os/usbcdcapi.c`). Read
and write run with the scheduler held, as FatFs calls do: they share
the one USB transaction engine with mass storage, and a process
switched out while holding it would let another's transfer start on
top of it. With no device plugged in, a USB connect is refused; a device
unplugged mid-session drops the connection with a message saying so,
and a single failed read does not.

**Flow control (round 32).** `serial` reads the next USB packet only
while its port to `term` has room -- fewer than
`Z_PORT_MAX_PENDING_SENDS` (8) sends awaiting ack -- and if a send is
refused anyway (a full mailbox) it holds the bytes and retries them
before reading more. Before, it read regardless and ignored
`z_port_send()`'s refusal, so bytes already taken off the device were
lost: a Blaustahl's full-screen editor redraw (~2 KB, 80x24 with a dot
in every empty cell) arrived with holes. USB's own backpressure makes
waiting free -- a device that is not read NAKs and keeps its data.
`term`'s VT100 emulator (`sw/common/zvt100.c`) was checked against
everything that firmware sends -- `ESC[;H` with an empty first
parameter, `ESC[r;cH`, `ESC[J`, `ESC[K`, `ESC[0m`, `ESC[7m`, cursor
moves, and deferred wrap at column 80 -- and handles all of it.

**Devices that drop on a full FIFO (round 33).** The Blaustahl editor
still drew with holes after round 32: its firmware writes the editor
grid with `cdc_putchar()`, which **drops the character when its 64-byte
CDC transmit FIFO is full** -- by design, for keystroke echo; its own
comment says bulk output needs `cdc_putchar_reliable()`. The escape
sequences go through Pico stdio, which waits, so every row started in
the right place and then ran out of dots. A PC's host controller polls
a bulk IN endpoint many times a frame, so the FIFO never fills there;
no host that reads from a polling process can promise that. The fix is
in the firmware -- `blaustahl-reliable-bulk-output.patch`: the editor's
text and hex grids, the viewer and the CLI's line redraw use
`cdc_putchar_reliable()`, and `blaustahl.h` declares it (xmodem.c was
calling it through an implicit declaration, an error from GCC 14). On
this side, `serial` now polls again on the next tick while data is
flowing, instead of every ~16 ms, so any device's burst drains several
times faster.

For trying a device out without a term window, the shell's `usbcdc`
([debug builds](#debug-build)) is a terminal on it: keys go to the device, its output comes back, Ctrl-]
leaves. Do not use it while `serial` has a USB client: the shell does not
hold the scheduler, and the two would share the device.

The design as first planned:

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

**CP210x is now bound** -- see
[USB serial devices](#usb-serial-devices), which also covers how the
next vendor driver (FTDI) goes in.

## USB serial devices

**Status: CP210x confirmed on hardware** -- a Heltec WiFi LoRa 32 V3
(CP2102) binds as `class=cdc (cp210x)` on a root port while a hub
with a keyboard and mouse runs on the other, and `usbserial` shows its
Meshtastic console. Passes `make test_usb_serial` (72 checks).
CDC-ACM and Silicon Labs CP210x bridges bind; FTDI is next. First
user: the Meshtastic client, [mesh_app.md](mesh_app.md).

### One data path, several kinds

"USB serial" is not one protocol (see "Expectation to set" above), but
every kind has the same shape once it is running: bytes out on one bulk
OUT endpoint, bytes back on one bulk IN endpoint. So there is one data
path -- `z_usbh_cdc_read()` / `_write()` in `usbh_cdc.c`, the
`Z_SYS_USBCDC_*` syscalls, `serial0` -- and each kind contributes only
what differs:

| | where | CDC-ACM | CP210x | FTDI (planned) |
|---|---|---|---|---|
| recognised by | `z_usbh_ser_probe()` | class 2/2 + class 0x0a with bulk IN/OUT | VID:PID **and** a vendor-class (0xff) interface with bulk IN/OUT | VID:PID and shape |
| set up by | `z_usbh_ser_setup()` | SET_LINE_CODING, SET_CONTROL_LINE_STATE | IFC_ENABLE, SET_BAUDRATE, SET_LINE_CTL, SET_MHS | reset, baud divisor, data, flow, modem control |
| IN packets hold | `rx_payload()` | data | data | 2 status bytes, then data |

`sw/os/usb/usbh_ser.h` is the interface; its header comment is the
checklist for adding a kind. The name `usbh_cdc.c` and the syscall
names are historical.

**Setup is a list of requests, not states.** A kind's setup generator
returns control request *n* (host-to-device, at most 8 data bytes, and
whether its failure is fatal); `usbh.c` runs them one per pass through
a single enumeration state, `E_SER_SETUP`, then binds. A fatal
failure fails enumeration, which retries it; any other is logged
(`usb serial: cp210x setup request 2 refused, continuing`) and setup
carries on, because devices STALL optional requests they do not
implement. This replaced CDC-ACM's two dedicated states (`E_CDC_LINE`,
`E_CDC_DTR`); state 21 is retired rather than reused.

**Class before IDs.** A device that declares CDC-ACM is taken at its
word whoever made it; ID tables are only for vendor-class devices.
An ID match with the wrong interface shape binds nothing.

**Still one device at a time.** A second USB serial device enumerates
and sits unbound, as a second CDC-ACM device did before. (It is not
bound later if the first goes away; replug it.)

### CP210x

`sw/os/usb/usbh_cp210x.c`, from Silicon Labs' public AN571. No driver
source was consulted -- in particular not Linux's `cp210x.c`, which is
GPL.

- **IDs:** `10c4:ea60` only -- the one-port family default (CP2102,
  CP2102N, CP2103, CP2104, CP2109). Rebadged IDs are added as they
  turn up. The multi-port parts (CP2105, CP2108) are left out on
  purpose: which of their ports is "the" serial port is an unasked
  question.
- **Setup:** `IFC_ENABLE` (fatal -- the UART passes nothing until it
  succeeds), `SET_BAUDRATE` 115200, `SET_LINE_CTL` 8N1, `SET_MHS`.
- **Data:** plain bytes both ways, so it needs no `rx_payload()` case.

**DTR and RTS are set in ONE request, `SET_MHS 0x0303`.** On ESP32
boards these lines drive the EN/IO0 auto-reset transistors esptool
uses: one asserted without the other holds the chip in reset or
restarts it into its ROM bootloader. Two separate writes would pass
through that state; one write with both mask bits does not. Both
asserted is what Linux does on `open()`. If a board turns out to
dislike it, `CP_MHS_BOTH_CLEAR` (0x0300) is the one-line alternative
and is just as safe. The model in `test_usb_serial` counts every
moment DTR and RTS differ; the count must be zero.

**Baud rate is fixed at 115200** for every kind (`Z_USBH_SER_BAUD`), as
CDC-ACM's line coding always was. A syscall to change it waits for a
user who needs another rate; a bridge makes that meaningful in a way
CDC-ACM did not.

### FTDI, next

What it will take, so the shape above is checked against it now rather
than discovered later:

- a probe on `0403:6001` (FT232R), `0403:6015` (FT-X) and friends;
- setup: `SIO_RESET`, `SIO_SET_BAUD_RATE` with the divisor in wValue
  and wIndex (the one piece of real arithmetic), `SIO_SET_DATA` 8N1,
  `SIO_SET_FLOW_CTRL` none, `SIO_SET_MODEM_CTRL` -- all vendor OUT with
  no data stage, which the request list already covers;
- `rx_payload()`: strip the two modem-status bytes at the start of
  every IN packet; a packet of only those two means "nothing". Reads
  are already one packet per call, which is what makes this a
  per-packet operation;
- a model in `tb_usb_device.v` (`SER=3`) that inserts the status bytes,
  so the stripping is tested byte for byte like everything else here.

### `serial` now closes the connection when the device goes away

It used to print a notice onto the connection and mark it
disconnected on its own side only. A person reading `term` could see
that; a program cannot act on it. It now also sends `Z_PORT_CLOSE`, so
`term` shows its "closed by the other end" panel and `mesh` can start
reconnecting.

### Testing

`make test_usb_serial` (`rtl/tb/tb_usb_serial_cosim.v`): the real
driver and gateware against a CP2102 model on port 0 and a CDC-ACM
model on port 1 (`tb_usb_device.v`, `SER=1` and `SER=2`). It checks:
the CP2102's setup order and values; zero DTR/RTS glitches; receive
of 1 to 4000 bytes, short packets, NAKs with data waiting; send of 1
to 1000 bytes including exact multiples of 64 and a device NAKing;
reads and writes failing after unplug and working after replug; an
optional request refused (bound anyway) and IFC_ENABLE refused (not
bound, UART never enabled, lsusb says where); a CDC-ACM device beside
a bound CP2102 left alone, and alone bound as ACM with its own two
requests carrying data both ways. Both directions carry a position
pattern, so a lost, repeated or reordered byte shows.

### Hardware check

With a Heltec V3 (or any CP2102 board) on a USB host port:

    > lsusb
    ... state=running ... class=cdc (cp210x)
    ... device 10c4:ea60 ...

The boot log shows `usb serial: cp210x, addr N ep in 1 out 1, mps
64/64`. In a term window, `usbserial` shows whatever the board prints
at 115200. On an ESP32 board, the board must **not** reset when it
binds; if it does, see `CP_MHS_VALUE` above.

## USB ethernet

**Status: co-simulated, not yet on hardware.** `sw/os/usb/usbh_ecm.c`
binds a CDC-ECM adapter -- one raw Ethernet frame per bulk transfer --
and `sw/apps/net` sends and receives through `Z_SYS_USBNET`. Frames move
in 512-byte `auto_cont` chunks through the storage sector area; the data
path runs in process context holding the transaction engine for a whole
frame, as a SCSI command does.

One change here affects every device, not just adapters: **enumeration
now chooses the configuration** rather than always taking index 0. If
the first interface of the configuration read is vendor-specific and
the device offers another, the next one is read instead. That is what
makes a Realtek RTL8152 -- vendor protocol in configuration 1, CDC-ECM
in configuration 2 -- bind at all.

Full detail, including the MAC address policy, the polling cost and
what to check first on hardware, is in
[usb_ethernet.md](usb_ethernet.md).

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

## Debug build

The bring-up and debugging tools are compiled out of a normal kernel to
save space -- the kernel image, `.bss` included, has to fit its 256 KB
(`docs/kernel.md`) -- and back in with

```
cd sw/os && make clean && make USBH_DEBUG=1
```

(`make clean` because the objects do not track the flag). One switch,
`USBH_DEBUG` (`sw/os/Makefile`, `sw/os/usb/usbh.h`), covers:

| In debug builds only | What for |
|---|---|
| `usbcap`, `usbcapok`, `usbcapd` | wire capture of a failing / good transaction (also needs `PROBE`); [Capturing a failing transaction on hardware](#capturing-a-failing-transaction-on-hardware) |
| `usbtune` | low-speed timing sweep, the TUNE register |
| `usbnak N` | hardware NAK budget behind a hub (fixed at 0 otherwise) |
| `usbidle`, `usbbuf` | phase-1 tests: release the ports and read the lines; packet-buffer round trip |
| `usbcdc` | shell terminal on a CDC-ACM device -- apps use `serial` |
| `usbbench` | storage throughput, `sdbench`'s layers for `/usb` ([Throughput](#throughput)) |
| `lsusb` detail | per-attempt history of a failed device, configuration descriptor dump, poll slot and mouse report diagnostics, wire counters, line states |
| storage driver messages | `msc_verbose`: every failed command's stage and status |

A normal `lsusb` keeps what is needed to see what is plugged in and
whether it is healthy: every device and hub port, state, address, type,
keyboard LEDs, class, retries and hub recoveries, VID:PID and
configuration, a failed device's reason, the hub's status and its
`ports disabled by the hub N, recovered M` counters, and the ports.

Measured with the project's toolchain (xPack `riscv-none-elf-gcc`
15.2.0-1), round 44: **242,272 bytes** (19,872 free) normally,
**252,132** (10,012 free) with `USBH_DEBUG=1` -- about 10 KB for the
tools. Co-simulation always builds with them (`Z_USBH_COSIM` implies
`USBH_DEBUG`): its benches use the capture and dump paths.

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

### Pin registers in the I/O cells

Since round 29, D+ and D- are registered in each pad's own I/O cell in
both directions: `ODDRX1F` driving out, with both halves the same value,
and `IDDRX1F` sampling in, using its rising-edge sample. The routed
result puts each pin's cell in `MODE = IDDRX1_ODDRX1` -- both
registers together in the pad -- and places D+ and D- in the two halves
of one I/O block (port 0 at X51/Y0, port 1 at X44/Y0 on mozart_ml1).

Before that, the flops driving and first sampling the two lines were
ordinary fabric flops the placer could put anywhere, each with its own
route to its pad: nanoseconds of skew between D+ and D-, different in
every build, showing up as a brief SE0 or SE1 at each transition. Fmax
does not see it -- it covers flop-to-flop paths only. The change makes
the FPGA's contribution to D+/D- skew fixed silicon, matched, and the
same in every build; what remains is picosecond-scale, plus the board
and cable. It was made because hub failures came and went between
builds with identical logic (Known issues, item 5); skew was a plausible
cause there, never a measured one.

Consequences: the output enable carries one fabric register to stay
aligned with the data's one-clock latency, and **nothing else may read
the USB host pins** -- an `IDDRX1F` must be its pin's only load, which
is why the logic probe takes `usb_host.v`'s `line0_o` (see
`docs/probe.md`). Simulation uses plain registers with the same latency
(`ifndef SYNTHESIS`).

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
| **2** | Software enumeration, HID driver, `usb_hid_compat.v`, **auto-poll and the hardware cursor datapath** | **Done on hardware.** Keyboard and mouse both bind and deliver reports; cursor has zero CPU in its path. `wm.c`, `gpu3d.c`, `bios.c` untouched |
| **3** | Second root port, hotplug, simultaneous mixed LS/FS | **Done on hardware.** Both ports populated at once, mixed speeds, hot-swap in any order. See [Phase 3 results](#phase-3-results) |
| **4** | Hub class driver, multi-device addressing, port-change endpoint, `tb_usb_hub.v`, **PRE validated against real hardware** | **Done on hardware.** Keyboard, mouse, stick and CDC device behind one hub at once. See [Hub class driver](#hub-class-driver) and [Hardware: working behind the hub](#hardware-working-behind-the-hub) |
| **5** | Bulk, MSC, SCSI, FatFs drive 2 at `/usb`, `auto_cont` | **Reads and writes done on hardware.** Unplug handling, STALL / Reset Recovery and the IN toggle check pass simulation; hardware checks and one-sector-per-command remain -- see [Also outstanding](#also-outstanding) |
| **6** | CDC-ACM, scroll wheel, keyboard LEDs via `Set_Report` | **Done on hardware.** CDC-ACM with apps via `serial`; keyboard LEDs and lock keys; scroll wheel through report protocol for mice whose descriptor has the simple layout with a wheel (rounds 35-38) |
| **7** | Hardening, error recovery, benchmarks against SD. Optionally USB Ethernet, optionally bus-mastering DMA | |

Each phase updates this document and `docs/user_input.md`.

### Scroll wheel — a note for Phase 6

**Round 38, final shape -- confirmed on hardware:** byte 3 is counted
only when the driver has confirmed from the report descriptor that it
is the wheel (poll mode bit 2, `in_wheel`). The HID spec defines the
boot mouse report as three bytes; a fourth in boot protocol is a common
convention, not a rule, so rounds 35-37 counting it for any 4-byte
report could have made a mouse with vendor data there scroll by itself.
With `USB_HID` (the older core) the register's top byte is a constant
0, so `wm` sends no wheel events and the mouse works as before.

**Round 37 update:** a descriptor parse after all, but a small one. A
Microsoft 045e:0737 sends no wheel in boot protocol -- its counter never
moved on hardware -- though its 4-byte endpoint is the size of the
simple report-protocol layout. So mice now have their report descriptor
read and parsed (`z_usbh_hid_rdesc_parse()`), and a mouse whose input
report is exactly buttons / X / Y / wheel, 8-bit, no report IDs, is set
to report protocol -- the same bytes the hardware already parses, plus
the wheel. Everything else stays in boot protocol. See
`docs/user_input.md`, "Scroll wheel".

**Round 35 outcome:** no descriptor parse. Linux's boot-protocol mouse
driver (`drivers/hid/usbhid/usbmouse.c`) reads `REL_WHEEL` from
`data[3]` unconditionally and TinyUSB's `hid_mouse_report_t` is
`buttons, x, y, wheel, pan`: in boot protocol the wheel is byte 3 by
convention, and a mouse without one sends three bytes. So
`usb_hid_compat.v` accumulates byte 3 into `reg_mouse[31:24]` when the
report had a fourth byte -- `in_len`, the payload count `usb_host.v`
already keeps while snooping the report -- and the accumulator design
below was the one chosen. See `docs/user_input.md`, "Scroll wheel". A
report-protocol parser remains the route for a mouse that only reports
its wheel there, if one turns up. The original note:

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

**With hindsight (phase 4):** the wrapper would have tested something
further. MODE 2 checked only the low nibble of the PRE PID, and the SIE
was sending PRE with a wrong check field. What exposed it was the hub
bench putting a *full-speed* listener on the same wires as the PRE
traffic -- the view a real hub has. The hub model did end up as a mode
of `tb_usb_device.v` (HUB=1) rather than its own file.

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

Enumeration was serialised as a stopgap, and the control state now
lives **per device** in `z_usbh_dev_t`. Only address zero is
serialised, which is the part the protocol actually requires: a freshly
reset device answers on 0, so two of them mid-enumeration would both
reply to the same token. Everything after `SET_ADDRESS` overlaps
freely, which is what hubs need.

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

## Timing, and receive margin

Two defects found after the device classes were working, both of which
had been silently distorting every earlier measurement.

### The SoC stopped meeting timing

| Build | Fmax | |
|---|---|---|
| `USB_HID` | 52.5 MHz | pass |
| `USB_HOST`, as first written | 45.30 MHz | **FAIL at 48** |
| `USB_HOST`, now | **55.07 MHz** | pass |

The failing path was 17.8 ns of routing against 4.2 ns of logic --
congestion, not depth. `wb_adr_i` was decoded combinationally in
sixteen places across six always blocks, so the top-level `wbm_adr`
net, which the arbiter's mux drives and every slave already loads,
gained that many more sinks.

The inputs are now latched once on entry and every decode works from
the copies, presenting eleven flop inputs instead. It costs one wait
state per register access, which nothing notices.

**This was running below 48 MHz for weeks.** The symptom was
intermittent receive CRC errors that moved between builds, and several
days went into chasing the receive path for a fault that was not
there. LUT4 and BRAM were tracked throughout development; Fmax never
was. `make usb_fmax BOARD=<board>` now exists.

Registering the inputs also introduced a bug worth recording: moving
`wb_ack_o` a cycle later made `wb_sel_cyc && !wb_ack_o` true for TWO
cycles, so every poll-table write, compat-block write, `XACT_B` start
and `IRQSTAT` clear fired twice. Co-simulation caught it immediately.
There is now a single `wb_wr_stb` that all of them key off.

### The receiver failed the full-speed spec limit

`phase` is reset when an edge is **detected**, which is one cycle after
it appears -- `ls_changed` compares the synchronised line against its
registered copy. Sampling at `DIV/2` therefore landed `DIV/2 + 1`
clocks after the real edge: 75% of the way into a four-clock
full-speed bit rather than the middle.

Invisible against a transmitter running at exactly nominal, which is
all `tb_usb_device.v` could produce. Given a `CLK_PPM` parameter, the
bias showed immediately:

| | before | after |
|---|---|---|
| full speed, slow device | +10000 ppm ok | +20000 ppm ok |
| full speed, **fast** device | **fails at -2500 ppm** | -20000 ppm ok |
| spec requires | ±2500 ppm | |

All the margin was on one side and it failed *at* the limit on the
other. `mid` is now `(DIV >> 1) - 1`. Low speed measures ±20000 ppm
against a ±15000 requirement.

`make test_usb_margin` runs this and should stay part of any change to
the receive path.

### A measurement that nearly became a wrong conclusion

The first tolerance run reported 5000-7000 ppm at low speed against a
15000 requirement -- a 3x spec shortfall. It was wrong. `CLK_PPM`
skewed the model's **own** receive sampler as well, and that sampler is
fixed-rate with no resynchronisation, far less tolerant than the host's
DPLL. The harness was measuring itself.

Skewing only the model's transmit path changed the answer from "3x
short of spec" to "2x better than spec". Worth remembering whenever a
testbench reports a limit: check which side of it is under test.

### What skew did not explain

`SKEW_NS` reproduces the one-sample SE1 real devices emit at every
transition. The receiver tolerates it up to 60 ns -- 72% of a
full-speed bit -- because `ls_stable` holds the last legal state
regardless of duration. So transition skew alone does **not** account
for the residual hardware error rate, and the theory carried for
several rounds was wrong.

Two later notes. The one-sample SE1s in the hardware captures were
sampled by the logic probe through fabric routing, so some of them may
have been the probe's own skew rather than the line's; since round 30
the probe records the pad-registered samples instead. And since round
29 the host's own D+/D- paths are in the I/O cells, so our side
contributes no build-dependent skew -- see
[Pin registers in the I/O cells](#pin-registers-in-the-io-cells).

## Hub class driver

`sw/os/usb/usbh_hub.c`. A state machine per hub, kept in the hub's own
device record and stepped from `z_usbh_poll()` like everything else, so
it never waits. Every request goes through the hub's control engine,
so the rule that mass storage owns the transaction engine for a whole
SCSI command covers hub traffic too.

**Bring-up.** GET_DESCRIPTOR(HUB) for the port count and
`bPwrOn2PwrGood`; SET_PORT_FEATURE(PORT_POWER) on each port; wait for
power to settle; then an auto-poll slot in RAW mode on the status
change endpoint, and one read of every port.

**Changes.** The slot's `changed` bit means the change bitmap has
landed. For each changed port: GET_PORT_STATUS, CLEAR_PORT_FEATURE for
each change bit, then act. A connect creates a child record that
debounces 100 ms on its own; a disconnect, or a connect change on a port
that already has a device, tears the child's whole subtree down. A hub
keeps reporting a change until it is cleared, so a report lost while
rewriting `POLL_B` comes round again on the next poll.

**Reset.** When a debounced child is waiting, address 0 is free and a
scratch area is available: SET_PORT_FEATURE(PORT_RESET), poll
GET_PORT_STATUS until C_PORT_RESET, clear it, and hand the child over
with the speed the hub reports -- `LOWSPEED|USE_PRE` for low speed,
**not** the inverted polarity of a direct attach. After 10 ms recovery
the child enumerates from E_DESC8 exactly as a root-port device does.
Every path out of the reset sequence that does not hand the child over
returns it to waiting; an abandoned reset used to hold address 0 for
ever and stop the whole tree.

**What had to change in `usbh.c`** -- it assumed device index = root
port throughout:

| Was | Now |
|---|---|
| `devs[2]`, one per root port | `devs[8]`: root ports at 0-1, the rest handed out to devices behind hubs, with parent and hub port |
| auto-poll slot = device index | slots allocated from the 4 in hardware |
| control scratch per port at 0x400/0x600 -- a third device would index past the 2 KB buffer | two 512-byte areas for devices enumerating, given back at bind; two 32-byte areas kept by hubs |
| teardown of one device | teardown of a subtree, deepest first, releasing slot, scratch, compat block, MSC binding and address |
| `lsusb` per root port | a tree under each root port |

The device record, control engine and states moved to `usbh_int.h`,
shared by `usbh.c` and `usbh_hub.c` only.

**Limits:** 8 devices including hubs (7 addresses); 2 hubs, from the
scratch pool; 7 ports per hub (one change byte); hubs served to depth 3.
A hub's own over-current and local-power changes are acknowledged and
not acted on.

### Hardware: retries behind a hub

First run on hardware, hub 05e3:0608: the stick and a low-speed mouse
behind it worked -- the first real confirmation of PRE. A low-speed
keyboard (04d9:1203) behind it did not. `lsusb` showed it pass
GET_DESCRIPTOR(8) and SET_ADDRESS, then fail three times on the DATA
stage of an IN at its new address with `TIMEOUT (no response)`, NAKs
recorded just before each.

**Cause.** Inside `usb_xact.v`, a NAKed transaction was re-issued by
jumping straight to `X_TOK`, skipping `X_GAP` -- the state whose own
comment describes the failure: a token sent the moment the previous
transaction ends collides with the device still driving its EOP, and
the device "decodes a mangled token, answers nothing". Only a
transaction's FIRST token went through `X_GAP`; retries after a NAK,
after a discarded resend, and the next auto-continue packet did not.
At low speed the device's EOP tail is eight times longer, and behind a
hub the retried PRE lands in it: the hub drops the PRE, the keyboard
never sees the IN, and the retry times out. The mouse happened not to
NAK during enumeration; a directly attached keyboard evidently
tolerates the early retry, which is why the same keyboard worked on a
root port.

**Measured before it was fixed.** With the mouse model behind the hub
set to NAK its first descriptor INs, co-simulation failed exactly as
the hardware did -- `get-desc8, DATA stage: TIMEOUT (no response)
(1 nak)`, twice. `test_usb` case 7 with two NAKs failed the same way;
case 6, low speed direct, passed, matching the hardware.

**Fix.** Every token now goes through `X_GAP`: wait for the line to
leave SE0, then 5 bit times at the transaction's speed. That costs 20
clocks per re-issue at full speed and 160 at low speed. The NAK cases
stay in `test_usb` (6 and 7) and `test_usb_hub`.

**That was not the whole story** -- see the next section. The retry
fix is real and stays, but the keyboard still failed after it, and the
"not explained yet" hub CRC error above turned out to be the main
fault.

### Hardware: full-speed traffic straight after low-speed

After the retry fix the keyboard still failed behind the hub, and the
retest showed what the first `lsusb` had only hinted at: requests to
the **hub itself** -- ordinary full-speed transfers -- failing with
SETUP-stage CRC errors and status-stage timeouts. The stick kept
working with the keyboard plugged in; the mouse stopped working once
the keyboard was there. The common factor was full-speed traffic
following low-speed traffic on the same wire, which before hubs never
happened: a low-speed device had a root port to itself.

`X_GAP` waited five bit times of the speed of the transaction *about to
start*. After a low-speed transaction the device drives a full
low-speed bit of J after its EOP (667 ns), and behind a hub the hub is
still repeating it upstream; a full-speed token after only five
full-speed bits (417 ns) started on top of it. Hub requests landing
there failed exactly as seen. A SOF landing there is a SOF the hub
never receives -- and SOFs are what a hub turns into keep-alives for
its low-speed ports, so the mouse starved once the keyboard's
enumeration traffic made the collisions frequent.

**Measured before it was fixed.** A monitor for `x` on the wire in the
hub co-simulation -- the host and a device driving at once -- found
two collisions in the enumeration section alone; the trace showed a
SOF starting 1.46 us after a low-speed ACK was received. Every bench had
passed throughout: the device models tolerate a collision, a real hub
does not.

**Fix** (`usb_xact.v`, `gap_limit`): the engine remembers the previous
transaction's speed and port, and after a low-speed transaction the
next token on that port waits five low-speed bits whatever its own
speed. The monitor found no collisions afterwards, and is now a
permanent check in `test_usb_hub` (the hub segment) and `test_usb_msc`
(both ports).

The fix costs about 3 us after each low-speed transaction on a shared
port -- once per mouse or keyboard poll.

**A second collision the monitor found.** Put into the storage bench
as well, the same monitor caught two collisions on port 1 -- a
*directly attached* low-speed mouse, no hub -- during enumeration, with
no transaction transmitted in between. The source was outside the
transaction engine: `usb_port.v`'s once-a-frame low-speed keepalive, an
EOP of the host's own. It was gated only on the engine being idle, and
the engine goes idle the moment it sees the device's EOP begin, while
the device drives for about a low-speed bit and a half more. Now
(`usb_host.v`, `ka_quiet`) a port's keepalive waits until the engine
has been idle and the line out of SE0 for five low-speed bit times, and
the scheduler starts nothing while a keepalive is waiting -- otherwise
continuous traffic on the other port would never leave that window,
and the keepalive would starve. It does not hold the scheduler while a
port is driving its 10 ms reset. This one predates hubs; it may account
for some of the earlier low-speed trouble on root ports.

### Hardware: a failed device poisons address 0

After round 9 the keyboard still failed behind the hub, a second hub
(Realtek 0bda:5411) behaved the same -- so the fault is on this side --
and three more things were reported: requests to the **hub itself**
failing on every port and every stage, the mouse failing whenever the
keyboard was plugged in, and things getting worse over repeated
plugging until the mouse and stick stopped enumerating too. Root ports
stayed fine throughout.

The second hub's `lsusb` had the decisive line: the mouse failed every
attempt at `get-desc8, SETUP stage: CRC/bitstuff error`. A corrupted ACK
to a SETUP on address 0 is two devices answering. The keyboard's last
attempt had died at `set-address, STATUS stage`, so it never took its
new address -- it was still on address 0, on a hub port still enabled.
`usbh_addr0_busy()` only counted devices mid-enumeration, so the mouse
was let onto address 0 beside it. On a root port a failed device has a
wire to itself; behind a hub every device shares one segment.

**Reproduced first.** A device model that STALLs the status stage of
SET_ADDRESS (`stall_set_addr`) fails enumeration while staying on
address 0. Behind the hub model it made the mouse fail every attempt,
and the contention monitor counted 39 collisions -- two devices
answering the same SETUPs.

**Fixed** (`usbh.c`, `usbh_hub.c`, `usbh_int.h`):

- `on_addr0`: a device behind a hub counts as on address 0 from its port
  reset until SET_ADDRESS succeeds -- including after a failure in
  between -- and while any other device is, nothing new may use address
  0.
- When a device behind a hub fails for good, the hub driver disables its
  port (CLEAR_PORT_FEATURE(PORT_ENABLE)), so it hears and answers
  nothing. `port_act()` does not mistake that disabled port for a fault
  to recover.
- One reserved address per device, kept across retries and freed only
  at teardown. A retry used to free it first, but a device whose port
  reset then failed still held it, and the next device was given the
  same address -- two devices on one address, which also explains the
  decline over repeated plugging.

After the fix: the failed device's port is disabled, the mouse
enumerates past it and works, zero collisions. `test_usb_hub` keeps the
case.

**Not explained yet**, and deliberately not theorised about further:
why the keyboard fails in the first place (it ACKs its SETUPs and never
answers the IN that follows -- clean timeouts), and the failures of
requests to the hub itself. Rounds 8 and 9 each fixed a real,
reproduced fault that was offered as the likely cause of this, and
neither was. The next step is a capture.

### Capturing a failing transaction on hardware

`usbcap` (shell; a [debug build](#debug-build) with `PROBE`) makes the
driver arm the built-in logic probe (`rtl/probe.v`) before every
transaction on root port 0,
and freeze it on the first one that ends in a timeout, CRC error or
babble. The capture -- port 0's D+/D- at 48 MHz, 170 us -- then starts
with the failing transaction. `usbcapd` prints it. While capturing,
the hardware's NAK retries are off, so each attempt is one transaction
and fits in one capture; software paces the retries as usual. If the
failure goes away in capture mode, that is itself a finding: it points
at the hardware retry path.

Decode it off the board:

    tools/usbcap.py capture.txt

One line per packet: start time, gap since the previous packet, speed,
PID (with its check field), token address and endpoint with CRC5, data
bytes with CRC16, EOP length -- and anything the wire should never
show: SE1 (two drivers), glitches, bit-stuff violations, packets cut
off by the end of the capture. Console messages landing inside the dump
do not shift the data; words are placed by their printed index.

The decoder was checked against a capture made the same way in
simulation, where the SIE's own trace says what was sent: it recovered
PRE, the low-speed SETUP to address 0, the DATA0 carrying
`80 06 00 01 00 00 08 00` with a valid CRC16, the ACK, and an IN answered
by NAK, with gaps and EOP lengths.

### The first capture: the hub goes deaf

With the keyboard plugged into the hub, `usbcap` froze on a request to
the **hub itself** (address 1), the DATA stage IN of a control
transfer, TIMEOUT. `tools/usbcap.py` on it:

    0.021 us  FS  IN addr 1 ep 0  EOP 2.0 bits   <-- the failing transaction
    ...then 165 us of idle J

The host's side is clean: valid SYNC, PID, CRC5, a 2-bit EOP, no
collision, no SE1. And the hub never answers -- not late, not garbled,
not a NAK: silence, for forty times the host's timeout. So the hub is
intermittently deaf to its own address in the middle of a control
request whose SETUP it had just ACKed, and only with low-speed devices
behind it.

That rules out host-side timing and collisions for this failure. It
points at the hub's own state; one candidate is the hub still in its
low-speed (PRE) repeater mode from traffic before, not listening
upstream at full speed. The capture cannot show that, because it began
at the failing token. So capture now keeps a window recording across
transactions -- the driver re-arms the probe only when the previous
window is over -- and a frozen capture holds the failing transaction
with whatever preceded it in the same 170 us, usually its own SETUP
and any PRE traffic just before. The decoder marks the failing packet.

### The cause: transfers ran across frame boundaries

Stepping back from individual captures to everything seen on hardware:

| Observation | |
|---|---|
| Everything works on root ports | keyboard, mouse, stick |
| Behind two different hubs | the keyboard always fails, the mouse mostly works, the stick works, requests to the hub itself fail at random |
| The keyboard fails in `get-cfg` / `get-desc` DATA stages | `get-desc8` -- a single packet -- always succeeds |
| The keyboard's configuration descriptor | 59 bytes (`09 02 3b` in the first capture) |

**A hub enforces USB frame timing; a root port does not.** A hub times
each 1 ms frame from the host's SOFs and, near the end of one (EOF1,
EOF2), stops repeating traffic and treats anything still running as
babble. The host must send an SOF every millisecond on time, and must
not start a transaction that cannot finish before the next.

This host did neither. The SOF was only *queued* at the frame tick and
went out whenever the engine next went idle, and an auto-continue
request ran packet after packet through the boundary. At low speed
through a hub a packet takes ~150 us, so:

- the keyboard's 59-byte configuration descriptor, eight packets, holds
  the engine ~1.2 ms -- it crosses a frame every time, and fails every
  time; its 18-byte device descriptor often; its single-packet
  `get-desc8` never;
- the mouse's 34-byte descriptor, five packets, ~0.75 ms, often fits --
  so the mouse mostly worked, and failed once the keyboard's traffic
  made crossings frequent;
- the stick is full speed with short packets, and its bulk retries hide
  the occasional crossing;
- requests to the hub itself fail when they meet the hub's confused
  frame timing;
- a root port has no frame enforcement, so everything worked there.

**Measured before the fix:** in `test_usb_hub`, enumerating the mouse
and reading from the stick, SOFs went out up to **207 us** after their
frame tick. A hub expects them within a bit time or two.

**Fix** (`rtl/usb/usb_host.v`, `rtl/usb/usb_xact.v`): an end-of-frame
guard. The scheduler starts nothing that cannot finish before the next
frame tick -- 75 us of frame left for full speed, 210 us for low speed
-- and the engine checks the same before every re-issue inside a
request (a NAK retry, a discarded resend, the next auto-continue
packet), ending the request with `ST_NAK` and its progress kept in
`act_len`. Every driver already resumes from there after a NAK. The
engine is then idle at every frame tick and the SOF goes out on time:
**0 us late** after the fix, in the same scenario.

`test_usb_hub` and `test_usb_msc` now check on every frame that no SOF
starts more than 2 us after its frame tick.

**What the spec requires** (USB 2.0 11.2, "Hub Frame Timer"): a hub
locks its frame timer after two consecutive clean SOFs and free-runs
through at most two missing ones -- so a *late* SOF is worse than a
missing one, because the hub has already predicted where the frame
ends. Near that end it enforces two points: after EOF1 (~32 FS bit
times before the next SOF) it starts repeating no new packet, and a
port still transmitting upstream at EOF2 (~10 bit times) is babbling
and is disabled. So everything must be over before EOF1; the guards
leave 15 us (FS) and 25 us (LS) beyond the worst-case transaction.
Transaction translators and split transactions do not apply: behind a
full-speed host a USB 2.0 hub runs as a full-speed repeater.

This explains the whole pattern; it has not yet been confirmed on
hardware. The capture tool (below) has a known fault -- a window can
start at a transaction other than the one that later fails -- and is
parked: it is not needed to test this.

### Hardware: working behind the hub

After round 14, on hub 05e3:0608: **keyboard (04d9:1203), mouse
(045e:0737) and stick (16c0:05e1) all work behind the hub at the same
time**, for the first time. The keyboard's enumeration no longer shows
any NAK-then-timeout failures.

What remains is a different class: occasional **plain timeouts**,
transactions nobody answered -- a SETUP to the hub not ACKed, a
descriptor read right after a reset -- all with zero NAKs, and all
recovered by retries (the keyboard bound on its fourth attempt). The
wire is clean: 10432 receptions, 4 bad. One was user-visible: the first
`usbmount` failed with every request to the stick, Reset Recovery
included, timing out, followed by `usb msc: device removed` -- the hub
reporting that port changed, most likely disabled. After it was
re-enumerated the second mount worked. A hub may disable a port it
judges to be babbling (USB 2.0 11.8.x); there is no evidence here of
why it did, and it is recorded rather than guessed at.

### The last fault: a stale result on resumed transfers

Round 19, confirmed on hardware: keyboard, mouse, stick and CDC device
behind one hub at once. For anyone reading the history, the faults that
stood between "a hub enumerates" and that, in the order they were
found, each confirmed by a hardware change in behaviour:

| Round | Fault | Where |
|---|---|---|
| 7 | PRE sent with a wrong PID check field; no LS device behind any hub could work | `usb_sie.v` |
| 7 | devices consumed each other's transaction results | `usbh.c`, owner |
| 9 | full-speed tokens and keep-alives started on a low-speed EOP tail | `usb_xact.v`, `usb_host.v` |
| 10 | a failed device left on address 0 poisoned every later enumeration | `usbh.c`, `usbh_hub.c` |
| 13 | transfers ran across frame boundaries; SOFs up to 207 us late | `usb_host.v`, `usb_xact.v` |
| 14 | NAK retries went out microseconds apart, in hardware and software | `usbh.c` |
| 19 | a resumed transfer judged a result it no longer owned | `usbh.c` |

Round 19's is the one that finally made keyboards work: see
[Known issues](#known-issues), item 4. It was introduced, in effect, by
round 13, which made split-and-resumed transfers common. The resume
path had always judged the status stage one call late; it only mattered
once transfers were routinely resumed.

Theories along the way that were wrong, recorded so they are not
revisited: a hub going deaf (round 11, a tooling artifact), a late
low-speed turnaround (round 12, falsified in simulation), an
out-of-spec keyboard clock (round 18 -- the Holtek is +1.75%, but a
second keyboard failed identically).

### After the frame fix: NAK retries went out microseconds apart

On hardware after round 13: the mouse and the stick work behind the hub
even with the keyboard plugged in, and Fmax is 58.34 MHz. The keyboard
still fails, now even on the single-packet `get-desc8` -- so the frame
fix was real but not the keyboard's cause.

The keyboard's failures, across every log, share one signature: a NAK
on a data or status stage, then no response to the retry. It is a slow
microcontroller that NAKs while preparing a descriptor; the mouse
answers at once and never NAKs.

Our host retried a NAK far faster than any real one:

- the hardware NAK budget retried ~3 us after the NAK;
- and `ctrl_step()`'s "paced" software retry was not paced at all. In
  `CS_DATA_RUN`, `CS_STATUS` and `CS_FINAL` it set `ctrl_delay_until`
  and then relaunched **in the same call** -- the delay only postponed
  judging the next result. The comment promising retries ~2.7 ms apart
  was never true.

UHCI, OHCI and a high-speed hub's TT all revisit a NAKed control
endpoint on a later list pass, typically the next frame. Cheap
low-speed firmware has likely never seen a retry within microseconds
of its own NAK, and behind a hub every retry is also PRE-prefixed and
repeated. The mechanism inside the keyboard is not proven; the host's
behaviour was plainly wrong either way.

**Fixed** (`usbh.c`): on a NAK, judge it, set the delay and return; the
next call after the delay relaunches -- `CS_DATA_RUN` through its
existing launch path, a NAKed data stage resumed by `CS_STATUS` through
`CS_DATA_RUN` a packet at a time, a NAKed status stage by `CS_FINAL`.
And a low-speed device behind a hub gets a hardware NAK budget of 0,
so every NAK comes back to software and is retried a tick (~1.4 ms,
the next frame) later. In a [debug build](#debug-build), `usbnak N` sets
that budget at run time; otherwise it is fixed at 0.

### Three more captures, and what they corrected

Round 11's reading above -- "the hub goes deaf" -- was wrong. Three
captures (keyboard alone, mouse alone, stick alone, all behind the hub)
showed:

- **Hub requests fail with only a full-speed stick behind the hub**, so
  they are not about low speed at all.
- In the mouse and stick captures the hub *answers* -- it NAKs data and
  status-stage INs while busy, which is correct. The transaction that
  actually timed out was a **SETUP** to the hub that got no ACK, and it
  is in neither capture. Two tooling faults hid it: the capture header
  gave the engine *state* (`stage 3`, `CS_STATUS`, which judges the
  previous stage, the SETUP) rather than the failing transaction, and
  keeping a window recording across transactions meant a transaction
  launched near the end of a window transmitted after it closed. Both
  are fixed: the header now names the failing PID, the decoder marks
  that packet, and the probe is armed at every transaction again.
- **The keyboard does answer.** Its IN got a correctly formed DATA1
  starting `09 02 3b` -- the configuration descriptor -- 2.9 us after
  the host's EOP, legal and in time. The host missed it, called it a
  timeout, and later transmitted over the rest of it. The single-sample
  SE1s on its edges are the two lines crossing a few ns apart as the hub
  repeats slow low-speed edges; the decoder now says so instead of
  reporting "two drivers".
- **Falsified:** answering 2.9 us after the EOP is not the cause. The
  mouse model given that turnaround (`extra_turn_ns`) enumerates behind
  the hub model without trouble.

So two open questions, each needing one more capture with the fixed
tooling: why the hub does not ACK some SETUPs, and why the host's
receiver misses the keyboard's reply. The capture header now also
reports the controller's own receive counters across the failing
transaction -- whether the receiver started on the reply at all, and
whether it then rejected it.

### Two core bugs the hub found

Neither is specific to hubs; the hub bench is what made each routine.

**Devices consumed each other's transaction results** (`usbh.c`). Each
device's `ctrl_step()` waited for "not pending" and then read `XACT_S`
as its own. `z_usbh_poll()` steps devices in index order, so after
device 1 launched, device 0 ran first on the next poll and took device
1's result -- in the bench, a hub's 4-byte GET_PORT_STATUS reply judged
as a child's SETUP. Two devices enumerating on the two root ports at
once could hit it; the retry logic mostly hid it. Now the device that
launched a transaction owns its result until it reads it, and no other
device may read or launch meanwhile.

**PRE was sent with a wrong PID check field** (`rtl/usb/usb_sie.v`).
On the tick emitting the PRE PID's last bit, `TB_PREPID` also set
`tx_j <= 1` to start the idle gap, and that later assignment overrode
the bit's transition. The byte went out as 0xBC, not 0x3C. A hub checks
the PID check field and ignores a PRE that fails it, so **every
low-speed device behind a real hub would have been unreachable**. The
PRE-mode device model compared only the low nibble and passed it from
phase 1 on. Now the model checks the whole byte, as a hub does, and
`test_usb` case 7 fails against the old SIE; the fix is the removal of
that one assignment.

### The hub model, and what it corrected in the device model

HUB=1 in `tb_usb_device.v`: class requests, the status change endpoint
(NAK until a change), per-port state, a monitor turning `hub_conn` into
connect changes and completing resets after `HUB_RST_NS`, and a bus
reset that unpowers every port. Devices behind it are instances on the
same wires. Putting several models on one wire exposed four model
faults, all fixed:

- **A zero-time K at the end of every packet.** The EOP released SE0
  before setting J; the host's clocked receiver never saw it, but an
  event-driven receiver in another model took it as a packet start and
  swallowed the host's next token.
- **Bus reset after 667 ns of SE0**, 8 of the model's own bit times,
  shorter than the SE0 ending a low-speed packet. Now 2.5 us (TDETRST).
- **The PRE-mode device decoded full-speed traffic** meant for others.
  Now it skips anything not starting with PRE -- and checks the whole
  PRE byte.
- **Full-speed models decoded low-speed replies** coming upstream.
  Now a full-speed model recognises a low-speed SYNC and skips it.

## Mass storage status

`usbh_msc.c` implements bulk-only transport and the SCSI commands a
block device needs, `diskio_mux.c` serves FatFs drive 2, and `/usb`
mounts from the shell with `usbmount`. On hardware, `ls /usb` lists a
real FAT filesystem.

### The truncation bug (resolved in simulation)

**Status:** fixed. Passes `make test_usb_msc`, and on hardware FatFs
reads a text file and an 11 KB PNG through `/usb`. The cause is in
[What the truncation actually was](#what-the-truncation-actually-was----measured)
below. This section is the original report, kept as written.

Larger reads were **truncated**. A 512-byte `READ(10)` returns 13
bytes, and dumping those bytes shows `EB 3C 90 6D` -- the jump
instruction and the start of the OEM name in a FAT boot sector. That
is real sector data, so:

- the device is sending the sector
- there is no stale CSW and no transport desynchronisation
- only the first few bytes are captured, and because 13 is less than
  the 64-byte maximum packet size the hardware treats it as a short
  packet and ends the auto-continue

Earlier readings of 141 and 31 bytes are the same fault. They were
attributed to three different causes at the time -- 141 as "128 bytes
plus a swallowed 13-byte CSW", 13 as "a CSW where data belongs" --
by fitting each number to whatever theory was current rather than
checking it. Recorded because the pattern matters more than the
arithmetic.

Where to look, most likely first:

1. **The `pending`-assert wait in `bulk_xfer()`.** Writing `XACT_B`
   with START does not assert `pending` in the same cycle -- the block
   registers its Wishbone inputs, so the write lands a cycle later.
   A guard for this was added and then reverted with the rest; it
   bounded its spin and fell through regardless, so a missed assert
   reads a stale status, and a stale `act_len` looks exactly like a
   short count.
2. **`act_len` accumulation across auto-continue packets** in
   `usb_xact.v`. Nothing has exercised an eight-packet transfer:
   control transfers are paced one packet at a time in software.
3. **Genuine packet loss** -- least likely, `bad` is zero everywhere.

The cheap discriminator is one line: print `msc.last_status` alongside
the short length. `ST_SHORT` means the hardware genuinely saw a short
packet; `ST_OK` with 13 bytes means the COUNT is wrong rather than the
transfer.

### Two result-register faults, found in simulation

Both are in `usb_host.v`, both make `XACT_S` report a result that is
not the one software asked for, and both are fixed. Neither shows up
as receive errors, because the transfer itself is fine.

- **One stale cycle per transaction.** `usb_xact.v` drops `busy` and
  pulses `done` on the same edge; the result registers latch on
  `done`, one edge later. `xact_pending` did not include `done`, so for
  that one cycle it read zero while `res_status`/`res_len` still held
  the previous transaction's values. A polling driver lands on it at
  a rate set by its loop timing and reads a plausible, wrong result --
  for a bulk data stage, the CBW's `ST_OK` and its length -- with the
  real data sitting correctly in the buffer. `xact_pending` now
  includes `x_done`.
- **Auto-poll completions overwrote software's result.** The latch
  excluded SOFs but not poll slots. Any HID poll finishing after
  software's transaction replaced its status and length with the
  poll's (usually `ST_NAK`, length 0), and raised `xact_done` for a
  transaction software never started. With a keyboard and mouse
  attached this happens every frame. Poll results already go to
  `poll_b` and the compat block; they no longer reach `XACT_S`.

`tb_usb_host.v` now has a monitor that checks, every cycle, that a
clear pending bit comes with software's own result -- it counted
exactly one violation per software transaction before the first fix
-- and case 10, a four-packet exact-multiple auto-continue IN (the
shape of a sector read, ending on `remaining == 0` with `ST_OK` rather
than on a short packet) run with a poll slot live. Case 10 also
clears suspect 2 below: `act_len` accumulates correctly across
auto-continue packets.

Suspect 1 below does not exist as described: the start-bit write sets
`sw_req` on the `W_DEC` edge, before the write is acknowledged, so a
read cannot follow it and see pending clear. The real window was at
the other end of the transaction.

### What the truncation actually was -- measured

`make test_usb_msc` (the real driver including `usbh_msc.c`, the real
gateware, a bulk-only SCSI model, and a live low-speed mouse poll on
the other port) was run with each fix alone and with neither:

| RTL result fixes | driver NAK-resume fix | result | short counts |
|---|---|---|---|
| no  | no  | 5 checks fail | 13, 44, 461 |
| no  | yes | 5 checks fail | 13, 141, 44 |
| yes | no  | 1 check fails (NAK over budget) | 461 |
| yes | yes | pass | -- |

The bench reproduces the exact hardware readings, 13 and 141, and
only the RTL fixes remove them. The driver's NAK handling was a
separate real bug: a sector that paused mid-read was re-requested from
offset 0 and came back as the remaining 448 bytes plus the 13-byte
CSW, 461. Both are needed.

The `last_status` discriminator answers **`ST_SHORT`**: the hardware
really did see a short packet, because the transport HAD desynced --
a CSW landed where data belonged, and the device model reports
out-of-sequence and duplicate OUT packets. "No desync" in the earlier
reading was wrong. The `EB 3C 90` bytes that made it look like a
truncated sector were most likely the previous read of sector 0 still
sitting in the landing area; rejecting short sectors (below) stops
that from being mistaken for data again.

### Transmission errors on bulk IN

A single bad CRC on a data-in packet used to fail the command, and
that was worse than it sounds: the device was still mid-data-phase, so
the CSW read that followed took sector bytes as a status wrapper
(`ST_BABBLE`) and every later command was one step out -- the same
class of failure as the truncation above, from one flipped bit.

`bulk_xfer()` now retries a bulk IN that ends in `ST_CRCERR` or
`ST_TIMEOUT`, up to three consecutive times (USB 2.0 8.5.2), resuming
from `act_len` exactly as after a NAK. That is valid because the engine
stops on the bad packet without advancing: no ACK was sent, so the
device resends the same packet with the same toggle, and the toggle
XACT_S reports is that one.

`test_usb_msc` covers it with a CRC-corruption hook in the device
model (`msc_crc_bad`, `msc_crc_at`): one bad packet after three good
ones in a sector, and two in a row on a first packet. Without the
retry both fail and the next read desyncs; with it all pass. The model
also now treats a non-ACK after its data as the host's next token,
which it previously swallowed.

Since then (round 6): more than three strikes fails the command and
runs Reset Recovery, and `usb_xact.v` checks the IN data toggle -- see
[STALL and Reset Recovery](#stall-and-reset-recovery) and
[The IN data toggle check](#the-in-data-toggle-check). OUT is still
not retried: a lost handshake there is resolved by the device ignoring
a repeated toggle, and nothing exercises it yet.

### Fmax after these changes

**Superseded -- read the update at the end of this section.**

`make usb_fmax` could not be run on the reference toolchain. With
YoWASP nextpnr (which needs `--ignore-loops` for the TRNG ring
oscillators), `CLK_48` was 51.86 / 51.37 MHz before and 50.55 / 49.70
MHz after, on seeds 1 and 2. In every run the critical path is the
same and is not in the USB block: arbiter address through `montmul`
into the Ethernet MAC's LUT-RAM buffer read mux (`wbs_ethmac0_i`
`rxbuf`/`txbuf`) to its `wb_dat_o`, over 80% routing. The USB changes
move placement, not that path. It is the SoC's real margin limit and
the thing to fix if margin gets tight; confirm the numbers with
`make usb_fmax` on the reference tools.

**Update.** On the reference toolchain the build with these changes
measured 48.89 MHz against the 55.07 recorded here earlier, which
looked like a USB regression. It was not established as one:

- Only `rtl/usb/usb_host.v` among the changed files is synthesised,
  and its change is two gate inputs. Standalone, `usb_host` went from
  2152 to 2156 LUT4 and 144 to 150 PFUMX, one L6MUX21 fewer, same
  flops, same DP16KD.
- On the second toolchain the unchanged RTL measured 51.86, 51.37 and
  48.10 MHz on seeds 1-3, and the changed RTL 50.55, 49.70 and 51.57.
  The ranges overlap entirely. Placement alone moved the same design
  by almost 4 MHz, so 55.07 was one favourable run, not the design's
  margin.
- Every captured critical path ran into the Ethernet MAC's buffer
  read mux, because its buffers had been inferred as LUT RAM. That is
  fixed upstream (`rtl/ethmac_rmii.v`, now BRAM), along with a VRAM
  attribute change that freed about 20 BRAM blocks. After that the
  reference toolchain measures **56.50 MHz** with all the USB changes
  in.

Two things carried forward. A single-seed Fmax cannot judge a small
change: use `make usb_fmax_sweep` (below, in Testing) and compare
minimums. And an explanation of where the critical path lies is not a
measurement of whether a change cost timing -- that was argued here
before it was measured, the pattern this document warns against.

### The transaction engine has one owner

There is one transaction engine and two kinds of user. Enumeration
runs from the ISR -- IRQ 9 on every completion, and the ktimer. Mass
storage runs in process context and blocks, inside FatFs, which holds
off the scheduler but **not interrupts**. So the ISR can preempt a
storage command between any two register accesses, and the only guard
enumeration had was "is a transaction in flight right now". Between two
bulk transactions the engine is idle for a moment, and `XACT_DONE`
raises the interrupt at exactly that moment. Two failures followed:

- The ISR started a control transaction in the gap, and the storage
  side then read that transaction's result as its own -- or the other
  way round, and the other device's enumeration consumed a bulk
  result.
- An ISR landing between `bulk_xfer()`'s write of `XACT_A` and its
  write of `XACT_B` overwrote `XACT_A`, so the bulk transfer went out
  with the other device's address and PID.

Today it needs a device enumerating during disk traffic. Phase 4 puts
hub control traffic on the bus continuously, which would make it
routine.

**The rule** (`usbh.h`): `z_usbh_bus_reserve()` sets a flag that stops
the enumeration side *starting* a control transfer -- the single launch
point is `CS_SETUP` in `ctrl_step()` -- then waits until no transfer
already started is still on the bus, and the caller owns the engine
until `z_usbh_bus_release()`. `scsi_cmd()` holds it from CBW to CSW.
No interrupt masking is needed: the flag is set before the wait, and
the ISR runs to completion between any two of the waiter's reads.
Enumeration just pauses while a command runs.

The wait is bounded in kernel ticks, about 200 ms. The first version
counted loop passes, and a pass that short-circuits past the bus read
costs nothing -- in co-simulation it gave up in zero simulated time.
The consequence of the bound: if a device being enumerated NAKs a
control transfer for longer than that during disk traffic, the storage
command fails ("bus busy") rather than waiting. The failure message
lists each port's enumeration and control state.

**How it was found and tested.** The co-simulation ran each storage
operation to completion inside one step and polled only between
steps, so it could not interleave the two by construction.
`$usbh_irq(every, tick_every)` in `usbh_vpi.c` now runs `z_usbh_poll()`
from inside the register accessors during storage operations -- every
Nth access, as an ISR preempts between two accesses -- and advances
the tick every Mth. With the mouse replugged during 60 reads, the old
driver failed a read and the mouse never bound; with the rule, all 60
reads are exact and the mouse binds.

### STALL and Reset Recovery

`scsi_cmd()` follows BOT 6.6-6.7:

| What happened | What the host does | Result |
|---|---|---|
| Data phase STALLed | CLEAR_FEATURE(HALT) on that endpoint, read the CSW | FAIL, transport in step |
| CSW read STALLed | clear the IN halt, read the CSW once more | as the CSW says |
| CBW not accepted | Reset Recovery | ERR |
| Data phase failed part-way (more than three strikes, babble, timeout) | Reset Recovery -- the device is still mid-phase, so a CSW read would take data as status | ERR |
| CSW missing, short, bad signature or wrong tag | Reset Recovery | ERR |
| bCSWStatus 2, phase error | Reset Recovery | ERR |
| bCSWStatus 1 | nothing | FAIL |

Reset Recovery (BOT 5.3.4) is the class reset (`bmRequestType` 0x21,
`bRequest` 0xFF, to the interface) then CLEAR_FEATURE(ENDPOINT_HALT) on
both bulk endpoints. Clearing a halt resets that endpoint's toggle to
DATA0 on the device, and the host's copy follows. These are the only
control requests the storage driver sends itself, over the engine it
already holds, from a SETUP area at buffer offset 0x260. Every recovery
prints a line whatever `msc_verbose` says. A port with nothing
connected is not recovered.

`test_usb_msc` covers a READ past the end of the medium with the
device STALLing (one halt cleared, no reset, next read exact), a CSW
with a bad signature (one class reset, two halts cleared), and four bad
CRCs in a row (reset, next read exact).

### The IN data toggle check

A device that misses the host's ACK resends its last data packet with
the same DATA0/DATA1. The host already has it, and only the toggle
tells the resend from new data. `usb_xact.v` did not look: it took the
resend as the next packet, duplicating 64 bytes and leaving the device
a packet behind for the rest of the transfer.

Now a data packet whose PID matches the toggle the host has moved past
is ACKed -- so the device moves on -- and discarded: the SIE latches
the PID before the first payload byte, so its bytes never reach the
buffer or the babble count. The engine then asks again, charged to the
NAK budget, so a device that never moves on ends in `ST_NAK` and
software resumes it as it would a busy device. Two wires and one
branch; no new registers.

`test_usb_msc`'s lost-ACK case has the model ignore two ACKs; the read
is exact with no recovery needed. `test_usb` covers control transfers,
auto-continue, both speeds, PRE and auto-poll with the check in place.

**Hardware risk:** keyboards and mice now have their toggles checked
too. A device that does not alternate DATA0/DATA1 properly would lose
every second report. Check both on hardware. Fmax has not been
measured with this change; it is small, but it is in the engine.

### Known-good but reverted -- now reapplied

Both changes below are back in `usbh_msc.c`, and `test_usb_msc` passes
with them. The history is kept because the lesson is.

Three changes were made after the working directory listing and rolled
back together, because they moved the failure earlier -- from "opening
a file" to "mounting at all" -- on a diagnosis that turned out to be
wrong. Two are worth reapplying once the truncation is understood:

- **Always drain the CSW.** `scsi_cmd()` currently returns from a
  failed data stage without reading the status wrapper. Bulk-only
  transport is a strict three-phase sequence and the device still owes
  a CSW; leaving it in the pipe puts the stream one transaction out of
  step permanently, and every later command reads the previous one's
  status. This is a genuine latent bug.
- **Reject short sector reads.** A device that promises 512 bytes and
  delivers fewer leaves the rest of the buffer holding whatever the
  previous command left there. Handing that to FatFs as file content
  is worse than failing.

### Known issues

Seen on hardware, not understood, and parked because each is rare and
recovered from. Pick one up if it becomes a real problem; the evidence
so far is here.

1. **Occasional unanswered transactions behind a hub -- probably the
   same cause as item 4, now fixed; watch `failed requests`.** Many of
   these were hub requests resumed after a NAK and then judging another
   device's result (round 19); that no longer happens. Whether all of
   them were is not proven. If `lsusb` shows `failed requests` climbing
   on a quiet bus, this is the item to reopen. The original notes:
   **Occasional unanswered transactions behind a hub.** A SETUP to the
   hub not ACKed, a descriptor read right after a port reset timing
   out -- zero NAKs, clean wire (10432 receptions, 4 bad), all
   recovered by retries. Hub 05e3:0608 and 0bda:5411 alike. The keyboard
   (04d9:1203) typically needs one or more enumeration retries. Since
   round 16 these no longer print a line each: `lsusb` shows
   `failed requests N` on the hub, and a line appears only when a hub is
   given up. No theory yet. Already ruled out: collisions and host
   timing on the wire (the contention monitor, and captures showing the
   host's packets clean), frame overruns (fixed, round 13), NAK retry
   pacing (fixed, round 14).
2. **The hub occasionally drops a device's port.** Seen once on the
   stick during the first `usbmount`: every request to it timed out,
   Reset Recovery included, then `usb msc: device removed` -- the hub
   reporting the port changed, most likely disabled -- and after
   re-enumeration it worked. A hub may disable a port it judges to be
   babbling (USB 2.0 11.8); nothing shows why it did here.
   `usbmount` now waits for the drive to come back and retries once.
3. **`usbcap` can capture the wrong transaction.** A window sometimes
   starts at a transaction launched before the one that fails, so the
   failing one lands late or outside it. Reproduced in co-simulation
   with the real `rtl/probe.v` (`Z_USBH_COSIM_PROBE`): arm writes arrive
   ~15 us before the transaction they were meant for reaches the wire.
   Not fixed; every capture's header now names the failing PID so a
   mismatch is at least visible. `tools/usbcap.py` itself is sound.
4. **FIXED (round 19, confirmed on hardware). Keyboards unreliable
   behind a hub, even alone.** Both a Holtek (04d9:1203) and a Cherry
   (046a:c099) keyboard failed `get-cfg` with "TIMEOUT (0 naks)", and
   requests to the hub itself failed only while a keyboard was present.
   The mouse did not. Round 18 blamed the Holtek's clock -- measured at
   +1.75%, outside the +-1.5% low-speed limit -- but the Cherry failed
   identically, so that was at most a contributor, not the cause.

   The cause found in round 19 is a stale read in the control engine.
   Both keyboards have two interfaces, so a 59-byte configuration
   descriptor -- eight low-speed packets, which the end-of-frame guard
   (round 13) splits across frames almost every time; the mouse's 34
   bytes usually fit. A split transfer resumes through `CS_DATA_RUN`,
   and on its last packet that handed over to `CS_STATUS`, which judged
   `XACT_S` on the NEXT call -- after this device had released the
   engine, so another device's result could be sitting there. The
   keyboard's `get-cfg` then inherited a hub request's timeout, and a
   hub request resumed after a NAK inherited the keyboard's. `CS_STATUS`
   also re-entered itself when all the data was already in, re-reading
   its own NAK and counting the length again. Now the status stage is
   launched at the point the data stage is known complete
   (`ctrl_send_status()`), so no state judges a result it does not own.
5. **Devices behind a hub intermittently stop answering (rounds 20-27) --
   not seen since round 27; cause not identified.** With the round-27
   build, keyboard and mouse enumerated first time on every plug across
   multiple reboots, at the default timings and at a 12 us low-speed
   timeout alike. But round 27's defaults reproduce round 26's constants
   exactly (A_TUNE reset 0x041414af = 1400 / 160 / 160 clocks and 4
   bits), and round 26 failed. The one difference is a new place and
   route. So either the fault is build-dependent -- a marginal timing
   path in the gateware, on or near the USB pins, that one layout hits
   and another does not -- or it is intermittent and in a good phase, as
   it seemed to be once before (after round 19). **If it returns:** first
   note whether it came with a new bitstream. If so, compare the
   builds' timing reports around `rtl/usb/` and the USB I/O, and check
   the pin inputs' synchronisers and constraints before looking at
   protocol again. The recovery counters stay in as the monitor: `lsusb`
   `ports disabled by the hub N, recovered M` and `recovered=n`, and the
   `failure with port enabled` line. `usbtune` stays, in
   [debug builds](#debug-build), for sweeping low-speed timings. **Round 29 acted on the build-dependence
   proactively:** D+ and D- are now registered in the pads' own I/O
   cells (ODDRX1F out, IDDRX1F in), so the pin-to-flop paths are fixed
   by the silicon and matched between the two lines in every build --
   see "Pin registers IN THE I/O CELLS" in `usb_host.v`. A failure
   after that would not be routing skew on the USB pins. Round 27 notes: Round 26's one-PRE'd-transaction-per-frame rule did **not**
   fix it (hardware: failures continue, now mostly with the port still
   enabled); it stays, as the reference hosts' discipline. A closer
   reading of Pico-PIO-USB and TinyUSB's RP2040 driver found every part
   of a low-speed transaction through a hub matching this host in kind:
   PRE framing and polarity, handshakes, enumeration delays (TinyUSB:
   20 ms reset wait, 10 ms recovery), NAK pacing (RP2040: 300 us). Four
   timings differ in size only -- the J after a PRE (4 FS bits, the spec
   minimum), the low-speed response timeout (29 us; Pico 12), the
   turnaround inside a transaction and the gap after a low-speed packet
   (3.3 us each). Round 27 makes them adjustable at run time (`usbtune`,
   register A_TUNE, reset to the old values) so they can be swept on
   hardware without a rebuild per value. Round 26 notes: Round 25's recovery re-enumerated the keyboard
   four times and it failed every time, so it could not be the answer.
   Reading Pico-PIO-USB -- TinyUSB's software full-speed host, the design
   closest to this one -- showed a scheduling rule this host broke: its
   frame loop sends the SOF and then **at most one transaction per
   endpoint per frame**. A low-speed device behind a hub gets one PRE'd
   transaction (token, data, PRE'd handshake) a frame; a control
   transfer's SETUP, each data packet and the status stage go in
   separate frames; a NAK waits for the next. This host ran several
   PRE'd transactions back to back in one frame -- every multi-packet
   data stage auto-continued -- and that is exactly where devices
   failed: `get-desc` and `get-cfg` data stages (first packet never
   arriving, the hub disabling the port half the time), while
   single-packet `get-desc8` and HID polls (one transaction every 10
   frames) worked. ESP-IDF and TinyUSB's DWC2 fix hit the same limit
   ("can't handle two transactions with preamble in one frame") and
   space low-speed transactions one per frame. **Round 26:** the
   scheduler starts at most one PRE'd transaction per frame
   (`pre_done` in `usb_host.v`), and a PRE'd request never re-issues
   within a frame -- it ends with its progress, as the end-of-frame
   guard does, and software resumes it next frame. Only while frames
   run. The round-25 recovery and its counters stay, as the check.
   Round 25: Round 24-25 findings: our PRE + ACK
   after low-speed data is clean on the wire (`usbcapok`), closing the
   LKML lead; the mouse measures -0.16% against our clock, so there is
   no gross clock error; there is no pattern across cold, warm, SRAM or
   flash boots; and a device can complete a clean transfer and have its
   port disabled by the hub moments later. **Round 25 recovers it the
   way Linux does:** when a port is found disabled by the hub
   (C_PORT_ENABLE), the attempt is refunded and the device reset and
   re-enumerated, up to 16 times per device, with a line `hub H port P
   disabled by hub (port error), re-enabling (n)`. **This hides the
   symptom, not the cause.** To see whether it continues: `lsusb` shows
   `ports disabled by the hub N, recovered M` per hub and `recovered=n`
   per device; failures with the port still enabled -- the other half,
   not recovered -- print `hub H port P failure with port enabled`.
   Earlier findings: Round 22 update, first: the NAK-retry theory below was
   **falsified** -- with hardware NAK retries off for hubs by default,
   keyboard and mouse still failed. A capture of a failure on that build
   then showed the host's side clean -- a correct PRE, the hub setup gap,
   a low-speed IN to the keyboard's address with a valid CRC5, a 2-bit
   low-speed EOP -- and **no answer at all**, the receiver never
   starting (`rx started +0`). A device must answer an IN on endpoint 0,
   so the keyboard never received the token. The CDC device (full
   speed) also stopped answering mid-session. The two reasons a hub
   stops delivering to a connected port are that it has **disabled** the
   port (babble or loss of activity at end of frame) or the port is
   **suspended** (a low-speed device with no keep-alives for 3 ms).
   **Round 22's answer, from the hub itself:** in about half the failures
   the hub had DISABLED the port -- `status 0301 change 0002`: connected,
   low speed, not enabled, with C_PORT_ENABLE set, which USB 2.0
   11.24.2.7.2.2 sets only when the hub disables a port for a port
   error; 11.8.1 defines those as babble or loss of activity, a device
   still active at the hub's EOF2 point. Both keyboard and mouse. In the
   other half the port was still enabled. Round 23 adds a gateware
   counter of frames in which port 0's line is not idle in the last 4 us
   before our frame tick (lsusb: `frame(s) with port 0 busy at end of
   frame`), to settle whether anything on our side reaches the end of a
   frame.
   **Round 23's counter read 0** through a session of repeated
   hub-disabled ports: port 0 is idle in the last 4 us of every frame, so
   our frame timing is ruled out -- nothing of ours, and nothing
   answering us, reaches the hub's end of frame. Other implementations
   agree the guard is sound: OHCI's LSThreshold (Linux programs 0x628,
   ~131 us) is the same rule, ours is 210 us. Linux sees this symptom too
   ("port N disabled by hub (EMI?), re-enabling", `hub.c`, whose comment
   says it happens with mice) and recovers by re-running connect change;
   it sees it rarely, we on half of low-speed plugs. An LKML report with
   our topology -- full-speed host, low-speed mouse behind a hub, the hub
   resetting it -- traced it to the host sending bytes after its
   low-speed ACK. Failures here tend to follow a successful low-speed
   data packet, i.e. our PRE + ACK, which had never been captured. Round
   24 adds `usbcapok`: the probe freezes on the first successful IN with
   data from a low-speed device behind a hub, capturing the device's
   packet and our handshake after it.
   Round 22 had the hub driver read and log a port's status whenever a
   device behind it fails (`hub N port P after a failure: ... enabled
   E suspended S`), which answers which it is. The original note:
   **Low-speed devices behind a hub fail intermittently again (round
   20-21) -- being tested.** After round 19 both the keyboard and the
   mouse still failed some enumerations behind the hub (timeouts, bad
   receives, `device failed, port disabled`); the USB code was unchanged
   from round 19, so round 19's clean run was likely a good run. Then,
   on hardware, **both became completely reliable while `usbcap` was
   running** (11 keyboard and 5 mouse plugs, all first time). Capture
   mode's only protocol effect is NAK budget 0 on every transaction on
   port 0; low-speed devices behind the hub already had it, so the
   difference is the **hub's own control endpoint**. A hub NAKs requests
   while busy with a port reset -- exactly when a device behind it
   enumerates -- and the hardware re-sent each one every ~5 us, four
   times (seen in captures). Round 21 makes budget 0 the default for
   hubs and everything behind them; `usbnak 3` restores the old
   behaviour on the same build, to compare. Also reported and not
   explained: plugging full-speed devices first sometimes made the
   low-speed ones reliable too.
6. **Low-speed devices through a hub have no hardware NAK retries**
   (round 14, `usbnak`), so each NAK costs a frame. Fine for HID and
   enumeration; noted in case a low-speed device ever needs throughput.

### Also outstanding

(Pruned in round 34: the keyboard behind a hub, `make usb_fmax` after
the PRE fix and the toggle check, and keyboard and mouse with the
toggle check are all done on hardware -- see Known issues, item 4, and
the round log.)

- On hardware, still to exercise: unplug and replug with `/usb`
  mounted (including mid-copy), and a card reader with no card (it
  STALLs).
- One sector per SCSI command. Batching was tried (round 41) and
  reverted: see [Throughput](#throughput) for why, and what to measure
  first.
- A device NAKing a control transfer for over ~200 ms during disk
  traffic fails the storage command ("bus busy"). See
  [The transaction engine has one owner](#the-transaction-engine-has-one-owner).
- OUT transfers are not retried on a lost handshake.
- One drive at a time: `usbh_msc.c` holds a single device's state.

## Change log since the phase 5 handover

Each round lists what changed, where, and how it was verified. All of
it is simulation-verified only until the hardware check at the end.

**Round 1 -- result registers** (`rtl/usb/usb_host.v`,
`rtl/tb/tb_usb_host.v`)

- `xact_pending` now includes `x_done`; it read clear for one cycle
  while `res_*` still held the previous transaction's result.
- Auto-poll completions no longer overwrite `res_*` or raise
  `xact_done`.
- `test_usb` case 10 and the per-cycle result monitor. The monitor
  counted one violation per software transaction before the fix.
- Found while checking the handover's three suspects: the
  pending-assert race does not exist as described, and `act_len`
  accumulation is correct.

**Round 2 -- the truncation, measured** (`sw/os/usb/usbh_msc.c`,
`sw/os/usb/usbh.c`, `Makefile`, `rtl/tb/tb_usb_msc_cosim.v`,
`rtl/tb/tb_usb_device.v`, `rtl/tb/cosim/usbh_vpi.c`)

- A parallel session added `make test_usb_msc`, NAK resume in
  `bulk_xfer()`, and reapplied "always read the CSW" and "reject short
  sectors". Committed here as found.
- A two-by-two run of RTL fix against driver fix showed the hardware's
  13 and 141 byte results come from the round 1 faults; the NAK-resume
  bug is separate (461 bytes). Both are needed. Table in
  [What the truncation actually was](#what-the-truncation-actually-was----measured).
- `usbh.c`: `ctrl_soft_naks` widened to `uint16_t`; as a byte it never
  reached `CTRL_SOFT_NAKS` (1000), so a NAKing control stage retried
  forever.

**Round 3 -- transmission errors** (`sw/os/usb/usbh_msc.c`,
`rtl/tb/tb_usb_device.v`, `rtl/tb/tb_usb_msc_cosim.v`)

- `bulk_xfer()` retries a bulk IN after `ST_CRCERR`/`ST_TIMEOUT`, three
  strikes, resuming from `act_len`. One bad CRC previously desynced the
  stream for good.
- Device model: CRC corruption hook, and a non-ACK after data is held
  as the host's next token rather than swallowed.
- Fmax measured with a second toolchain; critical path is in the
  Ethernet MAC, not USB. See
  [Fmax after these changes](#fmax-after-these-changes).

**Round 4 -- packaging and docs**

- Changed files shipped as a zip that unpacks from the project root.
- This log; Testing section brought up to date; the truncation section
  marked resolved-in-simulation; "Also outstanding" updated.
- Corrected [Removal while mounted](#removal-while-mounted), which
  described unmount-on-unplug as existing; it does not. Added to
  "Also outstanding".
- `docs/filesystem.md` now lists the three volumes and where `/usb`
  comes from; `docs/flash_apps.md` no longer calls USB storage
  hypothetical.

**Round 5 -- re-evaluation on `403513e`**

- Hardware: FatFs reads a text file and an 11 KB PNG through `/usb`.
  Phase 5 reads are done; the phase table and status notes say so.
- Fmax: the apparent 48.89 MHz regression is written up in
  [Fmax after these changes](#fmax-after-these-changes). With the
  upstream Ethernet MAC BRAM fix, 56.50 MHz.
- `Makefile`: `usb_fmax_sweep`, which synthesises once and places
  several seeds.
- The round-4 files were confirmed present and unchanged in
  `403513e`. The reworked MAC passes `tb_ethmac_rmii.v` and
  `tb_ethmac_rmii_tx.v`.

**Round 44 -- debug tools behind `USBH_DEBUG`; kernel space** (on `18f8a99`)

- Reported: kernel.bin 252,844 bytes, 9,300 free of 262,144.
- `USBH_DEBUG` (off by default; `make USBH_DEBUG=1`; always on in
  co-simulation): `usbcap`/`usbcapok`/`usbcapd`, `usbtune`, `usbnak`,
  `usbidle`, `usbbuf`, `usbcdc`, `usbbench` and their code, `lsusb`'s
  detail, the per-attempt fields in the device table, the storage
  driver's verbose messages (`msc_verbose` a constant 0, so the
  compiler drops them) and the `usbbench` counters. See
  [Debug build](#debug-build).
- Not debug: the report-descriptor parser reads the packet buffer
  directly (`z_usbh_hid_rdesc_parse(addr, ...)`), dropping a 400-byte
  static copy; re-checked against the four test descriptors.
- Measured with xPack `riscv-none-elf-gcc` 15.2.0-1: 242,272 bytes
  (19,872 free) by default, 252,132 with `USBH_DEBUG=1`. Both build; the
  co-simulation module builds and links.
- `sw/os/Makefile`, `sw/os/sh.c`, `sw/os/fs/sdbench.c`, `sw/os/usb/usbh.c`,
  `usbh.h`, `usbh_int.h`, `usbh_hid.c`, `usbh_msc.c`. Docs: the tools marked
  as debug-build throughout, `docs/user_input.md`.

**Round 43 -- multi-sector transfers reverted** (on `18f8a99`)

- Hardware (reported): ~8% faster at best, and a failed run -- reset
  recovery failed, then the hub dropped the stick's port (Known issues,
  item 2), in layer 1, a one-sector-per-command path. Reverted
  regardless: not worth the complexity in the write path.
- `sw/os/usb/usbh_msc.c`: as on `main`, one sector per command, plus
  only the `usbbench` counters (`z_usbh_msc_cmds`, `_naks`, `_xacts`).
  Kept: `usbbench` (round 40), the counters and their report (round
  42), the device model's transfer-length support and the 4-sector
  checks (round 41). See [Throughput](#throughput).

**Round 42 -- where the data-phase time goes** (on `18f8a99`)

- Hardware (reported): round 41 cut layer 2 to 128 commands but only
  to 206 KB/s: ~1.1 ms per command, ~2.1 ms per sector of data -- the
  data phase, not command overhead, is the limit.
- `sw/os/usb/usbh_msc.c/.h`: `z_usbh_msc_naks`, `z_usbh_msc_xacts`,
  counted in `bulk_xfer()`. `sw/os/fs/sdbench.c`: `usbbench` prints bulk
  requests and NAKs per layer. See [Throughput](#throughput). Kernel only.

**Round 41 -- multi-sector transfers** (on `18f8a99`)

- Hardware baseline (round 40, `usbbench`): 187 KB/s, ~2.7 ms a sector,
  ~80% per-command overhead.
- `sw/os/usb/usbh_msc.c`: one READ(10)/WRITE(10) per run of up to 32
  sectors; the data phase chunked through the 512-byte landing area to
  or from host memory. See [Throughput](#throughput).
- `rtl/tb/tb_usb_device.v`: the MSC model honours the transfer length
  (it served one sector per command whatever was asked).
  `rtl/tb/cosim/usbh_vpi.c`, `rtl/tb/tb_usb_msc_cosim.v`: 4-sector
  read and write-then-read operations and checks. `test_usb_msc`: 75
  checks pass, SOFs on time, no contention.
- "Also outstanding": the one-sector-per-command item removed.

**Round 40 -- `usbbench`** (on `b80c4f2`)

- `sw/os/fs/sdbench.c`, `sdbench.h`: `sh_usbbench()` -- `sdbench`'s layers
  1-3 for drive 2 (`/usb`), sharing its buffer, timing and report
  format (kernel .bss is flash image; no second buffer). Layer 3 is the
  existing `sdb_layer3()`, now told which statistic to print.
  `sw/os/usb/usbh_msc.c/.h`: `z_usbh_msc_cmds`. `sw/os/sh.c`: `usbbench`.
  See [Throughput](#throughput). Kernel only.

**Round 39 -- phase 6 done; docs checked** (on `b80c4f2`)

- Hardware (reported): the scroll wheel works on the Microsoft
  045e:0737 in report protocol. Phase 6 done.
- Checked `USB_HID` builds against rounds 34-38: the older core's mouse
  register has a constant-zero top byte, so no wheel events; the
  kernel USB driver is inert without its controller (`usbh_present`);
  lock state and Caps Lock are shared code and work; LEDs cannot.
- Docs: phase table; the wheel note's final shape; `zwm.h`'s
  `Z_WM_WHEEL` comment (still said "boot report's fourth byte");
  `docs/window_manager.md` -- `Z_WM_WHEEL` and the missing `Z_WM_MOUSE`
  in the app-protocol table, and a bullet; `docs/user_input.md` -- the
  older core's behaviour for lock keys and LEDs. Docs and comments only.

**Round 38 -- the wheel only where the descriptor says so** (on `b80c4f2`)

- Point raised: the HID boot mouse report is three bytes by the spec;
  a fourth in boot protocol is a common convention, not a rule. Round
  35 counted byte 3 of any 4-byte report, boot protocol included.
- `rtl/usb/usb_host.v`, `usb_hid_compat.v`: poll mode bit 2 (`in_wheel`)
  -- byte 3 is counted only when set. `sw/os/usb/usbh_hid.c`, `usbh.c`:
  `z_usbh_hid_bind(..., wheel)` sets it only for a mouse put in report
  protocol after its descriptor showed the simple layout with a wheel
  (`MRD_REPORT`), before the slot is enabled. `test_usb` passes.
- Docs: `docs/user_input.md`, "Scroll wheel".

**Round 37 -- report protocol for wheel mice** (on `b80c4f2`)

- Hardware (reported): the wheel counter never moved for a Microsoft
  045e:0737 in boot protocol, with a 4-byte endpoint.
- `sw/os/usb/usbh_hid.c`: `z_usbh_hid_mouse_rdesc_len()` (from the HID
  descriptor) and `z_usbh_hid_rdesc_parse()` (short items; usage
  pages, sizes, counts, IDs, usage min/max; bit positions of buttons,
  X, Y, wheel). Unit-tested offline: a common wheel mouse is accepted;
  the HID spec's boot mouse (no wheel), the same wheel mouse with a
  report ID, and a 16-button 12-bit-axis layout are refused.
- `usbh.c`, `usbh_int.h`: `E_HID_RDESC` reads a mouse's report
  descriptor between SET_CONFIGURATION and SET_PROTOCOL, and chooses
  SET_PROTOCOL(1) for the simple layout with a wheel, (0) otherwise.
  `lsusb`: protocol and why, layout, register, last report on its own
  line (it was cut at 80 columns).
- Kernel only.

**Round 36 -- Num Lock off by default; wheel diagnostics** (on `403513e`)

- Hardware (reported): Num Lock on by default disabled the right-hand
  letter keys of a compact keyboard -- its firmware maps them to an
  embedded keypad while the host reports Num Lock. Now all locks start
  off (`hid.c`, `usbh.c`, `zkbd.h`, `docs/user_input.md`).
- Hardware (reported): the wheel did not scroll `term`. `lsusb` now
  prints, for a mouse, its register (wheel counter in the top byte),
  the poll length and the raw bytes of its last report, to tell a mouse
  that sends no byte 3 in boot protocol from an event that does not
  arrive. Kernel only.

**Round 35 -- scroll wheel** (on `403513e`)

- Research: Linux `usbmouse.c` (`REL_WHEEL` from `data[3]`), TinyUSB
  `hid_mouse_report_t` (byte 3 wheel, byte 4 pan). Boot-report byte 3
  is the wheel by convention; no descriptor parse needed.
- `rtl/usb/usb_hid_compat.v`: `wheel_acc`, a signed 8-bit accumulator
  of byte 3, only for reports of four bytes or more (`in_len`), in
  `reg_mouse[31:24]` (hardwired zero until now). `usb_host.v` passes
  `cap_idx` as `in_len`. `test_usb` passes.
- `sw/common/zwm.h`: `Z_WM_WHEEL` (121), signed notches, not
  coalesced. `sw/apps/wm/wm.c`: `dispatch_wheel()`, same target rules
  as `dispatch_mouse()`. `sw/apps/term/term.c`: three lines a notch.
- Docs: `docs/user_input.md` (register, "Scroll wheel"), phase table.

**Round 34 -- lock keys and keyboard LEDs** (on `403513e`)

- `sw/os/hid.c`: Num/Caps/Scroll Lock state (`hid_locks`, Num Lock on
  at start), toggled on each press, carried in every key event at bits
  19:17, sent to the keyboards. `sw/common/zkbd.h`: lock usages,
  `Z_KBD_LOCK_*`, `Z_KBD_EV_LOCKS()`.
- `sw/os/usb/usbh.c`, `usbh.h`, `usbh_int.h`: `z_usbh_kbd_leds()`;
  `kbd_led_step()` in `E_RUNNING` sends `SET_REPORT` (output, one byte)
  to each keyboard, retrying three times per value; a newly bound
  keyboard first rolls Num -> Caps -> Scroll three times. 16-byte areas
  at 0x300/0x310 of the packet buffer, one per HID block, used only
  while a transfer is in flight. `lsusb` shows `leds=`.
- `sw/apps/wm/wm.c`: Caps Lock inverts Shift for a-z.
- Docs: `docs/user_input.md` (event format, "Lock keys and keyboard
  LEDs", limitations brought up to date for the new core); phase
  table; "Also outstanding" pruned of items done on hardware.
- Kernel and apps; compile-checked only.

**Round 33 -- the Blaustahl drops, and faster CDC polling** (on `403513e`)

- Hardware (reported): round 32 did not fix the editor. Cause, in the
  Blaustahl firmware: the editor grid uses `cdc_putchar()`, which drops
  on a full 64-byte FIFO. Patch for that repo:
  `blaustahl-reliable-bulk-output.patch` (4 call sites to
  `cdc_putchar_reliable()`, plus its missing declaration).
- `sw/apps/serial/serial.c`: poll on the next tick while data moves.
  See [CDC](#cdc). Apps only.

**Round 32 -- no more lost CDC data in `serial`** (on `403513e`)

- Hardware (reported): a Blaustahl's editor over `usbserial` drew with
  holes. Its firmware's VT100 use checked against `zvt100.c`: all
  supported, including deferred wrap. Cause: `serial` ignored
  `z_port_send()` refusing a send (8 pending, or a full mailbox) and
  dropped data it had already read from the device.
- `sw/apps/serial/serial.c`: read only while the port has room; hold
  and retry refused bytes. Also for UART1, where waiting can overrun
  the FIFO -- reported as before rather than lost silently. See
  [CDC](#cdc). Apps only.

**Round 31 -- docs brought up to date** (on `403513e`)

- Register map: DEBUG0/DEBUG1 (with the end-of-frame counter) and TUNE,
  previously only in the change log. [PRE](#pre): corrected -- every
  host low-speed packet gets its own PRE -- plus the one-per-frame rule
  and `usbtune`. [Pin registers in the I/O cells](#pin-registers-in-the-io-cells):
  new, under board notes. "What skew did not explain": the capture
  caveat. `docs/probe.md`: the USB example was the wiring that no longer
  packs; now `line0_o`, with why. `docs/filesystem.md`: `usbmount`'s
  retry. Docs only.

**Round 29 -- USB pins in the I/O cells; one `serial` app** (on `403513e`)

- `rtl/usb/usb_host.v`: D+ and D- registered in the pads' I/O cells --
  ODDRX1F (both halves the same, a single-rate output register) and
  IDDRX1F (the rising-edge sample) -- so the pin paths are fixed and
  matched between D+ and D- in every build, rather than wherever the
  placer puts the flops. The output enable gets one fabric register to
  stay aligned with the data. Plain registers in simulation
  (`ifndef SYNTHESIS`). `test_usb` passes (77). Why:
  [Known issues](#known-issues), item 5.
- **Round 30 fix:** the first hardware build failed to pack --
  "IDDRX1F D input must be connected only to a top level input" --
  because the logic probe (`sysctl.v`, `PROBE`) read port 0's D+ pin
  directly, a second load on the pad net. `usb_host.v` now exports its
  pad-registered samples of port 0 as `line0_o`, and the probe records
  those: exactly what the receiver sees, a clock after the pins.
  Verified with the real toolchain (YoWASP yosys 0.69 and nextpnr-ecp5)
  on a minimal top with mozart_ml1's USB pin sites (A9, A10, C8, B8;
  45k, CABGA256): the old wiring reproduces the error exactly; the new
  one packs, places and routes (72 MHz), each USB pin's I/O cell in
  `MODE = IDDRX1_ODDRX1` -- input and output register together in the
  pad. **Correction:** rounds 27 and 29 reported yosys checks (4
  tristates, 4 ODDRX1F/IDDRX1F, no multiple drivers) that never ran --
  yosys was not installed in the environment and the error was
  filtered out. The round-30 checks above did run.
- `sw/apps/serial/serial.c`: one app for UART1 and the USB CDC device,
  on `serial0`, a connection each; stays resident without UART1.
  `zconnect.c/.h`: `usbserial` connects to `serial0` with
  `Z_CONN_USBSERIAL_ARG`. `term.c`: auto-connect waits for `serial0`.
  The second binary is gone: the serial Makefile is back to upstream,
  `mkfatimg.py` no longer lists `apps/usbserial`. Docs: connections,
  config, terminal, ports, [CDC](#cdc).

**Round 28 -- item 5 closed as not reproduced** (on `403513e`)

- Hardware (reported): with the round-27 build, keyboard and mouse
  behind the hub enumerate first time on every plug across multiple
  reboots, at default and at 12 us timeouts. Round 27's defaults are
  identical to round 26's constants; only the place and route changed.
  [Known issues](#known-issues), item 5, records what to check if it
  returns. Docs only.

**Round 27 -- low-speed timings adjustable at run time** (on `403513e`)

- Hardware (reported): round 26 did not fix item 5.
- Compared Pico-PIO-USB and TinyUSB's RP2040 HCD in detail: no
  difference in kind; four timings differ in size.
  [Known issues](#known-issues), item 5.
- `rtl/usb/usb_host.v`: A_TUNE (0x04a) -- low-speed response timeout,
  turnaround, gap after a low-speed packet, J after a PRE; reset value
  0x041414af reproduces the old constants. `usb_xact.v`, `usb_sie.v`
  take them as inputs. `usbh.c`/`usbh.h`/`usbh_hw.h`/`sh.c`: `usbtune`.
- Checked by compiling only (iverilog, yosys `check -assert`), by
  request -- hardware is the test.

**Round 26 -- one PRE'd transaction per frame** (on `403513e`)

- Hardware (reported): with round 25's recovery the keyboard was
  re-enumerated four times and failed every time.
- Read Pico-PIO-USB (`pio_usb_host.c`, `pio_usb.c`): one transaction per
  endpoint per frame; a PRE'd transfer never shares a frame. This host
  put several PRE'd transactions in one frame on every multi-packet
  data stage -- the failing case. [Known issues](#known-issues), item 5.
- `rtl/usb/usb_host.v`: `pre_done` -- at most one PRE'd transaction
  scheduled per frame (software requests and polls). `usb_xact.v`:
  `pre_gate` -- a PRE'd request ends instead of re-issuing. Both only
  while frames run, so `test_usb` case 7 (frames off) is unchanged.
- `test_usb` passes (77). `test_usb_hub` passes (40 checks, 0 model
  errors): the low-speed mouse behind the hub enumerates with every
  multi-packet read split one PRE'd transaction per frame, including
  re-enumerating during 40 stick reads; SOFs within 62 ns; no bus
  contention. Needs a gateware rebuild.

**Round 25 -- Linux-style recovery, with the counts kept** (on `403513e`)

- Hardware (reported): no pattern across cold/warm/SRAM/flash boots; a
  successful low-speed read followed by repeated hub-disabled ports.
  `usbcapok`: our PRE + ACK is clean. Mouse -0.16% vs our clock.
- `usbh_hub.c`, `usbh_int.h`, `usbh.c`: a port found disabled by the hub
  (C_PORT_ENABLE) refunds the failed attempt, takes back a port already
  given up on, and lets the retry path re-enumerate -- up to 16 times
  per device. Counted per hub (`ports disabled by the hub N, recovered
  M`) and per device (`recovered=n`) in lsusb. Failures with the port
  still enabled are logged, not recovered. [Known issues](#known-issues),
  item 5.
- Kernel only; host-compiler checks only.

**Round 24 -- capturing our own ACK** (on `403513e`)

- Hardware (reported): the end-of-frame counter stays 0 while the hub
  keeps disabling low-speed ports -- our frame timing is ruled out.
- Research: OHCI's LSThreshold, Linux's "disabled by hub (EMI?)"
  recovery, an LKML report of a full-speed host sending bytes after its
  low-speed ACK. [Known issues](#known-issues), item 5.
- `usbh.c`, `usbh.h`, `sh.c`: `usbcapok`, a capture that freezes on a
  successful low-speed IN with data. `usbh_vpi.c` follows the new
  `z_usbh_cap_start(mode)`. Kernel only.

**Round 23 -- measuring the end of the frame** (on `403513e`)

- Hardware (reported): the hub disables the low-speed devices' ports
  itself (C_PORT_ENABLE) in about half the failures -- a port error,
  meaning activity at its end of frame. [Known issues](#known-issues),
  item 5.
- `rtl/usb/usb_host.v`: `dbg_eof`, frames with port 0 not idle in the
  last `EOF_ZONE` (4 us) before the frame tick, in DEBUG1[31:16].
  `usbh.c`: lsusb prints it. `test_usb` passes.

**Round 22 -- asking the hub** (on `403513e`)

- Hardware (reported): round 21's NAK change did not help -- theory
  falsified. A capture of a failure: clean PRE and IN, no answer, the
  receiver never started. CDC dropped mid-session again.
- `usbh.c`, `usbh_hub.c`, `usbh_int.h`: when a device behind a hub
  fails, the hub driver reads the port's status and logs connected /
  enabled / suspended and the change bits. [Known issues](#known-issues),
  item 5.
- `sw/apps/serial/serial.c` (usbserial): a failed read no longer ends
  the session unless the device is really gone.
- Hardware NAK budget 0 for hubs (round 21) is kept: it did not fix
  this, but it is how real hosts behave.

**Round 21 -- NAK retries off for hubs** (on `403513e`)

- Hardware (reported): keyboard and mouse behind the hub fail
  intermittently with the round-20 build (USB code identical to round
  19); both became completely reliable while `usbcap` was running.
- `usbh.c`: hardware NAK budget 0 for control transfers to a hub and to
  everything behind one (was: low-speed behind a hub only); `usbnak N`
  sets it. [Known issues](#known-issues), item 5. An experiment with a
  default, not a confirmed fix.
- Kernel only; host-compiler checks only.

**Round 20 -- the hub milestone recorded; CDC for apps** (on `403513e`)

- Hardware (reported): round 19 fixed the keyboards; keyboard, mouse,
  stick and CDC device all work behind the hub together.
- Docs: status, phase table, [The last fault](#the-last-fault-a-stale-result-on-resumed-transfers)
  with the list of faults found, Known issues updated.
- CDC for apps: `sw/apps/serial` builds a second binary, `usbserial`
  (`-DSERIAL_USB`), registered `usbserial0`. Syscalls
  `USBCDC_PRESENT/READ/WRITE` (`syscalls.def`, appended),
  `sw/os/usbcdcapi.c/.h`, `sw/common/zusbcdc.h`; read/write hold the
  scheduler (`kernel.c`). `term`/`repl`: a `usbserial` connection kind
  (`zconnect.c/.h`, `term.c`, `repl.c`). The FAT image carries
  `apps/usbserial`. docs/connections.md, config.md, terminal.md,
  ports.md updated. See [CDC](#cdc).
- Kernel and apps; no gateware change. Host-compiler syntax checks only
  -- no RISC-V toolchain here, and no simulation (none covers apps).

**Round 19 -- a stale read on resumed control transfers** (on `403513e`)

- Hardware (reported): a second keyboard (Cherry 046a:c099) fails
  behind the hub like the first; both work on a root port and with the
  old `usb_hid` core. The out-of-spec clock was not the cause.
- `usbh.c`: `ctrl_send_status()`; `CS_DATA_RUN` and `CS_STATUS` launch
  the status stage directly instead of leaving `CS_STATUS` to judge a
  result the device no longer owns. [Known issues](#known-issues),
  item 4.
- Kernel only. One targeted co-simulation (a NAKing low-speed mouse
  behind the hub, which goes through the changed path): passes.

**Round 18 -- CDC works; the keyboard measured out of spec** (on `403513e`)

- Hardware (reported): CDC-ACM works. The Holtek keyboard is unreliable
  behind the hub even alone.
- Measured from an existing capture: the keyboard transmits at +1.75%,
  outside the +-1.5% low-speed limit. [Known issues](#known-issues),
  item 4. No code change.

**Round 17 -- full configuration descriptors** (on `403513e`)

- Hardware (reported): the CDC device (16c0:05e1, composite, 98-byte
  configuration) was not recognized; everything else behind the hub
  worked, with `failed requests 0` on the hub.
- `usbh_int.h`, `usbh.c`: `cfg_raw` holds the whole configuration
  descriptor (up to 200 bytes), not the first 64. See [CDC](#cdc).
- Kernel only. The CDC probe checked offline, host-compiled, against
  the device's descriptor.

**Round 16 -- tidy-up; CDC-ACM** (on `403513e`)

- `usbh_hub.c`: a failed hub request is counted, not printed; `lsusb`
  shows `failed requests N`, and a line appears only when a hub is
  given up.
- `sw/os/fs/fs.c`: `usbmount` retries once, after waiting up to ~1.5 s
  for a dropped drive to be back.
- [Known issues](#known-issues): the unresolved problems in one place,
  with their evidence.
- `sw/os/usb/usbh_cdc.c`, `usbh_cdc.h` (new), `usbh.c`, `usbh.h`,
  `usbh_int.h`: CDC-ACM -- see [CDC](#cdc). `sw/os/sh.c`: `usbcdc`.
  Build files updated.
- Kernel only. No simulation: compiled with the host compiler in the
  co-simulation and kernel configurations.

**Round 15 -- working behind the hub** (on `403513e`)

- Hardware (reported): keyboard, mouse and stick all work behind the hub
  together. Residual intermittent timeouts, all recovered; the first
  `usbmount` failed once when the hub dropped the stick's port. See
  [Hardware: working behind the hub](#hardware-working-behind-the-hub).
- Docs only.

**Round 14 -- NAK retries paced** (on `403513e`)

- Hardware (reported): after round 13 the mouse and stick work behind
  the hub with the keyboard present; Fmax 58.34 MHz. The keyboard
  still fails, NAK then no response.
- `usbh.c`: NAK retries in `CS_DATA_RUN`, `CS_STATUS` and `CS_FINAL`
  wait out `CTRL_NAK_TICKS` before relaunching; hardware NAK budget 0
  for low-speed devices behind a hub. `usbh.h`, `sh.c`: `usbnak N`.
- Kernel only; no gateware change. One targeted co-simulation (a mouse
  behind the hub NAKing its first descriptor INs): enumerates first
  time through the new paths. Nothing else run, by decision.

**Round 13 -- frames** (on `403513e`)

- Stepped back from capture-by-capture debugging to the whole hardware
  record; one theory fits all of it. See
  [The cause: transfers ran across frame boundaries](#the-cause-transfers-ran-across-frame-boundaries).
- `rtl/usb/usb_host.v`: end-of-frame guard on the scheduler
  (`GUARD_FS`, `GUARD_LS`, `late_fs`, `late_ls`).
- `rtl/usb/usb_xact.v`: the same guard before every re-issue inside a
  request, ending it with `ST_NAK` (`reissue`).
- `rtl/tb/tb_usb_hub_cosim.v`, `rtl/tb/tb_usb_msc_cosim.v`: SOF-on-time
  check on every frame. Measured 207 us late before the fix, 0 after.
- `sw/os/usb/usbh.c`: probe access through `cap_rd`/`cap_wr`, so the
  capture code can run against the real probe in co-simulation, which
  is how its remaining fault was found. `rtl/tb/cosim/usbh_vpi.c`:
  operations to start and dump a capture.

**Verification at the end of round 13:** `test_usb` (77), `test_usb_hub`
and `test_usb_msc` (126 commands) pass; in both, every SOF started
within 62 ns of its frame tick, against 207 us before; no bus
contention. Not run this round, by decision: `test_usb_cosim`, the
margin sweep, `make usb_fmax` -- hardware is the faster test now.

**Round 12 -- three captures, tooling corrected** (on `403513e`)

- Hardware: three captures; see
  [Three more captures, and what they corrected](#three-more-captures-and-what-they-corrected).
  Round 11's "hub goes deaf" reading was wrong.
- `usbh.c`: the probe is armed at every transaction again; the capture
  header names the failing PID and gives the receive counters across
  it.
- `tools/usbcap.py`: marks the failing packet by PID; a one-sample SE1
  is reported as edge crossover, not two drivers.
- `rtl/tb/tb_usb_device.v`: `extra_turn_ns`, used to rule out a late
  reply as the keyboard's cause.

**Round 11 -- the first hardware capture** (on `403513e`)

- Hardware: `usbcap` froze on a data-stage IN to the hub, which never
  answered a clean token. See
  [The first capture: the hub goes deaf](#the-first-capture-the-hub-goes-deaf).
- `usbh.c`: capture windows keep recording across transactions, so a
  capture holds what came before the failure; `usbcapd` waits for a
  window still recording.
- `tools/usbcap.py`: marks the failing transaction.

**Round 10 -- a failed device poisons address 0; wire capture** (on `403513e`)

- Hardware (reported): a second hub behaves the same; requests to the
  hub fail on every port; the mouse fails whenever the keyboard is
  plugged in; repeated plugging makes it worse; root ports fine.
- `usbh.c`, `usbh_hub.c`, `usbh_int.h`: `on_addr0`, disabling the port
  of a device that failed for good, and one reserved address per device
  held until teardown. See
  [Hardware: a failed device poisons address 0](#hardware-a-failed-device-poisons-address-0).
  Reproduced first (`stall_set_addr` in the device model, 39 collisions);
  zero after.
- `usbh.c`, `usbh.h`, `sw/os/sh.c`: `usbcap` / `usbcapd`, capturing the
  first failing transaction on port 0 with the built-in probe.
- `tools/usbcap.py` (new): decodes it; checked against a simulated
  capture.
- `rtl/tb/tb_usb_device.v`: `stall_set_addr`. `rtl/tb/tb_usb_hub_cosim.v`:
  a device that fails on address 0 behind the hub, then the mouse.
- Still open: why the keyboard fails, and the hub-request failures.

**Verification at the end of round 10:** `test_usb` (77), `test_usb_cosim`,
`test_usb_msc` (126 commands, no bus contention), `test_usb_hub` (with
the failed-device case; no bus contention, zero model errors) and all
four `test_usb_margin` corners -- all pass. `tools/usbcap.py` decodes a
simulated capture correctly. The driver files compile clean in the
co-simulation and kernel configurations with the host compiler; `make
-C sw/os` and `make usb_fmax` have not been run here.

**On hardware for round 10:** mouse and stick behind the hub with the
keyboard also plugged in -- they should work even while the keyboard
fails; then `usbcap`, plug the keyboard into the hub, and after
"capture frozen" paste `usbcapd`'s output (or report that it works in
capture mode).

**Round 9 -- full-speed traffic colliding with low-speed** (on `403513e`)

- Hardware (reported): the keyboard still failed after round 8; hub
  requests failed with CRC errors and timeouts; the mouse failed
  whenever the keyboard was plugged in; the stick was unaffected.
- `rtl/usb/usb_xact.v`: `gap_limit` -- after a low-speed transaction,
  the next token on that port waits five low-speed bits. See
  [Hardware: full-speed traffic straight after low-speed](#hardware-full-speed-traffic-straight-after-low-speed).
  Found with a new wire-contention monitor before the fix; zero
  collisions after it.
- `rtl/usb/usb_host.v`: the low-speed keepalive waits for a quiet
  line, and the scheduler yields to it. Found by the same monitor in
  the storage bench, on a directly attached low-speed mouse.
- `rtl/tb/tb_usb_hub_cosim.v`, `rtl/tb/tb_usb_msc_cosim.v`: the
  contention monitor, with a check that it saw nothing.
- Round 8's retry-gap fix was reported as likely fixing the keyboard.
  It was a real bug, reproduced and fixed, but not the cause of that
  failure; the collisions above were.

**Verification at the end of round 9:** `test_usb` (77), `test_usb_cosim`,
`test_usb_msc` (126 commands, no bus contention), `test_usb_hub` (35,
no bus contention, zero model errors) and all four `test_usb_margin`
corners -- all pass. `make usb_fmax` has not been run on the RTL
changes of rounds 6-9 (`usb_xact.v`, `usb_sie.v`, `usb_host.v`).

**On hardware for round 9:** the keyboard alone behind the hub; then
keyboard, mouse and stick behind it together; keyboard and mouse on
root ports, where the keepalive fix also applies; `make usb_fmax`.
`lsusb` wire counters showing receive errors now point at a collision
the monitor does not cover.

**Round 8 -- a low-speed keyboard behind a real hub** (on `403513e`)

- Hardware (reported): behind hub 05e3:0608 a stick and a low-speed
  mouse work; a low-speed keyboard failed with NAK-then-timeout.
- `rtl/usb/usb_xact.v`: every token re-issue goes through `X_GAP`. See
  [Hardware: retries behind a hub](#hardware-retries-behind-a-hub).
  Reproduced in co-simulation first; `test_usb` cases 6 and 7 fail
  without the fix (7) or pass (6) exactly as the hardware did.
- `rtl/tb/tb_usb_host.v`: two NAKs on the descriptor IN in cases 6 and
  7. `rtl/tb/tb_usb_hub_cosim.v`: the mouse NAKs its first three
  descriptor INs during the initial enumeration.

**Verification at the end of round 8:** `test_usb` (77), `test_usb_cosim`,
`test_usb_msc` (126 commands), `test_usb_hub` (35, with the mouse
NAKing, zero model errors) and all four `test_usb_margin` corners --
all pass. `make usb_fmax` still has not been run on the round 6-8 RTL
changes.

**On hardware for round 8:** the same low-speed keyboard behind the
hub; then keyboard, mouse and stick behind it together; then
`make usb_fmax`.

**Round 7 -- phase 4, the hub** (on `403513e`)

- `sw/os/usb/usbh_hub.c` (new), `usbh_int.h` (new), `usbh.c`, `usbh.h`:
  the hub class driver and the refactor behind it. See
  [Hub class driver](#hub-class-driver).
- `usbh.c`: transaction-result ownership between devices, and
  `buftest` no longer writes into the storage sector buffer. See
  [Two core bugs the hub found](#two-core-bugs-the-hub-found).
- `rtl/usb/usb_sie.v`: PRE sent with a correct PID check field.
- `rtl/tb/tb_usb_device.v`: HUB=1 mode, `ext_reset`, and the four model
  fixes in [The hub model](#the-hub-model-and-what-it-corrected-in-the-device-model);
  trace lines now name the instance.
- `rtl/tb/tb_usb_hub_cosim.v` (new) and `make test_usb_hub`.
- `rtl/tb/cosim/usbh_vpi.c`: an operation that runs `lsusb` into the
  simulation log.
- `sw/os/Makefile` and the co-simulation targets build `usbh_hub.c`.
- The model reports a CRC failure on a DATA packet only when the token
  before it was addressed to that model. At +-2500 ppm the hub model
  failed the CRC on every one of the stick's 64-byte packets -- its
  simple receiver against another model's off-rate transmitter, on data
  that was never for it -- about 200 lines per run, burying anything
  real.

**Verification at the end of round 7:** `test_usb` (77), `test_usb_cosim`,
`test_usb_msc` (126 commands), `test_usb_hub` (35), all four
`test_usb_margin` corners, and `test_usb_hub` at 20 ns skew with
+-2500 ppm FS and +-15000 ppm LS -- all pass. The driver files compile
clean with the host compiler in both co-simulation and kernel
configurations (`-Wall -Wextra`); `make -C sw/os` has not been run with
a RISC-V toolchain.

**On hardware for round 7, in order:** `make usb_fmax` (`usb_sie.v` and
`usb_xact.v` changed); a hub alone, `lsusb` showing it and its ports;
keyboard and mouse behind the hub -- a low-speed device there is the
first real test of PRE; a stick behind the hub, `usbmount`, read and
write; unplug each device, then the hub, and replug.

**Round 6 -- unplug, bus ownership, recovery, toggle check** (on
`403513e`)

- Hardware (reported): WRITE then read-back over `/usb` works. Phase 5
  reads and writes are done on hardware; `msc_verbose` is now off.
- **Bus ownership** (`usbh.c`, `usbh.h`, `usbh_msc.c`): a race between
  the ISR's enumeration and a running storage command, over the one
  transaction engine. Reproduced by adding interrupt emulation to the
  co-simulation (`$usbh_irq`, `usbh_vpi.c`), fixed with
  `z_usbh_bus_reserve()`/`release()`. See
  [The transaction engine has one owner](#the-transaction-engine-has-one-owner).
  The first version of the wait counted iterations and gave up in zero
  simulated time; now it is bounded in ticks.
- **Unplug** (`usbh.c`, `usbh_msc.c/.h`, `diskio_mux.c`, `fs.c`):
  unbind from the detach path, generation counter, `RES_NOTRDY` /
  `STA_NODISK`, `/usb` hidden while unplugged, no recovery attempted on
  a disconnected port. See [Removal while mounted](#removal-while-mounted).
- **STALL and Reset Recovery** (`usbh_msc.c`). See
  [STALL and Reset Recovery](#stall-and-reset-recovery).
- **IN data toggle check** (`rtl/usb/usb_xact.v`). See
  [The IN data toggle check](#the-in-data-toggle-check), including the
  hardware risk for keyboards and mice.
- Device model (`tb_usb_device.v`): a bus reset now resets endpoint
  state (toggles, halts, bulk-only state), as a real device does -- it
  reset only the address, which the new toggle check would have exposed
  on the mouse. New hooks: `msc_stall_short`, `msc_bad_csw`,
  `msc_ack_lost`, and counters for resets, halt clears and aborted
  commands. The protocol check now allows for commands a reset
  legitimately ended before their CSW.
- `test_usb_msc`: 58 checks, from 23. New cases: enumeration during
  reads with interrupts emulated; READ past the end with a STALL;
  invalid CSW; four bad CRCs; lost ACK; unplug idle and mid-read, with
  replug.
- Docs: this log; the header summary (Phase 5 had still said reads
  truncate), the software stack list (planned files marked as such),
  Removal while mounted, three new sections, Also outstanding, Testing;
  `docs/filesystem.md` Volumes.

**Verification at the end of round 6:** `test_usb` (77), `test_usb_cosim`
(19), `test_usb_msc` (58, also at 20 ns skew with +-2500 ppm FS and
+-15000 ppm LS), all four `test_usb_margin` corners -- all pass. The
kernel-side files (`diskio_mux.c`, `fs.c`) were syntax-checked with a
host compiler only; no RISC-V toolchain was available, so `make -C
sw/os` has not been run on these changes.

**On hardware for round 6, in order:** keyboard and mouse still work
(toggle check); `usbmount`, copy a file, pull the stick mid-copy -- the
shell must stay responsive and `/usb` report not ready; replug and read
the file back; a card reader with no card; `make usb_fmax`.

**Verification at the end of round 4:** `test_usb` (77), `test_usb_cosim`
(19), `test_usb_msc` (23, also at 20 ns skew with +-2500 ppm FS and
+15000 ppm LS), all four `test_usb_margin` corners -- all pass.

**On hardware, in order:** `make usb_fmax BOARD=mozart_ml1` (must clear
48 MHz with margin); `usbmount`, then read a file from `/usb`; with
`msc_verbose` on, no `data in N of 512` or `short sector` lines; then a
write and read-back. After that, turn `msc_verbose` off.

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

Six targets, and each exists because something got through the others:

    make test_usb           # 77 gateware checks against a device model
    make test_usb_cosim     # 19 checks, real driver against real RTL
    make test_usb_hub       # 35 checks, real driver incl. usbh_hub.c,
                            #   hub model, FS stick and LS mouse behind it
    make test_usb_msc       # 58 checks, real driver incl. usbh_msc.c,
                            #   bulk-only SCSI model, mouse poll live,
                            #   interrupts emulated where it matters
    make test_usb_ecm       # 62 checks, real driver incl. usbh_ecm.c,
                            #   CDC-ECM adapter model shaped like an
                            #   RTL8152, every frame length that matters
                            #   both ways, LS mouse polling throughout
    make test_usb_serial    # 72 checks, real driver incl. usbh_cdc.c and
                            #   usbh_cp210x.c, CP2102 and CDC-ACM models,
                            #   both directions byte for byte
    make test_usb_margin    # receive tolerance to device clock error
    make usb_fmax BOARD=x   # whole-SoC Fmax, place-and-route, ONE seed
    make usb_fmax_sweep BOARD=x [SEEDS="1 2 3 4 5"]
                            # Fmax min/median/max over several seeds,
                            #   with each seed's critical-path source

`$usbh_irq(every, tick_every)` in `rtl/tb/cosim/usbh_vpi.c` makes the
co-simulation interrupt the driver the way hardware does -- see
[The transaction engine has one owner](#the-transaction-engine-has-one-owner).
Without it, nothing the ISR does in the middle of a storage command
can be seen, which is how the ownership race went unnoticed.

`test_usb` also runs a monitor on every cycle: whenever XACT_S's
pending bit is clear, its status and length must be software's own
last result. It exists because two faults broke exactly that and
neither produced a receive error.

`usb_fmax` fails outright on newer nextpnr ("combinational loops"):
the TRNG ring oscillators are loops by design. Add `--ignore-loops`
there, not a design change.

`test_usb_margin` and `usb_fmax` were added late, after two defects
that neither of the first two could see: the SoC running below its own
clock for weeks, and a receiver that failed the full-speed clock
tolerance spec in one direction. Area was tracked throughout
development; timing was not. Both are in
[Timing, and receive margin](#timing-and-receive-margin).

Hardware this fiddly is not debugged on a board -- but the limit of
that is worth stating plainly, because this project found it. Every
bug in [Hardware bring-up](#hardware-bring-up) needed something the
device model did not do: a real pull-up holding the idle line, a
device that suspends, one that NAKs mid-descriptor, a second
interface, two devices enumerating at the same instant, a transmitter
whose clock is off, and a bus handover with real skew. Simulation
carries the weight it can, following the pattern `rtl/gpu/bench` and
`rtl/tb` already establish, and the model has since grown `SKEW_NS`
and `CLK_PPM` for exactly this reason.

**`tb_usb_device.v`** — a behavioural device: responds to SETUP, returns
a configurable descriptor set, NAKs on demand, STALLs on demand, can be
switched between FS and LS, can inject CRC errors and can go away
mid-transaction.

**Hub model** — `tb_usb_device.v` with HUB=1: a four-port hub with class
requests, a status change endpoint and per-port power, connect, enable
and reset. Devices behind it are further instances on the same wires,
gated by `hub_en` and reset through `ext_reset`. See
[Hub class driver](#hub-class-driver).

Cases that must exist because they are the ones that bite. Ticked ones
are in the phase 1 suite:

- [x] LS direct attach (inverted polarity) and LS behind a hub (normal
  polarity) in the same run. These two differing is the single most
  likely silent bug in the whole project.
- [x] DATA0/1 toggle preserved across a NAK retry.
- [x] Short packet terminating an `auto_cont` bulk IN.
- [x] STALL reported as STALL rather than as a timeout.
- [x] SOF generated, and a transaction still completing around it.
- [x] Exact-multiple `auto_cont` IN ending on OK rather than SHORT,
      with an auto-poll slot live (`test_usb` case 10).
- [x] XACT_S never shows a stale or foreign result with pending clear
      (`test_usb` monitor).
- [x] Bulk IN paused by NAKs beyond the hardware budget, resumed
      part-way (`test_usb_msc`).
- [x] Bad CRC mid-sector and on consecutive packets, retried without
      desync (`test_usb_msc`).
- [x] Lost host ACK on bulk IN: the resend is ACKed and discarded
      (`test_usb_msc`).
- [x] Bulk data phase STALLed: halt cleared, CSW read, no reset
      (`test_usb_msc`).
- [x] Invalid CSW, and four bad CRCs in a row: Reset Recovery, next
      command exact (`test_usb_msc`).
- [x] A device enumerating during storage commands, with the ISR
      preempting the driver between register accesses
      (`test_usb_msc`, `$usbh_irq`).
- [x] Unplug with no command running, and mid-read; replug, start,
      read (`test_usb_msc`).
- [ ] Device disconnect in the middle of every transaction phase.
      (phase 3)
- [x] Two devices wanting address 0 at once: both plugged into a hub
      before it is attached (`test_usb_hub`).
- [x] A low-speed device behind a hub through PRE, its auto-poll slot
      included, checked by a full-speed listener too (`test_usb_hub`).
- [x] Hub, and each device behind it, unplugged and replugged;
      storage reads while a device behind the hub re-enumerates
      (`test_usb_hub`).
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
