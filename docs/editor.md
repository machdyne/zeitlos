# `te` in `repl`

`sw/apps/repl` embeds [`te`](https://github.com/machdyne/te), a small
VT100-based text editor, as a git submodule at `sw/ext/te`, reachable
through the `te <filename>` command. This document covers the setup,
the memory-budget reasoning, and the filesystem-syscall groundwork
this needed that didn't exist before.

## Why `repl`, not a new app

`repl` is already becoming the general-purpose text-based app server
`term` talks to (a CLI, Machdyne Scheme, and now an editor -- see
`repl.c`'s own header comment). A separate `textedit` app with its own
port provider was considered and rejected for now: it would cost its
own static footprint plus a second, entirely separate process heap, on
boards where memory is already the binding constraint (see
`docs/scheme.md`'s own sizing notes) -- for a feature that, at least
for now, only needs occasional use, not standing readiness. Revisit if
`repl`'s own heap pressure (Scheme + `te` + ordinary port traffic, all
sharing one 64KB budget, `Z_PROC_STACK_SIZE_LARGE` in
`sw/os/kernel.c`) ever proves that tradeoff wrong in practice.

## Setup: adding the submodule

```
$ git submodule add https://github.com/machdyne/te.git sw/ext/te
```

`sw/ext/te/te.c` then needs the patch described below applied on top
(this tree's own `sw/ext/te/te.c`, if you unzipped this alongside an
existing checkout, already has it) -- either apply it by hand against
whatever upstream commit you pull, or overwrite `te.c` with the
version included here and commit that to your own fork/branch of the
submodule, same as `sw/ext/ms` (`docs/scheme.md`) is vendored
pre-patched rather than left as a bare upstream checkout.

## The `te.c` patch: `-DTE_HOST_IO`

`te`'s own `-DEMBEDDED` build (see its README.md) assumes a single
global `getch()`/`stdout` -- the right assumption for a target that's
entirely this editor, the wrong one for `repl`, which multiplexes
several `term` connections through one process and one shared port
protocol (`sw/common/zport.h`). The patch adds an **optional**
`-DTE_HOST_IO` flag, off by default -- every existing build (`-DCURSES`,
or plain `-DEMBEDDED` without this flag) is completely unaffected:

- Every `getch()`/`printf()`/`fflush(stdout)` call site `te.c` uses for
  its own interactive I/O goes through `te_host_getch()`/
  `te_host_write()`/`te_host_flush()` instead (declared in
  `te_host_io.h`, implemented by `repl`'s own `te_bridge.c` -- not
  provided by `te.c` itself). `te.c`'s actual editing logic (the
  state machine, the line-list document, `te_load()`/`te_save()`) is
  completely untouched.
- `te_edit()` -- which normally runs its own blocking
  `while(te_yield());` loop until the user quits -- is split into a
  new `te_edit_start()` (does the initial load + first screen draw,
  returns) plus the existing `te_yield()`, callable independently.
  This matters because `te_edit()`'s own loop would otherwise
  monopolize `repl`'s single message loop for as long as one `term`
  window stays in the editor, starving every *other* connected
  window. `repl` instead calls `te_yield()` once per incoming byte,
  from its own event loop -- exactly the "cooperative main loop"
  mode `te`'s own README.md already anticipates for `getch()`'s
  non-blocking contract, just applied to the whole interaction, not
  only key reads.

A second, unrelated change is also in this patch: the per-keystroke
full-screen-redraw fix and the `te_status_bar()` toggle -- see
"Responsiveness" below. Unlike the `-DTE_HOST_IO` work above, that
change isn't gated behind any flag; it changes `te_redraw()`'s own
call sites for every build, desktop included, on the theory that a
real bug (redrawing far more than necessary on every keystroke) is
worth fixing everywhere, not just for Zeitlos.

## Filesystem access: a new capability, not just a `repl` change

Before this, **no Zeitlos app could read or write a file at all** --
only `sh.c` (kernel code) could, by linking `sw/os/fs/fs.c` directly.
`te_load()`/`te_save()` need exactly the three functions its own
README.md's "Embedded targets" section already specifies
(`fs_size()`/`fs_mallocfile()`/`fs_write_file()`), so this had to be
built rather than worked around:

- `sw/os/fsapi.c`/`.h`: three new syscalls, `Z_SYS_FS_SIZE`/
  `_READ`/`_WRITE`, appended to the end of `sw/common/syscalls.def`
  (append-only, per that file's own comment -- never inserted
  earlier). Kernel handlers call straight into the existing
  `sw/os/fs/fs.c` (FatFs) functions, reading/writing directly into the
  **calling app's own buffer pointer** -- no kernel-side `malloc()`
  involved at all (see `fsapi.h`'s own comment on why: a syscall
  doesn't need `msg.c`'s `z_translate()`, since the MTU mirror is
  still pointing at the calling process for the syscall's whole
  duration, and `pidreg.c` already found one real newlib-reentrancy
  bug in kernel-compiled code that argued for staying away from more
  libc machinery than necessary in kernel context).
- `sw/common/zfsapp.c`/`.h`: the app-facing wrappers
  (`fs_size()`/`fs_mallocfile()`/`fs_write_file()`), deliberately
  **not** added to `zeitlos.h`/`zeitlos.c` -- that header is also
  compiled into the kernel (via `kernel.h`), which separately includes
  `sw/os/fs/fs.c`'s own `fs.h`, already declaring these exact three
  names with different signatures for its own direct-FatFs versions.
  Any app that wants file access links `zfsapp.c` and includes
  `zfsapp.h` directly.
- **Known limitation, not fixed here:** the new syscall handlers don't
  mask interrupts around their FatFs calls, so a KTIMER preemption
  mid-`f_read()`/`f_write()`, onto a different process that also makes
  an FS syscall before the first one's `f_close()` runs, is a real (if
  narrow) corruption window. This class of gap already existed for
  `sh.c`'s own direct FatFs calls; it's just reachable from more call
  sites now. Worth real mutual exclusion if concurrent FS access from
  more than one process ever proves to matter in practice -- see
  `fsapi.h`'s own comment for the shape that fix would take.

