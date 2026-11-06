# mauskart3d

A kart race in 3D on zgl and T2: the mouse in a matchbox on four wheels,
three laps of the Meadow Ring or Sandy Bay against the cat, the frog
and the owl.
`sw/apps/mauskart3d`, in the games set on the card.

    > run mauskart3d

Game mode in colour (F11, then C) on chunky VRAM; made for T2. Without
T2 it runs on zgl's software strips, slowly.

| key | |
|---|---|
| Up | accelerate |
| Down | brake, reverse |
| Left, Right | steer; on the title, the track |
| Space | use the power-up |
| Enter | start; again after the race |
| M | music on or off |
| S | scenery (trees, bushes, hills) on or off: the frame rate without it |
| F | pacing free (no lock to 30 or 20): the frame rate as it is |

## The race

The title, a three-second countdown on the grid (the mouse at the back
left), three laps, then the results: places, times and the best lap.
Places come from laps and segments done; a lap counts over the start
line only once past halfway, so backing over the line takes it back.

**Power-ups** (mausrun's, from the ? boxes in rows across the road;
the further back, the better the draw):

| | |
|---|---|
| SHOES | 2.5 seconds at 135% of the top speed |
| FEATHER | a jump |
| BELL | stuns every kart ahead within a third of a lap (unless shielded) |
| BUBBLE | a shield for 10 seconds: no stun, no speed lost in bumps |
| MAGNET | 3 seconds pulled to the kart ahead (118% while one is near) |

Cheese is a short burst of speed, a spring a jump. Taken ? boxes and
cheese come back after three and four seconds.

When the mouse rings the bell, DING! for the moment; a stunned kart has
stars turning over it, and DIZZY! shows while it is the mouse's.

## Sound

mausrun's tunes and effects (`../mausrun/sound.c`): the countdown's
ticks, cheese, power-ups, the bell, bumps, springs, laps and the
finish. The engine is a low buzz (25% pulse) at 55-165 Hz with the
mouse's speed, a fifth higher on a boost: a 7-frame note played again
every 6 frames at the lowest priority, so any other effect takes its
channel; it never takes the music's.

## Driving

Arcade physics at a fixed 60 Hz (`step_kart()`): positions Q8 world
units, speed along the heading. The steering is a wheel, -32..32: held, it turns
in over 16 ticks (a quarter second) and comes back over 8, and the
kart turns by its square, so a tap is a small nudge and a hold comes on
smoothly. The chase camera follows the heading with its own turning
eased on and off (no step in how fast the view swings). The AI steers
in proportion to how far its point is off. The top speed is 5.25 world units a
tick (315 a second; mausrun3d's mouse runs about 90), 45% of it on the
grass (past the kerbs). Steering is tighter with speed up to 760 turn16
a tick. Karts bump each other apart (radius 9), each losing a little
speed.

**The AI** steers to a point on its own line a few segments ahead
(further at speed), moves its line to pass a kart ahead on it, and
drives at the track's speed there (`vmax`: what the steering can hold
through the sharpest of the next five segments) times its skill
(92-98%). A rubber band: 8% faster an eighth of a lap behind the mouse,
6% slower as far ahead. It uses a power-up a random 0.7-5 seconds after
getting it.

## The tracks

`track.c`: the points of a closed Catmull-Rom curve, walked into
segments 32 world units long. The road is 80 wide, kerbs included.

| track | points | segments | ground | scenery |
|---|---|---|---|---|
| The Meadow Ring | 19 | 230 (7,360 world units: 25 s at the top speed) | grass | two thirds trees |
| Sandy Bay | 15 | 202 | sand | bushes and boxes, a quarter trees |

