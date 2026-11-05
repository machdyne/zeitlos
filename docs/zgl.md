# zgl

A GL 1.0-style 3D API for Zeitlos programs: `sw/common/zgl/`,
`#include <GL/gl.h>` (with `-I sw/common/zgl`) and `zgl.h`.

zgl uses GL 1.0's names and values, so small GL 1.0 programs port. It is
written for Zeitlos from GL 1.0's specification and is not OpenGL (a
Khronos trademark); it claims no conformance.

It draws into the arcade runtime's 320x240 picture ([arcade.md](arcade.md)).
The window, full screen, F11, colour and the colour window are the
runtime's, as for every Zeitlos game.

**Status: Phase 2.** Both outputs work on every board:

- `ZGL_LINES` draws lines and points, and polygons as outlines, through
  the line rasterizer. It is space3d's wireframe look.
- `ZGL_FRAMES` draws whole frames in software: filled, flat or smooth
  shaded, lit, depth-tested, in any colour count or in black and white.
  gldemo uses it ([gldemo_app.md](gldemo_app.md)), and so does
  space3d's solid look ([space3d_app.md](space3d_app.md)).

Display lists work in both.

T2, the engine that runs the whole frame, is in the SOC behind
`GPU_T2`, and zgl drives it for chunky frames ([zgl_t2.md](zgl_t2.md)).
Phase 4's first engine, gpu_tri, drew only the pixels; it was retired.

The phases to come are listed at the end.

## Using it

```c
#include "zarcade.h"
#include "zgl.h"            /* GL/gl.h with it */

ar_init(&desc);
zgl_desc_t zd = { ZGL_LINES, 0, 512 };   /* output, index bits, max lines */
zgl_open(&zd);

glMatrixMode(GL_PROJECTION);
glLoadIdentity();
zglFrustumi(-40, 40, -30, 30, 64, 100000);
glMatrixMode(GL_MODELVIEW);

for (;;) {
	/* ... ar_poll() ... */
	glLoadIdentity();
	glTranslatex(0, 0, -400 << 16);
	glRotatex(angle << 16, 0, 1 << 16, 0);
	glIndexi(5);
	glBegin(GL_LINES);
	glVertex3i(-50, 0, 0); glVertex3i(50, 0, 0);
	glEnd();
	zgl_frame();            /* this frame is the picture now */
	ar_frame();
}
```

The Makefile needs:

```
SRC_DIRS += $(COMMON_DIR)/zgl
APP_CFLAGS += -I$(COMMON_DIR)/zgl
OBJS += zgl.o zgl_lines.o
OBJS += zgl_frames.o          # for ZGL_FRAMES
OBJS += zgl_lists.o           # for display lists
```

### Whole frames

```c
zgl_desc_t zd = { ZGL_FRAMES, 0, 0, 1024 };   /* ..., max triangles and lines */
zgl_open(&zd);
zglRamp(16, 32, 0x013, 0x8cf);                /* palette 16..47: dark to light blue */

GLint ramp[3] = { 16, 47, 47 };               /* ambient, diffuse, specular */
glMaterialiv(GL_FRONT_AND_BACK, GL_COLOR_INDEXES, ramp);
glLightxv(GL_LIGHT0, GL_POSITION, dir);       /* w = 0: a direction */
glEnable(GL_LIGHT0);
glEnable(GL_LIGHTING);
glEnable(GL_DEPTH_TEST);
glEnable(GL_CULL_FACE);

/* each frame: */
glBegin(GL_TRIANGLES);
glNormal3x(...); glVertex3x(...);  /* ... */
glEnd();
zgl_frame();                       /* drawn and out to the picture */
```

A frame is drawn from nothing every time, so there is nothing to
invalidate, and the program draws its HUD on top after `zgl_frame()`.
Colour is index mode, as GL's: lighting and smooth shading move along a
ramp of palette entries (see "ZGL_FRAMES" below). Call `zglRamp` again
on `AR_EV_MODE` if the ramps depend on the colour count.

### Chunky

A program that runs chunky game mode itself, as Doom does
([color.md](color.md), "Chunky"), can have its frames go straight into
chunky VRAM: `zgl_desc_t.flags = ZGL_CHUNKY`, and link `zgl_chunky.o`.

