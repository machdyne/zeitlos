# term

A VT100 terminal window. `sw/apps/term`.

`term` is a dumb terminal: it relays keys to a **port**
(`docs/ports.md`) and renders what comes back. What it connects to --
`repl`, `posix`, a telnet or ssh session through `net`, a serial line --
is chosen per window, and nothing about that choice lives in `term`
beyond the choosing.

## Starting out: the panel

A new `term` window is **not connected to anything**. It shows a panel:

```
+------------------------------------------------------+
|                term0 -- not connected                 |
|  [ REPL ]  [ POSIX ]  [ CONSOLE ]  [ OPEN F11 ]       |
|  REPL    Scheme and system commands            ready  |
|  POSIX   Unix-style shell, zcc and vi    not running  |
|  CONSOLE Boot log and kernel shell             ready  |
|  OPEN    port, serial, telnet or ssh                  |
|  choose a shell, or open a connection                 |
|  Tab chooses, Enter connects, or type a target        |
|  F12 disconnects  Esc hides  Shift+PgUp scrolls       |
+------------------------------------------------------+
```

| on the panel | does |
| --- | --- |
| click a button, or Tab / arrows then Enter | connect to `repl0` / `posix0` / `console0`, or open the bar |
| type a printable character | open the Open bar with it -- `port posix0`, `telnet host` just work |
| Esc | hide the panel to read the old session underneath; any key brings it back |
| Ctrl+Shift+V | paste the clipboard's first line into the Open bar |

**CONSOLE is the kernel console**: everything the kernel has printed
since boot (its last 4 KB), then live output, and a prompt shared with
the serial console. It works on a machine with no card and no serial
cable. See `docs/console.md`.

**The connection buttons are live only while their provider is registered**
(`repl0`, `posix0`, `console0` in the pid registry), rechecked twice a second while
the panel is up. A disabled button is an empty frame -- the toolkit's
disabled look (`sw/common/zwidget.c`). At boot the panel is often on
screen before init has finished loading the shells off the sdcard, and
the buttons coming alive one by one says that better than a click that
fails. On a board with no sdcard both shells stay disabled, which is the
truth: they live on the card (`docs/flash_apps.md`). CONSOLE is a core
app in flash, so it is there either way. OPEN is always live.

Until you move focus yourself, it follows the first ready one -- REPL,
else POSIX, else CONSOLE, else OPEN -- so Enter does the likely thing
the moment the window appears.

### Connecting by itself: `apps.term.auto_connect`

Set in `/zeitlos.cfg` (`docs/config.md`), e.g.
`apps.term.auto_connect: port repl0`, every new window connects there
instead of waiting on the panel. It takes the same text as the Open bar.

A window opened at boot can be up before the shell has registered, so
term **waits up to 15 seconds for the provider's name** (`repl0`;
`serial0` for serial and usbserial; `net0` for
telnet/ssh), with the panel's status
line saying what it is waiting for, and connects once. **Esc** stops the
wait. A refusal, an unparseable value, or a provider that never appears
lands on the panel with the reason. It only happens when a window opens
-- F12 and a closed connection still stay on the panel.

`auto_poll()` runs at the top of `frame()`, before `render()`, so a
successful connect that hides the panel repaints the cells it covered in
the same frame.

### Why it no longer connects to repl by itself

It used to: `repl0` at startup, a fixed-pid fallback (`Z_PID_REPL`), and
local echo if neither answered. Two things changed that.

- **repl left flash.** It is on the card now, next to posix. A
  card-less board would have opened a terminal quietly echoing its own
  keys at you, which looks broken and is not.
- **There are two shells.** With a card present, repl and posix both
  start at boot and neither is the obvious default. One click is
  cheaper than a wrong guess followed by F11 and typing.

**There is no local echo any more.** A terminal connected to nothing has
nothing to say; keys go to the panel instead.

### F11 and F12

| key | does |
| --- | --- |
| **F11** | the Open bar -- go somewhere new |
| **F12** | disconnect and return to the panel -- from anywhere |

Both are intercepted before the port and work whatever the window is
connected to. That is the point of F12: a remote with no quit command,
or a child program that has hung, still has a way out. It used to go
back to `repl0`; with no default connection, "back where you started"
is the panel, and REPL is one key from there.

