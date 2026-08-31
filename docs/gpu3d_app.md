# Zeitlos gpu3d

Wireframe 3D viewer. Spins a cube by default and opens STL files from
the SD card.

Its real job is to be a load generator for the GPU line rasterizer
(`rtl/gpu/gpu_raster.v`): hundreds of clipped line commands per frame,
from a process that is simultaneously servicing wm messages and
reading a file. **The FPS readout in the window's top-left corner is
the point of the app** -- it is the number that says whether a change
to the rasterizer, the blitter, the cache or the CPU made anything
faster.

## Controls

| Input | Action |
|---|---|
| click on the object | stop spinning, begin rotating |
| drag | rotate (horizontal = Y axis, vertical = X axis) |
| Ctrl + drag | rotate without needing to hit the object |
| Shift + move | scale (away from you = bigger); no button needed |
| Space | toggle spin |
| `r` | reset rotation and scale |
| `e` | switch erase mode (interleaved / clear) |
| `o`, or the titlebar open icon | open an STL file |
| `+` / `-` | scale in steps |
| Alt | suppresses all of the above |

Alt is deliberately inert: `Alt`+arrows is wm's own window-move
gesture (`wm.c`'s `alt_move_focused()`), and an app that also acted on
Alt would be fighting the window manager for one gesture.

Clicking the object both stops the spin and starts a rotation, because
that is one intention rather than two -- clicking a spinning object is
how a user says "hold still, I want to look at this".

Ctrl exists because the bounding-box hit test is not always a
convenient target: a heavily decimated model is mostly empty space,
and a scaled-down one is small. Ctrl means "I mean this window, not
that pixel".

## Reading modifier keys

`Z_WM_MOUSE` carries x, y, buttons and an inside flag. It does **not**
carry modifiers, and it cannot be made to without changing the packed
layout that exists precisely to avoid allocating a `Z_MAP` at
pointer-movement rates.

Nor can modifier state be tracked from `Z_WM_KEY`: wm drops bare
modifier changes outright (`if (keysym == Z_KEY_NONE) continue;` in
`dispatch_keys()`), because a modifier press has no keysym of its own.
An app watching only `Z_WM_KEY` learns Shift is held on the next
Shift+letter and never otherwise -- no use at all for "hold Shift and
move the mouse".

So `kbd_mods()` reads the live modifier byte from the USB HID report
register, which is level state maintained in hardware
(`rtl/usb_hid.v`) and always current. It picks the port by asking
which one reports itself a keyboard, exactly as `wm.c`'s own
`mouse_port()` picks the mouse -- there is no fixed port-to-device
mapping, so assuming port 0 is wrong about half the time.

This is read-only level state that no other process can be confused
by, unlike the rasterizer registers -- whose shared *mutable* state is
why this app no longer touches them and goes through
`z_win_hw_line()` instead.

## Spin is timed, not counted

The original added a fixed number of degrees per **frame**. That
couples rotation speed to frame rate: the cube spun far too fast
(hundreds of frames per second at 5 degrees each is many revolutions
a second), and any heavier model would have silently slowed the
rotation as the frame rate dropped.

The fix is not simply "add `rate * dt / TICK_HZ` each frame" either,
and this is worth knowing because it looks correct and is not. A frame
takes 1-2 ticks (`Z_TICK_HZ` is 732), so that division truncates to
zero most frames -- at 5 degrees/second the Z axis would **never
move**, and the other two would run at whatever rate the truncation
left. The error gets worse the faster the app runs.

So the angle is computed from **total accumulated spinning time**
(`spin_ticks`), with manual rotation kept as a separate offset. One
division against a large number instead of one per frame against a
tiny one: no truncation, no drift. Verified exact at 1, 5 and 10
seconds against 11/17/5 degrees per second.

## Erase strategy, and why there is no double buffer

There is no double buffer and there **cannot** be one without a
gateware change. VRAM is 9600 words (`rtl/mem/vram.v`), which is
exactly `640*480/32` — one frame, nothing left over. A back buffer
needs another 9600 words of BRAM, plus a base-address register in
`gpu_video.v`, plus retargeting the rasterizer and blitter, plus a
flip at vblank. That is the right long-term answer and it is a real
project.

Without it, each frame must remove the last one in place. Two ways:

- **`ERASE_CLEAR`**: one blitter fill of the content area, then
  redraw. Cheapest possible in commands — one fill regardless of edge
  count.
- **`ERASE_INTERLEAVED`** (default): for each edge, redraw the
  previous frame's version in colour 0 and immediately draw the new
  one. Costs one extra line command per edge.

`ERASE_CLEAR` was the default for anything over ~96 edges and it
**flickers badly**. The phase breakdown shows why: at 290 edges the
draw phase is ~31ms of a ~41ms frame, so the window is blank or
half-drawn for roughly three quarters of every frame, at ~24Hz —
squarely in the band the eye is most sensitive to. Raw frame rate was
never the problem; *duty cycle* was.

Interleaved erase never blanks anything. At any instant exactly one
edge is missing, for the ~100us it takes to issue two line commands.
It roughly halves the frame rate, which the eye wins easily against a
75% blank duty cycle.

**Known artifact.** Where two edges cross, erasing this frame's old
edge punches a one-pixel hole in a new edge already drawn this frame —
a 1bpp framebuffer cannot know a pixel is owed to two lines. The hole
is repaired next frame, so crossings shimmer slightly. That is the
price of not double-buffering, and a much smaller one than the whole
object flashing.

Press **`e`** to switch modes on the running system. The choice is a
judgement about how something *looks*, which the numbers cannot
settle.

### The FPS readout

Drawn last, it was on screen only from the end of one frame until the
clear early in the next — about 10ms of a 41ms frame, so it flashed at
~24Hz despite costing under a millisecond to draw. It is now drawn
immediately **after** the erase, so it is present for essentially the
whole frame. The cost is that edges drawn afterwards can cross over
it; in the top-left corner of a centred projection that is rare, and a
stable readout with an occasional line through it beats a correct one
that strobes.

## Loading STL files

See `stl.c`'s own header comment for the full design. The summary:

**Nothing is ever held whole.** teapot.stl is ~400KB; main memory is
1MB total and an app's malloc heap is 16KB
(`Z_PROC_STACK_SIZE_DEFAULT` -- that tier is stack *and* heap,
together, for the process's whole life). `fs_mallocfile()`, which
every other file-reading app here uses, would need one 400KB
allocation. The file is instead pulled through a 4KB window with
`fs_read_chunk()`, and what is kept is a decimated model bounded by
`MODEL_MAX_VERTS`/`MODEL_MAX_EDGES`.

`STL_CHUNK` was one 512-byte SD sector and is now 4KB, after a 400KB
teapot took ~60 seconds to load. Every chunk costs a syscall plus a
pump callback; at 512 bytes over ~1MB of reads that was ~2400 of each,
against ~300 at 4KB. The per-byte SPI cost is unchanged — what goes
away is the fixed cost paid per chunk. The ceiling on this is the pump
interval, not memory: 4KB is roughly 4ms of SD time, two orders of
magnitude clear of wm's `REDRAW_ACK_TIMEOUT`, so there is headroom to
go further if the load report says I/O still dominates. The ceiling on what can be opened
is the SD card, not RAM.

**Decimation is vertex clustering, not "every Nth triangle."** The
stride approach is the first thing anyone reaches for and it does not
work: adjacent STL triangles share vertices exactly, so keeping every
6th one keeps a scattering that shares almost nothing. The vertex
count does not drop to a sixth, and the result looks like confetti in
the shape of a teapot. Clustering (Rossignac-Borrel) snaps vertices to
a grid, collapses each cell to one vertex, drops degenerate triangles
and dedupes edges -- yielding a genuine low-resolution wireframe of
the *whole* object. The edge dedup alone is a 2x frame-rate win, since
a closed mesh otherwise draws every shared edge twice.

**The grid resolution is estimated, not laddered**, and the
extrapolation is deliberately *not* linear. Occupied cells scale with
the square of grid resolution, and an overflow tells you the pool
filled after a known fraction `f` of the file. The tempting
extrapolation — "it filled after a fraction `f`, so it wanted `1/f`
times the budget" — is badly wrong, because occupied cells
**saturate**: early triangles each land in a fresh cell, later ones
mostly hit cells already taken. Linear extrapolation over-predicts and
the correction overshoots.

The two honest bounds are fully saturated (final = what we already
have) and fully linear (final = budget/`f`). Their geometric mean,
`budget/sqrt(f)`, is what's used, which — since count goes as grid
squared — means scaling the grid by the **fourth** root of `f`. Two
integer square roots, no floating point.

This is measurable and was measured. On the 384-edge budget, the
linear model produced 104 edges from the binary teapot (27% of budget,
three quarters of the affordable detail discarded); the fourth-root
model produces 267, and 382/384 on the ASCII one. Failed attempts stay
cheap — the pass aborts the moment the pool fills — so total I/O is
about 2.5x the file across all passes.

**Format detection is arithmetic, not the `solid` prefix.** A binary
STL's size is exactly `84 + 50*count`. Several common exporters write
binary files whose 80-byte header begins with the ASCII text
`solid ...`, and such a file fed to an ASCII parser yields nothing at
all. The size test gets these right; the prefix test does not.

**No 64-bit division in the hot loops.** Two separate instances of
the same mistake, both worth several seconds or milliseconds:
`cell_key()` divided an `int64_t` by the cell size nine times per
triangle (three axes, three vertices) on every build pass — ~85,000
`__divdi3` calls per pass on a 9438-triangle teapot. It is now a
32-bit divide, safe by construction: both terms are `fixed_t` and
their difference is bounded by the axis extent, which is itself a
`fixed_t`. Verified identical over 200k sampled coordinate/grid
combinations.

**One hardware divide per vertex, not two software ones.**
`project()` used to call `fixed_div()` twice per vertex, and
`fixed_div` computes `((int64_t)a << 12) / b` — rv32im has no 64-bit
divide, so each was a call into libgcc's `__divdi3`, several hundred
cycles. It now computes a Q18 reciprocal `2^30 / zo` instead: a
constant numerator that fits a uint32, so it is a single hardware
`DIV` (~32 cycles on picorv32's sequential divider), once per vertex.
The multiply stays 64-bit and that is fine — rv32im *has* a widening
multiply (`mul`/`mulh`), so a 64-bit product is two instructions. Only
division falls off the hardware. Verified against the old path over
396k sampled points: worst case 1px, mean 0.002px.

`fixed_div()` is retained for `model_normalize()`, where the int64
numerator is genuinely needed (raw STL coordinates can be in the
thousands) and which runs once per load rather than once per vertex
per frame.

**No floats anywhere.** This target is rv32im -- no F extension.
Touching a float pulls newlib's soft-float support and, for the ASCII
path, `strtod` and most of stdio's conversion machinery, into an image
where every byte of `.rodata` is a byte of the 1MB pool for the
process's lifetime. `f32_to_fixed()` takes the IEEE bit pattern apart
by hand and `parse_fixed()` parses ASCII decimals directly into Q12;
both are a few dozen integer instructions and add nothing to the link.

### Cost, and the pump callback

Clustering needs the bounding box before it can quantize, and the box
is only known after reading every vertex -- so this makes **more than
one pass** over the file (two normally, occasionally three). On 400KB
that is seconds, not milliseconds.

That makes `stl_load()`'s `pump` callback mandatory, not politeness.
wm blocks waiting for a redraw ack, and an app that stops acking
freezes the **whole screen** until `REDRAW_ACK_TIMEOUT` fires (see
`docs/window_manager.md`, "content z-order"). A multi-second load is
far more than long enough to hit that. `load_pump()` drains the queue
every 512-byte chunk.

`paint_full()` must not render the model while `loading` is set: the
pump runs mid-build, so `nverts`/`nedges` and the arrays they index
are inconsistent at that moment. This is also why the loader's hash
tables are *not* unioned with the projection arrays to save 7KB --
that saving turns into a corrupted model the first time a frame is
drawn during a load, which the pump makes possible.

## Titlebar

`Z_WIN_FLAG_OPEN_ICON` needed no wm changes -- the flag,
`Z_WM_TITLEBAR_ICON` and `Z_WM_TBICON_OPEN` already existed, and
`wm.c` already draws and hit-tests the icon. gpu3d sets the flag and
handles the message, as `sw/apps/text` does.

The close icon is set **without** `Z_WIN_FLAG_CLOSE_KILLS_OWNER`. The
old version could safely use the killing form because it had no
dialogs; this one owns a second window whenever the file dialog is
open, and the killing form takes every window of a pid down the
instant any one of them is clicked closed.

The title shows the model name, prefixed with `~` when the model was
decimated to fit -- without that, a teapot visibly coarser than its
source file looks like a parsing bug rather than a stated tradeoff.

gpu3d also honours `Z_WM_SET_ARG`, so the file browser can launch it
with an STL directly.

## Performance instrumentation

Once a second, gpu3d prints a frame breakdown to the serial console:

```
gpu3d: 16.42 fps  frame 60.9ms  2924 kcyc  CPI 5.8
gpu3d:   xform 4.1ms 6%  erase 5.0ms 8%  draw 41.2ms 67%  text 1.1ms 1%  other 9.5ms 15%
gpu3d:   382 edges/frame  107.8us/edge  135 verts
```

and after every load:

```
gpu3d: loaded /teapot.stl
gpu3d:   ascii, 1589 tris, grid 9 -> 135 verts 382 edges
gpu3d:   4 passes, 974 KB read, 8.1s total (io 6.2s 76%, cpu 1.9s)
gpu3d:   read rate 157 KB/s
```

These answer different questions and are worth reading differently.

**The frame breakdown** splits time by phase because the phases have
unrelated fixes: `xform` is arithmetic, `draw` is uncached MMIO,
`erase` is one blitter command, `text` is glyphs. `other` is the
message loop, the spin update, loop overhead — *and time this process
did not get*. A large `other` is the signature of preemption, not of
slow drawing.

`us/edge` is the number to watch when changing `MODEL_MAX_EDGES` or
the line-issue path, since frame time is very nearly linear in it.

**The load report** splits I/O from everything else. If `io`
dominates, the fix is in the SD path or `STL_CHUNK`; if the remainder
dominates, it is the parser; if `passes` is the outlier, it is the
clustering strategy reading the file again. Three unrelated pieces of
work, and this says which one to do.

### The caveat that matters

picorv32's `rdcycle`/`rdinstret` are single **global** hardware
counters — not virtualised per process, not saved across context
switches (`sw/os/sh.c` has the full note). Every figure above
therefore includes cycles and instructions burned by other processes
while gpu3d was preempted.

For "how long did the user wait", that is the honest number. For "how
expensive is my edge loop", it is inflated by however many other
runnable processes there are — so **measure with a quiet system**,
ideally just wm and gpu3d.

CPI survives this. Both columns are scaled by the same inflation, so
the ratio divides it out and is comparable between runs. A CPI far
above picorv32's ~4–6 means memory stalls or preemption rather than
more work.

Build with `-DGPU3D_PERF=0` to compile all of it out.

## Memory

`.bss` is roughly 15KB: ~4.5KB of model arrays, ~3KB of loader hash
tables and keys, ~1KB of projection arrays, and a 4KB stream buffer. Every byte
of that is a byte of the 1MB pool for the app's lifetime, which is why
the Makefile now builds with `--gc-sections` (this app links
`zdialog.o`/`zflist.o` for one dialog, and those pull in the whole
widget toolkit) and emits a `.zexe` rather than a padded raw binary --
the header carries the `.bss` size as a number instead of writing
30KB of literal zeros to be read off the SD card at every launch.

`MODEL_MAX_VERTS`/`MODEL_MAX_EDGES` (256/384) are sized for
**interactive rate, not fidelity**. They were 768/1536 and measured
4 FPS on hardware. Profiling put almost none of that in the
rasterizer and almost all of it in CPU-side per-edge cost — twelve
uncached MMIO writes per line, times 1428 edges, every frame. That
cost is linear in edge count and essentially nothing else, so the
edge budget is the frame-rate dial.

The vertex budget *follows* the edge budget rather than leading it: a
cluster-decimated closed surface comes out around 0.34 vertices per
edge (measured 490/1428 and 307/911 on two real teapots), so 256
against 384 leaves headroom without wasting `.bss`.

If you raise them, watch the FPS readout and the `us/edge` figure in
the console report.

## Known behaviour

A **truncated binary STL** is rejected ("no triangles found") rather
than partially loaded: its size no longer matches `84 + 50*count`, so
detection falls through to ASCII and finds no `vertex` tokens.
Recovering the readable prefix would be friendlier, but guessing at
damaged files is not obviously the right default. Reported honestly
rather than silently half-loaded.

## Testing the loader

`sw/apps/gpu3d/test/` builds the STL loader against host stdio so the
parser and decimator can be exercised on a workstation, with an
ASCII-art wireframe render to eyeball the result:

```
$ cd sw/apps/gpu3d/test
$ make
$ python3 gen_teapot.py 0.58 teapot.stl teapot_bin.stl
$ ./stl_test teapot.stl teapot_bin.stl
```

It asserts normalization (half-extent exactly 1.0, centred on the
origin) and edge-list integrity (in range, non-degenerate,
deduplicated, index-ordered).


## Faces and flat shading (in progress)

`model_t` now carries an optional face list (`tris` / `ntris`) alongside
its edges.

**Optional is the important word.** `ntris == 0` means "wireframe only",
and the renderer falls back to it rather than refusing to draw. Nothing
is synthesised or converted: a model without faces simply cannot be
shaded, which is the honest answer and keeps every existing model
working untouched.

### The cube has faces; STL imports do not

Deliberately, and in that order.

The cube is 12 triangles and its winding can be checked by hand — all
twelve were verified to have outward normals before anything was drawn
with them. That proves the shading pipeline against something whose
correct output is obvious.

STL import is a bigger problem than it looks. The loader already *reads*
triangles — that is what an STL is — and then converts them to a
deduplicated edge list, discarding the faces. Keeping them is not just
an array: the triangle count after cluster decimation is far above
`MODEL_MAX_TRIS`, so it needs its own budget and its own decimation
pass. `stl.c` sets `ntris = 0` explicitly so an imported model degrades
to wireframe rather than drawing garbage.

### Winding

Counter-clockwise seen from outside. Backface culling tests the sign of
the projected 2D cross product, so this convention has to hold for every
model that wants shading.

A face wound the wrong way **does not look like a winding error**. It is
culled when it should be drawn, so the solid gets a hole and you see the
inside of the far side through it. Worth checking each face against the
vertex table rather than trusting a pattern.

### Shading, as implemented

`S` toggles it. Software edge-walking, hardware span fills.

- **Backface cull** on the projected 2D cross product. Projection
  preserves winding, so the sign is the same as it would be in view
  space, and only the sign matters.
- **Light level from the face's true 3D normal** against a fixed light
  direction in view space, computed from the rotated vertices. See
  below for the version that did not work.
- **Painter's algorithm**, back to front, insertion sorted. Exact for a
  convex solid; wrong for interpenetrating geometry, which is the
  classic failure and worth knowing before pointing this at a
  complicated mesh. There is no Z-buffer and nowhere to put one.
- **Always clears.** `ERASE_INTERLEAVED` erases by redrawing last
  frame's *edges* in colour 0, which has no meaning for filled faces.
  Painting new faces over old almost works and fails exactly where it
  matters: a face that shrinks as the model turns leaves a fringe of
  the previous frame behind it.

### Lighting from projected area looked like a moving light

The first version had no normal at all. The projected cross product's
magnitude is twice the triangle's screen area, which for a face of fixed
3D size falls to zero as it turns edge-on — genuinely the cosine term a
diffuse light wants, free, since the value is already computed for
culling.

It was scaled against a running maximum over the visible faces, and that
is where it fell apart. A single face brightened and dimmed correctly,
but the **scale moved with the model**, because which face is largest
changes as it turns. Every face's level then shifted together, which
reads exactly as a light source orbiting the object rather than as
shading.

The fix is a real normal against a **fixed** direction: no shared term,
so a face's brightness depends only on its own orientation. Computed in
view space from the rotated vertices, not the projected ones —
perspective divide distorts angles, so projected coordinates are good
enough to decide *facing* (a sign) but not *angle* (a magnitude).

Checked numerically over a rotation: coplanar triangle pairs always
agree, and faces move independently — one falls 15 to 3 while another
rises 3 to 14, with no common drift.

There is an ambient floor of 3. A face turned fully from the light still
has a silhouette, and painting it black makes the solid look like it has
a bite taken out of it against a black background.

### The cube flashed, and clearing was the cause

Clearing the window and redrawing leaves the object **absent** for the
part of each frame between the two. At 15fps that is a large fraction of
the frame, and it flashes badly.

`ERASE_INTERLEAVED` already solves this for wireframes by erasing and
redrawing one edge at a time, so the object is never fully gone. The
equivalent for solid faces is to **erase only what is no longer
covered**.

Each frame records, per scanline, the leftmost and rightmost pixel the
faces reached. The next frame draws the object **first**, then clears
only the slivers of the previous silhouette the new one does not cover.

The object is therefore never blanked — nothing flashes — and the erase
is proportional to how far the silhouette moved rather than to the
window area, so it is also faster than the clear it replaces.

**Exact for a convex silhouette**, which a cube has: per scanline the
coverage is a single interval, so a min and a max describe it
completely. A non-convex model can have two intervals on one scanline
and the gap between them would not be erased — a real limitation, and
the reason to revisit this when STL faces arrive.

Verified by simulating a shape moving across a buffer for six frames and
checking for both leftover pixels and holes: zero of each.

### Clearing only the bounding box left trails

Also fixed by reverting. Last frame's faces extended to *last* frame's
bbox, and as the model turns the two differ — so a crescent of the
previous frame survives outside the new box, and the fresh black
rectangle reads as a box cutting into the shape.

Clearing the union of the two bboxes would fix it properly and is worth
revisiting, but only once the shading itself is settled: a partial clear
makes every other rendering bug look like a clearing bug.

### The cull sign was wrong, and reasoning did not settle it

Screen y runs down, which flips the handedness, and it is genuinely
easy to argue yourself into either answer — I did, and wrote a
confident comment for the wrong one.

It was settled by computing each face's true 3D normal independently
and checking which sign agreed. `cross > 0` matches on all twelve cube
faces, from a head-on view (2 triangles visible, since a cube seen down
an axis shows one face) and from a corner view (6 visible, 6 culled).

Worth recording because getting it backwards **does not look like an
inverted test**. You see the inside of the far side of the solid, which
reads as a hole.

The triangle filler was checked the same way: rendered to an ASCII
buffer and scanned for interior gaps, including the degenerate
single-scanline case that an edge-on face produces.

### Making it fast enough

First hardware run was 12-15fps. Two changes, both about the CPU rather
than the blitter:

**Spans write three registers, not six.** `z_fb_hw_span_begin(level)`
hoists height and grey level out of the loop, so each scanline writes
only `dst_x`, `dst_y` and `width`. At several hundred spans a frame,
each register write is a stalled bus cycle from the CPU, and the
register traffic costs as much as the blitting.

Clipping moved out of `zwin` and into the rasterizer for the same
reason: `z_fb_hw_span()` deliberately does **not** clip or validate, so
that the inner loop of something which has already clipped pays nothing
for checks it does not need.

**The scan range is clipped, not just each span.** A face partly outside
the window used to run its whole interpolation for rows that were then
discarded — which for a model scaled past the window edge is most of
them.

**The clear is the bounding box, not the window.** The model is
normalised to half-extent 1 and the projection is centred, so at default
scale it occupies well under half the content area.

### If the faces are solid white and black with no pattern

The gateware has no dither support — `z_fb_hw_dither_available()`
returned false and `z_fb_hw_fill_shade()` fell back to a plain fill,
white above level 8 and black below. As the model turns and levels cross
that threshold, faces flip between the two.

That is the fallback working as designed on old gateware, not a bug.
`make flash`, not `make dev-flash` — the dither is CTRL bit 8 in
`gpu_blit.v`.

### Why no triangle rasterizer

A flat-shaded face is a set of horizontal spans, and a shaded span is
exactly one `z_win_hw_fill_shade()` call. So software edge-walking plus
hardware span fills gets the whole feature with no new gates.

Whether that is fast enough decides whether a hardware rasterizer is
ever worth building — and it can now be measured rather than guessed,
which was the point of doing the pattern hardware first.
