# blocks

A falling-blocks game: shapes of four blocks fall into a well; turn and
steer them so they fill rows, and a full row clears. Don't let the stack
reach the top. It runs in a 320x240 window or full screen -- in sixteen
colours where the board has them -- with sound effects and music.

```
> run blocks
```

Built on the arcade runtime ([arcade.md](arcade.md)) and its sound engine
([sfx.md](sfx.md)). The source is `sw/apps/blocks/`.

## Ways to play

| | |
|---|---|
| **Endless** | for points; faster every ten rows; start at any level from 1 to 10 (left/right on the menu) |
| **40 lines** | clear forty rows as quickly as you can; the speed stays at level 1; the best ten times are kept |
| **3 minutes** | as many points as you can in three minutes |

The best ten of each are kept, with initials, in `/data/blocks/scores.txt`
([arcade.md](arcade.md), "Scores"). A 40-line time is stored as 9999999
minus its frames, so that the table's "highest first" puts the fastest
first.

## Keys

| Key | Does |
|---|---|
| Left / Right | move; hold to slide (after 10 frames, a column every 2) |
| Down | fall faster (half a row a frame), a point a row |
| Up, X | turn clockwise |
| Z | turn the other way |
| Space | drop at once, two points a row |
| H | hold the shape for later (once per shape) |
| P | pause (Start on a pad) |
| Esc | in the window: pause; again for the menu |
| F1 | help, in the window |
| F11 | full screen, or back to the window ([keys.md](keys.md)) |
| C | in full screen: colour on or off |
| M | music on or off |

W A S D work as the arrows, Q drops and E holds.

