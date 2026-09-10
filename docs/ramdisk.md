# The RAM disk (`/ram`)

A FatFs volume backed by main memory, created at boot and reachable
from every app as `/ram`.

## Why

The SD card is SPI, through the SOC's hardware SPI master
(`reg_spisd_*`, one byte per exchange with a busy poll), and it
shows. Measured on hardware,
`sw/apps/web` spent **13.3 seconds** re-reading a 258KB page off the
card to index it, against 1.4 seconds writing it — roughly 19 KB/s
read. A browser wants scratch space for one page, not durable
storage, and so does anything else that needs to put a few hundred
kilobytes somewhere and read it back.

## What it achieved

Measured on the same page after the change:

| | card | `/ram` |
|---|---|---|
| re-reading 258KB to index it | 13.3 s | **0.43 s** |

**The 13.3s figure was wrong.** Measured on hardware, the card does
239-274 KB/s, not 19 -- about thirteen times better. See
`docs/sdcard.md` for the full results. `/ram` is still faster and is
still the right place for a build directory, but the ratio is roughly
2x, not 30x, and nothing should be designed around the old number.

Original note, kept for the reasoning: It predates the yield/blocking
fixes in `docs/app_runtime.md` (which alone took a `view` JPEG decode
from 7.7s to 3.1s), and 19 KB/s is roughly fifty times slower than the
gateware SPI master and the DIV=1 divider should permit. See
`docs/sdcard.md`, which has the arithmetic, one cause already fixed, and
`sdbench` to measure the rest. The *ratio* to `/ram` may narrow; the
argument for `/ram` does not depend on it.

Thirty times faster, and it moved the bottleneck: indexing a page is
now **12.2 seconds of parsing and 0.4 seconds of reading**, where
before it was 13.3 seconds of reading. `html.c` is what to optimise
next, which was not visible while the card dominated.

The card is still the fallback, and still works.

## Why a disk rather than a buffer

An app-private buffer would have been a smaller change to `web`. But
an app's `malloc()` is bounded by its stack+heap allowance
(`z_proc_stack_size_for()`, `kernel.h`), so a megabyte of it needs
either a new syscall exposing `k_mem_alloc()`, or a "stack size" that
is mostly not stack. **Both are new mechanisms invented for one
caller.**

FatFs already has a block-device seam (`diskio.h`) and multi-volume
support, so a second drive costs one small driver and one line of
configuration — and every app gets it through the file API it already
uses, with **no new API at all**.

## `/ram` is the name to use; `1:` still works

Both reach the same volume.

`fs_path_resolve()` rewrites a known prefix and **passes everything
else through unchanged**, so FatFs's own drive syntax was never
removed — `1:/scratch` reaches the ramdisk exactly as `/ram/scratch`
does. It shows up in the wild: the kernel's delete message prints the
resolved path, so an app that wrote to `/ram/webspool.img` produces

```
deleting '1:/webspool.img' ...
```

which is the same file under its FatFs name, not a second one.

**`/ram` is the form to write.** It keeps one namespace: `ls /ram`
works like `ls` anywhere else, the file browser walks into it, and no
app learns new syntax. Drive prefixes would otherwise leak into every
path string in the tree, every listing, every file dialog, and these
docs.

The cost is a translation step in `fs.c` and a **reserved name at the
root** — a directory called `/ram` on the card is unreachable. A real
wart, accepted deliberately.

It is a small mount table rather than a hardcoded comparison, because
flash-as-a-volume is the obvious next one and app-facing syntax
should not have to change again for it:

```c
static const struct { const char *prefix, *vol; } fs_mounts[] = {
    { "/ram", "1:" },
};
```

## Two places resolve paths, not one

`fs.c`'s own functions resolve `/ram` themselves. **`sw/os/fsapi.c`
has to do it explicitly**, because the app-facing syscalls call
`f_open()` and `f_opendir()` directly rather than going through
`fs.c` — so a resolver private to `fs.c` never ran for anything an
application did. `fs_path_resolve()` is public for that reason, and
every path entering FatFs must pass through it wherever it enters
from.

Matching is **case-insensitive**. `sh.c` passes paths uppercased,
FAT-style, so `/RAM` and `/ram` are the same place; a case-sensitive
compare silently routed one of them to the SD card, where `ls /RAM`
reported `error 5` (`FR_NO_PATH`) because there is no `RAM` directory
on the card.

## Sizing

A **share** of main memory, not a fixed size — scratch space should be
proportional to the machine, and a figure that suits a 32MB board is
most of a 1MB one.

| | |
|---|---|
| `Z_RAMDISK_DIVISOR` | 8 — one eighth of main memory |
| `Z_RAMDISK_MAX` | 4MB — a cap, so a very large board does not reserve tens of megabytes nothing asked for |
| below 1MB | not created at all |

At 1MB the pool is the whole of memory and every megabyte is already
spoken for; an app wanting scratch there should use the card, which
is what it had to do anyway.

Created in `kernel.c` before any process starts, so an app can rely on
it being there or not rather than on when it happened to look.

## Where it is created, and why that matters

In `kernel.c`, **after `k_proc_create()` has reserved process zero** —
not next to `k_mem_init()`, where it obviously belongs.

The kernel runs from `Z_MEM_BASE` (`0x40000000`) and the pool starts
there too. Nothing tells `k_mem_alloc()` that the bottom of it is
occupied by the running kernel until process zero is created. Called
before that, `ramdisk_init()` is handed the kernel's own image and
stack, and `f_mkfs()` writes a FAT over it.

The first version did exactly that. The symptom was a hang
immediately after `- memory initialized.`, which is indistinguishable
from `k_mem_init()` itself having failed.

`ramdisk_init()` now refuses any allocation below the kernel's `_end`
and says so, so that getting the ordering wrong again produces a
message rather than a dead board.

## What it is not

- **Not persistent.** Contents are gone at reset, and the volume is
  reformatted whenever it is created. A volume that survived a
  crashed app would accumulate files nobody owns.
- **Not reserved.** The space comes from the same pool as everything
  else. A large ramdisk on a small board is a bad trade, which is why
  there is a cap and a floor.
- **Not arbitrated.** Two apps sharing it can starve each other.
  Callers must cope with it being absent or full rather than assume
  room — there is deliberately no eviction policy, because a scratch
  disk with one is no longer simple enough to be worth having.

## Using it

Nothing new. The ordinary file calls take `/ram/...` paths:

```c
int fd = fs_open_write("/ram/scratch");
```

`sw/apps/web` **prefers** it and falls back to the card, rather than
requiring it — so `web` still runs where there is no ramdisk, and the
two are directly comparable: same build, same page, one variable. It
reports which it chose:

```
web: spool: /ram/webspool
```

## Files

| | |
|---|---|
| `sw/os/fs/ramdisk.c/.h` | the block device |
| `sw/os/fs/fatfs/diskio_mux.c` | drive dispatch: 0 = SD, 1 = RAM |
| `sw/os/fs/fatfs/sdmm.c` | entry points renamed `sd_disk_*` |
| `sw/os/fs/fs.c` | mount table, `/ram` routing, `fs_ramdisk_create()` |
| `sw/os/fsapi.c` | resolves paths for the app-facing syscalls |
| `sw/os/mem.h` | `Z_RAMDISK_DIVISOR`, `Z_RAMDISK_MAX` |
| `sw/os/kernel.c` | creation at boot |

`sdmm.c` was renamed rather than extended so the vendored driver stays
recognisably the upstream sample, which is worth something the next
time it is updated.
