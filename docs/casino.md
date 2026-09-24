# The casino apps

Five games and a front desk, sharing a bankroll, a deck, a pair of dice
and a pile of drawing code.

| | | |
| --- | --- | --- |
| `sw/apps/casino` | `docs/casino_app.md` | the front desk: net worth, loans, and one dock icon for all of it |
| `sw/apps/poker` | `docs/poker_app.md`, `docs/poker_engine.md` | hold'em, five-card draw, five- and seven-card stud; 8 difficulty levels |
| `sw/apps/roulette` | `docs/roulette.md` | European and American wheels, animated spin |
| `sw/apps/blackjack` | `docs/blackjack.md` | 6 decks, splits, doubles, insurance, basic-strategy hints |
| `sw/apps/slots` | `docs/slots.md` | three reels, five paylines, an exact 5.359% house edge |
| `sw/apps/craps` | `docs/craps.md` | the whole felt, and the only fair bet in a casino |
| `sw/common/games` | `docs/casino_bank.md` | the bank, the cards, the shoe, the bounded draw |

Each app's `make test` runs the shared library's 196 checks first, so
the totals below include them:

    cd sw/apps/casino    && make test    # 196
    cd sw/apps/poker     && make test    # 608
    cd sw/apps/roulette  && make test    # 567
    cd sw/apps/blackjack && make test    # 378
    cd sw/apps/slots     && make test    # 267
    cd sw/apps/craps     && make test    # 327

Every suite builds the **shipped** sources with the host compiler.
There is no second copy of an evaluator, a payout table or a layout
anywhere in the tree.

Each app also has two targets that are not part of `make test`:

    make stress     # every layer at once, under ASAN and UBSAN
    make nmcheck    # no soft-float in the target objects (needs the
                    # RISC-V toolchain and a completed `make`)

`make stress` drives the full stack the way the app does -- place bets
both ways, spin or deal, redraw after every step, settle, repeat -- for
a few thousand rounds at several window sizes. It is separate from
`test` because the sanitizers make it slow and because a build machine
without them should still be able to run the suite. It is the right
thing to run after touching anything that writes into a buffer:

| | |
| --- | --- |
| poker | 1,093 hands, 15,591 steps |
| roulette | 2,500 rounds, 5,351 bets, 122,567 animation frames |
| blackjack | 2,500 rounds, 4,414 decisions, half of them following basic strategy |

`nmcheck` is scoped to each app's **own** objects. `sw/common/zobj.c`
has a `float` member in `z_obj_t` and compares it with `fabsf()`, so
`zobj.o` pulls `__subsf3` into every app in this tree on an rv32i
build. That is a property of the shared object system and a separate
question from whether a game kept its own arithmetic integer; failing
on somebody else's float would make the check something people turn
off. Worth knowing about independently.

## Installing

All of this is done in the tree; it is written out so the shape of the
integration is visible.

**1.** `casino poker roulette blackjack slots craps` are in `APPS` in
`sw/apps/Makefile`.

**2.** `dock_candidates[]` in `sw/apps/wm/wm.c` carries **one** entry:

    { "casino",     z_icon_casino_data },

`sw/apps/casino` launches the other five by name through `z_proc_run()`,
so they are on the card but not on the dock -- which is the point of
having a front desk. The other icons are still in `sw/data/icons` if you
would rather have them on the dock as well.

The icon data is generated, so after changing that list:

    cd sw/data/icons && python3 gen_dock_icon_data.py

**3.** `release/lib/mkfatimg.py` has a `CASINO` list with all six
binaries. The games have to be on the card even though nothing on the
dock points at them.

    release/zrelease sdcard      # the card, and nothing else
    tools/mkfatimg.sh            # the same thing, kept for habit

`tools/mkfatimg.sh` used to build the image itself and carry a second
copy of the file list, which `zrelease check` compared against
`mkfatimg.py`'s. It is a wrapper now: one list, one implementation, and
the drift check is gone along with the drift it policed. (It had already
drifted -- the shell list was missing gpudemo, chip8 and chess.)

**4.** Rebuild `wm` and the six apps.

## What is shared, and why it took several games to know

`sw/common/games` holds the bank, the card encoding, the card art, the
shoe and an unbiased bounded draw. Every one of those arrived when a
*second* caller appeared, never before:

- The **loan** arithmetic arrived with `sw/apps/casino`, and took the
  free top-up away with it: a bank that refills itself when it runs dry
  makes an exactly-computed house edge meaningless.
- The **bank** and the **bounded draw** were written for roulette and
  designed with blackjack in mind, because a shared bankroll was the
  point of the exercise from the start.
- The **cards**, the **art** and the **shoe** were poker's until
  blackjack wanted them. They moved then, and not when the `deck` or
  `casino` single-app designs were first proposed, because the shape of
  a shared thing is knowable only once two callers disagree about it.
- **`pk_eval.c` never moved.** Blackjack totals a hand, it does not
  rank one. A poker hand evaluator in a shared directory would be a
  shared file with a single user.