## Memory budget: why files stay small

`repl`'s entire process heap is 64KB, shared with Machdyne Scheme's
own cell heap and every port connection's in-flight message buffers
(`docs/scheme.md`). `te`'s line-list document representation (a
`struct te_line_t` **plus a separately `malloc()`'d text buffer, per
line**) costs noticeably more than the raw file size for ordinary
prose, and can cost dramatically more for a pathological many-short-
lines file -- a few hundred bytes of blank lines can turn into tens of
KB of per-line allocator overhead alone.

`te_bridge.c` enforces a conservative **2048-byte** default ceiling
(`TE_MAX_FILE_SIZE`, overridable at build time, e.g. `make
TE_MAX_FILE_SIZE=4096`) on top of `fs_size()`, checked *before* ever
calling into `te`. This is deliberately not "a simple fraction of
64KB" -- it's sized against the worst realistic case, not the average
one. If you raise it, check `free` (an existing `repl` command) before
and after opening a real file to see actual headroom on your board,
rather than assuming the ceiling alone is a safe bound.

A partial-load-under-memory-pressure (a `calloc()`/`malloc()` failing
partway through `te_load()`) fails silently per-operation (`te.c`'s
own `te_insert()`/`te_insert_line()`/`te_split_line()` just return
without effect on a failed allocation) rather than crashing, but can
leave a visibly incomplete document -- another reason to stay well
under the ceiling rather than test its exact edge.

## Only one editing session at a time, process-wide

