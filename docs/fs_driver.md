# The filesystem as a driver

`fs` (`sw/apps/fs`) is the filesystem. FatFs runs in this process and
nowhere else. Other processes are its clients, by message, and the
kernel is one of them. This is Phase 2 of
[userspace_drivers.md](userspace_drivers.md).

**Status (October 2026):** built and running on lakritz: the card,
`/ram` and `/tmp`, the flash underlay, USB sticks at `/usb`, executables
from the card; an NFS export at `/nfs` (read-only, through `net`,
written October 2026 and awaiting its first test against a real
server). It is restarted after a crash with `/ram` kept, and
libc's `open()`/`fopen()` reach it from apps built with `POSIX_IO = 1`
([posix_io.md](posix_io.md)). Limits are under "Known limits".

## What `fs` serves

| path | volume | how `fs` reaches it |
|---|---|---|
| `/` | the SD card (FatFs drive 0) | directly: `fatfs/sdmm.c`, the card's SPI driver, is linked into `fs` (a driver may use the SD nibble, [mpu.md](mpu.md)) |
| `/ram`, `/tmp` | the RAM disk (drive 1) | kernel memory, by sector, through `Z_SYS_BLK` |
| `/usb` | USB mass storage (drive 2) | the `usb` driver's drive, by request (`zusbc.c`, `Z_USBOP_MSC_*`; [usb_driver.md](usb_driver.md)) |
| `/nfs` | an NFS export, read-only, while one is mounted | by request to `net`, which speaks NFSv3 to the server (`znfsproto.h`; [nfs.md](nfs.md)) |
| underneath `/` | the flash archive's files | read-only, through `Z_SYS_ZAR` |

The rules are the ones the kernel's filesystem had (and
[filesystem.md](filesystem.md), [layout.md](layout.md) describe):

- **Paths.** Paths are case-insensitive, as FAT is. `/ram` and `/usb`
  are reserved names at the root. `/tmp` is the RAM disk when there
  is one; otherwise it is a card directory, emptied when `fs` starts.
- **Listings.** Listing `/` shows the live volumes (`ram`, `tmp`, and
  `usb` while a drive is mounted), then the card's entries, then
  whatever is only in flash.
- **The flash underlay.** A card file shadows its flash copy. A
  flash-only file opens for reading and refuses writes (`-Z_EROFS`).
  `Z_O_NOFLASH` asks for the card's copy only; the kernel uses it to
  decide where a program comes from.
- **Card layout.** `fs` makes `/sys`, `/home` and `/data` whenever it
  mounts a card.
- **The RAM disk** is formatted every time `fs` starts. Its memory is
  the kernel's, sized at boot from the machine's RAM (none on a 1MB
  board), so it does not depend on `fs`'s executable header.
- **The card** is tried once at start, because a board with no card
  must not wait for one. After that it is retried, at most once a
  second, by any request that needs it, which covers a slow or late
  card.
- **USB.** `/usb` follows the drive. `usb` tells `fs` when one comes
  or goes (`Z_USB_EVENT`), and `fs` also checks once a second when
  idle. Bringing a new drive to ready can take seconds, and `fs`
  answers nobody meanwhile.
- **Rename.** Renaming a file that is open for writing is refused
  (`-Z_EBUSY`), as the kernel refused it. So is renaming across
  volumes, a root, or a directory into itself.

## Who talks to it

```
 apps ── zfsapp fs_*() ──> Z_SYS_FS_* syscall ──> kernel: kfs.c / fsapi.c ──┐
 apps built with ZFSAPP_DIRECT (ipcbench) ── zfsc.c ────────────────────────┤
 kernel shell (pid 0), program loading, cfg ── kfs.c ──────────────────────┤
                                                                           v
                                                        fs  (Z_FS_REQ / Z_FS_REPLY)
```