```c
zgl_desc_t zd = { ZGL_FRAMES, 0, 0, 1024, ZGL_CHUNKY };
z_game_set_enabled(true, false);
z_color_set_chunky(true);
zgl_open(&zd);          /* zgl_info()->backend: T2 or SOFTWARE */
/* ... each frame: GL calls, zgl_frame(); glFinish() to wait for it */
```

- **With T2** in the bitstream ([zgl_t2.md](zgl_t2.md)), the frame
  streams to T2 as it is made, and `zgl_frame()` ends it and returns.
  T2 draws while the program goes on. `glFinish()` waits for it.
- **Without it**, or with `ZGL_NO_ENGINE`, the software strips go to
  VRAM a byte a pixel, whole words where they can. There is no
  conversion to planes.
- **The palette** is the program's: `z_color_set_palette()`. The
  arcade runtime is not running, so `zglRamp()` does not reach the
  hardware.
- **No text.** zgl draws no text, and the runtime's `ar_text` is not
  there.

`run gldemo chunky` is the example ([gldemo_app.md](gldemo_app.md)).

### Lines

On `AR_EV_MODE`, call `zgl_invalidate()`. The picture has been repainted
under zgl, so last frame's lines are not on the glass any more and must
not be erased. Call it too after the program has filled the picture
itself.

## What there is

