# roulette

**Status: complete.** European and American wheels, the full betting
layout by keyboard or mouse, an animated spin, and chips that persist
in `/USER/casino.dat`.

    cd sw/apps/roulette && make                   # the target binary
    make test                                     # 533 checks, six binaries
    make render WHAT=wheel|small|spin|board|game  # draws it, for looking at

Built for rv32i with section GC: 37,810 text, 52 data, 3,284 bss —
41,146 bytes, against `sw/apps/poker`'s 68,844. No soft-float reaches
any of this app's own objects.

Chips come from the shared bank, `/USER/casino.dat` — see
`docs/casino_bank.md`.

## One wheel, two layouts

European has 37 pockets, 0 to 36. American adds a second zero, and that
one extra pocket is the single most important fact about the game: it
nearly doubles the house edge, from 2.70% to 5.26%.

**American is the default.** It is the worse game, and that is not a
small difference -- but it is the wheel people picture, and `wheel euro`
is one command away. The table prints which one is in play, and the
edge is the first thing this document says about it.

## The invariant that checks every payout at once

On a European wheel, every bet pays the same in the long run. Stake one
unit on any bet, and over all 37 pockets the total returned is exactly
36 units:

| bet | pockets | returns | total |
| --- | --- | --- | --- |
| straight up | 1 | 36 | 36 |
| split | 2 | 18 | 36 |
| street | 3 | 12 | 36 |
| corner | 4 | 9 | 36 |
| six line | 6 | 6 | 36 |
| column | 12 | 3 | 36 |
| dozen | 12 | 3 | 36 |
| red / black / odd / even / low / high | 18 | 2 | 36 |

That single number validates the payout ratio **and** the coverage set
of every bet on the table simultaneously. Pay a split 16:1 and it comes
out 34. Put nineteen numbers in the red set and it comes out 38. There
is no way to be wrong about a payout or about which pockets a bet
covers and still land on 36.

It is a property of the game rather than of anybody's code, which is
what makes it usable here — the same reason the poker tests count all
2,598,960 five-card hands rather than checking a list of cases somebody
thought of.

The American wheel keeps 36 for every bet **except** the five-number
basket (0, 00, 1, 2, 3), which pays 6:1 over 5 pockets and returns 35.
That is not a rounding artefact; it is why the basket is the worst bet
on the table, and the test asserts it explicitly rather than excusing
it.

## Three things that are easy to get wrong

**Zero is neither odd nor even, and neither red nor black.** It loses
every even-money bet, which *is* the entire house edge on those bets. A
`% 2` shortcut gives that away without changing anything a casual test
would look at.

**The red numbers are a written-out list, not a formula.** There is a
rule of thumb — odd is red in the first and third dozen, even in the
second — and it is nearly right, which is worse than useless. It fails
at 10, 11, 28 and 29, because the wheel's colouring alternates around
the *rim* while the printed table is laid out numerically. A derived
version puts 10 and 29 in the wrong colour and still looks plausible.

**A split is two numbers adjacent on the printed table, not two
consecutive numbers.** 3 and 4 are consecutive and sit at opposite ends
of different rows; they share no edge and are not a split. That is the
mistake a "consecutive numbers" implementation makes, and it is
invisible until somebody's winning bet does not pay.

Zero splits (0-1, 0-2, 0-3) are left out along with the other
zero-adjacent bets: their geometry differs between the European and
American layouts, and supporting them on one but not the other would be
worse than supporting them on neither. A straight-up zero covers the
same ground at better odds.

## What the tests prove

180 checks. The 36-unit invariant for every bet type and every selector
on both wheels. Coverage counts for each type. The streets, columns and
dozens each partitioning 1 to 36 exactly. Every split covering two and
lying inside the table, with none listed twice and 3-4 absent. Every
corner being a genuine 2×2 block. Zero losing all six even-money bets.
The double zero not existing on a European wheel, and the basket not
being offered there. Quoted odds agreeing with what is actually paid.
Bets stacking rather than duplicating when the same square is clicked
repeatedly, and the table reporting when it is full.

### Mutation testing

Five deliberate breakages, each expected to fail one check: paying a
split 16:1, adding 10 to the red set, counting zero as even, writing
the column test as `n % 3 == sel`, and making splits consecutive
numbers. All five caught — the first and third by the 36-unit
invariant, exactly as designed.

## The wheel

