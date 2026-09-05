# hex

A hex editor for files of unlimited size. `sw/apps/hex`.

```
> run wm
> run hex
```

Where `sw/apps/text` is a notepad and `sw/apps/read` is a viewer, this
is the one that opens a file and shows you the bytes. It edits them in
place, so the file it works on can be any size the card can hold —
nothing is ever loaded whole.

```
+---------------------------------------------------+-+
| Offset   00 01 02 03 04 05 06 07  08 ...          |#|
| ------------------------------------------------- | |
| 00000000 7A 65 69 74 6C 6F 73 00  00 ...  zeitlos. | |
| 00000010 ...                                      | |
| ------------------------------------------------- | |
| 00000048/0001F400  68 'h'  (72)                 rw|-|
+---------------------------------------------------+-+
```

## Status

| phase | what | state |
| --- | --- | --- |
| 1 | `FS_OPEN_RW` / `FS_SYNC` / `FS_TRUNCATE` syscalls | done |
| 2 | the grid, navigation, goto, open | done |
| 3 | editing, the journal, save, undo, new | done |
| 4 | dock icon, file associations, this document | in progress |
| 5 | search, redo, value inspector, grow/truncate | optional |

## Using it

Titlebar buttons, left to right: **new**, **open**, **save**, **font**,
**close**. New and save do nothing until phase 3.

| key | does |
| --- | --- |
| `0`–`9`, `a`–`f` (hex pane) | type a nibble |
| any printable character (character pane) | write that byte |
| arrows | move the caret by a byte / a row |
| PageUp / PageDown | a screen at a time |
| Home / End | start / end of the row |
| Ctrl+Home / Ctrl+End | start / end of the file |
| Backspace | step back one byte, changing nothing |
| Tab | switch between the hex and character panes |
| Ctrl+G | go to an offset |
| Ctrl+N / Ctrl+O / Ctrl+S | new / open / save |
| Ctrl+Z | undo |
| Ctrl+Q | close |

A byte takes **two keystrokes** in the hex pane: the first digit
replaces the high nibble and the caret stays put, the second replaces
the low nibble and the caret advances. The byte on screen changes after
the first digit, so the half-finished state is visible rather than being
a hidden mode. Any caret movement abandons a half-typed byte rather than
carrying it to the next offset.

**Backspace moves, it does not delete.** There is no delete in an
overwrite-only editor and no obviously right substitute — zeroing the
byte would be a destructive surprise from a key that means "undo my
typing" everywhere else. Moving the caret is what the muscle memory of
typing a wrong digit actually wants.

Anything that would discard unsaved work — new, open, close — asks
first and offers to save. If that save is then cancelled or fails, the
whole operation is off.

Clicking a cell moves the caret there **and switches to that pane**, so
clicking a character and then finding the keyboard still aimed at the
hex side can't happen.

The window is resizable. Bytes per row is recomputed from the width —
16 at the default size, dropping to 8 or 4 in a narrow window rather
than clipping the grid off its own right edge. It is always a power of
two, because the whole value of the address column is that its low
digit names the column, and that only holds when the row length divides
a power of sixteen.

### Going to an offset

Ctrl+G takes **hex by default**, since that is the base an offset gets
quoted in. A `0x` prefix is accepted and ignored. A leading `.` forces
decimal, for the times the number came from a format spec that quotes
sizes in decimal.

An offset past the end of the file lands on the last byte rather than
being refused. "Go to the end", typed as a round number bigger than the
file, is a perfectly clear request.

## Files of unlimited size

The file is never held in memory — the same constraint `read` works
under, for the same reason. What is held is a 4KB window of it
(`CACHE_BYTES`), reloaded only when the view moves outside what the
cache already covers. So scrolling a row at a time usually costs no
card traffic at all, and jumping to a distant address costs one read of
a few sectors no matter how far the jump was.

Reloads align down to a 512-byte boundary. Not for correctness — FatFs
handles any offset — but because a read starting mid-sector makes FatFs
fetch the sector before the one asked for, so an unaligned cache pays
an extra sector on every reload forever.

`repaint()` covers the whole visible screenful in one `cache_cover()`
call before drawing any of it. Left to `byte_at()`, the cache would
fault in from whichever row drew first, and a screen straddling the
cache edge would reload part-way down — correct, but two reads where
one would do.

