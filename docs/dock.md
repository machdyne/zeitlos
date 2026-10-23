# The dock's files

The dock -- the row of icons at the bottom of the screen (or along any
other edge of it, "Sides" below)
([window_manager.md](window_manager.md#the-dock)) -- is drawn by `wm`, but
**what it shows is not in `wm`'s code**. Two files decide it:

| file | in the tree | what it says |
|---|---|---|
| `/data/wm/wm.cfg` | `sw/data/wm.cfg` | which apps the dock shows, and in what order |
| `/data/wm/dock.zic` | `sw/data/icons/dock.zic`, made from the PNGs and `apps.txt` beside it | each app's icon, its spoken name, and the hardware it needs |

Both are in the flash archive (`CORE_FILES`,
[flash_apps.md](flash_apps.md#files-in-flash)), so every machine has
them, with or without a card. A file at the same path **on the card
wins** over the one in flash, as for every file in flash. That is the
whole mechanism for changing the dock.

## Changing the dock

Make your own `/data/wm/wm.cfg` on the card. It **replaces the built-in
one whole**: nothing of the built-in file is kept, so start from a copy
of it. From posix, by way of `/tmp` (reading `/data/wm/wm.cfg` while the
card has none gives the copy in flash):

```
cp /data/wm/wm.cfg /tmp/wm.cfg
mkdir /data/wm                  # if the card has no such folder yet
cp /tmp/wm.cfg /data/wm/wm.cfg
vi /data/wm/wm.cfg
```

Then **reload the
configuration**: `cfg reload` at the serial console, or **Reload file**
in `settings`. The dock is rebuilt within a second of the next key,
click or pointer movement (`wm` looks no more than once a second, and
only when it is awake anyway, so an idle machine pays nothing for it).

To go back to the built-in dock, delete the card's `/data/wm/wm.cfg`
and reload.

Because your file replaces the built-in one, an app a later release
adds to the built-in file does not appear in yours -- unless your file
has `@rest`, which brings in every app with an icon that it does not
place itself.

## `wm.cfg`

```
# a comment, to the end of the line
side bottom
dots on

row wide
	term files text read web
	sheet draw view @games @misc

row tall term files text @games @misc

group games Games  chess snake blocks bricks
group misc  "More apps"  @rest
```

- **`row wide`** is the main view of a dock along the bottom or the
  top (17 fit); **`row tall`** of one on the left or the right (12 fit).
  A file with no `row tall` uses `row wide` on a side too, paging down
  it.
- **Names** are app file names, as in `/apps` or in flash -- the name
  `run` takes. The dock shows them in the order written.
- **`@<id>`** is a group's icon (below, "Groups"). The group may be
  given before or after the row that uses it; a name with no `group`
  line is reported (`no such group: @games`) and left out.
- **`@rest`** is every app with an icon in `dock.zic` that the row and
  its groups do not name, in name order. Once per row or group -- in a
  group (`group misc "More apps" @rest`), it keeps the row short and
  still catches an app installed later. Apps marked `hidden` in
  `apps.txt` (casino's tables, which `casino` starts) are not added by
  it, but a row that names one shows it.
- **`group <id> <name> <apps...>`**: `id` is what `@id` refers to (up
  to 11 characters, no `@`), `name` is what speech says for it ("quoted"
  if it has spaces, up to 23 characters). Up to 10 groups; a group
  cannot hold a group. A group may continue on indented lines like a
  row.
- **A line that starts with a space or a tab continues the row above
  it.** Blank lines and comments between continuation lines are fine.
- **`side`**: `bottom` (the default), `top`, `left` or `right` -- which
  edge of the screen the dock is on ("Sides", below).
- **`dots`**: `on` (the default) or `off` -- the running dots, below.

What the dock leaves out of a row, and only this:

- an app that is not installed (not in `/apps`, not in flash) --
  `z_exec_exists()`, the same answer `run` would give;
- an app whose hardware this bitstream does not have (`audio`, `gpio`
  in `apps.txt`): it would start only to exit;
- a group left with no apps by the two above (or an `@rest` group with
  nothing to catch). A group with **one** app is still a group.

Everything else is shown as written. An app with no icon in `dock.zic`
is drawn as the first letters of its name, and launches like any other.
A row longer than the dock pages, with NEXT at the far end, as before
([window_manager.md](window_manager.md#paging)).

Limits: 48 icons in the row and its groups together (the last are left
out, and the console says so), names up to 15 characters, lines up to
159 characters (use continuation lines), 768 bytes of names for both
rows and the groups together.

**A problem in the file** -- an unknown setting, a misspelt `side`, an
`@` name that is not `@rest`, a line too long, a second `row wide` --
is reported on `wm`'s console with its line number, and that line is
ignored; the rest of the file applies:

```
wm: dock: /data/wm/wm.cfg line 2: unknown setting: frobnicate
wm: dock: /data/wm/wm.cfg: no such group: @games
```

(A group is looked up once the whole file is read, so that message has
no line number.)

| what is there | what the dock does |
|---|---|
| no `/data/wm/wm.cfg` anywhere (a flash archive older than this) | `row wide term files text @rest`: the core apps, then every app with an icon |
| a `wm.cfg` with no `row wide` | every app with an icon (`@rest`), no groups |
| an empty `row wide` | no icons; the dock is an empty frame until a reload brings some |

## Groups

A group is one icon in the row -- a box with a thick bar across its top,
so it never looks like an app (`icon-<id>.png`, or
`icon-group.png` for a group with no icon of its own) -- that opens **in
place of the row**: the dock shows the group's apps, with **BACK** at
its start. The frame does not move or change size: it is as wide as the
widest view from the start, a group's counting its BACK.

```
 row:    [term] [files] ... [cal] [Sound] [Tools] [Games] 
 Games:  [BACK] [casino] [chess] [kidgames] ... [mausrun] [planedemo]
```

- **Open**: click the group's icon, or Enter on it.
- **Back to the row**: BACK (click, or Enter on it), **Esc** or
  **Backspace** while the dock has focus. In the row Esc and Backspace
  are not the dock's, as before. The row comes back on the page the
  group was opened from, with the selection on the group's icon.
- **Launch** from a group: the dock goes back to the row once the launch
  is over -- the app's window shown, or the launch timeout.
- Hiding the dock (a fullscreen window, game mode) also takes it back to
  the row.
- A group with more apps than the dock holds pages like the row: BACK
  first, NEXT last, on every page.
- **Speech** says the group's name with its first app when it opens
  ("Games: Casino"), and the name for the group's icon in the row.
- **Running dots** under a group's icon are its apps' copies added up
  (six at most, like any icon).

The built-in file has Sound, Tools, Games and "More apps" (the last,
`@rest`, empty and so not shown today) in `row wide`, and those with
Work, Desk and Network in `row tall`, for a dock on a side.

## Sides

`side` in `wm.cfg` puts the dock against any edge of the screen, 8
pixels in, as it always was at the bottom:

| `side` | where | runs | shows | fits |
|---|---|---|---|---|
| `bottom` | bottom left, growing right | across | `row wide` | 17 |
| `top` | top left, growing right | across | `row wide` | 17 |
| `left` | top left, growing down | down | `row tall` | 12 |
| `right` | top right, growing down | down | `row tall` | 12 |

Everything else is the same on every side, because none of the layout
knows which way it runs (`dock_layout.h`: a position is a distance
along the dock, and `wm` makes it an x or a y): BACK at the start of a
group, NEXT hard against the far end when a view pages -- the right
end of a row, the bottom of a column -- the frame as long as the
longest view, the arrow keys (Left and Up go back, Right and Down on,
whichever way it runs).

- **The running dots** are on the screen-edge side of the icons: under
  them along the bottom, above them along the top, left of them on the
  left, right of them on the right. That padding is the one a pixel
  deeper (`DOCK_DOT_ROOM`). The dock is 41 pixels thick on every side.
- **New windows** start beside a dock on the left or along the top
  instead of on it: the cascade (20, 20 and on in steps of 24, 20)
  starts past the dock. A dock on the right or the bottom is out of its
  way.
- **Captions** keep out of the dock: centred in the screen the dock
  leaves when it is on a side; above it at the bottom, below it at the
  top, for a caption placed there ([captions.md](captions.md)).
- **The keyboard layout label** (Super+Space) is over the far end of
  the dock: the right end of a row, the bottom of a column.
- **Maximize and full screen** hide the dock as before (it is moved off
  the screen whatever its side).
- **`cfg reload`** moves the dock to its new side at once -- or, while
  it is hidden, to where it comes back.

## `dock.zic`

The icons, and what each app needs to run. `wm` reads it front to back
and keeps only the icons and spoken names **its dock shows**,
compressed, expanding an icon as it draws it. Made by `sw/data/icons/gen_dock_icon_data.py` from
`icon-<name>.png` and one line per icon in `sw/data/icons/apps.txt`, and
checked in, so building the OS needs no Pillow:

```
cd sw/data/icons && python3 gen_dock_icon_data.py          # writes dock.zic
cd sw/data/icons && python3 gen_dock_icon_data.py --check  # is it current?
```

`apps.txt`:

```
#   name  ["spoken name"]  [needs...]  [hidden]
term        "Terminal"
track       "MOD player"    audio
mmod                        gpio
poker                           hidden
@next       "Next page"         hidden
```

- **spoken** is what speech says when the dock selection lands on the
  icon ([tts.md](tts.md)); the name itself when left out.
- **needs** is a `Z_FEATURE_*` / `Z_FEATURE2_*` name from
  `sw/common/zsoc.h`, lower case, without the prefix. The generator
  looks it up there, so any feature bit works.
- **hidden**: not added by `@rest`.
- Names starting with `@` are the dock's own: `@next` is the NEXT icon
  (`icon-next.png`), `@back` a group's BACK, `@<id>` the icon of the
  group with that id (`@games`, `icon-games.png`), and `@group` the
  icon of a group with none of its own.

The script stops if a PNG has no line or a line has no PNG, if a PNG is
not exactly 32x32 in pure black and white, or if an icon does not come
back the same from its own coding.

**Adding an app to the dock**: a PNG and a line in `apps.txt`, run the
script, and the app's name in a row of `sw/data/wm.cfg` (or nothing, if
`@rest` is enough). Nothing in `wm` changes, and `wm` is not rebuilt;
the flash archive is (`make flash_sw`), since that is where the two
files live.

### Format

Little-endian. Three parts, read in order, so `wm` never seeks:

| offset | |
|---|---|
| 0 | `"ZIC2"` |
| 4 | `u16` count, `u16` entry size (32) |
| 8 | `u32` bytes of spoken names |
| 12 | `u32` bytes of icons |
| 16 | entries, in name order: `name[16]`, `u32 feature`, `u32 feature2`, `u16 flags` (1 = hidden), `u16` spoken offset, `u16` icon offset, `u16` icon length |
| then | the spoken names, NUL-terminated (`""` for none) |
| then | the icons |

An icon is 32 rows of 4 bytes (MSB first, bit 1 = ink, white in the
PNG), every byte XORed with the one a row above it, then run-length
coded: a byte `c` below `0x80` is followed by `c+1` literal bytes, one
of `0x80` or more stands for `(c & 0x7f)+1` zero bytes. The icons are
mostly empty and mostly vertical lines; today's 47 take 3,514 bytes,
58% of their 6,016.

`wm` reads the table first (`dk_zic_read_table()`), marks the entries
its dock shows, and reads on through the names and icons keeping only
those (`dk_zic_read_parts()`, in `sw/apps/wm/dock_cfg.c`). It refuses a
file whose magic, entry size, sizes or offsets do not add up, a name
with no NUL, offsets out of order, or more than 64 entries or 384 bytes
of names. A refused or missing `dock.zic` leaves a dock that still
works: every icon is drawn as its app's name, NEXT as `>`, BACK as `<`;
a file cut short keeps the icons read before the cut.

A `/data/wm/dock.zic` on the card replaces the icons in flash, all of
them. Single icons are changed in `pix` ([pix_app.md](pix_app.md)),
which saves only the changed and added ones to `/data/wm/icons.zic`.
wm reads that file after dock.zic, at start and on `cfg reload`: an icon
there replaces the icon and spoken name of the dock entry of the same
name -- an app, a group, or one named in wm.cfg that had no icon --
and keeps dock.zic's hardware needs. An icon for an app that is in
neither file nor wm.cfg is not added by `@rest`; name it in a row.
Its table is read into the parser's memory, which is free by then, so
it costs wm no buffer; its icons go into the same store as dock.zic's.

```
wm: dock: /data/wm/icons.zic: 2 of 2 shown
```

## What `wm` says at start

`dock_build()` runs when `wm` starts and after every reload, and says
how long each step took on this board -- the figures to look at on the
first run of a new board or a slow card:

```
wm: dock: /data/wm/wm.cfg: 1876 bytes, 0 problem(s), 4 ms
wm: dock: /data/wm/dock.zic: 36/47 icons, 3069 bytes, 7 reads, 8 ms
wm: dock: 33/33 apps, row 17, 3 group(s), check 41 ms, 1 page(s), 52 ms
```

- the first line: reading `wm.cfg` -- whole, in one read (it was 128
  bytes at a time, which was most of the 120 ms the first board run
  showed) -- and how many lines it found wrong;
- the second: reading `dock.zic`: how many of its icons the dock shows
  and kept, the bytes kept (icons and spoken names), how many file
  calls it took (seven with today's file: the open, the header, the
  table, the names, the icons in three), and the time, the
  installed check left out;
- the third: how many of the apps the row and its groups name are kept,
  the icons in the row, the groups shown, the installed check
  (`z_exec_exists_many()`, one pass over `/apps`), the row's pages, and
  the whole of `dock_build()`.

Then one line for each app left out and why (`'mmod': no hardware for
it`, `'chess': not installed`, `'foo': no icon`). The times are from
`z_uptime_ticks()`, about 1.4 ms a tick. The figures above are from the
host test; the board's are the ones to look at.

## Running apps

Under each icon (beside it, on the screen-edge side, when the dock is
not at the bottom: "Sides"), **one dot for each copy of that app that is
running**, up to six:

```
 [term] [files] [text] [read] [web]
   .             . .            .
```

A dot is 2x2 pixels, the dots 2 apart and centred under the icon, in
the dock's bottom padding -- which is one row deeper than the top since
the dots came (`DOCK_DOT_ROOM`), so a dot has a clear row above it and
below it. Seven or more copies still show six. Under a group's icon, the
dots are its apps' copies added up.

What counts is **a process**, not a window: a copy on another workspace,
one started from a shell, by cron or by `casino`, and a program with no
window at all (`mausrun` in game mode) all show; an app with three
windows in one process shows one dot. `wm` asks the kernel's process
table (`z_proc_list()`), where a process is named by the program it was
started from (`text`) or, if it registered a name, by that (`web0`: the
base and a number) -- `dk_proc_is()` in `dock_cfg.c` matches both. A
process on its way out (`Z_PROC_FLAG_DIE`) no longer counts; one that is
waiting (`BLOCKED`) does.

**When it is counted**: when a window is created or destroyed, when the
dock launches something, and every 2 seconds -- the last for programs
that end without telling `wm`: a windowless one quitting, or one killed
from the kernel shell. **While any dot is shown**, that 2-second recount
wakes `wm` by itself (`wm_idle_ticks()`), so such a dot goes within
about 2 seconds with nobody touching anything; with no dot shown
(`dots off`, nothing running, the dock hidden) nothing wakes `wm` for
it. The dock is repainted only when a dot it draws changed (an eighth
copy changes none).

**Speech** says the count with the name: "Text editor, 2 running", when
the dock selection lands on an icon, when the dock takes focus, and for
Super+W over an icon. It says it with `dots off` too.

`dots off` in `wm.cfg` draws none.

**The kernel's part.** The process table never held a program's name --
only apps that call `z_pid_register()` had one, and every other process
was listed as `""`. The kernel now records it when it starts a program
(`z_proc.exe`, 16 bytes a slot, set by `k_proc_set_exe()` in
`sw/os/procapi.c` from what `k_proc_create_exec()` was given, the part
after the last `/`), and `Z_SYS_PROC_LIST` reports it for a process that
registered no name. The interface is unchanged (`z_proc_info_t` is as it
was, and the ABI id with it); what changed is that fewer names are
empty. `info` and posix's `ps` now show `text` where they showed
`-`, and `casino`'s "already open" check, which compared names that were
always empty, now works ([casino_app.md](casino_app.md)). With a kernel
older than this, only the apps that register a name (`web`, `ask`,
`keyboard` among the dock's) get dots. The new kernel: 160 bytes of
flash, 512 of RAM.

## Memory

Everything is static, none of it on `wm`'s 8KB stack (which its heap
shares):

| | bytes | |
|---|---|---|
| `dock_store` | 3,584 | the icons (compressed) and spoken names the dock shows, and the groups' names; the built-in files use 3,069 |
| `dock_apps` | 1,152 | the row and its groups: 48 names, with where each one's icon and name are in the store |
| `dock_scratch` | 2,144 | the parser, then `dock.zic`'s table (64 entries), then the process table for the dots -- one at a time, sharing the space; while the icons are read, the parser's space is the read buffer |

The whole of `dock.zic` is no longer in `wm`: the 8KB buffer it was
read into is gone, and an icon the dock does not show costs nothing.
With groups, `wm`'s image was 138,192 bytes with xPack GCC 15.2; the
sides took it to 139,020, and the pointer's shapes ([cursor.md](cursor.md))
to 140,240, and game-mode colour's reach below row 479 in `zgfx` to
140,276, and pix's icons laid over the release's (`icons.zic`, below)
and keeping the pointer for a game that asks to 140,848 (budget 138K,
141,312, `CORE_BUDGETS` in the top `Makefile`:
[flash_apps.md](flash_apps.md)), about 107KB in flash (budget 108K).
The colour window's part ([color.md](color.md), "Who has it") was paid
for by its key table (`kbd_sent`) going from 32 to 16 bits a key (512
bytes of RAM) and by the per-click, per-resize and titlebar-icon lines
on the console moving behind `wm_clip_debug` (about 480 bytes of flash):
140,880 bytes with xPack GCC 15.2, 106,760 in flash (with the
pointer hidden over a game's picture, `Z_WIN_FLAG_NO_POINTER`, 64 bytes).

The two files take 8KB of the flash archive's file reserve, which went
from 56K to 60K; `wm`'s flash budget went from 110K to 104K to pay for
it ([flash_apps.md](flash_apps.md)).

## Code and tests

- `sw/apps/wm/dock_cfg.[ch]`: the pure part -- the `wm.cfg` parser,
  the `dock.zic` reader (front to back, through a read function it is
  given) and icon expander, the list a row and its groups make
  (`dk_list()`) and what is left of it once the uninstalled apps and
  empty groups go (`dk_keep()`). No syscalls, no drawing.
- `sw/apps/wm/dock_layout.h`: the geometry -- slots, pages, NEXT, and a
  view's BACK, along either axis.
- `sw/apps/wm/wm.c`: `dock_build()` (the files, the installed and
  hardware checks), `dock_reload()`, `dock_open_view()` /
  `dock_close_view()` / `dock_return_check()`, `draw_icon_letters()`,
  `dock_count()` and the dots in `draw_dock()`; for the sides,
  `dock_rect()`, `dock_slot_xy()` and `dock_slot_at()` (the drawing and
  every hit test use these two), `dock_place_origin()`, and the dock's
  part of `cap_layout()`.
- `sw/os/procapi.c`: `k_proc_set_exe()`, and `k_proc_list()` reporting
  it; `sw/os/tests/test_proclist.c` (10 checks: a path cut to its last
  part, a long name cut to 15, a registered name winning, a slot
  reused).
- `make -C sw/apps/wm test` runs:
  - `tests/test_dock.c` (4,707 checks): the layout arithmetic -- 17
    across, 12 down, paging, NEXT, hit testing at every edge, every app
    on exactly one page at one position on either axis, with and without
    BACK.
  - `tests/test_dock_cfg.c` (392 checks, the cursor file reader's among them, [cursor.md](cursor.md)): the parser line by line,
    continuation lines, comments, CRLF, every reported problem, `@rest`
    in a row and in a group, groups (given before or after their row, a
    group in a group, an unknown `@id`, too many, an empty group
    dropped and the views renumbered, a group of one kept), a file fed
    a byte at a time, more names than the dock holds; `dock.zic` made
    in the test and read a few bytes at a time, damaged in every way
    (magic, sizes, offsets out of order, a name with no end, cut
    short), a store too small, the icon coding both ways; and the
    shipped `wm.cfg` and `dock.zic` themselves -- no problem in the
    file, the wide row's 14 apps and Sound, Tools and Games, the tall
    row's 12, every app and group with its icon, the hardware each
    needs; `dots`; which process names are which app.
  - `tests/test_dock_files.c` (152 checks, section 26 the pointer's shapes): the real `wm.c` with a
    scripted kernel serving the two files (7 bytes a read), installed
    apps and bitstream features, and a software framebuffer: the
    shipped files give 17 icons, each slot drawn from its icon; a group
    opened by key and by mouse, BACK, Esc and Backspace, the row coming
    back on its page with the group selected; launching from a group
    and back to the row when the window shows; a group bigger than the
    dock (BACK and NEXT); no AUDIO / no GPIO, and a group emptied by it;
    a card without the games; a person's own flat `wm.cfg` with an app
    that has no icon; `dock_reload()` shrinking the dock and clearing
    what it left; `dock.zic` missing, cut short, or damaged; no
    `wm.cfg`; no row wide; a file with problems; an empty dock and
    back; the running dots from a scripted process table (counts,
    2x2, centred, clear above and below, six at most, a group's added
    up, an eighth copy repaints nothing, a recount due when a window
    opens or closes and a wake deadline only while a dot is shown,
    `dots off` draws none); the other sides: on the left the tall row's
    12 down the column, a group opened by a click in it, a click in the
    padding beside an icon doing nothing, the dots a column left of the
    icon, new windows right of the dock, a caption centred beside it;
    on the right the dots right of the icon; along the top the dots
    above it, new windows and a top caption below it; a side with no
    `row tall` paging the wide row down it, NEXT at the bottom end;
    `side` changed while the dock is hidden, back in the new place.
    Needs the page at address 0
    (`sw/common/tests/ztramp.h`); exit 77 means skipped. Pictures in
    `/tmp/wm_test_dock-*.pbm`.
- `tests/test_lock.c` builds the real `wm.c` too (66 checks).

## On the board (to check)

- Each side: `side left` (then `right`, `top`) in a card `wm.cfg`, `cfg
  reload` and a key: the dock moves; the icons, dots, groups, BACK,
  paging and the arrow keys work the same; a new window opens beside
  it; Super+Space shows the label over its far end; a caption (volume
  keys) keeps clear of it; maximize hides it and it comes back on its
  side.

- The console shows the three lines above, with their times; `dock.zic`
  in a handful of reads.
- The row: 14 apps, then Sound, Tools and Games. Click Games: its apps
  with BACK first, the frame the same size. Esc: the row, Games
  selected. Enter on Tools, Right to an app, Enter: it starts and the
  dock is back on the row.
- Speech on: "Games: Casino" when Games opens.
- A bitstream without audio: no Sound group at all.
- No card: the same dock, from flash.
- A `/data/wm/wm.cfg` on the card with `row wide text term @g` and
  `group g Mine chess snake`, then `cfg reload` and a key: three icons,
  the third opening Mine. A typo: reported with its line. Delete the
  file, reload: the built-in dock.
- A `/data/wm/dock.zic` on the card that is cut short: the icons after
  the cut drawn as names, the console says why.
- Running dots: six copies of `text`, six dots, a seventh still six;
  `snake` running shows a dot under Games; kill one from the kernel
  shell and touch nothing: its dot goes within about 2 s.
  `dots off`, `cfg reload`, a key: no dots.

## Later

Overriding single icons on the card, and an editor for them, are a
possible last phase -- likely as part of a more general bitmap editor
(sprites, fonts, `.zic` files), to be proposed on its own.
