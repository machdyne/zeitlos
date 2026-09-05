# Zeitlos Filesystem Concurrency

## Overview

Zeitlos runs FatFs on a single SD card reached over SPI (`rtl/spim.v`,
driven by `sw/os/fs/fatfs/sdmm.c`). FatFs is built **non-reentrant**,
and the kernel is **preemptive**. Those two facts are in direct
conflict, and reconciling them is what this document is about.

The resolution: the syscall dispatcher refuses to let the scheduler
swap a process out while that process is inside a syscall that touches
FatFs. Nothing locks, nothing waits, and no other subsystem is
affected.

If you are adding a syscall that reads or writes files, the one thing
you must do is listed under "Adding a filesystem syscall" below.

## The problem

Two independent pieces of state are shared by every filesystem caller:

- **The FatFs volume work area** (`sdvol0` in `sw/os/fs/fs.c`). With
  `FF_FS_TINY 0`, the `FATFS` object carries a 512-byte window buffer
  used for every directory and FAT sector. It is a single global.
- **The SPI transaction in flight.** `sdmm.c` asserts CS, sends a
  command, and clocks a response or a 512-byte data block back. The
  card is a state machine; between CS going low and the transaction
  completing, no other command may be issued.

The kernel's KTIMER interrupt (`sw/os/kernel.c`) swaps processes on
every tick, at roughly 732Hz. Crucially, **a syscall is an ordinary
function call** made through the `reg_kernel` trampoline, not a trap
that masks interrupts (see the writeup at the top of `sw/os/fsapi.h`).
So the timer fires *inside* syscall handlers, and the scheduler will
happily deschedule a process that is halfway through `f_open()`.

When that happens and the next process also enters the filesystem, the
card is left mid-command and the window buffer is written by two
interleaved callers. The card then fails every subsequent access until
`disk_initialize()` runs again.

### What it looked like

The symptom that led here was the window manager's dock coming up
empty at boot while `ls` worked perfectly a few seconds later. The
instrumented boot log (see "Instrumentation" below) shows the moment it
breaks:

```
disk_initialize: CardType=0x00000018 Stat=0x00000000
init: fs_mount_now = 0x00000000  disk_status = 0x00000000
init: sdcard ready
...
fs_exec_info: f_open('wm')   = 0x00000004   <- FR_NO_FILE, a CORRECT answer
fs_exec_info: f_open('net')  = 0x00000004
fs_exec_info: f_open('term') = 0x00000004
        <- UART output garbles here: two processes interleaved
fs_exec_info: f_open('files') = 0x00000001  <- FR_DISK_ERR, and from
fs_exec_info: f_open('text')  = 0x00000001     here on, everything
fs_exec_info: f_open('read')  = 0x00000001     fails
```

Three things are worth reading carefully:

- The card was **healthy**. `CardType=0x18` is `CT_BLOCK|CT_SD2`, a
  normal SDHC card, and `Stat=0x0` means `STA_NOINIT` was cleared.
- The first three results are `FR_NO_FILE`, which is the *right*
  answer: `wm`, `net` and `term` are core apps that live in the flash
  archive, not on the card. FatFs read the root directory
  successfully. The filesystem was working.

  (This transcript predates the `apps/` search path. An equivalent
  trace today shows **two** `FR_NO_FILE` per app rather than one --
  the root, then `apps/` -- before the flash archive answers. Same
  meaning, twice the lines. See `docs/flash_apps.md`.)
- Then it flips to `FR_DISK_ERR` and never recovers. The card was not
  slow to start; it was **broken mid-boot**.

The garbled UART output at the transition is the direct evidence:
`sh` (pid 0) was loading `net` while `wm` (pid 1) ran `dock_build()`,
and both were inside the kernel at the same time.

This is timing-sensitive, which is why it appeared to come and go.
Anything that changes how much init's app loading overlaps wm's dock
probing changes the outcome -- including making the SoC *faster*. The
bug was latent for a long time behind a slower boot.

## The fix

`sw/os/kernel.c` keeps a counter:

```c
volatile uint32_t k_no_preempt;
```

Non-zero means "whoever is running is inside FatFs; do not swap them
out". It is maintained in two places, because there are two ways to
reach FatFs.

### Layer 1: the syscall dispatcher

Covers everything an application calls. Around the table call:

