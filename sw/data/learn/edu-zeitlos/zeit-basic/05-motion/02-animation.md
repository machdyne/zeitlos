# Animation

```lesson
(objective "Make a shape move smoothly: draw a frame, show it with SYNC, change the position, repeat.")
(minutes 20)
(keywords "animation" "frame (of an animation)")
```

In this lesson you make things move.

An **animation** is a run of still pictures, each a little different
from the one before, shown one after another fast enough to look like
movement. Each picture is a **frame**.

A program animates in a loop: draw the frame, show it, change the
position, and go round again.

The screen you see is refreshed many times a second, each refresh a
video frame. `SYNC` waits for the next video frame and shows the
screen as the program has drawn it. So one `SYNC` per frame keeps the
animation smooth and at an even pace.

```sources
(source "docs/basic_app.md, Beyond BASIC 1 (SYNC)")
```

```check
(choice
  (ask "What does SYNC do?")
  (right "Waits for the next video frame, and shows what has been drawn")
  (wrong "Clears the screen"
    "CLS clears the screen. SYNC waits for the next frame and shows the screen.")
  (wrong "Waits one second"
    "A video frame is much shorter than a second: the screen is refreshed many times each second.")
  (hint "It is about showing, and about the pace."))
```

---

## A rolling ball

```basic
10 X = 0
20 CLS: CIRCLE X, 120, 10: SYNC
30 X = X + 2: IF X < 320 THEN GOTO 20
```

Line 20 is one frame: clear the screen, draw the ball, show it. Line
30 moves the ball 2 pixels to the right and goes back for the next
frame until the ball reaches the right-hand edge.

Keep a frame's clearing, drawing and `SYNC` together on one line, as
here: BASIC may show the screen whenever it starts a new line, so a
frame spread over several lines can sometimes be seen half drawn.

```sources
(source "docs/basic_app.md, Beyond BASIC 1, the example; sw/apps/basic/basic_app.c, hw_break(): a frame is drawn when one is due at the start of a program line")
```

```check
(number
  (ask "How many times does the rolling ball program draw a frame?")
  (answer 160) (from "320 / 2")
  (explain "X is 0, 2, 4 ... 318: 160 values.")
  (wrong-if 320 "X goes up by 2 each frame, so it reaches 320 in half as many steps.")
  (hint "X goes from 0 in steps of 2, and stops when it reaches 320."))
```

---

## Faster and slower

The step decides the speed: `X = X + 4` moves twice as far each frame,
so the ball crosses in half the time. A `WAIT` in the loop slows it
down.

```check
(choice
  (ask "You change line 30 to X = X + 1. What happens?")
  (right "The ball moves half as fast, more smoothly")
  (wrong "The ball moves twice as fast"
    "A smaller step means it moves less each frame, so it takes twice as long to cross.")
  (wrong "Nothing changes"
    "The step is how far the ball moves each frame, so it sets the speed.")
  (hint "How far does the ball move from one frame to the next now?"))
```

---

## Bouncing

A second variable can hold the direction: D is 3 for right and -3 for
left. When the ball reaches an edge, D changes sign:

```basic
10 X = 20: D = 3
20 CLS: CIRCLE X, 120, 10: SYNC
30 X = X + D
40 IF X > 300 OR X < 20 THEN D = -D
50 GOTO 20
```

It bounces between the edges until you press Esc.

```check
(choice
  (ask "What does line 40 do when the ball reaches the right-hand edge?")
  (right "It turns D from 3 to -3, so the ball moves left")
  (wrong "It stops the program"
    "Nothing here ends the loop: line 50 always goes back. Line 40 only reverses D.")
  (wrong "It moves the ball back to the left edge"
    "Line 40 does not change X. It changes D, the step, so X starts going down.")
  (hint "What is -D when D is 3?"))
```

---

```try
(try (app "basic")
  (task "In BASIC, type NEW, then the bouncing program, then RUN. Then make it bounce up and down instead: draw CIRCLE 160, Y, 10, and change X to Y in lines 10, 30 and 40, with the limits 220 and 20. Press Esc to stop, and come back.")
  (check (choice
    (ask "Which line makes the ball turn round at the bottom?")
    (right "40 IF Y > 220 OR Y < 20 THEN D = -D")
    (wrong "40 IF Y > 300 OR Y < 20 THEN D = -D"
      "The screen is only 240 high: Y would pass the bottom long before 300, and the ball would vanish.")
    (hint "The bottom of the screen is y = 239."))))
```

So: an animation is a loop that draws a frame, shows it with `SYNC`,
and moves things on, and the step sets the speed.
