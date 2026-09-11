# libz, the runtime for zcc-compiled programs

## Status

**Phase 2 is done, including graphics.** A `zcc`-compiled program
calls `printf`, `malloc`, the filesystem, the framebuffer and the
window manager, and produces byte-identical output to the same program
built with GCC against the same runtime -- down to a checksum of the
framebuffer after drawing. Checked by the test suite on every run.

97 table entries, 35KB blob, ABI 2.

```
make -C sw/apps/zcc/libz          # needs the RISC-V toolchain
sw/apps/zcc/zcc -I libz -L libz hello.c -o hello
```


## Why the table is flat

`libz_table.def` used to be grouped -- memory, strings, the filesystem,
graphics -- with a heading over each. It reads better that way and it
**fights the one rule the file has**: order is the ABI, so a new entry
must go at the END and nowhere else.

A grouped list invites putting a new string function with the other
string functions. That is the single edit that breaks every binary
built against the old table, silently, because the jump still succeeds
and lands on the wrong function.

So the groups are gone. The list is flat, with the index every sixteen
lines, and the only place a new entry can go is obvious. `libz.sym` is
generated and always agrees with reality, so nothing is lost by not
having headings here.

There is also a **RETIRED convention** written down at the end of the
file: a withdrawn function keeps its slot with the name replaced,
because deleting the line shifts everything after it. Nothing is
retired yet -- it is recorded now because the moment it is needed is
the moment deleting the line looks harmless.

## ABI 3: the object system

`z_obj_uint32`, `_int32`, `_str` and `_none` went in with ABI 2
because `zgfx.c` and `zwin.c` call them. The rest of `zobj.h` did not,
and the gap was invisible until somebody looked for it: `z_msg_send()`
was reachable from a compiled program and there was **no way to build
anything to send but a scalar.** Lists, maps and blobs -- the payloads
the messaging protocol is made of -- could not be constructed at all.

Sixteen entries added: `z_obj_blob`, `z_blob_data`, `z_blob_len`,
`z_obj_list`, `z_list_append`, `z_list_get`, `z_obj_map`, `z_map_set`,
`z_map_find`, `z_map_get_key`, `z_map_get_val`, `z_obj_copy`,
`z_obj_free`, `z_obj_size`, `z_obj_equal`, `z_obj_float32`.

`z_obj_blob` is the one worth naming: a blob is what every port
message carries, and it was missing from the table **and** from
`sw/test/test_zobj.c`. Untested and unreachable turned out to be the
same oversight seen from two directions.

## The problem it solves

`zcc` compiles one translation unit into a flat image. It cannot
compile `sw/common/*.c` — those are full C99 with inline assembly and
newlib dependencies — and requiring it to would have made "compile all
of `sw/common`" a day-one prerequisite for a compiler that did not yet
exist.

So the runtime is built **once, by GCC**, into a blob that `zcc`
copies verbatim into every image it produces, and reached through a
**jump table** whose slots `zcc` resolves by index.

## How it fits together

```
0x80000000   _start          the kernel jumps here
+0x10        libz_header     magic, ABI version, two patch words
+...         libz_table      one `j` per entry, in libz_table.def order
+...         libz code and data
+...         libz .bss, as literal zeros
_libz_end    ---- everything above is the blob, copied verbatim ----
             user .text      zcc's output
             user .data
             user .bss       counted in the ZEXE header
```

The blob is linked at `0x80000000` and lands at `0x80000000`, because
every Zeitlos app runs at that address — the MTU remaps it per process
(`docs/app_runtime.md`). So every absolute address inside the blob is
correct by construction: no relocation, no position-independent code,
no GOT.

### Calls

A table slot holds one instruction: `j real_function`. A caller does
`jal ra, table + 4*i`, which lands in the slot with `ra` pointing back
into its own code; the `j` transfers to the function with `ra`
untouched, so the function returns straight to the caller.

**One extra instruction per call, and no register clobbered.**

