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
The stack and heap allowance was the same kind of guess: a table of
file names in the kernel.

## Layout

```
offset  size  field
0       4     magic    "ZEXE"
4       2     version  currently 1
6       2     flags    bits 2:0 tier; bits 15:3 must be 0
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
seek-to-end, read, seek-back on every launch to save nothing. The tier
has the same constraint, and it fits in the flags word that was
already there. 16 bytes also keeps `data` 16-byte aligned, which suits
the loader's 1KB chunked reads.

## The tier

Bits 2:0 of `flags`. The kernel turns the index into a byte count
(`z_proc_stack_size()` in `sw/os/kernel.h`) and will not grant more
than `Z_PROC_STACK_CAP`, which is HUGE. The bytes are the kernel's,
so a later change to what LARGE means does not need a new format.

| flags | name | stack + heap |
|---|---|---|
| 0 | unspecified | DEFAULT, 16KB |
| 1 | SMALL | 8KB |
| 2 | DEFAULT | 16KB |
| 3 | MEDIUM | 32KB |
| 4 | LARGE | 64KB |
| 5 | BIG | 1MB |
| 6 | HUGE | 4MB |
| 7, or any bit above bit 2 | unknown | DEFAULT, 16KB |

0 is "this program does not ask". That is what a binary written before
the field existed already has, because the word was reserved and
written as zero. A file with no `ZEXE` magic is the same request.
Index 2 asks for the default explicitly; the block is the same size.

An index this kernel does not know is the default, **not** the cap. A
corrupt flags word must not be handed 4MB. A known tier whose byte
count is above the cap is clamped down to the cap. No tier above
exceeds it today.

The allowance is the C stack and the malloc heap together, on top of
the image. Code, `.rodata` and `.bss` are the binary.
`k_proc_create()` allocates image + tier. `repl`'s Scheme cell heap is
a `.bss` array and does not come out of this number.

Process zero is not a ZEXE. The kernel asks for DEFAULT for itself.

An app sets it with `APP_TIER` in its Makefile (`sw/common/app.mk`
passes that to `tools/mkexec.py`). Unset leaves the field 0.

| app | tier | why this one |
|---|---|---|
| `wm`, `term` | SMALL | message loops. The margin the default adds was measured not to be needed, and 8KB is what made room for a second `term` on a 1MB board. `wm`'s clip list used to be a blob per send that was never freed; that is fixed. `term`'s port sends outlive the call, and they are bounded. |
| `net` | MEDIUM | a few relays, eight port sends in flight each way, each a heap copy until it is acked. More than 16KB. It stays off LARGE: `net` is a core app and stays small. The old per-message leak is fixed. |
| `repl` | LARGE | `te` mallocs the whole file and then the line list. MEDIUM does not leave the heap: a malloc of 18,505 bytes failed with 22,076 bytes between `sbrk` and `sp`. Scheme recursion is bounded separately (`MS_PROTECT_STACK_SIZE`). |
| `web` | LARGE | the parser context is nearly 6KB, and the draw path nests the parser back into layout. MEDIUM would very likely do. Running out of stack here is not a clean failure, and `web` is for 32MB boards either way. |
| `netserve` | LARGE | SSH engines on the heap, about 10KB each, two by default and four at most, plus eight sends in flight per session in each direction. |
| `bbs` | LARGE | each node is about 5KB (a 4KB output ring), four by default, and a caller can have eight 512-byte sends in flight. |
| `fed`, `cryptobench` | LARGE | ML-KEM keeps polynomial vectors on the stack. A decapsulation's deepest path is about 15KB, all of the default 16KB, with nothing left for the caller. There is no stack guard. On an 8.3 card the file is `cryptob`; the header is what the loader reads, not the name. |
| `vi` | BIG | nextvi allocates per line, about 3.4 times the file. A 154KB source needed between 273KB and 529KB. 1MB covers that and anything in this tree. It was 4MB, then 2MB, because `_fstat` reported size 0 and the editor allocated a 1,048,575-byte read buffer before it read a line. With the size reported, it starts in under 50KB. |
| `ask` | BIG | the pack this tree ships is `dense=no`: no resident vectors, about 30KB of `.bss` plus a stack. `dense=yes` still works up to about a megabyte of vectors and then says it needs HUGE. It was HUGE while the dense half was on (0.90MB for one pack, four packs maximum). |
| `zcc`, `posix` | HUGE | the token stream, the symbol and macro tables, the IR and the output buffer are live at once and scale with the source. A 2,000-line unit was sized at about 1MB; 4MB is the headroom that estimate should not be trusted without. `posix` hosts the compiler. This does not fit on a 1MB board; the failure is a clean refusal (`k_proc_create` returns 0). |
| `zfpga` | HUGE | the chip database is resident (1.5MB for a 25F, the whole file) plus the configuration it builds (560KB for a 25F, 1.9MB for an 85F). 1MB cannot hold the database. |
| anything else | (unset) | DEFAULT, 16KB. `console` and `cron` are in this row. They are core apps; the archive stores the ZEXE whole, flags and all, and theirs are zero. |

HUGE on an 8MB board is most of the machine once the kernel, `wm`,
`net` and a shell are also up. On 32MB it is unremarkable. A 4MB
request can also fail while 4MB is free, because the allocator is
first-fit and aligns to 4KB: that is fragmentation, and `free` shows
it.

## Backward compatibility

A file with no `ZEXE` magic is treated as the old raw format:
`data_size = file_size`, `bss_size = 0`, flags 0. That is **exactly
correct** for a `--pad-to` binary, whose `.bss` is already present as
zeros in the data.

So old and new binaries coexist on the same card and apps convert one
at a time. `z_exec_parse()` (`sw/common/zexec.h`) is a pure function
with no I/O, which is what makes that logic testable off-target.

An unknown *version* is refused rather than guessed at -- a future
format change that silently half-loaded would corrupt memory instead of
failing. An unknown *tier* is not a reason to refuse the file. The
program loads, and it gets the default allowance.

A kernel from before this field does not read `flags`. A binary that
asks for a tier still loads there. The version stays 1 so that remains
true: a new version would be refused by that loader.

The flash archive does not have its own copy of this field. A ZAR entry
is the whole ZEXE file (`sw/os/zar.h`), so `wm`, `net`, `term`,
`console` and `cron` carry whatever `mkexec.py` wrote.

## Pieces

| | |
|---|---|
| `sw/common/zexec.h` | format definition + `z_exec_parse()`, shared by loader and tools |
| `tools/mkexec.py` | wraps `objcopy` output in a header at build time |
| `fs_exec_info()` | reads and parses the header (`sw/os/fs/fs.c`) |
| `fs_load_exec()` | loads data, `memset()`s bss |
| `z_proc_stack_size()` | tier index to bytes, and the cap (`sw/os/kernel.h`) |

Split into inspect-then-load rather than one call because the caller
needs the image size *before* it can allocate: `k_proc_create()` must be
handed `data + bss`, and only then is there a base address to load
into. The tier is in the same header, so the same inspect answers both.
All three launch paths use it -- `sh.c`'s `run`, `init()`, and
`k_proc_run()` (the `Z_SYS_PROC_RUN` syscall behind wm's dock).

## Build

```make
APP_TIER = LARGE
@EDATA=$$($(PREFIX)nm app.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
 END=$$($(PREFIX)nm app.elf | awk '$$3=="_end"{print "0x"$$1}'); \
 $(PREFIX)objcopy -O binary app.elf app.data; \
 python3 ../../../tools/mkexec.py app.data app.bin $$(($$END - $$EDATA)) $(APP_TIER); \
 rm -f app.data
```

`objcopy` without `--pad-to` stops at `_edata`; `_end - _edata` is the
bss size. Both numbers were already being computed for `--pad-to`.
`APP_TIER` is optional. The app Makefiles set it above the
`include` of `sw/common/app.mk`, which is what actually runs that line.

`zcc` writes its own header for a program it compiles, with flags 0.
A compiled `hello` gets the default 16KB. The compiler itself is the
HUGE row above.

## What it does and doesn't save

**Load time and card space.** `repl` on disk drops from 237,032 to
127,420 bytes.

**Not RAM.** `k_proc_create()` still allocates `data + bss`, so the
memory budget in `docs/boot.md` is unchanged. The tier is a second
allocation on top of that, and it now comes from this header instead
of from the file's name.
