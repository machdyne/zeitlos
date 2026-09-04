# logic

A logic analyser, pin driver, burst generator and I2C decoder for the
GPIO ports, on the same machine as the pins.

This app is still under development.

```
> run wm
> run logic
```

`sw/apps/logic/logic.c`. Needs a bitstream with GPIO — see
`docs/gpio.md`.

## Why

Bit-banging a bus is much easier when you can see the bus. Before this,
debugging `sw/common/zi2c.c` on real hardware meant a scope, or printf
and guesswork.

Four things, in the order you reach for them:

| | |
|---|---|
| **CHANNELS** | drive a pin high or low, or watch it |
| **CAPTURE** | sample all eight pins into a buffer and draw them |
| **MEASURE** | frequency and duty from the captured trace |
| **DECODE** | read a captured I2C transaction back as bytes |

## The panel

```
 CHANNELS ─────────────── PORT [-] 0 [+]     TIMEBASE ─────── 2048 us / 1024 Sa
┌────────────────────────────────┐  ┌────────────────────────────────────────┐
│ 7  ○  [IN ] [ - ]  p10         │  │ 7 ────────┐    ┌───────┐    ┌─────    │
│ 6  ●  [OUT] [ HI]  p9          │  │ 6 ─────┐  └────┘       └────┘         │
│ ...                            │  │ ...                                   │
│ 0  ○  [OD ] [ LO]  p1          │  │ 0 ──┐ ┌──┐ ┌──┐ ┌──┐ ┌──┐ ┌──         │
└────────────────────────────────┘  └────────────────────────────────────────┘
┌──────────────────────────────────────────────────────────────────────────────┐
│ RATE [-] 100k [+]   DEPTH [-] 1024 [+]   TRIG [0] [RISE]        [   RUN   ] │
│ GEN [0] [10kHz] [BURST]      I2C [SCL0] [SDA1] [DECODE]                     │
│ ┌──────────────────────────────────────────────────────────────────────────┐ │
│ │ 1024 Sa @ 98 kSa/s  ch0: 84 edges, 4021 Hz, 49% hi                      │ │
│ │ I2C: S 3cw+ 01+ P                                                       │ │
│ └──────────────────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────────────────┘
```

Raised frames for control groups, recessed wells for anything that
displays a value, lamps that are rings when dark and filled when lit.
On a 1bpp display "recessed" is a box with a doubled top-left edge and
"raised" one with a doubled bottom-right — cheap, and it reads
correctly at this size.

**The waveform rows line up with the channel strip rows.** `WV_ROW_H`
and `CH_ROW_H` are equal on purpose, so the eye goes straight across
from a lamp to its trace. Change one and change the other; the layout
test asserts it.

## Channels

Each row: the pin number, a live lamp, a mode button, a level toggle,
and the PMOD pin it lands on.

The **PMOD pin reminder** (`p1`, `p2`, `p3`, `p4`, `p7`…) is there
because bit order is not pin order on a connector — bits 4–7 are the
bottom row, so bit 4 is diagonally below bit 0. That catches people out
every time (`docs/gpio.md`), and the panel is where the answer belongs.

The mode button cycles **IN → OUT → OD**. In OD, "HI" means *released*
rather than driven high — which is what open drain means, and why the
label reads HI/LO rather than DRIVE/FLOAT.

The level control is a **toggle**, not a button: it has a state the
panel should show, and a button reading HI while the pin is low would
be a switch that lies about its position.

**Every pin starts as an input**, whatever the last app left them as.
An instrument that starts by driving pins is the wrong kind of
instrument. Switching ports also resets the strip to all-inputs rather
than silently driving the new port's pins the way the old one was set.

The lamps poll at 15Hz. That is fast enough to look live and slow
enough to be a rounding error in the scheduler — and it is what makes
the panel feel like hardware rather than a form. The lamps are for "is
that pin high"; catching a pulse is what RUN is for.

## Capture

Sampling is a CPU loop reading one wishbone register, which puts the
ceiling somewhere around 1–2 MSa/s. The floor of the loop overhead is
not something a faster timebase setting can get under.

**So the rate you ask for is a request, and the readout always shows
what was actually achieved**, measured with `rdcycle` across the
capture. A bench instrument that lies about its timebase is worse than
one with a slow timebase — and every measurement derived from the rate
would be wrong by the same factor.

`MAX` is a real setting, not a placeholder: it is the only one that
shows what the hardware can actually do.

### Interrupts are masked, and that is what bounds the capture

A sample loop that gets descheduled leaves a gap in the trace with no
marker where it happened — and you cannot tell a gap from a slow
signal, which makes the trace worse than useless. So captures run under
`maskirq()`.

**Which means the whole machine stops for the duration.** No timer, no
keyboard, no display. Fine for a few milliseconds, unacceptable for a
few hundred. So the capture is capped at `CAPTURE_MAX_MS` (20ms) and the
app **reduces the depth to fit** rather than silently taking the machine
away. The readout says `(depth capped)` when it did.

That cap is the real constraint on this instrument, and it is the same
trade a real analyser makes between memory depth and timebase:

