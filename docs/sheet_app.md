# sheet

A spreadsheet. `sw/apps/sheet`.

```
> run wm
> run sheet
```

26 columns (A–Z) by 999 rows, formulas, and a grid that works with or
without a mouse. Resizable; 360x240 is the floor, pinned with
`Z_WIN_FLAG_MIN_IS_CREATE` because below it the row header plus two
columns plus a scrollbar stop fitting across, and the titlebar stops
having room for its five icons.

```
+-----------------------------------+
| A1  =SUM(B1:B9)                   |   edit bar
+---+-------+-------+-------+---+---+
|   |A      |B      |C      |   |#|     column headers
| 1 |Item   |  12.5 |       |   | |
| 2 |Widget |   8.0 |       |   | |
| 3 |Total  |  20.5 |       |   | |
+---+-------+-------+-------+---+-+
|  <horizontal scrollbar>       |     |
+-----------------------------------+
```

## The model is in its own file

`sheet_core.c` holds cells, formulas, evaluation and the file format,
and contains no drawing, windows or messages. `sheet.c` is the window.

```
cd sw/apps/sheet
make test          # the model:      126 checks
make test-layout   # the geometry: 12,817 checks
make test-files    # the file paths:  111 checks
make render        # draws the panel to /tmp/sheet.pbm
```

A spreadsheet that is subtly wrong is worse than no spreadsheet,
because the whole point of one is that nobody re-checks its
arithmetic. The split is what makes that demonstrable rather than
asserted.

The tests are written against the **cell grid**, not against the
parser. A formula is only correct in the context of the cells it
reads, and calling the expression parser in isolation would never
catch a stale cached value or a range that walks the wrong row.

## Fixed point at two precisions

`sw/common/zfix.c`, shared with `sw/apps/calc` — see
`docs/calc_app.md` for why fixed point at all. What is new is that the
number of decimal places is a **parameter** rather than a compile-time
constant:

```c
z_fix_mul(a, b, dp, &out);
z_fix_format(v, dp, places, out, cap);
```

calc runs at 6 places against a 10-digit display; sheet runs at 4
(`SHEET_DP`). A spreadsheet holds money and counts far more often than
the result of dividing by three, and the two places traded away buy
two more digits of integer range.

**Mixing precisions is the caller's problem.** Nothing records which
`dp` a value was made at, so handing a 6-place value to a 4-place
multiply is quietly wrong. Each app picks one and uses it everywhere.

The extraction paid for itself immediately. `z_fix_format_len()` —
which counts a number's width without writing it, so a column can
decide whether the value fits — hung forever the first time it ran.
The emit macro guarded its store with `if (out && ...)`, two call
sites passed `tmp[--t]`, and with `out == NULL` the argument was never
evaluated, so the digit counter never decremented. The bug had been
sitting harmlessly in `calc` for as long as the code existed, because
`calc` never formats without a buffer.

## Storage

Sparse. A 26x999 dense grid is 26,000 cells and there is not the
memory for it; a sheet with nine numbers in it should cost what nine
numbers cost.

Cells live in one flat array sorted by `row * 26 + col`, found by
binary search and inserted by `memmove` — the same deliberate trade
`sw/apps/text` makes for its document buffer, and for the same reason:
the worst case is a few milliseconds on a rare operation, against a
second representation of "where the data is" that every function would
have to understand.

| | |
|---|---|
| `SHEET_MAX_CELLS` | 1024 non-empty cells (16 bytes each) |
| `SHEET_ARENA` | 16KB of cell source text |
| `SHEET_SRC_MAX` | 96 bytes per cell |

`sizeof(sheet_t)` is 32,816 bytes — 16KB of cells at 16 bytes each,
16KB of arena, and the column widths. The same order as `text`'s
document buffer and line table. A full 26-column block is 39 rows deep at that
cap; past it, an edit fails cleanly (`#FULL`, and a dialog) rather
than losing data quietly. An existing cell can still be edited or
cleared when the array is full, which is what makes a full sheet
recoverable rather than stuck.

Source text is an **append-only arena**: overwriting a cell orphans
its old text rather than moving anything, and the arena is compacted
when it fills. Compaction has to move live strings in ascending offset
order or a string gets copied over one not yet moved — and cells are
sorted by key, not offset, so the order is worked out with a
stack-local index array and a shellsort. 2KB of a 16KB stack, taken at
the top of a `sheet_set()` call and released immediately; the
alternatives were a permanent 2KB of `.bss` for something that runs
rarely, or a repeated minimum scan at 1024² comparisons.