- **The kernel is a client.** `sw/os/kfs.c` uses the same client code
  as the apps, `sw/common/zfsc.c`, built with `-DZFSC_KERNEL`. A
  request is made by whichever process is running when it is needed:
  - pid 0, for the shell's `ls`, `xf`, `tget`, `format` and the rest;
  - an app inside its own syscall, for `z_proc_run()`, wm's dock
    check, `CFG_RELOAD`, and the old `Z_SYS_FS_*` calls.

  That process waits, blocked, for the reply, exactly as it would in
  `z_msg_recv()`. The proposal assumed a syscall could never wait
  ("the kernel never waits on userspace"). It can, and has since
  `z_msg_recv()`: the yield's interrupt is taken inside the syscall,
  and the process resumes there. What the kernel must never do is
  wait **with preemption held**. So the filesystem syscalls are no
  longer in `k_syscall_touches_fs()`, and cfg reads its file before
  it takes the lock.
- **Apps use the kernel path by default.** zfsapp's `fs_*()` still
  make the `Z_SYS_FS_*` syscalls, and the kernel forwards each one to
  `fs` (`sw/os/fsapi.c`). This costs an app no flash, and the flash
  archive is full. zcc's runtime and older binaries keep working
  unchanged.
- **Apps can go direct.** An app that links `zfsc.o` and is built with
  `-DZFSAPP_DIRECT` sends its requests to `fs` itself, one hop shorter.
  On gateware with the grant window its reads land in its buffer with
  no copy. `ipcbench` does this.
- **Handles are the same either way:** `fs`'s descriptor plus
  `0x4000`.
- **Small writes are gathered.** A program that writes in pieces (TFTP
  blocks, `stdio`, a log line at a time) used to pay a round trip to
  `fs` -- and a card write -- per piece. Both clients now keep a write
  buffer of 8KB per program (write-behind): zfsapp for the kernel path,
  zfsc for the direct path and the kernel's own pid 0 (`tget`, `xf`).
  - The **first** write to a descriptor always goes straight to `fs`,
    so a read-only file or a full disk fails at once, as before. Later
    writes smaller than the buffer are gathered and sent as one request
    when the buffer fills.
  - Any other call -- a read, `lseek`, `fstat`, `close`, `fsync`, any
    path call -- sends the buffer first, so a program always sees its
    own writes. Another program sees them once the writer closes or
    `fsync`s, which was already true of FatFs.
  - A gathered write that then fails is reported by the next write,
    `fsync` or `close` of that descriptor. Exit flushes too (`atexit`).
  - The buffer is `malloc`ed on first use, so a program that never
    writes in pieces pays nothing but code. cron is built without it
    (`-DZFSAPP_WBUF=0`): its malloc would cost ~5KB of flash. The
    kernel's is static, in bss, and used only by pid 0 -- apps calling
    the kernel's syscalls are gathered on their own side already.
  - `zfs_flush()` (zfsc.h) sends the buffer by hand.

## Booting

`fs` is a core app in the flash archive (`CORE_APPS`, first in the
list). The kernel shell starts it before anything else (`boot_fs()`
in `sw/os/sh.c`) and waits up to 3 seconds for it to register as
`fs0`. `fs` registers after it has formatted the RAM disk and tried
the card. Then boot continues:

- `/sys/zeitlos.cfg` is read, through `fs`.
- `usb` starts, from the card if `/apps/usb` is there, else from flash
  ([usb_driver.md](usb_driver.md)).
- `init()` runs. It starts wm, net and the rest; card copies in
  `/apps` still win over flash.
- The console lock comes last.

Since `fs` is pid 1, nothing relies on wm being pid 1 any more. `init`
remembers that it has run in a flag.

**`fs` itself always comes from flash**, because nothing can read the
card before it runs. Trying a new `fs.bin` means `make flash_apps`.
Copying it to `/apps/fs` on the card has no effect, and `init` does
not report it as an override.

**Without `fs` there is no filesystem.** Without it in flash, or if it
fails to start, the kernel still starts the core apps from flash, and
`ls`, `xf` and the rest say there is no filesystem.

## The protocol

`sw/common/zfsproto.h`. POSIX-shaped: descriptors, `open` flags,
`lseek` with whence, `stat`, `opendir`/`readdir`, plus volume
operations for the kernel shell (`MKFS`, `MOUNT`, `UMOUNT`,
`SYNCALL`).

