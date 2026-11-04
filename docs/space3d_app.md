# space3d

Kind: a **screen app** -- a 320x240 screen in a window or full screen ([app_kinds.md](app_kinds.md)).

A first-person space game: fly through an asteroid field, shoot rocks
and the occasional weaving UFO, past drifting galaxies. Everything is
drawn through zgl, the GL 1.0-style 3D API ([zgl.md](zgl.md)), on the
arcade runtime ([arcade.md](arcade.md)): the window, full screen,
colour and the colour window are the runtime's.

It has two looks, and **V** switches between them:

- **Wireframe** is the classic look and the fast one.
  `GL_LINES` and `GL_POINTS` in `ZGL_LINES` are drawn by the line
  rasterizer (`rtl/gpu/gpu_raster.v`).
- **Solid** draws the rocks and UFOs as lit, flat-shaded solids, with
  hidden surfaces removed. It uses `ZGL_FRAMES`, and each frame is
  drawn whole ("The solid look" below).

Which one it starts in is the machine's: the game runs in the most
colours the screen has and on the best hardware there is.

- **In the window**, the wireframe. V gives the solid look, drawn in
  software.
- **In game mode (F11) in colour**, on a board with chunky game mode,
  the solid look in chunky: 256 colours, each frame drawn into chunky
  VRAM by T2 where the bitstream has it ([zgl_t2.md](zgl_t2.md)), else
  by the triangle engine or in software ("Chunky" below). V gives the
  wireframe on the same frames.
- **In mono game mode, or on a board without chunky**, as in the window.

```
> run space3d
```

The design notes -- camera-relative coordinates, the frustum that
gives the game's projection, and zgl's display list that erases only
what moved -- are in the comment at the top of
`sw/apps/space3d/space3d.c`.

## Keys

