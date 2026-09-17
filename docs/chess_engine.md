# The chess engine

The engine behind `sw/apps/chess`. Clean-room: written for this
project from the rules, not derived from any existing engine.

Five files, none of which touches MMIO, stdio or the allocator:

| File | What |
| --- | --- |
| `ce_core.c` | Board, move generation, make/unmake, FEN, SAN, Zobrist, perft |
| `ce_psq.c` | Piece values and piece-square tables |
| `ce_eval.c` | Position evaluation |
| `ce_search.c` | Iterative-deepening alpha-beta, and the difficulty ladder |
| `ce_book.c` | A small opening book |

That purity is not tidiness for its own sake -- it is what lets
`tests/` build the **shipped** sources on the build machine rather than
a copy of them. A copy drifts, and the thing that drifts is never the
thing you are looking at when the bug shows up.

## Representation

**0x88 mailbox, not bitboards.** Bitboards want 64-bit shifts,
multiplies and a population count. `sw/common/arch.mk` allows rv32i,
where a 64-bit shift is a libgcc call and there is no popcount at all.
A 0x88 board makes every off-board test one AND against a constant and
every ray an add:

    sq = rank * 16 + file        a1 = 0x00, h8 = 0x77
    (sq & 0x88) != 0             <=>  off the board

**Piece lists alongside the board.** `docs/icache.md`: instruction
fetch is cached, data access is **not**, and a load costs about 11
cycles on SDRAM and 63 on QQSPI PSRAM. Scanning 128 squares to find the
16 pieces on them is therefore not a small constant factor, it is the
whole cost. `plist[colour][]` holds the occupied squares and `pidx[sq]`
is each piece's slot within it, so removing a piece is a swap with the
last entry rather than a scan.

**32-bit Zobrist keys, not 64.** A 64-bit XOR on rv32i is a libgcc call
at every one of them, and the transposition table is 2,048 entries --
nowhere near the size where 32 bits is the limiting factor. Every move
taken from a table hit is verified against the generated move list
before use, so a collision costs a wasted ordering hint rather than an
illegal move. The tables are generated at `ce_init()` by a fixed-seed
xorshift rather than stored, which saves 3KB of flash and keeps the
host tests and the target bit-for-bit identical with no generated file
to keep in sync.

**Seven pieces of derived state**, all of them maintained incrementally
by `ce_make()`/`ce_unmake()`: the piece lists, the index map, the
per-type counts, the king squares, non-pawn material, the tapered
piece-square accumulators, and the key. Every write to any of them goes
through `add_piece()`, `remove_piece()` and `move_piece()`, because the
way derived state goes wrong is one field being updated at a call site
that forgot the other six.

Legality is decided by making the move and testing whether the mover's
king is attacked, rather than by pin-and-ray analysis. That is one
make/unmake per illegal move against an analysis that would have to be
right for every one of en passant's peculiar discovered-check cases.

## What the tests actually prove

Three test binaries, and the technique in two of them is the same: a
**second implementation, deliberately unlike the first**, so that a
disagreement names which one is wrong instead of leaving a number
nobody can check.

### `tests/perft_test.c` -- 74 checks

Perft counts the leaves of the legal move tree at a fixed depth. It is
the only test of a move generator worth having, because every rule that
is easy to get wrong -- en passant, the pin that makes an en passant
capture illegal by discovered check, castling through an attacked
square, castling rights lost because a **rook** was captured rather
than moved, promotion under check -- changes the count, at a depth
shallow enough to find.

Six standard positions against the published public-domain counts,
including the start position to depth 5 (4,865,609 nodes) and five
others to depth 4. Then the same six against a second generator written
in the test file: a plain 8x8 array, file/rank arithmetic with bounds
checks instead of an off-board mask, legality by looking for the king
in the resulting position's move list. It shares no data structure and
no line of logic with `ce_core.c`.

The second generator earned its place immediately, by being wrong.
It computed "is this square attacked" from the opponent's generated
moves -- and a pawn's capture is only generated when there is something
on the target square, so an empty f1 read as unattacked with an enemy
pawn on g2 bearing down on it. The visible consequence was White being
allowed to castle *through* an attacked square, showing up as exactly
one extra node per affected line. "Attacked" and "reachable by a legal
move" are the same set for every piece except the pawn.

### `tests/core_test.c` -- 11,550 checks

Perft cannot see any of the derived state. A make/unmake that corrupts
the piece list but leaves `board[]` correct passes every perft in the
suite, and then the engine plays a move with a rook that is not there,
weeks later, with nothing to reproduce from.

So: 48 long random games, and after **every single ply** all seven
derived fields are rebuilt from `board[]` alone and compared, and the
incremental key is checked against a full recomputation. Then the whole
game is unwound and the position compared with where it started.

Also SAN and coordinate round trips for every legal move in several
positions -- SAN must be unambiguous by construction, so parsing its
own output and getting a different move back means the disambiguation
is wrong -- plus the sloppy input forms, checkmate, stalemate,
insufficient material, the fifty-move clock, threefold repetition, and
FEN edge cases.

### `tests/search_test.c` -- 336 checks

Not "does it play well" -- strength is a statistical property of
thousands of games and no unit test measures it. What is testable:

- **The evaluation is symmetrical.** Mirror a position, swap the
  colours, and the score must negate exactly.