| | |
|---|---|
| Primitives | `glBegin`/`glEnd`: `GL_POINTS`, `GL_LINES`, `GL_LINE_STRIP`, `GL_LINE_LOOP`, `GL_TRIANGLES`, `GL_TRIANGLE_STRIP`, `GL_TRIANGLE_FAN`, `GL_QUADS`, `GL_QUAD_STRIP`, `GL_POLYGON` (convex, up to 32 corners) |
| Vertices | `glVertex2/3` in `i`, `x` (16.16) and `f`; `glVertex3iv`, `glVertex3fv` |
| Colour | `glIndexi` (the palette's index: the runtime's colours); `glColor3ub/x/f` take the nearest corner of the colour cube, index `r \| g << 1 \| b << 2` |
| Lighting | `glNormal3x/f/i`, `glLightxv/fv`, `glLightx/f` (`GL_POSITION`: a direction, w 0, or a position, w not 0; `GL_DIFFUSE`; `GL_CONSTANT/LINEAR/QUADRATIC_ATTENUATION` for a positional light; lights 0 and 1), `glMaterialiv/fv` (`GL_COLOR_INDEXES`), `GL_LIGHTING`, `GL_NORMALIZE`: GL's colour-index lighting, diffuse only. On T2 a positional light is lit in the model's space, which needs a modelview of rotation, translation and one scale (otherwise the list is played back) |
| Matrices | `glMatrixMode`, `glLoadIdentity`, `glPush`/`PopMatrix` (16 modelview, 2 projection), `glLoadMatrix`/`glMultMatrix` (`x`, `f`), `glTranslate`, `glScale`, `glRotate` (`x`, `f`), `glFrustum`, `glOrtho` (double and `x`), `glViewport`, `glScissor` |
| State | `glEnable`/`glDisable`/`glIsEnabled` (`GL_CULL_FACE`, `GL_SCISSOR_TEST`, `GL_DEPTH_TEST`, `GL_LIGHTING`, `GL_LIGHT0/1`, `GL_NORMALIZE`), `glPolygonMode`, `glCullFace`, `glFrontFace`, `glShadeModel` |
| Other | `glClearIndex` (`ZGL_FRAMES`' background), `glClear` (a frame is drawn from nothing anyway), `glFlush`, `glFinish`, `glGetError`, `glGetString` |
| Display lists | `glGenLists`, `glNewList` (`GL_COMPILE`, `GL_COMPILE_AND_EXECUTE`), `glEndList`, `glCallList`, `glDeleteLists`, `glIsList`: lists 1..255, called 8 deep |
| Textures | On T2 with textures only (below): `glTexCoord2i` (texels), `glTexCoord2x` (16.16), `glBindTexture(GL_TEXTURE_2D, n)`, `GL_TEXTURE_2D`; `zglTexImage`, `zglTexRoom` |
| Extensions | `zglTranslatei` and `zglFrustumi` (whole units, for worlds wider than GLfixed's ±32767), `zglRamp` (palette ramps for shading), `zglSin`/`zglCos` (1/65536 turns) |

### Textures

On a T2 with textures ([zgl_t2.md](zgl_t2.md), "Textures"), in
`ZGL_CHUNKY` frames, a display list's triangles can be textured:

```c
zglTexImage(1, 4, 4, texels, ZGL_TEX_TONE, false);   /* 16x16, name 1 */
glNewList(l, GL_COMPILE);
glBindTexture(GL_TEXTURE_2D, 1);
glBegin(GL_QUADS);
glTexCoord2i(0, 0);   glVertex3i(...);              /* texels */
glTexCoord2i(64, 0);  glVertex3i(...);              /* four times across */
...
glEndList();
glEnable(GL_TEXTURE_2D);                            /* when it is called */
glCallList(l);
```

A texture is 2^lw x 2^lh texels (1 to 128 a side), a byte each,
repeated both ways, drawn perspective-correct. A texel is not a colour
but combines with the colour index the triangle would have had: DECAL
the texel itself, ADD the two added, TONE the triangle's ramp (16
entries) with the texel's tone (0..15) lit by the triangle's own shade,
RAMP the texel's ramp and tone lit; with `key`, texel 0 is not drawn.
TONE lets one texture of tones serve any ramp and keeps the lighting.
`GL_TEXTURE_2D` decides at the time a list is called, so a program can
turn textures off without making its lists again. Lists keep the
texture places they were compiled with: make the textures first.
Without textures (no T2, a T2 without them, the software strips,
immediate mode) the same triangles are drawn in their colours.

`GL_DEPTH_TEST`, shading and lighting only show in `ZGL_FRAMES`. There,
each triangle and line keeps the depth test as it was when it was
drawn, as GL's do. A sky can be drawn untested, then the solids
tested, then a HUD untested on top. In
`ZGL_LINES`, lines are drawn in submission order in their vertices'
index.

The `f` and `d` entry points convert on entry. A program that calls none
of them carries no float conversion of its own (`printf`'s is another
matter).

## How it works

### Numbers

| Quantity | Format | Why |
|---|---|---|
| matrices | 12.20, except column 3 | ratios; 2/320 in an orthographic view needs more than 16 bits of fraction |
| matrix column 3 (translation, the w row's constant) | 24.8 | positions |
| positions | 24.8: ±8,388,607 units, to 1/256 | a game world is wider than ±32767; a unit cube still resolves to 1/256 |
| clip coordinates | 20.12 | eye-space sized; an orthographic view's ±1 needs more fraction than 8 bits |

Products are summed in 64 bits. On RV32IM that is a `mul` and a `mulh`,
with no library call.

### The pipeline

Each `glVertex` goes from object to clip coordinates through the
combined projection × modelview. That combined matrix is made again
only when either matrix changes, over the projection's non-zero entries.

A vertex cache (64 entries, by object position) keeps a model's shared
corners from being transformed and projected once per line or
triangle; GL_LINES and GL_TRIANGLES repeat every corner. A cached
corner also keeps its lit index, for as long as the normal and index
it was lit with stay the same.

Lines are clipped in clip space against the six planes of the view
volume, so a line passing behind the eye never folds onto the screen:

- With a perspective projection, near and far are clipped against w
  itself, from glFrustum's own near and far. With a far plane thousands
  of times the near one, clip z hardly changes per unit of depth, and
  clipping against it put the near end of a long line pixels out.
- The parameter along the line has 30 bits.

After clipping, the line is divided by w (the 32-bit hardware divide)
and mapped to the viewport. Picture coordinates are (0,0) at the
top-left and y down; `glViewport` counts from the bottom, as GL does.

Polygons are culled by facing (screen area; a determinant in clip space
for one crossing the near plane). In `ZGL_LINES` they are drawn as
their outlines, in the last vertex's colour (GL's provoking vertex).

### ZGL_LINES: the display list

There is no back buffer: a window has no off-screen VRAM, and in
colour the planes are the picture. So a frame is a list of lines.

`zgl_frame()` erases last frame's lines that changed or went (drawing
them in colour 0) and draws this frame's. It compares the two lists
index by index, so a still scene stays still: nothing is touched where
nothing moved, as space3d always did.