### The pocket order is not 0, 1, 2, 3

A real wheel's pockets run in a fixed, deliberately scrambled sequence
so that neighbouring numbers on the rim are far apart on the betting
layout. The European order begins 0, 32, 15, 19, 4, 21; the American
one is different again and puts consecutive numbers opposite each
other.

Carrying the real sequence is most of what makes the wheel look like a
wheel, and it buys a free check on something else: **on a European
wheel the colours alternate all the way round after the zero.** Red,
black, red, black, thirty-six times.

That is a second, independent test of the red set in `rl_table.c` — one
written-out list validating another. Get a single number's colour wrong
in either and the alternation breaks somewhere.

### The result is decided first

The generator picks the pocket, and the animation is then made to land
on it. Spinning freely and reading off wherever it stops sounds more
honest and is worse: the stopping pocket would be a function of frame
timing, so the odds would silently depend on how fast the board is.

### The speed profile

A spin holds full speed for the first 30% of its length, decelerates
over the next 58%, then crawls into the pocket.

The first version eased from the very first frame, which is what a pure
ease-out does: almost all the travel happens in the opening half-second
and the rest is a long creep. On a plot that is a smooth deceleration;
on a screen it is "spins very fast then stops suddenly", which is how it
came back from the device. Deceleration that starts immediately is not a
spin, it is a flick.

A spin is 120 frames now -- four seconds at 30fps, against the previous
84 -- and nine revolutions, so each phase has room.

### The angle is an ease, not an integration

Each frame computes the ball's angle directly from the frame number:

    angle(f) = start + travel * ease(f / frames)

so it arrives exactly on target at the last frame with no accumulated
error and no final snap. Integrating a decelerating velocity instead
drifts by a pocket or two over a few hundred frames, and the fix for
that is always a correction on the last frame that looks like what it
is.

### The ball is drawn as a streak, not a point

At nine revolutions over 120 frames the peak speed was 43 degrees per
frame -- on a 41-pixel track, **31 pixels between one frame and the
next**, for a ball five pixels across. It never landed next to where it
had just been, so it read as a dot blinking at scattered positions.

The first attempt at fixing that made the ball bigger, which was the
wrong lever and was reported back as a ball that was too big and still
invisible. Size was never the problem; the gap was.

So the ball is drawn as the arc it travelled this frame, tapered head to
tail. That is motion blur, and it is what a camera would show: continuous
at any speed the wheel reaches, shortening back to a dot as it slows.
The spin is six revolutions now rather than nine, which halves the gap
before the streak has to cover it.

**Erasing it is free.** The ball's track -- the annulus between the rim
and the outer wall -- holds nothing else, so clearing it every frame is
black over black everywhere the ball is not. That replaced the
old-rectangle bookkeeping entirely: there is no erase that can disagree
with the draw about where the ball was.

### Keeping the ball on top of the wheel

Every frame the rim repaint covers the ball, and the ball is drawn again
afterwards, so there is a window in which it is not on the screen. While
the ball is out on the track the overlap is a single pixel; during the
**drop** it is inside the band and the rim repaints all ten pixels of
it. That is why it was reported as the wheel being in front of the ball,
and only while slowing.

**Two different calls erase the ball**, depending on where it is: out on
the track it is the track clear, during the drop it is the rim repaint.
Whichever one did it, the ball is off the screen from that moment until
it is drawn again -- and this draws straight to the visible page, so
that window is seen.

That makes the order load-bearing, and getting it wrong trades one phase
for the other. Clearing the track first fixed the drop and broke the
opening, because the ball was erased and then the entire rim ran before
it came back. Putting the rim first did the reverse. The only order
where *neither* eraser has anything expensive after it is:

    rim          erases the ball during the drop
    track clear  erases the streak on the track
    ball         immediately after both

Drawing to the visible page means the window cannot be closed
completely -- there is no back buffer to flip. Three more things make it
small:

- **The rim is only repainted when it has moved a pixel.** The head
  turns about half a pixel per frame at the outer edge, so most frames
  would repaint it into exactly the same pixels -- wasted work that
  covers the ball for nothing. Skipping those is invisible and roughly
  halves both the cost and the flicker. On a skipped frame only the
  dozen pixels the ball was covering are restored.
- **The ball's own pocket is painted last.** Starting the pocket loop
  just past the ball puts the cell that erases it at the very end of the
  rim, so the gap is the tail of the draw rather than the whole of it.
