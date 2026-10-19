# The arcade runtime

`sw/common/games/zarcade.[ch]`: one 320x240 game screen, in a window or
full screen, in colour or monochrome, for `snake` and the arcade games
that follow it (`blocks`, `bricks`). Sound is its companion,
[zsfx](sfx.md).

space3d, chip8 and gamedemo each carry their own answers to the same
questions -- how to draw into a window other windows overlap, how to go
full screen and come back, which keys do that, how colour becomes
bitplanes, how a keyboard and a gamepad become one set of buttons, how
the frame is paced. This is those answers, once.

## The model

A game draws one **320x240 picture** in **logical coordinates** (0,0 is
the top-left of the play area) and in **colour indices** 0..15. Where it
lands is the runtime's business:

| Mode | What it is |
|---|---|
| `AR_MODE_WINDOW` | a fixed 324x255 window whose content is exactly 320x240; monochrome; clipped to the part no other window covers |
| `AR_MODE_FULL_MONO` | game mode ([game_mode.md](game_mode.md)), pixel-doubled, monochrome |
| `AR_MODE_FULL_COLOR` | game mode with 16 colours: four bitplanes in the four quadrants (`z_color_16`, [color.md](color.md)) |

The window and full screen are the same picture pixel for pixel: the
window's content rectangle is the window inset by two on every side
below the 11-row titlebar (`z_win_content_rect()`), so asking for
324x255 gives 320x240 to draw in.

In colour an index is a palette entry. In monochrome every index has an
**ink**: an 8x8 pattern anchored to the screen. A game says "this is
colour 9" once, and the monochrome picture follows from its ink table
-- solid white for the snake, a 75% grey for the second snake, black
for the floor. `ar_ink_level()` makes any of the 17 ordered-dither
greys the blitter's shade fills use.

## Single-buffered, on purpose

A window has no off-screen VRAM to draw into (the whole framebuffer is
desktop), and 16 colours use all four quadrants. So no mode has a back
buffer, and the runtime is built around drawing only what changed:

**Paint.** The game's `paint(user, rect)` callback draws the static
picture inside one rectangle: the floor, the walls, the snake's body,
the status line, a panel of text. The runtime calls it for each
rectangle marked dirty with `ar_dirty()`, with every drawing call
clipped to that rectangle, so `paint()` may draw whole cells that
overlap it and not care.

**Overlays.** Whatever moves every frame -- a sliding head, a ball,
sparks, a score rising -- is submitted again each frame with `ar_ov_*`.
The runtime compares the list with last frame's, entry by entry:

- an overlay that moved, changed or went away marks where it **was**
  dirty, so it is painted over
- an overlay that is new, or that something under it just repainted,
  is drawn -- and anything submitted after it that overlaps it is drawn
  again, so the order on screen is the order of submission
- an overlay that did not change is not touched at all

A game therefore never erases anything. The host test
(`make -C sw/common/games test-arcade`) holds the runtime to exactly
that: random frames of moving sprites, fills, particles and model
changes, in every mode and with another window covering ours, each
compared with a repaint from nothing. Not one pixel may differ.

Everything is drawn straight after the frame boundary (the palette
first, then the viewport, then the drawing), so a frame's few changes
land while the beam is at the top and almost never tear.

### Keeping a moving sprite on the glass

Single-buffered has one hazard of its own. Between repainting the tiles
under a moving sprite and drawing it again, the sprite is not on the
screen; if the beam passes in that moment it is missing for a frame --
a snake's head or a ball blinking out. The frame boundary does not
always save it: the wait for the frame counter sleeps in ticks, other
processes run, and a frame whose logic overran starts drawing wherever
the beam happens to be.

So the runtime keeps that moment short. It collects the dirty runs
first, works out which overlays will be drawn, paints every run that
no overlay is over, and then goes overlay by overlay in submission
order: the runs under it, and at once the overlay itself. Every run
under an overlay is still painted before the overlay is drawn, and
overlapping overlays still land in submission order, so the picture is
exactly the one "paint everything, then draw the overlays" gave (the
verifier below checks it, every frame). What changes is how long a
sprite is gone: in the host renders, measured in blitter operations
from the first repaint under it to its drawing, on average, with the
same scripts and nothing else changed

