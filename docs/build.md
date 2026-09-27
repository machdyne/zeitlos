# Building apps: `sw/common/app.mk`

Every app under `sw/apps` builds with the same compiler flags, the same
link step and the same ZEXE packing. Those rules live once, in
`sw/common/app.mk`, and each app's Makefile says only what is
particular to that app.

## An app's Makefile

The whole of `sw/apps/files/Makefile`, less its comments:

```make
APP = files
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zfsapp.o zflist.o zdialog.o zedit.o ztype.o files.o

include ../../common/app.mk
```

That gives:

| | |
|---|---|
| `make` | `files.elf`, `files.bin` (ZEXE, see [executables.md](executables.md)), `files.map`, `files.dasm`, `files.asm` |
| `make clean` | removes all of the above, every `.o` and every `.d` |

Every object in `OBJS` is compiled from a `.c` of the same name, looked
for first in the app's own directory, then in `SRC_DIRS`, then in
`sw/common`. So `zgfx.o` comes from `../../common/zgfx.c` and `files.o`
from `files.c`, with no rule written for either.

## Variables

Set these **above** the `include`.

| Variable | Default | |
|---|---|---|
| `APP` | *(required)* | The target name: `$(APP).elf`, `$(APP).bin`. |
| `OBJS` | *(required)* | Every object to link, **in link order**. |
| `APP_CFLAGS` | empty | Extra `-D` / `-I` flags for this app. |
| `GFX_HW_BLIT` | `1` | Text through the hardware glyph blitter (`-DZ_GFX_HW_BLIT`); `0` for the software path. |
| `ZFMT` | `0` | `1`: integer-only `printf` family, about 50KB smaller. The core apps set it; see below. |
| `SRC_DIRS` | empty | More directories to search for sources, before `sw/common`. |
| `GC_SECTIONS` | `1` | Section garbage collection. See below. |
| `APP_LISTING` | `1` | Also write `$(APP).asm` from `$(APP).c`. |
| `APP_MAIN_RULE` | `1` | `app.mk` provides the `$(APP).o` rule. Set `0` if the app has no `$(APP).c` or compiles it itself. |
| `LDLIBS` | empty | Extra link inputs after `$(OBJS)`, e.g. `-lm` (`repl`). |
| `CLEAN_EXTRA` | empty | More files or directories for `make clean`. |
| `COMMON_DIR` | the directory `app.mk` is in | Rarely set; derived from the `include` path. |

## Rules that hold it together

**Include `app.mk` last.** It reads `APP`, `OBJS`, `SRC_DIRS`,
`GC_SECTIONS` and the rest at the moment it is included, because a
rule's prerequisites are expanded when make reads the rule. An app
that adds objects under a build option (`OBJS += ...` inside an
`ifeq`, as `web` and `net` do) must have done so by then. Rules an app
defines above the include are fine: the default goal is pinned to
`$(APP)`.

**Link order is the app's to state.** `OBJS` is the order the linker
sees, and that decides the layout of the binary. `app.mk` never
reorders or derives it.

**No app source may share a name with a file in `sw/common`.** The
search finds the app's own file first, so a local `zgfx.c` would
silently replace the shared one for that app. None does today.

**The relative path is deliberate.** `COMMON_DIR` stays
`../../common` rather than being resolved to an absolute path. The
path is what reaches the compiler as the source name, so it is what
`__FILE__` in `assert()` and the debug info contain. An absolute path
would make every binary depend on where the tree was checked out.

**Flags an app needs survive a command-line `CFLAGS`.** `app.mk`
appends with `override CFLAGS +=`. A plain `make CFLAGS=...`
replaces a makefile's `CFLAGS` outright, dropping `-Os`, `-march`,
section GC and the libc specs. The binary still builds; it is just
larger and slower, and can overrun the space the loader has for it.
Any later append in an app's own Makefile must also say `override`,
or make silently ignores it (`sw/apps/net/Makefile` has the story).

