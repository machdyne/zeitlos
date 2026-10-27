# Circles and patterns

```lesson
(objective "Draw circles, and use loops and variables to draw patterns of shapes.")
(minutes 20)
(keywords "radius (of a circle)")
```

In this lesson you draw circles, and let loops draw many shapes for
you.

`CIRCLE x, y, r` draws a circle with its centre at x, y. Its
**radius**, r, is the distance in pixels from the centre to the edge.
So a circle of radius 50 is 100 pixels across.

```basic
10 CLS
20 CIRCLE 160, 120, 50
```

draws a circle in the middle of the screen. A circle that goes past an
edge is drawn as far as the screen reaches. A radius below 0 stops the
program with `OUT OF RANGE`.

```sources
(source "docs/basic_app.md, Beyond BASIC 1")
```

```check
(number
  (ask "CIRCLE 160, 120, 30 draws a circle. What is the x coordinate of its right-hand edge?")
  (answer 190) (from "160 + 30")
  (wrong-if 220 "That adds 60, the whole width. The edge is one radius from the centre: 160 + 30.")
  (wrong-if 130 "130 is the left-hand edge, 30 pixels to the left of the centre.")
  (hint "The edge is one radius from the centre."))
```

---

## Rings

A `FOR` loop can change the radius:

```basic
10 CLS
20 FOR R = 10 TO 100 STEP 10
30 CIRCLE 160, 120, R
40 NEXT R
```

Move the slider to change the `STEP`.

```figure
(zb-screen)
(define s (slider 'step 5 50 10 "STEP" 5))
(let loop ((r 10))
  (if (<= r 100)
      (begin (ring (zb-sx 160) (zb-sy 120) (* zb-k r) 'paper 1)
             (loop (+ r s)))))
(define n (+ 1 (floor (/ 90 s))))
(text 160 218 (string-append "FOR R = 10 TO 100 STEP " (zb-n s)) 'body 'centre)
(text 160 234 (string-append (zb-n n) (if (= n 1) " circle" " circles")) 'small 'centre)
```

```check
(number
  (ask "How many circles does the program draw with STEP 10?")
  (answer 10) (from "(100 - 10) / 10 + 1")
  (wrong-if 9 "Both ends count: R is 10, 20, 30 ... and 100 too.")
  (wrong-if 100 "100 is the largest radius, not the number of circles.")
  (hint "List the values of R: 10, 20, ... 100."))
```

---

```check
(number
  (ask "With STEP 20, what is the radius of the largest circle?")
  (answer 90) (from "10 + 4 * 20")
  (wrong-if 100 "R goes 10, 30, 50, 70, 90. The next, 110, is past 100, so the loop stops at 90.")
  (hint "Count up from 10 in twenties, without going past 100."))
```

---

## A row of circles

The centre can move instead:

```basic
10 CLS
20 FOR X = 20 TO 300 STEP 40
30 CIRCLE X, 120, 20
40 NEXT X
```

Each circle is 40 pixels across, and each centre is 40 pixels from the
last, so the circles just touch.

```check
(choice
  (ask "Line 20 becomes FOR X = 20 TO 300 STEP 20. What happens to the circles?")
  (right "Twice as many, each overlapping the next")
  (wrong "They become smaller"
    "The radius is still 20 on line 30. Only the distance between centres changes.")
  (wrong "Half as many, with gaps between"
    "A smaller step means the centres are closer together, so there are more circles, not fewer.")
  (hint "The step is the distance from one centre to the next."))
```

---

```try
(try (app "basic")
  (task "In BASIC, type NEW and write a program with two loops, one inside the other, that draws a grid of circles: FOR Y = 30 TO 210 STEP 60, then FOR X = 30 TO 290 STEP 65, then CIRCLE X, Y, 25, then NEXT X and NEXT Y, on lines of their own. RUN it, and come back.")
  (check (number
    (ask "How many circles are in each row?")
    (answer 5) (from "(290 - 30) / 65 + 1")
    (wrong-if 4 "X = 30 is a circle too: 30, 95, 160, 225 and 290.")
    (hint "Count the values of X: 30, 95, ..."))))
```

---

```check
(choice
  (ask "Which statement draws a circle as tall as the screen, from top to bottom?")
  (right "CIRCLE 160, 120, 120")
  (wrong "CIRCLE 160, 120, 240"
    "240 is the height of the screen, but the radius is only half the circle's height.")
  (wrong "CIRCLE 0, 0, 120"
    "That centre is the top left corner, so only a quarter of the circle is on the screen.")
  (hint "The circle's height is twice its radius, and the screen is 240 high."))
```

So: `CIRCLE` draws a circle around a centre, and a loop that changes
the radius or the centre turns one shape into a pattern.
