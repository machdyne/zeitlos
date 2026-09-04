# midi

A General MIDI file player. Reads `.MID` off the SD card and plays it
through `rtl/audio_mixer.v` using generated waveforms.

    sw/apps/midi/smf.c      Standard MIDI File parsing and sequencing
    sw/apps/midi/synth.c    waveforms, GM mapping, envelopes, voices
    sw/apps/midi/midi.c     the app
    sw/apps/midi/midi_test.c host-side tests for the two engines

    make -C sw/apps/midi
    > run midi

It is on the `wm` dock; `sw/data/icons/icon-midi.png` goes through
`gen_dock_icon_data.py` like every other dock icon.
    > run midi /AUDIO/SONG.MID

Requires a bitstream with `AUDIO_MIXER`. It refuses to run without one
and says so, because there is no fallback worth having — see below.

---

## Why this is the easiest audio app here

**A MIDI file is a score, not audio.** `sw/apps/play` was hard because
the CPU had to put 46,875 output frames a second in front of a DAC, and
this machine's measured IPC of 0.08 left about 29 instructions per
frame to do it in. That is why `play` ended up handing playback to the
mixer entirely.

This app never touches an output frame. It reads a few tens of KB off
the card **once**, then hands note-on and note-off events to eight
mixer channels a few hundred times a second. The synthesis is gateware.
CPU cost is on the order of one percent, and every interesting
constraint is musical rather than computational.

There is no streaming, no ring buffer, and the SD card is not in the
playback path at all.

---

## What it sounds like

The honest answer to "can it play arbitrary MIDI files": **yes,
recognisably** — correct notes, correct timing, correct tempo. Whether
it sounds *good* depends on three things this app controls and one it
cannot.

### The one it cannot

**There are eight hardware voices.** General MIDI files routinely want
fifteen to twenty-five notes at once. Voice stealing is audible on
dense material and no amount of software changes it. The status row
reports `voices`, `peak` and `stolen` precisely so this is visible
rather than mysterious — a climbing steal count on a busy passage is
not a fault, it is the file asking for more polyphony than the hardware
has.

### The three it does

**Envelopes.** Without them every instrument is an organ: notes start
and stop at full volume and nothing has character. A piano that decays,
a string section that swells and a plucked bass are the same three
waveforms with different ADSR. This is the single biggest thing
separating a chiptune arrangement from a broken ringtone.

**Noise for percussion.** GM channel 10 carries the backbone of most
popular music, and a snare made from a square wave is a click. A 4KB
pseudo-random table turns the drum track from missing into present.

**Timbre spread.** All 128 GM programs collapsing to one waveform is
what makes a rendering sound like mush — parts written to be
distinguishable stop being so. Seven waveforms across sixteen GM
families is not orchestral, but it keeps the bass, the lead and the pad
apart.

Realistic expectation: **a good AY-3-8910 or SID-era rendition.**
Files written for chip playback will sound excellent. Orchestral and
acoustic material will sound like a transcription, because that is what
it is.

---

## The waveform bank

Generated at startup rather than shipped: the tables are arithmetic,
and a generated table cannot drift out of step with the offsets
`synth.c` computes from the same constants.

Six periodic waveforms — sine, triangle, saw, and pulse at 50%, 25%
and 12.5% — each at **three band-limited levels**, plus one 4096-sample
noise table. 17,408 bytes at 16-bit.

### Band-limiting, and the background noise

The first version of this app produced audible broadband noise behind
the music, and this is why.

**The mixer resamples by dropping samples.** It advances a phase
accumulator and fetches whatever byte it lands on — no interpolation,
no filter. A naive saw or pulse table is band-*unlimited*: 256 samples
of hard ramp carry harmonics all the way to the table's own Nyquist,
128 of them. How many of those fit below the *output* Nyquist depends
entirely on the note:

| note | harmonics that fit |
|---|---|
| C2 (65 Hz) | 358 |
| C4 (262 Hz) | 89 |
| C5 (523 Hz) | 44 |
| C6 (1047 Hz) | 22 |
| C7 (2093 Hz) | **11** |

