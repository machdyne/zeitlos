# blackjack

**Status: complete.** Six decks, splits, doubles, insurance and
surrender, by keyboard or mouse, with chips that persist in
`/user/casino.dat`.

    cd sw/apps/blackjack && make          # the target binary
    make test                             # 351 checks, four binaries
    make render WHAT=play|split|done      # draws it, for looking at

Cards, the shoe and the shuffle come from `sw/common/games` — promoted
out of `sw/apps/poker` when this app became their second caller. Chips
come from the shared bank, `/user/casino.dat`. See `docs/casino_bank.md`.

## An ace is eleven at most once

A hand counts every ace as one, then promotes **one** of them to eleven
if that still fits under 21. Two aces are twelve, not twenty-two; three
are thirteen; four are fourteen.

Writing the promotion as a loop over every ace is the classic way to
get this wrong, and it is wrong quietly — the hand is still playable,
just valued a few too many, for the whole session. `bj_total()` is a
single `if` for exactly that reason, and the tests check A-A, A-A-A and
A-A-A-A by name.

A hand is **soft** when that promotion happened. That is the only thing
distinguishing soft 17 from hard 17, and therefore the only thing
standing between the dealer rules and nonsense.

## Twenty-one is not always blackjack

A natural is **exactly two cards**. Twenty-one made from three, or from
a split, pays even money and loses the tie to a dealer's natural.

Two split aces drawing a ten each are two twenty-ones and neither is a
blackjack. That is worth 50% of the bet, so it is not a rounding
detail — and it is the kind of thing that looks right on screen while
quietly paying the wrong amount.

## Rules that change the edge

Every option is the player-friendly side by default: six decks, dealer
stands on all seventeens, blackjack pays 3:2, doubling after a split
allowed, late surrender available.

**6:5 blackjack is offered because it is what most modern tables
actually pay.** It raises the house edge from about 0.4% to about 1.8%
— more than every other rule combined — and a person should be able to
see that happen rather than read about it.

Also implemented, because leaving any of them out changes the game:

- **Split aces take one card each** and cannot be resplit unless the
  rule says so. Worth about a fifth of a percent.
- **Pairs are matched by value, not rank**: a king and a jack are both
  ten and are a legal split.
- **Insurance is capped at half the bet**, which is what makes 2:1
  exactly cover the main bet when the dealer has a natural. The round
  then nets zero, and the test asserts that rather than the payout.
- **The dealer does not draw when every hand has busted.** There is
  nothing to beat, and drawing would change the shoe for the next round
  on the strength of a decision nobody had to make.
- **The cut card reshuffles between rounds, never during one.** A shoe
  that reshuffled mid-hand would put back cards the player has already
  seen, which is not a shuffle, it is a rewrite.
- **Surrender returns half, rounded in the player's favour.** An odd
  bet is not the place to take an extra chip.

## What the tests prove

**The totaller against a second implementation**, over every two-card
hand and a wide sample of three- and five-card ones — more than a
hundred thousand hands. `bj_total()` promotes one ace with a single
`if`; the reference in the test searches for the best promotion that
fits. Two approaches agreeing is a much stronger statement than either
passing a list of cases somebody thought of.

**Scripted rounds with a stacked shoe**, asserting exact chip counts: a
natural paying 25 on 10 at 3:2 and 22 at 6:5, two naturals pushing,
insurance netting exactly zero against a dealer natural, split aces
paying as a plain twenty-one and *not* three to two, doubling taking
exactly one card, surrender returning six on eleven.

**A fuzz over 6,000 rounds** with every rule combination and one to six
decks, asserting what must hold whatever happens: every round settles,
no hand overruns its array, no total is impossible, and **no round
returns more than it could** — the ceiling being every hand at 3:2 plus
insurance at 2:1. Money appearing from nowhere is the failure a bank
would hide until somebody looked at the balance.

### A bug the tests caught immediately

A player natural against a dealer who cannot have one set the phase to
the dealer's turn without ever running it. The round parked forever
with the bet uncollected — a blackjack paying nothing at all. It was
the first assertion to fail.

## The table

Same origin-and-clip contract as the rest, so a window's content
rectangle and a game-mode page are one renderer with different
arguments.

**The card stride is computed, not constant.** A blackjack hand has no
fixed length — twelve cards is reachable (four aces, four twos, four
threes) and five or six is common. A stride that suits two cards runs
off the table at seven; one that suits twelve wastes the width in the
usual case. So the stride is whatever makes the hand fit its cell.

One hand gets the full 20×28 tiles and the whole width. Split into two,
three or four and each gets a quarter of the table, which is not enough
— those use the 12×17 minis, the same ones poker fans a stud hand with,
where the rank and suit live in columns 2 to 6 precisely so an
overlapped card stays readable. Past about eight cards in a split hand
even the minis overlap their ranks, and the printed total is what you
read; the alternative is cards running into the next hand's.

**The dealer's total counts only what is visible.** Showing the true
total with the hole card down would give the game away completely, and
is exactly the sort of thing that looks like a helpful feature.

**The shoe is drawn as a bar that drains.** A card counter's only
honest tell, and the one thing a player genuinely needs that the cards
do not show: how close the reshuffle is.

## What the render caught

