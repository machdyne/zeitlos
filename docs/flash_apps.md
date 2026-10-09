# Core Apps in Flash

The core apps -- `wm`, `net`, `term`, `console`, `cron`, `text` and
`files` -- are programmed into flash alongside the kernel and are
available with no sdcard attached. The list is `CORE_APPS` in the
top-level `Makefile`. `console` is the kernel console as a port (`docs/console.md`):
it is a core app because the machines that most need it, with no card
and no serial cable, are exactly the ones with nothing else.

`text` ([text_editor.md](text_editor.md)) and `files`
([file_browser.md](file_browser.md)) are core apps so that a board with
nothing but flash is a usable computer, not just a desktop: something
to write with, and something to find what has been written in `/ram`
or on a USB stick. `text` is built with `ZFMT = 1` for it (integer-only
printf, [build.md](build.md#integer-only-printf-zfmt)), which took it
from 145,600 bytes to 88,880 in flash, and from 215,608 to 162,992 in
RAM.

`cron` ([cron.md](cron.md)) is one for the same reason as `console`:
it is small -- about 22KB -- and a scheduled job should not depend on
anything but the card it reads its list from.

`repl` is a core app too: Scheme, and `te`, with no card at all. The
other shell, `posix`, is **not**: it lives on the sdcard. `term` starts
either when its REPL or POSIX button is pressed
([terminal.md](terminal.md), "Starting the shells"); `init()` starts
neither, and only looks for `posix`, which is what wakes a freshly
powered card. See "Why repl is a core app" below.

## Why

Two problems, and the second is the bigger one.

**Getting started.** A freshly flashed board used to boot to a serial
shell and nothing else, because every app lived on the sdcard. "Flash
the board and you have a desktop" is a far better first five minutes
than "flash the board, now go write an sdcard".

**Iterating.** Updating the core apps meant hand-driving `xf` in minicom
four times, about a minute of interactive work per cycle, with the
terminal tied up throughout. `make dev-flash` is now unattended and
takes seconds.

## Why flash is a good place for this

Flash is memory-mapped on this SOC, which is what makes the whole thing
cheap: loading an app from it is a copy, with no filesystem and no
SPI driver involved. It is also **faster than the sdcard**, which is
bit-banged SPI (`sw/os/fs/fatfs/sdmm.c`).

This is the second use of that same property. The BIOS already loads the
kernel this way (`load_zeitlos()`, `sw/bios/bios.c`).

Writing flash is slow, but that happens once per build, unattended, as
part of `make flash`.

**The copy reads a 32-bit word at a time** (`sw/os/zarcopy.h`). The
flash controller serves every read as one SPI transaction of 32 bits,
whatever width was asked for, so the byte-by-byte copy it replaced made
four transactions where one would do -- 235,000 of them for `net`.
Only an unaligned first or last few bytes are read singly.
`sw/os/tests/test_zarcopy.c` checks it against `memcpy` for every
alignment of source and destination.

## Layout

```
0x10000000   MEM_ROM base
             the Zeitlos region (docs/boot.md), at its base:
  +0x000000  kernel, 256KB
  +0x040000  core apps  <-- this, up to 752 KB (746,048 bytes used, Oct 2026)
  +0x0FC000  the flashtest sector
  +0x0FE000  the key/value store
             -- add 0x100000 for an ECP5 board, 0x300000 for an Artix-7 one
0x1F000000   the flash controller's registers (docs/spiflash.md)
```

While any program holds a flash **write** session (`docs/spiflash.md`),
core apps are not launched: `zar.c` refuses, because the archive may be
half-rewritten. Apps already running are in SDRAM and are unaffected.

The archive format is deliberately minimal:

```
offset  size  field
0       4     magic     "ZAR2"
4       4     count     number of entries, apps and files
8       8     reserved  must be 0
16      ...   entries[count], 16 bytes each:
                0   4   name    offset of its NUL-terminated name
                4   4   flags   bit 0: a file, not an app
                8   4   offset  of the data, from the start of the archive
                12  4   size    bytes (an app: the whole ZEXE file)
...           the names, then the data, each 4-byte aligned
```

The stored files are ZEXE (`sw/common/zexec.h`) **verbatim**, exactly as
each app's own Makefile produced them. `tools/mkzar.py` concatenates
them without re-encoding, so there is one executable format to keep
working rather than two.

`Z_ZAR_FLASH_OFFSET` in `sw/os/zar.h` and the offset in `Makefile`'s
`flash_apps` target must agree. Nothing checks that they do, and a
mismatch presents as "no core apps in flash" rather than an error.

## Files in flash

The archive also carries **files**: `CORE_FILES` in the top-level
`Makefile`: `docs/welcome.txt`, `docs/repl.txt` (the beginner's guide
to Scheme, [repl.md](repl.md)) and `data/repl/examples/todo.scm`, its example
app. Each is read-only, at its path
under the card, with or without a card, and a file of the same name on
the card wins over it -- the same rule as for apps:

```
CORE_FILES = docs/welcome.txt docs/repl.txt data/repl/examples/todo.scm
```

A `docs/<name>.txt` is `docs/<name>.md` rendered as plain text by
`tools/md2txt.py`, for `text` on a machine with no card (and so no
`read`): one line per paragraph or list item, since `text` wraps them,
headings underlined, tables as aligned columns, code blocks indented,
and the Markdown that only a renderer needs dropped. The card keeps
`welcome.md`; flash shows `welcome.txt` beside it.

**What sees them.** The kernel's file calls fall through to flash when
the card has no such file, or there is no card, and the path is not
under `/ram` or `/usb`:

- **open for reading** (`FS_OPEN_READ`, and `FS_READ` of a whole file):
  a handle that reads from flash, with seek;
- **size and stat**: a file is read-only (`Z_FS_ATTR_RDONLY`); a
  directory that is only in flash ("docs" with no card) is a directory;
- **directory listings** (`FS_LIST`, `FS_LIST_EX`): what is in flash
  under that directory and not on the card, after the card's own
  entries -- so with no card, `/` lists `apps`, `docs` and the mounted
  `/ram` and `/usb`, and `files` shows all of it;
- **the kernel shell's `ls`**, in its "in flash:" section.

Writing is the card's business: with no card a write fails; with one,
it creates the file there, which then shadows the flash copy. Deleting
a flash file fails.

**Apps are files too.** Each app in the archive is listed as
`/apps/<name>` (its ZEXE file, read-only), and launching that path runs
it -- so `files` shows the apps in flash in `/apps` and opens them as it
opens any program.

**The format** is ZAR2 (`sw/os/zar.h`): names in a string table,
NUL-terminated, up to 127 characters, and a flag that says file or app.
The kernel, `tools/mkzar.py`, zfpga's `boot.c` (which still reads a
ZAR1 when it finds one) and the release check read it. A kernel and its
archive are flashed together; a ZAR1 archive under a ZAR2 kernel is
"no archive", and only a card's apps run.

**Size.** The files come out of `CORE_RESERVE`, all together, and
`mkzar.py --files-max` refuses an archive where they do not fit. The
kernel side cost about 3.5 KB of its 256 KB (`zar.c`, and the
fall-throughs in `fsapi.c` and `fs/fs.c`).

**Testing.** `sw/os/tests/test_zar.c` runs `zar.c` against an archive
`mkzar.py` built:

```
python3 tools/mkzar.py /tmp/t.zar wm=sw/apps/wm/wm.bin net=README.md \
    docs/welcome.txt=docs/welcome.md docs/repl.txt=docs/boot.md \
    help/a/b.txt=LICENSE.md
cc -std=gnu99 -Wall -DZAR_HOST_TEST -I sw/os -o /tmp/test_zar \
    sw/os/tests/test_zar.c sw/os/zar.c && /tmp/test_zar /tmp/t.zar
```

## An underlay, not a filesystem (for apps)

The resolution rule is one line:

> if the filesystem has it, use that; otherwise use the flash copy.

`fs_exec_info_any()` and `fs_load_exec_any()` (`sw/os/fs/fs.c`) are the
single place that decides. Every path that starts a process goes
through them: `sh.c`'s `run`, `sh.c`'s `init`, and `k_proc_run()` --
which is what wm's dock calls, and what lets `term` launch on a
card-less board.

There is still exactly one name for `term`. `run term` behaves
identically whether it came from a card, from flash, with no card at
all, or after being killed and restarted.

### The search path

The card keeps its programs in `/apps`, one file each
([layout.md](layout.md)). `fs_exec_resolve()` is what knows that. For a
**bare name** it tries, in order:

1. `/apps/term`
2. the flash archive, under the bare name

A name **containing `/`** is taken literally and not searched at all,
so `docs/term` cannot resolve to `/apps/term`.

Typed at a shell, a bare name is first looked for in the shell's
working directory -- the kernel shell's is the root, so `run wm` runs a
`/wm` fetched with `tget` -- and only then searched as above. Boot, the
dock and apps launching apps do not do that, so `init` always takes
`wm` from `/apps` or from flash. See [layout.md](layout.md), "Finding a
program".

Two consequences worth stating plainly:

- **Nothing above this function changed when the apps moved.**
  `dock_candidates[]` in wm, the extension table in `ztype.c`,
  pidreg registrations and
  `run term` at the shell all still use bare names. The alternative --
  writing `/apps/` at every one of those call sites -- would have put a
  constant prefix in a dozen places where it carries no information,
  and spent 6 of `Z_ZAR_NAME_MAX`'s 16 bytes on it in the archive too.
  A prefix repeated everywhere belongs in the resolver instead.

- **The card root is not searched.** Before v0.0.6 it was, first, so
  that a file dropped there shadowed an installed app. The shadowing
  rule above survives with one place fewer to look: a release card
  carries no core apps, so the only way one reaches `/apps` is somebody
  deliberately putting it there, and `xf apps/wm` hot-swaps `wm` during
  development. Deleting it goes back to the flash copy. The root stays
  clean, and a launch costs one `f_open` rather than two.

This is not the drive-letter idea below under another name. It applies
to **executable resolution only** -- files are still opened by exact
path, and `fs_open`/`size`/`read`/`write`, `ls`, `te`, repl's file API
and `tget`/`tput` are all untouched. Data is addressed; programs are
resolved.

### Why not drive letters

Drive letters (`A:` = flash, `B:` = sdcard) were considered and
rejected. They are a *namespace* solution to what is actually a
*fallback* question, and the cost lands everywhere:

- Every path-taking API would have to learn about drives:
  `fs_open`/`size`/`read`/`write`, `ls`, `te`, repl's Scheme file API,
  `tget`/`tput`.
- Flash is read-only, so writes to `A:` need a new failure path in each
  of them.
- Worst, callers like wm's dock would have to know *which drive* an app
  lives on -- precisely the thing they should not have to care about.
  Adding default-drive rules to avoid that just reintroduces the
  ambiguity drives were meant to remove.

If explicit selection is ever genuinely needed, a `flash:term` prefix
handled inside `fs_exec_info_any()` is a much smaller change than
teaching the whole filesystem API about drives. That becomes more
attractive now that Zeitlos has a second real storage device -- USB
mass storage at `/usb` (see `docs/usb_host.md`), with a network mount
a possible third -- which is when drives start earning their keep.

### Shadowing

A file on the card wins. That is deliberate and needs no version scheme,
timestamp comparison or precedence rules: the only way an app got onto
the card is somebody deliberately putting it there, so treating that as
intent is exactly right. It keeps `xf apps/wm` working as a single-app
hot-swap during development.

To make that visible rather than mysterious:

- `init` prints each app's source at boot (`init: wm (flash)`), and
  names every core app the card overrides, including the ones it does
  not start (`init: /apps/text on the card overrides the flash copy`).
- `run` prints it too (`loading term from flash`).
- `ls` and `ls /apps` list flash apps in a separate `in flash:`
  section, **skipping any shadowed by `/apps/<name>`**, so what it
  shows matches what `run` would actually launch.

## How much fits

Two budgets, and RAM is the one that binds.

**Flash.** The archive's region is 770,048 bytes (`0x140000` to
`0x1FC000` on ECP5) -- 589,824 until the jumploader moved out of it
([zboot.md](zboot.md) section 5). At the commit that added `text` and
`files`:

