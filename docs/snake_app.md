# snake

The classic: steer a snake to the food, grow with every bite, and do not
run into anything. One player or two, in a 320x240 window or full screen
-- in sixteen colours where the board has them -- with sound effects and
music.

```
> run snake
```

Built on the arcade runtime ([arcade.md](arcade.md)) and its sound
engine ([sfx.md](sfx.md)). The source is `sw/apps/snake/`.

## Ways to play

| | |
|---|---|
| **Classic** | walls all round; every bite makes the snake a little faster, and the music fuller |
| **Open field** | no walls: leave by one edge and come back at the other |
| **Mazes** | six levels of inner walls; eat twelve to clear a level, which then gets faster |
| **Two players** | the arrows against W A S D (or two gamepads); a crash loses the round, a head-on is a draw; first to three rounds |

## Skill

The top line of the menu, `< EASY >`, sets how hard it is (Left and
Right; [arcade.md](arcade.md#skill)). **Easy is the default.**

| | Easy | Normal | Hard |
|---|---|---|---|
| speed, cells a second | 3.5, + 0.1 a bite, up to 7 | 7, + 0.3 a bite, up to 15 | 9, + 0.4, up to 17 |
| mazes | 4, + 0.3 a level | 8, + 0.6 a level | 9.5, + 0.7 |
| two players | 4.5 | 8.5 | 10 |
| a crash | costs a **heart** | ends the game | ends the game |
| the star | 14 seconds | 7 seconds | 6 seconds |
| food | appears near the head | anywhere | anywhere |

On easy the snake has **three hearts**, shown in the status line, and
wins another every ten bites (five at most). Running into a wall or a
snake costs one: there is a soft bump, the snake **stops**, and white
dots blink in the cells it could go to. Nothing moves until it is
steered somewhere free -- steering into something else only knocks. It
is also a little slower after a bump, so a child who is struggling is
never pushed further. The last heart ends the game as a crash always
did, and so does a bump with nowhere left to go. In two players each
snake has its own hearts, and a head-on bumps both.

Normal is the game as it was before skills, rule for rule. Each skill
keeps its own high scores.

The music follows the snake: at 3.5 cells a second it is the tune and
bass alone, slow; the drums come in as the snake speeds up
([sfx.md](sfx.md), "Calm first").

Each bite is worth more as the game goes on, and in the mazes more on
later levels. Every fifth bite a **star** appears for seven seconds --
worth up to 150, more the sooner it is reached; a gold line across the
top of the screen shows the time left, and the star blinks for its
last two seconds.

The best ten of each one-player way are kept, with three initials, in
`/data/snake/scores.txt` ([arcade.md](arcade.md), "Scores"). On a new
high score: up/down picks a letter, left/right moves, Enter finishes --
or just type the letters.

## Keys

| Key | Does |
|---|---|
| Arrows (or W A S D, or a pad) | steer; in two players, the arrows are player 1 and W A S D player 2 |
| Enter / Start | choose, start, go on |
| P (Start on a pad) | pause |
| Esc | in the window: pause; again for the menu (between rounds or levels: the menu at once) |
| F1 | help, in the window |
| F11 | full screen, or back to the window ([keys.md](keys.md)) |
| C | in full screen: colour on or off |
| M | music on or off |

Turns are queued two deep and taken one cell at a time, so a quick
up-then-left makes both turns; turning back into the body is ignored.

## How it is drawn

The grid is 40 x 28 cells of 8 pixels below a 16-pixel status line.

The body is a **tube**: each body cell is a 6x6 square in the middle of
its cell plus a one-pixel arm toward each neighbour it joins, so a run of
cells is a 6-pixel tube with a 2-pixel gap between runs side by side.
The cells change only once per move. The **head** (with eyes, and a
flick of the tongue when food is near) and the **tail tip** are sprites
that slide along the tube by the move's progress every frame, which is
what makes the motion smooth at any speed. Crossing the edge in the open
field, both the part leaving and the part arriving are drawn.

Everything static -- floor, walls, body cells, status line, the panels
of text -- is painted by the runtime's dirty rectangles; everything that
slides is an overlay ([arcade.md](arcade.md), "Single-buffered, on
purpose"). A frame of play repaints a handful of cells.

The outer four pixels of the field stay black (the walls at the edges
are drawn four pixels thick), so the crash's shake never shows anything
from the other bitplanes at the border.

### Colour

| Entry | Is |
|---|---|
| 0 | black: the background, and the frame the shake shows |
| 1 | the floor's dots |
| 2, 3 | wall and mortar (the brick pattern is one `ar_fill2` per run of wall) |
| 4-7 | player 1's body |
| 8-11 | player 2's body |
| 12 | the food |
| 13 | gold: the star, titles, popups |
| 14 | fire: sparks |
| 15 | white |

Each body cell is given one of its player's four entries, in turn, as
the head enters it -- and those four entries are recomputed every frame.
In one-player games they are four points 90 degrees apart on a hue wheel
that turns as the snake moves (faster as it speeds up), so a rainbow
flows down the snake from head to tail with not one cell redrawn; in two
players, player 1 ripples through greens and player 2 through reds. The
food breathes, the star glints and the sparks flicker the same way.

A crash flashes the floor and walls red and shakes the screen; the body
then bursts apart a segment at a time from the head, in its own colours.
Clearing a maze level cycles the walls through the rainbow.

### Monochrome

In the window, and in full screen without colour, the same picture uses
inks: the snake white, player 2's snake a 75% grey, the walls as white
mortar lines, the floor plain black. Without a palette the food pulses by
swapping between two drawings.

## Sound

Effects for eating, the star (appearing, eaten, gone), crashing (a
noise crash and a falling saw), each segment bursting, clearing a level,
the menu, a round won, and game over -- each panned to where it happens.
Two original tunes: "Garden" on the title screen and "Slither" in the
game, which grows with the snake's speed -- calm and slow at first, the
whole band only when the snake is fast. On easy a bump is a soft
"boing", steering into something while stopped a knock, and a heart
won back a chime. Menu lines are spoken as the cursor reaches them,
when speech is on. M turns the music off and on.

## The title screen

The title plays a game of its own behind the menu: a snake on the
autopilot (`sn_autopilot()`: of the three ways it can go, prefer one
that is not death, then one with room behind it by a small flood fill,
then the one nearest the food). The menu panel's cells are blocked to
it, so it never goes under the text.

## Source

| File | Is |
|---|---|
| `sn_game.[ch]` | the rules: grid, snakes, moves, food, the star, collisions, hearts and stopping (easy), the speeds per skill, the autopilot. No drawing; host-tested |
| `sn_levels.c` | the layouts, as ASCII pictures (40 x 28: `#` wall, `1`/`2` starts, `B` the title's panel) |
| `sn_draw.c` | the picture: paint, overlays, palette, effects |
| `sn_sound.c` | the effects and the two tunes |
| `snake.c` | the screens and the loop |

Every rule is decided when a move **starts** -- where the head is going
and whether that kills it -- so the head never visibly pokes into a wall
it then turns away from; eating happens when the move completes, with
the head on the food. A snake may follow its own tail into the cell the
tail is leaving. Running into the other snake's head while it comes the
other way is a head-on, whichever moved first.

## Tests

```
make -C sw/apps/snake test      # the rules, with ASan and UBSan
make -C sw/apps/snake render    # every screen, as pictures
```

The rules tests check, after every frame, that the grid and the snakes
agree: every segment a body cell of its owner, every pair of segments
adjacent and linked both ways, as many body cells in the grid as in the
snakes, food and star on empty cells. The cases drive the game into the
corners: eating and growing, walls, biting itself, chasing its own tail
(legal, forever), queued turns and reverses, wrapping, head-ons, the
star coming and going, every level reachable from its start, the
autopilot over 60,000 frames, the dissolve.

`render` runs the real game loop on the host's software framebuffer
([arcade.md](arcade.md), "On a build host") with the autopilot playing:
the title in every mode, a game of classic in colour to a crash and a
new high score, two players in a window partly covered by another one,
the open field in monochrome (steered once straight out of the top
and once back in through it: the head is cut at the field's edge, not
hidden -- it used to vanish there for a cell) and a maze -- and checks
every one of its 9,010 frames against a repaint from nothing
([arcade.md](arcade.md), "On a build host"). Until this, the script's
"open field" was really a second two-player game: Esc between rounds
did nothing, so the menu keys that followed started versus again.

Easy has its own cases: the snake stops at a wall with a heart less and
waits (600 frames, not a pixel moved); a turn the way it faces is not
queued, a free one starts it again; the last heart ends the game; boxed
in, it is over rather than stuck; a heart every ten bites, five at
most; slower after a bump; food within reach of the head; a head-on in
two players stops both; and a minute of random steering with the
autopilot, the invariants checked every frame. The render plays the
skill line and an easy game into the wall (pictures 22-26).