Two things improved in the move that would not have improved in a
rewrite. `zdeck` grew to hold a shoe of up to eight decks (blackjack
deals from six; poker asks for one, and pays 420 bytes for the
generality). And the deck stopped carrying its own random number
generator — poker's version had an injectable RNG *and* its own copy of
a rejection-sampling bound, which is how the tree ended up with two
implementations of that arithmetic, both wrong in different ways.

## The damage-based compositor

`wm` became damage-based after these apps were first written
(`docs/window_manager.md`). Two consequences were adopted rather than
ignored:

**`z_win_frozen()` in every periodic render.** While a window is being
dragged, `wm` empties its region and keeps the last paint on the glass.
A periodic render is asked to draw nothing and change nothing until the
thaw. Roulette's spin and poker's "thinking" indicator are the only two
periodic renders here, and both check it now. The roulette wheel still
*turns* while frozen -- a spin that stalled because somebody moved the
window would land on a different pocket depending on the drag, and the
result was decided before the first frame. Only the drawing is skipped.

**The clip is reloaded every spin frame.** `z_win_content_rect()` now
loads the current redraw's *damage* rather than the whole region, and
that restriction stays programmed until something reloads it. Roulette's
spin frames draw straight to `z_fb_*` without going through that
chokepoint, so a spin beginning after a partial redraw would have been
clipped to whatever strip that redraw invalidated -- a ball visible only
inside a band, for the rest of the spin.

Full repaints are still used everywhere else. `z_win_damage_rects()` is
optional, and ignoring it is correct -- just slower.

## Two bugs the host tests could not see

Both found by running on the device, and both now have a regression
test that fails on the old code.

**The roulette ball was never erased** -- every position it passed
through stayed on screen, so a spin ended with the wheel wearing a
necklace.

The repair works by redrawing the wheel clipped to where the ball was,
which restores the background because the same code drew it. Except
that `rl_wheel_draw()` painted only what it *puts down* -- the rim band,
the hub, the outlines -- and nothing in the dark annulus between the rim
and the outer wall. That annulus is the ball's track. So the repair
painted everything except the pixels the ball was actually on.

The wheel paints its own background disc first now, and the disc is
wider than the wheel by the ball's radius: the ball rides at 94% of the
radius and is a few pixels across, so it reaches past the outer wall --
which is what a ball on a track does and looks right, but it means a
background stopping at the wall left a one-pixel crescent behind. The
test caught that second, narrower version after the first fix.

**Why the test passed anyway.** `tests/wheel_test.c` checked that
repairing the ball's old patch restored it exactly -- and it did, at the
radius the test used. `rl_wheel_init()` leaves `ball_r` at the LANDED
radius, inside the rim band, which the ring fill repaints regardless.
The test exercised the one radius where the bug could not appear. It
uses the track radius now, and there is a second test that runs a whole
spin and counts the ink left on the track: 793 lit pixels before the
fix, under 80 after.

**Typing repainted the entire window, once per keystroke.** Every
character returned "redraw", and the apps had one redraw path: the
whole table. On a roulette wheel that is a visible flash per character.

Nothing was wrong with the picture, which is why no test noticed -- it
was the cost of getting there, and a test that asserts what is on the
glass cannot see how much work it took. There is a `*_STATUS` action
now, returned for the three keys that only edit the command line, which
redraws two rows of text.

That also moved blackjack's strategy hint into `bj_board_draw_status()`.
It lives on the message row, so the light repaint has to carry it or
typing would wipe it -- the same "drawn and then erased" mistake as
before, arriving from the other direction.

## Two things in the tree that are not ours

Found while merging onto the current `wm`, both reported rather than
patched, since they are outside these apps:

**`sw/apps/chess`'s host tests no longer link.**
`tests/chess_shim.h` defines `z_gfx_visible_clip()`, which
`sw/common/tests/zrender.h` now defines too: *redefinition of
'z_gfx_visible_clip'*. Deleting the copy in `chess_shim.h` fixes it --
the shim's other overrides are still needed.

**`zrender.h`'s `z_gfx_visible_clip()` disagrees with `zgfx.c`'s.**
With no region set, `zgfx.c` returns the whole screen and `true`;
`zrender.h` returns `false`. That is the case an app is in before `wm`
sends a region, and the case it is in *deliberately* in game mode,
where `z_gfx_clear_visible()` is called.

The effect is that every blit guarded by the standard
`z_gfx_blit_scissor()` loop is silently skipped in a host test while the
same code draws correctly on the device -- a harness that lies in the
expensive direction. It also contradicts `zrender.h`'s own paint walker
two functions below, which documents "a single unrestricted pass when
there is no region at all". Making the `gfx_region_n == 0` branch match
`zgfx.c` would fix it; the shims here work around it locally instead, so
nothing shared had to be edited.

## Changes outside the games

Files in `sw/common` and one in `sw/apps/wm`, all of them invited:

