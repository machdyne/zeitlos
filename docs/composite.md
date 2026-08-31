# Composite video

Monochrome CVBS output — NTSC 240p at 60Hz or PAL 288p at 50Hz — from a
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

## Not done

**Colour.** Needs a colour subcarrier (3.579545 MHz NTSC, 4.43361875 MHz
PAL) phase-locked to the line rate, which 25.2 MHz does not divide into
cleanly — it would need its own PLL output and a phase accumulator.
Pointless on a 1bpp framebuffer anyway; it becomes interesting only
alongside the 2bpp palette mode discussed in `docs/game_mode.md`.

**Interlace.** 480i/576i would double vertical resolution to 480/576
lines, at the cost of flicker on any horizontal edge — which on a 1bpp
display of mostly text and thin lines is every edge. 240p is the right
answer for this machine.
