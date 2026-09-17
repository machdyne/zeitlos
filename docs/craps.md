# craps

**Status: complete.** The whole felt, the come-out and point phases,
free odds, and chips that persist in `/USER/casino.dat`.

    cd sw/apps/craps && make                   # the target binary
    make test                                  # 131 checks, plus 196 shared
    make render WHAT=comeout|point|rolling     # draws it, for looking at

53,088 bytes for rv32i, no soft-float in the app's own objects.

## The edge is derived, not declared

The house edge of every bet here is a consequence of two dice and the
payout table, so `tests/edge_test.c` **walks the chain and computes
it**, in exact integer arithmetic, rather than comparing against a
constant somebody typed.

A bet that stands gets re-rolled, so its expectation is a geometric
series — but conditioning on the *resolving* rolls collapses it. Once a
bet can only win or lose, the chance of winning is
`ways to win / (ways to win + ways to lose)`, and every standing roll
cancels. That is why these come out as small exact fractions instead of
repeating decimals, and why the test needs neither a convergence limit
nor a tolerance.

| bet | expectation | edge |
| --- | --- | --- |
| **free odds** | **0** | **0.000%** |
| don't pass (bar 12) | −3/220 | 1.364% |
| pass line | −7/495 | 1.414% |
| place 6, place 8 | −1/66 | 1.515% |
| field (2×2, 12×3) | −1/36 | 2.778% |
| place 5, place 9 | −1/25 | 4.000% |
| place 4, place 10 | −1/15 | 6.667% |
| hard 6, hard 8 | −1/11 | 9.091% |
| hard 4, hard 10, any craps, eleven | −1/9 | 11.111% |
| two, twelve | −5/36 | 13.889% |
| any 7 | −1/6 | 16.667% |

The pass line is also stated the way people recognise it: **244 wins in
495**, which falls out of the dice and the rules rather than being
written down.

## One bet with no edge at all

**The odds behind the pass line pay true odds.** Not approximately: 2:1
on a four against a probability of exactly 1/3, 3:2 on a five against
2/5, 6:5 on a six against 5/11. The expectation is the integer **zero**.

It is the only bet in a casino with no edge, which is why it exists and
why the house caps it. The test asserts a numerator of 0 rather than a
tolerance — a tolerance would pass a bet that was merely *nearly* fair,
which is the one thing this bet must not be.

The 3-4-5× cap is not arbitrary either: those multiples are chosen so
the win is always six times the flat bet whichever point is on, which
is what lets a dealer pay it without thinking. There is a test for that
too.

## Three rules that are easy to get wrong

**The bar on twelve *is* the don't pass edge.** Twelve is a push, not a
win. Remove that single exception and the bet wins money, and no casino
would offer it.

**Hardways need the dice, not the total.** A six made of 4-2 loses hard
six and wins place six, *from the same roll*. It is the only bet on the
table that cares which pair came up, which is why the dice are carried
around and not just their sum.

**Place bets are off on the come-out.** That is the convention and it
protects the player from the one roll where sevens are the likeliest
thing on the table.

## The field is not a coin flip

Sixteen of the thirty-six ways win, which is under half. It only works
as a bet because 2 pays double and 12 pays triple — and it is still
−1/36. There is a test asserting the sixteen, because "seven numbers out
of eleven" is what it looks like and is not what it is.

## The round

**Every bet is resolved against the point as it was *before* the roll,
and only then does the point move.** Do it the other way round and a
seven-out pays the place bets it should have taken — by the time they
are looked at, the table is back on a come-out and place bets are off.

**A winning place or hard bet is paid and left working.** A winning pass
or come bet is paid and taken down; it has done its job. One-roll bets
are gone whatever happens. Getting that wrong does not crash anything —
it quietly changes how much is at risk on every subsequent roll, which
shows up only as "the money goes faster than it should".

**The line bets are contract.** A pass line bet with a point cannot be
pulled: it has already had its come-out roll, where most of its winning
chances are, so taking it down would be keeping the good half and
leaving the bad. A don't pass is the opposite — it has survived the
dangerous roll and is now the favourite, so the house is happy to let it
go.

## Closing the loop, as a paired comparison

`tests/edge_test.c` derives the pass line's expectation from the dice.
`tests/game_test.c` plays it through the state machine and checks the
money comes out in the same place.

A hundred thousand rounds of pass-and-full-odds has a standard error of
about 0.3% on the realised edge, because odds bets swing hard. A tight
band round the true 0.374% would fail at random, and a band wide enough
not to would catch nothing.

So **the same dice are played twice** — once flat, once with full odds
behind. Placing odds consumes no randomness and odds resolve on the same
roll the flat bet does, so both runs see an identical sequence and the
flat bet's P&L is *identical* between them. What differs is only the
odds' own P&L, whose mean is exactly zero. That makes the interesting
claim — free odds dilute the edge without the house taking any more —
checkable rather than folklore.

## A weak generator, found by a long simulation

The first version of that test used a bare xorshift32 with `% 6` for the
dice. It reported a house edge of **0.35%, 1.06% or 0.55% from identical
code**, decided only by how many draws had been taken beforehand.

