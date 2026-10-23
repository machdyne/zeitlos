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
6       2     flags    bits 3:0 stack size code; bit 8 compressed;
                       bit 15 driver; the rest must be 0
8       4     bss_size bytes to allocate and zero after data
12      4     entry    0; compressed: the data's size, unpacked
16      ...   data     the loadable image, verbatim -- or compressed,
                       as an LZ4 block
```

`.bss` becomes a **number** instead of a region of zeros -- the loader
`memset()`s it, which is far faster than reading it.

`data_size` is deliberately not stored: it is `file_size - 16`, and the
filesystem already knows the file size. One fewer field that can
disagree with reality. A compressed file is the exception (below).

**Header at the start**, not the end. Both were considered; the loader
needs `bss_size` *before* it allocates, so a trailing header would mean
seek-to-end, read, seek-back on every launch to save nothing. The stack
size has the same constraint, and it fits in the flags word that was
already there. 16 bytes also keeps `data` 16-byte aligned, which suits
the loader's 1KB chunked reads.

## The stack size

Bits 3:0 of `flags` are a size code for the stack and heap the program
wants on top of its image. Each code is twice the one before:

```
size = 8 KiB × 2^(code − 1)        code 1..14
```

| code | size | | code | size |
|---|---|---|---|---|
| 0 | unspecified: the default, 16KB | | 8 | 1MB |
| 1 | 8KB | | 9 | 2MB |
| 2 | 16KB | | 10 | 4MB |
| 3 | 32KB | | 11 | 8MB, the cap |
| 4 | 64KB | | 12 | 16MB |
| 5 | 128KB | | 13 | 32MB |
| 6 | 256KB | | 14 | 64MB |
| 7 | 512KB | | 15 | reserved |

0 is "this program does not ask". That is what a binary written before
the field existed already has, because the word was reserved and
written as zero. A file with no `ZEXE` magic is the same request.

**The cap.** The format can say 64MB; the kernel grants up to
`Z_PROC_STACK_CAP` (`sw/os/kernel.h`), which is **8MB**, code 11. That
is one doubling above the 4MB that `zcc`, `posix` and `zfpga` ask for,
so the largest ask in the tree can grow once without a kernel change,
and nothing in sight needs more. It is one constant; a board with
hundreds of megabytes can raise it, and `mkexec.py` reads it from
there. The cap is not what protects a small board: on 1MB a 4MB request
fails because the pool cannot hold it, cap or no cap.

**What the kernel cannot give, it refuses.** The program does not
start, and the console says what was asked for and what there is
(`k_proc_create_exec()`, `sw/os/kernel.c`). The four cases, as printed:

```
prog: stack size code 15 is reserved -- not started
prog: asks for 16384KB of stack+heap (code 12), above this kernel's cap of 8192KB -- not started
prog: unknown ZEXE flags 0x0010 -- not started
prog: needs 8312KB (image 117KB + 8192KB stack+heap), largest free block 6092KB of 6092KB free -- not started
```

(The last one is an 8MB request on a 32MB board with five `posix`
already running.)

It never grants less than was asked for. A program that asked for 32MB
and runs in 4MB fails later and somewhere less obvious; a request the
kernel does not understand is not rounded to anything. The launch
fails the way any failed launch does: `run` prints it, `init` carries
on (except for `wm`), and `Z_SYS_PROC_RUN` returns no pid. The line is
on the kernel console whichever path launched it; `posix`, for
example, says `cannot start` and the reason is there.

The last case is about one block, not the total: the allocator is
first-fit and aligns to 4KB (`sw/os/mem.h`), so a large request late
in a fragmented pool can fail while that much is free in pieces. The
line prints the largest free block for that reason, and `free` shows
the rest.

The allowance is the C stack and the malloc heap together, on top of
the image. Code, `.rodata` and `.bss` are the binary.
`k_proc_create()` allocates image + stack size. `repl`'s Scheme cell
heap is a `.bss` array and does not come out of this number.

Process zero is not a ZEXE. The kernel asks for the default for
itself.

An app sets it with `APP_STACK` in its Makefile, as a size (`8K`,
`64K`, `1M`, `4M`); `sw/common/app.mk` passes it to
`tools/mkexec.py`, which writes the code. A size that is not a power of
two from 8K to the cap fails the build. Unset leaves the field 0.

| app | `APP_STACK` | why this one |
|---|---|---|
| `wm`, `term` | 8K | message loops. The margin the default adds was measured not to be needed, and 8KB is what made room for a second `term` on a 1MB board. `wm`'s clip list used to be a blob per send that was never freed; that is fixed. `term`'s port sends outlive the call, and they are bounded. |
| `net` | 32K | a few relays, eight port sends in flight each way, each a heap copy until it is acked. More than 16KB. It stays off 64KB: `net` is a core app and stays small. The old per-message leak is fixed. |
| `zerdesk` | 32K | `zport` keeps a heap copy of every stripe in flight, up to a 6KB budget, and the page is read while that is live. Six viewers, and a page load while five were already up, left 17,984 bytes free of the 33,908 it gets. The unnamed 16KB left almost nothing past three. |
| `repl` | 64K | `te` mallocs the whole file and then the line list. 32KB does not leave the heap: a malloc of 18,505 bytes failed with 22,076 bytes between `sbrk` and `sp`. Scheme recursion is bounded separately (`MS_PROTECT_STACK_SIZE`). |
| `web` | 64K | the parser context is nearly 6KB, and the draw path nests the parser back into layout. 32KB would very likely do. Running out of stack here is not a clean failure, and `web` is for 32MB boards either way. |
| `netserve` | 64K | SSH engines on the heap, about 10KB each, two by default and four at most, plus eight sends in flight per session in each direction. |
| `bbs` | 64K | each node is about 5KB (a 4KB output ring), four by default, and a caller can have eight 512-byte sends in flight. |
| `fed`, `cryptobench` | 64K | ML-KEM keeps polynomial vectors on the stack. A decapsulation's deepest path is about 15KB, all of the default 16KB, with nothing left for the caller. There is no stack guard. On an 8.3 card the file is `cryptob`; the header is what the loader reads, not the name. |
| `vi` | 1M | nextvi allocates per line, about 3.4 times the file. A 154KB source needed between 273KB and 529KB. 1MB covers that and anything in this tree. It was 4MB, then 2MB, because `_fstat` reported size 0 and the editor allocated a 1,048,575-byte read buffer before it read a line. With the size reported, it starts in under 50KB. |
| `ask` | 1M | the pack this tree ships is `dense=no`: no resident vectors, about 30KB of `.bss` plus a stack. `dense=yes` still works up to about a megabyte of vectors and then says it needs 4MB. It was 4MB while the dense half was on (0.90MB for one pack, four packs maximum). |
| `zcc`, `posix` | 4M | the token stream, the symbol and macro tables, the IR and the output buffer are live at once and scale with the source. A 2,000-line unit was sized at about 1MB; 4MB is the headroom that estimate should not be trusted without. `posix` hosts the compiler. This does not fit on a 1MB board; the launch is refused, with the line above. |
| `zfpga` | 4M | the chip database is resident (1.5MB for a 25F, the whole file) plus the configuration it builds (560KB for a 25F, 1.9MB for an 85F). 1MB cannot hold the database. |
| anything else | (unset) | the default, 16KB. `console` and `cron` are in this row. They are core apps; the archive stores the ZEXE whole, flags and all, and theirs are zero. |

4MB on an 8MB board is most of the machine once the kernel, `wm`,
`net` and a shell are also up. On 32MB it is unremarkable.

## The driver flag

Bit 15 of `flags` (`Z_EXEC_FLAG_DRIVER`): the program is a **driver**
([userspace_drivers.md](userspace_drivers.md), "Drivers"). It is
started exactly like any other program -- `run`, `z_proc_run()`, init,
the dock -- and the loader marks the process `Z_PROC_FLAG_DRIVER`,
which is what lets it open the MPU's grant window onto a process that
allows drivers ([mpu.md](mpu.md), "The grant window"). The flag
declares; the client still decides.

From the top of the word down, so the stack code can grow up into 14:4
if it ever has to. An app sets it with `APP_DRIVER = 1` in its Makefile
(`mkexec.py --driver`). A kernel from before it refuses the binary --
"unknown ZEXE flags" -- rather than run a driver without its powers.

## Compressed executables

Bit 8 of `flags` (`Z_EXEC_FLAG_COMPRESSED`): the data after the header
is an **LZ4 block** ([lz4_Block_format.md](https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md)),
and `entry` holds the image's size unpacked -- the file's size no
longer says it, and the loader needs it before it allocates. The header
is not compressed, so the first 16 bytes still tell a loader everything
(`z_exec_parse()` reports the unpacked size as `data_size`, and
`stored_size` and `compressed` besides): sizing a process, the stack
code, the driver flag are all as before.

**Who writes them.** `tools/mkzar.py`, for the core apps in the flash
archive -- every one but those named in `CORE_STORE` (`wm` and `net`, for
the time they took to unpack at boot), anything
that is not a plain ZEXE file, and anything that would not get smaller.
Its compressor (`lz4_compress()`, hash chains and lazy matching) packs
about as small as lz4's own high-compression mode, keeps the format's
end rules so any LZ4 decoder reads its blocks, and every block is
unpacked again before it is stored. A core app's `.bin` in the tree
stays plain, and so do the binaries `tftp-dist` and `nfs-dist` publish,
for now.

**Who reads them.** Every ZEXE loader unpacks:

| loader | from | input |
|---|---|---|
| `z_zar_load_exec()` (`sw/os/zar.c`) | the flash archive | the flash window, a word per read |
| `load_card()` (`sw/os/kfs.c`) | any file: card, `/usb`, `/ram`, `/nfs` | `zfs_read()` |
| `machine_load_bin()` (`sim/machine.c`) | a file on the host | memory |

So a compressed binary copied out of flash (`/apps/wm`, say) onto the
card still runs. A load that does not unpack to exactly its size --
truncated, or with a length or offset that points outside the image --
is refused, and the process is not started. LZ4 carries no checksum: a
changed literal byte is not caught (nor was it before compression).

**The decoder** is `sw/common/zexec_lz4.c`, about 1.5KB of code
(compiled `-O2` in a `-Os` kernel) and 280 bytes of state (one copy in
the kernel, taken in turn, since a load can wait on `fs` mid-stream
while another starts). It writes straight into the new process's memory
and reads its history from there, so it needs no window; a long literal
run is filled straight from the source into the image. Its cost is per
*sequence* more than per byte -- `wm` is 9,777 sequences of 4 literal
bytes and a 7-byte match on average -- so nearly every one takes a fast
path: no length bytes, the whole sequence already in the buffer, read
with plain pointer steps and no refill checks. About 9 instructions a
byte in `sim/` (from 12 without it). A match of 8 bytes or more that
repeats one byte (offset 1: the zeros of tables and pictures) or copies
from a whole number of words back is written a word at a time
(`match_words()`): a quarter of the stores, and the stores are what
memory slower than the CPU charges for.

**Reading from a file** (`load_card()`): the decoder asks for its
buffer's worth, 256 bytes, at a time, and every `zfs_read()` is a round
trip to the `fs` process. So `card_fill()` reads 8KB at a time into a
stage and hands it out from there (one stage, used by the one load that
is unpacking). Before it, planedemo's 1.2MB on the card were 4,800 round
trips: **15.3 seconds** to load (October 2026).

**Why LZ4, and not DEFLATE.** DEFLATE came first: smaller (57% of the
images, to LZ4's 68%), but measured on hardware `wm` took **1,251ms**
to load. A PicoRV32 at 48MHz runs about 7 million instructions a second
(0.14 IPC, [dcache.md](dcache.md)), and DEFLATE cost about 41 a byte
(Huffman decoding, table lookups; measured in `sim/`). LZ4 has no
entropy coding -- byte-aligned literal runs and back-references -- and
cost 12 a byte at first: `wm` in **646ms**. With the fast path, 9. The
DEFLATE decoder (`zinflate.c`) stays where it was, for the web client
and PNG.

**What it saves**: a compressed app takes 68% of its size in flash. With
`wm` and `net` stored as they are, the archive's apps went from 615KB to
507KB (421KB with all of them compressed; `flash_apps.md`, "How much
fits").

**What it costs**: CPU time at every load. `init` and `run` print every
load's time, the unpacking's share of it, and how much of that the
loading process actually had the CPU for -- `init: usb loaded in 127 ms
(125 ms unpacking, 125 ms of it on the CPU)`; a CPU figure well under
the unpacking one says the other processes took the difference. That is what `CORE_STORE` -- an app kept
uncompressed -- is decided by.

**Finding the program once.** A launch asked the kernel's resolver
three times in a row -- for the size, for where it is from, for the
load -- and each was one or two lookups through `fs` (`/nfs/apps`, then
the card's `/apps`). The resolver now keeps its last answer for half a
second (`exec_resolve_kept()`, `sw/os/kfs.c`), so the second and third
are free; the load still refuses a file that does not match the sizes
it was given.

**`entry`** was reserved for a start address other than the image's
first byte. Nothing ever used it: every app is linked at and started
from 0x80000000. Should one ever be needed, it is a new header version,
which today's loaders refuse.

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
failing. An unknown stack size code is refused too, at launch (above).

A kernel from before this field does not read `flags`. A binary that
asks for a stack size still loads there, with that kernel's default.
The version stays 1 so that remains
true: a new version would be refused by that loader.

The flash archive does not have its own copy of this field. A ZAR entry
is the whole ZEXE file (`sw/os/zar.h`), so `wm`, `net`, `term`,
`console` and `cron` carry whatever `mkexec.py` wrote -- compressed by
`mkzar.py`, the header otherwise as it was.

A compressed binary is refused by a kernel from before bit 8 ("unknown
ZEXE flags"), rather than run as compressed bytes.

## Pieces

| | |
|---|---|
| `sw/common/zexec.h` | format definition + `z_exec_parse()`, shared by loader and tools |
| `tools/mkexec.py` | wraps `objcopy` output in a header at build time |
| `fs_exec_info()` | reads and parses the header (`sw/os/fs/fs.c`) |
| `fs_load_exec()` | loads data, `memset()`s bss |
| `z_exec_stack()` | size code to bytes, against a default and a cap (`sw/common/zexec.h`) |
| `sw/common/zexec_lz4.c` | unpacks a compressed image into the process's memory |
| `tools/mkzar.py` | compresses the core apps for the flash archive |
| `k_proc_create_exec()` | the launch: the cap, the refusal and its message (`sw/os/kernel.c`) |

Split into inspect-then-load rather than one call because the caller
needs the image size *before* it can allocate: `k_proc_create()` must be
handed `data + bss`, and only then is there a base address to load
into. The stack size is in the same header, so the same inspect answers both.
All three launch paths use it -- `sh.c`'s `run`, `init()`, and
`k_proc_run()` (the `Z_SYS_PROC_RUN` syscall behind wm's dock).

## Build

```make
APP_STACK = 64K
@EDATA=$$($(PREFIX)nm app.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
 END=$$($(PREFIX)nm app.elf | awk '$$3=="_end"{print "0x"$$1}'); \
 $(PREFIX)objcopy -O binary app.elf app.data; \
 python3 ../../../tools/mkexec.py app.data app.bin $$(($$END - $$EDATA)) $(APP_STACK); \
 rm -f app.data
```

`objcopy` without `--pad-to` stops at `_edata`; `_end - _edata` is the
bss size. Both numbers were already being computed for `--pad-to`.
`APP_STACK` is optional. The app Makefiles set it above the
`include` of `sw/common/app.mk`, which is what actually runs that line.

`zcc` writes its own header for a program it compiles, with flags 0.
A compiled `hello` gets the default 16KB. The compiler itself is the
4M row above.

## What it does and doesn't save

**Load time and card space.** `repl` on disk drops from 237,032 to
127,420 bytes.

**Not RAM.** `k_proc_create()` still allocates `data + bss`, so the
memory budget in `docs/boot.md` is unchanged. The stack size is a second
allocation on top of that, and it now comes from this header instead
of from the file's name.