`make test` rewrites one cell 4,000 times specifically to force
compaction, and checks the cells around it are undisturbed.

## What a cell holds

Exactly what you typed. Whether it is a number, a formula or a label
is decided by looking at the text, by one rule:

| | |
|---|---|
| leading `=` | formula |
| leading `'` | text, forced |
| parses whole | number |
| otherwise | text |

The apostrophe is not part of the text; it is how you store `007` or
`1-2` as a label rather than having it read as a number. `3 apples` is
already text without it — "parses whole" means the *entire* string,
not a number followed by something else.

**That one rule is shared by the editor, the loader and the saver**,
which is what makes the file format identical to what you type.

Trailing whitespace is dropped on the way in. It is invisible in the
grid and in the file, so keeping it would mean two cells that look
identical comparing unequal, and a file that grows every time it
round-trips through an editor that strips it.

## Formulas

```
=1+2*3            operators + - * /, precedence, parentheses
=-A1              unary minus
=A1+B2            references, case-insensitive, $ accepted and ignored
=SUM(A1:A13)      ranges, in function arguments
=SUM(A1:A3,100,B1)  mixed ranges and values
=SUM(A1:A9)/COUNT(A1:A9)   nesting
```

`SUM` `AVG` (or `AVERAGE`) `MIN` `MAX` `COUNT` `ABS` `INT` `ROUND(x,n)`.

`$` is accepted and ignored rather than rejected: nothing here copies
formulas between cells, so absolute and relative mean the same thing,
and refusing to load a file over a distinction with no effect would be
the worse answer. A two-letter column (`AA1`) is `#REF` — a reference
to somewhere this grid does not have, which is a better answer than a
syntax error in a confusing place.

**Text and empty cells inside a range are skipped, not counted as
zero.** That distinction is the whole difference between `AVG` and
"the sum over the cells I selected", and getting it wrong makes an
average of three numbers in a ten-cell selection come out as three
tenths of the right answer. `SUM` of nothing is 0; `AVG` of nothing is
`#DIV0`, because an average of nothing is not zero, it is a question
with no answer.

`INT` rounds toward negative infinity — `INT(-1.5)` is `-2`, matching
every spreadsheet there has ever been. C's division truncates toward
zero, so that needs an extra step.

### Errors are values

Cached like any other result and propagated through anything that
refers to them, so one bad cell shows up everywhere it actually
matters rather than zeroing itself out silently.

| | |
|---|---|
| `#ERR` | unparseable formula |
| `#DIV0` | division by zero |
| `#CIRC` | a cell that refers back to itself |
| `#NAME` | unknown function |
| `#REF` | a reference outside the grid |
| `#NUM` | overflow, or a bad argument |
| `#DEEP` | more than 24 levels of reference |
| `#FULL` | out of cells or arena |

All at most five characters, because they have to fit a column.

### Cycles

Detected by a **per-cell state flag**, not a dependency graph. A
formula that reaches a cell currently being evaluated has by
definition come back to where it started, so the check is one
comparison at the point it matters, and there is no graph to build,
invalidate or get wrong.

`=SUM(A1:A9)` written *into* A1 is a cycle, and it is the easy way to
write one by accident — putting a total at the foot of the column it
totals and catching one row too many. It reports `#CIRC` rather than
quietly evaluating.

Breaking a cycle has to actually clear it, which is the case a cached
error would get wrong; that is a test.

Depth is capped at `SHEET_MAX_DEPTH` (24). An app gets its heap and
stack out of one 16KB allocation (`Z_PROC_STACK_SIZE_DEFAULT`), so an
unbounded evaluator is a silent stack overflow rather than an error
message. A depth failure is **not cached** — it is a property of the
path taken to reach a cell, not of the cell, so caching it would make
an unrelated later evaluation wrong.

### Recalculation

On demand, memoised. Any edit resets every cached value: a walk of the
cell array, so it costs the number of cells that exist rather than the
number that could.

An **invariant worth knowing before changing anything in
`sheet_core.c`**: evaluation never moves a cell. It writes the cached
value, error and state, and nothing else. Both the range walker (which
holds an index while evaluating cells that may refer anywhere) and the
parser (which holds a pointer into the arena across nested
evaluations) depend on it. Anything that could insert, remove or
re-home a cell during evaluation breaks both, silently, and the
symptom would be a wrong number rather than a crash.