xorshift32's low bits are a short linear recurrence of their own, and a
small modulo reads exactly those bits. Nearly everything in these games
asks for a small bound — 6 for a die, 37 for a wheel, 32 for a reel — so
this was not a craps problem.

`sw/common/games/zrand.c`'s fallback generator now runs a splitmix32
finaliser over the xorshift output, which scatters the high bits down
into the low ones for the cost of two multiplies. **A chi-square over
370,000 draws never noticed**; a simulation of a hundred thousand craps
rounds did.

It is only the *fallback* — on hardware `zg_rng_use_system()` installs a
ChaCha20 stream, which has no such weakness. But the fallback is what
every host test runs on, and host tests are where long simulations live.

## The table

One end of a real table: the place numbers along the top, the come box,
the field, the two line bets, the odds behind them, and the proposition
strip at the bottom. A casino table is a mirrored pair of ends around a
shared middle and there is no room for that here — what is missing is
the duplication, and the big-6-and-8 box, which is a place bet paying
even money instead of 7:6 and exists only to catch people who do not
know that.

**The felt fills whatever space is left**, sized in six bands from the
gap between the dice and the message row. Fixed heights left a third of
the window empty below the propositions — the same "all the slack in one
place" that poker, blackjack and slots each had, and the same fix.

**The odds boxes have no number of their own.** The point is their
number and it moves under them, so the selector is resolved at draw time
rather than at layout time — a spot fixed when the window was laid out
would be laying odds on whatever the point happened to be then.

### What the render caught

**The free odds had no spot at all.** 125 riding on the point with
nothing on the felt to show it, on the one bet worth more than the rest
of the table put together. `tests/board_test.c` now asserts that every
bet the game will take has somewhere to be clicked.

**The odds box cut a hole for the wrong string.** A shaded spot knocks a
solid hole out of the dither for its label, because there is no grey
here — and the hole was sized for `"ODDS"` while `"ODDS 8"` was drawn,
putting the number that matters on the one part of the box that eats
glyphs.

## The dice need no clearing

A die tile from `sw/common/games/zdice.h` has a **lit body**, so it is
opaque: drawn over the previous face it covers it completely. A roll is
two blits a frame at a fixed position, with nothing erased and no frame
on which either die is blank.

Moving them about while they tumble would look better and would require
clearing where they had been — which is exactly what makes
`sw/apps/roulette` flash. Fixed and opaque is the trade this tree keeps
making, and on a display with no back buffer it is the right one.

## The bank is settled when the table empties

The other five games have a round: you stake, it resolves, the net goes
to `/USER/casino.dat`. **Craps has no such boundary.** Bets go up and come
down across many rolls, a place bet wins and stays working, a come bet
takes its own number and outlives the point that was on when it was
made.

So the bank is touched when `cr_at_risk()` reaches zero — when the felt
is genuinely clear, which is what a seven-out is. Until then the stake
is tracked in the game and the display shows what is left to bet rather
than what is in the bank.

That keeps the crash-safety the others have: chips do not leave
`/USER/casino.dat` until the shooter's run is over. It also means a long hot
run is not banked until it ends, which is exactly what a craps table
feels like.

The affordability check counts what is already on the felt, because it
has not left the bank yet — without that a player could put their whole
stack down twice.

## Playing

| | |
| --- | --- |
| Return, or `roll` | throws |
| `bet 25` | the stake a click puts down |
| click the felt | every bet the table takes has a spot |
| `pass`, `dont`, `come`, `field` | |
| `place 6`, `hard 8` | |
| `odds` | clamped to the house maximum, so `odds 9999` means "the most you will let me" |
| `any7`, `craps`, `eleven`, `two`, `twelve` | one-roll, and all terrible |
| `down 6` | takes a place bet back; the line is contract and stays |
| `bank`, `buyin`, `game`, `quit` | |

Refusals say **why** where the reason is a phase rather than a typo —
"the line is closed once a point is on, use come" rather than "no". On a
craps table the answer is usually about *when*, not *what*.

## Two things the device found

**The bet amounts were invisible.** They were drawn by filling a white
rectangle and then asking for colour-0 text — which is not the same
operation as drawing light text on a dark ground. The hardware glyph
path sets its blit pattern from the colour, so "dark text" is not a
thing you get by passing 0.

`z_fb_draw_text2()` exists for exactly this: it fills the glyph cell
with the background and lays the ink over it, which is what the
hardware does natively, so a clipped glyph and an unclipped one look
the same. Every knockout in this tree uses it now — `sw/apps/slots`
had the same construction in its winning-line markers.

**Clicking the dice does nothing.** It does now. They are the most
obvious thing on the table to pick up, and a target that size wants to
be forgiving, so both dice and the gap between them are live.

## Still to come

| | |
| --- | --- |
| the table | the hardest layout in the building — pass line, come, field, place numbers, the proposition box |
| the dice | art and a roll animation. Slots showed the rule: anything that writes every pixel exactly once is flicker-free, so two tumbling dice want opaque tiles rather than a cleared area |
