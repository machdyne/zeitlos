# gamedemo

A side-scrolling run-and-jump demo for game mode. You are a mouse; you
collect cheese, jump over boxes and pits, and avoid cats.

The level is 2048 pixels long against a 320 pixel viewport. That is the
entire reason this exists — it exercises the scrolling machinery rather
than fitting neatly on one screen.

Lives in `sw/apps/gamedemo/`, with its art, generator and level all in
that directory. Nothing it needs has been added to `sw/common` beyond
`zgame.[ch]`, which is general.

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
| Hardware mixer sound effects | `zaudio.h`, if the board has one |

## Controls

Arrow keys or a gamepad, both live at once. There is no mode to select
and no mode to get stuck in: the keyboard and pad are read every frame
and OR'd together, so a machine with no pad is playable and a machine
with one still responds to the keyboard.

| Action | Keyboard | Pad |
|---|---|---|
| Move | left / right | d-pad |
| Jump | up or space | A, B or up |
| Restart | R | start |
| Quit | Esc | select |

Unplugging a pad mid-jump releases every button rather than leaving the
mouse running forever — that falls out of the hardware clearing pad
state on disconnect (`docs/gamepad.md`), and needs no code here.

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

### The frame rate is the smoothness, and the HUD shows it

`dt` and the blit count are on the HUD on purpose.

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

`A` toggles the ambient layer. It is a diagnostic, not a feature.

Watch the HUD: if blits drop sharply and `dt` does not move, the drawing
was never the bottleneck and the answer is in the scheduler.

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

`gen_sprites.py` generates `sprites.c` / `sprites.h` from ASCII art in
the script. Placeholder art; replacing it means editing the strings and
re-running, not touching any C.

The generated files are **committed**, so a normal build needs no
Python. There is deliberately no make rule regenerating them: a
generated file that rebuilds itself mid-build is how a tree ends up
differing from what was committed without anyone noticing.

Bit order is **LSB-first** — pixel x at bit `x & 31` of word `x >> 5` —
which is the framebuffer's own convention (`zgfx.h`) and *not* the
MSB-first order `z_font_t` glyphs use. Getting this backwards mirrors
every sprite within each 16-pixel group, which looks like a shuffled
sprite sheet rather than a bit-order bug.

## The level

`level.c`, as ASCII, so the level is its own picture:

```
.  sky      =  grass floor   #  ground fill
B  box      c  cheese
M  mouse start             C  cat start
```

There is **no pit tile**. A pit is a run of columns with no floor, and
falling out of the bottom of the map is the failure condition — so a
hole in the floor is a pit by construction. The obvious alternative, a
pit tile you collide with, would need its own case in the physics for no
gain.

## Tuning, and why the numbers are what they are

Gravity, jump velocity and run speed are tuned **together**, not
independently. With gravity 6 in 1/16-pixel units, a jump of -96 gives
32 frames of airtime, which at a run speed of 32 covers exactly **64px —
four tiles** — and reaches **48px — three tiles** — high.

The level is built against those two numbers: no gap is wider than three
tiles and no step taller than two, so every jump has a tile of margin.
Change any of the four constants and the level needs rechecking.

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

**No sound at all.** `z_audio_start()` is what sets
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

The game builds and runs on the host against stubbed hardware, driven by
an autoplay routine that runs right and jumps when the tile map says a
gap or wall is within reach. It reports how far it got:

```
RESULT frames=4001 max_x=2017 score=20 lives=3 won=1
```

That is the whole level traversed with lives intact, which is the
property worth checking automatically — a level nobody can finish is a
much easier mistake to make than a level that renders wrong, and much
harder to notice by eye.

The stubs also flag any blit or text draw that lands outside the
framebuffer, so the world-to-framebuffer folding is checked on every
frame of every run rather than by inspection.

Cats have no avoidance logic in the autoplay, so a full run with cats
enabled dies to one — correctly. `NOCATS=1` disables them for the
traversal check.

## Music and sound

`music.c` / `music.h`. Four channels of music plus four of effects, on
the eight the hardware mixer has. They never overlap, so a jump landing
during a bass note cannot cut the bass off — which is what sharing a
channel pool would do at exactly the busiest moment.

### The score is ASCII, like the level

```
/*   lead harm bass perc */
    "A-4 A-3 A-2 C-2",
    "--- --- --- ---",
    "C-5 C-4 --- C-6",
```

Top to bottom is time, left to right is the four channels. `---` holds,
`===` is note off. Percussion uses pitch to pick a drum, because its
instrument is noise and pitch is the only knob that matters: `C-2` kick,
`C-4` snare, `C-6` hat.

Sixteen rows per pattern, four patterns, 8 rows/second — so the loop is
eight seconds. Short on purpose: the failure mode of game music is not
being boring, it is being **annoying**, and a short loop that stays out
of the way beats a long one that demands attention.

### How a note becomes a sound

The mixer plays 8-bit samples and loops them. It has no oscillators and
no notion of pitch. So an instrument here is **one cycle of a waveform,
64 bytes**, set to loop forever — and playing it faster makes it a
higher note:

```
note frequency = playback rate / 64
```