**Header dependencies are tracked.** `CFLAGS` carries `-MD` and
`app.mk` includes the `.d` file of every object it builds, so editing
a header rebuilds exactly what includes it. Only those `.d` files are
included. The old per-app Makefiles also included `*/*.d`, which in
`sw/apps/net` picked up `netserve/`'s dependency files. Their paths
are relative to `netserve/`, so every rebuild of `net` after
`netserve` had been built stopped with "No rule to make target".

## When an app needs something different

An explicit rule in the app's Makefile always takes precedence over
the pattern rule in `app.mk`. So an object that needs different flags
just gets its own rule.

| App | What it does differently |
|---|---|
| `web` | Crypto at `-O2` (`CRYPTO_CFLAGS`), `zimg.o` with PNG enabled, host tests on `HOSTCC`. |
| `posix`, `vi`, `zcc`, `zfpga` | `-Werror` on their own sources only, as `OWN_CFLAGS`. The `sw/common` objects keep ordinary warnings, which vary by toolchain version. |
| `zcc`, `zfpga` | Objects go in `build/` so the host build (`Makefile.host`) cannot overwrite them. `zcc` also builds `libz`, and `zfpga` its device database, as extra prerequisites of the default goal. |
| casino family | Shared game code compiled from `sw/common/games`. |

Extra prerequisites and clean steps are added without touching
`app.mk`, because a target may collect prerequisites from any number
of rules as long as only one has a recipe:

```make
zcc: libz.data           # built by `make` along with zcc.bin
libz.data:
	$(MAKE) -C libz

clean: clean-libz        # run by `make clean` along with the rest
clean-libz:
	$(MAKE) -C libz clean
```

## The hardware glyph path is on for every app

