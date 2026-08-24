# `te` in `repl`

`sw/apps/repl` embeds [`te`](https://github.com/machdyne/te), a small
VT100-based text editor, as a git submodule at `sw/ext/te`, reachable
through the `te <filename>` command.

## Usage

```
> te myfile.txt
```

See `sw/ext/te`'s own `README.md` for the full editing/command
reference (arrow keys, Page Up/Down, `Esc :w` to save, `Esc :q` to
quit, the status-line format).

## Current limits

- **Files are capped at 2048 bytes by default** (`TE_MAX_FILE_SIZE`
  in `te_bridge.c`, overridable at build time, e.g. `make
  TE_MAX_FILE_SIZE=4096`). This is sized conservatively against
  `repl`'s 64KB process heap, which is shared with Scheme and all port
  traffic -- `te`'s per-line document representation can cost
  noticeably more memory than the raw file size. Check the `free`
  command before and after opening a file to see actual headroom on
  your board if you raise the ceiling.
- **Only one editing session at a time, process-wide.** A second
  `te <filename>` from a different `term` connection while one is
  already active is refused with a clear message, rather than
  corrupting the first session's document. If the connection holding
  the session disconnects uncleanly, the lock is released
  automatically (with no attempt to save).
- **Filesystem syscalls aren't interrupt-protected around individual
  FatFs calls**, so two processes both doing file I/O at the same
  time have a narrow window for corruption. Not a practical concern
  for typical single-user use of `te`; worth real mutual exclusion if
  concurrent filesystem access from more than one process ever proves
  to matter.

## Historical notes and implementation details

The following covers why this feature is built the way it is. It
isn't needed to use the editor.

### Why `repl`, not a new app

`repl` is already the general-purpose text-based app server `term`
talks to (a CLI, Machdyne Scheme, and now an editor). A separate
`textedit` app with its own port provider was considered and rejected:
it would cost its own static footprint plus a second, entirely
separate process heap, on boards where memory is already the binding
constraint (see `docs/scheme.md`'s sizing notes) -- for a feature
that, at least for now, only needs occasional use, not standing
readiness.

### Setup: adding the submodule

```
$ git submodule add https://github.com/machdyne/te.git sw/ext/te
```

`sw/ext/te/te.c` in this tree already carries the patch described
below applied directly -- not an upstream checkout plus a separate
patch step.

### The `te.c` patch: `-DTE_HOST_IO`

`te`'s own `-DEMBEDDED` build assumes a single global `getch()`/
`stdout` -- the right assumption for a target that's entirely this
editor, the wrong one for `repl`, which multiplexes several `term`
connections through one process and one shared port protocol
(`sw/common/zport.h`). The patch adds an optional `-DTE_HOST_IO` flag,
off by default -- every existing build (`-DCURSES`, or plain
`-DEMBEDDED` without this flag) is unaffected:

- Every `getch()`/`printf()`/`fflush(stdout)` call site goes through
  `te_host_getch()`/`te_host_write()`/`te_host_flush()` instead
  (declared in `te_host_io.h`, implemented by `repl`'s own
  `te_bridge.c`). `te.c`'s actual editing logic is untouched.
- `te_edit()` -- which normally runs its own blocking
  `while(te_yield());` loop until the user quits -- is split into
  `te_edit_start()` (initial load + first screen draw, returns) plus
  the existing `te_yield()`, callable independently, so it doesn't
  monopolize `repl`'s single message loop for as long as one `term`
  window stays in the editor.

### Filesystem access: a new capability, not just a `repl` change

Before this, no Zeitlos app could read or write a file at all -- only
`sh.c` (kernel code) could, by linking `sw/os/fs/fs.c` directly.
`te_load()`/`te_save()` need `fs_size()`/`fs_mallocfile()`/
`fs_write_file()`, so this had to be built:

- `sw/os/fsapi.c`/`.h`: three new syscalls, `Z_SYS_FS_SIZE`/`_READ`/
  `_WRITE`, appended to `sw/common/syscalls.def`. Kernel handlers call
  straight into the existing `sw/os/fs/fs.c` (FatFs) functions,
  reading/writing directly into the calling app's own buffer pointer.
- `sw/common/zfsapp.c`/`.h`: the app-facing wrappers, deliberately
  not added to `zeitlos.h`/`zeitlos.c` since that header is also
  compiled into the kernel, which separately declares the same three
  names with different signatures for its own direct-FatFs versions.
  Any app that wants file access links `zfsapp.c` and includes
  `zfsapp.h` directly.

### Output batching

`te_bridge.c` collects every write into a ~3KB staging buffer and
sends it as one `z_port_send()` call per input byte processed,
flushed exactly when `te.c` calls `TE_FLUSH()`.

### Responsiveness: a redraw bug found and fixed

Real-world finding, connected over a `term` window rather than a
local terminal: typing felt noticeably laggier than the plain CLI
prompt `te` is launched from. The cause was `te.c` itself: its
`te_yield()` called `te_redraw()` (a full erase-and-redraw of every
visible row) on every plain character typed and every backspace, not
just on scrolls/page navigation/resize where a full redraw is
actually necessary.

Fixed directly in `te.c` (not worked around in `repl`): a new
`te_redraw_line(int line)` redraws just one row in place, used
whenever an edit didn't change which lines are visible or how many
lines exist (a plain character insert, or a backspace that doesn't
merge two lines). Every edit that genuinely changes the screen layout
still gets the full `te_redraw()` it needs. This is a small,
additive change to `te.c` independent of anything Zeitlos-specific.

A second, smaller lever added at the same time: `void
te_status_bar(int enabled)` toggles whether `te_status()`'s line
count/input-state/cursor-position counters are shown. Defaults to on;
`te_bridge.c` calls `te_status_bar(0)` before starting a session as a
further reduction in per-keystroke bytes. The filename and any notice
(`SAVED`/`FAILED`) still always show either way.
