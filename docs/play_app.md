# play

A streaming audio player. Reads WAV, `.au` and raw PCM off the SD card
and plays it without ever holding more than a fraction of the file in
memory.

    sw/apps/play/adec.c        container parsing and codecs
    sw/apps/play/stream.c      ring buffer and resampler (software path)
    sw/apps/play/play.c        the app, and the mixer-driven path
    sw/apps/play/prof.c        cycle-level phase profiler
    sw/apps/play/play_test.c   host-side tests for adec.c and stream.c
    tools/mkaudio.py           host-side converter

    make -C sw/apps/play
    > run play

No flags are needed; every knob has a default. `PLAY_PROF=1` adds the
profiler.

---

## Read this first

This document has been rewritten against hardware measurements, and
**the design it now describes is not the one it originally argued
for.** Two of the original conclusions were wrong:

- SD bandwidth was expected to be the constraint. It is 4% of what the
  app spends.
- IMA ADPCM was recommended as the format of choice. It is the one
  format that cannot play in real time.

The reasoning that produced those is kept below, under
[What the measurements changed](#what-the-measurements-changed),
because the mechanism was right and only the magnitudes were wrong —
and the same mistake is available to anyone estimating the next
subsystem from the same starting assumptions.

---

## Two playback paths

| | | |
|---|---|---|
| **mixer** | `rtl/audio_mixer.v` reads a ring buffer in main memory as a bus master | the real one |
| **software** | decode, resample, push to the audio FIFO | reference and fallback |

The path is chosen **per file**, automatically, on whether the format
can be converted to 8-bit cheaply enough. In practice that means
everything except IMA ADPCM goes to the mixer.

### Which one did this file get?

On the console, at every file open:

    play: /AUDIO/SONG.WAV  PCM8 2ch 22050Hz -> MIXER

    play: /AUDIO/OLD.WAV   IMA ADPCM 2ch 22050Hz -> FIFO -- SOFTWARE PATH
    play: not using the mixer: IMA: 5.5x the CPU of wav8 for identical output

And at the head of every profiler report: `-- play prof [MIXER]: ...`

In the window the format row reads `MIXER`, `MIXER 8b` or `SOFT`, and
when the software path is in use the diagnostics row is replaced by the
reason.

**None of that was there at first, and its absence cost three rounds of
hardware profiling.** A card still holding IMA files from an earlier
encoding run produced profiles that looked exactly like the mixer doing
nothing — when the mixer had never been engaged at all. A program that
picks between two strategies must say which one it picked, somewhere
nobody can miss. The giveaway in a profile is the presence of `render`
and `decode`: neither exists on the mixer path.

`m` forces the software path on a file the mixer could have taken, so
the two can be compared on the same material in one session rather than
across two runs with half the variables moving.

---

## What the measurements changed

### The starting assumptions

`spim.v` moves one byte per MMIO write / poll / read cycle. Its own
header estimates ~48 CPU cycles per byte, so SD bandwidth on this board
is CPU, not waiting. From that, two things looked obvious: streaming
would be SD-bound, and a 4:1 codec would buy back most of the cost.

### What hardware actually said

Measured with `PLAY_PROF=1` on a 46875 Hz S/PDIF board at a 35% CPU
share — a budget of **357 cycles per output frame**:

| phase | cyc/out frame | share needed at real time |
|---|---|---|
| resample | ~640 | 177% |
| decode (IMA) | ~490 | 139% |
| push | ~110 | 30% |
| **pump (SD)** | **~66** | **19%** |
| UI + profiler | ~180 | ~6% |
| **total** | **1493** | **4.2x over** |

Playback ran at 23% of real time.

**SD was never the problem.** It measured 49 cycles/byte, close to the
documented estimate, and came to 4% of what the app spent. Resample and
decode each *individually* exceeded the entire CPU share.

Two further numbers made the shape clear:

- **Measured IPC is 0.08**, less than half the 0.172 `docs/muldiv.md`
  records from the testbench — no data cache, and main memory on the
  other side of every load. So 357 cycles is about **29 instructions
  per output frame**, for everything.
- 29 instructions is less than it takes to interpolate two channels,
  store them, reload them, pack them and issue one MMIO write. **With
  zero decode and zero SD.**

Optimising the IMA decoder 2.2x (2320 to 900 cycles per source frame,
by hoisting the predictor state out of memory into registers) moved
playback from 12% of real time to 23% and no further. That is the
correct amount of improvement for the correct fix, and still nowhere
near enough.

### The structural problem

The CPU was visiting **all 46,875 output frames a second.** No
arrangement of software fits 29 instructions per frame. The fix is to
stop visiting them.

### And therefore, IMA

The original case for IMA was that it halves SD traffic. It does. The
measured trade, per sample delivered to the mixer's 8-bit ring:

| | SD | decode/convert | total |
|---|---|---|---|
| 8-bit WAV | 49 cyc | ~37 (one XOR) | **86** |
| IMA ADPCM | 24 cyc | ~450 | **474** |

**IMA saves 24 cycles of SD and spends 413 cycles of CPU doing it.**
5.5x more expensive, for half the file size.

And the second half is worse: the mixer truncates to 8 bits either way,
so IMA's ~12-bit resolution is discarded before it reaches the DAC. It
costs 5.5x the CPU to deliver audio bit-for-bit no better than what an
8-bit file delivers.

There is no operating point where it wins on this machine — not at low
rates, not in mono. The decode always dominates. It is kept because the
decoder is correct, tested and free to carry, and because a board with
a data cache or `spim.v` burst mode would move the numbers. Not because
it should be used. `mkaudio.py` prints the table above if you ask for
it.

---

## Formats

Everything ends up 8-bit, because the mixer fetches one signed byte per
channel per frame. What decides the path is whether the **conversion**
is cheap enough at the source rate.

| format | ring | conversion |
|---|---|---|
| WAV PCM 8-bit | 8-bit | XOR per byte |
| WAV PCM 16-bit LE | **16-bit** | **`memcpy`** |
| `.au` PCM 16-bit BE | **16-bit** | byte swap |
| WAV / `.au` u-law, A-law | **16-bit** | one table lookup |
| `.raw` / `.pcm` | as configured | — |
| **WAV IMA ADPCM** | software path | full decode — ~450 cyc/sample, see above |

...on a bitstream whose mixer has `CH_CTRL[18]`. Without it every row
falls back to an 8-bit ring: 16-bit takes the high byte, companded
formats truncate their expansion table. Checked with
`z_audio_mixer_fmt16()`, never assumed — an older mixer *accepts* the
bit and ignores it, so a 16-bit ring would play as 8-bit garbage at
half pitch, which is worse than not offering the feature.

**An 8-bit source stays 8-bit.** Widening it would double the ring
traffic to carry a zero byte and halve how much audio the ring holds,
for nothing.

The 16-bit path is not only better, it is **cheaper**: the mixer reads
a halfword out of a 32-bit word little-endian, which is exactly how a
16-bit WAV already sits on the card, so the conversion is a `memcpy`
rather than the per-sample strided high-byte copy it replaces. Same SD
traffic either way — those bytes were already being read and half of
them discarded.

What it costs is ring depth: 32 KB is 372 ms of 22050 Hz 8-bit stereo
and 186 ms of the same material at 16-bit. Still ample.

The format row distinguishes three cases, and the third is the one
worth seeing:

| | |
|---|---|
| `MIXER 16b` | full source resolution |
| `MIXER 8b` | source was 8-bit; nothing lost |
| `MIXER 8b!` | source had more than 8 bits and **they were thrown away** — an older bitstream |

The u-law and A-law tables are derived from `adec`'s own `tab8` with
gain forced to unity — not from a second copy of the G.711 arithmetic,
so the two cannot disagree about the companding law, and not with gain
folded in, because in mixer mode volume is the mixer's job and applying
it twice would be quietly wrong.

### What is not here

**MP3.** Patents expired in 2017 and `minimp3` is CC0, so licensing is
not the obstacle. Fixed-point decoders of the `libmad` generation need
roughly 25 MIPS for 44.1 kHz stereo. This board delivers about 7 across
every process. Off by an order of magnitude — and that was before IMA,
at a fraction of MP3's work, turned out to be too slow.

**FLAC.** Integer-only, which removes the usual objection, but Rice
decoding plus LPC is in the same range and a 4096-sample block must be
buffered whole.

**Vorbis, Opus, AAC.** Floating point, 100 KB+ of state.

There is a structural reason as well as an arithmetic one, and it
applies to anything proposed later: a codec in `adec.c` must decode
from an arbitrary block boundary, look back no further than one block,
and keep its state inside `adec_t`. Those three constraints are what
make seeking work and what let the ring be a plain byte ring. MP3 and
FLAC fail all three.

### Sample rates

The DAC rate is not negotiable and not round: `CLK_HZ / (64 * RATE)` —
44117.6 Hz at the default divider, 46875 Hz on an S/PDIF board. **No
file's rate ever equals it**, not even a 44100 Hz one, which is 0.04%
off.

On the mixer path this is free: `CH_STEP` is a phase increment and the
hardware does the conversion. On the software path it goes through a
32-bit accumulator with 14 fractional bits — the same split
`modplay.c` uses, deliberately, so that reading one is reading both.

`play` starts from **whatever rate the board came up with** rather than
a constant. `AUDIO_RATE_RESET` in `boards.vh` exists so a board can
pick a rate its outputs can carry, and an app that overwrites it throws
that away. On an S/PDIF board nothing below 32 kHz will lock at the
receiver.

Lowering the output rate is a real lever on the software path, where
everything scales per output frame — `RATE 22` gives 34090 Hz and cuts
the work 27%. On the mixer path it changes nothing, because the CPU no
longer sees output frames.

---

## The mixer path

`rtl/audio_mixer.v` walks memory as a bus master, doing phase
accumulation in 18.14, rate conversion, per-side gain and looping — in
gateware, on a clock that cannot be preempted, at about 11 cycles per
channel per frame. Point a channel at a ring buffer (`LOOPST` 0,
`LOOPLEN` = buffer size) and it is a playback engine.

The one thing software could not previously learn was **how much of
that ring had been consumed**. That is `MIXPOS`; see `docs/audio.md`
for the register, why it is registered rather than combinational, and
its measured cost (+160 `TRELLIS_COMB`, +32 FF, no BRAM, no DSP).

The main loop collapses to one call:

    if (mix_mode) mixer_fill();     /* read MIXPOS, read SD into the ring */

No render, no push, no interleaved FIFO top-up, and nothing is timing
critical — the mixer does not care when this process is next scheduled.

### Expected cost

At 22050 Hz stereo and a 35% share (16.8M cycles/s available):

| | SD | convert | UI | total | % of share |
|---|---|---|---|---|---|
| 8-bit | 2.2M | 1.6M | 1.0M | 4.8M | **29%** |
| u-law | 2.2M | 2.2M | 1.0M | 5.4M | 32% |
| 16-bit | 4.3M | 1.6M | 1.0M | 7.0M | 41% |

From 4.2x over budget to roughly a third of it.

### The details that are easy to get wrong

**The ring is at a physical address.** The mixer is a bus master and
this process sees memory through the MTU. `phys_of()` — the same trap
and the same fix as `track.c`, and the same class of mistake as the
`k_proc_create()` bug in `docs/app_runtime.md`.

**Both channels share one interleaved ring, and channel 1's `BASE` is
one byte further in.** That is a workaround for a real constraint:
`CH_CTRL`'s `TRIG` offset field is in units of **256 bytes**, so there
is no way to start a channel at byte 1. Offsetting `BASE` costs nothing
and keeps the ring one contiguous span the filesystem reads straight
into. It also means channel 1's last fetch before a wrap lands one byte
past the loop length — hence four bytes of slack on the array.

**`CH_STEP` is in bytes per output frame**, so an interleaved stereo
stream steps *two* bytes per source frame and the ratio is doubled.

**Mono uses ONE channel feeding both sides.** Two channels reading the
same byte would sum to double amplitude, and halving the gain to
compensate throws away a bit of resolution for nothing.

**WAV 8-bit PCM is unsigned; the mixer sign-extends.** One XOR per
byte. Getting it wrong does not produce noise — it produces correct
audio with a large DC offset, inaudible on a small speaker and a warm
resistor on headphones.

**Volume must set `EN` without `TRIG`.** `TRIG` restarts the sample,
which for a ring buffer means jumping the read position back to zero
mid-stream. `zaudio.h` calls this distinction out; this is the call
site that most needs it.

**Prefill before enabling.** The mixer starts consuming the instant
`MIXEN` goes up and has no notion of "not ready yet", so starting on an
empty ring plays a bufferful of whatever `.bss` held. It is also zeroed
first, so a partial prefill at the end of a short file cannot loop
uninitialised memory for as long as the track runs.

**Pause disables `MIXEN`.** Muting the output alone would leave the
channels advancing, so a long pause silently walks the ring forward.

**The conversion stages through `ring[]`**, the software path's 32 KB
buffer, which is idle whenever this path runs. The 2:1 conversions need
twice as many source bytes as they produce, so in-place is wrong for
them — and a buffer that is sometimes in-place and sometimes not is how
a subtle overlap bug gets written.

### What it costs

**8 bits, about 48 dB.** The mixer fetches one signed byte per channel
per frame — what it was built for. Widening it to 16 needs a halfword
select, a wider accumulator (24 bits today, and eight channels of 16x8
products overflow it) and a different output shift. Real work, worth
doing only if the 8-bit result is audibly short.

**About 12 dB of level.** The output stage is
`sum(sample * gain) * mixvol >> 10`, sized so *eight* channels at full
gain reach full scale. Two channels top out near a quarter of it. On a
1-bit sigma-delta driving headphones with no attenuator that is not
obviously the wrong end of the range — but it is an audible difference
when A/B-ing with `m`.

**A different failure mode.** The mixer never underruns; it replays
whatever is still in the ring. There is no sticky `UNDERRUN` bit to
catch it, so `mixer_fill()` compares its write head against `MIXPOS`
and counts starvations itself. That count is what `sv` shows.

**It takes the audio device.** On startup `play` clears `MIXEN` and
every mixer channel. If `track` is running, its music stops. There is
no arbitration for the audio block in this system, and a player that
left `MIXEN` where it found it would push frames into a FIFO the DAC is
not reading, produce silence, and give no indication why. It matters
for a second reason too: the mixer issues *physical* addresses, so if
`track` was killed rather than exiting, its channels are still fetching
from memory the kernel has since handed to something else.

---

## The software path

Kept as the reference implementation and the fallback for IMA. It is
not a usable playback mode on this hardware — it runs at about a
quarter of real time — but it is the path the host tests exercise, and
`m` makes it comparable against the mixer on identical material.

    SD --2KB chunks--> [ 32KB byte ring ] --> adec --> [ scratch ]
        --> resample --> [ 256-frame block ] --> hw FIFO --> DAC

The loop interleaves a FIFO top-up either side of the one blocking
call, because the FIFO holds 1024 frames — 23 ms at 44.1 kHz — and that
is the whole margin. `PLAY_CHUNK` and `PLAY_FEED_MAX` are **latency
budgets, not throughput knobs**: the same distinction `track.c`'s
`FEED_BLOCKS` comment makes. `wm`'s `repair_region()` blocks on a
redraw ack, so an app that disappears into a push loop for 40 ms
freezes the desktop for 40 ms.

**The ring holds file bytes, not decoded frames.** Buffering decoded
audio is the obvious simplification and a real loss: an ADPCM file
expands 4x on decode, so 32 KB of decoded audio absorbs a quarter as
much SD jitter as 32 KB of file.

`feed_fifo()` declines to render fewer than 64 frames unless the FIFO
is genuinely draining. A run where the FIFO stayed near full did 1515
passes for 32512 frames — 21 frames a pass — and the fixed per-pass
overhead came to 180 cycles per output frame against a 350-cycle
budget. The same workload with 85 large passes ran a third faster.

### Two failure modes, reported separately

They sound identical and want opposite fixes.

**`sv`** — the renderer had room in the FIFO and nothing to put there.
The *card* could not keep ahead. A smaller stream or a larger ring.

**`ur`** — the hardware's sticky `UNDERRUN` bit: the *FIFO* ran dry.
Possible with a healthy ring if something blocked the loop for more
than 23 ms. Less work in the loop.

`docs/audio.md` draws the same distinction for the mixer ("buffer too
small" vs "mixer too slow") for the same reason: a single "it glitched"
indicator sends you looking in the wrong place half the time.

`sv` deliberately does **not** count the short render at end of file.
The first version did, which put a permanent `sv1` on every track that
played flawlessly — and a number that is always 1 is a number nobody
reads.

---

## Controls

| | |
|---|---|
| space | play / pause |
| `s` | stop (rewind to the start) |
| `o` | open a file (also the titlebar open icon) |
| `n` / `p` | next / previous file in the directory |
| `-` / `=` | volume |
| `,` / `.` | seek back / forward 5 seconds |
| `w` | waveform on/off |
| `i` | interpolation on/off (software path only) |
| `m` | force the software path, for A/B |
| `h` or `?` | help — the key list with what each one does |
| `q`, Escape, close icon | quit |

`h` puts the bindings on screen **with their meanings**. A list of keys
without them is a reminder, not help: only readable by someone who
already knows what they do. Any key dismisses it, and the key that
dismissed it is not then acted on — pressing something to make a panel
go away and having it also do the thing is the wrong kind of surprise.

The overlay is painted one line per main-loop pass like everything
else. Twelve rows in one call is ~190ms, survivable on the mixer path
(372ms of ring in front of it) and an audible gap on the software path
(23ms of FIFO).

Transport buttons are in the window: play/pause, stop, and a volume
slider. File-open is `Z_WIN_FLAG_OPEN_ICON` in the titlebar, which is
what that flag is for and saves a button's width in a window that has
none to spare.

The transport button shows what pressing it will **do** — a triangle
while stopped or paused, two bars while playing. It once appeared
inverted, and the cause was not the logic: **nothing repainted it.**
Setting `z_widget_t.dirty` only marks a widget; `z_widget_draw_all()`
draws, and the only call to it was inside `draw_all()`, on a full
`Z_WM_REDRAW`. The button kept whatever face it had at the last full
repaint and lagged the state by one transition, which looks exactly
like inverted logic and is not.

`play` scans `/AUDIO`, then `/audio`, then the root, and plays through
what it finds, wrapping rather than exiting. A launch argument
overrides the scan: `run play /AUDIO/SONG.WAV`.

**Filenames are 8.3.** FatFs is built with `FF_USE_LFN 0`, so
`MYFAVOURITESONG.WAV` is `MYFAVO~1.WAV` from this side. `mkaudio.py`
names its output accordingly and says so when it truncates.

`.MOD` files are skipped, not played — modules load whole and drive the
mixer through a different engine (`sw/apps/track`).

---

## The display

Every rule here is inherited from `track`, which learned them
expensively (`docs/audio.md`): a text row costs about 16 ms of wall
time once the CPU is shared three ways.

- **At most one text row is repainted per loop pass.**
- **Rows are marked dirty, not redrawn on a timer.** Time, buffer meter
  and diagnostics change at most once a second.

### The status rows

    1/3 /AUDIO/SONG.WAV
    PCM8 2ch 22050Hz -> 46875Hz MIXER
    0:43 / 3:12  vol 62%  PLAYING
    buf 94%  sv0  sd 43K/s
    h = help

The bottom row shows a transient note when a key needs to explain
itself (`i` on the mixer path, which has no interpolator), the reason
for the software path when that is in use, and `h = help` otherwise.

It used to carry `fd/pm/ms/dr` — per-phase worst-case wall times in
kernel ticks. Those were left behind when the `rdcycle` profiler
replaced the `z_uptime_ticks()` instrumentation, and **had been reading
a constant `0000` ever since**: the variables were still declared and
still cleared once a second, and nothing wrote them. A status line
showing four zeros forever is worse than one showing nothing, because
it looks like a measurement. Timings live in `PLAY_PROF=1` now.

`ur` is absent on the mixer path. It is the FIFO's sticky `UNDERRUN`
bit and means nothing there — the mixer cannot underrun, it replays the
ring. A permanently blank field would imply the check was being made
and passing.

### The waveform

One column per pass, swept left to right, with a blank column ahead of
the cursor. Two blitter fills per pass and no pixel readback.

Scrolling the strip would mean moving the whole rectangle every column,
and `z_fb_hw_scroll()` only moves *vertically* — so that is a per-pixel
software loop across the full width, hundreds of times the work, for an
aesthetic difference.

What is drawn is the **peak-to-peak envelope**, not the waveform: at
one column per pass each column already covers thousands of frames, and
an envelope is honest about that. Peaks are sampled every eighth frame,
because two compares per frame at 44.1 kHz is 88,000 comparisons a
second spent on decoration.

`w` turns it off — the switch to reach for when `sv` is counting and
you want to know how much of it is the display.

**It stopped working entirely when the mixer path landed**, and the
cause is worth recording because it had three symptoms: `scope_accum()`
was called from `feed_fifo()`, which in mixer mode never runs. The
waveform went blank, `,`/`.` seeked from a position that never
advanced, and the elapsed time froze — all one bug. Anything hung off
one path's inner loop needs an equivalent on the other, and the way to
notice is that the *user-visible* symptoms look unrelated.

---

## Profiling

    make -C sw/apps/play PLAY_PROF=1

Off by default; every call site compiles to nothing otherwise.

### Why the first instrument was useless

The original diagnostics timed each phase with `z_uptime_ticks()`. It
is a **syscall** through the `reg_kernel` trampoline, called nine times
per pass, and its resolution is **one KTIMER tick** — 1.37 ms, 65664
cycles. Every phase worth optimising is shorter than that. It could
rank phases. It could not cost them.

### What replaces it

`rdcycle` and `rdinstret`: one instruction each, no syscall, no bus
transaction, single-cycle resolution — and **no BRAM and no LUTs,
because they are already built.** picorv32's `ENABLE_COUNTERS` and
`ENABLE_COUNTERS64` both default to 1 and `rtl/sysctl.v` overrides
neither; `zeitlos32.v` implements them explicitly.

They are emitted as **raw instruction words**, not the `csrr` mnemonic.
binutils 2.36 split the CSR instructions into the Zicsr extension, so
`-march=rv32im` no longer assembles `csrr`:

    Error: unrecognized opcode `csrr a0,0xC00', extension `zicsr' required

`-march=$(ARCH)_zicsr` works and was not taken: the 2018-era toolchain
picorv32's instructions pin — which `sw/common/arch.mk` still supports
— rejects `_zicsr` as unknown, so the flag would have to be probed for,
and that probe belongs in `arch.mk`, whose whole point is that the ISA
string is decided once and must match `boards.vh`. `.word` needs
neither:

    csrrs rd, csr, x0    [31:20] csr  [19:15] rs1=0
                         [14:12] 010  [11:7] rd  [6:0] 1110011

    rdcycle   a0  ->  0xC0002573
    rdinstret a0  ->  0xC0202573

Both verified against `as -march=rv32im_zicsr`, and both compile under
plain `rv32i`. `rd` is baked into the word, hence the local register
variable pinning it to `a0` — the construct every libc uses for syscall
stubs. Confirm a build contains them with
`objdump -d play.o | grep -c c0002573`; objdump renders them as `.word`
unless told about Zicsr itself.

**The one hazard, and why this is behind a build flag:** on a bitstream
built with `ENABLE_COUNTERS(0)`, `rdcycle` is an illegal instruction and
the process traps. Nothing in this tree does that, and there is no way
to probe for it without having already executed it.

### Reading the report

    -- play prof [MIXER]: 3038 ticks, 90 passes, out 45270, src 21296 --
       realtime 100% of 46875Hz   cpu share 34.9%
       phase      calls      min      avg     c/of  ipc  %avail
       pump          92   188732   285018       65    9      4
       push          92    19435    86242       28    8      2
       ...
       budget: 357 cyc/out frame  (~ = uneven or preempted)

**`c/of`** is the phase's cost attributed to one output frame.
Everything scales with the output rate, so this is the column that
makes phases comparable to each other and to the budget. **Sort by
it.**

**`ipc`** is instructions per cycle x100, and it decides *what kind* of
fix is needed. Around 17 is normal for this SOC; **well below that
means stalled, not busy** — waiting on the bus, the blitter or a
peripheral ack, where no algorithmic change helps. On this machine
everything measures 8, which is the whole reason the answer turned out
to be gateware.

**`min` and `avg`** are shown for their *ratio*. A row marked `~` has
`avg > 4 x min`, meaning either heavy preemption or wildly uneven call
sizes.

**`%avail`** is against the cycles this process was actually granted,
from `z_proc_list()`'s `cpu_ticks` — the KTIMER handler's own
per-process accounting. `rdcycle` says how many cycles a phase spanned;
`cpu_ticks` says how many were ever allowed. Only the ratio is
actionable.

**The budget line** is `clk * share / out_hz`: the cycles available to
produce one output frame. If the measured figure exceeds it, the app
cannot keep up however the work is rearranged.

### How the accounting was wrong, twice

Worth recording, because both versions produced plausible numbers that
were not true.

**First: `min * calls`.** The theory was that the cheapest call has no
preemption in it. Two holes. It assumes a phase never outlives a
timeslice — `decode` spanned a hundred of them, so every sample was
contaminated and the report claimed 135% of available, which is
impossible. Worse, it assumes **calls are homogeneous**: `feed_fifo()`
renders whatever space the FIFO has, so `stream_render()` was called
with 0 to 256 frames, the minimum was a call that did no work (646
cycles against an average of 843,688), and the report accounted for 14%
of a budget that was in fact 98% spent. It said the app was comfortably
inside budget while playing at a fifth of real time.

**Now: total wall cycles scaled by measured CPU share.** Neither
problem, and the totals close to within 3%.

**The report itself was 14.5M cycles** — a third of a second, 82% of
the share, once per second. It now runs every `PLAY_PROF_SECS` (4)
seconds and is still measured as its own phase rather than hidden. An
instrument that distorts the measurement is tolerable only if it says
by how much.

---

## Testing

    cd sw/apps/play && make test        # add test-wav to dump .wav files

71 checks across nine groups: parsing and rejection, G.711 companding
tables, pitch through six rate conversions, level and volume, IMA
ADPCM round trip in mono and stereo, ring behaviour at six chunk sizes,
interpolation on and off, seek arithmetic, and starvation reporting.

Built with the **host** compiler. `adec.c` and `stream.c` include
nothing from `sw/common` and touch no hardware — the same discipline,
for the same reason, as `sw/apps/track/modplay.c`.

**Fixtures are synthesised, not shipped.** A real audio file is a
copyrighted artifact, and a fixture whose correct output nobody can
state is not a test.

**Pitch is measured by counting zero crossings**, not by comparing
against a reference rendering — a sample-by-sample comparison passes
for a resampler wrong in exactly the way the reference was generated.

**The IMA fixture carries its own encoder with its own copy of the step
table**, transcribed separately from the decoder's. A test that encodes
with the decoder's own tables proves the decoder is self-consistent,
which is not the claim.

`rtl/tb/tb_audio_mixer.v` covers `MIXPOS` from the hardware side.

### Bugs it caught

1. **The resampler emitted a held sample forever at end of file.** No
   "ended" flag; the loop relied on the source running out. It does
   stop that call — and the next finds `s0`/`s1` still holding the last
   two real samples, interpolates, advances, fails, returns two more
   frames. Forever. On a board that is a DC tone at whatever level the
   track ended on, which sounds like a stuck DAC rather than a missing
   EOF check. The suite found it as a loop that never terminated.
2. **The render loop could not resume after a starve.** Emitting then
   advancing meant abandoning the loop mid-advance, with `s0` stepped
   and `s1` not yet replaced; the next call emitted a frame built from
   a pair that never existed in the source. Restructured to advance
   first, emit second.
3. **`sv` counted the end of every file.** A permanent `sv1` on tracks
   that played perfectly makes the one number worth watching worthless.

And one in the test itself, worth recording because it looked like a
decoder bug: the u-law check asserted that `0xFF` and `0x7F` expand to
a small positive and a small negative value. **G.711 has two codes for
zero and both expand to exactly 0.** The nearest non-zero codes are
`0xFE` and `0x7E`. Correct table, wrong expectation.

### On hardware

Play a tone first:

    python3 tools/mkaudio.py --tone 440 --seconds 30 -d /media/sd/AUDIO
    > run play /AUDIO/TONE440.WAV

That removes ffmpeg, the encoder and the codec from the list of things
that could be wrong, leaving the card, the player and the DAC. If
`audiotest` makes a clean tone and this does not, the fault is in the
streaming path.

Note the tone is written as 16-bit, so it takes the `MIXER 8b` route.
Convert it with `mkaudio.py` to exercise the 1:1 path.

---

## Preparing files

    python3 tools/mkaudio.py song.flac                  # wav8, 22.05kHz
    python3 tools/mkaudio.py -r 44100 song.wav
    python3 tools/mkaudio.py -m -f ulaw -r 11025 talk.mp3
    python3 tools/mkaudio.py -d /media/sd/AUDIO *.flac

It prints the resulting bandwidth and warns if you ask for IMA.

It also strips metadata, deliberately. `adec_parse()` sees the first
1 KB and walks the RIFF chunk list looking for `data`; a large
`LIST`/`INFO` block — what a tag editor leaves behind — can push `data`
past that window, and the file is then refused with "bad or oversized
header". Everything `mkaudio.py` writes passes `-map_metadata -1`.

One thing to know if you convert by hand: **ffmpeg writes 24- and
32-bit WAV as `WAVE_FORMAT_EXTENSIBLE` (tag `0xFFFE`)**, not as PCM
with a larger depth. `play` refuses `EXTENSIBLE` outright rather than
half-supporting it — the real format lives in a GUID whose only common
values are the two tags already handled, so "support" would be a lookup
table pretending to be a parser.

---

## Build knobs

`sw/apps/play/Makefile`:

| | default | |
|---|---|---|
| `PLAY_MIXRING` | 32KB | mixer ring, **power of two**. 372 ms at 22050 stereo |
| `PLAY_RING_SIZE` | 32KB | software-path ring, **power of two**; also the conversion staging buffer |
| `PLAY_CHUNK` | 2048 | bytes per read — a latency budget |
| `PLAY_VOLUME` | 40 | of 64. The 1-bit DAC drives headphones with no attenuator |
| `PLAY_PROF` | 0 | cycle profiler |
| `PLAY_PROF_SECS` | 4 | seconds between reports |
| `PLAY_RAW_RATE` / `_CHANNELS` | 22050 / 2 | what a headerless `.raw` is assumed to be |

Compile-time, in the sources: `PLAY_FEED_MAX` (256),
`PLAY_MAX_FILES` (64), `STREAM_SCRATCH_FRAMES` (1024),
`STREAM_MAX_BLOCK` (2048), `MIX_GUARD` (64).

Static footprint is about 75 KB, nearly all of it the two rings. It all
lives in `.bss`, which is the right budget: a process's stack tier
(`Z_PROC_STACK_SIZE_DEFAULT`, 16 KB) is the *only* room its C stack and
`malloc()` heap ever get, shared, for its whole life. Nothing in this
app allocates, so `play` needs no entry in `z_proc_stack_size_for()`.
`track.c`'s header explains this at length and it applies unchanged.

---

## Still to do

**A `FatFs` lock in `sw/os/fsapi.c`.** The one that matters, and a real
correctness gap rather than a missing feature. `sw/os/fsapi.h` already
documents that FatFs is not re-entrant and that a preemption mid-call
onto another process making its own FS syscall is "a real (if narrow)
window for corruption". A continuously streaming player turns that
window from narrow into routinely open. The fix `fsapi.h` itself
suggests is right: a busy flag plus a `maskirq()`-protected
check-and-set around the whole open/op/close sequence.

**An `on_idle` hook in `z_dialog_ctx_t`.** `z_dialog_open()` runs its
own loop until the user chooses and offers no per-iteration hook, so
playback stops while the file dialog is open. Half a dozen lines in
`sw/common/zdialog.c`, and anything else with a deadline wants it too.

**Playlists.** `.M3U`, loop modes, shuffle. Deferred deliberately: UI
and file parsing with no unknowns in it, and it would have obscured the
measurements.

**MOD in the same playlist.** The engine exists and `track`'s split was
built for the reuse, but it is a different playback model.

**16-bit mixer mode.** A halfword select, a wider accumulator and a
different output shift. Only worth it if 8-bit proves audibly short.

**A burst mode in `spim.v`.** Write a byte count, have the gateware
clock N bytes into a small FIFO, let the CPU drain 32-bit words. 49
cycles/byte down to roughly 8. No longer the largest available win —
the mixer took that — but it is what would make IMA viable and 44.1 kHz
comfortable.

**An audio server.** The right answer to "one device, several
processes", and still out of scope.

---

## Adding a codec

Three places: an `ADEC_*` value, a case in `adec_decode()`, and
recognition in `adec_parse()` plus `set_block_geometry()`. An 8-bit
companded format needs a builder next to `build_ulaw()`/`build_alaw()`
and nothing else.

Then decide whether it can reach the mixer: add it to
`mixer_supported()` and give it a `MIXCONV_*` case only if the
conversion to 8-bit costs a handful of instructions per byte. If it
needs a real decode it belongs on the software path, and on this
machine that means it will not play in real time — say so in
`mix_reject` rather than letting the user find out from a profile.

Then a group in `play_test.c`, with a synthesised fixture whose correct
output can be stated in one sentence. If it cannot be stated in one
sentence, resolve that before writing the decoder.