Every offset is a `uint32_t`. That is not a limitation worth removing:
`FF_FS_EXFAT` is 0 and there is no 64-bit LBA
(`sw/os/fs/fatfs/ffconf.h`), so FAT32's own 4GB ceiling arrives first.

### `byte_at()` returns -1, not 0

A row running past the end of the file must draw **blank** there, and a
zero byte is a real value that must draw as `00`. Conflating them would
make every file look as though it ended in a run of zeros.

## Editing is overwrite-only

Bytes can be changed. Bytes cannot be **inserted or deleted**.

This is a deliberate limit, not an unfinished feature. Inserting one
byte at the front of a file means rewriting every byte after it, which
on a large file is minutes of card traffic and a destroyed file if the
power goes during it. Every hex editor worth the name is overwrite-only
for exactly this reason.

Changing a file's **size** is a separate, explicit operation
(`fs_truncate()`, `zfsapp.h`), never a side effect of typing.

## The edit journal

Pending edits live in an array of `(offset, what was there, what is
there now)`, oldest first. That is the whole editing model, and it is
worth saying why it is this and not one of the two obvious
alternatives.

**Write-through** — put each keystroke straight on the card — needs no
journal, but costs an SD write and a directory sync per nibble and
leaves no way to back out of a session. It also makes the Save button a
lie: there would be nothing left to save.

**A dirty page cache** — hold pages, flush on save or eviction — keeps
Save meaningful only until you scroll far enough, at which point
eviction commits your edits without being asked. A Save button that
silently isn't the only thing that saves is worse than no Save button.

The journal has neither problem. Memory is O(entries), independent of
file size, so "unlimited size" survives. The display reads through it.
Save is the only thing that writes. And `old` makes it an undo stack for
free.

`JRN_MAX` is 1024 entries, 8KB of `.bss`. Running out is **reported**,
not handled by dropping the oldest entry: silently discarding the oldest
would break undo (the stack would no longer reach back to the original
bytes) *and* lose an edit Save was going to write. Both are invisible
when they happen and surface only as a file that isn't what you typed.

### Reading through the journal

The card still holds the old byte, so a cache reload would quietly
revert the display while the journal still claimed the edit existed.
`jrn_apply_to_cache()` replays the journal over whatever was just read.

It runs **once per cache load, not per byte**. A lookup per displayed
byte would be 816 × 1024 comparisons for one repaint, which is why edits
also patch the cache directly on the way in — that covers the common
case where the byte is already resident, and the replay covers reloads.

`tests/test_edit.c` asserts this specifically, because it is the failure
that looks like the user mistyping rather than like a bug.

### Saving

Entries are sorted by offset and runs of consecutive offsets are
coalesced, so changing sixteen adjacent bytes is one seek and one write
rather than sixteen of each. Scattered edits still cost a seek apiece,
which is what scattered edits are. The test counts seeks and writes
rather than only checking the resulting bytes — "it wrote the right
bytes" is true of the naive one-write-per-byte version too.

The **index** is sorted, not the journal: the journal is an undo stack
and its order is its meaning. Sorting it in place would make undo
restore bytes in the wrong sequence, which for two edits to one byte
gives the wrong final value. Ties are broken by journal position so the
last edit to an offset sorts last and wins; earlier edits to the same
offset are skipped rather than written and overwritten.

It is a shell sort. Insertion sort is O(n²), and a million comparisons
on this CPU is time the user is waiting on; `qsort()` would pull in
libc's implementation plus a comparator call per comparison. Fifteen
lines, and the SD writes dominate — which is where the time should go.

`fs_sync()` at the end commits the directory entry as well as the data.
Without it the bytes are in the right clusters and the recorded size is
stale, so pulling the card leaves the work unfindable rather than merely
unwritten.

**Every write is idempotent** — a known byte at a known offset, derived
from the journal and never from anything read back. So a save that fails
part-way can simply be retried, which is why the journal is *kept* on
failure rather than discarded.

### Undo, and the awkward case

Normally undo pops the last entry and puts `old` back. Two edits to one
byte therefore undo to the first edit's value, not to the original,
which is what a LIFO stack of `old` values means.

After a **partially failed save** it cannot pop. Some of those entries
are already on the card, and popping one would leave the display showing
the old byte while the file held the new one, with nothing left in the
journal to ever reconcile them. So in that state undo appends an
**inverse edit** instead, which the next save writes. `test_edit.c`
covers this path end to end — fail a save mid-way, undo, save again,
check the card.

