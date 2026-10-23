# space3d

Kind: a **screen app** -- a 320x240 screen in a window or full screen ([app_kinds.md](app_kinds.md)).

A first-person wireframe space game: fly through an asteroid field,
shoot rocks and the occasional weaving UFO, past drifting galaxies.
Everything is drawn by the line rasterizer (`rtl/gpu/gpu_raster.v`),
on the arcade runtime ([arcade.md](arcade.md)): the window, full
screen, colour and the colour window are the runtime's.

```
> run space3d
```

The design notes -- camera-relative coordinates, integer-only
projection, the app's own line clipping, and the incremental display
list that erases only what moved -- are in the comment at the top of
`sw/apps/space3d/space3d.c`.

## Keys

| Key | Does |
|---|---|
| Arrows (or a gamepad) | steer (the ship strafes) |
| Space (or the pad's A) | fire |
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

The lines are drawn with `ar_plane_line()`, a plane at a time, in
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

## On a build host

`make render` in `sw/apps/space3d` runs `space3d.c` itself on the
runtime's software framebuffer (`tests/render.c`): the window, steering
and firing, covered and uncovered, the colour window, F10 off and on,
full screen in colour and mono, F1. `tests/out/*.png` and `sheet.png`.
