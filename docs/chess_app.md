# chess

A chessboard for Zeitlos. Runs in a window or full screen, plays
against you at eight difficulty levels, and is fully playable with a
keyboard alone or a mouse alone.

**Status: complete.** Engine, board, panel, command line, mouse input,
game mode and the difficulty ladder all work. The host test suite is
green: 74 move-generation checks against published perft counts and a
second generator, 11,550 invariant checks, 336 search and evaluation
checks, and 31 layout checks.

The engine is a clean-room implementation written for this project.
Nothing in `ce_*.c` is derived from any existing engine; the only
external facts used are the rules of chess, the standard public-domain
perft counts in `tests/perft_test.c`, and the main lines in
`ce_book.c`, which are facts about openings rather than anybody's
code.

## Playing

Type a move and press Return, or click a piece and then click where it
should go. Both routes end in the same place -- a move that came out
of `ce_gen_legal()` handed to `game_play()` -- so neither is a special
case of the other.

Moves are accepted in whatever notation you think in:

    e4        Nf3       O-O       exd5      e8=Q
    e2e4      Ng1f3     0-0       e2-e4     e8q

Case matters in exactly one place, and it has to: `bxc4` is a pawn
capture and `Bxc4` is a bishop capture, and both can be legal at once.
Everything else is matched case-insensitively as a fallback, so `NF3`
and `e2E4` work.

### Commands

| Command | What it does |
| --- | --- |
| `new [white\|black\|two\|demo]` | Start again. `two` is two people at one keyboard; `demo` is the engine playing itself. |
| `level 1-8` | Difficulty. Survives `new`. |
| `undo` | Take back a move -- both sides' when the computer has a colour. |
| `flip` | Turn the board round. |
| `hints` | Toggle the markers showing where a selected piece can go. |
| `hint` | Suggest a move, and show it on the board. |
| `moves` | List the legal moves. |
| `fen [...]` | Show the current position, or set one up. |
| `game` | Full screen. Escape comes back. |
| `bench` | Time this board -- see below. |
| `help`, `quit` | |

`F1` steps through the help text on the message line. `F2` toggles
full screen. `Escape` clears the command line and puts down any piece
you have picked up, and stops the engine thinking.

A command is only looked up after the line has failed to parse as a
move, because chess notation collides with plausible command names --
`b4` and `d4` are both moves -- and somebody sitting at a chessboard is
far more often making a move than issuing a command.

## Difficulty

Eight levels. The weak ones are not made weak by giving the engine a
bad evaluation: an engine with a broken evaluation plays moves that are
*strange* rather than moves that are *weak*, and losing to a computer
that plays incomprehensibly is not enjoyable. They are made weak by
choosing imperfectly from a correctly ordered list.

| Level | Name | Roughly |
| --- | --- | --- |
| 1 | Beginner | Plays a random move 30% of the time. Hangs pieces. Answers instantly. |
| 2 | Casual | Two ply, wide blunder margin. |
| 3 | Novice | Sees one-move tactics, misses most two-movers. |
| 4 | Club | The last level that blunders on purpose. |
| 5 | Steady | Stops giving material away. |
| 6 | Strong | |
| 7 | Hard | |
| 8 | Toughest | As strong as the clock allows. |

Every level is bounded by **wall clock** first and search depth second.
That is the opposite of how an engine is normally configured, and it is
deliberate: a depth limit alone makes a quiet position return instantly
and a sharp one take fifteen times as long at the same nominal depth,
which a person experiences as the program hanging at random rather than
as the position being complicated. Equal times make the levels feel
like difficulty settings instead of like different amounts of waiting.

It also means the app needs no per-board tuning. A faster board spends
the same two seconds and gets another ply out of them. `bench` prints
what this particular board manages; on a board running from QQSPI PSRAM
without an instruction cache that is an order of magnitude less than
one running from SDRAM with one (`docs/icache.md`, `docs/boot.md`).

Levels 3 and up use a small opening book -- about twenty-five main
lines, eight to ten plies each. It is worth nearly nothing in playing
strength. It is there so that two games at the same level do not open
identically, which matters much more for a game somebody plays for fun.

## The window, and full screen

The default window is 320x240. The board is 24-pixel squares with rank
and file labels, the panel on the right shows whose move it is, the
level, the material balance and the move list, and the two lines at the
bottom are a message line and the command line.

Full screen goes through game mode (`docs/game_mode.md`): a 320x240
viewport the video hardware pixel-doubles, with two pages to flip
between.

**There is one renderer, not two.** `board_ui.c` draws against an
origin and a clip rectangle in absolute screen coordinates; windowed
passes the window's content rectangle and game mode passes a page.
That is the entire difference between the two modes. The alternative --
window-relative helpers for one and raw framebuffer writes for the
other -- is two renderers that drift, and the drift is invisible until
somebody switches modes mid-game and finds the board laid out
differently on the two sides.