Four instruments cost 256 bytes total and no runtime synthesis. A single
cycle also loops seamlessly by construction (its end joins its start),
so there is no click and no loop-point tuning — that is the property
that makes 64 bytes enough.

Instruments: 50% square (lead), 25% pulse (harmony), triangle (bass),
LFSR noise (percussion). The two pulse widths share a fundamental but
differ in harmonics, which is what makes them audibly different
instruments rather than one being a quieter copy of the other.

Measured pitch accuracy is within 0.02 Hz across six octaves.

### Music vs effects

The comparison that matters is an effect's gain against the **sum** of
the music gains, because music is four channels sounding together and an
effect is one.

The first version had music summing to 350 against an effect gain of
140 — so the music was two and a half times louder than any effect, and
every jump and coin vanished underneath it. Each instrument looked
reasonable next to 140 on its own, which is exactly how the mistake
survived being looked at.

| | music sum | effect | ratio |
|---|---|---|---|
| original | 350 | 140 | 0.4x (−8 dB) — effects buried |
| now | 85 | 255 | 3.0x (+10 dB) |

In practice better than the ratio suggests: music channels spend most of
their time part-way through an envelope decay, while an effect is always
heard from its own peak.

**The fix was to bring the music down, not push the effects up**, and
that was forced. One 8-bit channel simply cannot get loud on this mixer
— full scale takes roughly four channels at full gain, so a lone effect
tops out near 20% of full scale no matter what. There was no headroom to
push into.

`MUS_MUSIC_VOL_DEFAULT` and `MUS_SFX_VOL_DEFAULT` scale the two
independently at runtime; `[` and `]` move the music only. That is the
knob for this problem — the master moves both together and so cannot fix
a balance.

### Volume

`music_set_volume()` is **the** volume knob. It writes the mixer's own
output scale (`MIXVOL`), so it covers music and effects together:

```
out = clamp16((sum_of_channels * mixvol) >> 10)
```

The hardware resets it to 128 — `audio_mixer.v` calls that safe for all
eight channels, with 255 being what a 4-channel MOD wants. `music.h`'s
`MUS_VOL_DEFAULT` is 64, a straight 50% cut against the hardware
default. In game, `-` and `=` step it. `MUS_VOL_DEFAULT` is 255, and the level is
set by keeping the music gains low rather than by holding the master
down — see "Music vs effects" above for why there is no headroom to do
it the other way.

**Editing the per-channel gains in `music.c` instead is a wrong turn**,
and a convincing one. Those set the *balance* between the four
instruments; halving them leaves every jump, coin and landing exactly as
loud, because effects never pass through them — and those fire
constantly during play. The game barely gets quieter and the edit looks
like it did nothing.

The write is a **read-modify-write**, and that is not optional: bits
[11:8] of the same register hold the S/PDIF `fs_code`. A bare write
zeroes the sample-rate code and breaks digital output on every board
that has it, while sounding perfectly fine on the analogue board it was
tested on — the worst way for a bug to behave.

If turning the volume down genuinely does not get quieter, suspect the
**gateware**. `docs/audio.md` records an output-scaler bug whose exact
signature was *"turned the volume down and it's just as loud"*: a signed
part-select rectified the negative half of the waveform, so positive
half-cycles scaled with `MIXVOL` normally while negative ones railed
regardless. It is fixed in the current RTL, so a `make flash` is the
check.

### Envelopes

A looped waveform at constant gain is an organ, not a game. Each channel
carries a volume that decays every frame, written back with `EN` but
**not** `TRIG` — `docs/audio.md` is explicit that this changes gain
without restarting the sample, which is exactly what a volume-only
tracker row needs and the reason an envelope costs one register write.

Percussion is the same mechanism with a fast decay on the noise
instrument. No separate code path.

### Timing

Advanced from the game loop, once per frame, no interrupt and no thread.
`z_game_flip()` already returns the frames that actually elapsed, so a
frame the game overran advances the music by the right amount instead of
dragging the tempo down with the frame rate.

Rows per second derives from `z_video_frame_hz()`, so tempo is the same
on a 50Hz PAL composite board. Deriving it from a fixed frame count
would make every song 20% slow there — the classic version of this bug.

### Replacing the instruments with real samples

`ins_data[]` / `ins_len[]` are just pointers and lengths. Point them at
8-bit signed sample data of any length and the pitch maths still holds,
as long as the buffer is one cycle (or a whole number of cycles) of the
tone — that is what makes `rate = freq * len` correct. A multi-cycle
sample works too; divide its length by the number of cycles it contains.

Entirely optional throughout: `z_audio_mixer_present()` is false on
boards without `AUDIO_MIXER`, every call becomes a no-op, and the game
plays silently rather than not at all.

## Trees stood in the floor

A tree is **two** 16px cells — `TREE_TOP` above `TREE_BOT` — so its base
is at `y + 2*GD_SPR_H`, not `y + GD_SPR_H`. Placing it at
`floor - GD_SPR_H` buried the entire lower cell below the floor surface.

Now `GD_GROUND_ROW * GD_TILE_H - 2 * GD_SPR_H`, written in terms of the
constants rather than the literal 176, so it follows if the ground row
ever moves.