The window also returns to the panel when **the far end closes** the
connection (`exit` in posix, `quit` in repl, a remote hanging up), with
the reason on the status line.

## The Open bar

A prompt across the bottom row: `open> telnet 192.168.1.10`. Enter
connects, Escape cancels, Backspace edits. Same four words as repl's
commands and the same code underneath -- see `docs/connections.md`.

While a connection is being made the bar says so after a quarter of a
second (a local port answers faster than that, and a bar flashing up for
one frame on every REPL click reads as a glitch):

```
connecting to 192.168.1.10 ...  (Esc cancels)
```

**Esc cancels a connect in progress.** Telnet can legitimately take up
to 45 seconds to fail (`Z_CONN_TIMEOUT_NETWORK_TICKS`), and there used
to be no way out of that wait. A failed or cancelled connect lands on
the panel with the reason: `posix0 is not running`, `net0 refused:
...`, `no answer from 10.0.0.5`.

### The bar that would not go away

The bar used to stay on screen after connecting, partially erased as new
output wrote over it. Esc left it behind the same way.

The cause was the shadow buffer (below). The bar was painted straight
over the bottom row while the shadow went on describing the session's
text underneath. On dismissal the row was marked dirty -- enough back
when a dirty row was always redrawn, not enough once rendering started
comparing against the shadow: every cell compared *equal*, so nothing
was drawn, and only cells that later output actually changed got
repainted. See "Rendering" for how overlays work now.

## Scrollback

**200 lines** -- eight screens -- of what scrolled off the top.

| key | does |
| --- | --- |
| Shift+PgUp / Shift+PgDn | back / forward a page (24 lines, so the line you were on stays in view) |
| Shift+Up / Shift+Down | one line |
| Shift+Home / Shift+End | oldest line / live |

The **scrollbar** down the right-hand side shows where you are; drag the
thumb or click the trough. There is no mouse wheel in this system
(`sw/common/zwidget.h`). The window is `Z_SB_THICK` (12px) wider than
80 columns to make room -- 416px instead of 404 -- so the text grid is
still exactly 80x25.

**Shifted keys, because the unshifted ones belong to the far end.**
PgUp, arrows and Home/End mean things to pagers and line editors. `term`
already sent identical bytes for the shifted and unshifted forms, so
nothing on the far end can lose a key it could previously tell apart.

**Output while scrolled back does not move the view.** Each line pushed
into history keeps the text you are reading where it is, until the ring
is full and starts evicting it. **Typing or pasting returns to live** --
you want to see the answer.

### What goes into history