- **The ball lands toward the outer edge of the pocket**, not the
  middle. That is where a ball actually sits, and it cuts how much of it
  the band covers.

**None of this can be checked off the device.** A render always draws the
ball last, so every ordering looks identical in a picture; what is being
fixed is *when* things happen within a frame, and only a screen shows
that. A host test can confirm the ball is present at the end of every
frame -- it is, on all 120 -- which is necessary and nowhere near
sufficient.

### On XOR, which was the obvious alternative

XOR-to-erase restores what was underneath by writing the same pixels
twice. It assumes the background has not changed in between, and this
one rotates -- by the time the ball would be un-drawn, the rim beneath
it has already been repainted, so the XOR would corrupt the fresh rim
rather than restore the old one. It works only where the background is
static, which here is exactly the track, where clearing is already free.

An 8x8 ball sprite would genuinely be cheaper than the span fills the
streak uses. It is not used because an opaque blit paints its corners
too, which shows as a black square the moment the drop takes the ball
onto a lit pocket -- and the streak needs a tapered size per segment,
which one fixed tile cannot give.

### The ball drops into the pocket

**Any ease-out that brings the ball to rest has a tail where it moves
less than a pixel per frame.** A cubic ease over ninety frames spent
its last thirty covering under half a pixel between them: the animation
was technically still running and looked frozen, which is worse than
being over.

Shortening the ease does not fix it — decelerating to zero always ends
in sub-pixel motion, and that is also what a real ball does. What a
real ball *also* does is leave the track and fall into a pocket, and
that movement is radial rather than angular. So the last third of the
spin is the drop: the ball moves inward while it slows, and is visibly
moving right up to the moment it lands.

### The head turns, and the pockets turn with it

A real wheel spins its head one way while the ball orbits the other,
which is most of what makes one look alive. The first version rotated
only the three hub spokes and left the pockets at fixed angles, so it
read as a static ring with a dot going round it -- reported from the
device as "the wheel doesn't spin, only the centre does".

Because the head moves, the pocket the ball lands in depends on where
the head has got to. The head turns at a fixed rate and the spin has a
known length, so where it will *be* is arithmetic: `rl_wheel_spin()`
aims the ball at that future position. Nothing is decided by timing,
which is what keeps the odds independent of how fast the board runs.

### No numbers, and no full-screen wheel

Both were tried and both are gone.

A pocket's arc at the mid-rim radius is `2*pi*0.74*r / n` -- five pixels
at the size the board gives the wheel, thirteen at a radius of 110. So
numbers meant giving the wheel the whole page during a spin, and the
result was a wheel that looked *nearly* right rather than good: upright
digits on a rotating rim, a mode that appeared and vanished, and the
board gone for four seconds.

**The landing is the satisfying part, and it does not need to be on the
wheel.** The winning number is printed large beside it instead, with its
colour and the net spelled out -- which a two-colour rim cannot say and a
five-pixel digit could not either.

The zeros keep a dark slash through their pocket, so the one landmark on
a rim with no numbers is still there.

### Why it is fast, and why it stopped flashing

The wheel is redrawn every frame rather than repaired in two small
rectangles, because every pocket moves: the dirty area *is* the wheel.

**Nothing large is cleared.** The first rotating version cleared a disc,
filled the whole rim white, then cut the red pockets back out of it --
three full repaints of the same annulus per frame, straight to the
visible page, which is exactly what "it flashes a lot" was. Each pocket
is painted in its own colour now, and the pockets tile the band, so
every pixel is written once per frame and there is never a moment when
the ring is blank. The only clear left is the dozen pixels the ball just
vacated.

The old text below describes the repair approach, which still applies to
the ball itself:

The rim is drawn once. Each frame touches only what moved — the ball
and the hub spokes. `rl_wheel_ball_rect()` reports the small rectangle
that changed, and the app redraws the wheel *clipped to it*, restoring
the background exactly because the same drawing code produced it. A few
hundred pixels a frame instead of a few thousand, with no page flip and
no off-screen buffer.

The ball's radius is a parameter to that function rather than read from
the wheel. During the drop the ball moves inward as well as round, so a
rectangle sized to cover the whole radial travel would be a quarter of
the wheel wide and every frame would repaint most of it — which is the
cost the design exists to avoid.

## What the render caught