## New asks for a size

Hex editors that create an **empty** document (HxD, Hex Fiend, 010
Editor, Okteta) all rely on insert mode to grow it afterwards, and this
editor deliberately has none. Editors *without* insert mode — `hexedit`,
and essentially every disk editor — don't offer New at all; you open
something that exists.

An empty New here would hand you a document you cannot type a single
byte into: the worst of both. A name and a size is what `truncate -s`
and `dd` do, and it is the only shape that yields a usable document
under overwrite-only semantics.

Size is parsed the same way Ctrl+G parses an offset — hex by default,
`.` for decimal — because two number boxes in one app that disagree
about base would be a trap.

**The zeroing is not optional.** `fs_truncate()` grows a file by
allocating clusters, not by clearing them, so a file created by
truncation alone reads back as whatever the previous occupant of those
sectors left behind — someone else's deleted data, in a file you just
created.

It runs in 512-byte chunks with `pump_redraws()` between them. One big
write would be the longest single trip into FatFs anything in this
system makes, past `K_NO_PREEMPT_MAX_TICKS`, where the scheduler stops
deferring and the card protection this all depends on stops holding
(`docs/filesystem.md`). Chunks keep each syscall short and keep the
window alive.

`pump_redraws()` answers redraws but **ignores keys and clicks** — wm
blocks its whole main loop waiting for a redraw ack, so those must be
serviced, while a keystroke handled mid-operation would edit a document
that is still being created.

Free space is checked with `fs_df()` *before* anything is created. "I
meant 4MB and typed 4GB" deserves an answer, not a very long wait
followed by a half-written file.

## What phase 1 added to the filesystem

There was no way to modify a file in place. `fs_write_file()` rewrites
the whole thing from a buffer, and `fs_open_write()` is
`FA_CREATE_ALWAYS`, so it truncates the moment it opens. Both are the
right shape for `te`'s small documents and for a TFTP transfer, and the
wrong one for editing byte 900,000 of a 4MB file.

| call | does |
| --- | --- |
| `fs_open_rw()` | opens an existing file for read and write, truncating nothing |
| `fs_sync()` | commits data and directory entry without closing the handle |
| `fs_truncate()` | sets the file size, growing or shrinking |

Two behaviours worth knowing, both documented at the wire struct in
`sw/common/zfs.h`:

- **`fs_open_rw()` refuses to create.** An editor given a path that
  does not exist should say so, not silently produce an empty file at a
  mistyped name. Creating is `fs_touch()`, one explicit call away.
- **Growing does not zero.** FatFs allocates clusters without clearing
  them, so a grown region reads back as whatever the last file left
  there. Zeroing in the kernel would block the machine for the duration
  with no way to show progress; userland can do it in steps and show
  something.

All three go through the same handle table, owner-pid check and
`k_syscall_touches_fs()` deferral as the rest of the chunked API — see
`docs/filesystem.md`.

`Z_FS_MAX_OPEN` was raised from 4 to 8 at the same time. Four was
chosen when the table served one at-a-time transfer per caller, and
stopped fitting once apps began holding a handle for their whole
lifetime: `read` holds one on the document it is showing, `hex` holds
one on the file it is editing, and `read`'s own `open_path()` already
carried a scar from running out. The cost is 560 bytes a slot —
`sizeof(FIL)` is 552, almost all of it the per-file sector buffer, since
`FF_FS_TINY` is 0 — so +2,240 bytes of kernel `.bss`, which
`sw/os/Makefile` pads into `kernel.bin` byte for byte inside its 256KB
flash region.

**Anything touching `syscalls.def` means kernel and every app must be
rebuilt and reflashed together** (`make dev-flash`). An app built
against a newer `syscalls.def` than the running kernel calls the wrong
handler for every syscall past the divergence — see that file's own
warning.

### Running on an older kernel

An app built with these calls on a kernel that predates them does not
crash: `k_syscall_entry()` bounds-checks the id and returns `z_fail`,
so `fs_open_rw()` returns -1. `open_file()` falls back to
`fs_open_read()` and the status line says `read-only`.

That fallback is also why opening is a two-step. `fs_open_rw()` returns
-1 for "no such file", "no free handle" and "this kernel is too old"
alike, so a bare failure could mean any of three very different things.

