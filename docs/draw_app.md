# draw -- painting in black and white

Kind: a **desktop app** ([app_kinds.md](app_kinds.md)).

`sw/apps/draw`. A monochrome painting program in the manner of early
MacPaint: a 640x480 1bpp page, nine tools down the left, sixteen
patterns along the bottom.

```
> run wm
> run draw
```

or double-click a `.zbm` in `files`, which launches it with the file.

## Drawing

| Tool | Key | What it does |
|---|---|---|
| Pencil | P, 1 | one pixel; white when you start on black |
| Brush | B, 2 | a round brush in the pattern |
| Eraser | E, 3 | a square of white |
| Fill | F, 4 | click inside a shape: it fills with the pattern ([Fill](#fill)) |
| Line | L, 5 | a straight line, always black |
| Rectangle | R, 6 | an outline |
| Filled rectangle | Shift+R, 7 | the pattern inside a black outline |
| Oval | O, 8 | an outline |
| Filled oval | Shift+O, 9 | the pattern inside a black outline |

Click a tool, or press its key; click a pattern along the bottom, or
step through them with **[** and **]**. The pattern is used by the
brush, the fill and the filled shapes.

**Shift** while dragging a shape: a line goes straight across, straight
down or at 45 degrees, a rectangle is a square and an oval a circle.

| | |
|---|---|
| Drag | draw with the tool |
| Arrows, PgUp / PgDn, Home / End | pan the page (or the scrollbars, or the wheel) |
| Title icons | new, open, save, print |
| **Ctrl+N**, **Ctrl+O**, **Ctrl+S** | new, open, save (**Ctrl+Shift+S**: save as) |
| **Ctrl+P** | print ([Printing](#printing)) |
| **Ctrl+Z** | undo the last stroke, shape or fill |
| **Ctrl+Y**, Ctrl+Shift+Z | redo |
| **Esc**, Ctrl+C | stop a fill, a load or a save |
| **F1** | help ([app_help.md](app_help.md)) |

The keys are text's and sheet's. Ctrl+N, Ctrl+O and Ctrl+S go the way
the title icons do: "Save changes first?" still comes up, and they are
ignored while a file loads or saves, or a fill runs.

## Fill

Click inside an area with the Fill tool: everything joined to that
pixel -- up, down, left and right -- that is its colour takes the
pattern. A white area inside a black outline fills to the outline; a
black shape clicked on fills with the pattern over its black. The page
is not changed until the fill is complete, and the title says
"Filling" meanwhile -- a whole page takes a moment on this machine.
**Esc** stops it and leaves the page as it was. A fill is one level of
undo.

The window answers throughout: it redraws, scrolls and takes keys while
a fill runs, however large or intricate the area.

## Printing

**Ctrl+P**, or the printer in the title bar, opens the print dialog
([print.md](print.md#the-print-dialog)): the printer or a PDF or
PostScript file, copies, paper, tray, pages to a sheet, quality. The
page prints as one picture, as large as fits the paper, turned if that
fits better, black and white exactly as on the screen. `print` shows
its own small window with progress and **Cancel**; draw goes on working
meanwhile.

## Undo

One level is one stroke, shape or fill: from the button going down to it
coming up. **Ctrl+Z** puts the page back as it was before it, **Ctrl+Y**
(or Ctrl+Shift+Z) puts it back again, and a new stroke forgets what
could have been redone. Undoing back to the picture as last saved takes
the `*` off the title; a stroke that changed nothing is not a level and
leaves no `*`. New and Open start with no history. Undo and redo are
ignored mid-stroke, during a fill, and while a file loads or saves.

How many levels depends on the board: up to 64 where there is memory to
give (about six page-sized strokes, many more small ones), one on a 1MB
board. `draw: undo: ...` on the console says which.

## Files

Pictures are ZBM files (`sw/common/zbm.h`): a 16-byte header and the
page's pixels. Files from before the header (exactly 38,400 bytes) still
open. A colour picture (pix's, [pix_app.md](pix_app.md)) is a ZBM too,
and draw refuses it: "That is a colour picture: open it in pix".

Opening and saving never stop the app ([responsive.md](responsive.md)):
the title shows `Loading NAME 45%  (Esc stops)` or `Saving ...`, and the
window keeps redrawing, scrolling and answering keys.

**Opening** replaces the page only once the whole file is in. **Esc**
(or closing the window) stops it, and the drawing on screen is the one
there before. A file that is not a ZBM, is another size, or ends early
is refused with a message, and the drawing stays. While a file loads,
drawing on the page is ignored -- it is about to be replaced.

**Saving** writes `NAME.part` and, once every byte is written, renames it
to `NAME`. **Esc** stops it: the `.part` is deleted and the old file is
as it was. A failed write does the same and says so. Should the rename
fail after the old file has gone, the picture is kept as `NAME.part` and
the message says so -- the only case a `.part` is left. While a file
saves, the page is read-only: drawing is ignored.

During a load or a save:

- **New, Open and Save** are ignored; one file at a time, and the title
  says which is running.
- **Closing the window** during a load stops the load, then closes as
  usual (asking about unsaved changes). During a save it closes once the
  save has succeeded; Esc stops the save and keeps the window open.
- **Unsaved changes?** -> Yes, on New, Open or close, starts the save;
  the New, Open or close follows when it succeeds, and is dropped if the
  save fails or is stopped.
- Scrolling, choosing tools and patterns, and F1 work. A dialog (F1, an
  error) pauses the load or save until it is put away.

---

## For developers

How it is built and how it is tested. Nothing here is needed to use it.

### Strokes a word at a time

The brush and eraser are drawn a word (32 pixels) at a time, a row of
the stamp being at most two words: a brush point is at most 16 word
writes and an eraser point 24, where they were 64 and 144 pixel writes.
The rectangle's top and bottom edges likewise, and each row of a filled
oval is one span. The pixels are exactly the ones the per-pixel code
drew -- the tests compare the two. This is `drawstamp.c`.

### The fill

`drawfill.c`, a span fill run a few spans a step from the main loop
(`fill_step()`, about 10 ms, then the messages again):

1. Runs of the area's colour are found a row at a time, whole words at
   a time, and set to the other colour. A pixel set is a pixel visited,
   so no visited mask is needed. The runs still to look above and below
   are on a stack of spans (`DRAW_FILL_STACK`, 256), not of pixels.
2. If that stack runs out, nothing is lost: a span not pushed is found
   again by a sweep of the rows touched, which looks for a pixel of the
   area's colour beside one the fill has changed and starts there.
   Sweeps repeat until one finds nothing.
3. The pattern goes in where the page differs from `spare` -- the page
   as it was before, which undo keeps there anyway -- so a pattern half
   of whose pixels are the area's colour is not mistaken for unfilled
   area. Then the changed box is drawn and becomes an undo level.

Esc (`draw_fill_stop()`) copies the rows it touched back from `spare`.

### Printing

`do_print()` makes a print job ([print.md](print.md#printing-from-an-app))
with one page, a 4:3 ratio page fitted to the paper: a black rectangle,
then the page as a mask (`zc_blit_mask()`) painting white where the page
is white. That costs no copy of the page: the mask paints where its
source is 0, and the page's ink is 1. Linking `zcanvas`, `zpage`,
`zpaper`, `zprint` and `zcfg` for it took draw from 135KB to 168KB of
image.

### Undo's memory

A level is a rectangle of the page as it was: whole words across, and
only the rows and words the stroke actually changed -- found by
comparing the page with a copy of it from before the stroke. A pencil
dot keeps 4 bytes, a stroke across the page up to 37.5KB. Undoing swaps
the rectangle with the page, so the level then holds the page as it was
*after* the stroke, which is what redo needs: no second copy.

| Board | Memory | Levels |
|---|---|---|
| with an arena to give ([kernel.md](kernel.md#arenas)) | 256KB of arena, or 64KB if that is not there | up to 64; about six page-sized strokes in 256KB (one in 64KB), many more small ones |
| 1MB (no arenas) | nothing more | one |

With the arena, levels are kept in it as a ring and the oldest go when a
new one does not fit or there are 64. A stroke larger than the whole
ring (a page-sized one, with 64KB) cannot be undone, and nothing before
it can either. The copy of the page from before the stroke is the
second page (below), kept equal to the page as strokes end.

With no arena the second page *is* the level: it holds the page as it
was before the last stroke, and undo and redo swap the changed
rectangle between the two. The next stroke copies that rectangle
across, so the level is gone once a new stroke starts, even one that
changes nothing. Opening a file reads into the second page, so it takes
that level too; with the arena, levels survive a stopped or failed open.

The arena is taken once, at start; draw never resizes it. The Open and
Save dialogs may add to it for a big folder, which can move it, and say
so (`undo_moved()`; [widgets.md](widgets.md#big-folders-in-the-open-and-save-dialogs)):
undo's levels are offsets, so only its base changes.

### The second page

Opening reads into a second page and swaps it in only when the whole
file is in. The second page costs 37.5KB of memory for as long as draw
runs. It is the cheapest way that is right: reading into the page on
screen loses the old drawing when a load is stopped, and an arena (or a
copy in `/tmp`) is not there on a 1MB board. Between loads it is undo's
(above), and a fill's record of the page as it was.

Saving removes the old `NAME` just before the rename, as FAT will not
rename over a file.

### Tests

Host, AddressSanitizer: `make -C sw/apps/draw test` -- saves and loads in
slices, stops at every step of each (the old file and the old page kept,
no `.part`, nothing left open), damaged and foreign files, and every
write, close, unlink and rename failure. Then the stamps, undo and the
fill (`tests/test_draw.c`): the brush, eraser, fill and outline against
the per-pixel ones they replaced, on 300 pages of random strokes and at
every alignment along the page's edges; undo and redo putting the page
back byte for byte after every one of many strokes and shapes, with a
roomy ring, rings that page-sized strokes fill, a ring too small for
most strokes, and one level; and the paint bucket against a plain flood
fill with a visited array and an unbounded queue, on shapes, noise and a
comb of hundreds of slots that runs the span stack out (so the sweep
finishes it), in steps of 1, 16 and 1,000 spans, everything it changed
inside its box, a stop putting the page back, and a click off the page.

---

## Implementation notes

### The launch argument, before the window

draw asks wm for the file it was launched with *before* it makes its
window. Asked after, as it once was, the wait for wm's answer took the
new window's first region and redraw request out of the mailbox
unanswered, and draw came up with an empty frame -- no tools, no
patterns -- until it was dragged. Any app that paints before it takes
its launch argument has the same trap; the others take it first, or
paint after.

### The fill that was removed

draw had a paint bucket once, and it was taken out: a pattern fill
needs a visited mask (half the pixels of a 50% pattern are the area's
colour), it had a bounded stack of pixels that ran out on anything but a
simple area and left the fill half done, and the sweep over the whole
page that followed took long enough that wm gave up waiting for draw's
redraw ack. The one now differs in each: runs on the stack rather than
pixels, the area found in one colour and the pattern put in after
against `spare`, a sweep of only the rows touched, and all of it a few
spans at a time between messages.

### The line tool's ink

The line tool drew with the pencil's ink, which is latched when a pencil
stroke starts: after a pencil stroke begun on black, lines came out
white. A line is now always black.
