# Scheme in `repl`

`sw/apps/repl` embeds [Machdyne Scheme](https://github.com/machdyne/ms)
(`ms`) as its primary language, via a git submodule at `sw/ext/ms`.
This document covers the one-time setup, the memory-layout reasoning
behind how it's built, and what's actually reachable right now.

## Vendored, not a submodule checkout you need to patch

`sw/ext/ms/ms.c` and `sw/ext/ms/ms_stdlib.l` in this tree are the
complete files, already carrying the changes below applied directly --
not upstream originals plus a separate `.patch` step. Unzip/copy this
tree over your working copy and `sw/ext/ms/ms.c` is immediately
correct, nothing further to apply.

Pinned against upstream [machdyne/ms](https://github.com/machdyne/ms)
commit `e33a2f9772d17b640b74e36a74ee6981520f9710` -- if you want to
pull a newer upstream commit later, the three changes below are small
and easy to re-apply by hand (each is a short, self-contained,
heavily-commented diff from that commit's own `ms.c`); there's no
tooling here that assumes a `git submodule` relationship specifically,
these are just the files as they need to be.

Three changes from upstream, all additive -- upstream's own default
(non-`-DLIX`, non-`MS_STATIC_HEAP`) behavior is completely unaffected:

1. `ms_to_string()` made non-`static` (a plain visibility change, no
   logic touched) -- `ms_print()` hardcodes `printf()`-to-stdout,
   which is the wrong destination for a result that needs to go down
   a port or into a `Z_REPL_RESULT` reply instead of this process's
   own UART debug console.
2. `MS_STATIC_HEAP`, an opt-in compile-time macro -- see "Why a
   static heap" below for the reasoning, "Sizing" for the practical
   numbers.
3. A real number-formatting ABI bug, found and fixed on real
   hardware -- see "A real ABI bug this integration hit and fixed"
   below.

## Why a static heap

Upstream `ms_init()` does one large runtime `malloc()` for the cell
heap (`MS_HEAP_SIZE * sizeof(ms_val)`) and another for the GC protect
stack, both sized at compile time but allocated lazily, at first use --
see `ms`'s own README, "Session lifecycle". That assumes a target
where a process can grow its heap well past its own static footprint
on demand, which Zeitlos's process model doesn't provide:
`k_proc_create()` (`sw/os/kernel.c`) is always called with exactly
`fs_size(binary)` -- the app's own compiled-in static size, code +
data + `.bss` -- as the ENTIRE memory budget for that process, for its
whole lifetime (see `sh.c`'s `run` command, `init()`, and
`k_proc_run()` -- no caller anywhere passes anything beyond the static
size). `k_mem_alloc()` (`sw/os/mem.c`) only enforces a `Z_MEM_MIN_BLOCK_SIZE`
(32KB) *minimum*, it never rounds a request up further than that -- so
a process's own runtime `malloc()`/`_sbrk()` (`docs/app_runtime.md`)
only ever has as much headroom as `k_proc_create()` happened to leave
between the static footprint and whatever that 32KB-rounded block's
actual size turned out to be. For `repl`'s own small static footprint,
that's nowhere near enough for `ms`'s default multi-thousand-cell
heap -- `malloc()` would either fail outright or succeed and then
immediately collide with the stack (`_sbrk()`'s one safety check,
`docs/app_runtime.md`).

`MS_STATIC_HEAP` (the patch) replaces both `malloc()` calls with plain
compile-time-sized static arrays instead. That moves the heap into
`repl`'s own `.bss`, which means it's now counted as part of the
binary's own `_end` -- exactly the number `fs_size()` reports and
`k_proc_create()` requests. The heap's size becomes a build-time
fact, not a runtime gamble against however generously (or not) the
allocator happened to round up an oversized `malloc()`.

## Sizing

`sw/apps/repl/Makefile`'s `MS_HEAP_SIZE`/`MS_PROTECT_STACK_SIZE`
(default 4000 cells / 192 protect-stack slots) are overridable at the
command line (`make MS_HEAP_SIZE=8000`). 4000 was chosen empirically --
comfortable headroom for the full `ms_stdlib.l` to load plus ordinary
interactive use, tested on-host against the real stdlib source, not a
guess. `ms_val` is a tagged union whose largest member is 3 pointers
(the `T_LAMBDA` case) plus a 2-byte header, so cell size depends on
target pointer width -- work out the real number for a given
`MS_HEAP_SIZE` from `sizeof(ms_val)` on the actual build (`ms.c`'s own
struct definition, `struct ms_val`) before assuming a specific KB
figure, rather than trusting a number written down here that could go
stale.

Two real costs worth knowing about, not just the raw cell count:

- **`T_NUM` is a `double`.** This SoC's RV32I core has no hardware
  float -- every Scheme arithmetic operation goes through software
  floating point (`libgcc`'s soft-float routines, pulled in
  automatically), which is measurably slower than integer ops at
  48MHz. Not fixed by this integration; worth keeping in mind for
  anything performance-sensitive written in Scheme later.
- **One shared heap for the whole `repl` process**, not one per
  connection -- see `repl.c`'s own header comment on why (one shared
  `ms_global_env`, run a second `repl` instance for real isolation).
  `MS_HEAP_SIZE` has to be sized for whatever the busiest realistic
  combination of concurrent connections needs, not per-connection.

## What's reachable right now

`ms_init_lix(true)` runs once, in `main()`, loading the full
`ms_stdlib.l` (compiled in as a C string via
`sw/apps/repl/gen_ms_stdlib.py` -- see that script's own header
comment; NOT committed to the repo, regenerated by the Makefile
whenever the submodule's stdlib source is newer than the last build).
A boot-time failure (most likely an undersized `MS_HEAP_SIZE` for
whatever's actually in the stdlib) is non-fatal to `repl` itself --
builtin commands keep working, Scheme evaluation reports unavailable
rather than crashing the process.

Any line that doesn't match a builtin command (`help`, `ping`,
`uptime`, `echo`, `free`, `port <name>`, `quit`, the explicit
`scheme <expr>` prefix) is evaluated as Scheme by default -- `dispatch_line()`'s final fallback,
`sw/apps/repl/repl.c`. Builtins always win on a match first (typing
`help` always shows the builtin help text, never an "unbound symbol"
error, even if a user later `define`s their own Scheme binding named
`help` -- the builtin table and Scheme's own global environment are
two separate namespaces that only happen to share dispatch). One real
limitation as of this revision: each line is read and evaluated as
exactly one Scheme form on its own -- there's no multi-line/paren-
balance continuation across separate typed lines yet (a form split
across more than one line isn't handled), and `Z_REPL_EVAL` requests
(the non-interactive path, see "REPL_EVAL" below) only evaluate the
FIRST form in whatever text they're sent, silently ignoring anything
after it -- both are known, accepted gaps for now, not yet fixed.

Panics (a bad form like `(car 5)`) are caught via `ms`'s own opt-in
panic-recovery mechanism (`ms_panic_before_try()`/`setjmp`/
`ms_panic_after_recover()`, see `ms.c`'s own extensive comment on the
exact usage pattern this requires, and `sw/apps/repl/ms_api.h` for why
it can't be wrapped in a shared helper) -- a bad form reports an error
back down the port/reply message, it doesn't take the whole `repl`
process (and therefore every OTHER connection's session) down with it.

## `sw/apps/repl/ms_api.h`

`ms.c` has no separate public header of its own -- built with `-DLIX`
it has no `main()` at all (its REPL/`main()` section is `#ifndef LIX`),
so it's meant to be compiled as a standalone `.o` and linked against
an embedder's own `main()`, but every function an embedder would
actually call is declared inline in `ms.c` itself, mixed in with its
own `static` internals. `ms_api.h` is hand-copied from just the
non-`static` subset of those declarations -- keep it in sync BY HAND
if `sw/ext/ms` is ever bumped to a commit that changes one of them;
there's no build-time check that the two agree, a mismatch just fails
to link or (worse) misbehaves at runtime against a genuinely wrong
prototype. Small and easy to audit at this size; patching `ms.c` to
split out a real `ms.h` upstream would be a much bigger diff for not
much benefit here.

## Memory reporting: the `free` command

`ms_heap_used()`/`ms_cell_size()` (two more tiny accessors, same
pattern as `ms_to_string()`) expose what `ms.c` already tracks
internally -- live cell count and `sizeof(ms_val)` on this build --
otherwise invisible from outside since `ms_val` is deliberately opaque
in `ms_api.h`. `free` (`sw/apps/repl/repl.c`) reports three numbers,
none of which needed a new syscall:

- **Scheme heap** -- cells used/total (`ms_heap_used()`/`MS_HEAP_SIZE`,
  the latter already known to `repl.c` at compile time via the same
  `-DMS_HEAP_SIZE` its own Makefile passes to both translation units).
- **C heap** -- bytes grown via `malloc()` since boot, computed as
  `sbrk(0) - &_end` (the standard "where's the break right now"
  idiom) -- for `repl` specifically this is almost entirely `ms`'s own
  `T_STR`/`T_VECTOR` payloads (the only two `ms_val` types that own
  `malloc`'d memory -- everything else lives entirely inside the
  fixed cell heap above).
- **Static footprint** -- `&_end - &_start`, the same computation
  `kernel.c`'s own `main()` already does for its own binary, fixed at
  build time.

What it does NOT show: the kernel's own view of this process's total
allocated block (`k_mem_alloc()` rounds up to at least a 32KB minimum,
`mem.h`'s `Z_MEM_MIN_BLOCK_SIZE`) -- there's no syscall yet for a
process to ask the kernel that about itself. Left for later, along
with the rest of the system-API surface.

## Redirecting a terminal: the `port <name>` command

`port <name>` (e.g. `port portdemo0`) asks the SPECIFIC `term`
instance that typed it to disconnect from `repl` and connect to a
different port provider instead -- mainly useful for testing/debugging
the port protocol itself against `portdemo`'s simpler raw-echo
behavior (`sw/apps/portdemo`) without needing a second `term` window.

The mechanism: `sw/common/zterm.h` defines one new, small, fire-and-
forget control message, `Z_TERM_SET_PORT` -- sent directly to that
`term`'s own pid (`repl` already knows it, it's the connection's own
`z_port_t.peer_pid`), not through the port channel itself, since this
is asking `term` to change which connection it has, not exchanging
data over one that already exists. `term.c`'s own connection logic
(previously inline in `main()`, only ever run once at startup) is now
`connect_port()`, a small reusable function called both at startup
(`connect_port("repl0", Z_PID_REPL)`, with the well-known default's
fixed-pid fallback) and on `Z_TERM_SET_PORT` (`connect_port(name, 0)`
-- no fallback for a caller-specified name, if the lookup fails
there's nothing sensible to guess). `repl` closes its own end of the
connection proactively too (same "end this session" signal `quit`
already used), rather than waiting for `term`'s own close to arrive.

One honest, accepted limitation: the "requested switch..." confirmation
text `repl` sends back very likely never actually appears on screen --
`Z_TERM_SET_PORT` arrives at `term` before that confirmation text does
(both queued in the same connection's message stream, in that order),
and once `term` processes `SET_PORT` it immediately blocks inside
`z_port_connect()` for the new target, which discards any other queued
message (including that confirmation, and the `Z_PORT_CLOSE` right
after it) while it waits. Harmless -- the real evidence the switch
worked is the new provider's own banner showing up right after -- just
don't expect to see the confirmation linger.

## Historical notes

### A real ABI bug this integration hit and fixed

`scheme (+ 1 2)` (or, after the default-fallback change above, just
`(+ 1 2)`) initially printed `12884901888` instead of `3` -- exactly
`3 << 32`. `ms`'s own number-formatting code (`print_double()`, and
`tostr_inner()`'s `T_NUM` case -- the second is what backs
`ms_to_string()`, the function `repl.c` actually calls) detects a
whole-number `double` and formats it with
`printf("%lld", (long long)d)`. On this project's actual RV32I target
(`-mabi=ilp32` -- no hardware float, so under a pure soft-float ABI a
`long long` and a `double` are passed to a variadic function exactly
the same way, both as an 8-byte value split across the integer-
register/stack argument path), that 64-bit value's two halves came
back swapped or offset by one register slot -- the value `3` was
present, just in the wrong half. Confirmed on-target via `repl`'s own
`scheme`/default-Scheme path; not yet root-caused to a specific layer
(this toolchain's variadic long-long marshaling specifically, vs.
something in the double-to-int64 conversion feeding it), and not
reproducible on a desktop host build, where the ABI is different.

Fixed in the patch (`ms_num_to_str()`, next to `print_double()` in
`ms.c`) by never materializing or passing a 64-bit integer through a
variadic call at all -- digits are extracted one at a time with
`fmod()`/`floor()`, staying entirely in `double` and single-digit
`int` (32-bit, one register, no marshaling ambiguity possible) the
whole way. Exact for any double that's already a genuine integer
within a double's 53-bit exact-integer range, which both call sites'
existing `fabs(d) < 1e15` guard already assumes. Verified against
negative numbers, zero, `-0.0`, and 12-digit values -- all correct.
The `%g` (fractional-number) path is untouched and unconfirmed either
way -- same ABI class of risk in principle (a `double` argument goes
through the identical variadic-passing mechanism as the `long long`
case that WAS confirmed broken), but no evidence yet that it's
actually broken, and hand-rolling a general shortest-round-trip float
formatter with confidence, without a way to test it on-target, is a
substantially bigger and riskier undertaking than the whole-number
fix above. Worth testing deliberately on hardware (e.g. `(/ 1 2)`
should print `0.5`) -- flag it if a fractional result ever looks wrong
the same way the integer one did.
