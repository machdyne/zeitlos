# gldemo

Kind: a **screen app** -- a 320x240 screen in a window or full screen ([app_kinds.md](app_kinds.md)).

zgl's demo and test scene ([zgl.md](zgl.md)). A lit, smooth-shaded
sphere turns in the middle; a flat-shaded cube goes round it, passing
behind and in front; a ring of lines circles both. All of it is filled
and depth-tested in `ZGL_FRAMES`, on the arcade runtime
([arcade.md](arcade.md)). The window, full screen, colour and the
colour window are the runtime's.

```
> run gldemo
```

It runs on every board, in software. `run gldemo soft` keeps the
window in software even where there is an engine, to compare. On a bitstream with the triangle
engine ([gpu_tri.md](gpu_tri.md)), the engine draws its black-and-white
window whenever one rectangle of it is visible.

## Chunky, and the triangle engine

```
> run gldemo chunky
> run gldemo chunky soft
```

On a board with chunky game mode, `chunky` runs the scene full screen
at a byte a pixel, without the arcade runtime, as Doom does
([zgl.md](zgl.md), "Chunky").

- **Who draws it.** The triangle engine draws it if the bitstream has
  one ([gpu_tri.md](gpu_tri.md)); otherwise the software strips write
  straight to VRAM. `soft` uses the software even where there is an
  engine, to compare.
- **What it reports.** The first line on the console says which one is
  drawing. Every two seconds it prints:
  - the frame rate, and the cycles a frame takes to draw;
  - where those cycles go: the scene's GL calls (the front end), with
    their instructions; `zgl_frame()` (the engine's commands, or the
    software strips), with its instructions; and the wait in
    `glFinish()`.
- **On the way out** it clears chunky VRAM. Its second sheet is the
  desktop's accent bits when the desktop is in colour, and the
  picture's bytes would otherwise tint it in stripes.
- **Keys.** W, S, D, L and Space work as in the window (below). Esc, or Super+Esc,
  ends it.

## Keys

| Key | Does |
|---|---|
| W | wireframe or solid (`glPolygonMode`) |
| S | smooth or flat shading (`glShadeModel`) |
| D | depth test on or off |
| L | lighting on or off |
| P | 256, 64 or 16 colours in full screen (`ar_set_colours`) |
| Space | pause |
| F11 | full screen (game mode), or back to the window |
| C | in full screen: colour or black and white (the runtime's) |
| F1 | help |

The bottom line shows frames a second, the triangles and lines drawn,
the colours and the modes.

## What it shows

- **Lighting.** GL's colour-index lighting: each vertex's index lies
  between the material's ambient and diffuse indices
  (`glMaterial(GL_COLOR_INDEXES)`), according to its normal against the
  light. One directional light comes from the upper left.
- **Ramps.** Each object has a ramp of palette entries, made with
  `zglRamp`, from dark to light. With 256 colours a ramp has 32 entries,
  with 64 it has 16, and with 16 it has 4. The ramps start at index 16
  and leave the runtime's first 16 alone; with 16 colours they use
  1..12, and 15 stays white for the text. Fractions between two
  entries are dithered.
- **Black and white.** In the window, and in mono full screen, each
  index's brightness from the palette is dithered instead.
- **The sphere** is an icosahedron whose faces are each cut in four,
  twice: 320 triangles, half of them facing the eye.

## Tests

`make -C sw/apps/gldemo render` runs the demo on the build host
through the arcade runtime's host layer
(`sw/common/games/tests/ar_host.c`). It writes nine pictures to
`sw/apps/gldemo/tests/out/`:

- the window;
- full screen in 256 colours;
- wireframe, flat, no depth and unlit;
- 64 and 16 colours;
- mono full screen.

What a frame costs is measured by `make -C sw/common/zgl bench`
([zgl.md](zgl.md), "Cost").