Everything above the limit **folds back inharmonically** — at
frequencies with no musical relationship to the note. That is not
distortion, which would at least be in tune. It is broadband noise that
tracks the music, which is exactly what it sounded like. Saw and pulse
are the worst offenders, and between them they cover strings,
ensemble, pad, guitar, organ, brass, reed and lead.

The fix is additive synthesis with a harmonic limit, at three levels
(**32 / 12 / 4** harmonics), with `note_mip()` picking the *dullest*
table that is not brighter than the note can carry. Erring toward dull
costs a little sparkle; erring toward bright puts the fold-back
straight back.

Measured largest single-sample jump in the saw across the three
levels: **13392 / 5365 / 2095**.

### Pulses come from saw differences

`pulse(x) = saw(x + d) - saw(x)`. The difference of two copies of the
same band-limited waveform is automatically band-limited to the same
harmonic **and automatically free of DC**.

That second property matters. The first version built pulses as literal
high/low levels, and a 25% pulse swinging equally either way spends
three quarters of its period negative — a large DC offset. On one chip
voice that is harmless; here **eight sum into one accumulator and one
DAC**, so the offsets add into a permanent pull on the output plus a
click at every loop point. Deriving the pulse from a saw makes the
problem impossible rather than merely fixed.

One trap in doing it that way: the Fourier sum `sum sin(nx)/n` is a
*falling* ramp, so subtracting in the wrong order gives a pulse of duty
`1 - d`. Asking for 25% got 75%. Sonically identical — a pulse and its
inverse have the same harmonic content — but a table named `PULSE25`
that is 75% is a trap for whoever tunes the GM mapping next.

### 8 or 16 bit

Chosen at startup from `z_audio_mixer_fmt16()` (`CONFIG` bit 13, see
`docs/audio.md`); an older bitstream gets an 8-bit bank and the app
says so.

16-bit is **not** cosmetic, and it is not independent of the
band-limiting either. A band-limited saw has a far lower peak-to-RMS
ratio than a hard ramp, so quantising it to 8 bits throws away
proportionally much more of it. The two changes belong together:
band-limiting removes the aliasing, and 16 bits stops the cure being
audible as quantisation.

Note that `CH_STEP` counts **bytes**, so a 16-bit table advances twice
as far per sample. Forgetting that plays everything an octave low —
which sounds like a broken tuning table rather than a units error.

**The noise table is long and its length is checked.** A short looped
noise table does not sound like noise; it sounds like a buzzy tone at
the loop frequency, because that is exactly what it is. There is a test
that scans every period up to 256 bytes and fails if any of them
matches more than 20% of the time.

Pitch is nearly free: `CH_STEP` is a phase increment in bytes, so
playing a 256-byte table at N bytes per output frame produces a tone of
`N * out_hz / 256` Hz. The oscillator is one register write.

---

## Instrument mapping

One entry per GM family of eight programs — sixteen families, each with
a waveform and an ADSR envelope. Percussion is a separate range map
covering kick, snare, toms, hats and cymbals, with everything else
landing on noise rather than falling silent.

Percussion deliberately has **no sustain**: a drum decays to nothing
whether or not the key is still down. Modelling it as a one-stage decay
rather than forcing it through ADSR is both simpler and correct — a
snare that sustains while a note-off is pending is a snare that
rattles.

Replacing this with real sampled instruments is the obvious next step
and needs no change to `smf.c` at all: `mod_load()` in
`sw/apps/track/modplay.c` already produces `mod_sample_t` with data,
length, loop points and volume, which is exactly a wavetable
instrument. A `.mod` file makes a perfectly good soundbank, and the
parser for it already exists and is already tested.

---

## Timing

`division` is ticks per quarter note; tempo comes from meta event
`0x51` in microseconds per quarter note, defaulting to 500,000
(120 BPM) as the spec requires when a file never sets one — and plenty
do not.