| Key | Does |
|---|---|
| Arrows (or a gamepad) | steer (the ship strafes) |
| Space (or the pad's A) | fire |
| V | wireframe or solid |
| F11 | full screen (game mode), or back to the window ([keys.md](keys.md)) |
| C | in full screen: colour on or off |
| F10 | colour on or off, in the window too (its colour window) |
| Esc | leave full screen (once released) |

A crash strobes the screen and starts a new run.

**F1** shows a short help on the game's own screen ([screen_ui.md](screen_ui.md)), in the window and full screen alike.

## In colour

On a board with game-mode colour ([color.md](color.md)), full screen
is sixteen colours -- and so is the window, while it has the colour
window (focused and uncovered, on an eight-plane board: 32 colours,
16 used) -- used the way the Amiga called **dual playfield**:

| Planes | Holds |
|---|---|
| 0-2 | the wireframe, in seven line colours |
| 3 | a nebula: the runtime's picture under the lines (`paint()`), which the lines never touch |

The game was already single-buffered: each frame it erases only the
lines that changed and redraws them all. That keeps working unchanged,
with one difference -- the erase clears planes 0-2 and leaves plane 3
alone. A line passing over the nebula therefore never damages it, and
the nebula never has to be repaired; the runtime repaints it, black and
all, only where it repaints anything (uncovered, after a box, a mode
change), and the lines there are back a frame later. Palette entries 8-15
are the same colours as 0-7 seen over the nebula: 8 is the nebula
itself and 9-15 repeat the line colours, so lines look the same in
front of it as in front of empty space.

The lines are drawn by zgl with `ar_plane_line()`, a plane at a time
(`zgl_desc_t.index_bits` 3: planes 0-2, so plane 3 stays the nebula's), in
whichever layout the runtime has: full screen's quadrants, or the
colour window's folded planes. The colour window on a six-plane board
has eight colours: the wireframe, and no nebula.

### Line colours

| | Colour | Drawn as |
|---|---|---|
| 1 | white | near stars and their streaks; the score text |
| 2 | green | reticle and corner brackets; UFOs |
| 3, 4 | twinkling | far stars, in two groups |
| 5 | amber | asteroids |
| 6 | violet | galaxies |
| 7 | fire | missiles and explosions |

An erased line clears three planes; a drawn line sets only the planes
its colour has bits in, one to three. Where two lines of different
colours cross, the shared pixel takes both colours' bits: a stray
mixed point at a crossing, invisible in motion, and the price of not
clearing before drawing. The score text is drawn into plane 0, so it is
colour 1 (white) wherever it lands.

### The palette does the rest

Sixteen register writes a frame, no drawing:

- **Twinkling.** Far stars are split between two entries whose
  brightness cycles out of phase, so half the field brightens while the
  other half dims -- with not one star redrawn.
- **Fire.** Missiles and explosions share an entry that flickers
  through yellows and oranges every frame.
- **Galaxies** pulse gently between two violets.
- **The nebula** drifts slowly through purples and blues.
- **A crash** strobes in fire colour, and the nebula flares red with
  it.

### The nebula

A few soft blobs summed into a density field and thresholded through a
4x4 ordered dither -- at most ten pixels in sixteen lit, so it reads as
haze rather than a solid shape, with edges that feather into specks. It
is computed once into a 320x240 bitmap in memory, the first time it is
shown, and drawn into plane 3 by `paint()` wherever the runtime
repaints. The clouds are kept off the centre so the reticle sits on
dark space.

In black and white -- the window without the colour window, mono full
screen -- the lines are plane 0, white, and there is no nebula.

## The solid look

V reopens zgl in `ZGL_FRAMES`. Each frame is then drawn from nothing,
in this order:

1. The galaxies and stars, as points and lines, without the depth test.
2. The rocks and UFOs, depth-tested and lit. They cover the sky, and
   hide each other where they overlap.
3. The shots and explosions, as lines, depth-tested, so a shot behind
   a rock is hidden by it.
4. The HUD, on top.

A rock is its octahedron's eight triangles, and a UFO is its rim, its
dome and six sides. Each face has one normal, and the faces are made
into display lists once at the start. One light shines from above and
to the left, behind the ship.

The colours:

- **Sixteen colours or more.** The seven line colours stay where they
  are, and palette entries 8 to 15 are a ramp for the rocks, eight
  shades from dark to light. A UFO is lit from its green to white,
  dithered between the two.
- **The colour window with eight colours.** White, green, the stars,
  fire, and a ramp of three for the rocks.
- **Black and white.** The palette's brightness is dithered. The lines
  are white, and the UFOs take the rocks' ramp so they keep their
  shape.

There is no nebula in the solid look: every plane belongs to the
frame.

### Chunky

In game mode in colour on a board with chunky game mode (the arcade
runtime's `AR_MODE_FULL_CHUNKY`, [arcade.md](arcade.md), "Chunky"),
zgl opens in `ZGL_FRAMES` with `ZGL_CHUNKY`, and the frames go straight
to chunky VRAM. On a T2 bitstream the rocks' display lists are compiled
once and each frame's are copied to the engine, which transforms,
lights, clips and draws them: the CPU's part of a frame is the stream.
The console says which: `space3d: game mode, chunky: T2`.

- **256 colours.** The line colours as before, then 32 shades for the
  rocks (entries 16-47) and 16 for the UFO, from its green to white
  (48-63), so the light falls off smoothly rather than in eight bands.
- **The score** is drawn as lines, seven-segment figures, since the
  runtime draws no text on a chunky picture.
- **The crash strobe** is frames of nothing but their clear colour.
- **F1** puts the help over the planes' 256-colour mode while it is up
  (the runtime's doing); the game comes back to chunky after it.

The wireframe in chunky (V) is the same frames unlit: no depth test,
the line colours.

Each frame costs the CPU what a frame costs `ZGL_FRAMES` ([zgl.md](zgl.md),
"Cost"). That is about 0.12 M instructions for an empty sky, and more
as rocks come near and fill the picture. The game steps by the clock
either way: on a slow board, frames are dropped, not time.

## On a build host

`make render` in `sw/apps/space3d` runs `space3d.c` itself on the
runtime's software framebuffer (`tests/render.c`). The script covers:

- the window, steering and firing;
- covered and uncovered;
- the colour window, with F10 off and on;
- full screen in colour and in mono;
- F1;
- the solid look in the window, in full-screen colour (a rock coming
  near) and in mono, then back to the wireframe.

The pictures are written to `tests/out/*.png` and `sheet.png`.

`make render-chunky` plays game mode on a board with chunky
(`tests/render_chunky.c`): the window, game mode in colour, steering
and firing, a rock, the wireframe and back, F1, mono and colour again,
and the window. It runs twice, once with zgl's software strips and once
with T2's stream carried out by its model (the geometry firmware and
the raster's model, [zgl_t2.md](zgl_t2.md), "Tests"), and holds the two
to each other picture by picture: they agree to the pixel. It also says
how the T2 frames were made (the rocks as chunks from the lists, no
triangle set up by the CPU) and the most records a frame used (about
270 of the engine's 512). The T2 run's pictures are in
`tests/out_t2/`.