Only lines a **linefeed** scrolls off the top. Not lines deleted by
`DL` (`ESC[M`) at row 0, even though that shares the scroll code: that
is how a full-screen editor scrolls (nextvi's `term_room()`), and saving
it would fill the history with successive pictures of the editor instead
of the shell output around it. Not the screen `ED 2` clears either --
xterm's choice too. `ED 3` (`ESC[3J`) clears the history, as in xterm.

### Memory

History is a ring in `.bss`, **one byte per cell**: `put_char()` only
ever stores 0x20..0x7e, so bit 7 is free and carries reverse video, the
only attribute there is. 80 bytes a line, half what `vt_cell_t` would
cost, and lossless.

`.bss` is RAM for the process's whole lifetime (`docs/boot.md`, "Memory
budget"), per `term` instance. Measured at the commit this landed:

| | before | after |
| --- | --- | --- |
| text + data | 100,204 | 116,608 |
| .bss | 21,904 | 34,184 |
| image | 122,108 | 150,792 |
| block (image + 8KB SMALL tier, 4K pages) | 131,072 | 159,744 |

So **+28,672 bytes per term instance**:

- **History:** +16,000 `.bss`.
- **Clipboard buffers:** -4,096 `.bss`. `sel_copy()` and `sel_paste()` each had a 4KB static and are never live at once; they share one now.
- **Other state:** +~400 `.bss` for the scrollbar, panel and selection.
- **Code:** +16.4KB. That is the widget toolkit's buttons and scrollbar, the hardware fill/box/copy primitives that section GC used to drop, and `__divdi3` for the scrollbar's 64-bit arithmetic.

Depth is a build option: `make term SCROLLBACK=1000` (25..8000 lines,
80 bytes each). The shells no longer being in flash is what frees room
on small boards: repl's 368KB block is not in RAM unless a card started
it.

### Addressing lines: two kinds of index

`zvt100` names a line two ways (`sw/common/zvt100.h`):

- **Document index** -- 0 is the oldest retained history line, the live
  screen follows. Dense; what the view offset and scrollbar use.
- **Absolute id** -- `history_pushed - count + document index`. Every
  push shifts every document index by one, but a line's absolute id
  never changes: screen row r is id `pushed + r`, and it is still that
  id after it scrolls into history.

## Resizing

Drag the lower-right corner to make the window **shorter**, down to 3
rows. It keeps its full height by default.

The terminal itself stays 80x25: that size is compiled into the shared
emulator (`sw/common/zvt100.h`, also used by `repl` and `ssh`), and it
is what programs at the far end are told. A shorter window is a
**viewport** onto those 25 rows:

- **It follows the cursor.** Output keeps the prompt on the bottom row;
  `clear` brings the view back to the top; a full-screen program's
  cursor pulls the view to wherever it is.
- **Scrolling is one continuous document.** Shift+PgUp, the wheel and
  the scrollbar first move up through the hidden rows of the screen,
  then into scrollback. A page is the window's height, one line short.
  Scrolling to where the cursor is not in view stops the following;
  Shift+End (or typing) brings it back.
- **The panel and the Open bar pull the view to themselves**: the panel
  to its top (its title and buttons), the bar to the bottom row.
- **Taller than 25 rows** just leaves blank space below. **Narrower than
  80 columns** clips the right-hand side, scrollbar included.

In `term.c`, everything still works in the screen's own rows; only the
pixel placement, which rows are drawn, the mouse and the scrollbar
account for the hidden rows at the top (`vskip`) and how many are shown
(`vis_rows`). Rows outside the window keep their shadow "unknown", so a
blit or a viewport move that brings one into view repaints it. At full
height all of this reduces to what it was before: the render test's
results are identical, line for line.

## Selection and clipboard

Drag with the left button to select, release to keep it. Selection is
**reading order**, not a column rectangle: partial rows at each end,
full rows in between.

| key | does |
| --- | --- |
| Ctrl+Shift+C | copy the selection |
| Ctrl+Shift+V | paste |
| right-click | copy the selection |

**Selection works across scrollback.** Its endpoints are absolute line
ids, so it stays on its text while output keeps arriving and while the
view scrolls -- screen-row endpoints used to leave the highlight
standing still while the text moved out from under it. **Dragging above
or below the text scrolls the view** a line per pointer sample, which is
what makes selecting more than a screen possible. A selection whose
lines are evicted from history is clamped, and dropped once none of it
remains.

**The clipboard holds `Z_WM_CLIP_MAX - 1` (4,095) bytes.** A longer
selection is cut off there. Raising it is a wm-wide change.

**Ctrl+Shift, not plain Ctrl, and that is deliberate.** Ctrl+C in a
terminal is `^C` to the far end -- the single most-used key in a shell --
and rebinding it to copy would be indefensible. `sw/apps/text` uses the
plain Ctrl forms; the difference between the two apps is a decision, not
an inconsistency. `z_kbd_usage_to_keysym()` folds Ctrl+letter to
`0x01..0x1A` regardless of Shift, so the shift bit is what tells these
apart.

Right-click copies on the press -- there is no drag gesture on that
button. It sits after the titlebar guard, so a right-click on the
titlebar does not copy.

Copying strips trailing blanks from each row -- the grid is padded to
the full width, and nothing wants that pasted back. Rows other than the
last get a newline.

Any key other than the clipboard pair and the scrollback keys drops the
selection: the far end is about to echo something.

### Paste makes no assumption about lines

Pasted bytes go out the port exactly as typed keys do. Against `sh`
each newline submits a command, which is correct. Against a reader that
knows it is mid-form, the same bytes accumulate instead. Neither
behaviour belongs to `term`, so it does not have one.

## Reading aloud

With speech on ([tts.md](tts.md)), **Super+A** reads the screen as it
is shown -- the live screen, or the scrollback page being looked at --
top to bottom, a row at a time, skipping blank rows. The rows are fixed
by their line ids when reading starts (see "Addressing lines" above),
so output arriving meanwhile does not shift what is being read. A row
that runs to the right edge is a line the terminal wrapped, and is said
as running on into the next row, without a pause at the wrap. A key
press stops it before the key is typed; so does a click. Super+A again
while it is reading stops it too.

**Super+C** says the highlighted text, once, leaving the clipboard as it
was ("Nothing selected" if there is none); **Super+V** says the
clipboard. The selection is gathered by the same code as a copy
(`sel_gather()`), trailing blanks and all handled the same way.

## Rendering

### The glass model

`shadow[25][80]` records what is **on the screen** for each cell:
character in the low 8 bits, inverted in bit 8. `render()` works out
what each cell *should* show and draws only the cells that differ. A
cell should show:

- **Content:** the emulator's cell at the current scroll offset, from history or the live screen.
- **Selection:** inverted if selected.
- **Cursor:** inverted if the cursor is on it.

Everything that changes the picture goes through that one comparison:
output, the cursor, the selection, scrolling the view, and an overlay
arriving or leaving. There used to be three separate redraw paths
(dirty rows, a separate cursor overlay, direct selection repaints), and
the Open bar bug lived in the gap between them.

**Overlays.** The Open bar owns the bottom row while it is up; the panel
owns a block of cells while it is visible. `render()` never draws a cell
an overlay owns and marks it `GLASS_UNKNOWN` (0xFFFF), a value no cell
can hold. The overlay draws itself afterwards. When the overlay goes,
its cells are simply unequal to what they should show, and come back by
themselves.

**What gets examined.** When live with nothing forcing a full pass, only
rows the emulator marked dirty plus the rows the cursor left and entered
-- a quiet screen costs nothing. Scrolled back, or after a selection or
overlay change, every cell is compared; the comparison is cheap and only
differing cells cost a glyph.

### What a redraw from wm actually invalidates

A `Z_WM_REDRAW` used to invalidate the whole shadow, so a change term
had no part in -- a window dropped next door, a neighbour raised --
cost 2000 glyph blits. At roughly half a millisecond each while the
remote desktop is streaming, that is a second per window; with three
terminals on screen it is the three-second curtain.

wm now says what it invalidated (`z_win_damage_rects()`, `zwin.h`), and
`handle_redraw()` treats the three answers differently:

| | |
|---|---|
| `-1` | everything: `shadow_invalidate()`, and the next frame clears first. |
| `0` | nothing. **Return without invalidating anything** -- every pixel this window owns is still on the glass. The frame that follows is an ordinary dirty-cell render, which matters: output that arrived in the same breath as the redraw must not be lost to an empty damage. |
| `n` | those rectangles: `cell_range()` turns each into a cell range and only those cells stop describing the glass. |

Either way `drawn_valid` is cleared, so the frame that follows does not
hardware-scroll: the shadow of the damaged cells no longer describes
pixels, and shifting it for a blit over unknown content is exactly the
stale-text failure the shift/blit agreement rule below exists to
prevent.

The overlays are the exception, and deliberately crude: the Open bar is
one row and the panel one block, so both are marked dirty whole rather
than intersected with the damage. The arithmetic would cost more than
the glyphs.

**A marked cell outside every visible rectangle is not painted, and its
shadow still says it was.** That is the contract rather than a bug:
those pixels belong to the window in front, and when this window gets
them back, wm names them in a redraw's damage -- which is what turns
them back into `GLASS_UNKNOWN`.

### A full repaint is a fill plus the ink

When the damage really is "everything", painting 2000 cells one blit at
a time is still the wrong shape. A blank cell is the background colour
and so is a filled rectangle, so `render()` fills the whole grid once --
`z_win_clear()`, which walks the visible region like every other
primitive -- and then records that every cell is blank. The comparison
is unchanged and draws the difference, which for a shell prompt is a
hundred glyphs rather than two thousand.

It must be `z_win_clear()` and not `z_fb_fill_rect()`: the latter is
the software path, one `z_fb_set_pixel()` per pixel, and 400x200 of
those took four seconds -- worse than the two thousand blits it was
meant to replace. The fill also clears the scrollbar strip, which is
why the scrollbar is marked dirty alongside it.

The fill happens **only** when wm named no damage. A damage-limited
redraw must not clear what it was not given, and is already cheap.

**An idle terminal opens no paint session at all.** Walking the region
to look at two thousand cells that are all up to date cost 6% of the
CPU per terminal at thirty frames a second -- three of them took the
`gpu3d` cube from 75fps to 50. The comparison marks what needs painting
first; if nothing does, there is no session and no walk.

### Nothing is drawn while frozen

`frame()` returns immediately when `z_win_frozen()` (`zwin.h`) is true.
wm freezes every window for the duration of a drag so the screen holds
still under its rubber band, and nothing drawn then reaches the glass.

Returning is the point, not an optimisation. Everything below that test
*records* what it drew -- the shadow, the overlays' dirty flags, the
emulator's dirty rows, which `render()` clears as it goes. Run while
frozen it would record cells as shown that were never drawn, and the
only way back from that is a full repaint of the window, which is
exactly what wm's `DREW` ack asks for. Every terminal on screen used to
run this thirty times a second during a drag, and the release repainted
all of them at once.

Skipping leaves the work pending. The dirty flags stay set, the shadow
goes on describing the glass correctly, and the first frame after the
thaw draws exactly the cells that changed during the drag -- a line of
output rather than a screen.

### The hardware scroll

A scroll changes every cell of the model, but the pixels that survived
are already correct, just in the wrong place.

**How it works.** `render()` moves them with `z_fb_hw_scroll()` (a VRAM-to-VRAM blit), shifts the shadow by the same amount, and lets the comparison draw only what scrolled in.

**Cost.** 1.07ms instead of 4.67ms for a full screen (`docs/gpu_blitter.md`).

**When it applies.** Only in the two cases where the **whole picture moves uniformly**:

- **Live before and after, and the screen scrolled.** That includes `DL` at row 0, how vi scrolls, which is why `DL` routes through `scroll_up()` and its counter.
- **No output since the last frame, and the view offset changed.** This covers Shift+Up/Down, PgUp/PgDn and scrollbar drags.

Output arriving while scrolled back, or a history clear, is not a uniform shift; the full comparison handles those without a blit.

**Never while an overlay is up.** Its pixels would travel with the text and nothing would put them back.

**The blit and the shift must agree exactly.** If they disagree the comparison finds cells equal that are not, and the terminal shows stale text with no way to notice.

That is also why term calls **`z_fb_hw_scroll_allowed()`** first:

- `z_fb_hw_scroll()` returns `void` and silently does nothing for a partially covered window, because the blitter cannot clip a copy (`copy_region_allows_rect()`, `sw/common/zgfx.c`).
- A caller that shifted its shadow anyway would be wrong in exactly the way described above.
- Occluded, term repaints instead.

This path was `if (0 && ...)` until now, disabled with a note pointing
at `sw/apps/text`. `text` has since re-enabled its own scroll blit after
the blitter's stale-ack fix (`ST_MEM_SETTLE1/2` in `rtl/gpu/gpu_blit.v`,
`docs/gpu_blitter.md`), and that is what term relies on too. **It needs
a bitstream with that fix** -- `make flash`, not `make dev-flash`, if
the gateware is older.

