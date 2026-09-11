# zcc, the Zeitlos C compiler

## Status

**Phases 1, 2 and 3 are done.** `zcc` runs on the development host AND
as a Zeitlos app, emits ZEXE images that run, and links them against
the `libz` runtime including the tree's own graphics and window code.

The device build compiles all eleven test programs and produces output
**byte-for-byte identical** to the host build. Not verified on
hardware -- see "On the device" below for exactly what that means. See `docs/posix.md` for the
phase plan.

It compiles a program that includes the tree's own
`sw/common/zeitlos.h` and produces byte-identical output to the same
program built with `riscv64-unknown-elf-gcc`. That is the headline
result and it is checked by the test suite on every run.

```
cd sw/apps/zcc && make -f Makefile.host && make libz
./zcc -I libz -L libz hello.c -o hello
```

With `-L` pointing at a built runtime, a program gets `printf`,
`malloc`, the filesystem and `maskirq` -- see `docs/libz.md`. Without
it, `zcc` emits its own entry stub and the program is freestanding.

## Provenance

Written from scratch, clean-room, under this tree's own licence. No
code from any other compiler is in it.

What was looked at while surveying the alternatives (`docs/posix.md`,
section 3.2) was shecc's licence, build, arena-size constants and
command-line options, plus measured memory and timing figures. Not its
lexer, parser, SSA or code generator. The design here comes from
published compiler-construction material and the RISC-V unprivileged
specification.

One architectural idea is shared with every compiler that targets a
format like this one, and is forced by the target rather than
borrowed: **straight to machine code, no assembler, no linker.**

## Why there is no assembler and no linker

A ZEXE image (`docs/executables.md`) is a 16-byte header and a flat
blob loaded at a fixed virtual base, with `.bss` as a number rather
than as bytes. Every app is linked at `0x8000_0000` because the MTU
remaps that base per process.

So there is exactly one address space and it is known at compile time.
There is nothing to assemble, because the parser can emit instruction
words directly into a buffer; and nothing to link, because a linker's
whole job is the fixup table in `emit.c`, which is small when there is
one translation unit and one fixed base.

This removes two components that would otherwise each be comparable in
size to the compiler.

## Shape

```
lex.c    source -> tokens
cpp.c    directives and macros, interleaved
parse.c  recursive descent, emitting RV32IM as it goes
emit.c   instruction encoding, labels, fixups, ZEXE output
util.c   allocation and diagnostics
```

4,600 lines. One pass: no AST, no IR. The parser walks the token
stream and code comes out.

### The evaluation model, and its cost

Every expression leaves its result in `a0`. An lvalue leaves its
*address* in `a0`, so `x = y` is "evaluate y, push, evaluate &x, pop
the value, store". Binary operators push the left operand to the
stack, evaluate the right into `a0`, and pop the left into `t0`.

That is an accumulator machine with a memory stack, and it is **about
four and a half times the size of what GCC produces**:

| test | zcc | gcc -O1 | |
|---|---|---|---|
| `t01_arith` | 3,820 | 1,097 | 3.5x |
| `t02_control` | 3,884 | 691 | 5.6x |
| `t03_data` | 5,224 | 1,168 | 4.5x |
| `t04_funcs` | 4,536 | 852 | 5.3x |
| `t06_string` | 5,116 | 1,160 | 4.4x |
| `t07_real` | 6,476 | 1,404 | 4.6x |
| `t08_zeitlos` | 2,564 | 902 | 2.8x |

Against a runtime the ratio collapses, because the 5.4KB blob is the
same object in both builds and dominates a small program:

| test | zcc | gcc -O1 | |
|---|---|---|---|
| `l01_libz` | 8,344 | 6,484 | 1.3x |
| `l02_fs` | 7,480 | 6,080 | 1.2x |

That is the price of having no register allocator, no liveness
analysis and no spilling logic — three things that are individually
harder than everything else in the compiler combined, and whose bugs
appear as wrong answers rather than as crashes.

**It is a contained later job.** Two things would recover most of it,
and neither touches the front end:

- **A peephole pass over the emitted stream.** `sw ra,0(sp)` /
  `lw t0,0(sp)` pairs across a push/pop with nothing in between
  collapse to a `mv`, and a great deal of the output is exactly that.
- **A small register stack**, using `t1`–`t6` for the first six levels
  of expression nesting and falling back to memory below that. Most
  expressions never go six deep.

Two other known-cheap wins, both noted at their site in `parse.c`:
`for` loops emit three extra branches per iteration because the step
expression is emitted before the body and jumped around (buffering its
tokens and re-parsing them after the body removes that), and function
parameters are all spilled to the frame at entry.

## The supported subset

### Present

Types: `void`, `char`, `short`, `int`, `long` (32-bit), all with
`unsigned`, plus pointers to any depth, arrays, `struct`, `union`,
`enum`, `typedef`, and function pointers including in struct
initialisers.

Statements: `if`/`else`, `while`, `do`, `for`, `switch`/`case`/
`default`, `break`, `continue`, `return`, `goto` and labels, compound
statements with nested scopes.

Expressions: the whole operator set — arithmetic, bitwise, shifts,
comparisons, `&&`/`||` with short-circuit evaluation, `?:`, comma,
assignment and all ten compound assignments, prefix and postfix
`++`/`--`, `sizeof` on both types and expressions, casts, `&`, `*`,
array subscripting, `.` and `->`, direct and indirect calls.

Declarations: globals with constant initialisers including
address-of and string literals, `static` locals, `extern`, local
arrays and structs with brace initialisers and zero-fill, `char s[] =
"..."` sizing itself.

Preprocessor: `#include` with `-I` search and `""` vs `<>`,
object-like and function-like macros, `#` and `##`, the whole `#if`
family with a real constant-expression evaluator, `defined`, `#undef`,
`#pragma once`, `#error`, `#warning`.

### Absent, deliberately

| | why |
|---|---|
| floating-point arithmetic | The SOC has no FPU; GCC's float is soft-float library calls. `float` and `double` exist **as sizes only**, so that `z_obj_t`'s union lays out correctly — arithmetic on one is a clean error. |
| `long long` | rv32/ilp32 has no 64-bit register pair support here, and silently truncating would be worse than refusing. |
| bitfields | Nothing in `sw/common` uses them. |
| variadic function *definitions* | Calls to variadic functions work for up to 8 arguments, which covers `printf`. Defining one needs a register-save area. |
| more than 8 arguments | Everything past `a7` goes on the stack, which is a calling-convention change rather than a code-generation one. |
| structs by value as arguments or return values | Needs the indirect-return ABI. Pointers work. |
| designated initialisers, compound literals | C99 conveniences; nothing in the tree needs them yet. |
| VLAs, `_Generic`, atomics, threads | No. |
| **structs or unions by value** | **No, and now REFUSED rather than miscompiled.** An argument passed one word in `a0`, so a struct passed its address where the callee wanted its contents; a return arrived in `a0` alone, so an 8-byte `z_obj_t` came back as a 4-byte fragment that reads as `Z_NONE`. Both compiled silently and produced wrong answers. |
| two-word structs by value | Not yet, and this is the bounded fix. `z_obj_t` is exactly 8 bytes, which is the RISC-V ABI's register case -- `a0`/`a1` for an argument and for a return. Supporting that one size makes the whole `z_obj_*` family callable, which is most of libz ABI 2-4. |
| designated initialisers | **This is what blocks self-hosting.** `parse.c` uses `.kind = TY_VOID` in its own type table, so zcc cannot yet compile zcc. Phase 6's first job. |
| inline assembly | Anything needing it becomes a libz table entry instead -- see "The `maskirq` problem" below. |

Nothing on that list is architecturally blocked. Each is a bounded
addition to a specific file.

## The `maskirq` problem, and how it was solved

`zcc` reads all of `sw/common/zeitlos.h` **except one function**:

```c
static inline uint32_t maskirq(uint32_t new_mask) {
#if defined(__riscv)
    __asm__ volatile (".insn r 0x0B, 0x6, 0x03, %0, %1, zero" ...);
```

