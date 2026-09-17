# poker: the engine

The parts of `sw/apps/poker` that have no I/O in them: the card
encoding, the hand evaluator, the deck, and (as they arrive) the
betting engine and the opponents. `docs/poker_app.md` covers the app
a person actually sits in front of.

The split matters for one reason above all others: everything
described here builds and runs on the build machine with `cc`, against
the **shipped** sources. There is no second copy of the evaluator
anywhere in this tree.

**Status: complete.** Cards, evaluation, the deck, the betting engine,
the four variants and the opponents are all done and tested. See
`docs/poker_app.md` for the app itself.

This is a clean-room implementation. Nothing in `pk_*.c` is derived
from any existing poker program. The only external facts used are the
rules of poker and the combinatorial counts in `tests/eval_test.c`,
which are properties of a deck of cards rather than anybody's code.

## Card encoding

One byte, 0..51. `rank = card >> 2`, `suit = card & 3`, with rank 0
the deuce and rank 12 the ace.

Rank occupies the high bits so that the natural ordering of the byte
is the ordering of the rank. Sorting a hand by card value sorts it by
rank, which both the evaluator and the display want and neither then
has to do for itself.

The deuce is rank 0 rather than rank 2 so a rank fits in a nibble with
room for the ace, which is what lets the whole evaluation pack into 24
bits. The cost is that `Z_RANK()` does not return the number printed
on the card, and everything that needs that number goes through
`pk_rank_char()`.

Suit order is arbitrary and fixed only so a shuffle is reproducible
from a seed. **No poker rule breaks a tie by suit.** Anything in this
tree that appears to rank one suit above another is a bug, and
`tests/eval_test.c` asserts the negative directly.

`Z_CARD_NONE` is `0xff`, not `0`, because `0` is the two of clubs and
a zeroed structure full of deuces is far harder to notice than one
full of obvious nonsense.

## Evaluation

`pk_eval5()` returns a `uint32_t` with the property that a better hand
is a larger number:

```
    bits 23..20   category, 0 (high card) .. 8 (straight flush)
    bits 19..16   first tiebreak rank
    bits 15..12   second
    bits 11..8    third
    bits  7..4    fourth
    bits  3..0    fifth
```

A showdown compares every live hand against every other, a split pot
needs exact equality, and the Monte Carlo rollouts will do both a few
hundred thousand times per hand. One integer makes all three `<`, `>`
and `==` on a machine word, with no comparison function and no special
cases.

Ties are **exact**. Two hands that tie compare equal as integers, so
the pot splitter needs no epsilon and cannot invent a winner out of a
rounding difference.

### One tiebreak rule for every category

The tiebreak ranks are the hand's distinct ranks ordered by
multiplicity, then by rank, both descending. That single rule produces
the correct answer for every category with no per-category code:

| category | falls out as |
| --- | --- |
| quads | quad rank, kicker |
| full house | trips rank, pair rank |
| two pair | high pair, low pair, kicker |
| one pair | pair rank, three kickers |
| flush, high card | five ranks, high to low |

Straights and straight flushes are the exception and store their high
card alone. The other four ranks are the same four numbers in every
straight of that height and could never break a tie, so carrying them
would be noise.

### The wheel

A-2-3-4-5 is a five-high straight and the ace plays low. It is the one
place in poker where an ace is not the highest card, and an evaluator
that misses it is otherwise indistinguishable from a correct one.

`straight_high()` finds ordinary straights by sliding a five-bit
window down a rank mask from the ace. That cannot find the wheel,
because the ace sits at the far end of the mask rather than four bits
below the five, so the wheel is tested separately and last. It reports
its high card as the five, which makes it sort below every other
straight by the ordinary comparison rather than by a special case
somewhere else.

There is no round-the-corner straight. Q-K-A-2-3 is not a hand, and
the tests say so.

### Best of seven

`pk_eval_best()` evaluates all C(n,5) subsets for n from 5 to 7, which
is 21 at seven cards and around a microsecond on this hardware.

A faster method exists and is not worth the risk. The published fast
evaluators all come with lookup tables, which would compromise the
clean-room requirement, for a saving that disappears next to the
rollouts that call this.

It reports the winning five cards as well as the value, because the
showdown display has to be able to say *why* a hand won. A best-of-N
that returns the right number and the wrong cards highlights the wrong
cards, and no value check catches that.

### The Omaha constraint

