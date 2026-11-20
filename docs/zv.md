# ZV, the Zeitlos Video Format

Status: **draft 0, round 2 of the video work.** The format, the
encoder (`zvenc`), the Linux player (`zvplay`), the player for Zeitlos
(`cinema`, [cinema_app.md](cinema_app.md)) and a board benchmark
(`zvbench`) exist and are tested on the host, in the one-app simulator
and in the whole-system emulator (zsys). Not yet on a board. Everything
here may still change; files are marked `ZVF0`, and a file made before
a change to draft 0 may need making again.

The plan behind it is in the project's `claude/video-proposal.md`.

## What it is for

Films -- mostly public-domain ones from archive.org, mostly mp4 -- at
their real frame rate, on every Zeitlos: in a window on the desktop,
on the desktop's full screen, and in game mode. Conversion is done on a
host computer, once; Zeitlos only plays.

Why a format of its own: no existing codec fits a 48 MHz picorv32 that
reads the card by polling. MPEG and its descendants need far more CPU
than there is, and they end in true-colour pixels the machine cannot
show anyway. ZV puts all the hard work in the encoder and leaves the
board almost nothing but copying bytes: its output **is** what the
screen shows -- dithered bits for the desktop, palette indices for game
mode.

**No patented techniques.** Everything in ZV is old enough that any
patent on it has long expired: vector quantisation (Linde, Buzo and
Gray, 1980), skipping unchanged blocks (conditional replenishment, the
1970s), run lengths, ordered dither (Bayer, 1973) and void-and-cluster
masks (Ulichney, 1993), 8-bit PCM. There is no entropy coder newer than
those, and no technique from a modern video standard. `zvenc` contains
no decoder for any other format: the user's own ffmpeg reads the
source and hands over plain grey frames and PCM through a pipe. (This
is a description of the design, not legal advice.)

## Layers and views

A ZV file carries:

| Layer | | Status |
|---|---|---|
| **base** | the picture in grey, at full size, and which blocks changed | draft 0 |
| **colour** | a chroma class for each 8x8 block or each quarter of one, for the palette views; stored after the index, so a mono player never reads it | draft 0: `zvenc`, `zvplay`, and `cinema`'s game mode (`ZV_F_COLOUR`, `colour_off`; "The colour layer" below) |
| **audio** | 8-bit PCM, interleaved with the base layer a frame at a time | draft 0 |
| **index** | the seek points | draft 0 |

From the one base layer the decoder draws whichever **view** the screen
wants:

| View | Size | Pixels | Where |
|---|---|---|---|
| `ZV_VIEW_MONO_HALF` | w/2 x h/2 | 1 bit, dithered | **the window** (the default), game mode in mono, the console |
| `ZV_VIEW_MONO_FULL` | w x h | 1 bit, dithered | the desktop's full screen |
| `ZV_VIEW_PAL_HALF` | w/2 x h/2 | 8 bits: indices into the grey palette, dithered | game mode in greys (chunky) |
| `ZV_VIEW_PAL_FULL` | w x h | the same at full size | game mode in greys, for a film of 320x240 or less |
| `ZV_VIEW_GREY_FULL` / `_HALF` | | 8 bits, undithered | reference: tests, the encoder |

1-bit views are in the framebuffer's order (bit 0 of a byte is its
leftmost pixel, bytes in address order), so a picture can go to the
screen with the blitter as it is.

**Which blocks changed.** With `zv_dec_t.dirty` set to a byte a block,
the decoder marks each block it draws: a player keeping two pictures
brings one up to date with the other by those blocks alone, and copies
only those to the screen (cinema's game mode, [cinema_app.md](cinema_app.md)).

## Picture size and shape

`zvenc` takes the source's display shape from ffprobe (including
non-square pixels, as on DVDs), converts to square pixels, and fits the
picture inside a maximized window's content area, **640 x 464**, in
multiples of 8:

| Source | Picture | Window (half) |
|---|---|---|
| 4:3 | 616x464 | 308x232 |
| 1.37:1 (Academy) | 640x464 | 320x232 |
| 16:9 | 640x360 | 320x180 |
| 2.35:1 | 640x272 | 320x136 |

