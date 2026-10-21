# bricks

A brick-breaking game: a wall of bricks at the top, a paddle at the
bottom, a ball between them. Keep the ball in play with the paddle and
break every brick that can be broken. It runs in a 320x240 window or
full screen -- in sixteen colours where the board has them -- with
sound effects and music, and the mouse, the arrow keys or a gamepad
move the paddle.

```
> run bricks
```

Built on the arcade runtime ([arcade.md](arcade.md)) and its sound engine
([sfx.md](sfx.md)). The source is `sw/apps/bricks/`.

## Playing

Where the ball lands on the paddle decides where it goes: the middle
sends it nearly straight up, the ends out wide (sixteen zones, eight
angles from 8 to 63 degrees from the vertical). Walls and bricks only
turn it round. A served ball sits on the paddle until launched, and
goes by itself after six seconds.

| Brick | |
|---|---|
| six colours | one hit; 50 to 100 points, by colour |
| silver | two hits (three from round 7, four from round 13); 50 points times the round; shows a crack once hit |
| gold | never breaks. A round is clear when everything else is gone |

The ball gets a little faster every eight bricks it hits, and a step
faster the first time it reaches the top wall after a serve. Three
paddles to start, another at 20,000 points and every 50,000 after.

Twelve rounds, each with a name and its own backdrop -- *First Light*,
*Stairs*, *Windows*, *Diamond*, *Pillars*, *Checkers*, *Fortress*,
*Visitors*, *Rainbow*, *Maze*, *Heart*, *Armour* -- and then the first
again, faster and with tougher silver. The menu starts at any of the
twelve (left/right on PLAY).

### Skill

