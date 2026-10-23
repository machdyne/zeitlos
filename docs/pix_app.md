# pix

A pixel editor for the system's own small pictures: sprite sheets in
up to 256 colours (game art, mausrun's tiles and sprites, the mouse
pointer), the dock's icons, bitmap fonts, and pictures of one bit or up
to 256 colours -- pictures from elsewhere (PNG, GIF, BMP, JPEG) brought
in and reduced to a palette. Every
pixel is placed on purpose -- pencil, lines, rectangles, flood fill and
selections -- on a zoomed grid, in a 320x240 window or full screen.

```
> run pix                          a new 16x16 sprite sheet, as many colours as the board shows
> run pix /data/wm/dock.zic        the dock's icons
> run pix /sys/fonts/m8x13.zfn     a font
> run pix /data/photo.png          a colour picture of it
```

A `.zic`, `.zfn` or `.zsp` opened in `files` starts pix on it
(`sw/common/ztype.c`); a `.zbm` still opens in `draw`, and pix opens one
when asked. pix is in the dock's **Tools** group and in the `work` set
of the release card.

Built on the arcade runtime ([arcade.md](arcade.md)): the same picture
in the window (monochrome) and full screen (F11), the mouse in both,
and only what changed is drawn. The source is `sw/apps/pix/`:
`px_doc.c` the document, `px_io.c` the file formats, `px_edit.c` the
tools and undo, `px_anim.c` animations, `px_import.c` pictures from
elsewhere, `pix.c` the screen and the input.

Phases 1 to 4 of the plan (`pix-proposal.md` in the project's notes):
the editor, colour sprite sheets with the tiered palette
([zsp.md](zsp.md)), animation, and colour pictures with importing.

## The screen

```
+------------------------------------------+----------+
| the canvas: the cell, zoomed (1-16x),    | tools    |
| a grid from 6x, the cell framed;         | colours  |
| a font's ascent and the pointer's        | zoom     |
| centre drawn as guides                   | New Open |
| 256 x 192                                | Save     |
+----------+-------------------------------+----------+
| preview  | the cells: 1:1, or shrunk to fit; the     |
| 1:1      | current one framed; a mark on changed ones|
+----------+-------------------------------------------+
| status: 3/49 term  32x32  5,7  ink -- or a message  |
+-----------------------------------------------------+
```

**What is drawn when.** Only what changed is drawn again: a pencil
stroke the pixels it touched (on the canvas and in the preview), a line
or a box being dragged the pixels where it was and where it is now, a
selection its frame, a selection being moved where it was and is. The
canvas's part of a change is put together whole -- the art, the grid,
the frames -- and drawn in one pass, so nothing on it is cleared and
drawn again (in 256 colours, eight planes of each). A colour is
looked up once per palette (`luts_setup()`), not per pixel, and the
planes are made eight pixels at a time by transposing their bits
(`c2p8()`), not a pixel and a plane at a time. A line being dragged is
worked out once a move, a run a row (`line_prep()`). `render_pix out
cost` prints what each frame of a drawing session writes.

The keyboard's cursor (a frame round one pixel, from 3x) is shown once
a key moves it, and hidden when the mouse draws: where the mouse went
down it looked like a pixel selected.

A new cell opens at the zoom that shows all of it (at most 16x). A cell
larger than the canvas scrolls (Shift+arrows); the preview shows the
part in view at 1:1.

Full screen keeps the mouse pointer, where a game's hides it (`pointer`
in its `ar_desc_t`, [arcade.md](arcade.md)).

## Colour

Icons, fonts and one-bit pictures are two colours, as their formats
are. A **sprite sheet** or a **colour picture** has a palette of 2, 4,
16, 64 or 256 colours, and
every colour says what stands in for it with fewer: the entry at 64
and at 16, a slot at 4, a grey at 2 ([zsp.md](zsp.md)). So the same
sheet is shown the same way on every board, and pix shows it exactly
as each would:

- **Full screen** (F11) shows the art in colour: as many as the board
  has (16, 64 or 256), a bigger sheet through its stand-ins. pix's own
  controls use the entries the art leaves free at the top of the
  palette, or the art's darkest and lightest when it uses all of them.
- **The window** is monochrome: each colour has a pattern of its own
  while it is edited (its 16-colour stand-in's, with more), so two
  colours are never confused; the **preview and the strip** show the
  real result, each colour in its grey.
- **F7** shows the sheet as 64, 16, 4 and 2 colours in turn, then as it
  is: only the display changes, never the art.
- **See-through** pixels (`.`) are a checker in both.

**The colours**: in the window the side column shows a page of 16 chips
(Shift+PgUp and PgDn page through more); full screen, a sheet of 64 or
256 colours shows them all at once, as small squares (8 x 8, or 16 x
16), the page of 16 that the keys pick from marked at the left. Then
the first colour and the second. Left
click on a chip picks the first, right the second; 0-9 and A-F pick in
the page, `.` see-through, X swaps the two. The left button draws the
first colour, the right the second -- see-through, in a new sheet, so
the right button rubs out.

**The palette (F8)** opens over the canvas: the entries as a grid, and
for the first colour its fields.

| Field | |
|---|---|
| Colours | the sheet's: fewer maps every pixel through the stand-ins (asked first; it cannot be undone), more changes none |
| Entry | which colour |
| Red, Green, Blue | 0..15 each |
| to 64, to 16 | its stand-in at 64 and at 16 colours (sheets of 256, and of 64 and 256) |
| Slot | its slot at 4 colours |
| Ink | its grey, 0 black .. 16 white: the window and 2 colours |

Up and Down choose a field, Left and Right change it (with Shift, by
4), or click a field's left or right half. A field set by hand is
marked `!` and is never worked out again; **D** puts it back to the
default, from the colour's RGB. Changing RGB works out every field not
set by hand. **M** on two colours in turn marks them "must differ", or
unmarks them; when two that must differ fall on the same colour at
64, 16, 4 or 2, the palette says which and where. F8 or Esc closes it.

## Tools

| Key | Tool | |
|---|---|---|
| P | pencil | one pixel a step; a drag draws a connected line |
| L | line | press, drag, release: shown while dragging |
| R | rectangle | R again: outline or filled |
| G | fill | 4-connected flood fill |
| K | pick | takes the colour under it |
| S | select | drag a rectangle; then drag inside it to move it |

The **left button** draws the first colour, the **right** the second,
whatever the tool (in two colours: ink and background). Without a
mouse: the arrows move a cursor on the grid and **Space** does what a
left click would (twice for a line, a rectangle or a selection: start,
then end). Esc drops a line or rectangle under way, or the selection.

**A selection** is moved by dragging it or with the arrows; Enter or a
click outside puts it down. H, V and T flip it across, flip it down
and turn it (a quarter clockwise; only a square part turns in place),
Ctrl+arrows shift it with wrap, Del clears it. With no selection, H V
T and Ctrl+arrows work on the whole cell -- turning a cell that is not
square swaps its width and height.

**Undo**: Ctrl+Z and Ctrl+Y, 64 steps or 512 KB of pixels, whichever
comes first. Each step holds the whole cell before a change (its size
too, so a turn undoes). Adding or removing a cell clears the history.

## Animation

A sheet's cells are the frames, and an **animation** is a line of the
sheet: its name, how many milliseconds each frame shows, and its cells
in order -- one may come more than once ([zsp.md](zsp.md)):

```
anim run 120 mouse_stand mouse_run1 mouse_run2 mouse_run1
```

- **F5** plays the current cell's animation in the preview, looping,
  at 2:1 where it fits, and F5 again stops it. A cell in no animation
  plays every cell of its size, in order, at 100 ms. It goes on while
  you draw: a frame changed, added or renamed shows the next time it
  comes round.
- **Shift+F5** edits the animation the cell is in, as a line of words
  -- `run 120 mouse_stand mouse_run1 mouse_run2` -- or, if it is in
  none, offers a new one of the cells its size. A name of its own adds
  one; just a name deletes that one.
- **F6** is onion skin: under the cell, where it is clear, the frame
  before it as dots in that frame's colours; F6 again adds the frame
  after (dots nearer the corner of each pixel); a third time, off. The
  frames around a cell are its animation's, or the cells its size.
- **Ctrl+D** is the next frame: a copy of the cell, named one up
  (`mouse_run2` -> `mouse_run3`), right after it -- in the strip and in
  every animation that shows it.

Renaming a cell renames it in the animations; deleting it takes it out
of them (and an animation left with no frames goes). Animations are
kept in sprite sheets; the others play and onion-skin their cells the
same way, but have nowhere to keep a line.

## Keys

| | |
|---|---|
| P L R G K S | the tools |
| 0-9 A-F | a colour of this page (two colours: 0 background, 1 ink) |
| . | see-through (a sprite sheet) |
| X | swap the first and second colours |
| Shift+PgUp PgDn | the page of 16 colours, in a bigger palette |
| F5 | play the animation in the preview, or stop |
| Shift+F5 | the animation: name, ms a frame, cells |
| F6 | onion skin: the frame before, before and after, off |
| F7 | show as 64, 16, 4, 2 colours, then as it is |
| F8 | the palette |
| + - (or the wheel) | zoom |
| arrows / Space | the cursor; draw there |
| Shift+arrows | scroll |
| Ctrl+arrows | shift with wrap |
| PgUp PgDn, Home End | previous and next cell, first and last |
| Ins | a new cell: asks for its name (an icon: the app's name; a glyph: its character) |
| Ctrl+D | a copy of this cell: the next frame |
| Shift+Del | delete this cell |
| H V T | flip across, flip down, turn |
| Del | clear the selection |
| Ctrl+C X V A | copy, cut, paste, select all |
| Ctrl+Z Y | undo, redo |
| F2 | rename the cell |
| F3 | go to a cell by name, or a glyph by its character (`A`, `U+00C4`, `0x41`, `65`) |
| Ctrl+N O S | new, open, save; Ctrl+Shift+S save as |
| Ctrl+I | a picture (PNG, GIF, BMP, JPEG) into this cell, in its colours |
| Ctrl+Q | quit (asks to save) |
| F1 | help |
| F11 | full screen |

pix takes every letter as a key (`ar_text_input(true)`), so C and M are
not the runtime's colour and music keys here. The tools, the colour
chips and the New / Open / Save buttons can also be clicked, and a
cell in the strip is chosen by clicking it.

**Speech** says the tool, the colour, the cell and its size when it
changes ("term, 32 by 32"), and where the cursor is ("5, 7, ink").

## New

Ctrl+N asks what to make, in a few words:

| | |
|---|---|
| `icon` | an icon set with one 32x32 icon, `new` |
| `cursor` | the pointer: two 16x16 sprites, `normal` and `busy`, the centre (8,8) marked |
| `sprite W H N` | a sprite sheet with one W x H sprite in N colours, the default palette ([zsp.md](zsp.md)); 16x16 if no size, and as many colours as the board shows full screen if no N (256 on an 8-plane board such as Sergei ML2, 64 on 6, else 16) |
| `font W H` | a font with the glyph `A` (6x12 if no size; at most 16x32) |
| `picture W H` | a one-bit picture, draw's kind (320x240 if no size; at most 1024x1024) |
| `picture W H N` | a colour picture in N colours (4, 16, 64, 256), the default palette; at most 512K pixels (1024x512, 800x600) |

## Files

Every one is written back the way it was read: dock.zic, the card's
fonts and mausrun's sheets come back byte for byte (the host tests
check it).

### `.zic` -- the dock's icons

Opened as 32x32 cells, each with its app's name and spoken name; F2
renames, Ins adds an icon for an app that has none. The format is
[dock.md](dock.md)'s.

**`/data/wm/dock.zic` is never written over.** Opening it lays
`/data/wm/icons.zic` over it, if there is one, and saving it writes
**only the icons that were changed or added** to `icons.zic`
(`Z_PATH_WM_ICON_OVERRIDES`): the release's icons stay the release's,
and icons a later release adds or changes still arrive, except the ones
you drew. Deleting `icons.zic` gives all of them back.

wm lays `icons.zic` over the release's icons by name at start and on
`cfg reload` ([dock.md](dock.md)), so a saved icon shows in the dock
after `cfg reload`. An icon added for an app that has none shows where
wm.cfg names that app in a row; `@rest` adds only apps dock.zic has.

Any other `.zic` is saved whole, as it is.

### `.zfn` -- fonts

Each glyph is a cell, named by its character and kept in codepoint
order; the strip shows the glyphs themselves. The ascent is drawn as a
guide line. Ins asks for a character and adds its glyph in order; F3
goes to one. Fonts are used where they are loaded: `/sys/fonts/*.zfn`
by `slide` and `print` on their next load, `/data/jfont/jp12.zfn` when
jfont restarts. The desktop's own 5x8 and 6x12 are in the bitstream and
are not edited here ([fonts.md](fonts.md)).

### `.zsp` -- sprite sheets, and the pointer

Text: the palette, then the sprites, one character (or two hex digits,
past 16 colours) a pixel ([zsp.md](zsp.md)). `anim` lines are the
animations (above); other lines pix does not edit -- comments -- are
kept and written back where they were. mausrun's art is
two sheets, on the card in `/data/mausrun/art`: open one, change it and
save it as `/data/mausrun/sprites.zsp` (or `tiles.zsp`), and it shows
the next time mausrun starts ([mausrun.md](mausrun.md#art)).

A sheet holding exactly `normal` and `busy`, both 16x16, is the
**pointer**: its centre (8,8), where the pointer points, is marked.
Save it as `/data/wm/cursor.zsp`, then `cfg reload` and Super+M show it
([cursor.md](cursor.md)).

### `.zbm` -- pictures

ZBM1, one bit a pixel ([draw_app.md](draw_app.md)): draw's pages and
chip8's screenshots. A picture is one cell, up to 1024x1024; the canvas
scrolls.

**A colour picture** is a ZBM too: the header's flags say how many
bitplanes (2 to 8), the tiered palette follows the header, then each
row's planes in turn ([zbm.h](../sw/common/zbm.h)). A game blits a
plane of it straight from the file -- plane k is a bitmap whose rows
are `planes` rows apart -- and `view` shows it through each colour's
ink ([view_app.md](view_app.md)). `draw` is one bit a pixel and says
so: "That is a colour picture: open it in pix". It has a palette like
a sheet's (F8, F7) and no see-through: `.` does nothing, and the
second colour starts as colour 0.

## Importing

**Opening a PNG, GIF, BMP, PNM or JPEG** (Ctrl+O, or `run pix
file.png`) makes a new colour picture of it, through the system's
decoders ([view_app.md](view_app.md), in colour). Its palette is the
image's own colours, as RGB444, when there are 256 or fewer; with more,
256 chosen by median cut. Each pixel is the nearest. The palette has 4,
16, 64 or 256 entries, the fewest that hold them, and is ordered so the
first 16 and the first 4 are spread out -- what stands in at 16 and 4
colours (the stand-ins are worked out as for any new colour). Saving
offers the same name as `.zbm`.

**Ctrl+I** brings a picture into the cell being drawn, in its colours:
each pixel the nearest colour of the sheet or picture (an exact match
is itself), see-through where the image is (PNG alpha under half, or a
palette's transparent entry). In an icon or a font, white is ink. It
comes as a pasted selection at the cursor: move it, then click or
Enter.

An image bigger than 640 x 480 comes in halved until it fits (a ZBM is
cut to it instead). JPEG, grey PNM and one-bit ZBM come in grey: their
decoders give no colour. See-through comes from PNGs (RGBA, grey with
alpha, a palette's transparent entries); a GIF's transparent colour is
not read yet.

## Into the tree

Art drawn on the board goes into the source with `tools/pixconv.py`, on
a build host:

```
python3 tools/pixconv.py zic icons.zic sw/data/icons     # icon-<name>.png, as gen_dock_icon_data.py wants
python3 tools/pixconv.py zsp sprites.zsp out/ [--sheet] [--as 16]
python3 tools/pixconv.py zbm picture.zbm out.png [--as 64]
python3 tools/pixconv.py zfn font.zfn font.bdf
```

`zic` writes each icon as the 32x32 black-and-white PNG
`sw/data/icons` keeps (and prints its `apps.txt` line); run
`gen_dock_icon_data.py` there after. Turning the release's `dock.zic`
back into PNGs gives the tree's own icons pixel for pixel. `zsp` and
`zbm` write PNGs (see-through as alpha); `--as N` shows the art as a
board with N colours would, through the stand-ins. A `.zsp` itself is
text and goes into the tree as it is (mausrun's: `sw/data/mausrun`).

## Limits

| | |
|---|---|
| cell or picture | 1024 x 1024; a colour picture 512K pixels |
| cells in a file | 8192 |
| names | 15 letters, no spaces |
| undo | 64 steps, 512 KB; a step keeps only the rows and bytes its edit changed (an edit that changes nothing leaves no step, and leaves redo as it was). Where there is not memory to take a step, pix says so and the edit goes ahead without one |
| colours | 256; icons, fonts and one-bit pictures 2 |
| importing | 640 x 480 (bigger is halved until it fits) |
| memory | pix runs with a 2 MB heap (`APP_STACK = 2M`): a sprite sheet takes 2 bytes a pixel, a colour picture 1, the rest 1 bit; an edit's undo step is taken whole, then cut down |

## Dialogs from an arcade program

Open, save and the prompts are the system's boxes (`zdialog.h`). A box
is a window and wm shows none over game mode, so pix brackets each one
with `ar_dialog_begin()` / `ar_dialog_end()` ([arcade.md](arcade.md),
"Dialogs"): full screen is left first, the picture stays painted while
the box waits, the sound is held, every key is released afterwards,
and full screen comes back once the box (or the boxes, one after the
other) are done.

## On a build host

```
make -C sw/apps/pix/tests          # the document, the formats, the tools (ASan, UBSan)
make -C sw/apps/pix/tests render   # the editor itself: out/*.png, out/sheet.png
```

`test_import` brings in pictures `gen_import.py` makes (a palette PNG
with a transparent entry, RGBA, a gradient of 2,000 colours, GIF, BMP,
one too big) as pictures and as pieces, and a colour ZBM back in.
`test_anim` reads, changes and writes back `anim` lines, and follows
cells renamed, deleted and copied. `test_io` reads and writes back the release's `dock.zic` and four card
fonts byte for byte, checks the overrides file and its overlay, and
round-trips a ZBM and `.zsp` sheets of 2 to 256 colours. `test_edit` checks the shapes against
their previews, flood fill, the piece operations and undo through a
resize.

`render` runs the real `pix.c` on the host framebuffer, every frame
checked against a full repaint (`AR_HOST_VERIFY`): `icons` (dock.zic:
a line, a selection flipped and moved, zoom, full screen in mono and
colour), `font` (m8x13: going to `g`, a rectangle being dragged),
`cursor` (a test pointer, `tests/cursor.zsp`), `picture` (a new
320x240: rectangle, line, fill, zoom, undo), `colour` (a new 16-colour
sheet: drawing, shown as 4 and 2, the palette, full screen), `c256`
(`tests/c256.zsp`, all 256: the palette, full screen as 256, 64 and 16,
and again on 6- and 4-plane boards, `AR_HOST_PLANES`) and `mausrun`
(its sprites, full screen, as 2), `anim` (mausrun's sprites: onion
skin, an animation made and played, the next frame), `cpic` (a new
picture in 64 colours: shapes, undo, full screen), `import` (a PNG
opened as a colour picture, another brought in with Ctrl+I). The boxes are answered by the script; a "save first?"
always answers no, so a script never writes over its inputs.