A picture is never made larger than the source shows (unless
`--upscale`): archive.org's small copies ("512kb.mp4") are 320x240, and
stay so. The player sizes its window from the file. `--size` and
`--max` override.

## Frame rate

The file keeps the source's exact rate as a fraction (24, 24000/1001,
25, 30000/1001, 18, ...). The display stays 640x480 at 60 Hz; frame *n*
is shown at the refresh nearest its time:

    zv_frame_vsync(h, n, 60) = round(n * 60 * fps_den / fps_num)

So 30 fps is two refreshes a frame, 24 fps is 3,2,3,2 (film on NTSC
television), 18 fps 3,3,4, and 29.97 is two with a third every 33
seconds. Nothing is retimed. The clock is the audio's: the player shows
the frame whose refresh has come by the samples played.

## The base layer

The picture is cut into 8x8 blocks, in raster order. Each block is one
of:

| Block | Bytes | What |
|---|---|---|
| SKIP | (runs) | unchanged since the last frame |
| V1 | 1 | one 4x4 entry of the **V1 codebook**, doubled to 8x8 -- smooth areas |
| REP | (runs) | a run of V1 blocks with one entry -- flat areas |
| V4 | 4 | four 4x4 entries of the **V4 codebook** -- edges, texture |
| V16 | 16 | sixteen 2x2 entries of the **V16 codebook** -- fine detail |

Each codebook has 256 entries of grey. The books carry over from frame
to frame; a frame sends only the entries that changed. This is
Cinepak's idea (V1/V4, 1991) with one level more.

### File layout

All numbers little-endian.

**Header**, 64 bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `ZVF0` |
| 4 | 2 | header size (64) |
| 6 | 2 | flags: bit 0 audio, bit 1 colour layer |
| 8 | 2 | width (multiple of 8, at most 640) |
| 10 | 2 | height (multiple of 8, at most 480) |
| 12 | 4 | frame rate numerator |
| 16 | 4 | frame rate denominator |
| 20 | 4 | frames |
| 24 | 4 | audio rate, Hz |
| 28 | 1 | audio channels (1, 2) |
| 29 | 1 | audio format (1: unsigned 8-bit PCM) |
| 32 | 4 | index offset |
| 36 | 4 | index entries |
| 40 | 4 | colour layer offset (0: none) |
| 44 | 4 | largest record, in bytes |
| 48 | 4 | video bytes (statistics) |
| 52 | 4 | audio bytes (statistics) |
| 56 | 2 | **refresh**: frames after a seek point until the picture is whole (0: at once) |
| 58 | 6 | 0 |

**Records**, one a frame, from offset 64:

| Size | Field |
|---|---|
| 4 | record size, these 8 bytes included |
| 1 | kind: 1 frame |
| 1 | flags: bit 0 **seek point** |
| 2 | audio sample frames in this record |
| ... | the audio, channels interleaved, then zeros to a multiple of 4 from the record's start |
| ... | the video part |

Frame *n*'s record carries samples `[zv_audio_before(n), zv_audio_before(n+1))`
with `zv_audio_before(n) = floor(n * rate * fps_den / fps_num)`.

**The video part:**

| Size | Field |
|---|---|
| 2 | V1 entries sent (n1) |
| 2 | V4 entries sent (n4) |
| 2 | V16 entries sent (n16) |
| 17 x n1 | index, 16 grey values (4x4, rows) |
| 17 x n4 | index, 16 grey values |
| 5 x n16 | index, 4 grey values (2x2) |
| ... | block commands, until every block is covered |

**Block commands:**

| Byte | Command | Followed by |
|---|---|---|
| `00`-`3f` | SKIP n = (b & 63) + 1 blocks | -- |
| `40`-`7f` | V1, n = (b & 63) + 1 blocks | n entry indices |
| `80`-`bf` | V4, n = (b & 63) + 1 blocks | 4n indices: top-left, top-right, bottom-left, bottom-right |
| `c0`-`df` | REP, n = (b & 31) + 1 blocks | one V1 index |
| `e0`-`ff` | V16, n = (b & 31) + 1 blocks | 16n indices, 2x2 cells in raster order |