On a gamepad: the d-pad moves, **Up drops**, A turns clockwise, B the
other way, X or Y holds, Start pauses. Up turns on a keyboard and drops
on a pad, because that is what each one's players expect; the runtime
says which a button came from (`AR_EV_BUTTON`'s `source`).

## The rules

- **The well** is 10 wide and 20 deep, with two more rows above it where
  shapes appear. A shape that cannot appear, or one that locks entirely
  above the well, ends the game.
- **Seven shapes**, named by the letter each looks like: I O T S Z J L.
  They come from a shuffled bag of all seven, so the same shape never
  comes more than twelve apart and none goes missing for long. The next
  five are shown.
- **Turning** happens about the centre of the shape's box. A turn that
  does not fit is tried at up to four nearby offsets -- the wall kicks --
  before it is refused, using the kick tables players know.
- **Locking:** a shape that has landed locks after half a second (30
  frames). Moving or turning it restarts that, up to fifteen times;
  falling to a new lowest row resets the count.
- **Holding** swaps the shape for the held one (or the next, the first
  time), once per shape. The held shape is grey while it cannot be used.
- **Speed** follows the familiar curve, from a row a second at level 1 to
  everything at once at level 20.

### Points

| | at level *n* |
|---|---|
| one row | 100 × *n* |
| two | 300 × *n* |
| three | 500 × *n* |
| four | 800 × *n* |
| a twist (no rows / 1 / 2 / 3) | 400 / 800 / 1200 / 1600 × *n* |
| back to back (a four or a twist after another) | half as much again |
| combo (rows cleared by shape after shape) | + 50 × combo × *n* |
| an empty well after a clear | + 3000 × *n* |
| soft drop / hard drop | 1 / 2 a row |

A **twist** is a T whose last move was a turn and that ends with three of
the four corners of its box taken (walls and floor count).

## How it is drawn

The well is 100 x 200 pixels in the middle, cells of 10 pixels; HOLD and
the score on the left, NEXT on the right, and below the score the
**announcer**, which says what just happened: SINGLE ... FOUR!, TWIST,
CLEAR!, COMBO 3, BACK TO BACK, LEVEL.

The stack, the boxes and the text are painted by the runtime's dirty
rectangles; the falling shape and its **ghost** (an outline where it would
land), the flashes, the hard-drop streaks, sparks and the title's rain are
overlays ([arcade.md](arcade.md), "Single-buffered, on purpose").

A block has a light edge on its top and left, a small glint, and a
one-pixel gap: one sprite, drawn the same in the well as on the falling
shape.

### A clear

1. **Flash** (16 frames): the rows blink white, and every block in them
   bursts into a spark in its own colour. A four, a twist and an empty
   well shake the screen.
2. **Fall** (10 frames): the rows go, and everything above falls into the
   gap, accelerating. The falling stack is **not redrawn**: each run of
   rows that falls the same distance is moved down in video memory by the
   blitter (`ar_copy()`, every bitplane), from the runtime's
   `before_paint()` hook so the copies land at the frame boundary with the
   rest of the frame. Only the strip each run leaves behind is painted.
   The landing shakes the screen a little and thuds.
3. The rules then close the gap for real (`bk_clear_done()`); the picture
   is already what it would draw.

The paint knows the falling offsets too, so a repaint in the middle of a
fall (a window moved away, the help box closed) draws exactly what the
copies made -- and in a window that another window partly covers, where
a copy cannot be done safely, the well is simply repainted at those
offsets each frame instead.

A copy moves pixels, so a block must look the same wherever it is drawn:
in colour every block is solid colours, and in monochrome every shape's
texture is a pattern that is the same on every row (vertical stripes of
different widths), so moving down by any number of pixels still matches
a fresh paint.

### Colour

| Entry | Is |
|---|---|
| 0 | black: the well's floor, the panels, the shake's border |
| 1-7 | the shapes: I cyan, O yellow, T purple, S green, Z red, J blue, L orange |
| 8 | the **theme**: frame, backdrop, grey blocks |
| 9-15 | the shapes' light edges (entry *s* + 8); 15, the L's, is white, and is the white of text and flashes |

That is, a shape's colour is planes 0-2 and its light edge adds plane 3.
The theme changes with the level, and breathes with the music
(`zsfx_music_rows()`): once a bar, rising and falling over a second, a
quarter of the way to pale blue. (It used to flash on every beat, which
pulled the eye away from the well.) The music speeds up a little each
level.

When the game is over the well greys over from the bottom, row by row.

### The title

The title is spelt in blocks, over a backdrop through which five shapes
fall at different speeds, behind the letters and the menu -- cut at
their edges a pixel at a time (`ar_ov_sprite_part()`), not hidden a
block at a time.

The backdrop is a field of staggered dots in the theme colour. It was a
diagonal, a different pattern byte on every row, which costs the
blitter twice the operations of the dots for every tile repainted
behind something moving.

## Sound

Effects for moving, turning (and kicking), locking, landing, holding, the
hard drop, one to four rows (rising), a twist, combos (a step up the scale
each time), back to back, an empty well, a new level, the countdown, game
over and a goal reached. Two original tunes: "Drift" on the title, and
"Lattice" in the game.

## Source

| File | Is |
|---|---|
| `bk_game.[ch]` | the rules: shapes, turning and kicks, the bag, gravity, lock delay, hold, points, twists, the three modes, and the autopilot. No drawing; host-tested |
| `bk_draw.c` | the picture: paint, overlays, the clear's flash and copies, effects |
| `bk_sound.c` | the effects and the two tunes |
| `blocks.c` | the screens, the controls and the loop |

The rules do not remove completed rows at once: they mark them
(`clear_rows`) and wait, so the picture can show them going;
`bk_clear_done()` removes them and brings the next shape.

The **autopilot** (`bk_ai_plan()`) tries every turn of the shape (and of
the held one) at every column, dropped straight down, and scores the well
it leaves: rows cleared, minus total height, holes and bumpiness, with a
well-known set of hand-tuned weights. It survives hundreds of shapes. The
host renders play with it.

## Tests

```
make -C sw/apps/blocks test      # the rules, with ASan and UBSan
make -C sw/apps/blocks render    # every screen, as pictures
```

The rules tests cover the shapes in every turn, the bag (each shape a
hundred times in 700, never more than twelve apart), moving to the walls,
a kick off the wall, a twist (and the same rows filled without a turn not
being one), a four that empties the well, back to back, combos, the lock
delay and its limit, gravity and soft and hard drops landing where the
ghost said, holding, topping out, all three modes, and the autopilot
surviving 400 shapes.

`render` runs the real game for 12,000 frames on the host's software
framebuffer with the autopilot playing -- the title in every mode, an
endless game in colour, in a window, in a window partly covered by
another (where the fall has to repaint instead of copy), in monochrome,
to topping out, initials and the table, then forty lines to the end --
and with `AR_HOST_VERIFY` it checks **every frame** against a repaint
from nothing ([arcade.md](arcade.md), "On a build host"), every frame of
every fall included. Not one differs.
