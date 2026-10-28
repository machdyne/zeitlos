# draw -- painting in black and white

Kind: a **desktop app** ([app_kinds.md](app_kinds.md)).

`sw/apps/draw`. A monochrome painting program in the manner of early
MacPaint: a 640x480 1bpp page, seven tools down the left, sixteen
patterns along the bottom.

```
> run wm
> run draw
```

or double-click a `.zbm` in `files`, which launches it with the file.

draw asks wm for the file it was launched with *before* it makes its
window. Asked after, as it once was, the wait for wm's answer
took the new window's first region and redraw request out of the
mailbox unanswered, and draw came up with an empty frame -- no tools,
no patterns -- until it was dragged. Any app that paints before it
takes its launch argument has the same trap; the others take it first,
or paint after.

## Drawing

| | |
|---|---|
| Left column | pencil, brush, eraser, line, rectangle, filled rectangle, oval |
| Bottom row | the pattern for the brush and the filled rectangle |
| Drag | draw with the tool |
| Scrollbars, wheel | pan the page |
| Title icons | new, open, save |
| **Ctrl+N**, **Ctrl+O**, **Ctrl+S** | new, open, save (**Ctrl+Shift+S**: save as) |
| **Ctrl+Z** | undo the last stroke or shape |
| **Ctrl+Y**, Ctrl+Shift+Z | redo |
| **Esc**, Ctrl+C | stop loading or saving |
| **F1** | help ([app_help.md](app_help.md)) |

The keys are text's and sheet's. Ctrl+N, Ctrl+O and Ctrl+S go the way
the title icons do: "Save changes first?" still comes up, and they are
ignored while a file loads or saves.

The pencil draws white when the stroke starts on a black pixel.

The brush and eraser are drawn a word (32 pixels) at a time, a row of
the stamp being at most two words: a brush point is at most 16 word
writes and an eraser point 24, where they were 64 and 144 pixel writes.
The rectangle's top and bottom edges likewise. The pixels are exactly
the ones the per-pixel code drew -- the tests compare the two. This is
`drawstamp.c`.

## Undo

One level is one stroke or shape: from the button going down to it
coming up. **Ctrl+Z** puts the page back as it was before it, **Ctrl+Y**
(or Ctrl+Shift+Z) puts it back again, and a new stroke forgets what
could have been redone. Only the part of the page that changes is
redrawn. Undoing back to the picture as last saved takes the `*` off
the title; a stroke that changed nothing is not a level and leaves no
`*`. New and Open start with no history. Undo and redo are ignored
mid-stroke and while a file loads or saves.

A level is a rectangle of the page as it was: whole words across, and
only the rows and words the stroke actually changed -- found by
comparing the page with a copy of it from before the stroke. A pencil
dot keeps 4 bytes, a stroke across the page up to 37.5KB. Undoing swaps
the rectangle with the page, so the level then holds the page as it
was *after* the stroke, which is what redo needs: no second copy.

How many levels depends on the board (`drawundo.c`):

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

The arena is taken once, at start, and never resized: undo holds a
pointer into it, and it never moves. On a board short of memory the
256KB is not taken and draw asks for 64KB; refused that too, it has one
level. `draw: undo: ...` on the console says which.

## Files

Pictures are ZBM files (`sw/common/zbm.h`): a 16-byte header and the
page's pixels. Files from before the header (exactly 38,400 bytes) still
open. A colour picture (pix's, [pix_app.md](pix_app.md)) is a
ZBM too, and draw refuses it: "That is a colour picture: open it in
pix".

Opening and saving never stop the app ([responsive.md](responsive.md)):
the file is read or written a chunk at a time between messages, the
title shows `Loading NAME 45%  (Esc stops)` or `Saving ...`, and the
window keeps redrawing, scrolling and answering keys.

**Opening** reads into a second page and swaps it in only when the whole
file is in. **Esc** (or closing the window) stops it, and the drawing on
screen is the one there before. A file that is not a ZBM, is another
size, or ends early is refused with a message, and the drawing stays.
While a file loads, drawing on the page is ignored -- it is about to be
replaced.

**Saving** writes `NAME.part` and, once every byte is written, renames it
to `NAME` (the old `NAME` is removed just before, as FAT will not rename
over a file). **Esc** stops it: the `.part` is deleted and the old file
is as it was. A failed write does the same and says so. Should the
rename fail after the old file has gone, the picture is kept as
`NAME.part` and the message says so -- the only case a `.part` is left.
While a file saves, the page is read-only: drawing is ignored.

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

The second page costs 37.5KB of memory for as long as draw runs. It is
the cheapest way that is right: reading into the page on screen loses
the old drawing when a load is stopped, and an arena (or a copy in
`/tmp`) is not there on a 1MB board. Between loads it is undo's (above).

Tests (host, AddressSanitizer): `make -C sw/apps/draw test` -- saves and
loads in slices, stops at every step of each (the old file and the old
page kept, no `.part`, nothing left open), damaged and foreign files,
and every write, close, unlink and rename failure. Then the stamps and
undo (`tests/test_draw.c`): the brush, eraser, fill and outline against
the per-pixel ones they replaced, on 300 pages of random strokes and at
every alignment along the page's edges; and undo and redo putting the
page back byte for byte after every one of many strokes and shapes,
with a roomy ring, rings that page-sized strokes fill, a ring too small
for most strokes, and one level -- the 64-level limit, a stroke too
large for the ring, a new stroke dropping redo, an open taking the
second page, and the `*` coming off when undo reaches the save.