Indexing by slot rather than by address is what makes a rebuilt
runtime not invalidate every binary ever produced — as long as the
*order* is stable. Which is the whole discipline:

> **THE ORDER OF `libz_table.def` IS AN ABI. NEVER INSERT IN THE
> MIDDLE.**

Inserting anywhere but the end shifts every later index, and a binary
built against the old order calls the wrong function. Not a crash:
`printf` where `malloc` was expected. The same hazard
`sw/common/syscalls.def` documents, and its own history records
`HID_READ_KEY` having to be moved to the end for exactly this reason.

Two mechanical protections, neither of which replaces reading that
file's header comment: `LIBZ_ABI_VERSION` is stamped into the blob and
into `libz.sym`, and `zcc` refuses a mismatch outright rather than
warning; and the build refuses to emit a table shorter than the
previous one.

### The two patched words

The blob is built before any program exists, so two things in it can
only be filled in by the compiler:

| | |
|---|---|
| `libz_main_ptr` | the address of `main`, which `_start` calls indirectly |
| `libz_heap_start` | the image's `_end`, where `malloc` begins |

`zcc` records both as ordinary fixups against labels, because neither
value exists at the time: `main` is not defined until it is parsed,
and `_end` is not known until the image is laid out.

### `libz.sym`

Generated from the linked blob by `mksyms.py`, never hand-written, so
it cannot disagree with the blob it describes:

```
version 1
blob 5472
table 16
patch main 24
patch heap 28
sym 0 malloc
sym 1 free
...
```

One directive per line, space-separated. Deliberately trivial to
parse, because **this exact code has to run on the device in Phase 3**,
where there is no line-splitting library and the compiler parsing it
has no `sscanf`.

## What is in it, and what is not

97 entries. The libc surface is libz's own: memory
(`malloc`/`free`/`calloc`/`realloc` and the `mem*` family), strings,
console I/O (`putchar`/`puts`/`getchar`/`printf`/`snprintf`).

**Everything else is the tree's own code, compiled unmodified**:
`zgfx.c`, `zwin.c`, `zobj.c`, `zkbd.c` and `zfont_data.c` from
`sw/common`, giving the framebuffer, the GPU paths, the window
protocol, the object system and the fonts. Plus the syscall wrappers
and `maskirq`.

That was the surprise of this phase. The plan (`docs/posix.md` 3.1(b))
assumed the runtime would have to be written; in the event four of the
five files needed no change at all. What they needed was five small
shim headers in `libz/shim` -- freestanding `<stdio.h>`, `<stdlib.h>`,
`<string.h>`, `<stddef.h>`, `<math.h>`, `<stdarg.h>` -- declaring only
the handful of names those files actually call.

### The runtime provides the tree's OWN names

`fs_mallocfile()`, not a parallel `fs_read()`. `z_win_create()`,
`z_fb_draw_text()`, `z_msg_send()` -- the same spellings, the same
signatures, from the same headers.

This started as an aesthetic preference and became a hard rule after
`libz.h` declared `z_getpid()` as `unsigned` where `zeitlos.h` says
`uint32_t`. Those are the same type until the include path changes,
and then they are a compile error in whichever file is unlucky. So
`libz.h` now **includes** `zeitlos.h` and `zfsapp.h` rather than
shadowing them, and declares only what they do not.

The payoff is bigger than avoiding that bug: a program moving between
a GCC build and a zcc build does not change a single call.

**It is not newlib and is not trying to be.**
`docs/app_runtime.md` records what newlib costs on this machine:
about 100KB for the printf formatter, and about 40KB more for anything
touching a `FILE` — even `fputs(s, stdout)`, which formats nothing.
The tree's own standing advice is to format numbers by hand rather
than call `printf` at all, and `sw/apps/read` hit that trap twice in
one week, both times presenting as an unexplained tripling of the
binary.

`zcc` output is already about 4.5x GCC's size. Adding a 100KB
formatter would produce a hello-world that does not fit.