Left and Right on the title choose; the track's lists are made again
(`tk_lists()` gives back the last one's). The ? boxes, cheese and the
spring are placed by segment for 230 segments and scaled to the track.

The road and its kerbs are **one textured quad a segment**: a 64 x 16
`ZGL_TEX_RAMP` texture (each texel its own ramp and tone), stretched
once across, the kerbs red and white by turns a segment long, a dashed
middle line. Separate kerb quads cost twice the vertices.

Lists of 8 segments, each twice: near (the road a segment at a time,
posts every 4th segment) and far (two segments a quad, the boards on
the bends alone), chosen by the chunk's distance ahead. Chunks outside
a 100-degree view or past the draw distance are not called. The grass
is the sky list's band under the horizon: ground quads under the camera
cost T2 a clip each (about 150 k cycles a frame, measured), for nothing
to see.

## Scenery

Trees, bushes and mausrun's boxes (one or two side by side) beside the road, `ZGL_TEX_RAMP` cards with a key (texel
0 not drawn), standing up and facing back along the track towards the
karts; placed by a hash of the segment, about three a chunk, never on
or by another part of the road. A list a chunk again, near (all) and
far (the trees), drawn to three quarters of the draw distance. Hills on
the horizon are a 2D band after the sky, turning with the view (a full
turn is four screen widths). **S** turns all of it off, for the frame
rate without it; the report says "no scenery" while it is off.

Also: the bubble is a ring behind a shielded kart (flashing in its last
two seconds), and nothing shakes: a pixel's shake at full boost read as judder.

## Karts

`karts.c`: the matchbox (its label a texture on the sides and back),
four wheels as black cards and the driver's head. The chase camera is
always behind, so there are no front faces and the ears face back.
Near (closer than 120): about 22 triangles; far: the box and a quad in
the driver's colour, 8. A shadow under the near ones.

## Frame budget

**On the board** (sergei_ml2_console, 2026-11-06, the first build):
19-29 frames a second -- the pacing's even 20 and 30 -- with the CPU
waiting on T2 in every part (about 10 cycles an instruction). Not 60.

**Why: T2's frame is its geometry and then its raster.** One record
buffer: the raster starts on a frame's records when the geometry core
has made them all, and it draws **a pixel a clock across each
triangle's bounding box**, every row (gpu_t2.v, R_TWORD). The host's
model had counted four pixels a clock (the first design's) and set the
two side by side, so it said 620 k a frame where the board took two to
three times that. Now (tests/t2_raster.c, tests/render.c) it counts a
pixel a clock and adds them:

| a frame, on the host (the Meadow Ring, 700 frames) | average | most |
|---|---|---|
| T2's geometry core (zeitlos32g's ISS) | 648 k cycles | 840 k |
| raster and output (the model) | 335 k | 434 k |
| **T2, the two one after the other** | **983 k** | **1.27 M** |

mausrun3d checks it: the model now says 1.03 M a frame, about 46 fps;
the board does 40 (1.2 M).

What a part costs T2 (the geometry; the same 700 frames, the part left
out): the karts 200 k, the HUD 125 k (lines: about a triangle each),
the track and scenery 300 k, the ? boxes and cheese 10 k. Since the
board, the ground is the clear colour and only the sky (the smaller
half) is drawn: two triangles over the whole screen were 90 k of
raster.

So with one record list an even **30** (1.6 M) holds with room, and
60 (800 k) does not. sergei_ml2_console is now built with T2's overlap
options ([zgl_t2.md](zgl_t2.md), "Overlap"): two record lists, so a
frame is the longer of geometry and raster, two strip buffers, and
rows that end past a triangle's span. The model with them
(`T2_TWO_LISTS=1 T2_ROW_END=1`, the same 700 frames):

| | average | most |
|---|---|---|
| raster and output, rows ending past the span | 279 k | 347 k |
| **T2, a frame: the longer of the two** | **648 k** | **840 k** |

7 frames of the 700 over 800 k: about 60 frames a second, to be seen
on the board.

**On the board with the overlap gateware** (2026-11-06): 19-29 fps,
still -- now the CPU's. Its frame was 0.9-1.45 M cycles before any wait:
the HUD's list made again every frame (the speed bar was in its key:
0.2-0.7 M), the ? boxes' and cheese's matrices (0.1-0.17 M), the game's
steps (0.13-0.23 M: the nearest segment searched with two divisions a
segment, every thing against every kart). Since: the speed bar drawn
outside the list; each thing a list of its own with its place in its
vertices, called for the ten segments ahead; things looked for on a
kart's own segment and the next; the nearest segment by ±3 without a
division; the minimap's karts dashes without matrices. The CPU still
copies every list word into T2's FIFO (zgl_t2_call()): the track, the
karts and the things are mostly that, at about 10 cycles an
instruction, waiting on the FIFO.

