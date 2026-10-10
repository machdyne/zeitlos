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

### Volumes

The card is not the only FatFs volume any more.
`sw/os/fs/fatfs/diskio_mux.c` dispatches by physical drive:

| drive | path | backend |
|---|---|---|
| 0 | `/` | SD card, `sw/os/fs/fatfs/sdmm.c` |
| 1 | `/ram` | ramdisk, `sw/os/ramdisk.c` |
| 2 | `/usb` | USB mass storage, `sw/os/usb/usbh_msc.c` |

`/ram` and `/usb` are prefixes `fs.c` rewrites into volume ids, not
directories on the card. **`/usb` mounts itself when a drive is plugged
in and is released when it is pulled.** The USB driver notices both in
an interrupt, where mounting cannot happen (it waits for the unit to
report ready), so it asks for deferred kernel work
(`k_deferred_request()`, `sw/os/uart.h`): pid 0 runs `fs_usb_poll()`
while the kernel shell waits for input, waking for it the way it wakes
for a keystroke. If pid 0 is busy with a command, it happens at the next
prompt. The slow start-up (TEST UNIT READY, geometry) runs outside the
filesystem guard, since nothing else can be using a drive that is not
mounted yet; `f_mount()` itself runs inside it.

The shell's `usbmount` and `usbunmount` remain. `usbunmount` is the safe
way to pull a stick -- it releases `/usb` and keeps it released until
that drive is removed, so the next poll does not mount it again --
and `usbmount` mounts it again. If a mount fails, it waits up to about
1.5 s for the drive to be present again and tries once more -- behind a
hub the first attempt occasionally met a drive the hub had just dropped
and was re-enumerating (docs/usb_host.md, "Known issues").

Everything in this document applies to all three: FatFs is one
non-reentrant instance whichever volume a call touches, and the USB
backend blocks on bus transfers just as `sdmm.c` blocks on SPI. `/usb` is specific in one way: the
drive can leave. Pulling it makes the disk layer report "not ready"
and `/usb` drop out of listings; plugging one back in, FatFs re-reads
the medium on next access, and handles opened on the old drive fail
rather than touch the new one. Details in `docs/usb_host.md`, "Removal
while mounted".

One more interaction with this document's subject: a USB storage
command holds the USB transaction engine from start to finish, so
while FatFs is on `/usb`, enumeration of a newly plugged device pauses
until the command is done. See "The transaction engine has one owner"
in `docs/usb_host.md`.

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

  (This transcript predates the `/apps` search path. An equivalent
  trace today shows one `FR_NO_FILE` per app for `/apps/<name>`
  before the flash archive answers. Same meaning. See
  `docs/flash_apps.md`.)
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

`k_syscall_touches_fs()` lists them. `PROC_RUN` is not in the list.
Loading a program is not one FatFs call: `fs_load_exec()` brackets
each open, seek, 1KB read and close on its own, and lets the
scheduler run between them. A whole program is hundreds of
milliseconds, far past the cap below. The dispatcher's hold is timed
from the moment the counter leaves zero, and a nested enter does not
restart that clock, so wrapping `PROC_RUN` would let the cap fire
inside a read. The card is then mid-command, and the next user gets
`FR_DISK_ERR` until a manual `mount`. Between chunks chip-select is
up, so another process using the card is ordinary single-threaded
FatFs use.