The clock is `z_uptime_ticks()`, so 1366 µs of resolution. That is
ample: a sixteenth note at 120 BPM is 125 ms, so a tick is about one
percent of the shortest thing in an ordinary score. `smf.c` carries the
**fractional** tick remainder between calls, so the granularity costs
jitter rather than drift — the difference between a file that ends in
time and one that loses a bar over three minutes.

Advancing is **clamped to 64 ticks** per pass. The elapsed time is wall
clock and this process can be away for a long time — a file dialog, a
redraw storm, another app misbehaving. Without the clamp, coming back
after a second would advance the score a second in one step and fire
every event in it at once. The music pauses for the missing interval
instead: a gap is a glitch, a chord of forty simultaneous notes is a
fright.

---

## Voice allocation

Order matters, and it is the difference between "some notes are
missing" and "the melody keeps dropping out":

1. a free voice
2. the quietest voice already in **release** — on its way out anyway,
   and cutting a decaying tail is nearly inaudible
3. the quietest sounding voice, oldest breaking the tie

Stealing the *oldest sounding* voice instead is the obvious policy and
is worse: in a held chord under a moving melody, the oldest voice is
usually a chord tone that is still needed, while the quietest is
whatever has already decayed furthest.

---

## What is supported

| | |
|---|---|
| SMF format 0 | yes |
| SMF format 1 | yes — tracks merged by timestamp |
| SMF format 2 | **refused** |
| SMPTE division | **refused** |
| running status | yes |
| note-on velocity 0 as note-off | yes |
| tempo changes | yes |
| program change | yes |
| pitch bend | yes, ±2 semitones, quarter-tone quantised |
| CC 7 / 11 volume, expression | yes |
| CC 10 pan | yes |
| CC 120 / 123 all notes off | yes |
| aftertouch, sysex, other CCs | parsed and dropped |

Format 2's tracks are *independent* sequences rather than parallel
ones, so "play the file" has no single meaning and guessing one is
worse than saying so. SMPTE division needs a completely different
clock; treating it as PPQN would play at an arbitrary wrong tempo,
which reads as a sequencer bug rather than an unsupported mode.

Unknown chunks between the header and the tracks are **skipped**, not
treated as errors — the spec says so and real files carry them. A
truncated final track is clamped and played rather than refusing the
whole file, because that is what a stream that was cut short leaves
behind.

---

## It opens as an instrument

The app starts with a file **loaded but not playing**, and the
keyboard live. Space starts the file.

Auto-playing on launch is the wrong default for something you might
have opened to play rather than to listen to, and it is a worse
surprise besides: the first thing it would do is take the audio device
and start making noise. Once started, a finished track still advances
to the next one — `open_file()` leaves a file stopped, and the
end-of-track path overrides that.

**Play-along works.** The keyboard is on MIDI channel 16, which is its
alone, so pausing or stopping the file releases only the file's notes
and leaves whatever is under the player's fingers sounding. Playing
over a paused backing track is a normal thing to want.

Pause deliberately does **not** touch the audio block's `EN` bit. It
used to, and that bit is the DAC's output enable for the whole device
rather than a property of the file — so the sequencer stopped, the
keyboard kept triggering voices, the mixer kept mixing them, and none
of it reached the output. Pressing pause silenced the instrument. That
was correct before there was a keyboard, when "paused" and "silent"
meant the same thing; now they do not. `z_audio_start()` sets that bit
at startup and `z_audio_stop()` clears it on the way out, and nothing
in between goes near it. Silence is a frame of zeros.

## Where the files live

`/AUDIO`, then `/audio`, then the root — the **same directory**
`sw/apps/track` and `sw/apps/play` use. One place for everything that
makes a sound, rather than a folder per app: a card with `AUDIO`,
`MIDI` and `MOD` directories makes you remember which app wanted which,
and the only thing that actually distinguishes them is the extension.

