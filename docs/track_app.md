# track

A ProTracker MOD player. `sw/apps/track`.

```
> run wm
> run track
```

Plays every `.mod` on the SD card in turn, wrapping rather than
exiting — it is meant to be left running while other things happen.
Also runs without `wm`, printing to the console instead of drawing.

300x145, fixed size. See [Why seven rows](#why-seven-rows) — the height
is set by how much can be redrawn in the time a tracker row lasts, not
by taste.

## The display

```
DOODLE.MOD  "doodle by hollywood"
pos 04/12  pat 07  row 32  spd 6/125
46875 4ch vol35 sep65 rate100% brst1
[####    ][##      ][######  ][#       ]
30 C-2 01 A08  --- .. ...  E-3 05 000  --- .. ...
31 --- .. ...  --- .. ...  --- .. ...  --- .. ...
32 D#2 01 C20  G-3 03 ...  --- .. ...  A-2 07 F06
33 --- .. ...  --- .. ...  --- .. ...  --- .. ...
fd1 ms0 pt2 st2  n spc v r s -=
```

### The four white bars

Per-channel activity, one bar per channel of the module — four for a
standard `M.K.` file, more for `6CHN`/`8CHN`. Bar length is that
channel's current volume, 0..64, from the **engine's** state:

```c
int lvl = player.ch[c].active ? player.ch[c].volume : 0;
```

They are deliberately drawn from what the player *believes* each
channel is doing, not from measuring the mixed output. That makes them
evidence rather than decoration: when the bars are moving sensibly and
the sound is wrong, the fault is below the engine — in the mixer, the
bus, or the output stage — and when the bars are wrong too, it is the
engine or the module. That distinction has already earned its keep
more than once.

They also cost almost nothing: one filled rectangle per channel, no
glyphs, which is why they share a redraw slot with the text lines
without unbalancing it.

Bars follow the **volume column**, so a channel playing at full volume
with a silent sample still shows a full bar. They are not a VU meter.

### The rest

| line | |
|---|---|
| 1 | filename and the module's embedded title |
| 2 | order position / song length, pattern number, row, speed/tempo |
| 3 | rate, channels, volume, stereo separation, rate%, burst |
| 4 | the four bars |
| 5-11 | pattern grid, playing row inverted in the middle |
| 12 | loop timings and key help |

`rate%` reads `mix%` when the hardware mixer is absent. Under software
mixing it is frames rendered against frames due — 100 means keeping up.
Under hardware mixing nothing is rendered, so it measures tracker
pacing instead. Same field, different meaning, which is why the label
changes.

`brst` is the largest number of tracker ticks `feed_hw()` had to make
up in one call over the last second. **1 is healthy.** Anything higher
means the main loop stalled and the player made the time back in a
rush, which is audible. See [Nothing that draws may change the
tempo](#nothing-that-draws-may-change-the-tempo).

`fd ms pt st` are the worst wall times, in kernel ticks of 1.37ms, for
`feed_hw`, `drain_messages`, `draw_pattern` and the header queue. A
tracker tick is about 15 of them, so double digits anywhere is the
stall.

## Keys

| | |
|---|---|
| `n` | next module |
| space | pause |
| `-` `=` | volume |
| `[` `]` | stereo separation |
| `v` | pattern grid on/off |
| `r` | sample rate |
| `s` | channel dump to the serial console |
| `q`, Escape, close icon | stop |

`r` only offers rates the board's outputs can actually carry. On a
board with S/PDIF that means an even divider of 23 or less, so it stays
at 46875 Hz and the key does nothing — 22kHz and 11kHz are below the
32kHz floor every IEC 60958 receiver is specified to, and an odd
divider makes the line ragged. See `docs/audio.md`.

`s` is off by default because `printf` over the UART is slow enough to
disturb the timing it would be reporting on.

## The engine is separate, and host-testable

`modplay.c` / `modplay.h` contain the parser, the tick engine and the
software mixer, and have **no Zeitlos dependencies at all**. `track.c`
holds the file loading, the mixer registers, the window and the keys.

```
cd sw/apps/track && make test
```

Eight groups: parse and reject, pitch, volume and panning, looping,
timing and tempo, effects, headroom, and a chunked sixty-second run.

That split has paid for itself repeatedly. Three bugs were caught on
the host before any of it reached hardware — a `uint16_t` finetune
table where 65536 wrapped to zero and made every note the same pitch, a
pan law that lost 6dB in the centre, and a leftover variable. Later,
when playback sounded wrong on hardware, the same tests **exonerated**
the engine in one run: every row measured exactly 2646 frames across a
pattern boundary, which moved the search to the app's pacing where the
bug actually was.

Supported effects: `0` arpeggio, `1`/`2` portamento, `3` tone
portamento, `4` vibrato, `5`/`6` combinations, `9` sample offset, `A`
volume slide, `B` position jump, `C` set volume, `D` pattern break,
`E1`/`E2`/`E9`/`EA`/`EB`/`EC`/`EE`, `F` speed and tempo.

## Two back ends

At startup `track` asks the hardware which it has:

```c
hw_mix = z_audio_mixer_present();
```

**Hardware mixer.** The app writes channel registers about fifty times
a second and the mixer fetches samples from main memory itself. Costs
almost no CPU. This is the normal path on every current board.

**Software mixing.** No mixer in the bitstream, so `modplay_render()`
mixes into the FIFO. Roughly 20% of the CPU for four channels at 22kHz
and about double at 44.1kHz — affordable for a player on its own, not
for a player running alongside anything else.

The fallback is not vestigial. It has twice been the thing that proved
where a fault was, by sounding correct while the hardware path did not.

## Nothing that draws may change the tempo

This is the app's one real design rule, arrived at the hard way.

Drawing is expensive: a row of text is about sixteen milliseconds of
wall time, because each glyph is a separate blitter operation and the
CPU is shared three ways. A tracker row lasts 120ms at the default
speed.

Early versions drew the whole grid in one call — 131ms, longer than a
20ms tracker tick — so every redraw blocked the audio and the player
made the time back in a burst. It was heard as the music speeding up at
the end of every pattern, because rows 59 through 4-of-the-next are the
only place in a song where the grid has blank rows to fill.

So both the grid and the header are **queues drawn one item per
main-loop iteration**:

- `pattern_step()` draws one grid row per pass, **centre-out** — the
  playing row first, then outwards. The order only matters when a
  redraw runs out of time, which it will on a module with a fast speed
  setting; centre-out drops the outer rows, which nobody notices,
  instead of the row being read.
- `header_step()` draws one header item per pass, and only items marked
  dirty. The title never changes during a module and the rate line
  changes only on a keypress, so most of the header is free.

`feed_hw()` advances the tracker to **where the clock says it should
be**, up to eight ticks, rather than one tick per call. Advancing one
per call silently tied the tempo to the loop rate.

### Why seven rows

Not aesthetics. Seven rows is 115ms of redraw against a 120ms row
period; eleven is 180ms and can never finish before the next row change
restarts it, so the last slots never get drawn at all. Raise
`PATTERN_ROWS` only after making rows cheaper.

## Memory

The module lives in a static `.bss` buffer, `MOD_MAX_FILE`, 192KB by
default:

```
make -C sw/apps/track MOD_MAX_FILE=$((1024*1024))
```

Static rather than `malloc` because the process block is sized from the
image plus a stack tier, so a 192KB heap allocation would need a tier
that does not exist. The file is read in with `fs_read_chunk` rather
than in one go.

`MOD_VOLUME` sets the startup volume, 0..100, default 35. Not
cosmetic: on a board with sigma-delta output the DAC drives headphones
directly, with no amplifier and no attenuator between the FPGA pin and
the jack.

## The mixer needs physical addresses

The hardware mixer is a bus master and issues physical addresses. This
process sees its memory through the MTU, so a pointer has to be
translated:

```c
Z_AUDIO_CH_BASE(c) = phys_of(modbuf) + h.base;
```

Hand it an untranslated pointer and it fetches from whatever physically
lives at that offset — on this SOC, the BIOS and the kernel. It plays,
and it plays the wrong memory.

## See also

`docs/audio.md` for the hardware: the mixer, the FIFO, the output
stages, and why an S/PDIF board runs at 46875 Hz.
