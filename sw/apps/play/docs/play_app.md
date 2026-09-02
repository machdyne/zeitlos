# play

A streaming audio file player. Reads WAV, `.au` and raw PCM off the SD
card and plays them through the audio FIFO, without ever holding more
than a fraction of the file in memory.

    sw/apps/play/adec.c        container parsing and codecs
    sw/apps/play/stream.c      ring buffer and resampler
    sw/apps/play/play.c        the app: files, window, transport
    sw/apps/play/play_test.c   host-side tests for the two above
    tools/mkaudio.py           host-side converter

This is **phase 1**, and it is scoped deliberately: everything lives
inside `sw/apps/play`, nothing in the kernel or the RTL is touched, and
the app measures itself so that the question "can this machine stream
audio at all" gets answered with numbers before anything system-wide is
built on the answer. [What phase 1 leaves out](#what-phase-1-leaves-out)
lists what that costs.

---

## Why this is not `track` with a different file loader

`sw/apps/track` loads a whole module into `.bss` and plays it from
memory. A ProTracker module is under 100KB and that works.

A minute of 44.1kHz 16-bit stereo audio is 10MB. There is no version of
this app that loads the file, so everything below follows from having
to read it continuously while it plays.

The second fact everything follows from is in `rtl/spisd.v`'s own
header: `sdmm.c` moves **one byte per MMIO write / poll / read cycle**,
roughly **48 CPU cycles per byte**. SD bandwidth on this board is CPU,
not waiting. There is no DMA path — `rtl/dma.v` is an empty stub — so
every byte of every file is carried by the processor, in a loop, while
it is also meant to be feeding a DAC.

| stream | bytes/s | % of a whole 48MHz core, in `spi_xchg` alone |
|---|---|---|
| 44.1kHz 16-bit stereo | 176 KB/s | **17.6%** |
| 22.05kHz 16-bit stereo | 88 KB/s | 8.8% |
| 44.1kHz IMA ADPCM stereo | 44 KB/s | 4.4% |
| 22.05kHz IMA ADPCM stereo | 22 KB/s | 2.2% |

With `wm` and a shell resident this process gets about a third of the
core, so the first row is spending roughly **half of everything it
has** before decoding a single sample. That is the number that decides
the shape of this app.

---

## The loop

```
feed_fifo();          top up the hardware FIFO
pump_ring();          ONE bounded read off the card
feed_fifo();          top up again, immediately
drain_messages();
ui_step();            at most one text row
scope_step();         one column of the waveform
```

The FIFO holds 1024 frames — **23ms at 44.1kHz**. That is the whole
margin. Nothing in the loop may block for longer than it, and the loop
is arranged around that one constraint.

**`PLAY_CHUNK` is a latency budget, not a throughput knob.** It is the
longest this app can be inside a blocking call. 2KB is about 2ms of
CPU and several ms of wall time once shared three ways — comfortably
inside 23ms. Raising it amortises a little FatFs overhead and spends
the entire safety margin doing so. Same distinction `track.c`'s
`FEED_BLOCKS` comment makes, for the same reason.

**The second `feed_fifo()` is not redundant.** It runs immediately
after the only blocking call in the loop, which is precisely when the
FIFO is at its emptiest.

**`PLAY_FEED_MAX` is the other latency budget.** `wm`'s
`repair_region()` sends `Z_WM_REDRAW` and *blocks* on the ack
(`docs/window_manager.md`), so an app that disappears into a push loop
for 40ms freezes the entire desktop for 40ms. 256 frames is 5.8ms of
audio at 44.1kHz.

### Where the buffering actually is

```
SD --2KB chunks--> [ 32KB byte ring ] --> adec --> [ scratch ]
    --> resample --> [ 256-frame block ] --> hw FIFO --> DAC
```

The ring is 186ms at 44.1kHz 16-bit stereo, and four times that for an
IMA ADPCM file. It covers what the loop shape cannot: a FAT cluster
lookup, another process doing filesystem work, `wm` blocking on a
redraw ack.

**The ring holds file bytes, not decoded frames**, and that is the
choice worth defending. Buffering decoded audio is the obvious
simplification and it is a real loss: an ADPCM file expands 4× on
decode, so 32KB of decoded audio absorbs a quarter as much SD jitter as
32KB of file. The jitter is on the SD side, so the buffer sits in the
compact units.

**The FIFO is topped up from a small block, not written to directly**,
which costs one store and one load per sample. That buys the whole
render path being testable on the host, which found two bugs before any
of this reached a board. Worth it.

---

## The two ways this fails, and why they are reported separately

They sound identical and they want opposite fixes. Both are on the
status line.

**`sv` — stream starvation.** The renderer had room in the FIFO and
nothing to put there. The *card* could not keep ahead of the DAC. A
smaller stream (lower rate, or ADPCM) or a larger ring.

**`ur` — the hardware's sticky `UNDERRUN` bit.** The *FIFO* ran dry.
This can happen with a perfectly healthy ring if something blocked the
loop for more than 23ms — a redraw, another process, a long dialog.
Less work in the loop.

`docs/audio.md` draws the same distinction for the mixer ("buffer too
small" vs "mixer too slow") and it is the same lesson: a single "it
glitched" indicator sends you looking in the wrong place half the time.

`sv` deliberately does **not** count the short render at end of file.
The first version did, which put a permanent `sv1` on every track that
played flawlessly — and a number that is always 1 is a number nobody
reads.

### The rest of the status line

    2/7 /AUDIO/SONG.WAV
    IMA ADPCM 2ch 22050Hz -> 44117Hz lerp
    0:43 / 3:12  vol 62%  PLAYING
    buf 94%  sv0  ur  sd  22K/s
    fd1 pm4 ms1 dr2   spc s o n p -= , . w i

`buf` should sit high and steady; a sag means the card is not keeping
up. `sd` is measured throughput. `fd`/`pm`/`ms`/`dr` are the worst wall
time each loop phase took over the last second, in kernel ticks of
1.37ms — directly comparable against the FIFO's 17-tick margin at
44.1kHz. Anything there in double digits **is** the stall. That
diagnostic line is lifted straight from `track`, where `pt96` named a
culprit after three wrong guesses.

---

## Formats

Chosen on decode cost, not on file size. Bandwidth is cheap in absolute
terms — the card has gigabytes — and CPU is not.

| Format | Cost per sample | |
|---|---|---|
| **WAV PCM 16-bit** | unpack only | baseline |
| **WAV PCM 8-bit** | one table lookup | unsigned, `0x80` is silence |
| **WAV IMA ADPCM** (tag 0x11) | ~4 integer ops | 4:1, ~12-bit. The one that makes 44.1kHz comfortable |
| **WAV u-law / A-law** (tags 7 / 6) | one table lookup | 2:1, ~13-bit range, *cheaper than 16-bit PCM* |
| **`.au`** | as above | u-law, A-law, 8-bit signed, 16-bit big-endian |
| **`.raw` / `.pcm`** | unpack only | headerless; format is a build-time assumption |

Everything decodes to interleaved 16-bit stereo. A mono source is
duplicated here rather than carried as mono and split later: it costs
one store per frame and means the resampler, the FIFO loop, the scope
and the volume control each exist once instead of twice. Two paths that
must agree about pitch is exactly how you get a bug audible on half the
files.

**Volume is applied at decode time, not in the resampler.** This looks
like the wrong layer and is the right one: one multiply per *source*
sample rather than per *output* sample — identical when the rates
match, cheaper whenever the file is below the DAC rate — and for every
8-bit codec it folds into the 256-entry expansion table and costs
nothing at all. The price is that a volume change lands one scratch
buffer later, about 6ms.

### What is not here, and why

**MP3.** Patents expired in 2017 and `minimp3` is CC0, so the obstacle
is not licensing. Fixed-point decoders of the `libmad` generation need
roughly 25 MIPS for 44.1kHz stereo. This board delivers about 7 MIPS
across every process, so `play` has something like 2.3 — off by an
order of magnitude, before the SD reads it would still have to do. It
also wants ~30KB of tables and state. Not close.

**FLAC.** Integer-only, which is the usual objection removed, but Rice
decoding plus LPC synthesis is in the same 15–20 MIPS range, and a
4096-sample block has to be buffered whole. Same answer.

**Vorbis, Opus, AAC.** Floating point, 100KB+ of state. No.

There is a structural reason as well as an arithmetic one, and it is
worth stating because it applies to anything proposed later: a codec in
`adec.c` must decode from an arbitrary block boundary, look back no
further than one block, and keep its state inside `adec_t`. Those three
constraints are what make seeking work and what let the ring be a plain
byte ring. MP3 and FLAC fail all three.

### Sample rates

The DAC rate is not negotiable and not round. It is
`CLK_HZ / (64 * RATE)` — 44117.6Hz at the default divider, 46875Hz on
an S/PDIF board. **No file's rate ever equals it**, not even a 44100Hz
one, which is 0.04% off.

So everything goes through the phase accumulator; there is no
"rates match, just copy" fast path to take, and pretending otherwise
would drift a frame every 2500. 32-bit accumulator, 14 fractional bits
— the same split `modplay.c` uses, deliberately the same so that
reading one is reading both.

`play` starts from **whatever rate the board came up with** rather than
a constant. `AUDIO_RATE_RESET` in `boards.vh` exists so a board can
pick a rate its outputs can actually carry, and an app that overwrites
it throws that away. On an S/PDIF board that matters: nothing below
32kHz will lock at the receiver.

Interpolation is switchable at runtime with `i` because it is the
largest single term in the per-output-frame cost, and whether it is
affordable is a question about the board rather than about the code.
Linear costs about 10 instructions a frame; nearest-neighbour costs
about 2 and is audible as a faint roughness on tonal material.

---

## The waveform display

One column per pass of the main loop, swept left to right, with a blank
column ahead of the cursor. Two blitter fills per pass and no pixel
readback.

Scrolling the strip instead would mean moving the whole rectangle every
column, and `z_fb_hw_scroll()` only moves *vertically* — so that would
be a per-pixel software loop across the full width, hundreds of times
the work, for an aesthetic difference.

What is drawn is the **peak-to-peak envelope**, not the waveform. At
one column per pass each column already covers thousands of frames;
there is no waveform left to draw at that scale and an envelope is
honest about it.

Peaks are sampled **every eighth frame**. Two compares per frame at
44.1kHz is 88,000 comparisons a second spent on decoration. One in
eight still catches the envelope, because the column is averaging
thousands of frames either way.

`w` turns it off. That is the switch to reach for when `sv` is
counting and you want to know how much of it is the display.

---

## The display, generally

Every rule here is inherited from `track`, which learned them
expensively and wrote them down in `docs/audio.md`:

- **At most one text row is repainted per loop pass.** A row costs
  about 16ms of wall time once the CPU is shared three ways. Five rows
  in one call is more than the FIFO's entire margin.
- **Rows are marked dirty, not redrawn on a timer.** The time, the
  buffer meter and the diagnostics all change at most once a second.
- **Nothing continuous is allowed to redraw continuously.**

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
| `i` | interpolation on/off |
| `q`, Escape, close icon | quit |

Transport buttons are in the window: play/pause, stop, and a volume
slider. The file-open control is `Z_WIN_FLAG_OPEN_ICON` in the
titlebar, which is what that flag is for and saves a button's width in
a window that has none to spare.

`play` scans `/AUDIO`, then `/audio`, then the root, and plays through
what it finds, wrapping rather than exiting. A launch argument, if `wm`
passed one, overrides the scan:

    > run play /AUDIO/SONG.WAV

**Filenames are 8.3.** FatFs is built with `FF_USE_LFN 0`
(`sw/os/fs/fatfs/ffconf.h`), so `MYFAVOURITESONG.WAV` is
`MYFAVO~1.WAV` from this side. `tools/mkaudio.py` names its output
accordingly, and says so when it has to truncate.

`.MOD` files in the directory are **skipped**, not played. Modules are
loaded whole and driven through the hardware mixer, which is a
different engine entirely (`sw/apps/track`). Merging the two into one
playlist is phase 3.

---

## Things this app does that are not obvious

**It takes the audio device.** On startup it clears `MIXEN` and every
hardware mixer channel. If `track` is running, its music stops.

That is deliberate, and what is missing is not politeness. There is no
arbitration for the audio block in this system. A player that left
`MIXEN` wherever it found it would push frames into a FIFO the DAC is
not reading, produce perfect silence, and give no indication why — a
considerably worse experience than "the other player stopped". It also
matters for a second reason: the mixer is a bus master issuing
*physical* addresses, so if `track` was killed rather than exiting, its
channels are still fetching sample data from memory the kernel has
since handed to something else.

**It does not set `Z_WIN_FLAG_CLOSE_KILLS_OWNER`.** `track` does, and
`track` is right to — it holds nothing that needs releasing. This app
holds an open filesystem handle for the length of a track, and `zfs.h`
is explicit that a handle whose owner dies without closing it is
**never released**; there is no process-exit hook that sweeps them, and
there are four in the entire system. So the close icon sends
`Z_WM_CLOSE` and the app unwinds through `close_track()`.

Killing `play` from the shell still leaks the handle. Nothing in this
app can prevent that.

**Pause does not drain the FIFO.** `EN=0` mutes without discarding, so
resume is instant. `rtl/audio.v` was changed specifically so this
works; see `docs/audio.md`.

**Seeks round *down* to a block boundary.** For PCM a block is a frame
and it does not matter. For IMA ADPCM the predictor state lives in the
block header, and starting mid-block decodes the remainder of it as a
burst of noise. Rounding down rather than to nearest also means a seek
to the very end lands inside the file rather than past the last block.

**Playback stops while the file dialog is open.** `z_dialog_open()`
runs its own message loop until the user chooses
(`sw/common/zdialog.c`) and offers no per-iteration hook — only a
callback for messages it does not handle itself. With 23ms of FIFO,
anything longer than a moment underruns continuously, which is a far
worse noise than a clean pause. See
[What phase 1 leaves out](#what-phase-1-leaves-out).

---

## Testing

    cd sw/apps/play && make test        # add test-wav to dump .wav files

71 checks across nine groups: parsing and rejection, G.711 companding
tables, pitch through six rate conversions, level and volume, IMA
ADPCM round trip in mono and stereo, ring behaviour at six chunk sizes,
interpolation on and off, seek arithmetic, and starvation reporting.

Built with the **host** compiler. `adec.c` and `stream.c` include
nothing from `sw/common` and touch no hardware, which is what makes
this possible and is the reason they are split out of `play.c` at all
— the same discipline, for the same reason, as
`sw/apps/track/modplay.c`.

**Fixtures are synthesised, not shipped.** A real audio file is a
copyrighted artifact, and a fixture whose correct output nobody can
state is not a test. Every input is a sine wave of known frequency and
amplitude at a known rate.

**Pitch is measured by counting zero crossings**, not by comparing
against a reference rendering. A sample-by-sample comparison passes for
a resampler that is wrong in exactly the way the reference was
generated. Counting crossings measures the claim actually being made:
that a 440Hz tone comes out at 440Hz after a rate conversion.

**The IMA fixture carries its own encoder with its own copy of the step
table**, transcribed separately from the decoder's. A test that encodes
with the decoder's own tables proves the decoder is self-consistent,
which is not the claim.

### The three bugs it caught

1. **The resampler emitted a held sample forever at end of file.**
   There was no "ended" flag; the loop relied on the source running out
   to stop it. It does stop that call — and the *next* call finds `s0`
   and `s1` still holding the last two real samples, interpolates
   between them, advances the phase, fails again, and returns two more
   frames. Forever.

   On a board that is not silence and it is not a click. It is a DC
   tone at whatever level the track happened to end on, indefinitely,
   which sounds like a stuck DAC rather than a missing end-of-file
   check. The test suite found it as a loop that never terminated.

2. **The render loop could not be resumed after a starve.** The
   original arrangement emitted a frame and then advanced the phase, so
   a starve had to abandon the loop partway through the advance — with
   `s0` already stepped and `s1` not yet replaced. The next call then
   emitted one frame built from a sample pair that never existed in the
   source. Restructured to advance first and emit second, so a starve
   returns with `s0`, `s1` and `frac` describing exactly one consistent
   position.

3. **`sv` counted the end of every file.** Running out of input at EOF
   is the file ending, not starvation. Conflating them put a permanent
   `sv1` on tracks that played perfectly, which makes the one number
   this phase exists to watch worthless.

And one in the test itself, worth recording because it looked like a
decoder bug: the u-law table check asserted that `0xFF` and `0x7F`
expand to a small positive and a small negative value. **G.711 has two
codes for zero and both expand to exactly 0.** The nearest non-zero
codes are `0xFE` and `0x7E`. Correct table, wrong expectation.

### On hardware

Play a tone first, before any real material:

    python3 tools/mkaudio.py --tone 440 --seconds 30 -d /media/sd/AUDIO
    > run play /AUDIO/TONE440.WAV

This removes ffmpeg, the encoder, the codec and the compression ratio
from the list of things that could be wrong, leaving the card, the
player and the DAC. If `audiotest` makes a clean tone and this does
not, the fault is in the streaming path.

Then work up: 22.05kHz IMA, 22.05kHz 16-bit, 44.1kHz IMA, 44.1kHz
16-bit, watching `sv`, `ur` and `buf` at each step. That ordering is
the point — the last row of that list is the one predicted to be
marginal, and finding out where it actually breaks is what this phase
is for.

---

## Preparing files

    python3 tools/mkaudio.py song.flac                  # IMA, 22.05kHz
    python3 tools/mkaudio.py -f wav16 -r 44100 song.wav
    python3 tools/mkaudio.py -m -f ulaw -r 11025 talk.mp3
    python3 tools/mkaudio.py -d /media/sd/AUDIO *.flac

It prints the resulting bandwidth as a percentage of the core and warns
above 176 KB/s.

It also strips metadata, deliberately. `adec_parse()` is shown the
first 1KB of the file and walks the RIFF chunk list looking for `data`;
a large `LIST`/`INFO` block — which is what a tag editor leaves behind
— can push `data` past that window, and the player then refuses the
file with "bad or oversized header". Everything `mkaudio.py` writes
passes `-map_metadata -1` so the question does not arise.

One thing to know if you convert by hand: **ffmpeg writes 24-bit and
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
| `PLAY_RING_SIZE` | 32KB | **must be a power of two.** How long the card may be late |
| `PLAY_CHUNK` | 2048 | bytes per read — a latency budget |
| `PLAY_VOLUME` | 40 | of 64. The 1-bit DAC drives headphones with no attenuator |
| `PLAY_RAW_RATE` | 22050 | what a headerless `.raw` is assumed to be |
| `PLAY_RAW_CHANNELS` | 2 | ditto — must match `mkaudio.py --raw` |

Compile-time, in the sources:

| | | |
|---|---|---|
| `PLAY_FEED_MAX` | 256 | frames pushed per feed call — the other latency budget |
| `PLAY_MAX_FILES` | 64 | directory scan cap |
| `STREAM_SCRATCH_FRAMES` | 1024 | decoded frames between decoder and resampler |
| `STREAM_MAX_BLOCK` | 2048 | largest ADPCM block accepted |

Static footprint is about 40KB: the ring, a 1KB feed block, a 1KB
probe buffer, a 4KB directory staging buffer, a 2KB scratch, 3.5KB for
the file dialog's list, and the code.

That all lives in `.bss`, which is the right budget to be in. A
process's stack tier (`Z_PROC_STACK_SIZE_DEFAULT`, 16KB) is the *only*
room its C stack and its `malloc()` heap ever get, shared, for its
whole life — so nothing in this app allocates, and `play` needs no
entry in `z_proc_stack_size_for()`. `track.c`'s header comment explains
this at length and it applies unchanged here.

---

## What phase 1 leaves out

Listed in the order they are worth doing.

**A `FatFs` lock in `sw/os/fsapi.c`.** This is the one that matters and
it is a real correctness gap rather than a missing feature.
`sw/os/fsapi.h` already documents that FatFs is not re-entrant, that
the handlers do not mask interrupts around `f_open`/`f_read`/`f_close`,
and that a preemption mid-call onto another process making its own FS
syscall is "a real (if narrow) window for corruption".

A continuously streaming player turns that window from narrow into
**routinely open** — `play` is inside `f_read` a large fraction of the
time it exists. The fix `fsapi.h` itself suggests is right: a busy flag
plus a `maskirq()`-protected check-and-set around the whole
open/op/close sequence, returning `Z_FAIL` on contention, with the
app-side wrapper retrying. Small, contained, and it should land before
anything else here does.

**An `on_idle` hook in `z_dialog_ctx_t`.** Called once per `dlg_run()`
iteration, so an app with real periodic work can keep doing it while a
dialog is open. Half a dozen lines in `sw/common/zdialog.c`, and it is
what removes the "playback stops during the file dialog" limitation
above. Anything else with a deadline — a network transfer, a recorder —
wants it for the same reason.

**Playlists.** `.M3U`, loop modes, shuffle. Deferred on purpose: it is
UI and file parsing with no unknowns in it, and it would have obscured
the measurements this phase is for.

**MOD in the same playlist.** Requires switching `MIXEN` between
tracks, a second large buffer, and linking `modplay.c`. The engine
already exists and the split in `track` was built for exactly this
reuse, but it is a different playback model and belongs after
playlists.

**A burst mode in `spisd.v`.** Write a byte count, have the gateware
clock N bytes into a small FIFO, let the CPU drain 32-bit words. That
takes 48 cycles/byte down to roughly 8 and would make every row of the
bandwidth table at the top of this document a non-issue. It is by a
wide margin the largest available improvement to audio streaming on
this machine, and it is not in this app.

**An audio server.** The right long-term answer to "one device, several
processes" — and explicitly out of scope until the numbers above say
whether a single app can stream at all. That is what phase 1 is for.

**Recording.** Nothing in the hardware supports it. Listed only so the
absence is deliberate.

---

## Adding a codec

Three places, in this order:

1. an `ADEC_*` value in the enum,
2. a case in `adec_decode()`,
3. whatever recognises it in `adec_parse()` and a case in
   `set_block_geometry()`.

If it is an 8-bit companded format it needs a builder next to
`build_ulaw()`/`build_alaw()` and nothing else — `adec_decode()`'s
table path already handles any of them, gain included.

Then a group in `play_test.c`, with a synthesised fixture whose correct
output can be stated in one sentence. If it cannot be stated in one
sentence, that is worth resolving before writing the decoder.

---

## Profiling

    make -C sw/apps/play PLAY_PROF=1

Off by default. When on, `play` prints a per-phase cycle report to the
console once a second, alongside the normal status line.

### Why the first instrument was useless

The original diagnostics timed each phase with `z_uptime_ticks()`.
Both things wrong with that matter more than they sound.

**It is a syscall.** `z_uptime_ticks()` builds a `z_obj_t` and calls
through the `reg_kernel` trampoline into the kernel's dispatch table.
`play` called it nine times per loop pass, to ask a question about
regions that may themselves cost less than the asking.

**Its resolution is one KTIMER tick** — 1.37ms, 65664 cycles at 48MHz.
Every phase worth optimising here is shorter than that. `fd9` meant
"somewhere between 8 and 10 ticks of *wall* time", which for a
preempted process is largely a measurement of the other two processes.
Those numbers could rank phases. They could not cost them.

### What replaces it

`rdcycle` and `rdinstret`. One instruction each, no syscall, no bus
transaction, single-cycle resolution — and, given where this project's
budget is actually tight, **they cost no BRAM and no LUTs, because
they are already built**:

- picorv32's `ENABLE_COUNTERS` and `ENABLE_COUNTERS64` both default to
  1 and `rtl/sysctl.v` overrides neither.
- `zeitlos32.v` implements `rdcycle`/`rdcycleh`/`rdinstret`/
  `rdinstreth` explicitly.

They are emitted as **raw instruction words**, not as the `csrr`
mnemonic, and that is a binutils-version story rather than a
preference.

binutils 2.36 split the CSR instructions out of the RISC-V base ISA
into the Zicsr extension. On anything that recent, `-march=rv32im` no
longer permits `csrr`:

    Error: unrecognized opcode `csrr a0,0xC00', extension `zicsr' required

The obvious fix is `-march=$(ARCH)_zicsr`. It works, and it was not
taken. The 2018-era toolchain picorv32's own instructions pin — which
`sw/common/arch.mk` explicitly still supports — rejects `_zicsr` as an
unknown extension, so the flag has to be probed for rather than set,
and the probe belongs in `arch.mk` if it belongs anywhere. And
`arch.mk`'s whole point is that the ISA string is decided in one place
and must match `rtl/boards.vh`'s `CPU_MUL`/`CPU_DIV`; a profiler is a
poor reason to put a second, conditionally-different ISA string beside
it.

`.word` needs neither. The encoding is fixed by the ISA and cannot
drift:

    csrrs rd, csr, x0    [31:20] csr   [19:15] rs1=0
                         [14:12] 010   [11:7] rd   [6:0] 1110011

    rdcycle   a0  ->  csr 0xC00, rd = x10  ->  0xC0002573
    rdinstret a0  ->  csr 0xC02, rd = x10  ->  0xC0202573

Both verified against `as -march=rv32im_zicsr`, which assembles the
mnemonics to exactly those words, and both compile under plain `rv32i`
as well. `rd` is baked into the word, so the destination cannot be
left to the register allocator — hence the local register variable
pinning it to `a0`, the same construct every libc uses for its syscall
stubs.

To confirm a build actually contains them:

    riscv64-...-objdump -d sw/apps/play/play.o | grep -c c0002573

Note that objdump will render them as `.word` rather than `rdcycle`
unless it is itself told about Zicsr — the same split, from the other
side.

**The one hazard, and the reason this is behind a build flag:** on a
bitstream built with `ENABLE_COUNTERS(0)`, `rdcycle` is an illegal
instruction and the process traps. Nothing in this tree does that, and
there is no way to probe for it from software without having already
executed it.

### Reading the report

    -- play prof: 734 ticks, 243 passes, out 6120, src 2880 --
       realtime 13% of 46875Hz   cpu share 32.4%
       phase     calls      min      avg     c/of  ipc  %avail
       render      243    41200    58300     1636   14      52
        decode       6    18400    19100       18   19       0
       push        243     9100    11200      361   11      11
       pump          3    12400    91000        6   31       0
       msg         243       44      210        1   40       0
       text          5   198000   210000      161    6       5
       scope       243    25100    31000      996    9      31
       dump          1   410000   410000       67    8       2
       accounted 20400000 cyc of 15900000 available (128%), 3221 cyc/out frame
       budget: 331 cyc/out frame at this share

Column by column:

**`min`** is the cheapest observed call, in cycles, and it is the
number to optimise against. `rdcycle` is a free-running counter on
`sys_clk` — it keeps counting while the scheduler runs `wm` or `sh` —
so any single sample may contain somebody else's timeslice. A slice is
65664 cycles and most of these phases are far shorter, so the majority
of samples are clean and the contaminated ones are contaminated by a
lot. The minimum is therefore the real, uninterrupted cost.

**`avg`** is the same measurement with preemption folded in. It means
nothing alone. A large `avg`/`min` ratio says "this phase is
interrupted often", not "this phase is expensive".

**`c/of`** is `min * calls / output frames` — the phase's clean cost
attributed to one output frame. Everything in this app scales with the
output rate, so this is the column that makes phases comparable both to
each other and to the budget on the last line. **Sort by this.**

**`ipc`** is instructions retired per cycle, ×100, and it is the column
that decides *what kind* of fix is needed:

- **around 17** is normal for this SOC. `docs/muldiv.md` measured
  exactly 0.172 on the real core. A phase here is executing a lot of
  instructions, and the fix is to execute fewer.
- **well below 17** means the phase is **stalled, not busy** — waiting
  on the bus, the blitter, or a peripheral ack. No algorithmic
  cleverness helps. The fix is fewer transactions, or gateware.

Telling those two apart is precisely the question "should this move
into hardware", and no wall-clock timer can answer it.

**`%avail`** is the phase's clean cost against the cycles this process
was actually granted, taken from `z_proc_list()`'s `cpu_ticks` — the
KTIMER handler's own per-process accounting (`sw/os/kernel.h`).
`rdcycle` says how many cycles a phase spanned; `cpu_ticks` says how
many this process was ever allowed. Only the ratio is actionable.

`decode` is nested inside `render` and is excluded from the total, as
is `wait`. `dump` is the profiler's own `printf`, measured rather than
hidden — this libc's formatting is not cheap and burying its cost in
the loop overhead would be dishonest about the instrument.

### The last two lines

    accounted ... (128%)
    budget: 331 cyc/out frame at this share

The budget is `clk * share / out_hz`: the cycles available to produce
one output frame at the current CPU share and DAC rate. Compare it
against `cyc/out frame` directly above it.

If the measured figure exceeds the budget, the app cannot keep up
**however the work is rearranged**, and only three moves remain: a
lower output rate (`AUDIO_RATE_RESET` — on an S/PDIF board anything up
to divider 23 still locks, which is 32608Hz and a 30% cut), less work
per frame, or moving the work into gateware.

An `accounted` figure above 100% is not necessarily an error. It means
the clean-cost estimates sum to more than the share measured over the
same window, which happens when `min` is not achievable in practice
because the phase is essentially always interrupted. Treat a large
overshoot as a signal that the phase costs are contaminated and the
interval was unrepresentative, not as a contradiction.