| | before | after |
|---|---|---|
| snake | 38.4 | 25.7 |
| blocks | 44.2 | 19.5 |
| bricks | 27.7 | 13.4 |

(`make render` prints it as "off the screen for ... operations"). The
worst case is a frame that repaints everything -- entering full screen
-- where nothing can help.

### Overlays behind things, and inside an area

An overlay is drawn after the paint, so it is always in front. Two
calls let it look otherwise:

- `ar_ov_sprite_part(s, sx, sy, w, h, x, y)` draws only a rectangle of
  a sprite, so a sprite passing behind a panel can be drawn as the
  pieces still outside it (blocks' title shapes pass behind the menu
  and the letters this way, a pixel at a time, instead of losing a
  whole block at once). `ar_sprite_part()` is the same in a paint.
- `ar_ov_area(x, y, w, h)` keeps every overlay inside one rectangle,
  cut at its edge: snake sets it to the field, so a head sliding out of
  the top of the open field is cut at the status line rather than
  drawn over it or hidden.

### The cost

picorv32 at 48MHz retires a few million instructions a second, so a
60Hz frame is on the order of a hundred thousand for everything. The
per-frame work here is linear in what changed: overlays are compared
in order rather than searched, and the dirty map is 30 rows of 40
tiles walked as words. Colour costs one blitter operation per plane,
which is the unavoidable part ([color.md](color.md), "Drawing"). A
dirty tile of snake floor is six operations in colour (three runs of
the dotted pattern in one plane, a solid fill in each of the others)
and one in mono.

## The loop

```c
ar_init(&desc);                       /* window, mixer, everything */
for (;;) {
    ar_event_t e;
    while (ar_poll(&e)) handle(&e);   /* keys, buttons, mouse, quit */
    for (i = 0; i < dt; i++) step();  /* the game's rules */
    submit overlays; ar_dirty() what changed;
    dt = ar_frame();                  /* draw, wait for the frame */
}
ar_shutdown();
```

