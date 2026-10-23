# planedemo

**planedemo**: a one-minute demoscene film about Zeitlos, in 256
colours and eight voices, made for the Sergei ML2 (eight bitplanes,
the 16-bit hardware mixer). A clock whose hands fall off; the name
catching fire; a storm over a city at night, and a mouse running the
rooftops under a leaf; a glazed donut turning in 3D; a tunnel into the
machine; wireframe vectors over copper bars; sunrise with a rainbow and
a sine scroller; and the mouse, on the name, eating the donut. 128 BPM,
32 bars, about 63 seconds.

Lives in `sw/apps/planedemo/`. It is in the **Games** group of the dock
("Plane demo") and in the card's `games` set.

## Watching it

Start it from the dock, or `planedemo` in a terminal. It goes full
screen (game mode) by itself and plays once; **Esc** or **F11** (back
to the window) ends it, and so does the end of the film.

It is made for eight planes. On a board with six or four it plays in
64 or 16 colours, each scene choosing what to give up ("On fewer
planes", below); without game mode it plays in the window, in
monochrome; without the 16-bit mixer it plays silently.

On the console (the UART) it prints a line as each scene ends, from the
CPU's own counters (`rdcycle`, `rdinstret`):

```
planedemo: the donut: 450 frames, 0 dropped; frame 180 kcyc (slowest 260), ipc 0.17; drawn 90 kcyc (slowest 140), 0 late, 0 behind the beam; begin 140 kcyc; jobs 0 Mcyc
```

-- frames shown and dropped; the scene working a frame out, on average
and at its slowest, in thousands of cycles (a frame at 60 a second is
800 thousand of the 48 MHz CPU's); instructions retired a cycle; the
frame's drawing run at the boundary ("Racing the beam", below), on
average and at its slowest, the frames it began outside the blanking
(late: a dropped frame, the beam anywhere) and those in which a band of
rows was finished after the beam had reached it; its `begin()`; and the
work done behind it. The counters count the whole machine, so
a frame in which the scheduler ran someone else looks dearer, the
slowest most of all. A scene that can do less does while frames are
being dropped (`pd_late`): the vectors draw half the stars, the
scroller blits columns of eight pixels, not four.

The program is 3 MB, most of it the film's data (below), packed to
1.2 MB on the card (LZ4, which every loader unpacks:
[executables.md](executables.md)). The first r3 run took 15.3 s to
load: the loader read the packed file 256 bytes at a time. It reads 8KB
at a time now, and writes runs a word at a time.

## The story

The film is cut to the music: each scene owns a span of bars, and
everything happens on a row (a sixteenth note) of the song.

| Bars | Scene | File | What happens |
|---|---|---|---|
| 1-2 | the clock | `pd_s_clock.c` | A brass clock face ticks in the dark. On bar 2 the hands come loose and fall, bounce and lie still; the screen shakes. "zeitlos" is typed out, a letter a row, and glows; in the last moments the clock catches fire |
| 3-4 | ignition | `pd_s_plasma.c` | Out of the burning clock: the word stays, white hot, a fire plasma floods in behind it, brighter on every kick; the snare roll burns everything to white |
| 5-8 | the storm | `pd_s_city.c` | Rain in two depths over a city at night, far and near skylines in parallax, a cat on a chimney; lightning on the thunder, twice |
| 9-12 | the mouse | `pd_s_city.c` | mausrun's mouse, twice its size, runs the rooftops under a leaf, jumping the gaps; a last high leap, and the picture goes to white |
| 13-16 | the donut | `pd_s_donut.c` | A glazed, sprinkled donut turning and rocking against the night sky, shaded in 3D, flaring on the snare; it goes dark on the last beat |
| 17-20 | the tunnel | `pd_s_tunnel.c` | Out of the dark, the light at the end first, then a tunnel of circuitry, rushing past and twisting with the beat; the mouse, a silhouette, flies away down it toward the light; in the last bar it rushes faster and faster, and cuts to black space |
| 21-24 | vectors | `pd_s_vectors.c` | Black space: a wireframe Z turns while a dodecahedron orbits it, stars stream past, copper bars sweep behind; in the last bar everything speeds up into white |
| 25-28 | sunrise | `pd_s_sunrise.c` | Morning over the city: the sky from violet to gold to blue, the sun climbing, the windows going dark street by street, a rainbow drawn across the sky, a sine scroller with greetings |
| 29-32 | the end | `pd_s_ending.c` | ZEITLOS in gold with a glint running over it; the mouse runs in and eats the donut on top of it, four bites on four beats; dusk; it sleeps; on the last chord, "timeless computing"; black |

## Planes as layers

The picture is one 320x240 screen of eight planes (`z_color_256`,
[color.md](color.md)), and a pixel's colour is the palette entry its
eight bits make. planedemo almost never draws a pixel's whole index.
Each scene splits the eight planes into **layers** -- a sky in two
planes, a skyline in two, rain and the mouse in three, say -- and draws
each layer into its own planes only, with the blitter
(`ar_plane()`, `ar_plane_or()`, `ar_plane_andn()`, `ar_plane_fill()`,
[arcade.md](arcade.md)) or the line rasterizer (`ar_plane_line()`).
The palette then does the compositing: for each of the 256 indices the
scene works out which layer is in front and gives the entry that
layer's colour. A layer can move, scroll or change without touching
the others, and without a mask: what is behind it is still there, in
its own planes.

The palette is also where most of the motion is. It is rewritten every
frame (256 entries are cheap), so:

- **plasma** and the **tunnel** are drawn once and never again; the
  fire and the rushing walls are palette entries being rotated;
- **fades**, **flashes** and the drums' **flares** are every entry
  mixed toward white or black;
- the **sunrise** is one picture whose sky, sun and windows are entries
  that change colour with the time of day, the windows in eight groups
  that go out at different times;
- the **glint** on the gold letters runs over eight diagonal stripes of
  the same gold.

What is not palette is blits: the donut is 64 frames, each six planes
of 176x176 in RAM, one blitted per two film frames; the skylines are
pictures wider than the screen, scrolled; the scroller is 80 small
blits a frame, a four-pixel column each at its own height.

## Made before it plays

Everything a scene shows that takes computing -- the instruments, the
plasma field, the skylines, the rain, the donut's 64 frames (a torus of
points, a depth buffer, a light), the tunnel's map, the sunrise and the
ending's pictures -- is made **on the build host**, by the same code,
and linked into the program as the film's data: `tests/mkfilm.c`
writes `film.pdd` (2.8 MB, made whenever the film's sources change) and
`pd_blob.c` includes it. Each part has a visitor (`pd_*_store()`,
`pd_gfx.h`) that writes what it made there and, on the board, reads it
back in the same order -- the pictures and the samples point into the
data, nothing is copied. On the board it is a few hundred thousand
instructions; made there, it was some 700 million (the instruments
alone took three minutes on the first run).

A build without the data (a host test, or data that does not fit the
program) makes it all on the board as **jobs** (`pd_job_add()`): a
function that does a small step and says when it is done, given what is
left of each frame after the drawing (`pd_jobs_run()`). There the donut
does not wait for its 64 frames: it turns through the ones made so far
while the rest are made. `make slow` (below) plays the film that way,
with the jobs given a frame in forty. On fewer than eight planes the
ending makes its own pictures that way (they differ by board).

**What a frame costs** is counted on a RISC-V emulator (`tests/rv`): the
real program, built for RV32IM with the runtime's machine half doing
nothing, every instruction between the film's marks counted. Most of a
frame is the palette: 256 entries worked out each frame, most of it
`pd_mix()` (now four multiplies, not a loop; exactly the runtime's
rounding). As of r4c, a frame's working out is 12,000 (the storm) to
56,000 (vectors, sunrise) instructions, and running its drawing at the
boundary 5,000 to 51,000 (below).

## Racing the beam

There is one screen and no back buffer: 256 colours take all eight
planes of both VRAM sheets, so nothing is left to draw into unseen. The
first run on the board (r3) showed what that costs when drawing goes
wherever the beam is: the mouse flashed (the rain is blitted over its
planes, then the mouse; between the two it is not there), the donut had
horizontal bands (six planes blitted one after the other, each first
cleared), the scroller lost columns (its band cleared, then 80 columns
blitted back), and the palette, written a frame apart from the pixels,
gave a picture a frame's colours too early.

So no scene draws while it works the frame out. Every `pd_put()`,
`pd_shape()` and `pdp_*()` is **queued** (`pd_queue()`, `pd_gfx.h`):
the operation and its source, which is always a picture that lives for
the film. The runtime wakes exactly at the boundary
(`ar_vsync_exact()`, [arcade.md](arcade.md), "Racing the beam"),
writes the palette, and calls `draw_frame()` (`ar_frame_hook()`), which
runs the queue **eight bands of 30 rows from the top**: each band gets
its part of every operation, in the order asked for, so the top of the
picture is finished first and the beam, 69,000 cycles of blanking
behind and then 3,050 cycles a row, does not catch up. A line falls
in every band it crosses and is drawn there clipped to it -- unless
only lines that set and ORs follow it in its plane and rows, when it is
drawn whole at the end of the band of its last row (the vectors' 400
lines, once each).

And the operations themselves change nothing on the screen twice: a
picture put opaque is one blitter pass (`ar_plane()` is the COPY ROP
now, no clear first), and what used to be drawn over something just
drawn is put together in RAM first (`pd_canvas_*()`, `pd_gfx.h`) and
blitted as one: the scroller's 72 rows; the storm's mouse, in a box of
its rows and the rain under it, the rain blitted round the box; the
ending's strip of planes 5-7 with the donut, the mouse and the z's.
Ahead of the beam or not, every pixel of those is written once a frame.
(r4 still drew the mouse over the rain and over the cleared strip, and
it still flashed on the board.) On the host the frames are byte
for byte the same queued or drawn as asked (`PD_HOST_DIRECT=1`).

What the queue costs is counted with the rest on the emulator
(`tests/rv`, "boundary"): 5,000 to 17,000 instructions a frame, 51,000
for the vectors.

## On fewer planes

The scenes draw in eight **logical** planes and write all 256 palette
entries. On a board with fewer, each scene's `begin()` says which
logical planes to keep with six and with four (`pd_keep()`, `pd_gfx.h`):
those land, in order, on the physical planes, the rest are simply not
drawn (`pdp_*()` and every `pd_put()` go through the map), and at the
end of the frame each physical index is given the entry of the logical
index it stands for (`pd_logical()`). On eight planes nothing changes:
the frames are byte for byte the same.

| Scene | 6 planes (64 colours) | 4 planes (16 colours) |
|---|---|---|
| the clock | the face in steps of 4 shades | in steps of 8; hands, second hand and word in two planes, no shadow |
| ignition | the fire in 32 steps | in 8 |
| the storm, the mouse | no clouds, fewer far windows | only the near roofs behind the rain, the mouse and the lightning |
| the donut | no glow behind it | the donut in steps of 4 |
| the tunnel | no fog; the slices past the eighth repeat | two slices round |
| vectors | stars in one brightness, bars in 4 steps | the solids in two planes (every edge still shows), flat bars |
| sunrise | fewer sky bands, a coarser rainbow | a plain sky, the sun, the city, the words |
| the end | the letters in 3 planes (4 stripes of glint), a plain sky, no stars | letters in 2, the mouse in 2 |

Where dropping planes would lose a colour that matters, the scene makes
its pictures in other values for the board (the end: `enc_logo()`) and
reads them back as the eight-plane ones for the palette (`dec_logo()`).

## One clock

The film has one clock: the frame counter, 60 a second. At 128 BPM a
row (a sixteenth) is 225/32 frames, so `pd_frame_of_row()` and
`pd_row_of()` turn one into the other, and the music and the pictures
can never drift apart. When a frame takes too long `ar_frame()` says
how many went by, the clock jumps by that many, and both stay in step
-- the film drops frames rather than slowing down.

## The music

`pd_synth.c` makes every instrument when the film starts, as 16-bit
samples at 35200 Hz, from integer oscillators: a kick (a falling sine),
a snare and a clap (noise and a body), hats, a bass and a pluck (a
pulse's harmonics whose brightness falls as it rings), a lead and a pad
(six detuned saws each, at whole-Hz pitches so that one second of them
loops without a click), thunder, the clock's tick and the clatter of its
hands, a noise riser, a bell. `pd_song.c` writes the 32 bars as the film is cut (A minor, then
C major for the sunrise, an A major chord to end), and `pd_music.c`
plays them on the hardware mixer's eight channels, one per part, with
an envelope table per instrument. Scenes listen: `pdm_level(channel)`
is how loud a part is now, and the kick and snare are most of the
flashes.

## Files

| File | |
|---|---|
| `planedemo.c` | the main loop: the clock, the scenes, the jobs |
| `pd.h`, `pd_scenes.[ch]` | what a scene is; the scenes, in order |
| `pd_gfx.[ch]` | chunky (byte a pixel) and planar pictures, putting them in planes, the drawing queue, dither, sine, jobs |
| `pd_music.[ch]`, `pd_synth.c`, `pd_song.c` | the music |
| `pd_s_*.c` | the scenes |
| `pd_blob.c`, `tests/mkfilm.c` | the film's data: made on the host (`film.pdd`), linked in |
| `zpack.py` | the program packed (LZ4) after `mkexec.py` |
| `tests/rv/` | instructions counted on a RISC-V emulator |
| `tests/` | the film on a build host |

The mouse and the cat are mausrun's own art (`sw/apps/mausrun/sprites.c`).

## On a build host

`sw/apps/planedemo/tests`:

```
make wav      # the music alone, out/planedemo.wav
make video    # the film, out/planedemo.mp4 (needs ffmpeg)
make sheet    # one picture every two seconds, out/sheet.png
make slow     # a board much slower than the host: out/slow-sheet.png
make film     # the film's data, ../film.pdd (the app's Makefile does this)
./render_pd <dir> <every> [endframe]   # frames as .ppm
PD_FILM=../film.pdd ./render_pd ...    # from the film's data, as the board
AR_HOST_PLANES=6 ./render_pd ...       # on 6 (or 4) planes
make -C rv run   # instructions a frame, by scene and by function
                 # (riscv-none-elf-gcc; pip install unicorn pyelftools)
PROFILE=boundary make -C rv run   # what runs at the frame boundary, by function
PD_HOST_DIRECT=1 ./render_pd ...  # drawn as asked, not queued: the same frames
```

Played from the film's data or made by the jobs, the 3750 frames are
byte for byte the same, and so is the music.

The host build runs the same code against `ar_host.c` (the arcade
runtime with a framebuffer in memory and a software line rasterizer)
and a model of the mixer.

## Adding a scene

A scene is a `pd_scene_t`: a name, the bar it starts on, `begin()`
(draw the first picture; it is told nothing and may assume nothing
about the planes) and `frame(t, lt)` (the film's frame and the scene's
own). Add it to `pd_scenes[]` in order of bars, and anything slow to
make as a job in `planedemo.c`. Draw through `pd_put()`, `pd_shape()`
and `pdp_*()`, not the runtime's `ar_plane*()`: they are queued and run
at the boundary, so draw from pictures that stay as they are until the
next frame. Say in `begin()` what to keep on fewer planes (`pd_keep()`).