| app | in flash | in RAM (image) | stack |
|---|---:|---:|---:|
| `wm` | 110,084 | 133,556 | 8K |
| `net` | 136,768 | 267,592 | 32K |
| `term` | 91,044 | 135,100 | 8K |
| `console` | 13,688 | 12,481 | 16K |
| `cron` | 24,032 | 32,652 | 16K |
| `text` | 88,880 | 162,992 | 16K |
| `files` | 66,976 | 92,612 | 16K |
| `repl` | 194,524 | 308,608 | 64K |
| files in flash (welcome.txt, repl.txt, todo.scm) | 19,692 | | |
| **archive** | **746,048** | | |

24,000 bytes are left, of 770,048 (the region grew from 589,824 when
the jumploader moved out of it).

**Flash is nearly full, so the flash budgets are tight.** They add up,
with the reserve, to all but a few KB of the region, so there is no
slack to hand out: a few KB over each app's size is all there is
(console and files got 2 KB more after a different toolchain built
them about 1 KB bigger). The RAM budgets, which only have to meet the
RAM rule below, have 8-24 KB of headroom each. More flash room would
have to come from compressing the archive or making apps smaller.
`zrelease layout` and the release build refuse an archive that does
not fit.

**Budgets.** Each core app has one: the most it may take in flash
(its `.bin`) and in RAM (its image: code, data and `.bss`, not its
stack). They are `CORE_BUDGETS` in the top-level `Makefile`, next to
`CORE_APPS`:

```
CORE_BUDGETS = wm=112K/152K net=136K/288K term=90K/152K console=16K/24K \
	cron=24K/48K text=88K/176K files=68K/104K repl=192K/320K
CORE_RESERVE = 22K
CORE_AT_BOOT = wm net console
CORE_RAM_MAX = 1M
```

`tools/mkzar.py` refuses to build the archive -- `make flash_apps`, and
a release -- when an app is over either budget, or has none, and says
by how much. That is the point: growth past a budget is a decision, made
when the change is made, rather than an archive that one day no longer
fits. A budget is raised by taking from another app's, from the
reserve, or by making something smaller. `zrelease check` holds the
budgets themselves to two rules:

- **Flash:** all of them, plus `CORE_RESERVE` (room kept for what is
  not an app yet, such as files in flash), fit the archive's region.
- **RAM:** the apps `init()` starts (`CORE_AT_BOOT`) plus the two
  largest of the rest fit in `CORE_RAM_MAX` -- one or two apps on
  demand beside the boot set, on the smallest board.

`make flash_apps` prints each app against its budget.

**RAM.** The core apps do not all run at once. `init()` starts `wm`,
`net`, `console` (and `cron` when it has a job list); the rest are
started on demand. The rule is that **one or two more core apps must
fit beside those**, on the 1MB boards included -- so a new core app is
judged by its RAM image as much as by its flash size. [boot.md](boot.md),
"Memory budget", has what a 1MB board holds.