*Note for `sw/apps/text`:* its `scroll_repaint()` calls
`z_fb_hw_scroll()` without the check, so a partially covered `text`
window draws only the rows that scrolled in. Checking
`z_fb_hw_scroll_allowed()` and falling back to `repaint()` is the fix.

## Testing

**`zvt100`** -- `cd sw/test && make test-zvt100`. Host-only, covering
the parser and the history ring: wraparound, eviction, stable ids, DL
not saving, ED 2 / ED 3, reverse video surviving packing.

**`term` itself** -- `sw/apps/term/tests/render.c` builds the real
`term.c` against the software framebuffer in
`sw/common/tests/zrender.h` and a scripted kernel (a mailbox, a pid
registry, a clock, providers that answer or refuse CONNECT):

```
sudo sysctl -w vm.mmap_min_addr=0
cc -std=gnu99 -Wall -no-pie -I sw/common -o /tmp/term_render \
   sw/apps/term/tests/render.c sw/common/zwin.c sw/common/zwidget.c \
   sw/common/zfont_data.c sw/common/zobj.c sw/common/zeitlos.c \
   sw/common/zvt100.c sw/common/zport.c sw/common/zcfg.c \
   sw/common/zsayall.c sw/common/zspeak.c
/tmp/term_render /tmp/term        # exit 0 pass, 1 fail, 77 skipped
```

