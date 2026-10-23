# mausrun

Kind: a **full-screen app** ([app_kinds.md](app_kinds.md)).

**Maus Run**: a side-scrolling run-and-jump game for game mode. You are
a mouse; you run to the cheese wheel at the end of each level, collect
the cheese, jump the boxes and pits, bounce on the cats (it makes them
dizzy, and then they nap), find what the ? boxes hold, and look for the
star hidden in every level -- through a meadow, a garden, a kitchen and
over the roofs at night.

It began as `gamedemo`, a demonstration of the scrolling machinery --
one 2048-pixel level against a 320-pixel view, a debug line on the
screen -- and was renamed and made into a game for children (0.0.7,
[games proposal](#history)). The demonstration is all still here and
still documented below; the game is in front of it.

Lives in `sw/apps/mausrun/`, with its art and generator there and its
levels in `sw/data/mausrun/levels/`. Nothing it needs has been added to
`sw/common` beyond `zgame.[ch]`, which is general.

## Playing

The title screen drifts over the first level while a mouse runs along
the bottom with three cats after it. Its menu:

| | |
|---|---|
| **PLAY** | from level 1 |
| **CONTINUE: LEVEL n** | from the furthest level reached (shown once there is one) |
| **< EASY >** | the skill: Left and Right change it ([below](#skill)) |
| **QUIT** | back to the desktop |

Each line is spoken as the cursor reaches it, when speech is on.

A level starts with its number and name in a box -- "LEVEL 2, Over the
Hills", spoken -- and goes after three seconds or a press of Space. Run
to the **cheese wheel** to finish it (a level without one: its far
end). A cat, a frog, an owl or a thorn bush costs a **heart** (the
status line shows them): the mouse blinks for two seconds, when nothing
can hurt it, and goes on from where it is. A **pit** costs a heart too,
and the mouse comes back at the last **mouse hole** it passed (a hole
lights up with a cheese in it when it does), or at the start. With no
hearts left it is **GAME OVER**, and Space plays the same level again
with every heart back -- never back to level 1. Finishing a level shows **WELL DONE!** with the
cheese counted up, its **three stars** lit one by one -- one for
finishing, one for every cheese, one for the star hidden in the level --
and the time, and the next level is unlocked (remembered: CONTINUE).
The title shows the stars found so far of every level's three. After the last level, **YOU DID IT!** and the
sky cycles through a rainbow.

The **status line** along the top: cheese found of the level's, the
hearts, the level and the score, in the 8x8 font -- 16 screen pixels a
letter at the doubled 320x240 view, readable from across the room. Under
it, the power-ups in hand, each with a bar for the time it has left
(they blink for the last three seconds).

On easy, a mouse that stands still for four seconds is shown which way
to go: a big arrow blinks at the right of the view.

### What is in a level

| | |
|---|---|
| grass, ground, boxes | as they always were |
| **spring** | land on it and fly, about five tiles up |
| **shelf** | solid from above only: jump up through it, land on it |
| **biscuit** | stand on it and it cracks, then crumbles (a second on easy); it is baked again five seconds later |
| **thorn bush** | its points hurt; its top is clear of them |
| **water** | slow; Jump is a stroke, any time, and coming up through the surface is a hop out onto the bank |
| **? box** | bump it from below: out pops a power-up (a heart when the mouse is short of hearts) |
| **mouse hole** | the checkpoint |
| **cheese wheel** | the end of the level |
| **moving plank** | to and fro along its track; solid from above, and it carries the mouse |
| **hidden star** | one in each level, somewhere high or out of the way: the third star |

### Who lives there

Nobody is ever hurt. Coming down on a head bounces the mouse up and
leaves the foe **dizzy** (stars) for a second and a half, then
**napping** (zzz) -- ten seconds on easy -- and harmless while it does.

| | |
|---|---|
| **cat** | walks its beat, turns at walls and ledges |
| **sleeping cat** | harmless while it sleeps, and jumping on it does not wake it; **landing** near it does, and then it comes for the mouse -- tiptoe past |
| **frog** | sits, and hops back and forth about its place, croaking |
| **owl** | flies to and fro, bobbing, at night |

### Power-ups

Lying in a level or out of a ? box; walked into to take them, each
announced (spoken) and shown under the status line.

| | | how long (easy / normal / hard) |
|---|---|---|
| **feather** | one more jump in the air | 30 / 20 / 15 s |
| **bubble** | the next bump is free (it pops) | until then |
| **magnet** | the cheese nearby flies to the mouse | 30 / 20 / 15 s |
| **bell** | everyone runs away, and nobody hurts | 30 / 20 / 15 s |
| **fast shoes** | half as fast again (and the music livelier) | 30 / 20 / 15 s |
| **heart** | a heart more (up to seven) | |

### Themes

A level's `theme` line says how it looks and sounds; the map says what
is where, the same in every theme.

| | four colours | the far layer | grass is | thorns are | boxes are | tune |
|---|---|---|---|---|---|---|
| **meadow** | the original sky blue, brown, green, cream | trees | grass | thorns | boxes | Meadow (A minor) |
| **garden** | brighter, greens readable at night | sunflowers | grass | thorns | boxes | Garden (G major, bouncy) |
| **kitchen** | a pale wall (a lamp-lit one at night) | wallpaper, a shelf of jars and cups | floor tiles | mousetraps | boxes | Kitchen (C major, plucked) |
| **roofs** | night blue and brick | houses up the hill | roof tiles | thorns | chimneys | Roofs (D minor, a lullaby) |

Each theme lights its own day, sunset, night and dawn (`mr_lights[]`),
so `time` still says which of them a level gets. Outdoors at night there
is a moon; the kitchen has no sky, so no stars, moon, birds or lightning,
and only the meadow and the garden have waterfalls in their pits (a
kitchen's gap and a roof's edge fall to nothing). A tune changes only
when the theme does, so two meadow levels in a row play straight on.

### Moving platforms

A run of three or more `P` cells is a plank's track: a plank two tiles
wide goes to and fro along it at half a pixel a frame, solid from above
as a shelf is, and carries whoever stands on it (until they jump or
walk off). `levelcheck` takes the track for a shelf along its whole run:
the mouse can stand anywhere the plank goes by waiting for it there, and
riding it goes no further than walking along it would -- which holds
for planks that move sideways, and is why there are no others.

### Particles and weather

A pool of 96 little things that fly and go: dust from a landing and a
spring, crumbs from a biscuit, splashes, sparkles from a cheese or a
stomp, confetti at a mouse hole and the cheese wheel, feathers for the
second jump, zzz over sleepers, the magnet's cheese -- and the level's
**weather**: falling leaves, snow, rain or fireflies (up to 36 of the
pool, so a burst always has room). Each is one fill or one little
bitmap: one or two blits.

### Skill

The same three settings as the arcade games ([arcade.md](arcade.md#skill)),
easy by default:

| | Easy | Normal | Hard |
|---|---|---|---|
| hearts | 5 | 3 | 3 |
| cats' pace (1/16 px a frame) | 6 | 9 | 12 |
| a foe's nap after the stars | 10 s | 7 s | 5 s |
| power-ups last | 30 s | 20 s | 15 s |
| a sleeping cat wakes to a landing within | 24 px | 40 px | 56 px |
| a biscuit holds | 1 s | 0.7 s | 0.5 s |
| music (calm first, [sfx.md](sfx.md#calm-first)) | from 1 | from 3 | from 5 |

and on easy, the arrow. The music grows a step every two levels.
Normal's hearts and cats are the game as `gamedemo` played. The choice is kept in
`/data/mausrun/progress.txt`; without one it comes from
`apps.arcade.skill`, then easy.

### What is remembered

`/data/mausrun/progress.txt`, plain lines:

```
skill easy
unlocked 3
best 1 28
best 2 17
stars 1 7
stars 2 3
```

the skill, the furthest level reached, the most cheese found in each
level, and its stars (1 finished, 2 every cheese, 4 the hidden star:
7 is all three; they are only ever added to). Lines the game does not know are kept when it writes the file
(`progress.c`, host-tested).

## What it demonstrates

| Feature | Where |
|---|---|
| Game mode, 320x240 pixel-doubled | `rtl/gpu/gpu_video.v`, via `zgame.h` |
| Toroidal horizontal scroll | `z_game_fold()`, one camera register |
| Double buffering | `z_game_flip()`, one register write |
| Incremental redraw | `z_game_scroll_span()` |
| Hardware tile blitting | `z_fb_hw_blit_mem()` (`gpu_blit.v`) |
| Software masked sprites | `spr_draw()` — see below |
| Gamepads, two ports, hotplug | `zpad.h` |
| Keyboard fallback | `input_read()` |
| Sound effects and music | zsfx ([sfx.md](sfx.md)), on the hardware mixer if the board has one |
| Four colours, double-buffered | `zcolor.h`, if the board has colour — see [Colour](#colour) |

## Controls

Arrow keys or a gamepad, both live at once. There is no mode to select
and no mode to get stuck in: the keyboard and pad are read every frame
and OR'd together, so a machine with no pad is playable and a machine
with one still responds to the keyboard.

| Action | Keyboard | Pad |
|---|---|---|
| Move | Left / Right | d-pad |
| Jump | Up or Space | A, B or up |
| Choose (menus, boxes) | Enter or Space | A, B or Start |
| Pause, and go on | P | Start |
| Back: pause in play, then the menu; on the menu, quit | Esc | Select |
| Volume down / up | - / = | |
| Music on or off | M | |
| Colour on or off | C or F10 | |
| Help: the keys, the game paused | F1 (F1 or P goes on) | |

The old diagnostic keys are gone: A (the ambient layer off) and T (a
quarter of a day later), and `[ ]` (the music against the effects --
the balance is fixed now). So are R (restart; GAME OVER asks) and Esc
quitting at once from the middle of a level. mausrun is always in game
mode ([keys.md](keys.md)): it draws a whole picture into a back page
every frame and flips, for its smooth parallax, so it stays off the
arcade runtime and out of a window (decided 2026-10-23); F1 and F10
mean what they mean on the runtime.

Unplugging a pad mid-jump releases every button rather than leaving the
mouse running forever — that falls out of the hardware clearing pad
state on disconnect (`docs/gamepad.md`), and needs no code here.

## Colour

On a board with game-mode colour ([color.md](color.md)) and blitter
raster ops, the demo starts in four colours: sky blue, dark brown, leaf
green and cream. C switches between colour and monochrome at any time,
for comparison; both clear the framebuffer and the next frame is drawn
whole.

### The layout changes; the renderer barely does

Monochrome uses zgame's two stacked 640x240 pages and a toroidal
camera. That spare width is what an incremental scroll needed. Since
parallax ([below](#parallax-deleted-the-damage-tracking)) the whole
frame is redrawn every frame anyway, so nothing needs the slack, and
colour spends it on the second bitplane instead (`z_color_4_below`):

```
  (0,0)    page 0, plane 0      (320,0)    page 1, plane 0
  (0,240)  page 0, plane 1      (320,240)  page 1, plane 1
```

Still double-buffered: draw into one 320x240 page and its plane, flip
to it, draw the other. Scrolling becomes plain subtraction --
framebuffer x is page x + (world x - camera) -- in `world_to_fb_x()`,
the one place the renderer converts coordinates, so the tile walk, the
ambient layer, the actors and the HUD did not change.

What the layout costs is slack. A page is now exactly the viewport, so
a sprite half off the left edge would land in the OTHER page, the one
on screen. Every colour primitive clips to the page being drawn, which
the monochrome path never needed to.

The flip is `col_flip()`: point the viewport at the page just drawn,
wait for the boundary, return the frames elapsed -- what
`z_game_flip()` does for the monochrome layout, so `dt` and the
stepped world are unchanged.

### Colour per object, from 1bpp art

The art did not change. Colour is assigned per object:

| Object | Where the bitmap is set | Where it is clear |
|---|---|---|
| ground, box | dark brown | cream (pebbles, planks) |
| grass | green (rows 0-4: blades, turf) | transparent, then earth as ground |
| cheese | cream | transparent |
| sub-surface | dark brown with a cream dither | -- |

| Sprite | Data | Outline (mask only) |
|---|---|---|
| mouse | cream | dark brown |
| cats, birds | dark brown | dark brown |
| trees | green | dark brown |
| clouds | cream | sky (none) |

A two-colour tile is, per plane, a fill (both colours have the same
bit), a copy (only the foreground has it) or a fill with the bitmap cut
out (only the background has it). A sprite is, per plane, the mask cut
out and then the mask or the data put back as that plane's bits of the
two colours say. One to three blitter passes per plane per object, two
planes: roughly two to four times the monochrome blit count, which the
HUD's `b` figure shows directly.

The sub-surface stays a run-length fill, not a blit per tile: plane 0
is filled solid and plane 1 gets the screen-aligned dithered fill, so
it is dark brown speckled with cream at the same cost as before.

The HUD text is drawn into plane 0 only, so it shows dark brown over
the sky -- and at night into plane 1 as well, so it shows cream.

### Colour, and what a frame costs

A frame's cost here is its blitter operations: each is register writes
and a wait, and the game gets a share of the CPU to issue them in. The
host render counts them (`make render`, `themes` prints them per level,
and where they go -- `MAUSRUN_PROF`). Colour drew about three times
mono's:

| level 1, a frame | blits | far layer | tiles | the rest |
|---|---|---|---|---|
| colour, round 4 | 259 | 105 | 110 | 44 |
| mono, round 4 | 78 | | | |
| colour, round 5 | **94** | 32 | 23 | 39 |
| mono, round 5 | **34** | | | |

Three changes, the pictures identical (compared pixel for pixel with
round 4's on every level):

- **Runs of tiles.** Every tile also exists as a strip of 21 copies
  (`strips_init()`); a run of the same tile in a row -- the ground --
  is drawn from it in one blit a band and plane, not one a tile.
  Twenty grass tiles were a hundred blits in colour and are five.
  Mono gains the same way.
- **Whole far sprites.** A cloud is one 32x16 draw and a tree or a
  house one 16x32 (`far_init()`), not two cells.
- **The fewest passes a plane.** A sprite whose colour and outline
  share a plane's bit takes one pass on it (OR or ANDN the mask), and
  one with no outline -- its mask is its data, clouds and trees -- one
  pass a plane whatever its colour (`c_sprite_wh()`).

So colour now costs less than mono did. Build with `-DMAUSRUN_DEBUG`
to see the blits and the frames per update on the console.

### Palette effects

The palette changes the colour of everything on screen at once for
four register writes a frame and no blits, however much of the picture
it touches. That is the one thing blitting cannot do, so the demo leans
on it. All of it is written once a frame, straight after the flip's
frame boundary (`fx_tick()`), so no change lands half way down a
picture.

**Time of day.** The four colours drift through day, sunset, night and
dawn on a two-and-a-half-minute loop (it was one minute: the night came
round too often), interpolated a channel at a time between keyframes.
Everything relights together -- sky, clouds, cheese, the mouse --
because they share entries, which is exactly how light behaves.

| | sky | dark | green | cream |
|---|---|---|---|---|
| day | `6be` | `320` | `4a3` | `fe8` |
| sunset | `e85` | `312` | `372` | `fcb` |
| night | `114` | `001` | `132` | `9ac` |
| dawn | `a8c` | `211` | `483` | `fdc` |

**Stars**, at night only: a 320x112 bitmap made once at startup, drawn
in cream over the sky with `z_color_blit_mono()` (transparent, so the
clouds drawn next cover it) on the farthest parallax layer. Four blits
a frame at night, none by day.

**Lightning**, at night, now and then: a bright-dark-dark-dark-bright
flash of the sky entry, with a thud.

**Waterfalls.** The pits are falling water now. Each pit run is two
8x8 pattern fills, one per plane, so each pixel picks one of the four
entries: neither plane is the sky's own colour (the water), plane 0
alone dark (shadow streaks), both cream (foam). Both patterns move down
a row a frame. This one is the blitter rather than the palette, but it
is drawn in palette entries, so it relights with the time of day like
everything else -- orange at sunset, moonlit at night. Pattern fills
are anchored to the screen's 8-pixel grid, so adjacent columns join
seamlessly, and both pages (x = 0 and 320) sit on that grid.

**Events.** Cheese flashes the sky gold, fading over eight frames. A
death tints the whole palette red, fading over two seconds. Game over
drains it to grey (luminance). Winning cycles the sky through a
twelve-colour rainbow every four frames -- classic colour cycling.

**What four shared colours cannot do.** Every effect above is global,
because every entry is shared: the sky cannot fade on its own without
the water fading too. Independent colour cycling -- water shimmering
while the sky holds, a lava pit glowing on its own -- needs entries that
belong to nothing else, which means 8 or 16 colours, which means giving
up the back buffer (docs/color.md). A sky gradient needs the palette to
change part way down the frame, which the CPU cannot time. A
"palette split" in hardware -- a second set of entries taking over at a
programmable scanline -- would give both cheaply. It is not built.

## The frame

```
1. read input (pad and keyboard, merged)
2. step the world
3. repair the back page where sprites were TWO frames ago
4. draw newly scrolled-in columns into the back page
5. draw sprites into the back page
6. flip
```

Steps 3 and 4 are both "draw background tiles over a world-space
rectangle", so they are one function, `draw_tiles_rect()`.

Step 3 is what makes double buffering work with incremental redraw, and
it is the part that is easy to get wrong. The back page still holds the
sprites drawn into it the *last* time it was the back page — two frames
ago — and nothing else will erase them. Damage is tracked **per page**
for exactly this reason: the marks a page must clean up are its own from
two frames back, not the other page's from one frame back. Tracking them
globally leaves a trail of sprite ghosts on alternate frames, which
reads as a flicker rather than as a bookkeeping mistake.

## Ambient layer: parallax clouds, trees and birds

10 clouds, 10 trees and 12 birds, all 16x16 cells (a cloud is two side
by side, a tree two stacked). Roughly **104 blits per frame**, most of
them masked sprites, which is the point — the demo exists to push the
blitter.

Clouds move at a quarter of the camera, trees at three quarters, so
trees read as "just behind the action" and clouds as "far away". Both
shifts are powers of two: picorv32 has no divider unless `CPU_DIV` is
built, and this runs per sprite per frame.

Placement comes from a small fixed-seed LCG rather than `z_rand()` so
the demo looks the same every run — a visual regression then shows up
as one.

### Parallax deleted the damage tracking

This file used to track, per page, where sprites had been drawn two
frames ago and repair those rectangles. That was correct and necessary
while the background scrolled incrementally.

Parallax killed the premise. A cloud at quarter speed exposes background
that the tilemap walk has no reason to think is dirty. And at 30-plus
ambient sprites, repairing each one would touch most of the screen
anyway.

So the whole mechanism went and **the renderer got simpler**, not more
complex:

```
fill the sky
draw clouds and trees
draw the NON-EMPTY tiles over them
draw birds, cats, player, HUD
```

Empty tiles are skipped rather than drawn black — that is the entire
reason the sky is one fill instead of 300 empty tile blits. Blitting an
empty tile would erase the clouds already drawn there. It is also
cheaper: one ~9,600 cycle fill plus the non-empty tiles beats a
300-tile walk on a mostly-sky screen.

Nothing needs to remember anything between frames now. No mark lists, no
repair pass, no "which page was this drawn into two frames ago".

### Two bugs the ambient layer introduced

**Sprites repeating horizontally after scrolling.** The sky fill was
placed at the page origin, but the viewport sits at `fold(cam)` and
walks the whole 640-column page as the camera scrolls. Filling columns
0..319 forever means that once the camera passes column 320 the visible
half is never cleared, so sprites drawn there in earlier frames stay
put.

Checked exhaustively afterwards: the old fill left uncleared viewport
columns at **2044 of 2048** camera positions; the fix leaves none. It
self-corrected whenever the camera wrapped back into the region that
*was* being cleared, which is what made it look intermittent rather than
simply broken.

The fill is now two rectangles when the visible span crosses the seam
and one otherwise — the same shape as everything else that lives on a
torus.

**Movement got slower.** The world was stepped once per loop iteration
rather than once per display frame. When the ambient layer pushed some
frames over budget, the loop still advanced the player by one step, but
that step now covered two frames of wall clock.

This reads as "the game got slower" rather than "the frame rate
dropped", because at 30fps the animation is still perfectly smooth —
just at half speed. The world is now stepped `dt` times, using the frame
count `z_game_flip()` already returns.

Stepping N times rather than scaling the velocities keeps collision
exact: a doubled velocity could tunnel through a tile that two single
steps would land on. Capped at 4, so a long stall loses time rather than
teleporting the player through a wall.

### The pit bottoms were owned by nothing

Two reported symptoms, one bug: the player falling down a pit left a
copy of itself on every frame — reading as "falling forever" — and the
accumulated leftovers showed as a solid block at the top of each pit.

The sky fill had been trimmed to stop at `GD_GROUND_ROW`, on the
reasoning that every row below is covered by opaque ground tiles anyway.
True everywhere **except a pit**: pit columns have no tiles in the
ground rows, and `draw_subsurface()` deliberately skips them.

So in a pit column, y 176..239 — the bottom 64 pixels — was written by
nothing at all. Whatever was last drawn there stayed forever.

The saving was about a third of one fill per frame. Not worth a region
of the screen that nothing owns.

The general shape of the mistake is worth remembering: **"something else
covers this" is only safe if it is true for every column**, and the
exception here was the one feature the level is built around.

### The frame rate is the smoothness

`dt` and the blit count were on the HUD on purpose while this was a
demonstration; the HUD is the player's now, and building with
`-DMAUSRUN_DEBUG` (`make CFLAGS_EXTRA=-DMAUSRUN_DEBUG`, or add it to the
Makefile) prints both on the console once a second instead.

Motion smoothness **is** the frame rate. There is no amount of
interpolation that makes 12 distinct positions a second look like 60. So
when movement looks wrong there is exactly one useful question — how
many display frames is one update covering — and that is `dt`.

- `dt 1` — keeping up
- `dt 2` — half rate
- `dt 4+` — starved

Stepping the world `dt` times makes the *speed* correct under starvation
but converts the symptom from slow motion into jumpiness. Those are two
views of the same thing and neither is fixable in the game loop: at
`dt 4` you get 15 positions a second whatever the code does. The fix is
always to bring `dt` back to 1.

### It is the CPU share, and the blit count is the lever

`ps` on hardware shows every process in `run`, none with
`Z_PROC_FLAG_BLOCKED` (0x4) set — `wm`, `net` and `repl` all poll rather
than block. With four runnable processes the game gets about a quarter
of the CPU, and the observed `dt` of 4-5 matches that almost exactly.

That is worth stating plainly because it means **`dt` tracks the number
of runnable processes, not the amount of drawing**. Killing the other
apps makes the game fast; nothing in this file can.

What this app *can* control is how much CPU its frame needs, and that
scales with **blit count** — each blit is eight peripheral register
writes plus a spin in `gpu_blit_acquire()`. So the count is the lever:

| | blits/frame |
|---|---|
| tile-per-cell background | 86 |
| sub-surface as run-length fills | **37** |

Rows 12-14 were ~63 identical tile blits a frame, spent drawing the same
black rectangle repeatedly. They are now one fill per contiguous run of
ground — this level has six pits, so a screen shows one or two runs.

The cost is that the sub-surface is flat instead of textured. **A
screen-aligned patterned fill gives that back for free**, which is the
strongest practical argument for building them: patterns are not a
3D-shading feature, they are how you draw a textured background without
a blit per tile.

### The decisive experiment

The `A` key used to toggle the ambient layer, as a diagnostic: if blits
drop sharply and `dt` does not move, the drawing was never the
bottleneck and the answer is in the scheduler. It is gone from the game;
the same experiment is a one-line change to `draw_world()` in a
`MAUSRUN_DEBUG` build.

### Why it is probably not the drawing

This app's own CPU time is mostly **spinning on the blitter** —
`gpu_blit_acquire()` waits for the previous operation, so wall time and
CPU time are close to the same thing here.

Killing `wm`, `net` and `repl` makes the game noticeably faster, which
says the competition for CPU matters more than the drawing does. Those
processes poll in their main loops, so a game sharing the machine with
them gets a fraction of the CPU rather than the idle remainder.

Two consequences worth knowing before optimising anything:

- reducing blit count helps less than it looks, because the blits are
  not what the frame is waiting on
- the numbers in the "Cost" section below are from a cycle-accurate
  simulation with ideal memories, and hardware evidently disagrees with
  them by a wide margin. Trust the HUD over the table.

The blit count is on the HUD alongside `dt` so the two can be compared
directly: if blits drop and `dt` does not, the drawing was never the
problem.

### Cost

About 28,000 cycles a frame, **3.6% of the 800,000 available** at
48MHz/60fps. A single-pass cookie-cut mode would take it to roughly 3.0%.

There is room for several times this many sprites.

## Blits are asynchronous

Tiles and sprites are started and not waited for; the next blit's
`gpu_blit_acquire()` waits for the previous one as a side effect. See
`docs/gpu_blitter.md`.

Two barriers matter, and both guard failures that would show up rarely
rather than in testing:

- `z_fb_hw_sync()` before `z_game_flip()` — the viewport origin is
  adopted at the next frame boundary, and a blit that has not landed by
  then means a frame one blit short
- `z_fb_hw_sync()` before the software sprite fallback, which reads VRAM
  with the CPU to composite and would otherwise see a half-drawn
  background

Roughly 19 blits per frame, one sync per frame.

## Sprites are drawn in hardware

Single-pass where the bitstream supports it (400 cycles for 16x16),
two-pass ANDN-then-OR where it does not (544), software where the
blitter has no raster ops at all. `z_fb_hw_blit_sprite()` picks; this
app does not choose.

The single-pass path is also what makes `z_fb_hw_blit_sprite_async()`
genuinely async — the two-pass version could only ever leave the second
pass running. See `docs/gpu_blitter.md`.


`z_fb_hw_blit_sprite()` — two blitter passes, ANDN the mask then OR the
data. See `docs/gpu_blitter.md` for the raster ops themselves.

Two things did **not** move to hardware:

**Mirroring.** The blitter has no reverse mode. Rather than mirror at
runtime into a temporary bitmap every frame — which would defeat the
point of moving the blit off the CPU — `gen_sprites.py` emits both
facings and the draw code picks a pointer. Four extra 32-byte bitmaps
per sprite: a little more ROM for a blit that costs the CPU nothing.

**Wrapping at the torus seam.** `z_fb_hw_blit_mem()` clips to the
screen, so a sprite straddling framebuffer column 639 would lose its
right half. The fix is the same one the software path used — draw it
twice, once at each end — which is two extra blits on the rare frames it
matters and no special case in the hardware.

The software path below is kept, and is still reached on a bitstream
whose blitter predates raster ops.

## Why the software fallback still exists

`gpu_blit.v`'s copy mode overwrites the destination rectangle wholesale;
it has no raster op. A mouse blitted over a brick wall would arrive
inside an opaque 16x16 box of its own background.

So sprites carry a **mask plane** alongside their data and are
composited in C as `(dst & ~mask) | data`. Tiles have no such problem —
every pixel of a tile cell belongs to that tile — so they go through the
hardware blitter.

That split is the right one rather than a compromise: hundreds of tiles
per frame take the fast hardware path, a handful of sprites take the
slow one. If `gpu_blit.v` ever grows an OR/mask raster op (see
`docs/game_mode.md`), `spr_draw()` becomes a hardware call and nothing
else changes.

Both paths clip against the **page**, not the framebuffer. Drawing
past the bottom of a 640x240 page would spill into the other page, which
is on screen. That is a hazard specific to this layout and not something
`zgfx.h`'s screen clipping would catch, since as far as it is concerned
the whole 640x480 surface is fair game.

The software path mirrors by reversing each row's bits — three
shift-mask steps — which is why it never needed a second copy of the
art. The hardware path cannot, so the sheet carries both facings.

## Art

The art is two sprite sheets ([zsp.md](zsp.md)):
`sw/data/mausrun/tiles.zsp` (29 tiles, 2 colours: 1 the tile, 0 its
background) and `sprites.zsp` (25 sprites, 4 colours: 1 the sprite, 2
its outline -- drawn in the outline colour, black in monochrome -- and
`.` see-through). The colours come from the level's theme, as before.
A tile's id is its place in `tiles.zsp` -- level maps store ids -- so a
new tile goes at the end. pix edits both ([pix_app.md](pix_app.md)).

`gen_sprites.py` makes `sprites.c` / `sprites.h` from the sheets: run
it after editing one. The art moved out of the script into the sheets
without changing a byte -- `gen_sprites.py --legacy` writes the
`sprites.c` and `sprites.h` of before, byte for byte. Without it the
arrays are not `const` and a table, `mr_art[]`, names each one, which
is what the card's sheets are laid over:

**At start, mausrun reads `/data/mausrun/tiles.zsp` and
`sprites.zsp`** if the card has them (`art.c`): a tile or sprite there
replaces the built-in one of the same name -- its software and
hardware forms, mirrored too -- before anything copies the art. So art
drawn in pix shows the next time mausrun starts, with no host
computer. A sheet that does not parse is reported and the built-in art
used; a name the game has no art for, or a size other than 16x16, is
skipped and said:

```
mausrun: /data/mausrun/sprites.zsp: 25 of 25 sprites from the card
```

A sheet with more colours is seen through its 4-colour form (its
slots): 0 and 2 mask only, the others the sprite.

The release card carries the two sheets in `/data/mausrun/art`, where
the game does not look: open one in pix, change it, and **save it as
`/data/mausrun/sprites.zsp`** (or `tiles.zsp`). A copy there is yours
and wins; the release's own art changes still arrive, except for the
tiles and sprites your copy has. Delete it to have the built-in art
back.

The generated files are **committed**, so a normal build needs no
Python. There is deliberately no make rule regenerating them: a
generated file that rebuilds itself mid-build is how a tree ends up
differing from what was committed without anyone noticing.

Bit order is **LSB-first** — pixel x at bit `x & 31` of word `x >> 5` —
which is the framebuffer's own convention (`zgfx.h`) and *not* the
MSB-first order `z_font_t` glyphs use. Getting this backwards mirrors
every sprite within each 16-pixel group, which looks like a shuffled
sprite sheet rather than a bit-order bug.

## Levels

Text files, `sw/data/mausrun/levels/01.txt`, `02.txt`, ..., copied to
`/data/mausrun/levels` on the card (`Z_DIR_MAUSRUN_LEVELS`), by the card
image and by `update`. **Every one of them is also compiled in**
(`levels_builtin.c`, generated by `gen_levels.py` and committed, as
`sprites.c` is), so the game has all its levels on any card. A file on
the card wins over the compiled-in level of its number, and files past
the last compiled-in one add levels; one that does not parse is
reported on the console and the compiled-in level played instead.
`levelcheck` checks every compiled-in level, and that each file says
what its compiled-in copy says -- an edited level without
`python3 gen_levels.py` fails the tests.

Until games round 5 only level 1 was compiled in, and the level files
were in the card's list of inputs but not in the list of what to write
(`release/lib/mkfatimg.py`), so neither an image nor an `update` put
them on the card: the game found one level, and finishing it was "YOU
DID IT! EVERY LEVEL". The image test (`release/lib/test_mkfatimg.py`,
"all listed apps and data files on the card") catches exactly that; it
needs mtools and dosfstools to run.

```
# mausrun -- a level
name Over the Hills        shown and spoken when it starts
time day                   day, night, or cycle (the 2.5-minute day)
music 0                    which tune (without it, the theme's)
weather leaves             none, leaves, snow, rain or fireflies
theme meadow               meadow, garden, kitchen or roofs (the look and the tune)
map
................................................................
...                        15 rows, up to 512 columns
```

The map is ASCII, so the level is its own picture:

```
.  sky      =  grass floor   #  ground fill
B  box      c  cheese        M  mouse start
S  spring   -  shelf         K  biscuit       ^  thorns
~  water    ?  ? box         H  mouse hole    G  cheese wheel
w  feather  b  bubble        m  magnet        l  bell
k  fast shoes                h  heart
*  the hidden star           P  a moving plank's track
C  cat      Z  sleeping cat  F  frog          O  owl
```

Rows may be different lengths (short ones are sky to the end); the map
is 24 to 512 columns wide; up to 24 foes. Rows 12 to 14 are drawn as
earth where they are ground, so a pond can go down into them. The parser (`level.c`) is a
pure function of the text, shared with the host tools, and says what is
wrong and on which line.

There is **no pit tile**. A pit is a run of columns with no floor, and
falling out of the bottom of the map is the failure condition — so a
hole in the floor is a pit by construction. The obvious alternative, a
pit tile you collide with, would need its own case in the physics for no
gain. The level is finished at its cheese wheel, or, without one, by
running into its last two columns.

Level 1, **The Meadow**, is the original map, kept as everyone learned
it -- with one cheese moved one column left: `levelcheck` found that the
one at row 2, column 74 could never be reached by any jump. Round 3
only added things to find to it: two ? boxes, three mouse holes, the
cheese wheel and falling leaves, and round 4 its hidden star; the
ground, the boxes, the cheese and the cats are where they always were.
Level 2, **Over the Hills**, is
rolling hills in daylight with little pits, and since round 3 a spring,
shelves, a thorn bush, a sleeping cat, ? boxes and mouse holes. Level 4,
**The Garden at Night** (level 3 in round 3), has everything: a pond with frogs and lily
pads, a picnic of biscuits over a gap, a spring to a high shelf, rose
bushes, a sleeping cat, an owl over the boxes, and fireflies.

Round 4 made eight levels, two to a theme, every one with a hidden star
(levels 1 and 2 got theirs without anything else moving):

| | | theme | |
|---|---|---|---|
| 1 | The Meadow | meadow, the day's cycle | the original |
| 2 | Over the Hills | meadow, day | |
| 3 | The Vegetable Patch | garden, day | two planks over wide gaps, a pond, frogs, a sleeping cat in the cabbages |
| 4 | The Garden at Night | garden, night | round 3's level 3 |
| 5 | The Kitchen Floor | kitchen, day | mousetraps, cupboards, shelves up the wall, a biscuit bridge |
| 6 | Up on the Worktop | kitchen, night | a spring up, the sink and its frog, a tray to the high shelf |
| 7 | Over the Roofs | roofs, night, snow | gaps between houses, chimneys, owls, a plank |
| 8 | Chimney Tops | roofs, night, fireflies | planks, a sleeping cat on a roof, the last cheese wheel |

### levelcheck: can it be finished?

`tests/levelcheck.c` reads a level with the game's parser and searches
every way the mouse can move with the game's own physics (`phys.c`) --
left, right or neither, jumping or not, a frame at a time, breadth
first over the exact fixed-point state. A level passes when the search
reaches the goal (the cheese wheel, or the last two columns); any
cheese the mouse can never touch is an error too, and so is a hidden
star. Springs, shelves and
water are physics and so are in the search; thorns end a path, as a pit
does; a biscuit counts as solid (it is, until stood on); no power-up is
used -- every level must be finishable without one. It reports the
quickest finish:

```
01.txt: "The Meadow", 128 columns, 21 cheese, 3 foes: finished in 970 frames at best (16.2 s)
02.txt: "Over the Hills", 160 columns, 38 cheese, 6 foes: finished in 1210 frames at best (20.2 s)
03.txt: "The Vegetable Patch", 150 columns, 18 cheese, 5 foes: finished in 1130 frames at best (18.8 s)
04.txt: "The Garden at Night", 146 columns, 21 cheese, 6 foes: finished in 1098 frames at best (18.3 s)
05.txt: "The Kitchen Floor", 150 columns, 17 cheese, 5 foes: finished in 1130 frames at best (18.8 s)
06.txt: "Up on the Worktop", 150 columns, 21 cheese, 5 foes: finished in 1139 frames at best (19.0 s)
07.txt: "Over the Roofs", 150 columns, 19 cheese, 4 foes: finished in 1130 frames at best (18.8 s)
08.txt: "Chimney Tops", 160 columns, 20 cheese, 5 foes: finished in 1202 frames at best (20.0 s)
```

Foes are left out -- they move, and the mouse can wait for them, or
bounce on them. `make -C sw/apps/mausrun test` runs it over every level (and on a
level with a six-tile pit, which it must refuse); `make check` prints
the figures. `make render` draws each level as one long picture
(`tests/levelstrip.c`, `out/level_NN.png`).

## Tuning, and why the numbers are what they are

Gravity, jump velocity and run speed are tuned **together**, not
independently. With gravity 6 in 1/16-pixel units, a jump of -96 gives
32 frames of airtime, which at a run speed of 32 covers exactly **64px —
four tiles** — and reaches **48px — three tiles** — high.

The levels are built against those two numbers: no gap is wider than
three tiles and no step taller than two, so every jump has a tile of
margin. The constants are in `phys.h`; change any of them and
`levelcheck` rechecks every level with the new physics.

## Bugs worth recording

These are all recorded because each one presented as a *different* kind
of fault than it was.

**The machine froze on the first sound.** `Z_AUDIO_CH_BASE` was handed
an app pointer. The mixer is a bus master and does not go through the
MTU, so it requested a physical address nothing decodes; an undecoded
address on this bus never acks, and the mixer held its grant on
`arbiter_main` forever, starving the CPU. The whole machine stopped with
the last frame still on screen — which looks like a graphics or
scheduler bug and is neither. Fixed with `phys_of()`, the same
translation `sw/apps/track` uses.

**Every odd tile row repeated the row above it.** `gpu_blit.v` walks the
source with `mem_row_addr += src_stride` in *byte* addresses but issues
*word* reads that ignore the low two bits, so a 2-byte stride resolves
rows 0 and 1 to the same word. Tiles are now `uint32_t` per row with
`GD_TILE_STRIDE 4`. This never hung anything; it just rendered subtly
wrong, which is why it survived a first look at the screen.

**The audio beeped at random, unconnected to events.** The mixer plays
**8-bit signed** samples (it was built for ProTracker MODs) and `CH_LEN`
is a length in bytes. The buffers were `int16_t`, so the mixer read them
as twice as many bytes and played the interleaved high and low halves of
each sample as consecutive values. Loud, tuneless, and only loosely
correlated with the trigger — which reads as "the audio isn't tied to
anything" rather than as a format mismatch.

**No sound at all.** `z_audio_start_hz()` is what sets
`Z_AUDIO_CTRL_EN`, and without `EN` the DAC is muted no matter how many
channels the mixer is happily summing. An earlier version dropped the
call in order to avoid overriding the board's sample rate, and the
result looked like a board with no audio hardware. The fix reads the
rate back and writes it straight out again — enable the output, do not
second-guess the rate, which matters because an S/PDIF board needs
divider 16 and a constant would break exactly those boards.

**The character vanished at a few places on the map.** `spr_draw()`
bounds-checked the second framebuffer word as `wordi + 1 <
FB_STRIDE_WORDS` and dropped it. But the page is a 640-column torus, so
a sprite across column 639 continues at column 0 of the same row. The
seam sits at a fixed framebuffer column while the world scrolls past it,
so the symptom is a character that disappears at a few specific *map*
positions — which reads as a level data bug rather than a rendering one.

**The mouse ran with its eyes toward its own rear.** The art faced left
while `spr_draw()` treats unmirrored as facing right. Invisible in the
ASCII, which reads fine either way round, and only visible on screen.

**Space only jumped sometimes.** See "Input", below. This one is worth
its own section because the cause is architectural rather than a typo.

## Two more bugs worth recording

**Jumps silently failing about half the time.** `on_ground` was inferred
from whether the vertical move was blocked. That looks right and is
subtly wrong: an actor resting on a floor has `vy` driven to 0 by
gravity-then-collision, so on a frame where it does not actually move
there is no blocked move to infer from, and the flag clears. The result
was a character that was grounded on alternate frames while standing
perfectly still, so a jump pressed on the wrong frame did nothing.

It reads as unresponsive controls rather than as a state bug, which is
exactly why `on_ground` is now **probed** — is there something solid
immediately under the feet? — rather than inferred.

**An unjumpable first pit.** The original level opened with a six-tile
gap against a jump that covered three. Found by running the game
headless with an autoplay driver, not by playing it.

## Input, and why the keyboard is read as a level

The game does **not** call `hid_read_key()`.

That ring is popped, and `wm` drains it too. Every event this process
takes is one `wm` never sees, and vice versa — the two race for one
queue and each gets roughly the half the other did not.

For a game that is broken in a specific and maddening way. Miss a
**press** and the key never registers, which is annoying. Miss a
**release** and the key is stuck down forever — so the jump edge never
fires again and jumping stops working entirely, until some later
press/release pair happens to arrive intact. "Space only jumps
sometimes" is exactly that, and it looks like a lost interrupt or a full
FIFO, which it is not.

So `kbd_held()` reads `rtl/usb_hid.v`'s report registers directly. Those
hold the **current** state — up to four held keys plus the modifier
byte, overwritten in place by hardware on every report. It is a level,
readable as often as you like, and reading it consumes nothing. `wm`
keeps every event; the game reads the same hardware `wm`'s kernel-side
ISR reads.

That is also the right shape for a game independent of the sharing
problem: a game asks "is this key down right now", and rebuilding a
level out of an edge stream is inventing state the hardware already has.
It also makes unplugging a keyboard mid-hold read as released, for free.

The one thing it cannot see is a key pressed *and* released entirely
between two polls. At 60Hz that is a 16ms keystroke.

Note this does **not** fix the underlying sharing problem — `wm` still
receives the game's keystrokes and acts on them. A full-screen app
genuinely wants exclusive input, and the right fix is an OS-level grab,
which is a kernel change.

## Giving the framebuffer back

On exit the game sends `Z_WM_REPAINT` (`sw/common/zwm.h`) to `wm`.

Every window is still alive, still owned and still exactly where it was
— but the game drew over all of their pixels, and none of them know that
happened. `wm` repaints damage it caused itself; this damage came from
outside, so it has to be told. Without it you return to a desktop that
is fine underneath and garbage on screen.

`Z_WM_REPAINT` takes no rectangle deliberately. An app that overwrote
the framebuffer generally cannot say what it damaged — a scrolling game
touches every pixel over a few frames — and a wrong rectangle would
leave debris that looks exactly like this bug not being fixed.

The game also silences all four mixer channels before exiting: a channel
left enabled would keep fetching from a buffer about to be freed with
the process.

## Testing

```
make -C sw/apps/mausrun test      # the parser, progress.txt, the art, levelcheck
make -C sw/apps/mausrun render    # every level as a picture, and the game played
```

`tests/test_level.c` reads level 1 and small levels written for it --
names, the time, CR LF lines, too few rows, too narrow, no start, a
character that means nothing, 512 columns and 513, more cats than there
is room for -- and writes and reads `progress.txt`, keeping lines it
does not know. `tests/test_art.c` reads the two sheets, writes them back
byte for byte, lays them over the art (which must change nothing), then
a changed pixel and an outline over the mouse -- in all six forms, the
mirrored ones too -- and a name the game has no art for. `make sheets`
(part of `test`) regenerates `sprites.c` and `sprites.h` from the sheets
and fails if they differ from the committed ones. `levelcheck` (above)
proves every level can be finished.

`make render` also plays the game itself on the build host
(`tests/host/`): `mausrun.c` unchanged, compiled with its frame waits
pointed at the host (`mr_wrap_game.c`), against a software machine
(`mr_host.c`) -- the SOC register pages it reads directly (the USB
keyboard report, the game-mode view and frame counter, the colour
control) mapped at their real addresses, a 640x480 framebuffer with the
blitter's fills, patterns, shades, copies and raster ops, the real
`zcolor.c` and `zgame.c`, files from the tree. A script and a small
autopilot (`render.c`: jump at edges and walls, over cats where it can
land, back off a cat in front of a pit) take it through the title and
its skill line, level 1 to WELL DONE!, level 2, the hearts lost one by
one to GAME OVER, and back to a title that offers CONTINUE: LEVEL 2;
every screen is written to `out/` as the display would show it, in
colour and in monochrome. A second run, `render_mausrun out garden`,
starts from a `progress.txt` that has level 4 unlocked, swims the pond
(and shows it in monochrome), takes a feather, a magnet and a bubble,
passes the sleeping cat and the owl, comes down on a cat (dizzy stars)
and finishes the level: its stars counted up, and on to level 5. A
third, `render_mausrun out themes`, plays a few seconds of each of the
eight levels and photographs them, drops the mouse onto each level's
first plank and checks that the plank carries it (the run fails if
not), and ends on a title showing the stars. Without ASan: its shadow
memory covers the register addresses. `tests/test_sound.c` reads every
cell of every tune with zsfx's own parser.

## Sound

On the arcade games' sound engine, zsfx ([sfx.md](sfx.md)), since round
3: `sound.c` has mausrun's effects as zsfx programs (jump, land, coin,
ouch, fall, spring, stomp, power-up, pop, splash, mouse hole, cheese
wheel, heart, ? box, crumble, croak, hoot, wake, the menu's and the
results' ticks), panned to where they happen, and a tune for each
theme: **Meadow**, the tune the game always had, as a zsfx score (its
drums now zsfx's kick, snare and hat), and since round 4 **Garden**,
**Kitchen** and **Roofs** -- all four laid out tune, harmony, bass,
drums, sixteen rows a pattern and four patterns. They play calm first:
tune and bass only, slow, on the title and level 1 on easy, a step
fuller every two levels and with the fast shoes ([Skill](#skill)).

`-` and `=` are zsfx's master volume, music and effects together; M
turns the music off and on in time; pause holds every channel. On a
board without the mixer every call does nothing and the game plays
silently.

mausrun's own `music.c` -- four channels of music and four of effects,
64-byte single-cycle instruments, per-frame envelopes, the score as
ASCII -- is gone: zsfx was made general from it (its lessons are in
[sfx.md](sfx.md): the music kept quiet against one-channel effects, the
read-modify-write of the mixer's scale, the tempo from the frame rate).

## Trees stood in the floor

A tree is **two** 16px cells — `TREE_TOP` above `TREE_BOT` — so its base
is at `y + 2*GD_SPR_H`, not `y + GD_SPR_H`. Placing it at
`floor - GD_SPR_H` buried the entire lower cell below the floor surface.

And the floor is not the tile boundary: a grass tile is three rows of
blades before its turf, and `TREE_BOT`'s last row is empty, so trees
ending at the boundary floated five pixels above the turf.
`GD_TREE_SINK` (5) lowers them onto it; the blades, drawn after the
ambient layer, stand in front of the trunk.

That only works if the blades are **transparent**, and tiles were
opaque: every tile was a COPY, so the black (or, in colour, sky-blue)
between the blades blanked the foot of the tree behind them, and a
cheese floating in front of a cloud or a tree arrived in its own
sky-coloured box. Tiles are sky-free solid ground everywhere else, so
the fix is narrow: where a tile's background is "nothing here", it is
drawn with a raster op instead of copied.

| | Monochrome | Colour |
|---|---|---|
| grass, blade rows | OR (rows 0-2), then the turf copied | `c_over()`: per plane OR where the colour has the bit, ANDN where not |
| cheese | OR | `c_over()` |
| ground, box | copy | opaque fg-on-bg, as before |

Both cost no more than the copy they replace: one blitter pass, or one
per plane, touching only the bitmap's own pixels. Without raster ops
every tile is a copy, as it always was.

Now `GD_GROUND_ROW * GD_TILE_H - 2 * GD_SPR_H`, written in terms of the
constants rather than the literal 176, so it follows if the ground row
ever moves.

## History

`gamedemo` until 0.0.7, when it became `mausrun` (the games proposal,
round 2): the name everywhere (the app, the dock and its spoken name,
the card, the docs), the debug line and keys out, the screens, the
status line, levels from files, progress, skill, and `levelcheck`. A
card updated with `update` loses `/apps/gamedemo` by itself: it is gone
from the release.

Round 3: the springs, shelves, biscuits, thorns, water, ? boxes, mouse
holes and the cheese wheel; sleeping cats, frogs and owls, and bouncing
on all of them; hearts lost without starting over; the power-ups; the
particles and the weather; level 3; and the sound moved onto zsfx.

Round 4: the themes (garden, kitchen and roofs beside the meadow: their
colours, far layers, tiles and tunes), moving planks, the three stars
and the hidden star, five new levels (eight in all), and a new dock
icon -- the mouse and its cheese.

Round 5 (fixes after a first play): every level compiled in and the
level files on the card at last ([Levels](#levels)); the day 2.5
minutes long; colour drawn in a third of the blits ([Colour, and what a
frame costs](#colour-and-what-a-frame-costs)); and wm no longer paints
its dock over the game three seconds in (docs/window_manager.md).