**A band of dead space across the top third.** Everything was anchored
to the bottom — the command line and buttons must not move when the
window resizes — which left all the slack in one place, above the
dealer's cards. The dealer is anchored to the top now and the gap sits
in the middle of the table, which is where felt belongs. `sw/apps/poker`
had exactly this and it was fixed the same way.

**The hands' totals cut in half by the shoe caption.** The shoe bar was
tucked beside the chips and its label landed on the same row. No
assertion noticed, because two pieces of *text* overlapping is not two
*rectangles* overlapping — the layout checks were all about rectangles.
The shoe has its own band now, and there is an assertion that its
caption clears the totals.

Bets are left-aligned in each cell and totals right-aligned, so they
cannot run into each other however long either gets. "bust" beside a
four-figure bet was the case that showed it.

## Playing

| Command | |
| --- | --- |
| `h` `s` `d` `p` `u` | hit, stand, double, split, surrender |
| `deal` (or Return) | |
| `bet 25`, `chip 5\|25\|100\|500` | |
| `insure 10`, `no` | |
| `decks 1-8`, `h17`, `s17`, `pays 3:2\|6:5`, `das`, `surr` | |
| `bank`, `buyin`, `game`, `quit` | |

Return on an empty line deals. It commits the bet that is already set
rather than choosing one, so it cannot cost anything unintended — the
same reasoning that gives roulette's Return the spin.

The rules are printed on the table, because every one of them moves the
house edge and a person should not have to read the source to find out
which game they are playing.

## The bank

**One adjustment for the whole round**, applied when the hand is over:
the net, not a deduction at deal time and a credit at payout. The stake
never leaves `/user/casino.dat` until the hand finishes, so a crash or a
window closed mid-hand costs nothing. `zbank_adjust()` re-reads before
it writes, so a win in another game meanwhile is not clobbered.

Doubling and splitting each put another bet up, and the engine knows
nothing about the bank — that is the app's business, so it is checked
in `bj_input.c`, on both routes at once.

## Basic strategy hints

`hint` toggles them. The correct play appears on the right of the
message row while a decision is open, and only while one is open — a
hint that lingers after the hand is advice about nothing.

**Basic strategy is a table, not a calculation.** It is the exact
solution to a multi-deck game with no counting, computed once decades
ago and published since; recomputing it per decision would be a
combinatorial search for an answer that never changes. That also makes
it testable the way the poker hand counts and roulette's 36-unit return
are — these are facts about the game rather than about anybody's code.

The engine already knows every **legal** action. This is the one that
is **correct**, which is a different question and the one somebody
learning the game actually wants answered. Play the hint every time and
the house edge is about half a percent.

### Three cells worth pointing at

**A pair is looked up before a total.** A pair of eights is sixteen,
and the hard chart says stand or surrender there — but the right play
is always to split. A totals-first lookup gives exactly the wrong
advice on the most famous hand in the game.

**Doubling has two fallbacks, not one.** The table carries `d` and `D`:
soft eighteen against a five wants to double, and if it cannot, it
wants to **stand**, not hit. Collapsing them turns a standing hand into
a hit whenever the hand has already been hit once.

**Never split tens or fives**, which is the row people get wrong
because twenty is already a winning hand and a pair of fives is a ten
that wants doubling. And nines split against everything except seven,
ten and ace — the one pair row that is not monotonic, so a "split small
cards" rule of thumb gets it wrong.

### What it does not model

The chart is the multi-deck, stand-on-soft-17, doubling-after-split
one. The handful of cells that differ under H17 or no-DAS are not
varied, and that is a deliberate limit: the differences are worth a few
hundredths of a percent, and a hint that is subtly wrong in ways the
player cannot see is worse than one that is honestly approximate. When
the rules in play are not the chart's own, the hint is shown with a
**`?`** after it.

Basic strategy also never takes insurance, and the test that follows
the hint for 4,000 rounds declines it every time. It is a bet on the
hole card at 2:1 when the true odds are worse — the one decision at the
table that no hand ever justifies.

### What the tests prove

The famous cells by name, every two-card hand against every up-card
returning *something* (a mistyped table leaves a hole, and a hole is a
hand with no advice), and the strongest one: **over 4,000 rounds and
5,349 decisions, following the hint every time, every hint is an action
the engine accepts.** A hint the player cannot follow teaches a rule
and then refuses it. That runs across rule sets the chart was *not*
built for too, because the reduction has to stay legal even where the
advice is approximate.

### A bug the render caught

The hint was drawn, and then the status line cleared that row on top of
it. The table came up with hints enabled and no hint anywhere. Nothing
asserted it, because "was something drawn here" is not a question the
layout tests ask about text.

## Installing

**1.** Add `blackjack` to `APPS` in `sw/apps/Makefile`.
**2.** `sw/data/icons/icon-blackjack.png` is included. Run
`cd sw/data/icons && python3 gen_dock_icon_data.py`, and add
`{ "blackjack",  z_icon_blackjack_data },` to `dock_candidates[]` in
`sw/apps/wm/wm.c`.
**3.** Add `("apps/blackjack", "sw/apps/blackjack/blackjack.bin"),` to
`release/lib/mkfatimg.py`.
**4.** Rebuild `wm` and `blackjack`.

## Still to come

- **No card counting display**, deliberately. The shoe bar says how
  much is left; a running count would be doing the interesting part for
  you.
- **No even money** on a player natural against an ace, which is
  insurance under another name and would want the same UI.