`pk_eval_constrained()` requires exactly *k* hole cards and exactly
5 - *k* board cards, rather than the best five of all of them.

It is a separate function rather than a flag because it is a genuinely
different question, and merging them produces the classic Omaha bug: a
board showing four hearts and a player holding one heart believing
they have a flush. They do not. The difference is invisible unless the
constraint is enforced at this level, so `tests/eval_test.c` checks
exactly that board.

No shipped variant uses it yet. It is here because writing it after
the fact would mean auditing everything that had assumed otherwise.

### Naming

`pk_eval_name()` builds its strings by hand rather than with
`snprintf`, and that is not a style choice. `docs/app_runtime.md`
records that one conversion specifier anywhere in an app links
picolibc's formatter at a cost of around 100KB, which is enough to
push a binary past the space the loader has for it. An app that names
a hand at every showdown must not pay that.

## The deck

### The generator is injected

Nothing in `zdeck.c` calls into Zeitlos. The app installs a wrapper
around `z_rng_below()` at startup; the host tests install a
counter-based generator and get the same deal twice.

Without that, a test of the betting engine could only assert things
true of every hand, which rules out almost everything worth asserting.
A side pot is only checkable against an exact board and exact
holdings.

A default is built in (xorshift32, fixed seed) so a forgotten
`pk_rng_set()` produces a playable game rather than a null call
through a function pointer. It is deliberately a poor default: it
deals the same first hand every boot, which is obvious in seconds and
far better than a subtle bias nobody notices.

### Why not `% 52`, and why the bounding is done here

The modulo alone is biased toward low values. In a shuffle that bias is
a deck that deals low cards to early positions slightly too often,
forever, in a way nobody would catch by playing and that would quietly
make the opponents' equity estimates wrong. So `pk_rng_below()`
rejection-samples.

**The injected source supplies raw 32-bit words, not bounded values.**
It used to supply bounded values, which put the rejection arithmetic in
every implementation rather than in one place -- and both
implementations that existed were wrong, differently. `sw/common`'s
`z_rng_below()` never returns for a power-of-two bound, which a deck
shuffle asks for five times per deal; the version here rejected
slightly the wrong interval, leaving an accept region that was not a
multiple of n. See `docs/poker_app.md` for the first one, which hung
the app on its first hand.

The lesson is the reusable part: the thing worth injecting is a source
of entropy, not a second copy of the hard part.

`tests/eval_test.c` pins the rejection boundary exactly -- how many
words a given bound consumes, not just that the output is in range --
because the accept region is the only place either bug lived, and
counting draws is the only way to see it from outside. The fake source
aborts after a thousand draws, so a broken bound produces a named
failure rather than a hung test.

`zrng.h` also says which question to ask about quality: shuffling wants
unpredictable-to-a-person and must **not** gate on `z_rng_secure()`,
which is for keys. A board with no TRNG deals a perfectly good game of
poker.

### Fisher-Yates, downward

Each card is swapped with one at or below it. The upward variant that
swaps with a card at or *above* it looks equivalent and is not: it
produces n^n equally likely outcomes over n! permutations, which do
not divide, so some orderings come up more often than others. The
uniformity test catches this specific mutation.

### Exhaustion is a real case

`zdeck_deal()` returns `Z_CARD_NONE` rather than wrapping. Eight
players in seven-card stud need 56 cards plus burns, and the rules say
the last card is dealt face up as a community card. The betting engine
can only do that if the deck reports the condition.

### Stacking

`zdeck_stack()` forces the next *n* cards, by swapping them up from
wherever they sit. It is for tests and nothing else. There is no debug
command that reaches it, because a stacked deck reachable from the
command line is a cheat that will eventually be found.

It validates the whole list before moving anything, so a bad list
leaves the deck exactly as it was. A test that stacked a duplicate and
got a half-rearranged deck back would fail a long way from its cause.

## The betting engine

`pk_game.c` is one state machine and one set of rules. The variants
are a table.

### The variant table is the load-bearing idea

Texas hold'em and seven-card stud differ in which cards are dealt face
up on which street, who acts first, and whether the forced bets are
blinds or antes. They do not differ in what a raise is, how a side pot
is built, or how a showdown is settled. So `pk_variant_t` is data:

| variant | streets | dealt | forced | act order |
| --- | --- | --- | --- | --- |
| `holdem` | 4 | 2 down, 5 board | blinds | position |
| `draw5` | 2 | 5 down, one draw | blinds | position |
| `stud5` | 4 | 1 down, 4 up | ante + bring-in | board |
| `stud7` | 5 | 3 down, 4 up | ante + bring-in | board |

