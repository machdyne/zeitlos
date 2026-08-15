# Zeitlos Networking Developer Guide

## Overview

Networking is built around a Microchip ENC28J60 SPI Ethernet
controller on a PMOD, with a Wiznet W5500 (raw IP mode) planned as a
second, swappable backend later. It lives in its own app,
`sw/apps/net`, following the same "an app owns a piece of hardware and
mediates access to it via messaging" pattern the window manager
(`sw/apps/wm`) established for graphics -- see `docs/window_manager.md`
if that precedent isn't already familiar.

**Current status: ARP + ICMP echo (ping) + TFTP client, all confirmed
working on real hardware.** Hardware bring-up (SPI link + raw frame
TX/RX), ARP/ICMP, and TFTP (both GET and PUT, streaming, no file size
limit) have all been confirmed against real hardware and a real TFTP
server -- see "Confirmed working" sections below.

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

- **IP**: static, `192.168.178.230` (no DHCP client -- out of scope
  for a dev-loop tool). **Netmask and gateway are assumed, not
  confirmed** -- only the IP address itself was specified. `net.c`
  assumes a typical home-router `/24` (`255.255.255.0`) with the
  gateway at `.1` (`192.168.178.1`). This only matters for traffic to
  destinations outside the local subnet -- `ping`/`tget`/`tput`
  to/from a machine on the same `192.168.178.0/24` network don't
  depend on the gateway being correct. Correct the constants in
  `net.c` (`OUR_NETMASK`/`OUR_GATEWAY`) if this assumption is wrong
  for your network.
- **MAC**: `02:00:00:00:00:01`, hardcoded in `sw/apps/net/net.c`. The
  ENC28J60 has no factory-assigned address. `02` as the first octet
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

## Confirmed working, and one bug found

Real hardware testing confirmed, in order:

- SPI/chip init: `enc28j60_revision()` returned `0x06`, a plausible
  ENC28J60 revision.
- Raw TX/RX: a test broadcast frame was accepted, and a real ARP
  request from another device on the network was correctly received
  and decoded.
- ARP + ICMP echo: `net` correctly answered an ARP "who-has" for its
  own IP and replied to `ping` from another machine on the network --
  logged output confirmed both.
- TFTP GET and PUT, streaming, against a real TFTP server on the
  network: multi-block transfers (well beyond the size of a single
  TFTP block, exercising the full streaming/flow-control path on both
  ends, not just a one-packet edge case) completed successfully in
  both directions.

It also turned up a real bug: the *first* received frame during raw
TX/RX testing was our own test broadcast, echoed straight back. The
ENC28J60's internal PHY has a half-duplex loopback that's **enabled by
default** -- every transmitted frame gets looped back into the RX path
unless explicitly disabled (`PHCON2.HDLDIS`, bit 8). Fixed in
`enc28j60_init()`. (Unrelated to the later half-vs-full-*duplex-mode*
question discussed in "TFTP debugging notes" below -- two different
things that happen to both involve the word "duplex".)

## Layers implemented so far

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
5. A message-based UDP API for other apps (not just the shell) to use
   directly, reusing `Z_BLOB` and the same messaging shape `znet.h`
   already established for TFTP -- straightforward extension once
   there's an app that actually needs raw UDP rather than going
   through TFTP.
6. W5500 backend. Per the design discussion that led here: W5500 has
   its own hardware IP/socket layer (unlike the ENC28J60's raw MAC),
   so the swappable boundary for that backend is at the socket/UDP
   level, not the Ethernet-frame level that ARP/IP below it sit at --
   `net`'s driver-facing internals stay ENC28J60-specific; only the
   public UDP-facing API needs to be something a W5500 backend could
   also implement.

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
