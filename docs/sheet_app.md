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

## Using it

**F1** opens this page, here.

Move with the arrows, Tab, Enter, PageUp/PageDown, Home/End; type into
the cell under the cursor -- a number, text, or a formula starting with
`=`. Shift+arrows (or a drag) select a range. Everything works without
a pointer.

**/** opens the command line in the edit bar, as in VisiCalc and 1-2-3:

```
/ Insert Delete Number Width Sort Extend Undo Redo Goto Find Chart Help
```

The first letter (or the arrows and Enter) picks one; Esc backs out.
Insert and Delete ask *Rows* or *Columns*, Sort *Ascending* or
*Descending*, Extend *Down* or *Right*. To type text that starts with
`/`, start it with `'` (`'/2` is the text `/2`).

| key | does |
|---|---|
| arrows, Tab / Shift+Tab, Enter | move (right / left, down) |
| Shift+arrows, mouse drag | select a range |
| PageUp / PageDown, Home / End, Ctrl+Home | a screen; first / last column; A1 |
| any printable character | starts a fresh entry, replacing the cell |
| F2, or a click on the edit bar | edit the cell's contents |
| Escape | cancel the entry |
| Delete / Backspace | clear the selection |
| Ctrl+Z / Ctrl+Y (Ctrl+Shift+Z) | undo / redo -- every change, as far back as memory allows |
| Ctrl+C / Ctrl+X / Ctrl+V | copy / cut / paste -- formulas move with the paste |
| Ctrl+D / Ctrl+R | fill the selection down / right from its first row / column |
| Ctrl+1 | number format for the selection |
| Ctrl+G | go to a cell |
| Ctrl+F, F3 | find text or a value; find the next |
| F10 | chart the selection (or show the sheet's chart) |
| Alt+Left / Alt+Right | narrow / widen the column |
| Ctrl+N / Ctrl+O / Ctrl+S | new / open / save |
| F1 | this page |

While typing, Up and Down **commit and move**, the way a spreadsheet
does -- typing down a column should not need Enter between every cell.

### The edit bar

Shows the cell's reference and what it holds -- or, for a range, its
**sum, average and count**: select a column of numbers and the total
is there without a formula.

### Formulas

```
=B2*C2              + - * /, ^ (power), parentheses
=A1>100             comparisons = <> < > <= >= give 1 or 0
=SUM(B2:B20)        a range
=IF(C2=0,0,B2/C2)   the branch not taken is not evaluated
=PMT(0.05/12,360,200000)  a 30-year loan: -1073.64 a month
```

| | |
|---|---|
| `SUM` `AVERAGE` (`AVG`) `MIN` `MAX` `COUNT` | of ranges and values; text and empty cells skipped |
| `ABS` `INT` `ROUND(x,n)` `MOD(x,y)` `SQRT` `POWER(x,n)` | `INT` rounds down, `MOD` takes the divisor's sign |
| `IF(c,a,b)` `AND` `OR` `NOT` `TRUE` `FALSE` | logic, as 1 and 0 |
| `PMT(rate,n,pv[,fv])` `PV(rate,n,pmt[,fv])` `FV(rate,n,pmt[,pv])` `NPV(rate,values...)` | loans and savings; money paid out is negative |
| `VLOOKUP(x,range,col[,0])` `INDEX(range,row[,col])` | a table's value: exact with 0, else the largest not above `x` |

Numbers are kept to **eight decimal places** and up to 92 thousand
million either way: a result past that is `#NUM`, never a wrong number.

**References move when they are copied.** `=A1*2` pasted one row down
becomes `=A2*2`; a `$` pins the part after it -- `$A$1` never moves,
`$A1` keeps its column, `A$1` its row. Inserting and deleting rows and
columns moves every reference that points past them; a reference to a
deleted cell becomes `#REF!`.

### Number formats

Ctrl+1 (or /Number) formats the selection:

| type | shows 1234.5 as |
|---|---|
| `G` | 1234.5 (general: as short as it can be -- the default) |
| `F2` | 1234.50 |
| `,2` | 1,234.50 |
| `$2` | $1,234.50 |
| `%1` | 123450.0% (for 0.125: 12.5%) |

The digit is the number of decimals (0-8). A number too wide for its
column shows `####` -- never a truncated number.

### Charts

Select a table and press **F10**: the first column, if it has text,
names the categories; the first row, if it has text, names the series;
every other column is a series (up to six). The chart takes the window.
**B**ar, **S**tacked, **L**ine or **P**ie switches its kind; Esc goes
back to the grid. The chart is saved with the sheet, and F10 with no
selection shows it again -- after the numbers have changed, too.

Series are told apart by **patterns** -- solid, hatching, grey, lines --
and, in a line chart, by marker shapes, the way charts were drawn on a
one-bit screen or a laser printer.

### Undo

Every change can be undone -- typing, pasting, clearing, filling,
sorting, inserting and deleting rows and columns, formats and widths
-- and redone. History is kept in the memory the machine can spare
(512KB on most boards; 16KB on one without an arena) and the oldest
steps go first when it fills; it is cleared when another file is
opened.

### Other formats

`.csv` opens here; `.xlsx`, `.ods` and `.tsv` open and save through
`convert` (below, and [convert.md](convert.md)).

### Limits

26 columns (A-Z) by 999 rows; 4,096 non-empty cells, 40KB of cell text
in all, 95 characters a cell; 64 formatted ranges; one chart.

## The model is in its own file

`sheet_core.c` holds cells, formulas, evaluation and the file format,
and contains no drawing, windows or messages. `sheet.c` is the window.

```
cd sw/apps/sheet
make test          # the model:      236 checks
make test-layout   # the geometry: 12,817 checks
make test-files    # files, and editing through the keys: 142 checks
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

calc runs at 6 places against a 10-digit display; sheet runs at 8
(`SHEET_DP`). It ran at 4 until 0.0.7, for integer range -- and a
monthly interest rate, 0.05/12, came out as 0.0042: a 30-year mortgage
payment five dollars out. At eight places it is right to the cent.

The range the four places bought was only ever needed because
`z_fix_mul()` checks its double-scaled intermediate, not its result
(at eight places it refuses 4,000 x 4,000). `sheet_core.c` multiplies
with its own `fx_mul()`, which splits each operand at the decimal point
and fails only when the *product* does not fit, and divides by long
division; the range left, +-92 thousand million, is ample for the books
this is for, and past it is `#NUM`.

The financial functions and `^` are the one place doubles are used:
`(1 + rate) ^ periods` and the `(g - 1) / rate` of an annuity magnify
the last fixed-point digit into cents. They are computed in software
double precision (a few KB of libgcc) and rounded back to eight places,
like every other result.

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
| `SHEET_MAX_CELLS` | 4096 non-empty cells (16 bytes each) |
| `SHEET_ARENA` | 40KB of cell source text |
| `SHEET_SRC_MAX` | 96 bytes per cell |
| `SHEET_MAX_FMTS` | 64 formatted ranges |

`sizeof(sheet_t)` is about 107KB: 64KB of cells, 40KB of arena, the
column widths, the formats and the chart. 0.0.6 had 1,024 cells and
16KB -- 39 full rows, a demonstration rather than a ledger; 4,096 is
800 rows of five columns. Past either limit an edit fails cleanly
(`#FULL`, and a dialog) rather than losing data quietly. An existing cell can still be edited or
cleared when the array is full, which is what makes a full sheet
recoverable rather than stuck.

Source text is an **append-only arena**: overwriting a cell orphans
its old text rather than moving anything, and the arena is compacted
when it fills. Compaction has to move live strings in ascending offset
order or a string gets copied over one not yet moved — and cells are
sorted by key, not offset, so the order is worked out with a
stack-local index array and a shellsort: 8KB at 4,096 cells, which is
why the app asks for a 64KB stack (`APP_STACK`). The alternatives were
a permanent 8KB of `.bss` for something that runs rarely, or a repeated
minimum scan at 4096² comparisons.

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

The grammar, lowest precedence first: comparisons (`= <> < > <= >=`,
giving 1 or 0), `+ -`, `* /`, `^` (whole powers), unary `- +`, then
numbers, references, `(...)`, `TRUE`/`FALSE`, `#REF!` and function
calls. The functions are listed under "Using it".

`IF` parses both branches -- a formula is checked whole -- but
evaluates only the one taken: the other runs in a *skip* mode in which
references are not followed and arithmetic errors do not count. That
is what makes `=IF(B1=0,0,A1/B1)` a number rather than `#DIV0`.

`$` is meaningful since 0.0.7, because things now copy formulas
(paste, fill, sort): see "Moving formulas" below. A two-letter column (`AA1`) is `#REF` — a reference
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

On demand, memoised. Any edit makes every cached value stale -- but
without visiting the cells: each cached state carries the six-bit
*generation* it was computed in, and an edit moves the sheet to the
next one, so every older value reads as not yet computed. Only when the
count wraps (once in 64 edits) are the cells walked. Before 0.0.7 every
edit walked every cell, which made loading a file -- an edit per cell
-- quadratic: 4,096 cells would have been 16 million steps.

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

- `!` starts a directive. `!w <col> <n>` sets a column width;
  `!f A1:B9 ,2` formats a range (0.0.7; later lines win); `!g bar
  A1:D9` is the chart (0.0.7: `bar`, `stack`, `line`, `pie`). Anything
  else is ignored rather than refused, so a directive from a later
  version costs an old build nothing but the line -- a 0.0.6 sheet app
  reads a 0.0.7 file, without its formats and chart.
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

### xlsx, ods, tsv

Through `convert` ([convert.md](convert.md)). Opening one -- from the
Open dialog or by double-clicking it in `files` -- converts it into a
`.zss` in `/tmp` and opens that in a sheet of its own; its first
**Save** asks where to keep it, starting in `/home`. **Save As** onto a
name ending `.xlsx`, `.ods` or `.tsv` exports: the sheet is written to
`/tmp` and converted, with `convert`'s window for progress and Cancel,
and the document keeps its own name. Formulas the sheet evaluates
survive both ways; others arrive as their values. The sheet's limits
apply to what comes in: A-Z, 1-999.

### Both directions are streamed

The writer takes an emit callback and the reader takes one line at a
time; neither ever holds more than a line. This is not a stylistic
choice: `fs_mallocfile()` (`zfsapp.h`) allocates the file's size on
the heap, and the heap here shares 16KB with the stack. A sheet is
easily bigger than that.

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

## Moving formulas

`sheet_rewrite()` is one tokenizer for every way a formula moves:

- **copying** (paste, fill, sort) moves the relative parts of each
  reference by the distance moved; `$` parts stay;
- **inserting** rows or columns moves every reference at or past them,
  absolute or not, since the cell it named has moved;
- **deleting** turns a reference to a deleted cell into `#REF!`, and
  clips a range that loses an end -- `SUM(A2:A9)` with rows 3-5 deleted
  is `SUM(A2:A6)`; a range wholly deleted is `#REF!`.

A reference is a letter, digits, and optional `$`s, not glued to a
name: `LOG10(` and `1.5E3` are left alone. Format ranges and the
chart's range move with the cells.

Inserting refuses before it starts if it would push a non-empty cell
off the grid. Deleting cannot fail.

Copying within the sheet carries cells, not text: Ctrl+C keeps the
range's cells in the app's memory as well as putting tab-separated
text on the system clipboard, and Ctrl+V uses the cells -- formulas
moved -- when the clipboard still holds exactly that text (a checksum
says so); otherwise it pastes the text, from anywhere. A selection a
whole multiple of what was copied is filled with copies.

## Undo

Two stacks of records -- undo and redo -- each record what a change
overwrote: the **cells of a rectangle** (an edit, a paste, a fill, a
sort, a clear), the **widths and formats** (a format, a width), or the
**whole document** (inserting and deleting rows and columns, which move
everything). Undoing a record first pushes what is there now onto the
other stack, then puts the record back, so undo and redo are one
operation in two directions. A rectangle is stored compactly -- each
non-empty cell's position and text -- so typing into one cell costs a
dozen bytes of history.

The memory is one block, taken at start from the arena
([kernel.md](kernel.md#arenas)) when the machine has 512KB (or 128KB)
to give, otherwise 32KB of the app's own: half for undo, a quarter for
redo, an eighth each for the copied cells and for scratch (a sort's).
When a stack fills, its oldest records go: history gets shorter, an
edit is never refused. A record larger than the whole stack (a
structural change to a big sheet on a board without an arena) empties
it instead -- that change is then not undoable, and no older one can be
undone past it.

## Charts

`chart.c`, about 450 lines, draws from the chart's range and the cells'
current values -- nothing is cached, so a chart always shows the
numbers as they are. On a one-bit display, series are told apart the
way they were on a Hercules card or a laser printer: an 8x8 **pattern**
per series (solid, rising and falling hatch, grey, horizontal and
vertical lines, cross-hatch, dots) for bars and wedges, outlined; a
**marker** per series (filled and open squares, x, +, filled and open
diamonds) on lines. A legend at the right shows each.

Everything is drawn with the blitter -- pattern fills
(`z_win_fill_pattern()`, new in `zwin.h`), boxes and lines -- so a bar
chart is a few hundred operations. The axis gets a tidy 1-2-5 step and
dotted rules. The pie is worked out row by row: each pixel's angle as an
integer *pseudo-angle* (monotonic in the true angle, one division) is
compared with where each wedge ends, and each run of one wedge on a row
is one pattern fill. Sine and cosine, needed only for the wedges' edges,
are a short series.

`make render` with modes 4-7 draws each kind to look at.

## Not implemented

Multiple sheets, text results from formulas (`VLOOKUP` returns numbers;
a text cell reads as 0), dates and times as such, named ranges,
printing. Each wants a real design rather than a corner of this file.
