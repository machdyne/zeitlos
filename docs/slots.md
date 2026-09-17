# slots

**Status: complete.** Three reels, five paylines, an exact 5.359% house
edge, and chips that persist in `/USER/casino.dat`.

Built for rv32i with section GC: 45,964 text, 64 data, 3,536 bss —
49,564 bytes. No soft-float in the app's own objects.

    cd sw/apps/slots && make          # the target binary
    make test                         # 55 checks, plus the shared library's 169
    make art                         # regenerates the symbol tiles
    make render WHAT=idle|spin|win   # draws it, for looking at

Three reels, 32 virtual stops each, a 3×3 window and five paylines —
the three rows and the two diagonals. Symbols: cherry, BAR, BARBAR,
3BAR, bell and the seven.

## The house edge is a number you can count

Three reels of 32 stops is 32³ = **32,768 positions**, and five paylines
over each is **163,840 line-bets**. That is small enough to walk
exhaustively, so the return to player is not a simulation result or a
designer's intention — it is an exact integer.

Staking one coin on every line of every possible position costs 163,840
and returns exactly **155,060**: an RTP of 94.641%, a house edge of
5.359%, which is where a real machine sits.

`tests/reel_test.c` asserts that integer. Any change to a reel strip or
a payout moves it, and that is the point — **this is the one property
of a slot machine that cannot be checked by playing.** At this edge it
would take tens of thousands of spins to tell 94% from 88%, and the
person who noticed would be the one who had lost enough to care.

| symbol | pays | once in | of the return |
| --- | --- | --- | --- |
| three sevens | 800 | 4,096 | 20.6% |
| three bells | 150 | 1,820 | 8.7% |
| three cherries | 100 | 1,365 | 7.7% |
| three 3BAR | 100 | 1,213 | 8.7% |
| three BARBAR | 50 | 512 | 10.3% |
| three BAR | 20 | 151 | 13.9% |
| any three bars | 2 | 17 | 12.2% |
| two cherries | 5 | 91 | 5.8% |
| one cherry | 1 | 8 | 12.0% |

About one line-bet in five pays something. The jackpot is 1 in 4,096
line-bets, or **1 in 819 spins** with all five lines played.

## Cherries pay from the left

One cherry counts only on reel 1, two only on reels 1 and 2.

That is the classic rule and it is not a detail. Paying a cherry
anywhere on the line was the first version here, and it put the return
to player at **133%** — the player owned the casino. Cherries are the
most common paying symbol, so position matters more for them than for
anything else on the reel.

## The strips

The order around a strip does not affect the return — every stop is
equally likely, so only the counts matter. It does affect what a near
miss looks like, so the symbols are scattered with no paying symbol
adjacent to another of its kind; nothing slides past in clumps.

|  | cherry | BAR | BARBAR | 3BAR | bell | 7 | blank |
| --- | --- | --- | --- | --- | --- | --- | --- |
| reel 1 | 4 | 6 | 4 | 3 | 3 | 2 | 10 |
| reel 2 | 3 | 6 | 4 | 3 | 3 | 2 | 11 |
| reel 3 | 2 | 6 | 4 | 3 | 2 | 2 | 13 |

Cherries thin out left to right and so do the bells. The leftmost reel
is generous because cherries pay from the left; the rightmost is stingy
because it is the one still spinning when the other two have already
matched, which is where the tension in a slot machine comes from.

## 32 stops, which is not an accident

An electromechanical machine had 22 physical stops. A virtual-reel
machine maps a larger table onto them, and 32 is the usual size.

It is also exactly the bound that used to hang this tree:
`z_rng_below(32)` never returned, because the accept bound
`2^32 - (2^32 % 32)` is `2^32` and truncates to zero. `sw/apps/poker`
found it on a card shuffle. A slot machine asks for it three times a
spin.

## The reels do not use the hardware scroll, and that was the plan

`z_fb_hw_scroll()` moves a rectangle's pixels and leaves only the
incoming strip to redraw, which looked like the obvious answer to
roulette's flicker. Measuring the alternative said otherwise.