- **Writes are never copied by the protocol.** `fs` reads the
  client's buffer where it is: the kernel resolved the blob to
  physical memory, and any process may load from anywhere. FatFs
  passes a whole-sector write straight down, so a `/ram` write can
  reach `Z_SYS_BLK` with the client's physical address. The kernel
  accepts any main-memory source for a WRITE, since it only reads it;
  until round 9b it demanded fs's own memory, and such a write failed
  (`tget` to `/ram`).
- **Reads land in the caller's buffer through the MPU grant window**
  when the gateware has one (MPU version 2) and the client allows it.
  Otherwise they come back in the reply, and the client copies them.
  The kernel's client always takes the copy path, because what it
  reads into is often kernel memory or a new process's.
- **Up to 32KB per request** (`Z_FS_CHUNK`). Every request pays a
  round trip, so requests are big; another client's request is served
  between two of them.
- **The reply is borrowed.** It lives in `fs`'s memory, and the client
  copies it before sending anything else. Small replies sit in a 2KB
  slot per client (6 slots, least recently used reused first). Read
  data by copy goes in one 32KB bulk buffer. That buffer is allocated
  on first use, so a board with the grant window never has it, and it
  is held for its client until that client's next request, its death,
  or a second. A second reader meanwhile gets at most 2KB per request
  from its own slot.
- **Merged requests.** `OPEN` returns the file's size. `OPENDIR`
  returns the first page of entries, and `fs` closes a directory that
  has ended (`Z_FSREP_END`). A small listing is one request, and a
  whole-file read is open, reads, close. A client may ask for smaller
  pages; the kernel uses 512 bytes, which fit on any stack it runs
  on.
- **Each client waits only for its own reply** (`z_msg_recv_match()`).
  Other mail stays queued in order. The timeout is 15 s, and 120 s for
  a format or the first free-space count on a large card.

## Memory and flash

| | flash | RAM |
|---|---:|---:|
| `fs` (code and data) | 42,364 | + 30,052 `.bss` = 72,400 image, 64KB stack/heap |
| kernel image, before and after Phase 2b | 256,380 -> 223,852 | the same, its `.bss` included |

- **fs's `.bss`:** 6 reply slots (12.5KB), 16 file descriptors, each
  with FatFs's 512-byte sector buffer and a 128-byte cluster map
  (11KB), the three volumes, and the rename probe.
- **No printf in fs.** The few lines `fs` prints go through `say()`.
  The printf family, with the 64-bit division it brings, was 12KB of
  `fs`, in an archive with no room for it.
- **The kernel boundary moved to 224KB.** The kernel's flash region
  went from 256KB to 224KB (`0x38000`) for `fs`, and the archive grew
  to 784KB. Phase 3 moved it again, to 160KB (`0x28000`), with the
  archive at 848KB ([usb_driver.md](usb_driver.md)); the budgets are
  in [flash_apps.md](flash_apps.md).
- **Old BIOS.** `ROM_OS_SIZE` is 160KB in `sw/bios/bios.c` since
  Phase 3. An older BIOS copies more, which includes the start of the
  archive; those bytes land in the kernel's `.bss`, which crt0 zeroes,
  and past `_end`, which nothing uses yet. Every BIOS version works.

## Inside `fs`

- One request at a time, in arrival order. FatFs is not reentrant, and
  a process serialises its own work for free. Nothing holds the
  scheduler: `fs` is preempted like any process.
- Descriptors belong to the pid that opened them, and are refused
  (`-Z_EBADF`) to anyone else. When a client dies, its descriptors are
  closed by a sweep: every 4 idle seconds, and every 256 requests.
- A read handle gets a cluster map (FatFs fast seek). Without one, a
  backwards seek walks the cluster chain from the start, one card read
  per step.
- Timestamps come from the RTC (`fs_fattime_of()`, `fsvol.h`), or
  2020-01-01 until the clock is set.

## Testing

```
make -C sw/apps/fs/tests
sw/apps/fs/tests/test_fs          # fs + the apps' client + zfsapp
sw/apps/fs/tests/test_fs_kernel   # the same with the kernel's client
sw/apps/fs/tests/test_fsvol       # rename's checks and timestamps
```