A V1 entry's 4x4 values are the block's 2x2 means; doubling repeats each.

**Seek points** send all three codebooks whole, as they are (an
entry's value does not change for being sent again, so a decoder that
already has it keeps what it drew of it). The picture is then
**refreshed a band at a time**: over the next `refresh` frames (one
second by default) every block row is coded at least once, a band of
rows a frame, with no SKIP in the band. So a seek point costs little
more than a frame -- the books, about 8.7 KB, and a band -- rather than
a whole picture at once, in bytes and in work.

A player that seeks to frame *n* starts at the last seek point at or
before `n - refresh` (`zv_start_for()`), decodes from there without
showing anything, and shows *n*. A decoder after `zv_dec_seek()`
ignores records until the next seek point. The film's first frame is
coded whole.

**Index**, at the index offset: `index entries` x (u32 frame, u32 file
offset of its record).

## Dithering

The mono views are dithered with an **8x8 threshold mask**, fixed to the
screen: one of

- `ZV_MASK_VAC8` -- a void-and-cluster tile (blue-noise-like): fine and
  irregular. The default.
- `ZV_MASK_BAYER8` -- Bayer's ordered matrix: the classic crosshatch.

Both are in `sw/common/zv_masks.h`, made by `tools/zv/gen_masks.py`.

A mask fixed to the screen keeps still what does not move: a static
background is the same bits every frame, so it neither shimmers nor
costs anything. Error diffusion would make every changed pixel ripple
through its neighbours.

Before the mask, grey is turned into light with a **gamma**:
`zv_dec_init(..., gamma100)`, 1.6 by default. 2.2 is the display's own
and exact seen from across a room; at a desk it loses the shadows. The
palette view mixes real light from the palette, so it always uses 2.2.

**Why it is cheap.** The decoder keeps, for each of the 256 greys, the
8x8 pattern of a flat field of that grey (`pat`). Drawing a codebook
entry for the view is picking bits from those patterns. An entry is
drawn the first time a block uses it at a given place in the mask, and
kept until it changes; drawing a block is then copying bytes -- a V1
block of the full mono view is eight byte stores.

**The grey palette.** The 12-bit palette has only 16 true greys, and
the palette view uses them (`ZV_GREYS_TRUE`, the default), with the 8x8
mask dithering between the two nearest: steps of one sixteenth of the
range, a texture far finer than black and white's. `ZV_GREYS_NEAR`
adds near-greys (channels at most one step apart) for up to 64 levels
and less dither -- but each near-grey has a faint tint, and in flat
areas the sky came out pink and the trees green, so it is not the
default (`zvplay --greys near` shows it).

**The free dithers.** `zvplay` can also show what a dither not tied to
the 8x8 mask would look like -- Floyd-Steinberg and a 64x64 blue-noise
mask (`ZV_WANT_BN64`) -- over the undithered picture. These are not on
the board: they are there to judge whether a design that dithers in
the encoder would be worth its larger files.

## zvenc

    make -C tools/zv
    tools/zv/zvenc [options] film.mp4 film.zv

It needs `ffmpeg` and `ffprobe` on the PATH, and nothing else.

