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
("/APPS" "/DOCS" "/ARK" "/USER")
> ls /APPS
("/APPS/FILES" "/APPS/TEXT" "/APPS/READ")
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

### What stays a builtin, and why

`F12` is worth knowing about and is mentioned in both repl's connect
banner and `help`: `term` intercepts it locally, before anything
reaches a port (`handle_key_event()` in `term.c`), so it works even
when term is relaying raw bytes to a remote that has no quit command
of its own -- which is exactly the situation you need it in, and
exactly the situation where nothing can tell you about it.

Five commands are **not** Scheme procedures and can't become ones:
`help`, `te`, `page`, `port`, `telnet`, plus `quit`/`exit`. Each needs
the *requesting connection itself* -- `te` and `page` take it over for
a full-screen VT100 session, `port` and `telnet` redirect it
elsewhere, `quit` ends it -- and a Scheme procedure has no access to
that. `dispatch_line()` passes the connection through as `conn`, which
is `NULL` for a `Z_REPL_EVAL` request; all five refuse cleanly in that
case rather than half-working.

Everything else moved to the Scheme API, so each command now lives in
exactly one place. Bare-word syntax (\S1b) means this is invisible in
use: typing `uptime` or `free` still works, it just means `(uptime)`
and `(free)` now and returns a value you can compute with.

**Reply buffers, and why truncation is now always visible.** This bit
three times, so it is worth stating plainly:

- `help` ran to 357 bytes into a 256-byte buffer and silently lost its
  last third.
- `(free)` prints to ~293 bytes and `(ps)` on a full 16-entry process
  table to ~770. Both used to stop mid-token -- `... ("mem-free-blocks"
  1) ("mem-blo` and nothing more -- which reads as corrupt output
  rather than as cut-off output.

There are now **two** constants, because the two paths have genuinely
different constraints:

| constant | value | scope |
|---|---|---|
| `Z_REPL_EVAL_REPLY_MAX` | 256 | wire cap for `REPL_EVAL` -- every peer sizes its buffer from it, so raising it means rebuilding both sides |
| `Z_REPL_REPLY_MAX` | 1024 | repl-internal stack buffer for interactive (port/`term`) replies -- no peer, no compatibility question |

`help` is held in a named constant with a negative-array-size typedef
beside it, so an over-long help string is a compile **error** rather
than something the next person has to notice. It is deliberately sized
against the *smaller* cap so it reads identically on both paths.

And because a printed Scheme value has no bounded size in general --
`(ls)` on a large directory, or any user expression -- `eval_scheme()`
routes its result through `copy_or_mark_truncated()`, which appends
` ...[truncated]` instead of just stopping. Silent truncation is the
failure mode all three of these shared; the fix is not just a bigger
buffer but saying so when it still doesn't fit.

### Scheme output goes to `term`, not the serial console

`ms`'s printing procedures -- `display`, `write`, `print`, `newline`,
`gc` and `dump` -- all write to stdout, which on this OS means the
UART. For someone running them from a `term` window that is the wrong
screen entirely: `(dump)` printed ~210 symbol names onto the serial
console and showed the user nothing at all.

Fixed with a redirect hook (`z_stdout_hook`, `sw/common/zeitlos.h`)
rather than by patching the call sites: `ms.c` has 50+ scattered
`printf()`/`fputs()`/`putchar()` calls and no output abstraction of
its own, so redirecting them individually would mean a large diff
against a submodule this project deliberately keeps close to upstream.
Catching it where they all already funnel through -- `_write()` -- costs
one branch and covers procedures nobody has written yet.

While an interactive command is dispatching, repl installs the hook,
**accumulates** output, and sends it once dispatch finishes. Details
that matter:

- **Buffered, not per-write.** `dump` emits one `fputs()` per symbol.
  Sending each would be one message apiece, and `z_port_send()` refuses
  once `Z_PORT_MAX_PENDING_SENDS` (8) sends go unacked -- with no ack
  possible mid-command, since repl only processes `Z_PORT_DATA_ACK`
  back in its main loop. The real budget is ~8 sends per command,
  shared with the reply and prompt. The buffer is 2KB so `dump`'s
  ~1.5KB goes out in one.
- **`fflush(stdout)` before taking the hook down.** stdout is
  line-buffered (`_isatty()` reports a tty), so anything printed
  without a trailing newline is still inside libc when dispatch
  returns.
- **stderr stays on the console.** `ms.c`'s `[panic]` diagnostics
  belong there, and redirecting them would let a panic raised while
  rendering output recurse into what was already failing.
- **Bare `\n` becomes `\r\n`, existing `\r\n` is left alone.** `ms.c`
  mixes both conventions -- `bi_dump`'s wrapping emits `\r\n`, while
  `newline` emits `\n` -- so blind expansion would produce `\r\r\n`.
- **`te`/`page` sessions are exempt.** Those draw their own full
  screen; captured diagnostics go to the console instead of being
  painted over it.
- **A failed send falls back to the console** rather than dropping the
  output.

`REPL_EVAL` requests are unaffected -- no connection, so stdout keeps
going to the console exactly as before.

#### `(print-console x)` -- the deliberate opposite