A colour index is drawn in planes 0 to n−1, with n from
`zgl_desc_t.index_bits` (0: all the runtime shows). A plane gets the
line where the index has that bit, and an erase clears all n. A
program can keep planes for itself; space3d's nebula is plane 3. In
monochrome, index 0 is black and the rest white.

### ZGL_FRAMES: strips

There is no frame-sized buffer. Each triangle (filled polygons are
clipped in clip space against the view volume, Sutherland-Hodgman, and
cut into a fan) is projected and set up once: sorted by y, with the
gradients of depth and colour index across it. It is kept in a list
(`zgl_desc_t.max_prims`, 72 bytes each).

`zgl_frame()` then draws the viewport in strips of 16 rows. For each
strip:

1. A colour buffer (320×16 bytes) and a depth buffer (320×16, 16
   bits) start clear.
2. Every triangle and line that touches the strip is drawn into them.
3. The strip goes out to the picture.

That is 15 KB, which stays in the cache. T2 draws the same pictures in
hardware, and `zgl_frames.c` is its reference.

- **Coverage.** A pixel is drawn when its centre is inside, with the
  top-left rule, so triangles that share an edge draw each pixel once.
  Each edge is walked exactly, a quotient and remainder per row, with
  no division per row.
- **Depth** is z/w to 16 bits, with 15 bits of fraction while
  interpolating. The test is "less than"; it is cleared to the far
  plane.
- **Colour** is an index with 8 bits of fraction, from smooth shading
  or lighting. It is dithered (4×4 ordered) between the two palette
  entries either side, so a ramp of 16 entries shades smoothly. Flat
  shading uses the polygon's last vertex, as GL does.
- **Lighting** is GL's colour-index lighting. A vertex's index is the
  ambient index plus the diffuse brightness (0..1, the sum of N·L over
  the lights) times the way from it to the diffuse index. The lights
  are directional.
- **Out to the picture.**
  - In colour, each eight pixels' bytes become n plane bytes by an 8×8
    bit transpose (about 30 instructions for all eight planes). Each
    plane of the strip goes out with `ar_plane()`, one blitter pass.
  - In black and white, each index's brightness from the palette picks
    an 8×8 ordered dither, and the strip goes out with
    `ar_bitmap_opaque()`.
  - A strip's rows record which columns were drawn. Outside those
    columns, nothing is cleared or converted: the bytes there are
    simply the background's.

### Display lists

`glNewList` to `glEndList` records the calls in between, and
`glCallList` makes them again. The arguments are stored already in
zgl's own numbers: a vertex as 24.8 and a normal as 16.16. So an `f`
call's conversion, or `glNormal3i`'s square root, is done once, when
the list is made.

These calls are recorded:

- `glBegin`, `glEnd`, `glVertex*`, `glNormal*`;
- `glIndexi`, `glColor*`, `glMaterial*`;
- `glTranslate*`, `glRotate*`, `glScale*`, `glPushMatrix`,
  `glPopMatrix`, `glLoadIdentity`;
- `glShadeModel`, `glEnable`, `glDisable`;
- `glCallList`. A list called from a list is recorded as a call, as GL
  does, so changing the inner list later changes the outer one too.

Anything else is carried out at once, also while a list is being made.
The work a list saves is the calls, not the transform: each vertex is
transformed, lit and projected every time the list is called.

Lists live in the heap, 20 bytes a call, and stay across `zgl_open`
and `zgl_close`. gldemo's sphere and space3d's solids are lists.

## Tests

- `make -C sw/common/zgl test` compares the front end on a build host
  with the same pipeline in double precision. 20,000 random lines go
  through random frustums, rotations (a third of them about a single
  axis) and translations, clipping included; every end must be within
  1.5 pixels. There are also orthographic lines, culling, strips, loops,
  points and errors.
- `make -C sw/common/zgl test` also runs `test_frames`. It feeds 300
  random triangles straight to the rasterizer and compares their
  coverage with a top-left-rule reference (0 pixels differ). It also
  checks:
  - a full-screen quad (76,800 pixels);
  - depth in both orders;
  - a smooth ramp;
  - near-plane clipping;
  - 200 random lines against the whole line walked at once (each strip
    walks only its own part);
  - the depth test per primitive;
  - display lists: a lit, nested scene called from lists, in both
    modes, draws exactly what the direct calls draw;
  - black and white.