```c
int fslock = k_syscall_touches_fs(syscall_id);

if (fslock) {
    if (k_no_preempt == 0) k_no_preempt_start = z_kernel_ticks;
    k_no_preempt++;
}

ret = (uint32_t *)z_syscall_table[syscall_id]((z_obj_t *)regs);

if (fslock && k_no_preempt) k_no_preempt--;
```

and honoured in the KTIMER branch of the same function, before the
swap:

```c
if (k_no_preempt &&
    (z_kernel_ticks - k_no_preempt_start) < K_NO_PREEMPT_MAX_TICKS) {
    ret = regs;
    goto done;
}
```

Three properties matter:

- **It is not a lock.** Nothing ever waits on it. It is only read by
  the scheduler.
- **Interrupts stay enabled.** Only the process *swap* is postponed.
  `z_kernel_ticks` is still incremented earlier in the same handler, so
  `z_msg_wait_timeout()` and every other tick-based timer are
  unaffected. Ethernet RX still lands in its buffer; only `net`'s
  userspace draining is delayed.
- **The counter is counted, not boolean**, so the two layers below
  nest harmlessly.

### Layer 2: fs.c's own entry points

The dispatcher is not sufficient on its own. **Kernel code that calls
`fs_*` directly does not pass through it**, and `sh.c`'s `init()` does
exactly that -- see `core_src_of()` at `sh.c:99`, which calls
`fs_exec_info_any()` with no syscall involved.

This was not theoretical. After the dispatcher guard alone, eleven of
twelve dock probes succeeded and one still returned `FR_DISK_ERR`:

```
fs_exec_info: f_open('read') = 0x00000001     <- wm, via EXEC_EXISTS (guarded)
wm: docfs_exec_info: f_open('net') = 0x0k: ... <- sh/init, direct call (not guarded)
```

Only one side of that collision was holding the scheduler off, which
is enough to lose. So `sw/os/fs/fs.c` guards its own entry points with
`k_fs_enter()` / `k_fs_leave()`, declared in `kernel.h`:

| Function | Notes |
|---|---|
| `fs_load()` | released after `f_close` |
| `fs_size()` | |
| `fs_list_dir()` | released at the very end -- the flash-archive loop calls `f_stat()` |
| `fs_exec_info()` | released after `f_close`, before `z_exec_parse()` |
| `fs_load_exec()` | released after `f_close`, before the bss `memset` and icache flush |

Two rules for these:

- **Every early return between an enter and its leave must release.**
  Each of the five above was checked path by path. A missed one leaks
  the counter.
- **Release as early as correctness allows.** `fs_load_exec()` gives
  the pattern: the bss `memset` can be tens of kilobytes and touches
  RAM only, so the scheduler is let go before it.

Callers of `fs_*` therefore do **not** need to bracket anything
themselves -- the guard is inside.

### The syscalls it covers

`k_syscall_touches_fs()` lists them. `PROC_RUN` is in the list and is
the important one -- it resolves *and loads* an executable, so
`init` starting `net` and `wm`'s `dock_build()` probing for apps were
the two callers that actually collided.

```
PROC_RUN        FS_OPEN_WRITE     FS_MKDIR
FS_SIZE         FS_OPEN_READ      FS_TOUCH
FS_READ         FS_READ_CHUNK     FS_SEEK
FS_WRITE        FS_WRITE_CHUNK    FS_DF
FS_UNLINK       FS_CLOSE          EXEC_EXISTS
FS_LIST         FS_OPEN_RW        FS_SYNC
FS_TRUNCATE
```

`FS_OPEN_RW` / `FS_SYNC` / `FS_TRUNCATE` are the in-place editing
calls added for `sw/apps/hex` (`docs/hex_editor.md`). Nothing about
them is special here — they reach FatFs like every other entry, so
they belong in the list for the same reason.

**`FS_TRUNCATE` is the one to watch for the cap below.** Growing a file
allocates clusters and updates the FAT, so a large expansion is the
longest single trip into FatFs this list contains — longer than a 64KB
`FS_READ`. A caller growing a file by a lot should do it in steps
rather than one call, for the same reason the chunked API exists.

It is a `switch` rather than a flag in `sw/common/syscalls.def`
deliberately: that file is shared with app-side code, and its own
comment warns that inserting an entry shifts every later enum value.

### The cap

`K_NO_PREEMPT_MAX_TICKS` is 64 ticks, about 87ms. Past it, the swap
happens anyway.