`ar_frame()` returns the display frames that actually elapsed, 1 when
keeping up. Stepping the world that many times keeps its speed when
the machine is busy ([gamedemo.md](gamedemo.md), "The frame rate is
the smoothness"). It waits on the display's frame counter, yielding
while it waits (a game that spins takes a full scheduler share from
everything else, [app_runtime.md](app_runtime.md)); on a board whose
counter does not count it falls back to the tick clock after a tenth
of a second.

## Keys the runtime owns

| Key | Does |
|---|---|
| F1 | help box, in the window only ([app_help.md](app_help.md)); nothing in game mode |
| F11 | full screen, or back to the window |
| Esc | in full screen: back to the window, once released (as space3d) |
| C | in full screen: colour on or off |
| M | music on or off ([sfx.md](sfx.md)) |

These follow [keys.md](keys.md): F1 is help (and nothing in game mode), F11
is full screen. The runtime creates its window with
`Z_WIN_FLAG_GAME_MODE`, so wm passes F11 on to the game rather than
making the window full screen itself.

Everything else reaches the game as `AR_EV_KEY` -- and the player
controls also as `AR_EV_BUTTON`. While a game takes typed letters
(`ar_text_input(true)`: initials for a high score), C and M reach it as
letters too.

While the help box is up the game gets `AR_EV_SUSPEND` first (and
pauses), the sound is held, and every button is released afterwards:
the releases went to the box.

## Players

Two players, each with four directions, A, B and START:

| | Player 0 | Player 1 |
|---|---|---|
| directions | arrow keys | W A S D |
| A | Space | Q |
| B | Backspace | E |
| START | Enter | Tab |
| gamepad | the first pad | the second pad |
| X, Y, SELECT | the pad's own buttons (no keys) | the same |

A one-player game reads `ar_any_held()` and `ar_any_pressed()` and
takes all of them. Letters are case-folded, so a key pressed with Shift
and released without it does not stay held. X, Y and SELECT exist only
on a pad; a game that wants "any face button" asks for `A | X`.

Button events come in the order they happened (keyboard edges as they
arrive, pad edges once a frame), which is what a snake's turn queue
needs. `ar_repeat()` gives hold-to-repeat for menus and sideways
movement.

There are two USB host ports ([gamepad.md](gamepad.md)), so two pads
means no keyboard: a game played with two pads must be startable from a
pad (START).

### The mouse

`AR_EV_MOUSE` carries the pointer in logical coordinates and its
buttons, whenever it moves or a button changes (wm sends nothing for a
mouse at rest). In the window, wm delivers it while the window has the
focus and the pointer is over it, and `inside` says whether it is over
the 320x240 content; the coordinates may be outside 0..319 near the
edges. Full screen, wm hands every pointer event to the game wherever
it is (`game_grab_pid` in wm.c), and the runtime halves the desktop
position, so the pointer's whole range is the whole picture and
`inside` is always 1. A click is the game's own business: keep the
last `buttons` and look for the change, as bricks does for launching.

## Drawing

| Call | Draws |
|---|---|
| `ar_fill(x, y, w, h, c)` | a rectangle |
| `ar_fill2(x, y, w, h, c0, c1, &pat)` | c1 where the pattern is set, c0 elsewhere: floors, bricks, textures in one fill per plane |
| `ar_bitmap(bits, stride, sx, sy, x, y, w, h, c)` | a 1bpp bitmap in colour c, transparent elsewhere |
| `ar_text(x, y, s, c, font, scale)` | text, scaled 1..4, transparent background |
| `ar_sprite(&s, x, y)` | a masked multicolour sprite |
| `ar_copy(sx, sy, dx, dy, w, h)` | VRAM to VRAM in every plane; false where it cannot (a window partly covered), and then the caller repaints instead |

Copies belong in the descriptor's `before_paint(user)` hook, which the
runtime calls at the start of each frame's drawing -- straight after the
frame boundary, before `paint()` -- so a copy lands with the rest of the
frame instead of half way down the picture. A copy moves whatever is in
its rectangle, overlays included, so keep overlays out of what you copy.
And the paint must know what the copies did: blocks' paint draws the
falling stack at the same offsets its copies moved it to
([blocks_app.md](blocks_app.md), "A clear").

`ar_fill2` in monochrome combines the two inks through the pattern --
an 8x8 screen-aligned pattern of two 8x8 screen-aligned patterns is
still one -- so it is exact, not approximated.

Sprites are built once from rows of characters (`.` transparent,
`0`-`9` `a`-`f` an index), optionally recoloured, and kept as up to four
bitplanes, a mask and a monochrome rendering (the inks applied at
sprite-local coordinates). They are drawn with the blitter's one-pass
masked mode where the board has it ([gpu_blitter.md](gpu_blitter.md)),
which is atomic and so safe on the visible page. They come from a 24KB
pool that lives for the whole run (`-DAR_POOL_WORDS=` for more).

Labels (`ar_label_make()`) render text once into the game's own buffer
for use as an overlay, like a rising "+10". A label re-rendered into
the same buffer is not noticed as a change: mark its rectangle dirty.

### Particles

`ar_parts_burst(x, y, n, c0, nc, speed, life, gravity)` throws `n`
2x2 sparks in colours c0..c0+nc-1, with drag and an optional pull
downward; `ar_parts_frame(dt)` moves them and submits them as overlays.
Sixty-four at once; a burst past that replaces the oldest.

### The palette and the shake

`ar_pal(i, rgb444)` stages an entry; the runtime writes the changed ones
at the next frame boundary. Most of the colour effects in these games
cost nothing else: a rainbow flowing down a snake is four palette
entries recomputed every frame, with not one cell redrawn.

`ar_shake(amp, frames)` jolts the whole picture by moving the
viewport register a few pixels each frame (full screen only; wrap is on
for this). Nothing is redrawn. What slides in at an edge comes from the
neighbouring quadrant, which in colour is another bitplane: keep the
outer `amp` pixels of the picture in colour 0, as snake does with its
walls, and the edges stay black.

## Inside the window

Each frame the window's visible region is read once (what
`z_win_content_rect()` loads), turned into rectangles, and zgfx's own
region is then cleared: the runtime cuts every operation to those
rectangles itself and hands the blitter only pieces that are wholly
visible. That is the only correct way to do copies
([gpu_blitter.md](gpu_blitter.md), "Copy and the scissor"), and it lets
fills, bitmaps and sprites share one clipping path. A `Z_WM_SET_CLIP`
marks the rectangles stale, so drawing between frames never uses a
region that has since narrowed.

