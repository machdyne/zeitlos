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

## Erase strategy

There is no double buffer -- one 1bpp surface, scanned out
continuously -- so each frame must remove the last one in place. Two
ways, and which is cheaper depends entirely on the model:

- **Incremental**: redraw last frame's edges in colour 0. One line
  command per edge, touches nothing else, no flicker. What the cube
  wants (12 edges).
- **Clear**: one blitter fill of the content area
  (`z_win_fill_rect()`, which goes through `rtl/gpu/gpu_blit.v`, not a
  per-pixel software loop). One command regardless of edge count, but
  blanks the window each frame, which reads as flicker.

The crossover is not close: a decimated teapot is ~900 edges, so
incremental erase costs 900 extra commands against **one** fill.
`INCR_MAX_EDGES` (96) picks between them. The `prev_x/prev_y` arrays
are sized for `PREV_MAX_VERTS` (128) rather than `MODEL_MAX_VERTS`,
since the incremental path is unreachable above that -- sizing them
for the maximum would be 3KB of `.bss` that can never be used.

## Loading STL files

See `stl.c`'s own header comment for the full design. The summary:

**Nothing is ever held whole.** teapot.stl is ~400KB; main memory is
1MB total and an app's malloc heap is 16KB
(`Z_PROC_STACK_SIZE_DEFAULT` -- that tier is stack *and* heap,
together, for the process's whole life). `fs_mallocfile()`, which
every other file-reading app here uses, would need one 400KB
allocation. The file is instead pulled through a 512-byte window with
`fs_read_chunk()`, and what is kept is a decimated model bounded by
`MODEL_MAX_VERTS`/`MODEL_MAX_EDGES`. The ceiling on what can be opened
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

**The grid resolution is estimated, not laddered.** Occupied cells
scale with the square of grid resolution, and an overflow tells you
the pool filled after a known fraction `f` of the model -- so the grid
that just fits is about `grid*sqrt(f)`. A fixed ladder cannot do this:
stepping 24 -> 17 on the 8257-triangle binary teapot overshot to
249/768 vertices, spending a third of the budget and discarding two
thirds of the available detail. The estimate lands near budget on the
first retry. Failed attempts are cheap -- the pass aborts the moment
the pool fills, so it reads a fraction of the file.

**Format detection is arithmetic, not the `solid` prefix.** A binary
STL's size is exactly `84 + 50*count`. Several common exporters write
binary files whose 80-byte header begins with the ASCII text
`solid ...`, and such a file fed to an ASCII parser yields nothing at
all. The size test gets these right; the prefix test does not.

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

## Memory

`.bss` is roughly 30KB: ~15KB of model arrays, ~7KB of loader hash
tables, ~6KB of projection arrays, plus the stream buffer. Every byte
of that is a byte of the 1MB pool for the app's lifetime, which is why
the Makefile now builds with `--gc-sections` (this app links
`zdialog.o`/`zflist.o` for one dialog, and those pull in the whole
widget toolkit) and emits a `.zexe` rather than a padded raw binary --
the header carries the `.bss` size as a number instead of writing
30KB of literal zeros to be read off the SD card at every launch.

`MODEL_MAX_VERTS`/`MODEL_MAX_EDGES` are jointly sized against memory
*and* time: at ~12 MIPS a 1500-edge model is already in the low tens
of frames per second, so raising them buys detail the frame counter
immediately spends. If you raise them, watch the FPS readout -- that
is exactly what it is there to tell you.

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
