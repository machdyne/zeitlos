# slide

Cards, slides and pages. `sw/apps/slide`.

```
> run wm
> run slide
> run slide /data/slide/tour.zsl
```

A deck is a stack of cards. A card holds objects -- text boxes,
rectangles, rounded rectangles, ovals, lines and arrows, polygons and
pictures -- drawn with `zcanvas` (docs/zcanvas.md) in public-domain
Hershey and misc-fixed fonts (docs/fonts.md). The same card is a slide
when it is shown on the whole screen, and a page when it is printed:
one drawing, at any scale.

It is a HyperCard-shaped app grown in phases (the plan is
`claude/slide-proposal.md` in the project): so far the editor, the
show and printing. Buttons and actions, and the Scheme `slide`
language, come next (see "Not yet").

```
+----------------------------------------------------------+
| slide - tour.zsl  3/7  text                         _ x  |
+---+------------------------------------------------------+
| b |  +------------------------------------------------+  |
| v |  |                                                |  |
| T |  |   the card, at the largest scale that fits     |  |
| [] |  |   (never more than 100%)                       |  |
| () |  |                                                |  |
| \ |  +------------------------------------------------+  |
| ->|                                                      |
+---+------------------------------------------------------+
| fill: [] # # # ...   line: 0 1 2 3 4 6   sans  B I U     |
| status, or the / command line                            |
+----------------------------------------------------------+
```

## Using it

**F1** (or `/help`) shows a short help box; its **Read docs** button opens this page ([app_help.md](app_help.md)). **/tour** opens the tour deck, which
shows most of this on its own cards.

The tools are down the left: **b**rowse (a click is the next card,
Shift+click the one before), select (**v**), **t**ext,
**r**ectangle, ro**u**nded rectangle, **o**val, **l**ine, **a**rrow,
**p**olygon and **i**mage -- the letter picks the tool. Drag to draw;
Shift keeps squares, circles and 45-degree lines. A polygon is a click
per corner and Enter (or a double-click) to finish; ending on the first
corner closes it. A text box is a drag,
or a click for one of default size; type into it straight away.

Click to select, Shift+click to add, drag on empty card for a band.
Handles resize; arrows nudge by 1 (Shift: 10). A double-click (or Enter)
on a text box edits its text; Escape ends that.

The bar under the card sets the selection's fill (none, or one of the
16 patterns), its line width, its font family and bold / italic /
underline. Everything else is a command.

