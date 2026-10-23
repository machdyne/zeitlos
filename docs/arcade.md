# The arcade runtime

`sw/common/games/zarcade.[ch]`: one 320x240 game screen, in a window or
full screen, in colour or monochrome, for `snake` and the arcade games
that follow it (`blocks`, `bricks`), `pix`, `planedemo`, `basic`,
`space3d` and `kidgames` -- the runtime for screen and full-screen apps
([app_kinds.md](app_kinds.md)). Sound is its companion, [zsfx](sfx.md).

chip8 and mausrun each still carry their own answers to the same
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

### Racing the beam

A game that redraws a lot of the screen every frame -- whole planes
scrolled, a layer cleared and drawn again -- can't rely on "straight
after the boundary" as above, for two reasons:

- the wait sleeps in scheduler ticks (1.4 ms at 732 Hz), so it notices
  the boundary up to a tick late, and the palette is written then;
- the program does its drawing as it works the frame out, so the
  blitter's operations are spread across the frame, wherever the beam
  is.

On the board that showed as a sprite flashing, a 3D object with
horizontal bands of half-new planes, and a scroller with columns
missing (planedemo r3). Two calls let a program do better:

| Call | Does |
|---|---|
| `ar_vsync_exact(true)` | `ar_frame()` sleeps until two ticks before the boundary it expects, then watches the frame counter (FRAME, [game_mode.md](game_mode.md)) without yielding: it returns within microseconds of the start of blanking. It learns where the boundaries fall from each one it sees while watching. Costs the last two ticks of each frame in a spin. |
| `ar_frame_hook(fn)` | `fn` runs in `ar_frame()` straight after the boundary and the palette (and viewport), before the runtime's own drawing. A program that queued its frame's drawing runs it there. |

The timing to race: the frame counter ticks at the end of the last
visible line; then 45 blank lines (about 69,000 cycles of the 48 MHz
CPU, a line being 1,525), then in game mode a picture row every two
lines (about 3,050 cycles a row). Drawing that starts at the boundary
and goes from the top down, a band of rows at a time, stays ahead of
the beam with most of the frame to spare. planedemo does this
([planedemo.md](planedemo.md), "Racing the beam").

Two more things help:

- `ar_plane()` is **one** blitter pass (the COPY ROP) where the
  bitstream has the ROPs (`z_fb_hw_rop_available()`): before, it
  cleared the rectangle and then ORed, and the plane was blank in
  between. Without the ROPs it still clears and ORs.
- `ar_plane_line_rows(k, x0, y0, x1, y1, op, y0clip, y1clip)` draws
  only a line's pixels in some rows, the same pixels the whole line
  sets there (the rasterizer clips pixel by pixel), so a line can be
  drawn a band at a time. `test-arcade` checks both.
