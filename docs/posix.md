# posix -- a Unix-shaped userland for Zeitlos

A shell, a C compiler and a vi, running on the machine itself. Edit a
file, compile it, run it, without a cross-compiler and without another
computer.

**Status: working.** What is not done is listed at the end.

---

## Getting there

`posix` is a port provider, like `repl`. With an sdcard present, `init`
starts both at boot, and a new `term` window's **POSIX** button connects
to it. By hand:

```
> run wm
> run posix
> run term
```

Then click **POSIX** on the `term` window's start panel, or type
`port posix0` straight onto it, or from a `repl` prompt type
`port posix0`. All three reach the same place. **F12** always
disconnects, back to that panel.

posix needs about 4.2MB of RAM (its 4MB tier plus its image), so it will
not start on the 1MB and 2MB boards; `init` prints why.

You get a `$` prompt.

## The shell

Commands, redirection `>` and `>>`, pipes `|`, and `&&` / `||`.
There is no quoting and no globbing -- a Zeitlos path has no spaces in
it, and every argument is a path or a flag.

```
$ ls
$ cat notes.txt
$ wc *.c            <- NO. globbing does not exist
$ cat a.c | grep printf | wc
$ zcc hello.c -o hello && run hello
```

| | |
|---|---|
| files | `ls` `cat` `cp` `mv` `rm` `touch` `mkdir` `rmdir` |
| text | `wc` `head` `tail` `grep` `sort` `uniq` |
| places | `cd` `pwd` |
| other | `echo` `clear` `df` `run` `help` `exit` |

`grep` matches a **fixed string**, not a regular expression. `vi` has
regular expressions (`:g`, `:s`) if you need them.

Anything that is not a builtin is run as a program, so `zcc hello.c`
and `run zcc hello.c` are the same thing.

### What streams and what does not

`cat`, `cp`, `wc`, `grep`, `head`, `tail` and `uniq` read a file in
chunks and have **no size limit**. `sort` has to hold every line and
refuses above 512 lines or 16KB rather than dropping the rest.

**A pipe holds its intermediate in memory, capped at 16KB**, and says
so when it truncates. So `wc big.c` is fine and `cat big.c | wc` may
not be.

## The compiler

```
$ zcc hello.c -o hello
$ run hello
```

`zcc` is a C compiler that runs on the machine. It handles most of C89
plus parts of C99 -- see `docs/zcc.md` for exactly what, and what it
refuses.

A program that includes `libz.h` gets `printf`, `malloc`, the string
functions, the filesystem, graphics and windows. The headers and the
runtime live in `/libz` on the card, and `zcc` looks there by default
-- so there is nothing to type:

```
$ zcc -o prog prog.c
```

Without `-L` you get a freestanding binary: no `printf`, no `malloc`.
`zcc` will tell you so rather than leaving you to discover it at the
first undefined symbol.

`sw/apps/zcc/examples/` has three programs to start from, and they
ship on the card in `user/`:

| | |
|---|---|
| `hello.c` | no includes at all -- compile with `-nolibz` |
| `hellolz.c` | `printf`, `malloc`, the string functions |
| `hellotrm.c` | output to the **term window** rather than the console |

The third is the one worth reading. **A program's output goes to the
serial console unless it asks otherwise** -- that is the right default
for something started from the kernel shell, and it means a program
run from `posix` prints where nobody is looking. `posix` relays for a
program that opens a second connection tagged `"stdout"`, which is
about fifteen lines, and `zcc` itself does exactly that.

## The editor

```
$ vi notes.txt
```

This is `nextvi`, a real vi. Movement is `hjkl` **and** the arrow
keys; `:w` writes, `:q` quits, `:wq` does both. Search with `/`, and
`:s` substitutes with real regular expressions.

Three things do not work, and will tell you so:

- **`:!`** cannot run a command -- there is no `fork` on this machine.
- **`:e` filename completion** offers nothing.
- **Syntax highlighting** is inert: the display is one bit per pixel,
  so there are no colours to highlight with.

The window is always 80x25 regardless of how large you make it.

## Gotchas worth knowing

- **A program's output does not come back to the shell unless the
  program asks.** `zcc` asks. Something you compile yourself will
  print to the serial console instead, which looks like silence if you
  have no cable attached -- see `user/hellotrm.c` for the fifteen
  lines that fix it.
- **Typing while a command runs is discarded**, not queued.
- **`&&` waits for the program to finish**, so `zcc x.c && run x` does
  what it looks like.
- **There is one current directory**, shared by every terminal
  connected to the same `posix`.

---

# The development record

Everything below is how this was built and why, kept in the order it
happened. Sections 1-8 are the ORIGINAL proposal, written before any
code; sections 9 onward record what each phase actually did, including
where it departed from the plan above it.

It is deliberately not tidied into a clean design document. What was
predicted and what happened are both useful, and the gap between them
is the most useful part -- several decisions here were reversed by
measurement, and the reversals are the load-bearing content.

## 1. What is actually being asked for

The stated goal is a POSIX layer at `sw/apps/posix` that `term` connects
to instead of `repl`, and an on-device C compiler that can build native
Zeitlos apps.

Those are two projects, and the second is the one with the value in it.
A POSIX layer with nothing to compile is a shell with `ls` in it, which
`repl` already has. A C compiler with no POSIX layer is a machine that
can extend itself, which is the whole point of "timeless computing" —
a Zeitlos that can rebuild its own applications, on the desk, with no
cross-compiler, is a materially different machine from one that cannot.

So this document treats them as three separable projects:

| | | depends on |
|---|---|---|
| **A** | `zcc` — a C compiler emitting ZEXE images | nothing |
| **B** | `posix` — a Unix-shaped userland in one process | nothing (A makes it worth having) |
| **C** | a Unix-shaped filesystem | nothing |

**None of the three depends on the others.** That is the most important
structural claim in this document, and the plan in §7 is built on it.
The temptation is to do C then B then A, because that is the order they
sit in a real Unix. That order is wrong here: it front-loads all the
core-system churn (a new filesystem is a change to `sw/os`, which is
exactly what we are trying not to bloat) and back-loads the only piece
that can fail for interesting reasons.

Do **A first**, standalone. `run zcc hello.c && run hello` is a
complete, demonstrable, shippable feature with no POSIX layer and no new
filesystem anywhere in it.

---

## 2. What the current system permits

Constraints taken from the tree, not assumed. These shape everything
below.

### 2.1 An app's heap is currently capped at 64KB

`_sbrk()` (`sw/common/zeitlos.c`) grows the heap up from `_end` and
refuses to pass the current stack pointer. `k_proc_create(size,
stack_size)` allocates `image + stack_size`, so the heap and the stack
share the `stack_size` allowance. The tiers
(`z_proc_stack_size_for()`, `sw/os/kernel.h`) top out at
`Z_PROC_STACK_SIZE_LARGE` = 64KB, for `web`.

**A C compiler needs megabytes.** This is the single hard blocker, and
it is also the smallest fix in this document: one more tier and one more
name in that function. It is a core change, but a two-line one, and it
is needed regardless of which compiler we end up with.

Related ceilings worth knowing before sizing anything: `Z_MEM_MAX_BLOCKS`
is 256, `Z_MEM_MIN_BLOCK_SIZE` is 32KB, `Z_MEM_ALIGNMENT` is 4096, and
`Z_PROCS_MAX` is 16.

### 2.2 Board RAM decides who gets this at all

`rtl/boards.vh`:

| board | `MEM` |
|---|---|
| Obst | 1MB (some 2MB) |
| sergei_ml1 | 8MB |
| Lakritz, mozart_ml1, Kölsch, ulx3s | 32MB |
| one target | 64MB |

The kernel reports the real figure through the capability CSRs
(`docs/csrs.md`), and `Z_MEM_SIZE_DEFAULT` (1MB) is only the pre-CSR
fallback. So the system can *know* at runtime whether it can host this.

**Obst cannot run any of this and should not try.** With the kernel
process alone at 233KB of a 1MB pool, there is no version of a C
compiler that fits. `posix` and `zcc` should refuse to start below a
declared floor and say so plainly, the way `sw/apps/serial` exits on a
board with no UART1 rather than staying resident to refuse every
connection. 8MB is the right floor to design against; 32MB is where it
will actually be comfortable.

### 2.3 The executable format is a gift

ZEXE (`docs/executables.md`) is a 16-byte header and a flat image loaded
at a fixed virtual base, `0x8000_0000`, with `.bss` as a number. No
relocations. No symbol table. No dynamic loader. Every app is linked to
the same address because the MTU remaps it per process.

For a compiler writer this removes two entire components: **there is no
linker to write and no ELF to emit.** A compiler that lays down machine
code into a buffer and prepends 16 bytes has produced a runnable Zeitlos
program. This is a much easier target than Linux/ELF, and it is why the
survey in §3.2 weights "generates machine code directly, no assembler"
so heavily.

### 2.4 Filesystem throughput is the thing to measure first

`docs/ramdisk.md` records ~19 KB/s reading from the SD card, against
`/ram` doing the same 258KB thirty times faster. A compiler is an I/O
amplifier — headers in, object out, and for self-hosting, its own source
read back — so this number decides whether a compile takes seconds or
minutes.