## Upgrading a card

A card written by a release from before an app became a core app still
has it in `/apps`, and that copy wins over the newer one in flash
(see "Shadowing"). `init()` says so for each core app the card
overrides:

```
init: /apps/text on the card overrides the flash copy
```

Deliberate during development; on an old card, delete the file or
write the new card image. The release notes say the same.

## Booting with no card

This is a first-class path, not a fallback that happens to work.

`sh.c`'s auto-init used to poll for up to `AUTOINIT_TIMEOUT_TICKS`
(~3s) waiting for a slow sdcard to become readable. On a board with no
card that answer will never change, so the flash case is checked first
and skips the poll entirely -- otherwise a card-less board would stall
for three seconds on every boot before the desktop appeared.

That check only decides *when* to call `init()`. `init()` still resolves
per app, so a card holding only `/apps/wm` still gets its `wm` from the
card and everything else from flash.

## Why repl is a core app

It was one from the day core apps existed until the shells were
reorganised, and then it left: the archive was 576KB and nearly full,
and `repl` cost ~220KB of it and a 368KB RAM block on every card-less
board, for a shell whose file commands had no card to act on.

It came back because the trade was the wrong way round. The 184KB the
jumploader held inside the archive's region went to the core apps
instead ([zboot.md](zboot.md) section 5); `repl` was slimmed by 49KB
with `ZFMT_FLOAT` ([build.md](build.md)); and a programming language on
a machine with nothing but flash is worth more to most people than
`jump`. `/ram` and a USB stick give its file commands somewhere to act
on without a card.