The RTL draws a mauskart3d frame (`T2_DUMP=420`, a bench vector from
the render test) as the model does, word for word, with all three
options, LINEAR and DDR3-like write latency: about 860 k cycles for one
frame and 750 k a frame back to back.

**The black lines** across the road on the board (2026-11-06) were the
model's too: rows the clear colour showed through, where a road quad
clipped at the near plane (and so, then, at the picture's sides) met
the next one, unclipped; T2's firmware now clips to the guard band's
sides (zgl_t2.md, "The geometry unit"). The firmware is block RAM:
a new bitstream.

**With the overlap gateware and the crack fix** (2026-11-06): 28-30
fps locked, the CPU's parts 0.6-0.97 M cycles a frame -- and they run
at 6-10 cycles an instruction even where nothing goes to T2 (the
game's steps, 10-25 k instructions, 0.06-0.19 M cycles): the CPU, not
T2, is the bound now. The bench says T2 alone does this frame in about
725 k cycles back to back. Since: the race's HUD in letters of texture
(z_font_5x8 in a 64x32 ZGL_TEX_ADD texture, two triangles a letter
where the lines were four or five); karts near under 90; 20 fps only
after 24 long frames at 30; **F** for free pacing (no lock), to read
the frame rate as it is. The model's geometry: 565 k a frame (627 k).

**Short black lines, flashing** on the left of the picture (one
scanline of two: the scanout's line fetch late) were the CPU's stores
into T2's full FIFO holding the main bus; zgl now waits for room
before it stores (zgl_t2.md, "Never a store into a full FIFO").

(Not alone: see zgl_t2.md -- the line fetch four lines ahead,
`CKFETCH_LINES=4`, a new bitstream.)

**The bump at full speed**: the view widened with the speed, its near
plane in by up to 3 -- and at the top the speed wavers by a few
units, so the near plane (and the horizon with it) flipped a step every
frame. Now it widens only on a boost, by 2, a step every 12 frames.

**Driving**: the road wider (half 42, was 34), the chase camera
quicker after the heading, and an assist for the mouse: on the road,
going forward, the kart turns a little with the road of its own (a
twelfth of the difference a tick with the wheel straight, a
twenty-fourth when steering, at most 240 turn16).

**The pacing.** Frames that run long three times hold an even 30 (two
refreshes a frame, `ar_frame_lock(2)`, docs/arcade.md "Even pacing")
for five seconds and shorten the draw distance (to 384), rather than
1, 2, 1, 2; eight more at 30 make it an even 20 (lock 3), and the first
time the scenery goes off (the console says so; S turns it on again).
The first build counted the misses down by ticks, which at 30 rarely
came round: everything ended at 20.

The report every two seconds, under 80 columns (k: thousands of
cycles; a part with many more cycles than instructions is waiting for
T2):

    mk3d: 30 fps, CPU 562k a frame, 91 tris, view 384, lock 30
    mk3d cyc: sky 84 trk 198(5) thg 20 krt 72 hud 30 zgl 10 game 30 wait 300
    mk3d ins: sky 7 trk 20 thg 3 krt 8 hud 2 game 8

## Tests

    make -C sw/apps/mauskart3d render     # pictures in tests/out, the budget
    QUICK=1 sw/apps/mauskart3d/tests/render_mauskart3d t2 out   # Sandy Bay, 700 frames

The game on the arcade runtime's host half, T2's model drawing, the
geometry core's cycles from its ISS (as mausrun3d's). The mouse is on
autopilot (the AI) and the checks are: the countdown, racing at speed,
a lap done, the race over.

## Next

- On the board: 60 fps on sergei_ml2_console; 30+ on Lakritz.
- Skid marks; a bridge (the track crossing itself needs heights).
- More tracks; a best lap kept for each (zcfg).