Resizing works. Shrink the window and the panel is dropped before the
board is clipped, because a board you can see all of beats a panel you
cannot read.

## How a square is drawn

Each of the 64 squares is one opaque `z_fb_hw_blit_mem()` of a
precomposed 24x24 tile. There are 26 of them -- thirteen contents
(empty, plus twelve pieces) against two square colours -- generated by
`gen_pieces.py` and committed as `pieces.c`. 2,496 bytes in total.

The obvious design instead is a transparent piece sprite over a
separately filled square. That was rejected for three reasons:

- It needs `z_fb_hw_blit_sprite()`, which needs raster ops, which
  `zgfx.h` says to probe for because an older bitstream silently
  treats every rop as `COPY` -- and a masked sprite that degrades to
  `COPY` is an opaque box around every piece.
- Where there is no cookie-cut mode it is two passes, so the sprite's
  footprint is momentarily blank. That is fine in a back buffer and
  not fine in a window drawn straight to the visible page.
- It makes a move a fill plus two masked sprites instead of two blits.

Light squares are white and dark squares are a 50% checkerboard dither,
which reads as grey rather than as texture because the video hardware
doubles every framebuffer pixel. Each piece is drawn once as a body
with a computed one-pixel outline ring, and the two colours are "body
white, ring black" and "body black, ring white". That is what makes
both sides readable on both square colours: a white piece on a white
square is defined by its ring, and a black piece on the grey dither is
separated from it by its ring.

## Not freezing the desktop

The engine can think for seconds, and a process that stops servicing
its message queue stalls the **window manager**, not just itself -- wm
blocks waiting for a redraw acknowledgement until `REDRAW_ACK_TIMEOUT`
(`docs/window_manager.md`). A four-second search with no message pump
would freeze every window on screen for four seconds.

So `ce_search_go()` takes a poll callback, called every 256 nodes,
which runs the message pump and can abort the search. Two consequences
run through `chess.c`:

- **The search runs on a copy of the position.** It makes and unmakes
  moves as it goes, so the board it is searching is, for almost all of
  that time, a different board from the one on screen. Repainting from
  it would draw a position thirty plies into a variation nobody chose.
- **Input is restricted while it runs** to window events and Escape.
  Running a command mid-search would change the game underneath it.

Aborting is safe at any point: the incomplete iteration is thrown away
and the best move from the last completed one is played. That is the
whole reason the search is iteratively deepened rather than run once at
full depth.

## Building and testing

    cd sw/apps/chess
    make              # the target binary
    make test         # four host test binaries, native cc
    make render       # draws the board and writes /tmp/chess.pbm
    make pieces       # re-runs gen_pieces.py

`make test` builds the **shipped** sources with the host compiler.
There is no second copy of the move generator, the evaluation or the
layout anywhere in this tree. See `docs/chess_engine.md` for what the
engine tests actually prove.

`make render` is the half a person looks at, and `tests/test_layout.c`
is the half that runs unattended. `sw/common/tests/zrender.h`'s header
explains why both are needed, using `sw/apps/logic`'s three
shipped-wrong panels as the worked example. For a chessboard most of
what matters is in the first category: whether the pieces are legible
at 24 pixels, whether the dither fights the outlines, whether the move
list is too cramped. None of those is an assertion and all of them are
obvious in one look.

    make render WHAT=midgame    a piece selected, destinations marked
    make render WHAT=thinking   the engine's progress indicator
    make render WHAT=mate       checkmate, with the check frame
    make render WHAT=narrow     too narrow for the panel
    make render WHAT=game       the full-screen page

## Memory

The transposition table is 24KB of `.bss` and the tiles are 2.5KB of
read-only data; the game itself is about 7KB, mostly the move list and
its SAN strings. Nothing is allocated: an app's `malloc()` grows into
its 16KB stack allowance (`docs/app_runtime.md`), so anything of size
has to be static.

The game is stored as a **move list**, not as a stack of undo records.
Taking a move back replays from the start. An undo stack would be 16
bytes a ply against a move's 4, and the move list is needed anyway --
for the panel, for repetition detection, for saving a game. Replaying
a few hundred moves is under a millisecond and only happens when
somebody presses a key.

## What is not here

- No clocks. A chess clock wants a game that is being played at a
  pace, and this one is played at whatever pace the person feels like.
- No saved games. The move list is the whole state and `fen` gets a
  position in and out, which covers the case people actually have.
- No PGN. Same reasoning: the move list is on screen in SAN already.
- No endgame tablebases. They are megabytes, and the levels people
  play at do not reach positions where they would decide anything.
