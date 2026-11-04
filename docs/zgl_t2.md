# T2: the triangle engine that runs the whole frame

Status: **built and checked in simulation (RTL bench, every VRAM word);
in the SOC behind `GPU_T2`; not yet on hardware.** The plan and the
decisions are in the project's `claude/zgl-t2-proposal.md`.

Today's triangle engine ([gpu_tri.md](gpu_tri.md)) draws the pixels;
the CPU still does everything before them, and on sergei_ml2 that was
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

## Registers

As today's engine's ([gpu_tri.md](gpu_tri.md)), at the same address:
CTRL (WAIT_BEAM, VSYNC), STATUS (busy: anything in the FIFO, the core
working, a strip being drawn or going out), CMD, VBASE, STRIPS, RESET,
ID = 0x5A545232 ("ZTR2"). No MONO.

The geometry unit's own registers (`sw/t2fw/io.c`): IN, STATUS (the
raster has the list), RX, RY, CLEAR, GO, and the record list.

## Building

```
make BOARD=sergei_ml2 EXTRA_DEFINES="-DGPU_T2 -DNO_KECCAK"                    # picorv32
make BOARD=sergei_ml2 EXTRA_DEFINES="-DGPU_T2 -DNO_KECCAK -DCPU_ZEITLOS32P"
```

`GPU_T2` takes `GPU_TRI`'s place (the same address, VRAM port and
plumbing in sysctl.v). After changing `t2fw.c`, `make -C sw/t2fw`
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
| today's engine (`GPU_TRI`), with the depth-test stage | 83% | 88 | 51.10 MHz |
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

## Tests

```
make -C sw/common/zgl test       # test_t2: model vs software; firmware (ISS) vs model
make -C sw/common/zgl simtest    # chunkytest, ZSIM_GTRI=2
T2_VEC=../../../rtl/gpu/bench/t2 sw/common/zgl/tests/test_t2   # the RTL benches' vectors
cp sw/t2fw/t2fw.hex rtl/gpu/bench/t2/
cd rtl/gpu/bench
iverilog -g2005 -DVEC=2 -o /tmp/tb_z32g tb_zeitlos32g.v ../zeitlos32g.v && vvp /tmp/tb_z32g
iverilog -g2005 -DVEC=2 -o /tmp/tb_t2 tb_gpu_t2.v ../gpu_t2.v ../zeitlos32g.v && vvp /tmp/tb_t2
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
- space3d's `make render-chunky` plays the game in chunky game mode
  on the host twice, by the software strips and by T2's model, and
  holds the pictures to each other: 0 pixels differ.
- `ZSIM_GTRI=2` in the simulator runs the host model at each `END`;
  `ZSIM_GTRI_STATS=1` prints each frame's counts.

## Next

- On hardware: `run gldemo chunky` against `chunky soft`.
- Speed: the geometry unit is the longest part of a list frame. The
  firmware has room (64-bit arithmetic where 32 bits would do; the
  record copy), and so has the core (loads in 2 cycles if timing
  allows, a 4-wide multiply-add instruction for transforms).
- More triangles than the record list holds (512): zgl sends the frame
  again for each group of strips (not yet: extra records are dropped
  and counted).
- Lakritz (25F): block RAM is the limit; drawing straight into VRAM
  instead of strip buffers is the plan (the proposal, "Lakritz").