**A cell is exactly the tile height, so the tiles abut.** Five opaque
blits cover a three-row reel completely — the cell scrolled partly off
the top, the three whole ones, and the cell arriving at the bottom —
with every pixel written exactly once and nothing cleared first. That is
already flicker-free. The scroll would save four blits and cost a
partial-tile path plus a fallback for when `z_fb_hw_scroll_allowed()`
says no.

And it would be slower where it matters. Content moving **down** costs
one blit per `dy`-deep strip, so a reel crawling to its stop at two
pixels a frame would take **48** of them against five for a redraw. The
scroll is fastest when a reel is moving fast, which is exactly when
nobody is looking closely.

The general lesson from roulette still holds, and it is the one that
matters: **the flicker was never about which blit to use, it was about
clearing.** Anything that writes every pixel exactly once is flicker-free
whatever primitive it uses.

## Why roulette flashes and this does not

Roulette's spin flashes, and the reason is structural: **a rotating rim
has no linear mapping, so every frame it has to be cleared and
redrawn.** Drawing straight to the visible page means every one of those
clears is seen.

**Reels translate.** That is exactly what `z_fb_hw_scroll()` does — it
*moves* the pixels of a rectangle and leaves the strip that scrolls in
for the caller to redraw. So a spinning reel is one hardware blit plus
one row of symbols per frame, and nothing is ever blanked. No flicker,
by construction rather than by tuning.

Two things to get right when it is written:

- **`z_fb_hw_scroll_allowed()` has to be asked first.** It does nothing
  silently when the window is partially occluded, and a caller that
  assumes it worked scrolls its model while the pixels stay put. Repaint
  the reel in full on false.
- **Content moving down costs one blit per strip rather than one
  overall**, issued bottom to top, because an overlapping copy would
  otherwise overwrite rows it has not read. That is `zgfx.c`'s business,
  not the app's, but it is why a downward-spinning reel is a little
  dearer than an upward one.

## The spin

Each reel's offset is computed **directly from the frame number**, not
by adding a velocity each frame. An integrator drifts over a few hundred
frames and the usual fix is a correction on the last one, which looks
exactly like what it is — a reel snapping into place.

The profile holds full speed for the first 55% of a reel's spin, then
decelerates, then crawls into the stop. Constant speed first is the part
that matters: `sw/apps/roulette` eased from the very first frame and it
came back from the device as "spins very fast then stops suddenly". A
reel that starts decelerating immediately never looks like it is
spinning at all.

**Reels stop left to right**, eighteen frames apart. That is where the
tension lives — two sevens and one reel still turning — and it is also
why the strips thin out to the right: the reel still spinning is the one
least likely to oblige.

The thing that must never break is that a reel lands **exactly** on the
stop the generator chose. A reel finishing one cell out pays the wrong
line, and the exhaustive house edge above would still be exactly right
while the machine paid something else entirely.
`tests/spin_test.c` walks every stop from every starting position a
previous spin could have left.

## The symbols

32×32 tiles, generated by `gen_slots.py` and committed, packed like the
card art — one `uint32_t` per row, least significant bit leftmost, which
is what `z_fb_hw_blit_mem()` wants and the opposite of the font's rows.

Drawn in arithmetic rather than by hand because a slot symbol is mostly
symmetry. Two of them needed a second attempt after looking at a render:

**The cherry had a highlight knocked out of each fruit**, meant to stop
them reading as blobs. At 32 pixels it read as a pair of eyes over a
mouth — a face, unmistakably, once seen. Solid discs with a gap between
them are what says "fruit".

**The reels hung under the status line** left a band of nothing between
the glass and the buttons, taller than the reels themselves — every
spare pixel in one place, which is what bottom-anchoring everything else
does. They are centred in what is left now. `sw/apps/poker` and
`sw/apps/blackjack` both had exactly this.

**The 3BAR tile came out as three blank stripes**, because the wordmark
was only drawn on plaques at least 9 rows tall and three stacked plaques
leave 8 each. Three blank stripes is not a slot symbol, it is a barcode.

## Five lines, and showing them