- **Forced mates are found and the distance is right**, checked
  against a brute-force mate solver written in the test file, which
  answers "can the side to move force mate in n" by definition rather
  than by search -- no alpha-beta, no evaluation, no table, no
  ordering. And the returned move must actually start a mate that
  short, because a correct score with the wrong move attached is a
  real failure mode once a transposition table is involved.
- **It never returns an illegal move**, under any limit, including
  when aborted mid-search.
- **Aborting works** and still yields a playable move.
- **The levels are ordered** -- level 5 against level 1 over eight
  games with the colours alternating.
- **Every line in the opening book is legal.**

## Evaluation

Material and tapered piece-square tables, accumulated incrementally, in
`ce_psq.c`. On top, at evaluation time: doubled, isolated and passed
pawns; the bishop pair; rooks on open and half-open files; and a
middlegame-only king shelter term.

The rule applied is that a term earns its place only if leaving it out
produces a move a person would call obviously bad. Mobility, pawn-chain
shape, outposts and space do not clear that bar -- they make the engine
a little better and noticeably slower, and on this machine a slower
engine is a *worse opponent*, because somebody is sitting in front of a
window waiting for it. One extra ply is worth far more than any of
them.

The piece-square values were written by hand from ordinary positional
principles and are deliberately modest: the largest placement term is
35 centipawns, so placement can break a tie between two moves but never
outvote material. An engine this shallow gets into more trouble from a
confident wrong evaluation than from a bland one.

Two bugs worth recording, both found by the symmetry test:

- The middlegame/endgame blend used `>> 8`, which rounds towards
  negative infinity. A position worth -27.5 became -28 while its mirror
  worth +27.5 became +27 -- an asymmetry of one centipawn, small enough
  to look like nothing and large enough to make the engine prefer one
  colour's version of an identical position. Integer division
  truncates towards zero and negates cleanly.
- The tempo bonus was added before the White/Black flip, handing it to
  White in both cases and biasing every evaluation by 16 centipawns.

## Search

Iterative deepening, fail-soft alpha-beta, with a transposition table
(2,048 entries, 24KB of `.bss`, depth-preferred), null-move pruning,
check extensions, killers, a history heuristic indexed by piece type
and destination square, MVV-LVA capture ordering, and a quiescence
search with delta pruning.

Three things about it are specific to this machine or this app.

**The poll callback.** See `docs/chess_app.md`; the search calls back
every 256 nodes so the app can pump its message queue, and can be
aborted at any point.

**Fail-soft is load-bearing, not a refinement.** With fail-hard, every
root move that fails low records a score of exactly `alpha` -- so all
of them look tied with the best one, and "pick uniformly among moves
within 12 centipawns of the best" degenerates into "pick anything at
all". The symptom was the engine finding mate in one, scoring it
correctly at 29,999, and then playing a random pawn move.

**The blundering levels search the root with a full window.** Fail-soft
fixes the tie but still only gives an upper bound for a cut-off move,
and that bound can land anywhere up to the best score. Exact scores for
every root move need a full window at the root, which costs two or
three times the nodes there. Levels 1-4 pay it -- they are shallow and
cheap and they are the ones that need the numbers. Levels 5-8 never
blunder, so they never need the losing moves' scores, and they keep the
fast narrowing search.

A third bug, found by nothing in particular and worth naming: there was
no ply ceiling in `alphabeta()`. Check extensions increase depth
without bound along a forcing sequence, so a long series of checks
could drive `ply` past `CE_MAX_PLY` and write off the end of
`path_key[]` and `killers[]`. The symptom was not a crash. It was the
engine playing weak moves in sharp positions, because what it was
corrupting was its own killer table.

## Randomness

`ce_search.c` holds one xorshift32, used for the book pick, the
random-move roll at the easy levels, and the choice inside the blunder
margin. `ce_search_seed()` is the only way in, and it is the only
place the engine touches anything outside itself.

That is on purpose: nothing in `ce_*.c` may call into Zeitlos, or the
host tests could not build the shipped sources, and a search test
seeded from a ring oscillator would not be reproducible. The app seeds
it from `z_rng_u32()` once per game -- see `docs/chess_app.md`.

## Performance

On the build machine at `-O2` the search manages 3-5.6 million nodes a
second. On a 48MHz PicoRV32 with uncached data loads, expect three to
four orders of magnitude less -- which is why the difficulty levels are
bounded by wall clock and why `bench` exists. Run it on the board you
care about rather than trusting a number from here.

## If you want to make it stronger

In rough order of value per line changed:

1. **Principal variation search** at non-root nodes. A null-window
   re-search for everything after the first move. Straightforward, and
   worth 20-30% of the node count at these depths.
2. **Late move reductions.** Bigger win, more ways to go wrong; needs
   the mate tests to stay green.
3. **A pawn hash table.** `pawn_scan()` runs on every evaluation and
   its result changes only when a pawn moves. A 1KB table would pay for
   itself, at the cost of another 1KB of `.bss`.
4. **Static exchange evaluation** for capture ordering and for pruning
   losing captures in quiescence.

None of these is likely to change how the app *feels*, which is the
thing it was built for. A level that returns in two seconds and
occasionally drops a pawn is a better opponent for most people than one
that returns in eight and does not.