**That figure needs re-measuring before anything is designed around it.**
`rtl/spim.v` moved the shift register and divider into gateware
(`sdmm.c`'s own header: "roughly 100 CPU cycles per BIT became roughly
48 per BYTE"), and `sd_set_speed(Z_SPISD_DIV_FAST)` drops the divider to
1 once init succeeds — 12MHz SCLK at 48MHz `wb_clk_i`, so ~1.5MB/s of
raw wire. 19 KB/s is what the *400kHz init* divider would give, which
suggests the measurement predates that change. If it does, there is a
large, cheap win sitting there. If it does not, there is a 70x
software-side overhead worth finding.

Either way: **the build directory should be `/ram`.** Compile in RAM,
write the finished binary to the card. That is what `sw/apps/web` already
does with its page spool, for the same reason.

### 2.5 FatFs is non-reentrant and the scheduler knows it

`docs/filesystem.md`'s `k_no_preempt` deferral means a syscall inside
FatFs is not preempted, capped at `K_NO_PREEMPT_MAX_TICKS` (64 ticks,
~87ms). A compiler doing whole-file `FS_READ` of a large source blocks
`wm` from redrawing for the duration. The chunked API
(`FS_OPEN_READ`/`FS_READ_CHUNK`/`FS_CLOSE`) exists precisely for this;
`zcc` should use it rather than `fs_mallocfile()`.

### 2.6 `printf` costs ~100KB, and that applies to diagnostics

`docs/app_runtime.md` is emphatic: one `%d` in a debug print links the
formatter and costs on the order of 100KB. A compiler is *made of*
diagnostics with line numbers in them. `zcc` needs its own tiny
formatter — a `zcc_err(file, line, col, msg)` that builds the string by
hand and emits with `puts()` — decided up front, not discovered when the
binary triples.

### 2.7 There is a simulator, and it should be part of this

`sim/` runs unmodified app `.bin` images against a software model of the
SOC, with the syscall trampoline intercepted in host code. It currently
models RV32I with no OS, and stubs `UI_PRINT`.

This is the right place to develop `zcc`'s code generator. A codegen bug
found under `sim/main_debug.c` with full processor state is a different
experience from one found by a board that hangs. Extending `sim/` to
handle ZEXE headers, the `Z_SYS_FS_*` syscalls against a host directory,
and RV32M is Phase 0 work that pays for itself immediately.

---

## 3. The compiler

### 3.1 What shape a Zeitlos compiler wants

Given §2.3, the pipeline is:

```
source -> tokens -> AST/IR -> RV32IM machine code in a buffer -> ZEXE
```

No assembler. No linker. No ELF. No relocations. The output is a flat
image at `0x8000_0000`, which is a compile-time constant.

The one genuinely interesting question is **how compiled code reaches
the existing runtime**. An app today gets `printf`, `malloc`, `z_win_*`,
`z_fb_*`, `z_msg_*` by linking `sw/common/*.c` with GCC. `zcc` cannot
compile those files (they are full C99 with `uint32_t` everywhere, and
requiring `zcc` to handle all of `zeitlos.h` on day one is how this
project never ships). Three ways out:

**(a) Compile the runtime too.** Cleanest conceptually, worst
practically: it makes "compile all of `sw/common`" a day-one
requirement.

**(b) A fixed-address runtime blob with a jump table.** Build the
runtime once with the real GCC toolchain into `libz.bin`, placed at a
known offset at the front of every `zcc` output. Its exported entry
points sit in a **jump table at a fixed offset** — `libz.bin + 0x100`
is `puts`, `+0x104` is `malloc`, and so on. `zcc` emits
`jalr` through that table. A generated `libz_syms.h` (produced from
`nm` at build time, exactly the way the app Makefiles already scrape
`_edata`/`_end`) tells `zcc` the indices.

The jump table, rather than raw symbol addresses, is the important
detail: it means rebuilding the runtime does not invalidate every
binary `zcc` ever produced, as long as the table's *order* is stable.
Same discipline `sw/common/syscalls.def` already documents about not
inserting entries in the middle.

**(c) Emit relocations and write a small linker later.** The general
answer, and the one to grow into if `posix` ever wants to load programs
into its own address space (§4.2). Not needed to start.

**Recommend (b).** It gives compiled programs the *entire existing
runtime* — graphics, windows, messaging, filesystem — on day one,
without `zcc` needing to parse a single line of `sw/common`. It directly
satisfies the "ideally using the existing runtime/headers" requirement,
and it converts "what C subset do we need?" from a blocking question
into a gradual one.

The cost is honest and worth stating: every `zcc` output carries a copy
of the runtime blob, so a hello-world is ~40KB rather than ~2KB. On a
32MB board that is not interesting. If it becomes interesting, the blob
moves into the flash core-app archive (`docs/flash_apps.md`) and is
shared.

### 3.2 Survey: is there something to vendor?

The requirement is "very permissive licence, and it obviously solves our
problem". Candidates, with the licence and the disqualifier:

| | licence | rv32 codegen | needs as/ld | verdict |
|---|---|---|---|---|
| **shecc** (sysprog21) | BSD-2 | **yes, direct** | **no** | closest by far — see below |
| **rvcc** (mausimus) | ? | yes, direct | no | shecc's ancestor in spirit; smaller subset still |
| **chibicc** (Ueyama) | MIT | no (x86-64) | yes | full C11 front end, wrong back end |
| **cproc** + **QBE** | ISC + MIT | riscv**64** only | yes | new QBE target + assembler + linker |
| **TinyCC** | **LGPL** | riscv64 only | no | licence is wrong for this tree |
| **lacc** | MIT | no (x86-64) | yes | as chibicc |
| **PCC** | BSD | no | yes | dormant, no rv32 |
| **selfie** | BSD-2 | rv64 subset | no | teaching artefact, tiny C subset |

Only shecc is worth serious attention, so it got measured rather than
guessed at.

#### shecc, measured

Cloned at `master`, built `ARCH=riscv`, run on a host:

| | |
|---|---|
| licence | BSD-2-Clause (NCKU Taiwan) |
| total source | ~29,000 lines; ~22,000 excluding the x86-64 and Arm back ends |
| output | RV32IM Linux ELF, **written directly — no assembler, no linker** |
| self-hosting | yes, bit-identical stage1/stage2 |
| peak RSS, empty file, `--no-libc` | **11.5 MB** |
| peak RSS, `hello.c` with its inlined libc | 11.6 MB |
| peak RSS, compiling its own 29k lines | **68.5 MB** |
| static footprint | 224KB text, 311KB bss |

The architecture is exactly right — it is the §3.1 pipeline, already
built, already self-hosting, under a licence we can use.

**And it is disqualified as a drop-in by its C subset.** Tested
directly:

```
unsigned int x;        -> [Error]: Unrecognized statement token
typedef unsigned int u32;  -> [Error]: Unable to find base type
enum E { A, B };       -> [Error]: Syntax error in global statement
float f;               -> [Error]: Unrecognized statement token
long long x;           -> [Error]: Unrecognized statement token
uint32_t x;            -> [Error]: Unrecognized statement token
```

structs, unions, pointers, function pointers and `switch` all work. But
**no `unsigned` at all** — the project's own Makefile explains why, the
compiler must be compilable by itself and it has no unsigned type — and
no `enum`.

`sw/common/zeitlos.h` is `uint32_t` from top to bottom. Every peripheral
register, every syscall id enum, every struct field. shecc cannot read
the first fifty lines of it. That is not a corner case; it is the
requirement.

The 11.5MB floor on an *empty* file is the second problem. That is fixed
arena allocation (`MAX_IR_INSTR` is 262,144 and `GENERAL_ARENA` reserves
a pointer per entry; `MAX_CODE` and `MAX_DATA` are 256KB each). Roughly
half of it is 8-byte host pointers that would be 4 bytes on rv32, and
the limits are all tunable, so a tuned build could plausibly land near
2–3MB. But it is a tuning exercise on someone else's arena design, and
it does not touch the `unsigned` problem.

#### Verdict

**Build our own — `zcc` — modelled closely on shecc's architecture.**

The BSD-2 licence means specific pieces can be lifted with attribution
where they are genuinely good (the register allocator and the peephole
pass are the obvious candidates), and the whole tree is worth reading
before writing a line. But the front end has to be ours, because the
subset we need is defined by `sw/common/*.h`, not by what a compiler can
compile itself with.

The counter-argument deserves stating: forking shecc and *adding*
`unsigned` and `enum` is less work than a from-scratch front end. It is,
maybe half. What it costs is that every subsequent decision is
constrained by shecc's self-hosting requirement and by upstream drift,
for a compiler whose whole purpose is to match this tree's headers. If
you would rather take that trade, it is a defensible one and Phase 1
below can be re-pointed at it without changing any other phase.

### 3.3 The subset to target

Driven by what `sw/common/*.h` and a plausible app actually use:

**Phase 1 (must have):** `int`/`char`/`short`/`long` and their
`unsigned` forms, `void`, pointers to all depths, arrays, `struct`,
`union`, `enum`, `typedef`, function pointers, all statements including
`switch`/`do`/`goto`, the full operator set including compound
assignment and `?:`, string/char literals with escapes, initialisers,
`static`, `const`, `volatile`, `sizeof`, casts, and a real preprocessor
(`#include`, `#define` with arguments, `#if`/`#ifdef`/`#elif`, `#undef`,
`#pragma once`).

**Phase 2 (should have):** variadic functions (`printf` is unusable
without them — until then, calls go through the jump table to the
GCC-built runtime, which is another point in (b)'s favour), bitfields,
`long long` via runtime helper calls, designated initialisers,
compound literals.

**Explicitly out, indefinitely:** `float`/`double` (soft-float via
runtime calls if ever needed), VLAs, `_Generic`, atomics, threads,
`inline` semantics beyond "ignore it".

That subset compiles `hello_win.c`, `clock.c`, `calc.c` and most of
`sw/common`. It does not compile `web` or `net`, and that is fine —
the stated goal explicitly does not require rebuilding the whole tree.

### 3.4 Memory budget

shecc's numbers are the warning. The reason an *empty* file costs 11.5MB
is fixed-size arenas plus re-parsing an inlined libc on every
invocation. `zcc` should instead:

- size arenas from the input file's size, not from a constant
- stream tokens through the parser rather than materialising the whole
  token list
- keep the preprocessor's macro table a hashmap, not a linear array
- hold one function's IR at a time where possible, emitting code
  per-function rather than after the whole translation unit

Target, for a 2,000-line source with 10 headers:

| | |
|---|---|
| source + headers, in memory | ~200KB |
| macro/symbol tables | ~100KB |
| IR for the largest single function | ~200KB |
| output code + data buffer | ~256KB |
| runtime blob | ~64KB |
| slack | ~200KB |
| **total** | **~1MB** |

Which means a 4MB heap tier is generous and an 8MB board is genuinely
enough — for compiling ordinary apps. Self-hosting (`zcc` compiling
`zcc`) is the case that will need more; shecc needed 68MB for 29k lines
in one translation unit, and the mitigation is to not do that — `zcc`
should be many files with a real (if trivial) archive step, not one
`#include`-everything `main.c`.

---

## 4. The POSIX layer

### 4.1 What "POSIX" should and should not mean here

It must not mean real POSIX process semantics in the kernel.
`fork`/`exec`/`wait`/signals/uids/ptrace is a kernel rewrite, it would
land squarely in `sw/os`, and the stated constraint is to keep this out
of the core system.

It should mean: **a Unix-shaped userland living inside one Zeitlos
process.** File descriptors, a current working directory, a VFS with
`/dev` and `/proc`, a shell with pipes and redirection, environment
variables, argv, exit statuses, and a set of small tools. That is what
makes a C compiler pleasant to drive, and it is all achievable in
`sw/apps/posix` with at most one new syscall.

Concretely, `posix` is a port provider (`docs/ports.md`) registering as
`posix0`, exactly parallel to `repl`. `term` reaches it through
`zconnect.h` with a new target name (**not needed in the event** --
section 9d), so `repl` and `posix` coexist and
`term`'s F11 bar picks between them. Nothing about `repl` changes.

### 4.2 The process model — three options

**Option A: everything in-process.** Builtins and loaded programs run
inside `posix`'s own address space. Pipes are ring buffers. Job control
is cooperative. No isolation at all — a segfaulting `ls` takes the
shell with it.

**Option B: real Zeitlos processes.** Each command is
`Z_SYS_PROC_RUN`; pipes are `zport`/`zstream` connections. Real
isolation. But `Z_PROCS_MAX` is 16 total for the whole machine,
`Z_MEM_MIN_BLOCK_SIZE` is 32KB so `ls | grep | wc` costs 96KB minimum
plus three process-table slots, and each one pays a `fs_load_exec()` off
the card.

**Option C: hybrid — recommended.** Builtins and small tools run
in-process (Option A). A `posix`-side `exec` that recognises a real
Zeitlos app spawns it properly (Option B) and bridges its port. Pipes
work between in-process commands, which is where they are actually used.

For loading in-process programs, Option A needs either a fixed load
address inside `posix`'s heap (one program at a time) or §3.1's option
(c), relocations. Start with the former: `zcc -m posix` emits an image
based at a `posix`-declared address. That is a compiler flag, not an
architecture.

### 4.3 `fork()` is possible, and still a bad idea

Worth writing down because the MTU makes it look easy, and it nearly is:
allocate a block of the same size, `memcpy` the parent's, copy the
register file, set the child's return to 0. The child then runs at
`0x8000_0000` like every other process and sees an identical address
space. No relocation needed. This is a genuinely unusual property of
this architecture and it is tempting.

Against it: no copy-on-write, so every `fork` is a full memcpy of the
parent (megabytes, for a compiler); no shared file-descriptor table
across the boundary, so the semantics that make `fork` useful in a shell
are exactly the ones we would not have; and a 16-slot process table.

Provide `posix_spawn()`-shaped semantics instead and do not pretend.
Document the omission loudly, because "it's Unix-like but there is no
`fork`" is the sort of thing that must be found in a README rather than
by a program that hangs.

### 4.4 What lands where

| | |
|---|---|
| `sw/apps/posix/` | everything: VFS, fd table, shell, tools |
| `sw/common/zconnect.c` | one new target name — **not needed**, see 9d |
| `sw/os/kernel.h` | one new stack/heap tier (§2.1) |
| `sw/apps/Makefile` | one word in `APPS` |

That is the whole core-system footprint. It is deliberately close to
zero.

---

## 5. The filesystem

### 5.1 Rank the requirements before picking

What does a self-hosting C development box actually need from its
filesystem?

| need | FAT has it? |
|---|---|
| long filenames | yes (LFN) |
| directories, nesting | yes |
| create/read/write/truncate/seek/delete/rename | yes |
| mtime | yes |
| **an executable bit** | **no** |
| **symlinks** | **no** |
| ownership / permissions | no — single-user machine, do not care |
| hardlinks | no — nothing needs them |
| >4GB files | no — nothing needs them |
| case sensitivity | no — `sh.c` uppercases paths today |

Two real gaps, and both have cheap workarounds: the exec bit can come
from the ZEXE magic (`fs_exec_info()` already reads it — "is this
executable?" is answerable without a permission bit), and symlinks can
be a text file with a magic prefix resolved in the `posix` VFS, which is
what a lot of small systems do.

**The honest conclusion is that FAT is about 90% adequate**, and that
replacing it is a want rather than a need. Which argues strongly for
doing it last, when there is a real user of the missing 10%.

### 5.2 Options

**1. FAT plus a POSIX shim in `sw/apps/posix`.** Zero new core code.
Host-mountable on every operating system, which matters enormously for
getting sources on and off the card. No permissions or symlinks without
the workarounds above. Case-insensitive, which will surprise someone.

**2. ext2 via lwext4.** BSD-3-Clause except `ext4_xattr.c` and
`ext4_extents.c`, which are GPLv2 and which an ext2-only build does not
need — they are removed, and the project documents this as the supported
way to use it as BSD-3. Reported ~20KB of text for ext2-only on a
Cortex-M4. Real inodes, symlinks, permissions, hardlinks. Loop-mountable
on Linux, which keeps the host workflow. No journaling in ext2, so a
power cut means `fsck` — and there is no `fsck` on the device.

**3. littlefs.** BSD-3, power-loss resilient by design, small RAM.
But no symlinks and no permissions, so it does not actually deliver the
"unix-like" part; it is designed for raw flash, where its log structure
earns its keep, and on an SD card that structure is pure cost; and it is
not host-mountable without `littlefs-fuse`, which is a real ergonomic
tax on a machine whose sources arrive from a laptop.

**4. Roll our own.** Most work, least payoff, and it would be the one
filesystem in the world with no host tooling.

**5. Root filesystem as a file on the FAT card.** The option raised as
"probably not ideal". It is not ideal, but it is not as bad as it looks:
it is still loop-mountable on the host, it keeps FAT as the drag-and-drop
surface, and it needs no partitioning. What it costs is a second layer of
indirection on every block access, on a machine where §2.4 says block
access is already the bottleneck. **A second partition on the same card
is strictly better** and costs nothing extra: `diskio_mux.c` already
dispatches by drive number (0 = SD, 1 = RAM), and adding a partition
offset to the SD backend is a smaller change than an image-file backend
would be.

### 5.3 Recommendation

**Do nothing to the filesystem until Phases 1–5 are done.** Build on
FAT. Let `posix`'s VFS be the abstraction boundary — if it is written
against `sw/common/zfs.h` cleanly, swapping what is underneath later is
a contained change.

When it does become worth doing: **lwext4, ext2-only, as a second
volume**, mounted from a second partition on the same card, with FAT
retained as the boot and exchange partition. `docs/ramdisk.md` already
anticipated this exact shape — "It is a small mount table rather than a
hardcoded comparison, because flash-as-a-volume is the obvious next one
and app-facing syntax should not have to change again for it".

The one thing to be careful about: lwext4 is not FatFs, so it does not
slot into `diskio_mux.c`. It needs a small VFS shim above `fs.c`, and
*that* is a core change. Keep it behind a build flag, keep it small, and
write it after `posix` has proven what it actually needs.

---

## 6. The Linux-under-emulation alternative

Worth taking seriously enough to do the arithmetic, because the answer
is clear once you do.

### 6.1 Native Linux

Needs Sv32 paging, supervisor mode, CLINT and PLIC. PicoRV32 has none of
these and is not going to grow them. `zeitlos32` (`docs/zeitlos32.md`) is
a nine-state multicycle RV32IM core that does not yet run on hardware;
adding an MMU, S-mode and the interrupt controllers to it is a real,
bounded, but substantial FPGA project — and it would roughly double the
core's size on an ECP5 that is already carrying a GPU and a framebuffer.

It also changes what Zeitlos *is*. A Zeitlos that boots Linux is a Linux
board, and `wm`, the GPU, the port system and every app in `sw/apps`
become things that have to be ported to it.

### 6.2 Emulated Linux

`mini-rv32ima` (cnlohr) is ~400 lines, MIT, no libc dependency, and
demonstrably boots Linux — `pico-rv32ima` does it on an RP2040 with SPI
PSRAM for guest memory. Vendoring it into `sw/apps/rvemu` would be a
genuinely small piece of work.

The problem is throughput. An RP2040 is a Cortex-M0+ at 133MHz (often
overclocked in that project); PicoRV32 at 48MHz is on the order of
**seven times slower**. Add the interpreter's own overhead — tens of host
instructions per guest instruction — and a Linux boot that takes a
couple of minutes there is plausibly **fifteen to thirty minutes here**,
with every subsequent `ls` taking seconds. Guest RAM would be 8–16MB out
of a 32MB board, so an 8MB board is out entirely.

"Hardware accelerated" does not have an easy answer either. The thing
that would actually help is not a coprocessor but an MMU and S-mode in
the CPU — which is §6.1, i.e. stop emulating and run it natively.

### 6.3 Verdict

**Not the path to an on-device C compiler.** It is slower, it needs more
memory, it gets you a Linux with no access to the GPU, the window manager
or the port system, and the C compiler you would run under it is GCC,
which will not fit or perform.

It is, however, a *great demo* and cheap. Keep it as an optional
low-priority side-quest (`sw/apps/rvemu`) — "Zeitlos boots Linux" is a
good screenshot and costs a weekend, provided nobody mistakes it for
infrastructure.

---

## 7. Phased plan

Each phase has a deliverable that stands on its own. Phases 1–3 produce
a usable feature with no POSIX layer; 4–6 produce the POSIX layer; 7 is
the filesystem, deliberately last.

### Phase 0 — groundwork — DONE

Small, independently useful, needed regardless of every decision below.
See section 9 for what actually happened, including three simulator
gaps that turned out to be load-bearing rather than cosmetic.

1. **Re-measure SD throughput** (§2.4) and correct or confirm
   `docs/ramdisk.md`'s 19 KB/s. If it is stale, this may be the single
   highest-value hour in the whole project.
2. **Add a large heap tier.** `Z_PROC_STACK_SIZE_HUGE` (4MB), granted to
   `zcc` and `posix` by name in `z_proc_stack_size_for()`, with a memory
   check so it degrades to a clean refusal on a small board rather than
   an allocation failure misread as something else — the failure mode
   `docs/app_runtime.md` records for `k_proc_create()`'s `Z_FAIL`.
3. **Extend `sim/`**: ZEXE header parsing, RV32M, and the `Z_SYS_FS_*`
   syscalls backed by a host directory. This is where `zcc` gets
   developed and where its test suite runs.

*Acceptance:* `sim/` runs an existing ZEXE app that reads a file.
*Docs:* update `docs/ramdisk.md`, `sim/README.md`; note the tier in
`docs/app_runtime.md`.

### Phase 1 — `zcc`, cross-hosted — DONE

`zcc` builds and runs **on the development host**, reads C, writes a
ZEXE image. Lexer, preprocessor, parser, IR, RV32IM code generator, ZEXE
writer. The §3.3 Phase-1 subset. Diagnostics with file/line/column.

*Acceptance, as met:* a differential suite of eight programs, each
compiled by both `zcc` and `riscv64-unknown-elf-gcc` and run under
`sim/zsim-headless`, with byte-identical output required. The last of
them includes the tree's own `sw/common/zeitlos.h`. Section 9a has the
result. There is no IR, for the reason `docs/zcc.md` gives.
*Risk, as handled:* the code generator, mitigated by `sim/` and by
comparing against GCC rather than against a golden file.
*Docs:* `docs/zcc.md`.

### Phase 2 — `libz`: the runtime blob and jump table — DONE

Build `sw/common`'s app-facing runtime into a fixed-position blob with a
stable jump table, generate `libz_syms.h` from it at build time, and
teach `zcc` to call through it.

*Acceptance, met:* a `zcc`-compiled program calls `printf`, `malloc`,
the filesystem, the framebuffer and the window manager through the
runtime, and matches GCC byte for byte -- including a checksum of the
framebuffer after drawing. What is NOT verified is that a window
appears on a screen: `sim/` has no `wm`, so that needs the board.
*Risk:* table-order discipline. Mitigated the way `syscalls.def` is:
a comment that says so, and a build-time check that the table did not
shrink or reorder.
*Docs:* `docs/libz.md`.

### Phase 3 — `zcc` on the device — DONE

Port `zcc` to run as a Zeitlos app. Chunked file I/O (§2.5), hand-rolled
diagnostics formatting (§2.6), `/ram` as the build directory (§2.4),
memory budget per §3.4.

*Acceptance, met under `sim/`:* the device compiler builds all eleven
test programs and produces output **byte-for-byte identical** to the
host build, and every one of those binaries runs. **This is the
milestone that makes the machine self-extending**, and it arrives
before any of section 4 or 5 exists. What it still needs is the board:
a real card, a real scheduler, and a 4MB allocation against a live
pool. See section 9c.
*Docs:* `docs/zcc.md` updated with the on-device memory profile.

### Phase 4 — `posix` skeleton — DONE

A port provider registering as `posix0`. VFS over `zfs.h`, fd table,
cwd, environment, argv, a shell with redirection, and a first handful of
builtins (`cd`, `pwd`, `echo`, `ls`, `cat`, `exit`). `term` reaches it
via `zconnect`.

*Acceptance, partly met:* the shell runs against a real filesystem and
behaves like one, checked by `sw/apps/posix/tests`. The port half
compiles but cannot be linked or run here -- see section 9d.
*Docs:* this section, and `sw/apps/posix/posix.h`.

### Phase 5 — userland — 5.1 and 5.2 DONE, rest in progress

Broken into 5.1-5.5 once the tree's existing mechanisms were read
properly; see section 9e. 5.1 (child lifecycle) and 5.2 (terminal
handoff) are done.


Pipes between in-process commands, `|`/`>`/`>>`/`<`, exit statuses,
`&&`/`||`, and the tools that make a compiler usable: `cp`, `mv`, `rm`,
`mkdir`, `grep`, `head`, `tail`, `wc`, `find`, and a `make`-shaped
build driver. Hook `te` (`docs/editor.md`) in as `$EDITOR`.

*Acceptance:* edit, compile, run, without leaving `posix`.

### Phase 6 — self-hosting

`zcc` compiles `zcc`. Requires §3.3's Phase-2 subset and probably a
trivial archive/multi-file story per §3.4.

*Acceptance:* stage1 and stage2 binaries byte-identical, the same bar
shecc sets for itself.

### Phase 7 — the filesystem

Only now, and only if Phases 4–6 produced real complaints that a
filesystem would fix. lwext4, ext2-only, second partition, VFS shim above
`fs.c`, behind a build flag.

*Docs:* `docs/filesystem.md` gains an ext2 section.

### Phase 8 — optional: `rvemu`

`mini-rv32ima` as an app. Fun, cheap, not infrastructure.

---

## 8. Decisions taken

Settled. Recorded here rather than in a commit message, because every
one of them is a thing somebody will otherwise re-litigate in six
months.

1. **`zcc` is written from scratch, clean-room, under this tree's own
   licence.** Not a shecc fork, despite the measurement in section 3.2
   that a fork would be roughly half the work. Two reasons: the licence
   stays uniform, and -- the larger one -- a compiler we wrote and
   documented ourselves is a different asset from one we inherited,
   most of all if it ever self-hosts.

   **Clean-room means clean-room.** What was looked at during the
   survey was: the licence, the build, the arena-size constants in
   `defs.h`, the CLI options in `main.c`, and measured memory and
   timing figures. Not the lexer, parser, SSA or code generator, and
   they are not to be read. `zcc` is designed from published
   compiler-construction material and the RISC-V specification.

   The one thing carried over is architectural, and forced by ZEXE
   anyway: **direct to machine code, no assembler, no linker**.

2. **Runtime access via the fixed blob and jump table** (section
   3.1(b)). Relocations and a mini-linker (3.1(c)) stay available for
   later if `posix` wants in-process program loading.

   The jump table is to be designed **shared-ready from day one** --
   see section 10 -- while the first implementation ships a copy per
   output.

3. **Minimum board is 8MB.** Design against it, test on 32MB. Obst is
   out, and says so rather than failing obscurely.

4. **`posix` sits beside `repl`, it does not replace it.** `repl` is
   Scheme and works. `term`'s F11 bar picks between them.

5. **Self-hosting is a real goal**, so section 3.3's Phase-2 subset and
   section 3.4's multi-file structure are designed for from the start
   rather than retrofitted.

6. **The compiler is `zcc`.** The Z80 collision is real and
   survivable: it is a different world, several projects already share
   the name, and `zcc` matches this tree's own `z`-prefix convention
   (`zar`, `zport`, `zline`, `zsvg`) in a way `zlcc` does not.

---

## 9. Phase 0: what actually happened

Done. Where it differs from the plan in section 7, the difference is
noted.

### 0.1 The SD card

**A large cause found by reading, and the rest still unmeasured.**

`sdmm.c` is ChaN's *bit-banged* sample driver, and both of its wait
loops still slept 100us between polls -- a number sized against a poll
byte costing about 17us of bit-banging, against 0.67us now. Worse,
`rcvr_datablock()` runs once per **sector** even inside a CMD18
multi-block read, so the streaming path that is supposed to be fast
paid it every time.

Both loops now poll tightly, with `rdcycle`-measured timeouts. That
second half is not cosmetic: the old loops expressed 500ms and 100ms
as *iteration counts* calibrated against the sleep, so removing the
sleep would silently have turned 500ms into about 3ms. A driver whose
timeouts shrink by 150x when somebody deletes a delay is a trap.
`SD_POLL_TIGHT=0` restores the old behaviour for A/B measurement from
one build flag.

This accounts for perhaps 100-200us per sector against an observed
excess of about 25ms, so it is **not** the answer. `sdbench` (a kernel
shell command, `sw/os/fs/sdbench.c`) measures four layers -- raw
`spi_xchg` with CS deasserted, single-block CMD17, multi-block CMD18,
and `f_read` through FatFs -- with poll and command counters, so the
ratios between adjacent layers say which one is responsible.

The full analysis, the ranked hypotheses and how to read the output
are in **`docs/sdcard.md`**. The one worth knowing before spending
effort on a faster SPI clock: layer 0 runs at DIV=1 *and* DIV=0, and
**if they measure the same, the CPU's bus access is the limit and
24MHz buys nothing.**

### 0.2 The heap tier

`Z_PROC_STACK_SIZE_HUGE`, 4MB, granted to `zcc` and `posix` by name in
`z_proc_stack_size_for()`. `sw/os/kernel.h` carries the reasoning and
the three consequences: it cannot fit on a 1MB board and is not meant
to, it is most of an 8MB one, and a 4KB-aligned first-fit 4MB request
can fail on fragmentation while 4MB is nominally free.

### 0.3 The simulator

More than planned, because three of the gaps turned out to be
load-bearing rather than cosmetic.

- **RV32M.** The core was RV32I only, on the once-true grounds that
  the SOC built picorv32 without M. `arch.mk` now defaults to
  `rv32im`, so the simulator could not run anything the current
  toolchain produces.
- **The counter CSRs read as zero**, and `dly_us()` spins until
  `rdcycle` reaches a target -- so any path through it **hung
  forever**. Both counters now return the retired-instruction count,
  which works as a monotonic clock and is transparently not a
  performance figure.
- **Syscall return values.** Every app-side wrapper dereferences the
  returned pointer; the simulator returned 0. That appeared to work
  only because `Z_OK` is 0 and unwritten low memory reads as 0 -- so
  every syscall silently looked successful and none could report
  failure at all. Fixed with two real `z_obj_t`s in low memory.
- **ZEXE loading**, including zeroing `.bss` from the header, because
  nothing else will -- there is no crt0 on this OS.
- **Syscall ids generated from `syscalls.def`** by the same X-macro
  the real enum uses, rather than hand-copied. The old list stopped at
  `UART_TX_FULL`, and everything past it fell through to
  "unimplemented".
- **`sim/simos.c`**, a host-backed OS layer: the whole `FS_*` family
  including chunked I/O, plus uptime, pids and messaging. `--root DIR`
  backs the guest filesystem with a host directory, refusing `..`
  escapes and doing FAT-style case-insensitive resolution -- `sh.c`
  passes paths uppercased, and a case-sensitive host would miss
  exactly where hardware hits.
- **`zsim-headless` has real options and a real exit status** (2 =
  illegal instruction, 3 = ECALL/EBREAK). It returned 0
  unconditionally, which made a binary that executed one illegal
  instruction indistinguishable from one that ran to completion.

Verified end to end with a freestanding RV32IM ZEXE image: `.bss`
zeroed, `mul`/`div`/`rem`/signed division all correct, `rdcycle`
advancing, `FS_SIZE`/`FS_READ`/`FS_LIST` against a host directory,
uppercase paths resolving to lowercase files, and `../../etc/passwd`
refused.

See `sim/README.md`.

---

## 9a. Phase 1: what actually happened

`zcc` exists, builds on the host, and emits ZEXE images that run.
4,600 lines across six files. The full writeup is `docs/zcc.md`; what
follows is what a reader of this document needs.

### It reads `zeitlos.h`

This was the bar set in section 3.3, and it is met. A program that
`#include`s `sw/common/zeitlos.h` compiles and produces byte-identical
output to the same program built with GCC -- the register map, the
X-macro'd syscall enum, `z_obj_t` with its `float` member, macros
three deep, all of it.

Two things had to give way for that, and both are more interesting
than the features they enabled:

- **`float` and `double` exist as SIZES, not as numbers.** `z_obj_t`'s
  union has a `float float32` member, and without a 4-byte type of
  that name the most important header in the tree cannot be read at
  all. So a float can be declared, be a member, be assigned and have
  its address taken; arithmetic on one is a clean error rather than
  integer arithmetic on its bit pattern.
- **The preprocessor had to interleave expansion with directives.** It
  was two passes -- all directives, then all macros -- which is simpler
  and wrong. `#define Z_MKSYSCALL` / `#include "syscalls.def"` /
  `#undef Z_MKSYSCALL` breaks under it: the `#undef` runs before
  anything is expanded, so the enum will not parse. The X-macro idiom
  is load-bearing in this tree, and a preprocessor that handles
  `#define` and `#include` and nothing else looks finished right up
  until it meets one.

### The boundary, named

One function in `zeitlos.h` does not compile: `maskirq()`, which wraps
a raw picorv32 custom instruction in `__asm__`. `-U__riscv` takes the
header's own `#else` branch, which is right for a host build and
**wrong on target**, where it silently disables the atomicity
`z_fb_hw_line()` and the ENC28J60 driver depend on.

The Phase 2 answer is better than building an inline assembler:
`maskirq` becomes a jump-table entry, compiled once by GCC, selected
by the header under `#ifdef __zcc__`. One entry, no new compiler
feature -- and the same treatment covers anything else that turns out
to need an instruction `zcc` cannot emit. Worth knowing that this
category exists and that it has a cheap answer, because the instinct
is to reach for an assembler.

### The code is about 4.5x GCC's size

Measured across the suite against `gcc -O1`. That is the price of an
accumulator model with no register allocator, no liveness analysis and
no spilling -- three things individually harder than the rest of the
compiler put together, and whose bugs appear as wrong answers rather
than as crashes.

It is a contained later job. A peephole pass over the emitted stream
and a shallow register stack would recover most of it, and neither
touches the front end; `docs/zcc.md` names the specific patterns.

**This matters more for Phase 6 than for anything before it.** It does
not change section 3.4's estimate of about 1MB to compile a 2,000-line
file, which is about the compiler's own data structures rather than
its output. It does mean a self-hosting `zcc` compiles a 4,600-line
program into something several times larger than GCC would -- worth
knowing before that phase starts, not after.

### The differential suite earned its keep immediately

Two bugs on the first run, both silent:

- **Plain `char` is unsigned on RISC-V**, per the psABI. `zcc` had it
  signed, on the x86 assumption. Nothing fails to compile; the only
  programs that notice are ones comparing a `char` against a negative
  sentinel, which is every `getchar()`-style loop in existence.
- **An empty macro ate the rest of the file.** `#define EMPTY`
  produced a bare end-of-stream sentinel that was spliced into the
  token stream and stopped the rescan dead. It presented as
  "unterminated function body" forty lines later.

Neither would have been caught by a golden-file suite written at the
same time as the compiler, because the golden file would have recorded
the wrong answer and called it correct.

### A note on indentation

`sw/apps/zcc` is space-indented, per `README.md`: LLM-assisted code in
this tree is space-indented so that it is visible as such until it has
been audited and converted. **The Phase 0 changes were tab-indented to
match their surroundings, which was the wrong call** -- they should be
spaces too by that rule, and converting them is one `expand` away if
you want the consistency now.

---

## 9b. Phase 2: what actually happened

`libz` exists. A `zcc`-compiled program calls `printf`, `malloc`, the
string functions, the filesystem syscalls and `maskirq`, and produces
byte-identical output to the same program built with GCC against the
same runtime. `docs/libz.md` is the writeup; what follows is what a
reader of this document needs.

### The mechanism works, and it is small

Blob at `.text` offset 0, linked at `0x8000_0000` and landing there,
so every absolute address inside it is right with no relocation. A
call is `jal ra, table + 4*i` into a slot holding `j real_function`:
**one extra instruction, no register clobbered.** Two words are
patched by the compiler -- `main`'s address and the image's `_end`.

| | |
|---|---|
| the whole runtime | **35,184 bytes** (5,472 before graphics) |
| hello-world with `printf`, complete image | **35,844 bytes** |
| a windowed app, complete image | **35,916 bytes** |
| table entries | 97 |

### `maskirq` is solved, and generally

The one function Phase 1 could not compile is now table entry 48:
GCC-built code behind an ordinary slot. **No inline assembler, now or
ever** -- and the same treatment covers anything else `zcc` cannot
express. That is the finding worth carrying forward, because the
instinct on meeting a `__asm__` is to build an assembler, and this is
cheaper by two orders of magnitude.

### It is not newlib, deliberately

The original sketch in section 3.1(b) said "build the runtime once
with the real GCC toolchain" and meant `sw/common` plus newlib. That
is not what got built, and the reason is section 2.6 of this document:
newlib's formatter costs ~100KB and anything touching a `FILE` another
~40KB, to the point that this tree's own advice is to format numbers
by hand rather than call `printf`.

`zcc` output is already 4.5x GCC's size. A hello-world at 105KB would
not fit the space the loader has for it. So libz is a purpose-built
runtime -- the subset the tree actually uses, sized for the machine --
and its `printf` handles the full flag/width/precision set in about
1KB.

**This is a change from the plan and it should be read as one.**
Section 3.1(b) assumed the runtime and the tree's existing runtime
were the same object. They are not, yet: `sw/common/zeitlos.c` is
written against newlib and defines `_read`/`_write`/`_sbrk`, so
sharing it would drag newlib in behind it. Reconciling the two is
what section 10 is really about, and it is now clearer that it is a
change to `sw/common` rather than a packaging exercise.

### Graphics went in, and the "reconciliation" was mostly imaginary

This section first said graphics needed a decision: reimplement them
in libz, or change `sw/common` first. **That was wrong, and it was
wrong because I had not tried it.** Four of the five files --
`zgfx.c`, `zwin.c`, `zobj.c`, `zkbd.c`, `zfont_data.c` -- compile into
the runtime with **no change to `sw/common` at all**. What they needed
was six small shim headers supplying a freestanding `<stdio.h>`,
`<stdlib.h>`, `<string.h>`, `<stddef.h>`, `<math.h>` and `<stdarg.h>`,
declaring only the names those files actually call.

The one real obstacle was `zeitlos.c`, which defines newlib's
`_read`/`_write`/`_sbrk` and would drag a libc in behind it. That is a
genuine exclusion and libz supplies those wrappers itself. Everything
else was generalised from that one file without checking.

`sw/common` did change, by four lines: a `#if defined(__zcc__)` arm
around `maskirq` in `zeitlos.h`, declaring it rather than defining it
as a `static inline` full of assembly. That removed the need for
`-U__riscv`, which had been selecting the host stub that returns 0 --
silently disabling the atomicity `z_fb_hw_line()` and the ENC28J60
driver depend on. **A wrong answer that compiles is worse than a
compiler that cannot read the header.**

The blob went from 5.5KB to 35KB and every zcc binary carries a copy.
Accepted deliberately: section 10 turns it into a one-time cost later,
and building a windowed app on the device is worth more now.

### The rule that fell out: the runtime provides the tree's OWN names

`fs_mallocfile()`, not a parallel `fs_read()`. `z_win_create()`,
`z_fb_draw_text()`, `z_msg_send()` -- same spellings, same signatures,
same headers.

This began as taste and became a hard rule after `libz.h` declared
`z_getpid()` as `unsigned` where `zeitlos.h` says `uint32_t`. Those
are the same type until the include path changes, and then they are a
compile error in whichever file is unlucky. `libz.h` now **includes**
`zeitlos.h` and `zfsapp.h` rather than shadowing them.

The payoff is larger than the bug avoided: a program moving between a
GCC build and a zcc build does not change a single call. That is what
resolving names at declaration time buys -- no special header, no
annotation, no second API.

### The ABI discipline is mechanical now

`libz_table.def`'s order **is** an ABI: inserting anywhere but the end
shifts every later index, and a stale binary then calls the wrong
function -- `printf` where `malloc` was expected, silently. Same hazard
`syscalls.def` documents, and its own history records `HID_READ_KEY`
having to be moved for exactly this reason.

`LIBZ_ABI_VERSION` is stamped into the blob and into `libz.sym`, and
`zcc` **refuses** a mismatch rather than warning. That is the part
worth having: a warning about this would be read past.

### Two things the suite caught

- **A static local array could not size itself.** `static const char
  s[] = "..."` failed with "incomplete type", because the self-sizing
  logic lived only in the global path. Found by the first libz test
  that wanted a string table inside a function.
- **The simulator had no `maskirq`.** The first `zcc`-compiled program
  to call it trapped on an illegal instruction. `sim/cpu.c` now
  implements picorv32 opcode `0x0b`; there are no interrupts there, so
  the mask is a register and nothing else, which is faithful for every
  caller in the tree.

The libz tests are a stronger check than they look, because the two
builds reach the runtime by completely different routes -- one through
a patched blob and a jump table, the other through an ordinary link --
while the runtime code itself is the same object either way. Any
difference is `zcc`'s calling convention and nothing else.

---

## 9c. Phase 3: what actually happened

`zcc` runs as a Zeitlos app. 80KB, GCC-built, linking libz's objects
directly -- the blob and the jump table are for what zcc *produces*,
not for zcc itself. `docs/zcc.md`, "On the device", is the writeup.

Under `sim/` it compiles all eleven test programs, produces output
byte-for-byte identical to the host build, and every binary runs.
`tests/run_dev.sh` checks exactly that, and the byte-identity is the
claim worth making: the two builds share every line of the compiler
and differ only in four functions, so an identical image means the
seam is genuinely the only difference.

### The seam is four functions

Read a file, write a file, print, stop -- `zio_*` in `zcc_port.h`,
implemented by `port_host.c` against stdio and `port_dev.c` against
libz. Not `#ifdef`s through the lexer and the emitter, because the
device build is the one that matters and the host build is the one
that gets tested; scattered conditionals would make the tested code
and the shipped code diverge in the files where a difference is
hardest to see.

### Two bugs the host build could not have found

**`fs_size()` returns 0 for a missing file AND for an empty one.**
`sw/os/fsapi.h` says so and calls it deliberate. The first
`zio_read_file()` read that 0 as success, so when the preprocessor
probed for a header by trying each directory in turn, the first
candidate that *did not exist* succeeded with no content --
`#include "libz.h"` expanded to nothing, silently, and the program
failed with `'printf' is not declared` on an innocent line.
`fs_open_read()` is the unambiguous test. `fopen()` on the host tells
the truth, so this was unreproducible there.

**`zalloc` was quadratic on libz's malloc.** The first device compile
of a ten-line file took **729 million instructions** -- libz's malloc
is a first-fit free-list walk, fine for an app with a few dozen
allocations and ruinous for a compiler making one per token. A bump
arena took it to **10.3M, a 70x improvement**, and costs nothing
because this allocator already never frees.

Worth noting what that says about libz: its allocator is right for
apps and wrong for compilers, and the fix belonged in the client
rather than in the runtime. A general-purpose malloc good at both
would be a much larger thing than 5KB.

### The numbers land where section 3.4 estimated

| | instructions | arena |
|---|---|---|
| 10 lines, including `zeitlos.h` | 10.3M | 1,024 KB |
| 200 lines, no system headers | 6.9M | 384 KB |
| with `zeitlos.h` + `zgfx.h` + `zwin.h` | 23.1M | 1,984 KB |

About 1MB for a small file with headers, under 2MB for a substantial
one, against the 4MB tier from Phase 0. Roughly a second and two
seconds respectively at 12 MIPS -- estimated, not measured.

**The cost is dominated by headers, not by the program.** 200 lines
with no `#include` cost 384KB and 6.9M instructions; ten lines with
`zeitlos.h` cost 1MB and 10.3M. That is the number to attack if
compile time ever becomes the complaint, and it argues for a
precompiled-header-shaped answer rather than for a faster parser.

### Arguments, for now

A Zeitlos process is started by name and carries no arguments, so the
device build reads its command line from the launch argument
(`z_launch_arg_take`) and failing that from `/zcc.args`. The file is a
stopgap; passing a real argv is what Phase 4 is for.

### Self-hosting is blocked on one feature

`zcc` cannot yet compile `zcc`: `parse.c` uses designated initialisers
(`.kind = TY_VOID`) in its own type table, and those are on the
not-supported list. That is Phase 6's first job and it is a bounded
one -- the alternative, rewriting those tables, would work today but
would be avoiding the feature rather than adding it.

---

## 9d. Phase 4: what actually happened

`sw/apps/posix` exists: a port provider registering as `posix0`, a
VFS with a cwd and a descriptor table, and a shell with redirection,
chaining and eleven builtins.

### Zero core changes -- fewer than section 4.4 predicted

Section 4.4 budgeted one new target name in `sw/common/zconnect.c`.
**That was not needed.** `prep_port()` takes an arbitrary name and
passes it through with no lookup and no fixed list, so `posix0` is
reachable three ways with nothing added:

- `port posix0` from a `repl` prompt;
- **F11 in `term`, then `open> port posix0`** -- the same
  `z_conn_prepare()` path, differing only in who initiates it;
- `Z_TERM_SET_PORT` from any other provider.

`repl` is untouched and the two coexist exactly as intended.

So the whole core-system footprint of Phase 4 is: nothing. The heap
tier from Phase 0 already names `posix`.

### The split that made it testable

`sh.c` and `vfs.c` contain no port, message or window code at all --
that is entirely `main.c`'s job. `px_exec_line()` takes a line and an
output callback and knows nothing else.

That was arranged rather than discovered, and it paid immediately:
the shell is driven from a script against a host filesystem
(`tests/`), on a machine with no Zeitlos anywhere. Given that the
container this was developed in cannot even LINK a Zeitlos app -- its
RISC-V toolchain has no newlib, which is the same limitation that made
libz necessary -- the alternative was shipping the shell untested.

It is a golden-file test, not a differential one like zcc's. There is
no second shell to compare against, so the trade runs the other way:
it catches regressions well and cannot catch a behaviour that was
wrong from the first run. The transcript is meant to be READ when it
changes.

### Three bugs, one of them destructive

- **`>>` replaced the file with NUL padding.** Append was implemented
  as open-for-write then seek to the end. `fs_open_write()`
  TRUNCATES, so the seek moved past a file that was now empty and
  FatFs filled the gap with zeros: `echo a > f; echo b >> f` gave
  eight NULs and then `b`. `fs_open_rw()` does not truncate and is the
  right call. Quiet, total, and caught on the first run of the suite.
- **`ls` was unsorted**, because FatFs returns creation order. Beyond
  being hard to scan, it made `ls` output depend on the history of the
  card.
- **`cd ..` at the root failed** rather than staying put. Every shell
  stays; failing is surprising for nothing gained. Note that escaping
  is still impossible -- `..` at the root is now a no-op, so
  `../../etc/passwd` normalises to `/etc/passwd` and simply does not
  exist.

### zcc's argv stopgap is resolved

`zcc hello.c -o hello` at this prompt sets the launch argument
(`z_launch_arg_set`) and starts the program, so zcc reads a real
command line rather than `/zcc.args`. That was the one thing Phase 3
left hanging, and it needed no change to zcc at all -- it already
preferred the launch argument.

Typing `run` is not required: a name that is not a builtin is run as a
program.

### What is NOT verified

`main.c` -- the port protocol, connection handling, the line editor
and output batching -- compiles cleanly and has never been executed.
It cannot be here: it needs a second process, `term`, and `wm`, none
of which `sim/` has.

The batching in it is written the way `repl` learned to write it
rather than the obvious way, and the reason is on record in
`repl.c`: `z_port_send()` refuses once eight messages are unacked, so
unbatched output silently loses its TAIL. `cat` of any real file would
have hit that on the first try. Worth knowing it is a deliberate
copy of a lesson rather than an invention.

### A spawned program's output does not come back to the shell

`zcc hello.c` at the `$` prompt compiles, and its diagnostics appear
on the **kernel console**, not in the `term` window the command was
typed in.

That is expected and it is a real gap, not a mystery. `zcc` is a
separate Zeitlos process; its `printf` goes through `_write()` to the
UART, which is the serial console. `posix` writes to the `zport`
connection `term` is on. Nothing joins the two, and nothing could
without one side being told about the other.

**The mechanism to fix it already exists and is the one `repl` uses.**
`sw/common/zeitlos.h` has `z_stdout_hook`: a per-process hook that
`_write()` calls instead of the UART. `repl` installs one so that
Scheme's `display` reaches the `term` window rather than the serial
line -- its own comment records the bug that prompted it, `(dump)`
printing 200 symbol names onto the console and showing the user
nothing.

So the shape of the answer is: `posix` passes its own pid to the child
alongside the command line; the child opens a port back to it and
installs a `z_stdout_hook` that forwards. Two things make that more
than an afternoon:

- **Every child has to opt in.** A program that does not know about
  the hook still writes to the console. That is fine for `zcc`, which
  is ours, and not fine as a general answer for `run`.
- **The shell has to wait.** `run` returned as soon as the process
  started, so there was no window during which output could be relayed
  and no exit status to report. Waiting means the shell stops serving
  its other connections unless it does so by message rather than by
  blocking.

The second is **done** -- 5.1, section 9e. The first is not, and it is
the real limit on this mechanism: `zcc` opts in because it is ours, and
`run` has no general answer for a program that does not.

### Pipes are Phase 5 -- and coroutines were the wrong answer

This section first said pipes needed "a coroutine discipline inside
this process -- a builtin that yields when its output buffer fills".
That was reasoning from first principles about a tree that had already
solved the problem twice, in two different ways, neither of which is a
coroutine.

**`Z_TERM_SET_PORT`** (`sw/common/zterm.h`): a provider can tell `term`
to go and connect somewhere else. `repl`'s `telnet <host>` uses it --
term stops talking to repl and talks to `net`'s telnet provider
instead, raw, until that ends. The terminal is handed over, not
proxied.

**`te_bridge`** (`sw/apps/repl/`): `te` runs INSIDE repl's process,
but only because te.c was built with `-DTE_HOST_IO`, which replaces
its `getch()`/`write()` with three functions repl implements. It does
not block; it is fed bytes.

Between them those cover every case pipes and full-screen programs
need, and neither requires a scheduler inside this process. What was
actually missing was smaller and more boring than a coroutine
discipline: **posix had no notion of a running child**, only of a
started one.

That is 5.1, and it is now done (section 9e). Pipes are 5.3 and need
5.2 first.



---

## 9e. Phase 5: the plan, and 5.1-5.2

Ordered so that each item unblocks the next. **5.1 and 5.2 are done**;
the rest are proposal. It was the keystone, and the writeup below keeps the
"what it needs" framing as it was written so that the change reads
against the reasoning that produced it.

### 5.1 Child lifecycle -- wait, and exit status -- DONE

`run` returns the moment `z_proc_run()` starts a process. Three things
are wrong because of that, and they are all the same thing:

- `zcc x.c && run x` tested whether the **compiler started**, not
  whether it succeeded.
- The stdout relay (section 9d) re-prompted when the child's output
  connection closed, which is a proxy for "the child finished" and not
  the same claim. **That proxy is gone**: the main loop watches the
  process now, so a program that closes its output early -- or never
  opens one -- is handled the same as one that does.
- Nothing could hand the terminal to a full-screen program and take it
  back afterwards, because there was no "afterwards". 5.2 can now be
  built on top of this.

**What was built.** The kernel keeps the last 16 exit statuses in a
ring and answers `Z_SYS_PROC_STATUS` from it (`docs/kernel.md`) --
`z_exit()`'s argument had been discarded up to now, so there was no
status to have. `posix` records the child's pid, returns from
`px_exec_line()` with the rest of the line saved, and its main loop
polls the status on the idle path. `px_resume()` continues the line
with the child's status as the shell's.

Polled rather than pushed: the kernel has no "process exited" message
and adding one is a bigger change than this needed. At `Z_TICK_HZ / 10`
the cost is one syscall per waiting connection per 100ms, and the
latency between a compile finishing and the prompt returning is that
same 100ms -- below noticing, well above its cost.

**Blocking was never an option.** This process serves up to four
terminal connections; a shell that blocks in `wait()` stops serving all
of them. That constraint is what made the suspend/resume split
necessary rather than a nicety.

Three things it fixed, two of which were already shipped as
approximations:

- `zcc x.c && run x` now tests whether the compiler **succeeded**.
- The prompt returns when the child **exits**, not when it happens to
  close its output connection. A program that closes early, or never
  opens one, is now handled the same as one that does.
- Input arriving while a child runs is **dropped**, not queued. Queuing
  would mean typing ahead into a shell about to print a compiler's
  output over it, then executing that input against a prompt the user
  never saw. Dropping is the honest version until 5.2 hands the
  terminal over properly.

**Two bugs the test transcript caught**, and both are the reason for
having one:

- A stale **launch argument** was inherited across commands. It is
  claimed by the child, not consumed by the parent, so a program that
  never calls `z_launch_arg_take()` left the previous command's
  arguments pending for the next one -- `zcc a.c -o a` then
  `run hello` would have handed `hello` a compiler command line. Fixed
  by always setting it, empty when there is nothing to pass.
- `false && a || b` **printed nothing**. A failed `&&` was abandoning
  the whole line instead of skipping one command. Shells evaluate
  strictly left to right with no precedence between the two, so the
  `||` should see the failure the `&&` left in place. Easy to state,
  easy to get wrong, and invisible until somebody writes a third
  clause.

### 5.2 Raw mode and the terminal handoff -- DONE

A full-screen program needs unprocessed bytes, and `posix` currently
runs every byte through `z_line_feed()`, which does line editing and
history.

The mechanism already exists: `Z_TERM_SET_PORT` points `term` at
another provider entirely, and `repl` uses it for `telnet`. So a
full-screen program is not something `posix` hosts -- it is something
`posix` hands the terminal TO, and takes it back from when the child
exits, which is 5.1.

This is strictly better than a raw-mode flag on the connection: the
bytes never pass through this process at all, so there is no relaying,
no batching, and no shell latency between a keystroke and the editor.

**This is the opposite direction from connecting TO posix.** Reaching
`posix0` from `term` -- by F11, or `port posix0` from `repl` -- has
worked since Phase 4 and needed no code (section 9d). What 5.2 adds is
`posix` GIVING its terminal away to a child and taking it back. Same
`Z_TERM_SET_PORT` message, opposite end.

**What was built.** A child asks for the terminal by opening a
connection to `posix0` with the connect argument `"tty:<name>"`, where
`<name>` is the provider name it registered with `z_pid_register()`.
`posix` accepts, tells the term window on the far end of the session to
go and connect there instead (`z_conn_handoff()`, the same helper
`repl`'s `telnet` uses), closes the request channel, and marks that
session's terminal as away. When the child exits -- which 5.1's watcher
already detects -- the terminal is handed back before anything is
written to it.

**The child asks; `posix` does not decide.** A list of full-screen
programs in the shell would be wrong twice: it has to be edited for
every new program, and it has to be consulted BEFORE the child has
started, which is before the child has registered a name for term to
resolve. Asking inverts both -- the child asks once it is running and
registered, so there is no race, and a program that never asks behaves
exactly as it does today.

**The name travels in the connect argument** because
`z_conn_handoff()` carries a provider NAME and term resolves it through
the pid registry. The connect argument is already a free-form string,
so this needs no change to `zport`, `zconnect`, `zterm` or `term` --
which is why this shape was chosen over adding a pid form to the
handoff message.

Two details that are easy to get wrong:

- The request is **accepted and then closed**, not refused. Refusing
  was the first version and says the wrong thing: the request
  succeeded. A child reading REFUSED cannot tell "posix is not there"
  from "posix did what you asked", and those want opposite responses --
  fall back to the console, or start drawing.
- Output to a session whose terminal is away is **dropped, not
  queued**. Queuing would replay a compiler's output over the editor's
  screen the moment the editor exits.

**Writing a client.** `sw/apps/ttytest` is the worked example and is
about 200 lines including its reasoning. The sequence:

1. **register a name** -- before asking, or term resolves a name that
   does not exist yet;
2. connect to `posix0` with `"tty:" + name`;
3. accept the connection term then makes to you;
4. draw, read keys;
5. close and exit -- `posix` takes the terminal back.

Step 1 before step 2 is what makes it work at all.

**The trap, which cost two attempts to get right.** A client of this
mechanism holds **two** connections: the request channel to `posix`,
and the session with `term`. Both can send `DATA`, `DATA_ACK` and
`CLOSE`, and acting on the wrong one closes the terminal you were just
given -- which on hardware looks like `term: port closed by peer --
local echo only from here on`, moments after a successful handoff.

Two plausible ways to tell them apart, both wrong:

- **By state** ("ignore a CLOSE while nothing is connected"). That is a
  guess about which message ARRIVES FIRST, and it is wrong: `posix`'s
  CLOSE for the request channel can land *after* term's CONNECT.
- **By tag.** `zport.h` suggests it -- "an app with exactly one
  connection can just check `msg.tag == port.conn_id`" -- and that is
  right for an app with one connection. This one has two, and their
  conn_ids **can collide**: `posix` assigns `slot + 1`, so a request
  landing in its slot 0 gets conn_id 1, and a provider that assigns
  its own conn_id 1 to term then cannot distinguish them.

**Filter by sender.** `msg.from` is the pid the message came from, and
`posix` and `term` are different processes, so it cannot collide.
`ttytest` ignores any non-CONNECT message whose `from` is not term's
pid.

`posix` itself does not have this problem: it is always the provider
and assigns every conn_id from one namespace.

F12 remains term's unconditional escape if a child hangs -- it
disconnects to term's start panel (it returned to `repl0` when this was
written) --
which is the same safety net a telnet session to a dead host has.

**The return path is the hard half, and the first version did not
work.** Handing the terminal over is one message; getting it back is a
sequence, and it turns on something that is obvious only once seen:

**term disconnects from `posix` before connecting to the child.** That
is what handing over means. So the moment the handoff succeeds, the
session's port is closed, `port.connected` is false and
`port.peer_pid` is gone. The first version early-returned on exactly
that in both the watcher and the return call, so the terminal was
handed away and never asked back -- on hardware, `ttytest` exited
cleanly and term sat in local echo.

Three things follow, all of them in `main.c`:

- **term's CLOSE during a handoff is not the session ending.** The
  slot keeps its shell state, its pending line and its `waiting` flag.
  Tearing it down leaves nothing to return the terminal TO.
- **The term pid is remembered before the handoff**, because
  `port.peer_pid` does not survive it.
- **term comes back as a fresh `Z_PORT_CONNECT`**, which is matched by
  pid to the slot that gave its terminal away. A new slot would mean a
  banner and a lost command line; matching means `ttytest && echo done`
  still prints `done`.

The child's exit status therefore waits with the slot: the watcher
notices the exit, asks for the terminal back, and stops. The line
resumes when term actually arrives, because nothing can be printed
before then.

**Still not verified beyond that.** All of this is in
`sw/apps/posix/main.c`, the half no test reaches -- it needs a second
process, `term` and `wm`. F12 remains the escape if a child hangs
while holding the terminal.

### How a child reaches the terminal: three cases, two built

Worth stating together, because the right mechanism is not obvious
from the program's point of view and picking the wrong one produces
output on the serial console instead of the screen.

| the child | mechanism | status |
|---|---|---|
| writes, never reads, does not draw | `PX_STDOUT_TAG` relay | **built** (5.1/9d) |
| owns the screen and the keyboard | `PX_TTY_TAG` handoff | **built** (5.2) |
| reads input but does not draw | -- | **nothing** |

**`zcc` is the first case and does NOT need 5.2.** It opens a second
connection tagged `stdout`, `posix` relays what arrives to whichever
session ran the command, and the compiler never touches the terminal
itself. That is already in `sw/apps/zcc/port_dev.c`.

Worth knowing its failure mode: if `z_pid_lookup("posix0")` fails --
no `posix` running, or a second instance registered as `posix1` --
`zio_out()` falls back to the UART. **That looks exactly like the
relay not being built**, so "zcc printed to the serial console" is not
evidence either way without checking whether a `posix0` was up.

**`vi` is the second case.** It wants raw keystrokes and cursor
addressing, and relaying every byte through `posix` would add latency
to each one and require `posix` not to interpret them. Handing the
terminal over removes this process from the path entirely.

**The third case has no mechanism yet**, and 5.3 needs one: a program
that reads standard input without drawing -- the receiving end of a
pipe, or anything that prompts. The `stdout` relay is one-way, and
input arriving while a child runs is currently dropped (9e, 5.1).

The obvious shape is to make that connection **bidirectional**: while
a child holds one, keystrokes go to the child instead of to the line
editor. That subsumes the first case rather than competing with it,
and it does not replace the handoff -- a full-screen program still
wants `posix` out of the path. Left undone deliberately until 5.3
gives it a user, because a mechanism with no client is a mechanism
nobody has tested.

### Flow control on the relay, and the one ack that goes last

`posix: z_port_send failed (44 bytes)` on hardware, with a child's
output losing its tail.

`z_port_send()` refuses past `Z_PORT_MAX_PENDING_SENDS` (8) unacked
messages. `posix` drains its mailbox once per idle pass -- every
`Z_TICK_HZ / 10` -- so a child producing fifteen lines in a burst
overran the window long before any ack was read, and the old
`out_flush()` **dropped on the first refusal**.

Three changes, and the middle one is the interesting one:

- **The batch is retried, not dropped.** `out_flush()` keeps the bytes
  and returns false; the main loop tries again next pass.
- **A sink is acked LAST.** A terminal is acked first so that typing is
  never held up by a slow command. A sink is the opposite case -- it is
  a PRODUCER, and the ack is the only backpressure there is. Holding it
  until the bytes are buffered makes the child wait instead of
  overrunning us.
- **The buffer is sized to the ceiling rather than guessed.** A child
  can have 8 messages in flight and `zcc` batches at 512 bytes
  (`OUT_BUF`), so 4KB is the most that can arrive before it must wait,
  and `OUT_BATCH` is 4KB. Nothing is dropped. A child sending larger
  messages can still overrun it, which `out_dropped` counts and
  reports -- a deliberate ceiling, because buffering without limit
  turns a misbehaving child into an out-of-memory failure in the shell.

**The retry is in the main loop, not in `out_flush()`, and that is not
a style choice.** Pumping acks inline would mean calling
`z_msg_read()` from inside the send path, which re-enters
`handle_data()`, which calls `conn_out()`, which calls the send path --
unbounded recursion on a 16KB stack, reachable only under exactly the
load that triggers it. The main loop has no call stack to grow.

### Where `te` fits

`te` already runs inside `repl`'s process, built `-DEMBEDDED
-DTE_HOST_IO` so that its `getch()`/`write()` become three functions
the host implements (`sw/apps/repl/te_bridge.c`). There is no
standalone `te` app; the editor exists only as that submodule and that
bridge.

That adaptation is the valuable part and it already exists -- which is
why `te` is the right first client for a port-based editor even if
`nextvi` (5.4) is the eventual answer. The glue is the same either way.

**Embedding it in `posix` as well would be the wrong move**, and 5.4c
says why: it would put the editor in the tree twice, and an editor
holding a file in the shell's address space means a crash in either
loses both. 5.2 exists so that is not the only option.

### 5.3 Pipes, as processes rather than coroutines

With 5.1 and 5.2 in place, `a | b` is two processes and a port between
them, which is what `zport` is for.

The real limits are the ones already documented in section 4.2 and
they are not about scheduling: `Z_PROCS_MAX` is 16 for the whole
machine and `Z_MEM_MIN_BLOCK_SIZE` is 32KB, so `ls | grep | wc` costs
three process slots and 96KB minimum.

**Both are raisable, and `docs/kernel.md` works out what it costs.**
The short version: `Z_PROCS_MAX` is now **32**, which is 14.5KB more
kernel `.bss` -- and, less obviously, 14.5KB more FLASH IMAGE, because
`.bss` is padded into `kernel.bin` so the BIOS's fixed 256KB copy
zeroes it. 64 was tried first and overran that budget. The block size is a separate change with a real
fragmentation risk to `zcc`'s 4MB allocation, and should land with a
measurement rather than on reasoning.

Even with better limits, a small filter as a process costs ~12KB and a
slot against a builtin's zero, so the argument for keeping the common
ones as builtins survives -- it just stops being an argument about
scarcity.

A builtin-to-builtin pipe still wants an in-process buffer, and that
IS the coroutine case -- but it is now an optimisation for the common
short pipeline rather than the mechanism everything rests on.

### 5.4 A real `vi` -- the survey

**A permissive real vi exists and the port is small.** The licence
question that ruled out `busybox vi` (GPL, the same objection that
ruled out TinyCC for `zcc`) does not apply to either serious
candidate.

| | licence | C lines | terminal layer |
|---|---|---|---|
| **neatvi** (Ali Gholami Rudi) | **ISC** | 10,049 | `term.c`, 232 lines |
| **nextvi** (Kyryl Melekhin, a neatvi rewrite) | **ISC** | 9,085 | `term.c`, 352 lines |
| `busybox vi` | GPL | -- | -- |
| `nvi` | BSD | ~30,000 | wants curses and a db library |

ISC is the same class as QBE and cproc -- permission to use, copy,
modify and distribute, with the notice retained. No objection.

**What either needs from the system**, counted rather than guessed:

- `tcsetattr` -- raw mode. Not needed: a port connection is already
  raw. Stub.
- `ioctl(TIOCGWINSZ)` -- terminal size. One call; `term` knows the
  answer and the port can carry it, or a fixed 80x25 to begin with.
- `fork`/`execvp` -- `:!` shell-outs and filters, two call sites. Stub
  to "not supported" first; `posix` could serve them later.
- `signal`, `poll`, `dirent` -- stub, stub, and only for directory
  listings.
- Otherwise plain C: `string`, `stdio`, `stdlib`, `ctype`, `stdarg`.

Everything else funnels through `term_read()` and `term_write()`. **So
the port is one file plus a handful of stubs** -- which is the same
shape as `sw/apps/ttytest`, and roughly the same size.

**Lean: `nextvi`.** Tighter (9,085 lines against 10,049), self-contained
regex, and without neatvi's LSP client and JSON parser, which are
several hundred lines this machine will never use. `neatvi` has the
smaller `term.c` and is the more established upstream, so it is a
reasonable second choice rather than a wrong one.

**Unverified:** neither has been compiled for rv32. 9,000 lines at
`-Os` is plausibly 80-120KB, which is an app-sized binary and not a
problem, but nobody has measured it.

### 5.4a UTF-8, on a machine that has none -- resolved

nextvi is built around UTF-8 and Zeitlos has no UTF-8 support at all.
That looked like the blocking objection and it is not one.

**The whole of UTF-8 in nextvi is one 256-byte table.** Every
multi-byte decision routes through `uc_len(s)`, which is
`utf8_length[(unsigned char)s[0]]`. Set entries 1-255 to 1 and every
byte is one character. The table IS the switch; no `#ifdef` touches
the editor's logic. `sw/ext/nextvi` carries that as `uc_bytemode()`,
three lines behind `#ifdef ZEITLOS`.

**And it is required, not optional.** Zeitlos terminal fonts cover
0x20-0x7f (`sw/common/zfont_data.c`) and `sw/common/zgfx.c` draws
nothing for a byte outside that range -- so a screen cell is exactly
one byte. Leave the table as upstream has it and a two-byte sequence
is one character to nextvi and two blank cells to the terminal: the
cursor column and the screen disagree from there on, and every redraw
after it is wrong. Flattened, the arithmetic matches the rendering
exactly.

A file containing high bytes therefore displays as blanks and edits
without corruption, which is the behaviour to want -- the editor and
the screen agree about where everything is.

Three things follow without needing their own switch, which is why
this is a small change rather than a fork:

- `ren.c`'s two non-ASCII outputs -- a combining-character joiner and
  a replacement glyph -- are both behind `if (l == 1) return NULL`, so
  they become unreachable.
- `conf.c`'s right-to-left character ranges are matched against file
  content, and ASCII never matches them.
- `xshape` and `xorder` (Arabic shaping, bidi reordering) are runtime
  flags the front end sets to 0. Already no-ops on ASCII; off for size
  and speed.

`sw/ext/nextvi/ZEITLOS.md` is the full list of local changes, kept
short so that re-vendoring upstream is mechanical.

### 5.4d What the port actually needed

Built as `sw/apps/vi`. **No line of `sw/ext/nextvi` is modified**
beyond the three-line `uc_bytemode()` in 5.4a.

The port is smaller than the survey suggested, because of one thing
the survey missed: nextvi's `term.c` reaches the outside world through
exactly two calls -- `write(1, ...)` behind the `term_write` macro,
and `read(fd, ..., 1)` in `term_read()`. Both are newlib, and
`sw/common/zeitlos.c` already routes `_write()` through
`z_stdout_hook`. So **`term.c` runs as upstream wrote it** and the
port is two hooks. Replacing `term.c` would have meant re-porting 352
lines on every re-vendor, including the input-buffer and recording
machinery the rest of the editor depends on.

Three things it turned up:

**A real bug in `_read()`** (`sw/common/zeitlos.c`). It waited for a
UART byte and then never called `uart_getc()` -- `p[i] = (char)c;`
with `c` uninitialised. It has been returning uninitialised stack for
as long as it has been in the tree. Nothing noticed because nothing
reads stdin through newlib: the console uses `getch()`/`readline()`,
and apps take input from a port or from HID. Fixed, and
`z_stdin_hook` added as the partner to the existing stdout hook.

**nextvi is a unity build.** `vi.c` `#include`s the other eight `.c`
files and they do not compile standalone. The file list looks exactly
like a set of translation units, which is the trap.

**Four headers have to be shimmed**: `<poll.h>`, `<dirent.h>`,
`<sys/ioctl.h>`, `<termios.h>`. Absent or `#error`-ing in embedded
newlib and picolibc alike -- they are hosted-OS headers. `sw/apps/vi/shim`
provides them and `posix_stubs.c` implements what is behind them.

**And three symbols have to be renamed** -- `main`, `itoa` and
`lstat`, all with `-D` rather than an edit to the vendored tree. `itoa`
is the interesting one: newlib declares it as a three-argument
extension and nextvi defines its own two-argument version, which is a
conflicting declaration rather than a warning. `lstat` becomes `stat`
because FAT has no symbolic links.

**What is stubbed, and what each one costs**, is the honest measure of
how well this machine passes for POSIX:

| stub | cost |
|---|---|
| `tcsetattr`, `tcgetattr` | none -- a port is already raw |
| `poll` | none -- the read underneath does its own blocking |
| `ioctl(TIOCGWINSZ)` | fixed 80x25 instead of the real window size |
| `isatty` | none -- must return true or the editor exits at once |
| `signal`, `kill` | none |
| `getenv` | none -- all four names it asks for have the same answer without an environment |
| `getcwd`, `chdir` | `:cd` reports it cannot determine the directory |
| `opendir`, `readdir` | `:e` filename completion offers nothing |
| `stat` | none -- only reached through the completion walk |
| `fork`, `execvp`, `pipe`, `dup2`, `getpgrp`, `tcsetpgrp` | **`:!` cannot run anything** |
| `sigaction` | none |

`signal()` is deliberately NOT stubbed: newlib's `signal.o` is linked
anyway for other things in that object, so defining one is a
duplicate-symbol error rather than an override. Its implementation
records the handler and never calls it, which is exactly what is
wanted.

**It builds and links**, with the tree's own toolchain: `vi.bin` is
228KB against `zcc`'s 118KB and `ttytest`'s 110KB.

Two of those losses are real, and both have a clear route back:
directory listing exists as `fs_list_into()` and is simply not
`opendir`-shaped, and a `:!` that asked `posix` to run something is a
message rather than a fork.

**One real loss of function**: `:!` cannot run anything, because there
is no fork (section 4.3). The route back is clear -- `posix` can
already start a program and collect its output -- so a `:!` that asked
`posix` to run something is a message, not a fork. `:e` filename
completion is also inert, because `opendir()` returns NULL; Zeitlos
has directory listing (`fs_list_into()`), it is simply not
opendir-shaped.

**CR has to become LF on input**, in the stdin hook. `term` sends 0x0d
for Enter and a POSIX tty driver would translate it before the editor
saw it -- `ICRNL` is an input flag, on by default, and nextvi clears
only `c_lflag` bits, so it relies on that translation rather than
disabling it. There is no tty driver here. Without it, Enter inserts a
literal CR which renders as `^M` and no line ever breaks.

That generalises: **anything ported here that assumed a tty was in
front of it will want the same.**

### 5.4g The stray CLOSE, and why conn_id is not an identity

The last bug in the handoff, and the one that had been intermittent
since 5.2 was built.

**`z_port_close()` sends a CLOSE to the peer.** When term leaves
`posix` for a child, posix's `handle_close()` called it -- and that
message arrived at term AFTER term was connected to the child,
carrying posix's conn_id. Both sides assign `slot + 1` from their own
tables, so both are usually **1**. term checked `msg.tag ==
port.conn_id`, matched, and dropped to local echo.

Two fixes, and the first is the real one:

- **`posix` no longer sends it.** There is nothing to tell: term
  initiated the close and knows about it. The port is marked closed
  locally instead.
- **`term` checks the sender too**, so the window is immune to any
  provider that closes late. One condition added to a check that was
  already there.

**This is the third time conn_id collision has caused a bug here** --
`ttytest` first, then `vi`, now this. Worth stating plainly:
**`conn_id` identifies a connection WITHIN one provider's table, not
across the system.** Anything holding two connections, or outliving a
handoff, has to use `msg.from`. `zport.h`'s advice to check the tag is
correct for its stated case -- "an app with exactly one connection" --
and that case stops applying the moment terminals start moving between
providers.

### 5.4h The terminal was missing two sequences

Moving down past the last line redrew only that line; the screen did
not scroll.

nextvi scrolls by emitting `ESC[nM` (delete lines) and `ESC[nL`
(insert lines) -- `term_room()` -- and never by redrawing. `zvt100`
implemented `A B C D H J K f m` and neither `L` nor `M`, so the
sequences were parsed and discarded. From the application's side that
is indistinguishable from a broken editor.

Both are implemented now (`docs/terminal.md`), with `DL` at the top of
the screen routing through the existing `scroll_up()` so the renderer
moves pixels instead of repainting rows.

**This is the useful kind of finding**: a real terminal feature that
nothing had needed until a program written for a real terminal asked
for it. The same is true of the CR/LF translation and of newlib file
I/O -- three gaps that existed for as long as the tree has, and were
invisible because everything here was written to the shape of what
already worked.

### 5.4f Things that look like bugs and are not

**Arrow keys** needed work, and the constraint shaped the answer.
`term` sends `ESC [ A` for Up and **must keep doing so** -- `repl`,
telnet and ssh all depend on it, and the far end of those is a real
terminal. So the fix belongs in the one program that cannot use the
sequences, not in `term`.

`sw/apps/vi` translates them to `h`/`j`/`k`/`l` in its stdin hook,
**only when the editor is reading commands**. Doing it unconditionally
types the letter into the document in insert mode. nextvi gained a
`vi_typing` counter for that (`sw/ext/nextvi/ZEITLOS.md`), set around
the two functions that read typed text.

A lone `ESC` passes through untouched: the translation fires only when
the whole three-byte sequence is already in the input ring, and a real
Escape keypress arrives in its own message.

**`:e` completion offers nothing and `:!` runs nothing.** Both are
stubbed, both are listed above with what they cost.

**There is no syntax highlighting**, and there cannot be: the
framebuffer is 1bpp and `vt_cell_t` carries only `reverse`, so the
colour SGR nextvi emits is parsed and discarded. The rules are still
compiled, and measurement says leave them: removing all 135 of them
saves about 100KB of heap and 11KB of binary, which is not worth a
local change to a vendored tree that would have to be reapplied on
every update.

### 5.4e What it took to make it actually run

The build linking was not the end of it. Four things, and three were
mine:

**`_read()` hooked every descriptor, not just stdin.** `_write()` in
`sw/common/zeitlos.c` has checked `fd == 1` from the start; I wrote the
input half by copying its shape without its condition. So `vi hello.c`
opened the file, called `read()`, and got the KEYBOARD -- the editor
sat waiting for a keystroke that was meant to be the file's contents.
Cost a round trip to hardware.

**newlib file I/O has never worked here.** `_open()` in `zeitlos.c`
returns `ENOENT` unconditionally, and always has: nothing written for
this machine reads a file through newlib, because `zfsapp.h` is the
filesystem API and every app calls it directly. Ported code does not
know that. nextvi opened its file, got -1, and displayed an empty
buffer -- `0L` with a filename in the status line.

`sw/apps/vi/fileio.c` is the adapter: a descriptor table over
`fs_open_read()` and friends. The runtime's versions are now **weak**,
so an app can provide real ones without every binary that includes
`zeitlos.c` gaining a dependency on `zfsapp.o`.

**CR has to become LF on input** -- see below.

**A console fallback, which was not planned and should have been.**
`vi` with no `posix0` now runs on the serial console instead of
refusing, and reads `/vi.args` when there is no launch argument, the
same stopgap `zcc` carries. That is worth having on its own (an editor
on the console is the recovery path when `wm` is not running) and it is
what made the editor testable under `sim/`, which has no `posix`, no
`term` and no `wm`.

It paid for itself immediately: running `vi` under the simulator showed
a full editor screen, which proved the port was sound and the fault was
in the plumbing. Both remaining bugs were found that way rather than on
hardware.

`vi` is in the new `Z_PROC_STACK_SIZE_BIG` tier -- **1MB**, between
`LARGE`'s 64KB and `HUGE`'s 4MB.

**The number moved three times and the story is the useful part.** 4MB
first, then 2MB, on the strength of an apparent startup cost of about
1.1MB. Then the cost was traced, and it was not nextvi's: **`_fstat()`
reported `st_size` as 0**, and `lbuf_rd()` falls back to a
**1,048,575-byte** read buffer when it cannot learn a file's size. Every
file opened allocated a megabyte before a line was read.

With the size recorded at `open()` and reported properly, nextvi starts
in **under 50KB with full syntax highlighting**, and a 154KB source
file needs about 3.4 times its own size. 1MB covers anything in this
tree -- which was the first proposal, rejected because `vi` would not
start in it. The instinct was right; the bug was mine.

**A stub that returns "no information" is not neutral.** Callers have
fallbacks, and a fallback chosen for a hosted system can be wildly
wrong here. That is the third time in this port -- `poll` returning
ready, `getcwd` returning NULL and this -- and only this one was
expensive.
An editor holds the file plus its undo history in that allowance, and
nextvi allocates per LINE, so a file costs roughly three times its own
size. `LARGE`'s 64KB does not hold a 60KB source file at all -- but
that argues for more than 64KB, not for 4MB, and `HUGE` would reserve a
quarter of an 8MB board for one editor. It was briefly `HUGE` on
exactly that mistaken step.

### 5.4b Where that leaves `te`

`te` is this tree's own tiny vi, and the choice is not exclusive.
`te` is far smaller and is the right editor for a board that cannot
spare 100KB; a real vi is the right one where the muscle memory
matters. Both can exist -- they are two apps.

**What should NOT persist either way is te being compiled into
`repl`.** See below.

### 5.4c `repl` should run the editor as a process too

Raised while discussing this, and the answer is yes.

`te` is currently linked INTO `repl`, built `-DEMBEDDED -DTE_HOST_IO`
with `te_bridge.c` supplying its I/O. If `te` also becomes a
standalone app for `posix`, the editor exists twice in the tree and
twice in flash.

Making it one standalone provider that both shells hand the terminal
to is better on four counts, and the last one is the one that matters:

- **One copy** of the editor, one set of bugs.
- **`repl` shrinks** by whatever `te` costs it today, which is real
  flash and real RAM for every user of `repl` whether they edit or not.
- **Isolation.** An editor holding a file in the shell's address space
  means a crash in either loses both -- and an editor is where the
  unsaved work is.
- **The glue is written once.** Whatever terminal layer a port-based
  `te` needs is the same layer a port-based `nextvi` needs. Choosing
  the editor stops being an architectural decision and becomes a
  build-time one.

**The cost is that `repl` needs a child watcher**, which it does not
have. `posix` grew one in 5.1 (`Z_SYS_PROC_STATUS` polled on the idle
path) precisely so it could take the terminal back when a child exits.
`repl` would need the same ~30 lines.

There is a cheaper-looking alternative -- **the child hands the
terminal back itself** on exit, by sending `Z_TERM_SET_PORT` to term
pointing at whoever gave it over. It needs no change to `repl` at all.
It is worse, and the reason is the case that matters: **a child that
crashes never returns the terminal.** Parent-driven recovery survives
a crashing editor; child-driven recovery does not, and an editor is
exactly the program a user most wants to survive crashing.

Doing both is defensible -- the child returns it promptly, the parent's
watcher is the safety net -- and is what a second pass should look
like. F12 remains the unconditional escape underneath either.

### 5.5 More builtins

`cp`, `mv`, `grep`, `head`, `tail`, `wc`, `find`. Cheap, and worth
doing after 5.3 rather than before, so that the ones that should be
pipe filters are written as pipe filters.

### What is deliberately NOT in Phase 5

`fork`. Section 4.3 covers why: it is implementable thanks to the MTU
and it would be a mistake -- no copy-on-write, no shared descriptor
table, and a 16-slot process table.

---

## 10. Sharing the runtime between existing apps

Raised while discussing the blob in section 3.1: if `zcc` output can
call a runtime at a fixed address, could **every existing app** stop
carrying its own copy?

**Yes, and the MTU is what makes it work.** Only `0x8xxx_xxxx` is
translated, so shared code sitting at a fixed *physical* address after
the kernel is directly callable from any process, with no relocation
and no indirection.

The obvious objection -- the runtime has data, and data cannot be
shared -- has a clean answer here that it would not have on an
ordinary flat-memory machine. Put the runtime's `.data`/`.bss` at a
**fixed virtual offset inside every app's own image**, reserved by
`riscv-app.ld` ahead of `.text`. Shared code then reaches its globals
through `0x8000_0xxx`, and the MTU translates that to *the calling
process's own block*. So `malloc`'s arena, newlib's `_impure_ptr` and
`term_echo` are per-process automatically, with no context pointer
threaded through anything and no rewrite of `zeitlos.c`.

Three things make it real work rather than an afternoon:

- **Every app's runtime data must be pinned identically**, which means
  generating a linker fragment from the built blob and including it in
  every app's script -- and a runtime rebuild becomes a whole-tree
  rebuild.
- **The blob must be built `-mno-relax`.** `gp`-relative addressing in
  shared code would resolve through the *caller's* `gp`, which is
  precisely the hazard `docs/app_runtime.md` documents for the syscall
  path. It took three bugs to find that the first time.
- **A version word and stable table ordering**, so a stale app fails
  loudly rather than calling the wrong function -- the same discipline
  `syscalls.def` already documents about never inserting in the
  middle.

**It is not a `zcc` dependency and should not block one.** Its real
payoff is the memory budget on small boards, which is an
existing-system optimisation with its own justification -- and its
biggest beneficiary is Obst, which cannot run `posix` regardless. The
decision taken in section 8.2 is to design the Phase 2 jump table so
this migration stays possible, and ship copy-per-output first.

---

## 11. What is not done

### In `posix`

- **No quoting, no globbing.** `cat "two words.txt"` and `rm *.o` do
  not work. A Zeitlos path cannot contain a space, so quoting has no
  job; globbing is a real absence and would go in `split()`.
- **A pipe is capped at 16KB** and holds its intermediate in memory.
  Process-to-process pipes would lift that and are unbuilt: nothing
  puts a compiler or an editor in a pipeline.
- **A spawned program's output only comes back if it asks** -- by
  opening a second connection tagged `stdout`. `zcc` does; a program
  you write does not unless you make it.
- **No job control**, no `&`, no signals. There are none to send.
- **One current directory** for every connection, not one per session.

### In `zcc`

- **No `long long`, no floating-point arithmetic**, no bitfields, no
  variadic definitions, more than 8 arguments, or structs by value.
  `docs/zcc.md` has the full list.
- **No designated initialisers**, which is the one thing standing
  between here and self-hosting: `parse.c` uses them in its own type
  table, so `zcc` cannot yet compile `zcc`. Bounded work, and section
  8's decision 5 says it is a real goal.

### In `vi`

- `:!` (no `fork`), `:e` completion (no `opendir`), and a fixed 80x25
  window (nothing asks `term` for its size). Syntax highlighting is
  compiled and inert -- the display is 1bpp.

### Elsewhere, and measurable

- **`Z_MEM_MIN_BLOCK_SIZE` is 32KB.** Dropping it to 4KB would quarter
  the cost of a small process, and risks fragmenting the pool against
  `zcc`'s 4MB request. Needs `free` either side, not reasoning
  (`docs/kernel.md`).
- **Integer-only printf in the kernel** is worth about 33KB of the
  256KB image. There is headroom now, so this is for when the space is
  wanted.
- **The ethernet half of the SPI change is unverified.** There is no
  `sdbench` equivalent for that path and there should be before anyone
  claims a number (`docs/sdcard.md`).
- **`repl` still links its own copy of `te`.** Now that `vi` is a
  standalone port provider, `repl` could hand the terminal over the
  same way `posix` does and drop the editor from its binary -- which
  needs `repl` to grow the child watcher `posix` has (section 9e,
  5.4c).
- **Section 10, the shared runtime**, remains unscheduled. Its payoff
  is the memory budget on small boards, and the board it helps most
  cannot run `posix` anyway.

### Not verified on hardware

`sw/apps/posix/main.c` has no automated test and cannot have one here:
the port protocol needs a second process, `term` and `wm`, none of
which `sim/` has. Every bug in the terminal handoff was found on a
board, which is why `sw/apps/ttytest` exists at 200 lines -- it is the
smallest thing that exercises that path.

`sh.c` and `vfs.c` ARE covered, by two suites with different jobs:
`tests/cases.txt` asserts what each command produces, and
`tests/script.sh.txt` records a transcript and diffs it. The first
caught a pipe bug the second would have recorded as correct.

## 12. See also

- `docs/zcc.md` — the compiler
- `docs/libz.md` — the runtime blob and the jump table
- `docs/kernel.md` — processes, the exit ring, and the 256KB image
  budget
- `docs/zcc_bringup.md` — hardware bring-up, and what is still
  unverified
- `docs/sdcard.md` — the SD path: measured, diagnosed and fixed
- `docs/executables.md` — ZEXE, the compiler's output format
- `docs/app_runtime.md` — the runtime `libz` would wrap, and the
  `printf` size trap
- `docs/filesystem.md` — FatFs non-reentrancy and the preempt deferral
- `docs/ramdisk.md` — `/ram`; its old 19 KB/s figure was wrong by 13x
  and is corrected there
- `docs/ports.md` — the provider protocol `posix` implements
- `docs/boot.md` — the memory budget these numbers come out of
- `sim/README.md` — where the code generator was developed, and what
  the simulator does and does not model
- `docs/zeitlos32.md` — the core that would have to grow an MMU for §6.1
