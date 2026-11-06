# mausrun3d

Kind: a **full-screen app** ([app_kinds.md](app_kinds.md)), for game mode on a T2 bitstream ([zgl_t2.md](zgl_t2.md)).

**Maus Run in 3D**: the mouse runs about an open island made from one
of mausrun's levels ([mausrun.md](mausrun.md)), the camera behind it.
The plan, and what is to come (the game's things, then textures in T2
with a toggle: it is T2's texture demo), is in the project's
`claude/mausrun3d-proposal.md`. **Phases 1 and 2** are here: the world,
the camera, and mausrun's game in it -- one level, world 1.

```
> run mausrun3d
```

It goes to game mode at once. In colour on a board with chunky game
mode it draws through zgl's `ZGL_CHUNKY`: by T2 where the bitstream has
it, else in software -- which works, at a few
frames a second. Anywhere else it says what it needs.

## Keys

| Key | Does |
|---|---|
| Arrows (or a gamepad) | run, as the camera looks: up away from it |
| Space (or the pad's A) | jump; in water, a stroke |
| Q, E | turn the camera |
| Enter (START) | the camera behind the mouse |
| M | music on or off |
| T | textures off or on (on a T2 with textures) |
| W | wait for each frame at its end, or not (to compare; the report says which) |
| L | lock to an even 30 frames a second (each frame two refreshes), or not |
| F11, Esc | game mode, or back to the window |
| F1 | help |

The camera swings round behind the mouse as it runs (not when it runs
at the camera), and comes in where a block is between it and the mouse.

## Worlds

A world is a text file, top-down, made by `gen_world.py` from one of
mausrun's levels: the level's run is a path five tiles wide winding
across an island -- its floor the path's height, its pits chasms across
it, its boxes, shelves, springs, ? boxes, cheese, foes, mouse holes,
star and cheese wheel along it -- with meadow on broad plateaus, a beach
and the sea around.

```
python3 gen_world.py ../../data/mausrun/levels/01.txt > ../../data/mausrun3d/world01.txt
python3 gen_world.py --size 256 ...                 # 16 to 512, a multiple of 16
python3 gen_world.py --builtin ../../data/mausrun3d/world01.txt mr3_world01 > world_builtin.c
```

```
# comments
name The Meadow
theme meadow                   the colours
size 128                       tiles a side
ground                         SIZE rows of SIZE: the ground's top in blocks
...                            0-9 a-f, '.' none (a chasm)
surface                        g grass, p path, s sand, r rock, d earth
...
water                          the water's surface in blocks, '.' none
...
blocks                         x z y c: B box, ? ? box, - shelf, S spring,
59 69 11 B                     K biscuit, ^ thorns
things                         x z y c: c cheese, C cat, Z sleeping cat,
62 64 4 c                      F frog, O owl, H mouse hole, G cheese wheel,
end                            * star, M the start, w b m l k h power-ups
```

A tile is 16 world units across and a block 16 high. World 1 is
compiled in (`world_builtin.c`, generated and committed, as mausrun's
levels are); `/data/mausrun3d/world01.txt` on the card, if there is
one, is played instead, so a hand-edited world needs no rebuild. The
parser (`world.c`) says what is wrong and on which line.

## Drawing the world

`world.c` keeps the blocks (the ground's columns and the blocks, 16
high) and cuts the map into **cells of 8×8 tiles**. A cell's faces --
where a block meets air -- are merged into rectangles (greedy meshing:
a run of one material along a row, then as many rows as match) and kept
as **seven display lists**: one for each way a face can face, and the
water's surface. On T2 a list is compiled once and copied to the engine
every frame it is drawn. Faces the water hides are left out (the sea
bed, a pond's banks).

A frame sends T2 only:

- the cells whose box is inside the view: its four sides and the draw
  distance, as planes (`box_out()`);
- of those, the face sets that can face the eye -- a cell's east faces
  when the eye is east of its westmost, its tops when the eye is above
  its lowest.

Cells are meshed as they first come into view, two a frame (those round
the start at once).

### Frame budget

T2's geometry core transforms every triangle it is sent, about 1,600
cycles each here, so **60 fps (800,000 cycles at 48 MHz) is about 450
triangles sent**. The draw distance follows what a frame costs: a frame
that sent more than 340 draws the next a tile shorter (to 14 tiles), one
that sent fewer than 270 a little longer (to 22). The haze under the
horizon hides where the world stops.

On the host (`make render`, below) a frame of world 1:

| a frame | average | most |
|---|---|---|
| T2's geometry core (zeitlos32g's ISS, the real firmware) | 426 k cycles | 885 k |
| raster and output (the model's estimate) | 186 k | 221 k |
| triangles sent | 265 | 427 |
| drawn | 137 | 334 (of 512 records) |

Three frames of the 1,004 went over 800 k (a turn of the camera across
the path's walls); the CPU's part is copying about 3,500 words.

What made it fit, from about 2.4 M cycles a frame at first: broad
plateaus in the generator (few edges, few faces), no faces under water,
cells of 8 tiles rather than 16 and a real frustum test for them, the
draw distance 22 tiles rather than 30, and in T2's firmware a **guard
band** (a triangle outside only the picture's sides goes to the raster
unclipped, docs/zgl_t2.md) and a normal shared by a chunk's vertices
lit once.

## The map

The world from above at the bottom right, 64 pixels a side: a 32x32
texture made once (`map_make()`), each texel its ramp and tone
(`ZGL_TEX_RAMP`) -- the surface's colour, boxes, thorns, water,
lighter the higher -- with chasms keyed out, drawn as one textured
quad (a list: textures go through lists), and the mouse a white pointer
the way it faces. Three triangles, where the edges as lines would be
dozens (a line costs T2 about what a triangle does). North is up.
Without textures (a T2 without them), no map.

## Colours

256 entries: white, the HUD's, the shadow's, then 16-entry ramps --
the sky (horizon to the top), grass, earth, path, sand, rock, box, ?
box, water, the mouse, pink, cheese, cat, shelf, thorns -- dark to
light, lit along by zgl's colour-index lighting (one light, from above
and behind the left). The sky is a gradient of four triangles above the
horizon, the haze under it the sea's colour.

## Textures

On a T2 with textures (sergei_ml2_console; [zgl_t2.md](zgl_t2.md),
"Textures") the world's faces are textured, a 16×16 texture of tones a
material, a tile a repeat (texture coordinates are world units from the
cell's corner): earth, boxes, ? boxes, biscuits, springs, thorns and
water are mausrun's own tile drawings (`../mausrun/sprites.c`), ink and
paper as two tones; grass and sand are specks, path bricks, shelves
boards. ZGL_TEX_TONE: the material's ramp, the texel's tone lit, so the
light falls on them as on the untextured faces. The models are not
textured. T turns them off and on, and the report every two seconds
says which: what textures cost is the difference. On the host, T2's
geometry goes from 514 k to 619 k cycles a frame with them (the
textured firmware on the ISS, `make render`).

On hardware (sergei_ml2_console, 2026-11-05) the frame rate is the same
with and without them, 18-20 frames a second at 9-12 tiles' view, and
up to 24 where the view holds fewer triangles. From the report:

| a frame (CPU cycles) | textures on | textures off |
|---|---|---|
| total | 1.45-2.35 M | 1.13-1.66 M |
| the world's lists | 0.84-1.46 M | 0.53-0.99 M |
| the things | 0.34-0.52 M | 0.33-0.41 M |
| the sky / the HUD | 0.13 M / 0.13-0.23 M | 0.12-0.14 M / 0.12-0.14 M |
| the game | 0.11-0.17 M | 0.12-0.18 M |
| triangles sent | 206-292 | 166-251 |

The CPU's part alone is 1.1-2.3 M cycles, over the 800 k of 60 fps
and 1.6 M of 30. The instructions (the report's second line, below)
say why: every part runs at about 15 cycles an instruction, the game's
steps too, which never touch T2 -- so it is the CPU (picorv32 on DDR3),
not T2's FIFO. On top of it came T2's tail, 0.47-0.54 M cycles after
the last word, which the frame waited for until zgl stopped waiting
(zgl_t2.md, "Not waiting for T2"); and sergei_ml2_console moves to
zeitlos32p (profiles.md).

**On zeitlos32p** (sergei_ml2_console, 2026-11-05): **40 frames a
second** with textures, 216 triangles at 9 tiles' view, 1.04-1.07 M
CPU cycles a frame at about 2 cycles an instruction (the world 0.39-0.43 M
for 37-39 k instructions, the things 0.19 M, the HUD 0.05 M, the game
0.05 M); the sky's 0.39-0.43 M is the wait for a free picture, all
three busy. From 18-20 on picorv32.

The report's second line gives the same parts in instructions, then
`ar_frame()`'s time in cycles: the wait for the frame boundary (and for
T2, if it is a frame behind; zgl_t2.md, "Not waiting for T2"). A part with many more cycles than
instructions is waiting -- on T2's FIFO, or on memory. In the simulator
(`make simbench`, its cycle model, T2 as its model) a frame is about
70 k instructions and 0.35-0.4 M cycles.

## The game

mausrun's, in three dimensions ([mausrun.md](mausrun.md), "Playing"):

- A level starts with its number and name (three seconds, or Space).
- **Cheese** to collect (the count at the top left); with the **magnet**
  the cheese near the mouse flies to it.
- **Foes** walk their beat -- along x or z, whichever runs further on
  the level they stand on, up to four tiles each way. **Cats** walk,
  **frogs** sit and hop, croaking, **owls** fly to and fro, bobbing.
  Coming down on one leaves it **dizzy** (stars), then **napping** ten
  seconds, harmless; meeting one any other way costs a **heart**, and
  the mouse blinks for two seconds, when nothing can hurt it, knocked
  back. A **sleeping cat** wakes when the mouse lands within three tiles
  of it and comes for it.
- **Thorns'** sides hurt; their tops are clear.
- A **? box** bumped from below is a box, and out of it pops a power-up
  -- a heart when the mouse is short of hearts.
- **Springs** throw the mouse five tiles up. A **biscuit** cracks under
  it and crumbles a second later, and is baked again five seconds after.
- **Power-ups**: the **feather** a second jump in the air, the
  **bubble** the next bump free, the **magnet**, the **bell** (everyone
  runs away, and nobody hurts), **fast shoes** (half as fast again),
  each 20 seconds, shown under the count with a bar; a **heart** (up to
  seven).
- A **mouse hole** passed is the checkpoint (it shows a cheese). A
  **pit** -- a chasm, or the edge of the world -- costs a heart and goes
  back to it.
- The **hidden star**; the **cheese wheel** ends the level: **WELL
  DONE!**, the cheese, the stars (one for finishing, one for every
  cheese, one for the star) and the time; Space plays it again.
- No hearts left: **GAME OVER**, and Space plays the same level again.
- mausrun's sounds and the meadow's tune (`../mausrun/sound.c`, linked
  as it is).

## Models

`models.c`: low-poly, after mausrun's 16x16 drawings and in their
colours, a display list each in world units, built from boxes, tapered
boxes, pyramids and prisms, a normal a face: the mouse (two running
frames), cat (two, and napping), frog (sitting, hopping), owl, mouse hole
(and with its cheese), cheese, cheese wheel, star, heart, the power-ups,
dizzy stars. 10 to 60 triangles each. The world's own blocks -- boxes,
? boxes, shelves, springs, biscuits, thorns -- are the world's lists
(`world.c`); a block that changes (a ? box used, a biscuit crumbling)
has its cell's lists made again.

## Words

On a chunky picture the runtime draws no text, so `vfont.c` draws
letters of lines: A-Z, 0-9 and a little punctuation, polylines on a
4x6 grid, at any scale. The HUD and the messages use them.

## The mouse

Kept in 1/256 world units: a box 10 wide and 11 high against the
blocks, an axis at a time (a wall stops that axis only). Shelves are
solid from above only. A jump is 3.4 tiles high; a block one high needs
one, as in mausrun. In water it is slower, falls slowly and Space is a
stroke. Out of the bottom of the world it is back at the start. A shadow
under it, on what is under it.

## On a build host

`make render` in `sw/apps/mausrun3d` runs the game itself on the arcade
runtime's host half with chunky game mode (`tests/render.c`): zgl's
stream a frame at a time through T2's model into the pictures, and
through zeitlos32g's instruction-set simulator with the real firmware
for its cycles; at the end what a frame cost (the table above).

The script plays the level by putting the mouse where things are (the
game's `mr3_test_*()` calls, compiled for the host only) and checks
what happens: landing on a cat costs nothing and running into one a
heart; a ? box bumped from below is a box; a cheese taken; the star
found; the wheel says WELL DONE and Space plays again with the hearts
back; the level card goes after three seconds; a fall costs a heart
and goes back to the mouse hole; the last heart is GAME OVER. With
everything in the world, a frame averages about 600 k geometry cycles
(4 of 850 over 60 fps's 800 k). `soft`
as the first argument draws with zgl's software strips instead.
`SHOW_OVER=1` lists the frames over 60 fps's budget. The pictures are
in `tests/out/`.

## Not yet

Moving planks (the generator makes their tracks shelves), weather, day
and night, skill, saving progress, a title, its icon in the dock;
textured models; more worlds; a 512×512 world wants more than the
8 MB an app may have (an arena, docs/app_runtime.md).