| | |
|---|---|
| **the whole blob** | **35,184 bytes** |
| hello-world with `printf`, complete image | 35,844 bytes |
| a windowed app, complete image | 35,916 bytes |

The blob was 5.5KB before graphics went in. Every `zcc` binary carries
a copy, which is a real cost and an accepted one: `docs/posix.md`
section 10's shared runtime turns it into a one-time cost later, and
being able to build a windowed app on the device is worth more now
than the megabyte it costs across a dozen of them.

`printf` here handles `%d %i %u %x %X %o %c %s %p %%` with `-` and `0`
flags, field width, precision, and `*` for either — and costs about
1KB. Length modifiers (`%ld`, `%zu`) are accepted and ignored, because
on rv32/ilp32 every integer type is 32 bits so they all mean the same
thing, and refusing them would reject format strings already in this
tree.

Absent: floating point (there is none in the compiler or the SOC),
`FILE`, `errno`, `qsort`, `setjmp`.

### The argument ceiling

`printf` takes at most seven conversions and `snprintf` five, because
the format string and the buffer occupy argument slots too.

That is not a limitation of the formatter — it uses real `<stdarg.h>`,
which works because the RISC-V ABI passes a variadic call's first
eight words in `a0`–`a7` and `zcc` passes arguments in `a0`–`a7`. It is
`zcc`'s own eight-argument ceiling showing through. A program that
compiles cannot exceed it, which is a better place for the limit to
live than in the runtime.

## `maskirq`, and why this mechanism was worth building

`maskirq` is the one function in `sw/common/zeitlos.h` that Phase 1
could not compile: a raw picorv32 custom instruction behind
`__asm__`, and `zcc` has no inline assembler.

Here it is ordinary GCC-built code behind an ordinary table slot. No
new compiler feature, now or ever — and **the same treatment works for
anything else that turns out to need an instruction `zcc` cannot
express**. That is the general answer, and it is worth knowing it
exists before the instinct to build an assembler takes hold.

The simulator needed a small change to match: `sim/cpu.c` now
implements opcode `0x0b`/funct3 `6`/funct7 `3`. The first
`zcc`-compiled program to call `maskirq` trapped on an illegal
instruction, which is how that gap got found. There are no interrupts
in the simulator, so the mask is a register and nothing else — faithful
for every caller in the tree, since they all only save, mask and
restore.

## Build flags that are requirements, not tuning

```
-mno-relax -msmall-data-limit=0
```

`gp`-relative addressing resolves through whatever `gp` the **caller**
has, and libz is called from `zcc`-compiled code that never sets `gp`
at all. This is the same hazard `docs/app_runtime.md` documents for
the kernel's syscall path, where it took three separate bugs to find:
a global read through the wrong `gp` does not crash, it silently reads
another process's memory. Turning the addressing mode off entirely
costs one instruction per global access and removes the class.

The reference build in the test suite passes the same flags, for a
reason worth stating: mixing relaxed and unrelaxed objects means some
accesses go through `gp` and some do not, and `gp` is never set.

```
-ffreestanding -fno-builtin
```

There is no libc here; libz *is* the libc. `-fno-builtin` because GCC
will otherwise recognise the loop inside `memcpy()` and rewrite it as
a call to `memcpy()`.

**No `--gc-sections`.** Every table entry is reached only through a
`j` in an assembly section, and the linker cannot see those as uses —
it would drop the entire runtime and leave a table of jumps to
nowhere. `KEEP()` on the table is not enough, because what needs
keeping is everything the table points *at*.

## `.bss` inside the blob

libz's `.bss` is padded out as literal zeros in the blob rather than
counted in the ZEXE header.

It has to be. The blob sits at the **front** of the image and the ZEXE
header describes exactly one `.bss`, at the end (`docs/executables.md`)
— libz's would otherwise sit in the middle, before the user's `.text`,
which is not a shape that format can express. The cost is a few
hundred bytes of image and it removes a class of ordering bug.

