# Userspace drivers

The filesystem, the SD card, the USB host stack and the network are
**driver processes**, not kernel code. The kernel is a scheduler,
memory, messaging, IRQ routing and protection; `fs`, `usb` and `net`
are programs with a flag in their header, loaded from the flash archive
(or the card) like any core app, preempted like anything else, and
started again if they crash.

Why: responsiveness first -- no I/O ever holds the scheduler -- and room
to grow: a driver that is a program is not bound by the kernel's share
of the 1MB flash region, nor by flash at all.

## Status (October 2026)

- **Phases 0-3 built, and the polish of round 10.** Run on lakritz: it
  boots, `fs`, `usb` and `net` come up, `/ram`, the card, a USB stick
  and USB ethernet work.
- **The kernel image is 137,528 bytes** (it was 254,176 with FatFs and
  USB inside it), in a 160KB region; the core app archive has 848KB.
- **Phase 4** (splitting the kernel shell out) is not planned; the
  shell stays in the kernel, perhaps trimmed later.
- **Open:** TFTP over USB ethernet runs at 30-36KB/s against ~50KB/s
  with the stack in the kernel; the keyboard and mouse were once not
  set up at boot. Both are under "Open issues" below.

## How it works now

| driver | what it does | starts from | registers | docs |
|---|---|---|---|---|
| `fs` | FatFs, the SD card, `/ram` and `/tmp`, the flash underlay, executables from the card | flash, first | `fs0` | [fs_driver.md](fs_driver.md) |
| `usb` | the USB host: hub, HID setup, mass storage, CDC serial, ECM ethernet | the card if there, else flash, after `fs` | `usb0` | [usb_driver.md](usb_driver.md) |
| `net` | ethernet (RMII, ENC28J60, ESP32 link, USB through `usb`), IP, TCP, TFTP, SSH ... | `init` | `net0` | [networking.md](networking.md) |

- **Clients** use small libraries that turn calls into requests:
  `zfsc.c` for files (also inside the kernel), `zusbc.c` for USB,
  `zposix.c` for libc's `open()`/`fopen()` ([posix_io.md](posix_io.md)).
- **What the kernel keeps:** processes, scheduling, memory, messaging,
  the name registry, IRQ routing, the MPU, exec from the flash archive,
  the UART console and panic, kvstore, auth, cfg, the hardware HID
  ports, the RAM disk's memory (`kblk.c`) and the kernel shell. See
  "Architecture" below.
- **The UART is the rescue console**: without `usb` there is no USB
  keyboard for the kernel shell.
