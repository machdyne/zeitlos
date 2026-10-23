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
| **Esc**, Ctrl+C | stop loading or saving |
| **F1** | help ([app_help.md](app_help.md)) |

The pencil draws white when the stroke starts on a black pixel. There is
no undo.

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
`/tmp`) is not there on a 1MB board.

Tests (host, AddressSanitizer): `make -C sw/apps/draw test` -- saves and
loads in slices, stops at every step of each (the old file and the old
page kept, no `.part`, nothing left open), damaged and foreign files,
and every write, close, unlink and rename failure.
