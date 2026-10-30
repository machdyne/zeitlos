# Core Apps in Flash

The core apps -- the drivers `fs` and `usb`, then `wm`, `net`, `term`,
`console`, `cron`, `text`, `files` and `repl` -- are programmed into flash alongside the kernel and are
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

`cron` ([cron.md](cron.md)) is one because its jobs can live in the
flash key/value store, so a board with no card can still run them; it
is small, about 22KB.

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
  +0x000000  kernel, 160KB (256KB until the filesystem left it, 224KB
             until the USB stack did and the image dropped its .bss)
  +0x028000  core apps  <-- this, up to 848 KB
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

The stored apps are ZEXE files (`sw/common/zexec.h`), **compressed**:
`tools/mkzar.py` packs each app's image as an LZ4 block and sets the
header's bit 8, and the kernel unpacks it into the process's memory as
it loads
([executables.md](executables.md#compressed-executables)). The format
is still ZAR2 -- an entry is one file, whatever is in it -- and still
one executable format, with a flag. `CORE_STORE` in the `Makefile`
names apps to keep verbatim (`wm` and `net`). Files in flash are stored as they
are.

`Z_ZAR_FLASH_OFFSET` in `sw/os/zar.h` and the offset in `Makefile`'s
`flash_sw` image must agree. A mismatch presents as "no core apps in
flash" rather than an error (`release/lib/layout.py` checks).

**The kernel and the archive are written as one image** (`flash_sw`;
`flash_os` and `flash_apps` are names for it): `kernel.bin` padded with
0xFF to +0x028000, then `apps.zar`. +0x028000 is not on a 64KB
boundary, and openFPGALoader erases 64KB blocks on a flash it does not
know to have 4KB sectors. Written separately, the archive's erase took
the end of the kernel, and the kernel's took the start of the archive
-- whichever went second won: a kernel that would not boot, or "no fs in
flash". With a padded kernel that was harmless, because what it lost
there was `.bss`.

## Files in flash

The archive also carries **files**: `CORE_FILES` in the top-level
`Makefile`: `docs/welcome.txt`, `docs/repl.txt` (the beginner's guide
to Scheme, [repl.md](repl.md)), `docs/zapi.txt` (every procedure repl
has, what its `help` and `apropos` read, [zapi.md](zapi.md)) and
`data/repl/examples/todo.scm`, its example
app, and the dock's two files, `data/wm/wm.cfg` and `data/wm/dock.zic`
([dock.md](dock.md)). Each is read-only, at its path
under the card, with or without a card, and a file of the same name on
the card wins over it -- the same rule as for apps:

```
CORE_FILES = docs/welcome.txt docs/repl.txt docs/zapi.txt data/repl/examples/todo.scm \
	data/wm/wm.cfg data/wm/dock.zic
```

The dock's files are how a card changes the dock: a `/data/wm/wm.cfg`
on the card wins over the one in flash, whole.

A `docs/<name>.txt` is `docs/<name>.md` rendered as plain text by
`tools/md2txt.py`, for `text` on a machine with no card (and so no
`read`): one line per paragraph or list item, since `text` wraps them,
headings underlined, tables as aligned columns, code blocks indented,
and the Markdown that only a renderer needs dropped. The card keeps
`welcome.md`; flash shows `welcome.txt` beside it.

`docs/zapi.txt` is the exception: no `.md` makes it. `tools/gen_zapi.py`
makes it from the entries beside repl's code, so it always lists what
the repl in the same archive has ([scheme_api.md](scheme_api.md)).

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
kernel side cost about 3.5 KB of its 256 KB (224 KB since Phase 2b) (`zar.c`, and the
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
  the dock's `wm.cfg`, the extension table in `ztype.c`,
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

**Flash.** The archive's region is 868,352 bytes (`0x128000` to
`0x1FC000` on ECP5). It was 802,816 from `0x138000` while the kernel's
region was 224KB ([fs_driver.md](fs_driver.md)), 770,048 from
`0x140000` before that, and 589,824 before the jumploader moved out of
it ([zboot.md](zboot.md) section 5). With `usb`, after compression
(October 2026), built here (the board's toolchain gives sizes a few
percent either way):

| app | `.bin` | in flash (compressed) | in RAM (image) | stack |
|---|---:|---:|---:|---:|
| `fs` | 45,204 | 33,384 | 75,848 | 64K |
| `usb` | 44,560 | 32,439 | 55,396 | 16K |
| `wm` | 108,772 | stored: 108,772 (70,900) | 136,368 | 8K |
| `net` | 144,644 | stored: 144,644 (97,329) | 290,580 | 32K |
| `term` | 87,420 | 59,386 | 135,572 | 8K |
| `console` | 11,112 | 8,301 | 12,488 | 16K |
| `cron` | 21,648 | 16,118 | 35,008 | 16K |
| `text` | 85,492 | 57,712 | 155,520 | 16K |
| `files` | 66,252 | 45,820 | 91,904 | 16K |
| `repl` | 196,792 | 117,968 | 310,880 | 64K |
| files in flash (welcome.txt, repl.txt, zapi.txt, todo.scm) | | 46,958 | | |
| **archive** | | **671,880** | | |

Compression (`mkzar.py`, LZ4, [executables.md](executables.md#compressed-executables))
saves about 187,000 bytes: every
app but `wm` and `net`, which are kept as they are (`CORE_STORE`) for
the time it costs to unpack them at boot (below; in brackets, what they
would take compressed). `repl` alone saves 79KB of it: Scheme's
strings and tables compress well. The budgets sum to 667KB, with the
reserve 48KB more: about 133KB of the region is unassigned. The archive
took about 823,000 bytes before, with budgets that left 1.8KB.

`repl` needs its submodules (`git submodule update --init sw/ext/ms
sw/ext/te sw/ext/basic`); without them it does not build, and these
figures were an estimate until October 2026. Its flash budget (140K)
now has 22KB to spare; its RAM budget (304K) has 416 bytes.

**The page gap.** Each app's `.bin` used to carry 0-4KB of zeros
between `.rodata` and `.data`, because the app linker script started
the data segment on a page boundary. `sw/common/app.mk` now links with
the 16-byte page the kernel already used (`SEG_PAGE_LDFLAGS`), which
took about 20KB out of the archive. It came to light when 1.5KB more
code made `fs.bin` 4KB longer.

**Flash is no longer tight; RAM is.** Since compression the flash
budgets and the reserve add up to 715KB of the 848KB region, each the
app's compressed size plus a few KB (about 4KB of code, at the ratio).
Before, they left about 1.8KB of it. The RAM budgets are a few KB over each image
too, so that the boot set with `usb` in it still meets the RAM rule
below (1,024 of 1,024 KB). `net`'s TCP send window took 2K of `wm`'s
RAM slack (`net` 280K, `wm` 133K). The NFS client ([nfs.md](nfs.md))
cost `net` about 8KB of flash and 14KB of RAM; it paid for itself in
flash and most of the RAM by building `net` with `ZFMT_LITE` (stdout
without newlib's stdio, [build.md](build.md)), about 8KB of each, and
`text`'s paste buffer moved from `.bss` to the stack (4KB), so `net`
went to 285K of RAM and `text` came down to 154K. The reboot dialog
(Ctrl+Alt+Del) took `wm` to 134K: 1,022 of 1,024 KB. The F1 help box
([app_help.md](app_help.md)) took `text` to 157K and `files`' flash to
49K; the RAM came from the slack in `usb` (55K) and `console` (13K):
1,023 of 1,024 KB, and 716KB of flash. Find and copying, moving and
deleting whole folders ([zwalk.md](zwalk.md)) took `files` to 57K of
flash and 120K of RAM -- outside the RAM rule, which counts the two
largest (`repl`, `text`). The dock icons and spoken names of the
arcade games ([arcade.md](arcade.md): `snake`, `blocks`, `bricks`, about
172 bytes each) took `wm` to 135K (137,100 bytes of 138,240 with all three), the last
kilobyte: 1,024 of 1,024 KB.
The files in flash (`CORE_FILES`) passed the 48K reserve in round 9:
the Scheme reference, `docs/zapi.txt`, reached 26,429 bytes with the
procedures added then, and the four files 49,241 of 49,152. The reserve
went to 56K, from the unassigned part of the region (676KB of budgets
plus the reserve is now 732KB of 848KB).
The dock's icons and its arrangement left `wm` for two files in flash
([dock.md](dock.md)): 8KB more of files (6,856 + 1,212 bytes), so the
reserve went to 60K, paid from `wm`'s flash budget, 110K to 104K
(`wm` itself went from 111KB to 96KB, mostly by `ZFMT_LITE`). The total
stays 732KB of 848KB.
The dock on any side of the screen ([dock.md](dock.md#sides)) took
`wm` past 135K of RAM (139,020 bytes): its budget went to 136K, paid
from `net`'s, 285K to 284K, which `net` (290,580 bytes) still fits. The
boot set's RAM stays 1,024 of 1,024 KB.
The pointer's shapes ([cursor.md](cursor.md): the large pair read
from the card, Super+M) took `wm` to 140,240 bytes: its budget went to
137K, paid from `text`'s, 157K to 156K (`text` is 159,588 bytes) --
`text` was then one of the two largest apps the RAM rule counted beside
the boot set (the rule names `term` and `repl` now), so the rule stayed
1,024 of 1,024 KB. `term`'s 16K of room
would not have done: it is in neither.
Game-mode colour's second VRAM sheet ([color.md](color.md), "More than
four planes") took it to 140,276 -- `zgfx`'s fills and blits can be told
to reach below row 479, a variable and one store -- 12 bytes inside.
pix's icons in the dock (`icons.zic`, [dock.md](dock.md)) and the
pointer kept for a game that asks took `wm` to 140,848: its budget went
to 138K, paid from `usb`'s, 55K to 54K -- `usb` now builds with
`ZFMT_NO64` (it prints no 64-bit number), which took libgcc's 64-bit
division out of it, 55,396 to 52,580. The boot set's RAM stays 1,024
of 1,024 KB.
The same change took `wm` (stored, so its flash is its `.bin`) to
106,524 bytes with the release toolchain, 28 over its 104K of flash:
that budget went to 105K, from the region's unassigned room (671KB of
budgets plus the 60K reserve is 731KB of 848KB).
In flash, `wm` (stored, so its flash is its `.bin`) passed its 104K by
28 bytes, at 106,524: the flash budget went to 105K, from the region's
unassigned part, which the flash rule still has room for.
The colour window, the hidden pointer over games and the colour
titlebar icon ([color.md](color.md), [app_kinds.md](app_kinds.md)) took
it to 106,792 with xPack GCC 15.2 and about 600 more with the
riscv32-unknown-elf toolchain: its flash budget went to 108K, again
from the unassigned part (now 734KB of budgets and reserve of 848KB).
Its RAM budget stays 138K: the boot set's RAM is still 1,024 of
1,024 KB.
Desktop colour ([color.md](color.md#desktop-colour): wm keeping the
accent plane, the palette per phosphor, the hand-over with a colour
window) took `wm` to 112,824 bytes in flash and 147,024 in RAM with
xPack GCC 15.2: its budgets went to 112K (from the unassigned part) and
145K. The widgets' tints (`z_win_accent_rect()`) took `text` to 160,448:
157K. `usb` gave 2K (52,580 in 52K). The themes (the table, the
custom-colour parser and three config keys) took `wm` to 115,364 and
149,580 with xPack GCC 15.2 and 116,048 in flash with the
riscv32-unknown-elf toolchain: 114K (from the unassigned part) and 147K. **The boot set's RAM is now 1,032 of 1,024 KB**:
`zrelease check` reports it until 8K comes out of something, or the
rule's 1M is raised -- not yet decided.
The responsiveness round ([responsive.md](responsive.md), October
2026) took `fs` to 77,040 bytes of RAM (its drive states for `backup`:
starting, not usable, tried again) and `usb` to 53,316 (bringing a drive
up without resetting it at every step): 77,056 and 53,504, both paid
from `console` (12,488 in 13K, now 12,544), so the boot set went *down*
256 bytes. The file list drawing in place instead of flashing, with
type-to-find and copy progress, took `files` to 61,006 in flash and
127,168 in RAM (60K/125K, outside the RAM rule) and `text`, which links
the list for its dialogs, to 61,526 and 162,352 (61K/159K).
**The RAM rule changed then**: it counted the two largest apps beside
the boot set, which made every byte of `text` (one of them) a byte the
boot set could not have, though no board needs `text` beside the boot
set more than any other app. It now counts the apps named in
`CORE_RAM_WITH`, the ones that must be able to run beside the boot set
on the smallest board: `term` and `repl`. `term`'s RAM budget came down
to what it uses (137,504 bytes in 140K, from 152K) and `repl`'s went to
307K: xPack GCC 15.2 puts it at 313,296, which was 2,000 over 304K and
failed `make BOARD=lakritz output/lakritz/apps.zar` on its own. The rule
is 1,018.25 of 1,024 KB. `text` -- its large copy kept whole, undo in
the arena, saving through a `.part` -- went to 64K/165K, which is now
its own business. Printing (the print dialog and zprint) then took
`text` to 80K/200K, and reading folders of any size took `files` to
64K/134K; neither is in the RAM rule, and both came from the slack
under `CORE_ZAR_MAX` (the archive is 735,536 bytes of 848K). The
`Makefile`'s comments above `CORE_BUDGETS` keep each change and why.
The next byte for the boot set comes out of another budget, or out of
something made smaller. `mkzar.py --max` checks the archive itself
against the region (`CORE_ZAR_MAX`, 848K), so a budget that turns out
too tight costs a bump, never a broken flash.

**Budgets.** Each core app has one: the most it may take in flash
(its `.bin` as stored, compressed) and in RAM (its image, unpacked:
code, data and `.bss`, not its stack). They are `CORE_BUDGETS` in the
top-level `Makefile`, next to `CORE_APPS`:

```
CORE_BUDGETS = fs=36K/77056 usb=35K/53504 wm=114K/147K net=148K/284K term=62K/140K \
	console=10K/12544 cron=18K/48K text=80K/200K files=64K/134K repl=140K/307K
CORE_RESERVE = 60K
CORE_STORE = wm net
CORE_ZAR_MAX = 848K
CORE_AT_BOOT = fs usb wm net console
CORE_RAM_WITH = term repl
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
- **RAM:** the apps that start at boot (`CORE_AT_BOOT`) plus those
  that must be able to run beside them (`CORE_RAM_WITH`: `term` and
  `repl`) fit in `CORE_RAM_MAX`, the smallest board. Every other core
  app's RAM budget counts only for that app: it says when the app grew.

`make flash_apps` prints each app against its budget, and `(stored)`
by one kept uncompressed. `zrelease check` also refuses a `CORE_STORE`
name that is not a core app.

**Load times.** Unpacking costs CPU at every start, where a stored app
is a copy. `init` prints how long each core app took (`init: usb loaded
in 127 ms (125 ms unpacking, 125 ms of it on the CPU)`), and `run`
the same; an app that starts too slowly for its job goes into
`CORE_STORE` (its flash budget is then its `.bin` again).

Measured on hardware, boot, LZ4 with its fast path: `fs` (45KB) 129ms,
`usb` (45KB) 125ms, `wm` (109KB) 508ms unpacking (452ms on the CPU),
`net` (145KB) 933ms (550ms on the CPU -- `wm` was running by then).
About 2.8us a byte where nothing else runs, on a CPU that manages about
7 million instructions a second; a plain copy from flash is about 0.8us
a byte. `wm` and `net` went into `CORE_STORE`: they are what boot waits
for. DEFLATE, first, had loaded `wm` in 1,251ms. A version of the LZ4
decoder that wrote the image a word at a time (four times fewer stores,
40% more instructions) came out even in the simulator's counts, and
was not kept.

**Next: documents in flash.** With the room, `read` is to become a core
app, and the archive's `.txt` files (`welcome.txt`, `repl.txt`, from
`md2txt.py`) are to be replaced by the Markdown itself -- `welcome.md`,
`repl.md` and a few more -- for `read` to show. After compression has
been tested on hardware.

**RAM.** The core apps do not all run at once. Boot starts `fs` first
(the filesystem, [fs_driver.md](fs_driver.md)), then `usb` (the USB
host, [usb_driver.md](usb_driver.md)); `init()` starts `wm`,
`net`, `console` (and `cron` when it has a job list); the rest are
started on demand. The rule is that **one or two more core apps must
fit beside those**, on the 1MB boards included -- so a new core app is
judged by its RAM image as much as by its flash size. [boot.md](boot.md),
"Memory budget", has what a 1MB board holds.

**Past the archive.** The drivers are core apps, and core apps can
come from the card: a copy in `/apps` wins over flash for everything
but `fs`. So a driver is not bounded by this archive the way it was
bounded by the kernel's region. The flash copy can be the small one
that brings the machine up, and the card's the full one.
[usb_driver.md](usb_driver.md), "Drivers past the 1MB flash region".

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
- **RAM.** Its image is 310,880 bytes, plus its 64K stack. That is RAM
  only while it runs; the budgets' RAM rule ("Budgets") counts it as
  one of the one or two apps started beside the boot set.
- **Its budget** is 140K of flash (117,968 used) and 304K of RAM
  (311,296: 416 bytes to spare). `repl` grows when
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

**Without the clean:** `make BOARD=obst flash_sw` (or `flash_os`, or
`flash_apps` -- the same) rebuilds the kernel and the core apps
incrementally, only what changed, then builds the archive and flashes
the kernel and archive as one image. It builds only the apps that go
into the archive (`core-bins`), not every app. Use it to reflash after
a flash that failed, or after a small change; `dev-flash` cleans
first, so a change to a shared header or `syscalls.def` reaches every
app. (A changed `syscalls.def` or other header in `sw/common` is the
case for `dev-flash`: per-app dependency files catch most of it, not
all.)

If anything under `rtl/` changed, use `make flash` instead.