## The file format

`.ZSS`. Line-oriented text, one line per non-empty cell, sparse:

```
zsheet 1
!w A 12
A1 Item
B1 Cost
A2 widget
B2 12.5
A3 gizmo
B3 8.25
A4 'total
B4 =SUM(B2:B3)
```

`REF`, one space, then **exactly what you typed** — including further
spaces, which is how a label with leading whitespace survives. No
quoting and no escaping anywhere, because a cell cannot contain a
newline and the line always starts with a reference. An empty sheet is
nine bytes.

- `!` starts a directive. `!w <col> <n>` sets a column width; anything
  else is ignored rather than refused, so a directive from a later
  version costs an old build nothing but the line.
- `#` starts a comment, and blank lines are fine.
- Leading whitespace is skipped, and CRLF is tolerated without the
  `\r` reaching the cell.
- The `zsheet 1` header is optional on the way in. Its version is not
  checked against anything: there is one version, and a file claiming
  a later one is still worth trying to read, since the worst case is
  that some lines are skipped.

**A bad line does not fail the file.** One malformed line is skipped
and the rest loads — refusing a whole document because line 40 is
wrong would be the wrong trade for a format people are invited to
hand-edit. `make test` loads a file with a comment, a blank line, an
indented cell, CRLF, an unknown directive and a garbage line, and
checks that the *good line after the garbage one* still arrives.

### CSV

Read and written for interchange, and mapped to this app in
`sw/common/ztype.c`, so double-clicking a `.csv` in the file browser
opens it here. `text` still opens one directly, the same arrangement
`.md` already has with `read`.

Formulas are written as their **values**, which is what every other
tool expects to read out of a `.csv`. That is exactly why CSV is not
the native format: saving to it would quietly destroy the model.

Reading is the other way round — a field is taken as cell source
verbatim, so a CSV that happens to contain `=SUM(A1:A9)` loads as a
formula, which is what someone who typed that into a CSV by hand
meant.

### Both directions are streamed

The writer takes an emit callback and the reader takes one line at a
time; neither ever holds more than a line. This is not a stylistic
choice: `fs_mallocfile()` (`zfsapp.h`) allocates the file's size on
the heap, and the heap here shares 16KB with the stack. A sheet is
easily bigger than that.

## Keyboard

Everything works without a pointer.

| | |
|---|---|
| arrows | move the cursor |
| shift+arrows | extend the selection |
| Tab / shift+Tab | right / left |
| Enter | down |
| PageUp / PageDown | a screen at a time |
| Home / End | first column / last used column |
| Ctrl+Home | A1 |
| any printable character | starts a fresh entry, replacing the cell |
| `=` | starts a formula (it is just a printable character) |
| F2 | edit the cell's existing contents |
| Delete / Backspace | clear the selection |
| Escape | cancel the entry in progress |
| Alt+Left / Alt+Right | narrow / widen the current column |
| Ctrl+C / Ctrl+X / Ctrl+V | copy / cut / paste |
| Ctrl+N / Ctrl+O / Ctrl+S | new / open / save |

While typing, Up and Down **commit and move**, the way a spreadsheet
does — typing down a column should not need Enter between every cell.

Alt+Left/Right for column width exists because there is no unmodified
key spare: every printable character starts an entry. A mouse-only way
to change a column would be a dead end on a machine with no pointer,
and keyboard-only operation is first-class here (see
`docs/window_manager.md`).

## Mouse

Click a cell to select it, drag to select a range, drag a column
header's right edge to resize that column. Clicking the edit bar opens
the current cell for editing — the pointer equivalent of F2.
Shift+click extends a selection.

`Z_WM_MOUSE` carries buttons but no modifiers, so shift+click is
resolved against the modifier state from the last key event. Modifier
*releases* are tracked for that reason: a shift let go between a key
and a click would otherwise leave the state stale and turn the next
plain click into a shift+click.

## The edit bar

Editing happens in the bar at the top, not in the cell. Every
spreadsheet before the mid-eighties worked this way, and here it is
the right answer for a concrete reason rather than nostalgia: a
formula is routinely wider than the column it lives in, and an in-cell
editor would either have to overflow into its neighbours — repainting
cells that are not its own — or scroll a nine-character window over
the text being typed. The bar has the whole width of the window.

