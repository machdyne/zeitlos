# The Zeitlos Scheme API

How the procedures in `repl`'s Scheme are made, documented and reached:
for someone adding one. The list of what there is, with how to call
each and an example, is [zapi.md](zapi.md), generated from the code; on
the machine it is `/docs/zapi.txt`, and `help <name>` and
`apropos <text>` look things up in it. How to use repl and Scheme is
[repl.md](repl.md); how ms is built into repl is [scheme.md](scheme.md).

## Status (October 2026)

280 names in 180 entries: ms's builtins, special forms and standard
library, and the Zeitlos procedures (files, windows, processes,
messages, TFTP, time, display and game mode, gamepads, GPIO, I2C, SPI,
UART1, random numbers, help). The reference is checked against the code
at every build of the flash archive; `help` and `apropos` are tested on
the host. Not yet tried on hardware: `help` and `apropos` themselves.

| | |
|---|---|
| `sw/apps/repl/zapi.c` | the Zeitlos procedures, each with its reference entry |
| `sw/apps/repl/zapi_scheme.txt` | entries for the Scheme language: ms's builtins, special forms, standard library |
| `tools/gen_zapi.py` | makes the reference, and fails if it does not match repl |
| `/docs/zapi.txt` | the reference in flash (`CORE_FILES`), made at build time |
| `docs/zapi.md` | the same as a page, committed: `make zapi` |
| `sw/apps/repl/zapi_doc.c` | the lookup behind `help` and `apropos` |
| `sw/apps/repl/ms_api.h` | what a procedure written in C can use of ms |

## Adding a procedure

1. Write it in `zapi.c`: `static ms_val *zapi_name(ms_val *args)`,
   arguments already evaluated, as a list.
2. Register it in `zapi_register()`: `ms_def_builtin("name",
   zapi_name);`. That makes it callable from Scheme and as a bare
   command. Its place in that function is its place in its section of
   the reference.
3. Give it an entry, above it, under the right `//@ Section`.
4. `make zapi` and commit `docs/zapi.md` with the change.

A procedure is better written in Scheme when it can be: add it to the
`ms_load_string()` block in `zapi_register()`, as `win-on` is, and give
it an entry there. Names starting with `%` are internal and need none.

## The entries

```c
//@ Files
//: (read-file "file")
//:   The whole file as a string; #f if it is missing, empty or cannot
//:   be read. For text: a zero byte ends the string.
//:   > (read-file "/ram/a.txt")  =>  "hi"
```

- `//@ Section` -- the section for the entries after it in the file,
  until the next. A section must be in `SECTIONS` in `gen_zapi.py`,
  which sets their order.
- `//: (name args)` -- a call shape, exactly one space after the colon.
  Each optional form gets its own line. Several shapes naming
  different procedures make one entry for all of them (`(caar x)`,
  `(cadr x)`, ...).
- `//:   text` -- the description, three spaces in: what it does and
  what it returns, including `#f` and errors. Plain words, for someone
  at the prompt; the reasons belong in the comment below the entry.
- `//:   > (example)  =>  value` -- at least one example, and what it
  returns. `=>  value` can be left off when it says nothing (a
  `define`).
- Any other line ends the entry. Lines stay under 100 characters.

`zapi_scheme.txt` has the same lines without the `//`, plus `~ name
...` for names that are bound but not listed (standard-library
internals, `else`, the constants) and `#` comments.

## The generator

`tools/gen_zapi.py` takes the names from the code, not from the
entries:

- `ms_def_builtin()` calls and `(define ...)`s in `zapi_register()`;
- ms's `def_builtin()` calls, through the C preprocessor with `-DLIX`
  as repl builds it (that drops ms's file builtins, which repl replaces
  with its own), and its special forms and constants;
- every top-level `define` in `sw/ext/ms/ms_stdlib.l`.

It fails, naming each, on a name with no entry, an entry for a name
repl does not have, a name in two entries, an entry with no
description or example, and a line too long. So an ms update that adds
a builtin, or a procedure added without an entry, stops the archive
build (`make flash_apps`, a release) until the entry is written.

| | |
|---|---|
| `make zapi` | writes `docs/zapi.md`; its date changes only when something else does |
| `$(OUTDIR)/files/docs/zapi.txt` | the flash copy, a `make core-files` target |
| `zrelease check` | fails if `docs/zapi.md` is stale (the date line is not compared) |
| `make -C sw/apps/repl/tests` | runs the Scheme examples through a desktop ms and compares their values, then tests `zapi_doc.c` |

`/docs/zapi.txt` is about 26KB, from `CORE_RESERVE` (48K, of which the
files in flash use 46KB; [flash_apps.md](flash_apps.md)).

## help and apropos

Both are ordinary procedures (`zapi.c`), so `help ls` at the prompt is
`(help "ls")` by the bare-command rule below; the builtin `help`, with
no argument, still prints repl's own commands. They read
`/docs/zapi.txt` through a 256-byte buffer, never whole, and print:

- `(help "name")`: the entry, or `#f` and a pointer to `apropos`.
- `(apropos "text")`: one line per entry whose name, call shapes or
  description contain the text, ignoring case -- the name and the start
  of the description, in 80 columns. At most 100 lines, then a count of
  the rest; it returns the number of matches.

They rely on the format the generator guarantees: an entry starts after
a blank line with its call shapes at column 0, its other lines are
indented, and a blank line ends it (`zapi_doc.h`).

## Writing a procedure in C