A function per variant would mean four copies of the side-pot code,
three of which are never exercised as hard as the hold'em one, and all
four of which drift apart.

### The engine advances itself

A caller acts and the engine does everything that follows: next seat,
next street, dealing the flop, running the board out when everybody is
all in, and settling. A caller never asks it to advance, because a
caller that *can* ask can also forget, and the symptom of forgetting is
a game that silently stops.

### Heads-up blind order

With two players the button posts the small blind, acts first before
the flop, and acts last after it. This is the rule that gets
implemented wrong most often.

It needs no special case here. The first actor is the first live seat
after a reference point: the big blind on the first street, the button
thereafter. Two-handed, "after the big blind" wraps round to the
button and "after the button" is the big blind. Correct both times
from one rule.

### The incomplete all-in raise

A short stack who moves in for less than a full raise moves the bet,
so everybody still has to match it, but does **not** reopen the
betting for anyone who had already called the previous amount. They
may call the difference or fold. Players who had not yet acted keep
every option.

This is the single most-often-wrong rule in a betting engine. It only
arises when somebody is short stacked, and the symptom is a re-raise
that should not have been allowed, which at the table looks like
somebody playing badly rather than like a bug.

### Uncalled bets come back before settlement

Returning the excess at the end of each round, rather than untangling
it at showdown, is what keeps the side-pot builder simple: afterwards
no live seat has committed chips that nobody could have matched, so
every pot layer has somebody eligible for it.

### Side pots are built from commitments, not tracked incrementally

At settlement the distinct commitment levels are sorted and one pot is
built per layer, with eligibility decided by whether a seat reached
that level. Three unequal all-ins produce three layers with no special
casing, and an incremental tracker that has to be right at every
moment during the hand is replaced by an arithmetic that only has to
be right once.

Odd chips go to the winners nearest the button's left, by rule rather
than by rounding.

### Dead money

An ante goes to the pot without becoming a street bet. Paying it the
same way as a blind would make the bet to match equal the ante and
turn the first voluntary action in every stud hand into a call.

### The stud bring-in uses suits

Third street's forced bet falls on the lowest exposed card, and a tie
is broken by suit with clubs lowest. It is the only place in poker
where one suit outranks another, and it is why `zcard.h` orders
suits alphabetically: the comparison is on the card byte.

From fourth street on, the best hand *showing* acts first, evaluated
on up-cards alone by `pk_eval_upcards()`. Straights and flushes are
not considered there, which is both unreachable with four cards and
what the rules say.

### Eight-handed seven-card stud

Eight players need 56 cards. The rule is a single community card on
the last street instead of a card each, so that is what happens, rather
than capping the table at seven seats to avoid it.

## The opponents

### Weak levels choose badly from a correct analysis

They are not given a broken hand evaluator. `sw/apps/chess` reached the
same conclusion for the same reason: an opponent with a broken
evaluation plays moves that are *baffling* rather than moves that are
*weak*, and losing to a computer that plays incomprehensibly is not
enjoyable.

A level 1 opponent computes its equity exactly the way level 8 does,
with far fewer samples, and then calls when it should fold, never
bluffs, and ignores its own conclusion a third of the time. The single
number that does most of this is `slack`: parts per thousand added to
the equity before it is compared with the pot odds. A positive slack
is a player who calls with the worst of it, which reads as recognisable
bad poker rather than as randomness.

| Level | Name | Samples | Noise | Bluff | Slack |
| --- | --- | --- | --- | --- | --- |
| 1 | Beginner | 60 | 35% | none | +260 |
| 2 | Casual | 120 | 22% | none | +170 |
| 3 | Novice | 250 | 12% | 4% | +90 |
| 4 | Club | 450 | 6% | 6% | +40 |
| 5 | Steady | 700 | none | 6% | 0 |
| 6 | Strong | 1100 | none | 9% | -10 |
| 7 | Hard | 1800 | none | 12% | -20 |
| 8 | Toughest | 3000 | none | 15% | -25 |

The random fraction of a beginner's decisions is taken *before* the
rollouts rather than after, so a weak level is also a fast one. That is
most of what makes an easy game feel easy to sit at.

### Wall clock first, sample count second

Chess's rule, for the same reason. A fixed sample count takes wildly
different times on a board running from QQSPI PSRAM without an
instruction cache and one running from SDRAM with one
(`docs/icache.md`), so a level tuned on one board is a different level
on the other.