```
FS_OPEN_WRITE   FS_MKDIR
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
ever hit it. A whole program load used to, while `PROC_RUN` held the
guard for the transfer; that syscall is no longer in the list above.

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

## Rename, stat and the extended listing

Three syscalls, appended to `sw/common/syscalls.def` so every earlier
id is where it was: an app built before them runs unchanged on a kernel
that has them. The argument structs and the rules are in
`sw/common/zfs.h`; the kernel side is `fs_rename()`/`fs_stat_info()` in
`sw/os/fs/fs.c` and the handlers in `sw/os/fsapi.c`; apps call
`zfsapp.h`.

| Syscall | `zfsapp.h` | |
|---|---|---|
| `FS_RENAME` | `fs_rename(from, to, &err)` | rename or move within one volume |
| `FS_STAT` | `fs_stat(path, &info)` | size, date and attributes of one path |
| `FS_LIST_EX` | `fs_list_ex(...)` | `FS_LIST` plus a `z_fs_info_t` per entry |

Built from those, in `sw/common/zfsutil.c` (link `zfsutil.o` as well
as `zfsapp.o`):

| | |
|---|---|
| `fs_copy_file(from, to)` | streams a copy; removes a partial one on failure |
| `fs_move(from, to, &err)` | rename, or copy then unlink across volumes |
| `fs_strerror(err)` | "destination exists", and so on |

`sw/apps/posix`'s `mv` and `ls -l` and the `files` app are the
callers ([posix.md](posix.md), [file_browser.md](file_browser.md)).
The kernel shell has `mv` too.

### What a rename refuses, and why

`fs_rename()` returns a `Z_FS_ERR_*` code. The ones that are not plain
FatFs results are each there because FatFs would otherwise do
something wrong without saying so.

**Different volumes: `Z_FS_ERR_XDEV`.** `/ram` and `/usb` are FatFs
drives 1 and 2 behind a path rewrite (`fs_path_resolve()`), and
`f_rename()` *ignores* any drive in the new name: it renames within
the old path's drive. So `/ram/x` renamed to `/x` would become `/x`
on the RAM disk, not on the card. The kernel resolves both paths and
refuses if the drives differ; `fs_move()` takes that as its cue to
copy and delete.

**Open for writing: `Z_FS_ERR_BUSY`.** FatFs is built without file
locking (`FF_FS_LOCK 0`). A write handle remembers the sector and the
position of its file's directory entry, so that closing it can record
the final size there. `f_rename()` writes a new entry and deletes the
old one, so a close after a rename writes into a deleted entry: the
file is left with size 0 and its data unreachable.
`sw/os/tests/test_fsrename.c` demonstrates exactly that, on the
project's own FatFs.

The check is exact rather than by name. `k_fs_write_open()` opens the
file read-only, which fills in the same two fields, and compares them
with every open write handle. The same entry is the same file, so a
long name and its 8.3 alias are caught alike. The probe borrows a free
slot of the kernel's handle table rather than keeping a `FIL` of its
own (a `FIL` carries a 512-byte buffer, and static storage counts
against the [kernel's 256KB image](kernel.md#the-256kb-image-budget));
with every slot in use it answers "busy".

Read handles are never a problem (they write nothing back), and nor is
renaming a directory with files open inside it: their entries live in
the directory's own clusters, which do not move.

**Into itself: `Z_FS_ERR_INVAL`.** A directory cannot be moved to a
path at or below itself; nor can a volume root be renamed.

### The extended listing

`FS_LIST_EX` is `FS_LIST` with an optional `info` array filled in
listing order, exactly as `types` is. It is a separate syscall, not a
new field on `z_fs_list_args_t`: an app built before the field existed
passes a smaller struct, and the kernel would read past it.

It exists because the alternative is slow. `f_stat()` finds its entry
by scanning the directory from the start, so stat-ing every entry of
an n-entry directory reads O(n²) directory sectors off the card. The
listing already holds each entry's `FILINFO` as it passes.

The synthetic `/ram` and `/usb` entries in a listing of `/`, and volume
roots, have no date: `fdate` is 0.

## Timestamps

FatFs used to be built with `FF_FS_NORTC 1`, which stamps every file
with the same fixed date (2020-01-01). It is now `0`, and
`get_fattime()` in `sw/os/fs/fs.c` reads the RTC ([rtc.md](rtc.md)).

- **UTC.** File times are the RTC's time, like every other time
  comparison in the system; `system.rtc.timezone` changes only what is
  shown ([config.md](config.md)). `z_fs_info_time()` (`zfs.h`) turns a
  FAT date and time into Unix seconds.
- **Until the clock is set**, and on a bitstream without the RTC,
  files get the old fixed date. It is neither 1970, which FAT cannot
  represent, nor a time counted from power-on.
- **Two-second resolution**, as FAT stores them.
- **2100 is not a leap year**, and the RTC's seconds do reach it (they
  run out in 2106). The conversion (`fs_fattime_of()`, `fs/fs.h`) is
  checked against `z_fs_info_time()` for every day from 1980 to 2106 by
  `sw/os/tests/test_fsrename.c`, which is how that case was caught.

### Testing

```
cc -std=gnu99 -Wall -I sw/os -I sw/os/fs -I sw/common \
   -o /tmp/t sw/os/tests/test_fsrename.c \
   sw/os/fs/fatfs/ff.c sw/os/fs/fatfs/ffunicode.c && /tmp/t
```

It runs the project's FatFs on three RAM disks standing in for the
card, `/ram` and `/usb`, and checks: that renaming a file open for
writing does lose it (the premise of `BUSY`); that the probe matches
the write handle under the long name and the alias and matches nothing
else; that `f_rename()` really does ignore the drive (the premise of
`XDEV`); the timestamp round trip; and the path helpers' edge cases.

## See also

- `sw/os/kernel.c` -- the counter, the classifier, and the scheduler check
- `sw/os/fsapi.h` -- why syscall handlers may dereference caller pointers directly, and why a syscall is an ordinary function call
- `sw/os/fs/fatfs/sdmm.c` -- the hardware SPI backend and its timeouts
- `sw/os/fs/fatfs/diskio_mux.c` -- drive dispatch for card, ramdisk and USB
- `docs/usb_host.md` -- the USB mass storage backend behind `/usb`
- `sw/common/zfs.h` -- the chunked I/O API and why the handle table exists
- `docs/boot.md` -- boot sequence and where the card is brought up
- `docs/flash_apps.md` -- the flash archive that core apps resolve
  from, and the root/`apps/`/flash search path used to find them