| Procedure | Behavior |
|---|---|
| `(display x)` / `(print x)` | print to the `term` window that ran the command (ms builtins) |
| `(print-console x)` | prints to the serial console, always |

Once `display` goes to `term`, there's no way to *deliberately* reach
the console -- which is exactly what you want when the console is a
second window onto a running system: tracing what a procedure does
without that trace scrolling through the output you're trying to read,
watching a long loop while the `term` window shows only its result, or
getting anything at all out of code running with no connection
attached.

Semantics match **`print`**, not `display` -- the name sets the
expectation and the behavior follows it: a trailing newline is added,
non-string values print in readable form (strings inside a list come
out quoted), a string argument prints raw, and no argument at all
emits just a newline. Returns `#f`, same as `print`. The automatic
newline is the right default for what this exists for: a trace line
that needs an explicit `"\n"` every time is a trace line that
eventually won't have one.

Implemented by writing to **stderr**, not by temporarily removing the
hook. Both would reach the UART, but the hook dance has a real hazard:
stdout is line-buffered, so bytes from an earlier `display` may still
be inside libc, and flushing them with the hook removed would misroute
that earlier output to the console. stderr is unbuffered and `_write()`
only ever redirects fd 1, so this needs no coordination with the
capture buffer and can't reorder or steal anything in flight. (This
path is already proven in this build -- `ms_log()` writes its
`[info]`/`[error]`/`[panic]` lines to stderr.)

Consequence worth knowing: this shares the console with those `[panic]`
diagnostics. That's the intent -- one debugging stream, in the order
things actually happened.

### `page` -- viewing files of any size

`te` (`docs/editor.md`) loads the whole document into repl's heap and
pays several times the file's raw size for its line-list
representation, which is why `te_bridge.c` enforces a conservative
`TE_MAX_FILE_SIZE` (2KB default). That ceiling is right for an editor
and useless for reading a book.

`page` (`sw/apps/repl/page.c`) never holds more than a screenful. It
keeps the file open across the session and re-reads what it needs on
each redraw, so peak memory is a fixed few KB whether the file is 2KB
or 2MB. Keys: space/`f` and `b` for pages, arrows or `j`/`k` for
lines, PgUp/PgDn, `g`/`G` for the ends, `q` to quit.

**Reading backwards is the whole design problem** -- a byte offset is
not a line number, and nothing in a text file lets you find the start
of the previous line without having already seen it. An offset for
every line would work and is exactly what this can't afford: a 2MB
book is ~40k lines, and 40k × 4 bytes is 160KB of index for a process
whose entire stack+heap allowance is 64KB. So `page` keeps a **sparse
index** -- one offset every `PAGE_IDX_STRIDE` (256) lines,
`PAGE_IDX_MAX` (256) entries, 1KB of `.bss`, covering 65,536 lines --
filled in lazily as a side effect of ordinary scrolling, with no
separate indexing pass. Any jump seeks to the nearest anchor at or
before the target and scans forward, at most 256 lines. Past that
coverage the index stops growing and long backward jumps get slower;
that's a deliberate choice over an unbounded index, and the failure
mode is a moment of latency, not running out of memory.

That seek is why **`FS_SEEK`** exists. Every pre-existing chunked-I/O
call only moves forward; without it, paging back one screen in a book
would mean closing the file, reopening it, and re-reading from byte 0
-- on an SD card, on every keypress.

The status line clamps the **filename**, not the key hints, when the
two don't fit the row together -- someone paging a file already knows
what they opened, while the hints are this viewer's only
discoverability.

`page` deliberately **truncates** long lines rather than wrapping
them. Wrapping would make one file line occupy several screen rows, so
"page down 24 lines" would no longer mean a fixed number of file
lines, and scrolling arithmetic that depends on content width is much
harder to keep correct. Tabs render as a space and other control bytes
as `.`, so a binary file opened by mistake scrolls harmlessly instead
of filling the terminal with escape sequences it will try to
interpret.