With no clock installed the budget is ignored and the sample count
governs alone. That is what makes the host tests deterministic and
bounded, and it is the only reason the clock is injected rather than
read directly.

### Equity by rollout

Deal the unknown cards at random, play it out, count. Ties count as a
fraction of a win split between the players tying, which is what makes
equity directly comparable with pot odds.

Opponents' **exposed** cards are held fixed and only the hidden part is
sampled. That matters enormously in stud: against a seat showing two
kings the same hand is worth 285 per thousand, and against the same
seat showing rags it is worth 605. Treating visible cards as unknown
would make every stud read wrong.

Sampling draws cards one at a time out of an already-excluded deck
rather than shuffling all 52 per rollout, since a rollout needs a dozen
cards out of forty-odd and the shuffle would otherwise be most of the
cost.

### No floating point, anywhere

Some boards here are rv32i (`sw/common/arch.mk`), with no FPU and no
soft-float linked. One `double` in the equity code pulls in the whole
soft-float runtime, on an app that `docs/app_runtime.md` says can crash
on start if it outgrows the loader's space, and nothing in the build
output would mention it.

Every probability is therefore an integer in parts per thousand.
`make nofloat` strips comments and checks the shipped sources for float
types and decimal literals. The definitive check is `nm` on the target
objects for `__adddf3` and friends; this one runs everywhere and
catches the mistake where it is made.

### Not freezing the desktop

A rollout budget of a second or two is a second or two of not
servicing the message queue, which stalls the **window manager**, not
just this app: wm blocks waiting for a redraw acknowledgement. So the
rollout loop calls a poll callback, every 16 rollouts, that can abort.
Aborting is always safe, because the samples already taken are a usable
estimate, which is the whole reason this is sampled rather than solved.

That interval is a desktop-responsiveness number, not a tuning knob,
and getting it wrong shipped a blank window and a wm timeout. See
`docs/poker_app.md` for what happened and what now asserts it.

### Opponent modelling

Levels 7 and 8 track how often each seat folds and raises. Against a
table that folds a lot they bluff more; against a table that never
folds they stop bluffing entirely and value bet thinner, since there is
no point representing anything to somebody who will call regardless.

Deliberately crude, and twenty observed actions is treated as no read
at all. A model with more parameters than a session has hands fits
noise, and an opponent confidently wrong from eleven hands is worse
than one with no opinion.

### What the opponents do not do

- **No bluff-raising over a bet.** Only bluff-betting into a checked
  pot, where there is something to win uncontested. Raising as a bluff
  is a much harder judgement and attempting it badly is worse than not
  attempting it.
- **No hand reading from betting patterns.** The read is a fold
  frequency, not a range.
- **No stud memory of folded up-cards**, which a strong human player
  would certainly use.

All three are honest gaps rather than oversights, and the third is the
one most worth closing later.

## What the tests prove

    cd sw/apps/poker && make test

931 checks, about 30 seconds across five binaries, plus `make stress`. The layout and input
ones are described in `docs/poker_app.md`; the rest are here. The centrepiece is an exhaustive walk
of all 2,598,960 five-card hands, checked three ways at once.

**1. Hand counts per category.** 40 straight flushes, 624 quads, 3,744
full houses, 5,108 flushes, 10,200 straights, 54,912 trips, 123,552
two pair, 1,098,240 pair, 1,302,540 high card. These are facts about a
deck of cards, which is what makes them usable in a clean-room
project, and they are unforgiving: an evaluator that mishandles the
wheel loses exactly 4 straight flushes and 1,020 straights and gains
them back as flushes and high cards, moving four of the nine counts at
once.

**2. Distinct values per category**, summing to 7,462. The hand counts
say the category boundaries are right; this says the **tiebreaks**
are. An evaluator that dropped the kicker on a pair would classify all
1,098,240 pairs correctly and would collapse 2,860 distinct values
into 13. That was verified by mutation rather than assumed.

**3. A second classifier**, written in the test file, sharing no code
and no approach. `pk_eval5()` builds a rank histogram; `ref_category()`
sorts the ranks and looks at runs of equal neighbours. Two
implementations agreeing on 2.6 million inputs is a far stronger
statement than either passing a list of cases somebody thought of.

This is the same three-way shape `sw/apps/chess` uses for perft:
published counts plus a second generator written in the test.

Around it:

- A twenty-hand ranking ladder checked pairwise rather than
  adjacently, because an evaluator can get every neighbouring pair
  right and still be non-transitive.
- The wheel, the steel wheel, and the absence of a round-the-corner
  straight.
- Suits never breaking a tie, asserted directly.
- Best-of-seven against a brute force written in the test with five
  nested loops, over 200,000 random hands, checking both the value and
  that the five cards reported are a genuine subset that evaluates to
  it.
- The Omaha four-flush board.
- A chi-square on where the ace of spades lands over 520,000 shuffles.
  Not a proof and not trying to be: it catches the two mistakes that
  actually happen, an off-by-one in the Fisher-Yates bound and a
  modulo bias, both of which skew it far past the threshold.

### The betting tests

Two kinds, because almost nothing here can be checked by playing. A
side pot built one chip wrong still pays somebody; a stud act order off
by one seat is invisible unless you are tracking it.

**Scripted hands** with a stacked deck, asserting exact chip counts:
three unequal all-ins producing two pots of 150 and 100, an incomplete
all-in that must not reopen the betting and a full one that must, the
uncalled portion of a raise coming back, the heads-up blind order, the
fixed-limit cap and the bet size doubling on the turn, the club
bringing it in ahead of the diamond, the pair showing acting first on
fourth street, eight-handed stud reaching a community card, and an odd
chip landing on a named seat.

**Fuzz**, 23,000 hands across every variant and every betting
structure with a legal action chosen at random at each decision,
asserting what must hold whatever happens: chips are conserved, no
stack goes negative, the pots sum to the pot, every hand reaches a
settlement, and no action the options say is unavailable is ever
accepted. Conservation alone catches most pot arithmetic mistakes,
because chips have nowhere to hide.

### The opponent tests

"Plays well" is not a testable proposition and these do not pretend
otherwise. Four things are testable and between them cover the failures
that actually happen.

**The estimator against known answers.** Aces against kings is about
82%, and both sides of a matchup must add to 1000. On a finished board
a made royal flush must be exactly 1000 and a dead tie exactly 500, not
nearly. Aces against five opponents is about 49%, not the 35% this test
originally asserted -- each extra opponent costs less than the last,
because they increasingly beat each other rather than the aces.

**Legality, exhaustively.** Every level, every variant, every betting
structure. An opponent that returns an illegal action does not misplay
a hand, it wedges the app: `pk_act()` refuses and the game stops with
nobody to act.

**The ladder being a ladder.** Level 6 against level 1, heads-up, 200
hands from each seat with the button alternating. It must win from both
seats -- a result that only holds in one seat is measuring position, not
skill. It currently wins by around +11,000 and +3,000 chips.

**Aborting cleanly**, since the real app will interrupt a search every
time the window manager wants a redraw. An aborted estimate must still
yield a legal action rather than a failure the caller has to handle.

### Mutation testing

Every suite here was checked by breaking the thing it tests and
confirming the right check failed.

The evaluator: dropping the wheel, truncating the tiebreaks, ranking
the wheel ace-high, reversing the Fisher-Yates direction. The betting
engine: making every all-in a full raise, posting the heads-up blinds
the wrong way round, never returning an uncalled bet, discarding odd
chips, bringing in on the highest card, paying the ante as a live bet,
ignoring commitment levels when deciding side-pot eligibility. The
opponents: counting a tie as a win, ignoring exposed cards, flattening
the level table.

Two findings from that exercise are worth keeping.

**Discarded odd chips were caught by the fuzz, not by the scripted
test.** Forty-nine chips quietly vanished across 3,000 hold'em hands.
That is what a conservation invariant is for: it notices a loss nobody
thought to look for.

**One mutant initially survived.** The stud exposed-card test asserted
only `vs_rags > vs_scary`, and an estimator mutated to ignore exposed
cards passed it -- because that mutation makes the two numbers *equal*,
and `>` on two equal-but-noisy estimates is a coin flip rather than a
test. The real gap is about 320 points and repeatable to within five,
so the assertion now requires a margin of 200.

The general rule that came out of it: any comparison between two
measured quantities needs a margin wider than the noise and narrower
than the effect. Without one it passes half the time on a broken build.

Worth repeating after any substantial change here. A test suite that
has never been seen to fail has not been shown to test anything.

## The app

`docs/poker_app.md` covers the rest: the card art, the layout, the
command line, what the render caught that no assertion could, the
binary size, and the four edits needed to put poker in the dock.