**All five paylines are played at once** — the three rows and both
diagonals — so a win can land on symbols nowhere near the middle. Left
to itself that makes the payouts look arbitrary: "why did I just win
40?"

Three things answer it, and none of them is a centre marker. **A centre
marker would say the opposite of what is true.**

- **Numbered markers down both edges**, each at the row its line passes
  through on that side. That is what says the diagonals exist at all.
  Rows 1 and 3 each carry two lines, so the second of each pair is
  nudged sideways rather than stacked.
- **A winning line's markers fill solid** with the number knocked out,
  so it reads at a glance without a second colour.
- **The three symbols that paid are boxed.** The markers say which
  lines; the boxes say which symbols. Between them there is nothing
  left to guess.

The message names them too — "lines 2, 4 win 40" rather than "won 40",
which is a number with no story.

## The handle

To the right of the reels, down while they turn and up when they are
not. It is the cheapest possible animation and the one that says the
most: it is the only part of a slot machine that answers "did my pull
register".

**Pulling it spins.** It is the most obvious thing on the machine to
click, and for a while it did nothing, which is worse than not drawing
one. The whole rod and knob are the target, not just the knob — a
handle is a thing you grab at whatever height your hand is.

## Nothing overlaps anything, and there is a test for it

The payline markers sat with their right border on the same column as
the reel frame, and shared a wall with each other. A full repaint drew
the frame through the marker, so the digit next to the reels came and
went depending on what had been drawn last. It looked like a rendering
fault and it was a layout one.

There are two clear columns between the inner marker and the frame now,
and one between each pair. `tests/layout_test.c` asserts the separation
as arithmetic on the rectangles rather than by counting pixels, so a
failure names the two things that touch — and it asserts the symptom
directly as well: every marker still has both ink and background inside
it after a full repaint, idle and just-won. A digit that has been drawn
over is a digit with nothing left.

## Playing

| command | |
| --- | --- |
| Return, `spin`, or the SPIN button | |
| `bet 5` (1–25 coins a line) | |
| `lines 1`–`lines 5` | |
| `max` | the biggest stake you can actually afford, not the biggest there is |
| `bank`, `buyin`, `game`, `quit` | |

The `bet` and `lines` buttons **cycle** rather than opening anything — a
slot machine has no keyboard, and somebody clicking `bet` wants the next
value.

`max` backs off until the stake is affordable. "Max bet" on a machine
you cannot max out should give you the biggest bet you *can* make, which
is what the button means everywhere else.

## The bank

**One adjustment for the whole spin**, applied when the reels land: the
net, not a deduction when they start and a credit when they stop. The
stake never leaves `/USER/casino.dat` until the spin is over, so a crash or a
window closed mid-spin costs nothing. `zbank_adjust()` re-reads before
it writes, so a win in another game meanwhile is not clobbered.

## Installing

**1.** Add `slots` to `APPS` in `sw/apps/Makefile`.
**2.** `sw/data/icons/icon-slots.png` is included. Run
`cd sw/data/icons && python3 gen_dock_icon_data.py`, and add
`{ "slots",      z_icon_slots_data },` to `dock_candidates[]` in
`sw/apps/wm/wm.c`.
**3.** Add `("apps/slots", "sw/apps/slots/slots.bin"),` to
`release/lib/mkfatimg.py`.
**4.** Rebuild `wm` and `slots`.

## A name clash the compiler caught

`sl_art.h`'s include guard is `SL_ART_TILES_H`, not the obvious
`SL_ART_H` — that collides with the tile-height macro two lines below
it. The compiler reports it as a redefinition, and left alone the guard
would be testing a number rather than its own presence.

## Still to come

| | |
| --- | --- |
- **No winning-line animation.** The line pays are computed and kept in
  `sl_view_t.line_pays`; drawing the line across the reels and flashing
  the symbols on it is the obvious next thing, and it is the half of a
  slot machine that says *why* you won.
- **No autoplay, and no "hold" or nudge.** Holds turn this into a
  different game with a different edge, which would mean recomputing the
  exhaustive return.