`.MOD` and `.WAV` files sitting alongside are simply not listed here,
and this app's `.MID` files are not listed by the other two. The filter
takes the **last** dot and requires exactly `mid` or `midi`, so
`SONG.mid.wav` is correctly skipped rather than matched on the `.mid`
in the middle.

Both letter cases are tried because FatFs is built with `FF_USE_LFN 0`
and matches 8.3 names case-insensitively, but the path handed to
`f_opendir()` is taken as written.

## The keyboard

Two rows of the computer keyboard laid out as two octaves, with the
letter drawn on each key:

    upper octave    Q W E R T Y U I O P     sharps  2 3   5 6 7   9 0
    lower octave    Z X C V B N M , . /     sharps  S D   G H J   L ;

### It is a real standard, and this matches it exactly

There is a de facto standard for this and it is the **Fasttracker II /
Impulse Tracker** layout, carried forward by OpenMPT, MilkyTracker,
Schism, Renoise and — with variations — Ableton Live, FL Studio, LMMS
and Reaper. It has been the same since the early nineties.

The map in `kbd_map[]` is that layout, checked entry by entry against
the reference rather than reproduced from memory:

    lower  z=C s=C# x=D d=D# c=E v=F g=F# b=G h=G# n=A j=A# m=B
           ,=C l=C# .=D ;=D# /=E
    upper  q=C 2=C# w=D 3=D# e=E r=F 5=F# t=G 6=G# y=A 7=A# u=B
           i=C 9=C# o=D 0=D# p=E

All 34 entries match. Anyone who has typed music before already knows
it, and anyone who has not can read it off the screen.

**A note for non-US keyboards.** `z_kbd_usage_to_keysym()` maps USB HID
usage codes, which are physical key POSITIONS, to US letters. So the
layout keeps its shape on a QWERTZ or AZERTY board — the key at the
bottom-left of the letter block is still low C — but the letter drawn
on screen is the US one and will not match the keycap. On a German
board, low C is the key labelled `Y`.

The two rows **deliberately overlap**: QWERTY starts an octave above
ZXCV, so semitones 12–16 are reachable from both (`,` and `q` are the
same note). That is how the original layout works and it is not a bug
to be tidied up — it lets a two-handed player cross rows without a gap.

Up and Down shift the whole keyboard an octave, releasing anything held
first: the note a key is holding would otherwise never get its
note-off, because the key now means something different. `[` and `]`
change the instrument.

**The keys can be clicked too.** Held rather than triggered: the note
lasts as long as the button is down, the same as a computer key, so the
envelope's release means something — a click that fired a fixed-length
blip would make every instrument sound the same regardless of its ADSR.
Dragging across the keyboard slides from note to note, which falls out
of tracking which key the mouse holds rather than being written on
purpose. Black keys are hit-tested first because they are drawn over
the whites and overlap them, so a click in the overlap belongs to the
black key — which is what a real keyboard does and what the picture on
screen says.

Key **releases** are what make this playable rather than a fixed-length
blip per press, and `wm` delivers them
(`Z_WM_UNPACK_KEY_PRESSED`). Key **repeat** is suppressed by tracking
which keys are down — retriggering on every repeat would be a machine
gun, not a held note.

Console mode is transport only: `hid_read_key()` has no release edge,
so notes would sustain forever.

## Controls

The note keys consume nearly the whole alphanumeric area, so the
transport lives on keys the layout does not touch.

| | |
|---|---|
| ZXCV… / QWER… | play notes — see above |
| Up / Down | shift the keyboard an octave |
| `[` / `]` | keyboard instrument |
| space | start / pause the file |
| Enter | stop, rewind |
| Tab | open a file (also the titlebar icon) |
| Left / Right | previous / next file |
| `-` / `=` | volume |
| F1 | help |
| F2 | mute / unmute drums |
| F3 | force mono — a diagnostic, see below |
| F4 | panic — all notes off |
| Escape, close icon | quit |

Per-channel muting on `1`..`9` and `0` is **gone**: those are black
keys now. It was a diagnostic aid and the keyboard is worth more.