`ms_api.h` is the whole interface: `ms_mk_str` (takes a `malloc`'d
string), `ms_mk_num`, `ms_mk_bool`, `ms_nil_val`, `ms_mk_str_list`;
`ms_is_str`/`_num`/`_pair`/`_nil`, `ms_str_val`, `ms_num_val`, `ms_car`,
`ms_cdr`; `ms_log` to raise an error. `ms_val` stays opaque.

- **Arguments:** `zapi_arg_str()` and `zapi_arg_int()` raise an error
  for the wrong type. `zapi_arg_truthy()` takes `#f` and `0` as false,
  for hardware, where `(gpio-set 0 3 0)` meaning high would cost
  someone an afternoon.
- **Errors:** `ms_log(MS_PANIC, "name: what went wrong")` unwinds to
  repl, which prints `repl: Scheme error` and the reason on the serial
  console. Use it for a request that failed for a reason the caller
  must know (a TFTP transfer, a window that is not this process's);
  return `#f` for an answer of "no" or "not known" (a missing file, an
  unset clock).
- **Lists:** `ms_mk_str_list()` for a list of strings. Anything nested
  is printed into a buffer and read back with `zapi_read_form()`
  (`ms_read`), as `ps`, `free` and `current-date` do: building pairs
  from outside ms means getting the garbage collector's protection
  right by hand. Only for numbers and fixed names, never text a user
  supplied.
- **Getters and setters** are one procedure: `(video-mode)` reads,
  `(video-mode "amber")` sets and returns the state in effect, not
  `#t`.
- **Handles** -- windows, buses -- are numbers, so they pass through
  the bare-command syntax.
- **Names** become commands (below), so a short generic one is taken
  from every user: `touch-file`, not `touch`.
- **Blocking** stops all of repl: one main loop serves every connected
  terminal and every window. `delay-ms`, `game-wait`, `msg-wait`
  without a timeout, `tget` and `tput` block; anything longer than a
  moment belongs in a C program.

Each procedure costs three cells of the Scheme heap (`MS_HEAP_SIZE`,
4,000) and its code. repl's RAM budget is the one that binds
([flash_apps.md](flash_apps.md#why-repl-is-a-core-app)): 416 bytes are
left of 304K.

## How a line reaches Scheme

`dispatch_line()` (`repl.c`) handles repl's own commands first -- those
that need the connection itself: `help` alone, `ping`, `echo`, `te`,
`page`, `scheme`, `lock`, `quit`, and the connections (`port`,
`serial`, `usbserial`, `telnet`, `ssh`; `zconnect.c`). Anything else is
Scheme.

**Bare commands.** A line whose first word is bound to a procedure
(`ms_is_callable()`, at the moment it is typed) is translated:
`ls /ram` becomes `(ls "/ram")`. Each further word becomes:

| word | becomes |
|---|---|
| a number, the whole word (`10`, `-3.5`) | a number (`192.168.1.1` is not one, so it stays a string) |
| `#t`, `#f`, `#true`, `#false` | a boolean |
| starting with `"` or `(` | itself: Scheme written by hand |
| anything else | a string |

A word bound to something else, or to nothing, is left as typed, so a
variable's name still shows its value. `scheme <expr>` evaluates
`<expr>` as written: `scheme ls` shows the procedure instead of calling
it. There is no quoting in bare words; a value from a variable needs
the real form.

## Output

ms prints through stdout. While a command from a terminal runs, repl
catches stdout (`z_stdout_hook`), turns a bare `\n` into `\r\n`, and
sends it to that terminal in 2KB pieces. A command has about 8 sends
before acknowledgements are needed (`Z_PORT_MAX_PENDING_SENDS`), so
about 14KB of output; past that, the rest goes to the serial console
with a warning. `te` and `page` sessions are not caught. A request with
no terminal (`Z_REPL_EVAL`) prints to the console.

`print-console` writes to stderr, which is never caught: the serial
console, always, in order with ms's own `[panic]` lines.

The value of an expression is printed into a 1KB reply
(`Z_REPL_REPLY_MAX`) and marked `...[truncated]` when it does not fit.
The builtin `help` text must fit 256 bytes (`Z_REPL_EVAL_REPLY_MAX`); a
compile-time check enforces it.

## Implementation log

**2025.** `zapi.c` began with five file procedures and the one patch to
`ms.c` they needed (`ms_def_builtin`, `ms_is_callable` and the small
constructors in `ms_api.h`). Windows, messaging and TFTP followed, then
processes, time and memory, which replaced repl's old builtin commands
of the same names; then display and game mode, colour, gamepads, GPIO,
I2C, SPI, UART1 and random numbers. Each batch was described here in
detail, with its design questions.

**October 2026.** That catalogue had fallen behind the code and did not
list ms's own procedures at all. It became the generated reference:
entries beside the code (`//:` in `zapi.c`, `zapi_scheme.txt` for the
language), `tools/gen_zapi.py` with its completeness checks, the
reference in flash as `/docs/zapi.txt` (a core file, `CORE_RESERVE`
from 21K to 48K), `docs/zapi.md` committed with `zrelease check`
watching it, and `help` and `apropos`. This page became the short one
for developers; the per-procedure reasoning stays in `zapi.c`'s
comments. Checking every entry against its code found the reference's
first errors before it shipped: the named-bus form of `i2c-init`,
optional arguments of `spi-init`, `i2c-read` and `i2c-write`, and
widget numbers starting at 1. repl was measured for the first time in
this tree (its submodules had not been checked out): 117,968 bytes in
flash compressed, a 310,880-byte image.
