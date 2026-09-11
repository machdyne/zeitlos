# nextvi, as vendored for Zeitlos

Upstream: https://github.com/kyx0r/nextvi -- ISC licensed, see `LICENSE`.

Vendored rather than submoduled so that the local changes below are
visible in the tree and in review. Re-vendoring means dropping a fresh
upstream copy in and reapplying this list, which is why the list is
short and each item says what it is for.

## Local changes

**1. `uc.c`, `vi.h` -- `uc_bytemode()`, behind `#ifdef ZEITLOS`.**

Sets `utf8_length[1..255]` to 1, so every byte is one character.

This is the whole of "disabling UTF-8". nextvi routes every
multi-byte decision through `uc_len(s)`, which is
`utf8_length[(unsigned char)s[0]]` -- so the table IS the switch, and
no `#ifdef` has to touch the editor's logic.

**It is required, not optional.** Zeitlos terminal fonts cover
0x20-0x7f and `sw/common/zgfx.c` draws nothing outside that range, so a
screen cell is exactly one byte. With the table left as upstream has
it, a two-byte sequence is one character to nextvi and two blank cells
to the terminal: the cursor column and the screen disagree from there
on and every subsequent redraw is wrong. Flattened, the arithmetic
matches the rendering exactly.

Index 0 stays 0 -- `uc_len()` returning 0 is the end-of-string test.

## What follows from that, needing no further change

- **`ren.c`'s two non-ASCII outputs** -- the combining-character joiner
  and the replacement glyph -- are both behind `if (l == 1) return
  NULL`. With every length 1, they are unreachable.
- **`conf.c`'s right-to-left character ranges** are matched against
  file content; ASCII never matches them.
- **`xshape` and `xorder`** (Arabic shaping, bidirectional reordering)
  are runtime flags the Zeitlos front end sets to 0 at startup. Both
  are already no-ops on ASCII; turning them off is for size and speed.
- **`kmap.h`** is input-method data for Farsi, Russian and others,
  reached only through the `:cm` command. Harmless, and a candidate
  for removal if the binary needs to be smaller -- about 1KB of the
  source is those tables.

## How it is built

**One translation unit.** nextvi is a unity build: `vi.c` `#include`s
`conf.c`, `ex.c`, `lbuf.c`, `led.c`, `regex.c`, `ren.c`, `term.c` and
`uc.c`, and upstream's `cbuild.sh` compiles `cc vi.c` and nothing else.
The individual files do NOT compile standalone -- they rely on `vi.h`
and on each other having been included first.

Worth stating because the file list looks exactly like a set of
translation units, and compiling them as one is the first thing anyone
will try.

`sw/apps/vi/Makefile` compiles `$(NEXTVI)/vi.c` with:

- `-DZEITLOS=1` -- selects `uc_bytemode()`;
- `-Dmain=nextvi_main` -- renames the entry point so that
  `vi_zeitlos.c` can own `main()` and do the terminal handoff before
  the editor starts. **That rename is what lets every line here stay
  untouched.**

## One translation the front end has to do

**CR becomes LF on input.** `sw/apps/vi/vi_zeitlos.c` does it in the
stdin hook, and it is not a Zeitlos quirk -- it is a tty driver's job
that there is no tty driver to do.

`term` sends 0x0d for Enter (`sw/common/zkbd.c` maps the HID usage to
CR). On a POSIX system the terminal driver translates that to 0x0a
before any application sees it, because `ICRNL` is an INPUT flag and is
on by default. nextvi's `term_init()` clears `ICANON`, `ISIG` and
`ECHO` -- all `c_lflag` bits -- and never touches `c_iflag`. So it does
not disable the translation; it **relies** on it.

Symptom without it: Enter inserts a literal CR, the editor renders it
as `^M`, and no line is ever broken.

Worth knowing before porting any other terminal program here: the same
applies to anything that assumed a tty was in front of it.

## Arrow keys, and `vi_typing`

`term` sends `ESC [ A` for Up, and it must keep doing so: `repl`,
telnet and ssh sessions all depend on it, and the remote end of those
is a real terminal that expects it. So nothing about `term` changes.

nextvi binds no escape sequences -- neither does real vi, where
movement is `hjkl` -- so Up read as three keystrokes: `ESC` (leave
insert mode), `[` (a motion prefix), then **`A`, which is "append" and
ENTERS insert mode.** The cursor did not move and typing started
inserting. A few arrow presses can produce `ZZ` and exit the editor.

`sw/apps/vi/vi_zeitlos.c` translates the sequences to `h`/`j`/`k`/`l`
in its stdin hook. **The translation must not happen while text is
being typed**, or an arrow inserts that letter into the document --
which is worse than the arrow doing nothing.

### Local change 2: `vi_typing` (`led.c`, `vi.h`)

`led_input()` and `led_prompt()` -- insert mode, the `:` line, search
prompts -- are renamed to `*_inner` and wrapped by functions that
increment and decrement `vi_typing` around them.

Wrapped rather than incremented in place because both have several
return points and a counter that leaks is worse than no counter. A
counter rather than a flag because `led_prompt()` can be entered from
inside `led_input()`.

The front end translates arrows only when `vi_typing` is zero, and
only when the whole three-byte sequence is already in its input ring --
a lone `ESC`, the user pressing Escape, arrives in its own message and
passes through untouched. That is what makes this safe rather than a
guess about timing.

## What still has to be ported