Sixteen channel activity bars sit between the status rows and the
keyboard, refreshed at about 10 Hz. Twelve pixels tall — they read fine
at that, being an activity indicator rather than a measurement.

### The window is sized to the layout, not the other way round

It was 344x236 and **nearly half of it was empty**. The text rows were
placed from the top and the keyboard and buttons from the bottom, with
the meters taking whatever was left in between — capped, so every pixel
of window height past the cap became a gap between the meters and the
keyboard. About 95 of them.

The layout now packs downwards and the window is exactly what it needs:

    text rows    2..47
    meters      49..61
    keyboard    64..94
    buttons     97..115      content needed: 117

    304x134, half the old area
    41% of a 512x384 screen -> 21%

Packing downwards is what stops this recurring: slack in a bigger
window ends up in one place at the bottom, where it is obvious, rather
than as a hole in the middle. Screen space is scarce at this
resolution and a window that is mostly nothing is not a neutral
default.

`layout()` computes the same total it places against, so a window
shorter than the layout needs degrades in reverse order of importance —
the keyboard goes first, then the meters. The buttons and status rows
always stay. A muted channel shows a floor line rather than nothing, so
"silent because muted" and "silent because nothing is playing" are
distinguishable at a glance.

Every fill goes through the **hardware blitter** — `z_win_fill_rect()`
does, and the Makefile passes `-DZ_GFX_HW_BLIT` so the text does too.
That is not optional at this size: the software path is a per-pixel
read-modify-write of VRAM whose cost is proportional to *area*, which
`sw/common/zwin.c` records as roughly three seconds to clear a 288x216
dialog.

The blitter is not free either, so **only changed bars are drawn**. The
first version repainted all sixteen unconditionally: thirty-two fills a
refresh, each re-deriving the window's clip rectangle, for a display
where a typical file uses two or three channels. Tracking the last
height drawn takes ~320 fills a second down to ~30.

### Labels need an explicit background

Key letters go through `z_win_draw_text2()`, not `z_win_draw_text()`.

The one-colour call hard-wires the glyph background to 0
(`z_fb_draw_char()` in `zgfx.c`) because the blitter paints a **solid
cell** rather than just ink. So a colour-0 letter on a light key is ink
of 0 on a cell of 0: not merely invisible, the cell *erases the key
underneath it*. That is exactly how the black keys came out with no
letters on them.

`z_win_draw_text2()` is new — `z_fb_draw_text2()` already existed and
the window-level wrapper did not, which is why nothing had hit this
before: everything else in the tree draws light text on the window
background. Anything drawing a label on a filled shape wants it.

The keyboard follows the same rule as the meters: a note on or off
repaints **one key**, not twenty-nine keys and their letters. A black key's white
neighbours are repainted after it, so the overlap is not left
half-erased, and a full draw paints all the whites before any black —
painting a black key second is what makes the overlap look right
rather than leaving a notch.

A changed meter bar is also **one** fill rather than two — growing paints only
the new part, shrinking erases only the part that left. Clearing the
column and redrawing the bar is twice the traffic and it *flickers*,
because there is a window between the two fills where the bar is not
there at all.

Per-channel muting is more useful here than it sounds: it is the
fastest way to find out whether a file sounds thin because of voice
stealing or because one channel is mapped to a bad waveform.

---

## Panning, and the `y` key

The pan law is **exactly symmetric at centre by construction**: the
side a note is panned toward stays at full scale and the other side is
attenuated, so centre is full on both and nothing is ever boosted above
unity.

The first version was not. It computed `l = g * (127 - pan) >> 6`
against `r = g * pan >> 6`, and at the default centre pan of 64 that is
63/64 on the left against 64/64 on the right. At full gain the
difference is 1.6% and inaudible — but **the two sides quantise at
different points all the way down the envelope**:

| gain | left | right | difference |
|---|---|---|---|
| 255 | 251 | 255 | 1.6% |
| 16 | 15 | 16 | 6.2% |
| 8 | 7 | 8 | 12.5% |
| 4 | 3 | 4 | **25%** |
| 2 | 1 | 2 | **50%** |

So the two channels decayed differently, and the right was
consistently the louder and more exact of the two. It also boosted a
hard-panned voice by 127/64 and clipped it.

There is now a test asserting `gain_l == gain_r` at centre pan across
five velocity levels, because this is only obviously wrong at the quiet
end.

**`y` forces both mixer gains to the left value.** It is a diagnostic,
not a feature: if something is audible on one channel only, this
collapses the two sides to identical numbers. If the fault persists
with mono on, nothing in the pan or gain arithmetic is responsible and
the difference is downstream — in the mixer or the analogue output. One
keystroke splits the search in half.

## Mixer writes are rate-limited, deliberately

The mixer's per-channel config lives in **distributed RAM**
(`TRELLIS_DPR16X4`), and its sequencer reads `ch_ctrl[seq]` once per
channel per frame — **375,000 reads a second** at eight channels and
46875 Hz. A write landing on the same cycle as a read gives undefined
read data.

The first version wrote all eight `CH_CTRL` words every main-loop pass
whether anything had changed or not: about **5,900 writes a second**.

| app | CH_CTRL writes/sec |
|---|---|
| `play` | ~0 — set once per file |
| `track` | ~200 |
| `midi`, first version | **~5,900** |

`track` and `play` have therefore never exercised this. Two changes
bring it down: the envelope advances on its own 5 ms clock rather than
the loop rate (accumulating the remainder, so timing is unchanged —
only the write rate falls), and a `CH_CTRL` word identical to the last
one written is not written again.

A **retrigger is always written**, identical word or not: the same note
played twice in a row produces the same `CH_CTRL`, and skipping it
would silently drop the second note. Note-ons also bypass the 5 ms
clock, because a note late by 5 ms is audible in a way a gain step is
not.

## Things that are easy to get wrong

**The waveform bank is at a physical address.** The mixer is a bus
master and this process sees memory through the MTU. `phys_of()` —
same trap and same fix as `sw/apps/track` and `sw/apps/play`.

**Gain updates must set `EN` without `TRIG`.** `TRIG` restarts a
channel from its offset field. A gain update that also retriggered
would restart the waveform every envelope tick, a few hundred times a
second — that is not a note, it is a buzz at the tick rate.

**Everything loops, including the noise.** A one-shot voice would stop
when the table ran out, which for a 256-byte table at middle C is about
six milliseconds. The envelope ends a note here, not the sample length.

**Note-off must not stop at the first match.** The same note can
legitimately be sounding twice on one channel if the file retriggered
it without an intervening note-off, and leaving one of them on is how a
file ends with a note still droning.

**Meta and sysex do not set running status.** Only channel messages
do. Letting `0xFF` become the running status turns the rest of a track
into garbage.

**It takes the audio device.** On startup this clears every mixer
channel, stopping `track` and `play` dead. There is still no
arbitration for the audio block, and a player that left `MIXEN` where
it found it would program eight channels nothing was listening to and
produce silence with no indication why.

---

## Testing

    cd sw/apps/midi && make test

84 checks across seven groups: header parsing and refusals, events and
running status, timing against a stopwatch, format 1 track merging,
pitch across the full MIDI range, the band-limited waveform bank, and
voices, envelopes and panning.

Built with the **host** compiler. `smf.c` and `synth.c` include nothing
from `sw/common` and touch no hardware — the same discipline, for the
same reason, as `sw/apps/track/modplay.c`.

**Fixtures are built byte by byte in the test file**, not shipped. A
real MIDI file is somebody's composition, and a fixture whose correct
output nobody can state is not a test. Every file is a few events long
and every expected result is arithmetic.

### Bugs it caught