**/** or **F4** opens the command line in the status line. Enter runs
it, Escape backs out; what it has to say comes back in the same line. A
command works on the selection, or on the card when nothing is selected.
**/** again at the start of the line (or F4 again) opens the **/ menu**:
every command in a list over the card. Up / Down / PageUp / PageDown
move, Enter or a click picks one: one that needs nothing more runs
(`dup card`, `undo`), one that takes words goes on the line to finish
(`fill `, `go `). Escape, or typing, goes back to the line
([keys.md](keys.md#the-prompt-f4-and-the--menu)).

| command | does |
|---|---|
| `new` | a new card after this one |
| `dup card` | a copy of this card |
| `delete card` | this card (undo brings it back) |
| `name NAME` | names the selection, or the card if nothing is selected |
| `go N` / `go NAME` / `go first` / `last` / `next` / `prev` | another card |
| `background NAME` / `none` | this card's background, made if it is new |
| `edit background` | edit the background instead (again: the card) |
| `page SIZE [landscape / portrait]` | `classic 4:3 16:9 16:10 square letter legal a4 a5 a6 4x6 5x7 3x5`; objects scale with it |
| `title TEXT` | the deck's title |
| `font NAME [SIZE] [bold] [italic] [underline] [plain]` | the selection's font |
| `size N`, `bold`, `italic`, `underline`, `plain` | the same, one at a time |
| `align left / center / right / top / middle / bottom` | text in its box |
| `color N` | text tone, 0 white .. 16 black |
| `fill none / N / shade N` | pattern 0-15, or a shade 0-16 |
| `line W [TONE]` | frame or stroke width (0: none) and tone |
| `radius N` | a rounded rectangle's corners |
| `arrows none / start / end / both` | arrowheads on a line or an open polygon |
| `pad N` | space inside a text box |
| `lineup left / center / right / top / middle / bottom` | lines the selection up |
| `front`, `back`, `forward`, `backward` | stacking order |
| `hide`, `unhide`, `lock`, `unlock` | hidden objects are screened in the editor and absent in a show |
| `contrast N`, `bright N` | a picture's, -100..100 |
| `notes TEXT` | the speaker's notes for this card |
| `transition cut / wipe / push [MS]` | how the show arrives at this card |
| `advance SECONDS` | go on by itself in a show (0: never) |
| `grid N` | snap to N units (0: off) |
| `zoom 100` / `200` / `400` / `fit` | the editor's zoom (Ctrl+= / Ctrl+- / Ctrl+0) |
| `select all`, `delete`, `duplicate`, `undo`, `redo` | |
| `image` | place a picture (a file dialog) |
| `print`, `print pdf`, `print ps`, `print setup ...` | see "Printing" below |
| `show`, `open`, `save`, `save as`, `new deck`, `help`, `tour`, `quit` | |

| key | does |
|---|---|
| F1 | help; Read docs opens this page |
| F2 | edit the selected text box's text in place; anything else: `name ` on the command line |
| F4, / | the command line; again: the / menu |
| F5 / Shift+F5 | show from this card / from the first |
| F6 | all cards (the sorter) |
| PageUp / PageDown, the wheel | previous / next card (zoomed in, the wheel scrolls) |
| Ctrl+= / Ctrl+- / Ctrl+0 | zoom in / out (100%, 200%, 400%) / fit the window again |
| Ctrl+M | a new card |
| Ctrl+Z / Ctrl+Y | undo / redo, 64 steps |
| Ctrl+C / Ctrl+X / Ctrl+V | copy / cut / paste, also between decks; plain text pastes as a text box |
| Ctrl+D, Ctrl+A | duplicate, select all |
| Delete / Backspace | delete the selection |
| Tab / Shift+Tab | select the next / previous object |
| Ctrl+N / Ctrl+O / Ctrl+S | new / open / save (also the titlebar icons) |
| Ctrl+P | print the deck |
| Esc, while a picture loads | stop loading pictures |
| Enter on a stopped or failed picture | load it again |

**Pictures load in the background** ([responsive.md](responsive.md)):
the app never stops for one. A picture not loaded yet is its frame with
a progress bar, and the status line says `Loading NAME NN%  (Esc
stops)`. **Esc** stops loading -- the picture shows a light cross --
and **Enter** on it loads it again. One that cannot be read shows a dark
cross. `image` puts a 4:3 frame on the card at once, which takes the
picture's shape when it has loaded (unless you moved or resized it
meanwhile); if the file turns out not to be a picture, the frame goes
again with a message.

**Zoom.** The editor fits the card in the window (at most 100%).
**Ctrl+=** zooms in -- 100%, 200%, 400% -- around the selection, or the
middle of the view; **Ctrl+-** zooms out and **Ctrl+0** fits the card
again (or `/zoom 200`, `/zoom fit`). Zoomed in, scrollbars appear, the
wheel scrolls (Shift+wheel across), and typing keeps the caret in view.
Only the part in view is drawn, so a card at 400% costs no more to
redraw than one at 100%. The status line shows the zoom.

**The sorter** (F6) shows every card small. Arrows move, a drag
reorders, Delete removes, F5 shows from there, Enter or Escape goes back
to the chosen card.

### Showing a deck

F5 takes the whole screen. Space, Right, Down, PageDown, Enter or a
left click: the next card. Left, Up, PageUp, Backspace or a right click:
back. The wheel moves both ways. Home and End: first and last. A number
and Enter: that card. **B** blanks the screen black, **W** white, **S**
shows the card at its own size instead of filling the screen. Escape or
**q** ends the show and leaves the editor on the card it ended on. A key
or a click during a transition finishes it at once. Super+Esc always
takes the screen back (docs/window_manager.md).
Pictures still loading appear when they are ready: the card on the
screen's first, then the next card's; changing cards never waits.

### Printing

**Ctrl+P** (or `/print`) prints the deck on the printer set up in
`zeitlos.cfg` ([print](print.md#setting-up)). The print service takes
it from there, in a window of its own: slide goes on working, and the
status line says how it ended.

`/print` takes words, in any order:

| word | |
|---|---|
| `pdf`, `ps` | to a PDF or PostScript file instead (a file dialog; the deck's name with `.pdf`) |
| `this`, `3`, `2-5`, `all` | which cards |
| `4up` (1up .. 16up) | cards to a sheet |
| `cut`, `nocut` | cut marks between them |
| `frame`, `noframe` | a hairline round each card |
| `actual`, `fit` | a paper-shaped deck at its own size, or as large as fits |
| `paper a4`, `paper page`, `paper auto` | the sheet: a paper, each card's own shape (a PDF of slides), or the deck's own paper / `apps.print.paper` |
| `tray rear`, `tray auto` | the printer's tray (its own names: `print` lists them); `auto` leaves it to `apps.print.trays` and the printer ([print](print.md#trays)) |
| `type labels`, `type auto` | the paper's kind, for a printer that cares (`labels`, `photographic`, `envelope` ...) |
| `draft`, `normal`, `high` | print quality |
| `copies 3` | |
| `setup` | only change the settings, print nothing |

The settings -- `4up`, `cut`, `frame`, `actual`, `paper`, `tray`,
`type`, the quality -- are the deck's: kept in its file as `(print "4up cut paper
a4")`, so a deck of index cards prints the way it should every time.
The cards, the copies and `pdf`/`ps` are for that time only.

| | does |
|---|---|
| `/print` | the deck as it is set up: one card a sheet, fitted (a landscape card turned onto portrait paper) |
| `/print 6up frame` | handouts, six to a sheet |
| `/print pdf` | a PDF, one page per card, each the card's own shape |
| `/print this copies 2` | the card shown, twice |
| `/print setup 4up cut paper a4` | four to an A4 sheet with cut marks, from now on |

A card prints as it is on the screen -- the same renderer draws it --
but at the printer's resolution: Hershey text and shapes are sharp at
any size, pictures print in real grey from their file, and hidden
objects are left out.

## The deck

Every card has a name (`card1`, `intro`, ...) and may have a
**background**: a card of its own, drawn first, shared by every card
that names it. A title band and a footer are a background.

The **page** is a grid of units the objects are placed on. Its long
side is always 512 units; the short side follows the shape:

| page | grid (landscape) |
|---|---|
| `classic` | 512 x 342 (the HyperCard card) |
| `4:3` | 512 x 384 |
| `16:9` | 512 x 288 |
| `letter` | 512 x 396 |
| `a4` | 512 x 362 |
| `3x5` | 512 x 307 |

A paper page remembers the paper as well as its shape, so the print
service puts it on the right sheet, at its own size with `/print
actual` (docs/zcanvas.md, "zpage").
In the editor a unit is a pixel at 100%; a smaller window shows the
card smaller, never larger.

## The file

A deck is a text file, `.zsl`, of S-expressions. `files` opens it with
slide (`sw/common/ztype.c`). It is read as data and never evaluated:
the language in Phase 4 is a separate form inside it.

```scheme
(deck (version 1)
 (page "classic" landscape)
 (title "A tour of slide")
 (background "plain"
  (rect "bar" (box 0 0 512 30) (fill shade 4) (line 0 16)))
 (card "intro" (background "plain") (transition wipe 300)
  (text "t1" (box 20 60 472 50) (fill none) (line 0 16)
   (font "serif" 24 bold) (align center middle) (color 16) (pad 4)
   (text "Hello"))
  (notes "Say hello.")))
```

Objects are `text rect rrect oval line poly image`, each with a name
and a `(box x y w h)` in page units. The other forms are the commands'
words: `(fill none | shade N | pattern N)`, `(line W TONE)`,
`(radius N)`, `(arrows ...)`, `(points x y ...)`, `(closed)`,
`(src "path")`, `(contrast N)`, `(bright N)`, `(hidden)`, `(locked)`,
`(advance MS)`; and the deck's `(print "...")`, how it prints.
`(action ...)` and `(script ...)` are kept as they are
for the coming phases. A custom page is
`(page custom paper|ratio W H landscape)`.

A form slide does not know is skipped, so an older slide reads a newer
deck as far as it can; a deck whose `(version N)` is newer than the app
is refused with "made by a newer slide". A picture's path is stored
relative to the deck's folder when it is inside it.

Fonts are by name: `sans serif heavy script gothic symbol`, with bold
and italic faces where they exist (`serifb`, `serifi`, ...) and
synthetic ones otherwise, and the misc-fixed bitmaps `m6x13 m7x13 m7x14
m8x13 m9x15 m9x18 m10x20` (crisp at their own size), all in
`/sys/fonts`; `mono5x8` and `mono6x12` are built in. A missing font
falls back to `sans`.

## Memory

slide is a card app, so it has no core budget to fit; it asks the
arena for 6 MB, and takes 3 MB, 1.5 MB or 768 KB if that is all there
is. Everything lives in that one pool: the deck, undo (each step is the
card -- or, for a change of cards, the deck -- as `.zsl` text), the
128 KB glyph cache, pictures decoded to grey, and the two screen
buffers of a show. A picture is decoded once and dithered at the size
it is drawn, so a resize stays sharp. Pictures let go of (by undo, cut,
an edit) are kept a while -- four, up to 1.2 MB -- so they come back
without decoding again; opening another deck lets them go.

## Host tests

```
make -C sw/apps/slide test       # the model, the file, the renderer (pictures loading, stopping, again), the editor
make -C sw/apps/slide render     # the window, as /tmp/slide_ui_*.pbm (the / menu, a card loading)
make -C sw/apps/slide look-deck  # every card of the tour as /tmp/slide/deck_N.pgm
make -C sw/apps/slide job        # the tour as a print job, /tmp/slide/deck.zpp
```

`sw/apps/print`'s tests print the tour end to end
([print](print.md#tests)).

## Not yet

- **Printing**: notes pages, and a print dialog (the words above are
  the way for now).
- **Buttons, actions and timers** (Phase 3): `(action ...)` is kept in
  the file but does nothing; the browse tool only turns cards.
- **The `slide` language** (Phase 4): Scheme over zapi.
- **Diagrams** (Phase 5): connectors that follow their shapes, editing
  a polygon's points.
- Zoom past 400%, and scrolling by dragging the card itself.