`app.mk` defines `Z_GFX_HW_BLIT` for every app: text is seven register
writes a glyph through the blitter instead of ~5,300 cycles in
software ([window_manager.md](window_manager.md#hardware-glyph-blitting)).
There is no app that should draw text the slow way. `zgfx.c` already
falls back to software per glyph where the hardware cannot help (a
font not in glyph memory, a glyph not wholly on screen), and in game
mode the blitter writes the same framebuffer bytes software would.

It used to be each app's own `-D`, repeated with a paragraph of
explanation in 36 Makefiles and missing from 19 others. Making it the
default left 52 of 56 binaries byte-identical: every app that already
had it, and every app that draws no text. Four changed, and got faster
text: `chip8`, `gamedemo`, `mmod` and `repl`.

`make GFX_HW_BLIT=0` (after `make clean`: `-MD` tracks headers, not
flags) builds the software path, to tell a blitter problem from a
layout one. No app sets it: `gpudemo`, which draws no text and links no
font data, did until it got section GC, because without GC the hardware
path's table of resident fonts reached the link and failed it.

## Integer-only printf (ZFMT)

`ZFMT = 1` in an app's Makefile replaces newlib's `printf` family with
`sw/common/zfmt.c`. It is **off by default** and set by the three core
apps in the flash archive whose size was the problem (`wm`, `net`,
`term`) and by the kernel ([kernel.md](kernel.md#the-256kb-image-budget)).

### Why

newlib's `printf` can print a `double`, so it links a formatting engine,
the double-to-decimal conversion, the multi-precision helpers and
libgcc's soft-float arithmetic whether or not the program ever prints
one. `snprintf` links a second engine. Measured in the core apps, that
was about half of each binary:

| | libc total | of which formatting and float | after `ZFMT = 1` |
|---|---:|---:|---:|
| `wm` | 76.7KB | 59.0KB | 17.6KB |
| `term` | 78.1KB | 59.0KB | 19.0KB |
| `net` | 136.8KB | 109.4KB | 19.1KB (with `net`'s own `sscanf` gone too) |

What is left is `malloc`, the string functions and newlib's FILE layer,
none of them worth replacing. `console` and `cron` call no `printf` at
all and have nothing to gain.

| | before | after |
|---|---:|---:|
| `wm` | 162,144 | 105,624 |
| `term` | 137,140 | 83,828 |
| `net` | 246,816 | 131,220 |
| the core-app archive (589,824 bytes) | 578,356 | 353,236 |

The same bytes are RAM while the apps run, which matters most on the
1MB boards.

### What `zfmt.c` is

One formatter, about 1.5KB, providing `printf`, `vprintf`, `fprintf`,
`vfprintf`, `sprintf`, `vsprintf`, `snprintf` and `vsnprintf`. It does
C99's integer, character, string and pointer conversions (`%d %i %u %o
%x %X %c %s %p %n %%`), every flag, width and precision (both also as
`*`), and every length modifier (`hh h l ll j z t`). `%p` is `0x` and
hex digits, `0x0` for NULL, as newlib prints it. A value that fits in 32
bits is divided in 32 bits, so libgcc's 64-bit division is linked only
by a binary that really prints a 64-bit number.

**Only the formatting is replaced.** Output to a `FILE` is formatted
into a 64-byte staging buffer and handed to `fwrite()` on that `FILE`,
so newlib's stdio still does everything it did:

- **stdout stays line-buffered** (`_isatty()` says it is a terminal), so
  output from `printf`, `puts` and `putchar` comes out in the order it
  was written. The core apps and the kernel mix them (`wm` has 8
  `puts`, the kernel 9 `puts` and 14 `fflush`), and a formatter that
  wrote straight to `_write()` would overtake whatever was still
  buffered.
- **`fflush()` means what it meant**: the kernel's prompts and progress
  lines without a newline still appear at once.
- **`z_stdout_hook`** (`zeitlos.h`, `repl` redirecting its output into a
  `term` window) still receives whole lines, not every fragment.
- **stderr** stays unbuffered and on the UART.

The FILE layer costs 5-6KB per binary, a tenth of what the formatting
engines did. The string forms (`snprintf` and the rest) never touch it.

### The one trap

**`%f`, `%e`, `%g` and `%a` print `?`.** They still consume their
`double`, so every argument after them is read correctly, and `?` is
visible where newlib's integer engines print nothing at all. But the
compiler's `-Wformat` accepts them, so nothing at build time says a
binary with `ZFMT = 1` has one. Before setting it, check every source
file the app links -- the list is in its `.map` -- for float
conversions:

```
grep -nE '"[^"]*%[-+ #0]*([0-9]+|\*)?(\.([0-9]+|\*))?[lL]?[fFeEgGaA]' <the .c files>
```

For the three core apps the only hit was `z_obj_print()`'s `%.6g` in
`sw/common/zobj.c`. With `ZFMT = 1`, `app.mk` defines `Z_ZFMT` and
`zobj.c` prints a float with integer arithmetic instead: sign, integer
part and six decimals from the IEEE-754 bits, trailing zeros dropped
(`1.5`, `-0.333333`), `big` from 2^31 up. It is a debug printer: close to
`%.6g`, not identical. Every other app still prints `%.6g` as before.

Apps that do print floating point, and must not set `ZFMT`: `ask`,
`audiotest`, `blackjack`, `midi`, `play`, `roulette`, `track`, `web`.

### Testing

```
cc -std=gnu99 -Wall -DZFMT_HOST_TEST -I sw/common \
   -o /tmp/test_zfmt sw/common/tests/test_zfmt.c sw/common/zfmt.c \
   && /tmp/test_zfmt
```

`ZFMT_HOST_TEST` builds `zfmt.c` with its names prefixed (`zf_printf`
and so on), so the build machine's own `printf` is still there to
compare against. Every integer conversion is compared **exhaustively**
-- every combination of the five flags, several widths and precisions,
every length modifier and every conversion, over values at each edge
(0, +-1, the limits of every width, 64-bit values): 958,464 cases, then
strings and characters, `*` (negative too), `%n`, `%p`, floats being
consumed, truncation and return values, and output to a line-buffered
`FILE` interleaved with `fputs` and `putc`, which must come out in
order. 958,571 checks; it also runs clean under AddressSanitizer and
UBSan.

### Opting another app in

Set `ZFMT = 1` above the `include`, check its sources as above, `make
clean` (`-MD` tracks headers, not flags), and build. The map should then
have no `vfprintf`, `svfprintf`, `vfiprintf`, `svfiprintf`, `dtoa` or
`mprec` in it.

## Section GC

With `GC_SECTIONS=1`, every function and data object gets its own
section and the linker drops those nothing references. That matters
here because each app links whole `sw/common` objects for the sake of
part of them.

Every app links with it. Eleven did not until September 2026 -- that
was their behaviour before `app.mk`, and the conversion was required to
be byte-identical -- and turning it on for them was its own change:

| App | Without GC | With GC | Saved |
|---|---:|---:|---:|
| `audiotest` | 96,400 | 71,524 | 24,876 (25%) |
| `chip8` | 186,572 | 149,960 | 36,612 (19%) |
| `gamedemo` | 154,868 | 104,336 | 50,532 (32%) |
| `gpudemo` | 112,796 | 75,620 | 37,176 (32%) |
| `hello_win` | 139,876 | 92,052 | 47,824 (34%) |
| `mmod` | 182,488 | 127,236 | 55,252 (30%) |
| `portdemo` | 92,296 | 67,428 | 24,868 (26%) |
| `serial` | 112,784 | 88,396 | 24,388 (21%) |
| `space3d` | 146,000 | 102,448 | 43,552 (29%) |
| `track` | 170,196 | 116,616 | 53,580 (31%) |
| `ttytest` | 108,704 | 86,524 | 22,180 (20%) |

**420,840 bytes** across the eleven: read off the card at every launch,
and resident while the app runs. The other 45 binaries were unchanged
by it. `gpudemo` also lost its exception to the hardware glyph path
(above), which it had needed only because nothing was collecting the
font table it never used.

Nothing in these apps is reached only from assembly or from the linker
script, which is where GC can drop something still needed; the script
KEEPs `.init`, `.fini` and the constructor arrays. `GC_SECTIONS=0`
still works, for comparing.

## Checks that run before anything is compiled

`make -C sw/apps` runs `tools/check_subjects.py` first
(`make -C sw/apps check-subjects` on its own). It fails the build if any
message subject in `sw/` is not defined through a block in
`sw/common/zsubjects.h` -- the one gap in that registry the compiler
cannot see; [messaging.md](messaging.md#subjects-and-tags) has why. It
takes about a second and needs only `python3`, which the build already
does for `mkexec.py`.

## Verifying a change to the build

A change to `app.mk` or to an app's Makefile that is not meant to
change the program should leave every binary byte-identical:

```
$ export SOURCE_DATE_EPOCH=1790000000   # tts and automate embed __DATE__
$ make -C sw/apps clean && make -C sw/apps
$ # copy every sw/apps/**/*.bin aside, make the change, rebuild, then:
$ cmp old/foo.bin sw/apps/foo/foo.bin
```

`SOURCE_DATE_EPOCH` fixes the two `__DATE__`/`__TIME__` stamps, which
would otherwise differ on every build. The conversion to `app.mk` was
checked this way: all 55 binaries, including `libz.bin`, identical.

## Adding an app

1. Create `sw/apps/<name>/` with `<name>.c` and a Makefile shaped like
   the one at the top of this page.
2. Add `<name>` to `APPS` in `sw/apps/Makefile`.
3. To ship it on the sdcard image, add a `("apps/<name>", "sw/apps/<name>/<name>.bin")`
   entry to the file list in `release/lib/mkfatimg.py` (see
   [releases.md](releases.md)).
4. Document it in `docs/<name>_app.md`, list it in `docs/readme.md`, and
   give it a row, linked to that doc, in the Apps table of `README.md`.
5. If it is to be a core app in the flash archive, consider `ZFMT = 1`
   (above).
