# Catch: the bat

```lesson
(objective "Write the first half of a game: a game loop that reads the keys and moves a bat across the screen.")
(minutes 20)
(keywords "game loop")
```

In this lesson and the next you build Catch: a ball falls, and you
catch it with a bat that you move with Left and Right.

Every action game is built round a **game loop**, which goes round
once a frame: read the keys, move things, draw the frame, and go round
again. Here is the first half of Catch, the bat alone. The line numbers
leave room for the ball, which comes next lesson.

```basic
10 REM CATCH: MOVE THE BAT WITH LEFT AND RIGHT
20 REM X BAT, B AND Y BALL, S SCORE, M MISSES, R MIXER
60 X = 136
80 K = KEY
90 IF K = 130 AND X > 0 THEN X = X - 8
100 IF K = 131 AND X < 280 THEN X = X + 8
120 CLS: BOX X, 220, X + 39, 227, 1: SYNC
130 GOTO 80
```

The `REM` lines list what each variable is for, as the plan in the
lesson on subroutines advised.

```check
(choice
  (ask "Which lines make the game loop?")
  (right "Lines 80 to 130")
  (wrong "Lines 10 to 60"
    "Lines 10 and 20 are remarks and line 60 sets up the bat: they run once. The loop is what line 130 jumps back to.")
  (wrong "Line 120 only"
    "Line 120 draws a frame, but the loop also reads the key and moves the bat. It runs from line 80 to the GOTO.")
  (hint "Where does line 130 jump back to?"))
```

---

## The bat

The bat is a filled box 40 pixels wide, from X to X + 39, and 8 high,
near the bottom of the screen. Line 120 is one whole frame on one line:
clear, draw, `SYNC`.

```check
(number
  (ask "When X is 280, what is the x coordinate of the bat's right-hand end?")
  (answer 319) (from "280 + 39")
  (wrong-if 320 "The bat runs from X to X + 39, 40 pixels in all: 280 + 39.")
  (hint "The box's right-hand corner is at X + 39."))
```

---

## Staying on the screen

Each press moves the bat 8 pixels. Lines 90 and 100 move it only while
there is room: `X > 0` on the left, `X < 280` on the right. The bat
starts at 136, a multiple of 8, so it stops exactly at 0 and at 280.

```check
(choice
  (ask "Suppose line 60 said X = 140 instead. What would go wrong at the left edge?")
  (right "X would go 12, 4, then -4: the bat would hang off the edge")
  (wrong "Nothing: X > 0 stops it at 0"
    "X > 0 is checked before the move. At X = 4 it holds, so the bat moves to -4, past the edge.")
  (wrong "The program would stop with an error"
    "Drawing beyond the screen is not an error in BASIC: part of the bat would simply be off the screen.")
  (hint "Count down from 140 in eights: 140, 132, ... 12, 4, ..."))
```

---

```try
(try (app "basic")
  (task "In BASIC, type NEW, then the program above, then RUN. Move the bat with Left and Right, all the way to each edge. Press Esc to stop, then SAVE CATCH. Come back.")
  (check (choice
    (ask "What happens when you press Right with the bat at the right-hand edge?")
    (right "Nothing: the bat stays where it is")
    (wrong "The bat moves off the screen"
      "Line 100 moves it only while X < 280, and at the edge X is 280.")
    (wrong "The bat jumps to the left edge"
      "Nothing in the program moves the bat round to the other side. At the edge, line 100 simply does not move it.")
    (hint "Look at the condition on line 100."))))
```

---

```check
(choice
  (ask "You want the bat to move in smaller steps, half as far for each press. What do you change?")
  (right "The 8 in lines 90 and 100 to 4")
  (wrong "The 39 in line 120 to 19"
    "That makes the bat half as wide. How far it moves is set by lines 90 and 100.")
  (wrong "Add WAIT 100 to line 120"
    "A WAIT slows every frame down, but each press still moves the bat 8 pixels.")
  (explain "136 and 280 are multiples of 4 as well, so the bat still stops exactly at the edges.")
  (hint "How far does one key press move the bat?"))
```

So: a game loop reads the keys, moves things and draws a frame, over
and over; the bat is a box that the arrow keys move within the screen.
