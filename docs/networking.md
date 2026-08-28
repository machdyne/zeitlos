# Zeitlos Networking Developer Guide

## Overview

Networking is built around a Microchip ENC28J60 SPI Ethernet
controller on a PMOD, with a Wiznet W5500 (raw IP mode) planned as a
second, swappable backend later. It lives in its own app,
`sw/apps/net`, following the same "an app owns a piece of hardware and
mediates access to it via messaging" pattern the window manager
(`sw/apps/wm`) established for graphics -- see `docs/window_manager.md`
if that precedent isn't already familiar.

A second hardware backend now exists alongside it: an in-fabric RMII
Ethernet MAC (`rtl/ethmac_rmii.v`) for `mozart_ml1`, which has an RMII
PHY (LAN8720A) but no SPI Ethernet PMOD. See "Second backend: RMII
(mozart_ml1)" below -- everything above `eth.c` (ARP/IP/UDP/DNS/TFTP)
is identical either way; only the driver underneath it differs, and
which one gets built in is a compile-time choice, not a runtime one.

**Current status: ARP + ICMP echo (ping) + TFTP client, all confirmed
working on real hardware over the ENC28J60 backend.** Hardware
bring-up (SPI link + raw frame TX/RX), ARP/ICMP, and TFTP (both GET
and PUT, streaming, no file size limit) have all been confirmed
against real hardware and a real TFTP server -- see "Confirmed
working" sections below. **The RMII backend has NOT yet been tried on
real hardware** -- see its own section for what has and hasn't been
verified. **DHCP (`dhcp.c`) and DNS (`dns.c`) are also written but not
yet confirmed** against a real server or real hardware -- see "DHCP
client"/"DNS client" below; `net` falls back to its previous static IP
config automatically if DHCP doesn't work out (and hostname resolution
simply isn't available, with a clear error, if no nameserver is
configured), so neither of these puts the rest of the confirmed stack
at risk.

See `docs/app_runtime.md` for `maskirq()` (used throughout
`enc28j60.c` to protect SPI bit-bang transactions from interrupt
preemption) and the rest of the app runtime this all sits within.

## Why an app, not the kernel

- Keeps the kernel small, consistent with the project's direction so
  far.
- Reuses the WM precedent instead of inventing a new architecture:
  an app owns the hardware, other processes (including `sh.c`, which
  is just pid 0) talk to it via messaging.
- Isolates a large amount of new, unavoidably-somewhat-buggy code
  (a NIC driver plus a protocol stack, none of it testable by the
  author without real hardware) in a process that can be restarted
  independently of the kernel.
- TFTP from the shell still works fine once this is built out: `sh.c`
  sends/receives messages like any other process, with `run net` as a
  one-time prerequisite (same pattern as `run wm`).

## Why the SPI protocol is entirely in software

`rtl/spibb_eth.v` (new) follows the exact same pattern as the existing
`rtl/spibb.v` (SD card): the RTL exposes the raw SPI pins
(`{ss,sck,mosi}` writable, `miso` readable, plus the chip's `INT` line
as a readable bit for future use) as a single memory-mapped register.
**All actual SPI protocol timing happens in C**
(`sw/apps/net/enc28j60.c`), not in RTL. This deliberately avoids
repeating the hand-rolled hardware state machine approach used for the
GPU blitter (`rtl/gpu/gpu_blit.v`), which took several rounds of
hardware-tested bugfixes to get right. A software bit-bang driver has
the same protocol-correctness risk, but bugs found through testing can
be fixed by editing a C file, not by resynthesizing an FPGA bitstream
-- much cheaper to iterate on, which matters a lot for code this hard
to get right on the first try.

Register/pin layout is deliberately identical in shape to the SD
card's (`reg_sdcard`/`reg_eth`, same bit positions), so the software
SPI routines in `enc28j60.c` closely resemble the proven,
widely-used ChaN FatFs bit-bang driver already in
`sw/os/fs/fatfs/sdmm.c`.

## Third backend: ESP32 link (ULX3S)

The ULX3S has neither an SPI Ethernet PMOD nor an RMII PHY; its onboard
ESP32 acts as the network interface over UART1 (`NET_PHY=ESP32LINK`,
`sw/apps/net/esp32link.c`). See [esp32link.md](esp32link.md).

## Second backend: RMII (mozart_ml1)

`mozart_ml1` has an RMII PHY (LAN8720A) wired directly to the FPGA
instead of an SPI Ethernet PMOD, so it needed a real Ethernet MAC in
the fabric -- `rtl/ethmac_rmii.v`, exposed to software through
`sw/apps/net/rmii_eth.c/h` as an alternative to `enc28j60.c/h`.
Everything above the driver layer (`eth.c` and up -- framing, ARP, IP,
UDP, TFTP) is completely unaware of which backend is underneath it;
this section is only about what's different below that line.

### Why this one couldn't follow the "protocol timing in software" philosophy above

