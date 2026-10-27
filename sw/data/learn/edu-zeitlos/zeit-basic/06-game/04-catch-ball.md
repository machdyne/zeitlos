# Catch: the ball and the score

```lesson
(objective "Finish the game: add a falling ball, test for a catch, keep the score and end the game.")
(minutes 25)
```

In this lesson you finish Catch. `LOAD CATCH` brings back the bat from
last lesson. Then type these lines; lines 60, 120 and 130 replace
the old ones.

```basic
30 CLS: PRINT "CATCH: PRESS A KEY TO START"
40 R = (R + 1) MOD 280
50 IF KEY = 0 THEN 40
60 X = 136: S = 0: M = 0
70 GOSUB 300
110 Y = Y + 3
120 CLS: PRINT "SCORE "; S: BOX X, 220, X + 39, 227, 1: CIRCLE B, Y, 4: SYNC
130 IF Y < 216 THEN 80
140 IF B >= X AND B <= X + 39 THEN S = S + 1 ELSE M = M + 1
150 IF M < 3 THEN 70
160 LOCATE 14, 15: PRINT "GAME OVER"
170 END
300 REM A NEW BALL AT THE TOP
310 R = (R + X + 97) MOD 280
320 B = R + 20: Y = 10
330 RETURN
```

```check
(choice
  (ask "Lines 30 to 50 count until a key is pressed. What is R used for?")
  (right "To start each ball at a place the player cannot predict")
  (wrong "To count the score"
    "S is the score. R is the count from the start screen, which line 310 uses to choose where each ball starts.")
  (wrong "To time the game"
    "The game has no clock. R mixes the player's timing into where each ball starts.")
  (hint "Look at lines 310 and 320."))
```

---

## A new ball

The subroutine at line 300 starts a ball. B, across, is R + 20, so
from 20 to 299, and Y, down, is 10, at the top. Line 310 stirs R with
the bat's position X, so where the player stands changes where the next
ball starts.

```check
(number
  (ask "R is 100 and X is 136 when line 310 runs. Where across does the new ball start: what is B?")
  (answer 73) (from "(100 + 136 + 97) - 280 + 20")
  (explain "100 + 136 + 97 is 333, and 333 MOD 280 is 53. B is 53 + 20.")
  (wrong-if 353 "Line 310 takes MOD 280 first: 333 MOD 280 is 53. Then line 320 adds 20.")
  (wrong-if 53 "That is the new R. Line 320 adds 20 to make B.")
  (hint "First R = 333 MOD 280. Then B = R + 20.")
  (hint "333 MOD 280 is 333 - 280."))
```

---

## Falling and catching

Each frame, line 110 moves the ball down 3 pixels, and line 120 draws
the score, the bat and the ball. Line 130 keeps the loop going while
the ball is above the bat, at y 216.

When it gets there, line 140 checks for a catch: the ball's x, B, must
be between the bat's ends, X and X + 39. A catch adds 1 to the score
S; otherwise M counts a miss.

```check
(choice
  (ask "The bat's X is 100 and the ball reaches the bottom with B = 145. What happens?")
  (right "A miss: M goes up by 1")
  (wrong "A catch: S goes up by 1"
    "The bat runs from 100 to 139. 145 is past its right-hand end, so B <= X + 39 does not hold.")
  (hint "Is 145 between 100 and 139?"))
```

---

## The end of the game

Line 150 starts a new ball while there have been fewer than 3 misses.
After the third, lines 160 and 170 print `GAME OVER` in the middle of
the screen and end the program.

```try
(try (app "basic")
  (task "In BASIC, LOAD CATCH, type the new lines, and LIST to check. RUN it and play. Press F11 for full screen, and F11 again to come back. Then SAVE CATCH. Come back.")
  (check (choice
    (ask "After a game, how do you play again?")
    (right "Type RUN")
    (wrong "Press Esc"
      "The program has ended by itself, at END. Esc would stop a program that was still running; RUN starts this one again.")
    (wrong "Press any key"
      "Once the program has ended, keys go to BASIC's line, not the game. RUN starts it again.")
    (hint "How did you start it the first time?"))))
```

---

## Make it yours

Now that it works, change it: the speed of the ball (line 110), the
width of the bat (lines 120 and 140, and the limit in line 100), more
misses allowed (line 150). Save each version that works under its own
name, so you can always go back.

```check
(choice
  (ask "You want the ball to fall faster as the score goes up. Which change to line 110 does that?")
  (right "110 Y = Y + 3 + S / 5")
  (wrong "110 Y = Y + 3 * S"
    "At the start S is 0, so the ball would never move at all.")
  (wrong "110 Y = Y - 3"
    "That moves the ball up, not down. y grows downwards.")
  (hint "The fall must start at 3 pixels a frame, and grow with S."))
```

---

```check
(choice
  (ask "With 110 Y = Y + 3 + S / 5, how many pixels does the ball fall each frame once the score is 12?")
  (right "5")
  (wrong "5.4"
    "BASIC has whole numbers only: 12 / 5 is 2, and 3 + 2 is 5.")
  (wrong "15"
    "Division comes before addition: S / 5 is 2, then 3 + 2.")
  (hint "Work out 12 / 5 as BASIC does, then add 3."))
```

So: Catch is a game loop with a subroutine for each new ball, an `IF`
for each catch or miss, and an `END` after the third miss: a program
you wrote yourself.