- **Tools:** `lsusb`, `usbstat` (the USB ethernet path's counters),
  `ps`, `pr`, `ic`; `kill -c <pid>` (a crash, to test restarts);
  `sched` (scheduler variants, see round 10); a `KTRACE=1` kernel's
  `ktrace`; `ipcbench` for round trips and the longest stall.

## Drivers

Decided in round 4 (October 2026).

**What makes a program a driver: a flag in its own header.** ZEXE flags
bit 15 ([executables.md](executables.md), "The driver flag"), set with
`APP_DRIVER = 1` in the app's Makefile. Not a list of names in the
kernel: a new driver needs no kernel change, and can be added to a
running system.

**Declaring is not granting.** The flag makes the process
`Z_PROC_FLAG_DRIVER`, which lets it *open* the grant window -- but only
onto a process that has opted in to being written by drivers
(`z_grant_drivers()`), one client at a time ([mpu.md](mpu.md), "The
grant window"). Clients opt in through the libraries that talk to
drivers (the file and stream client code), on first use; an app never
does it itself, and an app that does no I/O keeps full protection.

**Clients allow the class, not pids.** So a restarted driver (a new
pid) needs nothing re-granted, and a chain works without delegation: an
app that talks only to `fs` can still be written directly by `usb` when
its file is on `/usb`. Nothing is kept per pid, so this costs the same
at any `Z_PROCS_MAX`.

**Started like any other program.** `run`, `z_proc_run()`, init -- the
same plumbing. No `drv` command and no separate start path: they would
be machinery for something that already works.

**Boot order.** The kernel shell starts `fs` from flash and waits until
it has registered, then `usb` (from the card if a copy is there), then
reads the config and runs `init`, which starts `wm`, `net` and the
rest (`sh()`, `sw/os/sh.c`). Nothing depends on a fixed pid any more:
clients find drivers by name (`fs0`, `usb0`, `net0`).

**`net`** is a driver in this sense too (`APP_DRIVER = 1`): it binds the
Ethernet interrupt and talks to `usb` for a USB adapter. Writing
received data straight into an app's buffer helps less than it first
looks: a borrowed message blob is already zero-copy for a receiver
that consumes the data at once, as `tget` does, and the kernel shell's
`tget` is pid 0, whose memory must never be opened to a driver anyway.
The copy saved is in a receiver that *keeps* data, and only if `net`
copies from the MAC into that buffer directly; a change inside `net`'s
receive path, for later.

## Restarting drivers

A driver is a process, so a fault in one ends it and the kernel keeps
running. But the machine is not much use without its filesystem, so
when `fs`, `usb` or `net` **crashes**, the kernel starts it again
(`kernel.c`, "restarting a driver"; `k_drv_start()` in `sh.c`):

- **Who:** a process with the driver flag whose registered name is
  `fs<n>`, `usb<n>` or `net<n>`. Not one that exits (`usb` without a
  controller means to be gone) or is killed.
- **When:** a second after the crash, by pid 0, between lines of input
  -- the same place a byte from the UART wakes it. `k_fault()` only
  notes it, in the interrupt, and wakes pid 0 if it is waiting for
  input. If pid 0 is busy running a command, the restart waits for the
  prompt.
- **How:** as at boot. `fs` from flash; `usb` and `net` from the card
  when a copy is there, else flash.
- **To try it:** `kill -c <pid>` in the kernel shell kills a process
  as if it had crashed.
- **How often:** at most 3 times a minute each. A fourth crash in the
  minute prints `drv: X crashed again ...; not restarting it` and
  leaves it for `run X`.
- **Clients** find the new one by name: `zfsc.c` and `zusbc.c` look the
  pid up again when a send fails or a call times out; `net` checks once
  a second that `usb` is the one its rings were opened with; `usb`
  offers drives to `fs` from the start; `fs` polls `usb` each second.
  What each loses is in [fs_driver.md](fs_driver.md) and
  [usb_driver.md](usb_driver.md), "Restarting".

Kernel cost: about 1KB.

## Growing past the flash region

One reason for drivers as processes, recorded here because it is easy
to miss: **a driver in the kernel can never be bigger than the
kernel's share of the 1MB flash region, and a driver in the archive
shares the same megabyte.** A driver that is a program does not have to
be in flash at all:

- **Card copies.** A core app on the card wins over flash, so the flash
  copy of `usb` or `net` can be the small one that brings the machine
  up, and the card's the full one. Only `fs` must stay in flash.
- **Separate processes.** `net`'s SSH, telnet or TFTP could move out,
  as `web` already did for HTTP and TLS.
- **Driver modules.** `usb` could hand a device to a class-driver
  process; `fs` could pass a mount point to a filesystem process.

None of that is done; the drivers are not split. What Phase 3 settled
is that it is possible. The details are in
[usb_driver.md](usb_driver.md), "Drivers past the 1MB flash region".

## POSIX file I/O

`fs` needs a new client protocol whatever happens, because the
`Z_SYS_FS_*` syscalls go away. That protocol should be POSIX-shaped
from the start -- descriptors, `O_*` flags, `lseek` with `whence`,
`struct stat`, `opendir`/`readdir` -- rather than a message version of
`zfsapp`'s shape that would then be redone.

On top of it, in `sw/common`:

- **The real libc hooks**: `_open`, `_read`, `_write`, `_lseek`,
  `_close`, `_fstat`, plus `stat`, `unlink`, `rename`, `mkdir`,
  `rmdir`, `opendir`/`readdir`/`closedir`, `chdir`/`getcwd`. That makes
  `fopen`/`fprintf`/`fgets` work too, and retires the weak `ENOENT`
  stubs in `sw/common/zeitlos.c` and the private shim in
  `sw/apps/vi/fileio.c`. Descriptors 0-2 stay the console.
- **`zfsapp` reimplemented on the same client**, unchanged in its
  signatures (`fs_open_read()` is `open(O_RDONLY)`, `fs_mallocfile()`
  is `stat` + `read`). One implementation underneath, two APIs above;
  apps move over one at a time, or never.
- **libz** gains the POSIX entries, appended to the table.

Stdio is opt-in by use: an app that calls only `open`/`read` does not
link `fopen`'s buffering, so the small apps stay small.

## Open issues

### TFTP over USB ethernet: 30-36KB/s, not 50

Where it stands after round 10 (details in its log below):

- **Per block, ~10ms -- the monolithic kernel's pace.** What made it
  slower is fixed: `tget` in the kernel shell spun while it waited for
  each block (it took whole ticks from `usb` and `net`), and `usb`
  sometimes failed to wake `net` for a frame that had arrived.
- **What remains is one or two blocks a transfer that never reach
  `usb`**, each costing TFTP's fixed 0.5s retry. At the retry `usb`'s
  ring is empty, no frame is damaged, none arrives twice. So the frame
  is lost between the server and the adapter, or our ACK between the
  adapter and the server.

Next:

- `tcpdump -n udp and host <board>` on the server during a `tget`:
  an ACK that never arrives, or a block sent and never received.
- The same `tget` on the monolithic kernel, for its retry count: if it
  loses a frame now and then too, this is not a regression.
- A TFTP retry timeout matched to the round trip (~10ms here) instead
  of 0.5s, so a lost frame costs ~100ms.

### Keyboard and mouse not set up at one boot

Once, only the USB ethernet adapter was enumerated at boot; the next
boot was fine. During the TFTP tests, with a scheduler variant
(`sched drvwake on`), both devices were also torn down and set up again
mid-transfer; that variant is off. Next time: `lsusb` (which device is
behind which port or hub, and the hub's error counts), then
`usbrescan`.

### Measurement plan (the end of the work)

On the same MPU-v1 board as rounds 7b-7e, with `net` and a stick:

1. **TFTP.** `usbstat` (to reset), `tget` of a 1MB file, `usbstat`
   again; `ps` afterwards (the console is busy during a `tget`). The
   TFTP lines give the time a block takes on each side and, for each
   retry, the block, the moment and `usb`'s ring.
2. **`ipcbench`**, all sections, including `/usb`, against round 7e.
3. **Responsiveness.** Drag a window while a stick is plugged in and
   mounts, while `tget` runs, and while a big file is read: the
   longest stall (`ipcbench` [3]) against the 5.6ms of round 7b.
4. **Restart.** `kill -c <pid>` ends a driver as if it had crashed:
   `fs` back with `/ram` intact; `usb` back with the stick remounted
   and `net` reopened; a fourth `kill -c` in a minute left alone.
5. **Idle load** (`ps`) with nothing plugged in, then with a hub,
   against round 5c.
6. **Sizes:** the kernel image, the archive, and the RAM left with the
   boot set running.

## The design

What follows is the design as it was agreed before the work, kept
because the reasons still hold, then the phase plan. The rounds at the
end record what was built and measured, in order.

## The criterion: responsiveness first

Agreed after Phase 0 (October 2026). **Responsive and fast are
different things.** Some operations must take time -- copying a file,
fetching a page -- but the system never stops responding while they
do: you can always do something else. So what this project must not
make worse is *the longest time anything waits*, and what it is allowed
to make slightly slower is an individual small operation.

That makes the move a gain on its own terms: today FatFs and USB
transfers run with the scheduler held. As processes they are preempted
like anything else. Two rules follow:

- **The system never stops scheduling for I/O.** `ipcbench`'s "longest
  gap" is the headline measurement of every phase.
- **Interactive paths make one request, not many.** wm's dock asking
  which of ~30 apps exist is one call (`z_exec_exists_many()`); a file
  listing is one request for the whole directory; `fs` can cache.

And one requirement from the hardware side: **a driver must be able to
write into its client's buffer directly**, as the kernel does, so bulk
reads cost what they cost today. That is the MPU's grant window
([mpu.md](mpu.md), "The grant window").

The goal, stated as a test: `sw/apps/fs` and `sw/apps/usb` as driver
apps, with copies as fast as the kernel's and no noticeable slowdown
anywhere -- then it is a win.

## Why this is feasible here

Several properties of Zeitlos that look like limitations turn out to be
exactly what userspace drivers need:

- **Apps can already reach hardware.** There is no privilege mode on
  MMIO; the MPU decides per address nibble with `MASK`
  ([mpu.md](mpu.md)). `net` drives the RMII MAC and the ENC28J60
  directly from userspace today. The SD card is the only peripheral
  masked off, and only by a global register the kernel sets once.
- **There is no DMA to translate.** The USB host and the SD controller
  are CPU-copied (the USB packet buffer is its own BRAM at
  `0xc000_1000`). A driver's buffers at `0x8000_0000` never have to be
  seen by a bus master, so the MTU window is no obstacle.
- **Any process can read any memory.** A server can read a client's
  write buffer in place, and a client can read a server's buffer in
  place. Only *writes* across processes need the kernel.
- **The interrupt-to-process pattern exists.** `Z_IRQ_ETH` unblocks
  whatever registered as `net0`, with `Z_PROC_FLAG_WAKE` closing the
  early-wake race. Generalising that is most of what an IRQ API needs.
- **Drivers can live in flash with no filesystem.** The archive is
  memory-mapped; the kernel starts core apps from it with a copy. The
  filesystem server itself can therefore be loaded before there is a
  filesystem.
- **The MPU already contains crashes.** A faulting driver ends with a
  report instead of taking the kernel with it, and can be restarted.

## The one hard constraint: the kernel never waits on userspace

A syscall is a plain `jalr` with no saved frame
([kernel.md](kernel.md), "The syscall boundary"). The kernel cannot
switch away from inside one. So **kernel FatFs calling a userspace
block driver is not possible** without giving the kernel blocking
syscalls (kernel stacks, or continuations) -- which is a larger and
riskier change than moving FatFs.

That decides the shape: **the filesystem moves with the SD driver.**
FatFs, the VFS mux, `/ram` and the flash-file underlay become one
server process, `fs`, and the block backends sit underneath it.

The rule that keeps the design honest, and deadlock-free:

> The kernel never makes a synchronous call into a process.
> Drivers never call upward (a block driver never calls `fs`).

## Architecture

```
            apps (wm, term, files, text, posix, zcc ...)
              |  z_fs_*()  -- same C API, now a request/reply message
              v
   +----------------------+        +-----------------------+
   | fs   (core driver)   |<------>| usb  (core driver)    |
   | FatFs, VFS, /ram,    | ring   | host, hub, HID, MSC,  |
   | flash underlay,      |        | CDC, ECM, cp210x      |
   | SD backend (linked), |        +-----------------------+
   | card-exec loader     |          |  ring        |  input inject
   +----------------------+          v              v
              |                    net (eth)      kernel input queue
              |  z_proc_image_write()
              v
   +---------------------------------------------------------------+
   | kernel: procs, sched, mem, msg/ports, pidreg, IRQ routing,    |
   | MPU/MTU, exec from flash archive, UART/klog/panic, kvstore,   |
   | auth, cfg, hardware HID ports, monitor shell                  |
   +---------------------------------------------------------------+
```

### What stays in the kernel, and why

| | why it cannot (or should not) move |
|---|---|
| scheduler, procs, mem, messaging, pidreg | the microkernel itself |
| IRQ routing, MPU/MTU programming | the MTU/MPU registers are kernel-only in hardware |
| exec from the flash archive | `fs` itself is started this way, before any filesystem exists |
| kvstore, kvlog, auth | **flash stores are kernel-only in `rtl/mpu.v`**; moving them needs RTL |
| UART, console log, panic screen | must work when every process is dead |
| `hid.c` (hardware boot-keyboard ports) | 1.7KB, and it is the rescue console's input |

### Moving data without losing speed

| direction | how | copies |
|---|---|---|
| app writes a file | `fs` reads the app's buffer in place (reads are global) | same as today |
| app reads a file | `fs` reads into its own buffer; `z_copy_out()` (kernel, privileged) writes it into the blocked client's buffer, checked against that client's block | one more `memcpy` than FatFs's direct multi-sector read |
| USB MSC <-> `fs`, USB ECM <-> `net` | single-producer/single-consumer rings: each side writes only its own memory (data and head in the producer, tail in the consumer) and reads the other's; a message only when a ring goes non-empty | none extra |

A ring needs no lock: the CPU is single-core, the data cache is the
CPU's own, and each index has exactly one writer.

### Latency: the part that needs kernel work

Today a message send only unblocks the receiver; it runs when the
round-robin reaches it, which with several runnable apps is several
1.37ms timeslices. For RPC that is fatal. Three mechanisms fix it:

1. **Call with handoff** -- `z_msg_call()`: send, block the caller, and
   switch directly to the receiver if it is runnable. The reply hands
   back the same way. One switch each way, no scheduler pass.
2. **A driver class** -- `Z_PROC_FLAG_DRIVER`: runnable drivers are
   picked before ordinary apps, and an IRQ that wakes one switches
   immediately rather than at the next tick.
3. **Cheap switches for drivers** -- a driver never draws, so the GPU
   scissor save/restore in `k_sched_switch()` (which can wait for the
   rasterizer) is skipped for it.

## Performance: what is expected, and what is checked

Estimates first, labelled as such; Phase 0 replaces them with
measurements, and those become the acceptance criteria for every later
phase.

| path | today | after | expectation |
|---|---|---|---|
| bulk file read (8KB chunks, SD) | 604 KB/s, scheduler held for the call | RPC + one extra copy per chunk | -2 to -5% throughput |
| small FS op (stat, open, close) | one syscall | one call/reply | +20-40 µs each (estimate) |
| USB MSC read | in-kernel, scheduler held per transfer | ring to `fs` | about the same |
| USB ECM frame | syscall per frame, scheduler held | ring, batched notifications | about the same or better |
| USB HID key | ISR to input queue | IRQ -> `usb` -> inject | +1 switch, well under a frame |
| **everyone else during disk I/O** | **preemption off for up to 87ms** (`K_NO_PREEMPT_MAX_TICKS`) | **never off** | **better** |

The last row is the honest headline. FatFs and the USB stack run today
with the scheduler held, so a card read or a USB transfer stalls the
window manager, audio and the network for its whole length. As
processes, they are preempted like anything else, and `fs` serialises
its own requests -- FatFs's non-reentrancy becomes a property of one
process instead of a system-wide lock. `k_no_preempt` and
`k_syscall_touches_fs()` go away.

## Alternatives considered

- **Loadable kernel modules** (privileged code loaded from the archive
  into kernel memory). No IPC cost and it frees the budget, but no
  containment, and privilege is decided by *fetch address* -- code
  outside kernel text up to `KTEXT` is unprivileged, so modules would
  need the MPU's privileged range made dynamic. It gives up the point
  of the exercise. Rejected.
- **Block drivers in userspace, FatFs in the kernel.** Needs blocking
  syscalls (see the hard constraint above). Rejected.
- **A second MPU write window** (RTL) so `fs` can write straight into
  the client's buffer, removing the extra read copy. Cheap in logic,
  but a gateware change; kept as an option for phase 5 if the
  measured copy cost justifies it.

## Risks

- **ABI break for files.** The `Z_SYS_FS_*` syscalls become messages.
  Source-level `z_fs_*()` is unchanged, but every binary on a card is
  rebuilt -- which `syscalls.def` coupling already requires for any
  syscall change. libz's table keeps its entries (and ABI number); the
  blob is rebuilt.
- **`fs` is a single point of failure.** It already is, as kernel
  FatFs; the difference is that it can now be restarted.
- **Boot order.** `cfg` reads its file from the card
  (`Z_SYS_CFG_RELOAD`), and `init` looks for `posix` on the card; both
  now wait for `fs` to register. The kernel shell must still come up
  with no drivers at all.
- **RAM on the smallest board** (`CORE_RAM_MAX` = 1MB): `fs` at boot
  must fit beside `wm`, `net` and `console`. Phase 2 checks it with
  `zrelease check`, not by estimate.
- **Not a security boundary.** The MPU mask is per 256MB nibble and
  every process can read everything. This is containment and
  modularity, as the MPU already is.

## Phases

Each phase ends with the system working, the Phase 0 benchmarks re-run
against the baseline, and this document updated.

### Phase 0 -- measure, and make room (no behaviour change) -- done

**Measuring: built** (`sw/apps/ipcbench`). The layout change below is
not started; it waits on the decision the numbers inform.

Copy `ipcbench` to the card's `/apps` (it starts copies of itself by
name, so it must be findable by `run ipcbench`), then from the kernel
shell, with nothing else running for the cleanest figures:

```
> run ipcbench
```

It prints to the console and takes a few seconds. What each section
answers:

| section | measures | what it decides |
|---|---|---|
| [1] baseline | a syscall, an empty `z_msg_read`, `memcpy` of 8KB | the cost a message replaces, and the cost of the extra copy on a file read |
| [2] round trip, idle | PING/PONG between two blocked processes: two sends, two blocks, two switches -- what a call with handoff costs, without the handoff code | **the gate.** Under ~50 µs (2,400 cycles) and the design stands. Hundreds of µs means the switch itself is the problem (`k_sched_switch()`, the GPU scissor wait) and is fixed first |
| [2] with 1-3 CPU-bound processes | the same, with round-robin in the way | expected to grow by about a tick per spinner. Confirms the driver class and handoff are needed; not a go/no-go |
| [3] wakeup latency, idle | longest gap in a `z_proc_wait(1)` loop | the floor for [4]'s stall figures |
| [4] `/ram` and card, stat and open+close | small-op cost through FatFs | overhead of a small op as a message = round trip / this |
| [4] 8KB chunked read | throughput, and one `read_chunk` | overhead of a bulk read = (round trip + 8KB `memcpy`) / one chunk |
| [4] "stall during ..." | the longest the sleeper waited while the master did I/O | **what the move removes**: today's scheduler-held windows |

Paste the output into this section when it has been run.

#### Results (first run, 2026-10-10)

Run from the kernel shell with `wm`, `net` and `console` resident
(`net` had used 18% of all ticks since boot while idle). Minimum is the
intrinsic cost; the average includes whoever else ran.

| | min | avg |
|---|---:|---:|
| syscall (`z_getpid`) | 31.6 µs (1,517 cyc) | 72 µs |
| `z_msg_read`, empty | 38 µs | 94 µs |
| `memcpy` 8KB | 1,976 µs (~4 MB/s) | 3,115 µs |
| round trip, idle | **1,225 µs** | **2,560 µs** |
| round trip, +1 / +2 / +3 spinners | 2,257 / 3,405 / 4,765 µs | 3,908 / 6,217 / 8,013 µs |
| `/ram` stat, open+close | 803, 1,283 µs | 1,010, 1,391 µs |
| card stat, open+close | 813, 2,229 µs | 1,157, 2,454 µs |
| one 8KB `read_chunk`: `/ram`, card | 1,999, 9,960 µs | 2,237, 10,222 µs |

Throughput: `/ram` 3,169 KB/s write, 3,562 read; card 327 write, 772
read. Longest wait a sleeping process saw (a tick is 1,365 µs): 3.1 ms
idle; **1,544 ms** during the card write, **142 ms** during the chunked
card read, 61 ms during the chunked `/ram` read.

What it says:

1. **The round trip is tick-bound, not cost-bound.** 1.2 ms at best is
   about one tick; the average is two. The block-and-yield path
   (`k_proc_yield_blocked()`) should switch at once, so something puts
   a tick between send and wake. That is 25-50x the gate and is the
   first thing to find -- and it is paid today by every request/reply
   in the system (`wm` and its clients, `term` and its shells), not
   only by a future driver.
2. **The floor is high even without that.** A syscall costs ~1,500
   cycles; a round trip is ~6 syscalls and 2 switches, so a fixed path
   would still be roughly 0.3-0.5 ms -- about a stat. Small FS
   operations would cost up to twice what they do now.
3. **Copies are not cheap here.** 8KB of `memcpy` is 2-3 ms: 20-30% of
   a card chunk and about all of a `/ram` chunk. The "one extra copy"
   design for reads is ruled out; `fs` would need the MPU grant window
   (RTL) so it can write the client's buffer directly.
4. **A fairness bug, independent of all of this.** A KTIMER tick that
   lands inside a filesystem syscall is deferred and then *dropped*:
   `k_fs_leave()` does not switch when the guard clears, and the
   process goes straight into its next filesystem call. A process
   doing back-to-back file I/O starves everyone else -- 1.5 s during
   a 512KB card write. `K_NO_PREEMPT_MAX_TICKS` never fires, because
   it is measured per call.

**Decision: not yet.** On today's IPC the move would lose performance.
Items 1 and 4 are worth fixing for their own sake; once they are, the
benchmark is re-run and this is decided again.

#### Follow-up (round 2)

- **Item 4 fixed**, awaiting a hardware re-run: a deferred swap is now
  owed and paid when the guard drops (`docs/filesystem.md`, "A deferred
  swap is owed, not dropped"). Expect the "stall during ..." lines to
  fall to a few ticks.
- **Item 1 traced, and the premise was wrong.** No tick is lost. The
  `KTRACE=1` trace of six idle round trips (`docs/kernel.md`,
  "Scheduler trace") shows every step happening at once, and simply
  costing a lot. One hop -- a process sends, blocks, and the other
  process runs and reaches its reply -- in cycles:

  | step | cycles |
  |---|---:|
  | send syscall, to the unblock | ~500 |
  | back to the app, an empty `z_msg_read`, `z_proc_wait` up to BLOCKED | ~4,850 |
  | `k_proc_yield_blocked()`: the 32-slot runnable count | ~2,800 |
  | `timer(1)` to interrupt entry (`irq_vec` saves 32 registers) | ~1,450 |
  | interrupt entry to switch (`k_sched_switch()`: the count again, the GPU scissor, the register copy, the scan) | ~9,000 |
  | switch to the other process's reply (`irq_vec` restore, return, `z_msg_read`, the send) | ~9,500 |
  | **one hop** | **~28,000** |

  Two hops are ~56,000 cycles, 1.17 ms -- the measured minimum. The
  average is about twice that because `net` was runnable on almost
  every tick (it waits `z_proc_wait(1)` in this configuration) and
  round-robin put it between the two hops most of the time.

  The scan is visibly expensive: a switch that wraps from pid 6 round
  to pid 2 costs ~13,600 cycles against ~9,000 for 4 to 5, i.e. about
  175 cycles for each empty slot passed.


- Still to measure: `/usb` throughput and `net` over USB ECM -- both
  matter for phase 3, not for the go/no-go.
- Flash layout: `KERNEL_MAX_SIZE` and `Z_ZAR_FLASH_OFFSET` from one
  definition; archive moved to `+0x30000`; layout checks updated.
- **Gate:** the numbers. If a round trip with handoff cannot be brought
  under ~50 µs in phase 1, stop and reconsider.

### Phase 1 -- kernel mechanisms (~2-4KB of kernel) -- done, round 6

- `z_irq_bind(irq)` / `z_irq_ack(irq)`: the kernel masks a bound line,
  wakes the owner, unmasks on ack (level-sensitive lines -- USB, audio
  -- cannot be left asserted while a process gets around to it).
- Per-process MPU `MASK`, written on context switch beside
  `reg_mpu_size`; only drivers started from flash get extra nibbles.
- `Z_PROC_FLAG_DRIVER` scheduling, IRQ-wake switching, scissor skip.
- `z_msg_call()` with handoff; `z_copy_out()`; `z_proc_image_write()`.
- Driver start list (auto / `sys.drivers`), and `drv` in the shell.
- **Proof:** `net` moves from the hard-wired `net0` wake to
  `z_irq_bind(Z_IRQ_ETH)`, and must measure no worse.

### Phase 2 -- `fs`: FatFs, SD, `/ram`, flash underlay, card exec -- done, rounds 7-8

- New core app `drv.fs`; SD backend linked into it (one less hop per
  block), speaking the POSIX-shaped protocol above.
- The libc hooks and `zfsapp` on one client in `sw/common`; existing
  apps keep compiling unchanged. Porting apps to POSIX calls is
  ongoing work after this phase, not part of it.
- Card executables: `k_proc_run()` creates the process in a LOADING
  state and asks `fs` to fill it (`z_proc_image_write()`), so `wm`'s
  dock and the shell keep their one call.
- Remove `k_no_preempt`, `k_syscall_touches_fs()`, SD's MPU mask from
  the default. Kernel ~-45KB.
- **Accept:** bulk read within 5% of baseline, no lost files across
  `fs` restart tests, `posix`/`zcc`/`files` unchanged at source.

### Phase 3 -- `usb`: host, hub, HID, MSC, CDC, ECM -- done, round 9

- New core app `drv.usb`; IRQ 9 bound, timed states on `z_proc_wait`.
- MSC as an `fs` block backend over a ring; ECM to `net` over rings
  (replacing `Z_SYS_USBNET`); CDC to `term` over ports (replacing
  `Z_SYS_USBCDC_*`); HID keys and mouse into the kernel input queue.
- `USBH_DEBUG` tools can stay built: they no longer cost kernel bytes.
  Kernel ~-30KB.
- **Accept:** `/usb`, USB ethernet, CDC serial, hub and HID as today;
  hot-plug; throughput within 5%.

**Built in round 9**, with these differences ([usb_driver.md](usb_driver.md)):

- **Storage is requests to `usb`, not a ring.** A sector is one SCSI
  command on the wire anyway.
- **CDC is requests too**, through the same three calls apps already
  used. `serial` remains the port provider.
- **HID needed nothing:** the controller already feeds the kernel's
  HID registers with no CPU in the path.
- **The kernel shrank by about 34KB of USB, and 57KB more** because its
  image stopped carrying `.bss`.

### Phase 4 -- shell split (not planned)

The user's decision (round 10): the kernel shell stays in the kernel.
It may be trimmed later, but not spun out. The plan was:

- A small monitor stays in the kernel (`ps`, `kill`, `mem`, `run`,
  `log`, `mpu`, `drv`, `reboot`); everything that is a filesystem or
  USB client moves to a core-app shell. Kernel ~-15-20KB.

### Phase 5 -- polish -- mostly done, round 10

- Restart policy for crashed drivers (done, round 10); libc file I/O,
  late replies, `fs`'s reply slots, a USB stick started without a
  pause (done, round 10); card drivers
  shadowing flash ones (done for `usb` and `net`, round 9); the MPU
  second write window if Phase 0-2 numbers ask for it.

## Implementation log

The rounds as they happened: what was built, what was measured, what
was learned. Numbers in earlier rounds describe the system at the time.

## Before: where the kernel was (Phase 0 baseline)

Measured on commit `22acf58` (rv32im, newlib, xPack GCC 15.2,
`make -C sw/os`): **`kernel.bin` is 254,176 bytes -- 7,968 bytes free
of the 262,144 the BIOS copies.** The 256KB wall is not a future
problem; the next driver of any size does not fit.

Linked bytes by subsystem, from `kernel.map` (text + data + bss, since
`.bss` is flash image here -- [kernel.md](kernel.md), "The 256KB image
budget"):

| subsystem | bytes | files |
|---|---:|---|
| core: procs, sched, mem, msg, pidreg, exec, ZAR, syscalls | 95,487 | `kernel.c`, `msg.c`, `mem.c`, `pidreg.c`, `zar.c`, ... (includes `z_mailboxes` 24,960 and `z_procs` 6,016 of `.bss`) |
| FatFs + VFS (`/`, `/ram`, `/usb`, flash underlay) | 42,418 | `fs/fatfs/ff.c`, `fs/fs.c`, `fsapi.c`, `ffunicode.c`, `diskio_mux.c`, `ramdisk.c` |
| USB host + class drivers | 29,699 | `usb/*`, `usbcdcapi.c`, `usbnetapi.c` |
| kvstore, kvlog, auth, cfg | 25,970 | |
| kernel shell + xmodem | 23,005 | `sh.c`, `xmodem.c`, `xfer.c` |
| libc / libgcc | 19,824 | |
| UART + console log | 8,646 | `uart.c` (4KB `k_klog`) |
| SD card | 4,667 | `fs/fatfs/sdmm.c`, `fs/sdbench.c` |
| hardware HID ports | 1,692 | `hid.c` |
| panic font | 1,536 | |
| **total** | **252,944** | (+ alignment = 254,176) |

The core apps archive is in the same position: 746,048 bytes used of
its region (`docs/flash_apps.md`). Both halves of the 1MB are full.

## Space: where the bytes go

| | kernel | archive |
|---|---:|---:|
| today | 254,176 | 746,048 |
| after phases 2-3 (estimate) | ~180,000 | + `fs` ~55KB, `usb` ~40KB |
| after phase 4 (estimate) | ~160,000 | + shell ~30KB |

Moved code costs a little more outside the kernel than inside it --
each driver carries its own runtime (about 10-15KB with `ZFMT`). That
is paid for by **letting the archive start where the kernel ends**:

- `KERNEL_MAX_SIZE` drops to 192KB (then lower if phase 4 lands) and
  `Z_ZAR_FLASH_OFFSET` moves to match.
- **No BIOS or gateware change.** The BIOS still copies a fixed 256KB;
  the bytes past `_end` are the start of the archive, land in memory
  the kernel never reserved (process 0 reserves `_end` plus its stack),
  and are overwritten by the first process loaded there. `.bss` is
  still zeroed by the padding, as now.
- Kernel and archive are flashed together already (`dev-flash`,
  releases, DFU), which is what makes the move safe. `release/lib/layout.py`,
  `sw/apps/zfpga/boot.c` and the top-level `Makefile` change with it.

### If the kernel keeps its 256KB

Reserving the full 256KB for the kernel whatever its size is simpler
(no offset moves, room to grow back), and it is a choice about flash,
not about the design. The arithmetic, at today's figures:

| | today | drivers out, 256KB kept |
|---|---:|---:|
| kernel region used | 254,176 of 262,144 | ~180,000 of 262,144 (~80KB idle) |
| archive region used | 746,048 of 770,048 | ~840,000 of 770,048 -- **~70KB over** |

So keeping 256KB means something else gives: a core app leaves flash
(`repl`, at a 192KB budget, is the largest), or the Zeitlos region
grows past 1MB on boards whose flash has room. The middle course is a
256KB *ceiling* rather than a reservation: the archive starts where
the kernel ends, rounded to a sector, and moves if the kernel grows --
which is safe only because kernel and archive are always flashed
together.

RAM: the kernel's own reservation shrinks by the same ~75KB; each
server costs one process block (32KB minimum today) plus its stack. A
board without a USB host does not start `usb` and pays nothing for it.

## Round 3: cheaper messaging, the grant window, the dock

All three built; awaiting hardware.

| | what | where |
|---|---|---|
| 1 | the scheduler keeps runnable/dying processes as bitmasks (`k_proc_flags()`), so counting and choosing the next process no longer walk the 32-slot table | `sw/os/kernel.c`, [kernel.md](kernel.md) "The scheduler" |
| 1 | `z_msg_recv()`: receive, waiting in the same syscall -- one call where read/wait/read was three | `Z_SYS_MSG_RECV`, [messaging.md](messaging.md) |
| 2 | the MPU grant window: a second store region per process, opened only where the other process allowed it; the kernel accepts it for syscall buffers too (the pid-based allow was replaced in round 4 by the driver class) | `rtl/mpu.v` (version 2), `Z_SYS_MPU_GRANT`, [mpu.md](mpu.md) |
| 3 | `z_exec_exists_many()`: which of a list of programs exist, one pass over `/apps`; wm's dock uses it | `Z_SYS_EXEC_EXISTS_MANY`, [window_manager.md](window_manager.md) |

`ipcbench` measures each: [2] the round trip both ways (read/wait/read
and `z_msg_recv()`), [5] another process filling a buffer through the
grant window and being refused after a revoke, [4] "read, by another
process" -- a file read straight into this process's buffer by the
echo process, which is what `fs` would do -- and [6] the dock's check
one by one and batched.

The grant window is gateware: without a rebuilt bitstream the kernel
sees MPU version 1, `z_grant_*()` fail, and ipcbench says so and skips
[5] and the cross-process reads. Everything else works on the current
bitstream.

Kernel image: 256,584 bytes (5,560 free); with `KTRACE=1`, 260,880
(the ring is now 256 events to keep it under the limit).

## Round 4: drivers as a class, pid sets

| | what | where |
|---|---|---|
| ZEXE flags bit 15 | the program is a driver; `APP_DRIVER = 1`; the loader marks it `Z_PROC_FLAG_DRIVER` | `sw/common/zexec.h`, `tools/mkexec.py`, `sw/common/app.mk`, [executables.md](executables.md) |
| grant by class | a client allows drivers (`z_grant_drivers()`); only a driver may open, onto one opted-in client at a time; replaces round 3's per-pid allow | `sw/os/kernel.c` `k_mpu_grant()`, [mpu.md](mpu.md) |
| pid sets | `k_pidset_t` sized from `Z_PROCS_MAX` for the scheduler's sets and the UART's waiting writers; host-tested at 32-256 | `sw/os/kpidset.h`, `sw/os/tests/test_pidset.c`, [kernel.md](kernel.md) |

`ipcbench` is now itself a driver binary, so its echo copy can open the
window onto the master once the master allows drivers; section [5] then
checks that switching drivers off refuses the echo.

Kernel image: 256,656 bytes (5,488 free); with `KTRACE=1`, 260,928.

## Round 5: what the tick costs

The last trace showed the 732 Hz timer interrupt spending thousands of
cycles per beat whether or not there was anything to do. Before the
rest of the plan, measured -- `tick` in the shell, `ipcbench` [7] --
with two switches to compare the parts (the wake sweep, the USB poll).
See [kernel.md](kernel.md), "What a tick costs". The sweep over
sleepers only (`sleepset`) is the default; the old walk is `tick
sleepset off`.

To run: `tick reset`, wait a few seconds with the desktop idle, `tick`;
then `tick sleepset off`, `tick reset`, wait, `tick`; the same with
`usbpoll off` (then back on). And `run ipcbench` for [7].

Kernel image: 257,976 bytes (4,168 free); with `KTRACE=1`, 261,552 (the
trace ring is now 192 events, and ipcbench traces four round trips).

### Round 5 results, and a scheduler bug

`tick` with only the kernel shell running (no switches):

| | handler cycles per tick | share of the CPU |
|---|---:|---:|
| sleep set on, USB poll on (default) | 11,712 | 17.9% |
| sleep set off, USB poll on | 13,608 | 20.8% |
| sleep set off, USB poll off | 7,328 | 11.2% |

Add ~3,000 cycles a tick (4.6%) for the register save and restore. So
at 732 Hz the beat alone took over a fifth of the machine. The sleep
set saves ~1,900 cycles a tick; the USB host poll costs ~6,300.

`ipcbench` [7], with `wm`, `net` and `console` running: a process that
never blocks lost **52%** of half a second -- 117 gaps of ~11,000
cycles (ticks), and 116 of ~94,000 (another process running, about
1.4 ticks each, 232 times a second). The second is `net` waking every
tick and using most of a timeslice each time: the largest single cost
on an idle desktop, and worth its own look.

The run ended with `net` crashing -- a bug in round 3's pid-set
scheduler, not in `net`: see [kernel.md](kernel.md), "The scheduler"
(when nobody is left to run). Fixed in this round's zip, with the
crash-report loop that came with it.

### Round 5c: two causes of idle load

- The tick steps the USB host every tick only while something needs
  timing; settled, every eighth (`z_usbh_tick()`).
- `net`'s USB ethernet backend counts only frames addressed to us as
  activity, so LAN broadcasts no longer keep it polling every tick
  ([usb_ethernet.md](usb_ethernet.md)).

The tick rate is unchanged; the analysis and the rule for changing it
are in [kernel.md](kernel.md), "The tick rate".

Results (round 5c, hardware): the quiet tick fell from 11,712 to
~6,300-8,300 cycles; the timer as a whole now takes ~10-17% of an idle
CPU, which by the rule in [kernel.md](kernel.md) argues for 366 Hz --
recorded, not changed. With nothing else running, a CPU-bound process
lost 12%; with the desktop idle (`wm`, `net`, `console`) it lost 62% in
one run, almost all in ~84,000-cycle gaps about every second tick --
another process running a full timeslice while one was busy. Not yet
identified; `ps` before and after `ipcbench` (the `cpu:` column) will
say which.

## Round 6: Phase 1, the kernel's side of drivers

Built; awaiting hardware.

| | what | where |
|---|---|---|
| room | `bench`, `sdbench` and `tick` behind `KDIAG=1`: 9,364 bytes free, from 3,776 | [kernel.md](kernel.md), "Diagnostics build" |
| interrupt binding | a driver takes a line with `z_irq_bind(irq, silence, val)`; the kernel then only writes the optional "silence" register (for a level source), wakes the driver and switches to it. Bindable today: `Z_IRQ_ETH`. Released when the process ends | `Z_SYS_IRQ`, `sw/os/kernel.c` `k_irq()` |
| per-process MPU mask | apps `Z_MPU_MASK_DEFAULT`; drivers every nibble (the SD card's included), written on a switch only when it changes | [mpu.md](mpu.md) |
| drivers first, on a wake-up | when a process blocks or an interrupt wakes a driver, a runnable driver goes next; the tick's own switch prefers nobody, so a driver that computes for long shares the CPU fairly | `k_sched_switch(regs, prefer)` |
| the proof | `net` is a driver and binds `Z_IRQ_ETH`; the kernel's `net0` lookup remains for older binaries | [networking.md](networking.md) |

Not in this round, and why:

- **Level-sensitive lines** (USB, audio, the UART) become bindable as
  their code leaves the kernel; the `silence` register is how a driver
  will quieten one until it has served it.
- **Starting drivers from the archive before `init`** waits for `fs`:
  until then there is nothing that must start first, and `wm`'s fixed
  pid 1 has to be settled with it (see "Drivers" above).
- **`z_proc_image_write()`** from the original plan is not needed: the
  kernel can mark a process being loaded as allowing drivers, and `fs`
  writes it through the grant window.

What to watch on hardware: `net: driver (pid 2)` and `net: ethernet
interrupt bound` at boot (the second only means something on a wired
MAC board); networking, telnet and the desktop as usual. On a wired
board, interactive traffic should if anything feel quicker -- the
woken driver runs next instead of waiting its turn.

Kernel image: 254,012 bytes (8,132 free).

Seen on hardware before this round, not yet chased: with the desktop
idle and `ipcbench` running, `ps` showed `console` (pid 3) gaining
~2,100 ticks and `net` ~1,450 over the run, and `console` listed as
runnable rather than blocked. `console` relays the kernel log, which
`ipcbench` writes to -- worth a look at whether it ever blocks while
there is a reader.

## Round 7: Phase 2a, `fs` serving its own volume

Built and host-tested; awaiting hardware. The whole design is in
[fs_driver.md](fs_driver.md).

| | what | where |
|---|---|---|
| matching receive | `z_msg_recv_match(msg, subject, tag, timeout)`: wait for one reply and leave everything else queued in order. Needed by any client that also gets other mail | `Z_SYS_MSG_RECV` + `Z_MSG_RECV_MATCH`, `msg.c` `z_mailbox_pop_match()`; `test_msg` |
| protocol | POSIX-shaped requests (`open`/`read`/`write`/`lseek`/`stat`/`readdir`/...), newlib errno numbers, 32KB per data request | `sw/common/zfsproto.h`, subjects `Z_SUBJ_FS` 432-447 |
| server | `fs`: FatFs on a 1.5MB RAM disk of its own, at `/fs`. Writes are read from the client in place; reads go through the grant window, or by copy without it | `sw/apps/fs` |
| client | `zfs_*()`; zfsapp sends `/fs` paths and `fs` handles there when `zfsc.o` is linked (weak references otherwise) | `sw/common/zfsc.c`, `zfsapp.c` |
| tests | `test_fs` on the host: server and client together, 1,302 checks | `sw/apps/fs/tests` |
| measure | `ipcbench` [8]: `/ram` against `/fs` (stat, open+close, a 32-entry listing, 512KB written and read in 8KB chunks with the stall meter running, a whole-file read), and the same with the grant refused | `sw/apps/ipcbench` |

Why a separate volume first: see fs_driver.md, "Where it stands".
`/fs` and `/ram` differ only in who runs FatFs, so [8] shows the
server model's cost without the card's.

What to expect on the board. A request costs one round trip, and the
last measured round trip was 0.9ms at best and ~2ms on average, far
above Phase 0's 50µs gate. So the protocol moves 32KB per request,
whether a read goes through the grant window or comes back in the
reply. Rough expectations against `/ram` (fs_driver.md, "What a
request costs"): +8-15% for large reads and writes, +30-60% for 8KB
`fs_read_chunk()` loops, and roughly a round trip more for each small
operation. On the card the same overhead is a third of that or less,
because each chunk takes longer there. Stalls *during* `/fs` I/O
should be shorter than during `/ram` I/O, because `fs` holds no
scheduler lock. On this board (MPU version 1) reads take the copy
path: the same request size, one memcpy more each. The grant numbers
need the rebuilt bitstream.

If [8] confirms that, the case for moving the card is good for bulk
work. The round trip then becomes the thing to cut before Phase 2b,
because every small operation pays it: a trace with `KTRACE=1`
during [8] is the next measurement.

Kernel image unchanged from the matching receive: 254,628 bytes
(7,516 free). `fs` is 50KB, most of it FatFs.

To run: copy `sw/apps/fs/fs.bin` to the card as `/apps/fs` (as for any
app), then run `ipcbench`. It starts `fs` itself if `fs` is not
already running.

### Round 7 first run, and fixes (7b)

On the board (MPU version 1, so reads take the copy path):

| | `/ram` (kernel) | `/fs` (`fs`) |
|---|---:|---:|
| stat | 1.57ms | 3.28ms |
| open+close | 2.17ms | 8.50ms |
| list 32 entries | 9.5ms | (crashed, below) |

- **`ipcbench` crashed listing `/fs`:** a misaligned access in
  `zfs_readdir()`. The client's directory buffer was a byte array
  after a `bool`, so the entries in it were not 4-byte aligned. The
  host test did not see it, because x86 allows misaligned loads. The
  buffer is now first in the struct and made of words, and the host
  test builds with UBSan's alignment check and AddressSanitizer.
- **`fs` had 274KB of `.bss`** (8 reply slots of 32KB) and asked for
  4MB. Now it has 29KB of `.bss`: 2KB slots, plus one 32KB bulk buffer
  for reads by copy, allocated on first use and held for its client
  until that client has read it. The disk is 640KB and the stack
  request is 1MB. See fs_driver.md, "Memory".
- The stricter host test then caught a second bug that the board
  would have hit as soon as a read came back by copy: the result was
  stored through the reply pointer evaluated *before* the read
  switched that pointer to the bulk buffer. As a result the client
  read an uninitialised result.
- The run itself was under heavy load. [7] shows **60% of the CPU**
  taken from a process that never blocks, mostly in ~79,000-cycle gaps
  about every second tick. Round 5c's idle desktop measured 10-17%.
  Even the cheapest syscall averaged 83µs against a 33µs best. All of
  this run's numbers are inflated by it, `/ram`'s included. Something
  is running that wasn't before, and `ps` before `ipcbench` would
  show what. The small-op gap is in line with a ~1.7ms round trip
  (stat: one trip, open+close: two).

### Round 7c: the second run, and the hand-off

With the fixes, [8] ran to the end, and its correctness check passed
on `/fs`. The full table and its explanation are in fs_driver.md,
"What a request costs". In short:

- Small operations and 8KB calls were 2.5-5× slower on `/fs`. Each
  request cost ~5-8ms, because the answer's hop went round the run
  queue behind a process that was using the CPU.
- The whole-file read reached 58% of `/ram`. Most of the gap is the
  second copy of the data, which the grant window removes.
- During a whole-file read, the longest stall for everyone else
  dropped **from 90ms to 5.6ms**.

Changes in this round:

| | what | where |
|---|---|---|
| hand-off | a process that blocks right after sending gives the CPU to the recipient. Both hops of a request to a driver now go direct, whatever else is runnable | `z_proc.handoff`, `k_msg_send()`, `k_sched_switch()`; [kernel.md](kernel.md) "Who goes next when a process blocks" |
| who takes 60% | [7] prints the ticks each process was billed while it spun | `sw/apps/ipcbench` |

Kernel image: 254,948 bytes (7,196 free); `KTRACE=1` 258,532;
`KDIAG=1` 260,592.

To run: the new `kernel.bin` and `ipcbench`, then `ipcbench`. Expected:
- [2]'s round trip with spinners running should fall sharply. The
  idle figure should barely move, since there was nothing to jump
  over.
- [8]'s per-request overheads should drop to about one idle round
  trip.
- [7] names the process that takes 60%.

### Round 7d: the hand-off measured, where the time goes, fewer requests

Results with the hand-off (round 7c's kernel):

| | 7b | 7c (hand-off) |
|---|---:|---:|
| round trip with 1 / 2 / 3 CPU-bound processes, best | 1.2 / 2.7 / 4.1ms | **0.93 / 0.93 / 0.94ms** |
| `/fs` stat | 5.4ms | 4.4ms |
| `/fs` write, 8KB calls | 735 KB/s | **1,539 KB/s** |
| `/fs` read, 8KB calls | 484 KB/s | 642 KB/s |
| `/fs` whole-file read | 1,334 KB/s | 984 KB/s |
| `/fs` list 32 entries | 43ms | 46ms |

- The hand-off works. The best round trip no longer depends on how
  many processes are competing, and writes doubled.
- Reads went down, and averages vary widely from run to run. The 37%
  that [7] found taken was not billed to any process: every tick in
  the window went to `ipcbench` itself. So the time goes to interrupt
  handlers, or to processes that run between ticks (drivers woken by
  an interrupt). Tick billing cannot see either.

Changes:

| | what | where |
|---|---|---|
| CPU accounting | exact cycles per process, per kind of interrupt, and idle, in every build; `ps` shows them; `z_cpu_stat()` | [kernel.md](kernel.md) "CPU accounting", `Z_SYS_CPU_STAT` |
| ipcbench | [7] shows where its stolen time went; [8] shows the same around the `/fs` listing and the bulk tests (`ipcb0`, `fs`, `switch[n x cycles]`, `tick`, `usb-irq`, idle) | `sw/apps/ipcbench` |
| fewer requests | `OPENDIR` returns the first page and `fs` closes a directory that has ended: a small listing is 1 request, not 4. `OPEN` returns the size: a whole-file read needs no `FSTAT` | `zfsproto.h` (`Z_FSREP_END`), `fs.c`, `zfsc.c`; tested by request count |

Kernel image: 256,204 bytes (5,940 free); `KTRACE=1` 259,788;
`KDIAG=1` 261,848 (296 bytes from the limit).

The numbers to look at next run:
- [7]'s "where the cycles went", to see which interrupt or process
  takes the 37%.
- In [8], `switch[n x cycles]`: the average cost of the switch path,
  paid twice per request.
- `ps` with the desktop idle, for the `used` and `kernel:` lines.

### Round 7e: net was the 60%, and the switch path

The first run with CPU accounting (round 7d) gave the answer in [7]:

- `net0` **28%** (between ticks, invisible to tick billing)
- `ipcb0` 45%
- tick handler 13.7% (408 × 9,017 cycles)
- USB interrupt 5.7% (199 × 7,682)
- switch path, IRQ 0, 201 × **8,129** cycles

The rest of that run's accounting, and `ps`'s `used` line, showed
impossible figures (24,298%, more cycles than the uptime). That was an
accounting bug, now fixed (kernel.md, "CPU accounting").

`net` polls the USB adapter every tick for half a second after any
traffic, and every ARP reply it sent counted as traffic. One host on
this LAN asks "who has us?" every few hundred milliseconds, so the
fast rate never ended.

Changes:

| | what | where |
|---|---|---|
| net | an ARP frame we send no longer counts as traffic: idle polling stays at ~45/s instead of 732/s | `sw/apps/net/usb_ecm.c`; [usb_ethernet.md](usb_ethernet.md) |
| accounting | idle is closed at the next interrupt's entry; a negative charge is dropped | `sw/os/kernel.c` |
| switch path | the GPU clip restore compares against the copy just saved instead of reading nine GPU registers again | `k_gpu_clip_restore()` |
| trace | `step` events in `k_sched_switch()` (GPU save, register save, MMU/MPU, GPU restore), so a `KTRACE=1` run shows what each part of the 8,129-cycle switch costs | `K_TR_STEP` |
| ipcbench | the cycle report is two lines (processes; kernel), so it no longer runs off the terminal | `sw/apps/ipcbench` |

Kernel image: 256,380 bytes (5,764 free); `KTRACE=1` 260,012;
`KDIAG=1` 262,024 (120 bytes from the limit).

The next run needs:
- the new `kernel.bin`, `net.bin` and `ipcbench`;
- [7] should lose most of `net`'s share;
- then a `KTRACE=1` kernel with `ipcbench` followed by `ktrace`, for
  the switch's step costs.

## Round 8: Phase 2b, the kernel's FatFs is gone

Built and host-tested; awaiting hardware. The design is in
[fs_driver.md](fs_driver.md).

| | what | where |
|---|---|---|
| `fs` is the filesystem | the card (SD driver linked in), `/ram` and `/tmp` (kernel RAM disk, by sector), `/usb` (kernel MSC, by sector), the flash archive's files underneath | `sw/apps/fs`; `Z_SYS_BLK`, `Z_SYS_ZAR` (`sw/os/kblk.c`) |
| the kernel is a client | `kfs.c` on the apps' `zfsc.c` (`-DZFSC_KERNEL`): the shell's file commands, program loading (card `/apps` first, then flash), wm's dock check, cfg, and every `Z_SYS_FS_*` call, made from inside the caller's syscall | `sw/os/kfs.c`, `sw/os/fsapi.c` |
| boot | `fs` starts first, from flash; the shell waits for it, then cfg, then `init` | `boot_fs()`, `sw/os/sh.c` |
| no FatFs in the kernel | `sw/os/fs/` removed (FatFs, `fs.c`, `ramdisk.c`, `sdbench.c`); the filesystem syscalls no longer hold preemption | `sw/os/Makefile`, `k_syscall_touches_fs()` |
| flash layout | kernel region 256KB -> 224KB, archive at `+0x38000`, 784KB; `fs` is a core app (first), budgets tightened | `zsoc.h`, `zar.h`, `bios.c`, `Makefile`, `layout.py`, [flash_apps.md](flash_apps.md) |
| apps | zfsapp unchanged by default (through the kernel, no size cost); `-DZFSAPP_DIRECT` + `zfsc.o` goes straight to `fs` | `sw/common/zfsapp.c` |

The decisions behind it:

- **No FatFs fallback in the kernel.** `fs` loads from flash, and
  without it there are no files. This was the user's choice.
- **Two steps became one.** The kernel cannot hold both its FatFs and
  the client in 224KB, so 2b-1 and 2b-2 landed together.
- **The kernel's region shrank now**, so that `fs` fits in the
  archive. This was also the user's choice.

Kernel image: 223,852 bytes of 229,376 (`KTRACE=1` 227,868, `KDIAG=1`
226,988; `USBH_DEBUG=1` 232,412, over). `fs`: 42,364 bytes in flash,
72,400 image.

What changed for the user:

- **No more `sdbench` and `usbbench`.**
- **`ls` lists differently:** directories with a trailing `/`, files
  with their size, and flash-only files marked `(ro)`, all in one list
  instead of a separate "in flash:" section.
- **`fs` itself always comes from flash.** A new `fs` needs
  `make flash_apps`.
- **Files to delete** (a zip can only add files):
  `sw/os/fs/` (all of it), `sw/os/tests/test_fsrename.c`,
  `sw/apps/fs/fs_ram.c`, `sw/apps/fs/fs_ram.h`.

To test on the board:

1. Flash the new kernel and the archive: `make flash_os flash_apps`.
   The archive moves, so both are needed.
2. At boot, look for `init: fs started as pid 1`, then fs's own lines
   (ramdisk, sdcard), then the usual init.
3. Try `ls /`, `ls /apps`, `xf`, `tget`/`tput`, `run` of a card app,
   the dock, `cfg reload`, the files app, `usbmount` with a stick, and
   `format` on a spare card.

## Round 9: Phase 3, the USB host stack is a process

Built and host-tested, the co-simulation passing; awaiting hardware.
The design is in [usb_driver.md](usb_driver.md).

| | what | where |
|---|---|---|
| `usb` is the USB host | the stack from `sw/os/usb`, nearly unchanged, in a driver process: IRQ 9 bound, its own timeouts, requests from `fs`, `net`, `serial` and the shell | `sw/apps/usb` |
| the protocol | requests, borrowed replies per client, events; ethernet through two shared rings | `sw/common/zusbproto.h`, `zusbc.c`, `zusbnet.h`, `zusbcdc.h` |
| `fs` | `/usb` is `usb`'s drive, by request; a `Z_USB_EVENT` prompts a look | `sw/apps/fs/fs.c` |
| `net` | the rings; `net` no longer polls the adapter, `usb` does and wakes it | `sw/apps/net/usb_ecm.c` |
| `serial` | the same three calls, now requests (links `zusbc.o`) | `sw/common/zusbcdc.h` |
| the kernel | no USB stack. IRQ 9 bindable (the kernel clears IRQEN when unowned); `lsusb` and the debug commands go to `usb`; lock LEDs by a poke; `Z_SYS_USBCDC_*`, `Z_SYS_USBNET` retired (they fail); no syscall holds the scheduler; `k_deferred_*` gone | `sw/os/kusb.c`, `kernel.c`, `hid.c`, `sh.c`, `uart.c` |
| boot | `fs`, then `usb` (from the card if there), then cfg and `init` | `sh()`, `sw/os/sh.c` |
| the image | `.bss` no longer padded into `kernel.bin`: crt0 zeroes it (the Makefile checks) | `sw/os/Makefile`, [kernel.md](kernel.md) |
| flash layout | kernel region 224KB -> 160KB, archive at `+0x28000`, 848KB; `usb` a core app; budgets reset | `zsoc.h`, `zar.h`, `bios.c`, `Makefile`, `boot.c`, [flash_apps.md](flash_apps.md) |
| apps | linked with a 16-byte page, as the kernel was: 0-4KB less each in flash | `sw/common/app.mk` |

Sizes: kernel image 223,852 -> 134,264 bytes (`KTRACE=1 KDIAG=1`
138,564; 191,761 to `_end` in RAM). `usb`: 41,936 bytes in flash,
52,604 image.

Decisions along the way:

- **The UART is the rescue console.** Without `usb`, the kernel shell
  has no USB keyboard. This was the user's choice. Keyboards already set
  up keep working if `usb` dies.
- **`usb` starts after `fs`,** so that a bigger copy can come from the
  card. That raised the question of drivers growing past the flash
  region (above).
- **The padding went because the space was needed.** The first estimate
  had the archive about 3KB short at a 192KB kernel region. Reading
  `_start` showed crt0 zeroing `.bss`, as the user remembered.
- **CDC by request, the syscalls retired** rather than kept as kernel
  proxies (no preference given; fewer kernel bytes).

What changed for the user:

- **Shell commands.** `usbcdc`, `usbbench` and `tick usbpoll` are gone.
  `lsusb` and the debug commands work as before, through `usb`; there is
  a new `usbrescan`.
- **A debug `usb`** (`make -C sw/apps/usb USBH_DEBUG=1`) can live on
  the card as `/apps/usb`; no kernel rebuild.
- **Files to delete** (a zip can only add files): `sw/os/usb/` (all of
  it), `sw/os/usbcdcapi.c`, `sw/os/usbcdcapi.h`, `sw/os/usbnetapi.c`,
  `sw/os/usbnetapi.h`.

To test on the board:

1. Flash all three: `make flash_os flash_apps`, and the bitstream if
   you rebuild it, for the BIOS's new `ROM_OS_SIZE`. An older BIOS works
   too: it copies more than it needs.
2. At boot: `init: fs started`, then `starting usb`, `usb host: ...`,
   `usb: usb0 ready`, then the usual init.
3. Try:
   - keyboard and mouse behind the hub;
   - Caps Lock's LED;
   - `lsusb`;
   - a stick at `/usb`: plug, pull, plug;
   - USB ethernet (DHCP, ping, `tget`);
   - `usbserial` in a term window;
   - after `kill`ing `usb`: the keyboard still types, and `run usb`
     brings the rest back.

## Round 10: polish

Built, host-tested, the USB storage co-simulation passing, and run on
lakritz (MPU v1, USB ethernet behind the host controller). The TFTP
investigation below took most of the hardware time; its outcome is in
"Open issues" at the top.

| | what | where |
|---|---|---|
| libc file I/O | `open()`, `fopen()` and the rest over `fs`, for apps built with `POSIX_IO = 1`; `vi` uses it and dropped its own | `sw/common/zposix.c`, `posix/dirent.h`, `app.mk`, [posix_io.md](posix_io.md) |
| driver restart | `fs`, `usb`, `net` started again after a crash, 3 a minute at most; `/ram` survives `fs`'s | `kernel.c`, `sh.c`, `uart.c`, `fs.c`, `kblk.c` (above) |
| USB stick start | `usb` brings a drive up in steps and tells `fs`; no multi-second pause for anyone | `usb.c`, `usbh_msc.c`, `fs.c` |
| late replies | noted and drained by `zfsc.c` and `zusbc.c` | `sw/common` |
| `fs`'s reply slots | not reused while unread; `-Z_EAGAIN` and a retry instead | `fs.c`, `zfsc.c` |
| `ipcbench` | its worker found on the card or in `/ram`, `/tmp`, `/usb`; a `/usb` section when a stick is mounted | `sw/apps/ipcbench` |
| `usbstat` | the ethernet path's counters, for the TFTP slowdown | `usb.c` |
| `rmdir` | `-Z_ENOTEMPTY` for a directory that is not empty | `fs.c` |
| flashing | kernel and core apps written as one image (`flash_sw`): +0x28000 shares a 64KB erase block with the kernel's end | `Makefile`, [flash_apps.md](flash_apps.md) |
| TFTP get | two blocks queued, ACK on arrival; no retry while waiting on the receiver; per-block timing and retry diagnostics | `sw/apps/net/tftp.c`, `tests/test_tftp_get.c` |
| no spinning waits | `zstream`'s consumer and `z_msg_wait_timeout()` block instead of polling | `sw/common/zstream.c`, `sw/os/msg.c` |
| lost wake | `usb` wakes `net` by the ring tail read after a receive, not before | `sw/apps/usb/usb.c` |
| `sched` | two scheduler variants as switches, both off | `kernel.c`, `sh.c` |

Sizes: kernel image 137,528 bytes (26,312 free of 160KB). `usb` 43,472
in flash; `fs` 43,060; `net` 139,568; `vi` 236,184.

### The TFTP investigation

`tget` over USB ethernet ran at ~21KB/s, against ~50KB/s with the
stack in the kernel, with `tftp retry` lines. The lost-wake fix of
round 9 did not change it. Two causes are possible: frames lost (a
retry costs the 0.5s TFTP timeout, so a few of them halve the rate),
or the extra hop per block. `usbstat` tells them apart.

#### First numbers (round 10, hardware)

107KB `tget` to `/ram`: 14-16KB/s, 5-7 `tftp retry` lines a run.
`usbstat` after one: 258 frames in, 221 out, none failed; a frame
waited for `net` 6.9ms on average (max 100ms, and 520ms in another
run); a kick waited for `usb` 1.5ms (max 20ms). So:

- **The retries are half the time** (7 x 0.5s of 7.6s), and not all of
  them were the network. `net` ACKed a block only once `tget` had
  pulled it, and the retry timer ran meanwhile, so a `tget` slow to
  write its last block to `fs` looked like a lost packet and resent an
  old ACK.
- **Without them, ~19ms a block**, against ~10ms before. Of that, the
  6.9ms a frame sits in `usb`'s ring before `net` takes it is the
  largest piece measured; it should be well under a millisecond.

Changed in `net` (`tftp.c`, host test `tests/test_tftp_get.c`):

- **Two blocks queued, ACK on arrival.** A block is ACKed when it is
  stored, unless it fills the queue; then its ACK waits for `tget` to
  take one. So `tget`'s write to `fs` overlaps the next block's trip
  instead of adding to it, and the server never sends into a full
  queue.
- **No retry while waiting on our own receiver.**
- **Where a block's time goes,** printed after the rate:
  `ack->next block` (usb, the wire and the server, both ways) and
  `receiver took it after` (tget and fs), and the retries that were
  really the network's.

Next on the board: `usbstat`, `tget`, `usbstat`, and that new line.
If `ack->next block` is still near 15ms, the time is on the USB side
and the ring wait above is where to look; if `receiver took it` is
large, it is `tget` and `fs`.

#### Second numbers: the polling is slow, not the hop

With the queue: 16.7KB/s, and the new line read `ack->next block avg
25.1ms (max 535ms), receiver took it after avg 5.9ms (max 10.3ms); 5
retries`. `ic`: adapter rx 518, tx 224, no errors, nothing dropped,
no mailbox refused a send.

- **`tget` and `fs` are not the problem:** 5.9ms a block, and now
  overlapped with the network.
- **The network side is**: without the 5 retries, ~13.6ms from our
  ACK to the next block, on a LAN where the server answers in well
  under a millisecond.
- **`usb` was asking the adapter every ~5ms, not every tick.** (The
  two scheduler causes named here were tried and turned out not to be
  it; the real cause was the spinning shell, found later in the trace.) The
  first run asked 1,807 times in 7.6s, about 240 times a second; the
  intent was 732. There were two reasons, both in the scheduler:
  - **A 1-tick timeout lasted two ticks.** The sleep sweep woke a
    process at `ticks - wake_tick > 0`, one tick after its deadline.
  - **A driver woken by its timeout waited its turn** behind every
    runnable process (the shell, `fs`, `net`), up to a tick each. One
    woken by its interrupt goes first; one woken by its timer did not.
    In the monolithic kernel the adapter was polled from the tick
    interrupt itself.
- **The 6.9ms "frame waited for net"** is measured to when `usb` next
  looks at the ring, so it includes `usb`'s own slow wake. It is not
  `net` being slow.

Tried in the kernel (`kernel.c`, the tick's sleep sweep): a timeout
ending at its deadline tick, and a driver woken by its timeout
preferred as one woken by an interrupt is. Both turned out slower once
the real cause was found (below), and are off by default.

The retries remain unexplained: nothing was dropped by `usb` or the
adapter, and every block's ACK was sent. Faster polling may remove
them, if the adapter drops a frame that waits too long, or the server
gives up on an ACK that comes too late.

#### Third run: the keyboard and mouse dropped

With the new kernel: 19.7KB/s, 4 retries, but during the transfer
both HID devices went to `none` and came back (wm's "device type"
lines), which had not been seen before. Their type is written by
`usb`'s enumeration, so `usb` decided they were gone and set them up
again. `usbstat`: ring full 43 times, a kick waited for `usb` up to
101ms -- `usb` was busy for that long, likely re-enumerating.

To tell which change does it without reflashing, both are switches
in the kernel shell: `sched late 0|1` (1 is the old, one-tick-late
timeout) and `sched drvwake on|off` (off: a driver woken by its timer
waits its turn, as before). `sched late 1` with `sched drvwake off` is
the kernel before them.

And to see a transfer from the inside: a kernel built with
`KTRACE=1` has the scheduler trace, and `net` now arms it at block 100
of a get (`tftp.c`, `TFTP_TRACE_BLOCK`), so `ktrace` after the
transfer shows ~192 events from the middle of it -- who ran, who
woke whom, for how long.

#### The trace: the kernel shell was spinning

`sched late 1` / `drvwake off` (the old scheduler): 21.7KB/s, 1 retry,
11.4ms a block without it; `late 0`: 23.5KB/s. The retries vary from 1
to 11 between identical runs, so they are noise on top of the rest,
and the keyboard drops need the A/B still.

The `KTRACE=1` run showed where the ticks go. Most ticks belong to one
process from end to end:

- **`usb` spinning on the engine** for a whole tick at a time while it
  moves a frame (52-55K cycles with no events). That is the transfer
  itself, as in the monolithic kernel -- which did it in the tick
  interrupt.
- **pid 0 -- the kernel shell running `tget` -- for whole ticks with no
  events at all,** chosen by the round robin while `usb` had just been
  kicked by `net` and was waiting to run. `zstream_pull()` waited for
  each chunk with a `z_msg_read()` loop: runnable all the time it
  waited, so every turn the round robin gave it was a tick of nothing,
  taken from `usb` and `net`. So did `zstream_open()`, and the kernel's
  `z_msg_wait_timeout()` (`tput`).

  With the stack in the kernel this cost less: the adapter was polled
  from the tick interrupt whoever was running, so only `net` lost
  turns. Now every block needs `usb` to run twice, and each time it may
  queue behind a tick of the shell spinning.

Changed: `zstream.c`'s consumer waits blocked (`z_msg_recv()` with what
is left of its timeout; the kernel gained a `z_msg_recv()` in
`msg.c`), and `z_msg_wait_timeout()` is a blocked
`z_msg_recv_match()`, which also stops it throwing other mail away.

#### With the shell no longer spinning

| `sched` | KB/s | retries | ms a block, retries aside | tget's share |
|---|---|---|---|---|
| late 0, drvwake on | 20.5 | 5 | ~13 | 5.2ms |
| late 0, drvwake off | 18.5 | 6 | ~14 | 3.1ms |
| **late 1, drvwake off** | **36.5** | **1** | **~9.9** | **1.7ms** |

The old scheduler is the fast one now that the shell sleeps: ~9.9ms a
block, the monolithic kernel's ~10ms, and without its one retry the
run would have been ~44KB/s. Both variants poll the adapter more often
and both lose more frames; drvwake also re-enumerated the keyboard and
mouse. So both are off by default again (`k_sched_late = 1`,
`k_sched_drvwake = false`) and stay as `sched` switches for the next
measurement.

What is left is the retries: each costs the 0.5s TFTP timeout, a
block that never arrived although `usb` and the adapter report no
error and no drop. They come with polling: 1 at the old pace, 5-11 at
the faster one.

Another `late 1` run: 15.1KB/s with 9 retries; ~10.9ms a block
without them. So the per-block time is settled at the monolithic
kernel's, and the retries -- 1 to 11 a run, whatever the scheduler --
are the whole remaining difference. To find where a block goes:

- `usbstat` counts IPv4 frames whose length disagrees with their IP
  header: shorter (a packet lost inside the frame; `net` drops it
  silently, `ip.c`, and TFTP waits 0.5s) or longer (a packet too many).
  The class driver counts neither as an error.
- The TFTP line counts blocks that came again (our ACK lost or late)
  and blocks out of sequence.

If the short count matches the retries, packets are lost between the
adapter and the frame, in `usbh_ecm.c`'s reads; if neither count
moves, the block never reached the adapter, or the adapter dropped it.

With those counters, four runs: 33.2, 36.5, 30.2KB/s with 1, 1 and 2
retries; no frame shorter or longer than its IP header, no block twice,
none out of sequence. So no frame is damaged on its way through `usb`;
the block that times out never reached it. ~10ms a block otherwise.
The retry line now says which block it waited for and when, to see
whether it is the opening, random, or periodic.

**A lost wake, found by reading the code again.** `usb`'s `net_rx()`
read net's ring tail, then received a frame -- a millisecond or so of
spinning on the bus, during which it can be preempted -- then woke net
only if `head == tail` by that OLD tail. If net took the previous frame
in the meantime and went to sleep with an empty ring, the new frame
looked like the second in a ring that was not empty, so nobody woke
net: it waited for net's 100ms backstop, or the 0.5s TFTP retry timer.
That is the "frame waited for net, max 518ms" `usbstat` showed. The
wake is now judged by the tail after the receive.

And at each retry, `net` prints `usb`'s ring (`usb_ecm_note()`): `A
FRAME WAITS` means the block had arrived and net was not woken -- the
case above, if any is left; `empty` means it never reached `usb`.

**Result (last run of round 10):** 31.2KB/s, 2 retries, at blocks 5
and 148 (592ms and 2717ms in), and at both `usb`'s ring was empty:
the blocks never reached `usb`. No frame damaged, none twice, none out
of sequence. The lost-wake fix shows in `usbstat`: a frame now waits
for net 25ms at most (it was 100-518ms). ~9.9ms a block otherwise.

So what remains is one or two frames a transfer lost upstream of
`usb`: our ACK lost between the adapter and the server, or the block
lost between the server and the adapter. Next:

- `tcpdump -n udp and host <board>` on the server during a `tget`
  says which: an ACK that never arrives, or a block sent and never
  seen.
- The same `tget` on `main` (the monolithic kernel), for its retry
  count: if it loses a frame now and then too, this is not a
  regression.
- A TFTP retry timeout matched to the round trip (~10ms here) instead
  of a fixed 0.5s, so a lost frame costs ~100ms. Each costs 5% of a
  100KB transfer now.

## See also

- [kernel.md](kernel.md) -- the image budget, the syscall boundary
- [mpu.md](mpu.md) -- `MASK`, privilege by fetch address, kernel-only stores
- [fs_driver.md](fs_driver.md), [usb_driver.md](usb_driver.md), [posix_io.md](posix_io.md) -- the drivers and the libc hooks
- [networking.md](networking.md) -- `net`
- [filesystem.md](filesystem.md), [sdcard.md](sdcard.md), [usb_host.md](usb_host.md)
- [flash_apps.md](flash_apps.md) -- the archive and its budgets
