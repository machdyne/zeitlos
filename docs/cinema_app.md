# cinema

Kind: a **desktop app** ([app_kinds.md](app_kinds.md)).

The film player: ZV files ([zv.md](zv.md)), made from an mp4 (or
anything ffmpeg reads) on another computer with `tools/zv/zvenc`.

    sw/apps/cinema/cinema.c

    > run cinema /media/video/film.zv

or double-click a `.zv` file in `files` (`sw/common/ztype.c` maps ZV to
cinema), or open one with the icon in the title bar. Its icon is in the
dock's Media group, with the audio players ([dock.md](dock.md)). It is built with
the other apps (`sw/apps/Makefile`) and is on a card with the audio
set (`release/lib/mkfatimg.py`). Films are not: make one with `zvenc`
([zv.md](zv.md)) and copy it to `/media/video`.

## What it shows

| Window | View | |
|---|---|---|
| the window, as it opens | `ZV_VIEW_MONO_HALF` | the film at half size, sized to it (a 16:9 film: 320x180); a film 352 wide or less (archive.org's 320x240 copies) opens whole, `ZV_VIEW_MONO_FULL` |
| big enough for the whole film (maximized, or **F11**: wm's full screen) | `ZV_VIEW_MONO_FULL` | the film at full size, centred on black |

Black and white, in the desktop's phosphor, dithered with ZV's 8x8
void-and-cluster mask. Full screen keeps the window's 2-pixel frame,
so a 640-wide film loses 2 pixels on each side there.

**g** is game mode: the whole screen, the half view doubled, in as
many colours as the board has:

| Board | Game mode |
|---|---|
| chunky game mode in VRAM (the 45F boards) | the palette view: **true greys** from the palette, not dithered black and white, or **256 colours** for a film with a colour layer; the blocks that changed go to VRAM by the blitter |
| chunky in main memory (`VRAM_SDRAM`: lakritz_console, sergei_ml2_console) | the same, with a page flip a frame |
| neither (Lakritz's desktop build, the 12F boards) | black and white, doubled |

A film of 320x240 or less fills game mode at its full size
(`ZV_VIEW_PAL_FULL`, or mono full). Esc or g leaves it; the window comes
back as it was. Changing view decodes again from the last seek point,
quietly: a second or two on a board. **A film in colour** (one with a colour layer, [zv.md](zv.md)) shows in
colour there: 16 chroma classes a seek point's group, each over the 16
greys, as the palette's 256 entries. The layer is read through a second
handle, a group at a time, the next one in 2 KB pieces while this one
plays. Its palette goes in when its group's first picture is shown. A
group too big for the 48 KB buffer, or a damaged one, shows in greys.

**A black and white film in the window, in greys** (the 45F boards,
which have the colour window, [color.md](color.md#the-colour-window)):
16 true greys, dithered between, as game mode shows them -- not the
desktop's one bit. The decoder writes the four grey planes itself
(`ZV_VIEW_PL_FULL`, [zv.md](zv.md)), and the blitter moves the rows
that changed to the colour window's planes; the palette's 32 entries
are the 16 greys, in the desktop's phosphor at full brightness. It
works for a film shown at full size, 320x240 or less (what `zvenc`
makes by default); a 640-wide film at half size stays dithered, since
its plane view costs twice the mono one. As with colour, cinema has the
window only while it is focused and wholly uncovered, and F10 turns
the greys off and on. The greys and the colours are 640x480's, as the
colour window is ([screen_modes.md](screen_modes.md)): at another
resolution the film is dithered, and changing resolution while it plays
changes it over. Changing between the two costs no decoding, so a
click on another window is answered at once ([responsive.md](responsive.md)):
the picture in memory is turned into the other layout as it stands
(`zv_planes_to_mono()`, `zv_mono_to_planes()`) and the film carries on.
Into black and white, the greys are dithered with the same mask. Into
greys, the dithered picture stays as it was and turns grey as its
blocks are drawn, within a seek point's refresh (2 s).

When cinema gives the colour window up -- another window clicked, F10,
game mode, quitting -- it clears its planes out of the second sheet
first. wm tints the desktop from that sheet, and what was left of the
film showed as colour where the picture had been.

**A colour film in the window** (the 45F boards, which have the colour
window, [color.md](color.md#the-colour-window)) shows in 32 colours.
The picture is still the black and white one, dithered. The colour
window's four planes under it hold each cell's class, so a pixel shows
black or its class's colour at the brightness of the desktop's ink: a
hand-tinted film, 16 tints a seek point's group. It works at 320x240
or less, which covers the half view of a 640-wide film and the whole
view of archive.org's 320x240 copies. cinema asks wm for it only while
it shows such a film (`Z_WIN_FLAG_COLOUR_LATER`, `Z_WM_COLOR_WANT`), and
takes it only while the window is focused and wholly uncovered. F10,
or the rainbow in the title bar, turns it off and on.

The planes go to VRAM in two ways. A frame's changed blocks go by the
CPU. Main memory keeps the planes a row of cells at a time (a cell's
pixel rows are all alike), aligned as VRAM's words are. So a block is
one read-modify-write per row of cells and plane in memory, then plain
word stores to VRAM for its pixel rows: no VRAM reads, no multiply in
the loop, and the fold at column 640 falls between words. When the
colour is taken, the window moves, or a fifth of the picture changes at
once (a group's first frame), every block goes into memory, is expanded
to pixel rows, and goes by the blitter. While the colour window is not
held, nothing is kept up: it is built again when it is taken.

In zsys at `--cpi 8`, on a 320x176 colour clip at full size, showing
went from 612k cycles a frame to 428k; at `--cpi 2`, from about 153k
to 77k. A frame's class changes were about 95 blocks of 8x8 here.

The title bar shows the film's name, and "(paused)" while paused. It
used to show the time as well, redrawn once a second, but wm clears a
title bar before it draws a new title, and that showed as a black
flash.

## Keys

| Key | |
|---|---|
| Space | play / pause |
| Left, Right | 10 seconds back, on (to the seek point there) |
| Down, Up | a minute back, on |
| c | the controls under the picture, on / off |
| Home | the start |
| l | loop on / off |
| i | timing lines on the serial console, on / off |
| o | open a film |
| u | open a film on a web server, by its address ("From a web server") |
| g | game mode, and back (Esc too) |
| F1 | help |
| F11 | full screen (wm's) |
| q | quit |

## The controls

A bar under the picture in the window (not in game mode): play/pause,
loop, a timeline, and the time played and in all. A click on the
timeline jumps there; held, the knob follows the pointer and the film
jumps when it is let go. **c** hides it and brings it back; the window
keeps to the film's size either way.

It is drawn into a bitmap in memory and blitted whole, as the picture
is, so nothing is cleared on the screen and nothing flashes; and only
when what it shows has changed -- its state, the knob by a pixel, the
time by a second: two or three blits of 14 rows a second.

**Jumping about stays responsive** ([responsive.md](responsive.md)). A
seek from a key or the timeline is noted and done once, when the
waiting messages have all been read: ten presses of Right that arrive
together are one seek, 100 s on. And it lands on a seek point (they are
2 s apart): the last one not after the place asked for, or going
forwards the next one, so a step on always moves on. That is one frame
to decode. The exact frame would be up to two seconds of them, most of
a second on a board with nothing else answered meanwhile.

## How it works

**The clock is the sound.** Each ZV record holds one frame and its
share of the sound. The sound goes into a 64 KB ring that the hardware
mixer reads (`rtl/audio_mixer.v`, one channel, two for stereo; signed
8-bit, as `play`'s mixer path), about half a second ahead of what is
being heard (about a second). How much has been played comes from the
write position and the mixer's place in the ring -- what is still to
play is their difference, modulo the ring -- so a slow pass cannot make
the clock jump; frame *n* is due at the 60 Hz
refresh `zv_frame_vsync(n)` gives it. Without sound -- a file with
none, or a bitstream without the mixer -- the refresh counter (socctl's
FRAME) is the clock instead.

**The end.** The mixer does not stop at the write position: it goes
round, and would play the ring's last three seconds again. So when the
last record's sound is in the ring, an eighth of a second of silence
goes in after it, and the film is over once the mixer is inside that
silence (or has stopped getting nearer for half a second). Then loop
starts again and otherwise the film pauses. It used to wait for the ring
to be exactly empty, which one pass had to catch on one byte; missed --
on the board, usually -- the last seconds of sound went round for good,
with loop off too, and in game mode the last frame was never shown.

**Records** are read from the card one at a time into a 256 KB queue,
as far ahead as the sound needs: their sound into the ring at once,
their pictures when they are due.

**Two pictures** in main memory: one bit a pixel in the framebuffer's
order (the window), or a byte a pixel, 320x240 (game mode's chunky). The next frame is decoded into the back picture (a copy of the
front first: ZV skips unchanged blocks) as soon as the last one is up;
at the first refresh at or after its time the back goes to the window
with the blitter (`z_fb_hw_blit_mem()`, clipped to the window's visible
region, with a software copy on bitstreams without it), and the two
change places. A frame that falls behind is decoded and not shown, so
the picture catches up with the sound instead of the sound waiting --
at most three such a pass, so a board too slow for a film falls behind
(and says so, below) rather than freezing with keys unanswered.

**In chunky game mode** the decoder marks the blocks a frame changed
(`zv_dec_t.dirty`). The back picture is brought up to date with only
those blocks, not copied whole (76,800 bytes); on a 45F board only
those blocks go to VRAM, by the blitter: a chunky segment's rows are
framebuffer rows, so four lines of a segment are a 640x4 one-bit
rectangle, and each block row with changes is at most four blits from
main memory, started and not waited for. With `VRAM_SDRAM` the
pictures are flipped (`z_chunky_show()`), and the one going out of
sight is written only after the flip has been taken, at the next frame.

**Seeking** goes to the seek point before (target - refresh), decodes
from there without showing anything, and starts the sound at the
target ([zv.md](zv.md), "Seek points"). Changing between the half and
the full view does the same at the frame being shown.

**Pausing** stops the mixer (MIXEN off parks its channels) and the
clock with it.

## What it reports

On UART0, every five seconds while playing, once **i** has turned the
lines on (a film played for its own sake prints only the summary at
its end):

    cinema: frame 151, due 151: shown 151, late 0, not shown 0; a frame: decode 474568,
            read 176703 (3297 bytes), show 7550; sound ahead 1019 ms

-- the frame shown and the one the sound has reached; the cycles a
frame spent decoding, reading the card and showing. And at the end of
a film and on quit:

    cinema: 149 frames shown, 0 of them a refresh or more late, 0 decoded but not shown;
            decoding 258619 cycles a frame, at most 2392214

-- the numbers to send back after a run on a board. After each
five-second line, and before the end line, comes where the reading went
in `fs` (its counters, `Z_FSOP_STATS`; [sdcard.md](sdcard.md), "Where a
read goes"):

    cinema: fs: 125 reads, 479903 bytes; a read 98480 cycles here, in fs 83761,
            the card 77664 of them (937 sectors in 937 card reads, 477468 bytes by DMA)

-- per call: what it cost `cinema`, what of that was spent in `fs`, and
what of that was the card. "here" minus "in fs" is the trip there and
back. "in fs" minus "the card" is FatFs and copying. Few sectors per
card read means a small cluster or a fragmented file. Each card read is
a command, a wait and a stop.

Then where the whole machine's time went in those five seconds, from
the kernel's accounting (`z_cpu_stat()`):

    cinema: cpu: cinema 56%, fs 20%, wm 8%, net 1%, others 2%, interrupts 4%, idle 9%

If idle stays well above zero while frames are missed, the misses are
spikes rather than a lack of time on average.

### Keys that change the picture

| | |
|---|---|
| **g** | game mode: the whole screen, 320x240 doubled, 256 colours or 16 greys |
| **F11** | wm's full screen: the window without its neighbours, still the desktop's one bit (or the colour window, for a small film) |
| **F10** | colour (or greys) in the window off and on |

### A frame in pieces

The decoder draws 400 blocks a call while a film plays -- an ordinary
frame is still one call, a cut or a seek point three or four --
(`zv_dec_t.step_blocks`, [zv.md](zv.md)) and the loop comes round
between calls: messages are read, the bar is kept, sound is fed. A
frame that is every block -- a cut, a seek point -- took 50 ms and more
in one call, against the 20 ms an app's loop may be away
([responsive.md](responsive.md)). In zsys at `--cpi 8` the longest call
for a black and white film went from 1.8-2.0M cycles to 0.5M. Two
things are still whole: a seek's first frame, and a colour film's
classes at a seek point (1.2M there). "worst" in the timing lines is
now the longest single call.

### One picture, and frames shown late rather than skipped

Outside chunky game mode in main memory, the screen is VRAM: it holds
this frame while the next is decoded into the one picture in memory.
(There were two, and a copy a frame to keep them in step: 240k cycles a
frame in game mode, in zsys at `--cpi 8`.)

A frame that takes several frames' time to decode -- a cut, a seek
point -- is shown late, and the ones after it as each is ready, until
the picture has caught up with the sound. Frames are skipped only when
it trails by more than five (`LAG_MAX`, about 0.2 s). Skipping at any
lateness cost about five frames a cut, with half the CPU idle.

### Game mode's blits

The changed blocks go to chunky VRAM by the blitter, a segment (80
pixels) at a time down the picture: block rows with changes that
follow one another are one blit, over the widest of their spans. It
was a blit a block row and segment, up to 120 a frame, and the calls
cost more than the words moved: in zsys, whose blitter takes no time,
showing still cost 260k cycles a frame. Now 130-150k there.

wm repaints the window's frame once as it takes the colour window
back, which can land after game mode has the screen; in a film with
black bars it stayed, a line in the bar. The screen is wiped once
more, 20 frames in.

### Reading

The file is read in pieces of 8 KB that end on sector boundaries,
straight into the record queue's buffer, and records are found in
place. So `fs` is called every other frame, not every frame, FatFs
passes whole sectors straight to the card, and the card gets a few
long reads. In zsys at `--cpi 8`: 213k cycles a frame became 136k,
125 calls in 5 s became 42, and 677 sectors took 47 commands.

## From a web server

**u** asks for an address, `http://host[:port]/path/film.zv`, and plays
the film from there; `run cinema http://...` does the same. The film
is not downloaded first and nothing is written to the card: cinema asks
the server for the file a piece at a time, as it would read it from
the card, so it starts at once, and seeking, the timeline, loop, the
colour layer and game mode all work as they do for a file.

    tools/zv/zvserve.py ~/films            # on the computer with the films
    > run cinema http://192.168.1.20:8000/general.zv

**The server** has to answer byte ranges (`Range: bytes=a-b` with 206)
and should keep the connection open between requests. nginx, Apache
and Caddy do both as they come. Python's `http.server` does neither --
it sends the whole file to every request -- so `tools/zv/zvserve.py` is
that with ranges and keep-alive: it serves a directory, read-only, and
lists the films at `/` with the address to type for each. Plain http
only (no TLS: this is for a home network), and a name is looked up by
`net`'s DNS.

**How.** `netsrc.c` holds one TCP connection through `net`
([networking.md](networking.md#raw-tcp-sockets)) and has one request on
it at a time, built and parsed by `httpc.c`:

- **The film's records** are asked for 16 KB at a time, as a 32 KB ring
  has room for a whole answer -- so nothing that arrives waits for
  room, and the acks that let `net` send more go back at once. The
  player takes them from the ring where it would call `fs`. That is
  about half a second of a 320x240 film ahead of the picture, besides
  the second of sound already in the mixer's ring.
- **The header, the index and the colour layer** are other ranges of
  the file, between two of the film's: the first two waited for when
  the film opens, a colour group asked for a group ahead and left to
  arrive.
- **A seek** drops what is buffered and asks from the seek point. A
  request with little left to come (6 KB) is read to its end and
  thrown away; with more, the connection is closed and made again.
  The frames from the seek point to the one wanted are waited for --
  this is the one place cinema waits for the network -- so a seek
  takes as long as about two seconds of film take to arrive.
- **When the network is behind**, the sound is stopped before the
  mixer runs past what it has (it would play its ring again), and the
  clock with it; the title says "(buffering)", and it starts again
  with half a second queued. No frame is skipped for it.
- **A connection that drops** is made again and the request repeated
  from the byte it got to, three times; a server that closes after
  every answer (HTTP/1.0) works, a connection a request. After that,
  or when the server answers something final -- not found, no ranges
  -- the film stops and the title says why.

It uses one of `net`'s two sockets; with `web` and `irc` holding both,
the connection is refused and the box says so.

**What is not known yet:** how fast TCP arrives on a board. A 320x240
film is about 62 KB a second and TFTP measured 148
([net_throughput.md](net_throughput.md)), but TCP's own rate on a
local network has not been measured, nor what receiving costs
zeitlos32ps while it decodes. The timing lines (`i`) have a line for
it:

    cinema: net: 393216 bytes in 24 requests (0 connections, 0 tried again); 34108 bytes and 24 frames ahead; waited 0 ms

"ahead" near zero and "(buffering)" in the title mean the network is
the limit; "waited" is time spent in seeks.

**Tested** on the host and in the emulator, not on a board:
`make -C sw/apps/cinema test` runs `httpc.c` against made-up answers
at every piece size (401 checks), and
`/tmp/test_httpc --server HOST PORT /film.zv film.zv` fetches a file in
ranges from a real server and compares it. The emulator has no
network, so `sw/apps/netfake` stands in for `net` there: it takes the
name `net0` and serves the card's `/media/video` over HTTP, at a rate
(`rate=KB`), closing after each answer (`close`), dropping connections
(`drop=N`), stalling (`stall=N`) or without ranges (`noranges`):

    zsys ... --type "run netfake rate=120\nrun cinema http://x/g30.zv\n"

With it a black and white and a colour film play through from a
"server" to the last frame, in the window and in game mode, with
seeks, at 40 KB/s (buffering, nothing skipped), with every fifth
connection dropped, and with a connection a request.

## Checked

In the whole-system emulator (zsys, [zsys.md](zsys.md)), which has a
45F board's video: the real kernel and wm, `run cinema
/media/video/bunny.zv` from the console, a 10-second clip with sound --
every frame shown, none late; F11 to full screen and the full view;
game mode (g) in greys by the blitter, and Esc back to the window;
pause, resume and a seek past the end. The
sound comes out (`--wav`). zsys's CPU is not a Lakritz's: for the
board's cost see [zv.md](zv.md), "zvbench".

On Sergei ML2 (ECP5-45F, DDR3, 48 MHz, with the DMA), two-minute
clips at 320x240 and 24 fps, made with `zvenc`'s defaults:

| film | where | frames shown | a frame: decode, read, show (cycles of 2.0M) | idle |
|---|---|---|---|---|
| The General (black and white) | the window, in greys | 361 of 361 | 0.90M, 0.23M, 0.32M | 10-12% |
| Big Buck Bunny (colour) | the window, 32 colours | 359 of 360 | 0.79-0.95M, 0.20-0.26M, 0.26-0.29M | 10-20% |
| the same | game mode, 256 colours | all | 0.82M, 0.21M, 0.28-0.40M | 6-11% |

A quarter to a third of the frames were shown a refresh (1/60 s) late:
after a cut the picture trails the sound for a few frames and catches
up. The margin is thin, and `zvenc --budget` is what widens it. Not
run on any other board.

## Memory

About 600 KB of .bss: the record queue (256 KB), two pictures (320x240
at a byte, 154 KB), the decoder (about 60 KB), the seek index (64 KB),
the sound ring (16 KB),
the network's ring (32 KB, "From a web server").