## Testing

`tests/l*.c` in the `zcc` suite. Each is compiled twice — once by
`zcc` with the blob prepended and patched, once by GCC linking libz's
object files directly — and both run under `sim/zsim-headless` with
identical output required.

That is a genuinely strong test of the mechanism, because the two
builds reach the runtime by completely different routes: one through
a patched blob and a jump table, the other through an ordinary link.
The runtime code itself is the same object either way, so any
difference is `zcc`'s calling convention and nothing else.

The reference build needs its own entry stub (`ref_libz_start.S`),
which does by hand the two jobs `zcc` does by patching: store `_end`
into `libz_heap_start`, then call `main`. Without it the reference
build's `malloc` returns NULL forever and the outputs differ for a
reason unrelated to the compiler.

## One change to `sw/common`

`zeitlos.h` gained a `#if defined(__zcc__)` arm around `maskirq`,
declaring it instead of defining it as a `static inline` full of
assembly.

Small, and it replaced something worse. Before it, compiling anything
against `zeitlos.h` needed `-U__riscv`, which takes the header's host
branch -- `maskirq` returns 0, silently disabling the atomicity
`z_fb_hw_line()` and the ENC28J60 driver depend on. **A wrong answer
that compiles is worse than a compiler that cannot read the header.**

`syscall.c` defines `__zcc__` around its own include of that header,
for the same reason from the other side: it wants the declaration,
because the definition it is looking for is the one further down its
own file.

## Two build details worth knowing

**`-lgcc`, after `-nostdlib`.** libgcc is the *compiler's* support
library, not the C library: it holds the routines GCC emits calls to
for operations the ISA has no instruction for. `zobj.c` compares
`Z_FLOAT32` values, which on a core with no F extension becomes a call
to `__ltsf2` -- so the link failed on a soft-float comparison in a
runtime that has no floating point of its own. Everything is still
freestanding; libgcc is not libc.

**`fabsf` as a shim inline.** The same comparison wants it. Clearing
bit 31 of an IEEE-754 single *is* its absolute value, exactly, for
every input including zero, infinity and NaN -- so it is a mask rather
than a call, and no libm is needed for one line.

## What this is not, yet

**Not the shared runtime for existing apps.** `docs/posix.md` section
10 describes putting one copy of the runtime at a fixed physical
address after the kernel, with per-process data reached through the
MTU, so that every app in `sw/apps` stops carrying its own. This blob
is copy-per-image: simpler, no linker-script surgery across the tree,
and it costs 5KB per binary rather than a shared page.

The jump table is designed to make that migration possible — indices
rather than addresses, a version word, stable ordering — but the
migration is its own phase and its payoff is the memory budget on
small boards, not `zcc`.

**Not verified on hardware.** `sim/` has no `wm`, no second process
and no message delivery, so `z_win_create()` correctly times out and a
windowed app reports "no window manager" and exits. The framebuffer
tests prove the drawing calls reach the real implementation with the
right arguments -- by checksumming VRAM, which a wrong argument order
would change -- but "a window actually appears" needs the board.

## Files

| | |
|---|---|
| `libz_table.def` | the table, in order. **Read its header before touching it.** |
| `libz.h` | what a program includes |
| `start.S` | `_start`, the header, the table |
| `syscall.c` | syscall wrappers, `maskirq` |
| `mem.c` | `malloc` and the `mem*` family |
| `str.c` | the `str*` family |
| `fmt.c` | `printf` and friends |
| `libz.ld` | the fixed layout |
| `mksyms.py` | generates `libz.sym` from the linked blob |

## See also

- `docs/zcc.md` — the compiler
- `docs/posix.md` — the phase plan, and section 10 on sharing this
- `docs/executables.md` — ZEXE, and why `.bss` lives where it does
- `docs/app_runtime.md` — the `printf` size trap, and the `gp` hazard