`test_fs` builds `fs.c` with the client on the host. The kernel is
replaced by stubs: messages, `Z_SYS_BLK` over a RAM disk, `Z_SYS_ZAR`
over a small archive, the card's driver over another RAM disk, and the
`usb` driver's drive (`zusb_msc_*()`) over a third. It covers:

- **Volumes:** a blank card at start; `/ram` and `/tmp`; formatting
  and mounting; unmount refused with files open; the flash underlay,
  its shadowing and `Z_O_NOFLASH`; the volumes listed at the root;
  `/usb` following a drive that comes, goes and comes back.
- **Files:** both read paths; rename's checks, busy included.
- **Clients:** descriptor ownership and the sweep; two clients reading
  by copy at once; how many requests a listing and a whole-file read
  take.
- **Hygiene:** that a reply is never sent with the window open.

It also covers, since round 10: `zposix.c` over it
([posix_io.md](posix_io.md)), late replies, the reply slots and
`-Z_EAGAIN`, and a restart keeping `/ram`.

It builds with AddressSanitizer and UBSan's alignment check. 3,701
checks; 3,744 in the kernel-client build. `test_fsvol` is the kernel's
old `test_fsrename.c`, moved with the code it tests: 59 checks.

## Known limits

- **One writer per file is not enforced.** `FF_FS_LOCK` is 0, as it
  always was, so two writers on one file are not refused; rename is.
- **A restart of `fs` loses open descriptors.** It is started again
  by itself after a crash (below). Clients see `-Z_ENODEV` or
  `-Z_ETIMEDOUT` for the call that was in flight and for descriptors
  they still hold; the next call finds the new `fs0`.

Fixed in round 10 (they were limits here before):

- **Late replies** -- one that arrives after its client gave up -- no
  longer linger. The client notes the tag and takes the reply, without
  waiting, at its next calls (`zfsc.c`, `stale_drain()`; the same in
  `zusbc.c`).
- **The reply slots.** A slot is given to another client only once its
  own has copied the reply in it: it has sent again, died, or 256
  requests went by. With all six holding unread replies, a seventh
  client is told `-Z_EAGAIN`, and `zfsc.c` waits a tick and asks again.
- **A USB stick no longer pauses `fs`.** `usb` brings a new drive up
  itself, a TEST UNIT READY at a time between its other work, and says
  so once it is ready; `fs` mounts it then. `usbmount` still waits for
  it, by request ([usb_driver.md](usb_driver.md), "Storage").
- **libc's `open()`/`fopen()`** reach files in any app built with
  `POSIX_IO = 1` ([posix_io.md](posix_io.md)).
- **`rmdir`/`unlink` of a directory that is not empty** says
  `-Z_ENOTEMPTY`.

## Restarting

When `fs` crashes, the kernel starts it again from flash a second
later, at most three times a minute (kernel.c, "restarting a driver";
[userspace_drivers.md](userspace_drivers.md), "Restarting drivers").
What survives:

- **`/ram` and `/tmp`.** The RAM disk is the kernel's memory, and the
  new `fs` mounts the FAT it finds there rather than formatting it
  (`ram_start()`: "kept from before"). The kernel zeroes its first
  sector at boot, so a warm reboot still gets a fresh one.
- **The card** is mounted again; **`/usb`** at the next look (each
  second).
- **Not** open descriptors or working directories in `fs`: a client
  holding one gets an error and opens again.

## Measured, Phase 2a (for comparison at the end)

Round 7b on an MPU-v1 board, with `net` then taking ~28% of the CPU
(since fixed): requests to `fs` cost a round trip each, ~1-2ms idle.
During a whole-file read, the longest stall for everyone else fell
from 90ms (the kernel's FatFs) to 5.6ms (`fs`). Bulk reads by copy
cost a second `memcpy`, which the grant window removes. The rest is in
[userspace_drivers.md](userspace_drivers.md), rounds 7-7e. The plan is
to measure again once all phases are done.