| Option | |
|---|---|
| `--max WxH` | fit inside (default 640x464) |
| `--size WxH` | exactly this size (multiples of 8) |
| `--upscale` | allow a picture larger than the source shows (default: not -- a 320x240 source stays 320x240, a quarter of the board's work for the same detail) |
| `--start T`, `--duration T` | a part (seconds or hh:mm:ss) |
| `--fps N[/D]` | override the rate |
| `--ivtc` | undo 3:2 pulldown: 29.97 transfers of film back to 23.976 |
| `--deint` | deinterlace |
| `--denoise S` | remove grain first (hqdn3d), 0 off; default 3 |
| `--sharpen A` | unsharp mask amount, 0 off; default 0.4 |
| `--eq STR` | ffmpeg's eq filter, e.g. `contrast=1.1:brightness=0.02` |
| `--rate KB` | video kilobytes a second (default 80 for 640x464, scaled by the square root of the picture's area: about 40 for 320x240) |
| `--lambda L` | fixed quality instead of a rate (smaller is better) |
| `--key S` | a seek point every S seconds (default 2: a seek, or cinema's change of view, decodes up to key + refresh seconds of frames quietly first; every 5 s instead saves about 4% of the file) |
| `--refresh S` | the picture refreshed over S seconds after each (default 1) |
| `--loudness L` | normalise the sound to L LUFS before it is cut to 8 bits (default -16; 0 off) |
| `--threads N` | default: one a processor; the output is the same for any number |
| `--motion-probe` | measure what block copies from the last frame would save (nothing is coded differently) |
| `--audio-rate R` | default 22050 |
| `--stereo`, `--no-audio` | |
| `-v` | a line a frame |

What it does: the grey frames come from ffmpeg (`fps`, `scale` with
Lanczos to full range, the filters above, `format=gray`); the sound as
unsigned 8-bit, loudness-normalised first (`loudnorm`) and dithered
(triangular) into 8 bits -- 8 bits keep about 48 dB, and a quiet old
soundtrack would otherwise be a few steps of noise. For each frame it refines the three codebooks by
Lloyd iterations warm-started from the last frame's (reseeding unused
entries at the worst-coded vectors), keeps an entry's new value only
where the improvement pays for its bytes, then picks each block's
cheapest of distortion + lambda x bits. Rate control moves lambda after
each frame.

The nearest-entry search -- nearly all of the time -- tries entries in
the order of their sums outward from the vector's own, and stops when
the difference of sums alone rules the rest out (|a - b|^2 >= (sum a -
sum b)^2 / dim); it is exact. The searches run on every processor. On
two laptop cores a 616x464 film encodes at about 25 frames a second,
four times what round 1 did.

The encoder's idea of the picture is made by decoding its own output
with `zv.c`, so it and every player agree to the pixel.

## The colour layer

It colours the palette views without touching the base layer.
`zvenc` writes it, and `zvplay` (`--mode colour`, key 8) and `cinema`'s
game mode on chunky boards show it ([cinema_app.md](cinema_app.md)).

- **Each cell gets one of 16 chroma classes.** A cell is 8x8, or 4x4
  for a film of 320x240 or less (game mode shows those at full size,
  where an 8x8 cell's edge is a visible block; `--colour-cell`). A
  palette pixel's index is `class * 16 + level`, `level` being the grey
  level 0..15 the palette view already draws (`ZV_GREYS_TRUE`).
- **Class 0 is always neutral**, so indices 0..15 are the 16 greys and
  a grey film looks as it does without the layer.
- **The 256-colour palette** (`zv_col_palette()`) is, for each class,
  its chroma added to each of the 16 grey levels (BT.601), scaled down
  just enough for the colour to fit (worked out, three divisions an
  entry) and rounded to 12 bits. Each level keeps its brightness, so
  the dither between two levels of a class still works.
- **A group is one seek point's frames.** The 16 classes are fitted to
  the group's cells by k-means (weighted against the darkest and
  lightest cells, whose colour shows least; class 0 held neutral),
  starting from the last group's classes in their places. So a class
  keeps its index and nearly its colour across a seek point instead of
  jumping. The group's first frame sets every cell. After that a frame
  sends only the cells that change, and a cell keeps its class unless
  another is clearly nearer, so colours do not flicker.
- **The plane views.** `ZV_VIEW_PL_FULL` and `ZV_VIEW_PL_HALF` give the
  palette views' 16 greys as four bit planes, each laid out as the
  mono view's picture, `plane_off` bytes apart. They are the mono
  loops with a word where those have a byte -- a byte a plane, so one
  set of shifts and masks does all four -- and a row stored a byte a
  plane. For a display made of bit planes (the colour window): no
  chunky-to-planar pass. `zv_test` holds every pixel of both to the
  palette views. In zsys the full one costs about a third more than
  mono full (620k against 463k cycles a frame at `--cpi 8`); the half
  one about twice mono half, and cinema does not use it yet.
- **No more class changes a frame than the board can redraw.** A
  frame changes at most a 64th of the cells (`CHANGE_DIV`, `zvcol.c`):
  the ones that gain most; the rest keep their class and stand again
  next frame. A changed cell is a block redrawn in the colour window or
  blitted in game mode, and on Sergei ML2 a busy colour film spent
  500-850k cycles a frame of its 2.0M showing them. In zsys the same
  clip's showing went from 428k cycles a frame to 195k.
- **A black and white film gets no layer.** `zvenc` leaves it out when
  fewer than 1% of cells are coloured. `--no-colour` leaves it out
  anyway.
- **So does a tinted one.** An archive transfer of a black and white
  film often carries one tint over everything: a sepia or yellowed
  print. Its cells are all "coloured", but they lie on one line from
  grey through the film's mean chroma, stronger in the mid-tones. When
  they lie off that line by less than 4 (RMS, in the layer's chroma
  units; `zvc_tint()`, `TINT_SPREAD`), `zvenc` says "black and white,
  tinted" and leaves the layer out, so the film plays in greys.
  `--colour` keeps the layer. Measured on 6 s of Big Buck Bunny: in
  sepia 2.0, at 15% saturation 2.5, in full colour 17. Silent films
  tinted scene by scene (blue nights, amber days) spread far more and
  keep their layer. `ZVCOL_STATS=1` prints the figures.

```
colour_off:  "ZVC1", u16 cw, u16 ch (cells), u32 groups, u8 cell (8 or 4; 0 = 8), 3 x 0
             groups x (u32 first frame, u32 offset from colour_off)
a group:     16 x (s8 u, s8 v), class 0 = (0, 0)
             a frame each: u16 n, n bytes of cell runs in raster order:
               0x00-0x7f   (c + 1) cells unchanged
               0x80-0xff   (c & 127) + 1 cells, two a byte (low nibble first)
```

### On the board

The decoder draws the classes itself. With `zv_dec_t.colw` set, every
palette loop ORs each block's class into its stores. That is one OR a
word: the classes are kept as words (`zv_col_words()`, four a block,
the class in every byte), so the loops do no multiplying, which costs
32 cycles on picorv32. Before a frame's decode the player reads its
cell runs (`zv_col_frame_blocks()`), which update the classes and list
the blocks whose class changed. After the decode it puts the classes
into those blocks (`zv_col_apply_list()`), because the decode leaves
them alone when it does not draw them. A frame changes a few dozen
cells, so nothing scans every block. In zsys at `--cpi 8` this costs
about 115k cycles a frame on a 320x176 film, about a fifth of its
decode.

**In a window** on the boards with the colour window, cinema puts the
classes in the colour window's four planes under the dithered black
and white picture instead: 32 colours, black or a class's colour at
the ink's brightness ([cinema_app.md](cinema_app.md)).

**What it costs in bytes:** Big Buck Bunny at 640x368 (8x8 cells) gets
a 6.9 KB/s layer over 90 KB/s of video (7.5%). At 320x176 (4x4 cells)
it is 6.3 KB/s over 42 KB/s (15%). The base layer is byte for byte the
same with or without it, because `zvenc` reads the Y plane of
`yuv444p` as its grey.

## zvplay

    tools/zv/zvplay film.zv

The Linux player, built with SDL2 when `pkg-config` finds it
(`make NOSDL=1` without). It draws with `zv.c` -- the board's decoder
-- and shows a 640x480 Zeitlos screen, scaled 2x by default: frames on
the 60 Hz refresh nearest their time on the audio clock, as the board
will show them.

| Key | |
|---|---|
| 1 | the window: half size, mono (default) |
| 2 | the desktop's full screen: full size, mono |
| 3 | game mode, mono (half size, doubled) |
| 4 | game mode, 256 colours: greys (half size, doubled) |
| 5 | the undithered grey picture |
| 6, 7 | free dithers: Floyd-Steinberg, 64x64 blue noise (not on the board) |
| 8 | game mode, 256 colours: the colour layer (half size, doubled) |
| 9 | the window in 32 colours: the colour window, as cinema draws it (black or a cell's class at the ink's level, 0x888 -- dim here, as in zsys) |
| d | the mask: void-and-cluster / Bayer |
| p | the phosphor: white, amber, green, paper |
| space, `.` | pause, a frame forward |
| left, right, home | 10 seconds back, forward, the start |
| s | a screenshot (PNG) |
| q, Esc | quit |

Headless, for stills and previews:

    zvplay --info film.zv
    zvplay --compare 240 sheet.png film.zv           # one frame in every view
    zvplay --png DIR --mode full --every 24 film.zv  # stills
    zvplay --mode full --screen --cadence --y4m out.y4m --wav out.wav film.zv
    ffmpeg -i out.y4m -i out.wav -pix_fmt yuv420p -crf 10 preview.mp4

`--mode` is window, full, game, grey, ref, fs, bn64, colour or wcolour; `--mask`,
`--phosphor`, `--gamma` as the keys. `--cadence` writes 60 frames a
second, each film frame repeated as the board's refreshes repeat it.
`--bench` times the decoder per view on this machine. Seeking (the
arrows) decodes quietly from the seek point before, as a board does.

## A frame's cost

The board has a frame's time and no more: 2.0M cycles at 24 fps and
48 MHz, for decoding, reading and showing. Bytes do not say what a
frame costs to decode -- a V16 block is five times a V1 -- so `zvenc`
estimates it (`zve_stats_t.cost`, in decoder instructions: a block of
each kind, an entry drawn again, the frame's overhead; `ZVE_COST_*` in
`zvenc_core.h`) and rate control holds whichever is nearer its limit,
the bytes or the cost. `--budget PCT` is the share of a frame's time
decoding may take on average: 35 by default, 0 for no limit. The
summary says what came out:

    decoding on the board: about 35% of a frame's time (the worst frame 283%)

The weights' proportions are counted from the fast loops; their scale
is fitted to what zsys ran for a clip, and the board's 12 cycles an
instruction (`BOARD_CPI`) to Sergei ML2, where a 320x240 black and
white film cost 650k cycles a frame and a busier colour one 1.2M --
60% of the frame before reading and showing, and a third of its frames
were not shown. A worst frame over 100% (a cut, a seek point) is shown
late and caught up with ([cinema_app.md](cinema_app.md)).

## zvbench

How fast a Zeitlos decodes ZV. Not on any card:

    make -C sw/apps/zvbench
    # copy zvbench.bin to the card, and a clip as /zvbench.zv
    > run zvbench

It decodes up to 240 frames into each of the window's, the full
screen's and game mode's views, into a picture in main memory as the
player will, and prints the cycles a frame (average, median, 90th
percentile, worst), the cycles spent reading the card, and what that
leaves of a 24 and a 30 fps frame at 48 MHz.

In the simulator, with the cycle model ([sim/README.md](../sim/README.md), "Cycle model"):

    make -C sim zsim-headless
    mkdir -p /tmp/root && cp clip.zv /tmp/root/zvbench.zv
    ZSIM_COST=lakritz_dc sim/zsim-headless -q -n 0 -r /tmp/root sw/apps/zvbench/zvbench.bin

### Measured so far (simulator, round 2)

The decoder has a loop of its own for each of the board's views,
keeps its output pointer as it goes, draws V16 entries as they arrive
(they are used sixteen to a block) and the others the first time a
block uses them at a place in the mask, reads drawn patterns a word at
a time, and is built at -O2. A 24 fps clip at 640x368 (Big Buck Bunny,
fast motion), about 4.2 KB a frame; cycles a frame for decoding alone:

| Model | Window | Full screen | 256 greys |
|---|---|---|---|
| `lakritz_dc` (Lakritz desktop: picorv32, 4 KB data cache) | median 0.50 M (25% of a 24 fps frame) | 0.46 M | 0.57 M |
| `lakritz` (lakritz_console: no data cache) | 0.66 M (33%) | 0.62 M | 0.80 M |

Round 1's were 0.62 M and 0.84 M for the window. Game mode's greys
copied into chunky VRAM by the CPU, block by block, cost 0.81 M
(`lakritz_dc`; the copy is a third of it, VRAM stores being uncached):
which is why cinema has the blitter do it (`zvbench`'s fourth line;
the one-app simulator's blitter has no main-memory source, so that line
shows the CPU copy there -- measure it on a board). The worst frame after
the first is a scene cut: 2.2-2.6 M (`lakritz_dc`), a little over one
frame's time; seek points, now a band rather than a whole picture,
cost 1.3-1.4 M on the street clip (they cost 5-7 M in round 1). The
film's first frame is the one big one (5-7 M), before the clock
starts.

The simulator's card is free; on the board reading costs about 80
cycles a byte ([sdcard.md](sdcard.md)), about 0.33 M a frame here (17%
at 24 fps). So the Lakritz desktop's window should need about 40% of
the CPU at 24 fps. sim/README.md warns that lakritz_console runs about
two times slower than its model, with the scanout and T2 on the bus:
the console is the tight one, and where SD DMA would help most.

### File sizes and quality so far

| Clip | Picture | Video | PSNR |
|---|---|---|---|
| street, static camera (10 fps, 30 s) | 616x464 | 45 KB/s | 30.7 dB |
| the same | | 86 KB/s | 33.0 dB |
| Big Buck Bunny, fast motion (24 fps, 5 s) | 640x368 | 98 KB/s | 28.6 dB |
| Big Buck Bunny, other scenes (25 fps, 10 s) | 640x352 | 67 KB/s | 32.2 dB |

At 80 KB/s of video and 22 KB/s of sound a 90-minute film is about 550
MB.

**Motion, measured, not built.** `--motion-probe` counts the coded
blocks that a copy from the last picture at an even offset of up to 8
pixels would have coded more cheaply: on these clips, 3% (the street),
6.5% and 11% (Big Buck Bunny) of the video's bytes. It is not in the
format yet: in the mono views a copy would move the dither with the
picture, which needs a design of its own, and the gain should be
measured on real films first (camera pans may give more).

## Tests

    make -C tools/zv test

`zv_test`: the fixed-point gamma against `pow()`; the header; the grey
palette (count, order, near-grey); the cadence (24, 29.97, 18 fps) and
the audio shares; for 40 frames of a synthetic film with a pan, a
bouncing square, noise and a scene change, with both masks: every view
decoded from the encoder's output, the grey view **equal to the
encoder's own picture**, and each dithered view equal to its mask
applied to the grey picture; a player starting mid-film waits for the
next seek point and, after its refresh, matches; the board's loops for
each view equal the plain per-block code, frame for frame; a decoder
that changes view mid-film matches one that did not, after a seek;
2,000 damaged records decoded without harm; the colour layer on a
synthetic film (classes right cell by cell, frame by frame, the index
where it says, class 0 neutral and its palette the true greys, a black
and white film with nothing coloured, 3,000 damaged runs). `tools/run_tests.py` runs
it with the tree's other host suites.

## Files

| | |
|---|---|
| `sw/common/zv.h`, `zv.c` | the format and the decoder: no allocation, no I/O, no floating point |
| `sw/common/zv_masks.h` | the masks (generated) |
| `tools/zv/zvenc.c`, `zvenc_core.c/.h` | the encoder: ffmpeg front end, and the codec |
| `tools/zv/zvcol.c/.h` | the encoder's colour layer |
| `tools/zv/zvplay.c` | the Linux player |
| `tools/zv/zv_test.c` | the tests |
| `tools/zv/gen_masks.py` | makes `zv_masks.h` (needs numpy) |
| `sw/apps/zvbench` | the board benchmark |
| `sw/apps/cinema` | the player for Zeitlos ([cinema_app.md](cinema_app.md)) |

## Next

- **On a board**: `zvbench` and `cinema` on Lakritz (desktop and
  console) and a 45F board, with real films.
- **Game mode in cinema**: the half view doubled, in mono everywhere and
  in 256 greys on chunky boards (the arcade runtime's way, as view's
  screen face).
- **Motion**, if real films say it pays.
- **The colour layer on the board**: cinema's game mode reading it
  (the host prototype is above).
- **SD DMA**: built, `DMA` ([dma.md](dma.md)); `cinema` gets it through
  `fs` with no change of its own.
