# sw/apps/casino -- Zeitlos Casino

The front desk. One window that shows what you are worth, lends you
money when you are not worth anything, and starts the five games — so
the dock needs **one** icon instead of six.

    cd sw/apps/casino && make          # the target binary
    make render                        # draws it, for looking at
    make banner                        # regenerates the banner image

41,668 bytes for rv32i.

## The banner

`sw/apps/casino/banner.png` → `cs_banner.c`, **312×88**, one bit per
pixel. Drop a new `banner.png` in and run `make banner`.

**It is dithered, not thresholded.** An 8×8 ordered (Bayer) matrix.
Ordered rather than error-diffused: Floyd–Steinberg gives a better
still image, but its noise is unstructured, which on a 1bpp panel reads
as grain — the same reason `zgfx.c`'s shaded fill is an ordered dither.

**The width is fitted and the height is cropped**, anchored to the
bottom. Scaling to fit both squashes the picture, which on a photograph
of a building is immediately obvious; anchoring to the bottom throws
away sky rather than architecture.

### The tone curve, which took looking at

A night photograph spends most of its range in the dark and one bit of
depth has none to spare. The first attempt ran `autocontrast` before
dithering, which is exactly backwards: stretching the levels lifted the
sky into the dither and the banner came out as grain with a building
somewhere in it.

A night shot wants its blacks **crushed**. Everything below 24 goes to
pure black — that is the sky — and the rest is pulled down by a gamma of
1.3 so the lit stonework does not blow out. Chosen by rendering four
curves and looking at them, which is the only way to choose it.

If your own banner is flat artwork rather than a photograph, a gamma of
1.0 and a black point of 0 will suit it better; both are named constants
at the top of `load()` in `gen_banner.py`.

## There is no free money any more

`zbank_buyin()` used to hand out a fresh stack when the bank ran dry.
That makes an exactly-computed house edge meaningless: slots' 5.359%
costs nothing if losing is undone by asking.

Running out means **borrowing** now, and the loan is recorded. Borrow
1000 and you owe 1200 — the vig is charged once, at the counter, not as
interest that accrues. It is rounded **up**, so borrowing one chip still
costs something; a rate that rounds to nothing on small loans is not a
rate.

Nothing forces repayment. What the debt does is sit in the **net worth**
figure — chips minus debt — which is the number this window leads with.
Chips alone flatter anyone who has borrowed, which is everyone who has
been here a while, and it is the only number a loan actually changes.

`buyin` in any of the five games is now exactly "borrow a starting
stack", so none of them needed changing.

| command | |
| --- | --- |
| `borrow 500` | defaults to a full stack |
| `repay 500` | defaults to everything owed; pays no more than is owed or held |
| `bank` | net worth |
| `reset` | throws the bank away and starts over |
| a game's name | starts it |

The house stops lending at 20,000. The refusal says the limit rather
than just saying no — "no" without a number is a door with no handle.

## Launching

`z_proc_run()` takes a name with no path, the same way the dock and
`sh`'s `run` do.

**A game that is already open is not started again.** `z_proc_list()` is
asked first, because four copies of blackjack all writing
`/USER/casino.dat` is exactly the concurrency `zbank_adjust()` is careful
about, and there is no reason to invite it.

If a launch fails the likely cause is that the binary is not on the
card, which is a build step rather than a bug — so the message says so.

## The balance refreshes on a timer

It changes while a *game* is running, not while this window is, so it is
re-read once a second rather than only on a keypress. Often enough to
feel live, rare enough not to be a poll loop — and only the two lines
that show it are redrawn when it moves.

## Installing

**1.** `casino` is in `APPS` in `sw/apps/Makefile`, alongside the five
games.

**2.** `sw/data/icons/icon-casino.png` is included. Run
`cd sw/data/icons && python3 gen_dock_icon_data.py`, then put **only**
this one in `dock_candidates[]` in `sw/apps/wm/wm.c`:

    { "casino",     z_icon_casino_data },

The other game icons are still in the tree if you would rather have them
on the dock as well, but the point of this app is that you do not need
them.

**3.** `release/lib/mkfatimg.py` has a `CASINO` list with all six
binaries — the games have to be on the card for the casino to launch
them:

    ("apps/casino",    "sw/apps/casino/casino.bin"),
    ("apps/poker",     "sw/apps/poker/poker.bin"),
    ("apps/roulette",  "sw/apps/roulette/roulette.bin"),
    ("apps/blackjack", "sw/apps/blackjack/blackjack.bin"),
    ("apps/slots",     "sw/apps/slots/slots.bin"),
    ("apps/craps",     "sw/apps/craps/craps.bin"),

**4.** Rebuild `wm` and the six apps.

### The list grows, so the banner does not

The window holds the banner, the net-worth lines, one row per game and
three text rows. At a banner height of 100 the **fifth** game did not
fit, and the layout refused to draw rather than running off the bottom
— which is the check doing its job, but it still meant choosing. 88
leaves room for seven games, which is more than there will be.

## The render harness includes casino.c

There is no separate board module here — it is one file — so
`tests/render.c` includes it with `main()` renamed out of the way and
calls its statics. Worth the trick: the layout is the part this tree has
got wrong most often, and it is the part only a picture shows.