The top line of the menu, `< EASY >`, sets how hard it is (Left and
Right; [arcade.md](arcade.md#skill)). **Easy is the default.**

| | Easy | Normal | Hard |
|---|---|---|---|
| serve speed, round 1 | 1.1 px a frame (66 a second) | 1.9 | 2.25 |
| more each round | 0.06 | 0.125 | 0.125 |
| speed-ups | 0.02 every 16 bricks | 0.05 every 8 | 0.06 every 8 |
| top speed | 2 px a frame | 4.25 | 4.5 |
| paddle | 48 (64 with E) | 32 (48 with E) | 32 (48 with E) |
| paddles | 5 | 3 | 3 |
| the **safety floor** | yes | no | no |
| a held ball goes by itself after | 10 s | 6 s | 5 s |
| rounds 1-3 | every other row | as drawn | as drawn |
| capsules | from one break in two, more of the helpful ones | one in four | one in four |

On easy a glowing bar runs along the bottom under the paddle: the
**safety floor**. A ball that gets past the paddle bounces off it, once
-- a springy sound, the bar breaking up in sparks -- and it comes back
with the next ball served. Silver takes two hits at most. Normal is the
game as it was. Each skill keeps its own high scores.

The music grows with the rounds -- calm on round 1, the drums later,
on easy at half the pace ([sfx.md](sfx.md), "Calm first").

### Capsules

A broken coloured brick sometimes lets a capsule fall (never more than
three at once). Catch it with the paddle: 1,000 points and

| | |
|---|---|
| **E** | a longer paddle: 48 pixels instead of 32 |
| **M** | the ball becomes three |
| **S** | the balls slow right down (and speed up again as they hit) |
| **C** | the paddle catches the ball: it holds it until the button, or 2.5 seconds |
| **L** | the paddle fires two bolts at a time, each breaking (or denting) the first brick it meets |
| **P** | another paddle |

E, C and L are the paddle's state, so the newest replaces the last; a
lost paddle loses it. A capsule that would repeat what the paddle has,
or bring more balls while there are several, becomes S instead.

The best ten scores are kept, with initials, in
`/data/bricks/scores.txt` ([arcade.md](arcade.md), "Scores").

## Controls

| | Does |
|---|---|
| Mouse | the paddle follows it; a click launches the ball or fires |
| Left / Right | move the paddle, faster the longer it is held (3 to 7 pixels a frame) |
| Space, Up | launch the ball, fire the laser |
| P | pause (Start on a pad) |
| Esc | in the window: pause; again for the menu |
| F1 | help, in the window |
| F11 | full screen, or back to the window ([keys.md](keys.md)) |
| C | full screen: colour on or off |
| M | music on or off |

A gamepad: the d-pad moves, A, B, X or Y launch and fire, Start
pauses. The menu, the table and the initials take the usual arrows and
Enter (or a click).

In the window the mouse moves the paddle while the pointer is over the
game; full screen, the pointer's whole range is the field, so the
paddle reaches both walls.

## How it is drawn

Everything still -- the status line, the walls, the backdrop, the
bricks, the panels -- is painted for whatever rectangle the runtime
calls dirty. Bricks are 16x8 and sit on the runtime's 8x8 tile grid
(nineteen columns from x = 8, fourteen rows from y = 32), so a brick
breaking is exactly two dirty tiles. Each brick is a sprite with every
pixel set: a light top and left edge, a dark bottom and right one that
is also the gap to the next brick, a glint.

Everything that moves is an overlay, submitted every frame and erased by
the runtime: the paddle (a rounded steel bar with a light in the
middle -- amber, green while it catches, glowing with an emitter at
each quarter while it fires; four sprites with the wide one), each ball with a trail of three
shrinking dots, the capsules (two frames each, so they roll), the bolts,
a white flash on a brick that is hit, sparks in the colour of the brick
that broke, and the word a capsule says rising off the paddle.

When a round starts the bricks arrive a row every three frames, each row
one dirty strip, with a tick and a glint; then ROUND n and the round's
name. When the round is clear the gold flashes and the backdrop flashes
white. Losing the paddle blows it up into sparks, shakes the screen
(full screen: the viewport moves, nothing is redrawn) and flashes the
backdrop red.

### Colour

| Index | |
|---|---|
| 0 | black |
| 1 | the backdrop's pattern colour: each round its own, pulsing with the music |
| 2, 3 | the walls and their light edge (and labels) |
| 4-9 | the six brick colours |
| 10, 11 | silver, gold (gold glints every two seconds) |
| 12, 13 | the paddle (steel blue) and the amber light in its middle (it flashes when the ball hits) |
| 14 | bolts, trails, capsule gleam (it breathes) |
| 15 | white |

All the colour effects are palette writes: none redraws a pixel. On the
title the six brick colours rotate through the letters of BRICKS.

In monochrome each colour has a pattern (bricks: solid, three greys,
vertical and horizontal lines; silver dotted, gold diagonal) and all
text is white.

### The title

BRICKS in small bricks, the menu, and three balls bouncing off the
walls, the letters and the menu.

## Sound

Effects for the paddle, the walls, silver and gold bricks, a capsule
falling and each capsule caught, a launch, a catch, the laser and its
bolts hitting the top, a ball lost, the paddle lost, a round clear, a
round starting, the bricks arriving, an extra paddle, the menu and game
over. A brick breaking is a note that climbs a pentatonic scale with
each brick in a chain (bricks broken between two touches of the
paddle). Every effect is panned to where it happens. Two original
tunes: "Mortar" on the title, and "Rebound" in the game, a touch faster
each round.

## Source

| File | Is |
|---|---|
| `br_game.[ch]` | the rules: the ball, bricks, capsules, the laser, lives, the twelve layouts, and the autopilot. No drawing; host-tested |
| `br_draw.c` | the picture: paint, overlays, palette effects, effects for events |
| `br_sound.c` | the effects and the two tunes |
| `bricks.c` | the screens, the controls and the loop |

**No tunnelling.** A frame's movement is split into steps of at most one
pixel on each axis; each step moves across and then down, and a move
that would put the ball into a brick, a wall or the paddle's top is
refused and that axis turns round instead. A ball therefore never
overlaps a brick at any speed, and hitting the corner between two
bricks hits both.

**The direction is an angle, not a vector**: one of eight, plus two
signs, with one speed for all the balls. Speeding up or slowing down
cannot bend a path, and no bounce can leave a ball flat enough to
rattle from wall to wall for ever.

The layouts are ASCII in `br_game.c`, nineteen characters by up to
fourteen rows: `.` none, `1`-`6` the colours, `S` silver, `G` gold.

The **autopilot** (`br_ai_target()`) follows the most urgent ball --
coming down, lowest -- to where it will meet the paddle's height
(bouncing off the side walls), hits it off-centre by an amount that
changes every few seconds so the ball goes somewhere new, fetches
capsules when nothing is coming down, and fires when it has the laser.
The host renders play with it.

## Tests

```
make -C sw/apps/bricks test      # the rules, with ASan and UBSan
make -C sw/apps/bricks render    # every screen, as pictures
```

The rules tests cover every layout loading (and coming round again,
faster, with tougher silver), a ball breaking a brick and the round
being clear, silver taking two hits, gold taking many and staying, all
sixteen paddle zones giving the right angle and side, missing the paddle
and losing a life, the serve (held on the paddle wherever it goes,
launched by the button, or by itself after six seconds), each capsule
(wide keeps the paddle's middle where it was; the laser's first press
launches, the next fires two bolts, not again until it may, and the
bolts break bricks; three balls; slow; catch holds the ball while the
paddle moves and lets go by itself; a life; a capsule missed), an extra
paddle at 20,000 points, and the game ending. Then every layout is
played by the autopilot for 30,000 frames, four times each, with after
every frame: no ball inside a wall or a brick, a held ball on the
paddle, the speed within its limits, the brick count right, the score
never going back. About 10.8 million checks; the autopilot clears about
fifty rounds on the way.

`render` runs the real game for 24,000 frames on the host's software
framebuffer with the autopilot playing -- the title in every mode, a
click to start, the bricks arriving, the mouse moving the paddle, play
in colour, paused, in a window, in a window partly covered by another,
in monochrome, each capsule's paddle, three balls, the laser, the
paddle lost, a round clear and the next arriving, then the autopilot
stopped, game over, initials and the table -- and with `AR_HOST_VERIFY`
it checks **every frame** against a repaint from nothing
([arcade.md](arcade.md), "On a build host"). None differs. To reach the
end of a round in a few thousand frames the script thins the wall out
to its lowest row at one point, and drops the laser and catch capsules
on the paddle so their pictures are drawn too.

Easy has its own cases (speeds, lives, the paddle's two lengths, half
the rows on rounds 1-3 and all of them on round 4, the floor bouncing
once and coming back with the next ball), and the soak runs every round
on easy and on hard as well. The render plays the skill line, an easy
round and the floor saving a ball (pictures 40-44).
