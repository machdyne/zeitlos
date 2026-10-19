# DDMI audio

Sound over the same connector and the same four TMDS pairs as the
picture, plus a few bytes of display information that let a TV show
640x480 properly. **In progress.** Round 1 built the exact sample rates
it depends on and the bench it is checked against; round 2 sent the
display information (data islands, the AVI and SPD InfoFrames, the
video preamble and guard band), switched by `system.video.ddmi_audio`;
round 3 (this one) adds the **sound**: every frame the board plays, its
clock, and the Audio InfoFrame. What is left is turning it on per board
(round 4) and trying it on real displays. With the setting off a board
sends exactly the DVI it always has, and the bench proves that bit for
bit.

- [What it is](#what-it-is)
- [Status](#status)
- [Turning it on](#turning-it-on)
- [Design](#design)
- [The bench](#the-bench)
- [What it costs](#what-it-costs)
- [Phases](#phases)

## What it is

DDMI is DVI over an HDMI-type connector: three TMDS data pairs and a
clock pair, nothing else wired (no DDC, no hot-plug detect, no CEC) on
any board. Displays that take that connector also accept audio carried
in **data islands**: short bursts of packets sent in the blanking
intervals, on the same three data pairs, between the control
characters that carry the syncs. No new pins, no new clocks.

What a data island can carry, and what DDMI audio will send:

| packet | what it says |
|---|---|
| audio samples | up to four stereo frames per packet, 16-bit PCM, with IEC 60958 channel status (the same bits S/PDIF carries) |
| audio clock regeneration | N and CTS: the display rebuilds the sample clock as f_pixel × N / CTS |
| AVI InfoFrame (CEA-861) | 640x480 (VIC 1), 4:3, full-range RGB, "IT content", underscan |
| Audio InfoFrame (CEA-861) | two channels, details in the stream |
| SPD InfoFrame (CEA-861) | "Zeitlos", PC |

The AVI InfoFrame is worth having even without sound: it tells a TV
not to stretch 640x480 to 16:9, not to crop it for overscan, and not to
treat the RGB as limited range (crushed blacks), and "IT content" often
selects the TV's low-latency mode.

**There is no EDID on these boards**, so Zeitlos cannot ask a display
whether it takes any of this. DDMI audio will therefore be a setting —
`system.video.ddmi_audio` in `zeitlos.cfg`, off by default, applied at
boot — not a detection. With it off the output is bit-identical DVI. A
display that shows nothing with it on is recovered by editing
`zeitlos.cfg` on the card from a PC, or at the serial console with
`ddmi off` (at once, until the next boot) and then a corrected file.

## Status

| | |
|---|---|
| Exact sample rates (48000 / 44100 Hz) | **done** — [audio.md](audio.md#exact-sample-rates) |
| Reference model and stream checker | **done** — `tools/ddmi/ddmi.py`, below |
| RTL capture bench, DVI golden | **done** — `rtl/gpu/bench/tb_ddmi_capture.v` |
| Data islands, video preamble/guard band, AVI/SPD InfoFrames, general control | **done** (round 2), off by default, not yet on any board's default build |
| `system.video.ddmi_audio`, socctl `DDMI`, `ddmi` at the console | **done** (round 2) |
| Audio samples, clock regeneration, Audio InfoFrame | **done** (round 3) |
| `lakritz_ddmi_audio` release target | **done** (round 4a) — [below](#lakritz) |
| Board enables, Schoko's audio block | next |
| Hardware tests | the final phase |

## Turning it on

Three things have to be true:

1. **The bitstream has it.** `GPU_DDMI_AUDIO` (with `GPU_DDMI`). No
   board sets it by default yet. On Lakritz, use the release target
   made for it:

       ./release/zrelease build lakritz_ddmi_audio

   Elsewhere, build with it added:

       make BOARD=sergei_ml1 EXTRA_DEFINES=-DGPU_DDMI_AUDIO

2. **The config says so.** In `zeitlos.cfg`:

       system.video.ddmi_audio: yes

   The kernel applies it at boot and on `cfg reload`; it takes effect at
   the next frame. ([config.md](config.md#systemvideoddmi_audio))
3. **The display takes it.** There is no EDID to ask, so this is
   yours to find out. If the screen goes blank, `ddmi off` at the
   serial console turns it off at once, until the next boot; then set
   `no` in the file (from a PC, or `xf`/`xmf` at the console).

In a program: `z_ddmi_audio_available()`, `z_ddmi_audio_on()` and
`z_ddmi_audio_set()` in `sw/common/zsoc.h` — but the setting is the
person's, made once in the config file, and nothing else should change
it.

**What you hear:** everything the board plays — the same frames as the
3.5 mm jack or S/PDIF, after the system volume, muted when playback is
stopped. Nothing to set: `zaudio.h` keeps the sound's settings in step
whenever an app sets a rate. `audiotest` shows `DDMI` among the
outputs and prints N, CTS and the repeat count.

**What changes on screen with it on:** nothing in the picture. A TV
that used to stretch 640x480 to 16:9, crop its edges or treat the RGB
as limited range (greyed blacks, clipped whites) should now show it as
4:3, whole, full range, and may switch to its game/PC mode. A computer
monitor will usually look the same.

### The register

socctl word 10, `0x7000_0228` ([socctl.md](socctl.md)):

| bits | |
|---|---|
| 0 | `EN`: send data islands and InfoFrames from the next frame. Resets to 0. |
| 1 | `SOUND`: with `EN`, the audio too. Resets to 1. |
| 7:4 | `FS`: the channel-status rate code the samples carry (0 44.1k, 2 48k, 3 32k) |
| 8 | avail (read-only): this bitstream has `GPU_DDMI_AUDIO` |
| 9 | sound avail (read-only): ... and the audio block, so there is sound to send |
| 13:12 | `REP`: each frame is sent 2^REP times, for rates below 32 kHz |
| 31:16 | `0x5A44` ("ZD") signature |

Words 11 and 12 (`0x7000_022c`, `0x7000_0230`) are `ACRN` and `ACRCTS`,
20 bits each: the clock regeneration pair. `FS`, `REP`, `ACRN` and
`ACRCTS` reset to the board's power-on rate (from `AUDIO_FS_RESET`) and
are kept matching by `z_audio_set_rate_hz()`; nothing else should write
them.

`EN` reads 0 and does nothing without avail. gpu_video.v synchronises
it into the pixel clock and adopts it at the last pixel of a frame,
like the phosphor mode, so the output never changes mid-frame.

## Design

All of this is in the pixel-clock domain and in `rtl/gpu/gpu_ddmi.v`
except the sample path, which crosses from `sys_clk`.

**Where things go on a line.** `gpu_video.v` counts each 800-pixel line
from the front porch: hc 0–15 front porch, 16–111 hsync, 112–159 back
porch, 160–799 picture. One data island per line, every line including
vertical blanking:

| hc | |
|---|---|
| 0–11 | control |
| 12–19 | data-island preamble (8 characters) |
| 20–21 | leading guard band |
| 22–85 | two packets, 32 characters each |
| 86–87 | trailing guard band |
| 88–149 | control (62 characters: also the extended control period) |
| 150–157 | video preamble |
| 158–159 | video guard band |
| 160–799 | picture |

Every control period is at least 12 characters, and hsync is constant
through the island (the pulse is 16–111), so a DVI-only receiver loses
no sync edge.

**Packets** (`rtl/gpu/ddmi_island.v`) are built bit-serially as they go
out: one header bit and two bits of each of four subpackets per
character, BCH parity by five 8-bit LFSRs on the fly (G(x) = 1 + x^6 +
x^7 + x^8, LSB first), then TERC4 in `tmds_encoder.v`. Header bit c
leaves on channel 0 bit 2 of character c; subpacket k's bits 2c and
2c+1 on channels 1 and 2, bit k. Channel 0 bit 3 is 0 on the island's
first packet character only; bits 1:0 carry the syncs as they are on
the wire.

What each line carries:

| | slot 0 | slot 1 |
|---|---|---|
| vc 10 (first line of vsync) | samples | general control: clear AV mute |
| vc 11 | samples | AVI InfoFrame |
| vc 12 | samples | SPD InfoFrame |
| vc 13 | samples | Audio InfoFrame (sound on) |
| vc % 8 == 4 | samples | clock regeneration, N and CTS (sound on) |
| every other line | samples | null |

"Samples" is an audio sample packet with whatever frames have arrived,
up to four, or a null packet if none have (or the sound is off): at
48 kHz and 31.5 kHz lines that is one or two a line, at 96 kHz three or
four. The constant packets are in `ddmi_island.v`, generated by
`tools/ddmi/gen_packets.py` from the model the bench checks against,
and `test_ddmi.py` fails if the two drift apart:

| packet | bytes |
|---|---|
| AVI | `82 02 0d`, checksum `bc`, then `12 18 88 01 00` (RGB, active format = picture, underscan; 4:3; IT content, full range; VIC 1; graphics) |
| SPD | `83 01 19`, "Zeitlos", "Zeitlos", source 9 (PC general) |
| general control | `03 00 00`, `10` (clear AV mute) in all four subpackets |
| Audio InfoFrame | `84 01 0a`, checksum `70`, then `01` (two channels; coding, size and rate from the stream) |

### The sound

**The samples** are the frames every output plays: `rtl/audio.v` hands
the same post-volume frame S/PDIF takes, on the same `frame_req`, muted
by the same `EN`, to a 16-frame FIFO in `ddmi_island.v` — once, or 2 or
4 times in a row for 22.05 / 11.025 kHz, which displays do not accept
(they need 32 kHz or more): repeated, 22.05 kHz goes out as 44.1 kHz.
The FIFO crosses from the 48 MHz clock to the pixel clock with Gray
pointers, two flops each way; each line takes whatever has arrived at
hc 12..15, so it never holds more than a line or two (about 50 us). With
the sound off the reader discards everything, so nothing stale comes
out when it is switched back on.

Each frame is a subpacket: left and right in the top 16 bits of their
24, IEC 60958 channel status one bit per frame (consumer, PCM, copy
permitted, the rate code in bits 24..27, as S/PDIF sends it), the block
start as the header's B flag every 192 frames, V = U = 0, and even
parity.

**The clock** travels as N and CTS: the display rebuilds 128 × fs as
25.2 MHz × N / CTS. The sample clock and the pixel clock come from one
crystal, so this needs no counter: N/CTS is a fixed fraction of the
sample clock's STEP/MOD (`40 k STEP / 21 MOD` at 48 MHz), reduced and
then scaled to the N sinks expect —

| rate | N | CTS |
|---|---|---|
| 48 kHz | 6144 | 25200 |
| 44.1 kHz (and 22.05 / 11.025 sent 2 / 4 times) | 6272 | 28000 |
| 32 kHz | 4096 | 25200 |
| 96 kHz | 12288 | 25200 |
| 44117.6 Hz (an old bitstream's divider 17) | 5680 | 25347 |

— all **exact**: 25.2 MHz × N / (128 CTS) is the sample rate to the
last digit. A rate with no exact pair inside the range sinks take gets
the closest pair with CTS ≤ 84000, within 5 ppm (a source that counts
CTS, as most do, wobbles by 40). `z_audio_ddmi_acr()` in `zaudio.h` does
this at run time; `rtl/audio_rates.vh` gives the power-on values; the
host test checks every rate from 8 to 192 kHz.

**The encoder** (`tmds_encoder.v`) gained three kinds of character for
the periods between the pixels — TERC4, the video guard band and the
data-island guard band — chosen by a `MODE` input that `gpu_ddmi.v`
ties to 0 in a build without `GPU_DDMI_AUDIO`. Channels 1 and 2 now
take control bits too (the preambles are CTL patterns on them).

**The samples** are the same post-volume frames S/PDIF takes, latched on
`frame_req`, crossed into the pixel clock through a 16-frame FIFO with
Gray-coded pointers. Below 32 kHz (the 22.05 / 11.025 kHz rates), each
frame is repeated 2 or 4 times.

**Clock regeneration needs no counter.** `sys_clk` and the pixel clock
come from one crystal on every board, so the ratio is exact, and with
exact rates N and CTS are the standard values for a 25.2 MHz pixel
clock: 48 kHz is N = 6144, CTS = 25200; 44.1 kHz is 6272 / 28000;
32 kHz is 4096 / 25200. Software writes them when it sets the rate.

## The bench

    make test_ddmi

Each part checks something the next relies on:

1. `python3 tools/ddmi/test_ddmi.py` — the model against independent
   references: its BCH parity against an open-source table, its TERC4
   and guard-band codes, its TMDS encoder (every byte round-trips, DC
   balance bounded); then whole frames encoded by the model and decoded
   by the checker with every packet above, audio
   recovered bit-exact — and five deliberately broken streams the
   checker must reject (a flipped subpacket bit, a flipped header bit,
   channel 0 bit 3 wrong, a seven-character preamble, a missing guard
   band).
2. `rtl/gpu/bench/tb_ddmi_capture.v -DDIRECT` — `gpu_ddmi.v` alone,
   driven with the 640x480 timing and a pattern that puts every 8-bit
   value on every channel. The capture is decoded back to the pattern,
   and compared **character for character** with the model's own
   encoder: 412,000 characters, none different.
3. `rtl/gpu/bench/tb_ddmi_capture.v` — the whole of `gpu_video.v`, with
   VRAM returning a pattern made from the address. Checked as DVI and
   hashed against `rtl/gpu/bench/ddmi_dvi_golden.sha256`, which was
   recorded in round 1 before any DDMI audio change. **That hash is
   the promise that "DDMI audio off" is the old output, bit for bit**,
   and it is checked twice: built without `GPU_DDMI_AUDIO`, and built
   with it and switched off.
4. The same with `-DGPU_DDMI_AUDIO +en=1`, both ways (`gpu_ddmi.v`
   alone with the every-byte pattern, and the whole of `gpu_video.v`):
   the checker finds 525 islands a frame, every packet's parity right,
   the AVI, SPD and general-control packets with the right bytes, and
   the pixels untouched — and the model, given the same picture and
   the same schedule (`--schedule round2`), produces **the same
   412,000 characters**, none different.
5. `+en=2`: switched on half way down a frame. That frame must carry
   no islands and the next must carry 525 (`--frame-islands 0,525`) —
   the frame-boundary adoption.
6. `+snd=1`: the bench writes a known sequence of frames (`L(n) = 1237
   n`, `R(n) = 4813 n + 77`) into the FIFO on its own 48 MHz clock, as
   `audio.v` does. The checker must get them back **in order, bit for
   bit** (`--audio-seq`), at the right rate (`--audio-hz`), with N/CTS
   describing exactly that rate, every sample's parity right, the
   channel status decoded (rate code included) and every block 192
   frames long. Run at 48 kHz, at 22.05 kHz sent twice (each frame
   twice in a row, 44.1 kHz on the wire), through the whole of
   `gpu_video.v`, and — sound running, islands off — against the DVI
   golden once more.

`rtl/tb/tb_audio.v` section 12 checks the other end: `audio.v` hands
over each frame 1, 2 or 4 times, with the post-volume samples, and
zeros when muted.

Both captures also rebuild each channel's characters from the
serialiser's two-bits-per-`bclk` pin output (through
`rtl/tb/ecp5_io_cells.v`'s `ODDRX1F`) and check them against the
encoder's parallel words, so the 10:1 shift is checked as well as the
coding.

The checker can be pointed at any capture:

    python3 tools/ddmi/ddmi.py capture.txt [--expect dvi|hdmi]
        [--pattern xor] [--model] [--sha256 golden]

It rebuilds everything from the characters alone — syncs, pixels,
control periods, preambles, guard bands, every packet with its parity,
InfoFrame checksums, audio samples with IEC 60958 parity and channel
status, N and CTS — and knows nothing about the RTL that made them: the
same rule as `rtl/tb/tb_audio_spdif.v`'s receiver.

The model was written from public material: CEA-861 for the InfoFrames,
IEC 60958 for channel status, DVI 1.0 for TMDS, and the open-source
cores it is cross-checked against.

## What it costs

No pins, PLLs or clocks. `gpu_ddmi` on its own (`synth_ecp5 -abc9 -top
gpu_ddmi`):

| | LUT4 | CCU2C | PFUMX | L6MUX21 | FF | LUT RAM |
|---|---|---|---|---|---|---|
| `ISLANDS=0` (plain DVI) | 624 | 24 | 230 | 120 | 92 | — |
| round 2: islands, InfoFrames | 889 | 60 | 301 | 136 | 132 | — |
| round 3: and sound | 1,313 | 78 | 424 | 200 | 317 | 8 `DPR16X4` (the FIFO) |

So DDMI audio as a whole is roughly 900 LUT-equivalents and 225
flip-flops over plain DVI, and no block RAM. Most of the sound half is
the four latched frames (128 FF) and picking each character's bits out
of the sample packet. (A first version chose a whole 248-bit packet and
indexed it afterwards; picking each source's nine bits first and then
choosing saved 330 LUT4 for the same function.) The sound half also
adds about 30 LUT4 in `audio.v` and a few registers in `socctl.v`.

Placed, whole SoC, measured in round 2 (islands only; round 3 is not
placed here — test builds are faster on a desktop):

| target | COMB without | COMB with | `CLK_48` | pixel clock |
|---|---|---|---|---|
| `lakritz_gpio` + `GPU_DDMI_AUDIO` | 22,311 (91%) | 22,759 (93%) | 52.30 MHz (48 needed) | 49.33 MHz (25.2) |
| `sergei_ml1` + `GPU_DDMI_AUDIO`, seed 3 | 21,338 (48%) | 21,349 (48%) | 53.68 MHz | 48.75 MHz |

On Sergei the default seed did not route: nextpnr's router was still
rerouting after 30 minutes, where the build without islands takes three.
Seed 3 routed in six. That is the seed sensitivity [boards.md](boards.md)
describes, and `zrelease build` already retries with new seeds (three
attempts by default); round 4, which turns this on per board, will
pin a seed where one is needed, as ULX3S does.

The pixel clock keeps about twice the margin it needs. With the sound
half Lakritz will be past the 93% above, which is why it gets
`lakritz_ddmi_audio` without the GPIO port and the sigma-delta output.

The 45F and 85F boards have room. Lakritz does not, alongside its GPIO
port and sigma-delta output, so it gets a separate target.

### Lakritz

`release/targets/lakritz_ddmi_audio.spec`:

```
base = lakritz
defines =
    GPU_DDMI_AUDIO
    -AUDIO_SD
    -SPI_ETH
lpf_drop = AUDIO_L AUDIO_R ETH_SS ETH_MOSI ETH_MISO ETH_SCLK ETH_INT
```

| | `lakritz_gpio` (ships) | `lakritz_ddmi_audio` |
|---|---|---|
| Sound | sigma-delta, 3.5 mm jack | HDMI connector only |
| HDMI | plain DVI | DVI, plus islands when `system.video.ddmi_audio: yes` |
| PMOD | GPIO port | none |
| Ethernet | no | no |
| MPU, exact rates, mixer | yes | yes |

- **No sound from the jack.** `-AUDIO_SD` removes the two DACs and their
  pins. The audio block, its FIFO, the mixer and the system volume are
  all still there; the frames just have one way out. `audiotest` lists
  `DDMI` as the only output.
- **Still a picture with the setting off.** Islands are off at power-on
  and the output is the DVI golden bit for bit until the kernel reads
  `system.video.ddmi_audio: yes`, so a monitor that rejects them still
  works with this build. With the setting off it is silent.
- **No PMOD.** With the sigma-delta gone there may be room for the GPIO
  port again; if a build shows there is, `pmods = gpio` in the spec is
  the change (and it is what `lakritz_gpio` does). It is left out until
  a placed build says it fits.
- **The MPU stays**, as on every build.
- `zrelease check` reports it as `-7 vs the board .lpf` (the two audio
  pins and the five Langkatze pins).

The shipping Lakritz image stays `lakritz_gpio` with the sigma-delta
output and without DDMI audio.

Schoko has DDMI and no audio block at all today; it gets `AUDIO` and
`AUDIO_MIXER` with DDMI audio as its only output.

## Phases

| | |
|---|---|
| 0 | bench: model, checker, capture, golden — **done** |
| 1 | exact sample rates, apps moved to Hz — **done** ([audio.md](audio.md#exact-sample-rates)) |
| 2 | data islands, video preamble and guard band, AVI/SPD InfoFrames, general control packet; the socctl enable bit; `system.video.ddmi_audio` — **done** |
| 3 | audio packets, clock regeneration, Audio InfoFrame, the CONFIG bit, zaudio and `audiotest` — **done** |
| 4 | `lakritz_ddmi_audio` — **done**; board enables; Schoko's audio block |
| final | hardware tests, run together |

Hardware checks collected so far (run in the final phase):

- A TV, a monitor with speakers, a DVI-only monitor (setting off and
  on), a capture dongle, and a receiver/soundbar if available.
- With the setting on (round 2, no sound yet): the TV shows 640x480 as
  4:3 without cropping or stretching, blacks black and whites white;
  its info screen names the source "Zeitlos" (SPD) where it shows one;
  `ddmi off` / `ddmi on` at the console switch between DVI and islands
  without a lost or torn frame; game-mode colour and the phosphor
  modes look as before; the boot logo and rescue console are fine with
  it set in the config.
- 48 / 44.1 / 22.05 / 11.025 kHz; `play`, `midi`, `track`, `tts`,
  `chip8`, `kidgames`, `gamedemo`; volume keys; game-mode colour.
- 4:3, full range and no overscan on a TV.