- `make -C sw/common/zgl simtest` draws two frames with `ZGL_CHUNKY`
  in software into the simulator's VRAM, one of them scissored. It
  reads them back through the chunky layout, and holds T2's
  (`ZSIM_GTRI=2`, its model) to them: 0 pixels differ.
- `make -C sw/apps/gldemo render` renders gldemo in every mode
  ([gldemo_app.md](gldemo_app.md)).
- `make -C sw/apps/space3d render` renders the game through the arcade
  runtime's host layer in every mode. Against the pictures from before
  the port, the lit pixels agree to within 1-2%; positions differ by
  rounding, about a pixel.
## Cost

`make -C sw/common/zgl bench` counts instructions in the simulator
(`tools/simprof.py` breaks them down by function, sim/README.md).

- **Lines:** 400 spinning cubes take about **700 instructions a
  line**. That includes transform, clipping and projection, and leaves
  out the back end.
  - That is roughly 1,000 lines in 25 ms on zeitlos32p, and about 4×
    slower on picorv32.
  - space3d draws about 100 lines a frame at the start of a run (host
    render): about 2 ms on zeitlos32p and 9 ms on picorv32, against its
    33 ms a game step. Busier moments are to be measured on hardware.
- **Frames:** gldemo's scene has a lit, smooth sphere of 384 triangles
  over a sixth of the picture, a flat cube and 48 lines (260
  primitives after culling, 14,000 pixels).

  | | front end | frame | total |
  |---|---|---|---|
  | empty frame | | 0.12 M | 0.12 M |
  | 256 colours, depth | 0.58 M | 0.90 M | **1.48 M** |
  | 256 colours, no depth | 0.54 M | 0.75 M | 1.30 M |
  | 16 colours, depth | 0.58 M | 0.81 M | 1.38 M |
  | black and white, depth | 0.58 M | 0.88 M | 1.45 M |

  - That is about 20 frames a second on zeitlos32p (31 MIPS), and 4 to
    5 on picorv32. The blits are the GPU's and are not counted.
  - Of the frame, rasterizing is about half, and the strips' conversion
    to planes or dither about a third.
  - Instructions are not the whole cost. On sergei_ml2 a store goes out
    to DDR3 on its own (the data cache is write-through), so the front
    end measured about 12 cycles an instruction. `ZSIM_COST=ddr3`
    models that (sim/README.md, "Cycle model"), and `framebench` prints
    the modelled cycles too: the front end above is about 2.5 M cycles.
    So the front end keeps stores few: a primitive's corners are passed
    as pointers into the vertex cache, not copied, and the hot paths are
    small functions that save few registers.
  - The instruction cache is direct mapped, so the front end's hot code
    (`ZGL_HOT`, in `.text.hot`) sits together, about 7 KB, and does not
    evict itself; the rare paths (clipping, outlines, recording) are
    apart. Modelled: about 1.9 M cycles a frame, from 3.2 M.

## Phases

| Phase | | Status |
|---|---|---|
| 0 | GL/gl.h, the front end, `ZGL_LINES`; space3d ported, looking as before | **done** |
| 1 | `ZGL_FRAMES` in software: strips with a 16-bit depth buffer each, flat and smooth shading, lighting, every target format; gldemo | **done** |
| 2 | space3d improved (the solid look: lit rocks, depth; V); display lists | **done** |
| 3 | measurements on hardware (ml1, ml2, Lakritz) | |
| 4 | the triangle engine (new RTL, 45F/85F): the same strips on chip | gpu_tri drew only the pixels, the CPU still doing the geometry: less than 2× the software. **Retired** (November 2026) for T2 |
| T2 | the engine that runs the whole frame: the CPU streams compiled display lists, the engine (a small RV32IM core, zeitlos32g, and a raster of a pixel a clock) transforms, lights, clips, sets up and draws | **done, on hardware** (`GPU_T2`, sergei_ml2): [zgl_t2.md](zgl_t2.md). Textures and positional lights checked in simulation |

The plan, and why: `claude/gl-proposal.md` in the project.