This exists because `sdmm.c`'s `wait_ready()` spins for up to 500ms and
`rcvr_datablock()` for up to 100ms before giving up. Deferring across
one of those would freeze every process on the machine for half a
second, and a counter leaked by some future error path would freeze it
permanently. **A wedged machine is worse than the corruption this
prevents**, so the deferral is allowed to lose.

64 ticks is chosen to sit well clear of both ends. At roughly 48 CPU
cycles per byte (see `sdmm.c`'s header), a 512-byte sector is about
0.5ms, so a healthy operation is nowhere near the cap -- even a 64KB
single-syscall `k_fs_read` lands around 64ms. Nothing legitimate should
ever hit it.

## Performance

Filesystem operations get marginally **faster**, not slower. The
holder is never blocked and never spins; it simply is not interrupted.
An `f_open` that previously spanned three timeslices with two context
switches now runs straight through. There is no lock to acquire and no
contention path.

The cost is latency for *other* processes, and it is bounded by how
long a single syscall stays in FatFs:

| Operation | Sectors | Approx. duration | Ticks deferred |
|---|---|---|---|
| One sector read | 1 | ~0.5ms | 0 (under one tick) |
| `f_open` on the root directory | a few | 1--2ms | 1--2 |
| `EXEC_EXISTS` probe | a few | 1--2ms | 1--2 |
| `k_fs_read` of a 64KB file | 128 | ~64ms | ~47 |

The first three are imperceptible. The last one is the case to watch:
`k_fs_read` (`sw/os/fsapi.c`) reads a whole file in a single syscall,
bounded only by the caller's `maxlen`. During one, `wm` will not
redraw and `net` cannot drain the RMII RX buffer -- only 4 slots on
Sergei -- so sustained traffic concurrent with a large read could drop
packets.

The chunked API (`FS_OPEN_READ` / `FS_READ_CHUNK` / `FS_CLOSE`) exists
precisely so callers can bound this, and is the better choice for
anything large. If single-syscall reads become a problem in practice,
the fix is to have `k_fs_read` loop over sectors and release the
deferral between them, but that is worth measuring before building.

## Why not FatFs re-entrancy

`FF_FS_REENTRANT 1` is the conventional answer and it is **not** used
here. Two reasons, one of which is specific to this codebase.

### It would not be sufficient

FatFs's own documentation, quoted in `ffconf.h`, is explicit that
`FF_FS_REENTRANT` guards file and directory access to the same volume
and that **`f_mount()` and `f_mkfs()` are never re-entrant regardless**.
More importantly, its scope stops at the FatFs layer. The SPI
transaction underneath -- CS asserted, command in flight, 512 bytes
being clocked through the shifter in `sdmm.c` -- is outside anything
FatFs knows about. Protecting the window buffer while leaving the
transaction interruptible fixes half the bug.

### The timeouts would become scheduling-dependent

This is the decisive one. A mutex, whether FatFs's or our own, permits
the holder to be preempted: process A holds the lock, gets swapped
out mid-transaction, process B tries to acquire and blocks, A resumes
later and finishes. The *card* is fine with this -- SD in SPI mode
tolerates arbitrary gaps between bytes as long as CS stays asserted
and nobody else drives the bus.

The **driver** is not. `dly_us()` in `sdmm.c` measures with `rdcycle`:

```c
__asm__ volatile ("rdcycle %0" : "=r"(start));
target = (Z_SYSCLK_HZ / 1000000u) * (uint32_t)n;
```

`rdcycle` is a free-running cycle counter. It keeps advancing while
the process is descheduled. So a preempted holder's `wait_ready()`
500ms budget burns while *other processes* run, and the driver can
time out because of scheduling rather than because of the card. Every
timeout in `sdmm.c` -- `wait_ready()`, `rcvr_datablock()`,
`disk_initialize()`'s ACMD41 loop -- would silently become dependent
on system load. That is a miserable class of bug to chase.

Preempt-deferral sidesteps it entirely: the operation runs to
completion uninterrupted, so every cycle-counted timeout keeps meaning
what it says.

### What enabling it would actually involve

For the record, if the trade ever looks different:

1. **Set `FF_FS_REENTRANT 1`** in `sw/os/fs/fatfs/ffconf.h`, choose an
   `FF_SYNC_t` (currently the stub value `HANDLE`), and set
   `FF_FS_TIMEOUT`, which is in ticks.

2. **Implement four sync handlers** that FatFs will call and that do
   not currently exist in this tree:
   `ff_cre_syncobj()`, `ff_del_syncobj()`, `ff_req_grant()`,
   `ff_rel_grant()`. Samples ship in FatFs's `option/syscall.c`.

3. **Build a blocking mutex in the kernel**, which Zeitlos does not
   have. `Z_PROC_FLAG_BLOCKED` and `k_proc_unblock()` (`kernel.c`) are
   the primitives to build on: `ff_req_grant()` would mark the caller
   blocked and yield, `ff_rel_grant()` would unblock the next waiter.
   Priority and fairness policy would have to be decided; there is
   currently no wait queue of any kind.

4. **Make the SPI layer safe independently**, since FatFs's lock does
   not cover it. Either give `sdmm.c` its own guard, or keep a narrow
   preempt-deferral around just the transaction.

5. **Rewrite `dly_us()` so its timeouts survive preemption.** It must
   stop measuring elapsed *cycles* and start measuring elapsed
   *scheduled time for this process*, which means per-process cycle
   accounting the kernel does not currently keep. Without this, step 3
   makes the driver flaky.

Steps 3 and 5 are the real work, and step 5 in particular is a change
to the kernel's accounting model. The current approach costs about
forty lines and one `switch`.

## Adding filesystem code

**A new syscall whose handler reaches FatFs**: add its `Z_SYS_*` id to
`k_syscall_touches_fs()` in `sw/os/kernel.c`.

**A new public function in `sw/os/fs/fs.c` that calls `f_*`**: bracket
it with `k_fs_enter()` / `k_fs_leave()`, and check every early return.

**New kernel code that calls FatFs some other way**: bracket it by
hand. `fsapi.c`'s chunked handlers call `f_read`/`f_write` directly
rather than through `fs.c`, and are covered only because the
dispatcher guards their syscall ids.

Nothing will warn you if you forget any of these. The failure mode is
an intermittently corrupted card under load, which is the bug this
document exists to describe.

One deliberate safety property: because a leaked counter is caught by
`K_NO_PREEMPT_MAX_TICKS`, forgetting a `k_fs_leave()` degrades to
"protection stops working after ~87ms" rather than to a hung machine.
That is a soft failure, not an excuse -- it will corrupt cards.

## Debugging this class of bug

The instrumentation that found it has been removed, but it is worth
knowing what to re-add, because none of these values are printed by
default and all three were decisive.

| Where | Print | Reads as |
|---|---|---|
| `fs_exec_info()`, `fs.c` | the `FRESULT` from `f_open`, with the filename | `0x1` `FR_DISK_ERR`, `0x2` `FR_INT_ERR`, `0x3` `FR_NOT_READY`, `0x4` `FR_NO_FILE` |
| boot path, `sh.c` | `fs_mount_now()` result and `disk_status(0)` | `disk_status` bit 0 is `STA_NOINIT` |
| `disk_initialize()`, `sdmm.c` | `CardType` and resulting `Stat` | `CardType` 0 means the card never answered |

`fs_exec_info()` discards its `FRESULT` in normal operation, which is
why "the app is missing" and "the card is broken" looked identical
from the boot log for a long time. Printing it is the single most
useful thing to do first.

Use `kprint()` rather than `printf()` for any of these: raw UART, no
libc, no buffering, no heap -- see the note in `sw/os/pidreg.c` about
`snprintf()` hanging in kernel-compiled code. It also cannot perturb
what it is measuring.

Two signals worth recognising in a boot log:

- **Interleaved, garbled output** means two processes are inside the
  kernel at once. That is what pointed at this bug in the first place.
- **`disk_initialize()` running late.** It fires on the first *real*
  card access, not at `f_mount()` time (a deferred mount touches no
  hardware), so where it appears says when the card actually came up
  relative to everything else.

## See also

- `sw/os/kernel.c` -- the counter, the classifier, and the scheduler check
- `sw/os/fsapi.h` -- why syscall handlers may dereference caller pointers directly, and why a syscall is an ordinary function call
- `sw/os/fs/fatfs/sdmm.c` -- the hardware SPI backend and its timeouts
- `sw/common/zfs.h` -- the chunked I/O API and why the handle table exists
- `docs/boot.md` -- boot sequence and where the card is brought up
- `docs/flash_apps.md` -- the flash archive that core apps resolve
  from, and the root/`apps/`/flash search path used to find them