Two defects that every assertion passed.

**The ball was invisible.** It ran at 88% of the radius, in the middle
of the lit rim — a white ball on a white pocket. Nothing in the tests
noticed, because a ball drawn in exactly the right place is in exactly
the right place whether or not you can see it. The pockets now sit
inside an outer wall with a dark gap between them for the ball to run
in, which is both what a real wheel looks like and the only way a white
ball reads on a two-colour display.

**The pockets were speckled.** Each was filled by walking closely
spaced radial lines, and adjacent lines diverge as the radius grows, so
the outer end of every cell came out ragged. That is why
`z_fb_fill_quad()` exists: a wedge of an annulus is a quadrilateral to
within a third of a pixel at 37 cells, and a scanline fill of one has
no gaps by construction.

## The betting layout

### The grid runs upward, and "column" means two things

    3  6  9 12 15 18 21 24 27 30 33 36     <- row 0, column bet 2:1
    2  5  8 11 14 17 20 23 26 29 32 35     <- row 1
    1  4  7 10 13 16 19 22 25 28 31 34     <- row 2

`number(col, row) = col * 3 + (3 - row)`. A **column bet** is one of
the three horizontal rows above, because it is a column on the table as
the player faces it — so the top row of the grid is the *third* column
bet, and they run opposite ways. The code says `row` for the grid and
`column bet` for the wager and never mixes them; the tests check that
each 2:1 cell covers the number printed beside it.

### Clicking a line, not a square

On a real table a split is bet by placing a chip **on the line** between
two numbers, a corner on the point where four meet, a street on the
outer edge of a row of three. Offering only straight-up bets by click
would leave most of the table unreachable with a mouse.

So `rl_board_hit()` resolves a point to whatever it is nearest: the
middle of a cell is straight up, an edge band is a split, an interior
corner is a corner, and the strip below the grid is a street or a six
line. The band is a third of a cell — generous enough to hit with a
mouse at 320 pixels, tight enough that the middle of a cell is
unambiguous.

**The test for this is the one that matters.** For every point the
layout resolves, the bet's coverage must be exactly the set of numbers
whose cells are within reach of that point — checked over every pixel
of the grid, against `rl_board_cell()`, which is the *drawing* side. So
agreeing means the thing drawn and the thing clicked are the same
thing. A split selector off by one, a corner indexed from the wrong
end, a row confused with a column: all of them show up as a bet
covering numbers somewhere else on the table.

Getting that wrong is not a crash. It is a chip that pays out on the
wrong numbers, which nobody notices until they lose a bet they should
have won.

## What the render caught

Three defects that every assertion passed.

**The ball was invisible.** It ran at 88% of the radius, in the middle
of the lit rim — a white ball on a white pocket. Nothing in the tests
noticed, because a ball drawn in exactly the right place is in exactly
the right place whether or not you can see it. The pockets now sit
inside an outer wall with a dark gap for the ball to run in, which is
both what a real wheel looks like and the only way a white ball reads
on a two-colour display.

**The pockets were speckled.** Each was filled by walking closely
spaced radial lines, and adjacent lines diverge as the radius grows, so
the outer end of every cell came out ragged. That is why
`z_fb_fill_quad()` exists: a wedge of an annulus is a quadrilateral to
within a third of a pixel at 37 cells, and a scanline fill of one has
no gaps by construction.

**The chips spilled two cells sideways.** The value was drawn *beside*
a small disc, to keep the number underneath readable — which is the
wrong instinct. A chip on 17 sat across 20 and 23 and the table became
unreadable. A real chip sits *on* the number and hides it, and nobody
minds: you placed it, so you know what it is on, and the ones you can
still read are the ones you have not bet. Boxed, opaque and sized to
the text is both more legible and more like the thing it represents.

### And one in the harness

`zrender.h`'s shaded fill is `((x + y) & 3) < level`, which saturates
at level 4 — so every level `zgfx.h` defines from 4 to 16 renders
solid. This board leans on the dither much harder than poker did (it is
how a red pocket is told from a black one), so `tests/roulette_shim.h`
supplies a real 4×4 ordered dither over `Z_SHADE_MAX`. Without it the
render would be lying about the one thing it is most needed for.

Red cells are shaded and their numbers sit on a solid strip cut back
out of the dither — the same fix `sw/apps/poker`'s disabled buttons
needed, for the same reason: **there is no grey on this display**, so a
glyph drawn over a dither loses half its pixels to the background.

