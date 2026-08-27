# clock app

An analog and digital wall clock. `sw/apps/clock/clock.c`.

```
> run wm
> run net      # optional, but the clock has nothing to show without it
> run clock
```

Or click its icon in the dock.

## What it shows

Two buttons along the bottom, built with `sw/common/zwidget`: **Analog**
and **Digital**, a radio group selecting the view.

A radio group rather than two independent toggles means the widget
toolkit owns which view is current, and this file never keeps a second
copy of that fact that could disagree with what is on screen.

Keyboard: Tab / Left / Right move focus, Enter or Space activates.

There is deliberately **no Sync button**. Net keeps the clock in step
by itself — once shortly after the network comes up, then hourly — so a
button for it would be furniture that does nothing observable in the
overwhelmingly common case. It would also have nothing honest to report:
a sync is a round trip to a public server, so the only immediate
feedback available is "asked, no idea yet", which is worse than not
offering the button. `Z_NET_NTP_SYNC` (`sw/common/zntp.h`) still exists
for anything that genuinely needs to prod it.

## UTC

Displayed times are UTC, and the digital view says so on screen.

The label is not decoration. Nothing converts the clock, so anywhere
other than Britain in winter the displayed hour is deliberately not
local — and an unlabelled clock showing the wrong hour reads as broken
rather than as correct-but-elsewhere.

The RTC has a timezone-offset register but nothing sets it and nothing
here reads it. See `docs/rtc.md` on why that is a decision rather than
an oversight.

## Three states, not two

The app only ever **reads** the clock. The RTC is set by net's SNTP
client (`docs/rtc.md`), which is a deliberate split — a clock app that
also did the networking would mean the machine only knew the time while
a window was open.

So there are three things worth displaying, and they are genuinely
different:

| state | shown as |
|---|---|
| no RTC in this bitstream | a sentence saying so, and which command fixes it |
| RTC present, never set | `--:--:--` / `not set`, dial with hands parked at twelve |
| RTC set | the time |

The middle one is the common case for the first few seconds after boot,
and on a machine with no network at all it is permanent. Showing 1970
with total confidence would be worse than showing nothing; a dial with
no hands at all reads as a drawing error, while a stopped one reads as
a stopped clock, which is exactly what it is.

The first checks `z_rtc_available()`, **not** `z_rtc_present()` — see
`docs/rtc.md`, the latter can hang the CPU on an older bitstream.

## Analog view

The dial and hands go through the GPU line rasterizer (`z_win_hw_line()`,
`rtl/gpu/gpu_raster.v`).

The face is a 60-segment polygon standing in for a circle, because the
rasterizer draws lines and nothing else. At these radii the difference
is under a pixel — the segments are ~8px long on an 80px radius, whose
sagitta is about a fifth of a pixel. Reusing the same table the hands
use also means the dial and the hands agree *exactly* about where twelve
o'clock is, which a separately-computed circle would only do to within
rounding.

Positions come from a 60-entry integer sine table, `sin(6i°) × 1024`.
Integer because there is no FPU and no libm in these binaries; 1024
rather than 1000 so the divide back down is a shift. Cosine is the same
table 15 entries along, so there is only one table.

Hand granularity falls out of that table: the second and minute hands
have 60 positions each, and the hour hand is `(h % 12) * 5 + m / 12`,
which advances every 12 minutes. That is the conventional resolution for
a simple dial.

Screen y grows downward, so the vertical term is subtracted rather than
added. That single minus sign is the difference between a clock and its
mirror image, and it is not obvious from the maths.

### Why the hands are erased rather than the window cleared

A full clear-and-redraw once a second is visible as a flash, which on a
clock is the whole screen blinking at you forever. So each update
redraws the three hands in colour 0 at their previous endpoints, then
draws them at the new ones.

**That works only because of a layout constraint that is easy to break
later.** The hands are shorter than the tick marks' inner radius, so
erasing a hand can never rub out part of the dial. If a hand is
lengthened past `HAND_MAX_R`, the erase starts eating the ticks and the
dial slowly disintegrates — a bug that looks like a rasterizer fault
rather than a layout mistake.

All three hands are redrawn together on every update for the same class
of reason: erasing the second hand can cross the minute hand, so the fix
is to put all three back immediately rather than to reason about
overlaps. The hub goes back on top afterwards, since the erases cut
through it.

## Digital view

`HH:MM:SS`, the date, and the word `UTC`, drawn as `z_font_6x12` text
through the hardware glyph blitter.

6x12 rather than the 5x8 most apps use because it is the larger of the
two fonts wm loads into glyph memory, so it is the biggest text
available through the hardware path — and a clock is a thing you read
from across a room.

The app does **not** call `z_gfx_hw_font_load()` itself. wm is the only
process on the board that ever writes glyph memory, and it loads both
5x8 and 6x12 there at startup, which is what makes this possible without
breaking that single-owner rule (`docs/window_manager.md`, "Hardware
glyph blitting").

Short weekday and month names (`Thu 27 Aug 2026`) keep the date line to
15 characters, 90px, which fits the narrow window with room either side.
A numeric date would be shorter still and harder to read at a glance,
which is the wrong trade for a wall clock.

Lines are centred by measuring the string rather than by a hardcoded
offset, so this survives a font change. Only the three text lines are
erased on an update, not the whole area — same reasoning as the hands,
same benefit.

## Two coordinate systems

The one thing in this file most likely to trip up an edit:
`z_win_hw_line()` takes **absolute screen** coordinates, while
`z_win_fill_rect()` and `z_win_draw_text()` take **content-relative**
ones.

So `layout()` computes the dial's centre in both — `cx`/`cy` absolute,
`ccx`/`ccy` relative — once, and names them apart so a mix-up is visible
at the call site rather than being a plausible-looking variable.

It is also why `Z_WM_REDRAW` calls `layout()` before repainting: the
window may have moved, and stale absolute coordinates would draw the
hands where the window used to be.

## Update rate

The main loop wakes ~8 times a second (`z_proc_wait(Z_TICK_HZ / 8)`).

Not for smoothness — the second hand only has 60 positions and the
update does nothing when the second has not changed. It is so a hand
moves within ~125ms of the second actually turning over instead of
drifting up to a full second behind it. A clock whose second hand is
visibly out of step with the digits looks broken even though both are
right.

The cost is a wakeup that does three register reads and returns, eight
times a second.

## Window

Fixed size, 132×162 — a 128×147 content area with a 116px dial.

Small on purpose: a clock is something you leave open in a corner while
doing something else, so the useful size is the smallest one still
readable across a room. That floor is set from below by two things —
`HH:MM:SS` is 8 characters of `z_font_6x12`, so 48px plus margins, and
the dial needs roughly a 100px diameter before the hour and minute hands
stop being tellable apart at these proportions.

Not resizable: rescaling the dial on every drag is a lot of machinery
for a window nobody wants a different size of. `CLOSE_KILLS_OWNER` is
correct here — one window for the app's whole lifetime, nothing to save,
nothing to ask about on the way out.

The layout still derives everything from the real content size rather
than from the constants, so changing them is a one-line edit.
