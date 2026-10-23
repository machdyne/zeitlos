# The kernel

`sw/os`. Processes, the scheduler, memory, and the syscall boundary.

Zeitlos has a **microkernel**: the kernel schedules, manages memory,
passes messages, routes interrupts and enforces memory protection, and
keeps a few machine-level services (the UART console and panic, the
flash key/value store, auth, the configuration, the hardware HID ports,
the RAM disk's memory) and the kernel shell. The filesystem, the USB
host stack and the network are **driver processes** -- `fs`, `usb` and
`net`, programs loaded from flash or the card, preempted like any other
and started again if they crash -- reached by message
([userspace_drivers.md](userspace_drivers.md)). The kernel image is
about 138KB.

This file is the entry point for the kernel's own design; the
subsystems it owns have their own documents and are linked from here
rather than duplicated:

| | |
|---|---|
| `docs/messaging.md` | mailboxes, `z_msg_t`, payload lifetime |
| `docs/userspace_drivers.md` | drivers as processes: how they start, restart and are reached |
| `docs/fs_driver.md`, `docs/usb_driver.md` | the filesystem and USB drivers |
| `docs/filesystem.md` | the volumes and path rules (FatFs is `fs`'s now) |
| `docs/app_runtime.md` | the app side of the boundary, and its traps |
| `docs/executables.md` | ZEXE, and how a process image is loaded |
| `docs/boot.md` | the memory budget everything sits in |
| `docs/ports.md` | the connection protocol built on messaging |
| `docs/kvstore.md` | the flash key/value store, `Z_SYS_KV` |
| `docs/security.md` | the password, the screen lock, `Z_SYS_AUTH` |

## Processes

A process is a contiguous block of main memory, a register file, and a
mailbox. There is no MMU: the MTU (`rtl/mtu.v`) translates the single
window at `0x8000_0000` to whichever physical block the running process
owns, and every other address passes through untranslated.

Three consequences worth stating together, because each is obvious
alone and the combination is what actually governs:

- **Every app is linked at the same address**, so an executable needs
  no relocation and the loader is a `memcpy` (`docs/executables.md`).
- **A process can reach the kernel and the peripherals directly.** A
  wild pointer below `0x8000_0000` is not a fault, it is a write. The
  isolation is a courtesy, not a boundary.
- **A second bus master does not get the translation.** The blitter
  takes physical addresses and says so in its own header; anything
  else that becomes a master inherits that.

### The process table

`z_proc` (`sw/os/kernel.h`): base, size, flags, wake tick, CPU ticks,
32 saved registers, the GPU scissor, the grant window, the MPU mask,
the hand-off, CPU cycles, the process's serial and its arena. **224
bytes** on rv32 (the `uint64_t` cycle count aligns the end to 8).
`z_procs[Z_PROCS_MAX]` lives in kernel `.bss` with an explicit
`__attribute__((section(".bss")))` -- see "The `gp` hazard" below for
why that annotation is load-bearing rather than decorative.

### The scissor is part of the context

The GPU's clip registers -- the rasterizer's
`gpu_clip_{x0,y0,x1,y1,enable}` and the blitter's
`gpu_blit_clip_{x0,y0,x1,y1}` -- are global hardware with no privilege
attached, so an app writes them directly and the kernel cannot
intercept that. They are also *per drawing*, and a context switch can
land between a process setting its clip and using it.

What the kernel can do is what it already does for registers: read them
on the way out and put them back on the way in.
`k_gpu_clip_save()`/`k_gpu_clip_restore()` do exactly that, from
`k_sched_switch()`, and `gpu_clip_valid` is 0 until a process has been
switched away from once -- a brand-new process has never written the
GPU, so it is restored to the hardware reset (raster clip off, blitter
scissor the full screen) rather than to another process's rectangle.

**Restoring a different clip has to wait for the engines.** The
rasterizer samples the clip live, per pixel, because FIFO entries do
not carry it, and a glyph blit samples the vertical scissor live per
row -- so changing either while such an operation is in flight changes
the operation. Fills latch their clip at the start and are safe. Both
spins are bounded, because this runs inside the interrupt handler,
where an unbounded wait is unrecoverable and a `printf` is a hang (see
`sw/os/uart.c`'s `_write()`).

It costs 40 bytes per process slot, which at `Z_PROCS_MAX` of 32 is
1,280 bytes of `.bss` -- and therefore 1,280 bytes of the 256KB flash
image, for the reason "The kernel image budget" below explains.

### Memory for a process

`k_proc_create(size, stack_size)` takes one allocation of
`image + stack_size` from `k_mem_alloc()`. There is no separate heap:
`_sbrk()` (`sw/common/zeitlos.c`) grows the heap up from `_end` and
refuses to pass the stack pointer, so an app's heap and stack share the
`stack_size` allowance and eat toward each other.

The executable names its stack and heap size: a 4-bit code in the
ZEXE flags word (`docs/executables.md`), 8KB × 2^(code − 1). The
kernel grants up to `Z_PROC_STACK_CAP` (`sw/os/kernel.h`, 8MB). A
program that does not ask, and a binary with no header, get the
default 16KB. What the kernel cannot give -- the reserved code 15, a
code above the cap, a reserved flag bit, or a block the pool cannot
hold -- is refused by `k_proc_create_exec()` with one line saying what
was asked for and what there is. It never grants less.

**Allocation failure is a clean refusal.** `k_mem_alloc()` returns
NULL, `k_proc_create()` returns 0, and every caller treats 0 as "did
not start". That path was not always trustworthy -- see the `Z_FAIL`
bug in `docs/app_runtime.md` -- and two things now lean on it
deliberately: a 4MB request on a small board, and any future dynamic
per-process allocation.

### Launching in the background

A launch -- `run` in the kernel shell, `z_proc_run()` from any app (the
dock's, most often) -- makes the process (`k_proc_create_exec()`, in
the caller, so a refusal is immediate) and then returns: **the new
process loads its program itself**, in its own turns
(`k_proc_launch()`, `sw/os/kernel.c`). Before, the caller loaded it:
the shell, and so the serial console, or wm, and so the desktop, did
nothing else until the program was in -- 15 seconds for planedemo's
3 MB (October 2026; 8KB card reads have since cut that, see
executables.md).

How, without another process or any more memory:

- `k_proc_launch()` copies a stub of eight instructions to the start of
  the new block, where the program will go, and a **ticket** -- the
  name, the parsed header, the launch tick -- 256 bytes below the top,
  just under where the stack starts. It sets `Z_PROC_FLAG_LOADING`
  (`zproc.h`) and starts the process.
- The stub moves its stack below the ticket and calls the gate
  (`reg_kernel`) with `K_LOAD_SELF` -- not a syscall in
  `syscalls.def`; it is honoured only for a process that is LOADING.
- `k_load_self()` loads the program over the stub
  (`fs_load_exec_any()`, the same loader as before: it waits on `fs`,
  unpacks, and is preempted like any process), clears the flag and
  jumps to `0x8000_0000` with the stack `k_proc_create()` set up --
  `[sp]` = 0, no arguments. The program starts exactly as one started
  after its load did. Once the fetch is in the app window the code is
  unprivileged again (mpu.md).
- A load that fails (a file cut short, an image that does not unpack)
  is printed -- `planedemo: loading from /apps/planedemo failed -- not
  started` -- and the process ends with the crash status.

`run` prints `planedemo: pid 6, loading 3177924 bytes from
/apps/planedemo` at once, and when the program is in, from the new
process, the line it used to print: `planedemo: pid 6, 3177924 bytes
from /apps/planedemo at 404eb000, 2100 ms (1900 ms unpacking, ...)`.
Other output can come in between. `init` still loads the core apps
itself, one after another, as before.

The dock keeps a launched icon inverted until the app makes its first
window, as before; its timeout (3 s) now waits as long as the process
is still LOADING. The posix shell's `ps` shows such a process as
`load`.

What a caller gets back is the pid, at once: a load that fails later
is not a failed `z_proc_run()`. Callers that fall back when a start
fails (zlink: posix, else repl) fall back on what fails at once -- no
memory, no slot, no file -- which is what they meant.

Launches can now overlap -- a file opened in view from files while the
dock starts text -- so a **launch argument** (`Z_WM_SET_ARG`, zwm.h)
is no longer "the one pending": wm keeps two, each its launcher's, and
`k_proc_launch()` tells wm, as the launcher, which pid the launcher's
argument is for (`Z_WM_ARG_CHILD`). An app gets the argument meant for
it, else the oldest meant for nobody yet. `Z_WM_WIN_NEXT_PLACE` with
tag 0 ("the next process to make a window") has the same race and is
not fixed. wm's Super+S does not start a second `tts` while the first
is still loading.

One loader state is shared (the LZ4 decoder, `k_exec_unpack()`): loads
unpack one at a time. A process killed in the middle of its load
releases it (`kfs.c`, `unpack_held()`). The resolver's half-second
cache is read and written with interrupts masked, since two loads can
resolve at once.

Known limits: the program is looked up by name again when the load
starts (the launch's lookup is reused if it is under half a second
old), so a file replaced in between is loaded into a block sized for
the old one. The loader checks what it reads against the old sizes, so
that is a failed load -- except an uncompressed file replaced by a
bigger one, which would be cut short. The kernel's fs client keeps its late-reply list in one place
for every process, so a reply to a load that timed out can stay in the
new app's mailbox.

## The scheduler

Round-robin over runnable processes, driven by the KTIMER interrupt at
`Z_TICK_HZ` (~732Hz at 48MHz). `Z_PROC_RUNNABLE()` is "active and not
blocked".

**The scheduler works from two bitmasks, not the table.**
`k_run_mask` has a bit for each runnable pid and `k_die_mask` one for
each process waiting to be reaped. Counting runnable processes is a
popcount and finding the next one is a mask and a count of trailing
zeros. It used to walk all 32 slots for each, twice per switch, and the
scheduler trace measured ~175 cycles for every slot passed: each slot
is its own 188-byte `z_proc`, so every read was a trip to memory.

**So every change to a process's flags goes through
`k_proc_flags(pid, set, clear)`**, which updates the flags and the
masks together with interrupts masked. A direct write to
`z_procs[].flags` leaves the masks stale -- the scheduler would then
skip a process that can run, or run one that cannot.

**When there is nobody to switch to**, `k_sched_switch()` must leave
`z_pid` on the process it came from, whose registers it returns. The
old slot walk did that by accident -- 32 steps from any pid come back
to it -- and the first pid-set walk did not: when reaping a dying
process left nothing runnable, it returned with `z_pid` on the reaped
slot, and the process actually running was later resumed from a stale
frame. That crashed `net` (October 2026). Now the walk remembers where
it started; and if that process was itself the one reaped, the kernel's
own process (pid 0) takes the CPU. A dying process is also never
"kept" as the only runnable one: that resumed a crashed app at its
next instruction, which crashed again, once per instruction.

The masks are **pid sets** (`k_pidset_t`, `sw/os/kpidset.h`): one bit
per pid, as many 32-bit words as `Z_PROCS_MAX` needs. At 32 that is one
word and the code is what a bare `uint32_t` would be. A new per-pid set
anywhere in the kernel should be one of these, not `1u << pid`.
`sw/os/tests/test_pidset.c` checks the next-pid search against the slot
walk it replaced at 32, 64, 100, 128 and 256.

**Who goes next when a process blocks.** The tick's switch is plain
round robin. A switch caused by a process blocking picks in this
order:

1. **The hand-off.** If the process that blocked had just sent a
   message, the CPU goes to the recipient, if that process is now
   runnable (`z_proc.handoff`, set by `k_msg_send()` and cleared on
   every switch away from the sender). A request to a driver is
   "send, then wait for the answer", and the answer is "send, then
   wait for the next request". With the hand-off, both hops go
   straight to the other side. Before it, the answer's hop went round
   the run queue, and every runnable process took a whole tick first:
   with one CPU-bound process running, a request to `fs` cost ~8ms
   instead of ~2ms. The blocking process only gives up the rest of
   its own turn, so no other process waits longer than it would
   without the hand-off.
2. **A runnable driver** (`k_drv_set`). This also applies when an
   interrupt has woken a bound driver.
3. **Round robin.**

**Timeouts, and drivers woken by them.** A timeout of N ticks ends one
tick after its deadline tick, so it lasts between N and N+1 ticks; a
process woken by its timeout -- a driver included -- takes its turn in
the round robin. Two variants were tried in round 10 for USB ethernet
and are switches in the kernel shell, both off by default: `sched late
0` (end at the deadline tick) and `sched drvwake on` (a driver woken by
its timeout goes first, as one woken by its interrupt does). Both made
TFTP slower and lost more frames ([userspace_drivers.md](userspace_drivers.md),
round 10).

**Waiting means blocking.** A process that waits for a message must
block in `z_msg_recv()` or `z_msg_recv_match()`, not loop on
`z_msg_read()`: a loop is runnable all the while, and every turn the
round robin gives it is a whole tick of nothing, taken from everyone.
`zstream`'s consumer did that until round 10, and a `tget` in the
kernel shell took a tick from `usb` and `net` at every block. Kernel
code has the same calls (`msg.c`); `z_msg_wait_timeout()` is a blocked
`z_msg_recv_match()`. `z_msg_wait()` (no timeout) still loops.

A message sent from an interrupt handler sets the hand-off on whichever
process was interrupted. When that process next blocks, the recipient
(`wm`, for input) goes first, which is no harm. The trace shows each
hand-off as `handoff <pid>` (`KTRACE=1`).

### CPU accounting

Every build counts where the cycles go. A process is charged from the
cycle it is switched in to the start of the interrupt that switches it
out, minus the interrupts in between. Each interrupt's time is counted
by kind: the tick, the USB controller, IRQ 0 (a process blocked or a
deferred switch; this is the switch path of every message round trip),
and the rest. The time spent in `waitirq` with nothing to run is
counted as idle. The cost is a few cycles per interrupt and per switch.

The idle span is closed at the entry of the interrupt that ends the
wait, because that interrupt may switch to another process. The
first version measured `waitirq`'s own return instead. On a switch,
that span covered everything other processes ran in the meantime,
and counting it pushed later charges past zero: they wrapped, and
`ps` showed `used` figures larger than the uptime. A charge that
would be negative is now dropped.

`cpu_ticks` (in `ps` and `z_proc_info_t`) cannot see this. It charges
whoever is running when a tick lands, so a process that runs between
ticks is invisible: a driver woken by an interrupt, or a client and a
server bouncing messages. The kernel's own interrupt work is invisible
too. In round 7c, `ipcbench` [7] found ~37% of the CPU gone while every
tick was billed to `ipcbench` itself.

- `ps` prints both lines: `used` per pid, and `kernel: tick usb yield
  other idle`, all in units of 65,536 cycles so they compare with
  `cpu`.
- `z_cpu_stat()` (`Z_SYS_CPU_STAT`, `z_cpu_stat_args_t` in `zproc.h`)
  returns the raw counts. `ipcbench` samples it around [7] and around
  the filesystem tests and prints the shares, together with the count
  and average cost of each kind of interrupt.

### What a tick costs

The scheduler trace of October 2026 showed each KTIMER interrupt
spending ~7,500 cycles in the handler before anything visible
happened, and ~7,000 more when it switched -- out of a 65,536-cycle
tick. On a CPU that does ~8,000 instructions in a timeslice, that ratio
is the question, more than 732 Hz itself. So it is measured, in every
build:

```
> tick              handler cycles per tick (avg, max), switches, % of CPU
> tick reset        start a new window
> tick sleepset on|off
```

The figure runs from the KTIMER branch's start to the handler's exit;
`irq_vec`'s register save and restore (~1,500 cycles each) come on top.
`ipcbench` section [7] measures the other side: what a process that
never blocks loses in half a second, to the tick and to whatever else
runs.

Two things the tick did on every beat. The first is behind a switch
for comparing (on by default); the second left with the USB stack:

- **`sleepset`**: the wake sweep looks only at processes that set a
  deadline (`k_sleep_set`, noted by `k_sleep_note()` wherever
  `wake_tick` is set), rather than every slot. A stale bit costs one
  look and is cleared; it can never lose a wake. `off` is the old
  walk of all 32 slots.
- **The USB poll**, which `tick usbpoll off` used to switch off for
  measuring. Since Phase 3 the tick does not touch USB at all: the
  stack is the `usb` process, woken by IRQ 9 and by its own timeouts
  ([usb_driver.md](usb_driver.md)).

Measured with only the kernel shell running, before Phase 3: 11,712
cycles a tick in the handler by default (17.9% of the CPU), 13,608 with
the old sweep, 7,328 with the old sweep and no USB poll -- plus ~3,000 a
tick of register save and restore. The sleeper set saves ~1,900 cycles
a tick; the USB poll cost ~6,300.

So the kernel tick first stepped USB fully only while it had to (every
tick while enumerating, every eighth once settled), and then stopped
stepping it at all. `usb` keeps the same pace as a process
(`z_usbh_wait_ticks()`), and wakes for nothing once settled with no
hub.

### The tick rate

**Not changed; recorded here for when it is reconsidered.**

732 Hz is 48 MHz / 65,536, from a 16-bit counter in `rtl/sysctl.v`
(`rtc_ctr`). On this CPU a 1.37 ms timeslice is only ~8,000
instructions, so the beat's fixed cost is large in proportion: before
the changes above, ~22% of the CPU with nothing to do.

What the rate decides, under the responsiveness criterion
([userspace_drivers.md](userspace_drivers.md)):

- **Not idle latency.** A message or an interrupt wakes its process at
  once; nothing waits for a tick when the CPU is free.
- **Latency under load.** A woken process runs when the running one
  blocks or the tick arrives: up to one tick (1.4 ms at 732 Hz, 2.7 ms
  at 366 Hz -- both small against a 16.7 ms frame).
- **Sharing between processes that never block**, a tick each per turn.
- **Granularity of timed sleeps and timeouts.**

The decision rule, when there are new `tick` and `ipcbench` [7] figures
after the changes above:

1. Quiet tick (handler plus ~3,000 cycles of save/restore) **under
   ~6% of the CPU**: keep 732 Hz.
2. **Over ~10%**: halve it to 366 Hz (a 17-bit `rtc_ctr`), which halves
   the cost and doubles the latency bound to a still-small 2.7 ms.
3. Before any change, the ~40 hard-coded `732`s in `sw/` (zstream, zdns,
   zport, sh.c, the ms-to-ticks helper, tests) are converted to
   `Z_TICK_HZ`, which is already defined from the clock in `zsoc.h`. A
   rate change is then the RTL counter and that one define.

**Measured after round 5c** (October 2026, sleeper set on, USB
stepped every eighth tick when settled), with only the kernel shell:

| | cycles per tick | share of the CPU |
|---|---:|---:|
| `tick`, handler only, while the shell waits for input | 8,340 avg | 12.7% (+4.6% save/restore) |
| `ipcbench` [7], whole interruption, while a tiny loop runs | ~6,300 (7 in 8 ticks), ~12,300 (the USB tick) | 12.1% in all |

The same handler costs more when it interrupts the shell than a tiny
loop: the instruction cache. The handler's code is cold after whatever
it interrupted, so it runs largely from memory -- the figure depends on
what the machine is doing, and ~10-17% of the CPU is the honest range.

**By the rule above that is the "halve it" zone**, and the
recommendation is recorded rather than acted on: when the rate is next
touched, convert the hard-coded `732`s to `Z_TICK_HZ`, then go to
366 Hz -- or, if the tick is reworked anyway, tickless. Either way the
USB driver leaving the kernel (phase 3) has taken the USB stepping out
of the handler for good.

The longer-term alternative is **tickless**: a one-shot deadline for
the next wake-up instead of a fixed beat. The CPU's own countdown timer
(`timer()`, IRQ 0) already serves the yield; using it for deadlines too
would remove the idle tick entirely. A bigger change, worth it only if
the cheap tick is not cheap enough.

### Diagnostics build

`make -C sw/os clean && make -C sw/os KDIAG=1` adds the shell's
`bench`, `sdbench` and `tick` and the per-tick counters behind `tick`.
They were in every kernel; they are measuring tools, and the image is
tight until the drivers leave it, so they are a build option now, like
`USBH_DEBUG` and `KTRACE`. Off: 254,012 bytes. `KDIAG=1`: 259,648.
`KTRACE=1`: 257,564. **Both at once do not fit** (263,200): build one
or the other.

### Raising Z_PROCS_MAX past 32

Not done, and not planned yet -- but the places that would have to be
looked at are worth writing down while they are known:

| | |
|---|---|
| per-pid sets | `k_pidset_t` (the scheduler, the UART's waiting writers): nothing to do |
| `z_procs[]`, `z_mailboxes[]` | ~1KB of `.bss` per slot, so of the 256KB image; see "The limits, and raising them" below |
| `Z_PIDREG_MAX` | names, 64 today; see below |
| the scheduler trace | `ktrace.c` stores a pid in 16 bits: fine to 65535 |
| apps | anything that stores a pid in a byte, or assumes pids below 32. None known in `sw/os`; not audited in `sw/apps` |

A process blocks by calling `z_proc_wait(ticks)`, and is woken either
by the tick deadline or by a message arriving (`k_proc_unblock()`,
called from `msg.c` on every delivery).

**Blocking yields on the spot.** It used to not: the caller kept
whatever remained of its timeslice and spun in its own loop until the
next KTIMER tick noticed it was blocked. One partial timeslice per
block is a small waste in isolation, and a large one for a process that
blocks and wakes many times a second, which is what every well-behaved
app in this system now does.

The obstacle was the syscall boundary. A syscall is a plain `jalr` with
no register save (see "The syscall boundary" below), so the kernel
cannot switch away from inside one -- there is no saved frame to come
back to. Writing one would mean a second context-switch path to keep in
step with `irq_vec`, for about 800-1200 cycles per wait.

So `k_proc_yield_blocked()` uses the path that already exists: after
marking itself BLOCKED it fires picorv32's `timer` instruction with a
count of 1, which raises IRQ 0 as soon as the syscall returns. `irq_vec`
saves all 32 registers exactly as it does for any other interrupt, and
`k_sched_switch()` -- extracted from the KTIMER path for this, so there
is still only one switch implementation -- sees the caller is not
runnable and picks somebody else. If nobody else is runnable it
`waitirq()`s instead, sleeping the core until any interrupt rather than
spinning.

That needs `ENABLE_IRQ_TIMER=1` in the bitstream (`rtl/sysctl.v`);
without it the instruction is illegal, which raises IRQ 1 and retries
the faulting PC. A UART-THRE pulse was tried first, so that the
bitstream would not have to change, and does not work: writing
`IER.THRE` is a no-op when THRE is already enabled, which is the
common case during `printf`, and the `waitirq` in that hole froze every
runnable process until the next interrupt.

`k_sched_switch()` deliberately does **not** advance `z_kernel_ticks`
or sweep `wake_tick`; those stay the KTIMER caller's job. A yield must
not look like time passing.

**Blocking rather than spinning is worth more than it looks.**
`docs/app_runtime.md` records a `view` JPEG decode going from 7.7s to
3.1s once the other resident processes stopped busy-waiting. On a
single core with no idle state, one spinning process is a tax on every
other one.

`cpu_ticks` is the only CPU-time measurement in the system: the timer
handler increments the counter of a process once per tick. Sampled
accounting, so work that starts and finishes between two ticks is
invisible.

**Which process, exactly, is not obvious.** It used to be "whoever was
interrupted", which charges idle time as work: a process sitting in
`waitirq` is the one the timer interrupts, so the moment `wm` also went
to sleep, `net` inherited every idle tick in the system and `ps` showed
it busy. The charge now happens *after* the wake sweep and only if the
process is `Z_PROC_RUNNABLE()` -- a blocked waiter is idle and is
billed nothing. A process looping on `z_proc_wait(1)` is still billed,
correctly: the sweep unblocks it first.

`z_irq_census[]` counts interrupt entries by source line, reported at
the end of `k_proc_dump()`. It is measurement only, and the reason it
is worth the nine counters is that `cpu_ticks` cannot distinguish a
process doing work from a process being woken constantly by something
that has nothing to say -- two `ps` runs bracketing a workload give the
interrupt rate by source, which does.

## Memory

One pool at `Z_MEM_BASE` (`0x4000_0000`), sized from the capability
CSRs at boot (`docs/csrs.md`) with `Z_MEM_SIZE_DEFAULT` as the
pre-CSR fallback. First-fit over a linked list of blocks.

| constant | value | |
|---|---|---|
| `Z_MEM_ALIGNMENT` | 4096 | every allocation is page-aligned |
| `Z_MEM_MIN_BLOCK_SIZE` | 32768 | every allocation is rounded up to this |
| `Z_MEM_MAX_BLOCKS` | 256 | `mem_blocks[]` is a fixed array of descriptors: at most this many blocks, used and free, at once; `alloc_metadata()` returns NULL when all are in the list |

### Descriptors are recycled

Until 0.0.7 they were not. `alloc_metadata()` took the next entry of
`mem_blocks[]` and nothing ever gave one back: when `k_mem_free()`
coalesced a block with a free neighbour it unlinked the neighbour's
descriptor from the list and left it, and `mem_block_count` only grew.
Every allocation that splits a free block takes one, so the **255th
allocation since boot failed**, and every one after it, however much of
the pool was free. Each app launch is an allocation, so a board that
starts programs often -- cron, the dock, a demo -- would one day be
unable to start anything. `free`'s "meta" line was the only sign: it
counted descriptors ever taken, not ones in use.

Unlinked descriptors now go on a free list, chained through `next`, and
are taken from it first. The limit is now what the table says: blocks
in the list at once. "meta" in `free` and `blocks_used` in
`Z_SYS_MEM_STATS` report that.

```
cc -std=gnu99 -Wall -Wno-discarded-qualifiers -no-pie -I sw/os -I sw/common \
   -o /tmp/test_mem sw/os/tests/test_mem.c && /tmp/test_mem
```

`test_mem.c` includes `mem.c` and walks its list: 5,000 alloc/free
pairs, alone and around a block that stays (the old code failed at the
255th); 200,000 random steps checked for overlap, alignment, coverage of
the whole pool, coalescing and descriptors in use equal to blocks in
the list; and a full pool, holes, and recovery.

### Arenas

An app's memory is fixed when it starts: its image, `.bss` included,
and the stack-and-heap allowance its header asks for. That has to be
the same on a 1MB board as on a 32MB one, so an app that is only
sometimes large -- an editor with a long document -- either takes the
memory everywhere or cannot have it anywhere. Since 0.0.7 an app may
also have **one arena**: a block of the pool beside its own, sized while
it runs (`Z_SYS_ARENA`, `zproc.h`; `z_arena_set()`/`z_arena_get()`,
`zeitlos.h`).

```c
void *base;
uint32_t got;
if (z_arena_set(2u << 20, &base, &got) == Z_ARENA_OK) ...   // 2MB
z_arena_set(0, NULL, NULL);                                  // gone
```

- **One, and resizable**, rather than an allocator: one extra block per
  process is all it can add to the pool's fragmentation, and the one
  extra region the MPU has is enough for it (below).
- **Placed from the top** of the pool down (`k_mem_alloc_top()`), while
  process images come from the bottom up, so the two do not interleave.
- **Resized** in place when the block next to it is free -- shrinking
  always is -- else grown down into a free block below it (the contents
  slide down), else **moved**: a new block, a copy, the old one freed. A
  move changes the base, so an app addresses its arena by offset and
  takes the base again after every call. The copy runs with interrupts
  on; the block being filled is in `arena_pending` meanwhile, so a kill
  in the middle frees it too. A failed call changes nothing.
- **Kept clear for launching.** No arena may take the pool below the
  largest core app's image plus stack, read from the flash archive at
  the first call (`k_arena_reserve()`), so whatever the person opens
  next still opens; a move counts both blocks while it lasts. On a 1MB
  board that is all there is, and arenas are refused: those boards keep
  to what their apps hold in `.bss`, exactly as before.
- **Freed** with the process -- exit, kill or crash -- in the same place
  its block is, so nothing can leak it.
- **Written only by its owner.** While the app runs, the MPU's grant
  window ([mpu.md](mpu.md#the-grant-window)) holds the arena: the window
  was used only by drivers, which open grants into a client's block,
  and a driver may not have an arena, so one window serves both. Anyone
  can read it, as anything. Syscalls accept it (`k_user_ok()`).
- **Not a message payload.** A borrowed payload must lie in the sender's
  block, and a driver's grant into a client covers the block, not the
  arena -- so file I/O into or out of an arena goes through a buffer in
  the app's own memory. `fs` refuses a buffer outside the window rather
  than writing there.

`free` in the kernel shell lists each arena and the reserve.
`sw/os/tests/test_mem.c` covers the allocator's side -- placement from
the top, shrinking and growing in place, growing down, and two thousand
resizes -- and `sw/apps/text` is the first user
([text_editor.md](text_editor.md#the-document)).

The ramdisk takes a SHARE of the pool rather than a fixed size
(`Z_RAMDISK_DIVISOR`, an eighth, capped at 4MB) -- scratch space should
be proportional to the machine, and a figure that suits 32MB is most of
1MB. That instinct is worth reusing whenever something new wants a
fixed budget.

## The syscall boundary

Not a trap. The kernel installs a function pointer at `0x0000_000c`
(`reg_kernel`) and an app calls through it: a plain `jalr` from the
app's own code into kernel code, with the app's `gp` still in place.

### The `gp` hazard

Because a syscall is an ordinary call, kernel code reached that way
executes with **the calling app's `gp`**, not the kernel's. `gp` is not
part of the C ABI's caller/callee-saved convention, so nothing restores
it.

Any kernel global reached `gp`-relative therefore reads the wrong
address. The mitigation is the explicit `__attribute__((section(".bss")))`
on `z_procs[]`, `z_mailboxes[]`, `block_list` and `mem_blocks[]` --
which forces absolute addressing.

`docs/messaging.md` and `sw/os/mem.c` both carry the chase; it took
three separate bugs to identify, and it does not announce itself -- a
global read through the wrong `gp` returns another process's memory
without complaint.

The same hazard, from the other side, is why `libz` is built
`-mno-relax -msmall-data-limit=0` (`docs/libz.md`).

### Preempt deferral

FatFs was not reentrant, so a process inside a filesystem syscall was
not preempted (`k_no_preempt`, capped at `K_NO_PREEMPT_MAX_TICKS`, 64
ticks / ~87ms), and the USB syscalls were held the same way because
they shared the transaction engine with mass storage. Both reasons have
gone with FatFs (Phase 2) and the USB stack (Phase 3): **no syscall
holds the scheduler any more.** The mechanism stays for the kernel's
own short critical sections (`k_fs_enter()` in `cfg.c` and
`kvstore.c`). A swap deferred by it is owed and paid when the guard
drops (`k_resched_owed`), not dropped.

## The kernel image budget

**160KB since Phase 3** ([usb_driver.md](usb_driver.md)). Two things
happened together:

- **The USB host stack left** for the `usb` process (about 34KB of
  code and `.bss`).
- **The image stopped carrying `.bss`** (57KB of zeros).

`kernel.bin` went from 223,852 bytes to 134,264, and the kernel's flash
region from 224KB to 160KB, with the core-app archive starting there.
It was 224KB from Phase 2b (the filesystem left) and 256KB before;
the history below says what it was when it happened. Builds: default
134,264 (29,576 free), `KTRACE=1 KDIAG=1` 138,564.

**October 2026: 151,112 bytes, 12,728 free** -- under `KERNEL_WARN_FREE`
(16KB), so the build lists the largest symbols. What took it there: the
NFS search and `nfs` command, the rescue console's toggle, the shell's
`ls` and `run` changes, load timing, and the decoder for compressed
executables (`sw/common/zexec_lz4.c`, about 1KB;
[executables.md](executables.md#compressed-executables)). The DEFLATE
decoder tried first was 3.3KB, with 3.3KB of `.bss`. Building the kernel with
`ZFMT_LITE`'s stdout ([build.md](build.md)) would win back about 7KB;
its three `fflush(stdout)` calls (the prompt) would have to go first.

**`.bss` is not in the image.** Until Phase 3, `sw/os/Makefile` linked
`kernel.bin` with `objcopy --pad-to=_end`, so `.bss` was in the file as
zeros, on the belief that the BIOS copy was what zeroed it. It was not
the only thing. The kernel's `_start` -- the first instruction at
`0x40000000` -- is the toolchain's crt0, and it memsets
`__bss_start`..`_end` before anything else runs. The padding was
redundant, and cost a byte of flash per byte of `.bss`. Without it:

- **The BIOS copies `ROM_OS_SIZE`** (`sw/bios/bios.c`, 160KB; an older
  BIOS copies more, which only adds archive bytes past the image). What
  lands past `_edata` is garbage that crt0 then zeroes, up to `_end`.
- **`sw/os/Makefile` checks the crt0:** it fails the build if `_start`
  does not call `memset`, since a toolchain whose crt0 did not would
  leave `.bss` holding flash.
- **`.bss` is a RAM cost now, not a flash one.** RAM is still finite: the
  smallest board has 1MB.

So:

| | |
|---|---|
| flash layout | the 1 MB Zeitlos region at the base `rtl/csrs.v` word 63 reports (`z_flash_base()`; `0x100000` on ECP5, `0x300000` on Artix-7): kernel at `+0`, 160KB, core-app archive (`Z_ZAR_FLASH_OFFSET`) immediately after at `+0x28000`, to `+0xFC000`, the flashtest sector, the key/value store in the last 8 KB. `docs/boot.md`, "The flash layout" |
| the limit | `kernel.bin` (text and data) under 160KB (`KERNEL_MAX_SIZE`) |
| what spends it | code and initialised data; `.bss` spends RAM |
| failure mode | the BIOS truncates; the kernel boots and then misbehaves |

That last row is why `sw/os/Makefile` now **fails the build** if
`kernel.bin` exceeds `KERNEL_MAX_SIZE`, prints the free figure when it
does not, and lists the largest symbols when headroom drops below
`KERNEL_WARN_FREE`.

```
make -C sw/os kernel-size
```

gives `text`/`data`/`bss` and the twelve largest symbols. **Use it
rather than reasoning about struct sizes.** Two attempts to predict
this from arithmetic gave two wrong answers: the struct sizes were
right both times (`z_msg_envelope_t` is 24 bytes, a mailbox 780, and a
`z_proc` was 148 then and is 188 now) and the totals were not, which
means the model was missing something the model could not see.

### Spending it: a worked example

The rename, stat and extended-listing syscalls and real file
timestamps ([filesystem.md](filesystem.md)) first measured **+3,345
bytes**, which left 148 bytes of the 256KB. Most of that was not code:

| | bytes | what was done |
|---|---:|---|
| a `FIL` for the busy probe | 556 | borrow a free slot of the handle table's instead |
| two `FILINFO`s | 560 | share one (`fs_fno`) between the listing and `fs_stat_info()` |
| a days-to-date conversion | ~100 | a year/month walk instead of Hinnant's divisions |

and then one thing that had nothing to do with the feature: `sdbench`,
a benchmark run by hand, kept a **2KB static buffer** in every boot's
image. It now allocates the buffer for the length of a run
(`sdb_with_buf()`, `sw/os/fs/sdbench.c`).

Net: **+608 bytes** for three syscalls, real timestamps and the shell's
`mv`, with 2,880 bytes of headroom left (it was 3,488). The three
`apps.irc.*` rows added to the shared config table later took 312 more
(the kernel links `zcfg.c`'s descriptions too): **2,568 bytes** of
headroom.

Then two changes gave it back several times over. The kernel's
`printf`, `snprintf` and `vsnprintf` had been defined in `kruntime.c`
on top of newlib's two integer-only engines (`_vfiprintf_r` and
`_svfiprintf_r`, ~14KB between them); they are now
`sw/common/zfmt.c`, one engine of ~1.5KB that writes through the same
FILE layer ([build.md](build.md#integer-only-printf-zfmt)): **-11,212
bytes**. And the shell's `bench` command, like `sdbench` before it,
allocates its 4KB scratch buffer for the run instead of keeping it in
`.bss`: **-4,032**. That leaves **17,812 bytes** of headroom at the time
of writing (`kernel.bin` 244,332).

The lesson
generalises: before cutting code, look for static buffers that only a
diagnostic or a rarely-used command needs, and allocate them when used.

The build also no longer runs `strip` on `kernel.elf` in place. It
could never have affected the output -- `objcopy -O binary` emits
allocated section contents and nothing else -- and what it did do was
delete the only record of what is in the image, which is exactly what
is needed when the image is too big. Raising `Z_PROCS_MAX` to 64 put the image at 269,784 bytes
-- 7,640 over -- and it was found by flashing a board. A link-time
error is the right place for that; a kernel that boots and then
misbehaves is the worst one.

### Where the space actually went

Measured with `make -C sw/os kernel-size`, which is why it exists:

```
   text    data     bss     dec
 200164    2668   50964  253796
```

(Those are the figures from the round that found this. After the
`sscanf`, printf and padding fixes below, and with the USB ethernet
driver added, `kernel.bin` is 210,292 bytes with 51,852 free.)

`.bss` is 51KB and `z_mailboxes` is 24,960 of it -- exactly the
32 x 780 the arithmetic predicted. **`.bss` was never the problem.
`.text` was**, and about 80KB of that 200KB is newlib's formatting
machinery:

| symbol | bytes | |
|---|---|---|
| `_vfprintf_r` | 13,708 | float-capable printf |
| `_svfprintf_r` | 13,440 | ...and again, for the string variants |
| `categories` | 13,788 | locale tables |
| `__ssvfscanf_r` | 8,396 | scanf |
| `d02f4`, `b02cf` | 12,710 | decimal power tables for float conversion |
| `_strtod_l` | 6,532 | string to double |
| `_dtoa_r` | 6,460 | double to string |
| `_vfiprintf_r` | 6,620 | integer-only printf |

**This is the trap `docs/app_runtime.md` documents for apps**, in the
one place nobody had applied it. Its standing advice -- format by hand
rather than call printf -- was written about a 100KB cost in an 8KB
app, and the same cost had been sitting in the kernel unnoticed because
nothing reported it.

#### Fixed: three `sscanf` calls

`sh.c` parsed an address and a pid with `sscanf(arg, "%lx")` and
`sscanf(arg, "%ld")`. A generic `scanf` has to be able to handle `%f`,
so those three calls pulled in `__ssvfscanf_r`, `_strtod_l`, the locale
tables and the decimal power tables -- on the order of 40KB for parsing
two integers.

Replaced with `parse_uint()`, twenty lines. **Rebuild and re-run
`kernel-size` to see what it actually returned** rather than trusting
the estimate; the estimates in this document have a poor record.

#### Fixed: integer-only printf

`_vfprintf_r` and `_svfprintf_r` are the float-capable formatters, and
a formatter that can print a double drags in `_dtoa_r`, the
multi-precision helpers and libgcc's soft double arithmetic behind it.
Nothing in `sw/os` formats a float: there is no `%f`, `%g`, `%e` or
`%a` in any format string it links, and the one `%g` in
`../common/zobj.c` (`z_obj_print`) is in a function section GC already
drops from this kernel.

`sw/os/kruntime.c` now defines `printf`, `snprintf` and `vsnprintf`
itself, forwarding to `viprintf` / `vsniprintf` -- the integer-only
engine, which was already linked because `assert` uses it. The libc
members holding the full versions are then never pulled in. **Measured:
kernel.bin 256,224 -> 210,260 bytes, 45,964 saved.**

Defining the three functions rather than `#define`-ing the names keeps
gcc's `-Wformat` checking working at every call site. It is guarded
`#ifndef __PICOLIBC__`, because picolibc (`sw/common/arch.mk`, when the
newlib prefix is absent) does not name the integer variants the same
way.

**The trap this leaves:** a `%f` added to kernel code later compiles,
`-Wformat` accepts it, and it prints nothing useful. Print fixed point
instead, as `fs/sdbench.c` does ("cycles per byte, x100, so one decimal
prints without floating point").

#### Fixed: a page of linker padding

`../common/riscv-os.ld`'s `DATA_SEGMENT_ALIGN` puts the data segment on
a fresh page, or at the same offset in the next page, whichever the
linker thinks is cheaper -- so depending on where `.rodata` happens to
end, up to 4096 bytes of zeros sit between text and data **in the
image**. That is right for an ELF a loader maps with an MMU, and pure
waste for a flat image the BIOS copies to `0x40000000` with no MMU.

It was found the hard way: a 5KB code change made `kernel.bin` grow by
9KB, and the extra 4KB was this gap crossing a boundary. `sw/os/Makefile`
now links with `-z max-page-size=16 -z common-page-size=16`
(`SEG_PAGE_LDFLAGS`). Every section keeps its own alignment; only the
page-sized rounding between segments goes.

### If more room is ever needed

The long-term answer is [userspace_drivers.md](userspace_drivers.md):
the filesystem, SD and USB host stack as processes, and the core-app
archive starting where the kernel ends. Short of that, in order of how
much they disturb:

1. **`Z_MAILBOX_DEPTH`** (`sw/common/zmsg.h`) is 84% of a process
   slot's cost -- 780 bytes against `z_procs[]`'s 148. Halving it to 16
   halves that.
2. **A per-board `Z_PROCS_MAX`**, which needs a board define in the
   kernel build; `sw/os/Makefile` passes only `ARCH_DEFS` today.
3. **Moving the flash layout** -- `ROM_OS_SIZE` and
   `Z_ZAR_FLASH_OFFSET` together. This moves the core-app archive, so a
   board flashed with the old layout and a new BIOS finds neither. It
   is the only option that actually creates space rather than
   redistributing it.

**Dropping the padding** was on no list, because it was believed to be
what zeroed `.bss`. It was done in Phase 3, after reading `_start`.

## Scheduler trace

`make -C sw/os clean && make -C sw/os KTRACE=1` builds a kernel with a
ring of 192 timestamped scheduler events (`sw/os/ktrace.c`, about 5KB
of image, so off by default):

| event | recorded in | argument |
|---|---|---|
| `send` | `k_msg_send()` | recipient pid |
| `unblock` | `k_proc_unblock()` | pid, `+0x100` if it was blocked (otherwise a WAKE was recorded) |
| `block` | `k_proc_wait()` | timeout ticks |
| `noblock` | `k_proc_wait()` returning at once | 1 WAKE consumed, 2 mail waiting |
| `yield` | `k_proc_yield_blocked()` | 1 `timer(1)`, 0 `waitirq`, 2 already runnable |
| `irq` | interrupt entry | pending bits |
| `switch` / `keep` | `k_sched_switch()` | pid switched to / 1 nobody else runnable, 2 scan exhausted |
| `defer` / `resched` | the filesystem guard | |
| `tick` | the KTIMER branch | 0 entered, 2 after the wake sweep (1 was after the USB poll, until Phase 3) |

`net` arms the trace itself at block 100 of a TFTP get
(`TFTP_TRACE_BLOCK`, `sw/apps/net/tftp.c`), since the console is busy
during a `tget`; `ktrace` afterwards shows the middle of the transfer.

Each line carries `rdcycle` from the first event and from the previous
one, and the pid that was running. It is armed by `Z_SYS_KTRACE` (op
1, cleared and started; op 0, stopped) or `ktrace arm` / `ktrace off`,
stops by itself when full, and `ktrace` prints it. `ipcbench` arms it
around six round trips. A kernel without `KTRACE=1` fails the syscall,
and the shell has no `ktrace`.

## Exit status, and the exit ring

`z_exit()`'s argument used to be **discarded**. It took a `z_obj_t`,
called `k_proc_kill()`, and dropped it -- so every exit status in the
system was thrown away at the first step, and `sw/apps/zcc`'s entry
stub packed one carefully for nobody.

Two things needed it, both in `docs/posix.md`'s Phase 5: a shell cannot
make `a && b` mean anything without a status (the best it can do is
report whether `a` *started*), and it cannot hand the terminal to a
full-screen program and take it back without knowing when that program
ended.

### Why a ring rather than a zombie

The textbook answer is to keep the process slot until someone reaps it.
That leaks here: **nothing in this system is obliged to reap anything**,
so an unreaped child holds a slot forever -- and the shell that forgets
to ask is exactly the shell that will run many children.

So `k_exit_ring[]` holds the last 16 `(pid, status)` pairs.
`Z_SYS_PROC_STATUS` answers RUNNING from the process table, EXITED from
the ring, and UNKNOWN when it finds neither.

A ring cannot leak. It can only forget, and it forgets oldest-first,
which is the order nobody is still waiting on. Sixteen because a shell
asks within a few hundred milliseconds and nothing else asks at all; if
something ever misses, the symptom is UNKNOWN rather than a wrong
answer.

**UNKNOWN is a successful reply, not a failure.** "I do not know" is
information, and a caller that got `Z_FAIL` could not tell it from a
malformed call. A shell treats it the same as EXITED with status 0,
which is what it assumed about every child before there was a status at
all.

An entry for a pid already in the ring is replaced rather than
duplicated: pids are reused, and a stale entry would answer a question
about the NEW process with the OLD one's status -- worse than UNKNOWN.

## Process serials

A pid names a slot, not a process. `k_proc_create()` takes the lowest
free slot, so a program started just after another ended usually gets
its pid, and "is pid 7 still running?" is answered yes by whatever took
pid 7. The exit ring above already had to work around that.

So every process also gets a **serial** when its slot is filled
(`z_proc.serial`): the next value of a boot-wide counter, never 0,
never given twice (32 bits; a wrap skips 0). `Z_SYS_PROC_SERIAL`
returns the serial of the process at a pid, or 0 when nothing is
running there -- a process being torn down counts as gone, as it does
for `Z_SYS_PROC_STATUS`.

```c
uint32_t pid = z_proc_run("convert");
uint32_t ser = z_proc_serial(pid);       // right away
...
if (z_proc_serial(pid) != ser)            // ended, whatever has pid now
```

The pair (pid, serial) names one process for good. The filesystem
driver uses it for descriptor owners ([fs_driver.md](fs_driver.md),
"Owners that are gone"). The syscall is appended at the end of
`syscalls.def`, so no earlier number moves; the field is appended at
the end of `z_proc`, so no earlier offset moves.

## The ABI id

`Z_SYS_ABI` (`zproc.h`, `z_abi_args_t`; `z_abi()` in `zeitlos.h`)
fills in the ABI id the kernel was built with: `Z_ABI_ID` from
`sw/common/zabi.h`, a fingerprint of the interfaces between flash and
the card (syscalls, their structs, the executable format, the core
apps' protocols), made by `tools/gen_abi.py`. The kernel build stops
when the committed id is stale, so it is the id of the sources the
kernel was compiled from. `update` compares it with a release's before
writing a card ([update.md](update.md#abi)). A kernel older than the
syscall answers `z_fail` and leaves the struct alone, so `z_abi()`
returns `""` there.

```c
char id[Z_ABI_ID_MAX];
z_abi(id, sizeof id);          // "03a25f4cbf544cf0", or "" on an older kernel
```

## The limits, and raising them

`docs/posix.md`'s Phase 5 wants pipes as processes, which is what
prompted looking at these.

| | value | |
|---|---|---|
| `Z_PROCS_MAX` | **32** (was 16) | raised; see below |
| `Z_MAILBOX_DEPTH` | 32 | 24 bytes per envelope per slot |
| `Z_MEM_MIN_BLOCK_SIZE` | 32KB | not yet changed; see below |

### `Z_PIDREG_MAX` 32 -> 64 -- DONE, and it was nearly missed

Raising `Z_PROCS_MAX` had a consequence one layer up that the first
pass did not consider: `Z_PIDREG_MAX` (`sw/os/pidreg.h`) is a total
across every process and every NAME, and it was 32 -- equal to the old
process count, and already tight, because a process may register more
than one name and nine apps in this tree register at least one.

At 32 processes the registry, not the process table, would have become
the binding limit. The failure is quiet: `z_pid_register()` returns
false, the app runs on, and nothing can find it by name -- `term`
cannot open a port to a provider that never registered, and `posix`
cannot hand a terminal to a child whose name does not resolve.

64 entries, about 2KB.

**A limit raised in one place tends to move the binding constraint
somewhere else.** Worth looking one layer out each time.

### `Z_PROCS_MAX` 16 -> 32 -- DONE (64 was tried and did not fit)

Two arrays scale with it, 968 bytes a slot between them:

| | per slot | x16 | x32 | x64 |
|---|---|---|---|---|
| `z_procs[]` | 188 B | 2.9 KB | 5.9 KB | 11.8 KB |
| `z_mailboxes[]` | 780 B | 12.2 KB | 24.4 KB | 48.8 KB |
| | | 15.1 KB | **30.3 KB** | 60.5 KB |

64 was the first attempt and it overran the image budget above by
7,640 bytes, when a slot was 148 bytes rather than 188 -- the saved
GPU scissor has since added 40 to it, so the same attempt would now
overrun by about 10,200. **32 costs 15.1KB more than 16** and leaves
comfortable headroom -- and it is more than Phase 5 needs, which is one
process per pipeline stage against about a dozen resident.

The `.bss` figure is a RAM cost on every board and a FLASH cost on
every board, which is the part that was missed the first time.

The scheduler scans the table each tick, so the scan grows from 16 to
32 entries at 732Hz -- a few hundred instructions a second.

Raised **flat, not per-board**. The kernel is not
currently built with a board define (`sw/os/Makefile` passes only
`ARCH_DEFS`), so per-board scaling would mean adding that machinery for
a number whose real limit is memory -- and memory is already
board-scaled.

### Mailboxes stay static

An earlier draft of this proposed allocating mailboxes from the pool at
process creation, on the grounds that they were ~7.2KB each and 112KB
in total.

**That was wrong by a factor of nine, and the mistake is worth
recording.** `z_msg_t` is 216 bytes, and it is the APP-side structure:
it carries `_tables[4]` and `_items[16]` inline as scratch space for
resolving `Z_LIST`/`Z_MAP` payloads on receipt. The kernel's ring holds
`z_msg_envelope_t`, which is `to`/`from`/`subject`/`tag`/`obj` and
**24 bytes**. `zmsg.h` says so plainly next to `Z_MAILBOX_DEPTH`.

So a mailbox is 780 bytes and all sixteen are 12KB, not 112KB. There is
nothing there worth trading an invariant for.

And the invariant is a real one. `docs/messaging.md` states it as **"no
dynamic allocation in the kernel for messaging"**, and the incident
behind that rule is on record in the same file: `wm` built a five-key
`Z_MAP` per window move, 384 bytes at a time against an 8KB budget;
about twenty moves exhausted the heap, `z_obj_map()` wrote through the
NULL that `malloc()` returned, and the machine went down with nothing
in the symptom pointing at either the leak or the allocator.

That rule is really about the per-message path, which a
per-process mailbox allocation would not have touched. But the honest
reading is the other way round: the reason to keep mailboxes static was
never mainly the hot path, it is that **the kernel's own bookkeeping
should not be able to fail at a moment when reporting the failure needs
the thing that failed.** 43KB is a cheap price for that, and the
argument for spending it evaporated once the arithmetic was right.

### `Z_MEM_MIN_BLOCK_SIZE` 32KB -> 4KB is the one with a real risk

Every allocation is rounded up to 32KB, so a small filter -- 8KB of
stack, a few KB of image -- costs 32KB and `ls | grep | wc` costs 96KB
before doing any work. At 4KB (which is already `Z_MEM_ALIGNMENT`, so
no new rounding rule) the same pipeline costs about 48KB.

`Z_MEM_MAX_BLOCKS` may have to rise with it: it bounds the blocks in
the list at any one time (descriptors are recycled since 0.0.7, so it
is not a lifetime limit), and smaller blocks mean more of them. 1024
entries is 16KB of `.bss` against today's 4KB.

**This is the change to be least confident about.** A smaller minimum
means more, smaller blocks and more external fragmentation, and
`k_mem_alloc()` is first-fit. `zcc`'s 4MB request is exactly what
suffers: it needs one contiguous run, and a shell that has started and
exited fifty small processes is the situation most likely to deny it.

So it should land **with a measurement**. `free` (`k_mem_dump()`)
already reports fragmentation: run a realistic session -- several
pipelines, then `zcc` on a real file -- before and after.

If it does bite, the answer is not to revert but to place small
requests from one end of the pool and large ones from the other, so
compiler-sized runs stay contiguous. Bigger change; not to be done
pre-emptively.

### Order

1. ~~`Z_PROCS_MAX` to 32~~ -- **done**. 14.5KB more `.bss`, which is
   also 14.5KB more flash image then; see "The kernel image budget".
2. Block size and count, with `free` output either side. **Not done**;
   this is the one that could regress `zcc`.

## See also

Beyond the subsystem documents listed at the top:

- `docs/userspace_drivers.md` -- moving the filesystem, SD and USB
  host out of the kernel (proposal)
- `docs/csrs.md` -- how the kernel learns the machine's real memory size
- `docs/icache.md` -- why instruction fetch dominates CPI here
- `docs/console.md` -- the console log ring every console byte is
  recorded in (`k_uart_putc()`, `kprint()`), and the two syscalls that
  let `sw/apps/console` show it in a term window
- `docs/rescue_console.md` -- the kernel shell on the monitor and the
  USB keyboard, with no wm: ESC held at boot, or wm missing or crashed
- `docs/mpu.md` -- crash reports (`k_fault()`), the kernel panic, and
  the memory protection unit the kernel programs at boot and on every
  context switch
- `docs/dcache.md` -- the data cache the kernel enables at boot, and
  why no kernel code needs to flush or invalidate it
- `docs/sdcard.md` -- the SD path, and the bus behaviour a stalling
  peripheral has against the arbiter

## Reboot

`Z_SYS_REBOOT` (`k_reboot`, `z_reboot()` for apps; `reboot` in the
serial shell and in `posix`) syncs every open write handle of every
process, then reconfigures the FPGA through `rtl/socctl.v`'s RECONFIG
register and the PROGRAMN pin. On a board whose gateware cannot pull
PROGRAMN it fails and changes nothing. `docs/zboot.md` section 6.

`Z_SYS_JUMP` (`k_jump`, `z_jump()`; `jump` in the serial shell and in
`posix`) does the same from any flash address, through the jumploader
at `0x200000` (opt-in, `JUMP=1`, flash past 2 MB), which
`sw/os/jumpapi.c` re-points in place; `reboot` is a jump to 0, or, on
gateware without a jumploader (the default), a plain reload from 0. `k_boot_to()` in `kernel.c` is both, and lists why either
refuses. `docs/zboot.md` section 5.

## Writing the flash

`Z_SYS_FLASH` (`sw/os/flashapi.c`; apps use `sw/common/zflash.h`)
erases and programs the configuration flash through `rtl/spiflash.v`:
one process at a time holds a write session, released when it exits,
and no core app is launched from flash while one is open. Nothing below
`0x040000` can be written, which the controller enforces in hardware.
The serial shell has `flash` (ID, size, lock, status) and `flashtest`
(the on-board test). `docs/spiflash.md`.

The last 8 KB of the Zeitlos region is the kernel's own: the **key/value store**
(`sw/os/kvstore.c`, `sw/os/kvlog.c`; apps use `sw/common/zkv.h`), a few
KB of settings that must exist with no sdcard. `Z_SYS_FLASH` refuses to
touch it. Writes are serialised by a lock that a dying process releases
through `k_flash_release_pid()`; reads take no lock. The console has
`kv`. It costs 13.3 KB of image; its on-board test, 2.8 KB more, is
built only with `make KV_TEST=1`. `docs/kvstore.md`.

## The password

`Z_SYS_AUTH` (`sw/os/auth.c`, `sw/os/authcore.c`; apps use
`sw/common/zauth.h`) keeps the machine's one password as a salted
PBKDF2-HMAC-SHA256 hash in the key/value store, and checks it for the
lock screen, the console, and telnet and SSH password logins
(`docs/netserve.md`). One tally of
failures and one check at a time, system-wide; a process killed
mid-check releases the gate through the reaper
(`k_auth_release_pid()`, beside `k_flash_release_pid()`). The console
has `passwd`, `passwd reset` (UART0 only -- the receive ring marks
bytes injected through `console0`) and `lock`. 9 KB of image.
`docs/security.md`.
