# The Zeitlos Scheme API

`repl` (`sw/apps/repl`) embeds [Machdyne Scheme](https://github.com/machdyne/ms)
(`docs/scheme.md`). Since there is no C compiler on the system, **this
is how most Zeitlos software will actually get written** -- this
document is both the reference for what's callable from Scheme today,
and the design record for how new capabilities get added without
regularly touching the `ms` submodule.

> **Status:** \S1-\S3 (the mechanism) and every procedure in \S4
> (Files, Windows, Messaging, Networking) are implemented. Each batch
> was deliberately done and tested in isolation before building the
> next one on top of it.

## 1. Two ways to invoke a procedure

### 1a. Real Scheme, unchanged

Anything typed at `repl`'s prompt that doesn't match a builtin CLI
command (`help`, `port`, `telnet`, `te`, `quit`, ...) is handed to
`ms_eval()` exactly as typed, same as today:

```
> (+ 1 2)
3
> (define (square x) (* x x))
> (square 5)
25
```

### 1b. Bare command syntax, translated into Scheme

Typing a procedure name as the first word of a line, followed by
plain space-separated arguments, also works:

```
> ls
("/WM" "/TERM" "/NET")
> ls /SUBDIR
("/SUBDIR/FOO" "/SUBDIR/BAR")
```

This is translated, before evaluation, into the equivalent Scheme
form (`translate_command_line()`, `sw/apps/repl/repl.c`) --
`(ls)`, `(ls "/SUBDIR")` -- and evaluated exactly as if you'd typed
that. **This is sugar, not a separate command system**: `ls` and
`(ls)` reach the exact same Scheme procedure, defined in exactly one
place, and typing the real form yourself always works too -- it's the
only way to pass something that isn't a string/number/boolean
literal (a variable, a sub-expression, a list).

#### Which names get this treatment

**Any name currently bound to a callable procedure** (a builtin or a
closure) in the global environment -- checked dynamically
(`ms_is_callable()`, `sw/ext/ms/ms.c`) at the moment a line is typed,
not a separate maintained list. This was originally proposed as a
curated allow-list (see the design discussion this document grew out
of); simplified to "any bound procedure" since it costs nothing extra
to add a new procedure this way (one `ms_def_builtin()` call already
makes it both Scheme-callable and bare-word-callable, no second list
to keep in sync), and the one real ambiguity it introduces has an
existing, sufficient escape hatch (see below).

An unbound first word, or one bound to something that ISN'T callable
(an ordinary variable), falls through completely unchanged --
`eval_scheme()` sees exactly the line as typed, same as before this
mechanism existed. This is what keeps `myvar` alone still printing
`myvar`'s value rather than erroring "attempt to call a non-procedure".

#### Argument translation rule

Each token after the command name becomes:

| token looks like | becomes |
|---|---|
| a number, and the WHOLE token parses as one (`10`, `-3.5`) | a Scheme number, unquoted |
| `#t` / `#f` / `#true` / `#false` | a Scheme boolean, unquoted |
| already starts with `"` or `(` | passed through unquoted (you wrote real Scheme yourself) |
| anything else | wrapped in `"..."` as a string literal (`"`/`\` escaped) |

So `line 10 10 100 100 red` becomes `(line 10 10 100 100 "red")`.
`tget 192.168.1.1 firmware.bin` becomes
`(tget "192.168.1.1" "firmware.bin")` -- **not** `(tget 192.168
"firmware.bin")` -- the number check requires the *entire* token to
parse as a number (`strtod()` must consume all of it), so a dotted-
quad IP correctly falls through to the string case instead of being
silently truncated at its second `.`. Verified with a standalone
harness against the real reader/evaluator before this landed in
`repl.c`.

There's no way to pass a bare symbol or a variable reference through
bare-word syntax -- `line mywin 10 10 100 100` sends the *string*
`"mywin"`, not the value of a variable named `mywin`. Write real
Scheme (`(line mywin 10 10 100 100)`) for that. Window/connection
handles are plain Scheme **numbers** (\S4) specifically so the common
case -- passing a handle you just got back from a `create` call -- still
works fine through bare-word syntax without needing this escape hatch.

There's also no quoted-argument support in the tokenizer itself (a
filename with a space in it can't be typed at the bare prompt) -- FAT
short names make this a non-issue in practice.

#### `scheme <expr>` is the explicit escape hatch

Already existed, and resolves the one real ambiguity "any bound
procedure" introduces: `scheme <expr>` always evaluates `<expr>` as
literal Scheme, bypassing bare-word translation entirely -- e.g.
`scheme ls` evaluates the *symbol* `ls` (prints `#<builtin>`) rather
than calling it.

## 2. What each command actually costs

Per new Scheme-reachable procedure: **one `ms_def_builtin()` call**
plus the C function it points to. `ms_def_builtin()` interns the name
(a symbol -- one cell, plus a small one-time `malloc()` for the name
string, skipped entirely if that name is already interned) and conses
one binding into the global environment (two more cells). That's the
entire fixed cost of *adding* a procedure to the language -- with
`MS_HEAP_SIZE` at its current default (4000 cells, `docs/scheme.md`),
adding even a few dozen procedures this way is noise against the
budget; the C body itself (and whatever it allocates while running,
freed again once it returns) is where any real cost lives, same as
every existing `bi_*` builtin already inside `ms.c`.

## 3. The registration mechanism (implemented -- 4th patch to `ms.c`)

`ms.c`'s own `def_builtin()`/`mk_fun()`/`mk_sym()`/`env_define()` are
all `static` -- reachable only from inside `ms.c` itself. This adds
**one** more small, additive, non-static export, alongside the three
`docs/scheme.md` already documents (`ms_to_string` made non-static,
`MS_STATIC_HEAP`, the number-formatting ABI fix):

```c
void ms_def_builtin(const char *name, ms_builtin fn) {
	env_define(ms_global_env, mk_sym(name), mk_fun(fn));
}

bool ms_is_callable(const char *name) {
	ms_val *v = env_lookup(ms_global_env, mk_sym(name));
	return v && (v->type == T_FUN || v->type == T_LAMBDA);
}
```

Plus a handful of tiny, always-GC-safe constructor/accessor wrappers a
builtin body needs to build its return value and inspect its
arguments, without breaking `ms_val`'s deliberate opacity
(`ms_api.h`'s own comment on why it stays opaque):

```c
ms_val *ms_mk_str(char *owned);     /* takes ownership -- must be malloc'd */
ms_val *ms_mk_num(double n);
ms_val *ms_mk_bool(bool b);
ms_val *ms_nil_val(void);
ms_val *ms_mk_str_list(char **items, int count);  /* takes ownership of each item */

bool ms_is_str(ms_val *v);
bool ms_is_num(ms_val *v);
bool ms_is_pair(ms_val *v);
bool ms_is_nil(ms_val *v);
const char *ms_str_val(ms_val *v);
double ms_num_val(ms_val *v);
ms_val *ms_car(ms_val *v);
ms_val *ms_cdr(ms_val *v);
```

`ms_log()` (the panic/error-raising function every `bi_*` builtin
already calls) was also made non-static, same "plain visibility
change" as `ms_to_string()`'s own existing patch -- so a Zeitlos
builtin can raise a real Scheme error exactly the way `ms.c`'s own do:

```c
static ms_val *zapi_file_size(ms_val *args) {
	ms_val *name = ms_car(args);
	if (!ms_is_str(name)) ms_log(MS_PANIC, "file-size: expected a string");
	int sz = fs_size((char *)ms_str_val(name));
	return sz > 0 ? ms_mk_num(sz) : ms_mk_bool(false);
}
```

This is the *entire* recurring `ms.c` footprint -- confirmed by
building a standalone test harness against the real, patched `ms.c`
and the real generated `ms_stdlib.h`: registering a native builtin,
checking `ms_is_callable()`, evaluating `(ls)` through the real
reader, and triggering the panic path via `ms_log()` all worked
exactly as designed. Every future Zeitlos procedure after this patch
is added purely in `sw/apps/repl/zapi.c` -- `ms.c` doesn't need
touching again for it.

**Note on the vendored base commit:** `docs/scheme.md` pins `ms.c`
against upstream commit `e33a2f9`. Checking upstream directly: the
*current* upstream `machdyne/ms` HEAD already has all three of that
document's patches merged into its own mainline (same author/org) --
this 4th patch was built and tested against that current HEAD rather
than hand-reconstructing the three from the older pin, since it needs
zero guesswork and is objectively closer to whatever's actually
vendored today. Worth reconciling `docs/scheme.md`'s pinned-commit
line against your actual `sw/ext/ms` checkout when you next touch it,
if it turns out to disagree.

### `zapi.c`: where new procedures actually live

`sw/apps/repl/zapi.c`, one file, holding every Zeitlos-specific
`ms_builtin` function plus a single registration entry point called
once from `repl.c`'s `main()`, right after `ms_init_lix()` succeeds:

```c
void zapi_register(void) {
	ms_def_builtin("ls", zapi_ls);
	ms_def_builtin("file-size", zapi_file_size);
	ms_def_builtin("read-file", zapi_read_file);
	ms_def_builtin("write-file", zapi_write_file);
	ms_def_builtin("delete-file", zapi_delete_file);
	/* future procedures just add another line here */
}
```

### A `bi_*`-shaped example

Every `zapi_*` function has exactly the shape `ms.c`'s own builtins
already do (`ms_val *(*)(ms_val *args)`, args pre-evaluated) --
nothing Zeitlos-specific about the calling convention itself.

## 4. API surface

Handles (windows, open files, connections) are plain **Scheme
numbers** throughout, not a special type -- `ms_val` has no
embedder-extensible tag, and every Zeitlos id (pid, window id, FatFs
handle) is already a small integer in the C code, so this needs no new
representation and composes naturally with bare-word argument
translation (\S1b).

### Files -- IMPLEMENTED

| Procedure | Behavior |
|---|---|
| `(ls)` / `(ls "/path")` | list of filenames under `path` (root if omitted), each a full `/`-prefixed path (e.g. `"/WM"`) |
| `(file-size "name")` | size in bytes, or `#f` if missing (or genuinely empty -- see caveat below) |
| `(read-file "name")` | whole file contents as a string, or `#f` |
| `(write-file "name" "contents")` | create/truncate + write; `#t`/`#f` |
| `(delete-file "name")` | `#t`/`#f` |

Backed by two new syscalls (`sw/os/fsapi.c`, following the exact
"no kernel `malloc()`, caller-owned buffers" conventions the original
`FS_SIZE`/`_READ`/`_WRITE` syscalls established for `te`,
`docs/editor.md`):

- **`FS_UNLINK`** -- thin wrapper over the existing `fs_unlink()`
  (`sw/os/fs/fs.c`), which already did the real work, just never had
  an app-facing syscall.
- **`FS_LIST`** -- new. `fs_list_dir()` (`sw/os/fs/fs.c`) only ever
  *printed* to the console, kernel-side (`sh.c`'s own `ls`); this
  fixes that path's own doubled-slash bug in the process. `sh.c`'s
  existing `fs_list_dir("/")` builds each entry as `path + "/" +
  name`, which for `path = "/"` produces `"//WM"` -- correct formula,
  just never called with anything except the one path where it
  produces a double slash. `k_fs_list()` normalizes the join instead:
  exactly one leading/trailing `/` regardless of how `path` is
  phrased (`"/"`, `""`, `"/SUBDIR"`, `"/SUBDIR/"` all produce the same
  clean joins) -- resolving the design question of *what a listed path
  should even look like*: always a full path from root, e.g. `"/WM"`.
  FatFs treats a leading `/` and a bare name as the same file (no
  separate "current directory" concept beyond the root here), so
  these paths are directly usable as-is with `read-file`/`file-size`/
  `write-file`/`delete-file` above without stripping anything.

App-facing wrappers: `fs_unlink()`/`fs_list()` (`sw/common/zfsapp.c`,
alongside the three from `docs/editor.md`). `fs_list()` returns a
malloc'd `char **`, bounded by a 4KB internal staging buffer
(`FS_LIST_BUF_SIZE`) -- comfortably dozens of entries for an ordinary
directory of short 8.3 names, truncated (not grown) for anything
bigger; `zapi_ls()` hands the result straight to `ms_mk_str_list()`,
which takes ownership of each string.

**Caveat found and handled while implementing `read-file`:**
`fs_mallocfile()` returns exactly the file's raw bytes, NOT
NUL-terminated (`te.c`'s own loader tracks the size separately rather
than relying on a terminator -- it has to, since a file is not
guaranteed to end in a NUL). `ms`'s `T_STR` values ARE plain
NUL-terminated C strings throughout `ms.c`, so `zapi_read_file()`
allocates a fresh `size + 1` buffer and terminates it itself before
constructing the Scheme string. A related, unfixed limitation: a file
containing an embedded NUL byte reads back truncated at that byte (no
embedded-NUL support in this string representation) -- fine for
ordinary text, not a safe way to read arbitrary binary data as a
Scheme string.

### Windows -- IMPLEMENTED

| Procedure | Behavior |
|---|---|
| `(win-create)` / `(win-create "title")` / `(win-create "title" w h)` / `(win-create "title" w h x y)` | creates a window, returns its numeric id (or `#f`) |
| `(win-destroy id)` | destroys it; `#t`/`#f` |
| `(win-clear id)` | clears content area; `#t`/`#f` |
| `(line id x0 y0 x1 y1 color)` | hardware-accelerated line (`z_win_hw_line()`); `#t`/`#f`, or raises an error for an id this process didn't create |
| `(box id x0 y0 x1 y1 color)` | hardware-accelerated filled box (`z_win_hw_box()`); same |
| `(text id x y "s" color)` | draws text (`z_win_draw_text()`, `z_font_6x12`); same |

`z_win_create()`/`z_win_clear()`/`z_win_hw_line()`/`z_win_hw_box()`/
`z_win_draw_text()` (`sw/common/zwin.h`) already existed and already
did the hardware-line-rasterizer + clipping work -- these wrappers
really are thin. `z_win_destroy()` was the one missing client helper
(only the raw `Z_WM_DESTROY_WINDOW` message subject existed) -- added
to `sw/common/zwin.c`/`.h`, fire-and-forget, no reply (matches `wm.c`'s
own handler, which doesn't send one).

**Window ownership:** confirmed exactly as expected -- a window
created by evaluating Scheme is owned by `repl`'s own pid, since
`z_win_create()` runs inside `repl`'s process regardless of which
`term` connection's line triggered it. A small fixed-size table inside
`zapi.c` (`ZAPI_WIN_MAX`, currently 8) maps a window's wm-assigned id
to its `z_win_t` (needed for every draw call's clip/offset
computation) -- no cleanup on `Z_PORT_CLOSE`, a window's lifetime is
tied to `repl` itself.

**Titlebar close icon.** Every window `(win-create ...)` makes now
shows a titlebar close icon (`Z_WIN_FLAG_CLOSE_ICON`, see
`docs/window_manager.md`'s "Window titlebar icons") -- but clicking it
does NOT kill `repl`, and does not need `Z_WIN_FLAG_CLOSE_KILLS_OWNER`
(also NOT set here): since `repl` can have several of these windows
open under the one `zapi_windows[]` table at once, `wm` instead sends
`Z_WM_CLOSE` for just the clicked window's id, handled in `repl.c`'s
main loop by `zapi_win_close(id)` (`zapi.c`/`.h`) -- it destroys that
one table entry, the same bookkeeping `(win-destroy id)` above already
does, just triggered by the icon instead of an explicit Scheme call.
The window's `z_win_t` and its slot in the table both go away exactly
as if `(win-destroy id)` had been called; there's no separate
Scheme-visible event for this yet (nothing calls back into `ms_eval()`
when it happens) -- if Scheme code needs to react to a window closing
rather than just have the bookkeeping cleaned up, that's a real gap to
revisit, not attempted here.

**Coordinate system: window-relative, `(0,0)` = content top-left.**
`line`/`box`/`text` all take coordinates relative to the window's own
CONTENT area -- past the 1px frame border and the 1px breathing-room
margin `z_win_content_rect()` (`zwin.c`) insets by on every edge, same
place `z_win_clear()` already clears down to. `(line 2 0 0 20 20 1)`
draws from the window's own top-left corner; `(box 2 50 50 60 60 1)`
draws 50px right and down from there.

This required NOT going through `z_win_hw_line()`/`z_win_hw_box()`/
`z_win_draw_text()` (`sw/common/zwin.c`) as originally planned --
built and tested against them first, and found a real inconsistency
between the three:

- `z_win_hw_line()`/`z_win_hw_box()` take **screen** coordinates
  outright -- clipped to the window's content rect, but never offset
  by `win->x`/`win->y` at all. A small `x0`/`y0` draws near the
  screen's own origin, nowhere near the window, unless the window
  happens to already be positioned near `(0,0)`.
- `z_win_draw_text()` DOES offset, but inconsistently across its own
  two axes: `win->x + x` on X (the window's outer, border-inclusive
  edge) against `clip.y0 + y` on Y (the true content-area edge, past
  the margin). Text drawn at `x=0` has its leftmost ~2px silently
  clipped, since the real content edge (`clip.x0`) is `win->x + 2`,
  two pixels past where `x=0` actually lands.

Rather than fix `zwin.c` itself (touching it risks changing behavior
for its other existing callers, e.g. `sw/apps/hello_win`, which may
already depend on the current behavior one way or another), each
`zapi_*` drawing procedure computes the window's content rect itself
(`z_win_content_rect()`, already public) and calls the lower-level
`z_fb_hw_line()`/`z_fb_hw_box()`/`z_fb_draw_text()` (`sw/common/
zgfx.h`) directly, applying `(clip.x0 + x, clip.y0 + y)` consistently
on every call. This gives the Scheme API exactly the coordinate
convention above without touching `zwin.c` or risking any other
caller's behavior at all. `zwin.c`'s own inconsistency is unfixed and
still there for any other direct caller -- worth fixing at that level
too at some point, just not bundled into this change.

**Known limitation, not fixed in this revision:** `repl`'s own main
message loop doesn't read or respond to `Z_WM_REDRAW` at all -- a
window created here doesn't participate in the normal occlusion/
z-order redraw protocol (`docs/window_manager.md`). Content drawn via
the hardware-accelerated calls writes straight into the real
framebuffer and persists indefinitely as long as nothing else
overdraws that screen region, so a window that's never occluded works
fine forever with no redraw needed -- but if another window is moved
on top of it and away again, `wm.c`'s own `wait_for_redraw_done()`
will time out waiting for an ack this window never sends (bounded, not
an indefinite stall -- cosmetic, not a correctness/safety issue), and
the previously-covered area won't automatically repaint. Fixing this
would mean `repl`'s message loop recognizing `Z_WM_REDRAW` and
re-issuing whatever a window last drew, which means tracking draw
history per window -- not attempted here; revisit if real usage shows
it matters.

Build note: `repl` didn't link against the window/graphics stack
before this (`zgfx.o`/`zfont_data.o`/`zwin.o`) -- added to its
Makefile, same three objects/rules `sw/apps/hello_win`'s own Makefile
already uses for the same three source files.

**Window placement at creation.** New windows were landing almost
directly on top of `term`'s own window -- `wm.c`'s cascade formula
(`x = 20 + (n % 8) * 24`, `y = 20 + (n % 8) * 20`) only offsets each
new window by a small amount from the last, and `term`'s window is
large enough that the offset barely clears it. `create_window()`
(`wm.c`) already had a `fixed_x`/`fixed_y` parameter internally (used
by the dock, which needs one fixed spot rather than cascading) -- it
just wasn't reachable from the app-facing `Z_WM_CREATE_WINDOW`
message, which always passed `-1, -1`. Wired through: the message
handler now reads optional `x`/`y` from the request (falling back to
the normal cascade if either is missing, so `sw/apps/hello_win` and
every other existing caller is unaffected), and a new
`z_win_create_ex()` client helper (`zwin.c`/`.h`, alongside the
existing `z_win_create()`, not a change to it) sends them when given.
`(win-create "title" w h x y)` uses it.

This required no dependency on the redraw/message-handling discussion
above: window creation is already exempt from the wm's redraw-ack
wait (the `exclude_idx` mechanism described just above exists
specifically because a brand-new window's owner isn't listening for
`Z_WM_REDRAW` yet), so placing it at an exact position up front is
just as safe as letting it cascade -- unlike *moving* an existing
window later would be. Repositioning an already-created window (a
real `win-move`) is a different, not-yet-built feature for exactly
that reason: it would need at least the no-op-ack approach from this
section's "known limitation" above as a prerequisite, since moving
(unlike creating) does trigger the wait.

### Networking -- IMPLEMENTED (`tget`/`tput`); `udp-send`/`ping` still a sketch

| Procedure | Behavior |
|---|---|
| `(tget ip-or-host remote-file)` / `(tget ip-or-host remote-file local-file)` | fetches a file via TFTP (needs `run net`); returns bytes written, or raises an error |
| `(tput ip-or-host local-file)` / `(tput ip-or-host local-file remote-file)` | sends a file via TFTP; `#t`, or raises an error |

`tget`/`tput` are a direct port of `sh.c`'s own existing commands
(`sw/os/sh.c`, already using `zstream_open()`/`zstream_pull()`/
`zstream_producer_t` against `net`'s TFTP implementation,
`docs/networking.md`'s "TFTP: exposed to other processes via
messaging and streaming") -- the logic already existed and worked;
this just makes it reachable from Scheme. Unlike `line`/`box`/`text`
returning `#f` on failure, any `tget`/`tput` failure (DNS/host
resolution, opening either file, a mid-transfer error, no reply from
`net`) raises a real Scheme error describing what went wrong -- same
reasoning as the window-not-found fix earlier in this document: a
network operation can fail for many different reasons, and a bare
`#f` would lose exactly the information that matters most for
figuring out which one happened.

**Chunked file I/O -- built.** `sh.c`'s `tget` streams pulled chunks
straight to disk via `fs_open_write()`/`fs_write_chunk()`/
`fs_close_write()`, kernel-only before this (same gap the whole-file
`FS_SIZE`/`_READ`/`_WRITE` syscalls closed for `te`, `docs/editor.md`)
-- a transfer that isn't small shouldn't have to sit fully in memory
first. Five new syscalls (`FS_OPEN_READ`/`_OPEN_WRITE`/`_READ_CHUNK`/
`_WRITE_CHUNK`/`_CLOSE`, `sw/os/fsapi.c`) close it, following the
existing "no kernel `malloc()`, caller-owned buffers" conventions --
plus one new thing the earlier whole-file syscalls didn't need: a
file open across several syscalls needs somewhere to keep the live
FatFs `FIL` between them. Kept kernel-side, in a small (`Z_FS_MAX_OPEN
= 4`) bounded table, not in the caller's own memory -- `FIL` is a
FatFs-internal struct no app translation unit has ever included the
layout of, so handing the caller an opaque "allocate exactly
`sizeof(FIL)` bytes" contract would be fragile (a size mismatch
between kernel and app builds fails silently, not at compile time).
The caller gets back a small integer handle instead -- same shape a
Unix file descriptor is, scoped to exactly this use case. Every
operation on a handle checks it against the kernel's own `z_pid`
(reliably the calling process's pid for a syscall's whole duration,
`k_getpid()`'s own comment explains why) before touching it, so one
process can't read or close a handle another process opened.

**Known limitation, not fixed here:** a handle isn't released if its
owning process exits (crashes, or is killed) without closing it --
no process-exit hook sweeps abandoned handles. With the table kept
small and this meant for one `tget`/`tput` at a time per caller
rather than a general-purpose fd table, the practical exposure is
narrow (a handful of leaked slots at worst, recoverable by a reboot)
-- worth a real fix if it proves to matter in practice.

**The blocking/mailbox-drop tradeoff -- accepted, not mitigated.**
Both `tget` and `tput` block this whole process for the duration of
the transfer, same accepted tradeoff class as `te` (`docs/editor.md`)
-- matches `zstream.h`'s own reasoning almost exactly ("the consumer
side IS allowed to block... a consumer with nothing else to do while
it waits (e.g. the shell)" -- Scheme code running inside `repl` IS
that shell). `tput` specifically is sharper: its producer loop reads
`repl`'s mailbox directly while sending, discarding any unrelated
message that arrives mid-transfer -- meaning every OTHER connected
`term` window's traffic to `repl` is silently dropped, not just
delayed, for the duration of a `tput`. Not mitigated in this revision
(same "validate simple against real use first" preference
`docs/networking.md`'s own TFTP staged-bringup notes state) -- worth
revisiting if it proves to matter in practice with real concurrent
usage.

**No app-facing timeout primitive -- worked around, not fixed.**
`sh.c`'s own `tput` uses `z_msg_wait_timeout()` (`sw/os/msg.h`) for
its final reply wait -- that function references the kernel's raw
`z_kernel_ticks` global directly and is compiled only into
`kernel.elf`, not something an app can link against.
`zapi_msg_wait_timeout()` (`zapi.c`, shared with `msg-wait`'s own
optional-timeout case, \S4 "Messaging" above) reimplements the same
logic portably -- a polling loop against `z_uptime_ticks()`, which IS
app-facing -- so no kernel/`ms.c` change was needed for this
specifically. Same busy-wait cost `msg-wait` already has, for the same
reason.

`(udp-send ip port "data")` and `(ping ip)` (ICMP echo -- `net`
already speaks it internally, `docs/networking.md`'s feature list)
have no app-facing message subject at all yet in `sw/common/znet.h` --
genuinely new `net.c` work, not a port of something that already
exists.
exists.

### Graphics

Folded into Windows above -- there's no freestanding "draw graphics"
concept separate from "draw into a window" in the existing C API
(`zwin.h`'s own comment: this is "the sanctioned way for an app to
draw," always clipped to a window).

### Messaging -- IMPLEMENTED

| Procedure | Behavior |
|---|---|
| `(getpid)` | this process's own pid |
| `(pid-lookup "name")` | resolves a registered name (`"wm0"`, `"net0"`, ...) to a pid, or `#f` |
| `(msg-send pid subject tag "data")` | fire-and-forget `Z_STR` message; `#t`/`#f` |
| `(msg-wait subject tag)` / `(msg-wait subject tag timeout-ms)` | blocks for a matching reply, returns its payload as a string (or `#f` on a non-`Z_STR` reply, or on timeout) |

Thin wrappers over `z_getpid()`/`z_pid_lookup()`/`z_msg_new_send()`/
`z_msg_wait()` -- all already existed, all already app-callable.
`msg-send`/`msg-wait` deliberately handle `Z_STR` payloads only for
v1 (matches `Z_REPL_EVAL`'s own `Z_STR`-first convention, `zrepl.h`);
richer payloads (`Z_MAP`, `Z_BLOB`) are a natural follow-up once
something actually needs one.

**Correction from the original design sketch:** that version assumed
a `z_msg_wait_timeout()` already existed to build `msg-wait`'s
optional timeout on. It doesn't -- checked `zeitlos.h` directly, there
is no timeout variant, and no sleep/yield primitive in this OS at all.
`(msg-wait subject tag)` with no timeout calls `z_msg_wait()` directly
(indefinite block, same accepted tradeoff class as `te`/`tget`/`tput`
already are for this process). `(msg-wait subject tag timeout-ms)`
instead polls `z_msg_read()` (non-blocking) in a loop, checking
elapsed `z_uptime_ticks()` (~732Hz) against the deadline, discarding
any non-matching message along the way -- exactly what `z_msg_wait()`
itself already documents doing, just with a deadline added. This is a
genuine busy-wait: nothing to yield the CPU to between polls with, so
it burns real cycles on real hardware for however long nothing
matching arrives, though it doesn't change how long `repl` is
unresponsive to its other connections either way (already blocked for
the same duration regardless of whether it's spinning or sleeping).
Worth a real OS-level timeout primitive if this proves costly in
practice -- not built here.

## 5. Resolved design questions

1. **Window ownership** -- resolved above (\S4, Windows): `repl` owns
   windows it creates, not whichever `term` connection asked; no
   cleanup needed on connection close.
2. **`ls` scope** -- resolved: `ls` takes an optional path argument
   from the start (`(ls "/path")`), root if omitted. Every returned
   name is a full path (`"/WM"`), fixing the pre-existing `sh.c`
   double-slash display bug in the process (\S4, Files).
3. **Numeric precision** -- resolved: staying with `double` (`T_NUM`).
   Every id/pid/size this API deals with is well under 2^53 (exact in
   a double), no concern in practice.
