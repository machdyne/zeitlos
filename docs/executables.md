# The Zeitlos executable format

## What it replaces

App binaries used to be a raw `objcopy -O binary --pad-to=_end` dump:
the loadable image, followed by `.bss` written out as literal zeros.

That worked, and for a non-obvious reason: **nothing zeroes `.bss` at
startup on this OS.** There is no crt0 doing it, so zeros-in-the-file
*was* the mechanism.

The cost was that every process launch read its whole `.bss` off the SD
card. For `repl` that is **109,628 bytes of zeros out of a 237KB
image** -- 46% of its load time spent transferring nothing, over
bit-banged SPI.

It was also a format with no identity. A bare `.bin` says nothing about
itself, so the loader inferred everything from the file size and hoped.

## Layout

```
offset  size  field
0       4     magic    "ZEXE"
4       2     version  currently 1
6       2     flags    reserved, must be 0
8       4     bss_size bytes to allocate and zero after data
12      4     entry    reserved; 0 means "base address"
16      ...   data     the loadable image, verbatim
```

`.bss` becomes a **number** instead of a region of zeros -- the loader
`memset()`s it, which is far faster than reading it.

`data_size` is deliberately not stored: it is `file_size - 16`, and the
filesystem already knows the file size. One fewer field that can
disagree with reality.

**Header at the start**, not the end. Both were considered; the loader
needs `bss_size` *before* it allocates, so a trailing header would mean
seek-to-end, read, seek-back on every launch to save nothing. 16 bytes
also keeps `data` 16-byte aligned, which suits the loader's 1KB chunked
reads.

## Backward compatibility

A file with no `ZEXE` magic is treated as the old raw format:
`data_size = file_size`, `bss_size = 0`. That is **exactly correct** for
a `--pad-to` binary, whose `.bss` is already present as zeros in the
data.

So old and new binaries coexist on the same card and apps convert one
at a time. `z_exec_parse()` (`sw/common/zexec.h`) is a pure function
with no I/O, which is what makes that logic testable off-target.

An unknown *version* is refused rather than guessed at -- a future
format change that silently half-loaded would corrupt memory instead of
failing.

## Pieces

| | |
|---|---|
| `sw/common/zexec.h` | format definition + `z_exec_parse()`, shared by loader and tools |
| `tools/mkexec.py` | wraps `objcopy` output in a header at build time |
| `fs_exec_info()` | reads and parses the header (`sw/os/fs/fs.c`) |
| `fs_load_exec()` | loads data, `memset()`s bss |

Split into inspect-then-load rather than one call because the caller
needs the image size *before* it can allocate: `k_proc_create()` must be
handed `data + bss`, and only then is there a base address to load
into. All three launch paths use it -- `sh.c`'s `run`, `init()`, and
`k_proc_run()` (the `Z_SYS_PROC_RUN` syscall behind wm's dock).

## Build

```make
@EDATA=$$($(PREFIX)nm app.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
 END=$$($(PREFIX)nm app.elf | awk '$$3=="_end"{print "0x"$$1}'); \
 $(PREFIX)objcopy -O binary app.elf app.data; \
 python3 ../../../tools/mkexec.py app.data app.bin $$(($$END - $$EDATA)); \
 rm -f app.data
```

`objcopy` without `--pad-to` stops at `_edata`; `_end - _edata` is the
bss size. Both numbers were already being computed for `--pad-to`.

## What it does and doesn't save

**Load time and card space.** `repl` on disk drops from 237,032 to
127,420 bytes.

**Not RAM.** `k_proc_create()` still allocates `data + bss`, so the
memory budget in `docs/boot.md` is unchanged.
