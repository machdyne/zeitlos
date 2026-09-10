# A POSIX layer and an on-device C compiler

**Status: proposal. Nothing here is implemented.** Written ahead of the
code the same way `docs/ports.md` was, so the reasoning is on record
before it becomes hard to reconstruct. Update this file as phases land;
mark them done in place rather than deleting the reasoning.

Measured against commit `9efca5f`.

---

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
`zconnect.h` with a new target name, so `repl` and `posix` coexist and
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
| `sw/common/zconnect.c` | one new target name |
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

### Phase 5 — userland

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
**That was not needed.** The `port` target already takes any
registered name, so `port posix0` at a `term` prompt works with
nothing added. `repl` is untouched and the two coexist exactly as
intended.

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
- **The shell has to wait.** Right now `run` returns as soon as the
  process starts, so there is no window during which output could be
  relayed and no exit status to report. Waiting means the shell stops
  serving its other connections unless it does so by message rather
  than by blocking.

Both are Phase 5, alongside pipes, and for the same underlying reason:
this shell has no notion of a running child yet, only of a started
one.

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
need, and neither requires a scheduler inside this process. What is
actually missing is smaller and more boring than a coroutine
discipline: **posix has no notion of a running child**, only of a
started one. Section 9e is the plan.



---

## 9e. Phase 5: the plan

Ordered so that each item unblocks the next. The first is the keystone
and everything else is waiting on it.

### 5.1 Child lifecycle -- wait, and exit status -- DONE

`run` returns the moment `z_proc_run()` starts a process. Three things
are wrong because of that, and they are all the same thing:

- `zcc x.c && run x` tests whether the **compiler started**, not
  whether it succeeded.
- The stdout relay (section 9d) re-prompts when the child's output
  connection closes, which is a proxy for "the child finished" and not
  the same claim.
- Nothing can hand the terminal to a full-screen program and take it
  back afterwards, because there is no "afterwards".

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

### 5.2 Raw mode and the terminal handoff

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

### 5.4 A real `vi`

Possible, and 5.2 is what makes it so. Two routes, both with precedent
here:

**As its own process** (the `telnet` shape). `vi` becomes a port
provider; `posix`'s `vi file` spawns it with the filename as a launch
argument and hands the terminal over with `Z_TERM_SET_PORT`. `vi`
blocks on input all it likes -- it is its own process, and `posix` is
not waiting on it, only watching for its exit.

This is the better route and the reason is not effort, it is
isolation: an editor holding a file in memory in the same process as
the shell means a crash in either loses both. It also costs a process
slot and a memory block, which on a 32MB board is nothing.

**In-process** (the `te` shape). Needs the editor's `getch()`/`write()`
replaced with functions the host implements, which is what
`-DTE_HOST_IO` does to te.c. Feasible for a small vi clone, invasive
for a real one -- nvi is ~30k lines and expects curses and a database
library.

**On which `vi`:** the licence question has to be settled before the
engineering one. `busybox vi` is GPL, which is the same objection that
ruled out TinyCC for `zcc` (section 3.2). `nvi` is BSD but large and
dependency-heavy. A small BSD- or public-domain-licensed vi clone is
the likely answer and none has been surveyed yet -- that survey is the
first task of 5.4, not the last.

Worth noting where this ends up: a `vi` that runs as its own process
and talks a port needs `getch`, `putchar` and file I/O and nothing
else. That is within what `zcc` can compile and what `libz` provides,
so **`vi` is a plausible first substantial program to build on the
machine rather than for it.**

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

## 11. Still open

1. **When to schedule section 10**, if at all.
2. **Whether `sdbench` justifies raising `Z_SPISD_DIV_FAST` to 0.**
   Answerable only from hardware, and per board -- see
   `docs/sdcard.md`, "Trying DIV=0".

---

## 12. See also

- `docs/zcc_bringup.md` — hardware bring-up: what has never run on
  a board, and in what order to try it
- `docs/zcc.md` — the compiler
- `docs/libz.md` — the runtime blob and the jump table
- `docs/sdcard.md` — the outstanding throughput measurement
- `docs/executables.md` — ZEXE, the compiler's output format
- `docs/app_runtime.md` — the runtime `libz` would wrap, and the
  `printf` size trap
- `docs/filesystem.md` — FatFs non-reentrancy and the preempt deferral
- `docs/ramdisk.md` — `/ram`, and the SD throughput figure to re-check
- `docs/ports.md` — the provider protocol `posix` implements
- `docs/boot.md` — the memory budget these numbers come out of
- `sim/README.md` — where the code generator should be developed
- `docs/zeitlos32.md` — the core that would have to grow an MMU for §6.1