The reference box on its left shows the cursor, or the range when
there is a selection (`B3:C5`).

## Display

Numbers are fitted to their column: full precision if it fits, then
progressively fewer decimals, and finally a row of `#` if even the
integer part does not. **Never a truncated number** — `1234` in a cell
holding `1234567` is a lie, where `####` is a request for a wider
column. Text is truncated; numbers and errors right-align, text left.

The cursor cell is drawn **normal inside a multi-cell selection**, so
it reads as a hole in the highlight. That is how every spreadsheet
marks the active cell of a range, and it is the only way to see where
typing would go without inventing a second kind of marker. A
single-cell selection *is* the cursor, so there it inverts.

The cursor's row and column headers invert too, which is most of what
makes the grid readable without a pointer to look at.

### Two kinds of repaint

A clear followed by a redraw is a visible **flash** on this display,
so there are two:

- `repaint_view()` draws everything without clearing first. Every
  function it calls fills its own background — the edit bar its strip,
  the headers their row and column, the grid its cells and the slack
  past the last of them, each scrollbar its own track — so nothing
  stale can survive *as long as the geometry has not changed*.
- `repaint()` clears first, for the cases where it has: a resize, a
  font switch, a `Z_WM_REDRAW`, or a dialog that has just closed over
  the window.

The paths that run at pointer rates — a drag extending a selection, a
scrollbar being dragged — take the first one, and so does committing a
cell entry, which is the single most common thing anyone does here.

Columns scroll over the whole grid; rows only over what is used plus a
margin. The asymmetry is deliberate: 26 columns divided by the handful
on screen still leaves a thumb big enough to grab, so a pointer can
reach column Z, whereas 999 rows would leave a thumb about two percent
of the track — unusable for the twenty-row sheet that is the common
case. A keyboard reaches every row regardless, since the cursor
scrolls the view with it.

Both fonts (`z_font_5x8`, `z_font_6x12`) are resident in glyph memory,
so the titlebar font button switches between them and both still
render through the hardware blitter. Switching changes the column
count, the row height and which cell a click lands on — everything is
recomputed in `layout()`.

## Looking at it before it reaches a screen

```
make render
```

Draws the real panel — `sheet.c`, `zwin.c` and `zwidget.c` are the
actual sources, only the pixel plotting is software — to a PBM. See
`sw/common/tests/zrender.h`.

It earned its place on the first render. Two things were wrong that no
assertion would have caught: the corner box above the row numbers was
filled solid ink, which made it the heaviest thing on the screen while
labelling nothing, and the cursor cell was indistinguishable from the
rest of a multi-cell selection.

`make test-layout` is the other half, and does not replace it. The one
worth having above all others there is the **hit-test round trip**:
drawing a cell and clicking one are two separate walks over the same
variable-width column geometry, and the failure when they disagree is
not a crash — it is a grid where clicking a cell selects its
neighbour, which gets lived with and blamed on the mouse rather than
reported. `sw/apps/hex` has the same test for the same reason.

`make test-files` is the third piece, and covers what neither of the
others can reach: `sheet.c`'s own chunked reader and streamed writer.
That splitter is worth its own tests because its failure mode is
invisible in the common case — a line only straddles a 256-byte read
boundary depending on how long the lines before it were, so a sheet
can round-trip perfectly for months and then lose a cell when somebody
renames a label. It sweeps every line length across the seam, plus a
file with no trailing newline (forgetting to flush the last line loses
exactly one cell — very often the total), CRLF, and a 120-row sheet
written and read back cell for cell.

`make test-layout` found a real bug at the smallest window size wm
allows. `layout()` clamped the grid to a minimum of one row and one character, and when
the edit bar, headers and scrollbars already used more than the window
had, that minimum made the grid *extend past* the content area instead
of shrinking — drawing over the scrollbar, and answering hit tests for
coordinates that were not the grid's. Zero is the honest answer at
that size. The window cannot actually get there
(`Z_WIN_FLAG_MIN_IS_CREATE` pins the floor), which is exactly why it
was worth fixing: an unreachable size that underflows is a bug waiting
for the day the floor changes.

## Not implemented

No cell formatting (a number is shown as short as it can be, so `2.00`
displays as `2`), no row insert/delete, no sort, no charts, no
multiple sheets, no undo. Each wants a real design rather than a
corner of this file.

`ROUND(x, n)` is there for when a specific number of decimals actually
matters in a cell.
