# T2: the triangle engine that runs the whole frame

Status: **in the SOC behind `GPU_T2`, running on hardware:** sergei_ml2
(gldemo chunky at the display's 60 frames a second, chunky VRAM) and
sergei_ml2_console (the picture in main memory; with textures since
2026-11-05: gldemo 59-60 a second, mausrun3d textured 18-20, space3d 15;
"On hardware" below). Checked in simulation, every word of the picture
against the model, positional lights too (on hardware: gldemo's lamp,
60 to 42 frames a second). The plan and the decisions are in the project's
`claude/zgl-t2-proposal.md`.

The first triangle engine, gpu_tri (retired), drew only the pixels;
the CPU still did everything before them, and on sergei_ml2 that was
most of a frame (gldemo: 7.8 M of 9.9 M cycles). T2 takes the rest:
transform, lighting, culling, clipping, triangle setup and binning.
The CPU describes the frame.

```
 CPU (zgl)                    T2 (rtl/gpu/gpu_t2.v)
 ---------                    ---------------------
 compiled lists,   CMD    +--------+  IN   +-------------+ records +--------+ strips +--------+
 state, set-up   ------->| FIFO   |------>| zeitlos32g  |-------->| raster |------->| output |--> chunky
 triangles/lines          | 2048 w |       | (sw/t2fw)   |  list   | 1 px/  | 1 buf  | VSYNC  |    VRAM
                          +--------+       +-------------+         | clock  |        +--------+
                                            geometry: transform,    +--------+
                                            light, cull, clip, setup  edge functions, depth
```

## How zgl uses it

On a bitstream whose engine reads "ZTR2" (`ZGL_CHUNKY` frames,
`zgl_info()->backend == ZGL_BACKEND_T2`), zgl streams the frame into the
engine's command FIFO as the program makes its calls
(`sw/common/zgl/zgl_t2.c`; the stream's format: `zgl_t2.h`):

- **Display lists are the fast path.** A list is compiled the first
  time it is called: indexed triangles and lines, in chunks of up to
  64 vertices (the engine's vertex buffer), positions in 16 bits with a
  shift, normals in 2.14, a colour index a vertex, the material. Each
  call then sends the state the engine needs (the viewport, the matrix,
  the light in the model's space, depth, shading and culling; each only
  when it changed) and copies the chunks' words, with no work a vertex.
- **A list that can't be compiled** is played back as before: one with
  matrices, state changes, points or nested calls, or one in which a
  vertex comes before the list's first normal or index. So is any list
  drawn with `glPolygonMode` other than `GL_FILL`.
- **`GL_NORMALIZE` with lighting**: the list is compiled with its
  normals made unit vectors, and the light goes to the model's space
  as `M^T L` made a unit vector. That is exactly the software's
  `unit(M n) . L` when the modelview is a rotation or a mirror times
  one scale (space3d's rocks), which `zgl_t2_call()` checks (the
  columns as long as each other and at right angles, to a 64th); with
  any other matrix the list is played back. A list compiled one way
  and called the other is compiled again.
- **Immediate mode** (`glBegin`/`glVertex` outside lists) goes through
  zgl's front end, and each triangle or line it makes is sent set up
  (`TRI`, `LINE`). It works, at about the cost of today's front end.
- `zgl_frame()` ends the frame (`END`); `glFinish()` waits until the
  last strip is in VRAM.

gldemo keeps its sphere, cube and ring in display lists, so on T2 the
CPU's whole share of a frame is the stream.

## Inside

**The geometry unit** is **zeitlos32g** (`rtl/gpu/zeitlos32g.v`), a
small RV32IM core that exists only inside T2, running `sw/t2fw` from
its own block RAMs. It reads the stream word by word (`IN`, which waits
for the FIFO), transforms and lights each vertex of a chunk once into
its vertex buffer, then culls, clips (near and far, Sutherland-Hodgman,
as zgl does) and sets up each triangle and line into a record in the
**record list** (zgl_t2.h). A triangle outside only the picture's sides
is not clipped: it goes to the raster as it is (a **guard band**), if
its corners fit the record's 16-bit 12.4 fields (±2,048 pixels) --
the raster draws only its bounding box clamped to the frame, and its
edge functions don't mind where the corners are. A big merged face
partly off the picture (mausrun3d's world) costs a record, not a clip;
the pixels differ from zgl's clipped polygon by an index's rounding
along its planes. A chunk's vertices that share their normal are lit
once. At `END` it starts the
raster (`GO`) and goes on with the next frame's stream; it waits only
before overwriting the list.

- The program is zgl's own arithmetic (`sw/common/zgl/t2fw.c`), so T2
  draws what the software draws. The same file is the host model.
- zeitlos32g: three stages (fetch, decode, execute) with forwarding; a
  taken branch or a jump costs a bubble, a load 3 cycles, a multiply 3,
  a divide 19 (two bits a clock). No CSRs, traps, caches or bus: its I/O
  is the engine's registers (`sw/t2fw/io.c`).
- Its code (`sw/t2fw/t2fw.hex`, about 10 KB, read at synthesis and
  committed) and data (4 KB) are block RAM.

**The raster** draws the frame strip by strip (12 rows: a 960-word strip
buffer fits block RAM without the output multiplexers 16 rows needed),
from every record that touches the strip:

- A triangle: an 18x18 multiplier puts each of its three edges (and its
  depth and colour planes) at the strip's first row; then a row at a
  time, a pixel a clock across its bounding box: the edge functions
  decide coverage (zgl's top-left rule), and the depth test and write
  go through a three-stage pipe.
- A line: every step, those inside the strip drawn (as today's engine).
- One strip buffer (`TWO_BUFS=0`; two with `TWO_BUFS=1`, which lets the
  raster draw while a strip goes out, for 9 DP16KD and about 400 LUT4
  more). The output side is today's engine's (chunky VRAM, byte
  selects, VSYNC, clear behind).
- Four pixels a clock was the first design; it cost too much of the
  45F for no gain, since the geometry unit sets a frame's pace.

## Textures

Built with `T2_TEX_LOG2` (13 in sergei_ml2_console: 8 KB), T2 draws
textured triangles ([zgl.md](zgl.md), "Textures", for the GL side):

- **The stream.** A chunk with `T2_C_TEX` carries a texture word (where
  the texture is, its size, mode and key) and `s | t` a vertex (texels,
  12.4); STATE bit 5 turns texturing on for the frame. Off, or on a T2
  without textures, such a chunk is drawn in its colours.
- **The geometry unit** (`sw/t2fw/t2fw_tex.hex`, 16 KB of code and 8 KB
  of data) clips s and t with the rest in clip space and writes a TTRI
  record: TRI's words and three more planes, s/w, t/w and 1/w, which
  are linear on the screen (`zgl_t2.h`, "TTRI").
- **The raster** divides per pixel, a pixel a clock still: Q's leading
  bit and a 1,024-entry reciprocal table (`rtl/gpu/t2_recip.hex`, from
  `gen_t2_recip.py`), A and B brought to 18 bits, two multipliers,
  shifts, the texel's place, the texture memory, the mode -- nine
  stages before the depth test (the mode has one of its own: the
  memory's output, the byte and the combine in one clock made 44.9 MHz
  on sergei_ml2_console; `t2_uv()`, `t2_texel_at()`,
  `t2_combine()` in `zgl_t2.h` are the same arithmetic, which the model
  uses). Untextured pixels do not pass through them.
- **The texture memory** is written by the CPU, a word of four texels
  at a time (TEXADDR, TEXDATA), while T2 is idle.

Cost: about 1,300 LUT4, 150 CCU2C, 1,700 FF, 9 DP16KD (texture memory
4, reciprocals 1, the core's bigger memories 4) and 3 MULT18X18D. A
textured triangle costs the geometry unit three more planes: test_t2's
textured frame about 1.0 M cycles in the RTL bench against 660 k
untextured; mausrun3d's frames 619 k a frame of geometry against 514 k
(the ISS, the textured firmware).

## Registers

At 0x7C00_0000 (the first engine's address; zsoc.h's Z_GTRI_*):
CTRL (WAIT_BEAM, VSYNC), STATUS (busy: anything in the FIFO, the core
working, a strip being drawn or going out), CMD, VBASE, STRIPS, RESET,
ID = 0x5A545232 ("ZTR2"). And for textures: CAPS (11, read: bit
0 textures, bits 12:8 the texture memory's size as log2 texels; 0 on a
T2 without), TEXADDR (12) and TEXDATA (13, four texels; TEXADDR moves
on).

The geometry unit's own registers (`sw/t2fw/io.c`): IN, STATUS (the
raster has the list), RX, RY, CLEAR, GO, and the record list.

## Building

```
make BOARD=sergei_ml2 EXTRA_DEFINES="-DGPU_T2 -DNO_KECCAK"                    # picorv32
make BOARD=sergei_ml2 EXTRA_DEFINES="-DGPU_T2 -DNO_KECCAK -DCPU_ZEITLOS32P"
```

`GPU_T2` puts T2 into the SOC (it shares the rasterizer's VRAM port,
rtl/gpu/gpu_vmux.v). After changing `t2fw.c`, `make -C sw/t2fw`
rebuilds `t2fw.hex`.

## Size and speed

Out of context (yosys `-abc9`, nextpnr-ecp5 on the 45F, 65 MHz asked):

| | LUT4 | CCU2C | TRELLIS_COMB | FF | DP16KD | MULT18X18D | Fmax |
|---|---|---|---|---|---|---|---|
| zeitlos32g alone | 2,142 | 228 | 2,848 | 671 | 8 | 4 | 73 MHz |
| T2 whole | 3,764 | 842 | 6,014 | 2,867 | 30 | 5 | 72 MHz |
| today's engine (for comparison) | 3,682 | 598 | | 1,628 | 20 | 4 | 55 MHz |

Block RAM: the strip buffer 6, record list 11 (512 records), code 6,
data 2, the FIFO 4 (2,048 words: a list frame's stream goes in while the
geometry unit works, so the CPU doesn't wait on it), the rest small.
On sergei_ml2 the first T2 build (with
four pixel lanes and two strip buffers) came to 96% COMB and all 108
DP16KD; the numbers above are after the cuts.

**On sergei_ml2 it is tight.** With T2 in place of today's engine the
whole SOC came to about 93% COMB (41,073 of 43,848) and 102 of 108
DP16KD, and nextpnr had not finished routing after 90 minutes. The
largest optional block on that board is the Keccak accelerator (about
6,000 LUT4, docs/keccak_hw.md); `EXTRA_DEFINES="-DGPU_T2 -DNO_KECCAK"`
builds without it (SHA-3 and ML-KEM then take the software path, with
the same answers).

| sergei_ml2, picorv32, nextpnr 0.11 (yowasp), default seed | COMB | DP16KD | sys_clk |
|---|---|---|---|
| the first engine (gpu_tri, retired), with the depth-test stage | 83% | 88 | 51.10 MHz |
| T2 (`GPU_T2`) | 93% | 102 | not routed after 90 minutes |
| T2 without Keccak (`GPU_T2 NO_KECCAK`) | **78%** (34,579) | 99 | **52.70 MHz** |

In the last, the slowest path is the SOC's own (the memory arbiter and
the MTU into the instruction cache), not T2's.

A frame in the RTL bench (48 MHz):

| test_t2's frame | geometry (stream in) | first word to last VRAM write |
|---|---|---|
| lists: lit sphere, cube, mixed, ring; 352 triangles | 376 k cycles | 690 k (14 ms) |
| the same, unlit, no culling (more pixels) | 452 k | 828 k (17 ms) |

The geometry unit is about half to two-thirds of a list frame; with one
strip buffer, the raster and the output take turns for the rest. The
CPU's share is writing the stream: with the 2,048-word FIFO the list
frame's 2,168 words go in within 17 k cycles, and the CPU is free for
the rest of the frame.

## The picture in main memory (`VRAM_SDRAM`)

On the console's Lakritz ([profiles.md](profiles.md)) chunky's picture
is in SDRAM, not VRAM (docs/color.md, "In main memory"), and T2 is built
for it:

- **`LINEAR`**: the output writes line y's word 20 s + i (segment s) at
  VBASE + 4 (80 y + 20 s + i): a line of 320 bytes after another, the
  layout the scanout's fetch reads and software draws in.
- **`POST_LOG2`**: a posted-write queue (2^10 words, three DP16KD): the
  output side's words go into it and it to the bus, so the raster goes
  on while SDRAM takes several cycles a write. STATUS stays busy until
  it is empty.
- **`FIFO_LOG2`**: the stream FIFO's size (11, 2,048 words, by default).
- T2's master is on the main bus there (`rtl/arbiter_gfx.v`, after the
  scanout's fetch and the audio mixer), not VRAM's.

test_t2's frame 2 in `tb_gpu_t2` (one strip buffer), first word to last
write:

| | cycles |
|---|---|
| VRAM (acked the next cycle), as before | 689 k |
| a write every 4-12 cycles, no queue | 843 k |
| the same, with the 1,024-word queue | 717 k |
| VRAM, with the queue | 651 k |

With `-DWLAT=n` every write is acked n cycles after it is offered, as
DDR3 does a word: 19 cycles without the controller's write combining,
about 8 with (docs/ddr3.md, "Write combining"). Frame 2, LINEAR with the
queue:

| write cost | geometry (to GO) | raster | frame |
|---|---|---|---|
| 1 cycle | 477 k | 170 k | 651 k |
| 8 cycles | 477 k | 207 k | 705 k |
| 19 cycles | 477 k | 382 k | 901 k |

### Pages

With one picture T2 writes the one being shown, so its output waits for
vertical blanking (CTRL.VSYNC) to stay ahead of the beam, and with one
strip buffer the raster waits with it: on average half a video frame
(about 400 k cycles at 48 MHz) on every frame. In main memory a picture
is only 76,800 bytes of the program's heap, so zgl keeps three when a
frame is the whole picture (sw/common/zgl/zgl_chunky.c, "pages"): the
one shown, one waiting for the next frame boundary (CKBASE is taken as a
frame starts), and the one T2 draws into, with no VSYNC and no waiting.
At glFinish() the finished page becomes `z_chunky_mem`, so what the
program draws after it (a HUD) lands there too, and `ar_frame()` shows
it, from the next boundary, before it waits; a program that never calls
`ar_frame()` has it shown when its next frame starts. A smaller frame
keeps the one picture and VSYNC, since what is outside it is meant to
stay.

**Not waiting for T2.** `ar_frame()`'s present does not wait for T2 to
finish: the frame is handed over and shown once T2's STRIPS register
(strips written out since RESET) says its last strip is out, while the
program steps its game and streams the next frame into the third
picture. Every paged frame is the same number of strips, learned from
the first two frames, which wait as before. On mausrun3d this takes
T2's tail after the last word (0.47-0.54 M cycles on
sergei_ml2_console) off the CPU's frame. A frame is shown one frame
later than before; `glFinish()` still waits for everything.

That needs **VBASE taken frame by frame**: the next frame's picture is
written to VBASE while T2 may still be writing the last one out. T2
queues VBASE writes (four deep) and each frame's first strip going out
takes the next; the posted-write queue's words carry their frame's bit,
so a frame's tail drains to its own picture (CAPS bit 1;
`tb_gpu_t2 -DTWO`). Without it -- T2 used VBASE as it wrote, and again as
its write queue drained -- the last frame's late strips landed in the
next frame's picture, and everything wobbled as the view moved. On a
bitstream without CAPS bit 1, zgl waits for T2 to be idle before
changing VBASE.

## Tests

```
make -C sw/common/zgl test       # test_t2: model vs software; firmware (ISS) vs model
make -C sw/common/zgl simtest    # chunkytest, ZSIM_GTRI=2
T2_VEC=../../../rtl/gpu/bench/t2 sw/common/zgl/tests/test_t2   # the RTL benches' vectors
cp sw/t2fw/t2fw.hex sw/t2fw/t2fw_tex.hex rtl/gpu/bench/t2/
make rtl/gpu/t2_recip.hex       # the reciprocal table (gen_t2_recip.py), for -DTEX
cd rtl/gpu/bench
iverilog -g2005 -DVEC=2 -o /tmp/tb_z32g tb_zeitlos32g.v ../zeitlos32g.v && vvp /tmp/tb_z32g
iverilog -g2005 -DVEC=2 -o /tmp/tb_t2 tb_gpu_t2.v ../gpu_t2.v ../zeitlos32g.v && vvp /tmp/tb_t2
iverilog -g2005 -DVEC=10 -DTEX -DLINEAR -DPOST=10 -o /tmp/tb_t2t tb_gpu_t2.v ../gpu_t2.v ../zeitlos32g.v && vvp /tmp/tb_t2t
```

- `test_t2`: the same scenes by the software strips and by T2's model:
  immediate mode 0 pixels differ; lists 0-10 (positions and normals
  rounded when compiled); near clipping about 560, all but two by one
  index (T2 clips each triangle, the software each polygon). It also
  runs the real firmware on `tests/t2g_iss.c` (an RV32IM simulator of
  zeitlos32g) and holds its records to the model's, word for word.
- `tb_zeitlos32g`: the RTL core on each of test_t2's seven streams, its
  records held to the model's: all pass.
- `tb_gpu_t2`: T2 whole, each stream written into CMD, all of chunky
  VRAM held to the model's picture: all pass.
  `-DLINEAR` builds T2 LINEAR (below) and holds the linear picture to
  the same model; `-DSLOW` acks each write after 4-12 cycles, as main
  memory behind the bus; `-DPOST=10` adds the posted-write queue.
- Positional lights: test_t2's "immediate, point light" (0 pixels
  differ), "lists, point light" and "lists, point light, normalize"
  (a few pixels by one index: the software lights in the eye's space,
  T2 in the model's, PLIGHT).
- Textures: test_t2's "lists, textured" and "lists, textured, near"
  (frames 10 and 11) hold the firmware's TTRI records to the model's;
  `tb_gpu_t2 -DTEX` writes the texture memory through TEXADDR and
  TEXDATA and holds every word of the picture to the model's: frames 10
  and 11 pass, and an untextured frame with and without textures built
  in. `T2_PGM=prefix` writes the textured frames as pictures of indices.
- space3d's `make render-chunky` plays the game in chunky game mode
  on the host twice, by the software strips and by T2's model, and
  holds the pictures to each other: 0 pixels differ.
- `ZSIM_GTRI=2` in the simulator runs the host model at each `END`;
  `ZSIM_GTRI_STATS=1` prints each frame's counts.

## On hardware

sergei_ml2_console, 2026-11-05, with textures (`T2_TEX_LOG2=13`), the
texture combine in its own stage (x9: 44.9 MHz without it), DDR3 write
combining, three pages. From each program's console report:

| program | frames a second | cycles a frame |
|---|---|---|
| gldemo chunky (380 primitives) | 59.0-59.5 (52.8 the first report) | about 0.90 M drawing: scene 0.50-0.61 M, glFinish 0.38 M |
| mausrun3d, textures on | 18-20 | CPU 1.45-2.35 M (206-292 triangles sent) |
| mausrun3d, textures off | 18-24 | CPU 1.13-1.66 M (166-251 sent) |
| space3d, chunky | 15 | 2.8-3.1 M |
| mausrun3d, textures on, **zeitlos32p** (and zgl not waiting for T2) | 40 | CPU 1.04-1.07 M (216 sent) |
| gldemo chunky, the lamp on (O) | 41.6-42.1 | 1.23 M: scene 0.56 M, glFinish 0.66 M |

**A positional light** costs T2's geometry unit, not the CPU: with
gldemo's lamp on, glFinish (T2 after the stream) goes from 0.38 M to
0.66 M cycles, the scene's stream from 0.49 M to 0.56 M -- the light's
distance and attenuation for every vertex, in the firmware's cold path.

**Textures cost no frame rate**: mausrun3d's frames a second with T on
and off are the same within a view's variation. Its CPU part is 1.1-2.3
M cycles a frame: the world's lists 0.5-1.5 M (the stream, including
waits on a full FIFO, so T2's time is partly in it), the things
0.33-0.52 M, the sky and the HUD about 0.13 M each.

## Next

- **mausrun3d at 30 frames a second with textures** (1.6 M cycles a
  frame at 48 MHz), the goal before 3D work rests a while. Its frame is
  about 70 k instructions of the CPU's (the simulator,
  `make -C sw/apps/mausrun3d simbench`) but 1.5-2.3 M cycles on the
  board, at about 15 cycles an instruction in every part (picorv32 on
  DDR3), plus T2's 0.5 M tail after the last word. Done: zgl no longer
  waits for the tail ("Not waiting for T2"), and sergei_ml2_console
  moves to zeitlos32p. To measure on the board.
- Speed: the geometry unit is the longest part of a list frame (about
  1,250 cycles a triangle; loads, at 3 cycles each, are a third of
  it). The core could load in 2 cycles if timing allows; a frame's
  geometry could overlap the last frame's raster with a second record
  list (11 DP16KD).
- Size: T2 does not fit the 25F desktop builds at all, and only just
  fits the 45F ones. (RV32IMC for the geometry core was measured: 26%
  less code, but its expander would cost more LUTs than the block RAM
  it saves, and no speed.)
- More triangles than the record list holds (512): zgl sends the frame
  again for each group of strips (not yet: extra records are dropped
  and counted).
- Lakritz (25F): block RAM is the limit; drawing straight into VRAM
  instead of strip buffers is the plan (the proposal, "Lakritz").