`te.c`'s own document/cursor/filename state is all file-static globals
inside `te.c` -- there is exactly one possible editing session per
process, not per connection. `te_bridge.c` enforces this at the
`repl` level: a second `te <filename>` from a different `term`
connection while one is already active is refused with a clear
message ("editor is already in use by another connection"), rather
than silently corrupting the first session's document. If the
connection that owns the live session disconnects uncleanly (its
`term` crashes or closes without `Esc :q`), `repl`'s `handle_close()`
releases the lock automatically -- with no attempt to save first,
since there's no connection left to report success or failure to.

## Output batching

`te.c` redraws the **whole visible screen** on every keystroke -- or
did; see "Responsiveness" below, which fixes the actual dominant cost
there. What's left after that fix is still batched: `te_bridge.c`
collects every write into a ~3KB staging buffer and sends it as one
`z_port_send()` call per input byte processed (flushed exactly when
`te.c` itself calls `TE_FLUSH()`, at the end of every `te_status()`
call -- always the last thing a keystroke's worth of processing does).

## Responsiveness: `te_redraw()` firing on every keystroke

Real-world finding, connected over a `term` window rather than a
local terminal: typing felt noticeably laggier than the plain CLI
prompt `te` is launched from. The cause turned out to be `te.c`
itself, not anything port/message-specific -- its own `te_yield()`
called `te_redraw()` (a full erase-and-redraw of every visible row,
`CONTENT_ROWS` of them, ~2KB+ of VT100 output) on **every plain
character typed, and every backspace** -- not just on scrolls/page
navigation/resize, where a full redraw is actually necessary. Each of
those bytes is also a byte `term`'s VT100 emulator has to re-parse,
and a byte briefly duplicated in `repl`'s own tight 64KB heap
(`z_obj_blob()`, `zport.c`) for the duration of one `z_port_send()`
call -- multiplied by one redraw per keystroke, this was the actual
dominant cost, not the status line's coordinate counters (a status
line update is on the order of a dozen bytes; a full-screen redraw is
two to three orders of magnitude more).

Fixed directly in `te.c` (not worked around in `repl`): a new
`te_redraw_line(int line)` redraws just one row in place -- correct
to use instead of `te_redraw()` exactly when an edit didn't change
which lines are visible or how many lines exist (a plain character
insert, or a backspace that doesn't merge two lines). `te_yield()`'s
`STATE_NONE` branch now uses it for those two cases; every edit that
genuinely changes the screen layout (Enter/line-split, a
line-joining backspace, arrow/page navigation, load, resize) still
gets the full `te_redraw()` it actually needs -- including the
existing scroll-adjustment fallback at the end of `te_yield()`, which
still forces a full redraw itself if a "cheap" edit turns out to have
scrolled the view after all (e.g. a line just grew past the right
edge). This is a small, additive, easy-to-audit change to `te.c` --
worth committing upstream on its own merits, independent of anything
Zeitlos-specific.

## `te_status_bar()`: hiding the coordinate counters

A second, smaller lever, also added to `te.c` directly: `void
te_status_bar(int enabled)` toggles whether `te_status()`'s line
count/input-state/cursor-position counters (`l%i s%i x%i y%i`) are
shown. **Defaults to on** (`1`) -- every existing build (desktop,
`-DCURSES`, or a plain embedded target that never calls this) sees
exactly the unchanged status line. `te_bridge.c` calls
`te_status_bar(0)` before starting a session, purely because it's a
free reduction in per-keystroke bytes on top of the redraw fix above,
not because it was ever the main cost -- see its own comment there.
The filename and any notice (`SAVED`/`FAILED`) still always show
either way.

## Usage

```
> te myfile.txt
```

See `sw/ext/te`'s own `README.md` for the full editing/command
reference (arrow keys, Page Up/Down, `Esc :w` to save, `Esc :q` to
quit, the status-line format) -- `term`'s own key-to-bytes mapping
(`sw/apps/term/term.c`) already sends exactly the VT100/xterm escape
sequences `te.c` expects, so no changes were needed on that side.