1. **The pitch calculation overflowed above about 2 kHz.** The direct
   form, `WAVE_LEN * f << 14 / out_hz`, is 5.3e10 at the top of the
   MIDI range. It would have worked in any test written around middle
   C. Split into quotient and remainder it is exact, 32-bit safe, and
   accurate to 2 cents at the very bottom note and better than 0.05%
   everywhere musical.

2. **The narrow pulse waveforms had a large DC offset** — see the
   waveform bank section. Eight voices summing into one DAC makes that
   a real fault rather than a curiosity.

4. **The pulse duty came out inverted** once pulses were derived from
   saw differences: `PULSE25` was 75%. Caught by a test that asserts
   the three duties are in descending order.

5. **The band-limiting check was measuring the wrong thing.** It used
   the *mean* absolute sample-to-sample difference, and the total
   variation of any normalised single-peaked waveform is about twice
   its peak-to-peak whatever its harmonic content — so a 32-harmonic
   saw and a 4-harmonic one measured within 20% of each other and the
   check would have passed on tables that were not band-limited at
   all. The *largest* jump is what a hard edge actually is.

3. **Nothing, in the sine table** — but the test that checked it was
   wrong, and the failure looked exactly like a broken quarter-wave
   reflection. It compared signs over `i = 1..255`; the table's start
   and half-way points are exactly 0, which is not negative, so the
   positive-to-zero edge did not count and the 255-to-0 edge was never
   examined. Worth recording because "sine crosses zero once" is a
   frightening thing to read.

---

## A note on S/PDIF

If you hear static behind the music on one channel, it is probably not
this app. `rtl/audio_spdif.v` had a bug that corrupted one subframe on
every frame, consistently, on whichever channel the phase happened to
land in — see `docs/audio.md`, "S/PDIF sample staging". It affected
every app that used the optical output and was worst on sparse material
like a simple MIDI file, where there is little to mask it.

It was found from this app and fixed in the encoder. Nothing in
`midi` changed.

## MIDI hardware input and output

Not built, but worth recording as feasible.

MIDI is 31250 baud 8N1 async serial — a UART, and the SOC already has
`UART1` as a build option with its own Wishbone slot and pins. The
divisor works out **exactly**:

    clk / (16 * baud)    MIDI   31250 ->  96.0000   exact
                         console 115200 ->  26.0417  fractional

Input cannot be bit-banged: the bit time is 32us under a preemptive
scheduler. Output could be, but there is no reason to.

`synth.c` takes `smf_event_t` and knows nothing about where events come
from, so a live parser feeding it the same struct turns this into a
playable synth without touching the synth at all. Sequencing a file out
to external gear is the mirror image: `smf.c` already does the timing,
and the byte writer replaces `synth_event()`.

Three things a live byte stream needs that a file does not: real-time
messages (`0xF8`, `0xFA`, `0xFC`) can appear *inside* another message;
Active Sensing (`0xFE`) floods in every 300ms and must not become the
running status; and SysEx is unbounded and has to be skipped without a
buffer.

The off-chip part is the only piece that cannot live in the FPGA: MIDI
IN must be opto-isolated per the spec.

## Still to do

**Sampled instruments.** The soundbank is the quality ceiling and
replacing it needs no sequencer changes. A `.mod` file is the obvious
first bank: the format is 8-bit signed with loop points, which is what
the mixer eats natively, and `modplay.c` already parses it.

**16-bit instruments.** The mixer gained `CH_CTRL[18]` (see
`docs/audio.md`), so a sampled bank could be 16-bit at the cost of
double the RAM. Meaningless for generated waveforms, which have nothing
below eight bits to give.

**More than eight voices.** Not a software problem. `AUDIO_MIXER_CH_BITS`
is a build parameter and the mixer's per-channel cost is small — the
expense is the shared sequencer datapath — so sixteen channels is worth
measuring if polyphony proves to be the binding limit.

**MIDI input.** A real MIDI port is 31250 baud on a UART. Nothing in
this app would need to change except where events come from.

**An audio server.** Three apps now want the mixer exclusively.