What that means now:

- **`init()`** does not start it. `term`'s REPL button does, on demand,
  as before, and finds it in flash.
- **RAM.** Its image is 308,608 bytes, plus its 64K stack. That is RAM
  only while it runs; the budgets' RAM rule ("Budgets") counts it as
  one of the one or two apps started beside the boot set.
- **Its budget** is 192K of flash and 320K of RAM. `repl` grows when
  Scheme procedures (zapi) are added, so this is the budget that will
  bite first: a new procedure is paid for with a smaller one, a
  reserve, or a decision to raise the budget, not found out when the
  archive no longer fits.
- **The release** no longer ships `repl` on the card image (`SHELLS` in
  `release/lib/mkfatimg.py` is `posix` alone), and refuses a card list
  that has it. A card from an older release still carries
  `/apps/repl`, which wins over flash; `init()` says so.

`posix` stays card-only: it hosts `zcc`, whose runtime lives in
`/data/zcc/libz` on the card, and its 4MB tier rules out the small
boards anyway.

## Adding or removing a core app

The list is `CORE_APPS` in the top-level `Makefile`, and only there.
`make flash_apps` builds the archive from it, and `release/zrelease`
asks the Makefile for it (`make core-apps`), so a board flashed by hand
and a release image carry the same apps. Board and target specs cannot
set one; the release tools refuse a spec that tries.

A board can leave apps out with `CORE_APPS_OMIT` in its Makefile block.
No board does, headless ones included: `wm` stays in flash on Klinge
for remote desktop, and `init` simply does not start it on a bitstream
without a display. If a board ever does omit something, the release
builds that board its own archive and ships it as
`zeitlos-<target>-apps.zar`; everyone else gets the full
`zeitlos-apps.zar`. (0.0.5 shipped with the list kept per board spec
and ONE archive built from whichever target sorted first -- Klinge, at
the time headless without `wm` -- so every 0.0.5 image lacked `wm` and
`term`. That is what this arrangement replaces.)

1. Add or remove it in `CORE_APPS` in `Makefile`.
2. The card image's list (`release/lib/mkfatimg.py`, the only one --
   `tools/mkfatimg.sh` calls it): take a new core app off it, since the
   release refuses a card that carries a core app; or, if one is
   leaving flash but should still exist, put it on.
3. Give it a budget in `CORE_BUDGETS` ("How much fits"), and run
   `zrelease check`: the budgets must still fit the archive's region and
   the RAM rule. An app that prints no floating point should set
   `ZFMT = 1` first, and one that does, `ZFMT_FLOAT = 1` as well.
4. Rebuild and reflash: `make BOARD=<board> flash_apps`.

The kernel accepts up to `Z_ZAR_MAX_ENTRIES` (32) entries. There is no
requirement that a core app also appear in wm's dock -- the dock should
only offer apps guaranteed to resolve, which in practice means core apps
(`gpu3d` was removed from it for exactly this reason: it only ever
exists on a card, so its icon was a dead button on most machines).

Note apps must emit ZEXE, not a raw `objcopy` binary. `mkzar.py` warns
if one does not; such a file still loads, but ships its `.bss` as
literal zeros and wastes flash.

## Development cycle

```
$ make clean && make BOARD=obst dev-flash
```

`dev-flash` rebuilds and reflashes the kernel and core apps without
touching the bitstream. Both are flashed together deliberately:
`sw/common/syscalls.def` is compiled into `kernel.bin` *and* into every
app, and a binary built against a different copy calls the wrong kernel
handler for every syscall past the point they diverge -- which is not a
crash, just quietly wrong behaviour. See that file's own warning.

If anything under `rtl/` changed, use `make flash` instead.