While wm has the window frozen (a drag over it), nothing is drawn and
nothing dirty is forgotten. A `Z_WM_REDRAW` repaints everything and is
acknowledged after the frame that did it; in full screen it is
acknowledged at once.

The window has the close icon but not `CLOSE_KILLS_OWNER`, as chip8: a
game owns mixer channels that read its own memory, and being killed
would leave them reading memory that has just been freed. The close
icon arrives as `AR_EV_QUIT` and the game shuts down in order.

## Scores

A top-ten table per game and per mode in `/data/<app>/scores.txt`
([layout.md](layout.md); the folder comes from `zdata.h`), one line per
entry:

```
0 645 ZED
0 74 AAA
2 1210 BOB
```

mode, score, three initials. `ar_scores_load/save/rank/insert`; lines
of other modes are kept as they were.

## Building a game on it

```make
SRC_DIRS = $(COMMON_DIR)/games
OBJS = zeitlos.o zobj.o zgfx.o zcolor.o zfont_data.o zwin.o zwidget.o \
	zspeak.o zdialog.o zfsapp.o \
	zarcade.o zarcade_sys.o zsfx.o \
	<the game's own objects>
```

`zarcade.c` is the arithmetic half -- dirty tiles, overlays, colour and
ink expansion, sprites, text, the buttons, scores -- and includes no
Zeitlos header beyond the font type. `zarcade_sys.c` is the half that
talks to the machine (the window, wm's messages, game mode, the frame
counter, the files, the blitter), through the small `arh_*` interface
in `zarcade_int.h`.

## On a build host

`sw/common/games/tests/ar_host.c` is a third implementation of the
machine half: a 640x480 software framebuffer with the blitter's
semantics, the 16-colour scanout, a palette, a viewport, and a window
that another "window" can cover. A game links it in place of
`zarcade_sys.c` and runs its real loop, driven by a script of key
presses, mode changes and snapshots:

```
make -C sw/apps/snake render       # out/*.png and out/sheet.png
make -C sw/common/games test-arcade
```

The renders are how every screen of a game is reviewed before it
reaches a board.

Built with `-DAR_HOST_TESTS` and run with `AR_HOST_VERIFY=1` (every
game's `make render` does), the host also **repaints everything from
nothing after every frame** and compares: the incremental picture --
dirty tiles, overlays, the game's own copies -- must be exactly what a
full repaint gives. A frame that differs is reported with the area it
differs in, and written out as `out/verify_<frame>.ppm` (red: only in the
incremental picture; green: only in the repaint). It found two bugs in
snake that no screenshot showed: a head cell not repainted when a move
started (two pixels at the corners of its arm), and the length in the
status line not updated when a dissolving snake reached zero.

A script can also take pictures when something happens rather than at a
frame: it defines `ar_host_hook(frame)`, called after every frame, and
calls `ar_host_snapshot(name)` -- blocks' render catches the first clear
flashing, the stack mid-fall and the curtain this way.

`ARH_MOUSE` in a script is the pointer moving to a logical (x, y) with
the buttons in `a`, delivered as `AR_EV_MOUSE`; bricks' render starts
its game with a click and moves the paddle with it.