- **`zrng.c`: `z_rng_below()` never returns for a power-of-two bound.**
  *(Still unfixed upstream as of `c600390`; this patch reapplies.)*
  Its accept bound was `2^32 - (2^32 % n)`, which for such an `n` is
  exactly `2^32` and truncates to **0** — and `while (v >= 0)` on an
  unsigned never ends. A 52-card Fisher-Yates hits 32, 16, 8, 4 and 2
  on every shuffle. Poker hung on its first deal. `sw/apps/repl`'s
  `(random 16)` had the same exposure. The reject count is now a shared
  inline in `zrng.h` so there is one implementation.
- **`zshape.c`/`.h` are new**: circles, rings, quad fills and a
  turns-based sine table, for the roulette wheel. Its own object rather
  than part of `zgfx.c` because it contains no MMIO, which is what lets
  it be tested on a build machine — `zgfx.c` is register writes from
  top to bottom.
- **`zrand.c`: the fallback generator grew a finaliser.** Bare
  xorshift32's low bits are a short linear recurrence of their own, and
  almost everything here asks for a small bound -- 6 for a die, 37 for a
  wheel, 32 for a reel -- which reads exactly those bits. A craps
  simulation reported a house edge of 0.35%, 1.06% or 0.55% from
  identical code, decided only by how many draws had been taken
  beforehand. A chi-square over 370,000 draws never noticed. It is only
  the fallback -- hardware gets ChaCha20 -- but the fallback is what
  every host test runs on.
- **`zbank.h`/`.c`: loans, and a move to `/user`.** The bank used to
  hand out a fresh stack when it ran dry, which makes an
  exactly-computed house edge meaningless. Running out means borrowing
  now, and the debt sits in the net-worth figure. See
  `docs/casino_bank.md`.
- **`zdice.c`/`.h` are new**: six faces at two sizes, generated. In
  `sw/common/games` from the start, which is a deliberate exception to
  the rule below -- a die has no design question to settle.
- **`zeitlos.c`: `z_game_set_enabled()` now tells wm.** An app entering
  game mode takes the framebuffer the desktop lives in, and wm was never
  told, so a click still changed focus and painted a window over the
  game. wm's own game mode is a camera over an unchanged desktop, so the
  register write stayed in `zsoc.h` under a new name and the notifying
  wrapper went in `zeitlos.c` -- which every app already links, so no
  game needed a source or Makefile change. See `docs/window_manager.md`.
- **`zfsapp.c`** is now linked by every one of these apps, for
  `/user/casino.dat`.

## Flicker is about clearing, not about which blit

`sw/apps/roulette` flashes while it spins and `sw/apps/slots` does not,
and the difference is structural rather than a matter of tuning.

A rotating rim has no linear mapping, so every frame it has to be
cleared and redrawn — and drawing straight to the visible page means
every clear is seen. Reels **translate**, and a translated reel can be
covered by opaque tiles that abut exactly: five blits paint a three-row
reel with every pixel written once and nothing cleared first.

`z_fb_hw_scroll()` looked like the answer and is not needed. It would
save four blits, cost a partial-tile path and an occlusion fallback, and
be *slower* where it matters — content moving down costs one blit per
strip, so a reel crawling to its stop at two pixels a frame would take
48 of them against five for a redraw.

## Five things the tests caught that playing would not

**A blackjack paying nothing.** A player natural against a dealer who
cannot have one set the phase to the dealer's turn without running it.
The round parked forever with the bet uncollected.

**49 chips vanishing over 3,000 poker hands.** Odd chips on a split pot
were discarded rather than awarded. Caught by a conservation invariant,
not by any test written for split pots.

**The opponents servicing the message queue 0.9 times per decision.**
Which froze the desktop and left poker's window blank. Nothing in the
suite asked *how often* a callback fired until it did.

**A two-parts-in-four-billion shuffle bias**, from a rejection interval
that was very slightly wrong. Invisible to a chi-square over half a
million shuffles; obvious once the accept region had to be a multiple
of `n`.

**A roulette split that paid on the wrong numbers**, had the layout been
written the obvious way. 3 and 4 are consecutive but sit at opposite
ends of different rows, so they are not adjacent and not a split.

## And five the tests could not

Every one of these was found by `make render` and a look.

**Every playing card was a photographic negative.** Right size, right
place, right everything except that 1 is lit on this display.

**A band of dead space through the middle of two different tables**,
because everything was anchored to the bottom and the slack pooled in
one place.

**Disabled buttons with invisible labels — twice.** There is no grey
here: a dither is black pixels and white pixels, so a glyph drawn over
one loses half of itself. The second attempt shipped, because in a
render it looked *faint* rather than absent and the response was to
tune the dither instead of stopping drawing text on texture.

**An invisible roulette ball**, running white along the lit rim. A ball
drawn in exactly the right place is in the right place whether or not
you can see it.

**A basic-strategy hint drawn and then erased** by the status line
clearing that row on top of it.

The general shape: an assertion can tell you *where* something is, and
almost never *whether it can be seen*. `sw/common/tests/zrender.h`
exists for the second question, and five games in, it has earned its
keep many times over.