A raw picorv32 custom instruction, and there is no inline assembler.
Passing `-U__riscv` takes the header's own `#else` branch, which
returns 0 -- correct for a host build, and **wrong on target**, where
it silently disables the atomicity `z_fb_hw_line()` and the ENC28J60
driver depend on.

**Phase 2 answered this without an assembler.** `maskirq` is an entry
in the libz jump table (`docs/libz.md`): ordinary GCC-built code
behind an ordinary slot, called with a single `jal`. No new compiler
feature, now or ever.

That is the general answer, and it is worth knowing it exists before
the instinct to build an assembler takes hold: **anything `zcc` cannot
express becomes a table entry.** The cost is one indirection and one
line in `libz_table.def`.

**`-U__riscv` is no longer needed either.** `sw/common/zeitlos.h` now
has a `#if defined(__zcc__)` arm that declares `maskirq` rather than
defining it, so the header compiles as-is and the target-correct
branch is taken. That is a four-line change to `sw/common` and it
replaced something worse: `-U__riscv` selected the host stub, which
returns 0 and silently disables the masking. A wrong answer that
compiles is worse than a compiler that cannot read the header.

## Testing

```
cd sw/apps/zcc && make -f Makefile.host && ./tests/run.sh
```

Both `zcc` Makefiles build with **`-Werror`**, and that is not
tidiness. `zcc.h` was missing declarations for the three library
functions the compiler itself calls -- `malloc`, `free`, `vsnprintf` --
and GCC warned about the implicit declarations on every single build.
The warnings were there and were not read, because the build output
was being filtered for the word "error". GCC 14 promotes
implicit-function-declaration to an error by default, so the same
source that built here failed outright on a newer toolchain.

A warning nobody reads is a warning that becomes somebody else's
error. `-Werror` immediately caught a second one (`z_exit` undeclared
in `port_dev.c`) that had been present just as long, and a third
turned up later in `libz/syscall.c` — `malloc` and `free` with no
declaration in scope, which GCC 13 warned about and GCC 15 rejects.

`-Werror` applies to **each component's own sources only**, never to
the `sw/common` objects they compile: those have warnings that vary by
toolchain version and no app has business failing the build over code
it does not own. `libz`, which compiles `sw/common` sources directly,
uses the narrower `-Werror=implicit-function-declaration
-Werror=int-conversion` on its own files instead.

That pair is worth having even where blanket `-Werror` is too blunt.
An implicit declaration is never a style question: the compiler
*guesses* the prototype, and the guess is `int` — so a function
returning a pointer silently becomes a truncated integer, and on a
target with no MMU a truncated pointer is a wild store.

Every test is compiled **twice** — once with `zcc`, once with the
tree's RISC-V GCC — and both images run under `sim/zsim-headless`. The
test passes when the two outputs are identical.

Differential rather than golden-file, deliberately. A golden file
records what `zcc` did on the day it was written, which makes it
excellent at catching regressions and useless at catching a bug that
was there from the start. GCC does not share `zcc`'s bugs, so a
disagreement is a real one.

The accepted failure mode is that a wrong test passes, because both
compilers agree with each other and with nothing else. That is a much
better trade than the alternative.

`ZCC_NO_REF=1` skips the reference build where no RISC-V toolchain is
available; the tests then only check that the output runs to
completion, which is far weaker.

### What it caught

Both of these were found on the first run, and both are worth
recording because of how quiet they are:

- **Plain `char` is unsigned on RISC-V.** The psABI says so and GCC
  follows it; `zcc` had it signed, on the x86 assumption. Nothing
  fails to compile. The only programs that notice are ones comparing a
  `char` against a negative sentinel — which is to say every
  `getchar()`-style loop in existence.
- **An empty macro ate the rest of the file.** `#define EMPTY` with no
  body produced a bare end-of-stream sentinel, which was spliced into
  the token stream and stopped the rescan dead. It presented as
  "unterminated function body" forty lines later.

### A third, found by a header rather than a test

The preprocessor was originally two passes — run every directive, then
expand every macro. That is simpler and wrong, and `zeitlos.h` is what
exposed it:

```c
#define Z_MKSYSCALL(name, handler) Z_SYS_##name,
#include "syscalls.def"
#undef Z_MKSYSCALL
```

With expansion deferred, the `#undef` has already run by the time
anything is expanded, so every `Z_MKSYSCALL` in the included file
survives as a bare identifier and the enum will not parse. The X-macro
idiom depends on a macro's definition being the one in force *at that
point in the text*, so expansion is now interleaved with directive
processing.

Worth knowing because that idiom is load-bearing in this tree, and
because a preprocessor that handles `#define` and `#include` and
nothing else looks finished right up until it meets one.

## The runtime

`docs/libz.md` has it in full. In one paragraph: the runtime is built
once by GCC into a 5.4KB blob that `zcc` copies verbatim to `.text`
offset 0 of every image, and reaches through a jump table indexed by
slot. A call is `jal ra, table + 4*i`; the slot holds
`j real_function`, so the function returns straight to the caller with
no register clobbered. Two words inside the blob are patched by `zcc`:
`main`'s address, and the image's `_end`, where the heap starts.

Hello-world with `printf` is 5,588 bytes complete. The same program
against newlib would be north of 100KB, which is the reason libz
exists rather than a port of something.

## On the device

`sw/apps/zcc/Makefile` builds `zcc.bin` as **an ordinary Zeitlos app**:
sw/common's runtime, newlib, `sw/common/riscv-app.ld`, newlib's crt0 as
the entry point. The same shape as `repl`, `term` and everything else
in `sw/apps`.

It was not always, and the story is the useful part. The first version
was freestanding, with its own linker script and a hand-written
`_start`, linking libz instead of the C library. That was not a design
decision — it was an artefact of a development machine whose RISC-V
toolchain had no newlib and therefore could not link a normal app at
all. Every assumption that entry stub made was untested, and two of
them crashed a real system: `main()` was called with `a0`/`a1` holding
whatever the kernel had left in them, and the compiler read them as
`argc`/`argv`.

**A build shape adopted to work around the development environment is
a build shape nobody is testing.** There was nothing zcc needed that
an ordinary app does not have.

libz is still required, but only as *data*: `libz.bin` is the runtime
zcc embeds into the programs it produces, read from the card at run
time and not linked into zcc.

```
make -C sw/apps/zcc                # needs the RISC-V toolchain
./tests/run_dev.sh                 # runs it under sim/
```

### Where libz comes from: two copies, not one

This is the thing most likely to be misread, so it is worth being
exact.

**`zcc.bin` contains its own statically linked copy of libz.** GCC
links `libz/*.o` into the compiler the ordinary way, so `malloc`,
`printf`, `z_win_create` and `maskirq` all sit at real addresses
inside `zcc.bin`. That copy exists for *`zcc`'s own use* -- it is how
the compiler allocates and prints. It has nothing to do with the
programs it compiles.

**`libz.bin` and `libz.sym` are separate FILES on the card**, found
through `-L`, read at run time, and copied into each output. That copy
is what compiled programs call.

The two cannot be merged, and the reason is the MTU. libz inside
`zcc.bin` lives at addresses in *zcc's* `0x8000_0000` window; a program
`zcc` compiles gets its own window at the same virtual address, mapped
to different physical memory. So the compiler cannot hand out pointers
into its own runtime -- the blob has to be copied into the output
image, where it lands at that image's own base.

So a card that runs `zcc` needs both:

```
/zcc              the compiler, ~80KB
/libz/libz.bin   the runtime it embeds into what it builds, ~35KB
/libz/libz.sym   the table it resolves names against
```

Embedding the blob in `zcc.bin` as data would remove the second
requirement at the cost of 35KB on the compiler and a rebuild of `zcc`
whenever libz changes. Not done, because the files are also what makes
the ABI check meaningful: a mismatched blob is caught and refused
(`docs/libz.md`), and a blob welded into the compiler cannot mismatch
because it cannot be updated separately either.

Counting copies on a working card: one inside `zcc.bin`, one as
`libz.bin`, and one inside every compiled program. `docs/posix.md`
section 10's shared runtime collapses all of them into one.

