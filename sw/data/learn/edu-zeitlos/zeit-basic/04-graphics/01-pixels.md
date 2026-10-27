# Pixels and coordinates

```lesson
(objective "Give the coordinates of any point on BASIC's screen, and plot points there.")
(minutes 20)
(keywords "pixel (on the screen)" "coordinates (of a pixel)")
```

In this lesson you start to draw.

BASIC's screen is made of dots, 320 across and 240 down. Each dot is a
**pixel**, short for picture element, and each can be
lit (white) or dark (black).

A pixel is found by two numbers, its **coordinates**: x, how far
across from the left edge, then y, how far down from the top. Both
count from 0. So x goes from 0 to 319 and y from 0 to 239, and
`PLOT x, y` lights that pixel.

Drag the point on the screen below.

```figure
(zb-screen)
(define p (drag 'p 160 110))
(define x (zb-px (car p)))
(define y (zb-py (car (cdr p))))
(circle (zb-sx x) (zb-sy y) 3 'paper)
(text 40 14 "(0, 0)" 'small)
(text 280 214 "(319, 239)" 'small 'right)
(arrow 110 10 170 10 'ink 1)
(text 176 14 "x" 'small)
(arrow 26 60 26 120 'ink 1)
(text 22 134 "y" 'small)
(text 160 234 (string-append "PLOT " (zb-n x) ", " (zb-n y)) 'body 'centre)
```

```sources
(source "docs/basic_app.md, The screen; Beyond BASIC 1")
```

```check
(choice
  (ask "Where on the screen is the pixel PLOT 0, 0 lights?")
  (right "The top left corner")
  (wrong "The bottom left corner"
    "On graphs in school, y counts up from the bottom. On this screen y counts down from the top, so y = 0 is the top row.")
  (wrong "The middle"
    "Coordinates count from the left edge and the top edge, so 0, 0 is where they meet: the top left.")
  (hint "x counts from the left edge, y from the top edge."))
```

---

## Down is more

The one surprise: y grows **downwards**. A bigger y is lower on the
screen.

```check
(choice
  (ask "Which of these pixels is nearest the bottom of the screen?")
  (right "PLOT 10, 200")
  (wrong "PLOT 200, 10"
    "The first number is x, across. The second, y, says how far down: 10 is near the top.")
  (wrong "PLOT 100, 100"
    "y = 100 is less than half way down. y = 200 is lower.")
  (hint "Look at the second number, y. Bigger is lower."))
```

---

```check
(number
  (ask "What is the x coordinate of a pixel exactly half way across the screen?")
  (answer 160) (from "320 / 2")
  (wrong-if 120 "120 is half of 240, the height. Across, the screen is 320 pixels wide.")
  (hint "The screen is 320 pixels wide."))
```

---

## A line of points

A loop can plot many points. `CLS` clears the screen, drawings and
text alike.

```basic
10 CLS
20 FOR I = 0 TO 239
30 PLOT I, I
40 NEXT I
```

draws a slanting line from the top left corner, one pixel to the right
and one down at each step.

```try
(try (app "basic")
  (task "In BASIC, type NEW, then this program, then RUN. Then change line 30 to 30 PLOT I, 120 and RUN it again. Come back.")
  (check (choice
    (ask "What did the second program draw?")
    (right "A level line across the middle")
    (wrong "A line straight down"
      "With PLOT I, 120, y stays at 120 while x changes: the line runs across.")
    (wrong "The same slanting line"
      "With y fixed at 120, the points move only across. Did you RUN the changed program?")
    (hint "In PLOT I, 120, which coordinate changes?"))))
```

---

## Text and graphics share the screen

Text and drawings are on the same screen. Each character takes a
square of 8 by 8 pixels: that is how 320 by 240 pixels make 40
columns and 30 rows. Text printed over a drawing replaces the pixels
of its square, and when text moves up at the bottom, the drawings move
up with it.

```check
(number
  (ask "A character in column 10 starts how many pixels from the left edge?")
  (answer 80) (from "10 * 8")
  (wrong-if 10 "Columns and pixels are different units: each column is 8 pixels wide.")
  (hint "Each column is 8 pixels wide, and columns count from 0."))
```

---

```check
(choice
  (ask "Which line lights the pixel in the bottom right corner?")
  (right "10 PLOT 319, 239")
  (wrong "10 PLOT 320, 240"
    "Counting from 0, the last of 320 pixels is number 319. 320, 240 is just off the screen, and nothing is drawn.")
  (wrong "10 PLOT 239, 319"
    "x comes first. 239, 319 is far below the screen, so nothing is drawn.")
  (hint "x first, then y; both count from 0."))
```

So: a pixel's coordinates are x across from the left and y down from
the top, both from 0, and `PLOT x, y` lights it.
