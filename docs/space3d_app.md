# space3d

A first-person wireframe space game: fly through an asteroid field,
shoot rocks and the occasional weaving UFO, past drifting galaxies.
Everything is drawn by the line rasterizer (`rtl/gpu/gpu_raster.v`).

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
| Arrows | steer (the ship strafes) |
| Space | fire |
| F2 | full screen (game mode), or back to the window |
| C | in full screen: colour on or off |
| Esc | leave full screen (once released) |

A crash strobes the screen and starts a new run.

## Full screen in colour

On a board with game-mode colour ([color.md](color.md)), full screen
is sixteen colours, used the way the Amiga called **dual playfield**:

| Planes | Holds |
|---|---|
| 0-2 | the wireframe, in seven line colours |
| 3 | a nebula, drawn once when full screen starts and never again |

The game was already single-buffered: each frame it erases only the
lines that changed and redraws the rest. That keeps working unchanged,
with one difference -- the erase clears planes 0-2 and leaves plane 3
alone. A line passing over the nebula therefore never damages it, and
the nebula never has to be repaired or redrawn. Palette entries 8-15
are the same colours as 0-7 seen over the nebula: 8 is the nebula
itself and 9-15 repeat the line colours, so lines look the same in
front of it as in front of empty space.

The layout is `z_color_16` with the viewport at (0,0): plane 0 is the
top-left quadrant, 1 the top-right, 2 the bottom-left, 3 the
bottom-right.

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
is computed once into a 320x240 bitmap in memory, the first time
colour is switched on, and blitted into plane 3's quadrant in one
operation. The clouds are kept off the centre so the reticle sits on
dark space.

The window is unchanged: monochrome, as the desktop is.