- `ar_plane_line()` draws in black and white too -- the window, mono
  full screen -- where plane 0 is the picture, a covered window's line
  cut to each rectangle that shows: space3d draws the same lines in
  every mode, a plane at a time in colour. `test-arcade` checks the
  window's pixels, covered and not, against mono full screen's.

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
the machine is busy ([mausrun.md](mausrun.md), "The frame rate is
the smoothness"). It waits on the display's frame counter, yielding
while it waits (a game that spins takes a full scheduler share from
everything else, [app_runtime.md](app_runtime.md)); on a board whose
counter does not count it falls back to the tick clock after a tenth
of a second.

## Keys the runtime owns

| Key | Does |
|---|---|
| F1 | help: on the program's own screen, window and full screen alike, when it links the screen toolkit ([screen_ui.md](screen_ui.md)); otherwise the desktop's box, in the window only ([app_help.md](app_help.md)) |
| F10 | colour on or off: full screen as C; in the window, the colour window, for a program that asked for it (`ar_window_colour()`) on a board with one -- until F10 again |
| F11 | full screen, or back to the window |
| Esc | in full screen: back to the window, once released (as space3d); a program that keeps it (`ar_escape_leaves(false)`: basic) gets it instead |
| C | in full screen: colour on or off |
| M | music on or off ([sfx.md](sfx.md)) |

These follow [keys.md](keys.md): F1 is help, F10 colour, F11
is full screen. The runtime creates its window with
`Z_WIN_FLAG_GAME_MODE`, so wm passes F11 on to the game rather than
making the window full screen itself.

A program with a loop of its own -- basic's interpreter, a line at a
time -- calls `ar_poll()` as often as it likes and `ar_frame()` when
`ar_frame_due()` says a frame has passed, so it draws without waiting.
Messages the runtime does not know (a port's) go to `ar_msg_hook()`;
one another library read (zi2cx waiting for bench) is handed back with
`ar_msg()`.

Everything else reaches the game as `AR_EV_KEY` -- and the player
controls also as `AR_EV_BUTTON`. While a game takes typed letters
(`ar_text_input(true)`: initials for a high score), C and M reach it as
letters too.

While the help box is up the game gets `AR_EV_SUSPEND` first (and
pauses), the sound is held, and every button is released afterwards:
the releases went to the box.

### More than 16 colours

A program draws in 16 colours unless it asks for more:
`ar_set_colours(64)` or `(256)` says how many it draws in, and returns
how many full-screen colour will show on this board -- 16, 64 or 256,
at most what was asked (`z_color_planes_max()`, [color.md](color.md)).
The runtime then uses `z_color_64` or `z_color_256` instead of
`z_color_16`: six or eight planes, the first four in the quadrants as
before and the rest in the second sheet, 480 rows down. Every drawing
call takes indices up to 255, `ar_pal()` has 256 entries (16 and up
start as entry `i & 15`), and sprites keep up to eight planes. An index
past what the board shows is drawn through its low bits; a program that
wants better reduces its art itself, once, as it loads it
([zsp.md](zsp.md)).

| Call | Does |
|---|---|
| `ar_set_colours(n)`, `ar_colours()` | 16, 64 or 256; what is shown |
| `ar_mono_map(to16)` | in monochrome, index c in `ink[to16[c]]` instead of `ink[c & 15]` |
| `ar_sprite_from_indices(s, px, opaque, w, h)` | a sprite where every index, 255 too, is a colour; `opaque` says which pixels are drawn |
| `ar_plane(k, bits, ...)` | one plane of a picture, as it is (in colour only: full screen or the colour window): a picture of any indices without a fill per colour, a call per plane -- pix's canvas |
| `ar_plane_or(k, bits, ...)`, `ar_plane_andn(k, bits, ...)` | plane k's bits set, or cleared, where the bitmap has a bit: a shape in one plane, the rest left alone |
| `ar_plane_fill(k, x, y, w, h, bit)` | a rectangle of plane k to 0 or 1 |
| `ar_plane_line(k, x0, y0, x1, y1, op)` | a line in plane k by the line rasterizer: op 0 clear, 1 set, 2 xor |
| `ar_plane_line_rows(k, ..., y0clip, y1clip)` | the same line's pixels in rows y0clip..y1clip only |

With these a program can treat groups of planes as **layers** --
draw each into its own planes, and let the palette decide, for every
index, which layer is in front -- and animate mostly by rewriting the
palette. planedemo is built that way ([planedemo.md](planedemo.md),
"Planes as layers"). The runtime keeps the blitter and the rasterizer
in order: a line after a blit waits for the blit, and a blit after
lines waits for the lines.

The window and mono full screen are unchanged. snake, blocks and bricks
ask for nothing and run as before, and their renders still match a full
repaint frame for frame. On the host, `AR_HOST_PLANES=4` or `6` makes
`ar_host.c` a board with fewer planes (8 by default); its framebuffer
has both sheets.

### The pointer

wm hides the mouse pointer while a game holds the screen, and -- in the
window -- while it is over the game's picture: the runtime creates the
window with `Z_WIN_FLAG_NO_POINTER`, and wm hides the cursor while it
is over that window's content and the window is the topmost there
(not while a window is being dragged). Past the picture's edge, on the
titlebar or another window, it is back.

A program drawn with the mouse sets `pointer` in its `ar_desc_t` (the
last field; a game leaves it false) and the pointer stays, in both: the
window has no `Z_WIN_FLAG_NO_POINTER`, and the runtime calls
`z_game_set_pointer()` before game mode, which puts
`Z_WM_GAME_GRAB_POINTER` in the grab it sends wm. pix and kidgames do.
An older wm ignores both, and the pointer shows over the window and
hides full screen as before.

### The colour icon

Where the board has the colour window, the runtime's window has the
colour titlebar icon (`Z_WIN_FLAG_COLOUR_ICON`, a rainbow): a click does
what F10 does.

### A dual app

A dual app ([app_kinds.md](app_kinds.md)) has a desktop window of its
own and lends it to the runtime for its 320x240 screen face. It creates
the window resizable with `ar_window_flags(&desc)` among the flags (game
mode, so F11 is the app's; colour, the colour icon, no pointer for a
game). `ar_attach(&win, &desc)` takes it: the first time it sets the
runtime up as `ar_init()` would, without a window of its own; it
resizes the window to 320x240 and reads its messages from then on
(`ar_poll()`; the ones it does not know go to `ar_msg_hook()`). The app
then runs as a screen app: `ar_poll()`, its drawing, `ar_frame()`. An
app that is typed at (chess's moves, poker's commands) calls
`ar_text_input(true)` after attaching, so C and M reach it as letters
rather than the colour and music keys.

While attached, F10, the colour icon and a resize by the user (the
grip) send `AR_EV_DESKTOP`, and the app calls `ar_detach()`: out of
game mode, the colour window given back, the window its desktop size
again -- the size it had, or the user's new one -- and its messages
the app's again; wm's REDRAW at the new size follows. Events still
queued are dropped. The window's minimum size must let it be 320x240 inside
(no `Z_WIN_FLAG_MIN_IS_CREATE` above 324x255): wm clamps a resize to it. F11 from the desktop face is the app's to send to
`ar_attach()` and `ar_set_fullscreen(true)`.

On a build host `ar_attach()` stands the host window up as `ar_init()`
does, and F10 sends `AR_EV_DESKTOP` while attached.

### Dialogs

A program on the runtime may open the system's boxes (`zdialog.h`:
open, save, a prompt, a question) -- pix does, for its files. A box is
a window, and wm shows no windows over game mode, so the call is
bracketed:

```c
z_dialog_ctx_t ctx;
ar_dialog_begin(&ctx);      /* leaves full screen, paints, holds the sound */
ok = z_dialog_open(&ctx, ...);
ar_dialog_end();            /* sound back, every key released, repainted */
```

Opened from full screen, the box shows in the window and full screen
comes back at the next `ar_frame()` -- not at once, so that a second
box straight after the first (save first? then the file's name) does
not flick the screen to full and back between them.

`ar_dialog_begin()` fills `ctx` so the box keeps the window painted
while it waits, as the F1 box does. On a build host both are no-ops
(`ar_host.c`); a script answers the program's questions itself.

### A box on top

The [screen toolkit](screen_ui.md) draws its boxes on the program's own
screen rather than as windows. The runtime keeps such a box on top:

```c
ar_top_t t = { paint_box, box, { x, y, w, h }, inks, true };
ar_top(&t);        /* the box is up */
...
ar_top(NULL);      /* gone; its area is repainted */
```

Every dirty rectangle the program's `paint()` repaints is followed,
where it meets `area`, by the box's `paint`, clipped to it; with
`opaque`, a dirty rectangle wholly inside the box is not painted by the
program at all. In monochrome the box's indices 0..15 are drawn in its
own `ink` table, not the program's, so a box looks the same in every
program. While a box is up the program's overlays are not drawn, its
`before_paint()` is not called, and Esc, F1, C and M reach the program
(the box) as keys -- F11 still goes between the window and full screen.
`ar_push()` puts an event on the queue: what a box took that was the
program's (a mode change, the window closing) is handed back that way.

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

## Colour in the window

On a board with the colour window ([color.md](color.md), "The colour
window"), a program that asks -- `ar_window_colour(true)` before
`ar_init()`; snake, blocks and bricks do -- is drawn in colour in its
window too, while it is the focused window and nothing covers it: 16
colours (its own palette) on an eight-plane board, 8 on six. wm says
which window may have it (`Z_WM_COLOR`); the runtime takes it at a frame
when its window is wholly uncovered and gives it up when it is not.

Then `ar_color()` is true and `ar_mode()` is still `AR_MODE_WINDOW`;
`AR_EV_MODE` comes when it goes on or off, and the picture is repainted
either way. Index c is drawn with bit 0 in the window's own pixels (so
the desktop's picture of it is a black-and-white one of sorts) and its
other bits in the window's planes, folded into the second sheet
(`plane_pieces()` in `zarcade.c` cuts a rectangle where the fold
wraps). Everything is drawn as full screen in colour -- the
plane-at-a-time calls too (`ar_plane()` and the rest, planes
0..log2(`ar_colours()`)-1, plane 0 the window's own pixels; a line is
drawn once per piece of the fold, the piece its clip) -- except
`ar_copy()`, which says no (the program repaints instead).

The palette: colour i is written to every entry the window's planes
can show with i's low bits (`z_color_window_entry()`), so a plane the
program does not draw -- the fifth, for a 16-colour game on eight
planes, still holding what pix or space3d left there -- changes
nothing on the glass.

On a build host `ARH_WCOLOR` (`a` 1 or 0) stands for wm's message; the
host's window sits where the fold wraps inside it both ways, and
`test-arcade` checks the window's indices against full screen's, pixel
for pixel, on eight and six planes, and its frames against full
repaints.

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

Each skill keeps its own tables (below): a game passes
`ar_skill_table(mode)` as the mode, which is `mode` itself on normal --
so the tables written before skills existed are normal's -- and
`mode + 16` on easy, `mode + 32` on hard.

## Skill

Every arcade game has three settings: **easy**, **normal** and
**hard**. What each means is the game's business -- slower, longer, more
lives, a crash that costs a heart rather than the game -- but the
choice is the runtime's, so that it works the same everywhere:

- It is the **top line of each game's menu**, `< EASY >`, changed with
  Left and Right (Enter steps it too), and spoken when it changes.
- **Easy is the default.** The games are meant to be playable by a
  five-year-old as they start; normal is the game as it was before
  skills, unchanged, and hard starts where a normal game is after a
  while.
- It is remembered per game in `/data/<app>/settings.txt`, a line
  `skill easy` (other lines in the file are kept).
- Without a saved choice it comes from `apps.arcade.skill` in
  `zeitlos.cfg` ([config.md](config.md)) -- `easy`, `normal` or `hard`
  -- and then from easy. A saved choice wins over the configuration,
  so a child's machine can be set to `normal` and still be put back to
  easy from the menu.

```c
int s = ar_skill();                       /* AR_SKILL_EASY .. AR_SKILL_HARD */
ar_skill_set(AR_SKILL_NORMAL);            /* and written to settings.txt */
ar_skill_name(s);                         /* "EASY", for the menu */
ar_skill_spoken(s);                       /* "Easy", for ar_say() */
ar_scores_load(ar_skill_table(mode), &t); /* this skill's table */
```

The rules files take the skill as a plain number (`sn_new_skill()`,
`bk_new_skill()`, `br_new_skill()`, with their own `*_SKILL_*` names of
the same values), so they need nothing from the runtime and their host
tests can play every skill.

### Saying what is selected

`ar_say(text)` speaks a line through the text-to-speech service
([tts.md](tts.md)), interrupting whatever it was saying, and is silent
and cheap when speech is off. The games say each menu line as the
cursor reaches it ("Classic", "Skill, easy"), so a child who cannot
read yet still knows what Enter will do.

### Calm music

All three games start their music calm and slow and let it grow with
the game -- the tune and bass alone at first, the drums only later --
through `zsfx_music_intensity()` ([sfx.md](sfx.md), "Calm first").
The title screens play at intensity 3: no kick and no snare.

## Building a game on it

```make
SRC_DIRS = $(COMMON_DIR)/games
OBJS = zeitlos.o zobj.o zgfx.o zcolor.o zfont_data.o zwin.o zwidget.o \
	zspeak.o zdialog.o zfsapp.o \
	zarcade.o zarcade_sys.o zsfx.o zcfg.o \
	<the game's own objects>
```

`zarcade.c` is the arithmetic half -- dirty tiles, overlays, colour and
ink expansion, sprites, text, the buttons, scores -- and includes no
Zeitlos header beyond the font type. `zarcade_sys.c` is the half that
talks to the machine (the window, wm's messages, game mode, the frame
counter, the files, the configuration, speech, the blitter), through
the small `arh_*` interface in `zarcade_int.h`. `zcfg.o` is for
`apps.arcade.skill`.

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

A key step's `b` is the modifiers held (`Z_KBD_MOD_*`), 0 for none:
pix's render undoes with `{ f, ARH_TAP, 0x1a, Z_KBD_MOD_CTRL, ... }`.