Session ownership follows `te_bridge.h`'s rule exactly: the state is
file-static, so one connection pages at a time and a second `page`
command is refused rather than quietly stealing the first reader's
position. repl stays fully responsive to its other connections
throughout -- paging is driven one keystroke at a time through the
normal message loop and never blocks it. On `Z_PORT_CLOSE`,
`page_abort()` closes the open handle, which matters: handles live in
a bounded kernel-side table (`Z_FS_MAX_OPEN` is 8) with no
process-exit sweep.

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
| `(ls)` / `(ls "/path")` | list of filenames under `path` (root if omitted), each a full `/`-prefixed path (e.g. `"/APPS"`) |
| `(file-size "name")` | size in bytes, or `#f` if missing (or genuinely empty -- see caveat below) |
| `(read-file "name")` | whole file contents as a string, or `#f` |
| `(write-file "name" "contents")` | create/truncate + write; `#t`/`#f` |
| `(delete-file "name")` | `#t`/`#f` |
| `(mkdir "path")` | create a directory; `#t`/`#f` |
| `(touch-file "path")` | create an empty file; `#t`/`#f` |
| `(load "file.l")` | read a file and evaluate **every** form in it; `#t`, or `#f` if unreadable |
| `(file->str "name")` | alias for `read-file` (upstream `ms`'s own name for it) |
| `(df)` | filesystem capacity: `(("total-kb" N) ("used-kb" N) ("free-kb" N))` |

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
  name`, which for `path = "/"` produces `"//APPS"` -- correct formula,
  just never called with anything except the one path where it
  produces a double slash. `k_fs_list()` normalizes the join instead:
  exactly one leading/trailing `/` regardless of how `path` is
  phrased (`"/"`, `""`, `"/SUBDIR"`, `"/SUBDIR/"` all produce the same
  clean joins) -- resolving the design question of *what a listed path
  should even look like*: always a full path from root, e.g. `"/APPS"`.
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


#### `mkdir` / `touch-file`, and why the odd name

Two more thin syscalls (`FS_MKDIR`, `FS_TOUCH`) over the existing
`fs_mkdir()`/`fs_touch()` in `sw/os/fs/fs.c`, which already did the
real work but were reachable only from `sh.c` (kernel code, which
links `fs/fs.c` directly). Exactly the same relationship `FS_UNLINK`
has to `fs_unlink()`.

One trap worth restating, because it has already caused a bug in this
codebase once: `fs_mkdir()`/`fs_touch()`/`fs_unlink()` all return **0
on success**, inverted from `fs_size()`/`fs_write_file()`'s "0 means
failure". The app-side wrappers in `sw/common/zfsapp.c` normalize this
to that file's own 1-on-success convention, so no caller above them
has to think about it.

It is `touch-file`, not `touch`, for a reason that only exists because
of bare-word command syntax (\S1b): **any bound callable becomes a
typeable command**, so every short generic name registered here is a
name permanently taken away from the user's own global environment.
`-file` also matches the existing `read-file`/`write-file`/
`delete-file` family, which is where a reader will expect to find it.

#### `load` needs no `ms.c` patch

`ms.c` does have its own `load` and `file->str` builtins -- but they
live inside `#ifndef LIX` and go through `fopen()`/`fread()`. This
build is `-DLIX` precisely because there is no stdio file layer here,
which is why `sw/common/ms_platform/fs.h` is a deliberately empty
stub. Enabling them would mean routing Zeitlos's `fs_*` calls into
`ms.c` itself -- Zeitlos-specific I/O inside the submodule, which
makes it *harder* to upstream, not easier.

Instead `zapi_load()` uses `ms_load_string()`, which is already public
and already declared in `ms_api.h`. **No change to `sw/ext/ms` was
needed for any part of this revision.**

`load` is also genuinely more than the `(eval (read (read-file ...)))`
it replaces: that evaluates the *first* form and silently ignores the
rest of the file, which is essentially never what someone loading a
script wants. `ms_load_string()` loops over every form.

An error inside a loaded file raises a normal Scheme panic (caught by
`repl.c`'s existing recovery) with whatever forms already ran having
already taken effect -- no transactional all-or-nothing behavior, same
as any other Scheme's `load`.

#### `(df)` -- filesystem capacity

All figures in **kilobytes**, not bytes: these are 32-bit all the way
down and a 32GB card's byte count overflows a `uint32_t`. A caller
wanting bytes can multiply; a caller handed a pre-overflowed number
could not have recovered it.

`(df)` is the **SD card**; `(free)` is **main memory**. They're
unrelated, and the names deliberately match the `df`/`free` shell
commands (`sw/os/sh.c`) that report the same two things.

An absent or unmounted card reports all zeros rather than raising --
"how much space is there" has a truthful answer of "none" in that
state, and a script polling for a card shouldn't have to catch an error
to discover it isn't there yet.

`fs_total()`/`fs_free()` (`sw/os/fs/fs.c`) wrap FatFs's `f_getfree()`
and had existed all along -- there was simply no syscall, no shell
command, and nothing in Scheme that ever called them. The new `FS_DF`
syscall exposes them, and `df` was added to `sh.c` at the same time.

### Processes -- IMPLEMENTED

| Procedure | Behavior |
|---|---|
| `(ps)` | process table snapshot: a list of `(pid base size pc sp flags)` rows |
| `(run "name")` | start a process from a file (bare name: `"net"`, not `"/NET.BIN"`); new pid, or `#f` |
| `(kill pid)` | `#t`/`#f` |
| `(uptime)` | ticks since boot, as a number |
| `(delay-ms n)` | busy-wait at least `n` ms, then `#t` |

`(ps)` returns **data**, not the pre-formatted text `sh.c`'s own `ps`
prints. `k_proc_dump()` stays exactly as it is; the new `PROC_LIST`
syscall returns the same information to a caller -- the same
relationship `k_fs_list()` already has to `fs_list_dir()`. Values are
decimal rather than the hex the console dump uses: hex is right for
reading addresses by eye, numbers are right for a caller doing
arithmetic, and a caller who wants hex can format it.

The `pid` is the process **table index**, which is what `(kill ...)`,
`k_proc_base()` and `k_proc_kill()` all take -- so a pid from `(ps)`
goes straight back into `(kill ...)` with no translation:

```scheme
; kill every process except the kernel (pid 0) and ourselves
(for-each (lambda (row)
            (if (and (> (car row) 0) (not (= (car row) (getpid))))
                (kill (car row))))
          (ps))
```

`run` and `kill` needed **no new syscalls** -- `PROC_RUN` and
`PROC_KILL` already existed for wm's dock and close-icon handling.
`z_proc_kill()` was widened from `void` to `z_rv` so `(kill ...)` can
return a real `#t`/`#f` rather than an unconditional "probably";
existing callers that ignore the value compile unchanged.

`run` returns `#f` rather than raising, deliberately: unlike the
networking procedures, where losing the specific reason costs real
diagnostic information, "it didn't start" is a single plainly-visible
outcome a script may well want to branch on -- the same distinction
`file-size`/`read-file` already draw against the window procedures.
If it returns `#f` unexpectedly, `(free)` below is the thing to check
next.

`uptime` replaces a builtin that printed a fixed string and gave a
caller nothing to compute with. Ticks rather than seconds because
ticks are what the hardware counts (the KTIMER IRQ, ~732Hz): dividing
to seconds is one obvious expression, while recovering ticks from a
pre-rounded seconds value isn't possible at all. The counter is 32-bit
and wraps after roughly 68 days.

**`delay-ms` blocks this entire process.** On `repl` that means every
other connected `term` window stops being serviced for the duration,
not just the one that ran it -- repl's single main loop drains one
shared mailbox. There is no sleep/yield primitive in this OS, so this
genuinely burns cycles rather than giving them up. Fine for pacing a
short animation or a retry loop; actively antisocial for anything
long.

### Time -- IMPLEMENTED

| Procedure | Behavior |
|---|---|
| `(current-time)` | seconds since the Unix epoch, UTC, as a number; `#f` if the clock is unavailable or unset |
| `(current-date)` | `(year month day hour minute second weekday yearday)`; `#f` if unavailable or unset |
| `(current-date t)` | the same breakdown for an arbitrary timestamp `t` |

The wall clock (`rtl/rtc.v`, `docs/rtc.md`), as opposed to `(uptime)`
above. Both exist and they answer different questions: `uptime` is
monotonic and is what you time things with, this has an epoch and can
jump backwards the moment net's NTP client lands a correction. Using
the wrong one is how you get a negative duration.

```scheme
(current-date)          ; => (2026 8 27 14 31 2 4 238)
(current-date 0)        ; => (1970 1 1 0 0 0 4 0)
```

`month` is 1-12 and `day` is 1-31 -- not the 0-based months a C
`struct tm` uses. This is a value people read, and a 1-based month is
what they expect. `weekday` is 0 for Sunday; `yearday` is 0-365.

**Everything here is UTC.** There is no timezone conversion anywhere in
Zeitlos yet -- see `docs/rtc.md` on why that is a decision rather than
an oversight.

`#f` rather than a panic when the clock is unset, which is the opposite
call from the one `(video-mode ...)` makes for missing gateware. Setting
the display is something the caller asked to **do**, and failing it
quietly would hide a reflash they need to know about. Asking what time
it is is a **question**, and "I don't know" is a real answer to it -- on
a machine with no network that is the permanent, correct, entirely
unexceptional answer, and blowing up a one-liner over it would be
obnoxious. It also composes:

```scheme
(if (current-time)
    (display (current-date))
    (display "clock not set"))
```

Both halves of that check matter and neither implies the other: a board
can have an RTC nobody has told the time to (the normal state for the
first few seconds after boot), and a board can have no RTC at all.

The optional argument to `current-date` makes it a general calendar
function rather than only a clock reading -- formatting a file's
timestamp, working out what day some computed second falls on. It needs
no RTC, so `(current-date 0)` answers on a board with no clock. Same
optional-argument shape as `(ls)`/`(ls path)` and
`(video-mode)`/`(video-mode m)`.

A **positional** list, unlike `(free)` and `(df)` below, which return
association lists. Those hand back a dozen unrelated figures where a
name is the only thing telling them apart; a date is eight fields in an
order every calendar has used for a very long time, and
`(cadr (assoc "hour" (current-date)))` would be a worse way to ask for
the hour than `(list-ref (current-date) 3)`.

Numbers rather than a preformatted string, matching `(ps)` and
`(uptime)`: a string is one `str-append` away from a list of numbers,
and a list of numbers cannot be recovered from a string without parsing
it back.

Seconds rather than the RTC's own 1/1024s units, which is the opposite
of `(uptime)`'s ticks -- the reasoning there was that dividing to
seconds is easy while recovering ticks isn't, and here the raw unit
already **is** seconds. The fraction is deliberately not exposed:
nothing in Scheme runs anywhere near that fast, and a two-element
return would complicate every caller for the benefit of none.

### Memory -- IMPLEMENTED

`(free)` returns an association list, all figures in **bytes** except
the two cell counts:

```scheme
(cadr (assoc "mem-free" (free)))   ; => bytes free in the kernel pool
```

| Key | Meaning |
|---|---|
| `scheme-cells-used` / `scheme-cells-total` | `ms` cell heap, in cells |
| `scheme-bytes` | the same, in bytes |
| `c-heap` | bytes `malloc()`'d by this process since boot |
| `static` | this process's code+data+bss, fixed at build |
| `mem-total` / `mem-used` / `mem-free` | kernel pool |
| `mem-largest-free` | biggest single free block (fragmentation) |
| `mem-used-blocks` / `mem-free-blocks` | block counts |
| `mem-blocks-used` / `mem-blocks-max` | block descriptors consumed |

**Two different things are reported here, and conflating them is the
easy mistake this layout exists to prevent.** The `scheme-`/`c-heap`/
`static` figures describe *this process's own footprint inside the
block it was already given* -- what the old builtin `free` showed. The
`mem-` figures describe the *kernel pool* (`k_mem_alloc()`,
`sw/os/mem.c`): the memory whole processes get carved out of, and what
`sh.c`'s own `free` shows. This is what determines whether the next
`(run ...)` succeeds. A process can be comfortable while the pool is
nearly exhausted, and vice versa; neither number predicts the other.

The new `MEM_STATS` syscall shares one walk of the block list with
`k_mem_dump()` (see `mem_collect()` in `mem.c`) rather than
duplicating it -- a `free` that disagrees with itself depending on
whether you typed it at the kernel shell or from Scheme is exactly the
kind of thing nobody notices until it matters.

### Returning nested values: `ms_read()`, not a builder

`ps` and `free` both return lists of lists, and `ms.c`'s embedder API
has no exposed `cons` -- it offers the scalar constructors plus
exactly one composite, `ms_mk_str_list()`. That is deliberate:
building anything larger from outside `ms.c` means getting GC
protection right by hand across several allocations, which is
precisely what it doesn't want every embedder re-deriving.

Rather than patch `ms.c` to expose `PUSH`/`POP` (or add a builder API)
purely so two procedures can return tables, both build their result as
Scheme **source text** and hand it to `ms_read()` -- already public,
already used by `repl.c`'s own eval path, and it does all its own GC
protection internally. Safe by construction rather than by careful
hand-auditing.

The honest tradeoffs: it costs a `snprintf()` pass and a parse over a
small buffer, and it is only safe for data we format ourselves. Every
interpolated value in both procedures is a number or a fixed label --
no user-supplied string is ever interpolated, so nothing can inject
syntax. For at most 16 rows of six numbers this is far cheaper in
**code size** than the alternative, which matters here (see the memory
budget section below). **If a future API needs to return large or
user-derived nested data, this is the wrong tool and a real builder in
`ms.c` is the right one.**

### The memory budget, and how this revision paid for itself

Adding all of the above cost `repl` about **12.8KB** of image. The
project had roughly 16KB of headroom before wm+net+repl+term stopped
fitting in the 1MB minimum main-memory requirement, so this needed
paying for. Three things did:

**1. Section GC (`--gc-sections`) -- the big one.** `sw/common` is
shared by every app, so each links whole objects for the sake of a
fraction of them: `repl` pulls in `zfont_data.o` for the single font
`text` uses and gets all four; `zobj.o`/`zgfx.o`/`zstream.o` are each
used partially. Compiling with `-ffunction-sections -fdata-sections`
and linking with `--gc-sections` lets the linker drop the rest.
Measured:

| app | saving |
|---|---|
| `repl` | ~6.9KB |
| `wm` | ~14.2KB |
| `term` | ~13.0KB |

On by default in each app's Makefile; set `GC_SECTIONS=0` to disable.
It is a pure win on every build tested, but section GC on a bare-metal
target can in principle drop something reachable only from assembly or
referenced solely by the linker script -- **if an app starts
misbehaving in a way that smells like missing code, this is the first
thing to rule out.**

**2. `Z_PROC_STACK_SIZE_MEDIUM` (32KB) for `net`.** `kernel.h`'s own
comment left this as an explicit open question: the unbounded
per-message `zport.h` leak was the one thing genuinely justifying 64KB
for `net`, it *is* fixed (`Z_PORT_DATA_ACK`), but whether `net` could
come back down was deliberately not guessed at. This is a partial
answer -- half the reduction, not all of it. What `net` still has is
the one-shot, intentionally-leaked RPC replies (DHCP/DNS/TFTP in
`net.c`), bounded by request *count* rather than session length, which
is the kind of slow accumulation 16KB might eventually not survive but
32KB comfortably should. **If this regresses, the symptom is not
obvious:** `net` stops responding or dies mid-session after long
uptime with heavy DHCP/DNS/TFTP traffic, with no error anywhere. Put
it back on `LARGE` if that appears.

**3. Not patching `ms.c`.** Both the `load` and the nested-value
decisions above avoided adding code to the largest object in the
build.

Net effect: `repl` grows ~5.8KB after its own section GC, while `wm`
and `term` return ~27KB and `net` returns 32KB to the pool.

### A note on build warnings

`zobj.h` used to `static`-define `z_ok`/`z_fail` in the header, giving
every translation unit that included it (almost always via
`zeitlos.h`) a private copy -- and a `-Wunused-variable` pair for each
one that didn't use them, which was nearly all of them. That was two
warnings per object file, in every app in the tree.

They are now declared `extern` in `zobj.h` and defined once in
`zobj.c`. Checked before changing: nothing mutates either object and
nothing compares a returned pointer against `&z_ok`/`&z_fail` (callers
read `rv->val.uint32`, never the address), so collapsing the copies
changes no behavior; and the four apps that include the header without
linking `zobj.o` (`blinky`, `bounce`, `bounceblit`, `hello`) don't
reference the symbols, so an extern *declaration* never becomes an
undefined reference for them -- all four verified to still link.

This is worth more than tidiness. Dozens of known-harmless warnings in
every build are the noise a real one hides in: the silent help-text
truncation described above sat in a build whose output was already too
noisy to read.

> Numbers measured with a `picolibc` `rv32i/ilp32` stand-in for the
> project's newlib toolchain -- **absolute** values will differ from a
> real `/opt/riscv32i` build; the deltas are what these are for.
> `net`'s own section-GC saving is unmeasured. Note also that
> `k_proc_create()` rounds `image + stack` up to 4KB, so real
> allocations move in page-sized steps.

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
= 8`) bounded table, not in the caller's own memory -- `FIL` is a
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

### Display -- IMPLEMENTED

| Procedure | Behavior |
|---|---|
| `(video-mode)` | current virtual phosphor mode as a string: `"white"`, `"amber"`, `"green"` or `"paper"` |
| `(video-mode m)` | set it; `m` is a name or a number 0-3. Returns the mode actually in effect, as a string |

Screen-wide, not per-window: the framebuffer is a single 1bpp surface
and this selects how a set bit is coloured at scanout. See
`docs/socctl.md` for the register, the modes, and why a change lands on
the next frame boundary rather than immediately.

Both a name and a number are accepted because both are natural here --
a person types the name, generated or looping code
(`(video-mode (modulo n 4))`) wants the number.

Setting returns the resulting mode rather than `#t`, so `(video-mode)`
and `(video-mode "amber")` both answer the same question: what is the
screen doing now. The value is read back from the register, not echoed
from the argument.

An unrecognised name or an out-of-range number panics, like every other
bad argument in `zapi.c`, rather than falling back to white -- a typo
that quietly reset the display would be a confusing thing to debug.
Gateware without the register panics too, with a message that says so,
since the fix there is a reflash rather than anything the caller can
change.

Deliberately **not** named `color`: that word already means the 1-bit
pixel value in `(line ...)`, `(box ...)` and `(text ...)`, and reusing
it for something screen-wide would be actively misleading. The kernel
shell's equivalent *is* called `color`, because `sh.c` has no such
clash.

### Game mode and gamepads -- IMPLEMENTED

| Procedure | Behavior |
|---|---|
| `(game-mode)` | current mode as a string: `"off"`, `"on"` or `"wrap"` |
| `(game-mode m)` | set it; `m` is a name or a number 0-2. Returns the mode actually in effect, as a string |
| `(game-view)` | viewport origin as a list `(x y)`, in framebuffer pixels |
| `(game-view x y)` | move it; returns the origin afterwards |
| `(game-frame)` | the display's own frame counter, 0-65535 |
| `(game-wait)` | block until the next frame boundary; `#t` |
| `(gamepad)` / `(gamepad n)` | list of pressed buttons as symbols, `()` if none, `#f` if there is no such pad |
| `(gamepad-count)` | how many gamepads are attached right now, 0-2 |

See `docs/game_mode.md` and `docs/gamepad.md`.

`game-mode` is deliberately the same **shape** as `video-mode` above --
one getter/setter, a name or a number accepted, the resulting state
returned rather than `#t`, read back from the register rather than
echoed from the argument. It is the same kind of thing: a single
screen-wide register with no compound state. Two procedures that behave
alike are worth more than two that each fit their own register slightly
better.

Names rather than `#t`/`#f`, even though the underlying enable is one
bit, because there are three states a caller cares about. Wrap is a
separate bit in the hardware, but "game mode, clamped" and "game mode,
toroidal" differ enough that folding them into a boolean plus a second
procedure would make the common case -- turning it on the way a
scrolling game wants -- take two calls instead of one.

Entering game mode from the REPL is a genuinely useful thing to do
interactively; it is how you see what the viewport does without writing
a program. It is also a good way to lose sight of the terminal you
typed it into, since `repl`'s own window may well be outside the
320x240 view. **Alt+Esc toggles back** regardless of what any program
has done -- which is exactly why that hotkey lives in the window
manager rather than in a library.

`game-view` reads back what was **written**, which is not always what is
being scanned out: with wrap off the hardware clamps the origin so the
viewport cannot hang off the edge, and it applies that clamp at scanout
rather than on the write path. See `docs/game_mode.md`.

`gamepad` returns **symbols**, not a bitmask, because this is the
interactive layer: `(memq 'a (gamepad))` reads as what it means, and
this interpreter has no bitwise operations to make a mask usable
anyway.

It returns `#f` -- not `()` -- when there is no such pad, because "no
pad" and "a pad with nothing pressed" are genuinely different answers
and a "press start" screen needs to tell them apart. Same call as
`(current-time)` makes for an unset clock, and for the same reason:
asking what a pad is doing is a **question**, and "there isn't one" is
a real answer to it, so it composes rather than blowing up a one-liner.
Setting the mode is something the caller asked to **do**, so gateware
without game mode panics there instead.

`n` is a **pad index**, not a port: pad 0 is whichever port currently
holds a gamepad. Unplug the pad from port 0 while a second sits in port
1 and the survivor becomes pad 0. Nothing binds a device to a port on
this machine -- see `docs/gamepad.md`.

`game-wait` blocks this entire process for up to 16.7ms, with the same
consequences `(delay-ms ...)` has and for the same reason. Fine for a
one-shot at a prompt; a Scheme loop calling it sixty times a second is
not what this interpreter is for, and the answer to wanting that is a C
program against `sw/common/zgame.h`.

### GPIO -- IMPLEMENTED

| Procedure | Behavior |
|---|---|
| `(gpio-ports)` | how many GPIO ports this bitstream has pins for; `0` if none |
| `(gpio-dir p)` / `(gpio-dir p mask)` | the port's DIR register, one bit per pin, 1 = output. Returns what it reads back as |
| `(gpio-out p)` / `(gpio-out p v)` | the port's OUT register. Not the pins |
| `(gpio-in p)` | the pins, as a number. Read-only |
| `(gpio-mode p n)` / `(gpio-mode p n m)` | `"in"`, `"out"` or `"od"`. Returns the mode afterwards |
| `(gpio-get p n)` | the pin, `#t` or `#f` |
| `(gpio-set p n v)` | drive it. Returns the pin afterwards |
| `(gpio-toggle p n)` | flip OUT. Returns the pin afterwards |
| `(gpio-od p n v)` | open-drain write; `v` is the level the line ends up at |
| `(led)` / `(led v)` | the board LED |
| `(leds)` / `(leds v)` | the `` `LED_DEBUG `` LED bar, as a number |

See `docs/gpio.md`.

**Ports and pins are numbers, and that is what makes bare-word syntax
work here.** `gpio-set 0 3 1` reaches `(gpio-set 0 3 1)` with no
quoting, because a token that parses wholly as a number passes through
unquoted (\S1b). An earlier design used single-token pin names --
`"B3"` -- and every one of these procedures would have had to unpack a
string that arrived quoted. It was dropped for an unrelated and better
reason (letters already mean PMOD connectors in `release/hw/boards/*.spec`),
but this is what was gained.

**Whole-port procedures are named after the registers; per-pin ones are
not.** `gpio-dir`, `gpio-out` and `gpio-in` are what `docs/gpio.md`
documents and what somebody reading a register dump is holding in their
head. At the per-pin level nobody is thinking about registers, so those
get verbs.

**Setters take `#t`/`#f` or `1`/`0`, and `0` is false.** That last part
deviates from Scheme, where every number is true, and it is deliberate:
`(gpio-set 0 3 0)` meaning "drive it high" would be an afternoon lost,
and the shell's own `gpio 0 3 0` (`sw/os/sh.c`) means low. Accepting
both spellings means `(gpio-set 0 3 (gpio-get 0 2))` mirrors a pin and
`gpio-set 0 3 1` still works from the bare prompt.

Implemented with `zapi_arg_truthy()`, which needs no change to `ms.c`:
`#t` and `#f` are singletons in the interpreter, so `ms_mk_bool(false)`
hands back the one and only `#f` object and identity against it is the
entire test -- exactly `ms.c`'s own `truthy()`. Worth knowing about,
since `ms_api.h` exports no boolean predicate and the next procedure
that wants one has the same option.

**Every reader returns the pin, not the argument.** `(gpio-set 0 3 1)`
on a pin still configured as an input returns `#f`: nothing is driving
it, and the value was staged for whenever it becomes an output. An echo
would have been a lie, and this is a mistake worth having reported by
the value rather than by a silent nothing.

That matters most for `gpio-od`. On a working bus with a pull-up,
`(gpio-od 0 3 #t)` reads back `#t`; on a bus another device is holding
down it reads back `#f`. That is how you see a stuck I2C slave from a
prompt, in one call.

**`(gpio-mode p n "od")` reads back as `"in"`.** Open drain is not a
hardware mode and there is nowhere to record it (`sw/common/zgpio.h`);
a pin set that way is an input that `gpio-od` drives low on demand,
which is exactly what DIR says about it.

**Out-of-range ports panic rather than returning `#f`.** Unlike
`(gamepad n)` above, where "there isn't one" is a real answer to a
question, writing to a port that does not exist is silently dropped by
the hardware with no way to report it -- so a panic is the only place a
caller can find out they typed the wrong number. `(gpio-ports)` is the
question form.

`(led)` and `(leds)` are not gated on `(gpio-ports)`: they are words 0
and 1 of the same block and exist on every board, with or without any
GPIO pins.

### I2C and SPI -- IMPLEMENTED

Bit-banged over the GPIO pins above. There is no I2C or SPI hardware
behind these; see `docs/i2c.md` and `docs/spi.md`.

| Procedure | Behavior |
|---|---|
| `(i2c-init port scl sda [khz])` | bus handle, or `#f` if the bus is unusable right now |
| `(i2c-scan bus)` | 7-bit addresses that answered, as a list |
| `(i2c-write bus addr data)` | `data` is a list of bytes or a string |
| `(i2c-read bus addr [n])` | list of bytes, or `#f` |
| `(i2c-reg bus addr reg)` / `(i2c-reg bus addr reg val)` | read or write one register |
| `(i2c-recover bus)` | clock a stuck slave off SDA |
| `(i2c-error)` | why the last i2c call failed |
| `(i2c-khz bus)` | rate the last transfer actually managed |
| `(spi-init port sck mosi miso cs [mode [khz]])` | bus handle; `-1` for a pin the device lacks |
| `(spi-select bus v)` | assert or release CS |
| `(spi-xfer bus data)` / `(spi-xfer bus n)` | send bytes, or `n` idle bytes, and return what came back |

**Buses are handles, and handles are numbers**, the same convention
window handles use. Passing the pins to every call instead would mean
five arguments on every read, no place to keep the derived timing, and
reconfiguring a bus meaning "remember to change it everywhere".

**A bus lives on one port.** The C API lets each pin be on any port;
these take a single port and pin numbers within it. Not a
simplification for its own sake: `(spi-init)` would otherwise need ten
arguments, and a four-pin SPI device is plugged into one PMOD
connector essentially always. A bus genuinely spanning two ports is a
C program.

**Bus failures return `#f`; caller mistakes raise.** "Nothing
answered" is an ordinary result on an I2C bus — it is what a scan is
made of, and what a device that is still busy gives you — so a
procedure that blew up the whole expression on a NACK would be
unusable in a loop. A bad handle or a byte outside 0-255 is the
caller's error and raises normally.

`(i2c-error)` is how to tell which happened, and the distinction is
the point: `"nack"` means the bus works and nobody answered, so a
retry might help; `"timeout"` means a released line never came back
up, which is a missing pull-up or a short and no amount of retrying
will fix it.

**Addresses are 7-bit** — `0x3c`, not `0x78`. Getting this wrong is
the single most common I2C mistake and the only defence is to be
consistent about it and say so.

`i2c-write` accepts a string as well as a list because
`(i2c-write bus 60 "hello")` is what anyone driving a character
display wants to type.

`(spi-xfer bus n)` with a count rather than a list is the read case:
SPI is full duplex, so reading means sending something, and it sends
`255` — which is what a device expects to see while it is talking, and
is not a meaningful command byte on most parts the way `0` is.

**`spi-xfer` does not touch CS**; wrap it in `spi-select` calls.
Almost every real device wants several transfers inside one selection
(a command, an address, then a burst), and a select-per-transfer API
cannot express that.

At most 32 bytes per call and 2 open buses of each kind -- this is the
interactive layer, and a driver moving more than that belongs in C.

### UART1 -- IMPLEMENTED

A second 16550, independent of the console. See `docs/uart1.md`.

| Procedure | Behavior |
|---|---|
| `(uart1?)` | is there a general-purpose UART1 in this bitstream |
| `(uart1-open [baud])` | 8N1, default 115200. `#f` if absent or the rate is unreachable |
| `(uart1-close)` | |
| `(uart1-baud-error baud)` | how far off that rate would be, in percent |
| `(uart1-write data)` | string or list of bytes; blocks. Returns how many went |
| `(uart1-read [n])` | list of bytes; never blocks. `()` if nothing waiting |
| `(uart1-ready?)` | is a byte waiting |
| `(uart1-status)` | errors since the last call: `(overrun)`, `(framing)`, `()`, ... |

**UART0 is deliberately absent from this API**, and from
`sw/common/zuart.h`. It is the console -- `sw/bios/bios.c` writes to it
before anything else in the system exists. A Scheme one-liner that
changed its baud rate would take the machine's only diagnostic channel
with it.

**These talk to UART1 directly**, so this process competes with
`sw/apps/serial` for it if that is running. Nothing arbitrates MMIO
(`docs/app_runtime.md`). The honest reading: these are for poking at a
serial port from a prompt -- send an AT command, see what a device says
on power-up, check a baud rate -- and repl's `serial` command is for
actually using one. Doing both at once produces interleaved bytes and
two processes that each think they set the baud rate.

**`(uart1-read)` never blocks** and returns what is there rather than
waiting for `n`. A blocking read at a prompt with nothing on the other
end would hang the REPL with no way out, and the FIFO is 16 bytes deep
so there is rarely more to wait for. Call it again.

**`(uart1-baud-error)` is worth asking before blaming a cable.** At
48MHz the divisor makes 921600 land on 1 Mbaud -- 8.5% off, far outside
what a UART tolerates -- so `(uart1-open 921600)` returns `#f` rather
than silently giving you a port that works at 115200 and produces
garbage above it. `docs/uart1.md` has the table.

**`(uart1-status)` is sticky and cleared by reading**, because the
16550's own error bits are cleared by any read of its status register
and every data read goes past it. `(overrun)` means bytes were LOST,
silently, from the middle of the stream -- on a polled receiver at
115200 that is a real possibility, not a theoretical one: one
scheduler slice is 15.7 bytes of arrival against a 16-byte FIFO.

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
   name is a full path (`"/APPS"`), fixing the pre-existing `sh.c`
   double-slash display bug in the process (\S4, Files).
3. **Numeric precision** -- resolved: staying with `double` (`T_NUM`).
   Every id/pid/size this API deals with is well under 2^53 (exact in
   a double), no concern in practice.