After **every frame** of every scenario it recomputes, without term's
own helpers, what each uncovered cell should show and compares that
with both term's shadow **and the actual pixels**. The pixel comparison
is the one that matters: it catches a blit and a shift that disagree,
which nothing else would.

The scenarios:

- the start panel before and after `repl0` registers
- clicking REPL
- scrolling output one line and several lines per frame
- the Open bar dismissed by Esc and by a successful connect
- output written under the bar
- Shift+PgUp/Up/Down/Home/End
- output while scrolled back
- a drag-scrolled selection across history and its copy
- a partly occluded window, which refuses the blit exactly as hardware does
- a wm redraw with the bar up
- F12, a refused connect, and a far-end close
- hiding and re-showing the panel
- auto-connect: to a registered provider, waiting for one to register,
  timing out, Esc cancelling, a bad value, and F12 not re-triggering it
  (the scripted kernel answers `CFG_GET`)
- a short window: the panel in it, connecting from it, output one and
  five lines per frame, `clear`, Shift+PgUp into history, output while
  scrolled back, Shift+End, the Open bar, and back to full height

**Known failures.** As of the console and resizing work, 149 of the
276 checks fail, all of them in scenarios 2-10 and all present before
that work (same list, line for line, with and without it). The first is
right after clicking REPL: the shadow is correct but about 60 pixels at
the panel's left edge (row 17, column 12) stay set, and that follows
every later frame. Either the panel's box is not fully erased when it
hides, or the harness disagrees with the real drawing path; it has not
been investigated. Separately, scenario 8's redraw moves the window to
0,0 and back without moving its visible region, which clips everything
after it at y=214; the short-window scenario sets the region properly
(`redraw_at()` in the test) and passes in full.

