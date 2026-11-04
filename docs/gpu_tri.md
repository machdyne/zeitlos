# The triangle engine

`rtl/gpu/gpu_tri.v` (`gpu_tri_wb`) draws zgl's `ZGL_FRAMES` strips in
hardware and writes them into chunky VRAM. It is Phase 4 of zgl
([zgl.md](zgl.md); the plan is `claude/gl-proposal.md` in the project).

**Status: in the SOC behind a define, and driven by zgl; not yet run
on hardware.**

- **Enabling it.** `` `define GPU_TRI`` in a board's block of
  `rtl/boards.vh` puts it into the SOC. It needs `GPU_RASTER` and is
  meant for the boards with chunky game mode. No board defines it yet:
  see "In the SOC" below for what a build measured.
- **Driving it.** zgl uses it when the bitstream has it, for two
  kinds of frame:
  - `ZGL_CHUNKY` frames, into chunky VRAM ([zgl.md](zgl.md),
    "Chunky"). `run gldemo chunky` shows it, and `run gldemo chunky
    soft` draws the same in software, to compare.
  - **A black-and-white window's** frames, into the 1 bpp desktop at
    the window's place ("Out to a window" below). gldemo in its window
    shows it.

## The division of work

The CPU does what software does well, and the engine does the pixels:

- **The CPU** (`zglf_t_frame()` in `sw/common/zgl/zgl_frames.c`)
  transforms, lights, clips and projects as for any frame. It cuts the
  frame into strips of up to 16 rows, and sets up each strip's
  primitives as commands:
  - a triangle becomes one or two trapezoids, its rows above and below
    the middle vertex, each with both edges already at its first row;
  - a line becomes its steps inside the strip.

  That is the divisions and 64-bit arithmetic.
- **The engine** steps the edges a row at a time, takes one multiply a
  row (a plane's value at the span's first pixel), and draws a pixel a
  clock along the span: depth test, dither, colour. When a strip is
  done, it writes it into VRAM, clearing its buffer behind it.

Its pictures are the software's, pixel for pixel. The tests hold the
engine to that in two steps:

1. `test_frames` holds the engine's C model (`tests/t_model.c`) to the
   software strips: 12 random scenes, 0 pixels differ.
2. `rtl/gpu/bench/tb_gpu_tri.v` holds the RTL to the model's VRAM.

## Commands

The commands are written to `CMD`, a 512-word FIFO.

| Command | Words |
|---|---|
| `BEGIN` | `3 << 28 \| first << 27 \| y0 << 12 \| clear << 4 \| rows − 1`; `cx0 \| cx1 << 16`. `first` marks a frame's first strip |
| `TRAP` | `1 << 28 \| depth << 27 \| ya << 4 \| yb` (rows of the strip); left edge, right edge (3 words each: `q \| qs << 16`, `r \| rs << 16`, `D`); `z0`, `dz/dx`, `dz/dy`; `c0`, `dc/dx`, `dc/dy` |
| `LINE` | `2 << 28 \| depth << 27 \| steps − 1`; `x`, `y`, `dx`, `dy`, `z`, `dz`, `c`, `dc` |
| `END` | `4 << 28` |

- **An edge** is the software's exact walker:
  - `q` is the first pixel at or right of the edge;
  - `r` is the remainder;
  - each row adds `qs` and takes `rs` from `r`, carrying `D`.

  A row's pixels run from the left edge's `q` to the right edge's
  `q − 1`, inside `cx0..cx1`.
- **Depth and colour.** `z0` and `c0` are the planes' values at column
  0 of the trapezoid's first row. They are 16.15 and 8.8 numbers,
  taken mod 2^32, and give the same low 32 bits as the software's
  64-bit sum. A span starts at `z0 + dz/dx · x`, and each row adds
  `dz/dy`.
- **A line's** pixel is `x >>> 20, y >>> 20`. The CPU adds the half
  pixel.

## Inside

- **Two strip buffers.** Each is 1,280 words of four pixels: a byte
  each of colour, and 16 bits each of depth. One buffer is drawn while
  the other goes out to VRAM. Going out clears the whole buffer to the
  strip's background index. A `BEGIN` with another background clears
  its buffer first (1,280 cycles).
- **A span** runs a pixel a clock through four stages:
  1. ask for the depth word;
  2. test it;
  3. write the lane;
  4. the RAM takes it.

  Neighbouring pixels share a word but never a lane, so nothing waits.
- **A line** takes four clocks a step, because a step can land on the
  pixel just written.
- **Out to VRAM.** In chunky ([color.md](color.md), "Chunky"), row
  `r`'s segment `s` is framebuffer row `r + 240 (s & 1) + 480 (s >> 1)`,
  so a strip goes out in the order its buffer holds it: 80 words a
  row, one VRAM write each. Bytes outside `cx0..cx1` are left alone
  (byte selects).
- **`CTRL.VSYNC` and `CTRL.WAIT_BEAM`.** With `VSYNC`, a frame's first
  strip waits for vertical blanking ("In the SOC" below). With
  `WAIT_BEAM`, a strip waits while the beam (`beam_y`, the game-mode
  line) is inside its rows.

## In the SOC

- **Its registers** are at `0x7C00_0000`, decoded as the Keccak block
  is, just below it. Without the engine, `csrs.v` answers that address,
  and the ID word reads 0. `z_gtri_present()` (`sw/common/zsoc.h`)
  reads the ID.
- **Its VRAM writes share the rasterizer's port** on the VRAM arbiter
  (master 1), through `rtl/gpu/gpu_vmux.v`. A master keeps the port
  until it drops `cyc`. Then the port stays idle for three cycles, so
  an ack still on its way from the VRAM (which acks every cycle `stb`
  is held) lands on nobody. The rasterizer goes first when both ask.
  In game mode it is quiet anyway.
- **Vertical blanking is wired** (`gpu_video`'s `in_vblank`, already
  synchronised for `socctl`'s FRAME register). With `CTRL.VSYNC`, a
  frame's first strip waits for blanking before it goes out, and zgl
  sets it.
  - A strip goes out in about 7,700 cycles: 160 µs at 48 MHz.
  - The beam takes about 1.1 ms to show a strip's 16 game-mode lines
    (two scanlines each, at 60 Hz).
  - So from blanking on, the output stays ahead of the beam as long as
    drawing does, and the frame does not tear.
  - A frame that is late starts at the next blanking.
- **`beam_y` is not wired** (it is tied to "no line"), so `WAIT_BEAM`
  never waits. The beam's line is in `gpu_video`'s clock domain and
  would need a careful crossing. `VSYNC` makes it unneeded for now.

**A sergei_ml2 build (LFE5U-45F)**, synthesized with yosys 0.33 here,
with and without the engine:

| | without | with `GPU_TRI` |
|---|---|---|
| TRELLIS_COMB | 34,577 (78%) | 37,836 (86%) |
| DP16KD | 69 of 108 | 88 of 108 |

(before `MONO`, which adds about 1,150 LUT4 and a DP16KD: about 89%)

It fits, though LUTs are tight. Its timing is not known yet. The
nextpnr here (0.6) crashes after packing on sergei_ml2 with or without
the engine, so placing and routing it is for a build with your own
nextpnr:

```
make BOARD=sergei_ml2 EXTRA_DEFINES=-DGPU_TRI
```

The engine alone, out of context, meets 52 MHz ("Size and speed").

## Out to a window

With `CTRL.MONO`, a strip goes into the 1 bpp desktop framebuffer
instead of chunky VRAM. The desktop is 640×480, 20 words a row, and
pixel x is bit `x & 31`.

- **Where.** The picture lands at `WIN`, the window's top-left on the
  screen, and only inside `CLIPX`/`CLIPY`: the window's visible
  rectangle, in picture coordinates.
- **The dither.** Each index has a brightness (`LUM`, 0..16, from the
  palette), tested against an 8×8 ordered dither, exactly as the
  software's black-and-white strips do. The two agree pixel for pixel.
- **The words.** A screen word is made 32 pixels at a time, a pixel a
  clock through a four-stage pipe: ask for the colour word, take its
  byte, look up its brightness, test it. A word the rectangle covers
  only in part is read, merged and written back.
- **Clearing.** Since the strip is not read out in buffer order, the
  buffer is cleared in a sweep of its own afterwards.

zgl uses it for an arcade program's black-and-white window when the
engine is there and exactly one rectangle of the window is visible
(`ar_window_target()`, `sw/common/games/zarcade.h`). Otherwise it uses
the software strips, as before, and two rectangles or more are left to
software. In a window, `zgl_frame()` waits for the engine to finish,
so the program's text, drawn after it, stays on top.

gldemo's scene in a window takes 126,500 cycles (bench, frame 5): 2.6
ms at 48 MHz.

## Registers

The offsets are in words.

| | | |
|---|---|---|
| 0 `CTRL` | rw | [0] `WAIT_BEAM`; [1] `VSYNC`; [2] `MONO` |
| 1 `STATUS` | r | [0] busy (anything queued, drawn or going out); [1] a bad command was seen; [25:16] FIFO words free |
| 2 `CMD` | w | a command word. The write waits while the FIFO is full |
| 3 `VBASE` | rw | chunky VRAM's byte address (`0x20000000`) |
| 4 `STRIPS` | r | strips written out since reset |
| 5 `RESET` | w | empties the FIFO and stops the engine and the output |
| 6 `LUM` | w | `index \| brightness << 8`: an index's brightness, 0..16 (`MONO`) |
| 7 `ID` | r | `0x5A545249` ("ZTRI"); 0 on a bitstream without the engine |
| 8 `WIN` | rw | `x \| y << 16`: the picture's top-left on the screen (`MONO`) |
| 9 `CLIPX` | rw | `x0 \| x1 << 16`: the visible picture columns |
| 10 `CLIPY` | rw | `y0 \| y1 << 16`: the visible picture rows |

## Size and speed

Measured with yosys 0.33 `synth_ecp5`, out of context:

| | |
|---|---|
| | chunky only | with `MONO` |
|---|---|---|
| LUT4 | 2,526 | 3,682 |
| CCU2C (carry chains) | 489 | 598 |
| flip-flops | 1,405 | 1,628 |
| DP16KD | 19 | 20 |
| MULT18X18D | 4 | 4 |

- **DP16KD:** two buffers of 3 (colour) and 6 (depth), 1 for the FIFO,
  and with `MONO` 1 for the brightness table.
- **MULT18X18D:** a row's two plane multiplies.

nextpnr-ecp5 (LFE5U-85F, CABGA381, out of context, seed 1) places it
at **55 MHz** with `MONO` (52 MHz before it). The SOC runs at 48 MHz.

- The critical path is a depth word out of block RAM, through its lane
  multiplexer, to the depth compare. That stage can take another
  register if a full build needs the margin.
- `MONO`'s brightness lookup took another pipeline stage to get there
  (41 MHz without it).

The chunky engine is near the top of the proposal's estimate
(2,000-3,000 LUT4, 16 DP16KD), and well inside the 45F's and 85F's
room: the go/no-go is a go. `MONO` costs about 1,150 LUT4 more.

In the bench, gldemo's scene is 116,500 cycles from its first command
word to its last VRAM write: 2.4 ms at 48 MHz. That includes the bench
writing 7,636 command words. Going out is most of it: 15 strips of
1,280 words, about six cycles a word against a VRAM that acks at once.
Drawing the sphere's 14,000 pixels takes a pixel a clock.

The software frame was 0.9 M instructions on top of the same front end
([zgl.md](zgl.md), "Cost"). With the engine, the CPU's share is the
front end and the commands. A busy random frame (frame 2: 120
overlapping triangles and lines, 24,468 words) takes 1.36 M cycles,
most of it overdraw.

On sergei_ml2 (picorv32), gldemo's commands measured 2.6 M cycles a
frame for 222,000 instructions; the front end 7.8 M for 631,000. The
simulator models the engine (`ZSIM_GTRI=1`, sim/README.md) so the
CPU's side can be timed with its cycle model: `framebench`'s chunky
frame makes its commands in about 0.2 M instructions and 0.5 M modelled
cycles (zeitlos32p-like timing, DDR3), after these changes:

- the per-strip scan reads each primitive's rows from a small array of
  its own, not from the primitives (70 bytes each, which missed the
  4 KB data cache every strip);
- commands are stored straight to the register, not through a call;
- a trapezoid's two edges are set up inline, in registers.

## Timing

On sergei_ml2 (picorv32 + `GPU_TRI`) the first hardware build missed
48 MHz (44.98). Its slowest path was the strip buffer's depth output
through the buffer and lane muxes, the compare and the write enables.
The depth test now has a pipeline stage of its own, and that build
closes at 51.10 MHz. Two more paths were registered afterwards: MONO's
buffer word before its brightness lookup, and the output's byte selects
and VRAM address (which came from a multiply).

## Testing

From the repo root:

```
make -C sw/common/zgl test        # the model against the software
make -C sw/common/zgl vectors     # rtl/gpu/bench/tri/: commands and VRAM
iverilog -g2005 -o /tmp/tb_tri rtl/gpu/bench/tb_gpu_tri.v rtl/gpu/gpu_tri.v
cd rtl/gpu/bench && vvp /tmp/tb_tri
```

The bench runs six frames and checks every VRAM word after each. Four go to chunky VRAM:

1. gldemo's scene;
2. random depth-tested smooth triangles and lines;
3. an orthographic, scissored, flat frame (strips off the 16-row grid,
   byte selects at both sides);
4. gldemo's scene again, after the buffers have been cleared to other
   backgrounds.

Two more frames go out with `MONO` into the 1 bpp desktop, the second
clipped to part of the window:

5. gldemo's scene, with the window at 101,57 (no word boundary);
6. the random frame, with the window at 37,200 and only part of it
   visible.

The partly covered words are read, merged and written back.

Then it checks `WAIT_BEAM`, with the beam parked in strip 5's rows,
and `VSYNC`, with the frame held until blanking comes.
The vectors (about 1 MB) are made, not kept: `rtl/gpu/bench/tri/` is
ignored by git.