Everything the editor needs from the OS, counted:

| | |
|---|---|
| `tcsetattr` | raw mode -- not needed, a Zeitlos port connection is already raw |
| `ioctl(TIOCGWINSZ)` | terminal size; one call |
| `fork`/`execvp` | `:!` shell-outs, two sites; stubbed at first |
| `signal`, `poll` | stubbed |
| `dirent` | directory listings only |

All of it funnels through `term.c`'s `term_read()` and
`term_write()` -- and **neither is replaced.** Both reach the outside
world through newlib (`read(fd, ..., 1)` and `write(1, ...)`), and
`sw/common/zeitlos.c` routes those through `z_stdout_hook` and
`z_stdin_hook`. So `term.c` runs as upstream wrote it, and
`sw/apps/vi/vi_zeitlos.c` installs the two hooks.

The stdin hook did not exist before this; it was added alongside a
real bug in `_read()`, which had been returning uninitialised stack
for as long as it has been in the tree (it waited for a UART byte and
then never called `uart_getc()`). Nothing had noticed because nothing
in the tree reads stdin through newlib.

**Three symbols are renamed at build time**, in
`sw/apps/vi/Makefile`, with `-D` rather than an edit here:

| | | |
|---|---|---|
| `main` | `nextvi_main` | so `vi_zeitlos.c` owns `main()` and can do the terminal handoff before the editor starts |
| `itoa` | `nextvi_itoa` | see below -- NOT done with `-D` |
| `lstat` | `stat` | FAT has no symbolic links, so on this machine `lstat` IS `stat`, and newlib declares only the latter |

Every name nextvi exports was compared against what a C library
plausibly claims, rather than discovering them one build at a time.
`itoa` is the only real collision; `swap` looks like one and is
`static`.

**`itoa` is renamed in `sw/apps/vi/shim/stdlib.h`, not on the command
line**, and the reason is worth keeping: `-Ditoa=nextvi_itoa` applies
to the whole translation unit, **including newlib's own header**. So
newlib's three-argument declaration became `nextvi_itoa` too and the
conflict survived the rename intact. The rename has to happen after
newlib has been read and before nextvi's `vi.h`, and `vi.c` includes
them in that order with nothing in between -- so the only place to put
it is a shim that `#include_next`s the real header.

**Four headers are shimmed, in `sw/apps/vi/shim`, and they are not
optional.** nextvi includes `<poll.h>`, `<dirent.h>`, `<sys/ioctl.h>`
and `<termios.h>`. Checked against the toolchains this tree uses:
`poll.h`, `sys/ioctl.h` and `termios.h` are simply absent, and
newlib's `dirent.h` for this target is a single
`#error "<dirent.h> not supported"`. That is true of embedded newlib
and picolibc alike -- these are hosted-OS headers -- so the shims are
the difference between the editor compiling and not, whichever
toolchain is in use.

`-Ishim` comes first on the include path, the same arrangement
`sw/apps/zcc/libz` uses.

### File I/O needed an adapter

nextvi opens, reads and writes files with newlib's `open`/`read`/
`write`. On Zeitlos those have never worked -- `_open()` in
`sw/common/zeitlos.c` returns `ENOENT` unconditionally, because every
app here uses `sw/common/zfsapp.h` directly instead.

`sw/apps/vi/fileio.c` is a descriptor table over `fs_open_read()` and
friends. The runtime's `_open`/`_read`/`_write`/`_close`/`_fstat` are
now **weak**, so an app can override them without every binary that
links `zeitlos.c` gaining a filesystem it does not use.

Worth knowing for the next port: this is a general gap, not a nextvi
quirk. Anything that reads a file through stdio will hit it.

### Memory

**Under 50KB of heap to start, with full syntax highlighting.** A
154KB source file then needs about 3.4 times its own size, because
nextvi allocates per line. `sw/os/kernel.h` puts `vi` in a 1MB tier.

It looked like 1.1MB for a while, and the difference was not nextvi's
doing: `sw/apps/vi/fileio.c` reported `st_size` as 0 from `_fstat()`,
and `lbuf_rd()` falls back to a **1,048,575-byte** read buffer when it
cannot learn a file's size. Every file opened allocated a megabyte.

**Do not trim `conf.c`.** Removing all 135 syntax rules was measured
and saves about 100KB of heap and 11KB of binary -- not worth a local
change to a vendored tree that has to be reapplied on every update.
The highlighting is inert anyway: the framebuffer is 1bpp and the
terminal carries only a reverse-video attribute, so the colour
sequences are parsed and discarded.

### Measured

Built with the tree's own toolchain (xPack riscv-none-elf-gcc 15.2.0):

```
   text    data     bss     dec
 206972   19076   11588  237636      vi.elf
 vi.bin: 227996 bytes
```

228KB, which is app-sized -- `zcc` is 118KB and `ttytest` 110KB. About
34KB of it is newlib's printf family, which nextvi uses for its
message line; `docs/kernel.md` describes the same trap on the kernel
side if that ever needs reclaiming.

`sw/apps/vi/posix_stubs.c` is the rest: `tcsetattr` (not needed),
`ioctl` (fails on purpose, so `term_init()` falls through to 80x25),
`isatty` (must say yes, or the editor exits at once), `signal`/`kill`,
`poll` (always ready -- the stdin hook does the blocking), and
`fork`/`execvp`, which is the one real loss: `:!` cannot run anything.