It also writes PBM renders (`/tmp/term-*.pbm`) to look at.

What it cannot catch: anything in `zgfx.c` itself or the blitter RTL,
since the pixel primitives are software there (see `zrender.h`).

## Escape sequences the emulator implements

`sw/common/zvt100.c`:

| | |
|---|---|
| `A` `B` `C` `D` | cursor up/down/right/left |
| `H` `f` | cursor position |
| `J` | erase in display (0, 1, 2; **3 clears scrollback**) |
| `K` | erase in line |
| `m` | SGR -- attributes |
| `L` | insert lines (IL) |
| `M` | delete lines (DL) |

**`L` and `M` were added for `vi`, and their absence looked like a
different bug entirely.** nextvi scrolls the screen by emitting `ESC[nM`
and `ESC[nL` (`term_room()`) and never by redrawing -- so moving past the
last line repainted only that line and the rest of the display stood
still. `DL` at the top of the screen routes through `scroll_up()`, which
is what gives the renderer its scroll count and therefore the blit.

**Not implemented, and worth knowing before the next port:** DECSTBM
(`ESC[r`, scrolling regions), insert/delete characters (`@`, `P`),
save/restore cursor (`s`, `u`), and the alternate screen buffer
(`ESC[?1049h`). nextvi emits the last of these only with `-a`, which
this front end does not pass. An alternate screen, if added, should not
feed history either.

## Line editing lives at the far end

`term` is a **dumb terminal**. `key_to_bytes()` already sends arrows,
Home/End, PageUp/Down and Delete as proper VT100 sequences. Nothing
about cursor movement or insert-in-the-middle is missing here.

What reads those bytes decides what they mean:

- **`readline()` (`sw/common/zeitlos.c`)** -- the serial console's
  reader. Deliberately minimal: Enter and backspace, nothing else. The
  console is the recovery path of last resort. It swallows escape
  sequences rather than appending them to the line.

- **`zline` (`sw/common/zline.h`)** -- the line discipline for port
  providers, used by `repl` and `posix`: cursor movement, insert,
  delete, kill and history. See `docs/line_editing.md`.

Multi-line entry needs the reader to own a multi-row buffer and repaint
it across several terminal rows. That belongs with `zline`, not here.