The SPI backend's whole design point (see "Why the SPI protocol is
entirely in software" above) was pushing protocol timing into C so
bugs are fixable by editing a file, not resynthesizing a bitstream.
RMII can't do that: it's a synchronous 2-bit-wide bus clocked at
50MHz shared between the MAC and the PHY, with byte boundaries,
preamble/SFD framing, and CRC32 generation all needing to happen on
literally every clock edge. There's no slack for software to bit-bang
this over the wishbone bus the way `spibb_eth.v`/`enc28j60.c` bit-bang
SPI -- by the time a wishbone transaction round-tripped through the
CPU, several RMII bit-times would already have passed. So this MAC's
RX/TX state machines, byte assembly, and CRC32 all live in
`rtl/ethmac_rmii.v` itself, which is exactly the "hand-rolled hardware
state machine" risk profile the SPI section above was contrasting
itself against (same category of risk the GPU blitter,
`rtl/gpu/gpu_blit.v`, carried -- "took several rounds of
hardware-tested bugfixes to get right").

The mitigation available here that wasn't fully exploited for the
blitter: extensive simulation *before* real hardware. `tb/` has three
Icarus Verilog testbenches, all currently passing:

- `tb/ethmac_rmii_tb.v` -- drives a synthetic, correctly-CRC'd RMII
  frame into the RX engine and checks byte-exact buffer contents,
  correct length, and that the CRC32 check accepts it.
- `tb/ethmac_rmii_tb2.v` -- negative paths: a frame with a deliberately
  corrupted FCS must be rejected and counted, not accepted; a second
  frame arriving before software pops the first must be dropped and
  counted, not silently overwrite the still-unread buffer.
- `tb/ethmac_rmii_loopback_tb.v` -- instantiates *two* `ethmac_rmii_wb`
  cores sharing one `eth_refclk`, wires one's TX pins straight to the
  other's RX pins, and drives an actual send-then-receive. This is
  the test that actually validates TX: it means the TX engine's CRC32
  generation isn't just internally self-consistent, it's independently
  accepted by the RX engine's already-verified checker. Covers both a
  word-aligned (64-byte) and a deliberately non-word-aligned (61-byte)
  frame length, since the first two word-aligned lengths tried
  happened to not exercise the last-partial-word byte-lane addressing
  path.

Run any of them with:
```
iverilog -g2005 -o /tmp/tb rtl/ethmac_rmii.v tb/ethmac_rmii_tb.v && vvp /tmp/tb
```
(`ethmac_rmii.v`'s `module ... #()` empty-parameter-list syntax is a
Yosys-tolerated convention used throughout `rtl/` that plain `iverilog`
rejects -- strip the `#()` from a scratch copy first if compiling it
standalone this way; not an issue when it's included as part of the
real `synth_ecp5`/Yosys build.)

**None of this has been run on real hardware yet.** Simulation
confirms the logic is internally correct against synthetic stimulus;
it says nothing about synthesis, place-and-route, timing closure on
the new 50MHz `ETH_REFCLK` domain, or how a real LAN8720A and a real
switch actually behave. Treat `net.c`'s test output (`make
BOARD=mozart_ml1`, then `NET_PHY=RMII` -- see below) as the actual
first evidence of whether any of this works outside simulation, same
as the original SPI backend's equivalent line above said before its
own hardware bring-up.

### No MDIO/MDC

`mozart_ml1`'s LAN8720A has no MDIO/MDC connected -- see the pin list
in `boards/mozart_ml1.lpf`. That means:

- The PHY's mode (speed/duplex autonegotiation, PHY address) is set
  entirely by strap pull-ups (`PULLMODE=UP` on `rx_data`/`crs_dv` in
  the LPF) sampled at the moment `eth_rst_n` is released, per the
  LAN8720A datasheet's strap-pin table. There's no way to read this
  back or override it in software.
- This MAC assumes the result is 100M full duplex -- the common case
  against a modern switch -- and never checks. If a real link
  negotiates something else, nothing currently detects or reports
  that; it would likely just look like silent packet loss or garbled
  frames, indistinguishable at first from a real bug elsewhere in this
  list. Worth ruling out early on the first hardware bring-up attempt,
  e.g. by trying a different switch port/cable if TX/RX both look
  wrong from the start.
- No chip revision or link-status register to read, unlike
  `enc28j60_revision()` -- `rmii_eth_init()` can't tell you anything
  about whether the PHY is actually responding, only what its own
  `CRS_DV`/`ETH_REFCLK` pins look like from the FPGA side (see
  `rmii_eth_init()`'s print).

### Register map and driver

Full register map is documented in `rtl/ethmac_rmii.v`'s header
comment (the RTL-side source of truth); `sw/common/zeitlos.h`'s
`reg_ethmac_*` block is the C-side mirror of the same thing.
Summary: `STATUS`/`RX_LEN`/`RX_CTRL` for receive (single-buffer, one
frame at a time -- see below), `TX_LEN`/`TX_CTRL` plus a TX buffer
for transmit. No interrupt -- `net.c`'s existing poll loop (`eth_poll()`
-> `phy_recv()`) covers this the same way it already covers ENC28J60.

**Single RX buffer, not double-buffered.** A second frame arriving
before software has popped the first is dropped and counted
(`rx_drop_count` in `STATUS`), not queued. Deliberately simple for a
first version, consistent with the "keep buffers minimal" constraint
this was built under -- worth watching `rx_drop_count` under real
network load and revisiting (double-buffering, or a small ring) if it
turns out to matter in practice.

**No destination-MAC filtering in hardware.** The ENC28J60 filters in
hardware; this MAC has no MDIO to configure an equivalent filter, so
it receives every frame that reaches the wire -- broadcast, multicast,
unicast to other hosts, all of it. Functionally harmless (`arp.c`/
`ip.c` only act on frames whose contents they recognize), but on a
busy LAN it means the single RX buffer above fills with irrelevant
traffic more often than the ENC28J60 backend would see under the same
conditions -- another reason to watch `rx_drop_count`.

**CRC32 residual constant.** RX validates a received frame by running
the same bit-serial update continuously across the frame's data bytes
*and* its own trailing FCS, then checking the result against a fixed
constant (`0xDEBB20E3`) rather than computing an independent CRC and
comparing. That constant was verified against Python's `zlib.crc32()`
on synthetic frames while writing this (see the git history/session
notes around `rtl/ethmac_rmii.v`'s Phase 2), not transcribed from
memory and trusted -- an earlier guess (`0xC704DD7B`, a different, also
commonly-cited "CRC32 magic residual" that turned out to belong to a
different bit-order convention) was caught this way before it reached
RTL. Worth remembering if this constant is ever touched: it depends
exactly on the bit order chosen for the shift register (`{ eth_rxd,
rx_shift[7:2] }`, LSB-first per byte) -- re-derive and re-check against
`zlib.crc32()` rather than adjusting it by trial and error.

### Build-time backend selection

Unlike `sw/bios` (already built per-board via `-DBOARD_$(BOARD)`),
`sw/apps` isn't currently board-aware -- the top-level `apps` target
builds one binary set with no board distinction, because until this
backend existed nothing under `sw/apps` needed one. `sw/apps/net`'s
`NET_PHY` Makefile variable (`ENC28J60` by default, `RMII` for
`mozart_ml1`) is the first place that had to change. See
`sw/apps/net/net_phy.h`'s header comment for the full reasoning,
including why this is a build-time choice and not a runtime probe --
short version: there's no reliable way for software to ask "was
`rtl/ethmac_rmii.v` actually synthesized into this bitstream?" by
reading a register, since an unmapped wishbone address doesn't fault,
it just reads back whatever `sysctl.v`'s mux resolves its default
case to.

```
cd sw/apps/net
make NET_PHY=RMII net
```

`NET_PHY` changes `CFLAGS` and which driver object gets linked, not
any `.c` file's contents, so plain per-file `.c` prerequisites (already
used throughout this Makefile for the general "stale `.o` after an
edit" reason -- see the comment there) wouldn't notice a bare `make
NET_PHY=RMII` after a previous `NET_PHY=ENC28J60` build in the same
directory, and would happily link a stale `eth.o`/`net.o` still
compiled with the *other* driver selected. `.net_phy_selected` (a real
file, not just a phony target -- see the Makefile) exists specifically
to force the right objects to rebuild when this changes; run `make
clean` first if in doubt rather than trusting it blindly.

### `rtl/ethmac_rmii.v`/`rmii_eth.c` -- known risk areas

Same spirit as the ENC28J60 list below: worth checking these first if
hardware bring-up doesn't work, roughly in the order most likely to
explain a total failure vs. a subtler one.

1. **Never touched real fabric.** Everything above is simulation-only
   (see the testbenches above) -- synthesis, place-and-route, and
   timing closure on the new 50MHz `ETH_REFCLK` domain have not been
   confirmed. If `make BOARD=mozart_ml1` doesn't close timing cleanly,
   start there before doubting the logic itself.
2. **CDC correctness, reasoned through but not stress-tested.** The
   two synchronizers (`crs_dv`/`ETH_REFCLK` heartbeat into `wb_clk_i`;
   the RX "pop" and TX "start" toggle handshakes) follow standard,
   conservative patterns and are explained inline in
   `rtl/ethmac_rmii.v`, but "reasoned to be correct" and "survived
   real silicon at real voltage/temperature margins across many
   power-on cycles" are different claims. If RX/TX work most of the
   time but fail intermittently (as opposed to consistently, which
   would point elsewhere), this is the first thing to suspect.
3. **REF_CLK50 assumption.** Built assuming pin `C7` is a 50MHz
   oscillator shared between the FPGA and the LAN8720A's own
   XTAL/REF_CLK input (confirmed during design, not independently
   re-verified against a schematic here) -- if RX never locks onto
   even a plausible-looking bit pattern, confirm this pin is actually
   toggling at 50MHz before doubting the RX state machine itself.
4. **100M full-duplex assumed, never confirmed.** See "No MDIO/MDC"
   above -- there's no way to read back what the PHY actually
   negotiated. Worth ruling out early (different cable/switch port)
   if things look wrong from the very start rather than assuming a
   logic bug.
5. **Inter-frame gap (48 `ETH_REFCLK` cycles, 12 byte-times) is the
   IEEE 802.3 minimum, not independently validated against a real
   switch's tolerance.** If back-to-back transmissions are somehow
   involved in a failure, this is worth a second look, though it's
   the least likely item on this list -- 48 cycles matches the spec
   value exactly, no rounding or approximation involved.
6. **CRC32 residual constant** (`0xDEBB20E3`) -- see "Register map and
   driver" above for how it was derived/checked. Low risk (it was
   verified against `zlib.crc32()`, and the loopback testbench
   confirms TX's independently-generated FCS is accepted by RX's
   check of it), but worth re-deriving rather than hand-adjusting if
   it's ever touched.
7. **Single RX buffer, promiscuous reception** -- not a correctness
   risk, but expect a higher `rx_drop_count` than the ENC28J60 backend
   under equivalent network load. See "Register map and driver" above.

None of this has been run on real hardware. Treat `net.c`'s test
output, built with `NET_PHY=RMII` against `BOARD=mozart_ml1`, as the
actual first evidence of whether any of it works outside simulation.

## `Z_BLOB`: the object system's new binary-data type

Before any of this, `zobj.h`/`zobj.c` needed a type for arbitrary-
length binary data. Everything the object system carried before this
was either a scalar or `Z_STR` (NUL-terminated) -- fine for window
rects and argument maps, but wrong for network payloads, which can
legitimately contain zero bytes anywhere.

`Z_BLOB` (`z_obj_blob(data, len)`) stores an explicit length instead
of relying on NUL-termination, via an indirect `z_blob_t {len, data}`
header (parallel to how `Z_LIST`/`Z_MAP` use `z_obj_table_t`). It's
supported everywhere `Z_STR` is: `z_obj_free`/`_copy`/`_equal`/
`_size`/`_print`, and -- the part that actually mattered for this to
be useful across processes -- `sw/os/msg.c`'s `z_resolve_obj()`.
Passing a blob through a message works like `Z_STR` (the actual bytes
are never copied, only pointer-translated) except the header itself
also needs rebuilding into scratch space, since it's an indirect
struct rather than a single pointer -- see `Z_MSG_MAX_BLOBS` in
`zmsg.h`. Same borrowed-payload lifetime rules as everything else in
`docs/messaging.md` apply: never call `z_obj_free()` on a blob reached
through a message, copy it first if you need to keep it past your next
send.

Now used by the TFTP layer (`sw/apps/net/tftp.c` via `zstream.h`, and
`Z_NET_TFTP_PUT` in `sw/common/znet.h`) to carry each chunk of a
transfer -- see "TFTP: exposed to other processes via messaging and
streaming" below.

## Config

- **IP**: DHCP by default (`sw/apps/net/dhcp.c`), falling back to a
  static address if no DHCP server answers -- see "DHCP client" below
  for the full design. **Both DHCP itself, and the static values used
  either as its fallback or unconditionally, are build-time choices**
  (`sw/apps/net/Makefile`), the same "compile-time, not runtime"
  reasoning `NET_PHY` already uses (see `net_phy.h`'s own comment) --
  there's no persistent config store or way to pass arguments to an
  app in this OS yet (see `sh.c`'s `run`), so a Makefile variable is
  the natural place for this, consistent with how the backend driver
  itself is already selected:

  ```
  make                        # DHCP on (default), static fallback = 192.168.178.230/24 gw .1
  make NET_DHCP=0              # DHCP off entirely -- always use the static config below
  make NET_STATIC_IP=10.0.0.42 NET_STATIC_NETMASK=255.255.255.0 NET_STATIC_GATEWAY=10.0.0.1
                                # override the static values -- accepted as dotted-quad or
                                # 0x-prefixed hex, either works (see the Makefile);
                                # meaningful as the DHCP fallback (NET_DHCP=1, the default)
                                # or as the address used outright (NET_DHCP=0)
  ```

  With DHCP on and answering, the static values above are never used
  at all -- they only matter once DHCP is off, or fails. **The
  defaults for those values are still ASSUMED, not confirmed** -- only
  the IP address (`192.168.178.230`) was ever actually specified for
  them, back when this was the only option; `net.c` assumes a typical
  home-router `/24` (`255.255.255.0`) with the gateway at `.1`
  (`192.168.178.1`). This only matters for traffic to destinations
  outside the local subnet -- `ping`/`tget`/`tput` to/from a machine
  on the same subnet don't depend on the gateway being correct.
  Override `NET_STATIC_GATEWAY`/`NET_STATIC_NETMASK` above if this
  assumption is wrong for your network and you're relying on the
  static config (`NET_DHCP=0`) or its fallback role.
- **DNS (nameserver)**: DHCP-provided by default (option 6, parsed by
  `dhcp.c` alongside the IP/netmask/gateway/lease options it already
  reads), with a build-time override, `NET_STATIC_DNS` -- see "DNS
  client" below for the full design and why this one's override
  semantics differ from `NET_STATIC_IP`/`NETMASK`/`GATEWAY` above (a
  standing override that always wins, not merely a DHCP-failure
  fallback):

  ```
  make                              # DHCP on: uses whatever nameserver DHCP hands back, if any
  make NET_STATIC_DNS=1.1.1.1        # always use this nameserver, regardless of what DHCP says
  make NET_DHCP=0 NET_STATIC_DNS=1.1.1.1
                                      # DHCP off entirely, still resolve hostnames via this nameserver
  ```

  With neither DHCP nor `NET_STATIC_DNS` providing a nameserver (e.g.
  `NET_DHCP=0` and no override set), hostname resolution simply isn't
  available -- `z_dns_resolve()`/`z_resolve_host()` (`sw/common/
  zdns.h`) return a clear "no nameserver configured" error rather than
  hanging or silently failing. Plain dotted-quad IPs (`telnet
  192.168.1.1`, `tget 192.168.1.1 ...`) always work regardless, since
  they never need a nameserver at all.
- **MAC**: `02:00:00:00:00:01`, hardcoded in `sw/apps/net/net.c`.
  Neither backend has a factory-assigned address (the ENC28J60 has
  none at all; `rtl/ethmac_rmii.v` has no address-filter register to
  read one back from even if it did). `02` as the first octet
  (specifically, the locally-administered-address bit) marks this as
  intentionally not a real vendor-assigned MAC, avoiding any
  collision risk with real hardware.
- **Duplex**: full duplex (changed from an initial half-duplex
  choice -- see `enc28j60_init()`'s comment for the full reasoning).
  Half duplex seemed like the safer default at first (the ENC28J60's
  PHY doesn't autonegotiate duplex with the link partner, and a
  full-duplex mismatch causes silent packet loss), but it's itself a
  mismatch against a modern switched network, where virtually every
  port runs full duplex -- forcing half duplex against a full-duplex
  link partner is a well-known source of excessive collisions/
  retries. This was identified as a likely contributor to an
  intermittent TFTP stall that survived several other real fixes
  (UART FIFO race protection, SPI interrupt protection, a doubled
  BIOS interrupt stack) -- see the TFTP debugging notes below.

## `sw/apps/net/enc28j60.c` -- known risk areas

This file's header comment lists these in the file itself, but
worth repeating here since it's the thing most likely to need
debugging first:

1. **SPI timing/bit-bang correctness** (`spi_xfer()`). If
   `enc28j60_revision()` doesn't come back as a small nonzero value
   (typically 1-6) after `enc28j60_init()`, start here -- nothing else
   in the driver can work if this doesn't.
2. **MAC address register byte order.** The ENC28J60's register FILE
   address order (`MAADR1`..`MAADR6`, addresses 0x04, 0x05, 0x02,
   0x03, 0x00, 0x01) does not match the MAC BYTE order (byte 0..5).
   This is a well-known, easy-to-get-backwards source of bugs in
   ENC28J60 drivers generally -- see the comment block above the
   `MAADR*` definitions.
3. **Bank/address encoding for individual registers** (the `REG()`
   macro and the constants built from it). Transcribed from training
   knowledge of the datasheet, not a live lookup. If a specific
   register seems to do the wrong thing, this is worth cross-checking
   first.
4. **The receive-buffer pointer errata** in `enc28j60_recv()`
   (`ERXRDPT` must always be programmed with an odd value -- a real,
   documented ENC28J60 silicon errata). The workaround implemented
   (`next_ptr - 1`, or `RXSTOP_INIT` at the wrap boundary) is from
   memory of the commonly-cited fix, not a fresh datasheet read.
5. **Timing constants** (`eth_delay_us`/`eth_delay_ms`, `ETH_TX_TIMEOUT`)
   are uncalibrated loop-iteration counts, not real time units --
   likely need tuning against actual silicon/clock speed.

None of this has been simulated or run. Treat `sw/apps/net/net.c`'s
test output as the actual first evidence of whether any of it works.

## Layers implemented so far

- `sw/apps/net/net_phy.h` -- picks the driver underneath everything
  else (`enc28j60.c` or `rmii_eth.c`) at build time -- see "Second
  backend: RMII (mozart_ml1)" above. Everything below this bullet is
  unaware of which one is active.
- `sw/apps/net/eth.c/h` -- Ethernet framing (build/parse the 14-byte
  header, dispatch received frames to `arp.c`/`ip.c` by ethertype).
- `sw/apps/net/arp.c/h` -- a small fixed-size (8-entry) IP-to-MAC
  cache. Non-blocking by design (there's no sleep/yield primitive to
  block on): `arp_request()` fires off a request, `arp_lookup()` is
  polled afterward until it succeeds. Opportunistically learns
  IP-to-MAC mappings from *any* ARP traffic seen, not just replies to
  our own requests -- same behavior real ARP implementations use.
- `sw/apps/net/ip.c/h` -- IPv4 header construct/parse, routing (local
  subnet vs. gateway, via a simple netmask check), and ICMP echo
  reply folded directly into this file (small enough not to warrant a
  separate `icmp.c` yet). `ip_send()` returns false and kicks off ARP
  resolution if the next hop isn't cached yet -- callers should just
  retry after a few more `eth_poll()` iterations rather than treating
  that as a hard failure.
- `sw/apps/net/udp.c/h` -- per-port receive dispatch (a small
  fixed-size, 4-entry listener table) and send. **No checksum is
  computed or verified** -- valid per RFC 768 (checksum `0` means "not
  used"), and IP-level checksums already cover header corruption.
  Deliberate simplicity, revisit if this ever runs over a link less
  trustworthy than a local dev network.
- `sw/apps/net/dhcp.c/h` -- a DHCP (RFC 2131/2132) client:
  DISCOVER/OFFER/REQUEST/ACK, run once at startup ahead of everything
  else in this list (`net.c`'s `main()` calls `dhcp_acquire()` right
  after `eth_init()`, before `arp_init()`/`ip_init()`/`tcp_init()` are
  called with a real address). See "DHCP client" below for the full
  design, including why -- unlike everything else on this list -- it
  blocks with a bounded timeout instead of folding into `net.c`'s own
  non-blocking main-loop poll.
- `sw/apps/net/dns.c/h` -- a DNS (RFC 1035) client: A-record lookups
  only, **one resolution at a time**, non-blocking/poll-driven like
  `arp.c`/`tftp.c`/`tcp.c` (unlike `dhcp.c` just above -- see "DNS
  client" below for why the two differ here). Exposed to other
  processes via `Z_NET_DNS_RESOLVE`/`_RESOLVE_REPLY` (`znet.h`);
  `sw/common/zdns.h`'s `z_resolve_host()` is the blocking wrapper most
  callers (`repl`'s `telnet` command, `sh.c`'s `tget`/`tput`) actually
  use instead of sending those messages directly.
- `sw/apps/net/tftp.c/h` -- a TFTP (RFC 1350) client. Non-blocking,
  poll-driven, **one transfer at a time**: `tftp_get_start()`/
  `tftp_put_start()` kick off a transfer, `tftp_poll()` is called
  every loop iteration (handles retransmit timeouts, reports the
  final result); the actual protocol progress happens via a UDP
  listener callback driven by `eth_poll()`. Streams both directions
  through `sw/common/zstream.h`, no whole-file buffer, no size limit
  beyond what the other end of the stream imposes on itself -- see
  "TFTP: exposed to other processes via messaging and streaming"
  below.
- `sw/apps/net/tcp.c/h` -- a client-only (active open) TCP, **one
  connection at a time**, dispatched from `ip_handle()` for protocol
  6. See "TCP + telnet" below.
- `sw/apps/net/telnet.c/h` -- a minimal telnet (RFC 854) client
  layered directly on `tcp.c`. See "TCP + telnet" below.

## DHCP client

Adds `sw/apps/net/dhcp.c/h`, run once at startup (`net.c`'s `main()`)
ahead of everything else -- see that file's own header comment for
the full design writeup; summarized here. On by default, off with
`make NET_DHCP=0` -- see "Config" above for the full build-time
toggle and the static-IP override that goes with it either way.

- **Blocking, not polled -- the one exception to how every other
  layer in this stack is built.** `arp.c`/`tftp.c`/`tcp.c` are all
  non-blocking-poll by design because `net.c`'s main loop has other
  work to interleave with any one of them being in progress. Nothing
  else meaningful can happen before `net` has an address at all --
  there's no other process to serve yet (`net` hasn't even
  `z_pid_register()`'d itself), and `ip_send()` has nowhere useful to
  route non-broadcast traffic without `our_ip`/`our_gateway` set. So
  `dhcp_acquire()` just runs its own tight `eth_poll()`-driven wait
  loop with a timeout, the same shape `sw/common/zport.c`'s
  `z_port_connect_arg_timeout()` and `zstream.c`'s open/pull timeouts
  already use for their own single-purpose blocking exchanges.
- **One-shot: no lease renewal.** Acquires a lease at boot and keeps
  it for the process's entire run -- no T1/T2 timers, no re-REQUEST,
  no handling of the lease actually expiring. A real functional gap
  on a network with short DHCP leases; accepted deliberately for a
  first version, same category of scope cut as TFTP's
  one-transfer-at-a-time or TCP's single TCB elsewhere in this file --
  `net` is a dev-loop tool typically restarted often (`run net` is a
  fresh process, hence a fresh lease, every time), on networks that
  usually hand out long leases to begin with.
- **Every packet is broadcast, never unicast -- by design, not as a
  simplification.** The REQUEST that follows a `SELECTING`-state OFFER
  is broadcast per RFC 2131 itself (`ciaddr` is still `0.0.0.0` at
  that point), not a choice made here. DISCOVER and REQUEST both set
  the broadcast flag (RFC 2131 4.1) asking the server to reply via
  `255.255.255.255` rather than unicast to the not-yet-configured
  offered address -- and `ip_handle()` (`ip.c`) also relaxes its
  normal destination-IP filter while `our_ip` is still `0.0.0.0`,
  specifically so a server that unicasts anyway (valid per the RFC,
  and something real servers do once they know the client's MAC from
  the request's own `chaddr` field) still gets through. Net effect:
  `dhcp.c` never needs ARP at all -- every send goes out `ip_send()`'s
  limited-broadcast fast path (see below), straight to the Ethernet
  broadcast address.
- **`ip_send()` gained a limited-broadcast fast path** (`ip.c`): a
  destination of `255.255.255.255` now goes directly to the Ethernet
  broadcast MAC, bypassing the ARP-resolve-the-next-hop logic
  entirely (there's no real host to resolve, and during DHCP
  negotiation there may not even be a gateway configured yet to fall
  back to). General-purpose, not DHCP-specific -- any future caller
  that needs to broadcast benefits from this too.
- **Fails soft.** If no server answers within the retry budget
  (`DHCP_MAX_ATTEMPTS` attempts per phase, `DHCP_TIMEOUT_TICKS` each,
  `dhcp.c`), or a server sends a NAK, `dhcp_acquire()` returns `false`
  and `net.c` falls back to its existing static
  `OUR_IP`/`OUR_NETMASK`/`OUR_GATEWAY` constants -- a board with no
  DHCP server on its network (a bench setup with just a switch, say)
  behaves exactly as it did before this file existed, just with a
  bounded delay (a few seconds, not TCP's ~30s worst-case backoff)
  before falling back rather than hanging.

**Written, not yet run against a real DHCP server or real hardware**
-- same status TCP/telnet started at below; treat this section as a
design writeup, not a confirmation, until it's been checked against
at least one real router/server (e.g. the one built into a typical
home router, or `isc-dhcp-server`/`dnsmasq` on Linux) and the
resulting lease's IP/netmask/gateway confirmed correct by also
checking the server's own lease table.

## DNS client

Adds `sw/apps/net/dns.c/h`, letting `repl`'s `telnet` command (the
primary caller this was built for), `sh.c`'s `tget`/`tput`, and any
future caller take a hostname ("myserver.local") anywhere a plain
dotted-quad IP was previously required. See dns.c's own header
comment for the full design writeup; summarized here:

- **Non-blocking/poll-driven, unlike `dhcp.c`.** `dhcp_acquire()`
  blocks net's own main loop because nothing else useful can happen
  before net has an address at all (see "DHCP client" above). DNS is
  the opposite case: a resolution is kicked off by a message from some
  OTHER process, arriving well after net is already up and doing
  useful work, so there's no reason to block net's own loop for it --
  `dns.c` fits the same non-blocking poll shape `arp.c`/`tftp.c`/
  `tcp.c` already use (`dns_resolve_start()` kicks a query off,
  `dns_poll()` -- called every main-loop iteration alongside
  `eth_poll()`/`ip_poll()`/etc. -- drives retransmission and the
  overall timeout).
- **A records only, no CNAME following, no caching.** Only asks for
  (and only looks at) type-A answer records -- a server that replies
  with a CNAME chain instead of an A record is reported as "no A
  record in response", a resolve failure, even though a real resolver
  would follow the alias and try again. No result caching either:
  every `z_dns_resolve()` call is a fresh query on the wire, even for
  a hostname just resolved a moment ago. Both deliberate scope cuts
  for a first version -- same spirit as `dhcp.c`'s own "no lease
  renewal" cut elsewhere in this doc -- worth revisiting if a real use
  ever needs a CNAME-fronted hostname, or resolves the same name often
  enough that the extra round trips start to matter.
- **One resolution in flight at a time.** Same simplification as
  TFTP's one transfer/TCP's one connection -- a `Z_NET_DNS_RESOLVE`
  that arrives while one is already in progress gets an immediate
  "busy" reply, not queued.
- **The nameserver itself**: DHCP-provided by default (option 6,
  parsed by `dhcp.c` alongside the address/netmask/gateway/lease
  options it already reads), or the `NET_STATIC_DNS` build-time
  override -- see "Config" above for the exact Makefile invocations.
  Unlike `NET_STATIC_IP`/`NETMASK`/`GATEWAY`, this override is a
  standing one, not merely a DHCP-failure fallback: if set, it always
  wins over whatever DHCP said, even on an otherwise-successful lease
  -- reasoning being that DNS is a genuinely independent setting
  someone might want to override on its own (a different, faster, or
  more trustworthy resolver than whatever a given network's DHCP
  server happens to hand out) in a way that overriding the IP/netmask/
  gateway itself rarely is.
- **`sw/common/zdns.h`/`zdns.c`** -- the "usable from anywhere" half of
  this feature, and the reason a plain dotted-quad IP and an actual
  hostname can both just work at every call site that matters:
  - `z_parse_ipv4()` -- the dotted-quad parser itself, no networking
    involved. Used to be duplicated (byte-for-byte identical) between
    `sh.c` and `repl.c`, purely because there was nowhere shared both
    build contexts (kernel vs. a normal app) could reach before this
    file existed. Both copies were deleted in favor of this one.
  - `z_dns_resolve()` -- sends `Z_NET_DNS_RESOLVE` to `net` and blocks
    (with its own bounded timeout) for the `Z_NET_DNS_RESOLVE_REPLY`.
    Same "there's nothing else useful to do meanwhile, so just wait"
    reasoning `z_port_connect_arg_timeout()` (`zport.c`) and
    `zstream.c`'s own blocking calls already use elsewhere in this
    codebase.
  - `z_resolve_host()` -- tries `z_parse_ipv4()` first (fast, no
    messaging, doesn't even need `net` running), falls back to
    `z_dns_resolve()` only if that fails. This is the one function
    most callers actually want, and what `repl.c`'s `telnet` command
    and `sh.c`'s `tget`/`tput` both now call.
  - Builds into either an app (linking `zeitlos.o`) or the kernel
    (linking `msg.o`/`pidreg.o`) unmodified, via the exact same
    dual-build technique `zstream.c` already established (see its own
    header comment, and `zdns.c`'s) -- forward-declaring
    `z_msg_send()`/`z_msg_read()`/`z_msg_new_send()`/
    `z_uptime_ticks()`/`z_pid_lookup()` instead of `#include
    "zeitlos.h"`, which would collide with `kruntime.c`'s own
    `getch()`/`readline()`/`echo()`/`noecho()` in the kernel build.
- **A real, known cost worth flagging: `repl.c`'s own message loop.**
  `repl` services every connected `term`/`REPL_EVAL` request from one
  shared mailbox (`main()`'s own `while (1) { while (z_msg_read...)
  }`). A slow hostname lookup from ONE `telnet` command -- worst case,
  `net` not running, or a genuinely unresponsive nameserver -- blocks
  that whole loop for up to `z_dns_resolve()`'s timeout, delaying
  every OTHER connected window's response too, not just the one that
  typed `telnet`. Bounded (a few seconds) and rare in practice (a real
  nameserver on a local network answers in single-digit milliseconds,
  and a literal IP never touches this path at all, per
  `z_resolve_host()`'s parse-first order above), but a real cost, not
  a theoretical one -- worth revisiting with a genuinely non-blocking
  resolve-then-connect flow in `repl.c` if it ever proves to matter
  with several people using `term` at once. Documented inline at the
  `telnet` command's own call site in `repl.c` too.

**Written, not yet run against a real DNS server (or nameserver
provided by a real DHCP server) or real hardware** -- same status
DHCP/TCP/telnet all started at elsewhere in this doc; treat this
section as a design writeup, not a confirmation, until checked against
at least one real resolver (a home router's built-in one, or
`dnsmasq`/`unbound`/a public resolver like `1.1.1.1` via
`NET_STATIC_DNS`) and a hostname that's actually confirmed to resolve
to the address `dig`/`nslookup`/`host` (run from another machine on
the same network) agrees with.

## TCP + telnet

Adds Zeitlos's first non-UDP transport, in service of one concrete
feature: `repl`'s `telnet <ip>` command, letting a `term` window
connect to a real remote telnet server instead of the local `repl`
interpreter. See `docs/ports.md` for the `zport.h` provider mechanism
this rides on (`term` <-> `net`) and `sw/common/zterm.h` for
`Z_TERM_SET_PORT`, the message that gets a `term` instance to actually
switch over.

**Written, not yet run against real hardware or a real telnet
server** -- same status the ENC28J60/RMII drivers started at before
their own "Confirmed working" sections above; treat this section as a
design writeup, not a confirmation.

### Why client-only, one connection at a time

Nothing in Zeitlos needs to *accept* incoming TCP connections yet (no
listening/passive-open side, no SYN queue), and `telnet <ip>` only
ever needs one outbound connection open at a time -- so `tcp.c` keeps
a single static TCB rather than a connection table, the same
simplifying choice TFTP already made for "one transfer at a time" (see
the staged plan above). `telnet.c` inherits the same constraint by
construction, since it's just a byte-stream filter sitting directly on
top of `tcp.c`'s one connection.

### Stop-and-wait sending, not a real send window

`tcp_send()` allows at most one unacknowledged outbound segment at a
time -- the next call fails (caller retries on a later poll) until the
previous one is acked. No sliding window, no pipelining, no
retransmit *queue* (just one retransmit *slot*). This is a real
throughput ceiling (round-trip-time-bound, not bandwidth-bound) that
would be a bad tradeoff for a bulk transfer protocol -- which is
exactly why TFTP doesn't work this way, it streams through
`zstream.h` instead. Telnet traffic is different: small, bursty,
interactive, latency-sensitive rather than throughput-sensitive, so
this costs nothing in practice while avoiding a real
window/retransmit-queue implementation entirely. `telnet.c` layers a
small internal send queue (`TELNET_TX_QUEUE_LEN`, telnet.c) on top so
a burst of typed keystrokes and a handful of option-negotiation
replies don't fight each other over that single slot -- `telnet_poll()`
drains it through `tcp_send()` as room allows.

### No out-of-order reassembly

A segment that doesn't arrive with the exact sequence number expected
is dropped, not buffered -- relies entirely on the peer's own
retransmit timer to resend it in order. No reassembly queue, no
selective ack. Expected to be a non-issue on a local, low-latency LAN
(the same environment TFTP's own design already assumes); would need
revisiting for a path with real reordering.

### No TCP options, no half-close

No MSS negotiation, no window scaling -- `tcp.c` never sends a TCP
options field, so both ends fall back to RFC 879's 536-byte default
MSS (`TCP_MAX_PAYLOAD`, tcp.h). No half-close support either: a
remote-initiated FIN gets `net`'s own FIN sent right back immediately
(see `tcp_handle()`'s `TCP_ESTABLISHED` case, tcp.c) instead of
lingering in `CLOSE_WAIT` waiting for the application to decide --
telnet has no use for keeping one direction of a connection open after
the other has closed.

### Telnet option negotiation: accept ECHO/SGA, refuse everything else

`telnet.c` refuses (`WONT`/`DONT`) every option a server proposes *to*
it, except it *accepts* `WILL ECHO`/`WILL SUPPRESS-GO-AHEAD` from the
server (replies `DO`) -- deliberately asymmetric, not a bug. `term`
does no local character echo once connected to a port (see
`docs/ports.md`), so refusing server-side echo would leave a user
typing blind; accepting it is what makes a session actually usable.
Almost every common `telnetd` (Linux/BSD, busybox) offers exactly
these two options unprompted at connect time. Never replies to an
unsolicited `DONT`/`WONT` (only `DO`/`WILL` get an answer) --
deliberately, since always-answer-everything is the classic way to
build an infinite negotiation ping-pong with a peer that does the
same; see `telnet.h`'s own comment for the full reasoning.

### Getting back out of a telnet session

Once `term` is relaying raw bytes to a real remote, there's no
`quit`/`exit` command the way `repl`/`portdemo` have to hand control
back with. `term` reserves **F12** as a fixed, always-available escape
hotkey back to `repl0`, intercepted in `handle_key_event()` before it
ever reaches whatever port is currently connected -- not the classic
telnet-client Ctrl-], since Ctrl is only special-cased for letters in
`sw/common/zkbd.c`'s keysym translation (extending that to punctuation
would be a keyboard-layer change every app inherits, for a feature
only `term` needs). See `term.c`'s own header/`handle_key_event()`
comments.

### The CONNECT-time IP argument

`term` never itself knows a telnet target's IP -- `repl`'s `telnet
<ip>` command does. Rather than a separate message from `repl` to
`net` racing against `term`'s own connection attempt, the IP rides
along as the `zport` `CONNECT` message's own payload:
`z_port_connect_arg()` (`sw/common/zport.h`) lets a caller supply
that payload instead of the default `Z_NONE`, and
`Z_TERM_SET_PORT`'s `Z_MAP` form (`sw/common/zterm.h`) is how `repl`
gets that argument to `term` in the first place. `net`'s own
`Z_PORT_CONNECT` handler requires a `Z_UINT32` payload (the target
IP) and refuses anything else -- there's currently only one thing a
`CONNECT` to `net` can mean. Because the TCP handshake itself is
asynchronous, `net` doesn't call `z_port_accept()` synchronously the
way `portdemo`/`repl` do -- it defers `Z_PORT_CONNECTED`/
`Z_PORT_REFUSED` until the handshake actually resolves (or times out),
staying within `z_port_connect_arg()`'s own ~2 second connect timeout
(`zport.c`) for anything on a normal LAN.

## TFTP: exposed to other processes via messaging and streaming

Superseded the original whole-file-in-one-message design (see "TFTP
debugging notes" below for the bug that motivated the change, and
`sw/common/zstream.h` for the general mechanism). Both directions of
a transfer now move through a `zstream` pull-based stream, one
TFTP-block-sized chunk at a time, with no size limit beyond whatever
the two ends impose on themselves -- no more fixed-size whole-file
buffer anywhere in the path.

`sw/common/znet.h` still defines `Z_PID_NET` (the reserved pid, `2`
-- same "start things in a known order" convention as `Z_PID_WM`) and
the app-level protocol, but its shape changed with the direction of
each transfer:

- **GET**: `net` is the data source, so a requester calls
  `zstream_open()` (see `zstream.h`) directly against `Z_PID_NET`,
  with a `Z_MAP{"ip", "filename"}` payload. `net` streams the
  received data back as the reply stream -- `Z_STREAM_EOF` marks
  successful completion, `Z_STREAM_ERROR` carries a failure message.
  There's currently only one thing an `OPEN` to `net` can mean, so no
  separate subject is needed for this direction at all --
  `Z_NET_TFTP_GET`/`_GET_REPLY` are retired.
- **PUT**: the direction is reversed (`net` needs to *pull* data from
  the requester, not receive it pushed), so this can't reuse
  `zstream_open()` the same way. `Z_NET_TFTP_PUT` (`Z_MAP{"ip",
  "filename"}`, no more `data` blob) tells `net` to start a PUT; `net`
  then opens its *own* stream back to the requester to pull the
  file's bytes, forwarding each chunk to the server as it arrives.
  `Z_NET_TFTP_PUT_REPLY` (`ok`/`error`, same as before) is sent once
  the *whole* transfer -- including the remote server's handling of
  the final block -- completes, which is later than when the
  requester finishes producing chunks; those two finish at different
  times now.

Only one transfer is handled at a time; a request that arrives while
one is already in progress gets an immediate "busy" error/rejection
rather than being queued.

## `sw/apps/net/tftp.c`: the two streaming roles

`net` plays a different `zstream` role depending on direction, and
each shapes how its own TFTP-level ACK timing has to work to avoid
either side needing to buffer more than one block ahead:

- **GET, `net` is the stream producer.** Each block received from the
  server is *held* (a single 512-byte buffer, not a growing one)
  until the receiver actually pulls it -- only then is it delivered
  via `zstream_send_chunk()` **and** ACKed to the server. This keeps
  buffering bounded to exactly one block regardless of how the
  receiver's pace compares to the network's: a receiver slower than
  the network just means the server's own retransmits of the
  still-unacked block get ignored (we already have it, just haven't
  delivered it yet) until the receiver catches up, rather than `net`
  needing to buffer ahead of what's actually been delivered.
- **PUT, `net` is the stream consumer.** It pulls chunks from the
  sender and forwards each to the server as the previous block gets
  ACKed. Since `net` can't block waiting on either the network or the
  stream (it has to keep servicing `eth_poll()` throughout), the
  actual WRQ exchange with the server doesn't start until the first
  chunk is actually in hand -- there's nothing to send as block 1
  before then. `tftp_handle_stream_msg()` is how progress on the
  stream side gets fed into this from `net`'s own message loop
  (mirroring the fully non-blocking producer side of `zstream.h`
  itself); `zstream.h` also gained a non-blocking consumer API
  (`zstream_open_async()`/`zstream_pull_async()`/
  `zstream_consumer_handle()`) specifically for this role, since the
  original consumer API was blocking-only (fine for the shell, not
  for `net`).

## `sh.c`: `tget`/`tput` shell commands

`sw/os/sh.c`'s `tget <ip> <remote-file> [local-file]` and
`tput <ip> <local-file> [remote-file]`, plus a small dotted-quad IPv4
parser (`parse_ipv4()`). Each plays the opposite `zstream` role from
`net`:

- **`tget` is a stream consumer** (`zstream_open()`/`zstream_pull()`,
  the blocking API -- the shell has nothing else to do while a
  transfer runs), writing each pulled chunk straight to the SD card
  via `fs_open_write()`/`fs_write_chunk()`/`fs_close_write()` as it
  arrives.
- **`tput` is a stream producer** (`zstream_accept()`/
  `zstream_producer_handle()`/`zstream_send_chunk()`, same API `net`
  uses for GET, just driven from a simple blocking
  `z_msg_read()`-in-a-loop instead of a non-blocking poll loop, again
  because the shell has nothing else to do meanwhile), reading each
  chunk from the SD card via `fs_open_read()`/`fs_read_chunk()`/
  `fs_close_read()` as `net` pulls it.

Neither direction loads the whole local file into memory anymore --
`fs.h`'s chunked read/write API (`fs_open_read`/`fs_read_chunk`/
`fs_close_read`, `fs_open_write`/`fs_write_chunk`/`fs_close_write`)
was built specifically to pair with `zstream.h` for this. Requires
`net` to already be running -- same dependency shape as any WM-using
app needing `run wm` first; see "`init`: running `net` without `wm`"
below for running `net` without `wm` at all.

## `init`: running `net` without `wm`

`net` is expected to run as pid 2 (`Z_PID_NET`, `znet.h`) -- normally
whatever's true because `wm` was started first (`run wm` then
`run net` puts `wm` at pid 1, `net` at pid 2). The shell's `init`
command gets `net` to pid 2 without needing `wm` running at all: it
reserves pid 1 with a placeholder process that's created but never
started (just enough to occupy the slot), then starts `net`, which
lands on pid 2 as a result.

```
> init
> run net
```

Originally added to isolate `net` for testing (ruling out any
cross-process interaction with `wm` as a cause during the TFTP
debugging -- see "TFTP debugging notes" below), it's also just a
faster way to get `net` running on its own when `wm`/the window
manager isn't needed at all.

**A build-time wrinkle worth knowing about**, since it shaped how this
is wired up: `sh.c` runs as pid 0 (the kernel itself), so it needs
`z_msg_send`/`z_msg_read`/`z_msg_new_send`/`z_uptime_ticks` and the
`Z_BLOB`-aware `z_obj_*` functions -- but linking `zeitlos.c` (the
normal app-facing runtime that provides those) into the kernel build
collides with `kruntime.c`'s own `getch()`/`readline()`/`echo()`/
`noecho()` (kernel-native equivalents of the same functionality apps
reach via the syscall trampoline). Fixed by adding kernel-side
`z_msg_send`/`z_msg_read`/`z_msg_new_send`/`z_uptime_ticks` directly to
`sw/os/msg.c` -- same API shape as `zeitlos.c`'s versions, but
implemented by calling `k_msg_send`/`k_msg_read` directly instead of
through `z_kernel_ptr`'s syscall indirection, since `sh.c` doesn't
need that indirection at all (it *is* the kernel, not a separate
process reaching into it). `sw/common/zobj.c` (needed for
`z_obj_map`/etc., no naming collision) is linked into the kernel
build for the same reason. `zstream.c` follows the identical pattern
for the same reason -- see its own header comment -- which is why it
forward-declares just the four functions it needs instead of
`#include`-ing either `zeitlos.h` or `msg.h` directly: it builds into
either side unmodified, resolving to whichever implementation
(`zeitlos.o`/`msg.o`) actually gets linked.

## `z_uptime_ticks()`: a new syscall, needed for TFTP's retry timing

TFTP needs a real elapsed-time measurement for its retransmit timeout
(rather than an uncalibrated loop-iteration count, like
`enc28j60.c`'s `ETH_TX_TIMEOUT`). The kernel already tracked
`z_kernel_ticks`, incrementing on every `KTIMER` IRQ (`rtl/sysctl.v`'s
`rtc_ctr`, a 16-bit counter wrapping at the system clock -- confirmed
~732Hz from the RTL comment), but nothing exposed it to apps. Added as
a new syscall (`Z_SYS_UPTIME` in `syscalls.def`, handler in
`kernel.c`, app-facing `z_uptime_ticks()` wrapper in `zeitlos.c`/`.h`)
-- general-purpose, not TFTP-specific, useful anywhere something needs
real elapsed time rather than a loop-iteration proxy.

## Staged plan

Learned from the GPU blitter experience: build and verify hardware-
adjacent layers incrementally, not all at once, since debugging a
multi-layer stack built on an unverified foundation is much harder
than debugging one layer at a time.

1. **Done, confirmed on real hardware.** SPI + `enc28j60.c` + raw
   frame TX/RX.
2. **Done, confirmed on real hardware.** ARP -- respond to requests
   for our IP; resolve a target's MAC. `net` logged answering a
   real "who-has" from another device on the network.
3. **Done, confirmed on real hardware.** IP + ICMP echo reply --
   `net` logged answering real `ping` requests from another machine,
   and the pings themselves succeeded on the sending end.
4. **Done, confirmed on real hardware.** UDP + a TFTP client
   (`udp.c`, `tftp.c`), exposed through the kernel shell's `tget`/
   `tput` commands, streaming both directions with no file size limit
   (see "TFTP: exposed to other processes via messaging and
   streaming" below). Multi-block transfers completed successfully in
   both directions against a real TFTP server. Getting here took a
   long debugging pass -- root cause was an `objcopy` build
   truncation bug corrupting `net`'s own stack via an undersized
   memory allocation, not anything in the networking code itself --
   see "TFTP debugging notes" below for the full story.
4.5. **Written, not yet run on real hardware.** `net` now checks the
   SOC capability CSRs (`rtl/csrs.v`, `docs/csrs.md`) before touching
   any ethernet backend register, and exits cleanly instead of hanging
   forever on a board that confirms it doesn't have the hardware this
   binary was built for (e.g. Lakritz, which has neither `SPI_ETH` nor
   `ETH_RMII`) -- this is what lets `sw/os/sh.c`'s `init` start `net`
   automatically on every board now, instead of the old
   pid-reservation-only workaround. See `docs/csrs.md` for the full
   design; unrelated to phases 1-7 here, just riding alongside them in
   the same file/app.
5. **Written, not yet run on real hardware or against a real telnet
   server.** TCP client (`tcp.c`) + a minimal telnet client
   (`telnet.c`), exposed to other processes as a `zport` provider
   (`sw/common/zport.h`, `docs/ports.md`) -- reachable via `repl`'s
   `telnet <ip-or-hostname>` command through `term`. See "TCP +
   telnet" below for the design and the same kind of "known risk
   areas" writeup the ENC28J60/RMII drivers got before their own
   hardware bring-up passes (above) -- follow the same "confirm the
   simplest layer first" approach here too: a bare SYN/SYN-ACK/ACK
   handshake against something like `nc -l` before layering telnet
   negotiation or `term` on top.
6. A message-based UDP API for other apps (not just the shell) to use
   directly, reusing `Z_BLOB` and the same messaging shape `znet.h`
   already established for TFTP -- straightforward extension once
   there's an app that actually needs raw UDP rather than going
   through TFTP.
7. W5500 backend. Per the design discussion that led here: W5500 has
   its own hardware IP/socket layer (unlike the ENC28J60's raw MAC),
   so the swappable boundary for that backend is at the socket/UDP
   level, not the Ethernet-frame level that ARP/IP below it sit at --
   `net`'s driver-facing internals stay ENC28J60-specific; only the
   public UDP-facing API needs to be something a W5500 backend could
   also implement.
8. **Written, not yet run against a real DHCP server or real
   hardware.** DHCP client (`dhcp.c`) replacing the fully-static IP
   config phases 3-7 above were all built and tested against -- see
   "DHCP client" above for the full design and known scope cuts (no
   lease renewal, chief among them). Same "confirm the simplest thing
   first" approach as every hardware-adjacent layer above: check that
   a bare DISCOVER actually reaches a real router/server and gets an
   OFFER back before trusting the rest of the exchange, and cross-
   check the resulting lease's IP/netmask/gateway against that
   server's own lease table rather than trusting `net`'s own printed
   log of what it parsed.
9. **Written, not yet run against a real DNS server or real
   hardware.** DNS client (`dns.c`) plus the shared `sw/common/zdns.h`
   helpers, replacing the IP-only `telnet`/`tget`/`tput` these all
   used before -- see "DNS client" above for the full design and known
   scope cuts (A records only, no CNAME following, no caching). Same
   "confirm the simplest thing first" approach: check that a bare
   A-record query against a well-known, always-up hostname (e.g. one
   of the DHCP server's own vendor's domains, or anything `dig`/
   `nslookup` from another machine on the same network confirms
   resolves) round-trips correctly before trusting anything layered on
   top of it (`repl`'s `telnet`, `sh.c`'s `tget`/`tput`) -- and
   separately confirm the DHCP-provided-nameserver path (phase 8's own
   option 6 parsing) actually gets exercised, not just the
   `NET_STATIC_DNS` override, since a bench setup used to test DHCP in
   isolation might not have a nameserver on option 6 at all.

The RMII backend (`mozart_ml1`) followed the same "build and verify
incrementally" approach but as its own separate staged plan, run in
parallel to this one rather than as a continuation of it (different
board, different hardware layer, same lesson applied again) -- see
"Second backend: RMII (mozart_ml1)" above for where it currently
stands (simulation-verified, not yet real-hardware-verified).

## TFTP debugging notes

TFTP transfers intermittently stalled (and, before some of these
fixes, crashed) partway through -- typically after 1-2 successfully
exchanged DATA/ACK blocks, though not at a deterministic point. Root
cause was eventually found and fixed (see the `objcopy` entry below);
transfers are now confirmed working reliably. Getting there took a
long debugging session, and turned up several genuine, unrelated bugs
along the way, all worth keeping regardless of whether any single one
was the full explanation:

- **UART FIFO race** (`sw/os/uart.c`): `k_uart_putc()`/`k_uart_getc()`
  and the `tx_full()`/`rx_empty()` status checks shared `tx_head`/
  `tx_tail`/`rx_head`/`rx_tail` state across processes without proper
  mutual exclusion -- only masking the UART IRQ specifically, not the
  timer IRQ that drives scheduling, so one process's write could be
  preempted mid-update by another process also writing. Fixed by
  masking all IRQs (`maskirq(0xFFFFFFFF)`) for these critical
  sections, matching the pattern `msg.c`'s mailbox push/pop already
  used. This was a real, confirmed bug (directly observed as garbled,
  interleaved UART output from two processes printing concurrently --
  the first time in this project two processes had ever done that
  frequently) and fixed a genuine crash. It did not fully resolve the
  underlying stall.
- **`z_translate()` didn't special-case pid 0**: caused a real crash
  the first time the kernel (`sh.c`, pid 0) ever sent a message
  directly to an app -- see the git history around `z_msg_send`'s
  kernel-side addition for the full explanation. Unrelated to the
  ongoing stall, but a necessary fix to get this far at all.
- **SPI transactions weren't protected from interrupt preemption**
  (`enc28j60.c`): every bit-banged SPI transaction now masks IRQs for
  its full CS-asserted duration, preventing a timer/UART interrupt
  from stretching a clock pulse mid-transaction. A real correctness
  concern for bit-banged SPI in general; didn't resolve the stall.
- **`z_kernel_ticks` incremented on any interrupt, not just the
  timer** (`kernel.c`): inflated elapsed-time measurements under heavy
  UART activity, making tick-based timeouts (TFTP's own retry timer,
  `z_msg_wait_timeout()`) fire much sooner than their real-time
  intent. A real, confirmed bug (directly observed: timeouts firing
  in 1-5 real seconds instead of the intended 10). Fixed, unrelated to
  the stall itself.
- **Stale reply tag matching** (`sh.c`): `tget`/`tput` used a constant
  tag (0) for every request, so a reply arriving after the shell's own
  timeout had already given up could get matched to a later, unrelated
  request. Fixed with a monotonically increasing tag per request.
- **BIOS interrupt handler stack possibly too small**
  (`sw/bios/boot_picorv32.S`): the C-level interrupt handler runs on a
  dedicated, tiny (originally 1KB) stack that sits directly below the
  interrupted process's saved register state (`irq_regs`, including
  its saved return address) -- if interrupt handling ever used more
  than that, it would silently corrupt whichever process got
  interrupted. Doubled to 2KB as a plausible, low-risk fix. Did not
  resolve the stall on its own, though it's a reasonable defensive
  fix to keep regardless.
- **Half duplex**: switched to full duplex as a hypothesis (see
  "Config" above for the reasoning at the time) -- didn't resolve the
  stall. Ruled out as the cause; kept as full duplex anyway since it's
  still the more broadly correct choice for a modern switched network.
- **CONFIRMED ROOT CAUSE: `objcopy -O binary`'s standard behavior
  silently truncates the output file before `.bss`.** `.bss` (all of
  `net`'s static/global storage -- `tftp_file_buf`'s 32KB,
  `last_pkt`, the ARP cache, everything) is the last section with a
  real load address in `sw/common/riscv-app.ld`'s layout. Since
  `.bss` sections are `NOBITS` (no actual file content -- just an
  address range to be zeroed at load time) and nothing follows it,
  `objcopy` doesn't pad the output to include it -- there's nothing
  after it to force that padding. So `net.bin`'s size on disk never
  reflected the process's true memory footprint.
  `k_proc_create(fs_size("net"))` (in `sh.c`'s `run`/`init`) allocated
  memory based on that undersized figure, adding a fixed 8KB for the
  stack on top -- and since the stack starts at the very top of that
  allocation and grows downward, it ended up overlapping directly
  with where the linker had actually placed `.bss`. Confirmed by
  disassembling `handle_packet()` (`net.dasm`) and finding
  `last_pkt`'s linked address only ~1.7KB below the top of the
  process's entire allocated region -- squarely inside what was
  supposed to be stack space. Every write to a static buffer was a
  potential live-stack-frame corruption, including (whenever the call
  depth at that moment happened to reach that far) directly
  overwriting a saved return address -- explaining both the apparent
  randomness (depends on the call chain's exact depth at the moment
  of the write, not on any TFTP protocol state) and why only `net`
  triggered it (by far the largest `.bss` footprint of any app built
  in this project so far).

  **Fixed** in `sw/apps/net/Makefile`: `net.bin`'s build step now
  extracts `_end`'s actual linked address via `nm` and pads the
  output to it (`objcopy --pad-to`), so the file's on-disk size
  matches its true memory footprint including `.bss`.
  **This is a systemic bug, not specific to `net`** -- every other
  app's Makefile (`wm`, `hello_win`, `ping`, `pong`, etc.) has the
  same unpadded `objcopy -O binary` call, and is exposed to the same
  issue in principle. They likely haven't shown symptoms yet only
  because their `.bss` sections are much smaller, making the overlap
  either nonexistent or small enough to not yet corrupt a live stack
  frame in practice -- worth applying the same fix to all of them
  rather than treating this as `net`-specific.

One methodological note: heavy diagnostic instrumentation (verbose
per-packet/per-register logging) was added and removed several times
during this investigation. At one point, removing most of it also
resolved a hard crash that had been occurring with it in place --
strong evidence that the instrumentation itself was affecting timing
enough to change the failure mode, not just observe it. Worth keeping
in mind if this needs further debugging: prefer reasoning through the
code over adding more logging, and if logging is added, treat its own
potential side effects on timing as a real variable, not a neutral
observation tool. In the end, what actually found this was reading
disassembly, not more runtime logging -- a good reminder that "read
the code" includes reading the compiler's own output, not just the
source.

## Testing tget/tput

Needs a TFTP server running on some machine on the same network --
e.g. `tftpd-hpa` on Linux, or any TFTP server app on another OS,
serving a directory you can read/write test files in. Then, from the
Zeitlos shell (after `run net`, or `init` -- see "`init`: running
`net` without `wm`" above):

```
> tget 192.168.178.<server> testfile.bin
> tput 192.168.178.<server> testfile.bin
```

Multi-block transfers in both directions are confirmed working on
real hardware against a real server. These specific edge cases
haven't been individually confirmed one by one, though, and are worth
checking if this needs further debugging or after any future change
to `tftp.c`/`zstream.c`:

- Does a small file (well under 512 bytes, so a single TFTP block)
  round-trip correctly? This is the simplest possible case and the
  first thing to try.
- Does a file that's an exact multiple of 512 bytes work? This
  exercises the "send/expect one extra empty block" edge case in
  `tftp.c` (see the comments around `last_chunk_len` in
  `put_deliver_eof()`, and the `delivered_len < TFTP_BLOCK_SIZE` check
  in `try_deliver_get()`) -- the part of the TFTP state machine most
  likely to have a subtle bug, since it's the one edge case that's
  easy to get wrong and easy to not notice being wrong (an off-by-one
  here would likely just hang until the retry timeout, not corrupt
  data).
- What happens requesting a file that doesn't exist on the server?
  Should surface as a clean `tget: failed: <server's error message>`,
  not a hang -- exercises the `TFTP_OP_ERROR` handling path.
- A file large enough to span many megabytes -- confirms there's
  genuinely no size limit left anywhere in the path (`net`'s own
  memory footprint should stay flat regardless of transfer size, per
  the whole point of the streaming rework).

## Historical notes

### Real-hardware bring-up

Real hardware testing confirmed, in order: SPI/chip init
(`enc28j60_revision()` returned a plausible ENC28J60 revision), raw
TX/RX (a test broadcast frame was accepted and a real ARP request
from another device was correctly received and decoded), ARP + ICMP
echo (`net` correctly answered an ARP "who-has" and replied to
`ping` from another machine), and TFTP GET/PUT streaming against a
real TFTP server (multi-block transfers completed successfully in
both directions).

Bring-up also turned up a real bug: the *first* received frame during
raw TX/RX testing was the driver's own test broadcast, echoed
straight back. The ENC28J60's internal PHY has a half-duplex loopback
that's enabled by default -- every transmitted frame gets looped back
into the RX path unless explicitly disabled (`PHCON2.HDLDIS`, bit 8).
Fixed in `enc28j60_init()`. (Unrelated to the half-vs-full-*duplex-
mode* question discussed in "TFTP debugging notes" above -- two
different things that happen to both involve the word "duplex".)
