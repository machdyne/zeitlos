# Audio

Sample playback: eight channels mixed in hardware, a FIFO for software
mixing, a sample clock, and whichever output the board has — 1-bit
sigma-delta, PT8211, or S/PDIF.

    rtl/audio.v          FIFO, registers, interrupt, output-source mux
    rtl/audio_out.v      PT8211 serialiser + 2nd-order sigma-delta
    rtl/audio_mixer.v    8-channel hardware mixer, main-bus master
    rtl/audio_spdif.v    IEC 60958 transmitter
    sw/common/zaudio.h   register map and helpers
    sw/apps/audiotest    tone / sweep / L-R / silence
    sw/apps/track        ProTracker MOD player

All three phases are built. The subsystem grew in order — FIFO and
output stage first, then a software MOD player, then the hardware mixer
— and that ordering is described under [Phasing](#phasing), because the
reasons still apply to anything added next.

---

## What you get

| | |
|---|---|
| Channels | 8 in hardware, time-multiplexed |
| Format | 8-bit signed samples in main memory; 16-bit stereo out |
| Rate | programmable, 44117.6 Hz default (46875 on S/PDIF boards) |
| Buffer | 1024 frames, block RAM — software-mixing path only |
| Interrupt | `cpu_irq[7]`, level, on FIFO below watermark |
| Registers | `0x7000_05xx` |
| Feature bit | `Z_FEATURE_AUDIO`, bit 26 |

Per board:

| Board | Output | Pins | `boards.vh` |
|---|---|---|---|
| Obst | 1-bit sigma-delta, stereo | `AUDIO_L` J2, `AUDIO_R` J1 | `AUDIO`, `AUDIO_SD` |
| Lakritz | 1-bit sigma-delta, stereo | `AUDIO_L` M3, `AUDIO_R` N1 | `AUDIO`, `AUDIO_SD` |
| Mozart ML1 | PT8211/TM8211 | `AUD_BCK` T13, `AUD_WS` T14, `AUD_DIN` R12 | `AUDIO`, `AUDIO_PT8211` |
| Sergei ML1 | optical S/PDIF | `AUD_OPTICAL` A13 | `AUDIO`, `AUDIO_SPDIF` |
| ULX3S | coax S/PDIF | `AUD_OPTICAL` E5 | `AUDIO`, `AUDIO_SPDIF` |

Every board also sets `AUDIO_MIXER`. The S/PDIF boards set
`AUDIO_RATE_RESET 8'd16` — see [S/PDIF](#spdif) on why that rate is not
negotiable.

The register interface is identical on all three. Only
`audio_out.v`'s last stage differs, and software should not branch on
which one is present — `CONFIG`'s format bits exist so a diagnostic can
*say* which is wired, not so a player can behave differently.

---

## Measured cost

Synthesis-level counts (yosys 0.33, `synth_ecp5`) with everything
enabled — `AUDIO`, `AUDIO_MIXER`, and whichever output the board has.
BRAM, DSP and PLL counts are exact; LUT4 is pre-packing, so check
`make timing` / `make util` for the real post-`nextpnr` figures.

| | Obst | Lakritz | Mozart | Sergei | ULX3S |
|---|---|---|---|---|---|
| device | 12k | 25k | 45k | 45k | 25k |
| DP16KD | 52 / 56 | **55 / 56** | 52 / 108 | 52 / 108 | 52 / 56 |
| MULT18X18D | 7 / 28 | 7 / 28 | 7 / 72 | 7 / 72 | 7 / 28 |
| EHXPLLL | 2 / 2 | 2 / 2 | 2 / 2 | 2 / 2 | 2 / 2 |
| LUT4 | 14304 | 15906 | 17959 | 18068 | 14565 |

Post-`nextpnr`, the number that actually decides whether a build places
well is `TRELLIS_COMB`:

| build (Obst) | COMB | % |
|---|---|---|
| no audio at all | 16410 | **68%** |
| `AUDIO` — FIFO + output stage, software mixing | 17463 | 72% |
| `+ AUDIO_MIXER` | 19196 | **79%** |

**Read the first row before the other two.** Obst was already at 68%
before any audio existed. The mixer costs 1733 COMB and the phase-1/2
block another 1053; neither is outrageous alone, but together they push
a design that was already three-quarters full past the point where
nextpnr's placer starts visibly struggling and timing turns
seed-sensitive. `AUDIO_MIXER` is a separate define from `AUDIO` for
exactly that reason — commenting it out returns you to 72% and software
mixing, and `sw/apps/track` detects which it has and says so at
startup.

ULX3S lands in the same neighbourhood at 80% on the 25k.

**`AUDIO_MIXER_CH_BITS 2` (four channels) is not a useful lever.**
Measured: 18972 COMB, 78% — 224 out of the mixer's 1733. The cost is
the sequencer datapath and the wide muxes, not per-channel storage, and
`TRELLIS_RAMW` does not move at all because a 4-deep array occupies the
same 16-deep LUT-RAM primitives an 8-deep one does. The dial works; it
is not worth turning.

**Lakritz is the tight board at 55 of 56 DP16KD.** `ICACHE_KB 4` costs
three blocks and the 1024-frame FIFO costs two. If that one spare is
too close, the options in order are: drop `ICACHE_KB` to 2, or lower
`AUDIO_FIFO_LOG2` and switch the `ram_style` attribute in `audio.v`
back to `"distributed"`.

### Why the FIFO is 1024 frames of block RAM

It was 128 frames of distributed RAM, and that was wrong. The original
argument — Lakritz has three BRAM left, and 128 frames is 2.9 ms at
44.1 kHz, which is comfortable if refill is paced by the watermark
interrupt — assumed a system where the player gets the CPU when it
asks. **This one is preemptive round-robin, and both `wm` and `sh`
busy-poll rather than blocking**, so they are always runnable:

| runnable procs | player's CPU share | longest gap off-CPU |
|---|---|---|
| 2 | 50% | 1.37 ms |
| 3 | 33% | 2.73 ms |
| 4 | 25% | 4.10 ms |

128 frames is 5.8 ms at 22 kHz and 2.9 ms at 44.1 kHz. The second is
already shorter than a three-way round trip. 1024 frames is 46 ms and
23 ms — slack rather than a coin toss.

**1024 rather than 512 because a DP16KD is 18 bits wide**, so a 32-bit
FIFO costs two blocks at any depth. 512 and 1024 frames cost exactly
the same two blocks; anything under 1024 is paying for block RAM it
isn't using.

| | Obst | Lakritz | Mozart |
|---|---|---|---|
| DP16KD before audio | 50 | 53 | 50 |
| **with audio** | **52** / 56 | **55** / 56 | **52** / 108 |
| free | 4 | **1** | 56 |

**Lakritz is left with one spare block.** If that's too tight: drop
`ICACHE_KB` from 4 to 2 (frees one or two, costs some CPU), or set
`AUDIO_FIFO_LOG2` lower and change the `ram_style` attribute in
`audio.v` back to `"distributed"` — 128 frames cost 64
`TRELLIS_DPR16X4` and ~420 LUT4, and 256 nearly triples that because of
the read multiplexer.

### The output register

Distributed RAM could be read combinationally, which is what let
`audio_out` latch a frame on the same edge that raised `frame_req`.
Block RAM cannot, so `head` holds one frame ahead of the array and *is*
what `audio_out` sees. `ram_ready` tracks whether the registered read
output can be believed — cleared whenever the read pointer moves or a
write lands (which may have been to the address just read; same-cycle
read/write behaviour isn't worth relying on). Costs a cycle or two of
refill latency against a 1088-cycle frame period.

### `EN` no longer drains the FIFO

Pausing used to throw away everything buffered, and a muted FIFO
drained to empty and set `UNDERRUN` — reporting a fault where none had
occurred. Both only showed up once the FIFO got big enough that the
drain outpaced a fill loop. The pop and the refill are now gated on
`EN`. The output stage's clocks still run regardless, which is the
thing that actually matters for a PT8211.

## Buffer size vs mixer speed — they fail differently

This is the distinction to hold onto when something sounds wrong,
because the two causes sound similar and want opposite fixes.

**Buffer too small** → the FIFO occasionally starves, the hardware
repeats the last frame for a moment, you get a click. `UNDERRUN` sets.
Overall tempo is right.

**Mixer too slow** → the FIFO throttles the player, so the player
renders exactly as fast as it can and the music plays *slow and
smooth*, at whatever fraction of real time the CPU manages. It may
also set `UNDERRUN`, which is why the flag alone doesn't distinguish
them.

`sw/apps/track` reports a percentage on screen for exactly this —
labelled `mix` in software mode and `rate` in hardware mode, since in
hardware mode it measures tracker pacing rather than mixing throughput.
A healthy player sits at 100 — it renders as fast as the hardware
consumes and no faster, because the FIFO backs it up. **Below 100 means
the CPU could not keep up, and the music is slow by that factor.** No
FIFO size fixes that.

The mixer must run at *N* times real time during its own slice, since
it only has 1/N of the CPU. At 22 kHz with three runnable processes
that is 3×.

The watermark interrupt (`cpu_irq[7]`, level-sensitive, non-latched —
bit 7 is cleared in `LATCHED_IRQ` alongside the UART's bit 4) exists
for a game that has other work to do. `track` polls instead; see its own
notes.

## Clocking

Both `EHXPLLL` blocks are used (`pll0` for the system clocks, `pll1`
for pixel and TMDS bit clocks), so there is no third PLL to have. Every
audio clock is counted down from the 48 MHz `sys_clk`.

`RATE` is the half-period of BCK in `sys_clk` cycles:

```
f_bck = CLK_HZ / (2 * RATE)        f_s = f_bck / 32
f_s   = CLK_HZ / (64 * RATE)
```

| RATE | fs | error | note |
|---|---|---|---|
| 16 | 46875.0 Hz | +6.29% vs 44.1k | exact S/PDIF half-cell — see below |
| **17** | **44117.6 Hz** | **+0.04%** | default. 0.7 cents. Inaudible. |
| 34 | 22058.8 Hz | +0.04% | |

`RATE` is a register rather than a parameter because a MOD player and a
sample player want different answers and neither wants a rebuild. The
power-on value is `AUDIO_RATE_RESET` in `boards.vh`, defaulting to 17.

`RATE` is clamped to a minimum of 2 in hardware. A zero written there
would make the tick true every cycle and free-run the serialiser at
24 MHz — not a hang, but not something userspace should be able to do
to itself either.

---

## The sigma-delta

Second-order error-feedback, running at `sys_clk`, so the oversampling
ratio is the whole frame period: 48 MHz / 44.1 kHz = **1088**.

```
v  = x + 2*e1 - e2
q  = (v >= 0) ? +SD_FS : -SD_FS
e1 = clamp(v - q)
e2 = e1(previous)
```

The noise transfer function is (1 − z⁻¹)², which at that OSR pushes
quantisation noise far enough above the audio band for the board's RC
filter to deal with what is left. First order would have been about 20
LUTs cheaper and audibly worse; not a trade worth taking with 10K LUTs
free.

**`SD_FS` is 49152 — 1.5× full scale — and that headroom is load
bearing.** A second-order loop is only conditionally stable. Driven to
its own full scale, an input near 0 dBFS walks the integrators into a
limit cycle, and the symptom is a rasp rather than silence, so it does
not announce itself as a failure. The cost of the headroom is output
amplitude (a full-scale sample produces 83% duty, not 100%) and about
3.5 dB of SNR. The clamp at 4×`SD_FS` is the second line of defence and
should never engage on real material.

Verified in `rtl/tb/tb_audio.v` — measured output density against
theory:

| input | measured | theory |
|---|---|---|
| 0 | 0.5000 | 0.5000 |
| +16384 | 0.6666 | 0.6667 |
| −16384 | 0.3333 | 0.3333 |
| +32767 | 0.8332 | 0.8333 |
| −32768 | 0.1667 | 0.1667 |

---

## PT8211

16-bit LSB-justified. `phase[5:0]` counts 64 half-BCK slots per frame —
32 BCK periods, 16 per channel — so BCK is `phase[0]` and WS is
`phase[5]`, both free from one counter.

The DAC samples DIN on the **rising** edge of BCK, so DIN is updated on
the falling edge: half a BCK period of setup, 354 ns at the default
rate. With exactly 16 bits per half-frame, LSB-justified means the word
starts immediately after the WS edge, so MSB-first from the transition
is the whole rule.

### WS polarity is the one thing not verified against hardware

Which half of the frame is *left* is a `swap_lr` input, not a constant.
The part is documented both ways in different places and these boards
were not built to test it.

If the channels come out swapped, the fix is `CTRL.SWAPLR` — and the
*right* fix is `AUDIO_CTRL_RESET` in `boards.vh` so the board comes up
correct, not a call to `z_audio_swap_lr()` in every app that plays a
sound. Test 3/4 in `sw/apps/audiotest` is what tells you.

---

## Behaviour worth knowing

**`EN=0` mutes; it does not stop the clocks.** A PT8211 with a stopped
bit clock is in an undefined state, not a quiet one. This also happens
to be what a future S/PDIF path needs, for a different reason.

**A write to a full FIFO is dropped, silently.** The alternative is
stalling the ack until there is room, which turns a register write into
an unbounded wait on this bus — and would hang the CPU whenever `EN` is
clear, since the FIFO never drains and the write never completes.
`z_audio_push()` returns `false` in that case. Check it.

**An underrun holds the last frame.** It does not play whatever is
sitting at the read pointer, which would be a frame from a whole buffer
ago — a burst of unrelated audio is a far worse noise than a held
sample and much harder to recognise for what it is. `STATUS.UNDERRUN`
is sticky and says it happened.

**Check `Z_FEATURE_AUDIO` before reading `MAGIC`.** On a bitstream
built before `rtl/audio.v` existed, `0x7000_05xx` is decoded by nothing,
and an undecoded address on this bus gets no ack at all — the CPU waits
for it forever. That is a dead hang, not a read of undefined data. The
feature bit lives at `0x7000_0008`, which every bitstream ever built
decodes. `z_audio_present()` does it in the safe order; use it. Same
hazard and same rule as `rtl/cache.v`, `rtl/rtc.v` and `rtl/trng.v`.

---

## Register map

`0x7000_05xx` — the sixth tenant of nibble 7, alongside `csrs` (`00xx`),
`cache` (`01xx`), `socctl` (`02xx`), `rtc` (`03xx`) and `trng` (`04xx`).
No decode mask change was needed: `sysctl.v` widened the tenant mask to
`0x700` when `trng` arrived, and its own comment says the leftover
`05xx..07xx` range is what a future sixth tenant will want.

| # | Addr | | |
|---|---|---|---|
| 0 | `0500` | `MAGIC` | R | `0x5A415544` "ZAUD" |
| 1 | `0504` | `CTRL` | RW | see below |
| 2 | `0508` | `STATUS` | R | see below |
| 3 | `050c` | `DATA` | W | `{ left[31:16], right[15:0] }` |
| 4 | `0510` | `RATE` | RW | `[7:0]` half-BCK period in sysclk cycles |
| 5 | `0514` | `WMARK` | RW | `[15:0]` interrupt watermark, frames |
| 6 | `0518` | `CONFIG` | R | `{ 0x5A41, 3'b0, HAS_MIXER, FORMATS, DEPTH_LOG2 }` |
| 7 | `051c` | `CLKHZ` | R | `CLK_HZ`, for computing fs exactly |
| 8 | `0520` | `MIXVOL` | RW | `[7:0]` mixer master scale, `[11:8]` S/PDIF `fs_code` |
| 9 | `0524` | `MIXSTAT` | R | `[7:0]` bit per channel, set while sounding |
| 16..63 | `0540`+ | per-channel mixer state | W | six words per channel |

`CTRL`: 0 `EN`, 1 `IRQEN`, 2 `SWAPLR`, 3 `FLUSH`, 4 `CLRUR`, 5
reserved for an independent digital-output enable, 6 `MIXEN` (take the
DAC's input from the hardware mixer instead of the FIFO). `FLUSH` and
`CLRUR` are command bits — never stored, always read back 0.

`STATUS`: `[15:0]` `LEVEL`, 16 `EMPTY`, 17 `FULL`, 18 `BELOW`,
19 `UNDERRUN`.

`CONFIG` `FORMATS`: bit 0 sigma-delta, bit 1 PT8211, bit 2 S/PDIF.
Bit 12 says the hardware mixer is **built** — software cannot infer
that from anything else, because `MIXSTAT` reads 0 both when the mixer
is absent and when it is present with every channel idle. A build
without `AUDIO_MIXER` used to announce "mixing in HARDWARE" and then
play silence.

The `0x5A41` signature in the top half is the same trick, for the same
reason, as `socctl.v`'s `VIDEO` register: a bitstream can have this
block (so `MAGIC` is right) and predate this register, and that read
returns zero — indistinguishable from a working block reporting a depth
of 1 and no DACs.

**Per-channel mixer state**, channel `c` at word `16 + c*6`, write
only — these go straight into `audio_mixer.v`'s register file and do
not read back:

| n | | |
|---|---|---|
| 0 | `BASE` | byte address of sample data in main memory |
| 1 | `LEN` | sample length in bytes |
| 2 | `LOOPST` | loop start, bytes from `BASE` |
| 3 | `LOOPLEN` | loop length; 0 means one-shot |
| 4 | `STEP` | phase increment per frame, 18.14 fixed point |
| 5 | `CTRL` | `{ offset[31:24], TRIG[17], EN[16], gain_r[15:8], gain_l[7:0] }` |

`TRIG` restarts the sample from `offset` × 256 bytes — exactly
ProTracker's `9xx` effect, which is why the field is that shape. `EN`
without `TRIG` changes gains **without** restarting, which is what a
volume-only tracker row needs and the most common write here.

`wb_adr_i` is masked to **six** bits by `sysctl.v`, not three as `rtc.v`
and `trng.v` use, so the whole 256-byte window is covered — which is
what left room for the per-channel registers above. Masking at all is not
optional: at `0x7000_05xx` the raw `wbm_adr_sel_word` is `0xC0`-style
offset `0x140` for register 0, and unmasked, no case ever matches —
writes vanish and `MAGIC` reads zero, with no error anywhere.

---

## Testing

Five testbenches, all passing. Every one of them models the thing on
the far side of the wire rather than comparing against expected bits —
the pattern `rtl/tb/tb_spim.v` established, and the reason is recorded
under each.

    # audio block: registers, FIFO, PT8211, sigma-delta, interrupt
    iverilog -g2005 -I rtl -o /tmp/t rtl/tb/tb_audio.v \
        rtl/audio.v rtl/audio_out.v rtl/audio_mixer.v rtl/audio_spdif.v
    vvp /tmp/t +quick        # +quick skips the 65536-value sweep

    # hardware mixer, against a behavioural wishbone memory
    iverilog -g2005 -I rtl -o /tmp/t rtl/tb/tb_audio_mixer.v rtl/audio_mixer.v
    vvp /tmp/t

    # S/PDIF, against a biphase-mark receiver
    iverilog -g2005 -I rtl -o /tmp/t rtl/tb/tb_audio_spdif.v rtl/audio_spdif.v
    vvp /tmp/t

    # three-master bus fairness
    iverilog -g2005 -I rtl -o /tmp/t rtl/tb/tb_arbiter_main.v rtl/arbiter_main.v
    vvp /tmp/t

    # MOD engine, on the host compiler
    cd sw/apps/track && make test

`-I rtl` is required for the audio ones: `audio.v` includes `boards.vh`
to see `AUDIO_MIXER`, and `tb_audio.v` defines it itself so the
testbench builds the same configuration a board does rather than
silently testing the mixer-less variant and reporting a pass for it.

`tb_audio.v` covers nine groups: register interface, FIFO
(fill/full/drop/flush), frame rate against the arithmetic, PT8211 round
trip including both `SWAPLR` polarities, **all 65536 16-bit values**
through the serialiser, underrun hold-and-sticky-bit, sigma-delta DC
transfer, sigma-delta stability at full scale, and the interrupt.

### Three testbench bugs worth recording

The first run reported three failures, and **all three were in the
testbench, not the design.** Two are the exact trap `tb_spim.v`'s own
history warns about.

1. **The clock was 47.6 MHz, not 48.** `#(21/2.0)` is a 21 ns period.
   That is 0.8% slow, and it failed a *correct* frame divider.
   Measuring a rate to 0.1% means generating the reference to better
   than that.
2. **The receiver compared two halves of different frames.** The WS-low
   word is captured at the rising WS edge and the WS-high word half a
   frame later at the falling edge, so sampling both at an arbitrary
   moment gets words one frame apart — which looks exactly like a
   serialiser bug. Both receivers now latch an atomic pair at the
   falling edge only, and that is the only pair safe to compare. The
   exhaustive sweep showed `A` matching every value and `B` lagging by
   one, which is the signature of this bug rather than an RTL one.
3. **`check()`'s name argument was 255 bits**, silently truncating
   every message to 31 characters.

A receiver model that ignores clock edges proves nothing about a
serialiser — it only proves the bits were emitted in some order at
some time.

### On hardware

`run audiotest`. Deliberately the dullest possible consumer of
`zaudio.h`: no window, no mixer, no file loading, nothing that could
itself be the broken thing. Tone, sweep, left-only, right-only,
silence. If it makes a clean tone then the wiring, dividers, FIFO,
decode and serialiser are all correct and anything that misbehaves
afterwards is a software problem.

That ordering is the point of doing it before the MOD player. A tracker
exercises pitch, volume, looping and multi-channel mixing at once, so
when it sounds wrong there is nothing to bisect against. This gives it
something.

---

## Phasing

All three phases are built. The ordering is recorded because the
reasoning applies to whatever gets added next, not because anything
here is still pending.

**Phase 1 — output stage, FIFO, registers, interrupt, tone test.** No bus master, no arbiter change, no PLL. Independently useful
and independently flashable.

**Phase 2 — MOD player, software only.** `sw/apps/track`. Still
CPU-mixed. No RTL changes at all, which is what makes it a safe step:
it produces the working reference that phase 3's register interface
gets designed *against* rather than guessed at. See
[The MOD player](#the-mod-player) below.

**Phase 3 — hardware mixer and arbiter extension.** Done. See below.

The point of the hardware mixer is not audio quality. It is that
software mixing costs 15% of a CPU that is simultaneously running game
logic and filling a 320×240 back buffer. It is the difference between
"music plays" and "music plays while the game runs".

---

## The hardware mixer

`rtl/audio_mixer.v`. Eight channels, time-multiplexed, reading sample
data from main memory as the third master on `arbiter_main`.

### The measurement that justified it

Four channels at 22 kHz mixed in software cost about **40% of a whole
48 MHz picorv32** on Obst. The kernel schedules round-robin at 732 Hz
and both `wm` and `sh` busy-poll, so a player gets a third of the CPU.
A third of a CPU against a 40% job plays the music at 83% speed —
measured on hardware, reported by the player's own `mix` readout.

No FIFO size fixes that, and no tuning closes a 3× gap. The
requirement is simply: **audio must not care how many processes are
running.**

With the mixer, the CPU's entire remaining job is writing channel
registers when a tracker row changes — about fifty times a second.
Everything that scaled with the sample rate now happens on a clock that
cannot be preempted.

### Shape

At 44.1 kHz there are 1088 sys_clk per frame. Eight channels at ~10
cycles each is ~80, about 7% utilised — so one sequencer walking eight
sets of state, not eight parallel units idling 93% of the time.

Per channel: read state → fetch the sample byte over the bus → scale by
two gains → accumulate → advance the phase → handle the loop → write
the phase back. A disabled or finished channel costs one cycle and no
bus transaction.

Phase accumulator is 32-bit with 14 fractional bits, the same split the
software engine uses and for the same reason: 131070 bytes needs 18
integer bits, and 131070 × 2¹⁴ still fits.

Sample data is 8-bit signed only. Every tracker format in this family
uses it; a 16-bit path would double the fetch for nothing.

### Cost

See [Measured cost](#measured-cost) for the numbers — the mixer is 1733
`TRELLIS_COMB` on Obst, and it adds **no block RAM**. The six config
words per channel are written only by the CPU and read only by the
sequencer, one writer and one reader, which is exactly the shape
distributed RAM wants. `pos` and `active` are written from both the
sequencer and a CPU trigger, so those are ordinary registers.

### One operation per state, one shared multiplier

The sequencer is deliberately longer than it needs to be. The first
version packed each channel into four states, which put this in a
single cycle:

> LUT-RAM read → 32-bit add → shift → compare against another 32-bit
> add → mux → 32-bit subtract → register

Four carry chains deep, and there is no reason to accept it: eight
channels at ~11 cycles each is 88 of the 1088 sys_clk in a 44.1 kHz
frame, so cycles are the one resource this block has in abundance.
Every state now does one 32-bit operation from registered inputs, with
the loop arithmetic split across `ST_PREP` and `ST_PREP2` because
`pos + step` feeds both halves of it.

Same argument for the multiplier. Computing both gains and both output
scales as separate combinational expressions asked for four
multipliers, which yosys spread over seven DSPs. There are eighteen
multiplies to do per frame and a thousand cycles to do them in, so one
shared multiplier fed by the sequencer is strictly better:
**MULT18X18D 12 → 8**, shorter paths, nothing audible lost.

### Arbitration

`arbiter_main` goes from two masters to three, with a 2-bit rotating
pointer rather than the old single "prefer the other one" flag — three
masters can't be arbitrated with one bit.

**Audio is in the rotation, not below it.** The original plan gave it
lowest priority on the grounds that it has a FIFO and the others don't.
It asks for eight single-word reads per frame — 353k reads/sec at
44.1 kHz, under 1% of the bus. A master that light perturbs nobody by
being in the rotation, and putting it below invents a starvation corner
that appears exactly when a game is rendering hard, which is when the
audio must not break.

`rtl/tb/tb_arbiter_main.v` now runs three contending masters and checks
all three make progress: 400/400/399 transactions, no data errors.

### Both paths coexist

`CTRL` bit 6 (`MIXEN`) selects whether the DAC reads the mixer or the
FIFO. One mux; the paths are otherwise independent, so `audiotest` and
any software-mixed app keep working, and there's a fallback if the
mixer misbehaves on real hardware.

`sample_valid` is tied high in mixer mode — the mixer always has a
frame ready (silence is a frame of zeros), so there is no such thing as
a mixer underrun.

### The address trap

**The mixer issues physical addresses; an app's pointers are virtual.**
A process sees memory through the MTU, which remaps `0x8000_0000`. Hand
the mixer a `modbuf` pointer unchanged and it fetches samples from
whatever physically lives at that offset — the BIOS and the kernel. It
would play, and it would play the wrong memory.

`reg_mtu_base` is readable from an app (only `0x8xxx_xxxx` is
translated, so a load from `0x9000_0000` reaches the MTU itself), and
reads 0 where no translation is active — so `phys_of()` in `mod.c` is
correct in both contexts with no special case. Same class of mistake as
the `k_proc_create()` bug in `docs/app_runtime.md`.

### Trigger vs enable

`CH_CTRL` carries both. **`TRIG` restarts the sample; `EN` without
`TRIG` does not.** A tracker changes volume mid-note far more often
than it starts a note, and restarting on every volume change is the
classic way a hardware channel ends up sounding clicky. The trigger's
offset field is in units of 256 bytes, which is exactly ProTracker's
`9xx` sample-offset effect — that's why the field is that shape.

### One engine, two back ends

`modplay.c` gained `modplay_advance()` and `modplay_hw_channel()`
rather than a second player. The same `do_tick()` runs — patterns,
effects, tempo, all shared — and only the last step differs. A separate
hardware player would mean every effect implemented twice, and a bug
fixed in one living on in the other.

### Testing

`rtl/tb/tb_audio_mixer.v`, 22 checks: pitch from step, per-side gains,
one-shot end detection, loop wrap over 200 frames, eight-channel
summing, `mixvol` scaling, trigger offset, gain-change-without-restart,
and correctness under a deliberately slow bus (10 wait states per
read). The behavioural memory checks the mixer holds `cyc`/`adr` stable
for a whole transaction and never asserts `we`.

Bugs it caught: the trigger offset was built as `{offset, 24'b0}` and
then shifted again, walking the field off the top of the word and
losing `9xx` entirely; and my own `one_frame` task sampled `dut.state`
in the same timestep as its non-blocking update, so it returned before
the frame started and failed 17 of 22 checks against correct RTL.

### The one it did NOT catch, and why

The output scaler was written `mul_p[32:9] >>> 1` instead of
`mul_p >>> 10`. Those look equivalent and are not: **a part select of a
signed value yields an unsigned result**, so the second shift brought
in a zero rather than the sign bit. Every negative product came out as
a large positive one and clamped to +32767.

    mul_p = -1048576   old expression -> +8387584   correct -> -1024

On hardware that is a rectified signal pinned at full scale. It still
sounds like music, it is extremely loud, and it is **immune to the
volume control** — positive half-cycles scale with `MIXVOL` normally
while negative ones rail regardless. "Turned the volume down and it's
just as loud" is the diagnostic signature.

Twenty-two checks passed over it because the test memory is a ramp
`mem[i] = i` and every test happened to read bytes 0..15 — all
positive. **A signed datapath tested only with positive values is not
tested.** There is now a group that reads bytes 192..193, straddles
127/−128, and checks a negative sum against a non-default `mixvol`.


---

## The player, `track`

`sw/apps/track` — the reference workload the whole subsystem was
designed around, and the app the dock launches with the music icon. A MOD exercises multi-channel playback,
per-channel pitch, volume and looping samples at once, which is why it
is the test app rather than a sine wave.

    > run track

Windowed under the wm, with a console fallback when there is no wm to
talk to (`z_win_create_flags()` fails rather than hanging, so it
doubles as the probe). The console path is not kept for nostalgia — it
is the one to use when something is wrong, because it depends on
nothing but the audio block and stdout.

Scans the root directory for `.mod` files and plays through them,
wrapping rather than exiting: this is a player meant to be left running
while other things happen.

| key | |
|---|---|
| `n` | next module |
| space | pause |
| `-` / `=` | volume |
| `[` / `]` | stereo separation |
| `v` | pattern view on/off |
| `r` | sample rate, 44.1k / 22k / 11k |
| `q`, Escape, close icon | stop |

22 kHz by default, or 46875 Hz on a board that sets
`AUDIO_RATE_RESET 8'd16` for S/PDIF.

### The display

A tracker-style pattern grid: seven rows with the playing row in the
middle, inverted. Each channel column shows note, sample and effect
(`C-2 01 A08`), decoded through `modplay_get_cell()` so the engine
stays the only thing that knows the on-disk layout.

Three things about it are load-bearing rather than cosmetic.

**No single draw may block the audio loop.** The grid is redrawn ONE
ROW PER MAIN-LOOP ITERATION, not the whole grid in one call.

That is structural, not an optimisation. Measured on hardware, a full
eleven-row redraw is **131 ms of wall time** — about 44 ms of work,
stretched by sharing the CPU three ways with `wm` and `sh`. A tracker
tick is 20 ms, so every grid redraw blocked `feed_hw()` for six or
seven ticks, and the player made the time back in a burst. That is
what produced an audible speed-up at the end of every pattern, at rows
59 through 4-of-the-next — the only place in a song where the grid has
blank rows to fill.

Making the rows cheaper would only have moved the threshold. One row
is ~16 ms of wall time, comfortably inside a tick, and `feed_hw()` runs
between every one of them.

**The grid is seven rows, and the limit is time, not space.** A row
costs ~16 ms and a tracker row at the default speed lasts 120 ms, so
a taller grid needs longer than that and can never finish before the
next row change
restarts the redraw. The first few slots always got drawn and the last
ones never did — the bottom three or four lines stopped updating
altogether. Seven rows is 115 ms, inside the budget.

Rows are drawn **centre-out** — the playing row first, then outwards.
The order only matters when a redraw runs out of time, which it will on
a module with a fast speed setting. Top-down drops the rows nearest the
cursor, the ones actually being read. Centre-out degrades the other way
round: the outer rows lag, which nobody notices, and the playing row is
always current.

The diagnostic that found it is still there: the bottom line shows the
worst wall time each loop phase took over the last second, in kernel
ticks. `fd` feed, `ms` messages, `pt` pattern, `st` status. `pt96` is
what named the culprit after three wrong guesses.

**The grid is redrawn when the playing ROW changes, not on a timer.**
A row at the default speed lasts 120 ms, so that is about eight
redraws a second and the display cannot get busier than the module it
is showing. Redrawing every pass of the main loop would spend more CPU
on glyphs than on mixing — which, on a board where the mixer is already
the bottleneck, makes the display the cause of the underruns it is
displaying.

**`v` turns the grid off entirely**, leaving the status line. That is
the setting to reach for when `mix` is below 100 and you want to know
how much of it is the UI.

Note names come from `period_to_note()` — the same rounding the player
itself uses — so what is shown is what will actually be played. A
separate nearest-note search in the UI could disagree with the audio on
a module whose periods are slightly off the standard table, and a
display that disagrees with the audio is worse than no display.

### Mixer inner loop

The mix is **channel-outer, sample-inner**. The obvious arrangement
reloads every field of `mod_channel_t` on every sample, because the
compiler cannot prove the accumulators don't alias the player struct.
Inverting the loops hoists all of it into registers for a block at a
time. Per channel per sample, measured on the cross-compiled RV32IM
output:

| | before | after |
|---|---|---|
| loads | 8 | 3 |
| multiplies | 3 | 2 |

Volume and pan are folded into `gain_l`/`gain_r` once per tick by
`update_gains()` — sixteen multiplies per tick instead of two per
channel per sample. Done in one place at the end of every tick rather
than at each of the half-dozen sites volume can change (note trigger,
`Cxx`, `Axy`, `EAx`/`EBx`, `ECx`, tone-porta-plus-slide), so it cannot
fall out of step.

This matters more here than it would elsewhere: picorv32 on Obst has no
instruction cache and its main memory is SRAM, so every avoided load is
a real bus cycle rather than a cache hit.

### Memory: the module lives in `.bss`

This is worth reading before changing it, because the obvious version
is wrong and fails in a misleading way.

A process's stack tier — `Z_PROC_STACK_SIZE_DEFAULT`, 16 KB for any app
not named in `z_proc_stack_size_for()` — is the **only** room its C
stack *and* its `malloc()` heap ever get, shared, for its whole life.
It is not a stack allowance with a heap somewhere else.

So the first version of this app, which did `fs_mallocfile(path)`,
could never have worked for any real module: an 87 KB MOD wants 87 KB
of heap out of a 16 KB allowance. The failure is a `NULL` return that
is indistinguishable from "file unreadable", which is exactly what it
unhelpfully reported on hardware:

    > run track
    loading /DOODLE.MOD (88982 bytes)
      out of memory (or unreadable).

Static footprint is a different budget. Code, `.rodata` and `.bss` are
part of the binary, and `k_proc_create()` sizes the process block as
image + stack tier — so a `.bss` array is memory the kernel reserves up
front, at process creation. It either fits or the process never starts,
which is a much better failure than starting and dying on the first
allocation. `repl` already does exactly this with its 96 KB Scheme cell
heap; `kernel.h`'s tier comment calls it out.

`MOD_MAX_FILE` (default 192 KB) is therefore a hard compile-time cap,
and the app refuses an oversized module with both numbers rather than
truncating it. Truncation would not be a quiet degradation: pattern
data comes first in a MOD and sample data last, so a truncated module
plays all the right notes with missing instruments — which sounds
exactly like a mixer bug.

    make -C sw/apps/track MOD_MAX_FILE=$((1024*1024))     # 32MB boards

Rough budget on a 1 MB board, where this is tightest: about 227 KB of
own objects (of which 192 KB is the buffer) plus roughly 80 KB of libc,
plus the 16 KB tier — call it 320 KB against the ~450 KB free with wm
and a shell resident. Lower `MOD_MAX_FILE` if that is too close for
what else you want running.

One happy consequence: because nothing here allocates, the default
16 KB tier is ample and `track` needs **no** entry in
`z_proc_stack_size_for()`. The kernel needs no change to run it.

### Feeding the FIFO without stalling the window

`feed_audio()` must not block, and that is a window requirement rather
than an audio one. The console-only first version spun until the FIFO
had room, which is fine when there is nothing else to do. Under the wm
the same loop also services messages and redraws, so a spin would stall
the message queue for exactly as long as the FIFO stayed full — the
window would stop responding to drags and to its own close icon while
the music played perfectly.

So it pushes only into space that already exists and returns. The
partially drained block survives across calls in file scope; making it
local would silently drop samples on every pass where the FIFO filled
mid-block.

### The split, and why it matters

| | |
|---|---|
| `modplay.c` / `modplay.h` | the engine. Includes **nothing** from `sw/common` and touches no hardware. |
| `mod.c` | the app: file loading, the audio FIFO, the keyboard. |

That boundary is not tidiness. It is what lets the mixer be compiled
with the host compiler and checked against known input on a machine
where a wrong answer prints as a number:

    cd sw/apps/track && make test        # add test-wav to dump .wav files

Eight groups: parsing and rejection, pitch (zero-crossing frequency
against the Amiga period formula), volume and panning arithmetic,
looping vs one-shot, tempo and speed, effects, mix headroom, and a
60-second chunked run. Modules are **synthesised** by the test rather
than shipped — a real `.mod` is a copyrighted artifact, and a fixture
whose correct output nobody can state is not a test.

### Three real bugs it caught before any of this reached a board

1. **The finetune multiplier table was `uint16_t`.** Its first entry is
   65536, which wraps to **zero** — so finetune 0, the commonest value
   there is, multiplied every period by nothing. Every note came out at
   the clamped minimum period. By ear this is "the module plays but
   every note is the same high pitch"; on the host it is one failing
   assertion naming the exact value.
2. **The pan law lost 6 dB in the centre.** Panning as `128 ± half`
   sums to 256 and attenuates a centred channel by half, so a module at
   separation 0 was half as loud as the same module hard-panned, and no
   single master scale could be correct for both. Now the near side
   stays at full gain and only the far side is attenuated, which makes
   the worst case identical either way.
3. A stale unused variable left over from the fix, caught by
   `-Wextra`.

The pitch and headroom tests are the ones that found 1 and 2. Neither
would have been obvious on hardware.

### Engine notes

**Fixed point is 32-bit with 14 fractional bits**, and that split is
forced from both ends. A ProTracker sample can be 131070 bytes, so the
integer part needs 18 bits; 131070 × 2¹⁴ = 2147450880 fits a `uint32`
with headroom, and 2¹⁶ would overflow on the largest legal sample.
1/16384 of a sample is under a cent of pitch resolution.

**No 64-bit arithmetic anywhere.** On a 32-bit core a 64-bit divide is
a libgcc call. The step calculation wants to overflow, so it computes
four times the frequency first and shifts by 12: the largest
intermediate is 125554 << 12 = 514269184, comfortably under 2³⁰.

**Finetune is a multiplier, not 16 period tables.** ProTracker ships
16 × 36 periods — 1152 bytes — for something that is a single multiply
by 2^(−f/96) to within a period unit. One 36-entry table plus 16
multipliers is 104 bytes. On a 1 MB board that trade is worth making.

**Supported effects:** 0 arpeggio, 1/2 portamento, 3 tone portamento,
4 vibrato, 5/6 combinations, 9 sample offset, A volume slide, B
position jump, C set volume, D pattern break, E1/E2 fine portamento,
E9 retrigger, EA/EB fine volume slide, EC note cut, EE pattern delay,
F speed/tempo. Ignored: 7 tremolo, 8/E8 panning, E0 filter, E3
glissando, E4 vibrato waveform, E5 finetune, E6 pattern loop, ED note
delay. Nothing in that list is silent-breaking.

15-instrument Soundtracker modules are **rejected** rather than
guessed at: they have no magic word, so "detecting" one means assuming
any file that is not a MOD is one.

### CPU cost

The compiled inner mix loop is roughly 30 instructions per active
channel per frame. At 22 kHz with four channels that is about 2.6 M
instructions per second, which at picorv32's ~4 cycles per instruction
is on the order of 20% of a 48 MHz core — same ballpark as the 15%
this subsystem was planned around, and it roughly doubles at 44.1 kHz.

Treat that as an estimate from static analysis, not a measurement. The
number that settles it is on hardware: the player prints `UNDERRUN`
when the FIFO starves, and that flag is the honest answer to "is the
mixer fast enough".

This is also the whole argument for phase 3. 20% of a CPU is fine for
a player that has nothing else to do and is not fine for a game that
is simultaneously running logic and filling a 320×240 back buffer.

### Polling, not the watermark interrupt

The player spins on `z_audio_push()` rather than using `cpu_irq[7]`. A
game would want the interrupt; a player has nothing else to do, so
polling is simpler, has no handler to get re-entrancy wrong in, and
depends on less of the kernel. If playback misbehaves, the interrupt
path is not among the suspects.

Blocks are 64 frames — half the FIFO. Rendering a whole FIFO's worth
would leave the second half of each block with nowhere to go; rendering
one frame at a time pays call overhead 44100 times a second.

---

## S/PDIF

`rtl/audio_spdif.v` — IEC 60958 consumer format, biphase-mark coded.
One pin, no clock, no PLL. Sits **alongside** `audio_out` rather than
downstream of it: same `frame_req`, same source mux, its own pin, so a
board with both analogue and digital outputs runs both at once.

| board | connector | pin | crystal |
|---|---|---|---|
| Sergei ML1 | optical TOSLINK | `AUD_OPTICAL` A13 | 48 MHz |
| ULX3S | coax | `AUD_OPTICAL` E5 | 25 MHz |

Coax and optical are the same signal into different drivers, so both
use `AUDIO_SPDIF` and the same port name.

**ULX3S's 25 MHz crystal does not change the arithmetic.** `pll0_25`
produces a `sys_clk` of exactly 48.0000 MHz (480 MHz VCO ÷ 10), so the
half-cell is still 8 cycles and fs is still 46875 Hz. Worth checking
rather than assuming — a part landing on 47.99 MHz would have needed a
different divider and a different answer.

### The rate, which is the whole design

A stereo frame is 64 time slots, each biphase-mark coded into two
half-cells, so the line runs at **128 × fs**. From 48 MHz with both
EHXPLLLs already spoken for, the reachable rates are those where
48e6 / (128·fs) is an integer — fs = 375000/N:

| N | fs | vs 44.1k | vs 48k |
|---|---|---|---|
| 7 | 53571.4 | +21.5% | +11.6% |
| **8** | **46875.0** | +6.29% | **−2.34%** |
| 9 | 41666.7 | −5.5% | −13.2% |
| 10 | 37500.0 | −15.0% | −21.9% |

No standard rate is among them: 44.1 kHz wants N = 8.5034, 48 kHz wants
7.8125.

**So it runs at 46875 Hz** — N=8, half-cell exactly 8 `sys_clk`. That
is 2.34% below 48 kHz, much closer than the 6.29% above 44.1 kHz, and
comfortably inside the capture range of anything that recovers its
clock from the biphase stream, which is every receiver. Channel status
declares 48 kHz (`fs_code`, register 8 bits 11:8); that field is
advisory and receivers PLL to what actually arrives.

Exact, so there is no jitter at all, and no new clock domain.

### Why there is no upsampler

Two reasons, and the first is the one that matters.

**A tracker has no native rate.** MOD playback resamples every channel
through a phase accumulator anyway, so the output rate is a free
parameter and 46875 costs nothing — pitch and tempo are unaffected. Set
`AUDIO_RATE_RESET` to 16 and the whole audio block, analogue and
digital alike, runs at one rate.

**And 1.0629 is a real sample-rate conversion, not an upsample.**
Zero-order hold at that ratio aliases across the whole band — plainly
audible, and worse than the 6% pitch shift it was meant to avoid. Doing
it properly needs an interpolating resampler, which is more logic than
the mixer itself. The only case that would want it is playing 44.1 kHz
material unresampled while also feeding a digital output, which is not
what this machine does.

### RATE must be even

A half-cell is `rate/2` `sys_clk`, so an odd `rate` cannot be divided
evenly. 16 is the intended value; the analogue default of 17
(44117.6 Hz) is odd and produces a slightly ragged line. Receivers
tolerate far worse, but a board with a transmitter should come up at
16, which is what `AUDIO_RATE_RESET 8'd16` in `boards.vh` does.

### Frame format

192 frames per block, 2 subframes per frame, 32 time slots each:

| slots | |
|---|---|
| 0–3 | preamble, 8 half-cells, **not** biphase coded — the violation is what makes frame boundaries findable |
| 4–7 | aux, zero |
| 8–27 | audio, 20 bits LSB first |
| 28 | V, validity — 0 means valid |
| 29 | U, user data |
| 30 | C, one bit of the 192-bit channel status word |
| 31 | P, even parity over slots 4–31 |

A 16-bit sample sits in the **top** 16 of the 20-bit audio field
(slots 12–27) with 8–11 zero. That left-justification is what makes it
play at the right level on a 20- or 24-bit receiver rather than 16×
too quiet.

Preambles are emitted as absolute line states, inverted when the
previous half-cell left the line high, and the pattern is **latched**
at the transition that starts it — see the bug list below.

### Testing

`rtl/tb/tb_audio_spdif.v` is a **receiver**, not a bit comparator. It
finds preambles by their biphase violation, decodes biphase-mark back
into bits, and reconstructs samples, parity, block structure and
channel status from the wire.

    iverilog -g2005 -o /tmp/tb rtl/tb/tb_audio_spdif.v rtl/audio_spdif.v
    vvp /tmp/tb

That distinction earned itself immediately. Four bugs, none of which a
comparator against an expected bit vector would have caught:

1. **Half-cell indexing off by one.** The value assigned on a tick is
   held for cell `hc+1`, not `hc`. Every bit was present, half a cell
   out of position — a comparator sees the right bits, a receiver
   decodes nothing.
2. **`blkframe` advanced twice per frame**, because `frame_req` and the
   `hc` wrap both fired. The block counter never reached 0, so the Z
   preamble was never emitted and every channel-status bit read zero.
   The *audio* decoded perfectly; a receiver would simply never have
   found the start of a block. Fixed by giving each counter one owner:
   `frame_req` latches samples, the half-cell counter owns the frame.
3. **The preamble's last seven half-cells re-read `blk_next`** after
   `blkframe` had already advanced, so a Z went out with an X's tail.
   The pattern is latched now.
4. Two in the testbench itself — a frame generator two cycles long, and
   a preamble hunter that false-matched against the zeros it had been
   cleared to.

### Reserved

`CTRL` bit 5 remains reserved for enabling the digital output
independently of the analogue one. `CONFIG` `FORMATS` bit 2 now reports
a transmitter is present.

`DATA` stays 16+16. S/PDIF carries 20 or 24 audio bits per subframe,
but a 16-bit sample left-justified into a 20-bit field is a legal frame
and costs nothing. If true 24-bit digital out is ever wanted it should
be a *second* register pair, not a widening of this one — widening
`DATA` would double the FIFO's cost on the board with least room.

---

## Build knobs

Set per board in `rtl/boards.vh`; defaults live in `rtl/sysctl.v`.

| | default | |
|---|---|---|
| `AUDIO` | per board | FIFO, output stage, registers, interrupt |
| `AUDIO_SD` | — | two 1-bit sigma-delta DACs |
| `AUDIO_PT8211` | — | PT8211/TM8211 serial DAC |
| `AUDIO_SPDIF` | — | IEC 60958 transmitter |
| `AUDIO_MIXER` | per board | eight channels of hardware mixing |
| `AUDIO_FIFO_LOG2` | 10 | FIFO depth is 2^this, in frames |
| `AUDIO_RATE_RESET` | `8'd17` | power-on rate divider; 16 on S/PDIF boards |
| `AUDIO_MIXER_CH_BITS` | 3 | log2 of the mixer's channel count |
| `AUDIO_CTRL_RESET` | `8'h00` | power-on `CTRL`; set bit 2 for `SWAPLR` |

More than one output define may be set on a board that has more than
one; the FIFO and mixer are shared and only the last stage differs.

Software side, in `sw/apps/track/Makefile`:

| | default | |
|---|---|---|
| `MOD_MAX_FILE` | 192 KB | largest module, a static `.bss` buffer |
| `MOD_VOLUME` | 35 | startup volume, 0..100 |

---

## Still to do

- **Editing.** `track` is a player. Pattern editing is real UI work and
  wants its own pass.
- **`CTRL` bit 5**, reserved for enabling the digital output
  independently of the analogue one on a board that has both.
- **24-bit digital out**, if ever wanted, as a second `DATA` register
  pair rather than a widening of the existing one.

### Composite video

`VIDEO_D[3:0]` is present in `boards/obst_v0.lpf` and
`boards/lakritz_v0.lpf` but **commented out**, for the planned "game
mode" output. Nothing drives them.

They are commented rather than live because `nextpnr-ecp5` most likely
only warns about a `LOCATE` for a port the top module does not have,
but "most likely" is not a good enough reason to risk a build that
currently places. Uncomment them at the same time as adding the port to
`sysctl.v` — the two halves belong together and neither is useful
alone.

| Board | D0 | D1 | D2 | D3 |
|---|---|---|---|---|
| Obst | G4 | G3 | G2 | H2 |
| Lakritz | P1 | R1 | P2 | N4 |

Shared with the audio jack on both boards.