## The grid

Everything about where a field lands comes from four functions —
`cell_hex_x()`, `cell_byte_x()`, `cell_ascii_x()`, `cells_needed()` —
and **both the drawing and the mouse hit test walk them**, so what you
can see and what you can click are the same columns by construction.
That is the same "compute once, share everywhere" rule `wm.c`'s
`close_icon_rect()` follows, for the same class of bug: when the two
drift, nothing crashes, you just click one byte and select its
neighbour.

`tests/test_layout.c` asserts exactly that round trip — for every byte
of every row, the pixel where its hex digits and its character are
*drawn* must map back to that same byte — across 196 window size and
font combinations.

A row is drawn as **one string in one call**. A 16-byte row is 76
character cells; as three separate draws that is three glyph-blitter
setups per row, and as one call per byte it is thirty-two. The row is
naturally one line of text, and the only reason to split it is per-byte
colour, which the cursor needs for exactly one byte and gets by
overdrawing.

### The cursor is in two places at once

The caret marks its byte in **both** panes, so the eye can follow one to
the other. The focused pane draws its cell inverted; the other draws a
1px frame around the cell instead. Two identical inversions would be
ambiguous about which pane has the keyboard, which is the only thing the
distinction exists to say.

### The status line is built right to left

The mode indicator (`rw` / `read-only`) is placed first, right-aligned,
and everything else is built into what is left.

Built left to right, the mode is the field that falls off a narrow
window — which is exactly backwards. A truncated offset is an
inconvenience; a missing `read-only` means the user believes they can
edit, types, and finds nothing happens.

### The `.` placeholder, and the font change it caused

Non-printable bytes render as `.`, the universal convention.

`z_font_5x8`'s period used to be a three-pixel **diamond**
(`..#. / .###. / ..#.`) straddling the baseline into the descender row
— that is what stock misc-fixed ships. It is perfectly legible in
prose, where periods are rare. It stops being legible in a column of
them: a binary file came out as a wall of crosses dense enough to read
as content rather than as padding, which is the opposite of what a
placeholder is for.

It is now the same 2×2 baseline dot `:` already used for its lower
half, so it matches the rest of the punctuation in the font and matches
`z_font_6x12`, which always had a proper dot.

The change lives in `sw/data/font/5x8.bdf`, in a `COMMENT` block
recording the divergence from upstream. `font5x8.mem` and
`zfont_data.c` are both generated from it:

```
cd sw/data/font
python3 bdf_to_mem.py 5x8.bdf 5 8
python3 gen_font_data.py
```

Both generators reproduce their checked-in outputs byte for byte from
the unmodified sources, so the resulting diff in `zfont_data.c` is
exactly one line. It is board-wide, though — every app drawing a `.` at
5x8 gets the new glyph.

## Coordinates

Nothing in `hex.c` uses `z_win_hw_line()` or `z_win_hw_box()`. Those
take **absolute screen coordinates** while everything else an app draws
with is content-relative — the trap `docs/window_manager.md` devotes a
section to, which cost `sw/apps/logic` three shipped layouts. Rules and
frames here are built from `fill_content()`, which is content-relative
like the rest, so the two families are never mixed.

## Looking at the layout before shipping it

```
cd sw/apps/hex && make render
```

or with arguments:

```
cc -std=gnu99 -Wall -I sw/common -o /tmp/render \
   sw/apps/hex/tests/render.c \
   sw/common/zwin.c sw/common/zwidget.c sw/common/zfont_data.c \
   sw/common/zobj.c sw/common/zeitlos.c
/tmp/render /tmp/hex.pbm 500 300 1        # width, height, 1 = 6x12 font
```

The synthetic file in `tests/render.c` matters as much as the harness.
It holds every byte value in turn, a run of readable text, a run of
zeros and a run of `FF`, and parks the caret mid-row — an idle grid of
zeros would hide exactly what this exists to find. It also goes through
the real `cache_cover()` / `byte_at()` path against a stub
`fs_read_chunk()`, so the alignment and short-read behaviour is
exercised rather than bypassed.

The renderer found the `.`-is-a-diamond problem and the
status-line truncation order on the first look, neither of which any
assertion would have caught.

`tests/test_layout.c` is the other half and runs unattended:

```
cc -std=gnu99 -Wall -I sw/common -o /tmp/test_layout \
   sw/apps/hex/tests/test_layout.c \
   sw/common/zwin.c sw/common/zwidget.c sw/common/zfont_data.c \
   sw/common/zobj.c sw/common/zeitlos.c
/tmp/test_layout
```

## The dock

`hex` has a dock entry (`dock_candidates[]` in `sw/apps/wm/wm.c`), so it
launches without going through the browser or the shell.

The icon is a small hex dump: two bytes, the pane divider, and the
character column. It is drawn with the system's own `z_font_5x8`
glyphs, so it sits with the rest of the dock rather than beside it, and
the bytes are real -- 0x68, 0x65, 0x78 -- so the character column
correctly reads `h`, `e`, `x` down the right-hand side.

Regenerate after editing `sw/data/icons/icon-hex.png`:

```
cd sw/data/icons && python3 gen_dock_icon_data.py
```

That rewrites the checked-in `sw/apps/wm/dock_icons.c/.h` for every
icon, so check the diff is confined to the one you changed.

Note the dock holds **seventeen** icons before running off a 640px
screen, and nothing clamps it; sixteen are listed now. See
`docs/window_manager.md`, "The dock".

## File associations

`BIN`, `ROM`, `IMG` and `DAT` open in `hex` (`sw/common/ztype.c`).

Deliberately a short list. `hex` opens absolutely anything, so it is
tempting to make it the fallback for every unclaimed extension — and
that would be wrong. The file browser's "no application is associated
with this file" is a useful answer, and turning every unknown
double-click into a hex dump would bury a real question under something
that always technically works.

There is no entry for a file with *no* extension: that is the executable
case, and claiming it here would break launching programs from the
browser.

## Testing it

```
cd sw/apps/hex
make test      # geometry + editing model, unattended
make render    # writes /tmp/hex.pbm -- then LOOK at it
```

Both need `-no-pie` and `vm.mmap_min_addr=0`; the Makefile passes the
first and the tests exit 77 with an explanation rather than crashing if
either is missing. `tests/trampoline.h` says why.

That header is worth knowing about. `zrender.h` makes an app's
*drawing* run on a host by mapping real memory at VRAM's fixed address;
drawing is not the only thing at a fixed address. A message send is an
indirect call through `reg_kernel`, a pointer the kernel plants at
`0x0000000c`. That never mattered while renders only called layout and
repaint — it matters here because the edit path sends messages
(`put_byte()` updates the modified marker, which retitles the window),
so a host test that types a byte would dereference a null function
pointer and die before asserting anything. The trampoline maps a page at
0 and puts a stub there, so the real message-sending code runs
unmodified and the calls simply land in the test.

Both suites are **mutation-tested**, not just green: removing
`jrn_apply_to_cache()` produces "edit lost to a cache reload", removing
the duplicate-offset skip produces "three edits to one byte took 3
writes, expected 1", and narrowing the hex hit range by one cell
produces 29,456 layout failures.

## Known limitations

- **No insert or delete.** By design, above. Growing or shrinking an
  existing file is phase 5; `fs_truncate()` is already there for it.
- **Undo does not survive a save.** Save clears the journal, and the
  journal is the undo stack. Undoing a saved change means editing it
  back, which for an overwrite-only editor is the same operation.
- **Nothing prevents two apps opening the same file.** `FF_FS_LOCK` is
  0, so `text` can open and rewrite a file `hex` has open read-write,
  and the result is undefined. Worth knowing; not worth solving yet.
- **A killed process leaks its handle.** `hex` holds one open for its
  whole lifetime. Closing it through the titlebar or Ctrl+Q releases it,
  but `kill`ing the process from the shell does not — there is no
  process-exit sweep for the handle table (`zfs.h`). One of eight slots
  until reboot.
- **32 bytes per row never fits.** `cells_needed(32)` is 142 cells,
  which is 710px at 5x8 — wider than the 640px screen. The code handles
  it and the buffers are sized for it, so a larger display would get it
  for free.

## See also

- `docs/window_manager.md` — the window protocol, titlebar icons, and
  the off-device renderer
- `docs/widgets.md` — the scrollbar and the file dialogs
- `docs/filesystem.md` — why concurrent file access is safe, and what
  the new syscalls had to be added to
- `docs/text_editor.md` — the other editor, and where the dialog and
  title handling patterns here came from
