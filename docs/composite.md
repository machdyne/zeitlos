# Composite video

CVBS output -- monochrome, or NTSC/PAL colour (see [Colour](#colour)) — NTSC 240p at 60Hz or PAL 288p at 50Hz — from a
resistor ladder on four pins and an RCA socket.

## The one number that shapes everything

Composite is **320 pixels wide, always**, and that is a bandwidth fact
rather than a design choice.

Drawing 640 distinct pixels across a 52µs active line needs 12.6 MHz of
luma bandwidth. The channel carries about 4.2 MHz (NTSC) or 5.5 MHz
(PAL). A "640 wide" composite picture is a blur of the correct average
brightness, not a picture.

So `gpu_video.v`'s `FIXED_VIEWPORT` parameter makes the 320×240 viewport
**unconditional** on a composite build — `socctl`'s game bit is not
consulted at all. The desktop is still a 640×480 surface; only a quarter
of it is on screen at a time, permanently, and `Ctrl+Alt+arrow` is how
the rest is reached.

That means game mode stops being a nice extra on these boards and
becomes the thing that makes the machine usable at all. It is worth
stating plainly because it is a consequence, not an intention.

## Timing

Both standards come out of the **existing 25.2 MHz pixel clock**. No new
PLL output, which is most of why this is cheap.

| | NTSC 240p | PAL 288p |
|---|---|---|
| clocks/line | 1602 | 1613 |
| line period | 63.5714 µs | 64.0079 µs |
| error vs spec | +0.025% | +0.012% |
| lines/frame | 262 | 312 |
| frame rate | 60.04 Hz | 50.07 Hz |
| front porch | 65 | 57 |
| sync pulse | 118 (4.68 µs) | 118 (4.68 µs) |
| back porch | 139 | 158 |
| active | 1280 | 1280 |

Active video is **1280 clocks = 320 source pixels at 4 clocks each**.
The horizontal numbers in `sysctl.v` are in pixel clocks, not source
pixels, which is what lets one timing generator serve both this and VGA:
the divisor lives in `H_DIV_BASE` and the counters never need to know
about it.

The slack between the nominal porches and the exact line length is split
between front and back porch rather than dumped on one, so the
1280-clock image sits centred in the active window instead of hard
against its left edge.

## Levels and the DAC

A 1Vpp composite signal into 75Ω has three levels that matter for a
monochrome picture:

| | voltage | 4-bit code |
|---|---|---|
| sync tip | 0.000 V | 0 |
| blanking / black | 0.300 V | 5 |
| white | 1.000 V | 15 |

Four bits for three levels, because an R-2R ladder is four resistors
either way and the spare resolution is there if a grey mode ever
arrives. Only three codes are ever driven; the testbench asserts exactly
that.

0.3 V is 4.5 steps on a 0..1 V ladder. `DAC_BLANK` is **5** (0.333 V)
rather than 4 (0.267 V) because erring high keeps sync amplitude at
0.667 V rather than 0.733 V — still well inside the ±6% every receiver
allows, and the direction that loses picture contrast rather than sync
lock. A display that cannot lock shows nothing at all; one with 4% less
contrast looks fine.

Black and blanking are the same value, i.e. **0 IRE setup**. Exactly
right for PAL and NTSC-J, and 7.5 IRE low for original NTSC-M — which
shows up as blacks slightly darker than the receiver expects, on a 1bpp
display whose black is the absence of a pixel anyway. Not worth a fourth
level and a per-standard difference.

### Wiring

```
  COMP_DAC[3] ---[  R  ]---+
                           |
  COMP_DAC[2] ---[ 2R  ]---+
                           |
  COMP_DAC[1] ---[ 4R  ]---+---[ 75R ]--- RCA centre
                           |
  COMP_DAC[0] ---[ 8R  ]---+
                           |
                          75R to GND (or rely on the display's
                           |          own 75R termination)
                          GND
```

With R = 500Ω the ladder plus the 75Ω series resistor lands close enough
to 1Vpp into a terminated 75Ω line. Exact values are not critical — a
receiver cares about the *ratio* of sync to picture, which the ladder
sets, far more than absolute amplitude.

### Lakritz

Lakritz is wired for this: `COMP_DAC[3:0]` on **P1, R1, P2, N4**, in
`boards/lakritz_v0.lpf` — the same four pins that file has carried as
`VIDEO_D0..D3` since before any of this existed, renamed to match the
bus port in `sysctl.v`.

They are **commented out, and composite is off by default**, which is
deliberate. `GPU_COMPOSITE` is off in `boards.vh`, and on a build
without it `sysctl.v` does not declare `COMP_DAC` at all — so a live
`LOCATE` would reference a port the top module does not have on every
ordinary Lakritz build.

To turn it on, three edits that belong together:

1. uncomment `GPU_COMPOSITE` (and optionally `GPU_COMPOSITE_PAL`) in
   `rtl/boards.vh`
2. uncomment the four `COMP_DAC` lines in `boards/lakritz_v0.lpf`
3. comment out `GPU_DDMI` in the Lakritz board block

Step 3 is not optional — composite and DDMI cannot coexist. `sysctl.v`
suppresses the DDMI port declarations on a composite build, so
forgetting it fails loudly at place-and-route rather than producing a
board that drives HDMI pins with 15.7 kHz sync.

These are off per **bitstream**, not per board: Lakritz has the pins
either way, and which of its two video outputs is built is a choice
made at build time.

Any other board wanting composite must add `COMP_DAC[3:0]` to its own
`.lpf`/`.ccf`.

## Sync

Ordinary lines carry the horizontal pulse. During vertical sync the
pulse is **inverted into a broad pulse**: sync sits low for the whole
line except a short serration at the end.

This is the simple version — no equalizing pulses before and after the
vertical block, and no half-line offsets. Both exist in a broadcast
signal to keep an interlaced receiver's vertical oscillator phased
correctly across the half-line difference between fields. This is
progressive 240p/288p: every field is identical, there is no half-line,
and there is nothing for them to correct. Every consumer TV, capture
card and upscaler locks to this; it is what game consoles emitted for
twenty years.

The serration matters and is not decoration. Without it the receiver's
horizontal oscillator free-runs for three lines and the top of the
picture tears. The testbench checks for it specifically, because no
static reading of the code reveals its absence.

## Mutually exclusive with VGA and DDMI

Enforced in `rtl/sysctl.v`, not left to a board author to remember.

Not because the pixel pipeline could not feed all three — it could, they
share `hline` and the refill — but because the **timing** is different. A
15.7 kHz line rate and a 31.5 kHz line rate cannot come out of one set of
counters, and running two sets means two scanline buffers and an arbiter
on `vram.v`'s single graphics port. That is a real feature; it is not
this one.

On a composite build the VGA and DDMI pin declarations are **suppressed
entirely**, not merely left unconnected. An unconnected output port is
not harmless: it synthesises to a pin held at a constant, so a board with
a real VGA connector would show a monitor a dead signal rather than no
signal — which looks like broken hardware rather than an output that was
never built. Removing the port makes place-and-route fail loudly instead,
pointing at the constraint that no longer has anything to bind to.

## Software

| | |
|---|---|
| `Z_FEATURE_COMPOSITE` | bit 28 — this board outputs composite |
| `Z_FEATURE_COMPOSITE_PAL` | bit 29 — PAL rather than NTSC |
| `z_video_is_composite()` | the question to actually ask |
| `z_video_frame_hz()` | 50 or 60 |

Check `z_video_is_composite()` rather than assuming 640×480 is visible,
and do not assume turning game mode *off* gives the whole screen back —
on these boards it will not.

`z_game_wait_frame()` paces itself off the hardware and needs no help
either way; `z_video_frame_hz()` is for anything converting frames to
seconds.

## Testing

`rtl/gpu/bench/tb_composite.v` **measures** rather than inspects. A
composite signal is wrong in exactly one way that matters — a receiver
will not lock — and whether it locks is a question about microseconds
and voltages, not about which branch of a mux fired. So the testbench
times the real waveform with `$realtime` and compares against the
standards.

```
NTSC 240p: line 63.5706 us (want 63.5555), sync 4.682 us (want 4.700)
NTSC 240p: field 16655.488 us = 60.040 Hz over 262 lines (want 262)
NTSC 240p: 3/3 broad lines, 3 with serration
NTSC 240p: 3 distinct DAC levels used
NTSC 240p: source pixel held for 4 pixel clocks
RESULT: PASS

PAL 288p: line 64.0071 us (want 64.0000), sync 4.682 us (want 4.700)
PAL 288p: field 19970.205 us = 50.075 Hz over 312 lines (want 312)
PAL 288p: 3/3 broad lines, 3 with serration
PAL 288p: 3 distinct DAC levels used
PAL 288p: source pixel held for 4 pixel clocks
RESULT: PASS
```

```
sed 's/^\tinput \[31:0\] gb_dat_i,$/\tinput [31:0] gb_dat_i/' \
    rtl/gpu/gpu_video.v > /tmp/gpu_video_fix.v

iverilog -g2005 -DGPU_COMPOSITE -o /tmp/tb_comp.out \
    rtl/gpu/bench/tb_composite.v /tmp/gpu_video_fix.v
vvp /tmp/tb_comp.out

iverilog -g2005 -DGPU_COMPOSITE -DTB_PAL -o /tmp/tb_pal.out \
    rtl/gpu/bench/tb_composite.v /tmp/gpu_video_fix.v
vvp /tmp/tb_pal.out
```

Each takes a couple of minutes — it simulates whole fields, because that
is the only way to measure a field rate.

## Colour

On a composite build with `COLOR` (Lakritz has it), game-mode colour
([color.md](color.md)) goes out as a real NTSC or PAL colour signal:
the same palette, the same programs, on a TV.

### How

A colour subcarrier is synthesised **in the 25.2 MHz pixel clock**: a
32-bit phase accumulator advanced every clock, its top five bits a
phase in 32 steps. Each DAC sample is then

```
blank + Y + U sin(phase) + V cos(phase)
```

for the pixel's palette entry, and the back porch carries a colour
burst for the receiver to lock its own oscillator to. About seven
samples per subcarrier cycle; the TV's chroma filter is the
reconstruction filter.

| | NTSC | PAL |
|---|---|---|
| subcarrier | 3 579 545.45 Hz | 4 433 618.75 Hz |
| accumulator step | 610 080 582 | 755 644 743 |
| frequency error | +0.001 Hz | −0.002 Hz |
| burst | 180°, 9 cycles, 5.3 µs after sync | 135°/225° alternating, 10 cycles, 5.6 µs |
| V | as is | inverted on alternate lines (the PAL switch) |

**No new PLL output and no new clock domain.** The earlier plan was
`pll1`'s spare output at 630 MHz / 44 = exactly 4×fsc for NTSC. It
would have worked for NTSC only, and it would have meant driving the
DAC from a second clock whose samples fall 2.27 to a source pixel --
irregular pixel widths, shifting by a quarter sample every line. The
synthesiser does both standards from the clock the picture already
uses, and PAL is a different step and a sign flip.

**The phase can be coarse.** 32 steps is 11.25°, and truncating the
accumulator puts every sample up to one step late -- but the burst is
made from the same accumulator, so any systematic offset is in the
burst too, and the receiver measures hue against the burst. It cancels.
The subcarrier is not locked to the line either, for the same reason:
the receiver locks to the burst on every line. NTSC ends up with 227.56
cycles a line rather than 227.5, which changes only the dot-crawl
pattern.

**Y, U and V per palette entry** are computed when the entry is
written -- shift-adds in the wishbone domain, a few times a frame at
most -- and kept in distributed RAM beside the RGB palette:

```
Y = (51R + 100G + 19B) / 16       0.299R + 0.587G + 0.114B, x 160/15
U = (84B - 8Y) / 16               0.492 (B - Y)
V = (150R - 14Y) / 16             0.877 (R - Y)
```

in sixteenths of a DAC code, R, G, B the palette's 0..15. White is
Y = 159: ten codes above blank, code 15, the same white as monochrome.
The pixel path then has two small multiplies against a 32-entry sine
table, and two pipeline registers which every signal -- sync and burst
included -- goes through, so the whole output is 80 ns late and
nothing moves relative to anything else.

### Cost

`gpu_video` synthesised alone for ECP5 (yosys 0.33), composite timing,
colour off against on:

| | LUT4 | CCU2C | FF | DP16KD | DPR16X4 | MULT18X18D |
|---|---|---|---|---|---|---|
| monochrome | 968 | 136 | 802 | 0 | 0 | 0 |
| colour | 1451 | 258 | 1212 | 1 | 11 | 2 |

That is the bitplanes and palette (the same as on DDMI, about 460
LUT4-equivalents and the line buffer) plus about 270 for composite
colour itself: the phase accumulator, the Y/U/V palette and its
converter, and two small multiplies, which yosys puts in DSP blocks. A
composite build has no DDMI encoder or serialiser, which gives back
more than that, so Lakritz's composite variant should fit where its
DDMI build does -- but measure it (`make timing`) before relying on it.

### What a 4-bit DAC gives

One code is 10 IRE. The burst is ±20 IRE, ±2 codes; a saturated colour
swings about ±6. Codes are clamped to 2..15: never down at sync level,
where a receiver would see a pulse, and never past the top of the
ladder. So the colour is real but coarse -- in the class of the Apple
II and the Atari 8-bits, not broadcast. Bright, saturated colours
(yellow, light cyan) clip and lose some saturation.

### Black and white is kept

Colour goes out **only while a program has colour on**. The desktop,
monochrome game mode and anything that never asks for colour send the
three-level signal they always did, with no burst -- and with no burst
a receiver's colour killer switches its decoder off, so nothing about
them changes.

For a TV, a cable or a region where the colour signal does not work:

| | |
|---|---|
| `system.video.composite: mono` | in `config` ([config.md](config.md)) -- at boot and on reload |
| `(composite-color #f)` | from a REPL, until the next reload |
| `z_cvbs_set_mono(true)` | from C (`zsoc.h`) |
| `GPU_COMPOSITE_MONO` | build-time: start in black and white |
| no `COLOR` in the board block | no colour hardware: exactly the old monochrome output |

Mono with colour on is not plane 0 alone: it is the palette's
**brightness**, in greys (up to ten levels), with no burst. Colour
programs carry on unchanged; they simply show in black and white.

`socctl`'s CVBS register (word 14, `0x7000_0238`) holds the switch --
see [socctl.md](socctl.md).

### Testing

`rtl/gpu/bench/tb_cvbs_color.v` is a small software receiver: it
correlates a line's DAC samples against an ideal subcarrier at the
standard's frequency and reads off the burst's and the picture's phase
and amplitude, as a TV's decoder does. For NTSC and for PAL it checks:

- colour off is the three-level signal with no burst
- eight colours decode to the right luma, hue (to ±15° against the
  burst) and saturation (±30%, where the 4-bit DAC is not clipping)
- the burst phase does not drift over a hundred lines (the subcarrier
  frequency)
- PAL: both phases of the switch decode to the same colour
- mono: no burst and a flat line at the colour's luma
- the bitplanes on composite timing: a plane full of ones decodes to
  the right palette entry
- picture codes never go below 2

```
sed 's/^\tinput \[31:0\] gb_dat_i,$/\tinput [31:0] gb_dat_i/' \
    rtl/gpu/gpu_video.v > /tmp/gpu_video_fix.v
iverilog -g2005 -DGPU_COMPOSITE -o /tmp/tb_cvbs.out \
    rtl/gpu/bench/tb_cvbs_color.v /tmp/gpu_video_fix.v
vvp /tmp/tb_cvbs.out
```

Add `-DGPU_COMPOSITE_PAL` for PAL. `tb_composite.v` (timing, sync,
levels) still passes in both standards: it is built without colour, so
it sees the monochrome output it always did.

It has not been on a real TV yet. A receiver is more forgiving than this
bench about frequency and less forgiving about levels; if a set refuses
the colour, `mono` is the answer, and the measured level and timing
figures above are where to look.

## Not done

**Interlace.** 480i/576i would double vertical resolution to 480/576
lines, at the cost of flicker on any horizontal edge — which on a 1bpp
display of mostly text and thin lines is every edge. 240p is the right
answer for this machine.
