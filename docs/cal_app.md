# cal app

A month calendar. `sw/apps/cal/cal.c`.

```
> run wm
> run net      # optional -- without it there is no "today"
> run cal
```

Or click its icon in the dock.

## What it shows

One month as a 7×6 grid, with the weekday initials above it, the month
and year between two arrows, and a status line underneath.

```
+-- cal ---------------------[x]-+
| [<]   September 2026     [>]   |
|  Su Mo Tu We Th Fr Sa          |
|         1  2  3  4  5          |
|   6  7  8  9 10 11 12          |
|  13 14 15 16 17 18 19          |
|  20 21 22 23 24 25 26          |
|  27 28 29 30                   |
|                                |
|   Today Sun 6 Sep 2026 UTC     |
+--------------------------------+
```

Today is drawn inverted — a filled cell with the number knocked out of
it. Nothing else is markable and nothing is selectable: see
[What this deliberately isn't](#what-this-deliberately-isnt).

## Controls

| | |
|---|---|
| `<` `>` | previous / next month |
| Left / Right | previous / next month |
| Up / Down | previous / next year |
| Home, or `t` | back to today |
| `0`-`9` | start typing a year |
| Enter | go to the typed year, or press the focused arrow |
| Esc | abandon a half-typed year |
| Backspace | delete a digit |
| Tab / Shift+Tab | move between the arrows |

Fully usable with no pointer, which is the standing requirement
(`docs/window_manager.md`). Arrows are bound directly because there is
nothing else on this window for them to belong to — no list, no text
field — which is the condition `docs/widgets.md` sets for binding them
at all.

## Where the date comes from

Not from here. The app only ever **reads** the RTC; the clock is set by
net's SNTP client (`docs/rtc.md`, `sw/apps/net/ntp.c`). Same split as
`clock`, for the same reason — an app that also did the networking
would mean the machine only knew the date while a window was open.

## Three states

| state | shown as |
|---|---|
| no RTC in this bitstream | the calendar, no highlight, and which command fixes it |
| RTC present, never set | January 1970, no highlight, "type a year" |
| RTC set | the current month, today marked |

### Why it opens on the epoch

The middle row is the interesting one, and it is a different problem
from the clock's.

`clock` shows `--:--:--` when it does not know the time, because a
clock with no time has nothing else to show. A calendar does: every
month from 1970 to 2105 is exactly as correct without a network as
with one, so blanking the grid would throw away the part that still
works. Only the *highlight* depends on the RTC.

So the app shows a real month. Which one is the question, and the
answer is the one the hardware actually implies: an unset counter
reads zero, zero is 1970-01-01, and that is what gets drawn. It is not
a guess dressed up as a date — **1970 is visibly not now**, so the app
cannot be mistaken for one that knows something it doesn't.

On its own that would be useless, since 1970 is 672 presses of the
right arrow from anywhere anybody cares about. Hence year entry.

### Typing a year

Type four digits and press Enter. The heading is replaced by the
buffer as you type (`Year 202_`), with underscores for the digits still
wanted, and the status line says what Enter and Esc do.

The buffer replaces the heading rather than appearing beside it
because it is about to *become* the heading — so the result lands
where the entry was, and there is no extra field to notice, lay out or
dismiss.

Four digits are required. A short year is not padded or guessed at:
`26` could reasonably mean 2026 or 1926, and picking one silently is
worse than asking for the other two digits. An out-of-range year is
refused at **commit**, not while typing, because you have to be able
to type 1969's first digit to reach 1970.

The month is kept across a year jump, so typing a year lands you on
the same month of it.

On a machine with no network this is not a shortcut — it is the
primary navigation, which is why it is a tested state machine
(`cal_entry_*` in `cal_core.c`) rather than a few lines in the key
handler.

### Following the clock

`cal` is usually launched before net's first NTP sync lands, so it
opens on the epoch and the RTC becomes valid a few seconds later. When
that happens the app jumps to the real month — **unless the user has
already navigated somewhere**, in which case yanking the view out from
under them is not a feature. `user_navigated` is that one bit.

## UTC

"Today" is today in UTC, matching the RTC and everything else in the
system (`docs/rtc.md`). West of Greenwich the highlight therefore moves
several hours before local midnight.

The status line says `UTC` for the same reason the clock's does: an
unlabelled date that disagrees with the wall reads as broken rather
than as correct-but-elsewhere.

It spells the date out in full rather than just carrying the label,
because the highlight only answers "what is today" while today's month
is the one on screen. Page away and an unlabelled app would stop
telling you the date at all — which is a thing people open a calendar
to find out.

## What this deliberately isn't

No notes, no events, no reminders, no settings.

That is not a staging decision. A calendar that stores things needs a
file format, a place on the card to put it, and an answer for what
happens to that data when the clock is wrong — and none of it belongs
in the thing that draws a month. This app answers "what day is the
14th" and stops there.

## The arithmetic

`cal_core.c` has **no leap-year rule and no table of month lengths.**
Both already exist, correct and tested, in `sw/common/zrtc.c`, and a
second copy is a second thing to be wrong in February 2100.

Everything is derived from the two public functions there:

```
first weekday  = z_tm_to_time(1st of the month) -> z_time_to_tm()
length         = (1st of next month - 1st of this month) / 86400
```

The second one looks like a trick and is the reason there is no
calendar logic in this app at all: the difference between two
consecutive firsts *is* the month's length, by definition, for every
month including February in a century year. `zrtc.c` already knows the
Gregorian rules; this asks it rather than repeating it.

### The year range stops at 2105

Unix time here is `uint32` seconds, which runs out on 2106-02-07. That
is not a round number of months, so the last month `cal` will show is
**December 2105** — one whose *following* first (2106-01-01) is still
representable, which the length calculation above needs.

Stopping a year early is deliberate. The alternative is a final year
that works for January and silently produces a 28-day March for
February, which is exactly the kind of edge that ships.
`tests/cal_test.c` asserts that 2106-03-01 does *not* round trip, so
the constant cannot drift away from its reason.

## Week start

Sunday, matching `z_wday_name()`'s numbering (0 = Sunday) and the rest
of the system.

It is one constant — `CAL_WEEK_START` in `cal_core.h` — and everything
derives the column from it rather than assuming: `cal_wday_col()`,
`cal_col_wday()`, the lead-cell count and the weekday header row all
rotate together. A Monday-first calendar is that single line.

The header row in particular is built from `cal_col_wday()` rather
than a written-out list of initials, so it cannot quietly disagree
with the grid beneath it.

## Six rows, always

A 31-day month whose 1st falls in the last column spans six weeks
(6 lead cells + 31 days = 37 > 35). August 2026 is one.

The grid always allocates six, so the window does not change height as
you page through the year; short months leave the last row blank.
`tests/cal_test.c` confirms both that six-row months really occur in
range (350 of them) and that nothing ever needs seven.

## Fonts

`z_font_6x12` for the grid, the weekday initials and the heading;
`z_font_5x8` for the status line, which is secondary text and should
not compete with the numbers.

The app does **not** call `z_gfx_hw_font_load()`. wm is the only
process on the board that ever writes glyph memory and it loads both
fonts at startup, which is what makes using two of them possible
without breaking that single-owner rule
(`docs/window_manager.md`, "Hardware glyph blitting").

The today cell uses `z_win_draw_text2()`, not `z_win_draw_text()`. The
latter hardcodes its glyph background to 0, so ink 0 on a lit cell
would *erase* the cell rather than write on it — the failure documented
in `zwin.h` and in `docs/widgets.md`.

## Dead arrows at the ends

At January 1970 the `<` arrow is disabled, and at December 2105 the
`>` arrow is. A disabled widget draws as an empty frame
(`docs/widgets.md`), so it reads as unavailable rather than sitting
there looking live and ignoring you.

This is not an exotic state: the app opens on the epoch on any machine
without a clock, so the dead `<` is what a first-time user sees.

The range check is `cal_step_month()` on a scratch copy — it refuses
without moving anything — so there is no second notion of "where the
ends are" to keep in step with `cal_core`.

Focus is moved off an arrow that goes dead, since
`z_widget_key_activate()` returns -1 for a disabled widget and Enter
would otherwise appear to do nothing at all.

## Update rate

The main loop wakes once a second (`z_proc_wait(Z_TICK_HZ)`).

That looks generous for something that changes once a day, but the
thing being waited for is not midnight — it is net's first NTP sync,
which lands at an unpredictable moment a few seconds after boot and is
what turns the epoch into today. A minute of staring at 1970 after the
network came up would read as the app being broken.

The cost is one register read and a comparison per second.
`check_clock()` compares the UTC *day index* (`seconds / 86400`) and
returns immediately when it is unchanged, which is every time but one
per day. A rollover repaints the grid and the status line only, never
the whole window.

## Window

Fixed at 156×153 — a 152×138 content area.

The width is built up from the grid rather than chosen: seven columns
of `CELL_W`, a margin either side, plus the 4px `z_win_content_rect()`
insets. `layout()` still re-derives everything from the *real* content
size, so changing a constant moves everything that depends on it.

Not resizable: a month is a fixed amount of information, so there is
nothing a bigger window would reveal. `CLOSE_KILLS_OWNER` is correct —
one window for the app's whole lifetime, nothing to save, nothing to
ask about on the way out.

## Tests

```
cd sw/apps/cal
make test                   unattended
make render                 look at the panel
make render WHAT=sixrow     the six-row worst case
make render WHAT=epoch      no clock: 1970, no mark, dead `<`
make render WHAT=nortc      no RTC in the bitstream at all
make render WHAT=entry      a year being typed
```

Everything runs on the build machine with no RISC-V toolchain.
`zrtc.c` compiles on a host unmodified — its MMIO accessors are static
inlines nothing in the tests calls — so the calendar arithmetic under
test is the *shipped* calendar arithmetic, not a copy.

`tests/cal_test.c` (155 checks) walks every cell of every month from
1970 to 2105 and checks three properties together: the cells holding a
day are exactly `1..days` in ascending order, every day's cell
round-trips through `cal_cell_of_day()`, and the weekday implied by a
cell's column matches what that date actually was. Whole-month walks
rather than spot checks, because a spot check only finds the case
somebody thought of — which is exactly how an off-by-one in the
week-start rotation survives.

`tests/test_layout.c` (46 checks) covers the geometry: everything
inside the content rect, nothing overlapping anything, the 2px between
the arrows that their focus rings need, and the widest heading and
status line both fitting. It earned its place immediately — it caught
a 155px status string in a 152px window, in the one state that had not
been rendered.

The status strings are named constants in `cal.c` and the test
measures **those symbols**, not copies of them. Inline literals would
mean the test measuring its own copy, which passes forever while the
app overruns.

`tests/render.c` is the other half, and neither replaces the other:
an assertion only covers a relationship somebody thought to write
down, while a render covers all of them at once but only when a human
looks. See `sw/common/tests/zrender.h`.