## Playing

Type a command and press Return, or click. Both routes end at
`rl_bet_place()` with a bet `rl_bet_valid()` has accepted, and
`tests/input_test.c` checks that a bet placed each way leaves the round
identical.

| Command | What it does |
| --- | --- |
| `17`, `17 25` | straight up, for the chip value or the amount given |
| `red`, `black`, `odd`, `even`, `low`, `high` | the even-money bets |
| `dozen 2`, `column 1` | one-based, as printed |
| `street 4`, `six 3` | |
| `split 17 20`, `corner 17` | named by their members |
| `00` | American wheel only |
| `spin` (or Return), `clear` | |
| `chip 1\|5\|25\|100`, or `-` and `+` | |
| `wheel euro\|american` | |
| `bank`, `buyin`, `game`, `quit` | |

Left-click places a chip, right-click takes one back. `F1` steps
through the help, `F2` toggles full screen, `Escape` clears the command
line.

**Splits and corners are typed by their members** because they have no
names — the command looks up which selector covers exactly those
numbers rather than computing it, so it cannot disagree with
`rl_table.c` about what a split is. `split 3 4` is refused with a
message saying those two are not next to each other on the table, which
is the useful half: consecutive numbers are the obvious wrong guess.

**`00` is checked before the bare-number branch.** It is a perfectly
good integer literal, and read as one it puts the chip on the single
zero — silently, on the one bet where the two are genuinely different
pockets. That was a real bug, caught by the input tests.

## The bank

Chips come from `/USER/casino.dat` and go back to it, so a win here is
spendable in every other game. See `docs/casino_bank.md`.

**One adjustment for the whole round**, applied when the wheel stops:
the net, not a deduction at bet time and a credit at payout. The stake
never leaves the bank until the ball has landed, so a crash, a power
cut or a window closed mid-spin costs nothing rather than costing the
stake. And `zbank_adjust()` re-reads before it writes, so another game
that paid out meanwhile is not clobbered.

Bets are limited by the bank *minus what is already on the table*.
Checking against the balance alone would let somebody bet their stack
twice over, because the balance has not moved yet.

A damaged `/USER/casino.dat` is reported rather than replaced. `zbank`
refuses to write over one precisely so a real bankroll is never swapped
for a default, which means the app has to be the thing that says so.

## The spin

**The only thing in this app with a frame rate.** Everything else is
event driven.

Each frame changes two small rectangles: where the ball was and where
it is. Repairing the first by redrawing the wheel *clipped to it* — the
same code that drew the background — and then drawing the second is a
few hundred pixels.

**It does not double buffer, and that is deliberate.** A page flip
would require the whole table redrawn into the back page every frame:
the wheel, the 37 pockets, the grid, every chip. Orders of magnitude
more work for an animation that has nothing to tear. So game mode is
used for the full-screen page and not for the flip — the view stays on
page 0 and the drawing goes there, which is also exactly what happens
in a window, where there is no second page at all.

Paced by the clock at 30 frames a second in a window, or by
`z_game_wait_frame()` full screen. The loop pumps messages every frame,
because an app that stops servicing its queue stalls the window
manager, not just itself.

## Installing

Four edits outside `sw/apps/roulette`, left for you to make rather than
shipped as modified files.

**1.** Add `roulette` to `APPS` in `sw/apps/Makefile`.

**2.** `sw/data/icons/icon-roulette.png` is included. Then
`cd sw/data/icons && python3 gen_dock_icon_data.py`, and add one line to
`dock_candidates[]` in `sw/apps/wm/wm.c`:

    { "roulette",   z_icon_roulette_data },

**3.** Add to the table in `release/lib/mkfatimg.py`:

    ("apps/roulette", "sw/apps/roulette/roulette.bin"),

**4.** Rebuild `wm` and `roulette`.

## Still to come

- **No en prison or la partage.** Both halve the house edge on
  even-money bets when zero comes up, and both are common on European
  tables. Left out because they turn a settled round into a held one,
  which the round structure has no place for yet.
- **No table minimum or maximum.** A real wheel has both, and the
  maximum is what makes a martingale lose rather than merely take a
  long time.
- **The history panel means nothing**, and is there because every
  roulette table in the world has one. Six numbers that have no bearing
  whatsoever on the next spin.
