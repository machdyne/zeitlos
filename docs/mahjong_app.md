# sw/apps/mahjong -- mahjong solitaire

Kind: a **dual app** ([app_kinds.md](app_kinds.md)). The desktop face is a resizable black-and-white window. The screen face is the same table on a 320x240 screen, in colour: 16 colours in the colour window, and 64 colours full screen on a board with six or more planes.

    cd sw/apps/mahjong && make        # mahjong.bin
    make test                         # every host test, about 5 seconds
    make render                       # pictures to look at, tests/out/*.png

144 tiles are stacked in a layout, the Turtle being the classic one. Take two tiles that **match** and are both **free**; clear the table to win. Every deal can be won, unless you ask for random ones.

## Playing

- **Free**: nothing lies on the tile, and its left or its right side is open. Tiles above or below it in the same column, or tiles one layer up that only touch its edge, do not pin it.
- **Match**: the same picture. The exceptions are the eight bonus tiles: any flower goes with any flower, and any season with any season.

You only have to tell the pictures apart, not read them. The tiles carry no Western corner indices.

### Mouse

- Click a tile to pick it up, then click its match.
- Clicking a different tile that does not match picks that one up instead.
- Clicking a tile that is not free says so in the bar.
- Clicking the felt puts the tile back.
- The bar's buttons are **New**, **Undo**, **Hint** and **Shuffle**, plus **Zoom** on the screen face.

### Keys