| rate | samples in 20ms |
|---|---|
| MAX | 2048 (loop-bound, not time-bound) |
| 100 kSa/s | 2000 |
| 10 kSa/s | 200 |
| 1 kSa/s | 20 |

The trigger wait has its own separate 20ms budget: a trigger that never
arrives must not hold the machine longer than a capture would, and "no
trigger" is a normal outcome on an idle bus. The readout says so
instead of leaving you looking at a stale trace.

### Drawing

One column per pixel, and each column is the **OR of every sample that
falls in it** — so a pulse narrower than a pixel still shows as a
transition rather than disappearing depending on where it landed. An
instrument that hides narrow pulses at a wide timebase is exactly the
wrong tool for finding a glitch.

The graticule is a dotted division every eighth of the screen, so you
can count time without measuring pixels. The heading shows microseconds
across the whole screen, which is the number that tells you whether you
are looking at the thing you meant to.

## Measure

Frequency comes from the **edge count over the whole window**, not from
one period. A single-period measurement on a noisy or aperiodic signal
is a number that looks precise and isn't; an average over the window is
what it says it is.

Duty is high samples over total.

## Generate

`BURST` emits a bounded number of square-wave cycles on the selected
channel, then stops.

A burst rather than a continuous output, deliberately: a continuous
generator would need this process to hold the CPU forever, or to run
from an interrupt this app does not have. A burst is bounded, honest,
and enough to trigger something else or check a receiver.

**It refuses if the channel is not set to OUT**, rather than switching
it. Quietly reconfiguring a pin the user set to IN is how you drive
into something that was driving back.

The pin is left where it started, not where the burst ended.

## Decode

Pick SCL and SDA channels, capture, press `DECODE`. Output looks like:

```
I2C: S 3cw+ 01+ 5a- P
```

`S` start, `P` stop, then bytes. The first byte shows the 7-bit address
with `w`/`r`; every byte carries `+` for ACK or `-` for NACK.

**This decodes what was on the wire**, which is the point of having it
here rather than printf in `zi2c.c`. It will happily show you a NACK
where the library reported success, or an address one bit off from the
one you passed in.

Deliberately not a full decoder: no repeated-START distinction (both
show as `S`), no 10-bit addressing. It has to fit on one line of a
panel, and the first question is always "did the right address go out
and did anything answer".

## Keyboard

Tab and Shift+Tab move between controls; Enter and Space activate.
`R` runs a capture and `D` decodes — on an instrument the big button
has a shortcut, and RUN is the one you press over and over.

Keyboard-only operation matters here more than usual: this app is most
useful exactly when something is wrong, and requiring a pointer to
reach a button is a dead end for anyone whose pointer is the thing that
is wrong.

## Rendering the panel

```
cc -std=gnu99 -Wall -I sw/common -o /tmp/render \
   sw/apps/logic/tests/render.c \
   sw/common/zwin.c sw/common/zwidget.c sw/common/zfont_data.c \
   sw/common/zobj.c sw/common/zeitlos.c
/tmp/render /tmp/logic.pbm
```

Draws the real panel on the build machine. **Do this before changing
the layout** — this panel shipped wrong three times against a passing
arithmetic test, and the third bug (frames drawn in absolute screen
coordinates rather than content coordinates, which was the cause of
the first two) was found in one look at a render.

`docs/window_manager.md`, "Rendering a panel on the build machine",
explains what it can and cannot catch -- and the section above it
documents the coordinate-space trap that caused all three of this
panel's layout bugs.

## Testing the panel

```
cc -std=gnu99 -Wall -o /tmp/t sw/apps/logic/tests/test_layout.c && /tmp/t
```

Compiles the real `logic.c` and calls the real `layout()`, then asserts
what a screen would otherwise have to tell you: nothing outside the
window, no two widgets overlapping, nothing on the readout or the
waveform, the readout tall enough for two lines, strip and traces
row-aligned.

Hand-placed controls are the fragile part of an instrument panel. The
deck layout is cursor-driven — every label, well and button in a group
takes its position from one walk — precisely so a wrong number moves a
whole group instead of putting a button on top of a display. This is
what confirms it still does.

`logic.c` routes its two `maskirq()` calls through `LOGIC_MASKIRQ` so
that harness can compile it on a build machine; that instruction's
inline asm will not assemble on a host, and a static inline that is
never called is never emitted. Nothing else about the app changes.

## Known limits

**It is a slow instrument.** ~1–2 MSa/s ceiling, 20ms window. Good for
100kHz I2C and slow SPI; useless above a few hundred kHz. If that
proves too limiting, the fix is a hardware sampler in `rtl/gpio.v`
writing to a BRAM FIFO — not a tighter loop here. That is a phase of
its own, and worth finding out is needed rather than building
speculatively.

**One port at a time.** Sampling two would mean two register reads per
sample and half the rate, for a case nothing has needed yet.

**No storage.** A capture lives until the next one. There is no save,
no reference trace, no cursors.

**The machine visibly hitches** on every capture. That is the masked
window, and it is the correct trade for a trace you can trust.

## See also

- `docs/gpio.md` — the pins, the pull-ups, the open-drain idiom
- `docs/i2c.md` — the library this exists to debug
- `docs/window_manager.md` — the window and widget conventions