### The seam

The compiler's own code is identical in both builds. What differs is
four functions -- read a file, write a file, print, stop -- behind
`zio_*` in `zcc_port.h`, implemented by `port_host.c` against stdio
and `port_dev.c` against libz and the filesystem syscalls.

Four functions in two files rather than `#ifdef`s through the lexer
and the emitter, because the device build is the one that matters and
the host build is the one that gets tested. Scattered conditionals
would mean the tested code and the shipped code diverge line by line,
in the files where a difference is hardest to see.

`ctype` is ours in both builds for a related reason: the host's is
locale-dependent by specification, and `isalpha` returning true for a
byte above 127 in some locale would silently change which identifiers
are legal depending on the environment the compiler was run from.

### What it costs to run

| | instructions | arena |
|---|---|---|
| a 10-line file including `libz.h` (so `zeitlos.h` too) | 10.3M | 1,024 KB |
| `t07_real.c`, 200 lines, no system headers | 6.9M | 384 KB |
| `l03_gfx.c`, with `zeitlos.h` + `zgfx.h` + `zwin.h` | 23.1M | 1,984 KB |

At roughly 12 MIPS (PicoRV32 at 48MHz) that is **about a second** for
the first and **two** for the last. Estimated, not measured on
hardware.

Memory lands where `docs/posix.md` guessed: about 1MB for a small file
with headers, under 2MB for a substantial one, against the 4MB
`Z_PROC_STACK_SIZE_HUGE` tier. The headroom is real but not enormous,
and it is dominated by **headers**, not by the program -- 200 lines
with no `#include` cost 384KB.

### The bug that only the device could show

`fs_size()` returns 0 both for a missing file and for an empty one.
`sw/os/fsapi.h` says so outright and calls it deliberate. The first
version of `zio_read_file()` treated that 0 as success and handed back
an empty buffer -- so when the preprocessor probed for a header by
trying each candidate directory in turn, the **first candidate that
did not exist "succeeded" with no content.** `#include "libz.h"`
expanded to nothing, silently, and the program failed with
`'printf' is not declared` on a line that had nothing wrong with it.

`fs_open_read()` is the unambiguous test: -1 for missing, a handle for
empty. The host build cannot reproduce any of this, because `fopen()`
tells the truth.

### The other one: `zalloc` was quadratic

The first device compile of a ten-line file took **729 million
instructions**. libz's `malloc` is a first-fit walk over a free list
-- fine for an app holding a few dozen allocations, ruinous for a
compiler making one per token.

`zalloc` is now a bump arena over 64KB blocks. **729M to 10.3M, a 70x
improvement**, and it costs nothing to use one because this allocator
already never frees. That fix was written down as the eventual answer
in `util.c` before it was needed; it was needed sooner than expected,
and only the device build could have shown it.

### The card's layout is the default search path

On the device, `zcc` searches `/libz/include` and `/libz` without being
asked, so:

```
zcc -o hello hello.c
```

finds the runtime and its headers. That is where the release puts them
(`release/lib/mkfatimg.py`) and the layout is fixed, so the default is
always right.

**Not on the host**, where the paths depend on where the tree is
checked out -- a default there would be wrong everywhere except one
machine, and wrong silently. The host build still wants
`-I include -I libz -I ../../common -L libz`.

The defaults are appended AFTER any `-I` given on the command line, so
an explicit path is still searched first. They apply under `-nolibz`
too: a freestanding program still wants `<stdint.h>`.

### Getting arguments to it

A Zeitlos process is started by name and nothing else -- `run zcc` at
the shell carries no arguments. So the device build reads its command
line from the launch argument (`z_launch_arg_take`), and failing that
from a file, `/zcc.args`.

**The file is a stopgap and should be read as one.** It exists so the
compiler is usable and testable before anything can pass it a real
argv, which is exactly what `posix` is for (`docs/posix.md`, Phase 4).

### What is not verified