| Key | Does |
|---|---|
| Arrows | move the cursor between the tiles with nothing on them |
| Tab, shift-Tab | the next / previous **free** tile, in reading order |
| Enter, Space | pick up the tile under the cursor, or take the pair |
| Esc | put the tile back, put the hint out (full screen: back to the window) |
| H | hint: a pair you can take; again for the next pair |
| U, Backspace, Ctrl-Z | undo |
| R, Y | redo |
| S | shuffle the tiles that are left (they can still be cleared) |
| N | new deal |
| A | the same deal again, from the start |
| L, shift-L | next / previous layout |
| V, shift-V | turn the view: see the table from another corner |
| F | dim the tiles that are not free |
| D | random deals on or off (from the next deal) |
| Z | zoom, on the screen face: the whole table or close up |
| Wheel | scroll a board too big to show whole: down, or across with Shift, Ctrl or Alt held (a board only too wide scrolls across with the plain wheel) |
| M | music on or off, on the screen face (the arcade runtime's key) |
| C | full screen: black and white, or colour (the runtime's key; F10 does the same there) |
| F1 | help |
| Gamepad (screen face) | stick: the arrows; A: Enter; B: Esc; X: hint; Y: undo; Start: shuffle; Select: new deal |
| F10, the colour icon | the screen face in the window, and back |
| F11 | full screen (the screen face, in game mode) |

The arrows go to the nearest tile in that direction, among the tiles with nothing on them. When nothing lies ahead within a cone of the direction, they go to the nearest tile anywhere on that side, so a press always moves when there is somewhere to go.

A layout's odd shapes can still make one tile awkward to reach with arrows. **Tab** goes round every free tile, so any tile that can be taken is a few presses away. In the tests, a game played by the keyboard alone needed Tab for 6 of 144 tiles.

With speech on ([tts.md](tts.md)), the tile under the cursor is spoken ("seven of bamboo, blocked"), as are picks, hints and messages.

### The bar

The bar shows the layout, the deal's number, how many tiles are left, and the time. A `?` after the number marks a random deal; "stuck" means there are tiles left but no pair to take. Messages replace this text for a few seconds. "Cleared" stays until the next deal.

The clock starts at the first pair and stops when the table is clear. Undoing out of a win starts it again from where it stopped.

## The faces

| | Desktop face | Screen face |
|---|---|---|
| Window | resizable, any size | 320x240 |
| Picture | one bit a pixel | in the window: black and white, or the colour window's 16 colours (8 on a six-plane board); full screen: 64 colours, 16, or black and white |
| Tiles | the largest size that fits the window | Whole (the layout fitted) or Close (Z: larger tiles, scrolled) |
| Sound | none (the desktop face has no frame clock to drive the mixer) | music ("Tea Garden", M turns it off), a click on a pick, a clack on a pair, a fanfare on a win |
| Hint | two textured faces | two faces whose colour breathes (one palette entry, nothing redrawn) |
| Pairs taken | gone | gone in a burst of sparks |

### Depth

A tile is drawn as a face and two sides, its long side and its short side. A layer up is shifted by the side's thickness, away from the sides, so the sides of an upper tile land exactly on the faces under it.

Each tile also casts a shadow, as wide as its side, past its sides onto whatever is lower: the felt, or the faces of the layer below. That shadow is what makes a stack's height readable at a glance, and not only at its edges.

Faces have a light inner edge at the top left and a dark one at the bottom right. Tiles are drawn back to front, so a tile's sides fall under its neighbour's face and only the outer edges of a row show their sides, as on a real table.

| | Faces by layer | Shadows |
|---|---|---|
| 64 colours | a shade per layer, the table darkest | a darker shade of whatever they fall on |
| 16 colours | one ivory, so that a shadow can darken it (below) | as 64 |
| 8 colours | one face colour | none: there is no darker variant among eight, so stacks show by their sides alone |
| Black and white | white | a checkerboard over lit pixels |

### The view from a corner

**V** turns the view: from the front right (the default), front left, back left or back right. It mirrors the geometry and the drawing order, so the sides and shadows move to the other edges and a tall stack no longer hides what is behind it.

The faces themselves are not mirrored, and their light still comes from the top left. The free rule is about the tiles, not the view, so it does not change.

### Music

On the screen face a slow tune plays, "Tea Garden" (`mj_music.c`): G major pentatonic, G A B D E, on three soft channels -- the tune on a sine, a harmony and a bass under it -- and no drums, since this is a game to think in. It plays four rows a second and goes A B A C A B A D, about half a minute before it comes round. **M** turns it off and on, as in the other arcade games. The desktop face is silent: the mixer is stepped by the runtime's frames.

### Zoom and scroll

On the 320x240 screen, the Turtle fits whole with 20x26 tiles. Close up (**Z**) uses 30x40 tiles and the board scrolls:

- to keep the cursor and the hint in view;
- with the mouse wheel: down, and across with Shift, Ctrl or Alt held. A held modifier sends no key events, so the app asks the keyboard directly (`z_kbd_live_mods()`, [zkbd.h](../sw/common/zkbd.h)), as wm's Alt+double-click does; Super is wm's. A board that is only too wide scrolls across with the plain wheel;
- when the pointer rests within 6 pixels of an edge of the board.

A scroll moves the picture already composed and draws only the strip that comes into view (`mj_render_scrolled()`), about 40ms a 4-pixel step in 64 colours on the board.

On the desktop, the window's size chooses the tiles. The large size is used where it fits whole; the default window, 466x356, fits the Turtle at the large size. A layout file too big for the window at the small size scrolls in the same way.

## The set

| Faces | Tiles | Engine numbers | Drawn as |
|---|---|---|---|
| Dots, one to nine | 4 each | 0..8 | circles in blue, red and green, in the traditional patterns; the one is a large rosette |
| Bamboo, one to nine | 4 each | 9..17 | green sticks, some red; the one is a bird |
| Characters, one to nine | 4 each | 18..26 | the numeral in blue over 萬 in red |
| Winds | 4 each | 27..30 | 東 南 西 北 in black |
| Dragons | 4 each | 31..33 | 中 in red, 發 in green, the white dragon a blue frame |
| Flowers | 1 each | 34..37 | 梅 蘭 菊 竹 in red, under a red blossom |
| Seasons | 1 each | 38..41 | 春 夏 秋 冬 in green, under a blue sun |

That makes 42 faces in **36 classes** of four. Two tiles match exactly when their classes are equal (`mj_class()`, `mj_match()`). The blossom and the sun mark the two groups of bonus tiles, so you can see which tiles go together without reading them.

A layout with fewer than 144 places gets a random part of the set, in pairs. A layout with more gets a second set.

### The art

`gen_tiles.py` writes `mj_art.c` and `mj_art.h`, which are committed so the build needs no Python. Run `make tiles` after changing the art; the output must regenerate identically. Its contact sheets go to `tests/out/faces-small.png` and `faces-large.png`.

- **Characters** come from the tree's own Shinonome 12x12 font, `sw/data/font/jp12.zfn` (public domain, there for `sw/apps/jfont`). Nothing is downloaded. The small size uses a glyph as it is. The large size scales it with EPX (Scale2x) to 24x24, and box-filters that to 18x18 where 24 does not fit.
- **Dots and bamboo** are drawn by the script from circles and sticks.
- **Format**: per size and face, one part per colour. A part is a 1bpp mask cropped to its bounding box, with rows as `uint32_t`, least significant bit leftmost.
- **Size**: 160 parts, about 11 KB of rows for both sizes.
- **The tile itself** (face, sides, outline, shades) is drawn by `mj_board.c` from three numbers per size, because it changes with the layer, the state and the view.

| Size | Face | Depth | Used for |
|---|---|---|---|
| small | 20x26 | 3 | the screen face, whole; the desktop when the large does not fit |
| large | 30x40 | 4 | the screen face, close; the desktop |

## Layouts

### The text format

Built-in layouts and layout files share one format and one parser (`mj_layout_parse()`):

    # a comment
    name Pair on a plinth
    layer
    [][][]
    [][][]
    layer
    .[]
    .[]

- Positions are in **half tiles**. A tile is written as its whole 2x2 block of half cells: `[]` on one line and `[]` directly below it.
- `layer` starts the next layer, counting from 0 for the table.
- Every line after `layer` is one row, counted from the top of the layout. Blank lines count as rows too, so all layers line up on the same top line.
- A tile may sit on a half step, for example the Turtle's side tiles and its top tile.
- Use `.` (or a space) for an empty cell. `#` lines are comments and are not rows.
- `name` sets the title. CRLF line endings are accepted.

The parser reports these errors, with the line number where there is one:

- a `[` without its `]`, or a `]` without its `[`
- a tile's bottom half missing
- any other character
- tiles before the first `layer`
- no `layer` at all
- fewer than two tiles, or an odd number
- too many tiles, too wide, too tall, or too many layers (288 tiles, 96x64 half cells, 12 layers)
- more than eight tiles stacked on one

A layout is moved so its top-left corner is at (0, 0). The free rule's neighbours depend only on the layout, so they are worked out once, when it is parsed:

- **above**: tiles on any higher layer whose block overlaps by any amount;
- **left / right**: tiles on the same layer whose block starts exactly one tile to the side and overlaps vertically by any amount.

### Built in (`mj_layouts.c`)

| Layout | Tiles | Size (half cells) | Layers |
|---|---|---|---|
| Turtle | 144 | 30 x 16 | 5 |
| Pyramid | 120 | 16 x 16 | 4 |
| Fortress | 144 | 24 x 16 | 4 |
| Waves (staggered rows) | 126 | 20 x 16 | 4 |

### Your own

Put a `.txt` file in the `layouts` folder of mahjong's data folder: `/data/mahjong/layouts`, which the app creates when it starts. Then use **L** to cycle to it.

- **Name**: the file's `name` line, or its file name without `.txt`.
- **Size**: up to 4 KB a file.
- **Count**: up to 24 layouts in all, the four built-in ones included.
- **A broken file**: one that does not parse is refused with its name and the error in the bar, and the table stays as it was.
- **A shape that cannot be cleared**: if no order of play could empty it (a lone stack of two, say), the bar says it cannot be played.

To start from one of the built-in layouts, copy its text out of `mj_layouts.c`. Each string there is a file's lines.

## Deals

### Every deal can be won

A deal is built **backwards**. The layout is first emptied by taking random pairs of free places, with no faces decided yet. Each pair taken is then given a matching pair of faces. Played forwards, that order clears the table, so a solution exists by construction (`mj_game_t.sol`, which the tests replay).

You can still get stuck by taking other pairs; that is the game, and undo and shuffle are there for it.

Emptying can hit a dead end, with tiles left but fewer than two free. A one-step lookahead avoids most of them, and the rest are started over. In the tests:

- about 2% of 6,000 deals needed a second attempt, and none needed more than three;
- on Fortress, 1 deal in 1,000 needed a second attempt.

### Random deals, numbered deals

- **Random deals** (D) scatter the same tiles with no guarantee that they can be cleared.
- **Numbered deals**: a deal is a number. The engine draws from its own generator (xorshift32, no multiply) seeded with it, not from `zg_rng`. So a deal number is the same tiles on every board and in every test, and so are the shuffles made at the same point in it.
- **New deals** get a number from the ChaCha20 generator, 1 to 99,999,999.
- **A** deals the same number again.

### Shuffle

**S** redeals the tiles that are left onto the places that are left, built backwards in the same way, so they can be cleared. A shuffle **clears undo**, because the pairs taken before it came off a board that no longer exists.

When the places left cannot be emptied in any order, the shuffle is refused: undo, or deal again. In the tests, each refusal is proved hopeless by an exhaustive search.

## Statistics and settings

`/data/mahjong/stats.txt` (named with `z_data_file()`, [zdata.h](../sw/common/zdata.h)) is written:

- after a win;
- at the first pair of a deal;
- when a setting changes;
- when the app quits.

Its format:

    # mahjong: settings, and per layout: played won best-seconds name
    layout Turtle
    random 0
    free 0
    corner 0
    zoom 0
    stat 12 5 333 Turtle

A deal counts as played at its first pair. A win counts with its time, whatever hints or shuffles it took. The bar says when a time is your best for the layout.

Unknown lines are ignored. Deleting the file resets everything.

`run mahjong Fortress` starts on a layout by name.

## How it is built

| File | Does | I/O |
|---|---|---|
| `mj_layout.c`, `mj_layouts.c` | the text format, the neighbour lists; the built-in layouts | none |
| `mj_game.c` | the set, numbered deals, the free rule, take, undo, redo, hints, shuffles | none |
| `mj_art.c` | the faces' pictures (generated) | none |
| `mj_canvas.c` | composes a picture in memory: fills, masks, text, shades, in bitplanes or through inks | none |
| `mj_board.c` | where each tile lands for a size, a corner and a scroll; draws the board and the bar; hit-testing; arrows and Tab | none |
| `mj_play.c` | what a click or a key does: picks, pairs, messages, the clock, statistics, which rectangles changed | none |
| `mj_face.c` | the canvas's memory; sets it up for a face, composing once per mode; recomposes what changed and hands it to the face; the screen face's `paint()` | the arcade runtime |
| `mj_music.c` | the tune and the sounds | none |
| `mahjong.c` | the window, the two faces, files, speech, sound | everything |

### One canvas, two faces

The table is composed in memory and then copied to the screen in one go. That is word arithmetic in RAM, with every overlap exact and no raster ops needed, and the copy is never half drawn. Drawing a dozen overlapping fills per tile straight into a window would flicker, since a window has no back buffer.

- **Desktop face**: the canvas is the window's content in one plane, through the inks. It is shown with `z_fb_hw_blit_mem()` into each visible piece of the window (with a software fallback where the bitstream has no memory blit).
- **Screen face**: the canvas is 320x240 in 3, 4 or 6 bitplanes for 8, 16 or 64 colours, or one plane for black and white. The runtime's `paint()` copies it a plane at a time with `ar_plane()`, or the mono plane with `ar_bitmap_opaque()`.

A move recomposes only the boxes it touched and copies only those:

- the tiles' boxes, which include their sides and shadows;
- with dimming on, every tile whose freedom changed (`mj_free_flags()` before and after);
- the bar.

The canvas's memory is one static array the two faces take turns with: 14,400 words, enough for 320x240 in six planes, and more than the desktop's 640x480 in one.

### Speed

The board's CPU does about 7 million instructions a second (picorv32 at 48 MHz, [dcache.md](dcache.md)), so the cost of a picture is counted in instructions. The first version composed a whole table with a fill for every one-pixel line of every tile's sides and edges, and darkened shadows a pixel at a time, which took about a second -- and a change of mode composed it two or three times over while wm waited. On the board that showed as slow, blocking switches, and as buttons that seemed not to work: a click on New spent over a second before anything changed.

Four things changed:

- **Stamps.** Every tile on a layer has the same body -- sides, face, light and dark edges, outline -- so it is drawn once into a masked stamp and each tile is a copy of two words a row a plane (`mj_cv_stamp()`). There is one stamp per layer shade in 64 colours, and one in 16 and in mono. Stamps are drawn again when the size, corner, colours or planes change. In mono the inks are anchored to the stamp, so every tile's sides are dithered alike.
- **Shadows are one bit.** A colour's shadow is its index with one bit set: 8 in 16 colours, 32 in 64 (see the colours below). A shadow is an OR into one plane, a word at a time, instead of reading back every pixel.
- **Hidden tiles are not drawn.** A tile with a tile exactly on top, and its neighbours on the sides' side (beside, below and diagonally) all there, is covered entirely -- its face by the tile on top, its sides and shadow by the neighbours' faces, all drawn after it. About a third of a full Turtle. `tests/board_test.c` draws every layout, corner, size and depth both ways through whole games and finds them identical.
- **Once per mode.** Entering the screen face and every change of mode after only marks the canvas stale; `mj_face_check()` composes it once, straight before the frame that shows it. Into the window it waits up to six frames for the colour window rather than composing in black and white first. Before the canvas is composed, `paint()` shows the felt.

Glyphs are written all colours at once, a row at a time (`mj_cv_row_multi()`), and wide fills a plane at a time.

Instructions executed, counted on RV32 by running the same code built for rv32im under QEMU:

| | Before | Now | At 7 MIPS |
|---|---:|---:|---:|
| A whole table, 64 colours (with its stamps) | 7,916,204 | 1,435,257 | 0.2 s |
| A whole table, 16 colours | 6,325,815 | 1,084,084 | 0.15 s |
| A whole table, black and white | -- | 780,947 | 0.11 s |
| The desktop window, large tiles | 4,929,406 | 1,273,944 | 0.18 s |
| A pair taken | 337,695 | 118,831 | 17 ms |
| A 4-pixel scroll, close up in 64 colours | a whole table | 269,826 | 39 ms |

A switch between faces or modes is one whole table now, where it was two or three.

### The colours

| Index | Role | Index + 8 (its shadow) |
|---|---|---|
| 0 | the felt | 8: the felt in shadow |
| 1 | black ink | 9: outlines |
| 2 | red ink; the keyboard's frame | 10: dark red |
| 3 | green ink | 11: dark green; the bar |
| 4 | blue ink | 12: dark blue |
| 5 | a tile's long side | 13: the short side |
| 6 | a face; the bar's text | 14: a face in shadow; a face's dark edge |
| 7 | the tile picked up | 15: dark gold; pulses while a hint is up |

In 64 colours, 16..21 are faces by layer (the table darkest), 22..27 the long side by layer, 28..31 the short side, and **32..63 are 0..31 in shadow** (three quarters as bright).

That layout is what makes a shadow one bit. It also means an index drawn through its low three bits -- the colour window on a six-plane board, which shows 8 -- is the same colour out of the shade, so in 8 colours there are no shadows and nothing else changes. The mono inks for the first 16 are in `mj_inks`; the rest fold onto them through `mj_ink_map`.

### Size

| | Bytes |
|---|---|
| `mahjong.bin` on the card | 206,036 |
| image when loaded | 350,700 (144,680 of it zeroed storage) |
| this app's own code (`mj_*.o`, `mahjong.o`) | about 59,000 |
| the faces' art | about 12,600 |
| the canvas | 57,604 |
| the stamps | 16,128 |
| the session (layout, game, view, statistics) | about 13,000 |

Every large buffer is static, because the default stack is 16 KB ([app_runtime.md](app_runtime.md)). Nothing is floating point: `make nofloat` checks the sources, and `make nmcheck` (after `make`) checks that the objects reference no soft-float routine.

## Testing

`make test` (about 5 seconds) runs these suites:

- **includecheck**: every shipped source compiles with no `-I`, which is the header search the board build has.
- **nofloat**: no `float`, `double` or decimal literal.
- **engine_test** (4,469 checks):
  - **The parser**: every error it reports, line numbers, CRLF and comments.
  - **The free rule against a second implementation** written from the geometry, at every eighth move of 6,000 games.
  - **The set**: exactly one set in a deal.
  - **Deals**: 3,000 numbered Turtle deals and 1,000 of each other layout, every one replayed to an empty table.
  - **Undo, redo and hints.**
  - **Shuffles**: 1,200 games played badly into a dead end, shuffled, and replayed to the end. Every refused shuffle is proved hopeless by exhaustive search.
- **board_test**:
  - **The canvas against a one-pixel-at-a-time reference**: 4,000 random operations -- fills, rows, masks, text, one-bit shades, multi-colour rows and stamps -- in mono, 8, 16 and 64 colours, with random clips.
  - **Partial repaints against full ones**: 130 whole games, on every layout, from every corner, at both sizes, in every colour depth, with dimming on and off, and at odd desktop widths. An off-by-one in a tile's box fails 113 checks; that mutation was tried.
  - **Skipping covered tiles changes nothing**: every layout, corner, size and depth, through whole games, drawn with and without.
  - **Scrolling**: over 600 moves in every depth and corner, each compared with a full repaint at the new position; in mono, the moves that would put the dithers out of step are refused, not drawn wrong. Drawing the new strip a pixel short fails it; that mutation was tried.
  - **The pointer**: `mj_hit()` against the picture, pixel for pixel. The picture's owner of each pixel is found by drawing each tile alone.
  - **The arrows** always land ahead, on a tile with nothing on it, and **Tab** visits every free tile once each way.
  - **Fitting and scrolling.**
- **play_test**:
  - **Whole games**: one won by clicks, one by the keyboard alone (arrows, then Tab where they miss).
  - **The clock and the statistics**: a win's time; undo out of a win, and redo.
  - **Every key.**
  - **Layout files**, a good one and a broken one.
  - **stats.txt round trips**, including nonsense lines.
  - **Messages** that time out.
  - **The wheel**: nothing on a board that fits; down; across with a modifier; across on a board only too wide.
- **music_test**: every cell of the tune read with zsfx's own parser; the tune kept to G A B D E; the song played through zsfx's hosted build for half a minute with all three channels sounding; M's off silent; a sound for every event.
- **screen**: the screen face through the arcade runtime's host build (`ar_host.c`), with the real `zarcade.c` and `mj_face.c`. It runs through the window in black and white, the colour window, full screen in 64 colours and in black and white, zoom, a turned view, dimming and another window covering ours, with keys and clicks -- including New and Undo clicked on the bar full screen, which must act. With `AR_HOST_VERIFY`, every frame (52) is compared with a full repaint, and it counts the compositions: one for each mode the screen goes through, never two for the same one.

`make stress` runs the suites under AddressSanitizer and UndefinedBehaviorSanitizer.

`make render` writes the pictures to look at before changing anything visible, in `tests/out`:

- the Turtle in 64, 16 and 8 colours, and in black and white;
- a game in progress, with a selection, a hint and the cursor;
- the blocked tiles dimmed;
- the view from each corner;
- the close zoom;
- the desktop face at both sizes;
- the other layouts;
- the runtime's own frames (`scr_*`).

## Installing

- `mahjong` is in `APPS` in `sw/apps/Makefile`.
- The card image puts `/apps/mahjong` in the games set (`release/lib/mkfatimg.py`).
- The dock icon is `sw/data/icons/icon-mahjong.png`: two 中 tiles side by side, in the game's depth. Its line in `apps.txt` gives the spoken name "Mahjong", and `dock.zic` is regenerated (`gen_dock_icon_data.py --check` passes). It is in the Games group in `sw/data/wm.cfg`.
- `tools/check_fkeys.py` lists mahjong among the dual apps allowed F10.

`dock.zic` goes into the flash archive and `wm.cfg` onto the card, so the icon appears after the OS and the card are rebuilt (or after `update`).

## On hardware: what to check

These can only be checked on hardware; the host tests cannot.

- **The dock**: Games shows the new icon, and speech says "Mahjong". `run mahjong` and `run mahjong Fortress` both start.
- **The desktop window**: opens with the Turtle in large tiles.
  - Resize it smaller: the tiles become small.
  - Cover part of it, then uncover it: it repaints right.
  - Move it, then click: clicks still land on the tile under the pointer.
- **Clicking**: a pair goes; a blocked tile says so; Hint, Undo and Shuffle work -- in the window and full screen. New is a whole table: about 0.2 s.
- **Switching** (F10, F11, Esc, C): each switch about a fifth of a second, the rest of the desktop not held up.
- **F10 and the colour icon**: the table in colour in the window, in 16 colours. On a six-plane board, 8 colours, with no shadows.
- **F10 again, or resizing**: back to the desktop face, at the size it had.
- **F11**: full screen in 64 colours where the board has them.
  - Shadows and a shade per layer visible.
  - The hint breathes, a pair goes in sparks with a clack, and a win plays its fanfare.
  - C (or F10): black and white, and back. Esc: back to the window.
  - Music plays from the start; M stops it and starts it again.
- **A gamepad**, full screen: a whole game without the keyboard.
- **Z on the screen face**: close up. The board scrolls with the arrows, the wheel (down; across with Shift, Ctrl or Alt) and the pointer at an edge.
- **V**: the four corners, with the sides and shadows on the other edges.
- **The keyboard alone** (speech on): arrows, Tab, Enter, H, U, S, N, A, L. Speech says the tile under the cursor and "blocked".
- **A whole game cleared**: the time, and "your best". `stats.txt` as above, and it survives a restart along with the settings.
- **A layout file**: drop a copy of a built-in one, renamed, into `/data/mahjong/layouts`. L reaches it; a broken one is refused with its error.
- **Close zoom** while the board scrolls, at 48 MHz: about 40 ms a 4-pixel step in 64 colours.

## Not done

- **No sound on the desktop face**: the mixer is stepped by the runtime's frames, which the desktop face does not run.
- **No way to type a deal number**: A replays the current one, and the number is in the bar to note down.
- **Best times count every win**, whatever hints or shuffles it took.
- **No shadows in 8 colours**: the colour window on six-plane boards has no darker variants to shade with.
- **In 16 colours every layer's face is the same ivory**, so that a shadow can darken it with one bit; layers show by their sides and shadows. 64 colours has a shade per layer.
