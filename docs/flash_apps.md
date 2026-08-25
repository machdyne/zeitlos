# Core Apps in Flash

The core apps -- `wm`, `net`, `repl` and `term` -- are programmed into
flash alongside the kernel and are available with no sdcard attached.

## Why

Two problems, and the second is the bigger one.

**Getting started.** A freshly flashed board used to boot to a serial
shell and nothing else, because every app lived on the sdcard. "Flash
the board and you have a desktop" is a far better first five minutes
than "flash the board, now go write an sdcard".

**Iterating.** Updating the core apps meant hand-driving `xf` in minicom
four times, about a minute of interactive work per cycle, with the
terminal tied up throughout. `make dev-flash` is now unattended and
takes seconds.

## Why flash is a good place for this

Flash is memory-mapped on this SOC, which is what makes the whole thing
cheap: loading an app from it is a `memcpy`, with no filesystem and no
SPI driver involved. It is also **faster than the sdcard**, which is
bit-banged SPI (`sw/os/fs/fatfs/sdmm.c`).

This is the third use of that same property. The BIOS already loads the
kernel this way (`load_zeitlos()`, `sw/bios/bios.c`), and `sw/os/logo.c`
reads the boot splash straight out of flash into VRAM so it costs no
main memory at all.

Writing flash is slow, but that happens once per build, unattended, as
part of `make flash`.

## Layout

```
0x10000000   MEM_ROM base
0x100F0000   boot splash
0x10100000   kernel, 256KB
0x10140000   core apps  <-- this
```

The archive format is deliberately minimal:

```
offset  size  field
0       4     magic     "ZAR1"
4       4     count     number of entries
8       8     reserved  must be 0
16      ...   entries[count], 24 bytes each:
                0   16  name (NUL-padded)
                16  4   offset  from start of archive
                20  4   size    bytes, the whole ZEXE file
...           the ZEXE files themselves, in entry order
```

The stored files are ZEXE (`sw/common/zexec.h`) **verbatim**, exactly as
each app's own Makefile produced them. `tools/mkzar.py` concatenates
them without re-encoding, so there is one executable format to keep
working rather than two.

`Z_ZAR_FLASH_OFFSET` in `sw/os/zar.h` and the offset in `Makefile`'s
`flash_apps` target must agree. Nothing checks that they do, and a
mismatch presents as "no core apps in flash" rather than an error.

## An underlay, not a filesystem

The resolution rule is one line:

> if the filesystem has it, use that; otherwise use the flash copy.

`fs_exec_info_any()` and `fs_load_exec_any()` (`sw/os/fs/fs.c`) are the
single place that decides. Every path that starts a process goes
through them: `sh.c`'s `run`, `sh.c`'s `init`, and `k_proc_run()` --
which is what wm's dock calls, and what lets `term` launch on a
card-less board.

There is still exactly one name for `term`. `run term` behaves
identically whether it came from a card, from flash, with no card at
all, or after being killed and restarted.

### Why not drive letters

Drive letters (`A:` = flash, `B:` = sdcard) were considered and
rejected. They are a *namespace* solution to what is actually a
*fallback* question, and the cost lands everywhere:

- Every path-taking API would have to learn about drives:
  `fs_open`/`size`/`read`/`write`, `ls`, `te`, repl's Scheme file API,
  `tget`/`tput`.
- Flash is read-only, so writes to `A:` need a new failure path in each
  of them.
- Worst, callers like wm's dock would have to know *which drive* an app
  lives on -- precisely the thing they should not have to care about.
  Adding default-drive rules to avoid that just reintroduces the
  ambiguity drives were meant to remove.

If explicit selection is ever genuinely needed, a `flash:term` prefix
handled inside `fs_exec_info_any()` is a much smaller change than
teaching the whole filesystem API about drives. That would become more
attractive if Zeitlos ever grows a second real storage device (USB mass
storage, a network mount), which is when drives start earning their
keep.

### Shadowing

A file on the card wins. That is deliberate and needs no version scheme,
timestamp comparison or precedence rules: the only way an app got onto
the card is somebody deliberately putting it there, so treating that as
intent is exactly right. It keeps `xf wm` working as a single-app
hot-swap during development.

To make that visible rather than mysterious:

- `init` prints each app's source at boot (`init: wm (flash)`).
- `run` prints it too (`loading term from flash`).
- `ls` lists flash apps in a separate `in flash:` section, **skipping
  any shadowed by a real file**, so what it shows matches what `run`
  would actually launch.

## Booting with no card

This is a first-class path, not a fallback that happens to work.

`sh.c`'s auto-init used to poll for up to `AUTOINIT_TIMEOUT_TICKS`
(~3s) waiting for a slow sdcard to become readable. On a board with no
card that answer will never change, so the flash case is checked first
and skips the poll entirely -- otherwise a card-less board would stall
for three seconds on every boot before the desktop appeared.

That check only decides *when* to call `init()`. `init()` still resolves
per app, so a card holding only `wm` still gets its `wm` from the card
and everything else from flash.

## Adding or removing a core app

1. Add or remove it in `Makefile`'s `output/$(BOARD_LC)/apps.zar` rule.
2. Rebuild and reflash: `make BOARD=<board> flash_apps`.

The kernel accepts up to `Z_ZAR_MAX_ENTRIES` (32) entries. There is no
requirement that a core app also appear in wm's dock -- the dock should
only offer apps guaranteed to resolve, which in practice means core apps
(`gpu3d` was removed from it for exactly this reason: it only ever
exists on a card, so its icon was a dead button on most machines).

Note apps must emit ZEXE, not a raw `objcopy` binary. `mkzar.py` warns
if one does not; such a file still loads, but ships its `.bss` as
literal zeros and wastes flash.

## Development cycle

```
$ make clean && make BOARD=obst dev-flash
```

`dev-flash` rebuilds and reflashes the kernel and core apps without
touching the bitstream. Both are flashed together deliberately:
`sw/common/syscalls.def` is compiled into `kernel.bin` *and* into every
app, and a binary built against a different copy calls the wrong kernel
handler for every syscall past the point they diverge -- which is not a
crash, just quietly wrong behaviour. See that file's own warning.

If anything under `rtl/` changed, use `make flash` instead.