Everything above runs under `sim/`, whose filesystem is a host
directory and whose CPU is an interpreter. Not covered: the real SD
card, the real scheduler and preempt-deferral, the 4MB allocation
actually succeeding against a live memory pool, and anything involving
`wm`.

## The compiler's own headers

`sw/apps/zcc/include/` has `stdint.h`, `stddef.h` and `stdbool.h`.
Its own rather than the host toolchain's: newlib's headers are full of
GCC attributes, builtins and `__extension__` that this compiler cannot
read, and the tree only ever wants the fixed-width names.

Sizes are rv32/ilp32: `int` and `long` are both 32 bits, `int32_t` is
`int`, and there is no `int64_t`.

## Diagnostics

`file:line:col: error: message`, which every editor's error parser
already understands — including `te`, which is the editor this will
most often be driven from once it runs on the device.

A function that is called but never defined is reported here too, with
the position of its first call. That used to reach the emitter as a
label nobody placed and come out as `internal error: label 26 (?) was
never placed` -- true, useless, and pointing at the compiler instead of
at the program. The commonest cause is not a typo but building against
`libz.h` **without** `-L`, so every `printf` and `malloc` is declared
and none is resolved; `-L` naming a directory with no runtime in it is
now an error at that point rather than a silent fall back to a
freestanding build.

There is **no error recovery**. A one-pass compiler that continues
past a parse error is generating code from a token stream it has lost
its place in, and every message after the first is invented. One true
message beats twenty plausible ones, particularly on a screen that
holds twenty-five lines.

## Memory

The allocator never frees. A compiler runs once, produces one output
and exits, so every allocation is live until it does.

That stops being free on the device, where the process's whole heap is
its stack allowance (`Z_PROC_STACK_SIZE_HUGE`, 4MB — `sw/os/kernel.h`)
and nothing reclaims anything until the process exits. It still holds,
but the number that matters becomes peak use rather than steady state.
If it ever stops fitting, the fix is arena allocation per function
body, not scattered `free()` calls.

Phase 3 will measure this properly. `docs/posix.md`'s budget estimates
about 1MB for a 2,000-line translation unit.

## Command line

```
zcc [-o FILE] [-I DIR] [-D NAME[=V]] [-U NAME] [-E] [-v] input.c
```

No `-c` (there are no object files), no `-l` (there is no linker), no
`-O` (there is one code generator and it always does the same thing).

`-E` prints the token stream. It is not needed to build anything and
is worth keeping anyway: a preprocessor bug and a parser bug look
identical from the outside, and being able to see the tokens is the
difference between five minutes and an afternoon.

## Files

| | |
|---|---|
| `sw/apps/zcc/zcc.h` | tokens, types, symbols, the emitter interface |
| `sw/apps/zcc/lex.c` | tokenizer |
| `sw/apps/zcc/cpp.c` | preprocessor |
| `sw/apps/zcc/parse.c` | parser and code generator |
| `sw/apps/zcc/emit.c` | RV32IM encoding, fixups, ZEXE writer |
| `sw/apps/zcc/util.c` | allocation, diagnostics |
| `sw/apps/zcc/include/` | the compiler's own headers |
| `sw/apps/zcc/libz/` | the runtime blob -- see `docs/libz.md` |
| `sw/apps/zcc/tests/` | the differential suite |
| `sw/apps/zcc/zcc_port.h` | the host/device seam: four functions |
| `sw/apps/zcc/port_host.c`, `port_dev.c` | the two sides of it |
| `sw/apps/zcc/Makefile.host` | host build |
| `sw/apps/zcc/Makefile` | device build, producing `zcc.bin` |

The sources live in `sw/apps/zcc` rather than `tools/` so that nothing
has to move when Phase 3 builds the same files as a Zeitlos app.

## See also

- `docs/zcc_bringup.md` — what to do to test this on a board
- `docs/libz.md` — the runtime, the jump table, and the ABI discipline
- `docs/posix.md` — the phase plan and the decisions behind it
- `docs/executables.md` — ZEXE, the output format
- `docs/app_runtime.md` — the runtime Phase 2 will wrap, and the
  `printf` size trap
- `sim/README.md` — where the code generator is developed and tested
